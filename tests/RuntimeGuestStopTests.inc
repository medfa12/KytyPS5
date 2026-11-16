// Included in LibKernel. Positive async-stop tests use actual native RX code;
// their release flag MUST remain zero through actual native join.
namespace RuntimeGuestStopFixture {
constexpr unsigned CaseCount = 10;
struct Flags {
	std::atomic<uint32_t> entered {0};
	std::atomic<uint32_t> release {0};
	uint64_t alternate_stack = 0;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == 4);
static_assert(offsetof(Flags, release) == 4 && offsetof(Flags, alternate_stack) == 8);
struct Context {
	Flags flags;
	Loader::RuntimeGuestExecution* execution = nullptr;
	uint64_t code = 0;
	uint64_t gateway = 0;
	std::atomic<unsigned> destroyed {0};
	unsigned continued = 0;
	Loader::RuntimeGuestResult inner;
	std::shared_ptr<Common::HostLockCriticalCounter> critical;
	std::atomic<bool> critical_owned {false};
	std::mutex mutex;
	std::condition_variable cv;
	bool host_release = false;
	PthreadKey cleanup_key = -1;
	bool cleanup_atexit = false;
	int cleanup_registration = KERNEL_ERROR_EINVAL;
};
static void ReleaseCritical(Context* context) {
	if (context->critical_owned.exchange(false, std::memory_order_acq_rel)) { context->critical->fetch_sub(1, std::memory_order_seq_cst); }
}
struct Pin {
	Context* context;
	~Pin() { ReleaseCritical(context); ++context->destroyed; }
};
static uint64_t KYTY_SYSV_ABI NestedSpin(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {context};
	// An explicit test marker, NOT a real host mutex released remotely. It
	// tests the same shared counter with paired ownership on all failure paths.
	context->critical = Common::GetHostLockCriticalCounter();
	context->critical->fetch_add(1, std::memory_order_seq_cst);
	context->critical_owned.store(true, std::memory_order_release);
	context->inner = context->execution->InvokeCallback(context->code, &context->flags, true);
	return 0;
}
static uint64_t KYTY_SYSV_ABI HostPark(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {context};
	std::unique_lock lock(context->mutex);
	context->flags.entered.store(1, std::memory_order_release);
	context->cv.wait(lock, [&] { return context->host_release; });
	return 0;
}
static void* KYTY_SYSV_ABI Coordinator(void* argument) { HostPark(argument); return nullptr; }
static void* KYTY_SYSV_ABI CleanupEntry(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {context};
	context->cleanup_registration = context->cleanup_atexit ?
	    LibcInternalExt::LibcInternalExtCxaThreadAtexit(
	        reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(context->code), &context->flags, nullptr) :
	    PthreadSetspecific(context->cleanup_key, &context->flags);
	return reinterpret_cast<void*>(0xc1ea);
}
static void* KYTY_SYSV_ABI GatewayEntry(void* argument) {
	auto* context = static_cast<Context*>(argument);
	reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(context->gateway)(context);
	++context->continued;
	return nullptr;
}
template <typename Predicate> static bool Await(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}
}

