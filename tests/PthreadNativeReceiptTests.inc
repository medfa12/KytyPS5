// Included in LibKernel. Uses actual pool pthreads, original OS handles, native
// TLS teardown and joins. Fault seams refuse operations; none fake completion.
namespace PthreadNativeReceiptFixture {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
struct Context {
	std::atomic<bool> capture_now {false}, epilogue {false}, leave_epilogue {false};
	std::atomic<bool> tls_entered {false}, leave_tls {false}, tls_finished {false};
	std::atomic<unsigned> released {0};
	std::atomic<bool> release_after_join {false}, release_unlocked {false};
	PthreadNativeReceipt captured;
	int capture_error = -1, self_join_error = -1, retain_error = -1;
};
static std::atomic<Context*> active {nullptr};
static void Release(Context& context) {
	context.capture_now = true;
	context.leave_epilogue = true;
	context.leave_tls = true;
}
template<class F> static bool Await(F predicate) {
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= limit) { return false; }
		std::this_thread::yield();
	}
	return true;
}
struct NativeTlsExit {
	Context* context = nullptr;
	~NativeTlsExit() {
		if (!context) { return; }
		context->tls_entered.store(true, std::memory_order_release);
		while (!context->leave_tls.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		context->tls_finished.store(true, std::memory_order_release);
	}
};
struct Lifetime {
	Context* context;
	Pthread record;
	~Lifetime() {
		PthreadNativeSnapshot proof;
		context->release_after_join = PthreadInspectNativeReceipt(context->captured, proof) == 0 &&
		    proof.phase == PthreadNativePhase::Joined && context->tls_finished;
		const bool join_unlocked = record->join_mutex.try_lock();
		if (join_unlocked) { record->join_mutex.unlock(); }
		const bool stop_unlocked = record->stop_mutex.try_lock();
		if (stop_unlocked) { record->stop_mutex.unlock(); }
		const bool in_pool = g_pthread_context->GetPthreadPool()->Contains(record);
		context->release_unlocked = join_unlocked && stop_unlocked && in_pool && !record->free &&
		    PthreadJoinNativeReceipt(context->captured, record, nullptr) == 3;
		++context->released;
	}
};
static void Epilogue(Pthread record) {
	auto* context = active.load(std::memory_order_acquire);
	if (!context || record->arg != context) { return; }
	context->epilogue.store(true, std::memory_order_release);
	while (!context->leave_epilogue.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}
static void* KYTY_SYSV_ABI Entry(void* raw) {
	auto& context = *static_cast<Context*>(raw);
	thread_local NativeTlsExit native_tls;
	native_tls.context = &context;
	while (!context.capture_now.load(std::memory_order_acquire)) { std::this_thread::yield(); }
	context.capture_error = PthreadCaptureCurrentNativeReceipt(context.captured);
	if (!context.capture_error) {
		context.self_join_error = PthreadJoinNativeReceipt(context.captured, PthreadSelfOrNull(), nullptr);
		auto lifetime = std::make_shared<Lifetime>();
		lifetime->context = &context;
		lifetime->record = PthreadSelfOrNull();
		context.retain_error = PthreadRetainCurrentUntilJoin(std::move(lifetime));
	}
	return reinterpret_cast<void*>(0x1234);
}
struct JoinIdentityContext {
	Loader::Program* spoof_owner = nullptr;
	Pthread targets[2] {};
	PthreadNativeReceipt captured;
	int capture_error = -1, spoof_capture_error = -1, spoof_pid = -1;
	int joins[2] {-1, -1};
};
static void* KYTY_SYSV_ABI JoinIdentityTarget(void*) { return reinterpret_cast<void*>(0x5678); }
static void* KYTY_SYSV_ABI JoinIdentityEntry(void* raw) {
	auto& context = *static_cast<JoinIdentityContext*>(raw);
	context.capture_error = PthreadCaptureCurrentNativeReceipt(context.captured);
	auto* actual_owner = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(context.spoof_owner);
	(void)Loader::TryGetCurrentRetainedGuestProcessId(&context.spoof_pid);
	PthreadNativeReceipt rejected;
	context.spoof_capture_error = PthreadCaptureCurrentNativeReceipt(rejected);
	context.joins[0] = PthreadJoin(context.targets[0], nullptr);
	Loader::RuntimeLinker::SetCurrentProgram(actual_owner);
	context.joins[1] = PthreadJoin(context.targets[1], nullptr);
	return nullptr;
}
static bool CheckJoinIdentity(int owner_pid) {
	// Both scopes have real, distinct retained PIDs. The joiner is an actual
	// owner-created pool pthread; only its mutable current Program is spoofed.
	Loader::Program other;
	other.unique_id = 791100;
	other.file_name = "pthread-native-receipt-other.elf";
	Libs::LncUtil::Lnc::RegisterProgramLaunch(&other, {}, false, 791100);
	int32_t app_id = -1;
	if (!Libs::LncUtil::Lnc::TryGetProgramAppId(&other, &app_id)) { return false; }
	const int other_pid = Loader::RegisterCohostProcessIdentity(&other, app_id);
	if (other_pid <= 0) { return false; }
	JoinIdentityContext context;
	context.spoof_owner = &other;
	Pthread worker = nullptr;
	PthreadNativeReceipt targets[2], worker_receipt;
	struct Cleanup {
		JoinIdentityContext& context;
		Pthread& worker;
		Loader::Program& other;
		~Cleanup() {
			if (worker && !worker->free) { (void)PthreadJoin(worker, nullptr); }
			for (auto target: context.targets) {
				if (target && !target->free) { (void)PthreadJoin(target, nullptr); }
			}
			(void)Loader::DeactivateRuntimeProcessIdentity(&other);
		}
	} cleanup {context, worker, other};
	if (other_pid == owner_pid) { return false; }
	for (unsigned i = 0; i != 2; ++i) {
		if (PthreadCreateWithNativeReceipt(&context.targets[i], nullptr, JoinIdentityTarget, nullptr,
		                                   "receipt-identity-target", targets[i]) != 0) { return false; }
	}
	if (PthreadCreateWithNativeReceipt(&worker, nullptr, JoinIdentityEntry, &context,
	                                   "receipt-owner-joiner", worker_receipt) != 0) { return false; }
	if (PthreadJoin(worker, nullptr) != 0) { return false; }
	PthreadNativeSnapshot spoofed, valid, actual;
	return context.capture_error == 0 && context.captured == worker_receipt &&
	    context.spoof_capture_error == 3 && context.spoof_pid == other_pid &&
	    context.joins[0] == 0 && context.joins[1] == 0 &&
	    PthreadInspectNativeReceipt(worker_receipt, actual) == 0 && actual.pid == owner_pid && actual.tid > 0 &&
	    PthreadInspectNativeReceipt(targets[0], spoofed) == 0 &&
	    spoofed.phase == PthreadNativePhase::Joined && spoofed.join_source == PthreadNativeJoinSource::Explicit &&
	    spoofed.joiner_pid == -1 && spoofed.joiner_tid == -1 && spoofed.joined_value == 0x5678 &&
	    PthreadInspectNativeReceipt(targets[1], valid) == 0 &&
	    valid.phase == PthreadNativePhase::Joined && valid.join_source == PthreadNativeJoinSource::Explicit &&
	    valid.joiner_pid == actual.pid && valid.joiner_tid == actual.tid && valid.joined_value == 0x5678;
}
#endif
} // namespace PthreadNativeReceiptFixture

bool TestPthreadNativeReceipt(std::string* failure) {
	using namespace PthreadNativeReceiptFixture;
	const auto check = [&](bool ok, const char* text) {
		if (!ok && failure) { *failure = text; }
		return ok;
	};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto owner = std::make_unique<Loader::Program>();
	owner->unique_id = 791099;
	owner->file_name = "pthread-native-receipt-fixture.elf";
	Libs::LncUtil::Lnc::RegisterProgramLaunch(owner.get(), {}, false, 791099);
	int32_t app_id = -1;
	if (!check(Libs::LncUtil::Lnc::TryGetProgramAppId(owner.get(), &app_id), "fixture LNC identity")) { return false; }
	const int pid = Loader::RegisterCohostProcessIdentity(owner.get(), app_id);
	if (!check(pid > 0, "fixture retained cohost PID")) { return false; }
	auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(owner.get());
	Context first, second, detached;
	Pthread workers[3] {};
	PthreadNativeReceipt original, replacement, collector;
	std::thread joiner;
	struct Cleanup {
		Loader::Program* previous;
		Loader::Program* owner;
		Context& a; Context& b; Context& c;
		Pthread* workers;
		std::thread& joiner;
		void (*previous_epilogue)(Pthread);
		~Cleanup() {
			Release(a); Release(b); Release(c);
			if (joiner.joinable()) { joiner.join(); }
			for (unsigned i = 0; i != 3; ++i) {
				if (workers[i] && !workers[i]->free) { (void)PthreadJoin(workers[i], nullptr); }
			}
			active = nullptr;
			g_join_retention_epilogue_for_test = previous_epilogue;
			g_runtime_thread_fail_native_create = false;
			g_native_receipt_duplicate_fail_for_test = false;
			g_native_receipt_exhaust_for_test = false;
			Loader::RuntimeLinker::SetCurrentProgram(previous);
			(void)Loader::DeactivateRuntimeProcessIdentity(owner);
		}
	} cleanup {previous, owner.get(), first, second, detached, workers, joiner,
	           g_join_retention_epilogue_for_test.load()};
	PthreadNativeReceipt invalid;
	PthreadNativeSnapshot snapshot;
	if (!check(PthreadCaptureCurrentNativeReceipt(invalid) == 3 && !invalid &&
	           PthreadInspectNativeReceipt(invalid, snapshot) == 22, "main/null receipt rejection")) { return false; }
	int outsider_result = 0;
	std::thread outsider([&] { outsider_result = PthreadCaptureCurrentNativeReceipt(invalid); });
	outsider.join();
	if (!check(outsider_result == 3 && !invalid, "current-less native thread rejection")) { return false; }
	g_join_retention_epilogue_for_test = Epilogue;
	active = &first;
	if (!check(PthreadCreateWithNativeReceipt(&workers[0], nullptr, Entry, &first, "receipt-original", original) == 0 && original,
	           "creation-time original native receipt")) { return false; }
	if (!check(PthreadInspectNativeReceipt(original, snapshot) == 0 && snapshot.phase == PthreadNativePhase::Pending &&
	           snapshot.generation && snapshot.pid == pid && snapshot.tid > 0 && snapshot.native_tid &&
	           snapshot.owner_cookie == reinterpret_cast<uint64_t>(owner.get()) &&
	           snapshot.entry == reinterpret_cast<uint64_t>(&Entry) && snapshot.argument == reinterpret_cast<uint64_t>(&first),
	           "immutable identity available before Run/capture")) { return false; }
	const auto generation = snapshot.generation;
	void* value = reinterpret_cast<void*>(0xfeed);
	if (!check(PthreadJoinNativeReceipt(original, workers[0], &value) == 16 && value == reinterpret_cast<void*>(0xfeed),
	           "pending OS handle cannot fabricate native join")) { return false; }
	int joined = -1;
	joiner = std::thread([&] { joined = PthreadJoin(workers[0], &value); });
	if (!check(Await([&] {
		if (!workers[0]->join_mutex.try_lock()) { return true; }
		workers[0]->join_mutex.unlock(); return false;
	}), "joiner entered actual blocking native join")) { return false; }
	first.capture_now = true;
	if (!check(Await([&] { return first.epilogue.load(); }) && first.capture_error == 0 && first.captured == original &&
	           first.self_join_error == 11 && first.retain_error == 0, "current capture while joiner owns join_mutex")) { return false; }
	if (!check(PthreadInspectNativeReceipt(original, snapshot) == 0 && snapshot.phase == PthreadNativePhase::Pending &&
	           first.released == 0, "guest entry/cleanup return is not native exit or join")) { return false; }
	first.leave_epilogue = true;
	if (!check(Await([&] { return first.tls_entered.load(); }) && PthreadInspectNativeReceipt(original, snapshot) == 0 &&
	           snapshot.phase == PthreadNativePhase::Pending && first.released == 0, "native TLS epilogue still owns lifetime")) { return false; }
	first.leave_tls = true;
	joiner.join();
	if (!check(joined == 0 && value == reinterpret_cast<void*>(0x1234) && first.released == 1 &&
	           first.release_after_join && first.release_unlocked && PthreadInspectNativeReceipt(original, snapshot) == 0 &&
	           snapshot.phase == PthreadNativePhase::Joined && snapshot.join_source == PthreadNativeJoinSource::Explicit &&
	           snapshot.joiner_pid == -1 && snapshot.joiner_tid == -1, "real join proof precedes unlocked provider release")) { return false; }

	active = &second;
	if (!check(PthreadCreateWithNativeReceipt(&workers[1], nullptr, Entry, &second, "receipt-reused", replacement) == 0 &&
	           workers[1] == workers[0] && replacement != original, "actual pool reuse with independent receipt")) { return false; }
	if (!check(PthreadInspectNativeReceipt(replacement, snapshot) == 0 && snapshot.generation > generation &&
	           PthreadJoinNativeReceipt(original, workers[1], nullptr) == 3 &&
	           PthreadJoinNativeReceipt(replacement, reinterpret_cast<Pthread>(uintptr_t {0x1234}), nullptr) == 3,
	           "stale/mismatched target cannot substitute recycled record")) { return false; }
	Release(second);
	if (!check(Await([&] { return PthreadInspectNativeReceipt(replacement, snapshot) == 0 &&
	    snapshot.phase == PthreadNativePhase::NativeExited; }) && snapshot.join_source == PthreadNativeJoinSource::None && second.released == 0,
	    "OS exit independently observable before sole native join")) { return false; }
	workers[1]->test_fail_native_join = true;
	if (!check(PthreadJoinNativeReceipt(replacement, workers[1], nullptr) == 16 && second.released == 0 &&
	           PthreadInspectNativeReceipt(replacement, snapshot) == 0 && snapshot.phase == PthreadNativePhase::NativeExited,
	           "refused native join retains proof and provider pins")) { return false; }
	if (!check(PthreadJoinNativeReceipt(replacement, workers[1], &value) == 0 && second.released == 1 &&
	           second.release_after_join && second.release_unlocked && PthreadInspectNativeReceipt(replacement, snapshot) == 0 &&
	           snapshot.phase == PthreadNativePhase::Joined && snapshot.join_source == PthreadNativeJoinSource::Explicit &&
	           snapshot.joiner_pid == -1 && snapshot.joiner_tid == -1 && PthreadJoinNativeReceipt(replacement, workers[1], nullptr) == 3,
	           "selected-program launcher has no owned guest join identity, never repeats consumed join")) { return false; }

	active = &detached;
	PthreadAttr attr = nullptr;
	PthreadAttrInit(&attr);
	PthreadAttrSetdetachstate(&attr, 1);
	const int created = PthreadCreateWithNativeReceipt(&workers[2], &attr, Entry, &detached, "receipt-detached", collector);
	PthreadAttrDestroy(&attr);
	if (!check(created == 0 && collector, "detached native creation")) { return false; }
	Release(detached);
	if (!check(Await([&] { return detached.epilogue.load(); }), "detached entry ran")) { return false; }
	std::thread reaper([] { g_pthread_context->GetPthreadPool()->FreeDetachedThreads(); });
	reaper.join();
	if (!check(Await([&] { return PthreadInspectNativeReceipt(collector, snapshot) == 0 && snapshot.phase == PthreadNativePhase::Joined; }) &&
	           snapshot.detached_at_creation && snapshot.join_source == PthreadNativeJoinSource::DetachedCollector &&
	           snapshot.joiner_pid == -1 && snapshot.joiner_tid == -1 &&
	           Await([&] { return workers[2]->free.load(); }) && detached.released == 1 && detached.release_unlocked,
	           "collector join has its own origin and no manufactured guest identity")) { return false; }

	active = nullptr;
	if (!check(CheckJoinIdentity(pid), "actual owner joiner: scoped PID spoof yields unavailable identity; restored owner yields immutable PID/TID")) {
		return false;
	}
	Pthread failed = nullptr;
	g_runtime_thread_fail_native_create = true;
	if (!check(PthreadCreateWithNativeReceipt(&failed, nullptr, Entry, nullptr, "receipt-create-fail", invalid) != 0 && !failed && !invalid,
	           "failed native creation publishes no receipt/handle")) { return false; }
	g_native_receipt_duplicate_fail_for_test = true;
	if (!check(PthreadCreateWithNativeReceipt(&failed, nullptr, Entry, nullptr, "receipt-duplicate-fail", invalid) != 0 && !failed && !invalid,
	           "duplicate failure refuses startup and publishes no fake created receipt")) { return false; }
	g_native_receipt_exhaust_for_test = true;
	if (!check(PthreadCreateWithNativeReceipt(&failed, nullptr, Entry, nullptr, "receipt-exhaust", invalid) == 84 && !failed && !invalid,
	           "generation exhaustion fails before native creation")) { return false; }
	if (!check(PthreadInspectNativeReceipt(original, snapshot) == 0 && snapshot.generation == generation &&
	           snapshot.phase == PthreadNativePhase::Joined, "old receipt proof survives failures and repeated reuse")) { return false; }
	const auto owner_cookie = reinterpret_cast<uint64_t>(owner.get());
	Loader::RuntimeLinker::SetCurrentProgram(previous);
	if (!check(Loader::DeactivateRuntimeProcessIdentity(owner.get()), "fixture owner retirement")) { return false; }
	cleanup.owner = nullptr;
	owner.reset();
	if (!check(PthreadInspectNativeReceipt(original, snapshot) == 0 && snapshot.owner_cookie == owner_cookie &&
	           snapshot.phase == PthreadNativePhase::Joined, "receipt inspection dereferenced retired owner instead of metadata")) { return false; }
	return true;
#else
	PthreadNativeReceipt receipt;
	return check(PthreadCaptureCurrentNativeReceipt(receipt) == 45 && !receipt, "unsupported OS fails closed");
#endif
}
