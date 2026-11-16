#include "loader/staticExecutableLayout.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

namespace {
using Error = Loader::StaticExecutableLayoutError;
using Bytes = std::vector<uint8_t>;
constexpr size_t Ph = 64;
constexpr size_t PhSize = 56;

void Check(bool value, const char* label) {
	if (!value) {
		std::fprintf(stderr, "StaticExecutableLayoutTests: %s\n", label);
		std::abort();
	}
}

template <typename A, typename B>
void Equal(A actual, B expected, const char* label) {
	if (actual != expected) {
		std::fprintf(stderr, "StaticExecutableLayoutTests: %s: got 0x%llx, expected 0x%llx\n", label,
		             static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
		std::abort();
	}
}

void Put(Bytes& b, size_t at, uint64_t value, size_t count) {
	for (size_t i = 0; i < count; ++i) { b[at + i] = static_cast<uint8_t>(value >> (8 * i)); }
}
void Field(Bytes& b, size_t index, size_t field, uint64_t value, size_t width = 8) {
	Put(b, Ph + index * PhSize + field, value, width);
}
void Header(Bytes& b, size_t index, uint32_t type, uint32_t flags, uint64_t offset,
            uint64_t address, uint64_t filesz, uint64_t memsz, uint64_t align) {
	Field(b, index, 0, type, 4); Field(b, index, 4, flags, 4);
	Field(b, index, 8, offset); Field(b, index, 16, address);
	Field(b, index, 32, filesz); Field(b, index, 40, memsz); Field(b, index, 48, align);
}

// Synthetic plain ELF: header geometry mirrors the mini image, not its contents.
// Program headers are intentionally out of address order.
Bytes Fixture() {
	Bytes b(0xe0000);
	for (size_t i = 0x4000; i < b.size(); ++i) { b[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff); }
	b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
	b[4] = 2; b[5] = 1; b[6] = 1; b[7] = 9; b[8] = 2; // ELF64, little-endian, FreeBSD
	Put(b, 16, 2, 2); Put(b, 18, 62, 2); Put(b, 20, 1, 4);
	Put(b, 24, 0x400080, 8); Put(b, 32, Ph, 8);
	Put(b, 52, 64, 2); Put(b, 54, PhSize, 2); Put(b, 56, 4, 2);
	Header(b, 0, 1, 6, 0xcc000, 0x4c8000, 0xf4c4, 0x3e2e8, 0x4000);
	Header(b, 1, 1, 1, 0x4000, 0x400000, 0x8db1c, 0x8db1c, 0x4000);
	Header(b, 2, 7, 4, 0xcc000, 0x4c8000, 0x10, 0x46, 0x10);
	Header(b, 3, 1, 4, 0x94000, 0x490000, 0x37818, 0x37818, 0x4000);
	return b;
}

void Segment(const Loader::StaticExecutableSegment& s, uint64_t address, uint64_t memory,
             uint64_t offset, uint64_t file, uint64_t page, uint32_t flags, const char* label) {
	Equal(s.address, address, label); Equal(s.memory_size, memory, label);
	Equal(s.file_offset, offset, label); Equal(s.file_size, file, label);
	Equal(s.page_address, address, label); Equal(s.page_size, page, label); Equal(s.flags, flags, label);
}

void TestMini() {
	const auto bytes = Fixture();
	const auto original = bytes;
	const auto result = Loader::PlanStaticExecutableLayout(bytes);
	Equal(result.error, Error::None, "mini: success");
	const auto& p = result.layout;
	Equal(p.load_bias, 0, "mini: fixed bias"); Equal(p.entry, 0x400080, "mini: entry");
	Equal(p.owner_address, 0x400000, "mini: owner starts at first load, not zero");
	Equal(p.owner_size, 0x108000, "mini: owner end 0x508000");
	Equal(p.segments.size(), 3, "mini: segment count");
	Segment(p.segments[0], 0x400000, 0x8db1c, 0x4000, 0x8db1c, 0x90000, 1, "mini: text");
	Segment(p.segments[1], 0x490000, 0x37818, 0x94000, 0x37818, 0x38000, 4, "mini: read-only");
	Segment(p.segments[2], 0x4c8000, 0x3e2e8, 0xcc000, 0xf4c4, 0x40000, 6, "mini: data and BSS");
	Equal(p.segments[2].memory_size - p.segments[2].file_size, 0x2ee24, "mini: BSS extent");
	Check(p.tls.has_value(), "mini: TLS exists");
	Equal(p.tls->address, 0x4c8000, "mini: TLS address"); Equal(p.tls->file_offset, 0xcc000, "mini: TLS offset");
	Equal(p.tls->file_size, 0x10, "mini: TLS file size"); Equal(p.tls->memory_size, 0x46, "mini: TLS memory size");
	Equal(p.tls->alignment, 0x10, "mini: TLS alignment");
	Check(bytes == original, "mini: source unchanged");
}

void TestVariants() {
	auto b = Fixture();
	// Move all loads to another fixed base, and introduce a genuine unmapped page gap.
	Put(b, 24, 0x600080, 8);
	Field(b, 0, 16, 0x6cc000); Field(b, 1, 16, 0x600000);
	Field(b, 2, 16, 0x6cc000); Field(b, 3, 16, 0x690000);
	auto r = Loader::PlanStaticExecutableLayout(b);
	Equal(r.error, Error::None, "other base: success");
	Equal(r.layout.load_bias, 0, "other base: bias"); Equal(r.layout.entry, 0x600080, "other base: entry");
	Equal(r.layout.owner_address, 0x600000, "other base: owner"); Equal(r.layout.owner_size, 0x10c000, "other base: extent");
	Equal(r.layout.segments.size(), 3, "other base: segment count");
	Equal(r.layout.segments[1].page_address + r.layout.segments[1].page_size, 0x6c8000, "gap: start");
	Equal(r.layout.segments[2].page_address, 0x6cc000, "gap: end");
	Equal(r.layout.tls->address, 0x6cc000, "other base: TLS");

	Bytes unaligned(b.size() + 1); std::copy(b.begin(), b.end(), unaligned.begin() + 1);
	const auto saved = unaligned;
	r = Loader::PlanStaticExecutableLayout(std::span<const uint8_t>(unaligned.data() + 1, b.size()));
	Equal(r.error, Error::None, "unaligned source: success"); Equal(r.layout.entry, 0x600080, "unaligned source: entry");
	Check(unaligned == saved, "unaligned source: unchanged");

	b = Fixture(); Field(b, 2, 0, 0, 4); // PT_NULL instead of TLS
	Field(b, 0, 32, 0); // Entire writable segment is zero-filled.
	r = Loader::PlanStaticExecutableLayout(b);
	Equal(r.error, Error::None, "no TLS and zero-file BSS: success");
	Check(!r.layout.tls, "no TLS: absent");
	Equal(r.layout.segments.size(), 3, "zero-file BSS: count");
	Equal(r.layout.segments[2].file_size, 0, "zero-file BSS: file size");
	Equal(r.layout.segments[2].memory_size, 0x3e2e8, "zero-file BSS: memory size");

	b = Fixture(); Field(b, 2, 16, 0x4c8020); Field(b, 2, 8, 0xcc020);
	r = Loader::PlanStaticExecutableLayout(b);
	Equal(r.error, Error::None, "interior TLS: success");
	Check(r.layout.tls.has_value(), "interior TLS: present");
	Equal(r.layout.tls->address, 0x4c8020, "interior TLS: address");
	Equal(r.layout.tls->file_offset, 0xcc020, "interior TLS: matching backing");
	Equal(r.layout.tls->file_size, 0x10, "interior TLS: file size");
	Equal(r.layout.tls->memory_size, 0x46, "interior TLS: memory size");

	b = Fixture();
	// Unknown LOAD flag bits are data in the plan, not runtime admission policy.
	Field(b, 1, 4, 0x80000001u, 4);
	Put(b, 56, 6, 2);
	Header(b, 4, 0x6474e551, 6, 0, 0, 0, 0, 16); // GNU_STACK
	Header(b, 5, 0x6474e552, 4, 0xcc000, 0x4c8000, 0x100, 0x100, 1); // GNU_RELRO
	r = Loader::PlanStaticExecutableLayout(b);
	Equal(r.error, Error::None, "unknown flags and auxiliary headers: success");
	Equal(r.layout.load_bias, 0, "auxiliary headers: bias");
	Equal(r.layout.entry, 0x400080, "auxiliary headers: entry");
	Equal(r.layout.owner_address, 0x400000, "auxiliary headers: owner address");
	Equal(r.layout.owner_size, 0x108000, "auxiliary headers: owner size");
	Equal(r.layout.segments.size(), 3, "auxiliary headers: no additional segments");
	Segment(r.layout.segments[0], 0x400000, 0x8db1c, 0x4000, 0x8db1c, 0x90000, 0x80000001u, "unknown LOAD flag preserved");
	Segment(r.layout.segments[1], 0x490000, 0x37818, 0x94000, 0x37818, 0x38000, 4, "auxiliary headers: read-only unchanged");
	Segment(r.layout.segments[2], 0x4c8000, 0x3e2e8, 0xcc000, 0xf4c4, 0x40000, 6, "auxiliary headers: data unchanged");
	Check(r.layout.tls.has_value(), "auxiliary headers: TLS unchanged");
	Equal(r.layout.tls->address, 0x4c8000, "auxiliary headers: TLS address");
	Equal(r.layout.tls->file_offset, 0xcc000, "auxiliary headers: TLS offset");
	Equal(r.layout.tls->file_size, 0x10, "auxiliary headers: TLS file size");
	Equal(r.layout.tls->memory_size, 0x46, "auxiliary headers: TLS memory size");
	Equal(r.layout.tls->alignment, 0x10, "auxiliary headers: TLS alignment");

	b = Fixture(); Header(b, 2, 1, 4, 0, 0x100000, 0, 0, 1);
	r = Loader::PlanStaticExecutableLayout(b);
	Equal(r.error, Error::None, "zero-sized LOAD: success");
	Equal(r.layout.segments.size(), 3, "zero-sized LOAD: ignored");
	Equal(r.layout.owner_address, 0x400000, "zero-sized LOAD: does not expand owner");
	Equal(r.layout.owner_size, 0x108000, "zero-sized LOAD: owner size");
	Check(!r.layout.tls, "zero-sized LOAD: no TLS");
}

void Failure(std::span<const uint8_t> bytes, Error expected, const char* label) {
	const auto r = Loader::PlanStaticExecutableLayout(bytes);
	Equal(r.error, expected, label);
	Check(r.layout.load_bias == 0 && r.layout.entry == 0 && r.layout.owner_address == 0 &&
	      r.layout.owner_size == 0 && r.layout.segments.empty() && !r.layout.tls, label);
}

void TestFailures() {
	auto b = Fixture();
	Failure(std::span<const uint8_t>(b.data(), 63), Error::TruncatedHeader, "truncated ELF header");
	Failure(std::span<const uint8_t>(b.data(), Ph + 4 * PhSize - 1), Error::TruncatedProgramHeaders, "truncated PHDR table");
	b = Fixture(); b[0] = 0; Failure(b, Error::InvalidHeader, "bad magic");
	b = Fixture(); b[4] = 1; Failure(b, Error::InvalidHeader, "wrong ELF class");
	b = Fixture(); b[5] = 2; Failure(b, Error::InvalidHeader, "wrong byte order");
	b = Fixture(); Put(b, 54, 55, 2); Failure(b, Error::InvalidHeader, "bad PHDR size");
	b = Fixture(); Put(b, 56, 0xffff, 2); Failure(b, Error::InvalidHeader, "extended PHDR count");
	b = Fixture(); Put(b, 16, 3, 2); Failure(b, Error::UnsupportedExecutable, "dynamic executable type");
	b = Fixture(); Put(b, 18, 183, 2); Failure(b, Error::UnsupportedExecutable, "wrong architecture");
	b = Fixture(); Field(b, 2, 0, 3, 4); Failure(b, Error::UnsupportedDynamic, "interpreter");
	b = Fixture(); Field(b, 2, 0, 2, 4); Failure(b, Error::UnsupportedDynamic, "dynamic segment");
	b = Fixture(); Put(b, 32, std::numeric_limits<uint64_t>::max() - 8, 8); Failure(b, Error::ArithmeticOverflow, "PHDR table overflow");
	b = Fixture(); Field(b, 0, 16, 0xffffffffffffc000ull); Failure(b, Error::ArithmeticOverflow, "segment address overflow");
	b = Fixture(); Field(b, 0, 16, 0xffffffffffffc000ull); Field(b, 0, 32, 0); Field(b, 0, 40, 0x3fff);
	Failure(b, Error::ArithmeticOverflow, "page end rounding overflow without segment end overflow");
	b = Fixture(); Field(b, 0, 8, 0xffffffffffffc000ull); Failure(b, Error::ArithmeticOverflow, "segment file offset overflow");
	b = Fixture(); Field(b, 0, 32, 0x3e2e9); Failure(b, Error::InvalidSegment, "file larger than memory");
	b = Fixture(); Field(b, 0, 48, 3); Failure(b, Error::InvalidAlignment, "non-power-of-two alignment");
	b = Fixture(); Field(b, 0, 16, 0x4c8001); Failure(b, Error::IncongruentAlignment, "incongruent alignment");
	b = Fixture(); Field(b, 0, 8, 0xe0000); Failure(b, Error::TruncatedSegment, "segment file bounds");
	b = Fixture(); Field(b, 0, 16, 0x4c4000); Field(b, 2, 16, 0x4c4000);
	Failure(b, Error::OverlappingLoadSegments, "overlapping load pages");
	b = Fixture();
	// The byte ranges end at 0x490100 and start at 0x490200, but share a page.
	Field(b, 1, 32, 0x90100); Field(b, 1, 40, 0x90100);
	Field(b, 3, 16, 0x490200); Field(b, 3, 8, 0x94200); Field(b, 3, 48, 1);
	Failure(b, Error::OverlappingLoadSegments, "disjoint load bytes sharing one page");
	b = Fixture(); Put(b, 24, 0x490000, 8); Failure(b, Error::EntryNotExecutable, "entry in non-executable load");
	b = Fixture(); Put(b, 24, 0x48db1c, 8);
	Failure(b, Error::EntryNotExecutable, "entry at executable end in page padding");
	b = Fixture(); Field(b, 3, 0, 7, 4); Failure(b, Error::MultipleTlsSegments, "multiple TLS segments");
	b = Fixture(); Field(b, 2, 16, 0x510000); Failure(b, Error::TlsOutsideLoad, "TLS outside loads");
	b = Fixture(); Field(b, 2, 8, 0xcc010); Failure(b, Error::TlsOutsideLoad, "TLS wrong file backing");
	b = Fixture(); Field(b, 2, 16, 0x4d74c0); Field(b, 2, 8, 0xdb4c0);
	Failure(b, Error::TlsOutsideLoad, "TLS initialized bytes extend beyond LOAD file backing");
	b = Fixture(); Field(b, 0, 0, 0, 4); Field(b, 1, 0, 0, 4); Field(b, 3, 0, 0, 4);
	Failure(b, Error::NoLoadSegments, "no load segments");
}
} // namespace

int main() {
	TestMini(); TestVariants(); TestFailures();
	std::puts("static executable layout tests passed");
	return 0;
}
