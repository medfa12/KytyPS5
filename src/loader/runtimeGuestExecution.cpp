// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/runtimeGuestExecution.h"
#include "loader/runtimeLinker.h"

#include <limits>
#include <cstddef>
#include <new>

namespace Loader {
void GuestExecutionMutex::lock() {
	auto& counter = *Common::GetHostLockCriticalCounter();
	counter.fetch_add(1, std::memory_order_seq_cst);
	try { native_.lock(); }
	catch (...) { counter.fetch_sub(1, std::memory_order_seq_cst); throw; }
}
void GuestExecutionMutex::unlock() {
	native_.unlock();
	Common::GetHostLockCriticalCounter()->fetch_sub(1, std::memory_order_seq_cst);
}
namespace {
std::atomic<uint64_t> next_lifetime {1};
struct HostCallLease {
	RuntimeGuestExecution* execution = nullptr;
	uint64_t guest_return = 0;
	unsigned boundary_depth = 0;
	RuntimeGuestExecution::Lease lease;
};
struct HostCalls {
	std::array<HostCallLease, HostCallReturnThunk::MaximumDepth> calls;
	unsigned depth = 0;
};
thread_local HostCalls host_calls;
struct ThreadExitRequest {
	GuestCallOwner owner;
	GuestThreadExitOutcome outcome;
};
thread_local ThreadExitRequest thread_exit_request;
struct CallbackArguments {
	uint64_t target;
	void* argument;
};
struct IntegerCallbackArguments {
	uint64_t target;
	std::array<uint64_t, 7> values;
};
struct ModuleArguments {
	uint64_t target;
	size_t args;
	const void* argp;
	uint64_t callback;
};
struct EntryArguments {
	uint64_t target;
	void* params;
	uint64_t atexit;
};
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
static_assert(offsetof(CallbackArguments, argument) == 8);
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV CallbackTransition(void*) {
	__asm__ volatile("movq 0(%rdi), %rax\n\t"
	                 "movq 8(%rdi), %rdi\n\t"
	                 "jmp *%rax\n\t");
}
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV VoidCallbackTransition(void*) {
	__asm__ volatile("movq 0(%rdi), %rax\n\t"
	                 "movq 8(%rdi), %rdi\n\t"
	                 "subq $8, %rsp\n\t"
	                 "call *%rax\n\t"
	                 "addq $8, %rsp\n\t"
	                 "xorl %eax, %eax\n\t"
	                 "ret\n\t");
}
#if !defined(_M_ARM64EC)
static_assert(offsetof(IntegerCallbackArguments, values) == 8 && sizeof(IntegerCallbackArguments) == 64);
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV IntegerCallbackTransition(void*) {
	// Entry RSP is 8 mod 16. Reserve arg7 and align RSP for call; the target
	// sees its seventh argument at RSP+8, immediately above its return address.
	__asm__ volatile("movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "subq $8, %rsp\n\t"
	                 "movq 56(%r10), %r11\n\t"
	                 "movq %r11, 0(%rsp)\n\t"
	                 "movq 8(%r10), %rdi\n\t"
	                 "movq 16(%r10), %rsi\n\t"
	                 "movq 24(%r10), %rdx\n\t"
	                 "movq 32(%r10), %rcx\n\t"
	                 "movq 40(%r10), %r8\n\t"
	                 "movq 48(%r10), %r9\n\t"
	                 "call *%rax\n\t"
	                 "addq $8, %rsp\n\t"
	                 "ret\n\t");
}
#endif
static_assert(sizeof(size_t) == 8 && offsetof(ModuleArguments, callback) == 24);
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV ModuleTransition(void*) {
	__asm__ volatile("movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "movq 8(%r10), %rdi\n\t"
	                 "movq 16(%r10), %rsi\n\t"
	                 "movq 24(%r10), %rdx\n\t"
	                 "jmp *%rax\n\t");
}
static_assert(offsetof(EntryArguments, params) == 8 && offsetof(EntryArguments, atexit) == 16);
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV EntryTransition(void*) {
	// No host C++ ownership lies below the boundary. Preserve the caller's RBP
	// only for ordinary return; recovery restores the boundary's saved context.
	__asm__ volatile("pushq %rbp\n\t"
	                 "subq $16, %rsp\n\t"
	                 "movq $0, 0(%rsp)\n\t"
	                 "movq $0, 8(%rsp)\n\t"
	                 "movq %rsp, %rbp\n\t"
	                 "movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "movq 8(%r10), %rdi\n\t"
	                 "movq 16(%r10), %rsi\n\t"
	                 "call *%rax\n\t"
	                 "xorl %eax, %eax\n\t"
	                 "addq $16, %rsp\n\t"
	                 "popq %rbp\n\t"
	                 "ret\n\t");
}
#endif
}

std::shared_ptr<RuntimeGuestExecution> RuntimeGuestExecution::Create(const void* identity,
    GuestCallBoundaryStatus* status) noexcept {
	if (status) { *status = GuestCallBoundaryStatus::InvalidArgument; }
	if (!identity) { return {}; }
	uint64_t token = next_lifetime.load(std::memory_order_relaxed);
	do {
		if (token == std::numeric_limits<uint64_t>::max()) { return {}; } // Never reuse/wrap a generation.
	} while (!next_lifetime.compare_exchange_weak(token, token + 1, std::memory_order_relaxed));
	try {
		auto result = std::shared_ptr<RuntimeGuestExecution>(new RuntimeGuestExecution());
		result->owner_ = {identity, token};
		result->boundary_ = GuestCallBoundary::Create(result->owner_, status);
		if (!result->boundary_) { return {}; }
		return result;
	} catch (...) {
		if (status) { *status = GuestCallBoundaryStatus::OutOfMemory; }
		return {};
	}
}

RuntimeGuestExecution::Lease::~Lease() { Reset(); }
RuntimeGuestExecution::Lease::Lease(Lease&& other) noexcept:
    owner_(std::move(other.owner_)), kind_(other.kind_) {}
RuntimeGuestExecution::Lease& RuntimeGuestExecution::Lease::operator=(Lease&& other) noexcept {
	if (this != &other) { Reset(); owner_ = std::move(other.owner_); kind_ = other.kind_; }
	return *this;
}
void RuntimeGuestExecution::Lease::Reset() noexcept {
	if (!owner_) { return; }
	auto owner = std::move(owner_);
	std::lock_guard lock(owner->mutex_);
	--owner->counts_[static_cast<size_t>(kind_)];
	owner->changed_.notify_all();
}
RuntimeGuestExecution::Lease RuntimeGuestExecution::TryAcquire(GuestWorkKind kind) {
	const auto index = static_cast<size_t>(kind);
	std::lock_guard lock(mutex_);
	if (index >= counts_.size() || StopRequested() || counts_[index] == std::numeric_limits<size_t>::max()) { return {}; }
	++counts_[index];
	return Lease(shared_from_this(), kind);
}
GuestExecutionFailure RuntimeGuestExecution::Failure() const noexcept {
	std::lock_guard lock(mutex_);
	return failure_;
}
GuestExecutionFailure RuntimeGuestExecution::Fail(GuestFailureReason reason, uint64_t detail) noexcept {
	std::lock_guard lock(mutex_);
	if (!failure_ && reason != GuestFailureReason::None) {
		failure_ = {reason, owner_.token, detail};
		if (State() != GuestExecutionState::Retired && State() != GuestExecutionState::Draining) {
			state_.store(GuestExecutionState::Failed, std::memory_order_release);
		}
	}
	changed_.notify_all();
	return failure_;
}
void RuntimeGuestExecution::RequestStop() noexcept {
	std::lock_guard lock(mutex_);
	if (State() == GuestExecutionState::Running) { state_.store(GuestExecutionState::StopRequested, std::memory_order_release); }
	changed_.notify_all();
}
bool RuntimeGuestExecution::PublishStopText(std::shared_ptr<const GuestStopText> text) noexcept {
	if (!text || text->owner.identity != owner_.identity || text->owner.token != owner_.token) { return false; }
	for (const auto& range: text->ranges) { if (!range.begin || range.end <= range.begin) { return false; } }
	try {
		auto immutable = std::make_shared<const GuestStopText>(*text);
		stop_text_.store(std::move(immutable), std::memory_order_release);
		return true;
	} catch (...) { return false; }
}
void RuntimeGuestExecution::BeginDraining() noexcept {
	std::lock_guard lock(mutex_);
	if (State() != GuestExecutionState::Retired) { state_.store(GuestExecutionState::Draining, std::memory_order_release); }
	changed_.notify_all();
}
size_t RuntimeGuestExecution::Outstanding() const noexcept {
	std::lock_guard lock(mutex_);
	size_t total = 0;
	for (auto count: counts_) {
		if (count > std::numeric_limits<size_t>::max() - total) { return std::numeric_limits<size_t>::max(); }
		total += count;
	}
	return total;
}
bool RuntimeGuestExecution::WaitForDrain(std::chrono::milliseconds timeout) {
	std::unique_lock lock(mutex_);
	return changed_.wait_for(lock, timeout, [&] {
		for (auto count: counts_) { if (count) { return false; } }
		return StopRequested();
	});
}
bool RuntimeGuestExecution::Retire() noexcept {
	std::lock_guard lock(mutex_);
	if (!StopRequested()) { return false; }
	for (auto count: counts_) { if (count) { return false; } }
	state_.store(GuestExecutionState::Retired, std::memory_order_release);
	changed_.notify_all();
	return true;
}
RuntimeGuestResult RuntimeGuestExecution::Invoke(GuestBoundaryFunction guest, void* argument) noexcept {
	if (auto pending = PendingThreadExit()) {
		return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, Failure(), StopRequested(), pending};
	}
	auto lease = TryAcquire(GuestWorkKind::Guest);
	if (!lease) { return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, Failure(), true}; }
	if (StopRequested()) { return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, Failure(), true}; }
	auto call = boundary_->Invoke(guest, argument);
	const auto pending = PendingThreadExit();
	if (call.tag == GuestCallOutcomeTag::Aborted && !pending && !StopRequested()) {
		Fail(GuestFailureReason::GuestAborted);
	}
	else if (call.status != GuestCallBoundaryStatus::Ok) {
		Fail(GuestFailureReason::BoundaryUnavailable, static_cast<uint64_t>(call.status));
	}
	return {call, Failure(), StopRequested(), pending};
}
RuntimeGuestResult RuntimeGuestExecution::InvokeCallback(uint64_t target, void* argument, bool returns_value) noexcept {
	auto lease = TryAcquire(GuestWorkKind::Callback);
	if (!lease) { return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, Failure(), true}; }
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	CallbackArguments arguments {target, argument};
	if (target) { return Invoke(returns_value ? CallbackTransition : VoidCallbackTransition, &arguments); }
