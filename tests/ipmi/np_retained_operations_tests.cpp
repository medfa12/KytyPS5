#include "libs/npNativeTransport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace N = Libs::NpNativeTransport;
namespace R = N::Retained;
namespace L = N::Lifecycle;
using namespace Libs::LibKernel::Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int Guest(int error) { return static_cast<int>(0x80020000u | unsigned(error)); }
constexpr Identity Client {200, 1}, Receiver {100, 2}, Server {100, 1};
thread_local Identity current = Client;
struct IdentityScope {
	Identity previous = current;
	explicit IdentityScope(Identity id) { current = id; }
	~IdentityScope() { current = previous; }
};
template<class... Args> int Call(void* object, size_t slot, Args... args) {
	using Function = int (KYTY_SYSV_ABI *)(void*, Args...);
	return reinterpret_cast<Function>((*static_cast<void***>(object))[slot])(object, args...);
}

// Explicit dispatcher turns make lack of fabricated readiness deterministic.
// Hooks are fixture callbacks, not evidence of original ShellCore execution.
struct Fixture {
	std::array<uint64_t, 6> server_storage {};
	std::array<uint64_t, 2> session_storage {};
	std::array<uint64_t, 7> table {0, 0, 0x10, 0x18, 0x20, 0x28, 0x30};
	void* handler = table.data();
	void* server = nullptr;
	void* session = nullptr;
	std::shared_ptr<int> owner = std::make_shared<int>(1);
	std::weak_ptr<int> owner_probe = owner;
	bool allow_registration = true, allow_connect = true;
	bool reject = false, malformed = false, fail_request = false, hold_reply = false;
	std::atomic_int connects {0}, requests {0}, disconnects {0}, forced {0}, authorizations {0};
	Bytes expected_argument, expected_input;
	std::mutex mutex;
	std::condition_variable changed;
	bool published = false, release = false;
	std::atomic_int server_pins {0}, session_pins {0}, work_pins {0};
	bool deny_storage = false, empty_storage_lease = false;
	bool retain_dispatch = false, deny_dispatch = false;
	std::shared_ptr<void> dispatch_lifetime;
	// Explicitly simulated provider evidence, never kernel/native-thread proof.
	N::DispatcherReceipt receipt {1, Receiver, std::make_shared<int>(7)};
	N::DispatcherProof proof;
	int capture_error = 0, inspect_error = 0, captures = 0;
	std::function<void()> capture_action, inspect_action;
	struct Pin {
		std::atomic_int& count;
		explicit Pin(std::atomic_int& c): count(c) { ++count; }
		~Pin() { --count; }
	};

