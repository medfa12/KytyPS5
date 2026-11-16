// Included inside Libs::Sysmodule by libSysmodule.cpp in the CPU fixture build.
// Parent dispatch: Libs::Sysmodule::TestNativeModuleState(std::string*).
// Requires a dedicated fresh process: the final section initializes/shuts down
// FileSystem. No firmware, graphics, executable mappings or guest entry needed.
namespace NativeModuleFixture {
struct Calls {
	int start_result = 0;
	int stop_result = 0;
	int load_result = 1234;
	int unload_result = 0;
	unsigned starts = 0;
	unsigned stops = 0;
	bool valid_arguments = true;
	uint32_t critical_baseline = 0;
	std::string path;
};
static thread_local Calls* calls = nullptr;
static LibKernel::KernelModule KYTY_SYSV_ABI Start(
    const char* path, size_t args, const void* argp, uint32_t flags,
    const LibKernel::KernelLoadModuleOpt* opt, int* result) {
	++calls->starts;
	calls->path = path;
	calls->valid_arguments &= args == 0 && argp == nullptr && flags == 0 && opt == nullptr && result != nullptr;
	calls->valid_arguments &= Common::GetHostLockCriticalCounter()->load() == calls->critical_baseline;
	if (result != nullptr) { *result = calls->start_result; }
	return calls->load_result;
}
static int KYTY_SYSV_ABI Stop(
    LibKernel::KernelModule handle, size_t args, const void* argp, uint32_t flags,
    const LibKernel::KernelUnloadModuleOpt* opt, int* result) {
	++calls->stops;
	calls->valid_arguments &= handle == 1234 && args == 0 && argp == nullptr && flags == 0 && opt == nullptr && result != nullptr;
	calls->valid_arguments &= Common::GetHostLockCriticalCounter()->load() == calls->critical_baseline;
	if (result != nullptr) { *result = calls->stop_result; }
	return calls->unload_result;
}
} // namespace NativeModuleFixture

