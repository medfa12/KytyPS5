// Runs against the actual registered guest libc entry points, without firmware.
namespace LibcApplicationStateTest {
using Destructor = void (KYTY_SYSV_ABI *)(void*);
using Register = int (KYTY_SYSV_ABI *)(Destructor, void*, void*);
using Finalize = void (KYTY_SYSV_ABI *)(void*);
Finalize finalize = nullptr;
std::vector<int> calls;

void KYTY_SYSV_ABI Record(void* object) {
	Check(Common::GetHostLockCriticalCounter()->load() == 0, "destructor runs outside host suspension guard");
	calls.push_back(*static_cast<int*>(object));
}

void KYTY_SYSV_ABI Reenter(void* object) {
	Record(object);
	finalize(nullptr);
}

void KYTY_SYSV_ABI Count(void* object) {
	static_cast<std::atomic<unsigned>*>(object)->fetch_add(1);
}

template <typename T>
T Resolve(const Loader::SymbolDatabase& symbols, const char* nid, const char* library = "libc") {
	const auto* record = symbols.Find({nid, library, 1, library, 1, 1, Loader::SymbolType::Func});
	Check(record != nullptr && record->vaddr != 0, "application-state guest export exists");
	return reinterpret_cast<T>(record->vaddr);
}
} // namespace LibcApplicationStateTest

