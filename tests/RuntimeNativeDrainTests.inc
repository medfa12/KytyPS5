// Included in LibKernel. The worker is parked in a real native HOST epilogue,
// after logical cleanup but before native return/TLS teardown. No handle mocks.
namespace RuntimeNativeDrainFixture {
struct Park {
	std::mutex mutex;
	std::condition_variable cv;
	bool release = false;
	std::atomic<bool> entered {false};
};
struct Epilogue: Park {
	Loader::Program* owner = nullptr;
	std::atomic<unsigned> entry_destroyed {0};
	std::atomic<unsigned> epilogue_destroyed {0};
	std::atomic<bool> logical_zero {false};
	std::atomic<bool> on_host_stack {false};
};
struct Startup: Park {
	Loader::Program* owner = nullptr;
	std::atomic<unsigned> guest_entries {0};
};
static std::atomic<Epilogue*> active_epilogue {nullptr};
static std::atomic<Startup*> active_startup {nullptr};
static void Release(Park& park) {
	std::lock_guard lock(park.mutex);
	park.release = true;
	park.cv.notify_all();
}
static void Wait(Park& park) {
	std::unique_lock lock(park.mutex);
	park.entered.store(true, std::memory_order_release);
	park.cv.wait(lock, [&] { return park.release; });
}
struct Pin { std::atomic<unsigned>* destroyed; ~Pin() { ++*destroyed; } };
static void ParkNativeEpilogue(Pthread thread) {
	auto* epilogue = active_epilogue.load(std::memory_order_acquire);
	if (!epilogue || thread->arg != epilogue) { return; }
	Pin pin {&epilogue->epilogue_destroyed};
	uintptr_t marker = 0;
	const auto address = reinterpret_cast<uintptr_t>(&marker);
	const auto begin = reinterpret_cast<uintptr_t>(thread->attr->stack_addr);
	epilogue->on_host_stack.store(address < begin || address >= begin + thread->attr->stack_size);
	epilogue->logical_zero.store(thread->almost_done.load() && epilogue->owner->application_threads.load() == 0 &&
	    thread->execution->Outstanding() == 0 && !Loader::GuestCallBoundary::CurrentDepth() &&
	    !Loader::HostCallReturnThunk::CurrentDepth(), std::memory_order_release);
	Wait(*epilogue);
}
static void ParkNativeCreation() {
	auto* startup = active_startup.load(std::memory_order_acquire);
	if (startup && Loader::RuntimeLinker::GetCurrentProgram() == startup->owner) { Wait(*startup); }
}
static void* KYTY_SYSV_ABI Entry(void* argument) {
	Pin pin {&static_cast<Epilogue*>(argument)->entry_destroyed};
	return reinterpret_cast<void*>(0x1234);
}
static void* KYTY_SYSV_ABI StartupEntry(void* argument) {
	++static_cast<Startup*>(argument)->guest_entries;
	return nullptr;
}
template <typename Predicate> static bool Await(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}
}

