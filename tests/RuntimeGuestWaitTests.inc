// Included in LibKernel; all blocked entries are real owner-bound native workers.
namespace RuntimeGuestWaitFixture {
enum class Mode { Infinite, Relative, Absolute, Join };
struct Resource { PthreadMutex mutex = nullptr; PthreadCond cond = nullptr; };
struct Context {
	Resource* resource = nullptr;
	Mode mode = Mode::Infinite;
	KernelUseconds budget = 45000;
	Pthread target = nullptr;
	uint64_t gateway = 0;
	std::atomic<bool> entered {false};
	std::atomic<bool> returned {false};
	int result = KERNEL_ERROR_EINVAL;
	unsigned destroyed = 0;
	unsigned continued = 0;
	bool restored = false;
	bool released = false;
	bool join_value_untouched = false;
	bool native_setup = false; // Cond-admission fixtures deliberately bypass mutex API admission.
	void* join_value = nullptr;
	std::chrono::steady_clock::duration elapsed {};
};
struct Blocker {
	std::mutex mutex;
	std::condition_variable cv;
	bool release = false;
	std::atomic<bool> entered {false};
	unsigned destroyed = 0;
};
struct Pin {
	unsigned* destroyed;
	~Pin() { ++*destroyed; }
};
static void Release(Blocker* blocker) {
	std::lock_guard lock(blocker->mutex);
	blocker->release = true;
	blocker->cv.notify_all();
}
static void* KYTY_SYSV_ABI BlockedHost(void* argument) {
	auto* blocker = static_cast<Blocker*>(argument);
	Pin pin {&blocker->destroyed};
	std::unique_lock lock(blocker->mutex);
	blocker->entered.store(true, std::memory_order_release);
	blocker->cv.wait(lock, [&] { return blocker->release; });
	return reinterpret_cast<void*>(0x51);
}
static uint64_t KYTY_SYSV_ABI HostWait(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {&context->destroyed};
	const auto start = std::chrono::steady_clock::now();
	if (context->mode == Mode::Join) {
		void* value = reinterpret_cast<void*>(0x7171);
		context->entered.store(true, std::memory_order_release);
		context->result = PthreadJoin(context->target, &value);
		context->join_value = value;
		context->join_value_untouched = value == reinterpret_cast<void*>(0x7171);
	} else {
		auto* resource = context->resource;
		const auto acquire = [&] {
			return context->native_setup ? NativeMutexLock(resource->mutex, nullptr) : PthreadMutexLock(&resource->mutex);
		};
		if (acquire() != OK || acquire() != OK) {
			context->returned.store(true, std::memory_order_release);
			return 0;
		}
		context->entered.store(true, std::memory_order_release);
		switch (context->mode) {
			case Mode::Infinite: context->result = PthreadCondWait(&resource->cond, &resource->mutex); break;
			case Mode::Relative: context->result = PthreadCondTimedwait(&resource->cond, &resource->mutex, context->budget); break;
			case Mode::Absolute: {
				KernelTimespec deadline {};
				NativeClockGettime(resource->cond->clock_id, &deadline);
				deadline.tv_nsec += static_cast<int64_t>(context->budget) * 1000;
				deadline.tv_sec += deadline.tv_nsec / 1000000000;
				deadline.tv_nsec %= 1000000000;
				context->result = PthreadCondTimedwaitAbs(&resource->cond, &resource->mutex, &deadline);
				break;
			}
			case Mode::Join: break;
		}
		{
			std::lock_guard lock(resource->mutex->m);
			context->restored = resource->mutex->owner == PthreadSelfOrNull() && resource->mutex->count == 2;
			context->released = resource->mutex->owner != PthreadSelfOrNull();
		}
		// Only host fixture cleanup runs here; production's gateway suppresses
		// guest continuation after cancellation. Never unlock a peer's mutex.
		if (context->restored) {
			PthreadMutexUnlock(&resource->mutex);
			PthreadMutexUnlock(&resource->mutex);
		}
	}
	context->elapsed = std::chrono::steady_clock::now() - start;
	context->returned.store(true, std::memory_order_release);
	return static_cast<uint64_t>(context->result);
}
static void* KYTY_SYSV_ABI Guest(void* argument) {
	auto* context = static_cast<Context*>(argument);
	reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(context->gateway)(context);
	++context->continued;
	return reinterpret_cast<void*>(0x61);
}
static void* KYTY_SYSV_ABI HostJoin(void* argument) {
	HostWait(argument); // Boundary-free coordinator must remain an actual join.
	return reinterpret_cast<void*>(0x71);
}
static void* KYTY_SYSV_ABI SessionWait(void* argument) {
	HostWait(argument);
	++static_cast<Context*>(argument)->continued;
	return nullptr;
}
static void* KYTY_SYSV_ABI Immediate(void*) { return reinterpret_cast<void*>(0x8181); }
template <typename Predicate> static bool Await(Predicate predicate, unsigned milliseconds = 1500) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}
static unsigned Waiters(Resource* resource) {
	std::lock_guard lock(resource->cond->m);
	return static_cast<unsigned>(resource->cond->waiters.size());
}
}

