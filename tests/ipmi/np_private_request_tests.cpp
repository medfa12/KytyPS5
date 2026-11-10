// Real client/exports; the service is a deterministic fixture, not a PSN result.
#include "libs/npManagerClient.h"
#include "loader/runtimeNpManagerClient.h"
#include "loader/symbolDatabase.h"
#include "common/logging/log.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <set>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace N = Libs::NpManagerClient;
static std::atomic<std::shared_ptr<N::Client>> selected;
static thread_local bool admitted = false;
namespace Loader::RuntimeNpManagerClient {
std::shared_ptr<N::Client> AcquireQueryClient() noexcept { return admitted ? selected.load() : nullptr; }
std::optional<int> QueryLocalSigninState(int, uint32_t*) { return std::nullopt; }
}
namespace Loader { void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {} }
namespace Log { bool IsSilent() { return true; } void Write(fmt::text_style, std::string_view) {} }
namespace Libs { void PrintName(const char*, const char*, const char*) {} }

struct Endpoint: N::Endpoint {
	bool connected = true;
	int next = 7001, create_error = 0, transport_error = 0, delete_error = 0, abort_error = 0;
	int forced_id = -1;
	unsigned creates = 0, deletes = 0, aborts = 0;
	std::set<int> live;
	std::atomic_bool hold {false}, entered {false}, release {false};
	bool Connected() const noexcept override { return connected; }
	N::Reply Invoke(uint32_t method, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		if (method == 0) {
			CHECK(input.empty() && output.size() == 32);
			std::fill(output.begin(), output.end(), 0);
			std::memcpy(output.data(), "request-fixture", sizeof("request-fixture"));
			return {0, 0, 32};
		}
		CHECK(input.size() == 4 && output.empty() && (method == 6 || method == 7));
		int id; std::memcpy(&id, input.data(), 4);
		if (method == 6) {
			++deletes;
			if (delete_error) { return {0, delete_error, 0}; }
			return {0, live.erase(id) ? 0 : N::RequestNotFound, 0};
		}
		++aborts;
		return {0, abort_error ? abort_error : live.count(id) ? 0 : N::RequestNotFound, 0};
	}
	N::Reply InvokeInputs(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	                      std::span<uint8_t> output) noexcept override {
		CHECK(method == 5 && inputs.size() == 2 && output.empty());
		CHECK(inputs[0].size() == 16 && inputs[1].size() == 4);
		for (size_t i = 0; i < 16; ++i) { CHECK(inputs[0][i] == (i == 8 ? 4 : 0)); }
		for (auto b: inputs[1]) { CHECK(b == 0); }
		++creates;
		if (hold) { entered = true; while (!release) { std::this_thread::yield(); } }
		if (transport_error || create_error) { return {transport_error, create_error, 0}; }
		int id = forced_id >= 0 ? forced_id : next++;
		if (id > 0) { live.insert(id); }
		return {0, id, 0};
	}
	int OpenEvents(const char*) noexcept override { return 0; }
	int Poll(N::Event&) noexcept override { return 1; }
	void Disconnect() noexcept override { connected = false; live.clear(); }
};

void Lifecycle() {
	auto endpoint = std::make_shared<Endpoint>();
	auto client = std::make_shared<N::Client>();
	CHECK(client->CreateRequest() == N::NotInitialized);
	CHECK(client->Attach(endpoint) == 0); selected = client;
	CHECK(N::CreatePrivateRequest() == N::NotInitialized && endpoint->creates == 0);
	CHECK(N::DeletePrivateRequest(7001) == N::NotInitialized && endpoint->deletes == 0);
	CHECK(N::AbortPrivateRequest(7001) == N::NotInitialized && endpoint->aborts == 0);
	admitted = true;
	endpoint->create_error = N::OutOfMemory;
	CHECK(N::CreatePrivateRequest() == N::OutOfMemory);
	endpoint->transport_error = N::TransportIoError;
	CHECK(N::CreatePrivateRequest() == N::TransportIoError); // transport beats service
	endpoint->transport_error = endpoint->create_error = 0;
	for (int i = 0; i < 64; ++i) { CHECK(N::CreatePrivateRequest() == 7001 + i); }
	CHECK(endpoint->creates == 66 && endpoint->live.size() == 64);
	CHECK(N::CreatePrivateRequest() == N::RequestSlotsFull && endpoint->creates == 66);
	CHECK(N::AbortPrivateRequest(7001) == 0 && endpoint->live.count(7001));
	CHECK(N::CreatePrivateRequest() == N::RequestSlotsFull); // abort is not delete
	endpoint->abort_error = N::TransportBusy;
	CHECK(N::AbortPrivateRequest(7001) == N::TransportBusy);
	endpoint->abort_error = 0;
	endpoint->delete_error = N::TransportBusy;
	CHECK(N::DeletePrivateRequest(7001) == N::TransportBusy);
	CHECK(N::CreatePrivateRequest() == N::RequestSlotsFull); // failed delete retains ownership
	endpoint->delete_error = 0;
	CHECK(N::DeletePrivateRequest(7001) == 0);
	CHECK(N::DeletePrivateRequest(7001) == N::RequestNotFound);
	CHECK(N::CreatePrivateRequest() == 7065);
	const auto deletes = endpoint->deletes;
	client->Detach();
	CHECK(endpoint->deletes == deletes + 64 && endpoint->live.empty() && !endpoint->connected);
	CHECK(N::CreatePrivateRequest() == N::NotInitialized);
	auto replacement = std::make_shared<Endpoint>();
	CHECK(client->Attach(replacement) == 0 && N::CreatePrivateRequest() == 7001);
	selected.store({}); client.reset();
	CHECK(!replacement->connected && replacement->deletes == 1);
}

