// Parent includes libs/npManagerClient.h at global scope. This fixture exercises
// the actual client with an independent in-process IPC endpoint, not PSN traffic.
namespace NpManagerClientTest {
namespace Np = Libs::NpManagerClient;
constexpr const char* Name = "NpManagerClient";

struct Endpoint: Np::Endpoint {
	bool connected = true;
	int transport = 0;
	int service = Np::SignedOut;
	int subscribe_error = 0;
	int open_error = 0;
	int discovery_transport = 0;
	int discovery_service = 0;
	bool unterminated_name = false;
	size_t discovery_size = 32;
	int discoveries = 0;
	int opens = 0;
	int poll_error = 0;
	int subscriptions[8] {};
	int unsubscriptions[8] {};
	int invokes = 0;
	std::atomic_int disconnects {0};
	uint32_t last_method = 0;
	std::array<uint8_t, 8> last_input {};
	size_t last_input_size = 0;
	size_t last_output_size = 0;
	size_t written = 4;
	bool shape_ok = true;
	std::vector<Np::Event> events;
	size_t next_event = 0;
	bool Connected() const noexcept override { return connected; }
	Np::Reply Invoke(uint32_t method, std::span<const uint8_t> input,
	                 std::span<uint8_t> output) noexcept override {
		if (method == 0) {
			++discoveries;
			shape_ok = shape_ok && input.empty() && output.size() == 32;
			if (output.size() >= 32) {
				std::fill(output.begin(), output.end(), 0);
				std::memcpy(output.data(), "fixture-np-events", sizeof("fixture-np-events"));
				if (unterminated_name) { std::fill(output.begin(), output.end(), 'x'); }
			}
			return {discovery_transport, discovery_service, discovery_size};
		}
		if (method == 1 || method == 2) {
			uint32_t channel = 0, size = 0;
			if (input.size() >= 4) { std::memcpy(&channel, input.data(), 4); }
			if (input.size() >= 8) { std::memcpy(&size, input.data() + 4, 4); }
			shape_ok = shape_ok && (channel == 4 || channel == 7) && output.empty() &&
			           input.size() == (method == 1 ? 8 : 4) && (method == 2 || size == 20);
			if (channel != 4 && channel != 7) { return {Np::TransportIoError, 0, 0}; }
			if (method == 1) { ++subscriptions[channel]; return {subscribe_error, 0, 0}; }
			++unsubscriptions[channel];
			return {-9, 0, 0};
		}
		++invokes;
		last_method = method;
		last_input_size = input.size();
		last_output_size = output.size();
		last_input.fill(0);
		std::copy_n(input.begin(), std::min(input.size(), last_input.size()), last_input.begin());
		// Deliberately dirty the private reply even on error. Caller output must
		// not be published on transport/service failure or an incomplete reply.
		std::fill(output.begin(), output.end(), 0x5a);
		return {transport, service, written};
	}
	int OpenEvents(const char* name) noexcept override {
		++opens;
		shape_ok = shape_ok && std::strcmp(name, "fixture-np-events") == 0;
		return open_error;
	}
	int Poll(Np::Event& event) noexcept override {
		if (poll_error) { return poll_error; }
		if (next_event == events.size()) { return 1; }
		event = events[next_event++];
		return 0;
	}
	void Disconnect() noexcept override { ++disconnects; connected = false; }
	void Push(uint32_t channel, int user, int value) {
		Np::Event event;
		event.channel = channel;
		event.size = 20;
		std::memcpy(event.payload.data() + 8, &value, 4);
		std::memcpy(event.payload.data() + 12, &user, 4);
		events.push_back(event);
	}
};

struct Record {
	int calls = 0;
	int user = 0;
	int value = 0;
	Np::Client* self = nullptr;
	int id = 0;
	int removed = -1;
	static void KYTY_SYSV_ABI Receive(int user, int value, void* arg) {
		auto& record = *static_cast<Record*>(arg);
		++record.calls;
		record.user = user;
		record.value = value;
		if (record.self) { record.removed = record.self->Unregister(4, record.id); }
	}
};

void RequestsAndAbsentService() {
	Np::Client client;
	std::array<uint8_t, 24> guarded;
	guarded.fill(0xa7);
	const auto saved = guarded;
	int state = 0x12345678;
	Check(Name, client.GetUserData13(-1, nullptr) == Np::NotInitialized &&
	            client.GetServiceState38(7, nullptr) == Np::InvalidArgument &&
	            client.GetServiceState38(7, &state) == Np::NotInitialized && state == 0x12345678,
	      "absent endpoint/firmware validation order");
	Check(Name, client.Register(4, 7, Record::Receive, nullptr) == Np::NotInitialized,
	      "unavailable service fabricated callback ID");
	auto empty_reply = std::make_shared<Endpoint>();
	empty_reply->discovery_size = 0;
	Check(Name, client.Attach(empty_reply) == Np::TransportIoError && !client.Connected() &&
	            empty_reply->disconnects == 1 && empty_reply->opens == 0,
	      "generic success-with-no-reply became initialized NP service");
	auto invalid_name = std::make_shared<Endpoint>();
	invalid_name->unterminated_name = true;
	Check(Name, client.Attach(invalid_name) == Np::TransportIoError && invalid_name->opens == 0 &&
	            invalid_name->disconnects == 1, "unterminated semaphore name reached event backend");
	auto discovery_failure = std::make_shared<Endpoint>();
	discovery_failure->discovery_transport = static_cast<int>(0x80020035u);
	discovery_failure->discovery_service = Np::SignedOut;
	Check(Name, client.Attach(discovery_failure) == discovery_failure->discovery_transport &&
	            discovery_failure->opens == 0 && discovery_failure->disconnects == 1,
	      "discovery masked transport failure with service result");
	auto failed_events = std::make_shared<Endpoint>();
	failed_events->open_error = static_cast<int>(0x80020002u);
	Check(Name, client.Attach(failed_events) == failed_events->open_error && !client.Connected() &&
	            failed_events->disconnects == 1 && failed_events->opens == 1,
	      "missing real event semaphore left a partially initialized NP client");
	auto endpoint = std::make_shared<Endpoint>();
	CheckOk(Name, client.Attach(endpoint), "attach retained endpoint");
	Check(Name, endpoint->discoveries == 1 && endpoint->opens == 1 && endpoint->shape_ok,
	      "service initialization skipped event-channel discovery");
	Check(Name, client.Attach(endpoint) == Np::AlreadyInitialized, "live endpoint replaced");
	Check(Name, client.GetUserData13(7, nullptr) == Np::InvalidArgument &&
	            client.GetUserData13(-1, guarded.data() + 2) == Np::UserNotFound && endpoint->invokes == 0,
	      "invalid getter dispatched IPC");
	Check(Name, client.GetUserData13(7, guarded.data() + 2) == Np::SignedOut && guarded == saved,
	      "signed-out service result published user bytes");
	Check(Name, endpoint->last_method == 0x13 && endpoint->last_input_size == 8 &&
	            endpoint->last_output_size == 20 && endpoint->last_input[0] == 7 &&
	            endpoint->last_input[4] == 1 && endpoint->last_input[5] == 0,
	      "method13 request schema differs from firmware");
	endpoint->transport = static_cast<int>(0x80020035u);
	Check(Name, client.GetServiceState38(7, &state) == endpoint->transport && state == 0x12345678,
	      "service error hid transport failure or output was published");
	Check(Name, endpoint->last_method == 0x38 && endpoint->last_input_size == 4 &&
	            endpoint->last_output_size == 4, "method38 IPC shape");
	endpoint->transport = endpoint->service = 0;
	endpoint->written = 3;
	Check(Name, client.GetServiceState38(7, &state) == Np::TransportIoError && state == 0x12345678,
	      "short successful reply exposed an uninitialized state");
	endpoint->written = 4;
	CheckOk(Name, client.GetServiceState38(7, &state), "complete opaque state reply");
	Check(Name, state == 0x5a5a5a5a, "service state was fabricated or normalized");
	endpoint->written = 20;
	CheckOk(Name, client.GetUserData13(7, guarded.data() + 2), "complete opaque method13 reply");
	Check(Name, guarded[0] == 0xa7 && guarded[1] == 0xa7 && guarded[22] == 0xa7 && guarded[23] == 0xa7 &&
	            std::all_of(guarded.begin() + 2, guarded.begin() + 22, [](uint8_t b) { return b == 0x5a; }),
	      "method13 did not copy exactly twenty bytes to unaligned guest output");
	client.Detach();
	Check(Name, endpoint->disconnects == 1 && !client.Connected() &&
	            client.GetServiceState38(7, &state) == Np::NotInitialized,
	      "disconnect did not remove service access");
}

void Subscriptions() {
	Np::Client client;
	auto endpoint = std::make_shared<Endpoint>();
	CheckOk(Name, client.Attach(endpoint), "subscription endpoint");
	Record exact, all, other, seven;
	endpoint->subscribe_error = static_cast<int>(0x80020002u);
	Check(Name, client.Register(4, 17, Record::Receive, &exact) == endpoint->subscribe_error,
	      "backend registration failure became successful local registration");
	endpoint->subscribe_error = 0;
	const int a = client.Register(4, 17, Record::Receive, &exact);
	const int b = client.Register(4, 0xfe, Record::Receive, &all);
	const int c = client.Register(4, 18, Record::Receive, &other);
	const int d = client.Register(7, 17, Record::Receive, &seven);
	Check(Name, a == 1 && b == 2 && c == 3 && d == 1 &&
	            endpoint->subscriptions[4] == 2 && endpoint->subscriptions[7] == 1 && endpoint->shape_ok,
	      "failed registration leaked slot, wrong IDs, or duplicate server subscription");
	CheckOk(Name, client.DispatchCallbacks(), "no initial event");
	Check(Name, exact.calls == 0 && all.calls == 0, "registration fabricated a state event");
	endpoint->Push(4, 17, -17);
	endpoint->Push(7, 17, -7);
	CheckOk(Name, client.DispatchCallbacks(), "service event dispatch");
	Check(Name, exact.calls == 1 && exact.user == 17 && exact.value == -17 &&
	            all.calls == 1 && other.calls == 0 && seven.calls == 1 && seven.value == -7,
	      "event payload offsets, filtering or channel separation");
	exact.self = &client;
	exact.id = a;
	endpoint->Push(4, 17, -19);
	CheckOk(Name, client.DispatchCallbacks(), "reentrant callback unregister");
	Check(Name, exact.removed == 0 && endpoint->unsubscriptions[4] == 0,
	      "self-unregister failed or removed another subscriber");
	CheckOk(Name, client.Unregister(4, b), "remove wildcard");
	CheckOk(Name, client.Unregister(4, c), "last remove ignores backend failure like firmware");
	Check(Name, endpoint->unsubscriptions[4] == 1 &&
	            client.Unregister(4, c) == Np::InvalidArgument, "last unsubscribe or stale ID");
	for (int i = 1; i < 16; ++i) {
		Check(Name, client.Register(7, 17, Record::Receive, &seven) == i + 1, "event7 quota allocation");
	}
	Check(Name, client.Register(7, 17, Record::Receive, &seven) == Np::CallbackSlotsFull,
	      "native 16-slot event7 quota not enforced");
	for (int i = 0; i < 32; ++i) {
		Check(Name, client.Register(4, 17, Record::Receive, &other) == i + 1, "event4 quota allocation");
	}
	Check(Name, client.Register(4, 17, Record::Receive, &other) == Np::CallbackSlotsFull,
	      "native 32-slot event4 quota not enforced");
	const auto consumed = endpoint->next_event;
	for (int i = 0; i < 65; ++i) { endpoint->Push(4, 17, -21); }
	CheckOk(Name, client.DispatchCallbacks(), "bounded event batch");
	Check(Name, endpoint->next_event == consumed + 64, "pump did not preserve bounded event backlog");
	CheckOk(Name, client.DispatchCallbacks(), "remaining event batch");
	Check(Name, endpoint->next_event == consumed + 65, "pump lost event backlog");
	endpoint->poll_error = static_cast<int>(0x80020005u);
	Check(Name, client.DispatchCallbacks() == endpoint->poll_error, "poll transport error swallowed");
	client.Detach();
	Check(Name, endpoint->disconnects == 1 && client.Unregister(7, d) == Np::NotInitialized,
	      "detached service retained subscriptions");
}

void RegisteredAbsentEndpoint() {
	Loader::SymbolDatabase symbols;
	Libs::InitNpManagerClient_1(&symbols);
	using Register = decltype(&Np::RegisterEvent4);
	using Getter = decltype(&Np::GetServiceState38);
	const auto resolve = [&](const char* nid) {
		const auto* entry = symbols.Find({nid, "NpManager", 1, "NpManager", 1, 1, Loader::SymbolType::Func});
		Check(Name, entry && entry->vaddr != 0, "missing exact NP manager import identity");
		return entry->vaddr;
	};
	Np::Session().Detach();
	for (const char* nid: {"S9xDus0Cums", "E6rzFwsDFwE"}) {
		Check(Name, reinterpret_cast<Register>(resolve(nid))(17, Record::Receive, nullptr) == Np::NotInitialized,
		      "called unresolved replacement fabricated endpoint/subscription");
	}
	int state = 123;
	Check(Name, reinterpret_cast<Getter>(resolve("XRFchqddEVU"))(17, &state) == Np::NotInitialized && state == 123,
	      "registered query fabricated initialized state");
	std::array<uint8_t, 20> identity;
	identity.fill(0x91);
	const auto saved = identity;
	Check(Name, reinterpret_cast<decltype(&Np::GetUserData13ForUser)>(resolve("sTtvF4QVhjg"))(17, identity.data()) ==
	            Np::NotInitialized && identity == saved, "registered method13 failure modified user output");
	for (const char* nid: {"zh2KsQZlAN4", "PafRf+sxnwA"}) {
		Check(Name, reinterpret_cast<decltype(&Np::UnregisterEvent4)>(resolve(nid))(1) == Np::NotInitialized,
		      "unregister accepted absent service");
	}
}

void SelfDetachStopsBatch() {
	Np::Client client;
	auto endpoint = std::make_shared<Endpoint>();
	auto replacement = std::make_shared<Endpoint>();
	CheckOk(Name, client.Attach(endpoint), "self-detach endpoint");
	struct State {
		Np::Client& client;
		std::shared_ptr<Endpoint> replacement;
		int attach_result = 123;
		static void KYTY_SYSV_ABI Receive(int, int, void* arg) {
			auto& state = *static_cast<State*>(arg);
			state.client.Detach();
			state.attach_result = state.client.Attach(state.replacement);
		}
	} state {client, replacement};
	Record after;
	Check(Name, client.Register(4, 17, State::Receive, &state) == 1 &&
	            client.Register(4, 17, Record::Receive, &after) == 2, "self-detach subscriptions");
	endpoint->Push(4, 17, -9);
	endpoint->Push(4, 17, -10);
	Check(Name, client.DispatchCallbacks() == Np::NotInitialized && state.attach_result == Np::TransportBusy &&
	            after.calls == 0 && endpoint->next_event == 1 && endpoint->disconnects == 1,
	      "self-detach retained old listeners or admitted replacement into old pump");
	CheckOk(Name, client.Attach(replacement), "attach after self-detaching pump returns");
}

void DisconnectDrainsCallbacks() {
	Np::Client client;
	auto endpoint = std::make_shared<Endpoint>();
	CheckOk(Name, client.Attach(endpoint), "drain endpoint");
	struct Blocked {
		std::atomic_bool entered {false};
		std::atomic_bool release {false};
		static void KYTY_SYSV_ABI Receive(int, int, void* arg) {
			auto& state = *static_cast<Blocked*>(arg);
			state.entered.store(true);
			state.release.wait(false);
		}
	} blocked;
	Check(Name, client.Register(4, 17, Blocked::Receive, &blocked) == 1, "drain subscription");
	endpoint->Push(4, 17, -9);
	std::atomic_int dispatched {123};
	std::atomic_bool detached {false};
	struct Workers {
		Blocked& blocked;
		std::thread pump, detach;
		~Workers() {
			blocked.release.store(true);
			blocked.release.notify_all();
			if (pump.joinable()) { pump.join(); }
			if (detach.joinable()) { detach.join(); }
		}
	} workers {blocked};
	workers.pump = std::thread([&] { dispatched.store(client.DispatchCallbacks()); });
	const auto wait_for = [](auto predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!predicate() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return predicate();
	};
	Check(Name, wait_for([&] { return blocked.entered.load(); }), "callback did not enter");
	Check(Name, client.DispatchCallbacks() == Np::TransportBusy, "concurrent guest callback pump admitted");
	workers.detach = std::thread([&] { client.Detach(); detached.store(true); });
	Check(Name, wait_for([&] { return endpoint->disconnects.load() == 1; }) && !detached.load(),
	      "disconnect failed to retain active guest callback lease");
	auto replacement = std::make_shared<Endpoint>();
	Check(Name, client.Attach(replacement) == Np::TransportBusy && replacement->discoveries == 0,
	      "replacement endpoint attached before old callback drain");
	blocked.release.store(true);
	blocked.release.notify_all();
	workers.pump.join();
	workers.detach.join();
	Check(Name, detached.load() && dispatched.load() == Np::NotInitialized,
	      "old callback pump survived the endpoint generation change");
	CheckOk(Name, client.Attach(replacement), "reconnect after drained disconnect");
	replacement->Push(4, 17, -11);
	CheckOk(Name, client.DispatchCallbacks(), "old registration not retained after reconnect");
}
} // namespace NpManagerClientTest

void TestNpManagerClient() {
	NpManagerClientTest::RequestsAndAbsentService();
	NpManagerClientTest::Subscriptions();
	NpManagerClientTest::RegisteredAbsentEndpoint();
	NpManagerClientTest::SelfDetachStopsBatch();
	NpManagerClientTest::DisconnectDrainsCallbacks();
}
