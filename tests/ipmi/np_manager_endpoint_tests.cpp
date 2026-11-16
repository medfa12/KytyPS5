#include "libs/npNativeManagerEndpoint.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>

#if KYTY_TEST_NP_ENDPOINT_CLIENT
#include "loader/runtimeNpManagerClient.h"
#include "loader/symbolDatabase.h"
#include "common/logging/log.h"
// Only loader ownership/logging are test seams. Client, endpoint, retained
// operations and server dispatcher/replies below use production code together.
namespace Loader::RuntimeNpManagerClient {
std::shared_ptr<Libs::NpManagerClient::Client> AcquireQueryClient() noexcept { return {}; }
std::optional<int> QueryLocalSigninState(int, uint32_t*) { return std::nullopt; }
}
namespace Loader { void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {} }
namespace Log { bool IsSilent() { return true; } void Write(fmt::text_style, std::string_view) {} }
namespace Libs { void PrintName(const char*, const char*, const char*) {} }
#endif

namespace E = Libs::NpNativeManagerEndpoint;
namespace N = Libs::NpNativeTransport;
namespace C = Libs::NpManagerClient;
using namespace Libs::LibKernel::Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int Guest(int e) { return static_cast<int>(0x80020000u | unsigned(e)); }
constexpr Identity Worker {200, 1}, Receiver {100, 2}, Server {100, 1};
thread_local Identity current = Worker;
struct Scope {
	Identity previous = current;
	explicit Scope(Identity id) { current = id; }
	~Scope() { current = previous; }
};
template<class... A> int Call(void* object, size_t slot, A... args) {
	using F = int (KYTY_SYSV_ABI *)(void*, A...);
	return reinterpret_cast<F>((*static_cast<void***>(object))[slot])(object, args...);
}

