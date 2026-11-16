#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Loader {

enum class AuthorityMetadataProvenance { SELFContainer, RetainedExtendedInfo };

struct AuthorityMetadata {
	uint64_t authority_id = 0;
	uint64_t program_type = 0;
	uint64_t firmware_version = 0;
	uint64_t application_version = 0;
	std::array<uint8_t, 32> digest {};
	std::array<uint8_t, 64> raw_record {};
	uint64_t file_offset = 0; // Offset in the original input, including a SELF wrapper.
	AuthorityMetadataProvenance provenance = AuthorityMetadataProvenance::RetainedExtendedInfo;
	bool operator==(const AuthorityMetadata&) const = default;
};

// Recognition is deliberately limited to the audited 13.60 extended-info profile.
// The prefix is actual file bytes, not a zero-filled substitute for a short read.
inline constexpr size_t AuthorityMetadataHeaderLimit = 64 * 1024;

[[nodiscard]] inline std::optional<AuthorityMetadata> ReadAuthorityMetadata(
    std::span<const uint8_t> prefix, uint64_t file_size) {
	if (prefix.size() > AuthorityMetadataHeaderLimit) { prefix = prefix.first(AuthorityMetadataHeaderLimit); }
	if (file_size < prefix.size()) { prefix = prefix.first(static_cast<size_t>(file_size)); }
	auto fits = [](uint64_t offset, uint64_t length, uint64_t limit) {
		return offset <= limit && length <= limit - offset;
	};
	auto u16 = [&](size_t p) -> uint16_t { return uint16_t(prefix[p]) | (uint16_t(prefix[p + 1]) << 8); };
	auto u32 = [&](size_t p) -> uint32_t {
		uint32_t value = 0;
		for (size_t i = 0; i < 4; ++i) { value |= uint32_t(prefix[p + i]) << (8 * i); }
		return value;
	};
	auto u64 = [&](size_t p) -> uint64_t {
		uint64_t value = 0;
		for (size_t i = 0; i < 8; ++i) { value |= uint64_t(prefix[p + i]) << (8 * i); }
		return value;
	};
	if (!fits(0, 64, prefix.size())) { return std::nullopt; }
	size_t elf = 0;
	uint16_t self_count = 0;
	uint64_t self_header_end = 0;
	const bool self = u32(0) == 0x1d3d154f || u32(0) == 0xeef51454;
	if (self) {
		// Known SELF header variants, including the local updatemode container.
		constexpr std::array<uint8_t, 8> older {0, 1, 1, 0x12, 1, 1, 0, 0};
		constexpr std::array<uint8_t, 8> newer {0x10, 1, 1, 0x12, 1, 1, 0, 0x10};
		constexpr std::array<uint8_t, 8> update {0x10, 1, 1, 0x32, 1, 7, 0, 0x10};
		auto tail = [&](const auto& expected) { return std::equal(expected.begin(), expected.end(), prefix.begin() + 4); };
		if (!((u32(0) == 0x1d3d154f && tail(older) && u16(26) == 0x22) ||
		      (u32(0) == 0xeef51454 && tail(newer) && u16(26) == 0x32) ||
		      (u32(0) == 0xeef51454 && tail(update) && u16(26) == 0x52)) || u32(28) != 0) {
			return std::nullopt;
		}
		self_count = u16(24);
		if (self_count == 0 || !fits(32, uint64_t(self_count) * 32, prefix.size())) { return std::nullopt; }
		elf = 32 + size_t(self_count) * 32;
		self_header_end = uint64_t(u16(12)) + u16(14);
		if (u64(16) > file_size || self_header_end > u64(16) || u16(12) < elf) { return std::nullopt; }
	}
	if (!fits(elf, 64, prefix.size())) { return std::nullopt; }
	constexpr std::array<uint8_t, 8> ident {0x7f, 'E', 'L', 'F', 2, 1, 1, 9};
	if (!std::equal(ident.begin(), ident.end(), prefix.begin() + elf) || prefix[elf + 8] != 2 ||
	    !std::all_of(prefix.begin() + elf + 9, prefix.begin() + elf + 16, [](uint8_t b) { return b == 0; }) ||
	    (u16(elf + 16) != 0xfe10 && u16(elf + 16) != 0xfe18 && !(self && u16(elf + 16) == 0xfe00)) ||
	    u16(elf + 18) != 62 || u32(elf + 20) != 1 || u32(elf + 48) != 0 ||
	    u16(elf + 52) != 64 || u64(elf + 32) != 64 || u16(elf + 54) != 56 ||
	    u64(elf + 40) != 0 || u16(elf + 58) != 0 || u16(elf + 60) != 0 || u16(elf + 62) != 0) {
		return std::nullopt;
	}
	const uint16_t count = u16(elf + 56);
	const size_t table = elf + 64;
	const size_t table_size = size_t(count) * 56;
	if (count == 0 || !fits(table, table_size, prefix.size())) { return std::nullopt; }
	const size_t record = table + table_size;
	if (!fits(record, 64, prefix.size()) || (self && !fits(record, 64, u16(12)))) { return std::nullopt; }
	const uint64_t logical_record = record - elf;
	bool has_load = false;
	for (size_t i = 0; i < count; ++i) {
		const size_t p = table + i * 56;
		const uint64_t offset = u64(p + 8), length = u64(p + 32);
		if (length == 0) { continue; }
		if (!fits(offset, length, std::numeric_limits<uint64_t>::max()) || (!self && offset > file_size)) {
			return std::nullopt;
		}
		// A retained record must be in the header gap, outside every file-backed
		// segment, not merely outside PT_LOAD. SELF uses logical ELF offsets here.
		if (offset < logical_record + 64 && offset + length > logical_record) { return std::nullopt; }
		if (u32(p) == 1) {
			if (offset < logical_record + 64 || (!self && !fits(offset, length, file_size))) { return std::nullopt; }
			has_load = true;
		}
	}
	if (!has_load) { return std::nullopt; }
	if (self) {
		bool has_payload = false;
		for (size_t i = 0; i < self_count; ++i) {
			const size_t p = 32 + i * 32;
			const uint64_t offset = u64(p + 8), length = u64(p + 16);
			if (length != 0 && (offset < self_header_end || !fits(offset, length, u64(16)))) { return std::nullopt; }
			has_payload |= length != 0;
		}
		if (!has_payload) { return std::nullopt; }
	}
	// This tuple identifies the known extended-info profile, independently of
	// authority ID and filename. Other versions/types remain unknown.
	if (u64(record + 8) != 8 || u64(record + 16) != UINT64_C(0x1360000000000000) ||
	    u64(record + 24) != UINT64_C(0x1360000000000000)) { return std::nullopt; }
	AuthorityMetadata result;
	result.authority_id = u64(record);
	result.program_type = u64(record + 8);
	result.firmware_version = u64(record + 16);
	result.application_version = u64(record + 24);
	std::copy_n(prefix.begin() + record + 32, result.digest.size(), result.digest.begin());
	std::copy_n(prefix.begin() + record, result.raw_record.size(), result.raw_record.begin());
	result.file_offset = record;
	result.provenance = self ? AuthorityMetadataProvenance::SELFContainer : AuthorityMetadataProvenance::RetainedExtendedInfo;
	return result;
}

} // namespace Loader
