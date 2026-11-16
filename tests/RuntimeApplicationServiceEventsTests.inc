// Include inside the VirtualMemoryAllocationTests fixture namespace. At top level
// include libs/libAppContent.h, libs/libSystemService.h,
// loader/runtimeGuestExecution.h and loader/systemContent.h.
// Dispatch TestRuntimeApplicationServiceEvents() in a dedicated fresh process
// after InitSubsystems. This calls the REAL registered SysV exports and production
// lifetime hooks; no service-map access, fake execution or guest code/GPU required.
void TestRuntimeApplicationServiceEvents() try {
	constexpr const char* test = "RuntimeApplicationServiceEvents";
	namespace Service = Libs::SystemService;
	using Loader::RuntimeLinker;
	struct Restore {
		Loader::Program* previous = RuntimeLinker::GetCurrentProgram();
		~Restore() { RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	const auto critical_baseline = Common::GetHostLockCriticalCounter()->load();
	Loader::SymbolDatabase symbols;
	Libs::InitSystemService_1(&symbols);
	Libs::InitAppContent_1(&symbols);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr && symbol->vaddr != 0, "real service export registration");
		return symbol->vaddr;
	};
	using Initialize = int (KYTY_SYSV_ABI *)(const Libs::AppContent::AppContentInitParam*, Libs::AppContent::AppContentBootParam*);
	using Receive = int (KYTY_SYSV_ABI *)(Service::SystemServiceEvent*);
	using Status = int (KYTY_SYSV_ABI *)(Service::SystemServiceStatus*);
	using Param = int (KYTY_SYSV_ABI *)(uint32_t, int32_t*);
	using String = int (KYTY_SYSV_ABI *)(int, char*, size_t);
	const auto initialize = reinterpret_cast<Initialize>(resolve("R9lA82OraNs"));
	const auto receive = reinterpret_cast<Receive>(resolve("656LMQSrg6U"));
	const auto status = reinterpret_cast<Status>(resolve("rPo6tV8D9bM"));
	const auto param = reinterpret_cast<Param>(resolve("99b82IKXpH4"));
	const auto string = reinterpret_cast<String>(resolve("SsC-m-S9JTA"));
	constexpr int parameter = static_cast<int>(0x80d90002u);
	constexpr int not_initialized = static_cast<int>(0x80d90001u);
	constexpr int internal = static_cast<int>(0x80d9000au);

	// SystemContent's registry retains Program identities for the session. Keep
	// these synthetic owners resident just like the existing process-ID fixture.
	auto* shell = new Loader::Program;
	auto* shell_core = new Loader::Program;
	auto* first = new Loader::Program;
	auto* second = new Loader::Program;
	auto* malformed = new Loader::Program;
	Loader::Program private_first;
	shell->unique_id = 795000;
	shell_core->unique_id = 795001;
	first->unique_id = 795002;
	second->unique_id = 795003;
	malformed->unique_id = 795004;
	first->application_owner = first;
	second->application_owner = second;
	malformed->application_owner = malformed;
	private_first.application_owner = first;
	Check(test, Loader::InitializeApplicationExecution(first) && Loader::InitializeApplicationExecution(second),
	      "fixture requires real supported runtime execution boundaries");
	auto first_execution = Loader::GetApplicationExecution(first);
	auto second_execution = Loader::GetApplicationExecution(second);
	const auto old_first = first_execution;
	Check(test, first_execution && second_execution && first_execution->Owner().token != second_execution->Owner().token,
	      "distinct immutable execution lifetime snapshots");

	const auto root = std::filesystem::temp_directory_path() /
	    ("kyservices_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(test, std::filesystem::create_directories(root), "create isolated service metadata fixture");
	struct RemoveFiles {
		std::filesystem::path root;
		~RemoveFiles() { std::error_code error; std::filesystem::remove_all(root, error); }
	} remove_files {root};
	auto write = [&](const char* name, const char* text) {
		Common::File file;
		Check(test, file.Create(root / name), "create real metadata file");
		uint32_t written = 0;
		file.Write(text, static_cast<uint32_t>(std::strlen(text)), &written);
		Check(test, written == std::strlen(text), "write real metadata");
	};
	write("shell.json", R"({"titleId":"NPXS40087","userDefinedParam1":71})");
	write("first.json", R"({"titleId":"TEST00001","userDefinedParam1":11})");
	write("second.json", R"({"contentId":"UP0001-TEST00002_00-SERVICEEVENTTEST0","userDefinedParam1":22})");
	RuntimeLinker::SetCurrentProgram(shell);
	Loader::SystemContentLoadParamSfo(root / "shell.json");
	Check(test, Loader::SystemContentLoadForProgram(first, root / "first.json") &&
	            Loader::SystemContentLoadForProgram(second, root / "second.json"), "publish real owner content snapshots");
	auto count = [&](Loader::Program* caller, int expected) {
		RuntimeLinker::SetCurrentProgram(caller);
		Service::SystemServiceStatus actual;
		CheckOk(test, status(&actual), "query own pending event count");
		Check(test, actual.event_num == expected, "event count belongs to selected lifetime/session");
	};
	auto consume = [&](Loader::Program* caller, bool present) {
		RuntimeLinker::SetCurrentProgram(caller);
		struct Guarded {
			uint32_t before;
			Service::SystemServiceEvent event;
			uint32_t after;
		} actual;
		std::memset(&actual, 0xa5, sizeof(actual));
		const int result = receive(&actual.event);
		Check(test, result == (present ? OK : Service::SYSTEM_SERVICE_ERROR_NO_EVENT), "real event/no-event status");
		Check(test, actual.event.event_type == (present ? 0x10000003 : -1) &&
		            std::all_of(actual.event.data, actual.event.data + sizeof(actual.event.data), [](auto b) { return b == 0; }) &&
		            actual.before == 0xa5a5a5a5u && actual.after == 0xa5a5a5a5u,
		      "entitlement hint has exact type, empty payload and bounded output (also on no-event)");
	};
	auto init = [&](Loader::Program* caller) {
		RuntimeLinker::SetCurrentProgram(caller);
		struct Guarded {
			uint32_t before;
			Libs::AppContent::AppContentBootParam boot;
			uint32_t after;
		} actual;
		std::memset(&actual, 0xa5, sizeof(actual));
		Libs::AppContent::AppContentInitParam input {};
		CheckOk(test, initialize(&input, &actual.boot), "ordinary AppContent initialization");
		const Libs::AppContent::AppContentBootParam expected {};
		Check(test, std::memcmp(&actual.boot, &expected, sizeof(expected)) == 0 &&
		            actual.before == 0xa5a5a5a5u && actual.after == 0xa5a5a5a5u,
		      "initialization writes exactly the real boot record, without entitlement/attribute grants");
	};
	auto metadata = [&](Loader::Program* caller, int32_t expected) {
		RuntimeLinker::SetCurrentProgram(caller);
		int32_t value = -1;
		CheckOk(test, param(1, &value), "real application metadata query");
		Check(test, value == expected, "initializing another app did not overwrite source/shell metadata");
	};

	// Legacy session still has one shared shell/core/null-context queue, but no
	// application can consume it or copy its backlog at registration time.
	count(shell, 0);
	init(shell);
	Service::NotifyEntitlementUpdate();
	count(shell_core, 2);
	RuntimeLinker::SetCurrentProgram(first);
	Libs::AppContent::AppContentBootParam untouched;
	std::memset(&untouched, 0xa5, sizeof(untouched));
	const auto untouched_before = untouched;
	Check(test, initialize(nullptr, &untouched) == not_initialized &&
	            std::memcmp(&untouched, &untouched_before, sizeof(untouched)) == 0,
	      "missing subscription fails initialization without guest output or a shell event");
	Service::SystemServiceEvent unavailable_event;
	std::memset(&unavailable_event, 0xa5, sizeof(unavailable_event));
	const auto unavailable_before = unavailable_event;
	Check(test, receive(&unavailable_event) == Service::SYSTEM_SERVICE_ERROR_UNAVAILABLE &&
	            std::memcmp(&unavailable_event, &unavailable_before, sizeof(unavailable_event)) == 0,
	      "unregistered app must not pop the session queue");
	Check(test, !Service::RegisterApplicationEvents({}) && !Service::ReleaseApplicationEvents({}), "invalid lifecycle snapshots rejected");
	Check(test, Service::RegisterApplicationEvents(first_execution) && Service::RegisterApplicationEvents(second_execution),
	      "register two isolated subscribers before entry");
	count(first, 0);
	count(second, 0);
	count(nullptr, 2);
	init(first);
	init(first); // Existing initialization policy emits one update hint per call.
	init(second); // Exercises content-ID title fallback, not global title state.
	Check(test, Service::RegisterApplicationEvents(first_execution), "idempotent subscription registration");
	count(&private_first, 2);
	count(second, 1);
	count(shell, 2);
	consume(&private_first, true);
	consume(first, true);
	consume(first, false);
	count(second, 1);
	consume(second, true);
	consume(second, false);
	metadata(shell, 71);
	metadata(first, 11);
	metadata(second, 22);
	metadata(&private_first, 11);

	// Captured host producers run with the opposite TLS caller. Delivery must
	// follow the immutable target snapshot, not whichever app happens to be TLS.
	bool first_published = true, second_published = true;
	std::jthread producer_a([&] {
		RuntimeLinker::SetCurrentProgram(second);
		for (int i = 0; i < 24; ++i) { first_published &= Service::NotifyApplicationEntitlementUpdate(first_execution); }
		RuntimeLinker::SetCurrentProgram(nullptr);
	});
	std::jthread producer_b([&] {
		RuntimeLinker::SetCurrentProgram(first);
		for (int i = 0; i < 24; ++i) { second_published &= Service::NotifyApplicationEntitlementUpdate(second_execution); }
		RuntimeLinker::SetCurrentProgram(nullptr);
	});
	producer_a.join();
	producer_b.join();
	Check(test, first_published && second_published, "concurrent captured producers accepted");
	count(first, 24);
	count(second, 24);
	for (int i = 0; i < 24; ++i) { consume(first, true); }
	consume(first, false);
	count(second, 24);
	count(shell_core, 2);

	// Leave pending data at close. Stop closes publication immediately; teardown
	// may not discard the subscription while any admitted work is outstanding.
	init(first);
	Check(test, !Service::ReleaseApplicationEvents(first_execution), "cannot release running subscriber");
	auto in_flight = first_execution->TryAcquire(Loader::GuestWorkKind::Callback);
	Check(test, bool(in_flight), "real outstanding owner work lease");
	first_execution->RequestStop();
	Check(test, !Service::ReleaseApplicationEvents(first_execution) &&
	            !Service::NotifyApplicationEntitlementUpdate(first_execution) &&
	            !Service::RegisterApplicationEvents(first_execution), "stop rejects producers/registration and premature cleanup");
	RuntimeLinker::SetCurrentProgram(&private_first);
	Service::SystemServiceStatus unavailable_status;
	std::memset(&unavailable_status, 0xa5, sizeof(unavailable_status));
	std::array<uint8_t, sizeof(unavailable_status)> status_before;
	std::memcpy(status_before.data(), &unavailable_status, sizeof(unavailable_status));
	Check(test, status(&unavailable_status) == Service::SYSTEM_SERVICE_ERROR_UNAVAILABLE &&
	            std::memcmp(&unavailable_status, status_before.data(), status_before.size()) == 0,
	      "stopped private-module status cannot expose peer/session backlog");
	Check(test, receive(&unavailable_event) == Service::SYSTEM_SERVICE_ERROR_UNAVAILABLE &&
	            initialize(nullptr, &untouched) == not_initialized, "stopped service calls cannot reopen admission");
	Service::NotifyEntitlementUpdate(); // Even the legacy void producer cannot fall back to shell.
	in_flight.Reset();
	Check(test, Service::ReleaseApplicationEvents(first_execution) && first_execution->Retire(), "release quiescent old lifetime");
	count(second, 24);
	count(shell, 2);

	// Deliberately reuse both Program address and unique ID after actual retirement.
	// This simulates a new incarnation, not replacement of a live execution.
	first->application_execution.reset();
	Check(test, Loader::InitializeApplicationExecution(first), "create relaunched lifetime at same Program address");
	first_execution = Loader::GetApplicationExecution(first);
	Check(test, first_execution->Owner().token != old_first->Owner().token &&
	            Service::RegisterApplicationEvents(first_execution), "relaunch has a fresh non-reusing subscription identity");
	count(&private_first, 0);
	consume(first, false);
	init(first);
	Check(test, !Service::NotifyApplicationEntitlementUpdate(old_first) && Service::ReleaseApplicationEvents(old_first),
	      "late old producer/cleanup cannot modify a relaunched subscriber");
	count(first, 1);
	consume(first, true);
	for (int i = 0; i < 24; ++i) { consume(second, true); }
	consume(second, false);
	consume(shell_core, true);
	consume(nullptr, true);
	consume(shell, false);
	RuntimeLinker::SetCurrentProgram(malformed);
	Check(test, receive(&unavailable_event) == Service::SYSTEM_SERVICE_ERROR_UNAVAILABLE &&
	            initialize(nullptr, &untouched) == not_initialized, "owner without execution cannot borrow session services");
	count(shell, 0);

	// Native parameter failures must return normally and preserve all guest bytes.
	for (auto* caller: {shell, first, second}) {
		RuntimeLinker::SetCurrentProgram(caller);
		std::array<int32_t, 3> guarded {123, 456, 789};
		Check(test, param(0, nullptr) == parameter && param(5, &guarded[1]) == parameter &&
		            param(UINT32_MAX, &guarded[1]) == parameter && param(1100, &guarded[1]) == internal &&
		            guarded == std::array<int32_t, 3> {123, 456, 789}, "source-backed invalid/unsupported AppContent paths preserve output");
		std::array<char, 67> name;
		name.fill('x');
		const auto before = name;
		Check(test, string(-1, name.data() + 1, 65) == Service::SYSTEM_SERVICE_ERROR_PARAMETER &&
		            string(0, name.data() + 1, 65) == Service::SYSTEM_SERVICE_ERROR_PARAMETER &&
		            string(6, name.data() + 1, 64) == Service::SYSTEM_SERVICE_ERROR_PARAMETER &&
		            string(6, nullptr, 65) == Service::SYSTEM_SERVICE_ERROR_PARAMETER && name == before,
		      "unknown string selector/null/short native 65-byte buffer returns parameter without writes");
		CheckOk(test, string(6, name.data() + 1, 65), "native-sized local system name");
		Check(test, std::strcmp(name.data() + 1, "Kyty") == 0 && name[65] == '\0' && name.front() == 'x' && name.back() == 'x',
		      "system name terminates within native capacity and preserves canaries");
		Check(test, receive(nullptr) == Service::SYSTEM_SERVICE_ERROR_PARAMETER && status(nullptr) == Service::SYSTEM_SERVICE_ERROR_PARAMETER,
		      "typed null service outputs return normally");
	}
	first_execution->RequestStop();
	second_execution->RequestStop();
	Check(test, Service::ReleaseApplicationEvents(first_execution) && Service::ReleaseApplicationEvents(second_execution) &&
	            Service::ReleaseApplicationEvents(second_execution) && first_execution->Retire() && second_execution->Retire(),
	      "independent and idempotent final owner cleanup");
	{
		Loader::Program abandoned;
		abandoned.application_owner = &abandoned;
		Check(test, Loader::InitializeApplicationExecution(&abandoned), "real abandoned preparation lifetime");
		auto execution = Loader::GetApplicationExecution(&abandoned);
		std::weak_ptr<Loader::RuntimeGuestExecution> weak = execution;
		Check(test, Service::RegisterApplicationEvents(execution) && Service::NotifyApplicationEntitlementUpdate(execution),
		      "abandoned preparation owns a genuine pending notification");
		abandoned.application_execution.reset();
		execution.reset();
		Check(test, weak.expired(), "pending subscription must not pin an abandoned execution forever");
	}
	count(shell, 0);
	Check(test, Common::GetHostLockCriticalCounter()->load() == critical_baseline, "service calls balance host-critical protection");
} catch (const std::exception& error) {
	Fail("RuntimeApplicationServiceEvents", error.what());
}