// The host fixture identities and guest callback hook are explicit test seams.
// Production must supply an actual guest worker; Endpoint never runs a handler.
struct Fixture {
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> session_storage {};
	std::array<uint64_t, 7> table {0, 0, 0x10, 0x18, 0x20, 0x28, 0x30};
	void* handler = table.data();
	void* server = nullptr;
	void* session = nullptr;
	std::shared_ptr<C::Endpoint> endpoint;
	std::shared_ptr<E::Coordinator> coordinator, retained;
	bool alive = true, signal = false, close_fails = false, malformed = false, reject = false;
	unsigned connects = 0, requests = 0, disconnects = 0, closes = 0, batches = 0, rearms = 0;
	uint32_t remaining = 0;
	int private_result = 7001;
	int ticket_result = 0;
	int bind_result = 0, create_flag_result = 0, clear_flag_result = 0, poll_flag_result = 0;
	bool delete_flag_fails = false, bad_binding = false;
	uint64_t request_bits = 0, binding_mask = 0;
	unsigned flag_creates = 0, flag_deletes = 0, flag_clears = 0, flag_polls = 0, releases = 0;
	std::function<void()> on_owner_check;
	Fixture(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
		N::Hooks h;
		h.owner = std::make_shared<int>(1); h.identity = [] { return current; };
		h.copy_in = [](uint64_t p, void* out, size_t n) { std::memcpy(out, reinterpret_cast<void*>(p), n); return 0; };
		h.copy_out = [](uint64_t p, const void* in, size_t n) { std::memcpy(reinterpret_cast<void*>(p), in, n); return 0; };
		h.authorize_registration = [](Identity, bool) { return 0; };
		h.authorize_connect = [](Identity id, const ConnectionInfo&) { CHECK(id == Worker); return 0; };
		h.cooperate = [] { CHECK(false); return 0; };
		h.retain_storage = [](uint64_t, size_t, std::shared_ptr<void>& out) { out = std::make_shared<int>(1); return 0; };
		h.invoke_guest = [&](uint64_t entry, const uint64_t* a, size_t, int64_t& result) {
			CHECK(current == Receiver); result = 0;
			if (entry == 0x20) {
				++connects; CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
				if (reject) { CHECK(Call(session, 2) == 0); result = -42; }
			} else if (entry == 0x10) {
				++requests;
				auto* out = reinterpret_cast<N::BufferInfo*>(a[5]);
				const auto method = uint32_t(a[2]);
				int32_t service = 0;
				if (method == 0) {
					CHECK(a[6] == 1 && out[0].capacity == 32);
					std::memset(out[0].data, 0, 32); std::memcpy(out[0].data, "real-fixture-flag", 18); out[0].written = 32;
				} else if (method == 3) {
					++batches; CHECK(a[6] == 2 && out[0].capacity == 0x1e8 && out[1].capacity == 4);
					CHECK(a[4] == 1);
					auto* in = reinterpret_cast<N::DataInfo*>(a[3]); CHECK(in[0].size == 4);
					std::memset(out[0].data, 0, 0x1e8);
					uint32_t count = 2; std::memcpy(out[0].data, &count, 4);
					static_cast<uint8_t*>(out[0].data)[8] = uint8_t(batches);
					static_cast<uint8_t*>(out[0].data)[32] = uint8_t(batches + 10);
					std::memcpy(out[1].data, &remaining, 4);
					out[0].written = malformed ? 8 : 0x1e8; out[1].written = 4;
				} else if (method == 5) {
					CHECK(a[4] == 2 && a[6] == 0);
					auto* in = reinterpret_cast<N::DataInfo*>(a[3]);
					CHECK(in[0].size == 16 && in[1].size == 4);
					for (size_t i = 0; i < 16; ++i) { CHECK(static_cast<const uint8_t*>(in[0].data)[i] == (i == 8 ? 4 : 0)); }
					for (size_t i = 0; i < 4; ++i) { CHECK(static_cast<const uint8_t*>(in[1].data)[i] == 0); }
					service = private_result;
				} else if (method == 8) {
					CHECK(a[4] == 2 && a[6] == 1 && out[0].capacity == 16);
					auto* in = reinterpret_cast<N::DataInfo*>(a[3]); CHECK(in[0].size == 16);
					CHECK(in[1].size == sizeof("SceNpMgrIpcC000000c8"));
					CHECK(std::memcmp(in[1].data, "SceNpMgrIpcC000000c8", in[1].size) == 0);
					int id = 0; std::memcpy(&id, in[0].data, 4); CHECK(id == 7001);
					std::memcpy(&binding_mask, static_cast<const uint8_t*>(in[0].data) + 8, 8);
					CHECK(binding_mask && !(request_bits & binding_mask)); // cleared before bind
					uint64_t alias = bad_binding ? 0 : 789;
					std::memcpy(out[0].data, &alias, 8);
					std::memcpy(static_cast<uint8_t*>(out[0].data) + 8, &binding_mask, 8);
					out[0].written = 16; service = bind_result;
				} else if (method == 0x66) {
					CHECK(a[4] == 1 && a[6] == 2);
					auto* in = reinterpret_cast<N::DataInfo*>(a[3]); CHECK(in[0].size == 24);
					CHECK((out[0].capacity == 16 && out[1].capacity == 8) ||
					      (out[0].capacity == 0x801 && out[1].capacity == 0x25));
					// Distinct short payloads, including their terminators. These are
					// transport sentinels, not fabricated authentication responses.
					std::memcpy(out[0].data, "json", 5); out[0].written = 5;
					std::memcpy(out[1].data, "id", 3); out[1].written = 3;
					service = ticket_result;
				} else if (method == 0x65) {
					CHECK(a[4] == 3 && a[6] == 0);
					auto* in = reinterpret_cast<N::DataInfo*>(a[3]);
					CHECK(in[0].size == 8 && in[1].size == 8 && in[2].size == 3);
					CHECK(std::memcmp(in[1].data, "fixture", 8) == 0 && std::memcmp(in[2].data, "{}", 3) == 0);
					request_bits |= binding_mask; // actual fixture completion, not client-generated readiness
				} else {
					CHECK(a[6] == 0);
					if (method == 4) { ++rearms; }
					if (method == 9) { ++releases; }
				}
				CHECK(Call(session, 6, service, out, uint32_t(a[6])) == 0);
			} else {
				CHECK(entry == 0x28 || entry == 0x30); ++disconnects; CHECK(Call(session, 2) == 0);
			}
			return 0;
		};
		CHECK(N::Install(std::move(h)) == 0);
		{
			Scope scope(Server); N::ServerConfig config {0x2000, 0x4000, &handler, 1, {}, 0, 0};
			std::memcpy(config.name, "SceNpMgrIpc", 12);
			CHECK(N::CreateServer(&server, &config, nullptr, server_storage.data()) == 0);
		}
		E::Hooks hooks; hooks.owner = std::make_shared<int>(2); hooks.identity = [] { return current; };
		hooks.owner_alive = [&] { if (on_owner_check) { on_owner_check(); } return alive; };
		hooks.retain = [&](std::shared_ptr<E::Coordinator> c) { retained = std::move(c); return true; };
		hooks.open_flag = [](const char* name, uintptr_t& out) {
			CHECK(current == Worker && std::strcmp(name, "real-fixture-flag") == 0); out = 123; return 0;
		};
		hooks.poll_flag = [&](uintptr_t alias, uint32_t channel) {
			CHECK(current == Worker && alias == 123 && (channel == 4 || channel == 7));
			if (!signal) { return 1; } signal = false; return 0;
		};
		hooks.close_flag = [&](uintptr_t alias) { CHECK(current == Worker && alias == 123); ++closes; return close_fails ? Guest(Busy) : 0; };
		hooks.create_request_flag = [&](const char* name, uintptr_t& creator) {
			CHECK(current == Worker && std::strcmp(name, "SceNpMgrIpcC000000c8") == 0);
			++flag_creates; if (!create_flag_result) { creator = 456; } return create_flag_result;
		};
		hooks.clear_request_flag = [&](uintptr_t creator, uint64_t keep) {
			CHECK(current == Worker && creator == 456); ++flag_clears;
			if (clear_flag_result) { return clear_flag_result; } request_bits &= keep; return 0;
		};
		hooks.poll_request_flag = [&](uintptr_t creator, uint64_t mask) {
			CHECK(current == Worker && creator == 456); ++flag_polls;
			if (poll_flag_result) { return poll_flag_result; }
			if ((request_bits & mask) != mask) { return 1; } request_bits &= ~mask; return 0;
		};
		hooks.delete_request_flag = [&](uintptr_t creator) {
			CHECK(current == Worker && creator == 456 && disconnects == 1); ++flag_deletes;
			return delete_flag_fails ? Guest(Busy) : 0;
		};
		CHECK(E::Create(std::move(hooks), endpoint, coordinator, timeout) == 0 && retained == coordinator);
	}
	~Fixture() {
		CHECK(retained->Done()); endpoint.reset(); coordinator.reset(); retained.reset();
		Scope scope(Server); CHECK(Call(server, 4) == 0); CHECK(Call(server, 0) == 0);
		CHECK(N::Uninstall() == 0);
	}
	int Dispatch() {
		Scope scope(Receiver); std::array<uint8_t, 0x4100> work {};
		return Call(server, 3, work.data(), uint64_t(work.size()));
	}
	void Connect() {
		CHECK(!endpoint->Connected()); CHECK(coordinator->Advance() == WouldBlock);
		CHECK(coordinator->Advance() == WouldBlock && connects == 0);
		CHECK(Dispatch() == 0); CHECK(coordinator->Advance() == 0); CHECK(endpoint->Connected());
	}
	template<class T> T Pump(std::future<T>& future) {
		const auto deadline = Clock::now() + std::chrono::seconds(2);
		// Wait for publication before dispatcher turns; later calls in a client
		// sequence may overlap those turns and must retain their original deadline.
		while (coordinator->QueuedJobs() != 1 && future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			CHECK(Clock::now() < deadline); std::this_thread::yield();
		}
		while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			CHECK(Clock::now() < deadline); coordinator->Advance();
			int r = Dispatch(); CHECK(r == 0 || r == Guest(WouldBlock));
			std::this_thread::yield();
		}
		return future.get();
	}
	void Invoke(uint32_t method, uint32_t channel = 4) {
		std::array<uint8_t, 8> input {}; uint32_t capacity = 20;
		std::memcpy(input.data(), &channel, 4); std::memcpy(input.data() + 4, &capacity, 4);
		auto f = std::async(std::launch::async, [&] { return endpoint->Invoke(method, {input.data(), method == 1 ? 8u : 4u}, {}); });
		auto reply = Pump(f); CHECK(reply.transport_result == 0 && reply.service_result == 0);
	}
	void Events() {
		std::array<uint8_t, 32> output {};
		auto f = std::async(std::launch::async, [&] { return endpoint->Invoke(0, {}, output); });
		CHECK(Pump(f).written == 32);
		auto open = std::async(std::launch::async, [&] { return endpoint->OpenEvents(reinterpret_cast<char*>(output.data())); });
		CHECK(Pump(open) == 0); Invoke(1);
	}
	void Finish() {
		endpoint->Disconnect();
		for (unsigned i = 0; i < 20 && !retained->Done(); ++i) {
			retained->Advance(); int r = Dispatch(); CHECK(r == 0 || r == Guest(WouldBlock));
		}
		CHECK(retained->Done());
	}
	void WaitQueued(size_t count) {
		const auto deadline = Clock::now() + std::chrono::seconds(1);
		while (coordinator->QueuedJobs() != count) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
	}
};

