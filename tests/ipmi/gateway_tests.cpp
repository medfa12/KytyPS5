#include "kernel/ipmiGateway.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

using namespace Libs::LibKernel;
using namespace Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)

static void Put(Bytes& b, size_t offset, uint64_t n, size_t size = 8) {
	CHECK(offset + size <= b.size());
	for (size_t i = 0; i < size; ++i) { b[offset + i] = static_cast<uint8_t>(n >> (8 * i)); }
}
static uint64_t Get(const Bytes& b, size_t offset, size_t size = 8) {
	CHECK(offset + size <= b.size()); uint64_t n = 0;
	for (size_t i = 0; i < size; ++i) { n |= uint64_t(b[offset + i]) << (8 * i); }
	return n;
}
static Bytes Words(std::initializer_list<uint64_t> words) {
	Bytes b(words.size() * 8); size_t offset = 0;
	for (auto word: words) { Put(b, offset, word); offset += 8; } return b;
}

struct Fixture {
	Core core;
	Bytes memory = Bytes(1024 * 1024, 0xcd);
	uint64_t next = 0x100, fail_in = 0, fail_out = 0;
	Identity current {1, 1}, server {100, 1}, wa {100, 2}, wb {100, 3}, ca {200, 1}, cb {300, 1};
	int guest_errno = 0, reads = 0, policy_calls = 0, capability_calls = 0, client_policy_calls = 0;
	bool cap62 = true, allow_policy = true, allow_clients = true;
	std::function<int()> pump;
	std::function<void(uint64_t, size_t)> before_copy_out;
	std::vector<std::pair<uint64_t, size_t>> writes;
	size_t partial_out = 0;
	std::unique_ptr<IpmiGateway::Gateway> gateway;
	int32_t s = 0, a = 0, b = 0;
	static constexpr uint64_t server_object = 0xaaaa111122223333, handler = 0xbbbb444455556666;
	static constexpr uint64_t server_arg = 0xcccc777788889999, client_object = 0xddddaaaabbbbcccc;
	static constexpr uint64_t client_arg = 0xeeee111133335555, session_object = 0xffff222244446666;
	static constexpr uint64_t session_arg = 0x987654321abcdef0;
	Fixture(IpmiWire::SyncType type = IpmiWire::SyncType::Inline, size_t max_pending = 64, bool client_policy = true) {
		IpmiGateway::Callbacks callbacks;
		callbacks.copy_in = [&](uint64_t address, void* output, size_t size) {
			++reads;
			if (address == fail_in || address < 0x100 || address > memory.size() || size > memory.size() - address) { return 14; }
			std::memcpy(output, memory.data() + address, size); return 0;
		};
		callbacks.copy_out = [&](uint64_t address, const void* input, size_t size) {
			writes.emplace_back(address, size);
			if (before_copy_out) { before_copy_out(address, size); }
			if (address < 0x100 || address > memory.size() || size > memory.size() - address) { return 14; }
			if (address == fail_out) {
				std::memcpy(memory.data() + address, input, std::min(size, partial_out));
				return 14;
			}
			std::memcpy(memory.data() + address, input, size); return 0;
		};
		callbacks.identity = [&] { return current; };
		callbacks.set_errno = [&](int e) { guest_errno = e; };
		callbacks.cooperate = [&] { CHECK(pump); auto action = pump; return action(); };
		callbacks.has_capability62 = [&](Identity id) { ++capability_calls; CHECK(id == current); return cap62; };
		if (client_policy) {
			callbacks.authorize_client_registration = [&](Identity id, const std::string&) {
				++client_policy_calls; CHECK(id == current); return allow_clients ? Ok : Unsupported;
			};
		}
		callbacks.authorize_connect = [&](Identity id, const ConnectionInfo& info) {
			++policy_calls; CHECK(id == current && info.server_pid == server.pid);
			CHECK(info.server_config.native_object == server_object && info.server_config.handler == handler);
			CHECK(info.server_config.user_argument == server_arg && info.client_config.user_argument == client_arg);
			return allow_policy ? 0 : Unsupported;
		};
		gateway = std::make_unique<IpmiGateway::Gateway>(core, std::move(callbacks), IpmiWire::Limits {}, type, max_pending);
	}
	uint64_t Alloc(const Bytes& b) {
		const auto address = next; next += b.size() + 16; CHECK(next < memory.size());
		std::copy(b.begin(), b.end(), memory.begin() + address); return address;
	}
	Bytes Load(uint64_t address, size_t size) const { return Bytes(memory.begin() + address, memory.begin() + address + size); }
	struct Outcome { int rc, error; uint32_t result; };
	struct Transaction {
		Fixture& f; Identity id; uint32_t op; int32_t kid;
		uint64_t descriptor, size, result, tag;
		Outcome Run() {
			const auto saved = f.current; f.current = id; f.guest_errno = 0;
			const int rc = f.gateway->Call(op, kid, result, descriptor, size, tag);
			Outcome out {rc, f.guest_errno, static_cast<uint32_t>(Get(f.memory, result, 4))}; f.current = saved; return out;
		}
		void Bad(int error) { const auto r = Run(); CHECK(r.rc == -1 && r.error == error && r.result == 0x12345678); }
		void Good() { const auto r = Run(); CHECK(r.rc == 0 && r.result == 0); }
	};
	Transaction Saved(Identity id, uint32_t op, int32_t kid, const Bytes& descriptor) {
		return {*this, id, op, kid, Alloc(descriptor), descriptor.size(), Alloc({0x78,0x56,0x34,0x12}),
		        op == 0x320 ? Get(descriptor, 0, 4) : IpmiGateway::NativeDefaultTag};
	}
	Outcome Raw(Identity id, uint32_t op, int32_t kid, uint64_t desc, uint64_t size, uint64_t tag = IpmiGateway::NativeDefaultTag) {
		const auto saved = current; current = id; guest_errno = 0;
		const auto result = Alloc(Bytes {0x78, 0x56, 0x34, 0x12});
		const int rc = gateway->Call(op, kid, result, desc, size, tag);
		Outcome outcome {rc, guest_errno, static_cast<uint32_t>(Get(memory, result, 4))}; current = saved; return outcome;
	}
	Outcome Do(Identity id, uint32_t op, int32_t kid, const Bytes& desc = {}) {
		const auto tag = op == 0x320 && desc.size() >= 4 ? Get(desc, 0, 4) : IpmiGateway::NativeDefaultTag;
		return Raw(id, op, kid, Alloc(desc), desc.size(), tag);
	}
	int32_t Good(Identity id, uint32_t op, int32_t kid, const Bytes& desc = {}) {
		const auto r = Do(id, op, kid, desc); CHECK(r.rc == 0); CHECK(r.result <= INT32_MAX); return static_cast<int32_t>(r.result);
	}
	void Bad(Identity id, uint32_t op, int32_t kid, const Bytes& desc, int error) {
		const auto r = Do(id, op, kid, desc); CHECK(r.rc == -1 && r.error == error && r.result == 0x12345678);
	}
	Bytes ServerConfigBytes() {
		Bytes c(0x38); Put(c, 0, 0x38); Put(c, 8, 64); Put(c, 16, 4096);
		Put(c, 0x28, server_arg); Put(c, 0x30, handler); return c;
	}
	Bytes ClientConfigBytes() {
		Bytes c(0x150); Put(c, 0, 0x150); Put(c, 8, 7, 4); // ignored with flag144=0
		Put(c, 0x10, 101); Put(c, 0x18, 202); Put(c, 0x20, 3, 4);
		Put(c, 0x28, 303); Put(c, 0x30, 404); Put(c, 0x38, 2, 4);
		Put(c, 0x40, 32); Put(c, 0x48, 48); Put(c, 0x140, 4, 4); Put(c, 0x148, client_arg); return c;
	}
	Bytes Registration(uint64_t object, const Bytes& config, std::string name = "SceDbPreparationServer") {
		Bytes text(name.begin(), name.end()); text.push_back(0);
		return Words({object, Alloc(text), Alloc(config)});
	}
	void Register() {
		s = Good(server, 0, -1, Registration(server_object, ServerConfigBytes()));
		a = Good(ca, 2, -1, Registration(client_object, ClientConfigBytes()));
		b = Good(cb, 2, -1, Registration(client_object + 1, ClientConfigBytes()));
		CHECK(s >= 0 && a >= 0 && b >= 0 && s != a && a != b);
	}
	struct Received { Bytes payload, info; };
	Received Receive(Identity who, size_t capacity = 4096) {
		const auto work = Alloc(Bytes(capacity, 0xcc)), info = Alloc(Bytes(24, 0xcc));
		CHECK(Good(who, 0x201, s, Words({work, capacity, info, 0})) == 0);
		auto metadata = Load(info, 24); CHECK(Get(metadata, 16) == handler);
		return {Load(work, static_cast<size_t>(Get(metadata, 0))), metadata};
	}
	int32_t Session(Identity who, uint64_t object = session_object, uint64_t argument = session_arg) {
		return Good(who, 4, -1, Words({object, Alloc(Words({16, argument}))}));
	}
	void Query(Identity who, uint32_t op, int32_t kid, uint64_t expected, size_t size = 8) {
		const auto output = Alloc(Bytes(size + 1, 0xa5));
		Good(who, op, kid, Words({output})); CHECK(Get(memory, output, size) == expected && memory[output + size] == 0xa5);
	}
	int32_t ConnectA() {
		int32_t session = -1; const auto method = Alloc(Bytes(4, 0xaa));
		pump = [&] {
			auto r = Receive(wa); CHECK(Get(r.info, 8, 4) == 1 && Get(r.info, 12, 4) == static_cast<uint32_t>(a));
			session = Session(wa); Good(wa, 0x212, a, Bytes(4)); return 0;
		};
		Good(ca, 0x400, a, Words({0, 0, method, 0})); CHECK(Get(memory, method, 4) == 0); pump = {};
		return session;
	}
};