bool TestRuntimeGuestStop(std::string* failure) {
	using namespace RuntimeGuestStopFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[CaseCount];
	Context contexts[CaseCount];
	Pthread workers[CaseCount] {};
	void* mappings[CaseCount] {};
	void* alternate = nullptr;
	std::vector<std::unique_ptr<Loader::HostCallReturnThunk>> gateways;
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	struct Restore {
		Loader::Program* previous;
		Loader::Program* owners;
		Context* contexts;
		Pthread* workers;
		void** mappings;
		void** alternate;
		~Restore() {
			// Failure cleanup ONLY. Positive preemption checks join BEFORE this
			// executes and require their native release flags still be zero.
			for (unsigned i = 0; i != CaseCount; ++i) {
				contexts[i].flags.release.store(1, std::memory_order_release);
				ReleaseCritical(&contexts[i]);
				std::lock_guard lock(contexts[i].mutex);
				contexts[i].host_release = true;
				contexts[i].cv.notify_all();
			}
			for (unsigned i = 0; i != CaseCount; ++i) { if (workers[i] && !workers[i]->free) { PthreadJoin(workers[i], nullptr); } }
			for (unsigned i = 0; i != CaseCount; ++i) {
				if (contexts[i].cleanup_key >= 0) { PthreadKeyDelete(contexts[i].cleanup_key); }
				if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); }
				if (mappings[i]) { VirtualFree(mappings[i], 0, MEM_RELEASE); }
			}
			if (*alternate) { VirtualFree(*alternate, 0, MEM_RELEASE); }
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {previous, owners, contexts, workers, mappings, &alternate};
	// Deliberately not a SysV CALL-entry-aligned interrupted RSP: sub rsp,8;
	// entered=1; while(!release) pause; add rsp,8; return 0x1234. No stop poll.
	constexpr uint8_t spin[] = {0x48,0x83,0xec,0x08, 0xc7,0x07,1,0,0,0,
	    0xf3,0x90, 0x83,0x7f,0x04,0x00,0x74,0xf8, 0x48,0x83,0xc4,0x08,
	    0xb8,0x34,0x12,0,0,0xc3};
	// Unsupported native-only stack change, no HLE. Save original RSP in R11,
	// use alternate stack while spinning, restore only on TEST release.
	constexpr uint8_t other_stack[] = {0x49,0x89,0xe3, 0x48,0x8b,0x67,0x08,
	    0xc7,0x07,1,0,0,0, 0xf3,0x90,0x83,0x7f,0x04,0x00,0x74,0xf8,
	    0x4c,0x89,0xdc,0xb8,0x34,0x12,0,0,0xc3};
	alternate = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!check(alternate != nullptr, "allocate unsupported alternate guest stack fixture")) { return false; }
	contexts[6].flags.alternate_stack = reinterpret_cast<uint64_t>(alternate) + 4096 - 16;
	const auto publish = [&](unsigned i) {
		auto proof = std::make_shared<Loader::GuestStopText>();
		proof->owner = owners[i].application_execution->Owner();
		proof->ranges.push_back({contexts[i].code, contexts[i].code + 4096});
		return owners[i].application_execution->PublishStopText(proof);
	};
	for (unsigned i = 0; i != CaseCount; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 989000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owners[i]), "async stop requires supported boundary/CET configuration")) { return false; }
		contexts[i].execution = owners[i].application_execution.get();
		mappings[i] = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!check(mappings[i] != nullptr, "allocate owner-private native RX spin mapping")) { return false; }
		std::memcpy(mappings[i], i == 6 ? other_stack : spin, i == 6 ? sizeof(other_stack) : sizeof(spin));
		DWORD old = 0;
		if (!check(VirtualProtect(mappings[i], 4096, PAGE_EXECUTE_READ, &old) &&
		               FlushInstructionCache(GetCurrentProcess(), mappings[i], 4096), "seal actual owner-private guest executable mapping")) { return false; }
		contexts[i].code = reinterpret_cast<uint64_t>(mappings[i]);
		if (i != 3 && !check(publish(i), "publish immutable exact owner native-text snapshot")) { return false; }
	}
	Loader::RuntimeLinker::SetCurrentProgram(&owners[8]);
	if (!check(PthreadKeyCreate(&contexts[8].cleanup_key,
	    reinterpret_cast<pthread_key_destructor_func_t>(contexts[8].code)) == OK,
	    "register owner-private native key destructor")) { return false; }
	contexts[9].cleanup_atexit = true;
	for (unsigned i: {2u, 4u}) {
		auto gateway = Loader::HostCallReturnThunk::Create(contexts[i].execution->Boundary(),
		    reinterpret_cast<uint64_t>(i == 2 ? NestedSpin : HostPark), contexts[i].execution,
		    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall);
		if (!check(bool(gateway), "create returning host gateway for nested/host rejection fixture")) { return false; }
		contexts[i].gateway = gateway->GetAddress();
		gateways.push_back(std::move(gateway));
	}
	const auto create = [&](unsigned i) {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
		const auto entry = i >= 8 ? CleanupEntry : (i == 2 || i == 4 ? GatewayEntry : reinterpret_cast<pthread_entry_func_t>(mappings[i]));
		const auto argument = i == 2 || i == 4 || i == 5 || i >= 8 ? static_cast<void*>(&contexts[i]) : static_cast<void*>(&contexts[i].flags);
		const auto result = i == 5 ? PthreadCreateRuntimeCoordinator(&workers[i], nullptr, Coordinator, argument, "stop-coordinator") :
		    PthreadCreate(&workers[i], nullptr, entry, argument, "async-stop-native-fixture");
		return check(result == OK && Await([&] { return contexts[i].flags.entered.load(std::memory_order_acquire) != 0; }),
		             "actual native thread entered owner-private guest/host fixture");
	};
	for (unsigned i = 0; i != CaseCount; ++i) { if (!create(i)) { return false; } }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(TestRuntimeGuestStopServices(workers[0]), "shared signal, queue, user-context and fiber services excluded before ownership/global changes")) { return false; }
	if (!check(PthreadRequestOwnerStop({}).status == PthreadOwnerStopStatus::InvalidOwner, "empty stop owner refused")) { return false; }
	const auto stop = [&](unsigned i) { return PthreadRequestOwnerStop(owners[i].application_execution); };
	const auto positive = [&](unsigned i, bool nested) {
		auto result = stop(i);
		if (!check(result.status == PthreadOwnerStopStatus::Redirected && result.redirected == 1,
		           "positive stop must schedule the dedicated arbitrary-RIP landing")) { return false; }
		if (!check(Await([&] { return workers[i]->almost_done.load(); }), "pure native loop must actually stop without fixture release")) { return false; }
		void* value = nullptr;
		if (!check(PthreadJoin(workers[i], &value) == OK && value == reinterpret_cast<void*>(~uintptr_t {0}) &&
		               contexts[i].flags.release.load() == 0 && !contexts[i].execution->Failure() && contexts[i].execution->Outstanding() == 0,
		           "actual preemption joins with no test release, owner failure, leaked work or guest success")) { return false; }
		return check(nested ? (contexts[i].destroyed == 1 && contexts[i].continued == 0 && contexts[i].inner.stopped &&
		                         contexts[i].inner.call.tag == Loader::GuestCallOutcomeTag::Stopped) :
		                        (workers[i]->guest_result.call.tag == Loader::GuestCallOutcomeTag::Stopped),
		             "only innermost guest frames discarded; nested host RAII and outer return gateway survive");
	};
	// VSH's host coordinator is already joining the main worker when Close
	// arrives. A blocking join must not hold the stop descriptor hostage.
	int host_join_result = KERNEL_ERROR_EINVAL;
	void* host_join_value = nullptr;
	std::thread host_join([&] { host_join_result = PthreadJoin(workers[0], &host_join_value); });
	struct JoinCleanup {
		std::thread& thread;
		Flags& flags;
		~JoinCleanup() { if (thread.joinable()) { flags.release.store(1); thread.join(); } }
	} join_cleanup {host_join, contexts[0].flags};
	if (!check(Await([&] {
		std::unique_lock lock(workers[0]->join_mutex, std::try_to_lock);
		return !lock;
	}), "host join must be active before stopping its native target")) { return false; }
	const auto joined_stop = stop(0);
	if (!check(joined_stop.redirected == 1 && Await([&] { return workers[0]->almost_done.load() || workers[0]->free.load(); }),
	           "actual native stop must progress while the host is blocked in join")) { return false; }
	host_join.join();
	if (!check(host_join_result == OK && host_join_value == reinterpret_cast<void*>(~uintptr_t {0}) &&
	               contexts[0].flags.release.load() == 0 && !contexts[0].execution->Failure() &&
	               contexts[0].execution->Outstanding() == 0 &&
	               workers[0]->guest_result.call.tag == Loader::GuestCallOutcomeTag::Stopped,
	           "host join must reap a genuinely preempted native loop without fixture release")) { return false; }
	if (!check(contexts[1].execution->State() == Loader::GuestExecutionState::Running && !workers[1]->almost_done &&
	               contexts[1].flags.release == 0, "stopping one private mapping leaves a peer owner alive")) { return false; }
	if (!check(stop(2).status == PthreadOwnerStopStatus::Pending && contexts[2].destroyed == 0 &&
	               !workers[2]->almost_done && PthreadIsHostLockCritical(workers[2]), "host-critical nested callback must remain pending")) { return false; }
	ReleaseCritical(&contexts[2]);
	if (!positive(2, true)) { return false; }
	if (!check(stop(3).status == PthreadOwnerStopStatus::Unsupported && !workers[3]->almost_done &&
	               contexts[3].execution->Outstanding() != 0, "missing private executable proof never authorizes a redirect")) { return false; }
	if (!check(publish(3), "publish missing text proof after an unsupported attempt") || !positive(3, false)) { return false; }
	for (unsigned i: {4u, 5u, 6u}) {
		if (!check(stop(i).status == PthreadOwnerStopStatus::Pending && !workers[i]->almost_done && contexts[i].destroyed == 0,
		           "HLE host frames, boundary-free coordinator and unowned native stack remain ineligible/pending")) { return false; }
	}
	// Inject an incompatible reservation directly (NOT an accepted signal API).
	// It must remain intact/pending, never be silently consumed to permit stop.
	workers[7]->pending_signal_mask.store(1ull << 30, std::memory_order_release);
	if (!check(stop(7).status == PthreadOwnerStopStatus::Pending && workers[7]->pending_signal_mask.load() == (1ull << 30) &&
	               !workers[7]->almost_done, "incompatible pending work forbids stop and is not dropped")) { return false; }
	workers[7]->pending_signal_mask.store(0, std::memory_order_release);
	if (!positive(7, false)) { return false; }
	// Stop is requested only AFTER actual callback entry. A returned worker
	// cannot hide the cleanup loop by skipping destructors on an earlier stop.
	for (unsigned i: {8u, 9u}) {
		if (!check(contexts[i].cleanup_registration == OK && contexts[i].destroyed == 1 &&
		               contexts[i].execution->State() == Loader::GuestExecutionState::Running && !workers[i]->almost_done,
		           i == 8 ? "actual private RX key destructor entered after worker return" :
		                    "actual private RX thread-atexit destructor entered after worker return")) { return false; }
		const auto result = stop(i);
		if (!check(result.redirected == 1 && Await([&] { return workers[i]->almost_done.load(); }),
		           i == 8 ? "key destructor native loop preempted on original guest stack" :
		                    "thread-atexit native loop preempted on original guest stack")) { return false; }
		void* value = nullptr;
		if (!check(PthreadJoin(workers[i], &value) == OK && value == reinterpret_cast<void*>(~uintptr_t {0}) &&
		               contexts[i].flags.release.load() == 0 && workers[i]->guest_result.stopped &&
		               !contexts[i].execution->Failure() && contexts[i].execution->Outstanding() == 0 &&
		               owners[i].application_threads.load() == 0 && contexts[i].destroyed == 1,
		           i == 8 ? "key stop actually joins without release, leaked work or skipped host cleanup" :
		                    "thread-atexit stop actually joins without release, leaked work or skipped host cleanup")) { return false; }
	}
	// Only rejected/host/coordinator/peer fixture cases use explicit release.
	for (unsigned i: {1u, 4u, 5u, 6u}) {
		contexts[i].flags.release.store(1, std::memory_order_release);
		{ std::lock_guard lock(contexts[i].mutex); contexts[i].host_release = true; contexts[i].cv.notify_all(); }
		if (!check(PthreadJoin(workers[i], nullptr) == OK, "actually join explicitly released unsupported/host/peer fixture")) { return false; }
	}
	if (!check(contexts[4].destroyed == 1 && contexts[4].continued == 0 && contexts[5].destroyed == 1,
	           "host and coordinator release returns normally through original RAII, never async-aborted")) { return false; }
	// Reuse a shared pool record under a new lifetime; old-owner close cannot
	// reopen a recyclable numeric native-thread ID and redirect this incarnation.
	contexts[1].flags.entered.store(0); contexts[1].flags.release.store(0);
	if (!create(1)) { return false; }
	const auto old_owner = stop(0);
	if (!check(old_owner.redirected == 0 && !workers[1]->almost_done && contexts[1].execution->State() == Loader::GuestExecutionState::Running,
	           "old owner request cannot affect a reused shared record/native handle")) { return false; }
	if (!positive(1, false)) { return false; }
	for (auto& owner: owners) {
		if (!check(!owner.application_execution->Failure() && owner.application_execution->Outstanding() == 0 &&
		               owner.application_threads.load() == 0, "all native stop paths drain truthfully without owner failure")) { return false; }
	}
	return check(Common::GetHostLockCriticalCounter()->load() == baseline && Loader::GuestCallBoundary::CurrentDepth() == 0 &&
	                 Loader::HostCallReturnThunk::CurrentDepth() == 0, "async stop leaves boundary stacks and host-critical accounting balanced");
#else
	return check(false, "Windows x86-64 async stop fixture unavailable (not exercised)");
#endif
}