void PacketsAndInvalidation() {
	Fixture f; f.Connect(); f.Events();
	C::Event event; f.signal = true;
	CHECK(f.coordinator->Advance() == WouldBlock);
	for (int i = 0; i < 5; ++i) { CHECK(f.endpoint->Poll(event) == 1); CHECK(f.coordinator->Advance() == WouldBlock); }
	CHECK(f.batches == 0); CHECK(f.Dispatch() == 0); CHECK(f.coordinator->Advance() == 0);
	CHECK(f.endpoint->Poll(event) == 0 && event.channel == 4 && event.size == 20 && event.payload[0] == 1);
	f.Invoke(2); CHECK(f.endpoint->Poll(event) == 1); // second old-generation record discarded
	f.Invoke(1); CHECK(f.endpoint->Poll(event) == 1);
	f.remaining = 1; f.signal = true;
	CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0); CHECK(f.coordinator->Advance() == 0);
	f.remaining = 0; // remaining count, not a second flag signal, drives next fetch
	CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0); CHECK(f.coordinator->Advance() == 0);
	unsigned count = 0; while (f.endpoint->Poll(event) == 0) { ++count; }
	CHECK(count == 4 && f.batches == 3 && f.rearms == 0);
	for (int i = 0; i < 5; ++i) { CHECK(f.coordinator->Advance() == 0); CHECK(f.endpoint->Poll(event) == 1); }
	f.Invoke(4); CHECK(f.rearms == 1);
	f.close_fails = true; f.endpoint->Disconnect(); f.coordinator->Advance();
	CHECK(f.Dispatch() == 0); f.coordinator->Advance(); CHECK(!f.coordinator->Done() && f.closes >= 2);
	f.close_fails = false; f.Finish(); CHECK(f.disconnects == 1);
}

