// Include inside VirtualMemoryAllocationTests' fixture namespace. At top level
// include libs/runtimeUserService.h and libs/controller.h and declare:
// namespace Libs { void InitUserService_1(Loader::SymbolDatabase*); }
// Run each function in
// its OWN process after InitSubsystems (the session store intentionally loads
// once). No guest execution, GPU, physical controller or real profile is used.
namespace RuntimeUserServiceOwnerFixture {
struct Event { int type; int id; };
struct UserList { int ids[4]; };
struct Api {
	Loader::SymbolDatabase symbols;
	int (KYTY_SYSV_ABI *initialize)(const void*) = nullptr;
	int (KYTY_SYSV_ABI *initialize2)() = nullptr;
	int (KYTY_SYSV_ABI *initial)(int*) = nullptr;
	int (KYTY_SYSV_ABI *event)(Event*) = nullptr;
	int (KYTY_SYSV_ABI *list)(UserList*) = nullptr;
	int (KYTY_SYSV_ABI *name)(int, char*, size_t) = nullptr;
	int (KYTY_SYSV_ABI *number)(int, int32_t*) = nullptr;
	int (KYTY_SYSV_ABI *terminate)() = nullptr;
	Api() {
		Libs::InitUserService_1(&symbols);
		auto resolve = [&](const char* nid) {
			const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
			Check("RuntimeUserServiceOwners", symbol && symbol->vaddr, "registered UserService function");
			return symbol->vaddr;
		};
		initialize = reinterpret_cast<decltype(initialize)>(resolve("j3YMu1MVNNo"));
		initialize2 = reinterpret_cast<decltype(initialize2)>(resolve("az-0R6eviZ0"));
		initial = reinterpret_cast<decltype(initial)>(resolve("CdWp0oHWGr0"));
		event = reinterpret_cast<decltype(event)>(resolve("yH17Q6NWtVg"));
		list = reinterpret_cast<decltype(list)>(resolve("fPhymKNvK-A"));
		name = reinterpret_cast<decltype(name)>(resolve("1xxcMiGu2fo"));
		number = reinterpret_cast<decltype(number)>(resolve("qbwy0Ub8b3M"));
		terminate = reinterpret_cast<decltype(terminate)>(resolve("bwFjS+bX9mA"));
	}
};
struct Restore {
	Loader::Program* program = Loader::RuntimeLinker::GetCurrentProgram();
	bool no_foreground = Libs::NoForegroundUser();
	~Restore() {
		Loader::RuntimeLinker::SetCurrentProgram(program);
		Libs::SetNoForegroundUser(no_foreground);
	}
};
void Execution(const char* test, Loader::Program* owner) {
	owner->application_owner = owner;
	Check(test, Loader::InitializeApplicationExecution(owner), "create exact application execution lifetime");
}
void Retire(const char* test, Loader::Program* owner) {
	const auto execution = Loader::GetApplicationExecution(owner);
	execution->RequestStop();
	execution->BeginDraining();
	Check(test, execution->WaitForDrain(std::chrono::milliseconds(0)), "execution actually drained");
	CheckOk(test, Libs::UserService::RetireUserServiceOwner(owner), "retire subscription after drain");
	Check(test, execution->Retire(), "retire execution");
}
void ExpectEvent(const char* test, Api& api, Loader::Program* caller, int type, int id) {
	Loader::RuntimeLinker::SetCurrentProgram(caller);
	Event event {-123, -456};
	CheckOk(test, api.event(&event), "independent subscriber event");
	Check(test, event.type == type && event.id == id, "exact login/logout identity and order");
}
void NoEvent(const char* test, Api& api, Loader::Program* caller) {
	Loader::RuntimeLinker::SetCurrentProgram(caller);
	Event event {-123, -456};
	Check(test, api.event(&event) == Libs::UserService::USER_SERVICE_ERROR_NO_EVENT && event.type == -123 && event.id == -456,
	      "empty queue preserves event output");
}
} // namespace RuntimeUserServiceOwnerFixture

