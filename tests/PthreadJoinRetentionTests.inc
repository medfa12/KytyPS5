// Included in LibKernel, with access to real pool records and native epilogues.
namespace PthreadJoinRetentionFixture {
struct Context {
	std::atomic<bool> register_now {false};
	std::atomic<bool> epilogue_entered {false};
	std::atomic<bool> leave_epilogue {false};
	std::atomic<bool> native_tls_exited {false};
	std::atomic<unsigned> destroyed {0};
	std::atomic<bool> deleter_unlocked {false};
	std::atomic<bool> deleter_before_reuse {false};
	std::atomic<bool> deleter_after_native_exit {false};
	std::weak_ptr<void> lifetime;
	int registered = -1;
	int self_join = 0;
	bool deduplicated = false;
	bool bounded = false;
};
struct NativeExit {
	Context* context = nullptr;
	~NativeExit() { if (context) { context->native_tls_exited.store(true, std::memory_order_release); } }
};
struct Pin {
	Context* context;
	Pthread target;
	~Pin() {
		const bool join_unlocked = target->join_mutex.try_lock();
		if (join_unlocked) { target->join_mutex.unlock(); }
		const bool stop_unlocked = target->stop_mutex.try_lock();
		if (stop_unlocked) { target->stop_mutex.unlock(); }
		// Contains takes the real pool lock. A collector retaining that lock
		// across join would deadlock here, even if record locks were released.
		const bool in_pool = g_pthread_context->GetPthreadPool()->Contains(target);
		context->deleter_unlocked = join_unlocked && stop_unlocked && in_pool;
		context->deleter_before_reuse = !target->free && target->join_reclaiming && PthreadJoin(target, nullptr) != 0;
		context->deleter_after_native_exit = context->native_tls_exited.load(std::memory_order_acquire);
		++context->destroyed;
	}
};
static std::atomic<Context*> active {nullptr};
static void Epilogue(Pthread thread) {
	auto* context = active.load(std::memory_order_acquire);
	if (!context || thread->arg != context) { return; }
	context->epilogue_entered.store(true, std::memory_order_release);
	while (!context->leave_epilogue.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}
static void* KYTY_SYSV_ABI Entry(void* raw) {
	auto& context = *static_cast<Context*>(raw);
	thread_local NativeExit native_exit;
	native_exit.context = &context;
	while (!context.register_now.load(std::memory_order_acquire)) { std::this_thread::yield(); }
	auto lifetime = std::make_shared<Pin>();
	lifetime->context = &context;
	lifetime->target = PthreadSelfOrNull();
	context.lifetime = lifetime;
	context.registered = PthreadRetainCurrentUntilJoin(lifetime);
	context.deduplicated = true;
	for (unsigned i = 0; i != 32; ++i) { context.deduplicated &= PthreadRetainCurrentUntilJoin(lifetime) == 0; }
	context.deduplicated &= PthreadSelfOrNull()->join_lifetime_count == 1;
	context.bounded = true;
	for (unsigned i = 1; i != 16; ++i) {
		context.bounded &= PthreadRetainCurrentUntilJoin(std::make_shared<unsigned>(i)) == 0;
	}
	context.bounded &= PthreadRetainCurrentUntilJoin(std::make_shared<unsigned>(16)) == 28;
	context.self_join = PthreadJoin(PthreadSelfOrNull(), nullptr);
	return reinterpret_cast<void*>(0x1234);
}
template<class Predicate> bool Await(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		std::this_thread::yield();
	}
	return true;
}
} // namespace PthreadJoinRetentionFixture

bool TestPthreadJoinRetention(std::string* failure) {
	using namespace PthreadJoinRetentionFixture;
	bool ok = true;
	const auto check = [&](bool condition, const char* message) {
		if (!condition && ok && failure) { *failure = message; }
		ok &= condition;
	};
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owner;
	auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(&owner);
	auto probe = std::make_shared<int>(7);
	check(PthreadRetainCurrentUntilJoin(probe) == 3 && PthreadRetainCurrentUntilJoin({}) == 22,
	      "main/synthetic self or null lifetime accepted");
	int currentless = 0;
	std::thread outsider([&] { currentless = PthreadRetainCurrentUntilJoin(probe); });
	outsider.join();
	check(currentless == 3, "current-less host thread accepted");
	g_join_retention_epilogue_for_test.store(Epilogue, std::memory_order_release);
	for (bool detached: {false, true}) {
		Context context;
		active.store(&context, std::memory_order_release);
		Pthread worker = nullptr;
		const int created = PthreadCreate(&worker, nullptr, Entry, &context, "join-retain");
		check(created == 0 && worker, "real guest pthread creation failed");
		if (created != 0 || !worker) { break; }
		int joined = -1;
		void* result = nullptr;
		std::thread joiner;
		if (!detached) {
			joiner = std::thread([&] { joined = PthreadJoin(worker, &result); });
			check(Await([&] {
				if (!worker->join_mutex.try_lock()) { return true; }
				worker->join_mutex.unlock();
				return false;
			}), "joiner never entered native wait");
		}
		context.register_now.store(true, std::memory_order_release);
		const bool parked = Await([&] { return context.epilogue_entered.load(std::memory_order_acquire); });
		check(parked, "actual native epilogue did not run");
		if (parked) {
			check(context.registered == 0 && context.deduplicated && context.bounded,
			      "registration while joiner waits, deduplication, or lifetime bound failed");
			check(context.self_join == KERNEL_ERROR_EDEADLK && context.destroyed == 0 && !context.lifetime.expired(),
			      "failed join/guest entry return released retained lifetime");
			check(worker->almost_done && !context.native_tls_exited, "fixture did not park after guest cleanup and before native TLS exit");
		}
		if (detached) {
			check(PthreadDetach(worker) == 0, "detach failed");
			joiner = std::thread([] { g_pthread_context->GetPthreadPool()->FreeDetachedThreads(); });
		}
		context.leave_epilogue.store(true, std::memory_order_release);
		joiner.join();
		check(Await([&] { return worker->free.load(); }), "native join/collector did not publish a reusable record");
		check(context.destroyed == 1 && context.lifetime.expired() && context.deleter_unlocked &&
		      context.deleter_before_reuse && context.deleter_after_native_exit,
		      "lifetime was not released exactly once after native exit, outside locks, before pool reuse");
		if (!detached) { check(joined == 0 && result == reinterpret_cast<void*>(0x1234), "successful native join result changed"); }
	}
	active.store(nullptr, std::memory_order_release);
	g_join_retention_epilogue_for_test.store(nullptr, std::memory_order_release);
	// Reusing a joined record and then failing native creation must leave no
	// retained lifetime and must not publish a fake guest handle.
	g_runtime_thread_fail_native_create = true;
	Pthread failed = nullptr;
	check(PthreadCreate(&failed, nullptr, Entry, nullptr, "join-retain-fail") == KERNEL_ERROR_EAGAIN && !failed,
	      "failed native creation published or retained a reused record");
	g_runtime_thread_fail_native_create = false;
	Loader::RuntimeLinker::SetCurrentProgram(previous);
	return ok;
}