void TicketResultDescriptors() {
	Fixture f; f.Connect();
	std::array<uint8_t, 24> header {};
	const std::array<std::span<const uint8_t>, 1> inputs {header};
	std::array<uint8_t, 16> json; json.fill(0xa5);
	std::array<uint8_t, 8> correlation; correlation.fill(0x5a);
	std::array<C::Endpoint::Output, 2> outputs {{{json, 99}, {correlation, 99}}};
	auto result = std::async(std::launch::async, [&] { return f.endpoint->InvokeBuffers(0x66, inputs, outputs); });
	const auto reply = f.Pump(result);
	CHECK(!reply.transport_result && !reply.service_result && reply.written == 8);
	CHECK(outputs[0].written == 5 && outputs[1].written == 3);
	CHECK(std::memcmp(json.data(), "json", 5) == 0 && std::memcmp(correlation.data(), "id", 3) == 0);
	CHECK(std::all_of(json.begin() + 5, json.end(), [](auto b) { return b == 0xa5; }));
	CHECK(std::all_of(correlation.begin() + 3, correlation.end(), [](auto b) { return b == 0x5a; }));
	json.fill(0xa5); correlation.fill(0x5a);
	f.ticket_result = C::SignedOut;
	auto failed = std::async(std::launch::async, [&] { return f.endpoint->InvokeBuffers(0x66, inputs, outputs); });
	const auto error = f.Pump(failed);
	CHECK(!error.transport_result && error.service_result == C::SignedOut && !error.written);
	CHECK(!outputs[0].written && !outputs[1].written);
	CHECK(std::all_of(json.begin(), json.end(), [](auto b) { return b == 0xa5; }));
	CHECK(std::all_of(correlation.begin(), correlation.end(), [](auto b) { return b == 0x5a; }));
	std::array<uint8_t, 0x4000> large {};
	std::array<C::Endpoint::Output, 2> oversized {{{large, 99}, {correlation, 99}}};
	CHECK(f.endpoint->InvokeBuffers(0x66, inputs, oversized).transport_result == Guest(Unsupported));
	CHECK(!oversized[0].written && !oversized[1].written && f.requests == 2);
	f.Finish();
}