bool TestRuntimeGuestWaits(std::string* failure) {
	using namespace RuntimeGuestWaitFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[6];
	Loader::Program session;
	Resource resources[4];
	Context contexts[14];
	Blocker blocker, foreign_blocker;
	std::vector<Pthread> workers;
	std::vector<std::unique_ptr<Loader::HostCallReturnThunk>> gateways;
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	struct Restore {
		Loader::Program* previous;
		Loader::Program* owners;
		Resource* resources;
		std::vector<Pthread>* workers;
		Blocker* blocker;
		Blocker* foreign_blocker;
		~Restore() {
			for (unsigned i = 0; i != 6; ++i) { if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); } }
			Release(blocker);
			Release(foreign_blocker);
			for (unsigned i = 0; i != 4; ++i) { if (resources[i].cond) { PthreadCondBroadcast(&resources[i].cond); } }
			for (auto i = workers->rbegin(); i != workers->rend(); ++i) { if (!(*i)->free) { PthreadJoin(*i, nullptr); } }
			for (unsigned i = 0; i != 4; ++i) {
				if (resources[i].cond) { PthreadCondDestroy(&resources[i].cond); }
				if (resources[i].mutex) { PthreadMutexDestroy(&resources[i].mutex); }
			}
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {previous, owners, resources, &workers, &blocker, &foreign_blocker};
	for (unsigned i = 0; i != 6; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 988000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owners[i]), "wait fixture requires supported runtime guest boundaries")) { return false; }
		if (i < 3) {
			Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
			PthreadMutexattr attr = nullptr;
			if (!check(PthreadMutexattrInit(&attr) == OK, "create wait fixture mutex attributes")) { return false; }
			PthreadMutexattrSettype(&attr, 2);
			const auto mutex_result = PthreadMutexInit(&resources[i].mutex, &attr, "runtime-wait-mutex");
			PthreadMutexattrDestroy(&attr);
			if (!check(mutex_result == OK && PthreadCondInit(&resources[i].cond, nullptr, "runtime-wait-cond") == OK,
			           "create lifetime-owned recursive mutex and condition")) { return false; }
		}
	}
	const auto create = [&](unsigned owner, pthread_entry_func_t entry, void* argument, bool coordinator, Pthread* output) {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[owner]);
		const auto result = coordinator ? PthreadCreateRuntimeCoordinator(output, nullptr, entry, argument, "wait-host-fixture") :
		    PthreadCreate(output, nullptr, entry, argument, "wait-guest-fixture");
		if (result == OK) { workers.push_back(*output); }
		return check(result == OK, "create real stop-aware wait worker");
	};
	const auto guest = [&](unsigned owner, Context* context, Pthread* output) {
		auto execution = owners[owner].application_execution;
		auto gateway = Loader::HostCallReturnThunk::Create(execution->Boundary(), reinterpret_cast<uint64_t>(HostWait),
		    execution.get(), Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall);
		if (!check(bool(gateway), "create returning wait HLE gateway")) { return false; }
		context->gateway = gateway->GetAddress();
		gateways.push_back(std::move(gateway));
		return create(owner, Guest, context, false, output);
	};
	const auto joined = [&](Pthread worker) {
		return check(PthreadJoin(worker, nullptr) == OK, "host must actually join and reclaim wait worker");
	};
	Pthread stopped[3] {}, isolated = nullptr;
	for (unsigned i = 0; i != 3; ++i) {
		contexts[i].resource = &resources[0];
		contexts[i].mode = i == 0 ? Mode::Infinite : (i == 1 ? Mode::Relative : Mode::Absolute);
		contexts[i].budget = 60000000;
		if (!guest(0, &contexts[i], &stopped[i])) { return false; }
	}
	contexts[3].resource = &resources[1];
	if (!guest(1, &contexts[3], &isolated)) { return false; }
	if (!check(Await([&] { return Waiters(&resources[0]) == 3 && Waiters(&resources[1]) == 1; }),
	           "all infinite/long relative/absolute waits must really park")) { return false; }
	owners[0].application_execution->RequestStop();
	if (!check(Await([&] { return contexts[0].returned.load() && contexts[1].returned.load() && contexts[2].returned.load(); }),
	           "owner stop must boundedly release every condition waiter without a signal")) { return false; }
	for (unsigned i = 0; i != 3; ++i) {
		if (!joined(stopped[i])) { return false; }
		if (!check(contexts[i].result == KERNEL_ERROR_ECANCELED && contexts[i].released && !contexts[i].restored &&
		               contexts[i].destroyed == 1 && contexts[i].continued == 0,
		           "canceled condition HLE must return ECANCELED, release ownership, destroy host RAII and suppress guest continuation")) { return false; }
	}
	if (!check(Waiters(&resources[0]) == 0 && !contexts[3].returned.load() && Waiters(&resources[1]) == 1 &&
	               !owners[0].application_execution->Failure() && !owners[1].application_execution->StopRequested(),
	           "condition stop removes only owned waiters and never fails/stops an isolated application")) { return false; }
	PthreadCondSignal(&resources[1].cond);
	if (!check(Await([&] { return contexts[3].returned.load(); }), "ordinary condition signal must still release a waiter") || !joined(isolated)) { return false; }
	if (!check(contexts[3].result == OK && contexts[3].restored && contexts[3].continued == 1,
	           "ordinary signal must restore recursive mutex ownership and resume guest code")) { return false; }
	// A raw/dlsym-style call has a guest root but no returning HLE gateway.
	// It cannot use terminal cancellation's released-mutex contract.
	contexts[13].resource = &resources[2];
	contexts[13].native_setup = true;
	contexts[13].mode = Mode::Relative;
	Pthread raw_wait = nullptr;
	if (!create(2, SessionWait, &contexts[13], false, &raw_wait) || !joined(raw_wait)) { return false; }
	if (!check(contexts[13].result == KERNEL_ERROR_EOPNOTSUPP && contexts[13].restored &&
	               contexts[13].continued == 1 && Waiters(&resources[2]) == 0 &&
	               !owners[2].application_execution->StopRequested(),
	           "runtime wait without a matching return gateway must refuse before releasing the caller's mutex")) { return false; }

	for (unsigned i = 4; i != 6; ++i) {
		contexts[i].resource = &resources[2];
		contexts[i].mode = i == 4 ? Mode::Relative : Mode::Absolute;
		Pthread timed = nullptr;
		if (!guest(2, &contexts[i], &timed)) { return false; }
		if (!check(Await([&] { return Waiters(&resources[2]) == 1 || contexts[i].returned.load(); }), "timed wait entered")) { return false; }
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (!contexts[i].returned.load() && std::chrono::steady_clock::now() < deadline) {
			timed->cond_cv.notify_one(); // Spurious native wake, not a guest signal.
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
		if (!check(contexts[i].returned.load(), "spurious wakes must not restart the condition timeout budget") || !joined(timed)) { return false; }
		if (!check(contexts[i].result == KERNEL_ERROR_ETIMEDOUT && contexts[i].restored && contexts[i].continued == 1 &&
		               contexts[i].elapsed >= std::chrono::milliseconds(35) && contexts[i].elapsed < std::chrono::milliseconds(500),
		           "relative/absolute timeout must retain its budget and restore recursive ownership")) { return false; }
	}
	// Stop while reacquiring, not just while parked on the condition itself.
	contexts[6].resource = &resources[2];
	contexts[6].mode = Mode::Relative;
	Pthread relocking = nullptr;
	if (!guest(2, &contexts[6], &relocking)) { return false; }
	if (!check(Await([&] { return Waiters(&resources[2]) == 1; }), "relock fixture parked")) { return false; }
	if (!check(PthreadMutexLock(&resources[2].mutex) == OK, "hold condition mutex from another actual thread")) { return false; }
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	const bool waited_for_lock = !contexts[6].returned.load();
	owners[2].application_execution->RequestStop();
	const bool canceled_relock = Await([&] { return contexts[6].returned.load(); });
	const bool still_main_owned = resources[2].mutex->owner == PthreadSelfOrNull();
	PthreadMutexUnlock(&resources[2].mutex);
	if (!check(waited_for_lock && canceled_relock && still_main_owned, "stop must cancel relock without stealing a mutex or returning a false timeout") || !joined(relocking)) { return false; }
	if (!check(contexts[6].result == KERNEL_ERROR_ECANCELED && contexts[6].released && contexts[6].continued == 0,
	           "canceled relock must remain a terminal ECANCELED, not guest success")) { return false; }

	Pthread target = nullptr, joining = nullptr, host_joining = nullptr;
	if (!create(3, BlockedHost, &blocker, true, &target)) { return false; }
	contexts[7].mode = Mode::Join;
	contexts[7].target = target;
	if (!guest(3, &contexts[7], &joining)) { return false; }
	if (!check(Await([&] { return contexts[7].entered.load() && blocker.entered.load() && target->test_guest_join_polling.load(std::memory_order_acquire); }),
	           "owned guest join really acquired its target and entered the bounded native poll")) { return false; }
	contexts[8].mode = Mode::Join;
	contexts[8].target = target;
	if (!create(3, HostJoin, &contexts[8], true, &host_joining)) { return false; }
	if (!check(Await([&] { return contexts[8].entered.load(); }), "boundary-free host coordinator join entered")) { return false; }
	owners[3].application_execution->RequestStop();
	if (!check(Await([&] { return contexts[7].returned.load(); }), "owner stop releases guest join without terminating its blocked native target") || !joined(joining)) { return false; }
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	if (!check(contexts[7].result == KERNEL_ERROR_ECANCELED && contexts[7].join_value_untouched &&
	               contexts[7].destroyed == 1 && contexts[7].continued == 0 && !contexts[8].returned.load() && !target->free,
	           "canceled guest join must leave target/pins/output intact while actual host join remains blocked")) { return false; }
	Release(&blocker);
	if (!check(Await([&] { return contexts[8].returned.load(); }), "host join completes only after actual native target return") || !joined(host_joining)) { return false; }
	if (!check(contexts[8].result == OK && !contexts[8].join_value_untouched && blocker.destroyed == 1 && target->free,
	           "stopped owner's boundary-free coordinator must actually join/reclaim its peer")) { return false; }

	// Unsafe foreign wait/join is refused, never converted into global stop.
	contexts[9].resource = &resources[1];
	contexts[9].native_setup = true;
	Pthread foreign_cond = nullptr;
	if (!guest(4, &contexts[9], &foreign_cond) || !check(Await([&] { return contexts[9].returned.load(); }), "foreign condition operation refused") || !joined(foreign_cond)) { return false; }
	if (!check(contexts[9].result == KERNEL_ERROR_EOPNOTSUPP && contexts[9].restored && contexts[9].continued == 1,
	           "foreign condition refusal must leave the caller's held mutex unchanged")) { return false; }
	Pthread foreign_target = nullptr, foreign_join = nullptr;
	if (!create(5, BlockedHost, &foreign_blocker, true, &foreign_target)) { return false; }
	contexts[10].mode = Mode::Join;
	contexts[10].target = foreign_target;
	if (!guest(4, &contexts[10], &foreign_join) || !check(Await([&] { return contexts[10].returned.load(); }), "foreign native join refused") || !joined(foreign_join)) { return false; }
	if (!check(contexts[10].result == KERNEL_ERROR_EOPNOTSUPP && contexts[10].join_value_untouched &&
	               !owners[4].application_execution->StopRequested() && !owners[5].application_execution->StopRequested(),
	           "foreign join must not stop either owner or consume a native target")) { return false; }
	Release(&foreign_blocker);
	if (!joined(foreign_target)) { return false; }
	Pthread successful_target = nullptr, successful_join = nullptr;
	if (!create(4, Immediate, nullptr, false, &successful_target)) { return false; }
	contexts[11].mode = Mode::Join;
	contexts[11].target = successful_target;
	if (!guest(4, &contexts[11], &successful_join) || !check(Await([&] { return contexts[11].returned.load(); }), "ordinary owned guest join completes") || !joined(successful_join)) { return false; }
	if (!check(contexts[11].result == OK && contexts[11].join_value == reinterpret_cast<void*>(0x8181) &&
	               contexts[11].continued == 1 && successful_target->free,
	           "ordinary owned guest join preserves return pointer and reclaims only after actual native termination")) { return false; }
	// Explicit legacy/session program: no scoped execution, stop predicate or
	// gateway. Unrelated owners are already stopped and must not affect it.
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	PthreadMutexattr session_attr = nullptr;
	if (!check(PthreadMutexattrInit(&session_attr) == OK, "create legacy mutex attributes")) { return false; }
	PthreadMutexattrSettype(&session_attr, 2);
	const auto session_mutex = PthreadMutexInit(&resources[3].mutex, &session_attr, "session-wait-mutex");
	PthreadMutexattrDestroy(&session_attr);
	if (!check(session_mutex == OK && PthreadCondInit(&resources[3].cond, nullptr, "session-wait-cond") == OK,
	           "create legacy session wait resources")) { return false; }
	contexts[12].resource = &resources[3];
	contexts[12].mode = Mode::Relative;
	Pthread session_worker = nullptr;
	if (!check(PthreadCreate(&session_worker, nullptr, SessionWait, &contexts[12], "session-wait-fixture") == OK,
	           "create real non-runtime legacy wait worker")) { return false; }
	workers.push_back(session_worker);
	if (!check(Await([&] { return contexts[12].returned.load(); }), "legacy wait retains ordinary timeout") || !joined(session_worker)) { return false; }
	if (!check(contexts[12].result == KERNEL_ERROR_ETIMEDOUT && contexts[12].restored && contexts[12].continued == 1,
	           "session wait semantics stay independent of stopped shared application owners")) { return false; }
	for (auto& owner: owners) {
		if (!check(!owner.application_execution->Failure() && owner.application_threads.load() == 0 &&
		               owner.application_execution->Outstanding() == 0, "all wait paths release native workers, callback/guest/host/thread leases without owner failure")) { return false; }
		owner.application_execution->RequestStop();
		if (!check(owner.application_execution->WaitForDrain(std::chrono::milliseconds(0)), "wait owners must drain")) { return false; }
	}
	return check(Common::GetHostLockCriticalCounter()->load() == baseline && Loader::GuestCallBoundary::CurrentDepth() == 0 &&
	                 Loader::HostCallReturnThunk::CurrentDepth() == 0, "wait fixture leaves host locks and boundary stacks balanced");
}