	explicit Fixture(bool create_server = true, bool receipts = false) {
		CHECK(!N::Installed());
		N::Hooks hooks;
		hooks.owner = owner;
		hooks.identity = [] { return current; };
		hooks.copy_in = [](uint64_t p, void* out, size_t size) {
			std::memcpy(out, reinterpret_cast<void*>(p), size); return 0;
		};
		hooks.copy_out = [](uint64_t p, const void* in, size_t size) {
			std::memcpy(reinterpret_cast<void*>(p), in, size); return 0;
		};
		hooks.authorize_registration = [&](Identity id, bool is_server) {
			return allow_registration && id.pid == (is_server ? Server.pid : Client.pid) ? 0 : 1;
		};
		hooks.authorize_connect = [&](Identity id, const ConnectionInfo& info) {
			++authorizations;
			CHECK(id == Client && info.server_pid == Server.pid);
			CHECK(info.server_config.cross_no_cap == 1 && info.server_config.multi == 0);
			CHECK(info.client_config.sync_slots == 1 && info.client_config.event_count == 0);
			CHECK(info.client_config.input_limit == 0x2000 && info.client_config.output_limit == 0x4000);
			return allow_connect ? 0 : 1;
		};
		hooks.cooperate = [] { CHECK(false); return 0; }; // retained calls and TryDispatch must never wait
		hooks.retain_dispatch_thread = [&](std::shared_ptr<void> lifetime) {
			if (deny_dispatch) { return int(Busy); }
			if (retain_dispatch) { dispatch_lifetime = std::move(lifetime); }
			return 0;
		};
		if (receipts) {
			hooks.capture_dispatch_thread = [&](std::shared_ptr<void> lifetime, N::DispatcherReceipt& out) {
				CHECK(current == Receiver && server_pins == 1 && work_pins == 1);
				++captures; dispatch_lifetime = std::move(lifetime);
				if (capture_action) { capture_action(); }
				out = receipt; return capture_error;
			};
			hooks.inspect_dispatch_thread = [&](const N::DispatcherReceipt& bound, N::DispatcherProof& out) {
				CHECK(bound.generation == 1 && bound.identity == Receiver && bound.token);
				if (inspect_action) { inspect_action(); }
				out = proof; return inspect_error;
			};
		}
		hooks.retain_storage = [&](uint64_t address, size_t size, std::shared_ptr<void>& lease) {
			if (deny_storage) { return int(Busy); }
			if (empty_storage_lease) { return 0; }
			if (address == reinterpret_cast<uint64_t>(server_storage.data())) {
				CHECK(size == 0x30); lease = std::make_shared<Pin>(server_pins);
			} else if (address == reinterpret_cast<uint64_t>(session_storage.data())) {
				CHECK(size == 0x10); lease = std::make_shared<Pin>(session_pins);
			} else {
				CHECK(size == 0x4100); lease = std::make_shared<Pin>(work_pins);
			}
			return 0;
		};
		hooks.invoke_guest = [&](uint64_t entry, const uint64_t* args, size_t count, int64_t& result) {
			CHECK(current == Receiver);
			CHECK(args[0] == reinterpret_cast<uint64_t>(&handler));
			CHECK(server_pins == 1 && work_pins == 1);
			result = 777; // never a synthesized method response
			if (entry == 0x20) {
				++connects;
				CHECK(count == 5 && args[1] == reinterpret_cast<uint64_t>(server));
				CHECK(args[4] == expected_argument.size());
				if (args[4]) { CHECK(std::memcmp(reinterpret_cast<void*>(args[3]), expected_argument.data(), args[4]) == 0); }
				CHECK(Call(server, 5, &session, static_cast<void*>(nullptr), session_storage.data()) == 0);
				CHECK(Call(session, 3) == Client.pid);
				if (reject) {
					CHECK(Call(session, 2) == 0);
					CHECK(session_pins == 1); // callback-local close cannot release native storage yet
				}
				result = reject ? -42 : 0;
			} else if (entry == 0x10) {
				++requests;
				CHECK(count == 7 && args[1] == reinterpret_cast<uint64_t>(session));
				CHECK(args[4] == (expected_input.empty() ? 0u : 1u));
				if (args[4]) {
					auto* in = reinterpret_cast<N::DataInfo*>(args[3]);
					CHECK(in[0].size == expected_input.size());
					CHECK(std::memcmp(in[0].data, expected_input.data(), in[0].size) == 0);
				}
				if (fail_request) { return int(BadMessage); }
				auto* out = reinterpret_cast<N::BufferInfo*>(args[5]);
				CHECK(args[2] == 3 && args[6] == 2);
				CHECK(out[0].capacity == 0x1e8 && out[1].capacity == 4);
				std::memset(out[0].data, 0x5a, 0x1e8);
				std::memset(out[1].data, 0, 4);
				out[0].written = 0x1e8; out[1].written = 4;
				CHECK(Call(session, 6, int32_t(-31), out, uint32_t(malformed ? 0 : 2)) == 0);
				if (hold_reply) {
					std::unique_lock<std::mutex> lock(mutex);
					published = true; changed.notify_all();
					changed.wait(lock, [&] { return release; });
				}
			} else {
				CHECK((entry == 0x28 || entry == 0x30) && count == 2);
				++disconnects;
				if (entry == 0x30) { ++forced; }
				CHECK(Call(session, 3) == Client.pid);
				CHECK(Call(session, 2) == 0);
				CHECK(session_pins == 1); // still executing the disconnect callback
			}
			return 0;
		};
		CHECK(N::Install(std::move(hooks)) == 0);
		if (create_server) {
			IdentityScope identity(Server);
			N::ServerConfig config {0x2000, 0x4000, &handler, 1, {}, 0, 0};
			std::memcpy(config.name, "SceNpMgrIpc", 12);
			CHECK(N::CreateServer(&server, &config, nullptr, server_storage.data()) == 0);
		}
	}
	~Fixture() {
		if (server) {
			IdentityScope identity(Server);
			CHECK(Call(server, 4) == 0);
			CHECK(Call(server, 0) == 0);
		}
		CHECK(server_pins == 0 && session_pins == 0 && work_pins == 0);
		owner.reset();
		CHECK(!owner_probe.expired());
		if (dispatch_lifetime) {
			CHECK(N::Uninstall() == Guest(Busy)); // Run returned and all objects are gone; guest epilogue still pins provider.
			dispatch_lifetime.reset(); // Fixture models actual successful pthread join.
		}
		CHECK(N::Uninstall() == 0);
		CHECK(owner_probe.expired());
	}
	void Dispatch(int expected = 0) {
		IdentityScope identity(Receiver);
		std::array<uint8_t, 0x4100> work {};
		CHECK(Call(server, 3, work.data(), uint64_t(work.size())) == expected);
		CHECK(work_pins == 0);
	}
	R::Connection Accepted() {
		R::Connection c = 0;
		CHECK(R::Register(c) == 0 && c != 0);
		CHECK(R::BeginConnect(c) == 0);
		Dispatch();
		int32_t accepted = 123;
		CHECK(R::PollConnect(c, accepted) == 0 && accepted == 0);
		return c;
	}
	void Finish(R::Connection c) {
		CHECK(R::Retire(c) == WouldBlock);
		Dispatch();
		CHECK(R::RetryRetirements() == 0);
		CHECK(R::Retire(c) == NotFound);
	}
};