void PrivateRequestDescriptors() {
	Fixture f; f.Connect();
	std::array<uint8_t, 16> header {}; header[8] = 4;
	std::array<uint8_t, 4> payload {};
	const std::array<std::span<const uint8_t>, 2> inputs {header, payload};
	auto create = std::async(std::launch::async, [&] { return f.endpoint->InvokeInputs(5, inputs, {}); });
	f.WaitQueued(1);
	// Public input lifetime ends at staging; the guest worker must see its copy.
	header.fill(0xff); payload.fill(0xff);
	const auto reply = f.Pump(create);
	CHECK(reply.transport_result == 0 && reply.service_result == 7001 && reply.written == 0);
	CHECK(f.requests == 1);
	header.fill(0); header[8] = 4; payload.fill(0);
	f.private_result = C::OutOfMemory;
	auto rejected = std::async(std::launch::async, [&] { return f.endpoint->InvokeInputs(5, inputs, {}); });
	const auto error = f.Pump(rejected);
	CHECK(error.transport_result == 0 && error.service_result == C::OutOfMemory);
	std::array<uint8_t, 0x2000> large {};
	const std::array<std::span<const uint8_t>, 2> too_large {large, payload};
	CHECK(f.endpoint->InvokeInputs(5, too_large, {}).transport_result == Guest(Unsupported));
	CHECK(f.endpoint->InvokeInputs(1, inputs, {}).transport_result == Guest(Invalid));
	CHECK(f.requests == 2 && f.coordinator->QueuedJobs() == 0);
	f.Finish();
}

void RequestCompletionFlags() {
	Fixture f; f.Connect(); CHECK(f.endpoint->RequestEventsAvailable());
	constexpr uint64_t high = uint64_t {1} << 63;
	f.request_bits = high | 4;
	auto bind = std::async(std::launch::async, [&] { return f.endpoint->BindRequest(7001, high); });
	CHECK(f.Pump(bind) == 0 && f.binding_mask == high && f.request_bits == 4);
	CHECK(f.flag_creates == 1 && f.flag_clears == 1);
	auto pending = std::async(std::launch::async, [&] { return f.endpoint->PollRequest(high); });
	CHECK(f.Pump(pending) == 1 && f.flag_polls == 1);
	// Polling leaves the native worker available for unrelated requests.
	f.Invoke(4); CHECK(f.rearms == 1);
	f.request_bits |= high;
	auto done = std::async(std::launch::async, [&] { return f.endpoint->PollRequest(high); });
	CHECK(f.Pump(done) == 0 && f.request_bits == 4);
	f.bind_result = C::RequestAlreadyBound;
	auto duplicate = std::async(std::launch::async, [&] { return f.endpoint->BindRequest(7001, 2); });
	CHECK(f.Pump(duplicate) == C::RequestAlreadyBound && f.flag_creates == 1);
	CHECK(f.endpoint->BindRequest(7001, 3) == C::InvalidArgument);
	CHECK(f.endpoint->PollRequest(0) == C::InvalidArgument);
	f.delete_flag_fails = true; f.endpoint->Disconnect(); f.coordinator->Advance();
	CHECK(f.flag_deletes == 0); CHECK(f.Dispatch() == 0); f.coordinator->Advance();
	CHECK(f.flag_deletes == 1 && !f.coordinator->Done());
	f.delete_flag_fails = false; f.Finish(); CHECK(f.flag_deletes == 2);
}

