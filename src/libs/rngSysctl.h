#pragma once

#include "common/abi.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::RngSysctl {

inline constexpr size_t ResultSize = 64;
// Host safety policy, not a native sysctl length limit. Native range checking
// narrows length to signed32; the adapter rejects that ambiguous domain.
inline constexpr size_t MaxValidatedRange = INT32_MAX;
inline constexpr size_t MaxValidationRegions = 256;
inline constexpr size_t MaxLinuxMapLines = 4096;
using Bytes = std::array<uint8_t, ResultSize>;

// Trusted, non-null host callbacks. No guest callbacks or global test override.
// Copy/store may have partially written on failure; Run never rolls them back.
struct Callbacks {
	void* context;
	bool (*read_length)(void*, const size_t*, size_t&) noexcept;
	bool (*validate)(void*, const void*, size_t, bool write) noexcept;
	bool (*copy_output)(void*, void*, const void*, size_t) noexcept;
	bool (*store_length)(void*, size_t*, size_t) noexcept;
	bool (*entropy)(void*, Bytes&) noexcept;
};

// POSIX return: zero preserves guest errno, -1 sets positive guest errno.
// Only this isolated core accepts injection; production Sysctl always uses the
// fixed host adapter and its actual entropy source.
int Run(void* oldp, size_t* oldlenp, const void* newp, size_t newlen, const Callbacks& callbacks) noexcept;
int Sysctl(void* oldp, size_t* oldlenp, const void* newp, size_t newlen) noexcept;

// Numeric sysctl supports only kern.arandom {1,37}; other valid MIBs are
// unimplemented. Callbacks are trusted host functions; guest pointers are
// accessed only through checked copies and validation. Copy/store failures may
// have partially written. Range limits are host safety policies, not native
// huge-range or invalid zero-length pointer parity.
inline constexpr size_t ArandomMaxSize = 256;
inline constexpr size_t NumericMaxMib = 24;
struct NumericCallbacks {
	void* context;
	bool (*copy_mib)(void*, const void*, void*, size_t) noexcept;
	bool (*read_length)(void*, const size_t*, size_t&) noexcept;
	bool (*validate)(void*, const void*, size_t, bool write) noexcept;
	bool (*copy_output)(void*, void*, const void*, size_t) noexcept;
	bool (*store_length)(void*, size_t*, size_t) noexcept;
	bool (*entropy)(void*, uint8_t*, size_t) noexcept;
};

// Raw functions return 0 or a positive guest BSD errno and do not set guest
// errno. Arandom produces min(capacity, 256), including for null oldp;
// zero capacity returns ENXIO. A null oldlenp supplies zero capacity.
// NumericRun is the isolated injection seam; production uses host callbacks.
int NumericRun(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
               const void* newp, size_t newlen, const NumericCallbacks& callbacks) noexcept;
int NumericSysctlRaw(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
                     const void* newp, size_t newlen) noexcept;
// POSIX wrapper returns -1 and sets guest errno on failure; success preserves it.
int KYTY_SYSV_ABI NumericSysctl(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
                  const void* newp, size_t newlen) noexcept;

} // namespace Libs::RngSysctl