void AbsentAndStale() {
	R::Connection old = 0;
	{
		Fixture f(false);
		{
			IdentityScope identity({0, 0});
			CHECK(R::Register(old) == Invalid && old == 0);
		}
		f.allow_registration = false;
		CHECK(R::Register(old) == 1 && old == 0);
		f.allow_registration = true;
		CHECK(R::Register(old) == 0);
		CHECK(N::Uninstall() == Guest(Busy)); // no server: retained client alone pins runtime
		CHECK(R::BeginConnect(old) == NotFound);
		CHECK(R::Retire(old) == 0);
	}
	{
		Fixture f(false);
		R::Connection next = 0;
		CHECK(R::Register(next) == 0 && next != old);
		CHECK(R::Retire(old) == NotFound);
		CHECK(R::Retire(next) == 0);
	}
}

void StorageAdmission() {
	Fixture f(false);
	IdentityScope identity(Server);
	N::ServerConfig config {0x2000, 0x4000, &f.handler, 1, {}, 0, 0};
	std::memcpy(config.name, "SceNpMgrIpc", 12);
	void* output = reinterpret_cast<void*>(0x1234);
	f.server_storage.fill(0x5555);
	f.deny_storage = true;
	CHECK(N::CreateServer(&output, &config, nullptr, f.server_storage.data()) == Guest(Busy));
	CHECK(output == reinterpret_cast<void*>(0x1234) && f.server_storage[0] == 0x5555 && f.server_pins == 0);
	f.deny_storage = false; f.empty_storage_lease = true;
	CHECK(N::CreateServer(&output, &config, nullptr, f.server_storage.data()) == Guest(BadMessage));
	CHECK(output == reinterpret_cast<void*>(0x1234) && f.server_storage[0] == 0x5555 && f.server_pins == 0);
	f.empty_storage_lease = false;
	CHECK(N::CreateServer(&f.server, &config, nullptr, f.server_storage.data()) == 0 && f.server_pins == 1);
	f.deny_storage = true;
	f.Dispatch(Guest(Busy)); // no dispatcher admission when its work cannot be retained
	f.deny_storage = false;
	f.deny_dispatch = true;
	f.Dispatch(Guest(Busy));
	f.deny_dispatch = false;
	f.retain_dispatch = true;
	f.Dispatch();
	CHECK(f.dispatch_lifetime != nullptr);
}