#endif
	return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
	        Fail(GuestFailureReason::BoundaryUnavailable, target), StopRequested()};
}
RuntimeGuestResult RuntimeGuestExecution::InvokeIntegerCallback(uint64_t target,
    const uint64_t* values, size_t count) noexcept {
	auto lease = TryAcquire(GuestWorkKind::Callback);
	if (!lease) { return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, Failure(), true}; }
	if (!target || count > 7 || (!values && count != 0)) {
		return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
		        Fail(GuestFailureReason::BoundaryUnavailable, target), StopRequested()};
	}
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
	IntegerCallbackArguments arguments {target, {}};
	for (size_t n = 0; n < count; ++n) { arguments.values[n] = values[n]; }
	// Ownership, argument storage and both leases remain above the naked
	// transition; Invoke performs the existing stop/abort/thread-exit handling.
	return Invoke(IntegerCallbackTransition, &arguments);
#else
	return {{GuestCallBoundaryStatus::Unsupported, GuestCallOutcomeTag::NotInvoked, 0},
	        Fail(GuestFailureReason::BoundaryUnavailable, target), StopRequested()};
#endif
}
GuestThreadExitOutcome RuntimeGuestExecution::PendingThreadExit() const noexcept {
	return thread_exit_request.owner.identity == owner_.identity && thread_exit_request.owner.token == owner_.token ?
	    thread_exit_request.outcome : GuestThreadExitOutcome {};
}
bool RuntimeGuestExecution::RequestThreadExit(uint64_t value) noexcept {
	if (!boundary_->FailureFunction() || (thread_exit_request.outcome && !PendingThreadExit())) {
		Fail(GuestFailureReason::InvalidTransition, 0x7074687265786974ULL);
		return false;
	}
	if (!PendingThreadExit()) { thread_exit_request = {owner_, {owner_.token, value}}; }
	return !Failure(); // Failure remains sticky and wins over a requested exit.
}
GuestThreadExitOutcome RuntimeGuestExecution::ConsumeThreadExit() noexcept {
	const auto pending = PendingThreadExit();
	if (!pending) { return {}; }
	if (GuestCallBoundary::CurrentDepth() || host_calls.depth) {
		Fail(GuestFailureReason::InvalidTransition);
		return {};
	}
	thread_exit_request = {};
	return pending;
}
bool RuntimeGuestExecution::RegisterThreadDestructor(uint64_t target) noexcept {
	std::lock_guard lock(mutex_);
	const auto previous = thread_destructor_.load(std::memory_order_relaxed);
	if (StopRequested() || (previous && previous != target)) { return false; }
	thread_destructor_.store(target, std::memory_order_release);
	return true;
}
RuntimeGuestResult RuntimeGuestExecution::InvokeModule(uint64_t target, size_t args, const void* argp, uint64_t callback) noexcept {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	ModuleArguments arguments {target, args, argp, callback};
	if (target) { return Invoke(ModuleTransition, &arguments); }
#endif
	return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
	        Fail(GuestFailureReason::BoundaryUnavailable, target), StopRequested()};
}
GuestBoundaryFailureFunction RuntimeGuestExecution::FailureFunction() const noexcept { return boundary_->FailureFunction(); }
RuntimeGuestResult RuntimeGuestExecution::InvokeEntry(uint64_t target, void* params, uint64_t atexit) noexcept {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	EntryArguments arguments {target, params, atexit};
	if (target && params) { return Invoke(EntryTransition, &arguments); }
#endif
	return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
	        Fail(GuestFailureReason::BoundaryUnavailable, target), StopRequested()};
}
GuestBoundaryAbortFunction RuntimeGuestExecution::AbortFunction() const noexcept { return boundary_->AbortFunction(); }