void TestLibcApplicationState() {
	using namespace LibcApplicationStateTest;
	using RT = Loader::RuntimeLinker;
	Loader::SymbolDatabase symbols;
	Libs::LibC::InitLibC_1(&symbols);
	const auto init = Resolve<void (KYTY_SYSV_ABI *)(const Loader::EntryParams*)>(symbols, "bzQExy189ZI");
	const auto set = Resolve<int (KYTY_SYSV_ABI *)(const char*, const char*, int)>(symbols, "M4YYbSFfJ8g");
	const auto get = Resolve<const char* (KYTY_SYSV_ABI *)(const char*)>(symbols, "smbQukfxYJM", "LibcInternal");
	const auto check_environment = [&](const char* name, const char* expected, const char* message) {
		const auto* value = get(name);
		Check(value != nullptr && std::strcmp(value, expected) == 0, message);
	};
	const auto unset = Resolve<int (KYTY_SYSV_ABI *)(const char*)>(symbols, "CRJcH8CnPSI", "LibcInternal");
	const auto add = Resolve<Register>(symbols, "tsvEmnenz48");
	finalize = Resolve<Finalize>(symbols, "H2e8t5ScQGc");
	auto* previous = RT::GetCurrentProgram();
	Loader::Program app_a, module_a, app_b, legacy_a, legacy_b;
	app_a.application_owner = &app_a;
	module_a.application_owner = &app_a;
	app_b.application_owner = &app_b;
	// Same EntryParams layout, with room for argv's terminator and envp.
	struct Entry {
		int argc;
		uint32_t pad;
		const char* argv[5];
	};
	Entry legacy {1, 0, {"legacy", nullptr, "OWNER=legacy", nullptr}};
	Entry a {2, 0, {"app-a", "argument", nullptr, "OWNER=a", nullptr}};
	Entry b {1, 0, {"app-b", nullptr, "OWNER=b", nullptr}};
	RT::SetCurrentProgram(&legacy_a);
	init(reinterpret_cast<const Loader::EntryParams*>(&legacy));
	RT::SetCurrentProgram(&app_a);
	Check(Libs::LibC::GetArgc() == 0 && Libs::LibC::GetArgv() == nullptr, "fresh app cannot see legacy arguments");
	init(reinterpret_cast<const Loader::EntryParams*>(&a));
	RT::SetCurrentProgram(&app_b);
	init(reinterpret_cast<const Loader::EntryParams*>(&b));
	RT::SetCurrentProgram(&module_a);
	Check(Libs::LibC::GetArgc() == 2 && Libs::LibC::GetArgv() == a.argv, "private module shares app arguments");
	init(nullptr);
	RT::SetCurrentProgram(&app_a);
	Check(Libs::LibC::GetArgc() == 0 && Libs::LibC::GetArgv() == nullptr, "null init resets only its app");
	RT::SetCurrentProgram(&app_b);
	Check(Libs::LibC::GetArgc() == 1 && Libs::LibC::GetArgv() == b.argv, "other app arguments survive reset");
	RT::SetCurrentProgram(&legacy_b);
	Check(Libs::LibC::GetArgc() == 1 && Libs::LibC::GetArgv() == legacy.argv, "null-owner programs retain shared arguments");
	RT::SetCurrentProgram(nullptr);
	Check(Libs::LibC::GetArgv() == legacy.argv, "null current program retains legacy arguments");
	init(nullptr);

	constexpr const char* key = "KYTY_LIBC_APP_ISOLATION_TEST";
	constexpr const char* seed = "KYTY_LIBC_APP_SEED_TEST";
	RT::SetCurrentProgram(&legacy_a);
	check_environment(seed, "configured", "legacy environment seeded from initialized Config");
	Check(set(key, "legacy", 1) == 0, "set legacy environment");
	RT::SetCurrentProgram(&app_a);
	check_environment(seed, "configured", "runtime app environment seeded from Config");
	Check(set(seed, "app override", 1) == 0, "override app copy of configured environment");
	Check(set(key, "a", 1) == 0, "set app environment");
	RT::SetCurrentProgram(&app_b);
	check_environment(seed, "configured", "other app retains original configured environment");
	Check(set(key, "b", 1) == 0, "set second app environment");
	RT::SetCurrentProgram(&module_a);
	check_environment(seed, "app override", "private module shares configured environment override");
	check_environment(key, "a", "private module shares app environment");
	Check(set(key, "ignored", 0) == 0, "no-overwrite set succeeds");
	check_environment(key, "a", "no-overwrite semantics preserved");
	Check(unset(key) == 0 && get(key) == nullptr, "module can unset app environment");
	RT::SetCurrentProgram(&app_a);
	Check(get(key) == nullptr, "unset visible to app main");
	RT::SetCurrentProgram(&app_b);
	check_environment(key, "b", "other app environment survives unset");
	unset(key);
	RT::SetCurrentProgram(&legacy_b);
	Check(get(key) == nullptr, "null-owner environment retains per-program isolation");
	RT::SetCurrentProgram(&legacy_a);
	check_environment(key, "legacy", "legacy environment survives app mutation");
	check_environment(seed, "configured", "legacy environment survives app's configured override");
	Check(Config::GetGuestEnvironment().at(seed) == "configured", "guest mutation never changes source Config");
	unset(key);

	int first = 1, second = 2, third = 3, other = 4, old = 5;
	int dso_a = 0, dso_b = 0;
	RT::SetCurrentProgram(&legacy_a);
	add(Record, &old, &dso_a);
	RT::SetCurrentProgram(&app_b);
	add(Record, &other, &dso_a);
	RT::SetCurrentProgram(&app_a);
	add(Record, &first, &dso_a);
	add(Record, &second, &dso_b);
	RT::SetCurrentProgram(&module_a);
	add(Record, &third, &dso_a);
	finalize(&dso_a);
	Check(calls == std::vector<int>({3, 1}), "DSO finalization is reverse-order and app-scoped");
	finalize(&dso_a);
	Check(calls.size() == 2, "DSO destructors run at most once");
	add(Reenter, &third, &dso_a);
	finalize(nullptr);
	Check(calls == std::vector<int>({3, 1, 3, 2}), "reentrant finalization drains only the selected app");
	RT::SetCurrentProgram(&app_b);
	finalize(nullptr);
	RT::SetCurrentProgram(&legacy_b);
	finalize(nullptr);
	Check(calls == std::vector<int>({3, 1, 3, 2, 4, 5}), "other app and shared null-owner destructors remain independent");

	RT::SetCurrentProgram(&app_a);
	std::array<std::atomic<unsigned>, 128> counts {};
	for (auto& count: counts) { add(Count, &count, nullptr); }
	std::array<std::thread, 4> threads;
	for (auto& thread: threads) {
		thread = std::thread([&] {
			RT::SetCurrentProgram(&module_a);
			finalize(nullptr);
			RT::SetCurrentProgram(nullptr);
		});
	}
	for (auto& thread: threads) { thread.join(); }
	for (auto& count: counts) { Check(count.load() == 1, "concurrent finalizers claim each destructor exactly once"); }
	RT::SetCurrentProgram(&app_b);
	init(nullptr);
	RT::SetCurrentProgram(previous);
}