void StalledAndLate(bool reject) {
	Fixture f;
	f.reject = reject;
	R::Connection c = 0, duplicate = 0;
	CHECK(R::Register(c) == 0);
	CHECK(R::Register(duplicate) == Busy && duplicate == 0);
	f.allow_connect = false;
	CHECK(R::BeginConnect(c) == 1 && f.connects == 0);
	f.allow_connect = true;
	CHECK(R::BeginConnect(c, Bytes(0x2001)) == Invalid);
	Bytes argument {1, 2, 3}; f.expected_argument = argument;
	CHECK(R::BeginConnect(c, argument) == 0);
	argument.assign(3, 9); // retained request must own its original bytes
	CHECK(R::BeginConnect(c) == Busy);
	int32_t accepted = 123;
	for (int i = 0; i < 100; ++i) {
		CHECK(R::PollConnect(c, accepted) == WouldBlock && accepted == 123);
	}
	CHECK(f.connects == 0 && f.authorizations == 2);
	for (Identity wrong: {Identity {200, 2}, Identity {201, 1}, Identity {0, 0}}) {
		IdentityScope identity(wrong);
		CHECK(R::BeginConnect(c) == Invalid);
		CHECK(R::PollConnect(c, accepted) == Invalid && accepted == 123);
		CHECK(R::Retire(c) == Invalid);
	}
	CHECK(R::Retire(c) == WouldBlock);
	CHECK(R::RetryRetirements() == WouldBlock);
	CHECK(N::Uninstall() == Guest(Busy));
	CHECK(R::Register(duplicate) == Busy);
	CHECK(R::PollConnect(c, accepted) == Busy && accepted == 123);
	{
		IdentityScope identity({200, 2});
		CHECK(R::RetryRetirements() == Busy);
	}
	f.Dispatch(); // only now is acceptance/rejection actually produced
	CHECK(f.connects == 1 && f.disconnects == 0);
	if (reject) {
		CHECK(R::RetryRetirements() == 0);
	} else {
		CHECK(R::RetryRetirements() == WouldBlock);
		CHECK(R::RetryRetirements() == WouldBlock);
		f.Dispatch();
		CHECK(f.disconnects == 1 && f.forced == 0);
		CHECK(R::RetryRetirements() == 0);
	}
	CHECK(R::Retire(c) == NotFound);
}

void TwoOutputsAndDisconnect() {
	Fixture f;
	const auto c = f.Accepted();
	f.expected_input = {4, 0, 0, 0};
	std::vector<Bytes> inputs {f.expected_input};
	std::vector<uint64_t> capacities {0x1e8, 4};
	CHECK(R::BeginInvoke(c, 3, inputs, {0x4000}) == NoMemory); // transfer prefix counts too
	CHECK(R::BeginInvoke(c, 3, inputs, capacities) == 0);
	inputs[0].assign(4, 9); capacities.assign(2, 1);
	CHECK(R::BeginInvoke(c, 3, {}, {}) == Busy);
	Result reply; reply.value = 0xfeed;
	CHECK(R::PollInvoke(c) == WouldBlock);
	CHECK(R::ConsumeInvoke(c, reply) == WouldBlock && reply.value == 0xfeed);
	CHECK(R::BeginDisconnect(c) == Busy);
	f.Dispatch();
	CHECK(f.requests == 1);
	for (int i = 0; i < 3; ++i) { CHECK(R::PollInvoke(c) == 0); }
	{
		IdentityScope identity({200, 2});
		CHECK(R::BeginInvoke(c, 3, {}, {}) == Invalid);
		CHECK(R::PollInvoke(c) == Invalid);
		CHECK(R::ConsumeInvoke(c, reply) == Invalid && reply.value == 0xfeed);
		CHECK(R::BeginDisconnect(c) == Invalid);
	}
	CHECK(R::ConsumeInvoke(c, reply) == 0 && reply.method_result == -31);
	CHECK(reply.outputs.size() == 2 && reply.outputs[0] == Bytes(0x1e8, 0x5a) && reply.outputs[1] == Bytes(4));
	CHECK(R::ConsumeInvoke(c, reply) == Invalid);
	CHECK(R::BeginDisconnect(c) == 0);
	CHECK(R::BeginDisconnect(c) == Busy);
	int32_t result = 123;
	CHECK(R::PollDisconnect(c, result) == WouldBlock && result == 123);
	{
		IdentityScope identity({200, 2});
		CHECK(R::PollDisconnect(c, result) == Invalid && result == 123);
	}
	f.Dispatch();
	CHECK(f.disconnects == 1);
	CHECK(R::PollDisconnect(c, result) == 0 && result == 0);
	CHECK(R::Retire(c) == 0);
}