void RequestFlagFailures() {
	for (int failure = 0; failure < 4; ++failure) {
		Fixture f; f.Connect();
		if (failure == 0) { f.create_flag_result = Guest(Busy); }
		if (failure == 1) { f.clear_flag_result = Guest(Invalid); }
		if (failure == 2) { f.bad_binding = true; }
		auto bind = std::async(std::launch::async, [&] { return f.endpoint->BindRequest(7001, 1); });
		const int result = f.Pump(bind);
		CHECK(result == (failure == 0 ? Guest(Busy) : failure == 1 ? Guest(Invalid) : failure == 2 ? Guest(BadMessage) : 0));
		if (failure == 3) {
			f.poll_flag_result = Guest(Invalid);
			auto poll = std::async(std::launch::async, [&] { return f.endpoint->PollRequest(1); });
			CHECK(f.Pump(poll) == Guest(Invalid));
		}
		f.Finish(); CHECK(f.flag_deletes == (failure == 0 ? 0 : 1));
	}
}

void RequestBindTimeout() {
	Fixture f(std::chrono::milliseconds(100)); f.Connect();
	auto bind = std::async(std::launch::async, [&] { return f.endpoint->BindRequest(7001, uint64_t {1} << 63); });
	f.WaitQueued(1); CHECK(f.coordinator->Advance() == WouldBlock);
	CHECK(bind.get() == Guest(TimedOut) && f.flag_creates == 1);
	f.coordinator->Advance(); CHECK(!f.coordinator->Done() && !f.flag_deletes);
	CHECK(f.Dispatch() == 0); // late method8 is drained before creator deletion
	f.Finish(); CHECK(f.requests == 1 && f.flag_deletes == 1);
}

void TimeoutAndLateAcceptance() {
	Fixture f(std::chrono::milliseconds(10));
	CHECK(f.coordinator->Advance() == WouldBlock);
	// No dispatcher turn: connect remains truly pending, never fabricated success.
	const auto deadline = Clock::now() + std::chrono::milliseconds(15);
	while (Clock::now() < deadline) { std::this_thread::yield(); }
	f.coordinator->Advance(); CHECK(!f.endpoint->Connected() && !f.coordinator->Done());
	f.endpoint.reset(); f.coordinator.reset(); // registry retains original ticket
	CHECK(f.Dispatch() == 0 && f.connects == 1);
	f.retained->Advance(); CHECK(f.Dispatch() == 0); f.retained->Advance();
	CHECK(f.retained->Done() && f.disconnects == 1);
}

void InvokeTimeoutDoesNotCancel() {
	for (bool multiple: {false, true}) {
		Fixture f(std::chrono::milliseconds(100)); f.Connect();
		std::array<uint8_t, 32> output; output.fill(0xa5);
		std::array<uint8_t, 24> input {};
		const std::array<std::span<const uint8_t>, 1> inputs {input};
		std::array<C::Endpoint::Output, 2> outputs {{{{output.data(), 16}, 99}, {{output.data() + 16, 8}, 99}}};
		auto pending = std::async(std::launch::async, [&] {
			return multiple ? f.endpoint->InvokeBuffers(0x66, inputs, outputs) : f.endpoint->Invoke(0, {}, output);
		});
		f.WaitQueued(1); CHECK(f.coordinator->Advance() == WouldBlock);
		// Deliberately withhold dispatcher turns until public observation expires.
		CHECK(pending.get().transport_result == Guest(TimedOut));
		CHECK(output[0] == 0xa5 && f.requests == 0 && !f.endpoint->Connected());
		f.coordinator->Advance(); CHECK(!f.coordinator->Done());
		CHECK(f.Dispatch() == 0 && f.requests == 1); // original queued mutation still exists
		f.Finish(); CHECK(f.requests == 1 && f.disconnects == 1);
		CHECK(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xa5; }));
		if (multiple) { CHECK(!outputs[0].written && !outputs[1].written); }
	}
}

void MalformedAndOwnerLoss() {
	Fixture f; f.Connect(); f.Events(); f.malformed = true; f.signal = true;
	CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0);
	CHECK(f.coordinator->Advance() == BadMessage && !f.endpoint->Connected());
	f.alive = false; CHECK(f.coordinator->Advance() == NotConnected && !f.coordinator->Done());
	f.alive = true; f.Finish();
}