HostCallDisposition KYTY_GUEST_CALL_BOUNDARY_SYSV RuntimeGuestExecution::EnterHostCall(
    void* context, uint64_t guest_return, HostCallEnterReason reason) noexcept {
	auto* execution = static_cast<RuntimeGuestExecution*>(context);
	if (!execution) { return HostCallDisposition::Abort; } // Construction must forbid null runtime context.
	if (reason == HostCallEnterReason::CapacityExceeded || host_calls.depth == host_calls.calls.size()) {
		execution->Fail(GuestFailureReason::TransitionCapacity, guest_return);
		return HostCallDisposition::Abort; // No target lease and no recursive gateway entry.
	}
	if (reason != HostCallEnterReason::Call) {
		execution->Fail(GuestFailureReason::InvalidTransition, guest_return);
		return HostCallDisposition::Abort;
	}
	try {
		if (execution->PendingThreadExit()) { return HostCallDisposition::Abort; }
		auto lease = execution->TryAcquire(GuestWorkKind::HostCall);
		if (!lease) { return HostCallDisposition::Abort; }
		auto& call = host_calls.calls[host_calls.depth++];
		call.execution = execution;
		call.guest_return = guest_return;
		call.boundary_depth = GuestCallBoundary::CurrentDepth();
		call.lease = std::move(lease);
		GuestCallBoundary::PublishHostDepth(call.boundary_depth);
		return HostCallDisposition::Proceed;
	} catch (...) {
		execution->Fail(GuestFailureReason::InvalidTransition, guest_return);
		return HostCallDisposition::Abort;
	}
}
HostCallDisposition KYTY_GUEST_CALL_BOUNDARY_SYSV RuntimeGuestExecution::LeaveHostCall(
    void* context, uint64_t guest_return) noexcept {
	auto* execution = static_cast<RuntimeGuestExecution*>(context);
	if (!execution) { return HostCallDisposition::Abort; }
	if (!host_calls.depth || host_calls.calls[host_calls.depth - 1].execution != execution ||
	    host_calls.calls[host_calls.depth - 1].guest_return != guest_return) {
		execution->Fail(GuestFailureReason::InvalidTransition, guest_return);
		// Never release another live frame's lease to fabricate a drained owner.
		return HostCallDisposition::Abort;
	}
	auto& call = host_calls.calls[--host_calls.depth];
	call.lease.Reset();
	call.execution = nullptr;
	call.guest_return = 0;
	call.boundary_depth = 0;
	GuestCallBoundary::PublishHostDepth(host_calls.depth ? host_calls.calls[host_calls.depth - 1].boundary_depth : 0);
	return execution->StopRequested() || execution->PendingThreadExit() ? HostCallDisposition::Abort : HostCallDisposition::Proceed;
}

