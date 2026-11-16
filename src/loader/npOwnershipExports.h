#pragma once

#include <array>
#include <string_view>

namespace Loader {

// Audited 13.60 NpCommon callback-driven ownership closure. Heap, mutex,
// thread and service exports must not become fallback providers with this mode.
inline constexpr std::array<std::string_view, 21> NpOwnershipExports {
    "0syNkhJANVw", "V75N47uYdQc", "orRb69nSo64", "bKMVqRcCQ1U",
    "05KEwpDf4Ls", "4il4PZAZOnQ", "iwDNdnEGyhI", "q2USyzLF4kI",
    "CnDHI7sU+l0", "1x0jThSUr4w", "0gdlCVNNHCI",
    "mzlILsFx0cU", "a2qdVU8RWb4", "VWcTu8wKwlQ", "kKF3w-XkCWA",
    "BztTl7QeYqE", "Zh23aSLeeZo", "c8-4aC9opYE", "Cmd4+m7V00c",
    "vqA9bl6WsF0", "EziLjfyTnKI",
};

inline bool IsNpOwnershipExport(std::string_view nid) {
	for (const auto allowed: NpOwnershipExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}

} // namespace Loader
