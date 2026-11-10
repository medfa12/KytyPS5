// Private local-account callback engine, included inside Libs::UserService.
// 13.60 +0x2380 runs dispatch on SceUserServiceEvent; +0x22fc acknowledges
// login only after callbacks. Never enter guest code on a store/registry lock.
namespace {
struct LocalCallbackRun: std::enable_shared_from_this<LocalCallbackRun> {
	Loader::Program* program = nullptr;
	std::shared_ptr<Loader::BootCredentialLifetime> lifetime;
	std::shared_ptr<Loader::RuntimeGuestExecution> execution;
	int pid = -1;
	LibKernel::Pthread thread = nullptr;
	LibKernel::PthreadNativeReceipt receipt;
	bool starting = true, stop = false, failed = false, reaping = false;
	uint64_t submitted = 0, completed = 0;
	using Pair = std::pair<uint64_t, uint64_t>;
	std::vector<Pair> callbacks;
	Pair active {};
	std::deque<std::pair<uint64_t, SceUserServiceEvent>> events;
};
Loader::GuestExecutionMutex g_callback_mutex;
std::condition_variable_any g_callback_cv;
std::map<Loader::Program*, std::shared_ptr<LocalCallbackRun>> g_callback_runs;
thread_local LocalCallbackRun* g_dispatching_user_callback = nullptr;
bool g_local_transition_active = false;

// Serialize session state through its acknowledgment without holding a host
// lock during guest execution. Nested transitions from the worker are rejected
// by the entry points; read queries and registration remain reentrant.
struct LocalSessionTransition {
	LocalSessionTransition() {
		std::unique_lock lock(g_callback_mutex);
		g_callback_cv.wait(lock, [] { return !g_local_transition_active; });
		g_local_transition_active = true;
	}
	~LocalSessionTransition() {
		std::lock_guard lock(g_callback_mutex);
		g_local_transition_active = false;
		g_callback_cv.notify_all();
	}
};

static bool CallbackOwnerCurrent(const LocalCallbackRun& run) {
	auto self = LibKernel::PthreadSelfOrNull();
	int pid = -1;
	return self && LibKernel::PthreadGetOwnerProgram(self) == run.program &&
	    Loader::RuntimeLinker::GetCurrentProgram() == run.program &&
	    run.program->boot_credential_lifetime == run.lifetime &&
	    Loader::TryGetCurrentRetainedGuestProcessId(&pid) && pid == run.pid &&
	    Loader::IsGuestProcessIdentityActive(pid);
}

static void* KYTY_SYSV_ABI LocalCallbackWorker(void* pointer) {
	auto run = static_cast<LocalCallbackRun*>(pointer)->shared_from_this();
	const int retained = LibKernel::PthreadRetainCurrentUntilJoin(run);
	std::unique_lock lock(g_callback_mutex);
	if (retained || !CallbackOwnerCurrent(*run)) { run->failed = true; }
	try {
		while (!run->stop && !run->failed) {
			g_callback_cv.wait(lock, [&] { return run->stop || run->failed || !run->events.empty(); });
			if (run->stop || run->failed) { break; }
			const auto [sequence, event] = run->events.front();
			run->events.pop_front();
			std::vector<LocalCallbackRun::Pair> delivered;
			for (;;) {
				// Firmware rescans after each callback, deduplicating by the pair.
				// Unregistered callbacks are skipped; newly registered ones participate.
				auto next = std::find_if(run->callbacks.begin(), run->callbacks.end(), [&](const auto& pair) {
					return std::find(delivered.begin(), delivered.end(), pair) == delivered.end();
				});
				if (run->stop || run->failed || next == run->callbacks.end()) { break; }
				const auto pair = *next;
				delivered.push_back(pair);
				run->active = pair;
				lock.unlock();
				g_dispatching_user_callback = run.get();
				const uint64_t arguments[] {reinterpret_cast<uint64_t>(&event), pair.second};
				const bool current = CallbackOwnerCurrent(*run);
				LOGF("userservice-local: callback begin pid=%d event=0x%x user=0x%x target=0x%llx\n",
				     run->pid, static_cast<int>(event.event_type), event.user_id, static_cast<unsigned long long>(pair.first));
				const bool returned = current && run->execution->InvokeIntegerCallback(pair.first, arguments, 2).Returned();
				LOGF("userservice-local: callback end pid=%d event=0x%x target=0x%llx returned=%d\n",
				     run->pid, static_cast<int>(event.event_type), static_cast<unsigned long long>(pair.first), returned);
				g_dispatching_user_callback = nullptr;
				lock.lock();
				run->active = {};
				if (!returned) { run->failed = true; }
				g_callback_cv.notify_all();
			}
			if (!run->stop && !run->failed) { run->completed = sequence; }
			g_callback_cv.notify_all();
		}
	} catch (...) {
		if (!lock.owns_lock()) { lock.lock(); }
		run->failed = true;
		run->active = {};
		g_dispatching_user_callback = nullptr;
	}
	g_callback_cv.notify_all();
	return nullptr;
}

static bool CallbackInOwnerText(Loader::Program* program, uint64_t callback) {
	if (!program->elf) { return false; }
	const auto* ph = program->elf->GetPhdr();
	for (size_t i = 0; i < program->elf->GetEhdr()->e_phnum; ++i) {
		const uint64_t start = program->base_vaddr + ph[i].p_vaddr;
		if (ph[i].p_type == Loader::PT_LOAD && (ph[i].p_flags & Loader::PF_X) &&
		    callback >= start && callback - start < ph[i].p_filesz) { return true; }
	}
	return false;
}

// Caller has already passed retained guest/boot-role and local-selection checks.
static int ChangeLocalCallback(uint64_t callback, uint64_t userdata, bool add) try {
	if (!callback) { return add ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (add && !CallbackInOwnerText(program, callback)) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	const LocalCallbackRun::Pair pair {callback, userdata};
	std::unique_lock lock(g_callback_mutex);
	auto found = g_callback_runs.find(program);
	if (found == g_callback_runs.end()) {
		if (!add) { return OK; }
		auto run = std::make_shared<LocalCallbackRun>();
		run->program = program;
		run->lifetime = program->boot_credential_lifetime;
		if (!Loader::TryGetCurrentRetainedGuestProcessId(&run->pid)) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		run->execution = Loader::RuntimeGuestExecution::Create(program);
		if (!run->execution) { return USER_SERVICE_ERROR_NO_MEMORY; }
		run->callbacks.push_back(pair);
		g_callback_runs.emplace(program, run); // Retains startup across unlocked pthread creation.
		lock.unlock();
		const int result = LibKernel::PthreadCreateWithNativeReceipt(&run->thread, nullptr, LocalCallbackWorker,
		    run.get(), "SceUserServiceEventHle", run->receipt);
		lock.lock();
		run->starting = false;
		if (result) { run->failed = true; }
		g_callback_cv.notify_all();
		LOGF("userservice-local: callback worker pid=%d target=0x%llx result=0x%x\n", run->pid,
		     static_cast<unsigned long long>(callback), result);
		return result ? USER_SERVICE_ERROR_INTERNAL : OK;
	}
	auto run = found->second;
	if (run->starting || run->stop || run->failed || !CallbackOwnerCurrent(*run)) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	if (add) {
		if (std::find(run->callbacks.begin(), run->callbacks.end(), pair) == run->callbacks.end()) { run->callbacks.push_back(pair); }
	} else {
		std::erase(run->callbacks, pair);
		// Other threads must not free userdata until an admitted invocation returns.
		if (g_dispatching_user_callback != run.get()) {
			g_callback_cv.wait(lock, [&] { return run->active != pair; });
		}
	}
	return OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

static void QueueLocalCallbacks(const SceUserServiceEvent& event) {
	std::lock_guard lock(g_callback_mutex);
	for (auto& [program, run]: g_callback_runs) {
		if (run->stop || run->failed) { continue; }
		try {
			if (run->submitted == UINT64_MAX) { run->failed = true; continue; }
			run->events.emplace_back(run->submitted + 1, event);
			++run->submitted;
		} catch (...) { run->failed = true; }
	}
	g_callback_cv.notify_all();
}

static int WaitLocalCallbacks(int result) try {
	if (result) { return result; }
	std::unique_lock lock(g_callback_mutex);
	// Snapshot shared ownership before waiting; retirement may erase the registry.
	std::vector<std::pair<std::shared_ptr<LocalCallbackRun>, uint64_t>> pending;
	for (const auto& [program, run]: g_callback_runs) { pending.emplace_back(run, run->submitted); }
	for (const auto& [run, sequence]: pending) {
		g_callback_cv.wait(lock, [&] { return run->stop || run->failed || run->completed >= sequence; });
		if (run->stop || run->failed) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	}
	return OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

static bool ReleaseLocalCallbacks(Loader::Program* program) {
	std::unique_lock lock(g_callback_mutex);
	auto found = g_callback_runs.find(program);
	if (found == g_callback_runs.end()) { return true; }
	auto run = found->second;
	run->stop = true;
	run->execution->RequestStop();
	g_callback_cv.notify_all();
	if (run->starting || run->reaping || !run->execution->Retire()) { return false; }
	run->reaping = true;
	lock.unlock();
	// After execution retirement no callback can enter. Join the host wait loop
	// before dropping its retained run or allowing the owner's mappings to leave.
	const int result = run->thread ? LibKernel::PthreadJoin(run->thread, nullptr) : 0;
	lock.lock();
	run->reaping = false;
	if (result) { return false; }
	g_callback_runs.erase(program);
	return true;
}
} // namespace
