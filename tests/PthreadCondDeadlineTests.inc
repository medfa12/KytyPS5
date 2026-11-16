struct CondDeadlineSignal {
	Libs::LibKernel::PthreadMutex* mutex;
	Libs::LibKernel::PthreadCond* cond;
	int lock_result = -1;
	int signal_result = -1;
	int unlock_result = -1;
};

void* KYTY_SYSV_ABI CondDeadlineSignalWorker(void* opaque) {
	using namespace Libs::LibKernel;
	auto& state = *static_cast<CondDeadlineSignal*>(opaque);
	state.lock_result = PthreadMutexLock(state.mutex);
	if (state.lock_result == 0) {
		state.signal_result = PthreadCondSignal(state.cond);
		state.unlock_result = PthreadMutexUnlock(state.mutex);
	}
	return nullptr;
}

void TestPthreadCondDeadline() {
	using namespace Libs::LibKernel;
	constexpr const char* test = "PthreadCondDeadline";
	Initialize();
	PthreadInitSelfForMainThread();
	(void)Common::Singleton<Loader::RuntimeLinker>::Instance();
	for (const int clock : {0, 4}) {
		PthreadCondattr attr = nullptr;
		PthreadCond cond = nullptr;
		PthreadMutex mutex = nullptr;
		CheckOk(test, PthreadCondattrInit(&attr), "condition attributes");
		CheckOk(test, PthreadCondattrSetclock(&attr, clock), "condition clock");
		CheckOk(test, PthreadCondInit(&cond, &attr, "deadline-regression"), "condition init");
		CheckOk(test, PthreadCondattrDestroy(&attr), "attribute destroy");
		CheckOk(test, PthreadMutexInit(&mutex, nullptr, "deadline-regression"), "mutex init");
		KernelTimespec near_future {};
		CheckOk(test, KernelClockGettime(clock, &near_future), "read guest condition clock");
		near_future.tv_sec += 5;
		for (const KernelTimespec deadline : {
		         near_future,
		         KernelTimespec{9223372036854LL, 775807000},
		         KernelTimespec{INT64_MAX, 999999999}}) {
			CheckOk(test, PthreadMutexLock(&mutex), "lock before wait");
			CondDeadlineSignal state {&mutex, &cond};
			Pthread worker = nullptr;
			const int created = PthreadCreate(&worker, nullptr, CondDeadlineSignalWorker, &state, "deadline-signal");
			CheckOk(test, created, "create signal worker");
			if (created == 0) {
				CheckOk(test, PthreadCondTimedwaitAbs(&cond, &mutex, &deadline),
				        "far-future absolute wait wakes by signal rather than overflowing");
			}
			CheckOk(test, PthreadMutexUnlock(&mutex), "wait reacquires caller mutex");
			if (created == 0) {
				CheckOk(test, PthreadJoin(worker, nullptr), "join signal worker");
				Check(test, state.lock_result == 0 && state.signal_result == 0 && state.unlock_result == 0,
				      "signal worker completes under caller mutex");
			}
		}
		CheckOk(test, PthreadMutexLock(&mutex), "lock before expired wait");
		const KernelTimespec expired {0, 0};
		Check(test, PthreadCondTimedwaitAbs(&cond, &mutex, &expired) == KERNEL_ERROR_ETIMEDOUT,
		      "past deadline still times out");
		const KernelTimespec invalid {1, 1000000000};
		Check(test, PthreadCondTimedwaitAbs(&cond, &mutex, &invalid) == KERNEL_ERROR_EINVAL,
		      "invalid nanoseconds still fail validation");
		CheckOk(test, PthreadMutexUnlock(&mutex), "unlock after expired/invalid wait");
		CheckOk(test, PthreadCondDestroy(&cond), "condition destroy");
		CheckOk(test, PthreadMutexDestroy(&mutex), "mutex destroy");
	}
}
