// Real-image component fixture. Include at file scope beside the dispatcher
// fixture and dispatch TestNpNativeSynchronization in an isolated process.
// Parent TU must include loader/npSynchronizationExports.h BEFORE its anonymous
// namespace (like the other loader headers), not from inside this .inc.

namespace NpNativeSynchronizationTest {
constexpr int Timeout = static_cast<int>(0x8055800bU);
constexpr int EmptyEvent = static_cast<int>(0x8055800cU);
constexpr int EmptySema = static_cast<int>(0x8055800dU);
constexpr int AlreadyInitialized = static_cast<int>(0x80559e03U);

struct Native {
	uint64_t base;
	template<typename R, typename... A> R Call(uint32_t rva, A... args) const {
		using Function = R (KYTY_SYSV_ABI *)(A...);
		return reinterpret_cast<Function>(base + rva)(args...);
	}
};
// Disassembly: both vptrs at +0, opaque handle at +8, state at +0x10.
// Mutex uses only the low byte of state. Neither class is host-constructed.
struct Object { uint64_t vptr = 0; void* handle = nullptr; uint32_t state = 0; uint32_t padding = 0; };
static_assert(sizeof(Object) == 0x18);
enum class Operation { Event, Mutex, Cond, Sema };
struct Worker {
	Native native;
	Operation operation;
	Object* object = nullptr;
	void* handle = nullptr;
	void** mutex = nullptr;
	std::atomic_bool entered {false}, done {false}, release {false};
	int result = -1;
	int unlock_result = -1;
	int try_result = -1;
	uint64_t bits = 0;
};
void* KYTY_SYSV_ABI Entry(void* argument) {
	auto& w = *static_cast<Worker*>(argument);
	if (w.operation == Operation::Cond) {
		w.result = w.native.Call<int>(0x69f0, w.mutex);
		if (w.result == 0) {
			w.entered.store(true);
			// +66c0 takes scalar microseconds (not a timeout pointer).
			do { w.result = w.native.Call<int>(0x66c0, &w.handle, w.mutex, uint32_t(2000000)); }
			while (w.result == 0 && !w.release.load());
			w.unlock_result = w.native.Call<int>(0x6a30, w.mutex);
		}
	} else {
		if (w.operation == Operation::Mutex) {
			w.try_result = w.native.Call<int>(0x6a10, &w.object->handle);
			if (w.try_result == 0) { w.native.Call<void>(0x1e6a0, w.object); }
		}
		w.entered.store(true);
		if (w.operation == Operation::Event) {
			w.result = w.native.Call<int>(0x1db80, w.object, uint64_t(1), uint32_t(0x21), &w.bits, uint32_t(2000000));
		} else if (w.operation == Operation::Sema) {
			// +6be0 takes a pointer and consumes exactly one permit.
			uint32_t timeout = 2000000;
			w.result = w.native.Call<int>(0x6be0, w.handle, &timeout);
		} else {
			w.native.Call<void>(0x1e5f0, w.object);
			w.result = 0;
			w.native.Call<void>(0x1e6a0, w.object);
		}
	}
	w.done.store(true);
	return nullptr;
}
bool Await(const std::atomic_bool& flag) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!flag.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return flag.load();
}
template<typename Predicate> bool Enrolled(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return predicate();
}
template<typename Operation> bool TimesOut(Operation operation) {
	const auto begin = std::chrono::steady_clock::now();
	const int result = operation(uint32_t(20000));
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	// Reject a success-only/immediate-error stub, with margin for clock rounding
	// and host scheduling. The enclosing isolated-process runner bounds hangs.
	return result == Timeout && elapsed >= std::chrono::milliseconds(10);
}
bool Pending(Worker& w) {
	if (!Await(w.entered)) { return false; }
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
	while (!w.done.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return !w.done.load();
}
bool Start(Worker& w, Libs::LibKernel::Pthread& thread) {
	return Libs::LibKernel::PthreadCreate(&thread, nullptr, Entry, &w, "NpSyncProbe") == 0 && thread;
}
void Join(Libs::LibKernel::Pthread thread) {
	// No assertion can unwind a live worker or its native backing. A broken join
	// is fatal to this isolated process, rather than allowing use-after-unmap.
	if (thread && Libs::LibKernel::PthreadJoin(thread, nullptr) != 0) { std::abort(); }
}

bool Events(Native n) {
	Object owner, alias;
	n.Call<void>(0x1d910, &owner);
	n.Call<void>(0x1d910, &alias);
	bool ok = !n.Call<bool>(0x1d9e0, &owner) && owner.vptr == n.base + 0x48ba0;
	const int created = n.Call<int>(0x1d9f0, &owner, "NpNativeEvent", uint32_t(0x121));
	ok &= created == 0;
	if (created == 0) {
		ok &= n.Call<bool>(0x1d9e0, &owner) && owner.state == 1;
		ok &= n.Call<int>(0x1d9f0, &owner, "NpNativeEvent", uint32_t(0x121)) == AlreadyInitialized;
		uint64_t bits = 0;
		ok &= n.Call<int>(0x1db00, &owner, uint64_t(1), uint32_t(0x21), &bits) == EmptyEvent;
		ok &= TimesOut([&](uint32_t timeout) { return n.Call<int>(0x1db80, &owner, uint64_t(1), uint32_t(0x21), &bits, timeout); });
		const int opened = n.Call<int>(0x1da30, &alias, "NpNativeEvent");
		ok &= opened == 0;
		if (opened == 0) {
			Worker w {n, Operation::Event, &owner};
			Libs::LibKernel::Pthread thread = nullptr;
			const bool started = Start(w, thread);
			ok &= started;
			if (started) {
				ok &= Enrolled([&] { return Libs::LibKernel::EventFlag::EventFlagWaitersForTest(
				    static_cast<Libs::LibKernel::EventFlag::KernelEventFlag>(owner.handle)) == 1; });
			}
			ok &= n.Call<int>(0x1daa0, &alias, uint64_t(1)) == 0;
			Join(thread);
			ok &= started && w.result == 0 && (w.bits & 1) != 0;
			// Wait's CLEAR_PAT consumes the signalled bit across both handles.
			ok &= n.Call<int>(0x1db00, &alias, uint64_t(1), uint32_t(0x21), &bits) == EmptyEvent;
			n.Call<void>(0x1da60, &alias);
			ok &= !n.Call<bool>(0x1d9e0, &alias);
		}
		n.Call<void>(0x1da60, &owner);
		ok &= !n.Call<bool>(0x1d9e0, &owner);
		// Reuse the name to prove creator close actually deleted the object.
		ok &= n.Call<int>(0x1d9f0, &owner, "NpNativeEvent", uint32_t(0x121)) == 0;
	}
	n.Call<void>(0x1d930, &alias);
	n.Call<void>(0x1d930, &owner); // exercises initialized creator destructor
	Object probe;
	n.Call<void>(0x1d910, &probe);
	const int reopened = n.Call<int>(0x1da30, &probe, "NpNativeEvent");
	ok &= reopened < 0; // destructor removed name; failure leaves state zero
	n.Call<void>(0x1d930, &probe);
	return ok;
}

bool Mutexes(Native n) {
	Object object;
	n.Call<void>(0x1e410, &object);
	bool ok = !n.Call<bool>(0x1e4b0, &object) && object.vptr == n.base + 0x48be0;
	const int created = n.Call<int>(0x1e4c0, &object, "NpNativeMutex", uint32_t(0));
	ok &= created == 0;
	if (created == 0) {
		ok &= n.Call<bool>(0x1e4b0, &object);
		ok &= n.Call<int>(0x1e4c0, &object, "NpNativeMutex", uint32_t(0)) == AlreadyInitialized;
		n.Call<void>(0x1e5f0, &object);
		Worker w {n, Operation::Mutex, &object};
		Libs::LibKernel::Pthread thread = nullptr;
		const bool started = Start(w, thread);
		ok &= started;
		if (started) { ok &= Pending(w); }
		n.Call<void>(0x1e6a0, &object); // release BEFORE join or any Check
		Join(thread);
		ok &= started && w.result == 0 && w.try_result == static_cast<int>(0x8055800fU);
		n.Call<void>(0x1e500, &object);
		ok &= !n.Call<bool>(0x1e4b0, &object);
		ok &= n.Call<int>(0x1e4c0, &object, "NpNativeMutex", uint32_t(1)) == 0;
		if (object.state != 0) {
			// bit0 requests recursive type2 at lower pthread attribute seam.
			n.Call<void>(0x1e5f0, &object);
			n.Call<void>(0x1e5f0, &object);
			n.Call<void>(0x1e6a0, &object);
			n.Call<void>(0x1e6a0, &object);
		}
	}
	n.Call<void>(0x1e430, &object);
	return ok;
}

bool Conditions(Native n) {
	void* mutex = nullptr;
	void* cond = nullptr;
	const int mc = n.Call<int>(0x6950, &mutex, "NpNativeCondMutex", uint32_t(0));
	const int cc = n.Call<int>(0x6620, &cond, "NpNativeCond");
	bool ok = mc == 0 && cc == 0;
	if (ok) {
		const int locked = n.Call<int>(0x69f0, &mutex);
		ok &= locked == 0;
		if (locked == 0) {
			ok &= TimesOut([&](uint32_t timeout) { return n.Call<int>(0x66c0, &cond, &mutex, timeout); });
			ok &= n.Call<int>(0x6a30, &mutex) == 0; // timed wait reacquired it
		}
		Worker w {n, Operation::Cond};
		w.handle = cond;
		w.mutex = &mutex;
		Libs::LibKernel::Pthread thread = nullptr;
		const bool started = Start(w, thread);
		ok &= started;
		if (started) {
			ok &= Await(w.entered);
			// Worker announces while holding mutex; acquiring it guarantees its
			// atomic cond release/enrollment before publishing the predicate.
			const int acquired = n.Call<int>(0x69f0, &mutex);
			ok &= acquired == 0 && !w.done.load();
			w.release.store(true);
			ok &= n.Call<int>(0x66f0, &cond) == 0;
			if (acquired == 0) { ok &= n.Call<int>(0x6a30, &mutex) == 0; }
		}
		Join(thread);
		ok &= started && w.result == 0 && w.unlock_result == 0;
		Worker first {n, Operation::Cond}, second {n, Operation::Cond};
		first.handle = second.handle = cond;
		first.mutex = second.mutex = &mutex;
		Libs::LibKernel::Pthread a = nullptr, b = nullptr;
		const bool start_a = Start(first, a), start_b = Start(second, b);
		ok &= start_a && start_b;
		if (start_a) { ok &= Await(first.entered); }
		if (start_b) { ok &= Await(second.entered); }
		const int acquired = n.Call<int>(0x69f0, &mutex);
		ok &= acquired == 0 && !first.done.load() && !second.done.load();
		first.release.store(true); second.release.store(true);
		ok &= n.Call<int>(0x6700, &cond) == 0; // Both enrolled waiters must wake.
		if (acquired == 0) { ok &= n.Call<int>(0x6a30, &mutex) == 0; }
		Join(a); Join(b);
		ok &= start_a && start_b && first.result == 0 && second.result == 0 && first.unlock_result == 0 && second.unlock_result == 0;
	}
	if (cc == 0) { ok &= n.Call<int>(0x66a0, &cond) == 0; }
	if (mc == 0) { ok &= n.Call<int>(0x69e0, &mutex) == 0; }
	return ok;
}

bool Semaphores(Native n) {
	void* sema = nullptr;
	// Native +6b50 ABI: out, shared-bit, initial, maximum, name.
	const int created = n.Call<int>(0x6b50, &sema, uint32_t(0), int32_t(0), int32_t(1), "NpNativeSema");
	bool ok = created == 0;
	if (created == 0) {
		ok &= n.Call<int>(0x6c10, sema) == EmptySema; // audited private poll
		ok &= TimesOut([&](uint32_t timeout) { return n.Call<int>(0x6be0, sema, &timeout); });
		Worker w {n, Operation::Sema};
		w.handle = sema;
		Libs::LibKernel::Pthread thread = nullptr;
		const bool started = Start(w, thread);
		ok &= started;
		if (started) { ok &= Enrolled([&] { return Libs::LibKernel::Semaphore::SemaWaitersForTest(reinterpret_cast<uint64_t>(sema)) == 1; }); }
		ok &= n.Call<int>(0x6c40, sema) == 0;
		Join(thread);
		ok &= started && w.result == 0;
		ok &= n.Call<int>(0x6c10, sema) == EmptySema;
		ok &= n.Call<int>(0x6c40, sema) == 0;
		ok &= n.Call<int>(0x6c10, sema) == 0;
		ok &= n.Call<int>(0x6b80, sema) == 0;
	}
	return ok;
}
} // namespace NpNativeSynchronizationTest

