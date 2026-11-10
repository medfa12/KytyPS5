#include "loader/staticExecutableLayout.h"

#include "loader/elf.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Loader {
namespace {

constexpr uint64_t HeaderSize = 64;
constexpr uint64_t ProgramHeaderSize = 56;
constexpr uint16_t ExecutableType = 2; // ELF ET_EXEC, not Sony ET_DYNEXEC.
constexpr uint32_t InterpreterType = 3; // ELF PT_INTERP.
constexpr uint16_t ExtendedProgramHeaderCount = 0xffff;

uint16_t Read16(std::span<const uint8_t> bytes, size_t offset) {
	return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
}

uint32_t Read32(std::span<const uint8_t> bytes, size_t offset) {
	uint32_t value = 0;
	for (size_t i = 0; i < 4; ++i) { value |= uint32_t(bytes[offset + i]) << (i * 8); }
	return value;
}

uint64_t Read64(std::span<const uint8_t> bytes, size_t offset) {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) { value |= uint64_t(bytes[offset + i]) << (i * 8); }
	return value;
}

bool AddOverflows(uint64_t a, uint64_t b) {
	return b > std::numeric_limits<uint64_t>::max() - a;
}

bool Fits(uint64_t offset, uint64_t size, uint64_t limit) {
	return offset <= limit && size <= limit - offset;
}

StaticExecutableLayoutResult Failure(StaticExecutableLayoutError error) {
	StaticExecutableLayoutResult result;
	result.error = error;
	return result;
}

} // namespace

StaticExecutableLayoutResult PlanStaticExecutableLayout(std::span<const uint8_t> bytes) {
	using Error = StaticExecutableLayoutError;
	if (bytes.size() < HeaderSize) { return Failure(Error::TruncatedHeader); }
	if (bytes[EI_MAG0] != 0x7f || bytes[EI_MAG1] != 'E' || bytes[EI_MAG2] != 'L' ||
	    bytes[EI_MAG3] != 'F' || bytes[EI_CLASS] != ELFCLASS64 || bytes[EI_DATA] != ELFDATA2LSB ||
	    bytes[EI_VERSION] != EV_CURRENT || Read32(bytes, 20) != EV_CURRENT ||
	    Read16(bytes, 52) != HeaderSize || Read16(bytes, 54) != ProgramHeaderSize) {
		return Failure(Error::InvalidHeader);
	}
	// Both generic System V and FreeBSD ELF64 fixtures are supported. Other
	// executable types or architectures require their own loader contract.
	if ((bytes[EI_OSABI] != 0 && bytes[EI_OSABI] != ELFOSABI_FREEBSD) ||
	    Read16(bytes, 16) != ExecutableType || Read16(bytes, 18) != EM_X86_64) {
		return Failure(Error::UnsupportedExecutable);
	}
	const uint64_t phoff = Read64(bytes, 32);
	const uint16_t phnum = Read16(bytes, 56);
	if (phnum == ExtendedProgramHeaderCount || phoff < HeaderSize) { return Failure(Error::InvalidHeader); }
	const uint64_t table_size = uint64_t(phnum) * ProgramHeaderSize;
	if (AddOverflows(phoff, table_size)) { return Failure(Error::ArithmeticOverflow); }
	if (!Fits(phoff, table_size, bytes.size())) { return Failure(Error::TruncatedProgramHeaders); }

	StaticExecutableLayout plan;
	plan.entry = Read64(bytes, 24);
	for (uint16_t i = 0; i < phnum; ++i) {
		const auto pos = static_cast<size_t>(phoff + uint64_t(i) * ProgramHeaderSize);
		const auto type = Read32(bytes, pos);
		if (type == PT_DYNAMIC || type == InterpreterType || type == PT_OS_DYNLIBDATA) {
			return Failure(Error::UnsupportedDynamic);
		}
		if (type != PT_LOAD && type != PT_TLS) { continue; }

		const uint32_t flags = Read32(bytes, pos + 4);
		const uint64_t offset = Read64(bytes, pos + 8);
		const uint64_t address = Read64(bytes, pos + 16);
		const uint64_t file_size = Read64(bytes, pos + 32);
		const uint64_t memory_size = Read64(bytes, pos + 40);
		const uint64_t alignment = Read64(bytes, pos + 48);
		if (file_size > memory_size) { return Failure(Error::InvalidSegment); }
		if (alignment > 1 && (alignment & (alignment - 1)) != 0) { return Failure(Error::InvalidAlignment); }
		if (alignment > 1 && (address & (alignment - 1)) != (offset & (alignment - 1))) {
			return Failure(Error::IncongruentAlignment);
		}
		if (AddOverflows(address, memory_size) || AddOverflows(offset, file_size)) {
			return Failure(Error::ArithmeticOverflow);
		}
		if (file_size != 0 && !Fits(offset, file_size, bytes.size())) { return Failure(Error::TruncatedSegment); }

		if (type == PT_TLS) {
			if (plan.tls) { return Failure(Error::MultipleTlsSegments); }
			plan.tls = StaticExecutableTls {address, memory_size, offset, file_size, alignment};
			continue;
		}
		// Zero-sized PT_LOADs have no mapping or ownership extent.
		if (memory_size == 0) { continue; }
		const uint64_t end = address + memory_size;
		if (AddOverflows(end, StaticExecutablePageSize - 1)) { return Failure(Error::ArithmeticOverflow); }
		const uint64_t page_start = address & ~(StaticExecutablePageSize - 1);
		const uint64_t page_end = (end + StaticExecutablePageSize - 1) & ~(StaticExecutablePageSize - 1);
		plan.segments.push_back({address, memory_size, offset, file_size, page_start, page_end - page_start, flags});
	}
	if (plan.segments.empty()) { return Failure(Error::NoLoadSegments); }
	std::sort(plan.segments.begin(), plan.segments.end(), [](const auto& a, const auto& b) {
		return a.page_address < b.page_address;
	});
	bool entry_executable = false;
	for (size_t i = 0; i < plan.segments.size(); ++i) {
		const auto& segment = plan.segments[i];
		if (i != 0 && segment.page_address < plan.segments[i - 1].page_address + plan.segments[i - 1].page_size) {
			return Failure(Error::OverlappingLoadSegments);
		}
		if ((segment.flags & PF_X) != 0 && plan.entry >= segment.address &&
		    plan.entry - segment.address < segment.memory_size) {
			entry_executable = true;
		}
	}
	if (!entry_executable) { return Failure(Error::EntryNotExecutable); }
	if (plan.tls) {
		const auto& tls = *plan.tls;
		const bool covered = std::any_of(plan.segments.begin(), plan.segments.end(), [&](const auto& segment) {
			if (tls.address < segment.address) { return false; }
			const uint64_t relative = tls.address - segment.address;
			if (!Fits(relative, tls.memory_size, segment.memory_size)) { return false; }
			if (tls.file_size == 0) { return true; }
			return Fits(relative, tls.file_size, segment.file_size) &&
			       !AddOverflows(segment.file_offset, relative) &&
			       segment.file_offset + relative == tls.file_offset;
		});
		if (!covered) { return Failure(Error::TlsOutsideLoad); }
	}
	plan.owner_address = plan.segments.front().page_address;
	const auto& last = plan.segments.back();
	plan.owner_size = last.page_address + last.page_size - plan.owner_address;
	StaticExecutableLayoutResult result;
	result.layout = std::move(plan);
	return result;
}

} // namespace Loader