static void RegistrationQueriesAndDescriptorContract() {
	Fixture f; f.Register();
	f.Query(f.server, 0x466, f.s, Fixture::server_arg); f.Query(f.ca, 0x467, f.a, Fixture::client_arg);
	ObjectInfo info; CHECK(f.core.Inspect(f.server, f.s, ObjectKind::Server, info).syscall_errno == Ok);
	CHECK(info.native_object == Fixture::server_object && info.handler == Fixture::handler && info.user_argument == Fixture::server_arg);
	CHECK(f.core.Inspect(f.ca, f.a, ObjectKind::Client, info).syscall_errno == Ok && info.native_object == Fixture::client_object);
	ConnectionInfo connection; CHECK(f.core.Connection(f.ca, f.a, connection).syscall_errno == Ok);
	CHECK(connection.client_config.sync_slots == 1 && connection.client_config.async_slots == 3);
	CHECK(connection.client_config.async_input_limit == 303 && connection.client_config.async_output_limit == 404);
	CHECK(connection.client_config.queue_capacities == std::vector<uint64_t>({32, 48}));
	const auto output = f.Alloc(Bytes(26, 0xa5));
	f.Good(f.server, 0x46a, f.s, Words({output}));
	CHECK(f.Load(output, 25) == Bytes({'S','c','e','D','b','P','r','e','p','a','r','a','t','i','o','n','S','e','r','v','e','r',0,0,0}));
	CHECK(f.memory[output + 25] == 0xa5);
	f.Good(f.ca, 0x46b, f.a, Words({output}));
	for (uint32_t op: {0x466u, 0x46au}) { f.Bad(f.ca, op, f.s, Words({output}), NotFound); }
	f.Bad(f.server, 0x467, f.s, Words({output}), NotFound);
	f.Bad(f.ca, 0x466, f.a, Words({output}), NotFound);
	f.Bad(f.ca, 0x468, f.a, Words({output}), NotFound);
	const int reads = f.reads;
	auto huge = f.Raw(f.ca, 0x999, -1, UINT64_MAX, 0x41, UINT64_MAX);
	CHECK(huge.rc == 0 && huge.result == 0x800e0001 && f.reads == reads);
	// Short descriptors are zero-extended: a one-byte wake descriptor is {1,0,0}.
	f.Good(f.server, 0x202, f.s, {1});
	auto wake = f.Receive(f.wa); CHECK(wake.payload.empty() && Get(wake.info, 8, 4) == 0xb0);
	// Longer (but <=0x40) descriptors are copied, not rejected by exact size.
	Bytes query(0x40, 0x7f); Put(query, 0, output); f.Good(f.ca, 0x467, f.a, query);
	f.Bad(f.ca, 0x467, f.a, {}, 14); // zero-filled output pointer reaches copyout
	f.Bad(f.ca, 0x321, f.a, {}, Unsupported); // asynchronous invocation is explicit unsupported
	f.Bad(f.ca, 0x520, f.a, {}, Unsupported);
	auto tagged = f.Raw(f.ca, 0x467, f.a, f.Alloc(Words({output})), 8, 1);
	CHECK(tagged.rc == -1 && tagged.error == Unsupported && tagged.result == 0x12345678);
	f.fail_in = f.Alloc(Bytes(8)); auto fault = f.Raw(f.ca, 0x467, f.a, f.fail_in, 8);
	CHECK(fault.rc == -1 && fault.error == 14 && fault.result == 0x12345678);
	f.fail_in = 0; f.fail_out = output; f.Bad(f.ca, 0x467, f.a, Words({output}), 14); f.fail_out = 0;
	// Final opResult copyout fails atomically; a successful query does not hide it.
	const auto result = f.Alloc(Bytes {0x78,0x56,0x34,0x12}); f.fail_out = result; f.current = f.ca;
	CHECK(f.gateway->Call(0x467, f.a, result, f.Alloc(Words({output})), 8, 0) == -1 && f.guest_errno == 14);
	CHECK(Get(f.memory, result, 4) == 0x12345678);
	CHECK(f.gateway->Call(0x999, -1, result, UINT64_MAX, 0x41, 0) == -1 && f.guest_errno == 14);
	CHECK(Get(f.memory, result, 4) == 0x12345678); f.fail_out = 0;
	// Alias exclusion preserves the opResult sentinel too.
	CHECK(f.gateway->Call(0x467, f.a, result, f.Alloc(Words({result})), 8, 0) == -1 && f.guest_errno == Unsupported);
	CHECK(Get(f.memory, result, 4) == 0x12345678);
}