void InvalidIdsAndLease() {
	for (int id: {0, 7001}) {
		auto e = std::make_shared<Endpoint>(); N::Client c; CHECK(c.Attach(e) == 0);
		if (id) { CHECK(c.CreateRequest() == id); }
		e->forced_id = id;
		CHECK(c.CreateRequest() == N::TransportIoError && !e->connected);
	}
	auto e = std::make_shared<Endpoint>(); auto c = std::make_shared<N::Client>();
	CHECK(c->Attach(e) == 0); selected = c;
	std::weak_ptr<N::Client> retained = c;
	e->hold = true;
	std::thread call([&] { admitted = true; CHECK(N::CreatePrivateRequest() == 7001); });
	while (!e->entered) { std::this_thread::yield(); }
	selected.store({}); c.reset(); CHECK(!retained.expired());
	e->release = true; call.join();
	CHECK(retained.expired() && e->deletes == 1 && !e->connected);
}

struct TicketEndpoint: Endpoint {
	bool login = false, malformed = false, short_buffer = false;
	int bind_error = 0, submit_error = 0, result_error = 0, release_error = 0;
	uint64_t bound_mask = 0;
	unsigned bindings = 0, submissions = 0, results = 0, releases = 0;
	std::atomic_uint polls {0};
	std::atomic_bool completed {true};
	int poll_error = 0;
	bool RequestEventsAvailable() const noexcept override { return true; }
	int BindRequest(int request, uint64_t mask) noexcept override {
		CHECK(live.count(request) && mask && !(mask & (mask - 1)));
		++bindings; bound_mask = mask; return bind_error;
	}
	int PollRequest(uint64_t mask) noexcept override {
		CHECK(mask == bound_mask); ++polls;
		return poll_error ? poll_error : completed ? 0 : 1;
	}
	N::Reply Invoke(uint32_t method, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		if (method == 9) {
			CHECK(input.size() == 4 && output.empty()); ++releases;
			return {0, release_error, 0};
		}
		if (method == 7) { completed = true; }
		return Endpoint::Invoke(method, input, output);
	}
	N::Reply InvokeInputs(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	    std::span<uint8_t> output) noexcept override {
		if (method == 5) { return Endpoint::InvokeInputs(method, inputs, output); }
		CHECK(method == (login ? 0x88 : 0x65) && inputs.size() == 3 && output.empty());
		CHECK(inputs[0].size() == (login ? 4 : 8));
		int request = 0; std::memcpy(&request, inputs[0].data(), 4); CHECK(live.count(request));
		if (!login) { int user = 0; std::memcpy(&user, inputs[0].data() + 4, 4); CHECK(user == -1); }
		CHECK(inputs[1].size() == 8 && std::memcmp(inputs[1].data(), "fixture", 8) == 0);
		CHECK(inputs[2].size() == 3 && std::memcmp(inputs[2].data(), "{}", 3) == 0);
		++submissions; return {0, submit_error, 0};
	}
	N::Reply InvokeBuffers(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	    std::span<Output> outputs) noexcept override {
		CHECK(method == (login ? 0x89 : 0x66) && inputs.size() == 1 && inputs[0].size() == 24);
		CHECK(outputs.size() == 2 && outputs[0].bytes.size() == 0x801 && outputs[1].bytes.size() == 0x25);
		uint64_t size = 0; std::memcpy(&size, inputs[0].data() + 8, 8); CHECK(size == (short_buffer ? 2 : 32));
		std::memcpy(&size, inputs[0].data() + 16, 8); CHECK(size == 16);
		// Sentinels validate byte publication, not PSN authentication.
		std::memcpy(outputs[0].bytes.data(), "fixture-json", 13); outputs[0].written = 13;
		std::memcpy(outputs[1].bytes.data(), "fixture-id", 11); outputs[1].written = 11;
		if (malformed) { outputs[1].written = 0x26; }
		++results; return {0, result_error, outputs[0].written + outputs[1].written};
	}
	void WaitPolling() {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!polls.load()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
	}
};

