// Dedicated process: persistent store initialization is intentionally one-shot.
void TestLocalUserStorageStartup(bool invalid_store) try {
	const char* test = invalid_store ? "LocalUserStorageInvalidStore" : "LocalUserStorageUpgrade";
	namespace Storage = Libs::LocalUserStorage;
	namespace Service = Libs::UserService;
	namespace Files = Libs::LibKernel::FileSystem;
	RuntimeUserServiceOwnerFixture::Restore restore;
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty-user-startup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path root;
		~Cleanup() { Files::Shutdown(); std::error_code error; std::filesystem::remove_all(root, error); }
	} cleanup {root};
	Files::Initialize();
	Storage::Layout layout {{root / "user/home", root / "system_data/priv/home", root / "system_data/eap/rodata/home"},
	    root / "local-user-storage"};
	for (const auto& path: layout.roots) { std::filesystem::create_directories(path); }
	std::filesystem::create_directory(layout.journal);
	Files::Mount(root / "user", "/user");
	Files::Mount(root / "system_data", "/system_data");
	Storage::Transaction guest;
	Check(test, bool(Storage::PrepareCreate(layout, 0x20010000, Storage::Lifetime::Guest, guest)), "previous process guest storage");
	Check(test, bool(Storage::Commit(guest)), "previous guest receipt");
	const auto path = root / "users.json";
	const std::string original = invalid_store ? "{truncated" :
	    R"({"version":1,"next_user_number":6,"users":[{"id":268500996,"name":"User1"}]})";
	{ std::ofstream file(path, std::ios::binary); file << original; }
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	config.guest_environment["KYTY_USER_STORE"] = path.string();
	config.guest_environment["KYTY_NP_LOCAL_ACCOUNT_HLE"] = "1";
	Config::Load(config);
	RuntimeUserServiceOwnerFixture::Api api;
	CheckOk(test, api.initialize(nullptr), "initialize local backend");
	int users[20];
	CheckOk(test, Service::UserServiceStoreGetRegisteredUsers(users), "snapshot after startup");
	for (const auto& home: layout.roots) {
		Check(test, std::filesystem::exists(home / "20010000") == invalid_store,
		      "invalid authoritative data never authorizes guest retirement");
		Check(test, std::filesystem::is_directory(home / "10010004") == !invalid_store,
		      "legacy profile provisioned only after validated record load");
	}
	{ std::ifstream file(path, std::ios::binary); const std::string text {std::istreambuf_iterator<char>(file), {}};
	  Check(test, text == original, "startup preserves authoritative record bytes"); }
	if (invalid_store) {
		int user = -77;
		Check(test, Service::CreateLocalUser("Denied", 1, false, &user) == Service::USER_SERVICE_ERROR_INTERNAL && user == -77,
		      "invalid store blocks writable admission");
	} else {
		Check(test, users[0] == 0x10010004 && users[1] == -1, "legacy identity preserved");
		CheckOk(test, Service::UserServiceStoreLogin(0x10010004), "upgraded persistent user can log in");
		Check(test, std::filesystem::is_directory(layout.roots[0] / "10010004/webkit/secure"), "upgrade includes native defaults");
	}
} catch (const std::exception& error) { Check("LocalUserStorageStartup", false, error.what()); }

