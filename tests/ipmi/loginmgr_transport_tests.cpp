#include "libs/loginMgrNativeTransport.h"
#include "libs/ipmiClientConfig.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace L = Libs::LoginMgrNativeTransport;
namespace N = Libs::NpNativeTransport;
using Libs::LibKernel::Ipmi::Identity;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int Error(unsigned n) { return static_cast<int>(0x80020000u | n); }
thread_local Identity identity {200, 1};
template<class R = int, class... Args> R Call(void* object, size_t slot, Args... args) {
	using Function = R (KYTY_SYSV_ABI *)(void*, Args...);
	return reinterpret_cast<Function>((*static_cast<void***>(object))[slot])(object, args...);
}

int main() {
	std::array<uint8_t, 0x158> config {};
	Libs::IpmiClientAbi::Construct(config.data());
	std::memcpy(config.data(), L::ServiceName, sizeof(L::ServiceName));
	uint32_t one = 1; std::memcpy(config.data() + 0x1c, &one, 4);
	std::array<uint8_t, 0x1020> backing; backing.fill(0xab);
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> session_storage {};
	std::array<uint64_t, 7> handler_table {0, 0, 0x10, 0x18, 0x20, 0x28, 0x30};
	void* handler = handler_table.data();
	L::ServerConfig server_config {0xf00, 0xf00, &handler, 1, {}, 0, 0};
	std::memcpy(server_config.name, L::ServiceName, sizeof(L::ServiceName));
	void* server = nullptr; void* client = reinterpret_cast<void*>(0x1234); void* session = nullptr;
	CHECK(!L::Installed() && !N::Installed());
	CHECK(L::CreateClient(&client, config.data(), nullptr, backing.data() + 16) == Error(0x4e));
	CHECK(client == reinterpret_cast<void*>(0x1234));
	std::atomic<uint64_t> fault_out {0};
	std::atomic_bool accept {false}, allow_connect {true}, capability {true};
	std::atomic_bool fail_session_output {false}, hold_request {false}, request_entered {false}, release_request {false};
	std::atomic_bool event_parked {false}, cancel_wait {false}, forced {false};
	std::atomic<int> callbacks {0};
	// This is callback-fixture state, never a production LoginMgr flag cache.
	uint32_t fixture_flags = 0xa0;
	L::Hooks hooks;
	std::shared_ptr<void> dispatcher_pin;
	hooks.retain_dispatch_thread = [&](std::shared_ptr<void> pin) {
		CHECK(identity.pid == 100 && identity.tid == 2);
		dispatcher_pin = std::move(pin);
		return 0;
	};
	auto owner = std::make_shared<int>(7); std::weak_ptr<int> retained = owner;
	hooks.owner = owner; owner.reset();
	hooks.identity = [] { return identity; };
	hooks.copy_in = [](uint64_t p, void* out, size_t size) {
		if (!p && size) { return 14; }
		std::memcpy(out, reinterpret_cast<const void*>(p), size); return 0;
	};
	hooks.copy_out = [&](uint64_t p, const void* in, size_t size) {
		if (p == fault_out.load()) { fault_out = 0; return 14; }
		if (!p && size) { return 14; }
		std::memcpy(reinterpret_cast<void*>(p), in, size); return 0;
	};
	hooks.authorize_registration = [](Identity id, bool is_server) { return id.pid == (is_server ? 100 : 200) ? 0 : 1; };
	hooks.authorize_connect = [&](Identity id, const Libs::LibKernel::Ipmi::ConnectionInfo& info) {
		CHECK(info.client_config.event_count == 1 && info.client_config.sync_slots == 1);
		CHECK(info.client_config.input_limit == 0xf00 && info.server_config.cross_no_cap == 1);
		return id.pid == 200 && allow_connect ? 0 : 1;
	};
	hooks.cooperate = [&] {
		if (identity.pid == 200 && identity.tid == 3) {
			event_parked = true;
			if (cancel_wait.exchange(false)) { return 0x55; }
		}
		std::this_thread::yield(); return 0;
	};
	hooks.invoke_guest = [&](uint64_t entry, const uint64_t* args, size_t count, int64_t& result) {
		CHECK(identity.pid == 100 && identity.tid == 2);
		CHECK(args[0] == reinterpret_cast<uint64_t>(&handler));
		++callbacks; result = -999;
		if (entry == 0x20) {
			CHECK(count == 5 && args[1] == reinterpret_cast<uint64_t>(server));
			CHECK(args[3] == 0 && args[4] == 0);
			uint32_t events = 0; uint64_t size = 0;
			std::memcpy(&events, reinterpret_cast<const uint8_t*>(args[2]) + 0x38, 4);
			std::memcpy(&size, reinterpret_cast<const uint8_t*>(args[2]) + 0x148, 8);
			CHECK(events == 1 && size == 0x10);
			if (fail_session_output.exchange(false)) {
				fault_out = reinterpret_cast<uint64_t>(&session);
				CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == Error(14));
				result = -42; return 0;
			}
			CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
			CHECK(Call(session, 3) == 200);
			CHECK(Call(session, 8, uint32_t(0), uint64_t(0x100000002)) == 0);
			result = accept ? 0 : -42;
		} else if (entry == 0x10) {
			CHECK(count == 7 && args[2] == 0x30023 && args[4] == 1 && args[6] == 1);
			CHECK(Call(session, 3) == 200);
			auto* in = reinterpret_cast<L::DataInfo*>(args[3]);
			auto* out = reinterpret_cast<L::BufferInfo*>(args[5]);
			CHECK(in->size == 12 && out->capacity == 4);
			if (hold_request) {
				request_entered = true;
				while (!release_request) { std::this_thread::yield(); }
				CHECK(Call(session, 6, int32_t(0), out, uint32_t(1)) == Error(3));
			} else if (!capability) {
				out->written = 0;
				CHECK(Call(session, 6, int32_t(0x813e0005u), out, uint32_t(1)) == 0);
			} else {
				std::array<uint32_t, 3> input; std::memcpy(input.data(), in->data, 12);
				CHECK(input[0] == 0x10000001);
				fixture_flags = (fixture_flags & ~input[1]) | input[2];
				std::memcpy(out->data, &fixture_flags, 4); out->written = 4;
				CHECK(Call(session, 6, int32_t(0), out, uint32_t(1)) == 0);
			}
		} else {
			CHECK((entry == 0x28 || entry == 0x30) && count == 2);
			CHECK(Call(session, 3) == 200 && Call(session, 2) == 0);
			if (entry == 0x30) { forced = true; }
		}
		return 0;
	};
	CHECK(L::Install(hooks) == 0 && L::Install(hooks) == Error(0x10));
	CHECK(N::Install(hooks) == 0); // Independent instance, exact service names.
	hooks = {}; // The installed runtimes must retain their own lifetime pins.
	identity = {100, 1};
	CHECK(N::CreateServer(&server, &server_config, nullptr, server_storage.data()) == Error(0x16));
	CHECK(server == nullptr);
	CHECK(L::CreateServer(&server, &server_config, nullptr, server_storage.data()) == 0);
	CHECK(L::Uninstall() == Error(0x10) && !retained.expired());
	identity = {200, 1};
	CHECK(N::CreateClient(&client, config.data(), nullptr, backing.data() + 16) == Error(0x16));
	fault_out = reinterpret_cast<uint64_t>(&client);
	CHECK(L::CreateClient(&client, config.data(), nullptr, backing.data() + 16) == Error(14));
	CHECK(client == reinterpret_cast<void*>(0x1234));
	CHECK(L::CreateClient(&client, config.data(), nullptr, backing.data() + 16) == 0);
	void* duplicate = reinterpret_cast<void*>(0x5678);
	CHECK(L::CreateClient(&duplicate, config.data(), nullptr, backing.data() + 16) == Error(0x10));
	CHECK(duplicate == reinterpret_cast<void*>(0x5678));
	for (size_t i = 0; i < 16; ++i) { CHECK(backing[i] == 0xab && backing[0x1010 + i] == 0xab); }
	std::thread dispatcher([&] {
		identity = {100, 2}; std::array<uint8_t, 0x1000> work {};
		CHECK(Call(server, 2, work.data(), uint64_t(work.size())) == 0);
	});
	int32_t accepted = 123;
	allow_connect = false;
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == Error(1));
	CHECK(accepted == 123 && callbacks == 0);
	allow_connect = true;
	fail_session_output = true;
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == 0 && accepted == -42);
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == 0 && accepted == -42);
	accept = true;
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == 0 && accepted == 0);
	identity = {100, 1}; CHECK(Call(server, 4) == Error(0x10)); identity = {200, 1};
	uint64_t bits = 0xfeed;
	CHECK(Call(client, 4, uint32_t(0), uint64_t(2), uint32_t(0x22), &bits) == 0 && bits == 0x100000002);
	bits = 0xfeed;
	CHECK(Call(client, 4, uint32_t(0), uint64_t(2), uint32_t(2), &bits) == Error(0x10) && bits == 0xfeed);
	CHECK(Call(client, 4, uint32_t(1), uint64_t(2), uint32_t(2), &bits) == Error(0x16) && bits == 0xfeed);
	uint32_t timeout = 100;
	CHECK(Call(client, 5, uint32_t(0), uint64_t(4), uint32_t(2), &bits, &timeout) == Error(0x3c));
	CHECK(bits == 0xfeed && timeout == 100);
	std::thread waiter([&] {
		identity = {200, 3}; uint64_t observed = 77;
		CHECK(Call(client, 5, uint32_t(0), uint64_t(4), uint32_t(0x12), &observed, static_cast<uint32_t*>(nullptr)) == 0);
		CHECK(observed == 0x100000004);
	});
	while (!event_parked) { std::this_thread::yield(); }
	identity = {100, 1}; CHECK(Call(session, 8, uint32_t(0), uint64_t(4)) == 0); identity = {200, 1};
	waiter.join();
	event_parked = false;
	std::thread abandoned([&] {
		identity = {200, 3}; uint64_t observed = 77;
		CHECK(Call(client, 5, uint32_t(0), uint64_t(8), uint32_t(2), &observed, static_cast<uint32_t*>(nullptr)) == Error(0x55));
		CHECK(observed == 77);
	});
	while (!event_parked) { std::this_thread::yield(); }
	cancel_wait = true; abandoned.join();
	std::array<uint32_t, 3> input {0x10000001, 0xf0, 0x105};
	L::DataInfo in {input.data(), 12}; uint32_t flags = 77; L::BufferInfo out {&flags, 4, 0}; int32_t service = 123;
	capability = false;
	CHECK(Call(client, 11, uint32_t(0x30023), &in, uint32_t(1), &service, &out, uint32_t(1)) == 0);
	CHECK(service == int32_t(0x813e0005u) && flags == 77 && fixture_flags == 0xa0);
	capability = true;
	CHECK(Call(client, 11, uint32_t(0x30023), &in, uint32_t(1), &service, &out, uint32_t(1)) == 0);
	CHECK(service == 0 && flags == 0x105 && out.written == 4); // Value outside mask preserved.
	int disconnected;
	do { disconnected = Call(client, 3); } while (disconnected == Error(0x10));
	CHECK(disconnected == 0); // Abandoned waiter did not leave a permanent Busy.
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == 0);
	hold_request = true;
	std::thread invoking([&] {
		identity = {200, 4}; uint32_t value = 88; L::BufferInfo output {&value, 4, 55}; int32_t result = 99;
		CHECK(Call(client, 11, uint32_t(0x30023), &in, uint32_t(1), &result, &output, uint32_t(1)) == Error(3));
		CHECK(value == 88 && result == 99 && output.written == 55);
	});
	while (!request_entered) { std::this_thread::yield(); }
	event_parked = false;
	std::thread cancelled([&] {
		identity = {200, 3}; uint64_t value = 91;
		CHECK(Call(client, 5, uint32_t(0), uint64_t(8), uint32_t(2), &value, static_cast<uint32_t*>(nullptr)) == Error(0x55));
		CHECK(value == 91);
	});
	while (!event_parked) { std::this_thread::yield(); }
	CHECK(Call(client, 17) == 0);
	cancelled.join(); invoking.join();
	CHECK(!forced && Call(client, 0) == Error(0x10));
	identity = {100, 1}; CHECK(Call(server, 0) == Error(0x10) && Call(server, 4) == Error(0x10)); identity = {200, 1};
	release_request = true;
	while (!forced) { std::this_thread::yield(); }
	CHECK(Call(client, 0) == 0);
	identity = {100, 1}; CHECK(Call(server, 4) == 0); dispatcher.join();
	CHECK(Call(server, 0) == 0 && dispatcher_pin && L::Uninstall() == Error(0x10));
	// A returned dispatcher is insufficient: the actual pthread join releases
	// this pin in production. Model that explicit release after our join.
	dispatcher_pin.reset();
	CHECK(L::Uninstall() == 0);
	CHECK(!retained.expired() && N::Uninstall() == 0 && retained.expired());
	std::cout << "LoginMgr isolated native-vtable transport/events/lifetime checks passed (host callback fixture)\n";
}
