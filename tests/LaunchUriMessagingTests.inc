// Include in VirtualMemoryAllocationTests.cpp's fixture namespace; dispatch
// TestLaunchUriMessaging() in a dedicated process after InitSubsystems().
// Top-level declarations required (do not include a .cpp):
// namespace Libs::JscdNet { void InitJscdNet_1(Loader::SymbolDatabase*); }
// namespace Libs::JscdNet::AppMsgSys { void ResetShellUiMessaging(); }
// Existing fixture headers suffice. Link libJscdNet.cpp and its normal HLE
// dependencies, including libLncUtil.cpp, hostMemory.cpp, fileSystem/eventFlag.
void TestLaunchUriMessaging() {
	constexpr const char* test = "LaunchUriMessaging";
	namespace Lnc = Libs::LncUtil::Lnc;
	using Loader::RuntimeLinker;
	constexpr int empty = static_cast<int>(0x80c50005);
	constexpr int bad_address = static_cast<int>(0x80c50004);
	constexpr int missing = static_cast<int>(0x80c50007);
	constexpr int exhausted = static_cast<int>(0x80c50006);
	constexpr int bad_param = static_cast<int>(0x80e30003);
	struct Param { uint32_t option; int32_t user_id; };
	struct Payload { int32_t app_id; int32_t user_id; uint32_t option; char uri[0x1800]; };
	struct Message {
		int32_t sender;
		uint32_t type;
		uint8_t payload[0x2000];
		uint32_t size;
		uint32_t padding;
		uint64_t timestamp;
	};
	static_assert(sizeof(Param) == 8 && sizeof(Payload) == 0x180c && sizeof(Message) == 0x2018);
	struct Restore {
		Loader::Program* previous = RuntimeLinker::GetCurrentProgram();
		~Restore() {
			RuntimeLinker::SetCurrentProgram(previous);
			Libs::JscdNet::AppMsgSys::ResetShellUiMessaging();
		}
	} restore;
	Libs::JscdNet::AppMsgSys::ResetShellUiMessaging();
	Loader::SymbolDatabase symbols;
	Libs::JscdNet::InitJscdNet_1(&symbols);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr && symbol->vaddr != 0, "missing actual guest export");
		return symbol->vaddr;
	};
	const auto init = reinterpret_cast<void(KYTY_SYSV_ABI *)(Param*)>(resolve("gYOugDgfYJw"));
	const auto launch = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, const Param*)>(resolve("GYUk4t27Myw"));
	const auto send = reinterpret_cast<int(KYTY_SYSV_ABI *)(uint32_t, const void*, uint64_t, uint32_t)>(resolve("5ygy1IPUh5c"));
	const auto send_to = reinterpret_cast<int(KYTY_SYSV_ABI *)(int32_t, uint32_t, const void*, uint64_t, uint32_t)>(resolve("+zuv20FsXrA"));
	const auto receive = reinterpret_cast<int(KYTY_SYSV_ABI *)(Message*)>(resolve("ZVRXXqj1n80"));
	const auto flags = reinterpret_cast<int(KYTY_SYSV_ABI *)(uint64_t, uint64_t*, uint8_t)>(resolve("iKNXKsUtOjY"));
	const auto register_shell = reinterpret_cast<int(KYTY_SYSV_ABI *)()>(resolve("awS+eYVuXJA"));
	struct GuardedParam { uint64_t before; Param param; uint64_t after; } guarded {0x1122334455667788, {42, 42}, 0x8877665544332211};
	init(nullptr);
	init(&guarded.param);
	Check(test, guarded.before == 0x1122334455667788 && guarded.after == 0x8877665544332211 &&
	            guarded.param.option == 0 && guarded.param.user_id == -1, "exact 8-byte initializer/canaries");
	Check(test, launch(nullptr, &guarded.param) == bad_param && launch("", nullptr) == bad_param,
	      "both launch arguments are required");
	Check(test, launch(reinterpret_cast<const char*>(1), &guarded.param) == bad_param &&
	            launch("", reinterpret_cast<const Param*>(1)) == bad_param, "fault-contained input probes");

	const auto root = std::filesystem::temp_directory_path() /
	    ("kyuri_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(test, std::filesystem::create_directories(root), "create metadata fixture");
	struct RemoveFiles {
		std::filesystem::path root;
		~RemoveFiles() { std::error_code error; std::filesystem::remove_all(root, error); }
	} remove_files {root};
	Common::File metadata;
	Check(test, metadata.Create(root / "shell.json"), "create shell metadata");
	constexpr char json[] = R"({"titleId":"NPXS40087"})";
	uint32_t written = 0;
	metadata.Write(json, sizeof(json) - 1, &written);
	metadata.Close();
	Check(test, written == sizeof(json) - 1, "write shell metadata");
	// Session registries retain identities; keep these owner records alive.
	auto* shell = new Loader::Program;
	auto* first = new Loader::Program;
	auto* second = new Loader::Program;
	auto* unknown = new Loader::Program;
	shell->unique_id = 795100;
	first->unique_id = 795101;
	second->unique_id = 795102;
	unknown->unique_id = 795103;
	shell->file_name = root / "eboot.bin";
	first->file_name = root / "first.bin";
	second->file_name = root / "second.bin";
	Lnc::RegisterProgramLaunch(shell, root / "shell.json", true);
	Lnc::RegisterProgramLaunch(first, root / "missing.json", false);
	Lnc::RegisterProgramLaunch(second, root / "missing.json", false);
	int32_t first_id = -1, second_id = -1, shell_id = -1;
	Check(test, Lnc::TryGetProgramAppId(first, &first_id) && Lnc::TryGetProgramAppId(second, &second_id) &&
	            Lnc::TryGetProgramAppId(shell, &shell_id), "stable LNC sender identities");
	Message message;
	std::memset(&message, 0xa5, sizeof(message));
	const Message untouched = message;
	RuntimeLinker::SetCurrentProgram(unknown);
	Check(test, receive(&message) == static_cast<int>(0x80c50002) &&
	            std::memcmp(&message, &untouched, sizeof(message)) == 0, "uninitialized receive preserves output");
	RuntimeLinker::SetCurrentProgram(first);
	Check(test, launch("psapp:launch?titleId=NPXS49999", &guarded.param) == missing, "no receiver is not success");
	Check(test, register_shell() != 0, "ordinary app cannot claim ShellUI endpoint");
	RuntimeLinker::SetCurrentProgram(shell);
	Check(test, register_shell() == 0 && register_shell() == 0, "idempotent registered ShellUI identity");
	Check(test, receive(&message) == empty && std::memcmp(&message, &untouched, sizeof(message)) == 0,
	      "verified empty ABI preserves all envelope bytes");
	uint64_t bits = UINT64_MAX;
	Check(test, flags(UINT64_MAX, &bits, 1) == 0 && bits == 0, "idle event poll succeeds with zero bits");
	Check(test, flags(UINT64_MAX, reinterpret_cast<uint64_t*>(1), 0) == bad_address, "event output probe");

	Loader::Program private_module;
	private_module.application_owner = first;
	RuntimeLinker::SetCurrentProgram(&private_module);
	guarded.param = {0xfedcba98, -17};
	const char* uri = "psapp:launch?titleId=NPXS49999&argument=one%20two";
	Check(test, launch(uri, &guarded.param) == 0, "URI ingress export");
	Check(test, receive(&message) == empty, "sender cannot steal ShellUI messages");
	Check(test, send_to(second_id, 0, uri, 1, 0) == missing, "unimplemented non-ShellUI route fails");
	RuntimeLinker::SetCurrentProgram(shell);
	Check(test, receive(nullptr) == bad_address && receive(reinterpret_cast<Message*>(1)) == bad_address,
	      "invalid output does not dequeue");
	Check(test, receive(&message) == 0, "native TryReceiveMsg export delivers URI");
	Payload payload;
	std::memcpy(&payload, message.payload, sizeof(payload));
	Check(test, message.sender == first_id && message.type == 0 && message.size == 0x180c &&
	            message.timestamp != 0 && message.padding == 0 && payload.app_id == -1 &&
	            payload.user_id == -17 && payload.option == 0xfedcba98 && std::strcmp(payload.uri, uri) == 0,
	      "exact firmware URI/envelope layout and application owner");
	Check(test, std::all_of(message.payload + sizeof(payload), message.payload + sizeof(message.payload),
	                      [](uint8_t byte) { return byte == 0; }), "zero envelope tail");
	Check(test, receive(&message) == empty, "exactly-once dequeue");
	struct GuardedMessage { uint64_t before; Message value; uint64_t after; } envelope_guard {};
	envelope_guard.before = 0x1122334455667788;
	envelope_guard.after = 0x8877665544332211;
	Check(test, send_to(shell_id, 4, "owned", 6, 0) == 0 && receive(&envelope_guard.value) == 0 &&
	            envelope_guard.before == 0x1122334455667788 && envelope_guard.after == 0x8877665544332211,
	      "explicit ShellUI destination and exact 0x2018-byte output/canaries");

	std::string longest(0x17ff, 'x');
	Check(test, launch(longest.c_str(), &guarded.param) == 0 && receive(&message) == 0, "maximum URI length");
	Check(test, message.payload[12 + 0x17fe] == 'x' && message.payload[12 + 0x17ff] == 0, "maximum URI terminator");
	longest.push_back('x');
	Check(test, launch(longest.c_str(), &guarded.param) == static_cast<int>(0x80e30006) &&
	            receive(&message) == empty, "overlong URI rejected without enqueue");
	Check(test, launch("", &guarded.param) == 0 && receive(&message) == 0, "firmware accepts empty URI");
	Check(test, send(9, nullptr, 0x2001, 0) == static_cast<int>(0x80c50003) &&
	            send(9, reinterpret_cast<void*>(1), 1, 0) == bad_address, "payload size/address validation");
	Check(test, send(9, nullptr, 0, 0x80000000) == 0 && receive(&message) == 0 && message.size == 0,
	      "zero-size payload and ignored flag bits");

	// Read-only and split-page outputs must neither mutate nor dequeue.
	using Mode = Common::VirtualMemory::Mode;
	// InitSubsystems already reserves the guest address ranges. Allocate through
	// their owner rather than trying to reserve a second overlapping host range.
	const auto memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(0, 0x8000, Mode::ReadWrite, "uri-copy-probe");
	Check(test, memory != 0, "allocate guarded output");
	std::memset(reinterpret_cast<void*>(memory), 0x69, 0x8000);
	Check(test, Common::VirtualMemory::Protect(memory + 0x4000, 0x4000, Mode::NoAccess), "protect tail");
	auto* edge_param = reinterpret_cast<Param*>(memory + 0x4000 - 8);
	init(edge_param);
	Check(test, edge_param->option == 0 && edge_param->user_id == -1, "initializer touches exactly eight mapped bytes");
	auto* edge_uri = reinterpret_cast<char*>(memory + 0x4000 - 0x1800);
	std::memset(edge_uri, 'x', 0x1800);
	edge_uri[0x17ff] = 0;
	Check(test, launch(edge_uri, &guarded.param) == 0 && receive(&message) == 0,
	      "maximum terminated URI at inaccessible page boundary");
	edge_uri[0x17ff] = 'x';
	Check(test, launch(edge_uri, &guarded.param) == static_cast<int>(0x80e30006),
	      "unterminated maximum URI never reads the guard page");
	std::memset(reinterpret_cast<void*>(memory), 0x69, 0x4000);
	Check(test, send(8, nullptr, 0, 0) == 0, "enqueue before copyout failure");
	Check(test, receive(reinterpret_cast<Message*>(memory + 0x3ff0)) == bad_address &&
	            *reinterpret_cast<uint64_t*>(memory + 0x3ff0) == UINT64_C(0x6969696969696969), "split output unchanged");
	Check(test, Common::VirtualMemory::Protect(memory, 0x4000, Mode::Read), "make output read-only");
	init(reinterpret_cast<Param*>(memory));
	Check(test, *reinterpret_cast<const uint64_t*>(memory) == UINT64_C(0x6969696969696969),
	      "void initializer safely leaves read-only output unchanged");
	Check(test, receive(reinterpret_cast<Message*>(memory)) == bad_address && receive(&message) == 0,
	      "read-only output retains queued message");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(memory, 0x8000), "free guarded output");

	// Capacity is host policy; verify bounded backpressure and no lost entries.
	size_t accepted = 0;
	for (; accepted < 1024; ++accepted) {
		const auto value = static_cast<uint32_t>(accepted);
		const int result = send(11, &value, sizeof(value), 0);
		if (result == exhausted) { break; }
		Check(test, result == 0, "queue fill unexpected error");
	}
	Check(test, accepted > 0 && accepted < 1024, "bounded queue exhaustion");
	Check(test, send(12, nullptr, 0, 1) == exhausted, "full queue cannot replace an absent type");
	uint64_t timestamp = 0;
	for (size_t i = 0; i < accepted; ++i) {
		Check(test, receive(&message) == 0, "drain accepted entry");
		uint32_t value = 0;
		std::memcpy(&value, message.payload, sizeof(value));
		Check(test, value == i && message.timestamp >= timestamp, "FIFO payload and monotonic enqueue timestamp");
		timestamp = message.timestamp;
	}
	Check(test, receive(&message) == empty, "exhaustion does not append a phantom entry");
	Check(test, send(1, "a", 2, 0) == 0 && send(2, "b", 2, 0) == 0 && send(1, "c", 2, 1) == 0,
	      "firmware replace-by-type flag");
	Check(test, receive(&message) == 0 && message.type == 2 && receive(&message) == 0 && message.type == 1 &&
	            message.payload[0] == 'c' && receive(&message) == empty, "replacement preserves other types and appends");

	std::atomic<bool> valid {true};
	auto produce = [&](Loader::Program* owner) {
		RuntimeLinker::SetCurrentProgram(owner);
		for (uint32_t i = 0; i < 16; ++i) {
			if (send(3, &i, sizeof(i), 0) != 0) { valid = false; }
		}
		RuntimeLinker::SetCurrentProgram(nullptr);
	};
	std::thread a(produce, first), b(produce, second);
	a.join();
	b.join();
	Check(test, valid, "concurrent senders accepted");
	uint32_t next_first = 0, next_second = 0;
	for (int i = 0; i < 32; ++i) {
		Check(test, receive(&message) == 0, "concurrent message receive");
		uint32_t value = 0;
		std::memcpy(&value, message.payload, sizeof(value));
		Check(test, message.sender == first_id || message.sender == second_id, "concurrent stable sender");
		auto& next = message.sender == first_id ? next_first : next_second;
		Check(test, value == next++, "per-sender FIFO under concurrency");
	}
	Check(test, next_first == 16 && next_second == 16 && receive(&message) == empty, "all producers drained exactly once");
	Check(test, !Lnc::HasPendingLaunch(), "URI transport itself does not fabricate LNC launches");
	Libs::JscdNet::AppMsgSys::ResetShellUiMessaging();
	Check(test, receive(&message) == empty && send(0, nullptr, 0, 0) == 0 && receive(&message) == 0,
	      "known ShellUI poll lazily registers older native boot paths");
}
