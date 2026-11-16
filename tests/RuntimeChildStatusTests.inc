// Separate-process fixture: actual child command channel and guest status export.
void TestRuntimeChildStatus(const char* ending = "close") {
#if defined(_WIN32)
	constexpr const char* test = "RuntimeChildStatus";
	namespace Child = Loader::RuntimeChild;
	Loader::SymbolDatabase symbols;
	Libs::InitSystemService_1(&symbols);
	const auto* symbol = symbols.FindByNid("rPo6tV8D9bM", Loader::SymbolType::Func);
	Check(test, symbol != nullptr, "guest status export available");
	using Query = int (KYTY_SYSV_ABI *)(Libs::SystemService::SystemServiceStatus*);
	const auto query = reinterpret_cast<Query>(symbol->vaddr);
	const auto status = [&](bool background) {
		Libs::SystemService::SystemServiceStatus value;
		Check(test, query(&value) == 0 && value.is_in_background_execution == background && !value.is_system_ui_overlaid,
		      "guest status reflects applied visibility without inventing overlay from controller focus");
	};
	status(false); // Direct/session behavior before child configuration.
	HANDLE receive = nullptr, send = nullptr, observed = nullptr, notify = nullptr;
	Check(test, CreatePipe(&receive, &send, nullptr, 4096) && CreatePipe(&observed, &notify, nullptr, 4096), "owned command pipes");
	Child::Channel parent;
	Check(test, parent.Adopt(reinterpret_cast<uint64_t>(observed), reinterpret_cast<uint64_t>(send)), "parent channel adopts handles");
	const Child::Key key {108, 0, 1, 0x556677, 0x8899};
	Check(test, Loader::ConfigureRuntimeChildClient(Child::EncodeArguments({key, reinterpret_cast<uint64_t>(receive),
	    reinterpret_cast<uint64_t>(notify), 0})), "configure actual child role");
	Child::Message observed_message;
	Check(test, parent.Read(&observed_message) == Child::ReadResult::Received && observed_message.kind == Child::Kind::Hello, "real child hello");
	status(true); // Child starts hidden before any focus command is applied.
	uint64_t serial = 0;
	const auto focus = [&](uint64_t epoch, bool foreground, bool controller) {
		Child::Message message;
		message.key = key; message.serial = ++serial; message.kind = Child::Kind::Focus;
		message.value[0] = foreground; message.value[1] = controller; message.value[2] = epoch;
		Check(test, parent.Write(message), "parent writes actual focus request");
		Check(test, Loader::PollRuntimeChildCommand(&observed_message), "child validates focus request");
	};
	const auto apply = [&](uint64_t epoch, int result = 0) {
		Loader::AcknowledgeRuntimeChildFocus(epoch, result);
		Check(test, parent.Read(&observed_message) == Child::ReadResult::Received && observed_message.kind == Child::Kind::FocusAck &&
		      observed_message.value[0] == epoch && observed_message.value[1] == result, "applied focus acknowledged");
	};
	focus(1, true, true); status(true);
	Loader::AcknowledgeRuntimeChildFocus(2); status(true); // Wrong epoch cannot publish visibility.
	apply(1); status(false);
	focus(2, false, false); status(false); apply(2); status(true); // Home.
	focus(3, true, true); status(true); apply(3); status(false); // Resume.
	focus(4, true, false); apply(4); status(false); // Controller-only transfer.
	focus(5, true, true); apply(5, static_cast<int>(0x80020005u)); status(true);
	Loader::AcknowledgeRuntimeChildFocus(5); status(true); // Failed epoch cannot later be re-applied.
	focus(6, true, true); focus(7, false, false);
	Loader::AcknowledgeRuntimeChildFocus(6); status(true); // Superseded, unapplied epoch.
	apply(7); status(true);
	focus(8, true, true); apply(8); status(false);
	if (std::strcmp(ending, "eof") == 0) {
		parent.Reset();
		Check(test, !Loader::PollRuntimeChildCommand(&observed_message) && Loader::RuntimeChildChannelLost(), "real pipe EOF observed");
		status(true);
	} else if (std::strcmp(ending, "ack-failure") == 0) {
		focus(9, true, true);
		parent.Reset();
		Loader::AcknowledgeRuntimeChildFocus(9);
		Check(test, Loader::RuntimeChildChannelLost(), "failed ACK write marks channel lost");
		status(true);
	} else if (std::strcmp(ending, "window-loss") == 0) {
		focus(9, true, true);
		Loader::RevokeRuntimeChildFocus(); status(true);
		Loader::AcknowledgeRuntimeChildFocus(9); status(true);
	} else {
		focus(9, true, true); // Close must revoke even a still-unapplied foreground request.
		Child::Message close;
		close.key = key; close.serial = ++serial; close.kind = Child::Kind::Close;
		Check(test, parent.Write(close) && Loader::PollRuntimeChildCommand(&observed_message), "actual close revokes foreground");
		status(true);
		Loader::AcknowledgeRuntimeChildFocus(9); status(true);
	}
	std::printf("[host]    %-48s ok\n", test);
#endif
}