bool TestRuntimeNativeDrain(std::string* failure) {
	using namespace RuntimeNativeDrainFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[3];
	Epilogue epilogues[2];
	Startup startup;
	Pthread workers[3] {};
	std::thread creator;
	int startup_result = KERNEL_ERROR_EINVAL;
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	const auto previous_epilogue = g_runtime_native_epilogue_for_test.load(std::memory_order_acquire);
	const auto previous_publish = g_runtime_thread_before_publish;
	struct Restore {
		Loader::Program* previous;
		Loader::Program* owners;
		Epilogue* epilogues;
		Startup& startup;
		Pthread* workers;
		std::thread& creator;
		RuntimeNativeEpilogueHook epilogue_hook;
		void (*publish_hook)();
		~Restore() {
			g_runtime_native_epilogue_for_test.store(epilogue_hook, std::memory_order_release);
			active_epilogue.store(nullptr, std::memory_order_release);
			active_startup.store(nullptr, std::memory_order_release);
			Release(epilogues[0]); Release(epilogues[1]); Release(startup);
			for (unsigned i = 0; i != 3; ++i) { if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); } }
			if (creator.joinable()) { creator.join(); }
			g_runtime_thread_before_publish = publish_hook; // No creator can still read this legacy test hook.
			// Older pointers can alias the reused newest record; reverse order
			// plus the free check prevents a second join of a stale incarnation.
			for (unsigned i = 3; i != 0; --i) { if (workers[i - 1] && !workers[i - 1]->free) { PthreadJoin(workers[i - 1], nullptr); } }
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {previous, owners, epilogues, startup, workers, creator, previous_epilogue, previous_publish};
	for (unsigned i = 0; i != 3; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 991000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owners[i]), "native drain requires supported runtime guest boundaries")) { return false; }
	}
	if (!check(PthreadOwnerNativeThreadsTerminated({}).status == PthreadOwnerNativeDrainStatus::InvalidOwner &&
	    PthreadOwnerNativeThreadsTerminated(owners[0].application_execution).status == PthreadOwnerNativeDrainStatus::AdmissionOpen,
	    "native completion rejects empty owner and open admission even with zero records")) { return false; }
	epilogues[0].owner = &owners[0]; epilogues[1].owner = &owners[2]; startup.owner = &owners[1];
	active_epilogue.store(&epilogues[0], std::memory_order_release);
	g_runtime_native_epilogue_for_test.store(ParkNativeEpilogue, std::memory_order_release);
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(PthreadCreate(&workers[0], nullptr, Entry, &epilogues[0], "native-drain-epilogue") == OK &&
	    Await([&] { return epilogues[0].entered.load(std::memory_order_acquire); }), "actual worker reached post-cleanup native host epilogue")) { return false; }
	if (!check(epilogues[0].logical_zero.load() && epilogues[0].on_host_stack.load() && epilogues[0].entry_destroyed == 1 &&
	    epilogues[0].epilogue_destroyed == 0 && !workers[0]->free,
	    "guest cleanup/counts/leases are zero while native host RAII remains live")) { return false; }
	owners[0].application_execution->RequestStop();
	if (!check(owners[0].application_execution->WaitForDrain(std::chrono::milliseconds(0)), "old logical drain gate already passes")) { return false; }
	const auto pending = PthreadOwnerNativeThreadsTerminated(owners[0].application_execution);
	if (!check(!pending && pending.status == PthreadOwnerNativeDrainStatus::Pending && pending.pending == 1 && pending.terminated == 0,
	    "native gate stays pending after logical drain until actual OS termination")) { return false; }
	// Native creation has succeeded, but guest-visible publication/startup is
	// deliberately parked under the creator's stop pin. Closed admission must
	// not turn that incomplete incarnation into an empty successful proof.
	active_startup.store(&startup, std::memory_order_release);
	g_runtime_thread_before_publish = ParkNativeCreation;
	creator = std::thread([&] {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[1]);
		startup_result = PthreadCreate(&workers[1], nullptr, StartupEntry, &startup, "native-drain-startup");
	});
	if (!check(Await([&] { return startup.entered.load(std::memory_order_acquire); }), "real native creation paused before owner publication")) { return false; }
	owners[1].application_execution->RequestStop();
	const auto starting = PthreadOwnerNativeThreadsTerminated(owners[1].application_execution);
	if (!check(!starting && starting.status == PthreadOwnerNativeDrainStatus::Pending && !workers[1] && startup.guest_entries == 0,
	    "unpublished native startup retains its Thread lease and remains pending")) { return false; }
	Release(startup);
	creator.join();
	g_runtime_thread_before_publish = previous_publish;
	active_startup.store(nullptr, std::memory_order_release);
	if (!check(startup_result == KERNEL_ERROR_ECANCELED && !workers[1] && startup.guest_entries == 0 &&
	    bool(PthreadOwnerNativeThreadsTerminated(owners[1].application_execution)),
	    "refused startup proves completion only after creator actually joins/reclaims native target")) { return false; }
	struct OwnedHandle {
		HANDLE handle = nullptr;
		~OwnedHandle() { if (handle) { CloseHandle(handle); } }
	} original, replacement;
	{
		std::lock_guard pin(workers[0]->stop_mutex);
		if (!check(DuplicateHandle(GetCurrentProcess(), workers[0]->stop_handle, GetCurrentProcess(), &original.handle,
		    0, FALSE, DUPLICATE_SAME_ACCESS) != 0, "capture independent actual handle for native termination assertion")) { return false; }
	}
	// Keep native join serialization unavailable, so successful proof MUST use
	// stop_mutex/the retained OS handle, not join_mutex or a recursive join.
	std::unique_lock join_pin(workers[0]->join_mutex);
	if (!check(WaitForSingleObject(original.handle, 0) == WAIT_TIMEOUT &&
	    PthreadOwnerNativeThreadsTerminated(owners[0].application_execution).status == PthreadOwnerNativeDrainStatus::Pending,
	    "native gate remains pending independently of unavailable join serialization")) { return false; }
	Release(epilogues[0]);
	if (!check(WaitForSingleObject(original.handle, 2000) == WAIT_OBJECT_0 && epilogues[0].epilogue_destroyed == 1,
	    "release finishes host epilogue/RAII and the actual native handle becomes signaled")) { return false; }
	const auto terminated = PthreadOwnerNativeThreadsTerminated(owners[0].application_execution);
	if (!check(bool(terminated) && terminated.status == PthreadOwnerNativeDrainStatus::Terminated && terminated.terminated == 1 &&
	    !terminated.pending && !workers[0]->free, "native gate observes retained signaled record without reaping or acquiring join_mutex")) { return false; }
	join_pin.unlock();
	void* value = nullptr;
	if (!check(PthreadJoin(workers[0], &value) == OK && value == reinterpret_cast<void*>(0x1234) &&
	    bool(PthreadOwnerNativeThreadsTerminated(owners[0].application_execution)), "actual native join reclaims a proven completed incarnation")) { return false; }
	// Reuse that physical pool record under a different lifetime. The old owner
	// gate must neither wait for nor reap the new owner's still-live native tail.
	active_epilogue.store(&epilogues[1], std::memory_order_release);
	Loader::RuntimeLinker::SetCurrentProgram(&owners[2]);
	if (!check(PthreadCreate(&workers[2], nullptr, Entry, &epilogues[1], "native-drain-reused") == OK &&
	    Await([&] { return epilogues[1].entered.load(std::memory_order_acquire); }) && workers[2] == workers[0],
	    "actual record reused under new owner lifetime while native epilogue stays live")) { return false; }
	{
		std::lock_guard pin(workers[2]->stop_mutex);
		if (!check(DuplicateHandle(GetCurrentProcess(), workers[2]->stop_handle, GetCurrentProcess(), &replacement.handle,
		    0, FALSE, DUPLICATE_SAME_ACCESS) != 0, "capture reused incarnation's actual native handle")) { return false; }
	}
	owners[2].application_execution->RequestStop();
	if (!check(epilogues[1].logical_zero.load() && bool(PthreadOwnerNativeThreadsTerminated(owners[0].application_execution)) &&
	    !PthreadOwnerNativeThreadsTerminated(owners[2].application_execution) && !workers[2]->free &&
	    WaitForSingleObject(replacement.handle, 0) == WAIT_TIMEOUT,
	    "old lifetime proof cannot certify/reap a reused foreign live native incarnation")) { return false; }
	Release(epilogues[1]);
	if (!check(WaitForSingleObject(replacement.handle, 2000) == WAIT_OBJECT_0 &&
	    bool(PthreadOwnerNativeThreadsTerminated(owners[2].application_execution)) && PthreadJoin(workers[2], nullptr) == OK,
	    "reused owner's own native gate waits for its real termination before actual join")) { return false; }
	for (auto& owner: owners) {
		if (!check(!owner.application_execution->Failure() && owner.application_execution->Outstanding() == 0 &&
		    owner.application_threads.load() == 0 && bool(PthreadOwnerNativeThreadsTerminated(owner.application_execution)),
		    "all owners genuinely native-complete with no guest failure or leaked work")) { return false; }
	}
	return check(Common::GetHostLockCriticalCounter()->load() == baseline, "native drain leaves host-critical accounting balanced");
#else
	return check(false, "Windows x86-64 native drain fixture unavailable (not exercised)");
#endif
}