void TestNpNativeSynchronization() {
	constexpr const char* test = "NpNativeSynchronization";
	const char* path = std::getenv("KYTY_TEST_NP_ALTERNATE_IMAGE");
	Check(test, path != nullptr, "explicit audited alternate image required");
	if (!path) { return; }
	for (const auto* name: {"KYTY_NATIVE_NP_OWNERSHIP", "KYTY_NATIVE_NP_WORKQUEUE", "KYTY_NATIVE_NP_CALLOUT",
	                       "KYTY_NATIVE_NP_HTTP_POOL", "KYTY_NATIVE_NP_DISPATCHER"}) {
#if defined(_WIN32)
		_putenv_s(name, "");
#else
		unsetenv(name);
#endif
	}
#if defined(_WIN32)
	_putenv_s("KYTY_NATIVE_NP_ALTERNATE_STARTUP", "1");
#else
	setenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP", "1", 1);
#endif
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitLibKernel_1(linker->Symbols());
	auto* provider = linker->LoadProgram(path);
	Check(test, provider && provider->np_alternate54, "loader verifies exact alternate SHA256 before native execution");
	if (!provider) { return; }
	for (const auto& entry: Loader::NpSynchronizationExports) {
		Check(test, provider->export_symbols->FindByNid(entry.nid.data(), Loader::SymbolType::Func) == nullptr,
		      "review inventory must not widen production alternate54 exports");
	}
	linker->RelocateProgram(provider);
	const NpNativeSynchronizationTest::Native native {provider->base_vaddr};
	const bool events = NpNativeSynchronizationTest::Events(native);
	const bool mutexes = NpNativeSynchronizationTest::Mutexes(native);
	const bool conditions = NpNativeSynchronizationTest::Conditions(native);
	const bool semaphores = NpNativeSynchronizationTest::Semaphores(native);
	// Every worker has joined and every native object is retired before unmap or
	// any throwing Check. No module initializer or application entry is invoked.
	linker->Clear();
	Check(test, events, "native event constructor/open/poll/timeout/wake/close/destructor lifecycle");
	Check(test, mutexes, "native mutex exclusion/recursive lifecycle and teardown");
	Check(test, conditions, "native cond timeout/reacquire/signal/broadcast/destroy boundaries");
	Check(test, semaphores, "native semaphore timeout/wake/permit consumption/delete boundaries");
	std::printf("[native]  %-48s ok\n", test);
}
