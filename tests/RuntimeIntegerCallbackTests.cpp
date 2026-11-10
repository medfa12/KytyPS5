// Component fixture: actual RuntimeGuestExecution, GuestCallBoundary and
// HostCallReturnThunk. Guest functions are synthetic SysV code; no firmware.
#include "loader/runtimeGuestExecution.h"
#include "common/threads.h"
#include "common/singleton.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "loader/runtimeLinker.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
using namespace Loader;
void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "RuntimeIntegerCallbackTests: %s\n", message); std::abort(); }
}
#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
enum class Mode { Normal, Abort, Stop, Exit, ConsumeInside };
struct Context {
	std::shared_ptr<RuntimeGuestExecution> execution;
	std::thread::id thread = std::this_thread::get_id();
	Mode mode = Mode::Normal;
	uint64_t nested_gateway = 0, action_gateway = 0;
	RuntimeGuestResult inner, suppressed;
	unsigned live = 0, destroyed = 0, leaf_calls = 0, leaf_after = 0, outer_after = 0;
	bool pending_in_host = false, consumed_inside = false;
};
struct Pin {
	Context& context;
	explicit Pin(Context& c): context(c) { ++context.live; }
	~Pin() { --context.live; ++context.destroyed; }
};
constexpr uint64_t ExitValue = 0xfedcba9876543210ULL;
constexpr uint64_t ReturnValue = uint64_t(-42);
using Gateway = uint64_t(KYTY_GUEST_CALL_BOUNDARY_SYSV*)(Context*);

uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Action(Context* c) {
	Pin pin {*c};
	Check(c->execution->HasHostCallForCurrentBoundary() && GuestCallBoundary::CurrentDepth() == 2 &&
	          HostCallReturnThunk::CurrentDepth() == 2 && c->execution->Outstanding() == 6,
	      "inner host gateway must account for both nested callbacks and host calls");
	if (c->mode == Mode::Stop) { c->execution->RequestStop(); }
	if (c->mode == Mode::Exit || c->mode == Mode::ConsumeInside) {
		Check(c->execution->RequestThreadExit(ExitValue), "request exit inside actual innermost boundary");
		if (c->mode == Mode::ConsumeInside) { c->consumed_inside = bool(c->execution->ConsumeThreadExit()); }
	}
	return 13; // Actual return gateway performs recovery after this Pin dies.
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Leaf(Context* c, uint64_t b, uint64_t d,
    uint64_t e, uint64_t f, uint64_t g, uint64_t seventh) {
	++c->leaf_calls;
	Check(b == 0x1122334455667788 && d == 0x2233445566778899 && e == 0x33445566778899aa &&
	          f == 0x445566778899aabb && g == 0x5566778899aabbcc && seventh == 0xdeadbeeffeedcafe,
	      "six register arguments and seventh stack argument must retain every bit");
	Check(std::this_thread::get_id() == c->thread && GuestCallBoundary::CurrentDepth() == 2 &&
	          !c->execution->HasHostCallForCurrentBoundary() && c->execution->Outstanding() == 5,
	      "nested callback executes on current thread with its own accounted boundary");
	if (c->mode == Mode::Abort) {
		c->execution->FailureFunction()(); // No host RAII in this discarded guest frame.
	} else {
		reinterpret_cast<Gateway>(c->action_gateway)(c);
	}
	++c->leaf_after;
	return ReturnValue;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Zero() { return 71; }
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Nested(Context* c) {
	Pin pin {*c};
	Check(c->execution->HasHostCallForCurrentBoundary() && c->execution->Outstanding() == 3,
	      "outer callback owns actual host-return lease");
	const uint64_t values[] {reinterpret_cast<uint64_t>(c), 0x1122334455667788, 0x2233445566778899,
	    0x33445566778899aa, 0x445566778899aabb, 0x5566778899aabbcc, 0xdeadbeeffeedcafe};
	c->inner = c->execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Leaf), values, 7);
	Check(GuestCallBoundary::CurrentDepth() == 1 && HostCallReturnThunk::CurrentDepth() == 1 &&
	          c->execution->HasHostCallForCurrentBoundary() && c->execution->Outstanding() == 3,
	      "nested recovery must restore outer TLS and preserve host RAII/lease");
	c->pending_in_host = bool(c->execution->PendingThreadExit());
	if (c->mode == Mode::Exit) {
		c->suppressed = c->execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Zero), nullptr, 0);
	}
	return c->inner.Returned() ? c->inner.call.value : 0;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Outer(Context* c) {
	const auto value = reinterpret_cast<Gateway>(c->nested_gateway)(c);
	++c->outer_after;
	return value;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Padded(uint64_t* output, uint64_t value,
    uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
	Check(!(c | d | e | f | g), "unused argument lanes must be zero");
	*output = value;
	return ReturnValue;
}
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Alignment() {
	__asm__ volatile("movq %rsp, %rax\n\tandq $15, %rax\n\tret\n\t");
}
std::shared_ptr<RuntimeGuestExecution> Create(const void* identity) {
	GuestCallBoundaryStatus status;
	auto result = RuntimeGuestExecution::Create(identity, &status);
	Check(result && status == GuestCallBoundaryStatus::Ok, "actual Win64 boundary capability required; not a skipped fixture");
	return result;
}
void Balanced(const Context& c, uint32_t baseline) {
	Check(c.execution->Outstanding() == 0 && GuestCallBoundary::CurrentDepth() == 0 &&
	          HostCallReturnThunk::CurrentDepth() == 0 && Common::GetHostLockCriticalCounter()->load() == baseline && c.live == 0,
	      "all callback/guest/host leases, TLS and host critical accounting must drain");
}
void NestedCase(Mode mode) {
	Context c; c.mode = mode; c.execution = Create(&c);
	auto nested = HostCallReturnThunk::Create(c.execution->Boundary(), reinterpret_cast<uint64_t>(Nested),
	    c.execution.get(), RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
	auto action = HostCallReturnThunk::Create(c.execution->Boundary(), reinterpret_cast<uint64_t>(Action),
	    c.execution.get(), RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
	Check(nested && action, "actual owner-bound host gateways required");
	c.nested_gateway = nested->GetAddress(); c.action_gateway = action->GetAddress();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	RuntimeGuestResult outer;
	{
		Pin root_pin {c}; // Coordinator RAII above every boundary and gateway.
		const uint64_t argument = reinterpret_cast<uint64_t>(&c);
		outer = c.execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Outer), &argument, 1);
		Check(c.live == 1 && c.leaf_calls == 1, "return/recovery must preserve coordinator root ownership");
	}
	Balanced(c, baseline);
	Check(c.destroyed == (mode == Mode::Abort ? 2u : 3u), "every host destructor must run exactly once");
	if (mode == Mode::Normal) {
		Check(outer.Returned() && c.inner.Returned() && outer.call.value == ReturnValue && c.inner.call.value == ReturnValue &&
		          c.outer_after == 1 && c.leaf_after == 1, "nested normal signed return and guest continuations");
	} else {
		Check(outer.call.tag == GuestCallOutcomeTag::Aborted && c.inner.call.tag == GuestCallOutcomeTag::Aborted &&
		          c.outer_after == 0 && c.leaf_after == 0, "abort/stop/exit must suppress both guest continuations");
		if (mode == Mode::Abort) {
			Check(outer.failure.reason == GuestFailureReason::GuestAborted && !outer.ThreadExited(), "unmarked abort becomes sticky owner failure");
		} else if (mode == Mode::Stop) {
			Check(outer.stopped && c.inner.stopped && !outer.failure && !outer.thread_exit, "stop is not an invented failure or exit");
		} else {
			Check(c.pending_in_host && c.execution->PendingThreadExit().value == ExitValue && !c.consumed_inside,
			      "exit remains pending through host coordinator return");
			if (mode == Mode::Exit) {
				Check(outer.ThreadExited() && c.inner.ThreadExited() && c.suppressed.ThreadExited() &&
				          c.suppressed.call.tag == GuestCallOutcomeTag::NotInvoked && !outer.failure &&
				          c.execution->State() == GuestExecutionState::Running, "exit suppresses reentry without stopping peer owner");
				bool peer_ok = false;
				std::thread peer([&] {
					const auto result = c.execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Zero), nullptr, 0);
					peer_ok = !c.execution->PendingThreadExit() && result.Returned() && result.call.value == 71;
				});
				peer.join(); Check(peer_ok, "pending exit is actual-thread local, not shared owner state");
			} else {
				Check(outer.failure.reason == GuestFailureReason::InvalidTransition && !outer.ThreadExited(),
				      "attempted inner consume fails sticky without losing pending exit");
			}
			const auto consumed = c.execution->ConsumeThreadExit(); // Only this drained worker root may consume.
			Check(consumed.value == ExitValue && consumed.lifetime == c.execution->Owner().token &&
			          !c.execution->PendingThreadExit() && !c.execution->ConsumeThreadExit(), "root consumes exit exactly once");
		}
	}
	Balanced(c, baseline);
}
void Admission() {
	Context c; c.execution = Create(&c);
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	uint64_t output = 0;
	const uint64_t values[] {reinterpret_cast<uint64_t>(&output), ExitValue};
	auto result = c.execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Padded), values, 2);
	Check(result.Returned() && result.call.value == ReturnValue && output == ExitValue, "pointer copyout, zero padding and return bits");
	result = c.execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Alignment), nullptr, 0);
	Check(result.Returned() && result.call.value == 8, "zero-argument callback entry has SysV stack alignment");
	c.execution->RequestStop();
	result = c.execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(Zero), reinterpret_cast<const uint64_t*>(1), 7);
	Check(result.stopped && result.call.tag == GuestCallOutcomeTag::NotInvoked && !result.failure,
	      "stopped admission refuses before reading argument array");
	Balanced(c, baseline);
	for (unsigned kind = 0; kind != 3; ++kind) {
		Context invalid; invalid.execution = Create(&invalid);
		const auto target = kind == 2 ? 0 : reinterpret_cast<uint64_t>(Zero);
		const auto args = kind == 0 ? reinterpret_cast<const uint64_t*>(1) : nullptr;
		const auto count = kind == 0 ? 8u : kind == 1 ? 1u : 0u;
		result = invalid.execution->InvokeIntegerCallback(target, args, count);
		Check(result.call.status == GuestCallBoundaryStatus::InvalidArgument && result.call.tag == GuestCallOutcomeTag::NotInvoked &&
		          result.failure.reason == GuestFailureReason::BoundaryUnavailable, "count>7/null args/null target reject before any read or invocation");
		Balanced(invalid, baseline);
	}
}
struct WorkerContext {
	RuntimeGuestExecution* execution = nullptr;
	RuntimeGuestResult result;
	unsigned destroyed = 0, unreachable = 0;
	bool pending_before_root = false, clean_before_entry = false;
};
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV ExitLeaf(WorkerContext* c, uint64_t b, uint64_t d,
    uint64_t e, uint64_t f, uint64_t g, uint64_t value) {
	Check(b == 2 && d == 3 && e == 4 && f == 5 && g == 6 && value == ExitValue,
	      "real pthread callback receives seventh argument");
	Libs::LibKernel::PthreadExit(reinterpret_cast<void*>(value));
	++c->unreachable;
	return 0;
}
void* KYTY_GUEST_CALL_BOUNDARY_SYSV Worker(void* opaque) {
	auto* c = static_cast<WorkerContext*>(opaque);
	struct RootPin { WorkerContext* c; ~RootPin() { ++c->destroyed; } } pin {c};
	c->clean_before_entry = !c->execution->PendingThreadExit();
	const uint64_t values[] {reinterpret_cast<uint64_t>(c), 2, 3, 4, 5, 6, ExitValue};
	c->result = c->execution->InvokeIntegerCallback(reinterpret_cast<uint64_t>(ExitLeaf), values, 7);
	c->pending_before_root = c->result.ThreadExited() && c->execution->PendingThreadExit().value == ExitValue &&
	    GuestCallBoundary::CurrentDepth() == 0 && HostCallReturnThunk::CurrentDepth() == 0;
	return reinterpret_cast<void*>(41); // RunThread must consume exit AFTER RootPin destruction.
}
void NativeWorkerExit() {
	using namespace Libs::LibKernel;
	static Common::Subsystems subsystems;
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Memory::Lifecycle>();
	Initialize();
	PthreadInitSelfForMainThread();
	Common::Singleton<RuntimeLinker>::Instance();
	Program owner;
	owner.application_owner = &owner;
	owner.unique_id = 989701;
	Check(InitializeApplicationExecution(&owner), "create actual worker execution owner");
	auto* previous = RuntimeLinker::GetCurrentProgram();
	RuntimeLinker::SetCurrentProgram(&owner);
	for (unsigned n = 0; n != 2; ++n) {
		WorkerContext context; context.execution = owner.application_execution.get();
		Pthread thread = nullptr;
		Check(PthreadCreateRuntimeCoordinator(&thread, nullptr, Worker, &context, "integer-callback-root") == 0,
		      "create real owner-bound coordinator thread");
		void* value = nullptr;
		Check(PthreadJoin(thread, &value) == 0 && value == reinterpret_cast<void*>(ExitValue),
		      "real worker root must consume pending exit instead of coordinator return value");
		Check(context.destroyed == 1 && context.unreachable == 0 && context.pending_before_root && context.clean_before_entry &&
		          owner.application_execution->Outstanding() == 0 && owner.application_threads.load() == 0 &&
		          !owner.application_execution->Failure(), "real worker exit drains after host RAII, preserves owner and permits another worker");
	}
	RuntimeLinker::SetCurrentProgram(previous);
}
#endif
} // namespace

int main() {
#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
	Admission();
	for (auto mode: {Mode::Normal, Mode::Abort, Mode::Stop, Mode::Exit, Mode::ConsumeInside}) { NestedCase(mode); }
	NativeWorkerExit();
	std::puts("Actual runtime integer callback boundary: normal/nested/abort/stop/thread-exit/admission checks passed");
#else
	int identity = 0;
	Loader::GuestCallBoundaryStatus status;
	Check(!Loader::RuntimeGuestExecution::Create(&identity, &status) && status == Loader::GuestCallBoundaryStatus::Unsupported,
	      "unsupported boundary architecture must refuse execution control creation");
	std::puts("Unsupported architecture correctly refuses boundary creation (execution cases not exercised)");
#endif
}