void AbandonedInvokeAndReceiverLease() {
	Fixture f;
	L::ServerLease server_lease;
	{
		IdentityScope identity(Server);
		CHECK(L::AcquireServer(reinterpret_cast<uint64_t>(f.server), f.server_storage[1], server_lease) == 0);
	}
	const auto c = f.Accepted();
	f.hold_reply = true;
	CHECK(R::BeginInvoke(c, 3, {}, {0x1e8, 4}) == 0);
	CHECK(R::Retire(c) == WouldBlock); // public deadline abandons observation, not request
	CHECK(R::RetryRetirements() == WouldBlock && f.requests == 0);
	CHECK(R::BeginInvoke(c, 3, {}, {}) == Busy);
	std::thread dispatcher([&] { f.Dispatch(); });
	{
		std::unique_lock<std::mutex> lock(f.mutex);
		f.changed.wait(lock, [&] { return f.published; });
	}
	// Reply can drain before the handler returns; receiver lease still forbids
	// disconnect and native backing retirement. No caller slot is leaked.
	CHECK(R::RetryRetirements() == Busy);
	CHECK(R::RetryRetirements() == Busy);
	CHECK(N::Uninstall() == Guest(Busy));
	CHECK(f.requests == 1 && f.disconnects == 0);
	{
		IdentityScope identity(Server);
		L::ServerSnapshot state;
		CHECK(L::InspectServer(server_lease, state) == 0 && state.published && state.dispatching && !state.destroyed);
		CHECK(Call(f.server, 0) == Guest(Busy));
		CHECK(L::InspectServer(server_lease, state) == 0 && !state.destroyed);
	}
	{
		std::lock_guard<std::mutex> lock(f.mutex);
		f.release = true; f.changed.notify_all();
	}
	dispatcher.join();
	f.Finish(c);
	CHECK(f.disconnects == 1 && f.forced == 0);
}

void TerminalFailureAndMalformedReply() {
	{
		Fixture f;
		const auto c = f.Accepted();
		f.fail_request = true;
		CHECK(R::BeginInvoke(c, 3, {}, {0x1e8, 4}) == 0);
		f.Dispatch(Guest(BadMessage));
		CHECK(R::PollInvoke(c) == BadMessage);
		Result reply; reply.value = 123;
		CHECK(R::ConsumeInvoke(c, reply) == BadMessage && reply.value == 123);
		CHECK(R::Retire(c) == Busy); // force queued; native close still outstanding
		CHECK(R::Retire(c) == Busy); // never duplicate forced notification
		f.Dispatch();
		CHECK(f.forced == 1 && f.disconnects == 1);
		CHECK(R::Retire(c) == 0);
	}
	{
		Fixture f;
		const auto c = f.Accepted();
		f.malformed = true;
		CHECK(R::BeginInvoke(c, 3, {}, {0x1e8, 4}) == 0);
		f.Dispatch();
		Result reply; reply.value = 123;
		CHECK(R::ConsumeInvoke(c, reply) == BadMessage && reply.value == 123);
		CHECK(R::BeginInvoke(c, 3, {}, {}) == Busy);
		f.Finish(c);
	}
}