static void ConnectBytesAndTwoClientAcceptance() {
	Fixture f; f.Register();
	const Bytes original {0,0xff,0x31,0,0x7e}; const auto argument = f.Alloc(original);
	const auto ma = f.Alloc(Bytes(4, 0xee)), mb = f.Alloc(Bytes(4, 0xee));
	int32_t sa = -1, sb = -1;
	f.pump = [&] {
		// Alter guest source after copyin: CONNECT must retain the original bytes.
		std::fill(f.memory.begin() + argument, f.memory.begin() + argument + original.size(), 0x88);
		f.pump = [&] {
			auto da = f.Receive(f.wa), db = f.Receive(f.wb);
			CHECK(Get(da.info, 12, 4) == static_cast<uint32_t>(f.a) && Get(db.info, 12, 4) == static_cast<uint32_t>(f.b));
			CHECK(da.payload.size() == 0x154 + original.size());
			CHECK(Get(da.payload, 0, 4) == 200 && Get(da.payload, 8, 4) == 1);
			CHECK(Get(da.payload, 0x10) == 101 && Get(da.payload, 0x18) == 202);
			CHECK(Get(da.payload, 0x20, 4) == 3 && Get(da.payload, 0x28) == 303 && Get(da.payload, 0x30) == 404);
			CHECK(Get(da.payload, 0x38, 4) == 4 && Get(da.payload, 0x40, 4) == 2);
			CHECK(Get(da.payload, 0x48) == 32 && Get(da.payload, 0x50) == 48 && Get(da.payload, 0x148) == 0);
			CHECK(Get(da.payload, 0x150, 4) == original.size());
			CHECK(std::equal(original.begin(), original.end(), da.payload.begin() + 0x154));
			sa = f.Session(f.wa); sb = f.Session(f.wb, Fixture::session_object + 1, Fixture::session_arg + 1);
			f.Query(f.wa, 0x468, sa, Fixture::session_arg); f.Query(f.wb, 0x468, sb, Fixture::session_arg + 1);
			f.Query(f.wa, 0x302, sa, 200, 4); f.Query(f.wb, 0x302, sb, 300, 4);
			f.Bad(f.ca, 0x468, sa, Words({f.Alloc(Bytes(8))}), NotFound);
			f.Bad(f.wa, 0x212, f.b, Bytes(4), NotFound);
			f.Bad(f.wb, 0x212, f.a, Bytes(4), NotFound);
			f.Bad({999, f.wa.tid}, 0x212, f.a, Bytes(4), Invalid);
			f.Bad({100, 99}, 0x212, f.a, Bytes(4), Invalid);
			CHECK(Get(f.memory, ma, 4) == 0xeeeeeeee && Get(f.memory, mb, 4) == 0xeeeeeeee);
			f.Good(f.wb, 0x212, f.b, Bytes(4)); f.Good(f.wa, 0x212, f.a, Bytes(4)); return 0;
		};
		f.Good(f.cb, 0x400, f.b, Words({0,0,mb,0})); return 0;
	};
	f.Good(f.ca, 0x400, f.a, Words({argument,original.size(),ma,0})); f.pump = {};
	CHECK(sa != sb && Get(f.memory, ma, 4) == 0 && Get(f.memory, mb, 4) == 0);
	f.Query(f.wa, 0x302, sa, 200, 4); f.Query(f.wb, 0x302, sb, 300, 4);
}