void TestRuntimeUserServiceOwners() try {
	constexpr const char* test = "RuntimeUserServiceOwners";
	using namespace RuntimeUserServiceOwnerFixture;
	namespace Service = Libs::UserService;
	Restore restore;
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty-users-owner-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(test, std::filesystem::create_directory(root), "create private store fixture");
	struct Remove {
		std::filesystem::path root;
		~Remove() { std::error_code error; std::filesystem::remove_all(root, error); }
	} remove {root};
	const auto path = root / "users.json";
	constexpr char profiles[] = R"({"version":1,"next_user_number":3,"users":[{"id":4097,"name":"Original One","passcode":"1234","auto_login":true,"account_id":123456789,"signin_id":"one@example.invalid","login_flag":7,"last_login_order":0},{"id":4098,"name":"Original Two","passcode":"5678","auto_login":false,"account_id":987654321,"signin_id":"two@example.invalid","login_flag":9,"last_login_order":0}]})";
	{
		Common::File file;
		Check(test, file.Create(path), "create fixture profiles");
		uint32_t written = 0;
		file.Write(profiles, sizeof(profiles) - 1, &written);
		Check(test, written == sizeof(profiles) - 1, "write original profiles");
	}
	auto bytes = [&] {
		Common::File file(path, Common::File::Mode::Read);
		Check(test, !file.IsInvalid(), "read fixture profiles");
		return file.ReadWholeBuffer();
	};
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	config.guest_environment["KYTY_USER_STORE"] = path.string();
	Config::Load(config);
	Libs::SetNoForegroundUser(false);
	Api api;
	Loader::Program session, first, second, child, unprepared, empty;
	Execution(test, &first);
	Execution(test, &second);
	Execution(test, &unprepared);
	Execution(test, &empty);
	child.application_owner = &first;
	Check(test, Service::PrepareUserServiceOwner(&first) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED,
	      "preparation cannot lazily load session store");
	Loader::RuntimeLinker::SetCurrentProgram(&unprepared);
	int initial = -123;
	Check(test, api.initialize(nullptr) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED &&
	            api.initial(&initial) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED && initial == -123,
	      "unprepared application cannot borrow session identity");
	uint64_t local_account = UINT64_MAX;
	Check(test, Service::QueryLocalAccountId(4097, &local_account) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED &&
	            local_account == UINT64_MAX, "unprepared application cannot borrow session account");
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	CheckOk(test, api.initialize(nullptr), "legacy session initialization");
	Check(test, bytes() == std::vector<std::byte>(reinterpret_cast<const std::byte*>(profiles),
	            reinterpret_cast<const std::byte*>(profiles + sizeof(profiles) - 1)), "session load does not rewrite profiles");
	CheckOk(test, Service::PrepareUserServiceOwner(&empty), "prepare honestly logged-out application");
	Loader::RuntimeLinker::SetCurrentProgram(&empty);
	CheckOk(test, api.initialize2(), "initialize logged-out owner");
	Check(test, api.initial(&initial) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN && initial == -1,
	      "auto-login profile never fabricates a login");
	NoEvent(test, api, &empty);
	CheckOk(test, Service::UserServiceStoreLogin(4097), "real session login");
	CheckOk(test, Service::PrepareUserServiceOwner(&first), "prepare first snapshot");
	CheckOk(test, Service::PrepareUserServiceOwner(&second), "prepare second snapshot");
	const auto before_owner_calls = bytes();
	Loader::RuntimeLinker::SetCurrentProgram(&first);
	UserList list {{-123, -123, -123, -123}};
	Check(test, api.list(&list) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED && list.ids[0] == -123,
	      "prepared is not guest-initialized");
	CheckOk(test, api.initialize(nullptr), "initialize first owner");
	Check(test, api.initialize2() == Service::USER_SERVICE_ERROR_ALREADY_INITIALIZED,
	      "both initialize variants share only this owner's state");
	Loader::RuntimeLinker::SetCurrentProgram(&second);
	CheckOk(test, api.initialize2(), "initialize second independently");
	ExpectEvent(test, api, &child, 0, 4097);
	NoEvent(test, api, &first);
	ExpectEvent(test, api, &second, 0, 4097);
	ExpectEvent(test, api, &session, 0, 4097);
	ExpectEvent(test, api, &empty, 0, 4097);
	Check(test, api.initial(&initial) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN && initial == -1,
	      "initial user is launch identity, not the latest login");
	for (auto* caller: {&first, &second, &child}) {
		Loader::RuntimeLinker::SetCurrentProgram(caller);
		CheckOk(test, api.initial(&initial), "owner launch identity");
		Check(test, initial == 4097, "exact snapshot initial user");
		CheckOk(test, api.list(&list), "owner current login list");
		Check(test, list.ids[0] == 4097 && list.ids[1] == -1 && list.ids[2] == -1 && list.ids[3] == -1,
		      "one actual logged-in user only");
		char name[64] {};
		CheckOk(test, api.name(4097, name, sizeof(name)), "snapshot original name");
		Check(test, std::strcmp(name, "Original One") == 0, "no synthetic replacement profile");
		CheckOk(test, Service::QueryLocalAccountId(4097, &local_account), "read original local account association");
		Check(test, local_account == 123456789, "snapshot retained exact stored account ID");
		uint32_t property = UINT32_MAX;
		CheckOk(test, Service::GetStoredScalar(4097, 3, &property), "read initialized owner's auto-login snapshot");
		Check(test, property == 1, "stored auto-login true is preserved without creating a session");
		CheckOk(test, Service::GetStoredScalar(4097, 5, &property), "owner login-order snapshot");
		Check(test, property == 1, "owner sees committed login order");
		Check(test, Service::QueryLocalAccountId(7777, &local_account) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN &&
		            local_account == 123456789, "unknown user must not receive another user's account");
		Check(test, api.event(nullptr) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT &&
		            api.name(4097, name, 1) == Service::USER_SERVICE_ERROR_BUFFER_TOO_SHORT,
		      "truthful invalid argument and short-buffer errors");
	}
	Check(test, bytes() == before_owner_calls, "owner startup/queries never save or reload profiles");
	CheckOk(test, Service::UserServiceStoreLogin(4098), "live second-user login");
	CheckOk(test, Service::UserServiceStoreLogout(4097), "live initial-user logout");
	for (auto* caller: {&first, &second, &session, &empty}) {
		ExpectEvent(test, api, caller, 0, 4098);
		ExpectEvent(test, api, caller, 1, 4097);
		NoEvent(test, api, caller);
	}
	Loader::RuntimeLinker::SetCurrentProgram(&first);
	CheckOk(test, api.initial(&initial), "initial identity survives its logout");
	Check(test, initial == 4097, "initial user does not turn into second logged-in user");
	CheckOk(test, api.list(&list), "live list after logout");
	Check(test, list.ids[0] == 4098 && list.ids[1] == -1, "subscription tracks real current users");
	int32_t number = -77;
	CheckOk(test, api.number(4098, &number), "owner's number follows session assignment");
	Check(test, number == 2, "owner snapshot does not compact slots after logout");
	Check(test, api.number(4097, &number) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN && number == 2,
	      "launch identity survives logout but no longer has a login number");
	Loader::RuntimeLinker::SetCurrentProgram(&second);
	CheckOk(test, api.terminate(), "terminate only the second client");
	Check(test, api.number(4098, &number) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED, "terminated client cannot query slot state");
	CheckOk(test, api.initialize(nullptr), "reinitialize second client against current session");
	ExpectEvent(test, api, &second, 0, 4098);
	NoEvent(test, api, &second);
	CheckOk(test, api.number(4098, &number), "reinitialized snapshot retains stable slot");
	Check(test, number == 2, "client termination never logs out a session");
	Loader::RuntimeLinker::SetCurrentProgram(&first);
	Check(test, Service::RetireUserServiceOwner(&first) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED,
	      "cannot retire running execution");
	const auto old_execution = first.application_execution;
	auto lease = old_execution->TryAcquire(Loader::GuestWorkKind::Thread);
	old_execution->RequestStop();
	Check(test, Service::RetireUserServiceOwner(&first) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED,
	      "outstanding lease prevents premature retirement");
	lease.Reset();
	first.application_threads.store(1);
	Check(test, Service::RetireUserServiceOwner(&first) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED,
	      "actual thread drain remains mandatory");
	first.application_threads.store(0);
	const auto after_session_mutations = bytes();
	Retire(test, &first);
	Check(test, api.initialize(nullptr) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED,
	      "retained retired Program never reverts to session behavior");
	Check(test, Service::QueryLocalAccountId(4097, &local_account) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED,
	      "retired account query cannot revert to session profile");
	// Deliberately reuse the SAME retained Program address with a fresh control.
	first.application_execution.reset();
	Execution(test, &first);
	Check(test, first.application_execution->Owner().token != old_execution->Owner().token,
	      "relaunch gets a non-reused lifetime token");
	Check(test, api.initialize(nullptr) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED,
	      "new token cannot inherit old initialization");
	CheckOk(test, Service::PrepareUserServiceOwner(&first), "prepare relaunch");
	CheckOk(test, api.initialize(nullptr), "initialize relaunch");
	ExpectEvent(test, api, &first, 0, 4098);
	NoEvent(test, api, &first);
	NoEvent(test, api, &second);
	Loader::RuntimeLinker::SetCurrentProgram(&first);
	Check(test, api.initial(&initial) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN && initial == -1,
	      "no foreground user means no invented initial user despite remaining login");
	CheckOk(test, Service::UserServiceStoreLogin(4097), "live login after relaunch");
	for (auto* caller: {&first, &second, &session, &empty}) { ExpectEvent(test, api, caller, 0, 4097); }
	const auto before_retirement = bytes();
	Retire(test, &first);
	Retire(test, &second);
	Retire(test, &empty);
	const auto original_bytes = bytes();
	Check(test, original_bytes == before_retirement, "retirement does not touch profiles");
	const std::string saved(reinterpret_cast<const char*>(original_bytes.data()), original_bytes.size());
	for (const char* field: {"Original One", "Original Two", "1234", "5678", "123456789", "987654321",
	                         "one@example.invalid", "two@example.invalid", "\"login_flag\": 7", "\"login_flag\": 9",
	                         "\"auto_login\": true", "\"auto_login\": false"}) {
		Check(test, saved.find(field) != std::string::npos, "original profile fields survive real login/save and owner lifecycle");
	}
	Check(test, after_session_mutations != before_owner_calls, "only real session login/logout changes store metadata");
	Check(test, Common::GetHostLockCriticalCounter()->load() == depth, "owner paths balance host critical guards");
} catch (const std::exception& error) {
	Check("RuntimeUserServiceOwners", false, std::string("unexpected exception: ") + error.what());
}

