// Include inside VirtualMemoryAllocationTests' fixture namespace. At top level
// include loader/runtimeProcessIdentity.h and declare:
// namespace Libs::ShellCoreFloor { void InitShellCoreFloor_1(Loader::SymbolDatabase*); }
// Dispatch TestRuntimeProcessIdentity() in a dedicated process after InitSubsystems.
// Only synthetic metadata and host AF_UNIX sockets; no guest execution or GPU.
void TestRuntimeProcessIdentity() try {
	constexpr const char* test = "RuntimeProcessIdentity";
	namespace Net = Libs::Network::Net;
	namespace Fs = Libs::LibKernel::FileSystem;
	using Loader::RuntimeLinker;
	struct Restore {
		Loader::Program* previous = RuntimeLinker::GetCurrentProgram();
		~Restore() { RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	// Registry records deliberately retain their owners for the session.
	auto* boot = new Loader::Program;
	auto* first = new Loader::Program;
	auto* second = new Loader::Program;
	auto* missing = new Loader::Program;
	auto* missing_attribute = new Loader::Program;
	auto* zero_attribute = new Loader::Program;
	auto* unknown = new Loader::Program;
	Loader::Program private_module;
	boot->unique_id = 790100;
	boot->file_name = "NPXS40087/eboot.bin";
	first->unique_id = 790101;
	second->unique_id = 790102;
	missing->unique_id = 790103;
	missing_attribute->unique_id = 790104;
	zero_attribute->unique_id = 790105;
	first->application_owner = first;
	second->application_owner = second;
	missing->application_owner = missing;
	missing_attribute->application_owner = missing_attribute;
	zero_attribute->application_owner = zero_attribute;
	unknown->application_owner = unknown;
	private_module.application_owner = first;
	first->authority_metadata.emplace();
	first->authority_metadata->authority_id = UINT64_C(0x480000000000000f);
	second->authority_metadata.emplace();
	second->authority_metadata->authority_id = UINT64_C(0x3800000000000042);
	// Staged boot ELF has no SELF header; only its registered ShellUI role may
	// retain the session authority. Runtime owners never receive that fallback.
	first->process_abi = Loader::ProcessAbi::Prospero;
	first->compiled_sdk_version = 0x09000000;
	first->prospero_compiled_sdk_version = 0x09010000;
	private_module.compiled_sdk_version = 0xdeadbeef;

	const auto root = std::filesystem::temp_directory_path() /
	    ("kyid_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000));
	Check(test, std::filesystem::create_directories(root), "create fixture directory");
	struct RemoveFiles {
		std::filesystem::path root;
		~RemoveFiles() { std::error_code error; std::filesystem::remove_all(root, error); }
	} remove_files {root};
	auto write = [&](const char* name, const char* text) {
		Common::File file;
		Check(test, file.Create(root / name), "create metadata");
		uint32_t written = 0;
		file.Write(text, static_cast<uint32_t>(std::strlen(text)), &written);
		Check(test, written == std::strlen(text), "write metadata");
	};
	write("boot.json", R"({"titleId":"NPXS40087","applicationCategoryType":65536})");
	write("first.json", R"({"titleId":"TEST00001","applicationCategoryType":17,"attribute3":512})");
	write("second.json", R"({"titleId":"TEST00002","applicationCategoryType":34,"attribute3":2147483652})");
	write("missing-attribute.json", R"({"titleId":"TEST00003","applicationCategoryType":51,"attribute":4294967295})");
	write("zero-attribute.json", R"({"titleId":"TEST00004","applicationCategoryType":68,"attribute3":0})");
	Loader::SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	Libs::ShellCoreFloor::InitShellCoreFloor_1(&symbols);
	Libs::LibSysCore::InitSysCore_1(&symbols);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr && symbol->vaddr != 0, "guest query binding");
		return symbol->vaddr;
	};
	using Category = int (KYTY_SYSV_ABI *)(uint64_t, uint32_t*);
	using Info = int (KYTY_SYSV_ABI *)(uint64_t, void*);
	using Auth = int (KYTY_SYSV_ABI *)(int, void*);
	using AppId = int32_t (KYTY_SYSV_ABI *)(int32_t, int32_t*);
	const auto category = reinterpret_cast<Category>(resolve("+xy9ORMbd8U"));
	const auto info = reinterpret_cast<Info>(resolve("G-MYv5erXaU"));
	const auto auth = reinterpret_cast<Auth>(resolve("igMefp4SAv0"));
	const auto app_id = reinterpret_cast<AppId>(resolve("e-ODUaoHXiY"));
	const auto get_pid = reinterpret_cast<int (KYTY_SYSV_ABI *)()>(resolve("HoLVWNanBBc"));
	auto check_info = [&](int pid, uint32_t expected_attribute3, uint32_t expected_category) {
		std::array<uint8_t, 90> actual;
		actual.fill(0xa5);
		std::array<uint8_t, 90> expected {};
		expected.front() = expected.back() = 0xa5;
		std::memcpy(expected.data() + 1 + 0x4c, &expected_attribute3, sizeof(expected_attribute3));
		std::memcpy(expected.data() + 1 + 0x50, &expected_category, sizeof(expected_category));
		CheckOk(test, info(pid, actual.data() + 1), "metadata-backed app-info");
		Check(test, actual == expected, "app-info preserves exact ATTRIBUTE3/category and writes exactly 88 bytes");
	};
	// Exercise the normal --game path before any immutable ShellUI identity is
	// registered. Neither an explicit null TLS owner nor a direct-game Program
	// may derive ATTRIBUTE3 from authority, ATTRIBUTE or another owner's content.
	Loader::Program direct;
	direct.authority_metadata.emplace();
	direct.authority_metadata->authority_id = UINT64_C(0x480000000000000f);
	const std::array<std::optional<uint32_t>, 5> direct_attributes {
		std::nullopt, uint32_t {0}, uint32_t {0x200}, uint32_t {0x204}, uint32_t {0x80000004}};
	for (const auto declared: direct_attributes) {
		std::string json = R"({"titleId":"PPSA99999","applicationCategoryType":65536,"attribute":4294967295)";
		if (declared) { json += ",\"attribute3\":" + std::to_string(*declared); }
		json += "}";
		write("direct.json", json.c_str());
		RuntimeLinker::SetCurrentProgram(&direct);
		Loader::SystemContentLoadParamSfo(root / "direct.json");
		for (auto* caller: {static_cast<Loader::Program*>(nullptr), &direct}) {
			RuntimeLinker::SetCurrentProgram(caller);
			Loader::GuestProcessIdentity snapshot;
			Check(test, Loader::TryGetGuestProcessIdentity(0, &snapshot) && snapshot.pid == 100 &&
			            snapshot.attribute3 == declared, "direct/null-owner ATTRIBUTE3 presence and exact value");
			check_info(0, declared.value_or(0), 65536);
			check_info(100, declared.value_or(0), 65536);
		}
	}
	Check(test, info(0, nullptr) == Libs::LibKernel::KERNEL_ERROR_EINVAL, "null app-info output");
	RuntimeLinker::SetCurrentProgram(boot);
	Loader::SystemContentLoadParamSfo(root / "boot.json");
	Libs::LncUtil::Lnc::RegisterProgramLaunch(boot, root / "boot.json", true, 790100);
	int32_t boot_app_id = -1;
	Check(test, Libs::LncUtil::Lnc::TryGetCurrentAppId(&boot_app_id), "boot launch app ID");
	Loader::Program unrelated_boot;
	Check(test, !Loader::ConfigureBootProcessIdentity(&unrelated_boot), "unregistered boot program claimed ShellUI identity");
	Check(test, Loader::ConfigureBootProcessIdentity(boot), "configure immutable boot identity");
	Check(test, Loader::SystemContentLoadForProgram(first, root / "first.json") &&
	            Loader::SystemContentLoadForProgram(second, root / "second.json") &&
	            Loader::SystemContentLoadForProgram(missing_attribute, root / "missing-attribute.json") &&
	            Loader::SystemContentLoadForProgram(zero_attribute, root / "zero-attribute.json"), "load application snapshots");
	const int first_pid = Loader::RegisterRuntimeProcessIdentity(first, 790101);
	const int second_pid = Loader::RegisterRuntimeProcessIdentity(second, 790102);
	const int missing_pid = Loader::RegisterRuntimeProcessIdentity(missing, 790103);
	const int missing_attribute_pid = Loader::RegisterRuntimeProcessIdentity(missing_attribute, 790104);
	const int zero_attribute_pid = Loader::RegisterRuntimeProcessIdentity(zero_attribute, 790105);
	Check(test, first_pid > 100 && second_pid > 100 && first_pid != second_pid && missing_pid > second_pid,
	      "distinct explicit guest PIDs");
	Check(test, missing_attribute_pid > missing_pid && zero_attribute_pid > missing_attribute_pid,
	      "attribute-only fixture owners have distinct PIDs");
	Check(test, Loader::RegisterRuntimeProcessIdentity(first, 790101) == first_pid &&
	            Loader::RegisterRuntimeProcessIdentity(first, 790102) == -1 &&
	            Loader::RegisterRuntimeProcessIdentity(boot, 790104) == -1, "registration consistency");
	// Replacing the source content must not rewrite published process metadata.
	Check(test, Loader::SystemContentLoadForProgram(first, root / "second.json"), "replace source snapshot");
	// The configured boot PID must likewise keep its original absent ATTRIBUTE3
	// rather than adopting a later singleton's category/attribute word.
	Loader::SystemContentLoadParamSfo(root / "direct.json");
	// A valid category with missing ATTRIBUTE3 is still successful, but its
	// snapshot retains absence and the ABI record contains zero (no permission).
	for (auto* caller: {boot, first, second, missing_attribute, zero_attribute, &private_module}) {
		RuntimeLinker::SetCurrentProgram(caller);
		check_info(100, 0, 65536);
		check_info(first_pid, 0x200, 17);
		check_info(second_pid, 0x80000004, 34);
		check_info(missing_attribute_pid, 0, 51);
		check_info(zero_attribute_pid, 0, 68);
		const int self_pid = caller == boot ? 100 : caller == second ? second_pid :
		    caller == missing_attribute ? missing_attribute_pid : caller == zero_attribute ? zero_attribute_pid : first_pid;
		Loader::GuestProcessIdentity snapshot;
		Check(test, Loader::TryGetGuestProcessIdentity(0, &snapshot) && snapshot.pid == self_pid,
		      "attribute query resolves calling application owner");
		const std::optional<uint32_t> declared = self_pid == first_pid ? std::optional<uint32_t> {0x200} :
		    self_pid == second_pid ? std::optional<uint32_t> {0x80000004} :
		    self_pid == zero_attribute_pid ? std::optional<uint32_t> {0} : std::nullopt;
		Check(test, snapshot.attribute3 == declared, "runtime snapshot preserves ATTRIBUTE3 absence/zero/all bits");
		Check(test, snapshot.category.has_value(), "attribute fixture has a process category");
		check_info(0, declared.value_or(0), snapshot.category.value_or(UINT32_MAX));
	}
	Loader::Program shell_core;
	shell_core.unique_id = 790110;
	shell_core.file_name = "SceShellCore.elf";
	Libs::LncUtil::Lnc::RegisterProgramLaunch(&shell_core, {}, false, 790110);
	int32_t shell_core_app_id = -1;
	Check(test, Libs::LncUtil::Lnc::TryGetProgramAppId(&shell_core, &shell_core_app_id) &&
	            shell_core_app_id != boot_app_id, "distinct ShellCore LNC registration");
	RuntimeLinker::SetCurrentProgram(&shell_core);
	int32_t contextual_id = -1;
	CheckOk(test, app_id(0, &contextual_id), "firmware self retains contextual identity");
	Check(test, contextual_id == shell_core_app_id, "ShellCore self query became ShellUI");
	CheckOk(test, app_id(100, &contextual_id), "explicit boot PID from ShellCore");
	Check(test, contextual_id == boot_app_id, "explicit boot PID did not select ShellUI");
	const int core_pid = Loader::RegisterCohostProcessIdentity(&shell_core, shell_core_app_id);
	Check(test, core_pid > 100 && core_pid != first_pid && core_pid != second_pid,
	      "cohost must have a distinct retained PID");
	Check(test, get_pid() == core_pid, "cohost TLS must select its own PID");
	Loader::GuestProcessIdentity cohost_identity;
	Check(test, Loader::TryGetGuestProcessIdentity(core_pid, &cohost_identity) &&
	            !cohost_identity.authority && cohost_identity.capabilities == 0 &&
	            !cohost_identity.category && !cohost_identity.attribute3,
	      "cohost cannot inherit singleton ShellUI metadata/credentials");
	std::array<uint64_t, 17> denied_auth;
	denied_auth.fill(0xa5);
	const auto denied_before = denied_auth;
	Check(test, !Loader::TryGetSourceBoundProcessAuthInfo(core_pid, &denied_auth) && denied_auth == denied_before &&
	            !Loader::TryGetSourceBoundProcessAuthInfo(100, &denied_auth) &&
	            !Loader::AuthorizeSourceBoundAppDbRegistration(core_pid, "SceAppDbIpcmsg") &&
	            !Loader::AuthorizeSourceBoundAppDbRegistration(100, "SceAppDbIpcmsg"),
	      "unverified cohost and compatibility PID100 must fail source-bound policy");
	Check(test, Loader::DeactivateRuntimeProcessIdentity(&shell_core) && get_pid() == -1 &&
	            !Loader::IsGuestProcessIdentityActive(core_pid), "retired cohost must not fall back to PID100");
	contextual_id = -123;
	Check(test, !Loader::TryGetGuestProcessAppId(0, &contextual_id) && contextual_id == -123,
	      "retired cohost must not query legacy LNC self identity");
	RuntimeLinker::SetCurrentProgram(&unrelated_boot);
	contextual_id = -123;
	Check(test, app_id(0, &contextual_id) == Libs::LibKernel::KERNEL_ERROR_ESRCH && contextual_id == -123,
	      "unregistered firmware self borrowed configured ShellUI identity");
	for (auto* caller: {boot, first, second, &private_module}) {
		RuntimeLinker::SetCurrentProgram(caller);
		const int expected_pid = caller == boot ? 100 : caller == second ? second_pid : first_pid;
		Check(test, get_pid() == expected_pid, "guest getpid follows application owner");
		for (const auto& expected: {std::array<int, 3> {100, 65536, boot_app_id},
		                           std::array<int, 3> {first_pid, 17, 790101},
		                           std::array<int, 3> {second_pid, 34, 790102}}) {
			uint32_t actual_category = 0;
			CheckOk(test, category(expected[0], &actual_category), "explicit PID category");
			Check(test, actual_category == static_cast<uint32_t>(expected[1]), "category belongs to target PID");
			std::array<uint8_t, 90> record;
			record.fill(0xa5);
			CheckOk(test, info(expected[0], record.data() + 1), "explicit PID app-info");
			uint32_t info_category = 0;
			std::memcpy(&info_category, record.data() + 1 + 0x50, 4);
			Check(test, info_category == actual_category && record.front() == 0xa5 && record.back() == 0xa5,
			      "app-info category and 88-byte output boundary");
			std::array<int32_t, 3> guarded {-123, -456, -789};
			CheckOk(test, app_id(expected[0], &guarded[1]), "explicit PID app ID");
			Check(test, guarded[0] == -123 && guarded[1] == expected[2] && guarded[2] == -789,
			      "app ID must be a four-byte target-specific store");
			std::array<uint64_t, 19> authority;
			authority.fill(UINT64_C(0xa5a5a5a5a5a5a5a5));
			CheckOk(test, auth(expected[0], authority.data() + 1), "explicit PID authority");
			Check(test, authority[1] == (expected[0] == second_pid ? second->authority_metadata->authority_id :
			            first->authority_metadata->authority_id) &&
			            authority[2] == (expected[0] == 100 ? uint64_t {1} << 62 : 0) &&
			            authority.front() == UINT64_C(0xa5a5a5a5a5a5a5a5) && authority.back() == authority.front() &&
			            std::all_of(authority.begin() + 3, authority.end() - 1, [](auto word) { return word == 0; }),
			      "runtime apps must not inherit ShellUI capability or corrupt auth output");
		}
	}
	RuntimeLinker::SetCurrentProgram(&private_module);
	Loader::GuestProcessIdentity identity;
	Check(test, Loader::TryGetGuestProcessIdentity(0, &identity) && identity.pid == first_pid &&
	            identity.category == 17 && identity.attribute3 == 0x200 &&
	            identity.abi == Loader::ProcessAbi::Prospero && identity.compiled_sdk_version == 0x09000000 &&
	            identity.prospero_compiled_sdk_version == 0x09010000,
	      "private modules inherit executable process metadata");
	uint32_t self_category = 0;
	int32_t self_app_id = -1;
	Check(test, category(0, &self_category) == 0 && self_category == 17 &&
	            app_id(0, &self_app_id) == 0 && self_app_id == 790101, "PID zero resolves private-module application owner");
	for (int pid: {-1, 1, INT32_MAX, missing_pid}) {
		uint32_t value = 0xa5a5a5a5;
		Check(test, category(pid, &value) == Libs::LibKernel::KERNEL_ERROR_ESRCH && value == 0xa5a5a5a5,
		      "missing category preserves output");
		std::array<uint8_t, 88> app_info;
		app_info.fill(0xa5);
		const auto saved_info = app_info;
		Check(test, info(pid, app_info.data()) == Libs::LibKernel::KERNEL_ERROR_ESRCH && app_info == saved_info,
		      "missing app-info preserves entire output");
		std::array<uint64_t, 17> words;
		words.fill(UINT64_C(0xa5a5a5a5a5a5a5a5));
		const auto before = words;
		Check(test, auth(pid, words.data()) == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ESRCH && words == before,
		      "missing authority fails without ShellUI fallback");
	}
	RuntimeLinker::SetCurrentProgram(unknown);
	int32_t absent = -123;
	Check(test, Loader::GetCurrentGuestProcessId() == -1 &&
	            app_id(0, &absent) == Libs::LibKernel::KERNEL_ERROR_ESRCH && absent == -123,
	      "unregistered runtime self cannot borrow boot identity");
	std::array<uint8_t, 90> unknown_info;
	unknown_info.fill(0xa5);
	const auto unknown_before = unknown_info;
	Check(test, info(0, unknown_info.data() + 1) == Libs::LibKernel::KERNEL_ERROR_ESRCH && unknown_info == unknown_before,
	      "unregistered runtime app-info cannot borrow boot ATTRIBUTE3 or modify output");

	// Endpoint ownership is captured at socket creation. Deliberately connect
	// and accept from different application TLS contexts.
#if defined(_WIN32)
	const auto host_pid = GetCurrentProcessId();
#else
	const auto host_pid = ::getpid();
#endif
	auto private_paths = [&] {
		std::vector<std::filesystem::path::string_type> paths;
		const auto prefix = std::filesystem::path("kyipc_" + std::to_string(host_pid) + "_").native();
		for (const auto& entry: std::filesystem::directory_iterator(std::filesystem::temp_directory_path())) {
			// Other temporary files may have names outside the active ANSI code
			// page. Compare native filenames without converting unrelated paths.
			const auto name = entry.path().filename().native();
			if (name.starts_with(prefix)) { paths.push_back(name); }
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	};
	const auto initial_private_paths = private_paths();
	RuntimeLinker::SetCurrentProgram(boot);
	Fs::Initialize();
	Fs::Mount(root, "/identity_test");
	auto address = [](const char* path) {
		std::array<uint8_t, 106> result {};
		result[0] = static_cast<uint8_t>(std::strlen(path) + 3);
		result[1] = 1;
		std::memcpy(result.data() + 2, path, std::strlen(path) + 1);
		return result;
	};
	const auto server_address = address("/identity_test/server");
	const int server = Net::Socket(1, 1, 0);
	Check(test, server >= 0, "create boot listener");
	RuntimeLinker::SetCurrentProgram(first);
	const int client_a = Net::Socket(1, 1, 0);
	Check(test, client_a >= 0, "create first client");
	Check(test, Net::Connect(client_a, server_address.data(), server_address.size()) == -1,
	      "initial connect before bind must fail");
	Check(test, private_paths().size() == initial_private_paths.size() + 1, "failed connect retains one retryable private endpoint");
	RuntimeLinker::SetCurrentProgram(boot);
	CheckOk(test, Net::Bind(server, server_address.data(), server_address.size()), "bind boot listener");
	CheckOk(test, Net::Listen(server, 8), "listen");
	RuntimeLinker::SetCurrentProgram(second);
	CheckOk(test, Net::Connect(client_a, server_address.data(), server_address.size()), "retry first endpoint under second TLS");
	Check(test, private_paths().size() == initial_private_paths.size() + 1, "retry must not allocate another private endpoint");
	const int client_b = Net::Socket(1, 1, 0);
	const auto named_address = address("/identity_test/client_b");
	CheckOk(test, Net::Bind(client_b, named_address.data(), named_address.size()), "explicit client bind");
	RuntimeLinker::SetCurrentProgram(first);
	CheckOk(test, Net::Connect(client_b, server_address.data(), server_address.size()), "connect second endpoint under first TLS");
	const int accepted_a = Net::Accept(server, nullptr, nullptr);
	RuntimeLinker::SetCurrentProgram(second);
	const int accepted_b = Net::Accept(server, nullptr, nullptr);
	Check(test, accepted_a >= 0 && accepted_b >= 0, "accept two owners");
	auto peer = [&](int fd, int expected) {
		int result = -123;
		CheckOk(test, Net::GetUnixPeerProcessId(fd, &result), "Unix peer identity");
		Check(test, result == expected, "Unix identity must come from connecting endpoint");
	};
	peer(accepted_a, first_pid);
	peer(accepted_b, second_pid);
	peer(client_a, 100);
	peer(client_b, 100);
	std::array<uint8_t, 106> name {};
	uint32_t name_size = name.size();
	CheckOk(test, Net::Getsockname(client_a, name.data(), &name_size), "anonymous guest name");
	Check(test, name_size == 16 && name[1] == 1 && name[2] == 0, "private host endpoint must not leak into guest sockaddr");
	CheckOk(test, Net::SocketClose(client_a), "close connecting endpoint");
	peer(accepted_a, first_pid); // retained accepted identity is independent of fd reuse
	RuntimeLinker::SetCurrentProgram(boot);
	const int boot_client = Net::Socket(1, 1, 0);
	CheckOk(test, Net::Connect(boot_client, server_address.data(), server_address.size()), "boot connect");
	RuntimeLinker::SetCurrentProgram(first);
	const int accepted_boot = Net::Accept(server, nullptr, nullptr);
	peer(accepted_boot, 100);
	RuntimeLinker::SetCurrentProgram(unknown);
	const int unknown_client = Net::Socket(1, 1, 0);
	CheckOk(test, Net::Connect(unknown_client, server_address.data(), server_address.size()), "unknown owner connection");
	const int accepted_unknown = Net::Accept(server, nullptr, nullptr);
	int unknown_peer = -123;
	Check(test, Net::GetUnixPeerProcessId(accepted_unknown, &unknown_peer) == -1 && unknown_peer == -123 &&
	            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ESRCH,
	      "unregistered endpoint never defaults to PID 100");
	// A queued connection whose descriptor has disappeared is unmatched, not
	// ShellUI. Accept can still succeed, but peer identity must fail closed.
	RuntimeLinker::SetCurrentProgram(second);
	const int abandoned = Net::Socket(1, 1, 0);
	CheckOk(test, Net::Connect(abandoned, server_address.data(), server_address.size()), "queued connection");
	CheckOk(test, Net::SocketClose(abandoned), "close before accept");
	const int accepted_abandoned = Net::Accept(server, nullptr, nullptr);
	Check(test, accepted_abandoned >= 0, "accept closed queued endpoint");
	unknown_peer = -123;
	Check(test, Net::GetUnixPeerProcessId(accepted_abandoned, &unknown_peer) == -1 && unknown_peer == -123,
	      "unmatched queued endpoint preserves output");
	first->application_threads.store(1);
	Check(test, !Loader::DeactivateRuntimeProcessIdentity(first), "live threads prevent retirement");
	first->application_threads.store(0);
	Check(test, Loader::DeactivateRuntimeProcessIdentity(first), "retire after quiescence");
	unknown_peer = -123;
	Check(test, Net::GetUnixPeerProcessId(accepted_a, &unknown_peer) == -1 && unknown_peer == -123,
	      "retired identity cannot authenticate a retained connection");
	absent = -123;
	Check(test, app_id(first_pid, &absent) == Libs::LibKernel::KERNEL_ERROR_ESRCH && absent == -123 &&
	            Loader::RegisterRuntimeProcessIdentity(first, 790101) == -1, "retired PID is not revived");
	Check(test, info(first_pid, unknown_info.data() + 1) == Libs::LibKernel::KERNEL_ERROR_ESRCH && unknown_info == unknown_before,
	      "retired app-info cannot publish a retained ATTRIBUTE3 snapshot");
	for (int fd: {client_b, accepted_a, accepted_b, boot_client, accepted_boot, unknown_client,
	              accepted_unknown, accepted_abandoned, server}) {
		CheckOk(test, Net::SocketClose(fd), "close identity fixture sockets");
	}
	Check(test, private_paths() == initial_private_paths, "final transport release removes only owned private paths");
	Check(test, Loader::DeactivateRuntimeProcessIdentity(second) && Loader::DeactivateRuntimeProcessIdentity(missing) &&
	            Loader::DeactivateRuntimeProcessIdentity(missing_attribute) && Loader::DeactivateRuntimeProcessIdentity(zero_attribute),
	      "retire fixture owners");
	RuntimeLinker::SetCurrentProgram(boot);
	Fs::Shutdown();
	Check(test, Common::GetHostLockCriticalCounter()->load() == depth, "identity paths balance host critical guards");
} catch (const std::exception& error) {
	Check("RuntimeProcessIdentity", false, std::string("unexpected exception: ") + error.what());
}

// Retained-input production-provider/registry test, still no guest execution.
void TestSourceBoundCoreIdentity(bool np_transport = false) {
	constexpr const char* test = "SourceBoundCoreIdentity";
	const char* path = std::getenv("KYTY_TEST_ORIGINAL_CORE");
	const char* record_path = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
	Check(test, path && record_path, "explicit local original Core and raw record required");
	if (!path || !record_path) { return; }
	auto core = std::make_unique<Loader::Program>();
	core->unique_id = 790900;
	core->file_name = path;
	core->elf = std::make_unique<Loader::Elf64>();
	core->elf->Open(path);
	std::vector<uint8_t> source;
	Check(test, core->elf->SnapshotPlainFile(35337516, &source), "same-open original snapshot");
	if (source.empty()) { return; }
	std::array<uint8_t, 136> record {};
	Common::File file;
	Check(test, file.Open(record_path, Common::File::Mode::Read) && file.Size() == record.size(), "raw record extent");
	uint32_t read = 0; file.Read(record.data(), record.size(), &read);
	Check(test, read == record.size(), "raw record read");
	const auto* ph = core->elf->GetPhdr();
	const auto count = core->elf->GetEhdr()->e_phnum;
	for (size_t i = 0; i < count; ++i) {
		if (ph[i].p_type == Loader::PT_LOAD) { core->base_size = std::max(core->base_size, ph[i].p_vaddr + ph[i].p_memsz); }
	}
	std::vector<uint8_t> memory(core->base_size, 0);
	core->base_vaddr = reinterpret_cast<uint64_t>(memory.data());
	std::vector<Loader::BootMappedSegment> mapped;
	for (size_t i = 0; i < count; ++i) {
		if (ph[i].p_type != Loader::PT_LOAD) { continue; }
		std::memcpy(memory.data() + ph[i].p_vaddr, source.data() + ph[i].p_offset, ph[i].p_filesz);
		mapped.push_back({ph[i].p_vaddr, {memory.data() + ph[i].p_vaddr, static_cast<size_t>(ph[i].p_memsz)}});
	}
	record[16] ^= 1;
	Check(test, !Loader::AdmitOriginalCoreBootCredentials(core.get(), source, mapped, record), "record tamper rejected by production hasher");
	record[16] ^= 1;
	memory[0] ^= 1;
	Check(test, !Loader::AdmitOriginalCoreBootCredentials(core.get(), source, mapped, record), "actual mapping tamper rejected");
	memory[0] ^= 1;
	Check(test, Loader::AdmitOriginalCoreBootCredentials(core.get(), source, mapped, record), "production original admission");
	const auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(core.get());
	std::array<uint64_t, 17> auth {}; auth.fill(0xa5);
	Check(test, !Loader::TryGetSourceBoundProcessAuthInfo(0, &auth), "unregistered proof does not publish PID100 credentials");
	Libs::LncUtil::Lnc::RegisterProgramLaunch(core.get(), {}, false, 790900);
	int32_t app_id = 0;
	Check(test, Libs::LncUtil::Lnc::TryGetProgramAppId(core.get(), &app_id), "cohost LNC identity");
	const int pid = Loader::RegisterCohostProcessIdentity(core.get(), app_id);
	Check(test, pid > 100 && Loader::GetCurrentGuestProcessId() == pid, "retained actual cohost identity");
	Check(test, Loader::TryGetSourceBoundProcessAuthInfo(pid, &auth) &&
	            std::memcmp(auth.data(), record.data(), record.size()) == 0, "full exact production credential query");
	Check(test, Loader::AuthorizeSourceBoundAppDbRegistration(pid, "SceAppDbIpcmsg") &&
	            !Loader::AuthorizeSourceBoundAppDbRegistration(100, "SceAppDbIpcmsg"), "actual Core gate versus boot compatibility");
	const auto saved = auth;
	Check(test, !Loader::TryGetGuestProcessAuthInfo(100, &auth) && auth == saved,
	      "native Core request cannot authorize compatibility ShellUI peer");
	if (np_transport) {
		namespace N = Libs::NpNativeTransport;
		Loader::Program unrelated;
		Check(test, !Loader::InstallSourceBoundNpTransport(nullptr) && !Loader::InstallSourceBoundNpTransport(&unrelated),
		      "NP transport refuses missing/unadmitted owners");
		Check(test, Loader::InstallSourceBoundNpTransport(core.get()) && N::Installed(), "exact retained Core installs NP hooks");
		Check(test, !Loader::InstallSourceBoundNpTransport(core.get()), "duplicate NP runtime rejected");
		std::array<uint64_t, 6> unowned_storage {}; unowned_storage.fill(0xa5);
		void* server = reinterpret_cast<void*>(0x1234);
		N::ServerConfig config {0x2000, 0x4000, reinterpret_cast<void*>(1), 1, {}, 0, 0};
		std::memcpy(config.name, "SceNpMgrIpc", sizeof("SceNpMgrIpc"));
		// OS/host thread identity alone must never authorize native objects.
		if (!Libs::LibKernel::PthreadSelfOrNull()) {
			Check(test, N::CreateServer(&server, &config, nullptr, unowned_storage.data()) != 0 && server == reinterpret_cast<void*>(0x1234),
			      "NP hooks reject host-only thread without publishing output");
			Libs::LibKernel::Initialize();
			Libs::LibKernel::PthreadInitSelfForMainThread();
		}
		Libs::NpMemoryHeap::Heap heap {};
		Check(test, Libs::NpMemoryHeap::Init(&heap, 0x4000, "NpHookStorage") == 0, "real flexible heap for published native object");
		auto* storage = static_cast<uint64_t*>(Libs::NpMemoryHeap::Malloc(&heap.handle, 0x30));
		Check(test, storage != nullptr, "native storage allocation");
		std::fill_n(storage, 6, uint64_t(0xa5));
		struct Cleanup {
			Libs::NpMemoryHeap::Heap& heap;
			uint64_t* storage;
			void*& server;
			~Cleanup() {
				if (server == storage) {
					using Destroy = int (KYTY_SYSV_ABI *)(void*);
					if (reinterpret_cast<Destroy>((*static_cast<void***>(server))[0])(server) != 0) { std::abort(); }
				}
				if (!Loader::RetireSourceBoundNpTransport()) { std::abort(); }
				Libs::NpMemoryHeap::Free(&heap.handle, storage);
				Libs::NpMemoryHeap::Destroy(&heap);
				if (Libs::NpMemoryHeap::RetryAllocationRetirements() != 0) { std::abort(); }
			}
		} cleanup {heap, storage, server};
		Loader::RuntimeLinker::SetCurrentProgram(&unrelated);
		Check(test, N::CreateServer(&server, &config, nullptr, storage) != 0 && storage[0] == 0xa5,
		      "unknown caller cannot register reserved NP server");
		Loader::RuntimeLinker::SetCurrentProgram(core.get());
		Check(test, N::CreateServer(&server, reinterpret_cast<N::ServerConfig*>(1), nullptr, storage) == static_cast<int>(0x8002000eu),
		      "NP configuration copy fault translated without dereference");
		Check(test, N::CreateServer(&server, &config, nullptr, unowned_storage.data()) != 0 && server == reinterpret_cast<void*>(0x1234),
		      "production storage hook rejects unmanaged backing before publishing");
		Check(test, N::CreateServer(&server, &config, nullptr, storage) == 0 && server == storage,
		      "real guest identity registers server without claiming dispatcher readiness");
		Check(test, !Loader::RetireSourceBoundNpTransport(), "live native object retains installed owner");
		if (server == storage) {
			using Destroy = int (KYTY_SYSV_ABI *)(void*);
			const auto destroy = reinterpret_cast<Destroy>((*static_cast<void***>(server))[0]);
			Check(test, destroy(server) == 0, "native server destroyed before uninstall");
			server = nullptr;
		}
		Check(test, Loader::RetireSourceBoundNpTransport() && !N::Installed(), "NP runtime retires only after objects drain");
	}
	Check(test, Loader::DeactivateRuntimeProcessIdentity(core.get()) && Loader::GetCurrentGuestProcessId() == -1 &&
	            !Loader::TryGetSourceBoundProcessAuthInfo(pid, &auth) && auth == saved, "retirement revokes exact record");
	if (np_transport) { Check(test, !Loader::InstallSourceBoundNpTransport(core.get()), "revoked owner cannot reinstall NP runtime"); }
	Loader::RuntimeLinker::SetCurrentProgram(const_cast<Loader::Program*>(previous));
}

void TestSourceBoundUiIdentity() {
	constexpr const char* test = "SourceBoundUiIdentity";
	const char* path = std::getenv("KYTY_TEST_UI_IMAGE");
	const char* record_path = std::getenv("KYTY_APPDB_UI_AUTH_RECORD");
	const char* param = std::getenv("KYTY_TEST_UI_PARAM");
	Check(test, path && record_path && param, "explicit UI image, raw record and metadata required");
	if (!path || !record_path || !param) { return; }
	Loader::Program ui;
	ui.unique_id = 790901; ui.file_name = path;
	ui.elf = std::make_unique<Loader::Elf64>(); ui.elf->Open(path);
	std::vector<uint8_t> source;
	Check(test, ui.elf->SnapshotPlainFile(21878244, &source), "same-open UI snapshot");
	if (source.empty()) { return; }
	std::array<uint8_t, 136> record {};
	Common::File file;
	Check(test, file.Open(record_path, Common::File::Mode::Read) && file.Size() == record.size(), "UI record extent");
	uint32_t read = 0; file.Read(record.data(), record.size(), &read);
	Check(test, read == record.size(), "UI record read");
	const auto* ph = ui.elf->GetPhdr();
	const auto count = ui.elf->GetEhdr()->e_phnum;
	for (size_t i = 0; i < count; ++i) {
		if (ph[i].p_type == Loader::PT_LOAD) { ui.base_size = std::max(ui.base_size, ph[i].p_vaddr + ph[i].p_memsz); }
	}
	std::vector<uint8_t> memory(ui.base_size, 0);
	ui.base_vaddr = reinterpret_cast<uint64_t>(memory.data());
	std::vector<Loader::BootMappedSegment> mapped;
	for (size_t i = 0; i < count; ++i) {
		if (ph[i].p_type != Loader::PT_LOAD) { continue; }
		std::memcpy(memory.data() + ph[i].p_vaddr, source.data() + ph[i].p_offset, ph[i].p_filesz);
		mapped.push_back({ph[i].p_vaddr, {memory.data() + ph[i].p_vaddr, static_cast<size_t>(ph[i].p_memsz)}});
	}
	record[0] ^= 1;
	Check(test, !Loader::AdmitSourceBoundBootCredentials(&ui, source, mapped, record), "modified UI credential rejected");
	record[0] ^= 1;
	source[0x16305] ^= 1; memory[0x12305] ^= 1;
	Check(test, !Loader::AdmitSourceBoundBootCredentials(&ui, source, mapped, record), "extra coherent code patch rejected");
	source[0x16305] ^= 1; memory[0x12305] ^= 1;
	memory[0x122fd] ^= 1;
	Check(test, !Loader::AdmitSourceBoundBootCredentials(&ui, source, mapped, record), "loaded dimension mismatch rejected");
	memory[0x122fd] ^= 1;
	Check(test, Loader::AdmitSourceBoundBootCredentials(&ui, source, mapped, record), "reviewed UI source and mapping admitted");
	Libs::LncUtil::Lnc::RegisterProgramLaunch(&ui, param, true, 790901);
	Check(test, Loader::ConfigureBootProcessIdentity(&ui), "source-bound UI boot registration");
	auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(&ui);
	int pid = -1; std::array<uint64_t, 17> auth {};
	Check(test, Loader::TryGetCurrentRetainedGuestProcessId(&pid) && pid == 100 &&
	            Loader::TryGetSourceBoundProcessAuthInfo(pid, &auth) && std::memcmp(auth.data(), record.data(), record.size()) == 0,
	      "PID100 queries exact UI credentials, not compatibility data");
	const auto saved = auth;
	Check(test, Loader::DeactivateRuntimeProcessIdentity(&ui) && !Loader::TryGetSourceBoundProcessAuthInfo(100, &auth) && auth == saved,
	      "retired UI credential inaccessible");
	Loader::RuntimeLinker::SetCurrentProgram(previous);
}

void TestSourceBoundLoader() {
	constexpr const char* test = "SourceBoundLoader";
	const char* core_path = std::getenv("KYTY_TEST_ORIGINAL_CORE");
	const char* ui_path = std::getenv("KYTY_TEST_UI_IMAGE");
	const char* param = std::getenv("KYTY_TEST_UI_PARAM");
	Check(test, core_path && ui_path && param, "real input paths required");
	if (!core_path || !ui_path || !param) { return; }
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* core = linker->LoadProgram(core_path);
	auto* ui = linker->LoadProgram(ui_path);
	Check(test, core && ui && core->boot_credential_admission && ui->boot_credential_admission,
	      "production snapshot/map/host-ABI-transform admission");
	if (!core || !ui) { return; }
	Libs::LncUtil::Lnc::RegisterProgramLaunch(core, {}, false, 790902);
	Libs::LncUtil::Lnc::RegisterProgramLaunch(ui, param, true, 790903);
	Check(test, Loader::ConfigureBootProcessIdentity(core), "production Core identity and adapter installation");
	Check(test, Loader::ConfigureBootProcessIdentity(ui), "production UI identity");
	Loader::RuntimeLinker::SetCurrentProgram(core);
	const int core_pid = Loader::GetCurrentGuestProcessId();
	std::array<uint64_t, 17> auth {};
	Check(test, core_pid > 100 && Loader::TryGetGuestProcessAuthInfo(100, &auth) && auth[0] == UINT64_C(0x480000000000000f),
	      "Core resolves exact authenticated UI peer");
	namespace N = Libs::AppDbNativeTransport;
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> client_storage {};
	uint64_t handler = 0; // Never dispatched in this mapping/policy test.
	N::ServerConfig config {0xf00, 0xf00, &handler, 0, {}, 0, 0};
	std::memcpy(config.name, N::ServiceName, sizeof(N::ServiceName));
	void* server = nullptr; void* client = nullptr;
	CheckOk(test, N::CreateServer(&server, &config, nullptr, server_storage.data()), "production reserved-name gate and copyout");
	Check(test, N::Uninstall() != 0, "live native object pins runtime owner");
	std::array<uint8_t, 0x158> client_config {};
	Libs::IpmiClientAbi::Construct(client_config.data());
	std::memcpy(client_config.data(), N::ServiceName, sizeof(N::ServiceName));
	Loader::RuntimeLinker::SetCurrentProgram(ui);
	CheckOk(test, N::CreateClient(&client, client_config.data(), nullptr, client_storage.data()), "retained UI transport identity");
	Loader::RevokeBootCredentials(ui);
	using Connect = int (KYTY_SYSV_ABI *)(void*, const void*, uint64_t, int32_t*);
	int32_t accepted = 123;
	Check(test, reinterpret_cast<Connect>((*static_cast<void***>(client))[2])(client, nullptr, 0, &accepted) == static_cast<int>(0x80020001u) && accepted == 123,
	      "missing UI credential denies before native acceptance without modifying result");
	using Destroy = int (KYTY_SYSV_ABI *)(void*);
	CheckOk(test, reinterpret_cast<Destroy>((*static_cast<void***>(client))[0])(client), "client teardown");
	Loader::RuntimeLinker::SetCurrentProgram(core);
	CheckOk(test, reinterpret_cast<Destroy>((*static_cast<void***>(server))[0])(server), "server teardown");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	// Mapping preparation only: no guest entry, initializer or native handler ran.
	linker->Clear();
	Check(test, !Loader::TryGetSourceBoundProcessAuthInfo(core_pid, &auth) && !Loader::TryGetSourceBoundProcessAuthInfo(100, &auth),
	      "mapping release revokes both retained credentials");
}
