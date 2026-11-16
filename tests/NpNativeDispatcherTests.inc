// Isolated execution of the byte-audited pair, not expanded production export
// admission. No ShellCore initializer or service worker is started by this test.
namespace NpNativeDispatcherTest {
struct State {
	std::atomic_bool entered {false}, release {false}, joining {false}, joined {false};
	std::atomic_bool returned_early {false};
	Libs::LibKernel::Pthread thread = nullptr;
	using Join = int (KYTY_SYSV_ABI *)(Libs::LibKernel::Pthread, void**);
	Join join = nullptr;
	int result = -1;
	void* returned = nullptr;
	char name[32] {};
};
struct Retirement {
	State& state;
	Libs::LibKernel::Pthread joiner = nullptr;
	bool drained = false;
	void Drain() noexcept {
		if (drained) { return; }
		state.release.store(true);
		// Use the production kernel join for cleanup, independently of the native
		// wrapper being tested. Never unwind stack state or mappings past a live
		// thread, including when an assertion or joiner creation fails.
		if (joiner && Libs::LibKernel::PthreadJoin(joiner, nullptr) != 0) { std::abort(); }
		const bool native_joined = state.joined.load() && state.result == 0 && state.returned == &state && !state.returned_early.load();
		if (state.thread && !native_joined && Libs::LibKernel::PthreadJoin(state.thread, nullptr) != 0) { std::abort(); }
		drained = true;
	}
	~Retirement() { Drain(); }
};
void* KYTY_SYSV_ABI Entry(void* argument) {
	auto* state = static_cast<State*>(argument);
	Libs::LibKernel::PthreadGetname(Libs::LibKernel::PthreadSelf(), state->name);
	state->entered.store(true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!state->release.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return argument;
}
void* KYTY_SYSV_ABI JoinEntry(void* argument) {
	auto* state = static_cast<State*>(argument);
	state->joining.store(true);
	state->result = state->join(state->thread, &state->returned);
	state->returned_early.store(!state->release.load());
	state->joined.store(true);
	return nullptr;
}
}

void TestNpNativeDispatcherPair() {
	constexpr const char* test = "NpNativeDispatcherPair";
	const char* path = std::getenv("KYTY_TEST_NP_ALTERNATE_IMAGE");
	Check(test, path != nullptr, "explicit audited alternate provider required");
	if (!path) { return; }
	// This test is dispatched in its own process and admits only existing54.
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
	Check(test, provider && provider->np_alternate54, "exact image pin before executing audited pair");
	if (!provider) { return; }
	Check(test, provider->export_symbols->FindByNid("fhJ5uKzcn0w", Loader::SymbolType::Func) == nullptr &&
	            provider->export_symbols->FindByNid("EjMsfO3GCIA", Loader::SymbolType::Func) == nullptr,
	      "component execution does not widen production alternate54 exports");
	linker->RelocateProgram(provider);
	using Create = int (KYTY_SYSV_ABI *)(Libs::LibKernel::Pthread*, Libs::LibKernel::pthread_entry_func_t,
	                                    void*, int32_t, uint64_t, uint64_t, const char*);
	// Exact functions and reference closure in the retained alternate audit.
	const auto create = reinterpret_cast<Create>(provider->base_vaddr + 0x6c60);
	NpNativeDispatcherTest::State state;
	state.join = reinterpret_cast<NpNativeDispatcherTest::State::Join>(provider->base_vaddr + 0x6d80);
	NpNativeDispatcherTest::Retirement retirement {state};
	const int result = create(&state.thread, NpNativeDispatcherTest::Entry, &state, 700, 0x10000, 0, "NpPairProbe");
	Check(test, result == 0 && state.thread, "seven-argument native create publishes a real thread");
	if (result == 0 && state.thread) {
		const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!state.entered.load() && std::chrono::steady_clock::now() < entered_deadline) { std::this_thread::yield(); }
		Check(test, state.entered.load(), "native create actually executes entry with its argument");
		if (state.entered.load()) { Check(test, std::strcmp(state.name, "NpPairProbe") == 0, "stack-passed seventh name reaches guest pthread"); }
		const int joined = Libs::LibKernel::PthreadCreate(&retirement.joiner, nullptr, NpNativeDispatcherTest::JoinEntry, &state, "NpPairJoin");
		Check(test, joined == 0 && retirement.joiner, "actual guest join observer created");
		if (joined == 0 && retirement.joiner) {
			const auto joining_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!state.joining.load() && std::chrono::steady_clock::now() < joining_deadline) { std::this_thread::yield(); }
			Check(test, state.joining.load(), "join observer enters before observation interval");
			const auto observation = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
			while (!state.joined.load() && std::chrono::steady_clock::now() < observation) { std::this_thread::yield(); }
			Check(test, state.joining.load() && !state.joined.load(), "native join remains pending while entry owns resources");
			retirement.Drain();
			Check(test, !state.returned_early.load() && state.result == 0 && state.returned == &state, "native join returns actual entry result after release");
		}
	}
	retirement.Drain();
	Loader::Program stopped_owner;
	stopped_owner.application_owner = &stopped_owner;
	Check(test, Loader::InitializeApplicationExecution(&stopped_owner), "stopped-owner failure fixture");
	if (auto execution = Loader::GetApplicationExecution(&stopped_owner)) {
		execution->RequestStop();
		auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
		Loader::RuntimeLinker::SetCurrentProgram(&stopped_owner);
		Libs::LibKernel::Pthread failed = nullptr;
		Check(test, create(&failed, NpNativeDispatcherTest::Entry, &state, 700, 0x10000, 0, "NpPairStopped") < 0 && !failed,
		      "native create propagates closed-owner failure without publishing a handle");
		Loader::RuntimeLinker::SetCurrentProgram(previous);
	}
	linker->Clear();
}