void TestRuntimeUserServiceControllerOwners() try {
	constexpr const char* test = "RuntimeUserServiceControllerOwners";
	using namespace RuntimeUserServiceOwnerFixture;
	Restore restore;
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	config.user_name = "Original Local";
	Config::Load(config);
	// No foreground during session initialization also avoids shell wake/native
	// file seeding in this isolated CPU-only fixture.
	Libs::SetNoForegroundUser(true);
	Api api;
	CheckOk(test, api.initialize(nullptr), "legacy non-store session initialization");
	Libs::Controller::Initialize();
	struct StopController { ~StopController() { Libs::Controller::Shutdown(); } } stop_controller;
	Libs::SetNoForegroundUser(false);
	Loader::Program first, second;
	Execution(test, &first);
	Execution(test, &second);
	CheckOk(test, Libs::UserService::PrepareUserServiceOwner(&first), "prepare local first");
	CheckOk(test, Libs::UserService::PrepareUserServiceOwner(&second), "prepare local second");
	for (auto* caller: {&first, &second}) {
		Loader::RuntimeLinker::SetCurrentProgram(caller);
		CheckOk(test, api.initialize(nullptr), "initialize local subscriber");
		uint64_t account = UINT64_MAX;
		CheckOk(test, Libs::UserService::QueryLocalAccountId(Config::GetUserId(), &account), "query configured local profile");
		Check(test, account == 0, "local-only profile has no invented account enrollment");
		ExpectEvent(test, api, caller, 0, Config::GetPlayerUserId(0));
		NoEvent(test, api, caller);
	}
	// Move the host keyboard from shared/player-one to player two. No physical
	// devices or extra guest PadOpen handles are created by this fixture/service.
	Libs::Controller::CycleKeyboardPlayer();
	Libs::Controller::CycleKeyboardPlayer();
	const int guest = Config::GetPlayerUserId(1);
	ExpectEvent(test, api, &first, 0, guest); // Samples + fans out to second.
	Libs::Controller::CycleKeyboardPlayer(); // Player two disconnects, three connects.
	ExpectEvent(test, api, &first, 1, guest);
	ExpectEvent(test, api, &second, 0, guest); // Must retain both observed transitions.
	ExpectEvent(test, api, &second, 1, guest);
	for (auto* caller: {&first, &second}) {
		ExpectEvent(test, api, caller, 0, Config::GetPlayerUserId(2));
		NoEvent(test, api, caller);
		char name[64] {};
		CheckOk(test, api.name(Config::GetUserId(), name, sizeof(name)), "preserve configured profile name");
		Check(test, std::strcmp(name, config.user_name.c_str()) == 0, "controller changes never replace profile");
	}
	Retire(test, &first);
	Retire(test, &second);
} catch (const std::exception& error) {
	Check("RuntimeUserServiceControllerOwners", false, std::string("unexpected exception: ") + error.what());
}