static void NativeProducedReplyAndDisconnect(IpmiWire::SyncType type) {
	Fixture f(type); f.Register(); const auto session = f.ConnectA();
	for (uint32_t phase: {1u, 3u, 4u}) {
		Bytes producer(24);
		for (size_t i = 0; i < producer.size(); ++i) { producer[i] = static_cast<uint8_t>(i * 11 + phase); }
		Put(producer, 20, phase, 4); const int32_t method_result = -37 - static_cast<int32_t>(phase);
		const auto output = f.Alloc(Bytes(24, 0)), method = f.Alloc(Bytes(4, 0xaa));
		Bytes buffer; CHECK(IpmiWire::EncodeBufferInfo({output,24,0}, buffer) == IpmiWire::Error::Ok);
		const auto buffer_address = f.Alloc(buffer);
		Bytes invoke; CHECK(IpmiWire::EncodeInvoke320({0x160008,0,1,0,buffer_address,method}, invoke) == IpmiWire::Error::Ok);
		f.pump = [&] {
			auto received = f.Receive(f.wa); CHECK(Get(received.info, 8, 4) == static_cast<uint32_t>(type));
			CHECK(received.payload.size() == (type == IpmiWire::SyncType::Inline ? 52 : 28));
			CHECK(Get(received.payload, 0) == Fixture::session_object && Get(received.payload, 8, 4) == 200);
			CHECK(Get(received.payload, 12, 4) == 0x160008 && Get(received.payload, 16, 4) == 0);
			CHECK(Get(received.payload, 20, 4) == 1 && Get(received.payload, 24, 4) == 24);
			Bytes source; CHECK(IpmiWire::EncodeBufferInfo({f.Alloc(producer),24,24}, source) == IpmiWire::Error::Ok);
			Bytes reply; CHECK(IpmiWire::EncodeReply232({method_result,1,f.Alloc(source)}, reply) == IpmiWire::Error::Ok);
			f.Bad(f.ca, 0x232, session, reply, NotFound);
			f.Good(f.wa, 0x232, session, reply); return Interrupted; // completion wins over signal
		};
		f.Good(f.ca, 0x320, f.a, invoke); f.pump = {};
		CHECK(f.Load(output, 24) == producer && Get(f.memory, buffer_address + 16) == 24);
		CHECK(Get(f.memory, method, 4) == static_cast<uint32_t>(method_result));
	}
	const auto method = f.Alloc(Bytes(4, 0xcc));
	f.pump = [&] {
		auto received = f.Receive(f.wa); CHECK(Get(received.info, 8, 4) == 3);
		CHECK(Get(received.info, 12, 4) == static_cast<uint32_t>(f.a));
		CHECK(received.payload == Words({Fixture::session_object}));
		// 13.60 809ef269 tests session-present bit20000, not Connected.
		f.Query(f.wa, 0x302, session, 200, 4);
		f.Bad(f.wa, 0x222, f.a, Bytes(4), Busy);
		f.Bad(f.ca, 5, session, {}, NotFound);
		f.Good(f.wa, 5, session); CHECK(Get(f.memory, method, 4) == 0xcccccccc);
		f.Bad(f.wa, 0x468, session, Words({f.Alloc(Bytes(8))}), NotFound);
		f.Bad(f.wb, 0x222, f.a, Bytes(4), Invalid);
		Bytes ack(4); Put(ack, 0, 17, 4); f.Good(f.wa, 0x222, f.a, ack); return 0;
	};
	f.Good(f.ca, 0x310, f.a, Words({method})); f.pump = {};
	CHECK(Get(f.memory, method, 4) == 17);
	f.Good(f.ca, 3, f.a); f.Bad(f.ca, 0x467, f.a, Words({f.Alloc(Bytes(8))}), NotFound);
	f.Good(f.cb, 3, f.b); f.Good(f.server, 1, f.s);
	f.Bad(f.server, 0x466, f.s, Words({f.Alloc(Bytes(8))}), NotFound);
}

