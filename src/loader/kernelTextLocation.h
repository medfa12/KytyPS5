#pragma once

#include "common/abi.h"

#include <cstdint>

namespace Libs::LibKernel {

// Bounded native failure-path model. No success/bounds are exposed yet.
// Missing mapped owner provider => ENOENT; present/ambiguous => ENOSYS;
// unavailable/inactive process context => ESRCH. These are emulator policies.
// Like native's POSIX sysctl path, failure sets positive guest errno and
// returns its kernel encoding. Neither output is accessed on failure (the
// independent errno write remains observable if a caller aliases that slot).
int32_t KYTY_SYSV_ABI KernelGetLibkernelTextLocation(uint64_t* start, uint64_t* end);

// Native ignores the helper error and returns zero. Does not initialize out16.
int32_t KYTY_SYSV_ABI KernelInternalMemoryGetModuleSegmentInfo(void* out16);

} // namespace Libs::LibKernel
