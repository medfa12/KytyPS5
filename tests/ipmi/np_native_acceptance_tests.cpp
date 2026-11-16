#include "kernel/ipmiGateway.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

// This standalone executable injects host allocation failure only around a
// selected Core operation, with allocations restored before assertions/cleanup.
static thread_local int allocations_before_failure = -1;
void* operator new(std::size_t size) {
	if (allocations_before_failure == 0) { throw std::bad_alloc(); }
	if (allocations_before_failure > 0) { --allocations_before_failure; }
	if (void* p = std::malloc(size ? size : 1)) { return p; }
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace Libs::LibKernel;
using namespace Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)

namespace {
void Put(Bytes& bytes, size_t at, uint64_t value, size_t size = 8) {
	CHECK(at + size <= bytes.size());
	for (size_t i = 0; i < size; ++i) { bytes[at + i] = static_cast<uint8_t>(value >> (8 * i)); }
}
uint64_t Get(const Bytes& bytes, size_t at, size_t size = 8) {
	CHECK(at + size <= bytes.size());
	uint64_t value = 0;
	for (size_t i = 0; i < size; ++i) { value |= uint64_t(bytes[at + i]) << (8 * i); }
	return value;
}
Bytes Words(std::initializer_list<uint64_t> words) {
	Bytes bytes(words.size() * 8);
	size_t offset = 0;
	for (auto value: words) { Put(bytes, offset, value); offset += 8; }
	return bytes;
}

struct Fixture {
	Core core;
	Bytes memory = Bytes(0x10000);
	uint64_t next = 0x100;
	Identity current {100, 1};
	int error = 0, authorization_calls = 0;
	bool authorized = true;
	std::unique_ptr<IpmiGateway::Gateway> gateway;
	static constexpr Identity server {100, 1}, receiver {100, 2}, other {100, 3}, client {200, 1};
	Fixture() {
		IpmiGateway::Callbacks cb;
		cb.copy_in = [&](uint64_t address, void* out, size_t size) {
			if (address > memory.size() || size > memory.size() - address) { return 14; }
			std::memcpy(out, memory.data() + address, size); return 0;
		};
		cb.copy_out = [&](uint64_t address, const void* in, size_t size) {
			if (address > memory.size() || size > memory.size() - address) { return 14; }
			std::memcpy(memory.data() + address, in, size); return 0;
		};
		cb.identity = [&] { return current; };
		cb.set_errno = [&](int e) { error = e; };
		cb.cooperate = [] { return Interrupted; };
		cb.has_capability62 = [](Identity id) { return id.pid == server.pid; };
		cb.authorize_client_registration = [](Identity, const std::string&) { return 0; };
		cb.authorize_connect = [&](Identity, const ConnectionInfo&) {
			++authorization_calls; return authorized ? 0 : 1;
		};
		gateway = std::make_unique<IpmiGateway::Gateway>(core, std::move(cb));
	}
	uint64_t Alloc(const Bytes& bytes) {
		const auto address = next;
		next += bytes.size() + 16;
		CHECK(next < memory.size());
		std::copy(bytes.begin(), bytes.end(), memory.begin() + address);
		return address;
	}
	struct Call {
		Fixture& f; Identity id; uint32_t op; int32_t kid;
		uint64_t descriptor, size, result;
		int Run() {
			f.current = id; f.error = 0;
			const auto tag = op == 0x320 ? Get(f.memory, descriptor, 4) : IpmiGateway::NativeDefaultTag;
			return f.gateway->Call(op, kid, result, descriptor, size, tag);
		}
		void Bad(int expected) { CHECK(Run() == -1 && f.error == expected && Get(f.memory, result, 4) == 0x12345678); }
		int32_t Good() { CHECK(Run() == 0); return static_cast<int32_t>(Get(f.memory, result, 4)); }
	};
	Call Make(Identity id, uint32_t op, int32_t kid, const Bytes& descriptor = {}) {
		return {*this, id, op, kid, Alloc(descriptor), descriptor.size(), Alloc({0x78, 0x56, 0x34, 0x12})};
	}
	int32_t RegisterServer(uint32_t multi, uint32_t cross_no_cap = 0) {
		Bytes config(0x38);
		Put(config, 0, config.size()); Put(config, 8, 0x2000); Put(config, 16, 0x4000);
		Put(config, 0x18, multi, 4); Put(config, 0x30, 0xccaad0);
		Put(config, 0x1c, cross_no_cap, 4);
		return Make(server, 0, -1, Words({0x100000, Name(), Alloc(config)})).Good();
	}
	uint64_t Name() { return Alloc({'S','c','e','N','p','M','g','r','I','p','c',0}); }
	int32_t RegisterClient(uint32_t sync_slots = 2) {
		Bytes config(0x150);
		Put(config, 0, config.size()); Put(config, 8, sync_slots, 4);
		Put(config, 0x10, 0x2000); Put(config, 0x18, 0x4000); Put(config, 0x144, 1, 4);
		return Make(client, 2, -1, Words({0x200000, Name(), Alloc(config)})).Good();
	}
};

void NativeNpShapeAndDispatch() {
	Fixture f;
	// ShellCore userland byte+18 maps to KERNEL +1c, not multi at +18.
	const auto server = f.RegisterServer(0, 1), client = f.RegisterClient(1);
	const auto acceptance = f.Alloc({0x44, 0x33, 0x22, 0x11});
	const auto argument = f.Alloc(Bytes(0x2000, 0x5a));
	auto connect = f.Make(f.client, 0x400, client, Words({argument, 0x2000, acceptance, 0}));
	f.authorized = false;
	connect.Bad(1); // cross_no_cap must not bypass host policy
	f.authorized = true;
	connect.Bad(Interrupted);
	CHECK(Get(f.memory, acceptance, 4) == 0x11223344);
	Dispatch dispatch;
	CHECK(f.core.Receive(f.receiver, server, dispatch).syscall_errno == Ok);
	CHECK(dispatch.type == Dispatch::Type::Connect && dispatch.config.sync_slots == 1 && dispatch.client == Handle(client));
	CHECK(dispatch.connect_argument == Bytes(0x2000, 0x5a));
	Handle session = 0;
	CHECK(f.core.CreateSession(f.other, 0x300000, 0, session).syscall_errno == Invalid && session == 0);
	CHECK(f.core.CreateSession(f.receiver, 0x300000, 0, session).syscall_errno == Ok);
	CHECK(f.core.Accept(f.other, client, 0).syscall_errno != Ok);
	CHECK(Get(f.memory, acceptance, 4) == 0x11223344); // no fabricated acceptance
	CHECK(f.core.Accept(f.receiver, client, 0).syscall_errno == Ok);
	CHECK(connect.Good() == 0 && Get(f.memory, acceptance, 4) == 0);
	CHECK(f.authorization_calls == 2); // interrupted resume did not reconnect

	// NP method3 really has two outputs: a 0x1e8 batch and remaining i32.
	const auto batch = f.Alloc(Bytes(0x1e8, 0xa5)), remaining = f.Alloc(Bytes(4, 0xa5));
	const auto method = f.Alloc(Bytes(4, 0xa5));
	const auto outputs = f.Alloc(Words({batch, 0x1e8, 0, remaining, 4, 0}));
	const auto channel = f.Alloc({4, 0, 0, 0});
	Bytes invocation(0x30);
	Put(invocation, 0, 3, 4); Put(invocation, 4, 1, 4); Put(invocation, 8, 2, 4);
	Put(invocation, 0x10, f.Alloc(Words({channel, 4})));
	Put(invocation, 0x18, outputs); Put(invocation, 0x20, method); Put(invocation, 0x28, 1, 4);
	auto invoke = f.Make(f.client, 0x320, client, invocation);
	invoke.Bad(Interrupted);
	CHECK(f.core.Receive(f.receiver, server, dispatch).syscall_errno == Ok);
	CHECK(dispatch.method == 3 && dispatch.native_session == 0x300000);
	Bytes record_batch(0x1e8);
	Put(record_batch, 0, 1, 4); Put(record_batch, 16, 0x1234, 4); // test-origin record value
	CHECK(f.core.Reply(f.other, session, 0, {record_batch, Bytes(4)}).syscall_errno != Ok);
	CHECK(f.core.Reply(f.receiver, session, 0, {record_batch, Bytes(4)}).syscall_errno == Ok);
	CHECK(invoke.Good() == 0 && Get(f.memory, method, 4) == 0);
	CHECK(Get(f.memory, outputs + 16) == 0x1e8 && Get(f.memory, outputs + 40) == 4);
	CHECK(Get(f.memory, batch, 4) == 1 && Get(f.memory, batch + 16, 4) == 0x1234 && Get(f.memory, remaining, 4) == 0);
	CHECK(f.core.DestroyServer(f.server, server).syscall_errno == Busy);
	const auto disconnected = f.Alloc(Bytes(4, 0xa5));
	auto disconnect = f.Make(f.client, 0x310, client, Words({disconnected}));
	disconnect.Bad(Interrupted);
	CHECK(f.core.Receive(f.receiver, server, dispatch).syscall_errno == Ok);
	CHECK(dispatch.type == Dispatch::Type::Disconnect);
	CHECK(f.core.CloseSession(f.receiver, session).syscall_errno == Ok);
	CHECK(f.core.AcknowledgeDisconnect(f.receiver, client, 0).syscall_errno == Ok);
	CHECK(disconnect.Good() == 0 && Get(f.memory, disconnected, 4) == 0);
	CHECK(f.core.DestroyClient(f.client, client).syscall_errno == Ok);
	CHECK(f.core.DestroyServer(f.server, server).syscall_errno == Ok);
}

void SingleSlotRestriction() {
	Fixture f;
	const auto server = f.RegisterServer(0), client = f.RegisterClient();
	const auto acceptance = f.Alloc(Bytes(4, 0xa5));
	f.Make(f.client, 0x400, client, Words({0, 0, acceptance, 0})).Bad(Invalid);
	Dispatch dispatch;
	CHECK(f.core.Receive(f.receiver, server, dispatch).syscall_errno == WouldBlock);
	CHECK(Get(f.memory, acceptance, 4) == 0xa5a5a5a5);
	CHECK(f.core.DestroyClient(f.client, client).syscall_errno == Ok);
	CHECK(f.core.DestroyServer(f.server, server).syscall_errno == Ok);
}

void IndependentMultiSlotAdmission() {
	Fixture f;
	const auto server = f.RegisterServer(1), client = f.RegisterClient(2);
	const auto acceptance = f.Alloc(Bytes(4, 0xa5));
	auto connect = f.Make(f.client, 0x400, client, Words({0, 0, acceptance, 0}));
	connect.Bad(Interrupted);
	Dispatch dispatch;
	CHECK(f.core.Receive(f.receiver, server, dispatch).syscall_errno == Ok && dispatch.config.sync_slots == 2);
	CHECK(f.core.Accept(f.receiver, client, -42).syscall_errno == Ok);
	CHECK(connect.Good() == 0 && static_cast<int32_t>(Get(f.memory, acceptance, 4)) == -42);
	CHECK(f.authorization_calls == 1);
	CHECK(f.core.DestroyClient(f.client, client).syscall_errno == Ok);
	CHECK(f.core.DestroyServer(f.server, server).syscall_errno == Ok);
}

template<class F> bool Attempt(int fail_at, F&& operation) {
	allocations_before_failure = fail_at;
	try {
		operation();
		allocations_before_failure = -1;
		return true;
	} catch (const std::bad_alloc&) {
		allocations_before_failure = -1;
		return false;
	}
}

void AllocationRollback() {
	// Sweep every allocation in each published operation, then complete the
	// real ordinary lifecycle so a leaked slot/session/binding is observable.
	for (int stage = 0; stage != 6; ++stage) {
		bool reached_success = false;
		for (int fail_at = 0; fail_at < 32 && !reached_success; ++fail_at) {
			Core core;
			const Identity owner {100, 1}, receiver {100, 2}, caller {200, 1};
			Handle server = 0x777, client = 0x888, session = 0x999;
			Ticket connect, request, disconnect;
			Dispatch dispatch;
			Config config;
			const auto reg_server = [&] { CHECK(core.RegisterServer(owner, "SceNpMgrIpc", 0xccaad0, server).syscall_errno == Ok); };
			if (stage == 0) {
				reached_success = Attempt(fail_at, reg_server);
				if (!reached_success) { CHECK(server == 0x777); reg_server(); }
			} else { reg_server(); }
			const auto reg_client = [&] { CHECK(core.RegisterClient(caller, "SceNpMgrIpc", config, client).syscall_errno == Ok); };
			if (stage == 1) {
				reached_success = Attempt(fail_at, reg_client);
				if (!reached_success) { CHECK(client == 0x888); reg_client(); }
			} else { reg_client(); }
			const auto connect_client = [&] { CHECK(core.Connect(caller, client, connect).syscall_errno == Ok); };
			if (stage == 2) {
				reached_success = Attempt(fail_at, connect_client);
				if (!reached_success) {
					CHECK(!connect && core.Receive(receiver, server, dispatch).syscall_errno == WouldBlock);
					connect_client();
				}
			} else { connect_client(); }
			CHECK(core.Receive(receiver, server, dispatch).syscall_errno == Ok && dispatch.type == Dispatch::Type::Connect);
			const auto create_session = [&] { CHECK(core.CreateSession(receiver, 0x300000, 0, session).syscall_errno == Ok); };
			if (stage == 3) {
				reached_success = Attempt(fail_at, create_session);
				if (!reached_success) { CHECK(session == 0x999); create_session(); }
			} else { create_session(); }
			CHECK(core.Accept(receiver, client, 0).syscall_errno == Ok && connect->PollStatus()->method_result == 0);
			const auto invoke = [&] { CHECK(core.Invoke(caller, client, 3, {}, {0x1e8, 4}, request).syscall_errno == Ok); };
			if (stage == 4) {
				reached_success = Attempt(fail_at, invoke);
				if (!reached_success) {
					CHECK(!request && core.Receive(receiver, server, dispatch).syscall_errno == WouldBlock);
					invoke();
				}
			} else { invoke(); }
			CHECK(core.Receive(receiver, server, dispatch).syscall_errno == Ok && dispatch.type == Dispatch::Type::Request);
			CHECK(core.Reply(receiver, session, -17, {Bytes(0x1e8, 0x5a), Bytes(4)}).syscall_errno == Ok);
			CHECK(Attempt(0, [&] {
				const auto status = request->PollStatus();
				CHECK(status && status->outputs.empty() && status->method_result == -17);
			})); // completion observation must not copy the response payload
			Result reply;
			CHECK(core.BeginInvokeRead(caller, client, request, reply).syscall_errno == Ok && reply.outputs[0][0] == 0x5a);
			CHECK(core.EndInvokeRead(caller, client, request, true).syscall_errno == Ok);
			CHECK(core.ReleaseInvokeCaller(caller, client, request).syscall_errno == Ok);
			const auto close = [&] { CHECK(core.Disconnect(caller, client, disconnect).syscall_errno == Ok); };
			if (stage == 5) {
				reached_success = Attempt(fail_at, close);
				if (!reached_success) {
					CHECK(!disconnect && core.Receive(receiver, server, dispatch).syscall_errno == WouldBlock);
					close();
				}
			} else { close(); }
			CHECK(core.Receive(receiver, server, dispatch).syscall_errno == Ok && dispatch.type == Dispatch::Type::Disconnect);
			CHECK(core.CloseSession(receiver, session).syscall_errno == Ok);
			CHECK(core.AcknowledgeDisconnect(receiver, client, 0).syscall_errno == Ok);
			CHECK(core.DestroyClient(caller, client).syscall_errno == Ok);
			CHECK(core.DestroyServer(owner, server).syscall_errno == Ok);
		}
		CHECK(reached_success);
	}
}
} // namespace

int main() {
	NativeNpShapeAndDispatch();
	SingleSlotRestriction();
	IndependentMultiSlotAdmission();
	AllocationRollback();
	std::cout << "NP-shaped native gateway component checks passed (no PSN traffic)\n";
}