static void PackedInvokeInputCopy() {
	Fixture f; f.Register(); const auto session = f.ConnectA();
	const auto first = f.Alloc({0x11,0x22,0x33}), second = f.Alloc({0x44,0x55});
	Bytes inputs; CHECK(IpmiWire::EncodeDataInfo({first,3}, inputs) == IpmiWire::Error::Ok);
	Bytes next; CHECK(IpmiWire::EncodeDataInfo({second,2}, next) == IpmiWire::Error::Ok);
	inputs.insert(inputs.end(), next.begin(), next.end());
	const auto out1 = f.Alloc(Bytes(5, 0xee)), out2 = f.Alloc(Bytes(7, 0xee)), method = f.Alloc(Bytes(4, 0xee));
	Bytes outputs; CHECK(IpmiWire::EncodeBufferInfo({out1,5,0}, outputs) == IpmiWire::Error::Ok);
	CHECK(IpmiWire::EncodeBufferInfo({out2,7,0}, next) == IpmiWire::Error::Ok);
	outputs.insert(outputs.end(), next.begin(), next.end()); const auto descriptors = f.Alloc(outputs);
	Bytes invoke; CHECK(IpmiWire::EncodeInvoke320({0x12345678,2,2,f.Alloc(inputs),descriptors,method}, invoke) == IpmiWire::Error::Ok);
	f.pump = [&] {
		f.memory[first] = 0xff; f.memory[second] = 0xff;
		auto received = f.Receive(f.wa);
		CHECK(received.payload.size() == 57 && Get(received.payload, 24, 4) == 3 && Get(received.payload, 31, 4) == 2);
		CHECK(received.payload[28] == 0x11 && received.payload[35] == 0x44);
		CHECK(Get(received.payload, 37, 4) == 5 && Get(received.payload, 41, 4) == 7);
		Bytes buffers; CHECK(IpmiWire::EncodeBufferInfo({f.Alloc({9,8}),5,2}, buffers) == IpmiWire::Error::Ok);
		Bytes second_buffer; CHECK(IpmiWire::EncodeBufferInfo({f.Alloc({7,6,5}),7,3}, second_buffer) == IpmiWire::Error::Ok);
		buffers.insert(buffers.end(), second_buffer.begin(), second_buffer.end());
		Bytes reply; CHECK(IpmiWire::EncodeReply232({-9,2,f.Alloc(buffers)}, reply) == IpmiWire::Error::Ok);
		f.Good(f.wa, 0x232, session, reply); return 0;
	};
	f.Good(f.ca, 0x320, f.a, invoke); f.pump = {};
	CHECK(f.Load(out1, 5) == Bytes({9,8,0xee,0xee,0xee}) && f.Load(out2, 7) == Bytes({7,6,5,0xee,0xee,0xee,0xee}));
	CHECK(Get(f.memory, descriptors + 16) == 2 && Get(f.memory, descriptors + 40) == 3);
	CHECK(Get(f.memory, method, 4) == static_cast<uint32_t>(-9));
	// Count/length exclusions happen before payload dereference or queue admission.
	Bytes poison = Words({UINT64_MAX,UINT64_MAX});
	CHECK(IpmiWire::EncodeInvoke320({1,1,0,f.Alloc(poison),0,method}, invoke) == IpmiWire::Error::Ok);
	f.Bad(f.ca, 0x320, f.a, invoke, Unsupported);
	Dispatch d; CHECK(f.core.Receive(f.wa, f.s, d).syscall_errno == WouldBlock);
}

static void ReceiveCopyFailureAndFeatureGates() {
	Fixture f; f.Register();
	f.allow_policy = false;
	f.Bad(f.ca, 0x400, f.a, Words({0,0,f.Alloc(Bytes(4)),0}), Unsupported);
	f.allow_policy = true;
	f.Bad(f.ca, 0x400, f.a, Words({0,0,f.Alloc(Bytes(4)),f.Alloc(Bytes(4))}), Unsupported);
	f.Bad(f.ca, 0x400, f.a, Words({UINT64_MAX,65,f.Alloc(Bytes(4)),0}), Unsupported);
	// No failed gate queued anything.
	Dispatch d; CHECK(f.core.Receive(f.wa, f.s, d).syscall_errno == WouldBlock);
	const auto method = f.Alloc(Bytes(4, 0xab));
	f.pump = [&] {
		const auto work = f.Alloc(Bytes(4096)), info = f.Alloc(Bytes(24));
		f.Bad(f.wa, 0x201, f.s, Words({work,1,info,0}), NoMemory);
		f.fail_out = work; f.Bad(f.wa, 0x201, f.s, Words({work,4096,info,0}), 14);
		f.fail_out = info; f.Bad(f.wa, 0x201, f.s, Words({work,4096,info,0}), 14); f.fail_out = 0;
		f.Bad(f.wa, 0x201, f.s, Words({work,4096,info,1}), Unsupported);
		// Queue survives all failed copies and can be bound by a different worker.
		auto received = f.Receive(f.wb); CHECK(Get(received.info, 12, 4) == static_cast<uint32_t>(f.a));
		f.Session(f.wb); f.Good(f.wb, 0x212, f.a, Bytes(4)); return 0;
	};
	f.Good(f.ca, 0x400, f.a, Words({0,0,method,0})); f.pump = {};
	Fixture auth;
	auto server = auth.ServerConfigBytes();
	auth.cap62 = false; auth.Bad(auth.server, 0, -1, auth.Registration(Fixture::server_object, server), 1);
	auth.cap62 = true; auth.Bad(auth.server, 0, -1, auth.Registration(Fixture::server_object, server, "UserService"), 1);
	auth.cap62 = false; auth.Good(auth.server, 0, -1, auth.Registration(Fixture::server_object, server, "UserService"));
	// +18 multi-slot admission is covered by np_native_acceptance_tests.cpp.
	for (size_t flag: {0x20}) {
		Fixture gated; auto config = gated.ServerConfigBytes(); Put(config, flag, 1, 4);
		gated.s = gated.Good(gated.server, 0, -1, gated.Registration(Fixture::server_object, config));
		gated.a = gated.Good(gated.ca, 2, -1, gated.Registration(Fixture::client_object, gated.ClientConfigBytes()));
		gated.Bad(gated.ca, 0x400, gated.a, Words({0,0,gated.Alloc(Bytes(4)),0}), Unsupported);
		CHECK(gated.policy_calls == 0 && gated.core.Receive(gated.wa, gated.s, d).syscall_errno == WouldBlock);
	}
}

