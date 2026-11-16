#include "libs/dbPreparationNativeTransport.h"
#include "libs/dbRecoveryNativeTransport.h"
#include "libs/ipmiClientConfig.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef KYTY_TEST_DB_RECOVERY
namespace N = Libs::DbRecoveryNativeTransport;
namespace Other = Libs::DbPreparationNativeTransport;
constexpr uint32_t Query = 0x150011, Notify = 0x15000b;
constexpr size_t QuerySize = 12, NotifySize = 4;
#else
namespace N = Libs::DbPreparationNativeTransport;
namespace Other = Libs::DbRecoveryNativeTransport;
constexpr uint32_t Query = 0x160008, Notify = 0x160002;
constexpr size_t QuerySize = 24, NotifySize = 16;
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
	// These bytes belong only to the isolated callback fixture, not to a database
	// or a production recovery-result cache. The adapter must deliver them intact.
	std::array<uint8_t, QuerySize> native_result {};
	for (size_t i = 0; i < native_result.size(); ++i) { native_result[i] = static_cast<uint8_t>(i + 0x31); }
	std::array<uint8_t, NotifySize> event {};
	event[0] = 4;
	std::array<uint32_t, 2> key {0, 0xa};
	std::array<uint8_t, 0x158> config {};
	Libs::IpmiClientAbi::Construct(config.data());
	std::memcpy(config.data(), N::ServiceName, sizeof(N::ServiceName));
	uint32_t one = 1;
	std::memcpy(config.data() + 0x1c, &one, sizeof(one)); // one IPMI event
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> client_storage {}, session_storage {};
	std::array<uint64_t, 7> table {0, 0, 0x10, 0x18, 0x20, 0x28, 0x30};
	void* handler = table.data();
	N::ServerConfig cfg {0xf00, 0xf00, &handler, 0, {}, 0, 0};
	std::memcpy(cfg.name, N::ServiceName, sizeof(N::ServiceName));
	void* server = nullptr; void* client = nullptr; void* session = nullptr;
	CHECK(N::CreateClient(&client, config.data(), nullptr, client_storage.data()) == Error(0x4e));
	CHECK(client == nullptr); // No generic acknowledgement before owner installation.
	std::atomic_bool allow {true}, hold {false}, entered {false}, release {false}, done {false};
	std::atomic_bool no_reply {false};
	std::atomic<int> requests {0}, dispatch_result {0};
	int32_t native_error = 0;
	std::shared_ptr<void> dispatcher_pin;
	N::Hooks hooks;
	auto owner = std::make_shared<int>(1);
	std::weak_ptr<int> lifetime = owner;
	hooks.owner = owner;
	hooks.identity = [] { return identity; };
	hooks.copy_in = [](uint64_t p, void* out, size_t n) {
		if (n) { std::memcpy(out, reinterpret_cast<const void*>(p), n); } return 0;
	};
	hooks.copy_out = [](uint64_t p, const void* in, size_t n) {
		if (n) { std::memcpy(reinterpret_cast<void*>(p), in, n); } return 0;
	};
	hooks.authorize_registration = [](Identity id, bool server) { return id.pid == (server ? 100 : 200) ? 0 : 1; };
	hooks.authorize_connect = [&](Identity id, const Libs::LibKernel::Ipmi::ConnectionInfo& info) {
		CHECK(info.server_pid == 100 && info.server_config.cross_no_cap == 0);
		return id.pid == 200 && allow ? 0 : 1;
	};
	hooks.cooperate = [] { std::this_thread::yield(); return 0; };
	hooks.retain_dispatch_thread = [&](std::shared_ptr<void> pin) {
		CHECK(identity.pid == 100 && identity.tid == 2);
		dispatcher_pin = std::move(pin); return 0;
	};
	hooks.invoke_guest = [&](uint64_t entry, const uint64_t* args, size_t count, int64_t& result) {
		CHECK(identity.pid == 100 && identity.tid == 2); // Never run Core work on the UI caller.
		result = 0;
		if (entry == 0x20) {
			CHECK(count == 5);
			CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
		} else if (entry == 0x10) {
			CHECK(count == 7);
			++requests;
			if (no_reply) { return 0; } // Returning zero is NOT a native reply.
			const auto* in = reinterpret_cast<const N::DataInfo*>(args[3]);
			auto* out = reinterpret_cast<N::BufferInfo*>(args[5]);
			if (args[2] == Query) {
#ifdef KYTY_TEST_DB_RECOVERY
				CHECK(args[4] == 1 && in[0].size == 8 && std::memcmp(in[0].data, key.data(), 8) == 0);
#else
				CHECK(args[4] == 0);
#endif
				CHECK(args[6] == 1 && out[0].capacity == QuerySize);
				if (!native_error) {
					std::memcpy(out[0].data, native_result.data(), native_result.size());
					out[0].written = native_result.size();
				}
			} else {
				CHECK(args[2] == Notify && args[4] == 1 && args[6] == 0);
				CHECK(in[0].size == NotifySize && std::memcmp(in[0].data, event.data(), NotifySize) == 0);
				if (hold) {
					entered = true;
					while (!release) { std::this_thread::yield(); }
				}
				CHECK(Call(session, 8, uint32_t(0), uint64_t(4)) == 0);
			}
			CHECK(Call(session, 6, native_error, out, static_cast<uint32_t>(args[6])) == 0);
		} else {
			CHECK(entry == 0x28 && count == 2);
			CHECK(Call(session, 2) == 0);
		}
		return 0;
	};
	CHECK(N::Install(hooks) == 0 && Other::Install(hooks) == 0);
	hooks = {}; owner.reset();
	identity = {100, 1};
	CHECK(Other::CreateServer(&server, &cfg, nullptr, server_storage.data()) == Error(0x16));
	CHECK(server == nullptr); // Exact names and independent registries.
	CHECK(N::CreateServer(&server, &cfg, nullptr, server_storage.data()) == 0);
	identity = {200, 1};
	CHECK(N::CreateClient(&client, config.data(), nullptr, client_storage.data()) == 0);
	auto dispatch = [&] {
		identity = {100, 2}; std::array<uint8_t, 0x1000> work {};
		dispatch_result = Call(server, 2, work.data(), uint64_t(work.size()));
	};
	std::thread dispatcher(dispatch);
	int32_t accepted = 99;
	allow = false;
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == Error(1));
	CHECK(accepted == 99 && requests == 0);
	allow = true;
	CHECK(Call(client, 2, static_cast<void*>(nullptr), uint64_t(0), &accepted) == 0 && accepted == 0);
	std::array<uint8_t, QuerySize> output; output.fill(0xee);
	N::BufferInfo buffer {output.data(), output.size(), 77};
	N::DataInfo input {key.data(), sizeof(key)};
