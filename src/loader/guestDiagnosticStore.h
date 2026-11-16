#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace Loader {

// Existing host adaptation for the guest diagnostic-code store through FS:28.
// Windows cannot directly execute that guest FS access. Adapt only the store;
// following native INT45/UD2 traps must remain nonreturning, never RET/NOP them.
inline bool PatchGuestDiagnosticStore(std::span<uint8_t> code) {
	constexpr std::array<uint8_t, 8> prefix {0x64, 0xc7, 0x04, 0x25, 0x28, 0, 0, 0};
	if (code.size() < 12 || !std::equal(prefix.begin(), prefix.end(), code.begin())) { return false; }
	std::fill_n(code.begin(), 12, uint8_t {0x90});
	return true;
}

} // namespace Loader
