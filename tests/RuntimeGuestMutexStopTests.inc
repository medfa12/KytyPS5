// Included in LibKernel. Real owner-bound native workers execute the public
// mutex HLE/Posix adapters through returning host gateways, not a cancellation mock.
namespace RuntimeGuestMutexStopFixture {
constexpr unsigned OwnerCount = 6;
enum class Mode { Infinite, Relative, PosixInfinite, PosixAbsolute };
struct Resource { PthreadMutex mutex = nullptr; bool main_owned = false; };
struct Context {
	PthreadMutex* slot = nullptr;
	PthreadMutex inspect = nullptr;
	Loader::RuntimeGuestExecution* execution = nullptr;
	Mode mode = Mode::Infinite;
	KernelUseconds budget = 60000000;
	uint64_t gateway = 0;
	bool stop_before = false;
	std::atomic<bool> entered {false};
	std::atomic<bool> returned {false};
	int result = KERNEL_ERROR_EINVAL;
	unsigned destroyed = 0;
	unsigned continued = 0;
	bool owns_after_error = false;
	std::chrono::steady_clock::duration elapsed {};
};
struct Pin { unsigned* destroyed; ~Pin() { ++*destroyed; } };
static uint64_t KYTY_SYSV_ABI HostLock(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {&context->destroyed};
	const auto start = std::chrono::steady_clock::now();
	context->entered.store(true, std::memory_order_release);
	if (context->stop_before) { context->execution->RequestStop(); }
	switch (context->mode) {
		case Mode::Infinite: context->result = PthreadMutexLock(context->slot); break;
		case Mode::Relative: context->result = PthreadMutexTimedlock(context->slot, context->budget); break;
		case Mode::PosixInfinite: context->result = Posix::pthread_mutex_lock(context->slot); break;
		case Mode::PosixAbsolute: {
			KernelTimespec deadline {};
			NativeClockGettime(KERNEL_CLOCK_REALTIME, &deadline);
			deadline.tv_nsec += static_cast<int64_t>(context->budget) * 1000;
			deadline.tv_sec += deadline.tv_nsec / 1000000000;
			deadline.tv_nsec %= 1000000000;
			context->result = Posix::pthread_mutex_timedlock(context->slot, &deadline);
			break;
		}
	}
	if (context->result == OK) { PthreadMutexUnlock(context->slot); }
	else if (context->inspect) {
		std::lock_guard lock(context->inspect->m);
		context->owns_after_error = context->inspect->owner == PthreadSelfOrNull();
	}
	context->elapsed = std::chrono::steady_clock::now() - start;
	context->returned.store(true, std::memory_order_release);
	return static_cast<uint64_t>(context->result);
}
static void* KYTY_SYSV_ABI Guest(void* argument) {
	auto* context = static_cast<Context*>(argument);
	reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(context->gateway)(context);
	++context->continued;
	return reinterpret_cast<void*>(0x71);
}
static void* KYTY_SYSV_ABI Direct(void* argument) {
	HostLock(argument);
	++static_cast<Context*>(argument)->continued;
	return reinterpret_cast<void*>(0x72);
}
template <typename Predicate> static bool Await(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}
static unsigned Waiters(PthreadMutex mutex) {
	std::lock_guard lock(mutex->m);
	unsigned count = 0;
	for (auto* waiter = mutex->first_waiter; waiter && count < 64; waiter = waiter->next) { ++count; }
	return count;
}
static bool State(PthreadMutex mutex, Pthread owner, uint32_t count) {
	std::lock_guard lock(mutex->m);
	return mutex->owner == owner && mutex->count == count && !mutex->first_waiter && !mutex->last_waiter;
}
static std::atomic<PthreadMutex> handoff_target {nullptr};
static std::atomic<unsigned> handoff_calls {0};
static void StopAfterTransfer(PthreadMutex mutex, Loader::RuntimeGuestExecution* execution) {
	if (mutex == handoff_target.load(std::memory_order_acquire)) {
		++handoff_calls;
		execution->RequestStop(); // Exact FIFO-selected/removed-before-return race.
	}
}
}

