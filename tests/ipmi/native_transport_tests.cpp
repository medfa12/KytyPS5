#include "libs/npNativeTransport.h"
#include "loader/npDispatcherExports.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace N = Libs::NpNativeTransport;
using Libs::LibKernel::Ipmi::Identity;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int Error(unsigned n) { return static_cast<int>(0x80020000u | n); }
thread_local Identity identity {200, 1};
template<class R = int, class... Args> R Call(void* object, size_t slot, Args... args) {
	using Function = R (KYTY_SYSV_ABI *)(void*, Args...);
	return reinterpret_cast<Function>((*static_cast<void***>(object))[slot])(object, args...);
}
void Put(std::array<uint8_t, 0x158>& bytes, size_t offset, uint64_t value, size_t width = 8) {
	for (size_t n = 0; n < width; ++n) { bytes[offset + n] = static_cast<uint8_t>(value >> (n * 8)); }
}

int main() {
	CHECK(!Loader::IsNpHttpPoolExport("fhJ5uKzcn0w") && !Loader::IsNpHttpPoolExport("EjMsfO3GCIA"));
	CHECK(Loader::IsNpDispatcherExport("fhJ5uKzcn0w") && Loader::IsNpDispatcherExport("EjMsfO3GCIA"));
	CHECK(Loader::IsNpDispatcherExport("PznfSvchYJ8") && !Loader::IsNpDispatcherExport("kBON3bAtfGs"));
	CHECK(!N::Installed());
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> client_storage {}, session_storage {};
	std::array<uint64_t, 7> handler_table {0,0,0x10,0x18,0x20,0x28,0x30};
	void* handler = handler_table.data();
	void* server = nullptr; void* client = nullptr; void* session = nullptr;
	std::atomic_bool reject {true}, hold_async {true}, async_entered {false}, sync_entered {false}, release_sync {false};
	std::atomic_bool forced {false}, allow_connect {true};
	std::atomic<uint64_t> fault_out {0};
	std::atomic<int> callbacks {0};
	N::Hooks hooks;
	hooks.owner = std::make_shared<int>(1);
	hooks.identity = [] { return identity; };
	hooks.copy_in = [](uint64_t p, void* out, size_t size) { std::memcpy(out, reinterpret_cast<void*>(p), size); return 0; };
	hooks.copy_out = [&](uint64_t p, const void* in, size_t size) {
		if (p == fault_out.load()) { fault_out = 0; return 14; }
		std::memcpy(reinterpret_cast<void*>(p), in, size); return 0;
	};
	hooks.authorize_registration = [](Identity id, bool is_server) { return id.pid == (is_server ? 100 : 200) ? 0 : 1; };
	hooks.authorize_connect = [&](Identity id, const Libs::LibKernel::Ipmi::ConnectionInfo& info) {
		CHECK(info.server_config.cross_no_cap == 1 && info.server_config.multi == 0);
		return id.pid == 200 && allow_connect ? 0 : 1;
	};
	hooks.cooperate = [] { std::this_thread::yield(); return 0; };
	hooks.invoke_guest = [&](uint64_t entry, const uint64_t* args, size_t count, int64_t& result) {
		CHECK(identity.pid == 100 && identity.tid == 2); // never run a handler on a client thread
		CHECK(args[0] == reinterpret_cast<uint64_t>(&handler));
		++callbacks; result = -777; // ignored for method dispatch; explicit replies below
		if (entry == 0x20) {
			CHECK(count == 5 && args[1] == reinterpret_cast<uint64_t>(server));
			uint64_t memory_size = 0; std::memcpy(&memory_size, reinterpret_cast<uint8_t*>(args[2]) + 0x148, 8);
			CHECK(memory_size == 0x10 && args[4] == 3);
			CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
			CHECK(Call(session, 3) == 200);
			result = reject.exchange(false) ? -42 : 0;
		} else if (entry == 0x10) {
			CHECK(count == 7 && args[1] == reinterpret_cast<uint64_t>(session));
			if (args[2] == 88) {
				sync_entered = true;
				while (!release_sync) { std::this_thread::yield(); }
				CHECK(Call(session, 6, int32_t(0), static_cast<const N::BufferInfo*>(nullptr), uint32_t(0)) == Error(3));
			} else {
				CHECK(args[2] == 3 && args[4] == 1 && args[6] == 2);
				auto* input = reinterpret_cast<N::DataInfo*>(args[3]); CHECK(input[0].size == 4);
				auto* output = reinterpret_cast<N::BufferInfo*>(args[5]);
				CHECK(output[0].capacity == 0x1e8 && output[1].capacity == 4);
				std::memset(output[0].data, 0x5a, 0x1e8); std::memset(output[1].data, 0, 4);
				output[0].written = 0x1e8; output[1].written = 4;
				CHECK(Call(session, 6, int32_t(-31), output, uint32_t(2)) == 0);
			}
		} else if (entry == 0x18) {
			CHECK(count == 6 && args[3] == 0x30040);
			async_entered = true;
			while (hold_async) { std::this_thread::yield(); }
			CHECK(Call(session, 7, uint32_t(0x30041), uint32_t(args[2]), int32_t(-42),
			           static_cast<const N::BufferInfo*>(nullptr), uint32_t(0)) == 0);
		} else {
			CHECK((entry == 0x28 || entry == 0x30) && count == 2);
			CHECK(Call(session, 3) == 200);
			CHECK(Call(session, 2) == 0); // handler owns close, then frees its backing
			if (entry == 0x30) { forced = true; }
		}
		return 0;
	};
	CHECK(N::Install(std::move(hooks)) == 0);
	identity = {100, 1};
	N::ServerConfig cfg {0x2000, 0x4000, &handler, 1, {}, 0, 0};
	std::memcpy(cfg.name, "SceNpMgrIpc", 12);
	CHECK(N::CreateServer(&server, &cfg, nullptr, server_storage.data()) == 0 && server == server_storage.data());
	CHECK(N::Uninstall() == Error(0x10));
	std::array<uint8_t, 0x158> client_cfg {};
	std::memcpy(client_cfg.data(), "SceNpMgrIpc", 12);
	Put(client_cfg, 0x20, 1, 4); Put(client_cfg, 0x28, 0x2000); Put(client_cfg, 0x30, 0x4000);
	Put(client_cfg, 0x38, 2, 4); Put(client_cfg, 0x40, 32); Put(client_cfg, 0x48, 32);
	identity = {200, 1};
	CHECK(N::CreateClient(&client, client_cfg.data(), nullptr, client_storage.data()) == 0 && client == client_storage.data());
	std::thread dispatcher([&] {
		identity = {100, 2}; std::array<uint8_t, 0x4100> work {};
		CHECK(Call(server, 2, work.data(), uint64_t(work.size())) == 0);
	});
	int32_t accepted = 123; const std::array<uint8_t, 3> argument {1,2,3};
	allow_connect = false;
	CHECK(Call(client, 2, argument.data(), uint64_t(3), &accepted) == Error(1) && accepted == 123 && callbacks == 0);
	allow_connect = true;
	CHECK(Call(client, 2, argument.data(), uint64_t(3), &accepted) == 0 && accepted == -42);
	// Reuse the exact callback-rejected storage: both Core and adapter records
	// must have rolled back without dereferencing the freed native object.
	CHECK(Call(client, 2, argument.data(), uint64_t(3), &accepted) == 0 && accepted == 0);
	// Shared implementation must not broaden NP's audited vtable surface.
	uint64_t event = 0xfeed; uint32_t timeout = 123;
	CHECK(Call(client, 4, uint32_t(0), uint64_t(1), uint32_t(2), &event) == Error(0x4e));
	CHECK(Call(client, 5, uint32_t(0), uint64_t(1), uint32_t(2), &event, &timeout) == Error(0x4e));
	CHECK(Call(session, 8, uint32_t(0), uint64_t(1)) == Error(0x4e));
	CHECK(event == 0xfeed && timeout == 123);
	uint32_t channel = 4; N::DataInfo input {&channel, 4};
	std::array<uint8_t, 0x1e8> batch {}; uint32_t remaining = 99;
	N::BufferInfo output[] {{batch.data(), batch.size(), 0}, {&remaining, 4, 0}};
	int32_t service = 123;
	CHECK(Call(client, 11, uint32_t(3), &input, uint32_t(1), &service, output, uint32_t(2)) == 0);
	CHECK(service == -31 && batch.front() == 0x5a && remaining == 0 && output[0].written == 0x1e8 && output[1].written == 4);
	int disconnected;
	do { disconnected = Call(client, 3); } while (disconnected == Error(0x10));
	CHECK(disconnected == 0);
	CHECK(Call(client, 2, argument.data(), uint64_t(3), &accepted) == 0 && accepted == 0);
	uint32_t ticket = 999;
	CHECK(Call(client, 12, uint32_t(0x30040), static_cast<const N::DataInfo*>(nullptr), uint32_t(0), &ticket, static_cast<const void*>(nullptr)) == 0 && ticket == 0);
	while (!async_entered) { std::this_thread::yield(); }
	CHECK(Call(client, 13, uint32_t(0x30041), ticket, &service, static_cast<N::BufferInfo*>(nullptr), uint32_t(0)) == Error(0x23));
	hold_async = false;
	int collected;
	do { collected = Call(client, 13, uint32_t(0x30041), ticket, &service, static_cast<N::BufferInfo*>(nullptr), uint32_t(0)); } while (collected == Error(0x23));
	CHECK(collected == 0 && service == -42);
	CHECK(Call(client, 13, uint32_t(0x30041), ticket, &service, static_cast<N::BufferInfo*>(nullptr), uint32_t(0)) == Error(0x16));
	// A service-result copy fault consumes the packet but does not free its slot.
	CHECK(Call(client, 12, uint32_t(0x30040), static_cast<const N::DataInfo*>(nullptr), uint32_t(0), &ticket, static_cast<const void*>(nullptr)) == 0);
	fault_out = reinterpret_cast<uint64_t>(&service);
	do { collected = Call(client, 13, uint32_t(0x30041), ticket, &service, static_cast<N::BufferInfo*>(nullptr), uint32_t(0)); } while (collected == Error(0x23));
	CHECK(collected == Error(14));
	CHECK(Call(client, 13, uint32_t(0x30041), ticket, &service, static_cast<N::BufferInfo*>(nullptr), uint32_t(0)) == Error(0x23));
	std::thread invoke([&] {
		identity = {200, 2}; int32_t result = 123;
		CHECK(Call(client, 11, uint32_t(88), static_cast<const N::DataInfo*>(nullptr), uint32_t(0), &result,
		           static_cast<N::BufferInfo*>(nullptr), uint32_t(0)) == Error(3));
		CHECK(result == 123);
	});
	while (!sync_entered) { std::this_thread::yield(); }
	CHECK(Call(client, 17) == 0);
	invoke.join(); CHECK(!forced); // native callback still owns the session
	CHECK(Call(client, 0) == Error(0x10));
	release_sync = true;
	while (!forced) { std::this_thread::yield(); }
	CHECK(Call(client, 0) == 0);
	identity = {100, 1};
	CHECK(Call(server, 4) == 0); dispatcher.join();
	CHECK(Call(server, 0) == 0 && N::Uninstall() == 0 && !N::Installed());
	std::cout << "Native-vtable NP adapter component checks passed (host fixture callbacks)\n";
}
