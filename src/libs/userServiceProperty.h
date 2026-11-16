#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace Libs::UserService {
// Regular 13.60 UserService +28a0 -> +1930 copies 0xe8 bytes. +4ee0
// invokes slot +10 as (i32 user, u32 property, u64* value), property=6.
struct NativeBackendTable { std::array<uint64_t, 29> functions {}; };
static_assert(sizeof(NativeBackendTable) == 0xe8);

template <typename Invoke>
int ReadNativeProperty6(uint64_t getter, int user, int* output, Invoke&& invoke) {
	if (!output) { return static_cast<int>(0x80960005u); }
	if (!getter) { return static_cast<int>(0x80960002u); }
	uint64_t value = 0;
	const uint64_t arguments[] {static_cast<uint32_t>(user), 6, reinterpret_cast<uint64_t>(&value)};
	const int result = std::forward<Invoke>(invoke)(getter, arguments, 3);
	// Do not expose the native wrapper's uninitialized stack copy on error.
	if (result == 0) { *output = static_cast<int>(static_cast<uint32_t>(value)); }
	return result;
}
} // namespace Libs::UserService
