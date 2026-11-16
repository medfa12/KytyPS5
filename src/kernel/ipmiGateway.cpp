#include "kernel/ipmiGateway.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

namespace Libs::LibKernel::IpmiGateway {
namespace {
using namespace Ipmi;
namespace Wire = IpmiWire;

uint32_t U32(const uint8_t* p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t U64(const uint8_t* p) { return U32(p) | (uint64_t(U32(p + 4)) << 32); }
int32_t I32(const uint8_t* p) {
	const uint32_t n = U32(p);
	return n <= INT32_MAX ? static_cast<int32_t>(n) : -1 - static_cast<int32_t>(UINT32_MAX - n);
}
Bytes Word(uint64_t value, size_t size) {
	Bytes b(size);
	for (size_t i = 0; i < size; ++i) { b[i] = static_cast<uint8_t>(value >> (8 * i)); }
	return b;
}
int Codec(Wire::Error error) {
	// Codec budgets/invalid shapes have no verified native error translation.
	return error == Wire::Error::Ok ? Ok : error == Wire::Error::NoMemory ? NoMemory : Unsupported;
}
int Diagnostic(Core& core, Callbacks& cb, uint32_t selector, uint64_t result_address,
               uint64_t descriptor, uint64_t size) {
	if (!descriptor || size != 0x40) { return Invalid; }
	std::array<uint8_t, 0x40> bytes {};
	if (const int error = cb.copy_in(descriptor, bytes.data(), bytes.size())) { return error; }
	DiagnosticEvent event;
	event.selector_input = selector;
	event.selector = static_cast<uint16_t>(selector);
	event.caller = cb.diagnostic_caller ? cb.diagnostic_caller() : DiagnosticCaller {cb.identity(), {}, {}};
	const int32_t handle = I32(bytes.data());
	// Common lookup precedes selector-specific override, including inline name.
	// Core handles are monotonic/non-reused: exact lookup supplies the isolated
	// model's generation/liveness check without decoding invented native bits.
	if (handle != -1) { event.server_name = core.TraceServerName(static_cast<Handle>(handle)); }
	switch (selector) {
	case 0x1f00:
		event.handle = static_cast<uint16_t>(handle);
		event.a = U32(bytes.data() + 0x10); event.p = U64(bytes.data() + 8);
		break;
	case 0x1f01: case 0x1f03: case 0x1f05:
		event.handle = static_cast<uint16_t>(handle);
		event.a = U32(bytes.data() + 8); event.p = 0;
		break;
	case 0x1f02: {
		// Native bounded strncpy pads after the first NUL, overriding any
		// registered name from the common lookup, even for an empty inline name.
		event.server_name.fill(0);
		const auto end = std::find(bytes.data() + 8, bytes.data() + 8 + event.server_name.size(), uint8_t {0});
		std::copy(bytes.data() + 8, end, event.server_name.begin());
		event.a = U32(bytes.data() + 0x24); event.p = 0;
		break;
	}
	case 0x1f04:
		event.handle = static_cast<uint16_t>(handle);
		event.a = U32(bytes.data() + 8); event.p = U64(bytes.data() + 0x10);
		event.b = U32(bytes.data() + 0x18);
		break;
	default:
		if (cb.diagnostic_unknown) { cb.diagnostic_unknown(event); }
		break;
	}
	if (!(event.selector & 0x8000) && cb.diagnostic_enabled && cb.diagnostic_submit && cb.diagnostic_enabled()) {
		cb.diagnostic_submit(event);
	}
	const std::array<uint8_t, 4> result {};
	return cb.copy_out(result_address, result.data(), result.size());
}
using PendingKey = std::tuple<int32_t, int32_t, uint32_t, int32_t>;
struct Pending {
	uint64_t result_address, descriptor, desc_size, tag;
	std::array<uint8_t, 0x40> descriptor_bytes {};
	Bytes output_descriptors;
	Ticket ticket;
	bool active = true;
	bool terminal = false; // invoke completion cannot be replayed after any terminal copy fault
};
struct Call {
	Core& core;
	Callbacks& cb;
	const Wire::Limits& limits;
	Wire::SyncType receive_type;
	Identity id;
	uint64_t result_address;
	std::array<uint8_t, 0x40> d {};
	Pending* pending = nullptr;
	uint64_t Q(size_t offset) const { return U64(d.data() + offset); }
	uint32_t D(size_t offset) const { return U32(d.data() + offset); }
	int In(uint64_t address, void* data, size_t size) {
		if (!size) { return Ok; }
		if (size > UINT64_MAX - address) { return Unsupported; }
		return cb.copy_in(address, data, size);
	}
	int Out(uint64_t address, const void* data, size_t size) {
		if (!size) { return Ok; }
		if (size > UINT64_MAX - address || result_address > UINT64_MAX - 4) { return Unsupported; }
		// Exclude overlapping outputs rather than corrupting opResult on a later
		// failed copy. No guessed cross-buffer copyout rollback is attempted.
		if (address < result_address + 4 && result_address < address + size) { return Unsupported; }
		return cb.copy_out(address, data, size);
	}
	int Out(uint64_t address, const Bytes& bytes) { return Out(address, bytes.data(), bytes.size()); }
	int ReadBytes(uint64_t address, uint64_t size, uint64_t budget, Bytes& bytes) {
		if (size > budget || size > SIZE_MAX || size > UINT32_MAX) { return Unsupported; }
		bytes.resize(static_cast<size_t>(size)); return In(address, bytes.data(), bytes.size());
	}
	int Name(uint64_t address, std::string& name) {
		for (size_t i = 0; i < 25; ++i) {
			uint8_t ch = 0;
			if (address > UINT64_MAX - i) { return Unsupported; }
			if (const int e = In(address + i, &ch, 1)) { return e; }
			if (!ch) { return name.empty() ? Invalid : Ok; }
			name.push_back(static_cast<char>(ch));
		}
		return Unsupported;
	}
	int AuthorizeName(const std::string& name) {
		if (!cb.has_capability62) { return Unsupported; }
		const bool reserved = name.compare(0, 3, "Sce") == 0;
		return reserved == cb.has_capability62(id) ? Ok : 1;
	}
	int ConfigPayload(const Config& c, Bytes argument, Bytes& out) {
		Wire::Connect connect;
		connect.client_pid = id.pid; connect.sync_count = c.sync_slots;
		connect.sync_input_limit = c.input_limit; connect.sync_output_limit = c.output_limit;
		connect.async_count = c.async_slots; connect.async_input_limit = c.async_input_limit;
		connect.async_output_limit = c.async_output_limit; connect.event_count = c.event_count;
		connect.queue_sizes = c.queue_capacities; connect.argument = {argument.data(), argument.size()};
		return Codec(Wire::EncodeConnectPayload(connect, out, limits));
	}
	int Register(uint32_t op, int32_t& result) {
		std::string name;
		if (const int e = Name(Q(8), name)) { return e; }
		if (op == 0) {
			if (const int e = AuthorizeName(name)) { return e; }
		} else {
			if (!cb.authorize_client_registration) { return Unsupported; }
			if (const int e = cb.authorize_client_registration(id, name)) { return e; }
		}
		Handle handle = 0; Result r;
		if (op == 0) {
			std::array<uint8_t, 0x38> b {};
			if (const int e = In(Q(16), b.data(), b.size())) { return e; }
			if (U64(b.data()) != b.size()) { return Unsupported; }
			ServerConfig c;
			c.native_object = Q(0); c.argument_limit = U64(b.data() + 8); c.transfer_limit = U64(b.data() + 16);
			c.multi = U32(b.data() + 0x18); c.cross_no_cap = U32(b.data() + 0x1c); c.restricted = U32(b.data() + 0x20);
			// 13.60 kernel 80900f74..80900fb4 validates all three as 0/1.
			if (c.multi > 1 || c.cross_no_cap > 1 || c.restricted > 1) { return Invalid; }
			c.user_argument = U64(b.data() + 0x28); c.handler = U64(b.data() + 0x30);
			if (c.argument_limit > limits.max_argument_bytes || c.transfer_limit > limits.max_frame_bytes) { return Unsupported; }
			r = core.RegisterServer(id, name, c, handle);
		} else {
			std::array<uint8_t, 0x150> b {};
			if (const int e = In(Q(16), b.data(), b.size())) { return e; }
			if (U64(b.data()) != b.size()) { return Unsupported; }
			Config c;
			c.native_object = Q(0); c.sync_flag = U32(b.data() + 0x144);
			if (c.sync_flag > 1) { return Unsupported; }
			c.sync_slots = c.sync_flag == 0 ? 1 : U32(b.data() + 8);
			c.input_limit = U64(b.data() + 0x10); c.output_limit = U64(b.data() + 0x18);
			c.async_slots = U32(b.data() + 0x20); c.async_input_limit = U64(b.data() + 0x28); c.async_output_limit = U64(b.data() + 0x30);
			c.queue_count = U32(b.data() + 0x38); c.event_count = U32(b.data() + 0x140); c.user_argument = U64(b.data() + 0x148);
			if (c.queue_count > 32) { return Unsupported; }
			for (size_t i = 0; i < c.queue_count; ++i) { c.queue_capacities.push_back(U64(b.data() + 0x40 + i * 8)); }
			Bytes checked;
			if (const int e = ConfigPayload(c, {}, checked)) { return e; }
			r = core.RegisterClient(id, name, c, handle);
		}
		if (r.syscall_errno) { return r.syscall_errno; }
		result = static_cast<int32_t>(handle); return Ok;
	}
	int Query(uint32_t op, Handle kid) {
		if (op == 0x302) {
			int32_t pid = 0; const auto r = core.SessionPeerPid(id, kid, pid);
			return r.syscall_errno ? r.syscall_errno : Out(Q(0), Word(static_cast<uint32_t>(pid), 4));
		}
		ObjectInfo info;
		const auto kind = op == 0x466 || op == 0x46a ? ObjectKind::Server : op == 0x468 ? ObjectKind::Session : ObjectKind::Client;
		const auto r = core.Inspect(id, kid, kind, info);
		if (r.syscall_errno) { return r.syscall_errno; }
		if (op == 0x46a || op == 0x46b) {
			std::array<char, 25> name {}; std::copy(info.name.begin(), info.name.end(), name.begin());
			return Out(Q(0), name.data(), name.size());
		}
		return Out(Q(0), Word(info.user_argument, 8));
	}
	Result Wait(const Ticket& ticket) {
		for (;;) {
			if (auto r = ticket->PollStatus()) { return std::move(*r); }
			const int e = cb.cooperate();
			if (auto r = ticket->PollStatus()) { return std::move(*r); }
			if (e) { return Result::Failure(e); }
		}
	}
	int ConnectClient(Handle kid) {
		if (pending->ticket) { return FinishMethod(Q(0x10)); }
		if (!cb.cooperate || !cb.authorize_connect || Q(0x18)) { return Unsupported; }
		ConnectionInfo info;
		if (const auto r = core.Connection(id, kid, info); r.syscall_errno) { return r.syscall_errno; }
		// Config +18 enables multiple synchronous slots, not an authentication
		// exemption (kernel 809010ea -> flag10000, 805a6993..805a69c0).
		// +1c/+20 caller-class policy remains outside the verified adapter.
		// +1c is the native NP flag (userland byte+18). It does not waive
		// authorize_connect: the injected policy must resolve the caller class.
		if (info.server_config.restricted) { return Unsupported; }
		if (const int e = cb.authorize_connect(id, info)) { return e; }
		if (!info.server_config.multi && info.client_config.sync_slots >= 2) { return Invalid; }
		Bytes argument, payload;
		if (const int e = ReadBytes(Q(0), Q(8), std::min(limits.max_argument_bytes, info.server_config.argument_limit), argument)) { return e; }
		if (const int e = ConfigPayload(info.client_config, argument, payload)) { return e; }
		if (payload.size() > info.server_config.transfer_limit) { return Unsupported; }
		if (const auto r = core.Connect(id, kid, pending->ticket, std::move(argument), info.server); r.syscall_errno) { return r.syscall_errno; }
		return FinishMethod(Q(0x10));
	}
	int FinishMethod(uint64_t address) {
		const auto r = Wait(pending->ticket);
		if (r.syscall_errno) { return r.syscall_errno; }
		if (!r.method_result) { return Unsupported; }
		return Out(address, Word(static_cast<uint32_t>(*r.method_result), 4));
	}
	int Payload(const Dispatch& dispatch, Bytes& payload, uint32_t& type) {
		type = static_cast<uint32_t>(dispatch.type);
		if (dispatch.type == Dispatch::Type::Connect) {
			Wire::Connect connect;
			connect.client_pid = dispatch.caller.pid; connect.sync_count = dispatch.config.sync_slots;
			connect.sync_input_limit = dispatch.config.input_limit; connect.sync_output_limit = dispatch.config.output_limit;
			connect.async_count = dispatch.config.async_slots; connect.async_input_limit = dispatch.config.async_input_limit;
			connect.async_output_limit = dispatch.config.async_output_limit; connect.event_count = dispatch.config.event_count;
			connect.queue_sizes = dispatch.config.queue_capacities;
			connect.argument = {dispatch.connect_argument.data(), dispatch.connect_argument.size()};
			return Codec(Wire::EncodeConnectPayload(connect, payload, limits));
		}
		if (dispatch.type == Dispatch::Type::Request) {
			Wire::SyncRequest request;
			request.native_session = dispatch.native_session; request.caller_pid = dispatch.caller.pid; request.method = dispatch.method;
			for (const auto& b: dispatch.inputs) { request.inputs.push_back({b.data(), b.size()}); }
			request.output_capacities = dispatch.output_capacities;
			type = static_cast<uint32_t>(receive_type);
			return Codec(Wire::EncodeSyncPayload(request, receive_type, payload, limits));
		}
		if (dispatch.type == Dispatch::Type::Disconnect) { payload = Word(dispatch.native_session, 8); return Ok; }
		if (dispatch.type == Dispatch::Type::Wake) { payload.clear(); return Ok; }
		return Unsupported;
	}
	int Receive(Handle kid) {
		// Non-null pollstate semantics are not verified in this isolated adapter.
		if (Q(0x18)) { return Unsupported; }
		Dispatch dispatch;
		for (;;) {
			const auto r = core.BeginReceive(id, kid, dispatch);
			if (!r.syscall_errno) { break; }
			if (r.syscall_errno != WouldBlock || !cb.cooperate) { return r.syscall_errno; }
			if (const int e = cb.cooperate()) { return e; }
		}
		struct Reservation {
			Core& core; Identity id; Handle kid; bool active = true;
			~Reservation() { if (active) { core.EndReceive(id, kid, false); } }
		} reservation {core, id, kid};
		Bytes payload, info; uint32_t type = 0;
		if (const int e = Payload(dispatch, payload, type)) { return e; }
		if (payload.size() > Q(8)) { return NoMemory; } // host capacity rejection, not a claimed native mapping
		if (const int e = Codec(Wire::EncodeReceiveInfo({payload.size(), type, static_cast<int32_t>(dispatch.client), dispatch.handler}, info, limits))) { return e; }
		if (const int e = Out(Q(0), payload)) { return e; }
		if (const int e = Out(Q(0x10), info)) { return e; }
		const auto r = core.EndReceive(id, kid, true);
		if (!r.syscall_errno) { reservation.active = false; }
		return r.syscall_errno;
	}
	int Invoke(Handle kid) {
		if (pending->ticket) { return FinishInvoke(kid); }
		if (D(0x28) != 1 || !cb.cooperate) { return Unsupported; }
		const uint32_t ni = D(4), no = D(8);
		if (ni > limits.max_inputs || no > limits.max_outputs || ni > 10 || no > 10) { return Unsupported; }
		Bytes input_descriptors(ni * 16), output_descriptors(no * 24);
		if (const int e = In(Q(0x10), input_descriptors.data(), input_descriptors.size())) { return e; }
		if (const int e = In(Q(0x18), output_descriptors.data(), output_descriptors.size())) { return e; }
		std::vector<Bytes> inputs; std::vector<uint64_t> capacities; uint64_t total = 0;
		for (size_t i = 0; i < ni; ++i) {
			const auto* p = input_descriptors.data() + i * 16; Bytes b;
			if (const int e = ReadBytes(U64(p), U64(p + 8), limits.max_input_bytes - total, b)) { return e; }
			total += b.size(); inputs.push_back(std::move(b));
		}
		Wire::SyncRequest checked; checked.caller_pid = id.pid; checked.method = D(0);
		for (const auto& b: inputs) { checked.inputs.push_back({b.data(), b.size()}); }
		for (size_t i = 0; i < no; ++i) { capacities.push_back(U64(output_descriptors.data() + i * 24 + 8)); }
		checked.output_capacities = capacities; Bytes payload;
		if (const int e = Codec(Wire::EncodeSyncPayload(checked, receive_type, payload, limits))) { return e; }
		ConnectionInfo connection;
		if (const auto r = core.Connection(id, kid, connection); r.syscall_errno) { return r.syscall_errno; }
		if (payload.size() > connection.server_config.transfer_limit) { return Unsupported; }
		pending->output_descriptors = std::move(output_descriptors);
		if (const auto r = core.Invoke(id, kid, D(0), std::move(inputs), capacities, pending->ticket); r.syscall_errno) { return r.syscall_errno; }
		return FinishInvoke(kid);
	}
	int FinishInvoke(Handle kid) {
		const uint32_t no = D(8);
		const auto r = Wait(pending->ticket);
		if (r.syscall_errno == Interrupted) { return Interrupted; }
		// Only the interrupted native wait is resumable. DATA faults unreserve
		// the packet but still release the caller; later copy faults occur after
		// consumption. No completed-response cache authorizes replay.
		struct Completion {
			Core& core; Identity id; Handle kid; Pending& pending; bool reserved = false;
			~Completion() {
				if (reserved) { core.EndInvokeRead(id, kid, pending.ticket, false); }
				core.ReleaseInvokeCaller(id, kid, pending.ticket);
				pending.terminal = true;
			}
		} completion {core, id, kid, *pending};
		if (r.syscall_errno) { return r.syscall_errno; }
		Result response;
		if (const auto begin = core.BeginInvokeRead(id, kid, pending->ticket, response); begin.syscall_errno) { return begin.syscall_errno; }
		completion.reserved = true;
		if (!response.method_result || response.outputs.size() != no) { return Unsupported; }
		auto output_descriptors = pending->output_descriptors;
		// Validate the complete response before any DATA copy. Server publication
		// does not enforce this caller's count/capacities.
		for (size_t i = 0; i < no; ++i) {
			if (response.outputs[i].size() > U64(output_descriptors.data() + i * 24 + 8)) { return Unsupported; }
		}
		for (size_t i = 0; i < no; ++i) {
			if (const int e = Out(U64(output_descriptors.data() + i * 24), response.outputs[i])) { return e; }
			const auto written = Word(response.outputs[i].size(), 8);
			std::copy(written.begin(), written.end(), output_descriptors.begin() + i * 24 + 16);
		}
		if (const auto consumed = core.EndInvokeRead(id, kid, pending->ticket, true); consumed.syscall_errno) { return consumed.syscall_errno; }
		completion.reserved = false;
		if (const int e = Out(Q(0x20), Word(static_cast<uint32_t>(*response.method_result), 4))) { return e; }
		// Native copies the WHOLE saved BufferInfo array after methodResult.
		// Restore pointer/capacity even if guest code changed them while waiting.
		return Out(Q(0x18), output_descriptors);
	}
	int Reply(Handle kid) {
		if (D(0x10) != 1 || D(4) > limits.max_outputs || D(4) > 10) { return Unsupported; }
		Bytes descriptors(D(4) * 24);
		if (const int e = In(Q(8), descriptors.data(), descriptors.size())) { return e; }
		uint64_t total = 0; std::vector<Bytes> outputs;
		for (size_t i = 0; i < D(4); ++i) {
			const auto* p = descriptors.data() + i * 24; Bytes b;
			// Server supplies written bytes; its BufferInfo capacity is not the
			// waiting client's capacity and must not gate publication here.
			if (const int e = ReadBytes(U64(p), U64(p + 16), limits.max_output_bytes - total, b)) { return e; }
			total += b.size(); outputs.push_back(std::move(b));
		}
		return core.Reply(id, kid, I32(d.data()), std::move(outputs)).syscall_errno;
	}
	int Run(uint32_t op, int32_t kid, int32_t& result) {
		if (op == 0 || op == 2) { return Register(op, result); }
		if (op == 4) {
			if (kid != -1) { return Unsupported; }
			std::array<uint8_t, 16> init {};
			if (const int e = In(Q(8), init.data(), init.size())) { return e; }
			if (U64(init.data()) != 16) { return Unsupported; }
			Handle h = 0; const auto r = core.CreateSession(id, Q(0), U64(init.data() + 8), h);
			if (!r.syscall_errno) { result = static_cast<int32_t>(h); }
			return r.syscall_errno;
		}
		const auto h = kid < 0 ? Handle(0) : static_cast<Handle>(kid);
		switch (op) {
		case 1: return core.DestroyServer(id, h).syscall_errno;
		case 3: return core.DestroyClient(id, h).syscall_errno;
		case 5: return core.CloseSession(id, h).syscall_errno;
		case 0x466: case 0x467: case 0x468: case 0x46a: case 0x46b: case 0x302: return Query(op, h);
		case 0x400: return ConnectClient(h);
		case 0x201: return Receive(h);
		case 0x212: return core.Accept(id, h, I32(d.data())).syscall_errno;
		case 0x222: return core.AcknowledgeDisconnect(id, h, I32(d.data())).syscall_errno;
		case 0x202:
			if (Q(0) != 1 || Q(8) || Q(16)) { return Unsupported; }
			return core.Wake(id, h).syscall_errno;
		case 0x320: return Invoke(h);
		case 0x232: return Reply(h);
		case 0x310: {
			if (!cb.cooperate) { return Unsupported; }
			if (!pending->ticket) {
				if (const auto r = core.Disconnect(id, h, pending->ticket); r.syscall_errno) { return r.syscall_errno; }
			}
			return FinishMethod(Q(0));
		}
		default: return Unsupported; // async, events, queues and teardown are not gateway-enabled
		}
	}
};
} // namespace

struct Gateway::Impl {
	std::mutex mutex;
	std::map<PendingKey, std::shared_ptr<Pending>> pending;
	size_t capacity;
	explicit Impl(size_t capacity): capacity(std::min(size_t(64), capacity)) {}
};
Gateway::Gateway(Ipmi::Core& c, Callbacks cb, IpmiWire::Limits l, IpmiWire::SyncType type, size_t max_pending):
	m(std::make_unique<Impl>(max_pending)), core(c), callbacks(std::move(cb)), limits(l), receive_type(type) {}
Gateway::~Gateway() = default;

int Gateway::Call(uint32_t op, int32_t kid, uint64_t op_result, uint64_t descriptor, uint64_t desc_size, uint64_t tag) {
	auto fail = [&](int error) { if (callbacks.set_errno) { callbacks.set_errno(error); } return -1; };
	if (!callbacks.copy_in || !callbacks.copy_out || !callbacks.identity || !callbacks.set_errno) { return fail(Ipmi::Unsupported); }
	try {
		int32_t result = 0;
		if (desc_size > 0x40) {
			const auto bytes = Word(0x800e0001u, 4);
			const int error = callbacks.copy_out(op_result, bytes.data(), bytes.size());
			return error ? fail(error) : 0;
		}
		if (op == 0x10) {
			const int error = Diagnostic(core, callbacks, static_cast<uint32_t>(kid), op_result, descriptor, desc_size);
			return error ? fail(error) : 0;
		}
		if (receive_type != IpmiWire::SyncType::Request && receive_type != IpmiWire::SyncType::Inline) { return fail(Ipmi::Unsupported); }
		struct Call call {core, callbacks, limits, receive_type, callbacks.identity(), op_result};
		const PendingKey key {call.id.pid, call.id.tid, op, kid};
		const bool wait_operation = op == 0x400 || op == 0x320 || op == 0x310;
		std::shared_ptr<Pending> pending;
		int admission_error = 0;
		{
			std::lock_guard<std::mutex> lock(m->mutex);
			// Reject a different operation on a suspended guest thread as well as
			// concurrent/reentrant attempts to finish the same transaction twice.
			for (const auto& entry: m->pending) {
				if (wait_operation && std::get<0>(entry.first) == call.id.pid && std::get<1>(entry.first) == call.id.tid) {
					const auto& p = entry.second;
					if (entry.first != key || p->active || p->result_address != op_result ||
					    p->descriptor != descriptor || p->desc_size != desc_size || p->tag != tag) { admission_error = Ipmi::Busy; }
					else { pending = p; pending->active = true; }
					break;
				}
			}
			if (!admission_error && !pending && wait_operation) {
				if (m->pending.size() >= m->capacity) { admission_error = Ipmi::NoMemory; }
				else {
					pending = std::make_shared<Pending>();
					pending->result_address = op_result; pending->descriptor = descriptor;
					pending->desc_size = desc_size; pending->tag = tag;
					m->pending.emplace(key, pending);
				}
			}
		}
		if (admission_error) { return fail(admission_error); }
		struct Active {
			Impl& state; const PendingKey& key; std::shared_ptr<Pending> pending; bool completed = false;
			~Active() {
				if (!pending) { return; }
				std::lock_guard<std::mutex> lock(state.mutex);
				if (completed || pending->terminal || !pending->ticket) { state.pending.erase(key); }
				else { pending->active = false; }
			}
		} active {*m, key, pending};
		call.pending = pending.get();
		if (pending && pending->ticket) { call.d = pending->descriptor_bytes; }
		else {
			if (const int error = call.In(descriptor, call.d.data(), static_cast<size_t>(desc_size))) { return fail(error); }
			if (pending) { pending->descriptor_bytes = call.d; }
		}
		// Verified default tag and invoke's zero-extended method-ID tag are opaque
		// correlation values, not identity or authentication. Zero remains a host-test tag.
		if (tag != 0 && tag != NativeDefaultTag && !(op == 0x320 && tag == call.D(0))) { return fail(Ipmi::Unsupported); }
		if (const int error = call.Run(op, kid, result)) { return fail(error); }
		const auto bytes = Word(static_cast<uint32_t>(result), 4);
		if (const int error = callbacks.copy_out(op_result, bytes.data(), bytes.size())) { return fail(error); }
		active.completed = true;
		return 0;
	} catch (const std::bad_alloc&) { return fail(Ipmi::NoMemory); }
}
} // namespace Libs::LibKernel::IpmiGateway
