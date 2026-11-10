#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Loader {

// This is a plan for a plain ELF file, not a SELF decoder or an executable loader.
// All addresses are guest virtual addresses and all ranges are half-open.
inline constexpr uint64_t StaticExecutablePageSize = 0x4000;

enum class StaticExecutableLayoutError {
	None,
	TruncatedHeader,
	InvalidHeader,
	UnsupportedExecutable,
	TruncatedProgramHeaders,
	ArithmeticOverflow,
	UnsupportedDynamic,
	InvalidSegment,
	InvalidAlignment,
	IncongruentAlignment,
	TruncatedSegment,
	NoLoadSegments,
	OverlappingLoadSegments,
	EntryNotExecutable,
	MultipleTlsSegments,
	TlsOutsideLoad,
};

struct StaticExecutableSegment {
	uint64_t address = 0;
	uint64_t memory_size = 0;
	uint64_t file_offset = 0;
	uint64_t file_size = 0;
	uint64_t page_address = 0;
	uint64_t page_size = 0;
	uint32_t flags = 0; // ELF PF_R/PF_W/PF_X; not host protection flags.
};

struct StaticExecutableTls {
	uint64_t address = 0;
	uint64_t memory_size = 0;
	uint64_t file_offset = 0;
	uint64_t file_size = 0;
	uint64_t alignment = 0;
};

struct StaticExecutableLayout {
	uint64_t load_bias = 0;
	uint64_t entry = 0;
	// Page-aligned enclosing reservation; gaps are not load segments.
	uint64_t owner_address = 0;
	uint64_t owner_size = 0;
	// Sorted by guest address. Each segment's file bytes are followed by
	// memory_size - file_size zero bytes; aligned page ranges never overlap.
	std::vector<StaticExecutableSegment> segments;
	std::optional<StaticExecutableTls> tls;
};

struct StaticExecutableLayoutResult {
	StaticExecutableLayoutError error = StaticExecutableLayoutError::None;
	StaticExecutableLayout layout;
};

// Deterministic bounded plain-ELF byte-layout planning; on failure layout is empty.
// PT_LOAD and PT_TLS are validated. PT_DYNAMIC, PT_INTERP and PT_OS_DYNLIBDATA
// are rejected; other program headers are ignored. LOAD flags are preserved,
// including unknown bits; only PF_X is used here to check the entry's extent.
// Success is not runtime admission or a complete protection policy. An eventual
// mapper must validate supported auxiliary headers/flags, guest/host address
// admissibility, collisions and platform protections before allocating anything.
// No mapping, file access, SELF decoding, relocation or guest execution occurs.
[[nodiscard]] StaticExecutableLayoutResult PlanStaticExecutableLayout(std::span<const uint8_t> bytes);

} // namespace Loader