void ServerLifecycleGeneration() {
	Fixture f;
	L::ServerLease lease, rejected;
	const auto address = reinterpret_cast<uint64_t>(f.server);
	const auto handle = f.server_storage[1];
	L::ServerSnapshot snapshot;
	{
		IdentityScope identity(Server);
		CHECK(L::AcquireServer(address, handle, lease) == 0 && lease);
		L::DispatcherSnapshot dispatcher;
		CHECK(L::InspectDispatcher(lease, dispatcher) == 0 && dispatcher.binding == L::DispatcherBinding::Unbound);
		CHECK(!dispatcher.proof.native_exited && !dispatcher.proof.successfully_joined);
		CHECK(L::AcquireServer(address + 8, handle, rejected) == NotFound && !rejected);
		CHECK(L::AcquireServer(address, handle + 1, rejected) == NotFound && !rejected);
		CHECK(L::AcquireServer(address, 0, rejected) == Invalid && !rejected);
		CHECK(L::InspectServer(lease, snapshot) == 0);
		CHECK(snapshot.native_address == address && snapshot.handle == handle && snapshot.published &&
		      !snapshot.destroyed && !snapshot.stopping && !snapshot.dispatching && !snapshot.stop_succeeded);
	}
	for (Identity wrong: {Receiver, Client, Identity {101, 1}, Identity {0, 0}}) {
		IdentityScope identity(wrong);
		L::ServerSnapshot unchanged; unchanged.handle = 0xfeed;
		L::StopResult stop; stop.stop_errno = 123;
		CHECK(L::AcquireServer(address, handle, rejected) == Invalid && !rejected);
		CHECK(L::InspectServer(lease, unchanged) == Invalid && unchanged.handle == 0xfeed);
		CHECK(L::RequestStop(lease, stop) == Invalid && stop.stop_errno == 123);
	}
	const auto client = f.Accepted();
	{
		IdentityScope identity(Server);
		L::StopResult result;
		CHECK(L::RequestStop(lease, result) == 0 && result.stop_errno == Busy);
		CHECK(result.snapshot.published && !result.snapshot.stopping && !result.snapshot.stop_succeeded && !result.snapshot.destroyed);
		CHECK(Call(f.server, 4) == Guest(Busy)); // Same actual native Stop result, no early dispatcher exit.
		CHECK(Call(f.server, 0) == Guest(Busy)); // Core refuses genuine destruction while connected.
		CHECK(L::InspectServer(lease, snapshot) == 0 && snapshot.published && !snapshot.destroyed);
	}
	CHECK(f.connects == 1 && f.disconnects == 0 && f.requests == 0);
	f.Finish(client); // Real receiver closes the session and acknowledges disconnect.
	{
		IdentityScope identity(Server);
		L::StopResult result;
		CHECK(L::RequestStop(lease, result) == 0 && result.stop_errno == 0);
		CHECK(result.snapshot.stopping && result.snapshot.stop_succeeded && result.snapshot.published && !result.snapshot.destroyed);
		CHECK(L::RequestStop(lease, result) == 0 && result.stop_errno == Busy);
		CHECK(result.snapshot.stopping && result.snapshot.stop_succeeded); // Prior proof never rewrites Busy.
		CHECK(Call(f.server, 4) == Guest(Busy));
		CHECK(Call(f.server, 0) == 0);
		f.server = nullptr;
		CHECK(L::InspectServer(lease, snapshot) == 0 && snapshot.destroyed && !snapshot.published && !snapshot.dispatching);
		CHECK(snapshot.handle == handle && snapshot.native_address == address);
		result.stop_errno = 123;
		CHECK(L::RequestStop(lease, result) == NotFound && result.stop_errno == 123);
		CHECK(f.server_pins == 1 && N::Uninstall() == Guest(Busy));
		f.owner.reset();
		CHECK(!f.owner_probe.expired());

		// Fixture permits same-address storage reuse to stress identity binding.
		// Production allocation leases additionally prohibit backing reuse.
		N::ServerConfig config {0x2000, 0x4000, &f.handler, 1, {}, 0, 0};
		std::memcpy(config.name, "SceNpMgrIpc", 12);
		CHECK(N::CreateServer(&f.server, &config, nullptr, f.server_storage.data()) == 0);
		const auto replacement = f.server_storage[1];
		CHECK(replacement != handle && f.server_pins == 2);
		CHECK(L::AcquireServer(address, handle, rejected) == NotFound && !rejected);
		CHECK(L::InspectServer(lease, snapshot) == 0 && snapshot.destroyed && !snapshot.published && snapshot.handle == handle);
		CHECK(L::RequestStop(lease, result) == NotFound && result.stop_errno == 123);
		L::ServerLease next;
		CHECK(L::AcquireServer(address, replacement, next) == 0);
		CHECK(L::InspectServer(next, snapshot) == 0 && snapshot.published && !snapshot.destroyed && !snapshot.stopping);
		CHECK(Call(f.server, 4) == 0);
		CHECK(Call(f.server, 0) == 0);
		f.server = nullptr;
		CHECK(L::InspectServer(next, snapshot) == 0 && snapshot.destroyed && snapshot.handle == replacement);
		next.reset();
		CHECK(f.server_pins == 1 && N::Uninstall() == Guest(Busy));
	}
	lease.reset(); // Only refs are released: no automatic Stop, destroy, or join.
	CHECK(f.server_pins == 0 && f.session_pins == 0 && f.work_pins == 0);
	CHECK(!f.owner_probe.expired()); // Fixture destructor now performs genuine Uninstall.
}