static void RetainedWaitTransactions() {
	Fixture f; f.Register();
	const Bytes original_argument {4,0,0xff,9}; const auto argument = f.Alloc(original_argument);
	const auto connect_method = f.Alloc(Bytes(4, 0xaa));
	auto connect = f.Saved(f.ca, 0x400, f.a, Words({argument,original_argument.size(),connect_method,0}));
	int signals = 0;
	f.pump = [&] { ++signals; return Interrupted; };
	connect.Bad(Interrupted); const auto reads_after_connect = f.reads;
	// Same scalar call with changed guest bytes must resume its owned snapshot.
	std::fill(f.memory.begin() + connect.descriptor, f.memory.begin() + connect.descriptor + connect.size, 0xff);
	f.memory[argument] = 0x77;
	connect.Bad(Interrupted); CHECK(f.reads == reads_after_connect && signals == 2 && f.policy_calls == 1);
	const auto original_tag = connect.tag; connect.tag = 0; connect.Bad(Busy); connect.tag = original_tag;
	const auto original_descriptor = connect.descriptor; ++connect.descriptor; connect.Bad(Busy); connect.descriptor = original_descriptor;
	f.Bad(f.ca, 0x310, f.a, Words({connect_method}), Busy); // conflicting wait on suspended identity
	f.Bad({999, f.ca.tid}, 0x400, f.a, Words({0,0,connect_method,0}), NotFound);
	f.Bad({f.ca.pid, 99}, 0x400, f.a, Words({0,0,connect_method,0}), Busy); // not this identity's transaction
	auto dc = f.Receive(f.wa); CHECK(Get(dc.info, 8, 4) == 1);
	CHECK(std::equal(original_argument.begin(), original_argument.end(), dc.payload.begin() + 0x154));
	const auto session = f.Session(f.wa); f.Good(f.wa, 0x212, f.a, Bytes(4));
	Dispatch empty; CHECK(f.core.Receive(f.wb, f.s, empty).syscall_errno == WouldBlock);
	f.pump = [&] { CHECK(false); return Interrupted; }; // completion is polled before yielding
	const auto before_resume = f.reads; connect.Good(); CHECK(f.reads == before_resume && Get(f.memory, connect_method, 4) == 0);

	const auto output = f.Alloc(Bytes(24)), method = f.Alloc(Bytes(4, 0xaa)), decoy = f.Alloc(Bytes(24, 0xdd));
	Bytes buffer; CHECK(IpmiWire::EncodeBufferInfo({output,24,0}, buffer) == IpmiWire::Error::Ok);
	const auto buffers = f.Alloc(buffer);
	Bytes descriptor; CHECK(IpmiWire::EncodeInvoke320({0x160008,0,1,0,buffers,method}, descriptor) == IpmiWire::Error::Ok);
	auto invoke = f.Saved(f.ca, 0x320, f.a, descriptor); CHECK(invoke.tag == 0x160008);
	f.pump = [&] {
		// A recursive retry while the original frame is active cannot double-wait.
		invoke.Bad(Busy); return Interrupted;
	};
	invoke.Bad(Interrupted); const auto invoke_reads = f.reads;
	Put(f.memory, buffers, decoy); Put(f.memory, buffers + 8, 1);
	std::fill(f.memory.begin() + invoke.descriptor, f.memory.begin() + invoke.descriptor + invoke.size, 0xff);
	f.pump = [&] { return Interrupted; };
	invoke.Bad(Interrupted); CHECK(f.reads == invoke_reads);
	auto di = f.Receive(f.wa); CHECK(Get(di.payload, 12, 4) == 0x160008 && Get(di.payload, 24, 4) == 24);
	Bytes produced(24); for (size_t i = 0; i < produced.size(); ++i) { produced[i] = static_cast<uint8_t>(i * 13 + 7); }
	Put(produced, 20, 3, 4);
	Bytes reply_buffer; CHECK(IpmiWire::EncodeBufferInfo({f.Alloc(produced),24,24}, reply_buffer) == IpmiWire::Error::Ok);
	Bytes reply; CHECK(IpmiWire::EncodeReply232({-73,1,f.Alloc(reply_buffer)}, reply) == IpmiWire::Error::Ok);
	f.Good(f.wa, 0x232, session, reply);
	CHECK(f.core.Receive(f.wb, f.s, empty).syscall_errno == WouldBlock);
	f.pump = [&] { CHECK(false); return Interrupted; };
	const auto retry_reads = f.reads; invoke.Good(); CHECK(f.reads == retry_reads);
	CHECK(f.Load(output, 24) == produced && f.Load(decoy, 24) == Bytes(24, 0xdd));
	CHECK(Get(f.memory, buffers) == output && Get(f.memory, buffers + 8) == 24 &&
	      Get(f.memory, buffers + 16) == 24 && Get(f.memory, method, 4) == static_cast<uint32_t>(-73));

	const auto disconnect_method = f.Alloc(Bytes(4, 0xaa));
	auto disconnect = f.Saved(f.ca, 0x310, f.a, Words({disconnect_method}));
	f.pump = [&] { return Interrupted; }; disconnect.Bad(Interrupted);
	Put(f.memory, disconnect.descriptor, decoy); disconnect.Bad(Interrupted);
	auto dd = f.Receive(f.wa); CHECK(Get(dd.info, 8, 4) == 3 && dd.payload == Words({Fixture::session_object}));
	f.Good(f.wa, 5, session); Bytes ack(4); Put(ack, 0, 29, 4); f.Good(f.wa, 0x222, f.a, ack);
	CHECK(f.core.Receive(f.wb, f.s, empty).syscall_errno == WouldBlock);
	f.pump = [&] { CHECK(false); return Interrupted; }; disconnect.Good();
	CHECK(Get(f.memory, disconnect_method, 4) == 29 && f.Load(decoy, 24) == Bytes(24, 0xdd));
	// Success releases the retained identity; a fresh connect really dispatches.
	CHECK(f.ConnectA() >= 0);
}

enum class CompletionFault { None, Data, Method, Descriptors, FinalResult };

