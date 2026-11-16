#pragma once

#include <cstdint>

namespace Libs::StandardWrite {

struct Result {
	int64_t count = -1;
	int error = 0; // Guest POSIX errno, only meaningful when count < 0.
};

// The emulator's standard descriptors are stdin(read), stdout(write), stderr(write).
// Writes bytes to the actual host endpoint, returning its count (including short
// writes). No printf formatting, buffering/flush success, or guest errno writes.
Result Write(int descriptor, const void* bytes, uint64_t length);

} // namespace Libs::StandardWrite
