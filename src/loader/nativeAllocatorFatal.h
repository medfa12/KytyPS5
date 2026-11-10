#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Loader::NativeAllocatorFatal {

struct CodeRange { uint64_t rva; size_t size; };
// Exact post-FS-store-adaptation windows in the audited 13.60 image.
inline constexpr std::array<CodeRange, 7> ProfileRanges {{
    {0x150b0, 21}, {0x12fa0, 7}, {0x11ab6, 24}, {0x120e9, 23},
    {0xf3dc, 28}, {0x66ab, 24}, {0x674f, 29},
}};
inline constexpr std::array<uint64_t, 8> ProfileSites {
    0x150c0, 0x150c3, 0x12fa5, 0x11ac6, 0x120f9, 0xf3ec, 0x66bb, 0x675f,
};
inline constexpr size_t ProfileExtent = 0x150c5;

// Load/unload callers serialize publication. Fault lookup is allocation-free,
// lock-free and never reads guest code or acquires the runtime-linker mutex.
// Each successful registration is published as one batch; rejection publishes
// nothing. The legacy three-site API and 96-slot admission limit are retained.
bool Register(const std::array<uint64_t, 3>& sites);
// Caller verifies provider identity and executable segment coverage first.
bool RegisterProfile(uint64_t base, std::span<const uint8_t> image);
bool ContainsProfile(uint64_t base);
void Unregister(uint64_t base, uint64_t size);
bool Contains(uint64_t pc);

// Audited LibcInternal helper after the existing FS-store host adaptation, and
// its invalid-free caller. Offsets are checked by the loader's provider binding.
bool MatchCode(std::span<const uint8_t> helper, std::span<const uint8_t> caller);
bool MatchProfile(std::span<const uint8_t> image);

} // namespace Loader::NativeAllocatorFatal