void RejectionAndForeignWorker() {
	{
		Fixture f; f.reject = true;
		CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0);
		CHECK(f.coordinator->Advance() == NotConnected && !f.endpoint->Connected());
		f.Finish(); CHECK(f.connects == 1 && f.disconnects == 0);
	}
	{
		Fixture f; f.Connect();
		{ Scope foreign({200, 9}); CHECK(f.coordinator->Advance() == Invalid); }
		CHECK(!f.endpoint->Connected() && !f.coordinator->Done());
		f.Finish(); CHECK(f.disconnects == 1);
	}
}

void ResubscribeDuringPendingFetch() {
	Fixture f; f.Connect(); f.Events(); f.signal = true;
	CHECK(f.coordinator->Advance() == WouldBlock); // old generation method3 pending
	std::array<uint32_t, 2> input {4, 20};
	auto unsubscribe = std::async(std::launch::async, [&] {
		return f.endpoint->Invoke(2, {reinterpret_cast<const uint8_t*>(input.data()), 4}, {});
	});
	f.WaitQueued(1); // invalidation occurs at admission, before the native reply
	auto subscribe = std::async(std::launch::async, [&] {
		const auto deadline = Clock::now() + std::chrono::seconds(1);
		for (;;) {
			auto reply = f.endpoint->Invoke(1, {reinterpret_cast<const uint8_t*>(input.data()), 8}, {});
			if (reply.transport_result != Guest(Busy)) { return reply; }
			CHECK(Clock::now() < deadline); std::this_thread::yield();
		}
	});
	f.WaitQueued(2);
	CHECK(f.Dispatch() == 0); CHECK(f.coordinator->Advance() == 0);
	C::Event event; CHECK(f.endpoint->Poll(event) == 1); // stale fetch discarded
	CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0);
	CHECK(f.coordinator->Advance() == 0);
	CHECK(unsubscribe.get().transport_result == 0);
	CHECK(f.coordinator->Advance() == WouldBlock); CHECK(f.Dispatch() == 0);
	CHECK(f.coordinator->Advance() == 0);
	CHECK(subscribe.get().transport_result == 0);
	f.signal = true; CHECK(f.coordinator->Advance() == WouldBlock);
	CHECK(f.Dispatch() == 0); CHECK(f.coordinator->Advance() == 0);
	CHECK(f.endpoint->Poll(event) == 0 && event.payload[0] == 2);
	CHECK(f.endpoint->Poll(event) == 0 && event.payload[0] == 12);
	CHECK(f.endpoint->Poll(event) == 1 && f.batches == 2);
	f.Finish();
}