static void ClientCompletionCopyOrder(uint32_t count, CompletionFault fault) {
	Fixture f; f.Register(); const auto session = f.ConnectA();
	const Bytes producer(24, 0x69);
	const auto output = f.Alloc(Bytes(24, 0xcc)), decoy = f.Alloc(Bytes(24, 0xdd));
	const auto method = f.Alloc(Bytes(4, 0xaa));
	const auto buffers = f.Alloc(Words({output,24,0xeeee}));
	Bytes descriptor;
	CHECK(IpmiWire::EncodeInvoke320({0x160008,0,count,0,count ? buffers : 0,method}, descriptor) == IpmiWire::Error::Ok);
	auto invoke = f.Saved(f.ca, 0x320, f.a, descriptor);
	f.pump = [&] { return Interrupted; }; invoke.Bad(Interrupted);
	Ticket ticket; CHECK(f.core.InspectInvoke(f.ca, f.a, ticket).syscall_errno == Ok);
	f.Receive(f.wa);
	// Server BufferInfo's capacity is not a client-side length check.
	const auto source = f.Alloc(Words({f.Alloc(producer),0,24}));
	Bytes reply; CHECK(IpmiWire::EncodeReply232({0,count,count ? source : 0}, reply) == IpmiWire::Error::Ok);
	f.Good(f.wa, 0x232, session, reply);
	CHECK(ticket->InspectReply().published && ticket->InspectReply().receiver_released);
	CHECK(!ticket->InspectReply().caller_released && !ticket->InspectReply().consumed);
	Ticket blocked; CHECK(f.core.Invoke(f.ca, f.a, 9, {}, {}, blocked).syscall_errno == Busy);
	Dispatch empty; CHECK(f.core.Receive(f.wa, f.s, empty).syscall_errno == WouldBlock);
	const auto packet = ticket->InspectReplyPacket();
	CHECK(Get(packet, 0, 4) == 0x42 && Get(packet, 4, 4) == (count ? 0x2c : 0x10));
	CHECK(Get(packet, 12, 4) == count);
	if (count) { CHECK(Get(packet, 16, 4) == 24 && Bytes(packet.begin() + 20, packet.end()) == producer); }
	// Mutate the whole guest descriptor while suspended. Completion must use
	// and later restore the original pointer/capacity, not only written+16.
	Put(f.memory, buffers, decoy); Put(f.memory, buffers + 8, 1); Put(f.memory, buffers + 16, 0xbeef);
	const auto mutated = f.Load(buffers, 24);
	f.fail_out = fault == CompletionFault::Data ? output : fault == CompletionFault::Method ? method :
	             fault == CompletionFault::Descriptors ? buffers : fault == CompletionFault::FinalResult ? invoke.result : 0;
	f.partial_out = fault == CompletionFault::Data ? 7 : 0;
	f.writes.clear();
	f.before_copy_out = [&](uint64_t address, size_t size) {
		const auto state = ticket->InspectReply();
		if (address == output) {
			CHECK(size == 24 && state.read_reserved && !state.consumed && !state.caller_released);
		} else if (address == method) {
			CHECK(size == 4 && !state.read_reserved && state.consumed && !state.caller_released);
			CHECK(ticket->InspectReplyPacket().empty());
		} else if (address == buffers) {
			CHECK(size == 24 && state.consumed && !state.caller_released);
			CHECK(Get(f.memory, method, 4) == 0);
		} else if (address == invoke.result) {
			CHECK(size == 4 && state.consumed && state.caller_released);
			Ticket absent; CHECK(f.core.InspectInvoke(f.ca, f.a, absent).syscall_errno == NotFound);
		}
	};
	f.pump = [&] { CHECK(false); return Interrupted; };
	if (fault == CompletionFault::None) { invoke.Good(); } else { invoke.Bad(14); }
	f.before_copy_out = {};
	std::vector<std::pair<uint64_t, size_t>> expected;
	if (count) { expected.emplace_back(output, 24); }
	if (fault != CompletionFault::Data) {
		expected.emplace_back(method, 4);
		if (fault != CompletionFault::Method) {
			if (count) { expected.emplace_back(buffers, 24); }
			if (fault != CompletionFault::Descriptors) { expected.emplace_back(invoke.result, 4); }
		}
	}
	CHECK(f.writes == expected);
	const auto state = ticket->InspectReply();
	CHECK(state.caller_released && !state.read_reserved);
	CHECK(state.consumed == (fault != CompletionFault::Data));
	CHECK(ticket->InspectReplyPacket().empty() == state.consumed);
	CHECK(f.Load(decoy, 24) == Bytes(24, 0xdd));
	if (count) {
		Bytes expected_data(24, 0xcc);
		std::copy_n(producer.begin(), fault == CompletionFault::Data ? 7 : 24, expected_data.begin());
		CHECK(f.Load(output, 24) == expected_data);
		const bool restored = fault == CompletionFault::None || fault == CompletionFault::FinalResult;
		CHECK(f.Load(buffers, 24) == (restored ? Words({output,24,24}) : mutated));
	}
	CHECK(Get(f.memory, method, 4) == ((fault == CompletionFault::Data || fault == CompletionFault::Method) ? 0xaaaaaaaau : 0u));
	Ticket absent; CHECK(f.core.InspectInvoke(f.ca, f.a, absent).syscall_errno == NotFound);
	// Reissuing after a terminal fault never copies a cached response. An unread
	// packet is explicitly excluded pending native future-invocation semantics;
	// otherwise this is a fresh queued request, not resumption of the old ticket.
	f.fail_out = 0; f.partial_out = 0; Put(f.memory, invoke.result, 0x12345678, 4);
	Put(f.memory, buffers, output); Put(f.memory, buffers + 8, 24);
	f.pump = [&] { return Interrupted; };
	if (fault == CompletionFault::Data) {
		invoke.Bad(Unsupported);
		CHECK(f.core.Receive(f.wa, f.s, empty).syscall_errno == WouldBlock);
	} else {
		invoke.Bad(Interrupted);
		Ticket fresh; CHECK(f.core.InspectInvoke(f.ca, f.a, fresh).syscall_errno == Ok && fresh != ticket && !fresh->Poll());
		CHECK(f.core.Receive(f.wa, f.s, empty).syscall_errno == Ok && empty.method == 0x160008);
	}
}

