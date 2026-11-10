#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include "common/abi.h"
#endif

namespace Libs::KernelBacktrace {

inline constexpr size_t MaxFrames = 512;
inline constexpr size_t FrameBytes = 16;
inline constexpr size_t MaxFrameReadBytes = 8192;
inline constexpr size_t MaxPrefixBytes = 128;
inline constexpr size_t PresentationEdge = 100;
static_assert(MaxFrames * FrameBytes == MaxFrameReadBytes);

// Trusted host callback only; never a guest function or a global override.
// May partially modify destination even on failure. Both success and the exact
// byte count are required before any copied frame/prefix byte is consumed.
struct Reader {
	void* context;
	bool (*read)(void* context, uint64_t address, void* destination, size_t size, size_t& copied) noexcept;
};

enum class Stop { Complete, ReadFailed, Cycle, AddressOverflow, Limit, InvalidReader };

struct Capture {
	std::array<uint64_t, MaxFrames> pcs {};
	size_t count = 0;
	size_t requested_bytes = 0; // Includes failed attempts; never exceeds 8 KiB.
	uint64_t stop_frame = 0;
	Stop stop = Stop::Complete;
};

enum class PrefixStop { Complete, NullAddress, ReadFailed, AddressOverflow, Limit, InvalidReader };

struct Prefix {
	std::array<char, MaxPrefixBytes + 1> text {}; // Always locally NUL terminated.
	size_t length = 0;
	PrefixStop stop = PrefixStop::Complete;
};

// x86-64 little-endian {saved RBP, saved return PC} records. No monotonicity,
// alignment, executable-page or module assumptions; no second chain pass.
Capture Collect(uint64_t first_frame, const Reader& reader, size_t max_frames = MaxFrames) noexcept;
Prefix ReadPrefix(uint64_t address, const Reader& reader) noexcept;

} // namespace Libs::KernelBacktrace

#ifdef _WIN32
namespace Libs::LibKernel {

void KYTY_SYSV_ABI KernelBacktrace(const char* prefix);

} // namespace Libs::LibKernel
#endif