void LockContentionDoesNotExtendDeadline() {
	Fixture f(std::chrono::milliseconds(100)); f.Connect();
	std::array<uint8_t, 32> output; output.fill(0xa5);
	auto pending = std::async(std::launch::async, [&] { return f.endpoint->Invoke(0, {}, output); });
	f.WaitQueued(1);
	std::promise<void> entered, release;
	auto released = release.get_future().share();
	// Deliberate fixture-only contract violation holds the coordinator lock. A
	// public observer must still expire, and Disconnect must not wait for it.
	f.on_owner_check = [&] { entered.set_value(); released.wait(); };
	auto worker = std::async(std::launch::async, [&] { return f.coordinator->Advance(); });
	CHECK(entered.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
	auto public_calls = std::async(std::launch::async, [&] {
		CHECK(f.endpoint->Connected());
		C::Event event; CHECK(f.endpoint->Poll(event) == 1);
	});
	CHECK(public_calls.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
	public_calls.get();
	CHECK(pending.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
	CHECK(pending.get().transport_result == Guest(TimedOut));
	CHECK(!f.endpoint->Connected());
	CHECK(output[0] == 0xa5); // no worker/public buffer race after timeout
	auto disconnect = std::async(std::launch::async, [&] { f.endpoint->Disconnect(); });
	CHECK(disconnect.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
	disconnect.get();
	release.set_value(); worker.get(); f.on_owner_check = {};
	CHECK(f.requests == 0); f.Finish();
}

void AdmissionTimeoutAndRetirement() {
	for (bool retire: {false, true}) {
		Fixture f(std::chrono::milliseconds(100)); f.Connect();
		std::promise<void> entered, release;
		auto released = release.get_future().share();
		f.on_owner_check = [&] { entered.set_value(); released.wait(); };
		auto worker = std::async(std::launch::async, [&] { return f.coordinator->Advance(); });
		CHECK(entered.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
		std::array<uint8_t, 32> output; output.fill(0xa5);
		auto pending = std::async(std::launch::async, [&] { return f.endpoint->Invoke(0, {}, output); });
		CHECK(pending.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
		if (retire) { f.endpoint->Disconnect(); }
		CHECK(pending.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
		CHECK(pending.get().transport_result == Guest(retire ? NotConnected : TimedOut));
		CHECK(f.coordinator->QueuedJobs() == 0 && f.requests == 0);
		CHECK(f.endpoint->Connected() == !retire);
		CHECK(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xa5; }));
		release.set_value(); worker.get(); f.on_owner_check = {};
		if (!retire) { f.Invoke(4); CHECK(f.requests == 1); } // no late timed-out discovery
		f.Finish();
	}
}

#if KYTY_TEST_NP_ENDPOINT_CLIENT
void ClientLifecycleDuringWorkerTurn() {
	// Exercise real attachment, request creation and authentication-ticket flow
	// while Advance holds its mutex. The ticket gets a real service failure from
	// the fixture; no token/account or positive authentication reply is supplied.
	for (int operation = 0; operation < 3; ++operation) {
		Fixture f; f.Connect(); C::Client client;
		if (operation > 0) {
			auto attach = std::async(std::launch::async, [&] { return client.Attach(f.endpoint); });
			CHECK(f.Pump(attach) == 0);
		}
		if (operation > 1) {
			auto create = std::async(std::launch::async, [&] { return client.CreateRequest(); });
			CHECK(f.Pump(create) == 7001);
			f.ticket_result = C::SignedOut;
		}
		std::promise<void> entered, release;
		auto released = release.get_future().share();
		f.on_owner_check = [&] { entered.set_value(); released.wait(); };
		auto worker = std::async(std::launch::async, [&] { return f.coordinator->Advance(); });
		CHECK(entered.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
		std::array<char, 32> json; json.fill('J'); std::array<char, 16> correlation; correlation.fill('C');
		auto call = std::async(std::launch::async, [&] {
			if (operation == 0) { return client.Attach(f.endpoint); }
			if (operation == 1) { return client.CreateRequest(); }
			return client.AuthenticationTicket(false, 7001, -1, "fixture", "{}",
			    json.data(), json.size(), correlation.data(), correlation.size());
		});
		CHECK(call.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
		CHECK(f.endpoint->Connected() && f.coordinator->QueuedJobs() == 0);
		release.set_value(); worker.get(); f.on_owner_check = {};
		CHECK(f.Pump(call) == (operation == 0 ? 0 : operation == 1 ? 7001 : C::SignedOut));
		CHECK(f.requests == (operation == 0 ? 1 : operation == 1 ? 2 : 6));
		CHECK(f.releases == (operation == 2 ? 1 : 0));
		CHECK(std::all_of(json.begin(), json.end(), [](char b) { return b == 'J'; }));
		CHECK(std::all_of(correlation.begin(), correlation.end(), [](char b) { return b == 'C'; }));
		auto detach = std::async(std::launch::async, [&] { client.Detach(); return 0; });
		CHECK(f.Pump(detach) == 0); f.Finish();
	}
}

int main() {
	ClientLifecycleDuringWorkerTurn();
	std::cout << "native NP client/endpoint attachment, request and ticket contention tests passed\n";
}
#else
int main() {
	RequestCompletionFlags(); RequestFlagFailures(); RequestBindTimeout();
	TicketResultDescriptors();
	PrivateRequestDescriptors();
	PacketsAndInvalidation(); TimeoutAndLateAcceptance(); InvokeTimeoutDoesNotCancel(); MalformedAndOwnerLoss();
	RejectionAndForeignWorker();
	ResubscribeDuringPendingFetch(); LockContentionDoesNotExtendDeadline();
	AdmissionTimeoutAndRetirement();
	std::cout << "native NP manager endpoint tests passed\n";
}
#endif