static void ServerPublicationFaultsAndClientValidation() {
	for (int shape: {0, 1, 2}) { // valid; exceeds caller capacity; mismatched output count
		Fixture f; f.Register(); const auto session = f.ConnectA();
		const auto output = f.Alloc(Bytes(24, 0xcc)), method = f.Alloc(Bytes(4, 0xaa));
		const auto buffers = f.Alloc(Words({output,24,0xbeef}));
		Bytes descriptor; CHECK(IpmiWire::EncodeInvoke320({0x160008,0,1,0,buffers,method}, descriptor) == IpmiWire::Error::Ok);
		auto invoke = f.Saved(f.ca, 0x320, f.a, descriptor);
		f.pump = [&] { return Interrupted; }; invoke.Bad(Interrupted);
		Ticket ticket; CHECK(f.core.InspectInvoke(f.ca, f.a, ticket).syscall_errno == Ok);
		f.Receive(f.wa);
		const auto data = f.Alloc(Bytes(shape == 1 ? 25 : 24, 0x57));
		const auto source = f.Alloc(Words({data,0,shape == 1 ? 25u : 24u}));
		Bytes reply_desc; CHECK(IpmiWire::EncodeReply232({0,shape == 2 ? 0u : 1u,source}, reply_desc) == IpmiWire::Error::Ok);
		auto reply = f.Saved(f.wa, 0x232, session, reply_desc);
		if (shape == 0) {
			for (uint64_t bad_source: {source, data}) {
				f.fail_in = bad_source; reply.Bad(14); f.fail_in = 0;
				CHECK(!ticket->Poll() && !ticket->InspectReply().receiver_released);
				Dispatch busy; CHECK(f.core.Receive(f.wa, f.s, busy).syscall_errno == Busy);
			}
			f.fail_out = reply.result; reply.Bad(14); f.fail_out = 0;
			CHECK(ticket->Poll() && ticket->InspectReply().receiver_released && !ticket->InspectReply().caller_released);
			// Final server opResult failure cannot undo publication or replay it.
			reply.Bad(Invalid);
		} else { reply.Good(); }
		f.pump = [&] { CHECK(false); return Interrupted; };
		if (shape == 0) {
			invoke.Good(); CHECK(f.Load(output, 24) == Bytes(24, 0x57));
		} else {
			invoke.Bad(Unsupported);
			CHECK(f.Load(output, 24) == Bytes(24, 0xcc) && Get(f.memory, method, 4) == 0xaaaaaaaa);
			CHECK(f.Load(buffers, 24) == Words({output,24,0xbeef}));
			CHECK(!ticket->InspectReply().consumed && !ticket->InspectReplyPacket().empty());
		}
		CHECK(ticket->InspectReply().caller_released);
	}
}

static void PendingBudgetAndClientAuthorization() {
	Fixture f(IpmiWire::SyncType::Inline, 1); f.Register();
	const auto method_a = f.Alloc(Bytes(4)), method_b = f.Alloc(Bytes(4));
	auto a = f.Saved(f.ca, 0x400, f.a, Words({0,0,method_a,0}));
	auto b = f.Saved(f.cb, 0x400, f.b, Words({0,0,method_b,0}));
	f.pump = [&] { return Interrupted; };
	a.Bad(Interrupted); b.Bad(NoMemory); a.Bad(Interrupted); // full budget never evicts/resubmits a
	auto received = f.Receive(f.wa); CHECK(Get(received.info, 12, 4) == static_cast<uint32_t>(f.a));
	f.Session(f.wa); f.Good(f.wa, 0x212, f.a, Bytes(4)); a.Good();
	Dispatch empty; CHECK(f.core.Receive(f.wb, f.s, empty).syscall_errno == WouldBlock);
	b.Bad(Interrupted); received = f.Receive(f.wb); CHECK(Get(received.info, 12, 4) == static_cast<uint32_t>(f.b));
	f.Session(f.wb, Fixture::session_object + 1); f.Good(f.wb, 0x212, f.b, Bytes(4)); b.Good();
	Fixture auth;
	auth.cap62 = false;
	auth.Good(auth.ca, 2, -1, auth.Registration(Fixture::client_object, auth.ClientConfigBytes()));
	auth.cap62 = true;
	auth.Good(auth.ca, 2, -1, auth.Registration(Fixture::client_object, auth.ClientConfigBytes(), "UserService"));
	CHECK(auth.capability_calls == 0 && auth.client_policy_calls == 2);
	auth.allow_clients = false;
	auth.Bad(auth.ca, 2, -1, auth.Registration(Fixture::client_object, auth.ClientConfigBytes()), Unsupported);
	Fixture excluded(IpmiWire::SyncType::Inline, 64, false);
	excluded.Bad(excluded.ca, 2, -1, excluded.Registration(Fixture::client_object, excluded.ClientConfigBytes()), Unsupported);
	CHECK(excluded.capability_calls == 0);
}

int main() {
	RegistrationQueriesAndDescriptorContract(); ConnectBytesAndTwoClientAcceptance();
	NativeProducedReplyAndDisconnect(IpmiWire::SyncType::Inline);
	NativeProducedReplyAndDisconnect(IpmiWire::SyncType::Request);
	PackedInvokeInputCopy();
	ReceiveCopyFailureAndFeatureGates();
	RetainedWaitTransactions(); PendingBudgetAndClientAuthorization();
	for (auto fault: {CompletionFault::None, CompletionFault::Data, CompletionFault::Method,
	                  CompletionFault::Descriptors, CompletionFault::FinalResult}) {
		ClientCompletionCopyOrder(1, fault);
		if (fault != CompletionFault::Data && fault != CompletionFault::Descriptors) { ClientCompletionCopyOrder(0, fault); }
	}
	ServerPublicationFaultsAndClientValidation();
	std::cout << "IPMI isolated gateway tests passed\n";
}
