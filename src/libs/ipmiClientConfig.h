#pragma once

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Libs::IpmiClientAbi {

// 13.60 libSceIpmi: Config ctor +f40, estimateClientMemorySize +fd0.
// Config extent and client-object backing extent are distinct contracts.
inline constexpr size_t ConfigSize = 0x158;
inline constexpr size_t ClientMemorySize = 0x1000;

// Use byte copies: firmware callers need not provide host struct alignment.
// Only fields written by the original constructor are changed; name/padding
// canaries must not become accidental ABI fields.
inline void KYTY_SYSV_ABI Construct(void* storage) {
	if (!storage) { return; }
	auto* bytes = static_cast<uint8_t*>(storage);
	const auto put = [bytes](size_t at, uint64_t value, size_t width) {
		for (size_t n = 0; n < width; ++n) { bytes[at + n] = static_cast<uint8_t>(value >> (8 * n)); }
	};
	bytes[0] = 0;
	bytes[0x19] = 0;
	put(0x1c, 0, 4);
	put(0x20, 1, 4);
	put(0x28, 0xf00, 8);
	put(0x30, 0xf00, 8);
	put(0x38, 1, 4);
	put(0x40, 0xf00, 8);
	put(0x48, 0xf00, 8);
	put(0x50, 0, 4);
	std::memset(bytes + 0x58, 0, 0x100);
}

inline uint64_t KYTY_SYSV_ABI Estimate(const void*) { return ClientMemorySize; }

} // namespace Libs::IpmiClientAbi