namespace LocalCallbackFixture {
using Change = int (KYTY_SYSV_ABI *)(uint64_t, uint64_t);
struct State {
	Loader::Program* owner = nullptr;
	int caller_tid = -1;
	Change add = nullptr, remove = nullptr;
	State* removed_peer = nullptr;
	std::vector<RuntimeUserServiceOwnerFixture::Event> events;
	bool valid = true;
};
static uint64_t KYTY_SYSV_ABI Callback(const RuntimeUserServiceOwnerFixture::Event* event, State* state) {
	namespace Service = Libs::UserService;
	state->valid &= Loader::RuntimeLinker::GetCurrentProgram() == state->owner &&
	    Libs::LibKernel::PthreadGetOwnerProgram(Libs::LibKernel::PthreadSelfOrNull()) == state->owner &&
	    Libs::LibKernel::PthreadGetthreadid() != state->caller_tid;
	int foreground = -1;
	state->valid &= Service::UserServiceStoreGetForegroundUser(&foreground) == 0;
	if (event->type == 0) {
		state->valid &= foreground == event->id;
		state->valid &= Service::UserServiceStoreLogin(event->id) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED;
	}
	if (event->type == 0x100 || event->type == 0x101) {
		uint64_t account = UINT64_MAX;
		const int result = Service::QueryLocalAccountId(event->id, &account);
		state->valid &= event->type == 0x100 ? result == 0 && account == 0 : result == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN;
		if (event->type == 0x100) {
			char id[9];
			std::snprintf(id, sizeof(id), "%08x", event->id);
			for (const char* root: {"/user/home/", "/system_data/priv/home/", "/system_data/eap/rodata/home/"}) {
				const auto path = Libs::LibKernel::FileSystem::ResolveGuestPath((std::string(root) + id).c_str());
				state->valid &= !path.error && !path.unmounted_fallback && std::filesystem::is_directory(path.host);
			}
		}
	}
	const auto target = reinterpret_cast<uint64_t>(&Callback);
	state->valid &= state->add(target, reinterpret_cast<uint64_t>(state)) == 0;
	if (state->removed_peer) {
		state->valid &= state->remove(target, reinterpret_cast<uint64_t>(state->removed_peer)) == 0;
	}
	state->events.push_back(*event);
	return 0;
}
}