// These tests prove adapter contracts using a fixture-only receipt provider.
// They do not assert OS exit or successful kernel pthread collection.
void DispatcherReceipts() {
	Fixture f(true, true);
	IdentityScope identity(Server);
	L::ServerLease lease;
	CHECK(L::AcquireServer(reinterpret_cast<uint64_t>(f.server), f.server_storage[1], lease) == 0);
	L::DispatcherSnapshot snapshot;
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.binding == L::DispatcherBinding::Unbound);
	CHECK(!snapshot.proof.native_exited && !snapshot.proof.successfully_joined);
	const auto original = f.receipt;
	f.receipt.identity = Client; f.Dispatch(Guest(Invalid));
	f.receipt = original; f.receipt.generation = 0; f.Dispatch(Guest(Invalid));
	f.receipt = original; f.receipt.token.reset(); f.Dispatch(Guest(Invalid));
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.binding == L::DispatcherBinding::Unbound);
	f.receipt = original; f.Dispatch(); f.Dispatch(); // same incarnation is idempotent
	f.receipt.generation = 2; f.Dispatch(Guest(Invalid));
	f.receipt = original; f.receipt.token = std::make_shared<int>(7); f.Dispatch(Guest(Invalid));
	// Same pointer with different ownership must also fail.
	f.receipt.token = std::shared_ptr<void>(original.token.get(), [](void*) {}); f.Dispatch(Guest(Invalid));
	f.receipt = original;
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.binding == L::DispatcherBinding::Bound);
	CHECK(snapshot.receipt.token == original.token && !snapshot.proof.native_exited);
	{
		IdentityScope foreign(Client);
		CHECK(L::InspectDispatcher(lease, snapshot) == Invalid && snapshot.receipt.token == original.token);
	}
	f.inspect_action = [&] {
		L::ServerSnapshot state;
		CHECK(L::InspectServer(lease, state) == 0); // hook holds neither adapter lock
		L::DispatcherSnapshot nested; nested.receipt.generation = 99;
		CHECK(L::InspectDispatcher(lease, nested) == Busy && nested.receipt.generation == 99);
	};
	CHECK(L::InspectDispatcher(lease, snapshot) == 0);
	f.inspect_action = {};
	f.inspect_error = Busy; f.proof.native_exited = true;
	CHECK(L::InspectDispatcher(lease, snapshot) == Busy && !snapshot.proof.native_exited);
	f.inspect_error = 0;
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.proof.native_exited && !snapshot.proof.successfully_joined);
	f.proof.native_exited = false;
	CHECK(L::InspectDispatcher(lease, snapshot) == BadMessage && snapshot.proof.native_exited);
	f.proof = {true, true, {}};
	CHECK(L::InspectDispatcher(lease, snapshot) == BadMessage && !snapshot.proof.successfully_joined);
	f.proof.join_source = Server;
	f.proof.join_origin = N::DispatcherJoinOrigin::Explicit;
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.proof.successfully_joined);
	f.proof.join_source = Receiver;
	CHECK(L::InspectDispatcher(lease, snapshot) == BadMessage && snapshot.proof.join_source == Server);
	f.proof.join_source = Server;
	CHECK(Call(f.server, 4) == 0 && Call(f.server, 0) == 0);
	// Exact destroyed object remains inspectable through its lease.
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.receipt.token == original.token);
	f.Dispatch(Guest(NotFound)); // successful destroy erased the registry entry; Find rejects before Run admission
	f.server = nullptr;
	snapshot = {}; lease.reset();
}