void TicketsAndErrors() {
	for (bool login: {false, true}) {
		for (int failure = 0; failure < 8; ++failure) {
			auto e = std::make_shared<TicketEndpoint>(); auto c = std::make_shared<N::Client>();
			e->login = login; CHECK(c->Attach(e) == 0); selected = c; admitted = true;
			const int request = N::CreatePrivateRequest(); CHECK(request == 7001);
			std::array<char, 32> json; json.fill('J'); std::array<char, 16> correlation; correlation.fill('C');
			if (failure == 1) { e->bind_error = N::RequestAlreadyBound; }
			if (failure == 2) { e->submit_error = N::SignedOut; }
			if (failure == 3) { e->result_error = N::SignedOut; }
			if (failure == 4) { e->poll_error = N::TransportIoError; }
			if (failure == 5) { e->release_error = N::TransportIoError; }
			if (failure == 6) { e->malformed = true; }
			if (failure == 7) { e->short_buffer = true; }
			const size_t size = e->short_buffer ? 2 : json.size();
			const auto call = [&] {
				return login ? N::LoginCreateAuthenticationTicket(request, "fixture", "{}", json.data(), size, correlation.data(), correlation.size()) :
				    N::CreateAuthenticationTicket(request, -1, "fixture", "{}", json.data(), size, correlation.data(), correlation.size());
			};
			admitted = false; CHECK(call() == N::NotInitialized && e->bindings == 0); admitted = true;
			const int result = call();
			CHECK(result == (failure == 0 ? 0 : failure == 1 ? N::RequestAlreadyBound : failure <= 3 ? N::SignedOut : N::TransportIoError));
			CHECK(e->releases == (failure == 1 ? 0 : 1));
			CHECK(e->submissions == (failure == 1 ? 0 : 1));
			CHECK(e->results == (failure == 1 || failure == 2 || failure == 4 ? 0 : 1));
			if (!failure) {
				CHECK(std::memcmp(json.data(), "fixture-json", 13) == 0 && json[13] == 'J');
				CHECK(std::memcmp(correlation.data(), "fixture-id", 11) == 0 && correlation[11] == 'C');
			} else {
				CHECK(std::all_of(json.begin(), json.end(), [](char b) { return b == 'J'; }));
				CHECK(std::all_of(correlation.begin(), correlation.end(), [](char b) { return b == 'C'; }));
			}
			if (failure == 1) { e->bind_error = 0; CHECK(call() == 0); } // failed bind does not leak acquisition
			CHECK(c->CreateRequest() == (failure == 5 ? N::NotInitialized : 7002));
			selected.store({}); c->Detach();
		}
	}
}

void TicketWaitLifecycle() {
	for (int action = 0; action < 4; ++action) {
		auto e = std::make_shared<TicketEndpoint>(); auto c = std::make_shared<N::Client>();
		CHECK(c->Attach(e) == 0); selected = c; admitted = true;
		for (int i = 0; i < 64; ++i) { CHECK(c->CreateRequest() == 7001 + i); }
		e->completed = false;
		std::array<char, 32> json; json.fill('J'); std::array<char, 16> correlation; correlation.fill('C');
		auto pending = std::async(std::launch::async, [&] {
			admitted = true;
			return N::CreateAuthenticationTicket(7064, -1, "fixture", "{}", json.data(), json.size(), correlation.data(), correlation.size());
		});
		e->WaitPolling(); CHECK(e->bound_mask == (uint64_t {1} << 63));
		CHECK(N::CreateAuthenticationTicket(7064, -1, "fixture", "{}", json.data(), json.size(), correlation.data(), correlation.size()) == N::RequestAlreadyBound);
		CHECK(c->CreateRequest() == N::RequestSlotsFull);
		if (action == 0) { CHECK(c->AbortRequest(7064) == 0); }
		if (action == 1) { CHECK(c->DeleteRequest(7064) == 0); }
		if (action == 2) {
			c->Detach(); auto replacement = std::make_shared<Endpoint>(); CHECK(c->Attach(replacement) == 0);
			CHECK(c->CreateRequest() == 7001); // old wait cannot release this generation
		}
		if (action == 3) {
			selected.store({}); c.reset(); // export's shared owner lease survives the wait
			e->completed = true;
		}
		CHECK(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		CHECK(pending.get() == (action == 1 ? N::RequestNotFound : action == 2 ? N::NotInitialized : 0));
		CHECK(e->releases == (action == 2 ? 0 : 1));
		if (action == 1) { CHECK(c->CreateRequest() == 7065); }
		if (action == 2 || action == 3) { CHECK(!e->connected && e->live.empty()); }
		if (action == 1 || action == 2) { CHECK(json[0] == 'J' && correlation[0] == 'C'); }
		selected.store({}); if (c) { c->Detach(); }
	}
}

int main() {
	// A connected owner-neutral fixture must never become a production fallback.
	auto global = std::make_shared<Endpoint>(); CHECK(N::Session().Attach(global) == 0);
	Lifecycle(); InvalidIdsAndLease(); TicketsAndErrors(); TicketWaitLifecycle();
	CHECK(global->creates == 0); N::Session().Detach();
	std::cout << "NP private requests: owner routing, descriptor ABI, server IDs, limits, failures and lease cleanup passed\n";
}