void TestLocalUserCreation() try {
	constexpr const char* test = "LocalUserCreation";
	using namespace RuntimeUserServiceOwnerFixture;
	namespace Service = Libs::UserService;
	Restore restore;
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty-local-users-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(test, std::filesystem::create_directory(root), "private profile directory");
	struct Cleanup {
		std::filesystem::path root;
		~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
	} cleanup {root};
	Libs::LibKernel::FileSystem::Initialize();
	struct StopFiles { ~StopFiles() { Libs::LibKernel::FileSystem::Shutdown(); } } stop_files;
	for (const auto& suffix: {"user/home", "system_data/priv/home", "system_data/eap/rodata/home"}) {
		std::filesystem::create_directories(root / suffix);
	}
	Libs::LibKernel::FileSystem::Mount(root / "user", "/user");
	Libs::LibKernel::FileSystem::Mount(root / "system_data", "/system_data");
	const auto path = root / "users.json";
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	config.guest_environment["KYTY_USER_STORE"] = path.string();
	Config::Load(config);
	int untouched = -77;
	Check(test, Service::CreateLocalUser("Disabled", 1, false, &untouched) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED &&
	            untouched == -77 && !std::filesystem::exists(path), "creation requires explicit local selection");
	{
		Loader::SymbolDatabase disabled;
		Libs::InitUserService_1(&disabled);
		Check(test, !disabled.FindByNid("AT6NySjQ2gY", Loader::SymbolType::Func), "native creation binding preserved when disabled");
	}
	config.guest_environment["KYTY_NP_LOCAL_ACCOUNT_HLE"] = "1";
	Config::Load(config);
	Api api;
	CheckOk(test, api.initialize(nullptr), "load initially empty store");
	using Create = int (KYTY_SYSV_ABI *)(const char*, int, bool, bool, int*);
	const auto* binding = api.symbols.FindByNid("AT6NySjQ2gY", Loader::SymbolType::Func);
	Check(test, binding && binding->vaddr, "selected local CreateUser ABI registration");
	Check(test, api.symbols.Find({"AT6NySjQ2gY", "LoginMgrServer", 1, "LoginMgrServer", 1, 1,
	            Loader::SymbolType::Func}) == binding, "exact firmware identity wins before loaded native fallback");
	const auto create = reinterpret_cast<Create>(binding->vaddr);
	Check(test, create("No caller", 1, false, false, &untouched) == static_cast<int>(0x813e0001u) && untouched == -77,
	      "guest export cannot mutate store without retained guest caller");
	// Exercise the real SysV exports on a retained, registered boot-role caller.
	// The registry and pthread intentionally retain this Program until process exit.
	auto* boot = new Loader::Program;
	boot->unique_id = 792100;
	boot->file_name = "NPXS40087/eboot.bin";
	const auto param = root / "boot.json";
	{
		constexpr char json[] = R"({"titleId":"NPXS40087","applicationCategoryType":65536})";
		Common::File file;
		Check(test, file.Create(param), "boot-role metadata fixture");
		uint32_t written = 0;
		file.Write(json, sizeof(json) - 1, &written);
		Check(test, written == sizeof(json) - 1, "write boot metadata");
	}
	Loader::RuntimeLinker::SetCurrentProgram(boot);
	Loader::SystemContentLoadParamSfo(param);
	Libs::LncUtil::Lnc::RegisterProgramLaunch(boot, param, true, boot->unique_id);
	Check(test, Loader::ConfigureBootProcessIdentity(boot), "retained boot identity");
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	(void)Common::Singleton<Loader::RuntimeLinker>::Instance(); // Worker TLS cleanup uses the loader singleton.
	using Destroy = int (KYTY_SYSV_ABI *)(int);
	using SetFlag = int (KYTY_SYSV_ABI *)(int, uint32_t, uint32_t, uint32_t*);
	const auto destroy = reinterpret_cast<Destroy>(api.symbols.FindByNid("REIRIXzv-H0", Loader::SymbolType::Func)->vaddr);
	const auto set_flag = reinterpret_cast<SetFlag>(api.symbols.FindByNid("Jha8yMoMzrg", Loader::SymbolType::Func)->vaddr);
	const auto* status_binding = api.symbols.Find({"-+h1C78SdyU", "LoginMgrServer", 1, "LoginMgrServer", 1, 1, Loader::SymbolType::Func});
	Check(test, status_binding != nullptr, "exact SetUserStatus firmware binding");
	const auto set_status = reinterpret_cast<SetFlag>(status_binding->vaddr);
	using GetStatus = int (KYTY_SYSV_ABI *)(int, uint32_t*);
	const auto* get_status_binding = api.symbols.Find({"RJX7T4sjNgI", "UserService", 1, "UserService", 1, 1, Loader::SymbolType::Func});
	Check(test, get_status_binding != nullptr, "exact GetUserStatus firmware binding");
	const auto get_status = reinterpret_cast<GetStatus>(get_status_binding->vaddr);
	using Login = int (KYTY_SYSV_ABI *)(int, int64_t);
	using Logout = int (KYTY_SYSV_ABI *)(int, int);
	using IsLoggedIn = int (KYTY_SYSV_ABI *)(int);
	auto session_export = [&](const char* nid) {
		const auto* symbol = api.symbols.Find({nid, "UserService", 1, "UserService", 1, 1, Loader::SymbolType::Func});
		Check(test, symbol != nullptr, "exact session-transition firmware binding");
		return symbol->vaddr;
	};
	const auto login = reinterpret_cast<Login>(session_export("uvVR70ZxFrQ"));
	const auto logout = reinterpret_cast<Logout>(session_export("3T9y5xDcfOk"));
	const auto is_logged_in = reinterpret_cast<IsLoggedIn>(session_export("MZxH8029+Wg"));
	using GetNumber = int (KYTY_SYSV_ABI *)(int, int32_t*);
	const auto get_number = reinterpret_cast<GetNumber>(session_export("qbwy0Ub8b3M"));
	using Terminate = int (KYTY_SYSV_ABI *)();
	const auto terminate = reinterpret_cast<Terminate>(session_export("bwFjS+bX9mA"));
	// An owner-text fixture admits precisely this callback address. Executing
	// through the real callback boundary still requires a real retained pthread.
	const auto callback_path = root / "callback.elf";
	{
		Loader::Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\177ELF", 4);
		header.e_ident[Loader::EI_CLASS] = Loader::ELFCLASS64;
		header.e_ident[Loader::EI_DATA] = Loader::ELFDATA2LSB;
		header.e_ident[Loader::EI_VERSION] = Loader::EV_CURRENT;
		header.e_ident[Loader::EI_OSABI] = Loader::ELFOSABI_FREEBSD;
		header.e_type = Loader::ET_DYNAMIC;
		header.e_machine = Loader::EM_X86_64;
		header.e_version = Loader::EV_CURRENT;
		header.e_ehsize = sizeof(header);
		header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Loader::Elf64_Phdr);
		header.e_phnum = 1;
		Loader::Elf64_Phdr segment {};
		segment.p_type = Loader::PT_LOAD;
		segment.p_flags = Loader::PF_R | Loader::PF_X;
		segment.p_vaddr = reinterpret_cast<uint64_t>(&LocalCallbackFixture::Callback);
		segment.p_filesz = segment.p_memsz = 1;
		Common::File file;
		Check(test, file.Create(callback_path), "create callback owner ELF");
		uint32_t written = 0;
		file.Write(&header, sizeof(header), &written);
		file.Write(&segment, sizeof(segment), &written);
	}
	boot->elf = std::make_unique<Loader::Elf64>();
	boot->elf->Open(callback_path);
	Loader::SymbolDatabase callback_symbols;
	Libs::JscdUser::InitJscdUser_1(&callback_symbols);
	const auto add_callback = reinterpret_cast<LocalCallbackFixture::Change>(callback_symbols.FindByNid("wuI7c7UNk0A", Loader::SymbolType::Func)->vaddr);
	const auto remove_callback = reinterpret_cast<LocalCallbackFixture::Change>(callback_symbols.FindByNid("spW--yoLQ9o", Loader::SymbolType::Func)->vaddr);
	const auto last_order = reinterpret_cast<GetStatus>(callback_symbols.FindByNid("4nUbGGBcGco", Loader::SymbolType::Func)->vaddr);
	const auto auto_login = reinterpret_cast<GetStatus>(callback_symbols.FindByNid("nqDEnj7M0QE", Loader::SymbolType::Func)->vaddr);
	LocalCallbackFixture::State callbacks {boot, Libs::LibKernel::PthreadGetthreadid(), add_callback, remove_callback};
	LocalCallbackFixture::State removed {boot, callbacks.caller_tid, add_callback, remove_callback};
	callbacks.removed_peer = &removed;
	const auto target = reinterpret_cast<uint64_t>(&LocalCallbackFixture::Callback);
	const auto userdata = reinterpret_cast<uint64_t>(&callbacks);
	CheckOk(test, add_callback(0, 0), "null registration matches native no-op");
	Check(test, remove_callback(0, 0) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT, "native null unregistration error");
	Check(test, add_callback(1, 0) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT, "callback must belong to owner executable segment");
	CheckOk(test, add_callback(target, userdata), "register real guest worker callback");
	CheckOk(test, add_callback(target, userdata), "duplicate pair registration");
	CheckOk(test, add_callback(target, reinterpret_cast<uint64_t>(&removed)), "same target with distinct userdata");
	int abi_home = -1, abi_guest = -1;
	Check(test, create("Bad output", 1, false, false, reinterpret_cast<int*>(1)) == static_cast<int>(0x813e0002u),
	      "invalid guest output rejected before creation");
	Check(test, create(reinterpret_cast<const char*>(1), 1, false, false, &untouched) == static_cast<int>(0x813e0002u) && untouched == -77,
	      "invalid guest name rejected without touching output");
	CheckOk(test, create("ABI home", 1, true, true, &abi_home), "persistent creation through actual export");
	CheckOk(test, create("ABI guest", 3, false, false, &abi_guest), "guest creation through actual export");
	uint32_t scalar[3] {0xdeadbeef, UINT32_MAX, 0xfeedface};
	CheckOk(test, auto_login(abi_home, &scalar[1]), "typed auto-login property output");
	Check(test, scalar[0] == 0xdeadbeef && scalar[1] == 0 && scalar[2] == 0xfeedface,
	      "boolean property writes exactly four bytes into dirty output");
	CheckOk(test, last_order(abi_home, &scalar[1]), "never-logged-in order");
	Check(test, scalar[1] == 0, "creation does not fabricate login order");
	Check(test, last_order(abi_home, reinterpret_cast<uint32_t*>(1)) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT,
	      "invalid property output rejected");
	uint32_t abi_flags = 0;
	CheckOk(test, set_flag(abi_home, 1, 0, &abi_flags), "complete local profile through actual flag ABI");
	Check(test, abi_flags == 0, "profile flag output written");
	Check(test, set_status(abi_home, 1, 2, reinterpret_cast<uint32_t*>(1)) == static_cast<int>(0x813e0002u),
	      "bad status output rejected before mutation");
	for (int user: {abi_home, abi_guest}) {
		CheckOk(test, set_status(user, 1, 0x80, &abi_flags), "persistent and guest status export");
		Check(test, abi_flags == 0x80, "status preserves value outside mask");
		CheckOk(test, get_status(user, &abi_flags), "query newly created user status");
		Check(test, abi_flags == 0x80, "status round trip");
		CheckOk(test, set_status(user, 0x80, 0, &abi_flags), "managed completion mask sequence");
		std::array<char, 18> passcode;
		passcode.fill('x');
		CheckOk(test, Service::GetStoredPasscode(user, passcode.data() + 1, 16), "query unset passcode into dirty buffer");
		Check(test, passcode[0] == 'x' && passcode[1] == 0 && passcode[2] == 'x' && passcode[17] == 'x',
		      "unset passcode is an empty string with intact canaries");
		Check(test, Service::GetStoredPasscode(user, reinterpret_cast<char*>(1), 16) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT,
		      "invalid passcode destination rejected");
	}
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	Check(test, login(abi_home, -1) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED, "login requires retained current guest caller");
	Loader::RuntimeLinker::SetCurrentProgram(boot);
	Check(test, login(abi_home, 7) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED && is_logged_in(abi_home) == 0,
	      "unimplemented device association never claims login");
	for (int user: {abi_home, abi_guest}) {
		Check(test, is_logged_in(user) == 0 && is_logged_in(-1) == 0, "created records are not login sessions");
		CheckOk(test, login(user, -1), "actual ShellUI login export commits session");
		Check(test, is_logged_in(user) == 1, "login predicate sees committed session");
		CheckOk(test, last_order(user, &scalar[1]), "last-login order after committed login");
		Check(test, scalar[1] == (user == abi_home ? 1u : 2u), "stored login order consumed through actual property export");
		int foreground = -1;
		CheckOk(test, Service::UserServiceStoreGetForegroundUser(&foreground), "read committed foreground user");
		Check(test, foreground == user, "foreground follows actual login");
		ExpectEvent(test, api, boot, 0, user);
		CheckOk(test, login(user, -1), "idempotent session login");
		NoEvent(test, api, boot);
		CheckOk(test, logout(user, 0), "actual logout export ends session");
		Check(test, is_logged_in(user) == 0, "logout updates predicate");
		ExpectEvent(test, api, boot, 1, user);
	}
	Check(test, callbacks.valid && callbacks.events.size() == 7 && removed.events.empty(),
	      "worker preserves owner, runs before API completion, deduplicates and honors reentrant removal");
	const int expected_types[] {0x100, 0x100, 0, 1, 0, 1, 0x101};
	const int expected_users[] {abi_home, abi_guest, abi_home, abi_home, abi_guest, abi_guest, abi_guest};
	for (size_t i = 0; i < callbacks.events.size(); ++i) {
		Check(test, callbacks.events[i].type == expected_types[i] && callbacks.events[i].id == expected_users[i],
		      "creation, session and destruction callback ordering");
	}
	CheckOk(test, remove_callback(target, userdata), "unregister callback and drain admission");
	CheckOk(test, remove_callback(target, userdata), "unregister absent pair is native no-op");
	boot->elf.reset(); // Release the fixture file before the directory-rename save-failure test.
	int32_t number = -77;
	Check(test, get_number(abi_home, &number) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN && number == -77,
	      "registered profile has no login slot and preserves output");
	int slot_second = -1, slot_third = -1;
	CheckOk(test, create("Slot second", 3, false, false, &slot_second), "create second slot user");
	CheckOk(test, create("Slot third", 3, false, false, &slot_third), "create replacement slot user");
	NoEvent(test, api, boot);
	CheckOk(test, login(abi_home, -1), "allocate first login slot");
	ExpectEvent(test, api, boot, 0, abi_home);
	CheckOk(test, login(slot_second, -1), "allocate second login slot");
	ExpectEvent(test, api, boot, 0, slot_second);
	CheckOk(test, get_number(slot_second, &number), "read assigned slot");
	Check(test, number == 2, "slot number is one-based and independent of ID bits");
	CheckOk(test, logout(abi_home, 0), "release first slot");
	ExpectEvent(test, api, boot, 1, abi_home);
	CheckOk(test, get_number(slot_second, &number), "read surviving slot");
	Check(test, number == 2, "logout does not compact another user's number");
	CheckOk(test, login(slot_third, -1), "reuse free first slot");
	ExpectEvent(test, api, boot, 0, slot_third);
	CheckOk(test, get_number(slot_third, &number), "query reused slot");
	Check(test, number == 1, "first unused native slot reused");
	Check(test, get_number(slot_third, reinterpret_cast<int32_t*>(1)) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT,
	      "invalid number output rejected");
	for (int user: {slot_second, slot_third}) {
		CheckOk(test, logout(user, 0), "retire slot test guest");
		ExpectEvent(test, api, boot, 1, user);
		NoEvent(test, api, boot); // Public polling excludes create/destroy, like native GetEvent.
	}
	CheckOk(test, destroy(abi_home), "persistent deletion through export");
	Check(test, destroy(abi_guest) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN, "guest already removed by actual logout");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	for (const int type: {-1, 7}) {
		Check(test, Service::CreateLocalUser("Invalid", type, false, &untouched) == Service::USER_SERVICE_ERROR_INVALID_ARGUMENT && untouched == -77,
		      "invalid type preserves output");
	}
	Check(test, Service::CreateLocalUser("", 1, false, &untouched) != 0 &&
	            Service::CreateLocalUser("12345678901234567", 1, false, &untouched) != 0 &&
	            Service::CreateLocalUser("Special", 5, false, &untouched) != 0 && untouched == -77,
	      "reject invalid names and unsupported special-user class");
	Loader::Program application;
	Execution(test, &application);
	CheckOk(test, Service::PrepareUserServiceOwner(&application), "app prepared before user exists");
	Loader::RuntimeLinker::SetCurrentProgram(&application);
	CheckOk(test, api.initialize(nullptr), "initialize app subscription");
	Check(test, Service::CreateLocalUser("App attempt", 1, false, &untouched) == Service::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED,
	      "application cannot mutate local session profiles");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	int home = -1, guest = -1;
	CheckOk(test, Service::CreateLocalUser("Persistent", 1, true, &home), "create persistent user");
	CheckOk(test, Service::CreateLocalUser("Guest", 3, false, &guest), "create guest user");
	Check(test, (home & 0xf0000000) == 0x10000000 && (guest & 0xe0000000) == 0x20000000, "distinct home/guest ID classes");
	int registered[20];
	CheckOk(test, Service::UserServiceStoreGetRegisteredUsers(registered), "registered home and guest slots");
	Check(test, registered[0] == home && registered[1] == -1 && registered[16] == guest && registered[17] == -1,
	      "16 home slots followed by four guest slots");
	UserList logged {{-1, -1, -1, -1}};
	CheckOk(test, api.list(&logged), "query actual logins");
	Check(test, logged.ids[0] == -1, "creation does not fabricate login");
	NoEvent(test, api, nullptr);
	uint64_t account = UINT64_MAX;
	CheckOk(test, Service::QueryLocalAccountId(home, &account), "created profile account state");
	Check(test, account == 0, "creation does not fabricate PSN enrollment");
	auto bytes = [&] {
		Common::File file(path, Common::File::Mode::Read);
		Check(test, !file.IsInvalid(), "read durable store");
		return file.ReadWholeBuffer();
	};
	auto content = [&] {
		const auto data = bytes();
		return std::string(reinterpret_cast<const char*>(data.data()), data.size());
	};
	Check(test, content().find("Persistent") != std::string::npos && content().find("Guest") == std::string::npos,
	      "guest records never persisted");
	uint32_t flags = UINT32_MAX;
	CheckOk(test, Service::GetLocalLoginFlag(home, &flags), "read incomplete property");
	Check(test, flags == 1, "incomplete creation retained");
	CheckOk(test, Service::SetLocalLoginFlag(home, 1, 0x80, &flags), "complete creation and set another flag");
	Check(test, flags == 0x80, "firmware mask expression does not discard value bits outside mask");
	uint32_t status = UINT32_MAX;
	CheckOk(test, Service::SetLocalUserStatus(home, 0, 0x42, &status), "save independent user status");
	Check(test, content().find("\"user_status\": 66") != std::string::npos, "status durably stored");
	const auto before_failure = bytes();
	// Block only atomic record replacement: storage provisioning must actually
	// succeed before this real save error, then roll back all three roots.
	const auto moved = path.string() + "-held";
	std::filesystem::rename(path, moved);
	std::filesystem::create_directory(path);
	const int failed_create = Service::CreateLocalUser("Must fail", 1, false, &untouched);
	const int failed_flag = Service::SetLocalLoginFlag(home, 0xff, 2, &flags);
	const int failed_status = Service::SetLocalUserStatus(home, 0xff, 1, &status);
	const int failed_login = Service::UserServiceStoreLogin(home);
	const int failed_destroy = Service::DestroyLocalUser(home);
	std::filesystem::remove(path);
	std::filesystem::rename(moved, path);
	Check(test, failed_create != 0 && failed_flag != 0 && failed_login != 0 && failed_destroy != 0 && untouched == -77 && flags == 0x80,
	      "failed persistence never publishes creation, flag output, login or deletion");
	Check(test, bytes() == before_failure, "previous durable profile preserved exactly");
	char failed_id[9];
	std::snprintf(failed_id, sizeof(failed_id), "%08x", home + 1);
	for (const auto* suffix: {"user/home", "system_data/priv/home", "system_data/eap/rodata/home"}) {
		Check(test, !std::filesystem::exists(root / suffix / failed_id), "failed record save rolled back provisioned storage");
	}
	Check(test, failed_status != 0 && status == 0x42, "failed status save preserves output");
	CheckOk(test, Service::GetLocalUserStatus(home, &status), "status after failed save");
	Check(test, status == 0x42, "failed status save rolled back");
	CheckOk(test, Service::GetLocalLoginFlag(home, &flags), "state after failed saves");
	Check(test, flags == 0x80, "failed flag save rolled back");
	NoEvent(test, api, nullptr);
	CheckOk(test, Service::UserServiceStoreLogin(home), "login persistent profile");
	ExpectEvent(test, api, &application, 0, home);
	char name[32] {};
	CheckOk(test, api.name(home, name, sizeof(name)), "existing app learns newly logged-in identity");
	Check(test, std::strcmp(name, "Persistent") == 0, "new identity retains actual name");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	CheckOk(test, Service::UserServiceStoreLogin(guest), "login guest");
	CheckOk(test, Service::UserServiceStoreLogout(guest), "guest session ends");
	Check(test, Service::QueryLocalAccountId(guest, &account) == Service::USER_SERVICE_ERROR_NOT_LOGGED_IN,
	      "guest record removed on logout");
	ExpectEvent(test, api, &application, 0, guest);
	ExpectEvent(test, api, &application, 1, guest);
	CheckOk(test, api.name(guest, name, sizeof(name)), "retained application snapshot survives guest removal");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	int replacement = -1;
	CheckOk(test, Service::CreateLocalUser("Replacement", 3, false, &replacement), "replace departed guest");
	Check(test, replacement != guest, "guest IDs not reused within session");
	for (int i = 1; i < 4; ++i) { int id = -1; CheckOk(test, Service::CreateLocalUser("Guest slot", 3, false, &id), "fill guest slots"); }
	Check(test, Service::CreateLocalUser("Overflow", 3, false, &untouched) == static_cast<int>(0x80960102u) && untouched == -77,
	      "guest capacity enforced independently");
	int second_home = -1;
	CheckOk(test, Service::CreateLocalUser("Second", 1, false, &second_home), "home capacity independent of guests");
	Check(test, second_home == home + 1, "failed creation did not consume persistent ID");
	CheckOk(test, Service::DestroyLocalUser(home), "delete logged-in persistent user");
	ExpectEvent(test, api, &application, 1, home);
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	Check(test, content().find("Persistent") == std::string::npos && content().find("Guest") == std::string::npos &&
	            content().find("Second") != std::string::npos, "only surviving persistent profile saved");
	for (int i = 1; i < 16; ++i) { int id = -1; CheckOk(test, Service::CreateLocalUser("Home slot", 1, false, &id), "fill persistent slots"); }
	Check(test, Service::CreateLocalUser("Overflow", 1, false, &untouched) == static_cast<int>(0x80960102u), "persistent capacity enforced");
	Retire(test, &application);
	Loader::RuntimeLinker::SetCurrentProgram(boot);
	Check(test, callbacks.events.size() == 7, "unregistered callback receives no subsequent sessions");
	CheckOk(test, terminate(), "actual no-argument termination stops and joins callback worker");
	Check(test, Service::ReleaseNativeUserServiceBackend(boot), "owner release after client termination");
	boot->boot_credential_lifetime.reset();
	Check(test, create("Retired", 1, false, false, &untouched) == static_cast<int>(0x813e0001u) && untouched == -77,
	      "retired boot lifetime cannot create users");
	Check(test, login(second_home, -1) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED, "retired boot cannot log in");
	Check(test, add_callback(target, userdata) == Service::USER_SERVICE_ERROR_NOT_INITIALIZED, "retired caller cannot register callbacks");
	std::printf("[OK] %s\n", test);
} catch (const std::exception& error) { Check("LocalUserCreation", false, error.what()); }
