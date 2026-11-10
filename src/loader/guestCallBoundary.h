// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_GUEST_CALL_BOUNDARY_H_
#define KYTY_LOADER_GUEST_CALL_BOUNDARY_H_

#include <cstdint>
#include <atomic>
#include <memory>

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#define KYTY_GUEST_CALL_BOUNDARY_SYSV __attribute__((sysv_abi))
#else
#define KYTY_GUEST_CALL_BOUNDARY_SYSV
#endif

namespace Loader {

struct GuestCallOwner {
	const void* identity = nullptr;
	uint64_t token = 0; // Nonzero application-lifetime token, not a module handle.
};

enum class GuestCallBoundaryStatus { Ok, Unsupported, InvalidArgument, OutOfMemory, CodeAllocationFailed };
enum class GuestCallOutcomeTag { NotInvoked, Returned, Aborted, Stopped };
struct GuestCallOutcome {
	GuestCallBoundaryStatus status = GuestCallBoundaryStatus::Unsupported;
	GuestCallOutcomeTag tag = GuestCallOutcomeTag::NotInvoked;
	uint64_t value = 0; // Meaningful ONLY for Returned; negative integers retain their bits.
};

using GuestBoundaryFunction = uint64_t (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(void*);
// A valid abort never returns. Rejection returns zero WITHOUT changing the
// active boundary. This is transition control, not an unresolved API result.
using GuestBoundaryAbortFunction = uint32_t (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(const void*, uint64_t);
using GuestBoundaryFailureFunction = void (KYTY_GUEST_CALL_BOUNDARY_SYSV *)();

// Stable native-thread incarnation storage. Bind/unbind on the actual thread;
// remote inspection ONLY after a pinned native handle has been suspended and
// GetThreadContext completed. Never retain top beyond that suspension/landing.
struct GuestBoundaryStopState {
	std::atomic<void*> top {nullptr};
	std::atomic<unsigned> host_boundary_depth {0};
	std::atomic<bool> in_flight {false};
	uint64_t sequence = 0; // Actual thread only; no wrap/reuse within an incarnation.
};
struct GuestBoundaryStopLanding {
	uint64_t entry = 0;
	uint64_t frame = 0; // Trusted R11 input to the call-free arbitrary-RIP entry.
	explicit operator bool() const noexcept { return entry && frame; }
};

// Synchronous, same-stack recovery for Windows x86-64 GNU/Clang ABI builds.
// Create also rejects an unavailable mitigation query or enabled CET shadow
// stacks; no hardware shadow-stack adjustment/unwind support is assumed.
//
// CRITICAL CONTRACT: Invoke surrounds an ACTUAL host-to-guest call. Between the
// failing guest instruction and this innermost boundary there may be guest
// frames and trivial transition frames ONLY. Every intervening host-to-guest
// callback needs its own boundary. After inner recovery, host operations return
// normally (destroying their RAII); only a trivial guest-facing epilogue may
// then call Abort again to propagate to an older guest segment. Never abort
// from a resolver while its host objects, locks or callers' host RAII are live.
// Host C++ running on a guest stack is still host C++, NOT a discardable segment.
//
// No stack switching, C++/SEH exception propagation, guest unwind metadata,
// asynchronous signal/APC recovery or lifecycle rollback is provided. The
// separate async-stop landing requires the controller's pinned native thread,
// original-stack/private-text proof and incompatible-transition exclusions.
// Guest must not alter TEB, host TLS, mappings or saved
// boundary storage. Normal return obeys SysV nonvolatile GPR/stack rules.
// Caller supplies sufficient committed stack/headroom and valid platform stack
// bounds for the gateway and synchronous validator/TLS runtime. No unwind
// tables are registered for the emitted code; exceptions must not cross it.
// Abort may discard guest register/FP/DF changes; the saved host GPRs,
// XMM0-15/legacy x87 state, MXCSR and entry flags are restored without unwinding.
// Those flags are the low-level entry's flags; C++ Invoke itself follows the
// host ABI and does not promise caller arithmetic-flag preservation.
// Upper YMM/ZMM state is volatile in the supported Windows ABI, not preserved.
// TF must be clear. The abort gateway clears DF and establishes masked default
// FP controls before its integer-only host validation call, after which no
// host frame remains live above the guest segment being discarded.
//
// The object and owner identity/token must remain pinned for every invocation
// and use of its AbortFunction. Concurrent/nested invocations are permitted;
// process mitigation policy must remain unchanged during their lifetime.
// boundary stacks are TLS of the ACTUAL executing host thread. The gateway
// accepts only that object's top boundary and exact owner/token, never scans
// for an older matching owner and never trusts a guest-supplied continuation.
// This is a cooperative correctness boundary, not a memory security sandbox.
class GuestCallBoundary final {
public:
	static std::unique_ptr<GuestCallBoundary> Create(GuestCallOwner owner, GuestCallBoundaryStatus* status = nullptr) noexcept;
	~GuestCallBoundary();
	GuestCallBoundary(const GuestCallBoundary&) = delete;
	GuestCallBoundary& operator=(const GuestCallBoundary&) = delete;
	GuestCallOutcome Invoke(GuestBoundaryFunction guest, void* argument) noexcept;
	GuestBoundaryAbortFunction AbortFunction() const noexcept;
	// Resolver handoff: getter returns null unless THIS owner/object is the
	// actual thread's active innermost boundary. Returned entry takes no args,
	// can be tail-jumped to with the original unknown API arguments intact, and
	// never returns. The resolver must return normally before that transfer.
	// Entry revalidates TLS/owner/token. Without a matching boundary, invocation
	// traps with UD2, NOT an app-local termination fallback. Entry is owner-bound,
	// not a per-invocation capability: caching across later same-owner segments
	// cannot be detected. Do not cache or hand it to another thread.
	GuestBoundaryFailureFunction FailureFunction() const noexcept;
	static unsigned CurrentDepth() noexcept;
	static void BindStopState(GuestBoundaryStopState* state) noexcept;
	static void PublishHostDepth(unsigned depth) noexcept;
	// Caller has proved exact owner-private PC, clear host-critical protection,
	// unsupported signal/context/fiber exclusion, and pinned original guest stack.
	GuestBoundaryStopLanding PrepareAsyncStop(const GuestBoundaryStopState& state, uint64_t native_thread,
	    uint64_t rsp, uint64_t stack_begin, uint64_t stack_end) const noexcept;

private:
	GuestCallBoundary() noexcept;
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace Loader
#endif