#ifdef KYTY_TEST_DB_RECOVERY
	constexpr uint32_t Inputs = 1;
#else
	constexpr uint32_t Inputs = 0;
#endif
	int32_t result = 99;
	CHECK(Call(client, 11, Query, &input, Inputs, &result, &buffer, uint32_t(1)) == 0);
	CHECK(result == 0 && buffer.written == QuerySize && output == native_result);
	native_error = static_cast<int32_t>(0x809c1145u);
	output.fill(0xee); buffer.written = 77;
	CHECK(Call(client, 11, Query, &input, Inputs, &result, &buffer, uint32_t(1)) == 0);
	CHECK(result == native_error && buffer.written == 0);
	for (auto byte: output) { CHECK(byte == 0xee); }
	native_error = 0;
	hold = true;
	std::thread notifier([&] {
		identity = {200, 3}; N::DataInfo notification {event.data(), event.size()};
		int32_t notified = 99;
		CHECK(Call(client, 11, Notify, &notification, uint32_t(1), &notified,
		           static_cast<N::BufferInfo*>(nullptr), uint32_t(0)) == 0 && notified == 0);
		done = true;
	});
	while (!entered) { std::this_thread::yield(); }
	CHECK(!done && N::Uninstall() == Error(0x10));
	uint64_t bits = 99;
	CHECK(Call(client, 4, uint32_t(0), uint64_t(4), uint32_t(2), &bits) == Error(0x10));
	CHECK(bits == 99); // Completion event cannot precede the native owner callback.
	release = true; notifier.join();
	CHECK(done && Call(client, 4, uint32_t(0), uint64_t(4), uint32_t(2), &bits) == 0 && bits == 4);
	no_reply = true; result = 99; buffer.written = 77;
	CHECK(Call(client, 11, Query, &input, Inputs, &result, &buffer, uint32_t(1)) == Error(0x59));
	dispatcher.join();
	CHECK(dispatch_result == Error(0x10) && result == 99 && buffer.written == 77);
	no_reply = false;
	// Resume solely to drain the fixture connection after the deliberately broken handler.
	std::thread cleanup(dispatch);
	CHECK(Call(client, 3) == 0 && Call(client, 0) == 0);
	identity = {100, 1}; CHECK(Call(server, 4) == 0); cleanup.join();
	CHECK(dispatch_result == 0 && Call(server, 0) == 0);
	CHECK(N::Uninstall() == Error(0x10)); // Dispatcher epilogue/native join pin still retained.
	dispatcher_pin.reset();
	CHECK(N::Uninstall() == 0 && Other::Uninstall() == 0 && lifetime.expired());
	std::cout << N::ServiceName << ": native output, notification, failure and owner-lifetime checks passed\n";
}
