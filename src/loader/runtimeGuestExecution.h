// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_RUNTIME_GUEST_EXECUTION_H_
#define KYTY_LOADER_RUNTIME_GUEST_EXECUTION_H_

#include "loader/guestCallBoundary.h"
#include "loader/hostCallReturnThunk.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

namespace Loader {
struct Program;

// BasicLockable with per-actual-thread host-critical accounting. In contrast
// to a guard surrounding a wait, unlock/relock drops protection while parked.
class GuestExecutionMutex final {
public:
	void lock();
	void unlock();
private:
	std::mutex native_;
};

enum class GuestExecutionState { Running, StopRequested, Failed, Draining, Retired };
enum class GuestWorkKind { Guest, Thread, Module, Callback, HostCall, Count };
enum class GuestFailureReason { None, GuestAborted, BoundaryUnavailable, InitializerRejected, TransitionCapacity, InvalidTransition, Explicit };
struct GuestExecutionFailure {
	GuestFailureReason reason = GuestFailureReason::None;
	uint64_t lifetime = 0;
	uint64_t detail = 0;
	explicit operator bool() const noexcept { return reason != GuestFailureReason::None; }
};
struct GuestThreadExitOutcome {
	uint64_t lifetime = 0;
	uint64_t value = 0;
	explicit operator bool() const noexcept { return lifetime != 0; }
};
struct GuestStopTextRange { uint64_t begin = 0; uint64_t end = 0; };
struct GuestStopText {
	GuestCallOwner owner;
	std::vector<GuestStopTextRange> ranges; // Exact owner-private executable spans, [begin,end).
	bool Contains(uint64_t pc) const noexcept {
		for (const auto& range: ranges) { if (pc >= range.begin && pc < range.end) { return true; } }
		return false;
	}
};
struct RuntimeGuestResult {
	GuestCallOutcome call;
	GuestExecutionFailure failure;
	bool stopped = false;
	GuestThreadExitOutcome thread_exit;
	bool Returned() const noexcept { return call.tag == GuestCallOutcomeTag::Returned && !failure && !stopped && !thread_exit; }
	bool ThreadExited() const noexcept { return bool(thread_exit) && !failure && !stopped; }
};

// Lifetime-scoped shared-application control. RequestStop closes admission;
// PthreadRequestOwnerStop separately handles eligible native guest segments.
// Host waits still need cancellation integration. Counts alone never prove
// native thread termination or waive unsupported transition admission.
class RuntimeGuestExecution final: public std::enable_shared_from_this<RuntimeGuestExecution> {
public:
	class Lease final {
	public:
		Lease() noexcept = default;
		~Lease();
		Lease(Lease&& other) noexcept;
		Lease& operator=(Lease&& other) noexcept;
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		explicit operator bool() const noexcept { return owner_ != nullptr; }
		void Reset() noexcept;
		// Atomically linearize a SHORT HOST-ONLY publication against stop/fail.
		// No guest calls, blocking operations, Fail/RequestStop, or owner API
		// recursion inside commit. Caller may already hold its registry lock;
		// the control never acquires those external locks itself.
		template <typename Commit> bool Publish(Commit&& commit) {
			if (!owner_) { return false; }
			std::lock_guard lock(owner_->mutex_);
			if (owner_->state_.load(std::memory_order_relaxed) != GuestExecutionState::Running) { return false; }
			std::forward<Commit>(commit)();
			return true;
		}
	private:
		friend class RuntimeGuestExecution;
		Lease(std::shared_ptr<RuntimeGuestExecution> owner, GuestWorkKind kind) noexcept:
		    owner_(std::move(owner)), kind_(kind) {}
		std::shared_ptr<RuntimeGuestExecution> owner_;
		GuestWorkKind kind_ = GuestWorkKind::Guest;
	};
	static std::shared_ptr<RuntimeGuestExecution> Create(const void* identity,
	    GuestCallBoundaryStatus* status = nullptr) noexcept;
	GuestCallOwner Owner() const noexcept { return owner_; }
	GuestExecutionState State() const noexcept { return state_.load(std::memory_order_acquire); }
	bool StopRequested() const noexcept { return State() != GuestExecutionState::Running; }
	GuestExecutionFailure Failure() const noexcept;
	GuestExecutionFailure Fail(GuestFailureReason reason, uint64_t detail = 0) noexcept;
	void RequestStop() noexcept;
	// HOST loader publication only. Copies immutable PT_LOAD/PF_X proof; never
	// include generated HOST transitions or shared providers. Parent must retain
	// all published mappings through owner drain. Getter used BEFORE suspension.
	bool PublishStopText(std::shared_ptr<const GuestStopText> text) noexcept;
	std::shared_ptr<const GuestStopText> StopText() const noexcept { return stop_text_.load(std::memory_order_acquire); }
	void BeginDraining() noexcept;
	bool Retire() noexcept; // Only closed admission + zero outstanding leases.
	bool WaitForDrain(std::chrono::milliseconds timeout);
	size_t Outstanding() const noexcept;
	Lease TryAcquire(GuestWorkKind kind);
	RuntimeGuestResult Invoke(GuestBoundaryFunction guest, void* argument) noexcept;
	// No C++ frame/RAII below this boundary. Supports SysV void(void*), void(),
	// uint64_t(void*) and uint64_t(); void results are explicitly normalized.
	RuntimeGuestResult InvokeCallback(uint64_t target, void* argument = nullptr, bool returns_value = false) noexcept;
	// Trusted host argument array; current-thread, nonvariadic SysV integer/pointer
	// callback only. No floating-point, aggregate or hidden-return ABI support.
	RuntimeGuestResult InvokeIntegerCallback(uint64_t target, const uint64_t* arguments,
	                                       size_t count) noexcept;
	// Request only at this owner's active innermost guest boundary. Does not
	// stop/fail peers. The caller must return normally before transferring.
	bool RequestThreadExit(uint64_t value) noexcept;
	GuestThreadExitOutcome PendingThreadExit() const noexcept;
	// HOST WORKER ROOT ONLY, after all guest boundaries/gateways have returned.
	// Coordinators must inspect ThreadExited(), return normally, and NOT consume.
	GuestThreadExitOutcome ConsumeThreadExit() noexcept;
	bool RegisterThreadDestructor(uint64_t target) noexcept;
	uint64_t ThreadDestructor() const noexcept { return thread_destructor_.load(std::memory_order_acquire); }
	// Only a trivial transition may be discarded: this adapter tail-jumps
	// directly to SysV int(size_t,const void*,callback), without a host frame.
	RuntimeGuestResult InvokeModule(uint64_t target, size_t args, const void* argp, uint64_t callback) noexcept;
	// Executable entry on the caller's existing guest stack. The disposable
	// transition supplies a zero-terminated frame root and (params, atexit).
	// A void entry return is normalized to zero; abort remains a tagged outcome.
	RuntimeGuestResult InvokeEntry(uint64_t target, void* params, uint64_t atexit) noexcept;
	GuestBoundaryFailureFunction FailureFunction() const noexcept;
	GuestBoundaryAbortFunction AbortFunction() const noexcept;
	// Borrowed immutable gateway construction input. The caller must pin this
	// control, gateway/context and target mappings through the final epilogue.
	GuestCallBoundary* Boundary() const noexcept { return boundary_.get(); }
	// True only for the currently executing host call of this innermost guest
	// segment. An older gateway surrounding a nested callback is not a match.
	bool HasHostCallForCurrentBoundary() const noexcept;
	// HostCallReturnThunk callbacks; context is this exact control (not Program
	// or borrowed pthread state). Actual-thread TLS stores move-only HostCall
	// leases in a bounded LIFO. Refusal/leave always returns normally; ONLY the
	// generated gateway performs the eventual transfer after host RAII is gone.
	static HostCallDisposition KYTY_GUEST_CALL_BOUNDARY_SYSV EnterHostCall(
	    void* context, uint64_t guest_return, HostCallEnterReason reason) noexcept;
	static HostCallDisposition KYTY_GUEST_CALL_BOUNDARY_SYSV LeaveHostCall(void* context, uint64_t guest_return) noexcept;
private:
	RuntimeGuestExecution() = default;
	GuestCallOwner owner_;
	std::unique_ptr<GuestCallBoundary> boundary_;
	mutable GuestExecutionMutex mutex_;
	std::condition_variable_any changed_;
	std::atomic<GuestExecutionState> state_ {GuestExecutionState::Running};
	std::atomic<uint64_t> thread_destructor_ {0};
	std::atomic<std::shared_ptr<const GuestStopText>> stop_text_;
	GuestExecutionFailure failure_;
	std::array<size_t, static_cast<size_t>(GuestWorkKind::Count)> counts_ {};
};

// Explicit opt-in; Program's constructor leaves session programs untouched.
// Call before owner publication; never replace a control for a live lifetime.
bool InitializeApplicationExecution(Program* owner, GuestCallBoundaryStatus* status = nullptr) noexcept;
std::shared_ptr<RuntimeGuestExecution> GetApplicationExecution(const Program* program) noexcept;
} // namespace Loader
#endif