bool TestRuntimeGuestMutexStop(std::string* failure) {
	using namespace RuntimeGuestMutexStopFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[OwnerCount], session;
	Resource resources[OwnerCount];
	PthreadMutex ownerless = nullptr;
	Context contexts[12];
	Pthread workers[12] {};
	std::vector<std::unique_ptr<Loader::HostCallReturnThunk>> gateways;
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	struct Restore {
		Loader::Program* previous;
		Loader::Program* owners;
		Resource* resources;
		Pthread* workers;
		~Restore() {
			g_runtime_mutex_handoff_for_test.store(nullptr, std::memory_order_release);
			handoff_target.store(nullptr, std::memory_order_release);
			for (unsigned i = 0; i != OwnerCount; ++i) {
				if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); }
				if (resources[i].main_owned) { PthreadMutexUnlock(&resources[i].mutex); resources[i].main_owned = false; }
			}
			for (unsigned i = 12; i != 0; --i) { if (workers[i - 1] && !workers[i - 1]->free) { PthreadJoin(workers[i - 1], nullptr); } }
			for (unsigned i = 0; i != OwnerCount; ++i) { if (resources[i].mutex) { PthreadMutexDestroy(&resources[i].mutex); } }
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {previous, owners, resources, workers};
	for (unsigned i = 0; i != OwnerCount; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 990000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owners[i]), "mutex stop requires supported runtime guest boundaries")) { return false; }
		Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
		if (!check(PthreadMutexInit(&resources[i].mutex, nullptr, "runtime-mutex-stop") == OK,
		           "initialize lifetime-owned mutex")) { return false; }
		if (i != 5) {
			if (!check(PthreadMutexLock(&resources[i].mutex) == OK, "host fixture retains native mutex ownership")) { return false; }
			resources[i].main_owned = true;
		}
	}
	const auto create = [&](unsigned owner, unsigned index, bool gateway = true) {
		auto& context = contexts[index];
		if (owner < OwnerCount) {
			Loader::RuntimeLinker::SetCurrentProgram(&owners[owner]);
			context.execution = owners[owner].application_execution.get();
		} else { Loader::RuntimeLinker::SetCurrentProgram(&session); }
		if (gateway) {
			auto entry = Loader::HostCallReturnThunk::Create(context.execution->Boundary(), reinterpret_cast<uint64_t>(HostLock),
			    context.execution, Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall);
			if (!check(bool(entry), "create exact returning mutex gateway")) { return false; }
			context.gateway = entry->GetAddress();
			gateways.push_back(std::move(entry));
		}
		return check(PthreadCreate(&workers[index], nullptr, gateway ? Guest : Direct, &context, "runtime-mutex-wait") == OK,
		             "create actual native mutex worker");
	};
	const auto joined = [&](unsigned index, bool canceled) {
		void* value = nullptr;
		return check(PthreadJoin(workers[index], &value) == OK &&
		    (!canceled || value == reinterpret_cast<void*>(~uintptr_t {0})), "actually join native mutex worker");
	};
	for (unsigned i = 0; i != 4; ++i) {
		contexts[i].slot = &resources[0].mutex;
		contexts[i].inspect = resources[0].mutex;
		contexts[i].mode = static_cast<Mode>(i);
		if (!create(0, i)) { return false; }
	}
	contexts[4].slot = &resources[1].mutex;
	contexts[4].inspect = resources[1].mutex;
	if (!create(1, 4) || !check(Await([&] { return Waiters(resources[0].mutex) == 4 && Waiters(resources[1].mutex) == 1; }),
	    "indefinite/long kernel and POSIX mutex operations really park")) { return false; }
	owners[0].application_execution->RequestStop();
	if (!check(Await([&] { return contexts[0].returned.load() && contexts[1].returned.load() &&
	    contexts[2].returned.load() && contexts[3].returned.load(); }), "stop releases all standalone waits without owner unlock")) { return false; }
	for (unsigned i = 0; i != 4; ++i) {
		if (!joined(i, true) || !check(contexts[i].result == (i < 2 ? KERNEL_ERROR_ECANCELED : Posix::POSIX_ECANCELED) &&
		    !contexts[i].owns_after_error && contexts[i].destroyed == 1 && contexts[i].continued == 0,
		    "terminal mutex stop returns through host RAII and suppresses guest continuation")) { return false; }
	}
	if (!check(State(resources[0].mutex, PthreadSelfOrNull(), 1) && !contexts[4].returned.load() &&
	    Waiters(resources[1].mutex) == 1 && !owners[1].application_execution->StopRequested(),
	    "stop removes only owned waiters without touching blocker ownership or isolated owner")) { return false; }
	PthreadMutexUnlock(&resources[1].mutex); resources[1].main_owned = false;
	if (!check(Await([&] { return contexts[4].returned.load(); }), "isolated owner receives ordinary ownership transfer") ||
	    !joined(4, false) || !check(contexts[4].result == OK && contexts[4].continued == 1 &&
	    State(resources[1].mutex, nullptr, 0), "ordinary isolated lock/unlock semantics retained")) { return false; }
	// Timeout budget survives native spurious wakes without dropping/requeueing.
	contexts[5].slot = &resources[2].mutex; contexts[5].inspect = resources[2].mutex;
	contexts[5].mode = Mode::Relative; contexts[5].budget = 45000;
	if (!create(2, 5)) { return false; }
	const auto timeout_limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	while (!contexts[5].returned.load() && std::chrono::steady_clock::now() < timeout_limit) {
		{
			std::lock_guard lock(resources[2].mutex->m);
			for (auto* waiter = resources[2].mutex->first_waiter; waiter; waiter = waiter->next) { waiter->cv.notify_one(); }
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	if (!check(contexts[5].returned.load(), "spurious wakes do not restart timed mutex budget") || !joined(5, false) ||
	    !check(contexts[5].result == KERNEL_ERROR_ETIMEDOUT && contexts[5].continued == 1 && contexts[5].destroyed == 1 &&
	    contexts[5].elapsed >= std::chrono::milliseconds(35) && contexts[5].elapsed < std::chrono::milliseconds(500) &&
	    State(resources[2].mutex, PthreadSelfOrNull(), 1), "timeout leaves owner and FIFO list intact")) { return false; }
	// Raw same-owner call, foreign live mutex, stale lifetime and ownerless lazy
	// word must refuse BEFORE acquisition, shadow repair or lazy publication.
	contexts[6].slot = &resources[2].mutex; contexts[6].inspect = resources[2].mutex;
	contexts[7].slot = &resources[1].mutex; contexts[7].inspect = resources[1].mutex;
	contexts[8].slot = &resources[3].mutex; contexts[8].inspect = resources[3].mutex;
	const auto real_owner = resources[3].mutex->wait_owner;
	++resources[3].mutex->wait_owner.token;
	if (!create(2, 6, false) || !create(2, 7) || !create(3, 8)) { return false; }
	for (unsigned i: {6u, 7u, 8u}) {
		if (!check(Await([&] { return contexts[i].returned.load(); }), "unsafe runtime mutex operation refused") || !joined(i, false) ||
		    !check(contexts[i].result == KERNEL_ERROR_EOPNOTSUPP && contexts[i].destroyed == 1 && contexts[i].continued == 1 &&
		    !contexts[i].owns_after_error, "missing barrier/foreign/stale mutex never acquired")) { return false; }
	}
	resources[3].mutex->wait_owner = real_owner;
	// An active shared callback on the legacy main pthread is still a shared
	// guest operation: absence of a physical-thread execution binding is NOT a
	// license to bypass the matching return-gateway requirement.
	Loader::RuntimeLinker::SetCurrentProgram(&owners[2]);
	Context host_thread_callback;
	host_thread_callback.slot = &resources[2].mutex; host_thread_callback.inspect = resources[2].mutex;
	const auto callback = owners[2].application_execution->InvokeCallback(reinterpret_cast<uint64_t>(Direct), &host_thread_callback, true);
	if (!check(callback.Returned() && host_thread_callback.result == KERNEL_ERROR_EOPNOTSUPP &&
	    host_thread_callback.destroyed == 1 && host_thread_callback.continued == 1 &&
	    State(resources[2].mutex, PthreadSelfOrNull(), 1), "shared callback on session pthread cannot fall through to legacy lock semantics")) { return false; }
	contexts[11].slot = &ownerless;
	if (!create(2, 11) || !check(Await([&] { return contexts[11].returned.load(); }), "ownerless lazy mutex refused") || !joined(11, false)) { return false; }
	const void* shadow = nullptr;
	if (!check(contexts[11].result == KERNEL_ERROR_EOPNOTSUPP && !ownerless &&
	    !PthreadShadowGet(reinterpret_cast<uint64_t>(&ownerless), PthreadStaticObject::Type::Mutex, &shadow),
	    "ownerless lazy refusal neither mutates word nor publishes a shadow object")) { return false; }
	// Deterministic stop AFTER FIFO ownership has been transferred/unlinked.
	// A legacy successor still has to acquire normally; cancellation cannot
	// strand ownership in the stopped worker or delete its waiter twice.
	contexts[9].slot = &resources[4].mutex; contexts[9].inspect = resources[4].mutex;
	contexts[10].slot = &resources[4].mutex; contexts[10].inspect = resources[4].mutex;
	// Legacy successor has a failure-cleanup bound if a broken implementation
	// strands ownership in the canceled first waiter. Success still requires
	// actual FIFO acquisition, never that timeout.
	contexts[10].mode = Mode::Relative; contexts[10].budget = 5000000;
	if (!create(4, 9) || !check(Await([&] { return Waiters(resources[4].mutex) == 1; }), "runtime FIFO waiter parked") ||
	    !create(OwnerCount, 10, false) || !check(Await([&] { return Waiters(resources[4].mutex) == 2; }), "legacy FIFO successor parked")) { return false; }
	handoff_calls.store(0);
	handoff_target.store(resources[4].mutex, std::memory_order_release);
	g_runtime_mutex_handoff_for_test.store(StopAfterTransfer, std::memory_order_release);
	PthreadMutexUnlock(&resources[4].mutex); resources[4].main_owned = false;
	if (!check(Await([&] { return contexts[9].returned.load() && contexts[10].returned.load(); }),
	    "terminal handoff releases ownership onward without another unlock") || !joined(9, true) || !joined(10, false)) { return false; }
	g_runtime_mutex_handoff_for_test.store(nullptr, std::memory_order_release);
	handoff_target.store(nullptr, std::memory_order_release);
	if (!check(handoff_calls == 1 && contexts[9].result == KERNEL_ERROR_ECANCELED && contexts[9].continued == 0 &&
	    contexts[9].destroyed == 1 && contexts[10].result == OK && contexts[10].continued == 1 && contexts[10].destroyed == 1 &&
	    State(resources[4].mutex, nullptr, 0), "FIFO transfer/stop race releases selected ownership and cleans both waiter endpoints")) { return false; }
	// Already-terminal acquisition must not even take a currently free mutex.
	// Reuse an already joined context slot only after all readers are gone.
	contexts[11].slot = &resources[5].mutex; contexts[11].inspect = resources[5].mutex;
	contexts[11].stop_before = true; contexts[11].returned.store(false);
	contexts[11].destroyed = 0; contexts[11].continued = 0;
	if (!create(5, 11) || !check(Await([&] { return contexts[11].returned.load(); }), "already stopped mutex call returned") ||
	    !joined(11, true) || !check(contexts[11].result == KERNEL_ERROR_ECANCELED && contexts[11].continued == 0 &&
	    contexts[11].destroyed == 1 && State(resources[5].mutex, nullptr, 0), "terminal call never acquires or queues")) { return false; }
	for (auto& owner: owners) {
		if (!check(!owner.application_execution->Failure() && owner.application_execution->Outstanding() == 0 &&
		    owner.application_threads.load() == 0, "all mutex paths release host/guest/thread leases without owner failure")) { return false; }
	}
	return check(Common::GetHostLockCriticalCounter()->load() == baseline && Loader::GuestCallBoundary::CurrentDepth() == 0 &&
	    Loader::HostCallReturnThunk::CurrentDepth() == 0, "mutex stop leaves host-critical and boundary stacks balanced");
}