void DispatcherCaptureStopRace() {
	Fixture f(true, true);
	IdentityScope identity(Server);
	L::ServerLease lease;
	CHECK(L::AcquireServer(reinterpret_cast<uint64_t>(f.server), f.server_storage[1], lease) == 0);
	f.Dispatch();
	f.capture_action = [&] {
		IdentityScope creator(Server);
		L::ServerSnapshot state;
		CHECK(L::InspectServer(lease, state) == 0 && state.dispatching);
		std::unique_lock<std::mutex> lock(f.mutex);
		f.published = true; f.changed.notify_all();
		CHECK(f.changed.wait_for(lock, std::chrono::seconds(10), [&] { return f.release; }));
	};
	std::thread dispatcher([&] { f.Dispatch(); });
	{
		std::unique_lock<std::mutex> lock(f.mutex);
		CHECK(f.changed.wait_for(lock, std::chrono::seconds(10), [&] { return f.published; }));
	}
	CHECK(f.server_pins == 1 && f.work_pins == 1);
	CHECK(Call(f.server, 0) == Guest(Busy));
	L::StopResult stopped;
	CHECK(L::RequestStop(lease, stopped) == 0 && stopped.stop_errno == 0);
	L::DispatcherSnapshot snapshot;
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.binding == L::DispatcherBinding::Bound);
	f.proof.native_exited = true;
	CHECK(L::InspectDispatcher(lease, snapshot) == BadMessage && !snapshot.proof.native_exited);
	f.proof.native_exited = false;
	{
		std::lock_guard<std::mutex> lock(f.mutex); f.release = true; f.changed.notify_all();
	}
	dispatcher.join();
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.binding == L::DispatcherBinding::Bound);
	CHECK(!snapshot.proof.native_exited && !snapshot.proof.successfully_joined); // Run return is neither
	f.capture_action = {};
	const int captures = f.captures;
	f.Dispatch(); // already stopped on entry, nevertheless capture is mandatory
	CHECK(f.captures == captures + 1);
	CHECK(Call(f.server, 0) == 0); f.server = nullptr;
	snapshot = {}; lease.reset();
}

void DispatcherStoppedBeforeFirstRun() {
	Fixture f(true, true);
	IdentityScope identity(Server);
	L::ServerLease lease;
	CHECK(L::AcquireServer(reinterpret_cast<uint64_t>(f.server), f.server_storage[1], lease) == 0);
	CHECK(Call(f.server, 4) == 0);
	f.Dispatch();
	L::DispatcherSnapshot snapshot;
	CHECK(f.captures == 1 && L::InspectDispatcher(lease, snapshot) == 0);
	CHECK(snapshot.binding == L::DispatcherBinding::Bound && !snapshot.proof.native_exited);
	CHECK(Call(f.server, 0) == 0); f.server = nullptr;
	// A host collector has no fabricated guest principal. Origin and actual
	// optional joiner identity are distinct immutable provider evidence.
	f.proof = {true, true, {-1, -1}, N::DispatcherJoinOrigin::DetachedCollector};
	CHECK(L::InspectDispatcher(lease, snapshot) == 0 && snapshot.proof.successfully_joined &&
	      snapshot.proof.join_source.pid == -1 && snapshot.proof.join_source.tid == -1);
	f.proof.join_origin = N::DispatcherJoinOrigin::Explicit;
	CHECK(L::InspectDispatcher(lease, snapshot) == BadMessage &&
	      snapshot.proof.join_origin == N::DispatcherJoinOrigin::DetachedCollector);
	snapshot = {}; lease.reset();
}

int main() {
	DispatcherReceipts();
	DispatcherCaptureStopRace();
	DispatcherStoppedBeforeFirstRun();
	StorageAdmission();
	ServerLifecycleGeneration();
	AbsentAndStale();
	StalledAndLate(true);
	StalledAndLate(false);
	TwoOutputsAndDisconnect();
	AbandonedInvokeAndReceiverLease();
	TerminalFailureAndMalformedReply();
	std::cout << "NP retained host operation checks passed (fixture dispatcher; no guest startup claim)\n";
}
