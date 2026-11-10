// Included in kernel/pthread.cpp. Real native handles, native joins and actual
// pool-record reuse: no numeric-ID/OpenThread mock and no forced thread death.
namespace RuntimeNativeTargetPinFixture {
struct Park {
	std::atomic<bool> entered {false};
	std::mutex mutex;
	std::condition_variable cv;
	bool release = false;
};
static void Release(Park& park) {
	std::lock_guard lock(park.mutex);
	park.release = true;
	park.cv.notify_all();
}
static void* KYTY_SYSV_ABI Entry(void* argument) {
	auto& park = *static_cast<Park*>(argument);
	std::unique_lock lock(park.mutex);
	park.entered.store(true, std::memory_order_release);
	// Periodically return to user mode so the real special-APC exclusion probe
	// does not depend on whether this host's condition wait is alertable.
	while (!park.release) { park.cv.wait_for(lock, std::chrono::milliseconds(5)); }
	return reinterpret_cast<void*>(0x1234);
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

bool TestRuntimeNativeTargetPins(std::string* failure) {
	using namespace RuntimeNativeTargetPinFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::Program session;
	Loader::Program shared;
	shared.application_owner = &shared;
	Park old_park, replacement_park, shared_park;
	Pthread old_worker = nullptr, replacement = nullptr, shared_worker = nullptr;
	std::thread joiner;
	int join_result = KERNEL_ERROR_EINVAL;
	void* join_value = nullptr;
	struct Restore {
		Loader::Program* previous;
		Park& old_park;
		Park& replacement_park;
		Park& shared_park;
		Pthread& old_worker;
		Pthread& replacement;
		Pthread& shared_worker;
		std::thread& joiner;
		~Restore() {
			Release(old_park); Release(replacement_park); Release(shared_park);
			if (joiner.joinable()) { joiner.join(); }
			// After the first join, old_worker may alias replacement. Never reap
			// that stale handle a second time.
			if (old_worker && !old_worker->free) { PthreadJoin(old_worker, nullptr); }
			if (replacement && replacement != old_worker && !replacement->free) { PthreadJoin(replacement, nullptr); }
			if (shared_worker && !shared_worker->free) { PthreadJoin(shared_worker, nullptr); }
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {previous, old_park, replacement_park, shared_park, old_worker, replacement, shared_worker, joiner};
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	PthreadLegacyNativeTarget main_target;
	if (!check(PthreadPinLegacyNativeTarget(PthreadSelfOrNull(), &main_target) == OK &&
	               main_target.ThreadId() == GetCurrentThreadId(), "legacy main publishes its actual native handle")) { return false; }
	main_target.UnlockRecord();
	PthreadLegacyNativeTarget invalid;
	if (!check(PthreadPinLegacyNativeTarget(nullptr, &invalid) == KERNEL_ERROR_ESRCH && !invalid.Handle(),
	           "invalid native target admission leaves output empty")) { return false; }
	if (!check(PthreadCreate(&old_worker, nullptr, Entry, &old_park, "native-pin-old") == OK &&
	               Await([&] { return old_park.entered.load(std::memory_order_acquire); }), "legacy native worker entered")) { return false; }
	joiner = std::thread([&] { join_result = PthreadJoin(old_worker, &join_value); });
	if (!check(Await([&] {
		std::unique_lock lock(old_worker->join_mutex, std::try_to_lock);
		return !lock;
	}), "native join already owns original pthread handle before target admission")) { return false; }
	PthreadLegacyNativeTarget old_target;
	if (!check(PthreadPinLegacyNativeTarget(old_worker, &old_target) == OK,
	           "legacy admission uses separate native handle while pthread_join is blocked")) { return false; }
	const auto old_id = old_target.ThreadId();
	const auto old_handle = static_cast<HANDLE>(old_target.Handle());
	Release(old_park);
	if (!check(WaitForSingleObject(old_handle, 2000) == WAIT_OBJECT_0,
	           "admitted duplicate observes actual native termination before record reclamation")) { return false; }
	old_target.UnlockRecord(); // Keep owned HANDLE while allowing actual join/reuse.
	joiner.join();
	if (!check(join_result == OK && join_value == reinterpret_cast<void*>(0x1234) && old_worker->free,
	           "actual native join reclaims original handle and record")) { return false; }
	if (!check(PthreadCreate(&replacement, nullptr, Entry, &replacement_park, "native-pin-reused") == OK &&
	               Await([&] { return replacement_park.entered.load(std::memory_order_acquire); }) && replacement == old_worker,
	           "actual legacy pool record reused while old admitted handle remains owned")) { return false; }
	PthreadLegacyNativeTarget replacement_target;
	if (!check(PthreadPinLegacyNativeTarget(replacement, &replacement_target) == OK,
	           "replacement has a separately admitted native object")) { return false; }
	const auto replacement_handle = static_cast<HANDLE>(replacement_target.Handle());
	replacement_target.UnlockRecord();
	if (!check(GetThreadId(old_handle) == old_id && WaitForSingleObject(old_handle, 0) == WAIT_OBJECT_0 &&
	               WaitForSingleObject(replacement_handle, 0) == WAIT_TIMEOUT,
	           "old admitted handle stays terminated; it cannot resolve to the reused live record")) { return false; }
	if (!check(Loader::InitializeApplicationExecution(&shared), "shared native pin fixture requires supported boundary/CET policy")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&shared);
	if (!check(PthreadCreateRuntimeCoordinator(&shared_worker, nullptr, Entry, &shared_park, "native-pin-shared") == OK &&
	               Await([&] { return shared_park.entered.load(std::memory_order_acquire); }), "shared coordinator entered")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	PthreadLegacyNativeTarget rejected;
	shared_worker->pending_signal_mask.store(1ull << 30, std::memory_order_release);
	if (!check(PthreadPinLegacyNativeTarget(shared_worker, &rejected) == KERNEL_ERROR_EOPNOTSUPP && !rejected.Handle() &&
	               shared_worker->pending_signal_mask.load() == (1ull << 30), "shared admission rejected before handle or pending-work mutation")) { return false; }
	shared_worker->pending_signal_mask.store(0, std::memory_order_release);
	struct OwnedHandle {
		HANDLE handle = nullptr;
		~OwnedHandle() { if (handle) { CloseHandle(handle); } }
	} shared_target;
	{
		// Fixture-only async exclusion probe; production legacy admission above
		// MUST refuse this shared target before exposing a native handle.
		std::lock_guard pin(shared_worker->stop_mutex);
		if (!check(DuplicateHandle(GetCurrentProcess(), shared_worker->stop_handle, GetCurrentProcess(),
		    &shared_target.handle, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0, "duplicate actual shared handle for exclusion probe")) { return false; }
	}
	const auto suspended = SuspendThread(old_handle);
	if (suspended != static_cast<DWORD>(-1)) { ResumeThread(old_handle); }
	if (!check(suspended == static_cast<DWORD>(-1) && !replacement->almost_done && !shared_worker->almost_done &&
	               WaitForSingleObject(replacement_handle, 0) == WAIT_TIMEOUT,
	           "native operation on stale admitted object cannot suspend replacement or shared worker")) { return false; }
	Release(replacement_park);
	if (!check(WaitForSingleObject(replacement_handle, 2000) == WAIT_OBJECT_0 &&
	               TestRuntimeNativeTargetPinServices(replacement, shared_target.handle),
	           "direct signals, real failed-enqueue rollback and actual shared-thread APC exclusion")) { return false; }
	if (!check(PthreadJoin(replacement, nullptr) == OK, "replacement actually joined")) { return false; }
	Release(shared_park);
	if (!check(PthreadJoin(shared_worker, nullptr) == OK && shared.application_execution->Outstanding() == 0,
	           "shared coordinator unaffected and actually joined")) { return false; }
	return true;
#else
	return check(false, "Windows native-target pin fixture unavailable (not exercised)");
#endif
}
