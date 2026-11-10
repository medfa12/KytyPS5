// Included inside Libs::LibKernel in pthread.cpp. Exercises real native workers,
// pooled records, guest mappings, owner controls and return/abort gateways.
namespace RuntimeGuestThreadFixture {
struct Context {
	Loader::Program* owner = nullptr;
	Loader::RuntimeGuestExecution* execution = nullptr;
	PthreadKey key = -1;
	unsigned entered = 0;
	unsigned unreachable = 0;
	unsigned callbacks = 0;
	bool callbacks_accounted = true;
	bool fail_cleanup = false;
	bool exit_cleanup = false;
	bool fail_after_exit = false;
	bool started_without_exit = true;
	unsigned host_destroyed = 0;
	bool on_guest_stack = false;
	bool inherited = false;
	bool coordinator_unwrapped = false;
	bool coordinator_continued = false;
	bool host_balanced = false;
	unsigned depth = 0;
	uint64_t gateway = 0;
	Loader::RuntimeGuestResult inner;
};
static thread_local Context* cleanup_context = nullptr;
static void ObserveCallback(Context* context) {
	++context->callbacks;
	context->callbacks_accounted &= Loader::GuestCallBoundary::CurrentDepth() == 1 &&
	    context->execution->Outstanding() >= 3; // Thread + Callback + Guest.
}
static void KYTY_SYSV_ABI Destructor(void* argument) {
	auto* context = static_cast<Context*>(argument);
	ObserveCallback(context);
	if (context->fail_cleanup) {
		context->execution->Fail(Loader::GuestFailureReason::Explicit, 0xc1ea);
		context->execution->FailureFunction()();
		++context->unreachable;
	}
	if (context->exit_cleanup) {
		PthreadExit(reinterpret_cast<void*>(99));
		++context->unreachable;
	}
}
static void KYTY_SYSV_ABI ThreadDtors() { if (cleanup_context) { ObserveCallback(cleanup_context); } }
static std::atomic<unsigned> session_old_dtors {0}, session_new_dtors {0};
static void KYTY_SYSV_ABI SessionOldDtors() { ++session_old_dtors; }
static void KYTY_SYSV_ABI SessionNewDtors() { ++session_new_dtors; }
static void* KYTY_SYSV_ABI SessionWorker(void* argument) { return argument; }
static uint64_t KYTY_SYSV_ABI CallbackValue(void* argument) { return reinterpret_cast<uint64_t>(argument); }
static void KYTY_SYSV_ABI CallbackVoid(void* argument) { ++*static_cast<unsigned*>(argument); }
static void Observe(Context* context) {
	uintptr_t marker = 0;
	auto* self = PthreadSelfOrNull();
	const auto address = reinterpret_cast<uintptr_t>(&marker);
	context->on_guest_stack = self && address >= reinterpret_cast<uintptr_t>(self->attr->stack_addr) &&
	    address < reinterpret_cast<uintptr_t>(self->attr->stack_addr) + self->attr->stack_size;
	context->inherited = Loader::RuntimeLinker::GetCurrentProgram() == context->owner &&
	    self && self->execution.get() == context->execution;
	context->depth = Loader::GuestCallBoundary::CurrentDepth();
	context->started_without_exit &= !context->execution->PendingThreadExit();
	++context->entered;
}
static void RegisterCleanup(Context* context) {
	cleanup_context = context;
	PthreadSetspecific(context->key, context);
	LibcInternalExt::LibcInternalExtCxaThreadAtexit(Destructor, context, nullptr);
}
static void* KYTY_SYSV_ABI Returned(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	RegisterCleanup(context);
	return reinterpret_cast<void*>(0x12345678);
}
struct Pin {
	Context* context;
	~Pin() { ++context->host_destroyed; }
};
static Loader::GuestBoundaryFailureFunction PrepareFailure(Context* context) {
	Pin pin {context};
	context->execution->Fail(Loader::GuestFailureReason::Explicit, 0xfeed);
	return context->execution->FailureFunction();
}
static void* KYTY_SYSV_ABI Failed(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	RegisterCleanup(context);
	PrepareFailure(context)(); // All host locals have already been destroyed.
	++context->unreachable;
	return nullptr;
}
static void* KYTY_SYSV_ABI Exited(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	RegisterCleanup(context);
	PthreadExit(reinterpret_cast<void*>(17));
	++context->unreachable;
	return nullptr;
}
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV ExitSegment(void* argument) {
	PthreadExit(argument);
	++static_cast<Context*>(argument)->unreachable;
	return 0;
}
static uint64_t KYTY_SYSV_ABI HostNestedExit(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin host_pin {context};
	context->inner = context->execution->Invoke(ExitSegment, context);
	if (context->fail_after_exit) { context->execution->Fail(Loader::GuestFailureReason::Explicit, 0xe817); }
	return 41;
}
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV GatewaySegment(void* argument) {
	auto* context = static_cast<Context*>(argument);
	reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(context->gateway)(context);
	++context->unreachable;
	return 0;
}
static void* KYTY_SYSV_ABI Coordinator(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	Pin coordinator_pin {context};
	context->coordinator_unwrapped = Loader::GuestCallBoundary::CurrentDepth() == 0;
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	const auto result = context->execution->Invoke(GatewaySegment, context);
	context->coordinator_continued = result.call.tag == Loader::GuestCallOutcomeTag::Aborted &&
	    context->inner.call.tag == Loader::GuestCallOutcomeTag::Aborted && result.ThreadExited() &&
	    context->inner.ThreadExited();
	context->host_balanced = Loader::GuestCallBoundary::CurrentDepth() == 0 &&
	    Loader::HostCallReturnThunk::CurrentDepth() == 0 &&
	    Common::GetHostLockCriticalCounter()->load() == baseline;
	return reinterpret_cast<void*>(41);
}
static void* KYTY_SYSV_ABI NestedWorker(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	RegisterCleanup(context);
	GatewaySegment(context);
	return nullptr;
}
static std::atomic<bool> peer_ready {false};
static std::atomic<bool> peer_release {false};
static void* KYTY_SYSV_ABI Peer(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Observe(context);
	RegisterCleanup(context);
	peer_ready.store(true, std::memory_order_release);
	while (!peer_release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
	return reinterpret_cast<void*>(0x12345678);
}
static Loader::RuntimeGuestExecution* stop_before_publish = nullptr;
static void StopPublication() { stop_before_publish->RequestStop(); }
}

bool TestRuntimeGuestThreads(std::string* failure) {
	using namespace RuntimeGuestThreadFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[12];
	Context contexts[12];
	const auto previous_program = Loader::RuntimeLinker::GetCurrentProgram();
	const auto previous_dtors = g_pthread_context->GetThreadDtors();
	struct Restore {
		Loader::Program* previous;
		thread_dtors_func_t dtors;
		Context* contexts;
		~Restore() {
			g_runtime_thread_before_publish = nullptr;
			g_runtime_thread_fail_native_create = false;
			stop_before_publish = nullptr;
			g_pthread_context->SetThreadDtors(dtors);
			Loader::RuntimeLinker::SetCurrentProgram(previous);
			for (unsigned i = 0; i != 12; ++i) { if (contexts[i].key >= 0) { PthreadKeyDelete(contexts[i].key); } }
		}
	} restore {previous_program, previous_dtors, contexts};
	// Native process startup may initialize firmware libc before bundled libc.
	// Exercise callback replacement through real worker cleanup, then clearing;
	// the superseded callback must never run and the owner path stays isolated.
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	session_old_dtors = 0;
	session_new_dtors = 0;
	KernelSetThreadDtors(SessionOldDtors);
	KernelSetThreadDtors(SessionOldDtors);
	KernelSetThreadDtors(SessionNewDtors);
	for (unsigned i = 0; i != 2; ++i) {
		Pthread session_thread = nullptr;
		void* session_result = nullptr;
		if (i == 1) { KernelSetThreadDtors(nullptr); }
		if (!check(PthreadCreate(&session_thread, nullptr, SessionWorker, nullptr, "session-dtors-fixture") == OK,
		           "create process-local destructor worker")) { return false; }
		if (!check(PthreadJoin(session_thread, &session_result) == OK && session_result == nullptr &&
		               session_old_dtors == 0 && session_new_dtors == 1,
		           "replacement destructor runs once; superseded and cleared destructors never run")) { return false; }
	}
	KernelSetThreadDtors(previous_dtors);
	for (unsigned i = 0; i != 12; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 987000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owners[i]),
		           "runtime guest recovery capability unavailable (fixture NOT exercised)")) { return false; }
		contexts[i].owner = &owners[i];
		contexts[i].execution = owners[i].application_execution.get();
		Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
		if (!check(PthreadKeyCreate(&contexts[i].key, Destructor) == OK, "allocate owner-bound pthread cleanup key")) { return false; }
		KernelSetThreadDtors(ThreadDtors);
	}
	if (!check(g_pthread_context->GetThreadDtors() == previous_dtors, "shared owners must never overwrite session thread destructors")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	const auto callback_value = contexts[0].execution->InvokeCallback(reinterpret_cast<uint64_t>(CallbackValue),
	    reinterpret_cast<void*>(0xfedcba9876543210ULL), true);
	unsigned callback_void_calls = 0;
	const auto callback_void = contexts[0].execution->InvokeCallback(reinterpret_cast<uint64_t>(CallbackVoid), &callback_void_calls);
	if (!check(callback_value.Returned() && callback_value.call.value == 0xfedcba9876543210ULL &&
	               callback_void.Returned() && callback_void.call.value == 0 && callback_void_calls == 1 &&
	               contexts[0].execution->Outstanding() == 0,
	           "typed callback adapter must preserve uint64 results and normalize void results without leaked work")) { return false; }
	const auto run = [&](unsigned index, pthread_entry_func_t entry, bool coordinator, void** value, Pthread* record) {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[index]);
		const auto created = coordinator ?
		    PthreadCreateRuntimeCoordinator(record, nullptr, entry, &contexts[index], "runtime-coordinator-fixture") :
		    PthreadCreate(record, nullptr, entry, &contexts[index], "runtime-worker-fixture");
		if (!check(created == OK, "create real owner-bound native worker")) { return false; }
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while ((!(*record)->almost_done.load() || contexts[index].execution->Outstanding() != 0) &&
		       std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
		const bool drained_before_join = (*record)->almost_done.load() &&
		    contexts[index].execution->Outstanding() == 0 && owners[index].application_threads.load() == 0 &&
		    (*record)->execution.get() == contexts[index].execution && !(*record)->free;
		const auto joined = PthreadJoin(*record, value);
		if (!check(drained_before_join, "cleanup must release Thread work before join while retaining the stable control pin")) { return false; }
		return check(joined == OK, "join real owner-bound native worker");
	};
	Pthread record = nullptr;
	void* value = nullptr;
	const auto main_counter = Common::GetHostLockCriticalCounter()->load();
	if (!run(0, Returned, false, &value, &record)) { return false; }
	if (!check(value == reinterpret_cast<void*>(0x12345678) && contexts[0].entered == 1 &&
	               contexts[0].on_guest_stack && contexts[0].inherited && contexts[0].depth == 1 &&
	               contexts[0].callbacks == 3 && contexts[0].callbacks_accounted && owners[0].application_threads.load() == 0 &&
	               contexts[0].execution->Outstanding() == 0 && record->guest_result.Returned() &&
	               !record->execution && !record->execution_thread && record->free,
	           "normal worker must return its pointer, use the real guest stack, clean up and release all work")) { return false; }
	const auto normal_record = record;
	if (!run(1, Failed, false, &value, &record)) { return false; }
	if (!check(value == reinterpret_cast<void*>(~uintptr_t {0}) && contexts[1].entered == 1 &&
	               contexts[1].on_guest_stack && contexts[1].inherited && contexts[1].depth == 1 &&
	               contexts[1].host_destroyed == 1 && contexts[1].unreachable == 0 && contexts[1].callbacks == 0 &&
	               record->guest_result.call.tag == Loader::GuestCallOutcomeTag::Aborted &&
	               contexts[1].execution->Failure().detail == 0xfeed && contexts[1].execution->Outstanding() == 0 &&
	               owners[1].application_threads.load() == 0 && record == normal_record,
	           "failure gateway must preserve host cleanup, suppress guest continuation/destructors and safely reuse the pool record")) { return false; }
	if (!run(2, Exited, false, &value, &record)) { return false; }
	if (!check(value == reinterpret_cast<void*>(17) && contexts[2].unreachable == 0 &&
	               contexts[2].callbacks == 3 && contexts[2].callbacks_accounted && record->guest_result.ThreadExited() &&
	               record->guest_result.thread_exit.value == 17 && !contexts[2].execution->Failure() &&
	               contexts[2].execution->State() == Loader::GuestExecutionState::Running &&
	               contexts[2].execution->Outstanding() == 0 && owners[2].application_threads.load() == 0,
	           "normal guest pthread exit must preserve its pointer, admit bounded cleanup and leave its owner running")) { return false; }
	Loader::HostCallReturnThunkStatus gateway_status;
	auto gateway = Loader::HostCallReturnThunk::Create(contexts[3].execution->Boundary(),
	    reinterpret_cast<uint64_t>(HostNestedExit), contexts[3].execution,
	    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall,
	    Loader::HostCallAbi::BaselineSysV, &gateway_status);
	if (!check(gateway != nullptr, "create real nested exit return gateway")) { return false; }
	contexts[3].gateway = gateway->GetAddress();
	if (!run(3, Coordinator, true, &value, &record)) { return false; }
	if (!check(value == &contexts[3] && contexts[3].on_guest_stack && contexts[3].inherited &&
	               contexts[3].coordinator_unwrapped && contexts[3].coordinator_continued && contexts[3].host_balanced &&
	               contexts[3].host_destroyed == 2 && contexts[3].unreachable == 0 && contexts[3].callbacks == 0 &&
	               contexts[3].execution->Outstanding() == 0 && owners[3].application_threads.load() == 0,
	           "inner exit must preserve both the outer host gateway and the unwrapped runtime coordinator's RAII")) { return false; }
	// Checked once's thread-exit/rollback/outer-RAII case now lives in
	// RuntimeCrtSyncTests with private RX callbacks and owned control storage.
	for (unsigned i = 7; i != 8; ++i) {
		auto nested_gateway = Loader::HostCallReturnThunk::Create(contexts[i].execution->Boundary(),
		    reinterpret_cast<uint64_t>(HostNestedExit), contexts[i].execution,
		    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall,
		    Loader::HostCallAbi::BaselineSysV, &gateway_status);
		if (!check(nested_gateway != nullptr, "create worker nested callback gateway")) { return false; }
		contexts[i].gateway = nested_gateway->GetAddress();
		if (!run(i, NestedWorker, false, &value, &record)) { return false; }
		if (!check(value == &contexts[i] && contexts[i].host_destroyed == 1 && contexts[i].unreachable == 0 &&
		               contexts[i].callbacks == 3 && contexts[i].callbacks_accounted && record->guest_result.ThreadExited() &&
		               !contexts[i].execution->Failure() && !contexts[i].execution->PendingThreadExit(),
		           "nested callback exit must destroy HLE RAII, propagate its pointer and leave no stale request")) { return false; }
	}
	contexts[9].fail_cleanup = true;
	if (!run(9, Returned, false, &value, &record)) { return false; }
	if (!check(contexts[9].callbacks == 1 && contexts[9].callbacks_accounted && contexts[9].unreachable == 0 &&
	               contexts[9].execution->Failure().detail == 0xc1ea && contexts[9].execution->Outstanding() == 0,
	           "failed cleanup callback must recover to host teardown and suppress later callbacks")) { return false; }
	contexts[10].exit_cleanup = true;
	if (!run(10, Returned, false, &value, &record)) { return false; }
	if (!check(contexts[10].callbacks == 1 && contexts[10].unreachable == 0 && !contexts[10].execution->Failure() &&
	               !contexts[10].execution->PendingThreadExit() && contexts[10].execution->Outstanding() == 0,
	           "cleanup exit must not escape host teardown or leave stale actual-thread TLS")) { return false; }
	contexts[11].fail_after_exit = true;
	auto failed_exit_gateway = Loader::HostCallReturnThunk::Create(contexts[11].execution->Boundary(),
	    reinterpret_cast<uint64_t>(HostNestedExit), contexts[11].execution,
	    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall,
	    Loader::HostCallAbi::BaselineSysV, &gateway_status);
	if (!check(failed_exit_gateway != nullptr, "create failure-after-exit gateway")) { return false; }
	contexts[11].gateway = failed_exit_gateway->GetAddress();
	if (!run(11, NestedWorker, false, &value, &record)) { return false; }
	if (!check(value == reinterpret_cast<void*>(~uintptr_t {0}) && contexts[11].inner.ThreadExited() &&
	               bool(record->guest_result.thread_exit) && !record->guest_result.ThreadExited() &&
	               record->guest_result.failure.detail == 0xe817 && contexts[11].callbacks == 0 &&
	               contexts[11].unreachable == 0 && contexts[11].host_destroyed == 1,
	           "sticky owner failure must win over an earlier normal thread-exit request without skipping host cleanup")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[4]);
	contexts[4].execution->RequestStop();
	record = reinterpret_cast<Pthread>(0x12340);
	if (!check(PthreadCreate(&record, nullptr, Returned, &contexts[4], "stopped-admission") == KERNEL_ERROR_ECANCELED &&
	               record == reinterpret_cast<Pthread>(0x12340) && contexts[4].entered == 0 &&
	               contexts[4].execution->Outstanding() == 0 && owners[4].application_threads.load() == 0,
	           "stopped owner must refuse before pool/output publication")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[5]);
	stop_before_publish = contexts[5].execution;
	g_runtime_thread_before_publish = StopPublication;
	record = reinterpret_cast<Pthread>(0x12340);
	const auto raced = PthreadCreate(&record, nullptr, Returned, &contexts[5], "stop-publication-race");
	g_runtime_thread_before_publish = nullptr;
	if (!check(raced == KERNEL_ERROR_ECANCELED && record == reinterpret_cast<Pthread>(0x12340) &&
	               contexts[5].entered == 0 && contexts[5].callbacks == 0 && contexts[5].execution->Outstanding() == 0 &&
	               owners[5].application_threads.load() == 0,
	           "stop after native creation must refuse publication, join and reclaim without entering guest code")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[6]);
	g_runtime_thread_fail_native_create = true;
	record = reinterpret_cast<Pthread>(0x12340);
	if (!check(PthreadCreate(&record, nullptr, Returned, &contexts[6], "native-create-failure") == KERNEL_ERROR_EAGAIN &&
	               record == reinterpret_cast<Pthread>(0x12340) && contexts[6].entered == 0 &&
	               contexts[6].execution->Outstanding() == 0 && owners[6].application_threads.load() == 0,
	           "native creation failure must leave no output, stack binding or owner work lease")) { return false; }
	// A final real creation proves both failure paths returned their pool record
	// with no stale execution/context pins. Its owner is still running.
	if (!run(6, Returned, false, &value, &record)) { return false; }
	if (!check(contexts[6].entered == 1 && contexts[6].callbacks == 3 && record == normal_record &&
	               Common::GetHostLockCriticalCounter()->load() == main_counter,
	           "failed creation paths must leave reusable records and balanced host critical accounting")) { return false; }
	// Keep a real same-owner peer inside guest code while another native worker
	// exits. An exit request must be actual-thread-local, not owner-wide state.
	Context peer_context;
	peer_context.owner = &owners[2];
	peer_context.execution = contexts[2].execution;
	peer_context.key = contexts[2].key;
	Loader::RuntimeLinker::SetCurrentProgram(&owners[2]);
	peer_ready.store(false);
	peer_release.store(false);
	Pthread peer = nullptr;
	if (!check(PthreadCreate(&peer, nullptr, Peer, &peer_context, "runtime-peer") == OK, "create live same-owner peer")) { return false; }
	const auto peer_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!peer_ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < peer_deadline) { std::this_thread::yield(); }
	Pthread exiting = nullptr;
	const auto exit_created = PthreadCreate(&exiting, nullptr, Exited, &contexts[2], "runtime-peer-exit");
	void* exit_value = nullptr;
	const auto exit_joined = exit_created == OK ? PthreadJoin(exiting, &exit_value) : KERNEL_ERROR_EAGAIN;
	const bool peer_survived = peer_ready.load(std::memory_order_acquire) && !peer->almost_done.load() &&
	    contexts[2].execution->State() == Loader::GuestExecutionState::Running;
	peer_release.store(true, std::memory_order_release);
	void* peer_value = nullptr;
	const auto peer_joined = PthreadJoin(peer, &peer_value);
	if (!check(exit_created == OK && exit_joined == OK && exit_value == reinterpret_cast<void*>(17) &&
	               peer_survived && peer_joined == OK && peer_value == reinterpret_cast<void*>(0x12345678) &&
	               peer_context.callbacks == 3 && peer_context.callbacks_accounted && peer_context.started_without_exit &&
	               contexts[2].started_without_exit && !contexts[2].execution->Failure() &&
	               contexts[2].execution->Outstanding() == 0 && !contexts[2].execution->PendingThreadExit() &&
	               Loader::GuestCallBoundary::CurrentDepth() == 0 && Loader::HostCallReturnThunk::CurrentDepth() == 0,
	           "normal thread exit must leave an already-running same-owner peer alive with isolated TLS and balanced boundaries")) { return false; }
	for (auto& owner: owners) {
		owner.application_execution->RequestStop();
		if (!check(owner.application_execution->WaitForDrain(std::chrono::milliseconds(0)) &&
		               owner.application_execution->Retire(), "all worker cleanup must permit owner drain and retirement")) { return false; }
	}
	return true;
}
