// Global includes required: libs/npManagerService.h and kernel/eventFlag.h.
// Uses the actual HLE broker, registered IPMI vtable and named kernel event flags.
namespace NpManagerServiceTest {
namespace Service = Libs::NpManagerService;
namespace Np = Libs::NpManagerClient;
namespace Flags = Libs::LibKernel::EventFlag;
constexpr const char* Name = "NpManagerService";
constexpr int Missing = static_cast<int>(0x80020002u);
constexpr int Disconnected = static_cast<int>(0x80020039u);
constexpr int Unsupported = static_cast<int>(0x8002004eu);

struct Scope {
	Scope() { Service::StopService(); }
	~Scope() { Service::StopService(); }
};

struct Guest {
	void* object = nullptr;
	using Create = int (KYTY_SYSV_ABI *)(void**, const void*, void*, void*);
	using Connect = int (KYTY_SYSV_ABI *)(void*, const void*, uint64_t, int*);
	using Invoke = int (KYTY_SYSV_ABI *)(void*, uint32_t, const Service::DataInfo*, uint32_t,
	                                   int*, Service::BufferInfo*, uint32_t);
	using RawInvoke = int (KYTY_SYSV_ABI *)(void*, uint32_t, const void*, uint64_t,
	                                      int*, void*, uint64_t*, uint64_t);
	template<class T> T Method(size_t index) const { return reinterpret_cast<T>((*static_cast<void***>(object))[index]); }
	explicit Guest(const char* name) {
		Loader::SymbolDatabase symbols;
		Libs::LibIpmiClient::InitIpmiClient_1(&symbols);
		const auto* entry = symbols.Find({"0zsTiDhM0nU", "Ipmi", 1, "Ipmi", 1, 1, Loader::SymbolType::Func});
		Check(Name, entry && entry->vaddr, "registered IPMI client factory missing");
		std::array<char, 0x180> config {};
		std::strncpy(config.data(), name, 24);
		CheckOk(Name, reinterpret_cast<Create>(entry->vaddr)(&object, config.data(), nullptr, nullptr), "IPMI client create");
	}
	~Guest() { if (object) { Method<int64_t (KYTY_SYSV_ABI *)(void*)>(0)(object); } }
	int ConnectNow(int& result) { return Method<Connect>(2)(object, nullptr, 0, &result); }
	int Call(uint32_t method, std::span<const Service::DataInfo> in, std::span<Service::BufferInfo> out, int& result) {
		return Method<Invoke>(11)(object, method, in.data(), static_cast<uint32_t>(in.size()),
		                          &result, out.data(), static_cast<uint32_t>(out.size()));
	}
};

struct Record {
	int calls = 0, user = 0, value = 0;
	static void KYTY_SYSV_ABI Receive(int user, int value, void* data) {
		auto& self = *static_cast<Record*>(data);
		++self.calls; self.user = user; self.value = value;
	}
};

void ServiceAndGuest() {
	Scope scope;
	Guest guest("SceNpMgrIpc");
	int result = 0x12345678;
	Check(Name, guest.ConnectNow(result) == Missing && result == 0x12345678,
	      "absent service produced an acceptance result");
	std::shared_ptr<Np::Endpoint> endpoint;
	Check(Name, Service::ConnectEndpoint(endpoint) == Missing && !endpoint, "absent factory fabricated endpoint");
	CheckOk(Name, Service::StartUnavailableService(), "register local HLE service");
	CheckOk(Name, guest.ConnectNow(result), "broker guest acceptance");
	Check(Name, result == 0, "real local acceptance status");
	CheckOk(Name, Service::ConnectEndpoint(endpoint), "concrete endpoint connection");
	Np::Client client;
	CheckOk(Name, client.Attach(endpoint), "method0 discovery and real named event-flag open");
	Record callback;
	Check(Name, client.Register(4, 17, Record::Receive, &callback) == 1, "actual service subscription");
	CheckOk(Name, client.DispatchCallbacks(), "empty local queue");
	Check(Name, callback.calls == 0, "local service synthesized initial state");
	int opaque = 0x12345678;
	Check(Name, client.GetServiceState38(17, &opaque) == Np::NotInitialized && opaque == 0x12345678,
	      "local service acceptance fabricated an account backend");
	std::array<char, 32> flag_name {};
	Service::BufferInfo name_output {flag_name.data(), flag_name.size(), 99};
	CheckOk(Name, guest.Call(0, {}, {&name_output, 1}, result), "guest method0 transport");
	Check(Name, result == 0 && name_output.written == 32 && flag_name[0] != 0,
	      "discovery returned incomplete name");
	Flags::KernelEventFlag alias = nullptr;
	CheckOk(Name, Flags::KernelOpenEventFlag(&alias, flag_name.data()), "open real guest discovery flag");
	struct Alias {
		Flags::KernelEventFlag& handle;
		~Alias() { if (handle) { Flags::KernelCloseEventFlag(handle); } }
	} alias_scope {alias};
	uint32_t subscription[2] {4, 20};
	Service::DataInfo in {subscription, sizeof(subscription)};
	CheckOk(Name, guest.Call(1, {&in, 1}, {}, result), "guest subscribe transport");
	Check(Name, result == 0, "guest subscription failed");
	for (int value = 0; value < 25; ++value) {
		// Test-origin producer records; production has no attached event source.
		std::array<uint8_t, 24> record {};
		const int user = 17;
		std::memcpy(record.data() + 8, &value, 4);
		std::memcpy(record.data() + 12, &user, 4);
		record[23] = 0x6d;
		CheckOk(Name, Service::PublishEvent(4, record), "enqueue service record");
	}
	CheckOk(Name, client.DispatchCallbacks(), "real flag/poll/method3/callback path");
	Check(Name, callback.calls == 20 && callback.user == 17 && callback.value == 24,
	      "event backlog, queue capacity, payload offsets or callback forwarding");
	uint64_t bits = 0;
	CheckOk(Name, Flags::KernelPollEventFlag(alias, 1u << 4, 0x22, &bits), "guest event flag notification");
	Check(Name, (bits & (1u << 4)) != 0, "missing real notification bit");
	in.size = 4;
	std::array<uint8_t, 0x1ec> guarded;
	guarded.fill(0xa7);
	int remaining = -1;
	Service::BufferInfo output[2] {{guarded.data() + 2, 0x1e8, 0}, {&remaining, 4, 0}};
	// Bad descriptors must not dequeue the real events.
	output[0].capacity = 0x1e7;
	CheckOk(Name, guest.Call(3, {&in, 1}, output, result), "short output service result");
	Check(Name, result < 0 && output[0].written == 0 && remaining == -1, "invalid dequeue modified output");
	output[0].capacity = 0x1e8;
	CheckOk(Name, guest.Call(3, {&in, 1}, output, result), "guest dequeue transport");
	int count = 0, first = -1;
	std::memcpy(&count, guarded.data() + 2, 4);
	std::memcpy(&first, guarded.data() + 2 + 8 + 8, 4);
	Check(Name, result == 0 && count == 20 && first == 5 && remaining == 0 &&
	            output[0].written == 0x1e8 && output[1].written == 4 && guarded[33] == 0x6d &&
	            guarded[0] == 0xa7 && guarded[1] == 0xa7 && guarded[0x1ea] == 0xa7 && guarded[0x1eb] == 0xa7,
	      "native batch layout, full record preservation, overflow policy or canaries");
	CheckOk(Name, guest.Call(3, {&in, 1}, output, result), "second dequeue");
	std::memcpy(&count, guarded.data() + 2, 4);
	Check(Name, result == 0 && count == 0, "dequeue required guessed acknowledgment or replayed consumed records");
	CheckOk(Name, guest.Call(4, {&in, 1}, {}, result), "explicit event set");
	Check(Name, result == 0, "method4 event set result");
	CheckOk(Name, Flags::KernelPollEventFlag(alias, 1u << 4, 0x22, &bits), "method4 set real flag");
	uint64_t raw_size = guarded.size();
	CheckOk(Name, guest.Method<Guest::RawInvoke>(8)(guest.object, 0x150002, nullptr, 0, &result,
	                                             guarded.data(), &raw_size, guarded.size()), "unknown raw request transport");
	Check(Name, result == Unsupported && raw_size == 0, "NP raw method accidentally routed to disc-success fallback");
	Service::StopService();
	Check(Name, !endpoint->Connected() && client.GetServiceState38(17, &opaque) == Np::NotInitialized,
	      "service stop did not revoke endpoint");
	Check(Name, guest.Call(0, {}, {&name_output, 1}, result) == Disconnected, "stopped guest connection retained dispatch");
	Flags::KernelEventFlag missing = nullptr;
	Check(Name, Flags::KernelOpenEventFlag(&missing, flag_name.data()) != 0 && !missing,
	      "stopped service leaked discoverable named flag");
	client.Detach();
	// Preserve the existing generic IPMI behavior outside the exact NP service.
	Guest unrelated("NpManagerDifferentName");
	CheckOk(Name, unrelated.ConnectNow(result), "unrelated IPMI connect behavior changed");
}

void AdmissionBoundAndRollback() {
	Scope scope;
	CheckOk(Name, Service::StartUnavailableService(), "bounded service start");
	std::shared_ptr<Np::Endpoint> candidate;
	Flags::EventFlagFailForTest(Flags::EventFlagFailure::Prepare);
	Check(Name, Service::ConnectEndpoint(candidate) != 0 && !candidate, "failed named-flag creation accepted a connection");
	std::array<std::shared_ptr<Np::Endpoint>, 16> endpoints;
	for (auto& endpoint: endpoints) { CheckOk(Name, Service::ConnectEndpoint(endpoint), "bounded connection admission"); }
	Check(Name, Service::ConnectEndpoint(candidate) == Np::TransportBusy && !candidate, "connection quota not enforced");
	endpoints[0]->Disconnect();
	CheckOk(Name, Service::ConnectEndpoint(candidate), "closed connection slot reuse");
	Service::StopService();
	CheckOk(Name, Service::StartUnavailableService(), "restart broker");
	for (const auto& endpoint: endpoints) { Check(Name, !endpoint->Connected(), "old endpoint reconnected across generation"); }
	Check(Name, !candidate->Connected(), "old replacement connection survived service stop");
}

void RetirementRetry() {
	Scope scope;
	CheckOk(Name, Service::StartUnavailableService(), "retirement service start");
	std::shared_ptr<Np::Endpoint> endpoint;
	CheckOk(Name, Service::ConnectEndpoint(endpoint), "retirement endpoint");
	std::array<uint8_t, 32> name {};
	const auto reply = endpoint->Invoke(0, {}, name);
	Check(Name, reply.transport_result == 0 && reply.service_result == 0 && reply.written == name.size(),
	      "retirement flag discovery");
	Flags::EventFlagFailForTest(Flags::EventFlagFailure::Prepare);
	Check(Name, Service::StopService() != 0 && !Service::Running() && !endpoint->Connected(),
	      "cleanup failure was hidden or retained a live connection");
	Check(Name, Service::StartUnavailableService() == Np::TransportBusy,
	      "service restarted before retained kernel handles retired");
	CheckOk(Name, Service::StopService(), "retry retained creator cleanup");
	Flags::KernelEventFlag leaked = nullptr;
	Check(Name, Flags::KernelOpenEventFlag(&leaked, reinterpret_cast<const char*>(name.data())) != 0 && !leaked,
	      "cleanup retry lost the retained named flag");
	CheckOk(Name, Service::StartUnavailableService(), "restart after actual kernel cleanup");
}
} // namespace NpManagerServiceTest

void TestNpManagerService() {
	NpManagerServiceTest::ServiceAndGuest();
	NpManagerServiceTest::AdmissionBoundAndRollback();
	NpManagerServiceTest::RetirementRetry();
}
