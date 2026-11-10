#ifdef KYTY_TEST_APPDB_TRANSPORT
#include "libs/appDbNativeTransport.h"
#else
#include "libs/loginMgrNativeTransport.h"
#endif
#include "libs/ipmiClientConfig.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef KYTY_TEST_APPDB_TRANSPORT
namespace N = Libs::AppDbNativeTransport;
constexpr int ServerPid = 303;
constexpr uint8_t CrossNoCap = 0;
#else
namespace N = Libs::LoginMgrNativeTransport;
constexpr int ServerPid = 100;
constexpr uint8_t CrossNoCap = 1;
#endif
using Libs::LibKernel::Ipmi::Identity;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int Error(unsigned n) { return static_cast<int>(0x80020000u | n); }
thread_local Identity identity {200, 1};
template<class... Args> int Call(void* object, size_t slot, Args... args) {
	using Function = int (KYTY_SYSV_ABI *)(void*, Args...);
	return reinterpret_cast<Function>((*static_cast<void***>(object))[slot])(object, args...);
}

int main() {
	std::array<uint8_t, 0x158> config {};
	Libs::IpmiClientAbi::Construct(config.data());
	std::memcpy(config.data(), N::ServiceName, sizeof(N::ServiceName));
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> client_storage {}, session_storage {};
	std::array<uint64_t, 7> handler_table {0, 0, 0x10, 0x18, 0x20, 0x28, 0x30};
	void* handler = handler_table.data();
	N::ServerConfig cfg {0xf00, 0xf00, &handler, CrossNoCap, {}, 0, 0};
	std::memcpy(cfg.name, N::ServiceName, sizeof(N::ServiceName));
	void* server = nullptr; void* client = nullptr; void* session = nullptr;
	std::atomic_bool parked {false}, cancel {false}, fail_publication {false};
	std::atomic_bool allow {true}, authorized {false}, entered {false}, release {false}, done {false};
	std::atomic<int> callbacks {0};
	int32_t accept_result = -42;
	N::Hooks hooks;
	hooks.owner = std::make_shared<int>(1);
	hooks.identity = [] { return identity; };
	hooks.copy_in = [](uint64_t p, void* out, size_t size) {
		std::memcpy(out, reinterpret_cast<void*>(p), size); return 0;
	};
	hooks.copy_out = [&](uint64_t p, const void* in, size_t size) {
		if (p == reinterpret_cast<uint64_t>(&server) && fail_publication.exchange(false)) {
			// Reenter before factory commit: neither immediate lookup nor the
			// already parked connector may see this provisional server.
			const auto saved = identity; identity = {200, 9}; int32_t sentinel = 71;
			CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &sentinel) == Error(2));
			CHECK(sentinel == 71 && !authorized && !done);
			identity = saved; return 14;
		}
		std::memcpy(reinterpret_cast<void*>(p), in, size); return 0;
	};
	hooks.authorize_registration = [](Identity id, bool is_server) { return id.pid == (is_server ? ServerPid : 200) ? 0 : 1; };
	hooks.authorize_connect = [&](Identity id, const Libs::LibKernel::Ipmi::ConnectionInfo& info) {
		CHECK(id.pid == 200 && info.server_pid == ServerPid); authorized = true;
		CHECK(info.server_config.cross_no_cap == CrossNoCap);
		return allow ? 0 : 1;
	};
	hooks.cooperate = [&] {
		if (identity.pid == 200) { parked = true; if (cancel) { return 0x55; } }
		std::this_thread::yield(); return 0;
	};
	hooks.invoke_guest = [&](uint64_t entry, const uint64_t* args, size_t count, int64_t& result) {
		CHECK(identity.pid == ServerPid && identity.tid == 2);
		++callbacks;
		if (entry == 0x20) {
			CHECK(count == 5 && args[1] == reinterpret_cast<uint64_t>(server));
			CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
			CHECK(Call(session, 3) == 200); // Actual peer, not server PID or compatibility PID100.
			entered = true;
			while (!release) { std::this_thread::yield(); }
			result = accept_result;
		} else {
			CHECK(entry == 0x28 && count == 2);
			CHECK(Call(session, 2) == 0); result = 0;
		}
		return 0;
	};
	CHECK(N::Install(std::move(hooks)) == 0);
	CHECK(N::CreateClient(&client, config.data(), nullptr, client_storage.data()) == 0);
	int32_t accepted = 123; uint32_t zero = 0, finite = 100;
	CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, &zero) == Error(2));
	CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, &finite) == Error(0x4e));
	CHECK(accepted == 123 && finite == 100 && zero == 0);
	std::thread cancelled([&] {
		identity = {200, 2};
		CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, static_cast<uint32_t*>(nullptr)) == Error(0x55));
	});
	while (!parked) { std::this_thread::yield(); }
	cancel = true; cancelled.join();
	CHECK(accepted == 123 && callbacks == 0 && !authorized);
	cancel = false; parked = false;
	std::thread connector([&] {
		identity = {200, 2};
		CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, static_cast<uint32_t*>(nullptr)) == 0);
		done = true;
	});
	while (!parked) { std::this_thread::yield(); }
	identity = {ServerPid, 1}; fail_publication = true;
	CHECK(N::CreateServer(&server, &cfg, nullptr, server_storage.data()) == Error(14));
	CHECK(server == nullptr && !authorized && !done && callbacks == 0);
	CHECK(N::CreateServer(&server, &cfg, nullptr, server_storage.data()) == 0);
	while (!authorized) { std::this_thread::yield(); }
	CHECK(!done && callbacks == 0); // Published but no dispatcher/acceptance yet.
	std::thread dispatcher([&] {
		identity = {ServerPid, 2}; std::array<uint8_t, 0x1000> work {};
		CHECK(Call(server, 2, work.data(), uint64_t(work.size())) == 0);
	});
	while (!entered) { std::this_thread::yield(); }
	CHECK(!done && Call(server, 4) == Error(0x10));
	release = true; connector.join();
	CHECK(accepted == -42 && callbacks == 1);
	identity = {200, 1}; allow = false; accepted = 456;
	CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, static_cast<uint32_t*>(nullptr)) == Error(1));
	CHECK(accepted == 456 && callbacks == 1);
	allow = true; accept_result = 0;
	CHECK(Call(client, 16, static_cast<void*>(nullptr), uint64_t(0), &accepted, static_cast<uint32_t*>(nullptr)) == 0);
	CHECK(accepted == 0 && callbacks == 2);
	CHECK(Call(client, 3) == 0 && Call(client, 0) == 0);
	identity = {ServerPid, 1}; CHECK(Call(server, 4) == 0); dispatcher.join();
	CHECK(Call(server, 0) == 0 && N::Uninstall() == 0);
	std::cout << "IPMI native slot16 publication/acceptance/lifetime checks passed (host callback fixture)\n";
}