bool TestNativeModuleState(std::string* failure) {
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure != nullptr) { *failure = message; }
		return ok;
	};
	using Result = ModuleRegistry::LoadResult;
	const auto critical_counter = Common::GetHostLockCriticalCounter();
	const auto critical_baseline = critical_counter->load();
	bool lock_protected = true;
	{
		std::mutex native_mutex;
		RegistryLock adapter(native_mutex);
		std::unique_lock lock(adapter);
		lock_protected &= critical_counter->load() == critical_baseline + 1;
		lock.unlock();
		lock_protected &= critical_counter->load() == critical_baseline;
		lock.lock();
		lock_protected &= critical_counter->load() == critical_baseline + 1;
	}
	if (!check(lock_protected && critical_counter->load() == critical_baseline,
	           "registry lock/relock must acquire suspension protection and unlock/destruction release it")) { return false; }
	constexpr uint16_t esvm = 0x012e;
	const auto* mapping = FindNativeModule(esvm);
	if (!check(mapping != nullptr && std::string(mapping->path) == "/system/common/lib/libSceEsvm.sprx" &&
	               std::string(mapping->alternate_path) == "/common/lib/libSceEsvm.sprx" &&
	               FindNativeModule(0x0115) != nullptr && FindNativeModule(0x7ffe) == nullptr,
	           "firmware Esvm mapping and existing native/HLE mappings")) { return false; }

	Loader::Program owner_a, owner_b, private_a;
	owner_a.unique_id = 900001;
	owner_b.unique_id = 900002;
	owner_a.application_owner = &owner_a;
	owner_b.application_owner = &owner_b;
	private_a.application_owner = &owner_a;
	if (!check(Loader::InitializeApplicationExecution(&owner_a) && Loader::InitializeApplicationExecution(&owner_b),
	           "initialize shared owner execution lifetimes")) { return false; }
	ModuleRegistry registry;
	unsigned starts = 0, stops = 0;
	bool callbacks_unprotected = true;
	const auto start = [&] {
		callbacks_unprotected &= critical_counter->load() == critical_baseline;
		++starts;
		return Result {0, 1234};
	};
	const auto stop = [&](LibKernel::KernelModule handle) {
		callbacks_unprotected &= critical_counter->load() == critical_baseline;
		++stops;
		return handle == 1234 ? 0 : -1;
	};
	if (!check(registry.LoadModule(nullptr, esvm, [] { return Result {LibKernel::KERNEL_ERROR_ENOENT, -1}; }) ==
	               LibKernel::KERNEL_ERROR_ENOENT && !registry.IsLoaded(nullptr, esvm) &&
	               registry.UnloadModule(nullptr, esvm, stop) == SYSMODULE_ERROR_UNLOADED && stops == 0,
	           "missing provider must not publish loaded state/reference")) { return false; }
	if (!check(registry.LoadModule(nullptr, esvm, start) == 0 && registry.LoadModule(nullptr, esvm, start) == 0 && starts == 1 &&
	               !registry.IsLoaded(&owner_a, esvm) && !registry.IsLoaded(&owner_b, esvm),
	           "successful retry and repeated references must remain session-owner scoped")) { return false; }
	if (!check(registry.LoadModule(&owner_a, esvm, start) == 0 && registry.LoadModule(&owner_b, esvm, start) == 0 && starts == 3 &&
	               registry.UnloadModule(&owner_a, esvm, stop) == 0 && !registry.IsLoaded(&owner_a, esvm) &&
	               registry.IsLoaded(&owner_b, esvm) && registry.IsLoaded(nullptr, esvm) && stops == 1,
	           "distinct runtime owners must own independent native references")) { return false; }
	if (!check(registry.UnloadModule(nullptr, esvm, stop) == 0 && stops == 1 && registry.IsLoaded(nullptr, esvm) &&
	               registry.UnloadModule(nullptr, esvm, [](auto) { return SYSMODULE_ERROR_FATAL; }) == SYSMODULE_ERROR_FATAL &&
	               registry.IsLoaded(nullptr, esvm) && registry.UnloadModule(nullptr, esvm, stop) == 0 && stops == 2 &&
	               !registry.IsLoaded(nullptr, esvm) && registry.UnloadModule(&owner_b, esvm, stop) == 0 && stops == 3,
	           "last reference stops once; failed stop preserves the reference for retry")) { return false; }
	ModuleRegistry recycled_owner;
	if (!check(recycled_owner.LoadModule(&owner_a, esvm, start) == 0, "initial owner lifetime load")) { return false; }
	++owner_a.unique_id; // Simulate Program storage reused for a new application.
	if (!check(!recycled_owner.IsLoaded(&owner_a, esvm) && recycled_owner.LoadModule(&owner_a, esvm, start) == 0 &&
	               recycled_owner.UnloadModule(&owner_a, esvm, stop) == 0,
	           "a recycled Program address must not inherit a previous application lifetime's loaded state")) { return false; }
	--owner_a.unique_id;
	if (!check(recycled_owner.UnloadModule(&owner_a, esvm, stop) == 0, "release old lifetime fixture reference")) { return false; }
	const auto stops_before_reentry = stops;

	// Same-thread recursion must fail, not deadlock or advertise the unfinished
	// outer image as loaded. A different, idle module can still be initialized.
	bool reentry_ok = false, finalizer_ok = false;
	const auto recursive = [&] {
		reentry_ok = critical_counter->load() == critical_baseline && !registry.IsLoaded(nullptr, esvm) &&
		    registry.LoadModule(nullptr, esvm, start) == SYSMODULE_ERROR_FATAL &&
		    registry.UnloadModule(nullptr, esvm, stop) == SYSMODULE_ERROR_FATAL &&
		    registry.LoadModule(nullptr, 0x7ffe, [] { return Result {0, -1}; }) == 0 &&
		    critical_counter->load() == critical_baseline;
		return Result {0, 1234};
	};
	if (!check(registry.LoadModule(nullptr, esvm, recursive) == 0 && reentry_ok &&
	               registry.UnloadModule(nullptr, esvm, [&](auto) {
		               finalizer_ok = critical_counter->load() == critical_baseline && !registry.IsLoaded(nullptr, esvm) &&
		                   registry.LoadModule(nullptr, esvm, start) == SYSMODULE_ERROR_FATAL &&
		                   registry.UnloadModule(nullptr, esvm, stop) == SYSMODULE_ERROR_FATAL &&
		                   critical_counter->load() == critical_baseline;
		               return 0;
	               }) == 0 && finalizer_ok && registry.UnloadModule(nullptr, 0x7ffe, stop) == 0 && stops == stops_before_reentry,
	           "initializer/finalizer reentry must run unlocked without fabricating native/HLE handles")) { return false; }
	bool failed_reentry = false;
	if (!check(registry.LoadModule(nullptr, esvm, [&] {
		           failed_reentry = registry.LoadModule(nullptr, esvm, start) == SYSMODULE_ERROR_FATAL;
		           return Result {-91, -1};
	           }) == -91 && failed_reentry && !registry.IsLoaded(nullptr, esvm) &&
	               registry.LoadModule(nullptr, esvm, start) == 0 && registry.UnloadModule(nullptr, esvm, stop) == 0,
	           "failed recursive initialization must leave no state and permit an independent retry")) { return false; }
	if (!check(callbacks_unprotected && critical_counter->load() == critical_baseline,
	           "load/stop callbacks and all early/error/reentry returns must leave no registry suspension guard")) { return false; }

	// Observe a real registry waiter only after it released the registry mutex.
	// Its host counter must become clear while the initializer remains blocked,
	// rather than carrying protection across condition_variable_any::wait.
	ModuleRegistry parked;
	std::atomic<bool> initializer_entered {false}, finish_initializer {false}, waiter_published {false}, parking_ok {true};
	std::shared_ptr<Common::HostLockCriticalCounter> waiter_counter;
	std::thread initializing([&] {
		if (parked.LoadModule(nullptr, esvm, [&] {
			if (Common::GetHostLockCriticalCounter()->load() != 0) { parking_ok.store(false); }
			initializer_entered.store(true);
			while (!finish_initializer.load()) { std::this_thread::yield(); }
			return Result {0, 1234};
		}) != 0 || Common::GetHostLockCriticalCounter()->load() != 0) { parking_ok.store(false); }
	});
	while (!initializer_entered.load()) { std::this_thread::yield(); }
	std::thread waiting([&] {
		waiter_counter = Common::GetHostLockCriticalCounter();
		waiter_published.store(true);
		if (parked.LoadModule(nullptr, esvm, [] { return Result {-96, -1}; }) != 0 ||
		    waiter_counter->load() != 0) { parking_ok.store(false); }
	});
	while (!waiter_published.load()) { std::this_thread::yield(); }
	const auto park_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (parked.TestWaiters() == 0 && std::chrono::steady_clock::now() < park_deadline) { std::this_thread::yield(); }
	const bool waiter_observed = parked.TestWaiters() == 1;
	while (waiter_counter->load() != 0 && std::chrono::steady_clock::now() < park_deadline) { std::this_thread::yield(); }
	const bool waiter_unprotected = waiter_observed && waiter_counter->load() == 0;
	finish_initializer.store(true);
	initializing.join();
	waiting.join();
	if (!check(waiter_unprotected && parking_ok.load() && parked.TestWaiters() == 0 &&
	               parked.UnloadModule(nullptr, esvm, [](auto) { return 0; }) == 0 &&
	               parked.UnloadModule(nullptr, esvm, [](auto) { return 0; }) == 0,
	           "condition waiter must drop host critical protection while parked and restore/release it on wake")) { return false; }

	// All callers share one initializer, then hold exactly one reference each.
	ModuleRegistry concurrent;
	std::atomic<unsigned> ready {0}, concurrent_starts {0}, concurrent_stops {0};
	std::atomic<bool> entered {false}, release {false}, worker_ok {true};
	constexpr unsigned workers_count = 8;
	std::thread workers[workers_count];
	for (auto& worker: workers) {
		worker = std::thread([&] {
			ready.fetch_add(1);
			while (ready.load() != workers_count) { std::this_thread::yield(); }
			if (concurrent.LoadModule(&owner_a, esvm, [&] {
				if (Common::GetHostLockCriticalCounter()->load() != 0) { worker_ok.store(false); }
				concurrent_starts.fetch_add(1);
				entered.store(true);
				while (!release.load()) { std::this_thread::yield(); }
				return Result {0, 1234};
			}) != 0 || Common::GetHostLockCriticalCounter()->load() != 0) { worker_ok.store(false); }
		});
	}
	while (!entered.load()) { std::this_thread::yield(); }
	const bool pending_hidden = !concurrent.IsLoaded(&owner_a, esvm) && !concurrent.IsLoaded(&owner_b, esvm);
	release.store(true);
	for (auto& worker: workers) { worker.join(); }
	if (!check(pending_hidden && worker_ok.load() && concurrent_starts.load() == 1 && concurrent.IsLoaded(&owner_a, esvm),
	           "concurrent loads must wait for one initializer and never expose pending state")) { return false; }
	for (auto& worker: workers) {
		worker = std::thread([&] {
			if (concurrent.UnloadModule(&owner_a, esvm, [&](auto handle) {
				if (handle != 1234 || Common::GetHostLockCriticalCounter()->load() != 0) { worker_ok.store(false); }
				concurrent_stops.fetch_add(1);
				return 0;
			}) != 0 || Common::GetHostLockCriticalCounter()->load() != 0) { worker_ok.store(false); }
		});
	}
	for (auto& worker: workers) { worker.join(); }
	if (!check(worker_ok.load() && concurrent_stops.load() == 1 && !concurrent.IsLoaded(&owner_a, esvm),
	           "concurrent final releases must stop exactly once without dropping references")) { return false; }

	// Two guest initializers requesting each other's in-flight module must not
	// form a cross-thread wait cycle. Ordinary external callers still wait above.
	ModuleRegistry cyclic;
	std::atomic<unsigned> in_initializers {0};
	std::atomic<unsigned> cycle_failures {0};
	std::thread left([&] {
		cyclic.LoadModule(nullptr, 1, [&] {
			in_initializers.fetch_add(1);
			while (in_initializers.load() != 2) { std::this_thread::yield(); }
			if (cyclic.LoadModule(nullptr, 2, start) == SYSMODULE_ERROR_FATAL) { cycle_failures.fetch_add(1); }
			return Result {0, -1};
		});
	});
	std::thread right([&] {
		cyclic.LoadModule(nullptr, 2, [&] {
			in_initializers.fetch_add(1);
			while (in_initializers.load() != 2) { std::this_thread::yield(); }
			if (cyclic.LoadModule(nullptr, 1, start) == SYSMODULE_ERROR_FATAL) { cycle_failures.fetch_add(1); }
			return Result {0, -1};
		});
	});
	left.join();
	right.join();
	if (!check(cycle_failures.load() >= 1 && cyclic.IsLoaded(nullptr, 1) && cyclic.IsLoaded(nullptr, 2),
	           "cross-thread recursive dependency must fail rather than deadlock")) { return false; }

	namespace Fs = LibKernel::FileSystem;
	Fs::Initialize();
	struct Cleanup {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		std::filesystem::path root;
		~Cleanup() {
			NativeModuleFixture::calls = nullptr;
			Loader::RuntimeLinker::SetCurrentProgram(previous);
			Fs::Shutdown();
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup;
	Fs::ScopedGuestFsContext fs_scope(Fs::CreateBootstrapFsContext());
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	cleanup.root = std::filesystem::temp_directory_path() /
	    ("kyty_sysmodule_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root / "system");
	std::filesystem::create_directories(cleanup.root / "common");
	Fs::Mount(cleanup.root / "system", "/system/common/lib");
	Fs::Mount(cleanup.root / "common", "/common/lib");
	int with_arg_result = 77;
	if (!check(SysmoduleLoadModule(esvm) == LibKernel::KERNEL_ERROR_ENOENT && SysmoduleIsLoaded(esvm) == SYSMODULE_ERROR_UNLOADED &&
	               SysmoduleLoadModuleInternalWithArg(esvm, 0, 0, 0, &with_arg_result) == LibKernel::KERNEL_ERROR_ENOENT &&
	               with_arg_result == LibKernel::KERNEL_ERROR_ENOENT &&
	               SysmoduleLoadModuleInternal(0x8000012e, 0, 0, 0, 0, 0) == LibKernel::KERNEL_ERROR_ENOENT &&
	               SysmoduleUnloadModule(esvm) == SYSMODULE_ERROR_UNLOADED,
	           "real missing-provider path must propagate errors through all load wrappers without loaded state")) { return false; }
	if (!check(SysmoduleLoadModuleInternal(0x800000c6, 0, 0, 0, 0, 0) == LibKernel::KERNEL_ERROR_ENOENT &&
	               !SysmoduleIsLoadedId(0x800000c6),
	           "missing CloudMessaging must fail native loading rather than publish an HLE-only success")) { return false; }
	if (!check(SysmoduleLoadModuleInternal(0x80000076, 0, 0, 0, 0, 0) == LibKernel::KERNEL_ERROR_ENOENT &&
	               !SysmoduleIsLoadedId(0x80000076),
	           "missing RNPS AppMgr must fail native loading rather than publish an HLE-only success")) { return false; }

	// Narrow backend seam exercises the actual native path and start/stop result
	// handling, without executing a firmware binary or replacing global services.
	NativeModuleFixture::Calls calls;
	calls.critical_baseline = critical_baseline;
	NativeModuleFixture::calls = &calls;
	auto loaded = LoadNativeModule(esvm, NativeModuleFixture::Start);
	if (!check(loaded.status == 0 && loaded.handle == 1234 && calls.path == "/common/lib/libSceEsvm.sprx" && calls.starts == 1,
	           "alternate common mount must be used when system path is missing")) { return false; }
	Common::File file;
	if (!check(file.Create(cleanup.root / "system/libSceEsvm.sprx"), "create path-selection fixture")) { return false; }
	file.Close();
	loaded = LoadNativeModule(esvm, NativeModuleFixture::Start);
	if (!check(loaded.status == 0 && calls.path == "/system/common/lib/libSceEsvm.sprx",
	           "system common provider must take precedence when present")) { return false; }
	calls.start_result = -93;
	calls.load_result = LibKernel::KERNEL_ERROR_EINVAL;
	ModuleRegistry native_errors;
	const auto native_start = [&] { return LoadNativeModule(esvm, NativeModuleFixture::Start); };
	const auto native_stop = [&](auto handle) { return StopNativeModule(handle, NativeModuleFixture::Stop); };
	if (!check(native_errors.LoadModule(nullptr, esvm, native_start) == LibKernel::KERNEL_ERROR_EINVAL &&
	               !native_errors.IsLoaded(nullptr, esvm) && calls.stops == 0,
	           "kernel-rejected negative initializer must propagate API error without a second stop/rollback")) { return false; }
	calls.start_result = 7;
	calls.load_result = 1234;
	if (!check(native_errors.LoadModule(nullptr, esvm, native_start) == 0 &&
	               native_errors.IsLoaded(nullptr, esvm) && calls.stops == 0,
	           "positive initializer output with a successful handle must publish a loaded reference")) { return false; }
	calls.stop_result = -94;
	if (!check(native_errors.UnloadModule(nullptr, esvm, native_stop) == 0 &&
	               !native_errors.IsLoaded(nullptr, esvm) && calls.stops == 1,
	           "negative finalizer output with API success must release the native reference")) { return false; }
	if (!check(native_errors.LoadModule(nullptr, esvm, native_start) == 0,
	           "reload after successful unload with negative finalizer output")) { return false; }
	calls.stop_result = 9;
	if (!check(native_errors.UnloadModule(nullptr, esvm, native_stop) == 0 &&
	               !native_errors.IsLoaded(nullptr, esvm) && calls.stops == 2,
	           "positive finalizer output is informational and must not leave stale loaded state")) { return false; }
	calls.load_result = LibKernel::KERNEL_ERROR_ENOENT;
	loaded = LoadNativeModule(esvm, NativeModuleFixture::Start);
	if (!check(loaded.status == LibKernel::KERNEL_ERROR_ENOENT && calls.stops == 2,
	           "negative load handle must propagate without attempting stop")) { return false; }
	calls.load_result = 1234;
	if (!check(native_errors.LoadModule(nullptr, esvm, native_start) == 0, "load before API stop-failure fixture")) { return false; }
	calls.stop_result = 0;
	calls.unload_result = -95;
	if (!check(native_errors.UnloadModule(nullptr, esvm, native_stop) == SYSMODULE_ERROR_FATAL &&
	               native_errors.IsLoaded(nullptr, esvm) && calls.valid_arguments,
	           "only kernel stop API failure preserves the native reference for retry")) { return false; }
	calls.unload_result = 0;
	if (!check(native_errors.UnloadModule(nullptr, esvm, native_stop) == 0 && !native_errors.IsLoaded(nullptr, esvm),
	           "successful API stop retry must release the retained reference")) { return false; }
	const auto cloud = LoadNativeModule(0x800000c6, NativeModuleFixture::Start);
	if (!check(cloud.status == 0 && cloud.handle == 1234 &&
	               calls.path == "/system/common/lib/libSceCloudMessaging.sprx" && calls.valid_arguments,
	           "CloudMessaging returns the native loader handle for the provider PSM resolves")) { return false; }
	const auto rnps = LoadNativeModule(0x80000076, NativeModuleFixture::Start);
	if (!check(rnps.status == 0 && rnps.handle == 1234 &&
	               calls.path == "/system/common/lib/libSceRnpsAppMgr.sprx" && calls.valid_arguments,
	           "RNPS AppMgr returns the native loader handle for the provider PSM resolves")) { return false; }
	ModuleRegistry namespaces;
	if (!check(FindNativeModule(0xc6) == nullptr && FindNativeModule(0x76) == nullptr &&
	               namespaces.LoadModule(nullptr, CanonicalModuleId(0xc6), [] { return Result {0, -1}; }) == 0 &&
	               !namespaces.IsLoaded(nullptr, CanonicalModuleId(0x800000c6)) &&
	               namespaces.LoadModule(nullptr, CanonicalModuleId(0x800000c6), [] { return Result {0, 1234}; }) == 0 &&
	               namespaces.UnloadModule(nullptr, CanonicalModuleId(0xc6), [](auto) { return -1; }) == 0 &&
	               namespaces.IsLoaded(nullptr, CanonicalModuleId(0x800000c6)) &&
	               namespaces.UnloadModule(nullptr, CanonicalModuleId(0x800000c6), [](auto h) { return h == 1234 ? 0 : -1; }) == 0,
	           "explicit internal providers must not inherit or consume public low-half references")) { return false; }

	// Exercise production owner derivation through the public/internal wrappers,
	// including private modules inheriting their application's registry scope.
	constexpr uint16_t hle = 0x7ffe;
	if (!check(SysmoduleLoadModule(hle) == 0, "null-owner HLE load")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owner_a);
	if (!check(SysmoduleIsLoaded(hle) == SYSMODULE_ERROR_UNLOADED && SysmoduleLoadModule(hle) == 0,
	           "runtime application must not inherit session HLE loaded state")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&private_a);
	if (!check(SysmoduleIsLoaded(hle) == 0 && SysmoduleLoadModuleInternal(0x80007ffe, 0, 0, 0, 0, 0) == 0 &&
	               SysmoduleUnloadModule(hle) == 0 && SysmoduleIsLoaded(hle) == 0 && SysmoduleUnloadModule(hle) == 0 &&
	               SysmoduleIsLoaded(hle) == SYSMODULE_ERROR_UNLOADED,
	           "private-module owner inheritance, internal low-id matching and balanced references")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owner_b);
	if (!check(SysmoduleIsLoaded(hle) == SYSMODULE_ERROR_UNLOADED, "unrelated owner must remain unloaded")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	return check(SysmoduleIsLoaded(hle) == 0 && SysmoduleUnloadModule(hle) == 0 &&
	                 SysmoduleIsLoaded(hle) == SYSMODULE_ERROR_UNLOADED,
	             "runtime application releases must not consume session references");
}