bool RuntimeGuestExecution::HasHostCallForCurrentBoundary() const noexcept {
	if (!host_calls.depth) { return false; }
	const auto& call = host_calls.calls[host_calls.depth - 1];
	const auto depth = GuestCallBoundary::CurrentDepth();
	return depth != 0 && call.execution == this && call.boundary_depth == depth;
}

bool InitializeApplicationExecution(Program* owner, GuestCallBoundaryStatus* status) noexcept {
	if (!owner || owner->application_owner != owner) {
		if (status) { *status = GuestCallBoundaryStatus::InvalidArgument; }
		return false;
	}
	if (owner->application_execution) {
		const bool matches = owner->application_execution->Owner().identity == owner;
		if (status) { *status = matches ? GuestCallBoundaryStatus::Ok : GuestCallBoundaryStatus::InvalidArgument; }
		return matches && !owner->application_execution->StopRequested();
	}
	owner->application_execution = RuntimeGuestExecution::Create(owner, status);
	return owner->application_execution != nullptr;
}
std::shared_ptr<RuntimeGuestExecution> GetApplicationExecution(const Program* program) noexcept {
	const auto* owner = program ? program->application_owner : nullptr;
	if (!owner || owner->application_owner != owner || !owner->application_execution ||
	    owner->application_execution->Owner().identity != owner) { return {}; }
	return owner->application_execution;
}
} // namespace Loader
