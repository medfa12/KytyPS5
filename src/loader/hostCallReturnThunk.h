// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_HOST_CALL_RETURN_THUNK_H_
#define KYTY_LOADER_HOST_CALL_RETURN_THUNK_H_

#include "loader/guestCallBoundary.h"

#include <memory>

namespace Loader {

enum class HostCallDisposition : uint32_t { Proceed, Abort };
enum class HostCallEnterReason : uint32_t { Call, CapacityExceeded };
// Both callbacks are HOST calls: return normally, including all local RAII.
// Abort requests the bound boundary's failure gateway, never an API result.
// Enter/leave must implement the owner's sticky failure/admission protocol.
// CapacityExceeded must latch owner failure and return Abort, without acquiring
// a target-execution lease or recursively entering another gateway.
using HostCallEnter = HostCallDisposition (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(
    void* context, uint64_t guest_return, HostCallEnterReason reason) noexcept;
using HostCallLeave = HostCallDisposition (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(void* context, uint64_t guest_return) noexcept;

enum class HostCallAbi { BaselineSysV, MicrosoftX64, Avx, Avx512 };
enum class HostCallReturnThunkStatus { Ok, Unsupported, InvalidArgument, OutOfMemory, CodeAllocationFailed };

// Guest -> known typed SysV HLE -> guest, with a post-HLE owner-failure barrier.
// Windows x86-64 GNU/Clang only; rejects enabled/unqueryable CET shadow stacks.
// No exceptions, longjmp, thread exit, stack switching or coroutine suspension
// may cross the target or callbacks. Every HLE -> guest callback needs its own
// actual GuestCallBoundary, even when it uses the same owner/boundary object.
// See hostCallReturnThunk.md for ABI, bounded-depth and lifecycle requirements.
class HostCallReturnThunk final {
public:
	static std::unique_ptr<HostCallReturnThunk> Create(
	    GuestCallBoundary* boundary, uint64_t typed_target, void* context,
	    HostCallEnter enter, HostCallLeave leave, HostCallAbi abi = HostCallAbi::BaselineSysV,
	    HostCallReturnThunkStatus* status = nullptr) noexcept;
	~HostCallReturnThunk();
	HostCallReturnThunk(const HostCallReturnThunk&) = delete;
	HostCallReturnThunk& operator=(const HostCallReturnThunk&) = delete;
	uint64_t GetAddress() const noexcept;
	// Actual executing host thread, not emulated/borrowed pthread identity.
	static unsigned CurrentDepth() noexcept;
	static constexpr unsigned MaximumDepth = 256;

private:
	HostCallReturnThunk() noexcept;
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace Loader
#endif
