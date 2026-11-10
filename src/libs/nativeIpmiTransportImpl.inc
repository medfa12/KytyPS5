#include "libs/npNativeTransport.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <new>

// Token dispatch keeps this interface out of the AppDb/Login specializations.
#define KYTY_IPMI_RETAINED_NpNativeTransport 1
#define KYTY_IPMI_RETAINED_AppDbNativeTransport 0
#define KYTY_IPMI_RETAINED_LoginMgrNativeTransport 0
#define KYTY_IPMI_RETAINED_DbPreparationNativeTransport 0
#define KYTY_IPMI_RETAINED_DbRecoveryNativeTransport 0
#define KYTY_IPMI_RETAINED_SELECT_(name) KYTY_IPMI_RETAINED_##name
#define KYTY_IPMI_RETAINED_SELECT(name) KYTY_IPMI_RETAINED_SELECT_(name)
#define KYTY_IPMI_HAS_RETAINED KYTY_IPMI_RETAINED_SELECT(KYTY_IPMI_ADAPTER_NAMESPACE)

// Compiled separately for explicitly audited services. Each namespace has
// its own registry, Core, hooks and vtables; there is no arbitrary-name factory.
namespace Libs::KYTY_IPMI_ADAPTER_NAMESPACE {
namespace {
using namespace LibKernel::Ipmi;
constexpr char Name[] = KYTY_IPMI_ADAPTER_NAME;
constexpr bool EnableEvents = KYTY_IPMI_ADAPTER_EVENTS;
constexpr uint64_t Budget = 2 * 1024 * 1024;
int Guest(int error) { return error ? static_cast<int>(0x80020000u | static_cast<uint32_t>(error)) : 0; }
int ReplyError(int error) {
	if (error == Guest(NotFound)) { return Guest(NotConnected); }
	if (error == Guest(14) || error == Guest(Invalid)) { return Guest(BadMessage); }
	if (error == Guest(NoMemory)) { return Guest(0x28); }
	return error;
}
struct Fault { int error; };
void Check(int error) { if (error) { throw Fault {error}; } }
void Check(Result r) { Check(r.syscall_errno); }
template<class F> int Safe(F&& f) noexcept {
	try { return f(); } catch (const Fault& f) { return Guest(f.error); }
	catch (const std::bad_alloc&) { return Guest(NoMemory); }
	catch (...) { return Guest(BadMessage); }
}
uint64_t Address(const void* p) { return reinterpret_cast<uint64_t>(p); }
struct Object {
	ObjectKind kind;
	Identity owner {};
	Handle handle = 0;
	uint64_t native = 0, argument = 0, handler = 0, work_size = 0;
	bool multi = false;
	Handle parent = 0;
	bool published = false;
	bool retirement = false;
	std::mutex shutdown_mutex;
	std::atomic_bool stopping {false}, dispatching {false};
#if KYTY_IPMI_HAS_RETAINED
	std::shared_ptr<void> storage_lease;
	std::atomic_bool destroyed {false}; // Only successful published-server destroy + exact erasure.
	DispatcherReceipt dispatcher_receipt; // Immutable after publication under shutdown_mutex + Runtime::mutex.
	DispatcherProof dispatcher_proof; // Monotonic observations under shutdown_mutex.
	std::atomic_bool inspecting_dispatcher {false};
#endif
};
#if KYTY_IPMI_HAS_RETAINED
struct RetainedState;
#endif
struct Runtime {
	Hooks hooks;
	Core core;
	std::mutex mutex;
	std::map<uint64_t, std::shared_ptr<Object>> objects;
#if KYTY_IPMI_HAS_RETAINED
	// Entries deliberately pin Runtime until explicit successful retirement.
	std::map<Handle, std::shared_ptr<RetainedState>> retained;
#endif
	explicit Runtime(Hooks h): hooks(std::move(h)) {}
	Identity Id() { auto id = hooks.identity(); if (id.pid <= 0 || id.tid <= 0) { throw Fault {Invalid}; } return id; }
#if KYTY_IPMI_HAS_RETAINED
	std::shared_ptr<void> RetainStorage(uint64_t address, size_t size) {
		std::shared_ptr<void> lease;
		if (hooks.retain_storage) {
			Check(hooks.retain_storage(address, size, lease));
			if (!lease) { throw Fault {BadMessage}; }
		}
		return lease;
	}
#endif
	void Read(uint64_t p, void* out, size_t size) {
		if (size && (!p || size > UINT64_MAX - p)) { throw Fault {14}; }
		if (size) { Check(hooks.copy_in(p, out, size)); }
	}
	void Write(uint64_t p, const void* in, size_t size) {
		if (size && (!p || size > UINT64_MAX - p)) { throw Fault {14}; }
		if (size) { Check(hooks.copy_out(p, in, size)); }
	}
	template<class T> T Read(const void* p) { T value {}; Read(Address(p), &value, sizeof(value)); return value; }
	template<class T> void Write(T* p, const T& value) { Write(Address(p), &value, sizeof(value)); }
	std::shared_ptr<Object> Find(const void* p, ObjectKind kind) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = objects.find(Address(p));
		if (i == objects.end() || !i->second->published || i->second->kind != kind) { throw Fault {NotFound}; }
		return i->second;
	}
	void Erase(const std::shared_ptr<Object>& object) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = objects.find(object->native);
		if (i != objects.end() && i->second == object) { objects.erase(i); }
	}
	Result Release(Identity id, const Object& object) {
		if (object.kind == ObjectKind::Server) { return core.DestroyServer(id, object.handle); }
		if (object.kind == ObjectKind::Client) { return core.DestroyClient(id, object.handle); }
		return core.CloseSession(id, object.handle);
	}
	void PruneSessions(Identity id, Handle server) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto i = objects.begin(); i != objects.end();) {
			int32_t pid = 0;
			if (i->second->kind == ObjectKind::Session && i->second->parent == server &&
			    core.SessionPeerPid(id, i->second->handle, pid).syscall_errno == NotFound) { i = objects.erase(i); }
			else { ++i; }
		}
	}
	int64_t Callback(const Object& server, size_t slot, std::initializer_list<uint64_t> args) {
		uint64_t table = 0, entry = 0;
		Read(server.handler, &table, 8);
		if (table > UINT64_MAX - slot) { throw Fault {14}; }
		Read(table + slot, &entry, 8);
		if (!entry) { throw Fault {BadMessage}; }
		int64_t result = 0;
		Check(hooks.invoke_guest(entry, args.begin(), args.size(), result));
		return result;
	}
	Result Wait(const Ticket& ticket) {
		for (;;) {
			core.ExpireEvents();
			if (auto result = ticket->PollStatus()) { return *result; }
			const int error = hooks.cooperate();
			if (auto result = ticket->PollStatus()) { return *result; }
			// Native wrappers retry EINTR. Keep this same operation/ticket rather
			// than issuing another connect/invoke when an interrupt is pumped.
			if (error && error != Interrupted) { throw Fault {error}; }
		}
	}
};
std::mutex global_mutex;
std::shared_ptr<Runtime> installed;
std::shared_ptr<Runtime> Current() {
	std::lock_guard<std::mutex> lock(global_mutex);
	if (!installed) { throw Fault {Unsupported}; }
	return installed;
}

#if KYTY_IPMI_HAS_RETAINED
// Native accept handlers can create then destroy a rejected session before
// returning. Retain each such object until the entire dispatch turn ends,
// independently of registry erasure or callback-local allocator frees.
struct CallbackPins {
	Runtime* runtime;
	std::vector<std::shared_ptr<Object>> created;
	CallbackPins* previous;
	static thread_local CallbackPins* current;
	explicit CallbackPins(Runtime& r): runtime(&r), previous(current) { current = this; }
	~CallbackPins() { current = previous; }
};
thread_local CallbackPins* CallbackPins::current = nullptr;
#endif

// Provisional records reserve the native address before any Core publication.
// A failing factory never leaves a discoverable transport object behind.
struct Publication {
	std::shared_ptr<Runtime> runtime;
	std::shared_ptr<Object> object;
	Identity id;
	bool committed = false;
	Publication(std::shared_ptr<Runtime> r, ObjectKind kind, void* storage): runtime(std::move(r)), object(std::make_shared<Object>()), id(runtime->Id()) {
		if (!storage) { throw Fault {Invalid}; }
		object->kind = kind; object->native = Address(storage); object->owner = id;
#if KYTY_IPMI_HAS_RETAINED
		object->storage_lease = runtime->RetainStorage(object->native, kind == ObjectKind::Server ? 0x30 : 0x10);
#endif
		std::lock_guard<std::mutex> lock(runtime->mutex);
		if (!runtime->objects.emplace(object->native, object).second) { throw Fault {Busy}; }
	}
	~Publication() {
		if (committed) { return; }
		try {
			if (!object->handle || !runtime->Release(id, *object).syscall_errno) {
				runtime->Erase(object); return;
			}
		} catch (...) {
			// Failed rollback retains its address authority and installed owner.
			// Never replace/reuse this backing on a plausible destroy success.
		}
		std::lock_guard<std::mutex> lock(runtime->mutex);
		object->retirement = true;
	}
	void Commit(void** out, void* table, size_t size) {
		std::array<uint64_t, 6> bytes {};
		bytes[0] = Address(table); bytes[1] = object->handle;
		runtime->Write(object->native, bytes.data(), size);
		void* pointer = reinterpret_cast<void*>(object->native);
		runtime->Write(out, pointer);
		std::lock_guard<std::mutex> lock(runtime->mutex);
		object->published = true; committed = true;
		if (object->kind == ObjectKind::Server) { Check(runtime->core.PublishServer(id, object->handle)); }
	}
};

std::vector<Bytes> Inputs(Runtime& r, const DataInfo* input, uint32_t count) {
	if (count > 10) { throw Fault {Invalid}; }
	std::array<DataInfo, 10> descriptors {};
	r.Read(Address(input), descriptors.data(), count * sizeof(DataInfo));
	std::vector<Bytes> bytes; bytes.reserve(count);
	uint64_t total = 0;
	for (uint32_t n = 0; n < count; ++n) {
		if (descriptors[n].size > Budget - total) { throw Fault {Invalid}; }
		total += descriptors[n].size;
		bytes.emplace_back(static_cast<size_t>(descriptors[n].size));
		r.Read(Address(descriptors[n].data), bytes.back().data(), bytes.back().size());
	}
	return bytes;
}

int KYTY_SYSV_ABI UnsupportedCall() { return Guest(Unsupported); }
void* KYTY_SYSV_ABI UnavailablePointer() { return nullptr; }
int KYTY_SYSV_ABI DestroyServer(void* self) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Server);
#if KYTY_IPMI_HAS_RETAINED
		if (o->dispatching) { return Guest(Busy); }
		const auto id = r->Id(); // Identity hook must never run under adapter locks.
		std::lock_guard<std::mutex> shutdown(o->shutdown_mutex);
		if (o->dispatching) { return Guest(Busy); }
		Check(r->core.DestroyServer(id, o->handle));
		{
			std::lock_guard<std::mutex> lock(r->mutex);
			auto i = r->objects.find(o->native);
			if (i != r->objects.end() && i->second == o) {
				r->objects.erase(i);
				o->destroyed = true;
			}
		}
		return 0;
#else
		if (o->dispatching) { return Guest(Busy); }
		Check(r->core.DestroyServer(r->Id(), o->handle)); r->Erase(o); return 0;
#endif
	});
}
int KYTY_SYSV_ABI DestroyClient(void* self) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client);
		Check(r->core.DestroyClient(r->Id(), o->handle)); r->Erase(o); return 0;
	});
}
int KYTY_SYSV_ABI CloseSession(void* self) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Session);
		Check(r->core.CloseSession(r->Id(), o->handle)); r->Erase(o); return 0;
	});
}
int KYTY_SYSV_ABI PeerPid(void* self) {
	int32_t pid = -1;
	const int error = Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Session);
		Check(r->core.SessionPeerPid(r->Id(), o->handle, pid)); return 0;
	});
	return error ? -1 : pid;
}
void* UserData(void* self, ObjectKind kind) {
	void* value = nullptr;
	Safe([&] {
		auto r = Current(); auto o = r->Find(self, kind); ObjectInfo info;
		Check(r->core.Inspect(r->Id(), o->handle, kind, info));
		value = reinterpret_cast<void*>(info.user_argument); return 0;
	});
	return value;
}
void* KYTY_SYSV_ABI ServerUserData(void* self) { return UserData(self, ObjectKind::Server); }
void* KYTY_SYSV_ABI SessionUserData(void* self) { return UserData(self, ObjectKind::Session); }
void* KYTY_SYSV_ABI ClientUserData(void* self) { return UserData(self, ObjectKind::Client); }

int KYTY_SYSV_ABI Reply(void* self, int32_t result, const BufferInfo* output, uint32_t count) {
	return ReplyError(Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Session);
		if (count > 10) { return Guest(BadMessage); }
		std::array<BufferInfo, 10> descriptors {};
		r->Read(Address(output), descriptors.data(), count * sizeof(BufferInfo));
		std::vector<Bytes> bytes; bytes.reserve(count);
		uint64_t total = 0;
		for (uint32_t n = 0; n < count; ++n) {
			// Native reply wrapper44e0 receives BufferInfo records: the published
			// byte count is +10 (written), not the caller's capacity at +8.
			const auto size = descriptors[n].written;
			if (size > Budget - total) { return Guest(BadMessage); }
			total += size; bytes.emplace_back(static_cast<size_t>(size));
			r->Read(Address(descriptors[n].data), bytes.back().data(), bytes.back().size());
		}
		int error = r->core.PublishReply(r->Id(), o->handle, result, std::move(bytes)).syscall_errno;
		if (error == NotFound) { error = NotConnected; }
		else if (error == Invalid || error == 14) { error = BadMessage; }
		else if (error == NoMemory) { error = 0x28; }
		return Guest(error);
	}));
}

int KYTY_SYSV_ABI AsyncReply(void* self, uint32_t method, uint32_t ticket, int32_t result,
                           const BufferInfo* output, uint32_t count) {
	return ReplyError(Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Session);
		if (count > 10) { return Guest(BadMessage); }
		std::array<BufferInfo, 10> descriptors {};
		r->Read(Address(output), descriptors.data(), count * sizeof(BufferInfo));
		std::vector<Bytes> bytes; bytes.reserve(count); uint64_t total = 0;
		for (uint32_t n = 0; n < count; ++n) {
			if (descriptors[n].written > Budget - total) { return Guest(BadMessage); }
			total += descriptors[n].written; bytes.emplace_back(static_cast<size_t>(descriptors[n].written));
			r->Read(Address(descriptors[n].data), bytes.back().data(), bytes.back().size());
		}
		Check(r->core.ReplyAsync(r->Id(), o->handle, method, ticket, result, std::move(bytes))); return 0;
	}));
}

void* SessionTable();
int KYTY_SYSV_ABI CreateSession(void* self, void** out, void* argument, void* storage) {
	return Safe([&] {
		if (!out) { return Guest(Invalid); }
		auto r = Current(); auto server = r->Find(self, ObjectKind::Server);
		Publication p(r, ObjectKind::Session, storage);
		p.object->argument = Address(argument); p.object->parent = server->handle;
		Check(r->core.CreateSession(p.id, Address(storage), Address(argument), p.object->handle, server->handle));
#if KYTY_IPMI_HAS_RETAINED
		if (auto* pins = CallbackPins::current; pins && pins->runtime == r.get()) { pins->created.push_back(p.object); }
#endif
		p.Commit(out, SessionTable(), 0x10); return 0;
	});
}
#if KYTY_IPMI_HAS_RETAINED
// Caller holds the object's shutdown mutex. Shared verbatim by native Stop and
// host lifecycle Stop, with an actual caller identity obtained outside locks.
int StopServer(Runtime& runtime, Object& object, Identity id) {
	if (object.stopping) { return Busy; }
	const auto sealed = runtime.core.SealServer(id, object.handle);
	if (sealed.syscall_errno) { return sealed.syscall_errno; }
	const auto woke = runtime.core.Wake(id, object.handle);
	if (woke.syscall_errno) { return woke.syscall_errno; }
	object.stopping = true;
	return 0;
}
#endif
int KYTY_SYSV_ABI Stop(void* self) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Server);
#if KYTY_IPMI_HAS_RETAINED
		if (o->stopping) { return Guest(Busy); }
		const auto id = r->Id();
		std::lock_guard<std::mutex> lock(o->shutdown_mutex);
		return Guest(StopServer(*r, *o, id));
#else
		std::lock_guard<std::mutex> lock(o->shutdown_mutex);
		if (o->stopping) { return Guest(Busy); }
		// Do not publish stop before the drain gate: a failed stop must not
		// make the actual dispatcher exit while an attached caller still waits.
		Check(r->core.SealServer(r->Id(), o->handle));
		Check(r->core.Wake(r->Id(), o->handle));
		o->stopping = true;
		return 0;
#endif
	});
}

void Put(std::array<uint8_t, 0x150>& bytes, size_t at, uint64_t value, size_t width = 8) {
	for (size_t n = 0; n < width; ++n) { bytes[at + n] = static_cast<uint8_t>(value >> (n * 8)); }
}
void DispatchOne(Runtime& r, const Object& server, Dispatch& d) {
#if KYTY_IPMI_HAS_RETAINED
	CallbackPins callback_pins(r);
#endif
	const auto id = r.Id();
	if (d.type == Dispatch::Type::Connect) {
		std::array<uint8_t, 0x150> config {};
		Put(config, 0, static_cast<uint32_t>(d.caller.pid), 4);
		Put(config, 8, d.config.sync_slots, 4); Put(config, 0x10, d.config.input_limit); Put(config, 0x18, d.config.output_limit);
		Put(config, 0x20, d.config.async_slots, 4); Put(config, 0x28, d.config.async_input_limit); Put(config, 0x30, d.config.async_output_limit);
		Put(config, 0x38, d.config.event_count, 4); Put(config, 0x40, d.config.queue_count, 4);
		for (size_t n = 0; n < d.config.queue_capacities.size(); ++n) { Put(config, 0x48 + 8 * n, d.config.queue_capacities[n]); }
		Put(config, 0x148, 0x10);
		int32_t accepted;
		try {
			accepted = static_cast<int32_t>(r.Callback(server, 0x20,
			    {server.handler, server.native, Address(config.data()), d.connect_argument.empty() ? 0 : Address(d.connect_argument.data()), d.connect_argument.size()}));
		} catch (...) {
			r.core.Accept(id, d.client, Guest(BadMessage));
			r.PruneSessions(id, server.handle);
			throw;
		}
		auto acceptance = r.core.Accept(id, d.client, accepted);
		if (acceptance.syscall_errno && accepted == 0) { r.core.Accept(id, d.client, Guest(BadMessage)); }
		// Drop any host record for a callback-freed rejected native session.
		r.PruneSessions(id, server.handle);
		Check(acceptance);
	} else if (d.type == Dispatch::Type::Request) {
		try {
#if KYTY_IPMI_HAS_RETAINED
			auto session_pin = r.Find(reinterpret_cast<void*>(d.native_session), ObjectKind::Session);
			if (session_pin->handle != d.session) { throw Fault {NotFound}; }
#endif
			std::vector<DataInfo> input; input.reserve(d.inputs.size());
			for (auto& bytes: d.inputs) { input.push_back({bytes.data(), bytes.size()}); }
			std::vector<Bytes> buffers; buffers.reserve(d.output_capacities.size());
			std::vector<BufferInfo> output; output.reserve(d.output_capacities.size());
			for (auto capacity: d.output_capacities) {
				if (capacity > Budget) { throw Fault {Invalid}; }
				buffers.emplace_back(static_cast<size_t>(capacity));
				output.push_back({buffers.back().data(), capacity, 0});
			}
			r.Callback(server, 0x10, {server.handler, d.native_session, d.method, Address(input.data()), input.size(), Address(output.data()), output.size()});
			// The native handler must explicitly call session+30. Never turn its
			// callback return into a plausible successful method reply.
			auto released = r.core.ReleaseReplyReceiver(id, d.session);
			if (released.syscall_errno) { r.core.AbortRequest(id, d.session, BadMessage); Check(released); }
		} catch (...) { r.core.AbortRequest(id, d.session, BadMessage); throw; }
	} else if (d.type == Dispatch::Type::AsyncRequest) {
		try {
#if KYTY_IPMI_HAS_RETAINED
			auto session_pin = r.Find(reinterpret_cast<void*>(d.native_session), ObjectKind::Session);
			if (session_pin->handle != d.session) { throw Fault {NotFound}; }
#endif
			std::vector<DataInfo> input; input.reserve(d.inputs.size());
			for (auto& bytes: d.inputs) { input.push_back({bytes.data(), bytes.size()}); }
			// 13.60 +2948: (handler, session, ticket, method, DataInfo*, count).
			// Return is ignored; completion of dispatch is separate from reply242.
			r.Callback(server, 0x18, {server.handler, d.native_session, d.async_ticket, d.method, Address(input.data()), input.size()});
		} catch (...) { r.core.ReleaseAsyncReceiver(id, d.session); throw; }
		Check(r.core.ReleaseAsyncReceiver(id, d.session));
	} else if (d.type == Dispatch::Type::Disconnect || d.type == Dispatch::Type::ForcedDisconnect) {
#if KYTY_IPMI_HAS_RETAINED
		auto session_pin = r.Find(reinterpret_cast<void*>(d.native_session), ObjectKind::Session);
		if (session_pin->handle != d.session) { throw Fault {NotFound}; }
#endif
		const bool forced = d.type == Dispatch::Type::ForcedDisconnect;
		r.Callback(server, forced ? 0x30 : 0x28, {server.handler, d.native_session});
		if (!forced) { Check(r.core.AcknowledgeDisconnect(id, d.client, 0)); }
	} else if (d.type != Dispatch::Type::Wake) { throw Fault {Unsupported}; }
}

int Run(void* self, void* work, uint64_t size, bool single) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Server);
		if (!work || size < o->work_size) { return Guest(Invalid); }
		if (single && o->multi) { return Guest(1); }
#if KYTY_IPMI_HAS_RETAINED
		auto work_lease = r->RetainStorage(Address(work), static_cast<size_t>(size));
		const auto receiver = r->Id();
		{
			std::lock_guard<std::mutex> shutdown(o->shutdown_mutex);
			std::lock_guard<std::mutex> lock(r->mutex);
			const auto entry = r->objects.find(o->native);
			if (o->destroyed || o->retirement || !o->published || entry == r->objects.end() || entry->second != o) { return Guest(NotFound); }
			if (receiver.pid != o->owner.pid) { return Guest(Invalid); }
			if (o->dispatching.exchange(true)) { return Guest(Busy); }
		}
#else
		if (o->dispatching.exchange(true)) { return Guest(Busy); }
#endif
		struct End { Object& object; ~End() { object.dispatching = false; } } end {*o};
		if (r->hooks.retain_dispatch_thread) { Check(r->hooks.retain_dispatch_thread(r)); }
#if KYTY_IPMI_HAS_RETAINED
		if (r->hooks.capture_dispatch_thread) {
			DispatcherReceipt receipt;
			Check(r->hooks.capture_dispatch_thread(r, receipt));
			// Hooks cannot authorize a saved identity, nor change the actual caller.
			if (!(r->Id() == receiver) || !(receipt.identity == receiver) || !receipt.generation || !receipt.token) { return Guest(Invalid); }
			std::lock_guard<std::mutex> shutdown(o->shutdown_mutex);
			std::lock_guard<std::mutex> lock(r->mutex);
			const auto entry = r->objects.find(o->native);
			if (o->destroyed || o->retirement || !o->published || entry == r->objects.end() || entry->second != o) { return Guest(NotFound); }
			auto& bound = o->dispatcher_receipt;
			if (bound.token) {
				if (bound.generation != receipt.generation || !(bound.identity == receipt.identity) ||
				    bound.token.get() != receipt.token.get() || bound.token.owner_before(receipt.token) || receipt.token.owner_before(bound.token)) { return Guest(Invalid); }
				if (o->dispatcher_proof.native_exited || o->dispatcher_proof.successfully_joined) { return Guest(Invalid); }
			} else { bound = receipt; }
		}
#else
		const auto receiver = r->Id();
#endif
		while (!o->stopping) {
			Dispatch dispatch;
			auto result = r->core.Receive(receiver, o->handle, dispatch);
			if (result.syscall_errno == WouldBlock) {
				if (single) { return 0; }
				const int error = r->hooks.cooperate();
				if (error != Interrupted) { Check(error); }
				continue;
			}
			Check(result); DispatchOne(*r, *o, dispatch);
		}
		return 0;
	});
}
int KYTY_SYSV_ABI RunDispatcher(void* self, void* work, uint64_t size) { return Run(self, work, size, false); }
int KYTY_SYSV_ABI TryDispatch(void* self, void* work, uint64_t size) { return Run(self, work, size, true); }

int ConnectInternal(void* self, const void* argument, uint64_t size, int32_t* accepted, bool wait_server) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client); const auto id = r->Id();
		if (!accepted || size > 0x2000) { return Guest(Invalid); }
		Ticket readiness;
		struct ReleaseWait {
			Runtime& runtime; Identity id; Handle client; Ticket& ticket; bool consumed = false;
			~ReleaseWait() { if (ticket && !consumed) { runtime.core.AbandonServerWait(id, client, ticket); } }
		} release {*r, id, o->handle, readiness};
		Handle expected = 0;
		if (wait_server) {
			Check(r->core.WaitServer(id, o->handle, readiness));
			const auto ready = r->Wait(readiness); Check(ready); expected = ready.value;
			if (!expected) { return Guest(BadMessage); }
		}
		ConnectionInfo info; Check(r->core.Connection(id, o->handle, info, expected));
		// Core registration precedes factory copyout. An unpublished/provisional
		// native server must never become connectable during a reentrant copy hook.
		auto server = r->Find(reinterpret_cast<void*>(info.server_config.native_object), ObjectKind::Server);
		Check(r->hooks.authorize_connect(id, info));
		if (!info.server_config.multi && info.client_config.sync_slots >= 2) { return Guest(Invalid); }
		if (size + 0x154 > info.server_config.transfer_limit) { return Guest(NoMemory); }
		Bytes bytes(static_cast<size_t>(size)); r->Read(Address(argument), bytes.data(), bytes.size());
		Ticket ticket; Check(r->core.Connect(id, o->handle, ticket, std::move(bytes), info.server, readiness));
		release.consumed = true;
		auto result = r->Wait(ticket); Check(result);
		if (!result.method_result) { return Guest(BadMessage); }
		r->Write(accepted, *result.method_result); return 0;
	});
}
int KYTY_SYSV_ABI Connect(void* self, const void* argument, uint64_t size, int32_t* accepted) {
	return ConnectInternal(self, argument, size, accepted, false);
}
// Native client+80. Null waits for publication AND genuine acceptance. The
// finite-deadline path also needs native post-dispatch cancellation semantics;
// do not silently discard that deadline while those semantics are unavailable.
int KYTY_SYSV_ABI ConnectWithWaitServer(void* self, const void* argument, uint64_t size,
                                     int32_t* accepted, uint32_t* timeout) {
	if (!timeout) { return ConnectInternal(self, argument, size, accepted, true); }
	return Safe([&] {
		auto r = Current();
		if (r->Read<uint32_t>(timeout) != 0) { return Guest(Unsupported); }
		return ConnectInternal(self, argument, size, accepted, false);
	});
}

int EventError(int error) {
	return error == Guest(NotFound) ? Guest(NotConnected) : error;
}
// 13.60 Client vtable+20 -> bd0/5390/selector491: no timeout argument.
int KYTY_SYSV_ABI PollEvent(void* self, uint32_t index, uint64_t bits, uint32_t mode, uint64_t* output) {
	return EventError(Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client);
		auto result = r->core.PollEvent(r->Id(), o->handle, index, bits, mode);
		Check(result);
		if (output) { r->Write(output, result.value); }
		return 0;
	}));
}
// Client+28 -> bf0/52c0/490. Timeout is a borrowed uint32 microsecond
// in/out pointer, not a scalar or a seventh integer argument.
int KYTY_SYSV_ABI WaitEvent(void* self, uint32_t index, uint64_t bits, uint32_t mode,
                          uint64_t* output, uint32_t* timeout) {
	return EventError(Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client); const auto id = r->Id();
		std::optional<std::chrono::microseconds> duration;
		if (timeout) { duration = std::chrono::microseconds(r->Read<uint32_t>(timeout)); }
		const auto start = Clock::now();
		Ticket ticket; Check(r->core.WaitEvent(id, o->handle, index, bits, mode, duration, ticket, start));
		struct Release {
			Runtime& r; Identity id; Handle client; Ticket ticket;
			~Release() { r.core.AbandonEventWait(id, client, ticket); }
		} release {*r, id, o->handle, ticket};
		auto result = r->Wait(ticket); Check(result);
		if (output) { r->Write(output, result.value); }
		if (timeout) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
			const auto remaining = static_cast<uint32_t>(std::max<int64_t>(0, duration->count() - elapsed.count()));
			r->Write(timeout, remaining);
		}
		return 0;
	}));
}
// Session+40 ->39a0/5490/493; available during the accepting callback.
int KYTY_SYSV_ABI SetEvent(void* self, uint32_t index, uint64_t bits) {
	return EventError(Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Session);
		Check(r->core.SetEvent(r->Id(), o->handle, index, bits)); return 0;
	}));
}
int KYTY_SYSV_ABI Disconnect(void* self) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client);
		Ticket ticket; Check(r->core.Disconnect(r->Id(), o->handle, ticket)); Check(r->Wait(ticket)); return 0;
	});
}
int KYTY_SYSV_ABI Terminate(void* self) {
	return Safe([&] { auto r = Current(); auto o = r->Find(self, ObjectKind::Client); Check(r->core.ForceTeardown(r->Id(), o->handle)); return 0; });
}
int KYTY_SYSV_ABI AsyncStart(void* self, uint32_t method, const DataInfo* input, uint32_t count,
                           uint32_t* ticket_out, const void* option) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client); const auto id = r->Id();
		if (!ticket_out) { return Guest(Invalid); }
		if (option) {
			struct Option { int32_t index; uint32_t pad; uint64_t value; };
			const auto value = r->Read<Option>(option);
			if (value.index != -1 || value.value != 0) { return Guest(Unsupported); }
		}
		auto bytes = Inputs(*r, input, count); uint32_t ticket = 0;
		ConnectionInfo connection; Check(r->core.Connection(id, o->handle, connection));
		uint64_t payload = 0x14 + uint64_t(count) * 4;
		for (const auto& b: bytes) { payload += b.size(); }
		if (payload > connection.server_config.transfer_limit) { return Guest(NoMemory); }
		Check(r->core.InvokeAsync(id, o->handle, method, std::move(bytes), ticket));
		try { r->Write(ticket_out, ticket); }
		catch (...) { r->core.ReleaseAsyncStartOnCopyFault(id, o->handle, ticket); throw; }
		return 0;
	});
}
int KYTY_SYSV_ABI AsyncCollect(void* self, uint32_t method, uint32_t ticket, int32_t* service_result,
                             BufferInfo* output, uint32_t count) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client); const auto id = r->Id();
		if (!service_result || count > 10) { return Guest(Invalid); }
		std::array<BufferInfo, 10> descriptors {};
		r->Read(Address(output), descriptors.data(), count * sizeof(BufferInfo));
		std::vector<uint64_t> capacities; capacities.reserve(count); uint64_t total = 0;
		for (uint32_t n = 0; n < count; ++n) {
			if (descriptors[n].capacity > Budget - total) { return Guest(Invalid); }
			total += descriptors[n].capacity; capacities.push_back(descriptors[n].capacity);
		}
		Result result; Check(r->core.BeginAsyncRead(id, o->handle, method, ticket, capacities, result));
		struct Release {
			Runtime& r; Identity id; Handle client; uint32_t ticket; bool reserved = true;
			~Release() { if (reserved) { r.core.EndAsyncRead(id, client, ticket, false); } }
		} release {*r, id, o->handle, ticket};
		for (uint32_t n = 0; n < count; ++n) {
			r->Write(Address(descriptors[n].data), result.outputs[n].data(), result.outputs[n].size());
			descriptors[n].written = result.outputs[n].size();
		}
		Check(r->core.EndAsyncRead(id, o->handle, ticket, true)); release.reserved = false;
		// Packet consumption precedes these copyouts; a fault retains an occupied
		// ticket with an empty channel. Never replay a cached successful result.
		r->Write(service_result, *result.method_result);
		r->Write(Address(output), descriptors.data(), count * sizeof(BufferInfo));
		Check(r->core.ReleaseAsyncTicket(id, o->handle, ticket)); return 0;
	});
}
int KYTY_SYSV_ABI Invoke(void* self, uint32_t method, const DataInfo* input, uint32_t count,
                       int32_t* service_result, BufferInfo* output, uint32_t output_count) {
	return Safe([&] {
		auto r = Current(); auto o = r->Find(self, ObjectKind::Client); const auto id = r->Id();
		if (!service_result || output_count > 10) { return Guest(Invalid); }
		auto bytes = Inputs(*r, input, count);
		std::array<BufferInfo, 10> descriptors {};
		r->Read(Address(output), descriptors.data(), output_count * sizeof(BufferInfo));
		std::vector<uint64_t> capacities; capacities.reserve(output_count);
		uint64_t total = 0;
		for (uint32_t n = 0; n < output_count; ++n) {
			if (descriptors[n].capacity > Budget - total) { return Guest(Invalid); }
			total += descriptors[n].capacity; capacities.push_back(descriptors[n].capacity);
		}
		ConnectionInfo connection; Check(r->core.Connection(id, o->handle, connection));
		uint64_t payload = 0x18 + uint64_t(count + output_count) * 4 + total;
		for (const auto& b: bytes) { payload += b.size(); }
		if (payload > connection.server_config.transfer_limit) { return Guest(NoMemory); }
		Ticket ticket; Check(r->core.Invoke(id, o->handle, method, std::move(bytes), std::move(capacities), ticket));
		struct Release {
			Runtime& r; Identity id; Handle client; Ticket ticket; bool reserved = false;
			~Release() { if (reserved) { r.core.EndInvokeRead(id, client, ticket, false); } r.core.ReleaseInvokeCaller(id, client, ticket); }
		} release {*r, id, o->handle, ticket};
		Check(r->Wait(ticket));
		Result result; Check(r->core.BeginInvokeRead(id, o->handle, ticket, result)); release.reserved = true;
		if (!result.method_result || result.outputs.size() != output_count) { return Guest(BadMessage); }
		for (uint32_t n = 0; n < output_count; ++n) {
			if (result.outputs[n].size() > descriptors[n].capacity) { return Guest(BadMessage); }
		}
		for (uint32_t n = 0; n < output_count; ++n) {
			r->Write(Address(descriptors[n].data), result.outputs[n].data(), result.outputs[n].size());
			descriptors[n].written = result.outputs[n].size();
		}
		Check(r->core.EndInvokeRead(id, o->handle, ticket, true)); release.reserved = false;
		r->Write(service_result, *result.method_result);
		r->Write(Address(output), descriptors.data(), output_count * sizeof(BufferInfo));
		return 0;
	});
}

void* ServerTable() {
	static void* table[] = {reinterpret_cast<void*>(&DestroyServer), reinterpret_cast<void*>(&ServerUserData),
	    reinterpret_cast<void*>(&RunDispatcher), reinterpret_cast<void*>(&TryDispatch), reinterpret_cast<void*>(&Stop),
	    reinterpret_cast<void*>(&CreateSession), reinterpret_cast<void*>(&DestroyServer), reinterpret_cast<void*>(&DestroyServer)};
	return table;
}
void* SessionTable() {
	static std::array<void*, 18> table = [] {
		std::array<void*, 18> value {}; value.fill(reinterpret_cast<void*>(&UnsupportedCall));
		value[0] = value[1] = value[2] = reinterpret_cast<void*>(&CloseSession);
		value[3] = reinterpret_cast<void*>(&PeerPid); value[4] = reinterpret_cast<void*>(&SessionUserData);
		value[5] = reinterpret_cast<void*>(&UnavailablePointer);
		value[6] = reinterpret_cast<void*>(&Reply); value[7] = reinterpret_cast<void*>(&AsyncReply);
		if constexpr (EnableEvents) { value[8] = reinterpret_cast<void*>(&SetEvent); }
		return value;
	}();
	return table.data();
}
void* ClientTable() {
	static std::array<void*, 20> table = [] {
		std::array<void*, 20> value {}; value.fill(reinterpret_cast<void*>(&UnsupportedCall));
		value[0] = reinterpret_cast<void*>(&DestroyClient); value[1] = reinterpret_cast<void*>(&ClientUserData);
		value[2] = reinterpret_cast<void*>(&Connect); value[3] = reinterpret_cast<void*>(&Disconnect);
		if constexpr (EnableEvents) {
			value[4] = reinterpret_cast<void*>(&PollEvent); value[5] = reinterpret_cast<void*>(&WaitEvent);
		}
		value[11] = reinterpret_cast<void*>(&Invoke); value[17] = reinterpret_cast<void*>(&Terminate);
		value[16] = reinterpret_cast<void*>(&ConnectWithWaitServer);
		value[12] = reinterpret_cast<void*>(&AsyncStart); value[13] = reinterpret_cast<void*>(&AsyncCollect);
		return value;
	}();
	return table.data();
}
uint64_t Get(const std::array<uint8_t, 0x158>& bytes, size_t at, size_t width = 8) {
	uint64_t value = 0;
	for (size_t n = 0; n < width; ++n) { value |= uint64_t(bytes[at + n]) << (8 * n); }
	return value;
}
#if KYTY_IPMI_HAS_RETAINED
enum class RetainedPhase { Idle, Connecting, Connected, Invoking, Disconnecting, Terminating, Retired };
struct RetainedState {
	std::shared_ptr<Runtime> runtime;
	Identity owner {};
	Handle handle = 0;
	uint64_t token = 0;
	std::array<uint64_t, 2> backing {};
	RetainedPhase phase = RetainedPhase::Idle;
	std::atomic_bool entered {false};
	std::atomic_bool retiring {false};
	Ticket ticket;
	std::vector<uint64_t> capacities;
};
// Separate from Core's installation-local handles; stale host tokens cannot
// acquire a later installation's client. Protected by global_mutex.
uint64_t next_retained_token = 1;
template<class F> int HostSafe(F&& f) noexcept {
	try { return f(); } catch (const Fault& f) { return f.error; }
	catch (const std::bad_alloc&) { return NoMemory; }
	catch (...) { return BadMessage; }
}
template<class F> int WithRetained(Handle handle, F&& f) noexcept {
	return HostSafe([&] {
		auto r = Current(); std::shared_ptr<RetainedState> state;
		{
			std::lock_guard<std::mutex> lock(r->mutex);
			auto i = r->retained.find(handle);
			if (i == r->retained.end()) { return int(NotFound); }
			state = i->second;
		}
		if (state->entered.exchange(true)) { return int(Busy); }
		struct Leave { RetainedState& s; ~Leave() { s.entered = false; } } leave {*state};
		const auto id = r->Id();
		if (!(id == state->owner)) { return int(Invalid); }
		return f(*state, id);
	});
}
int RetainedConnectResult(RetainedState& s, int32_t& acceptance) {
	auto result = s.ticket->PollStatus();
	if (!result) { return WouldBlock; }
	if (result->syscall_errno) { return result->syscall_errno; }
	if (!result->method_result) { return BadMessage; }
	acceptance = *result->method_result;
	s.phase = acceptance == 0 ? RetainedPhase::Connected : RetainedPhase::Idle;
	s.ticket.reset(); return 0;
}
int RetainedDisconnectResult(RetainedState& s, int32_t& result) {
	auto status = s.ticket->PollStatus();
	if (!status) { return WouldBlock; }
	if (status->syscall_errno) { return status->syscall_errno; }
	if (!status->method_result) { return BadMessage; }
	result = *status->method_result;
	s.phase = RetainedPhase::Idle; s.ticket.reset(); return 0;
}
// Consume into owned host storage; never copy to a caller's saved pointer.
int RetainedConsume(RetainedState& s, Identity id, Result* output) {
	auto status = s.ticket->PollStatus();
	if (!status) { return WouldBlock; }
	if (status->syscall_errno) { return status->syscall_errno; }
	Result reply;
	Check(s.runtime->core.BeginInvokeRead(id, s.handle, s.ticket, reply));
	struct Unreserve {
		RetainedState& s; Identity id; bool active = true;
		~Unreserve() { if (active) { s.runtime->core.EndInvokeRead(id, s.handle, s.ticket, false); } }
	} unreserve {s, id};
	bool valid = reply.method_result.has_value() && reply.outputs.size() == s.capacities.size();
	if (valid) {
		for (size_t i = 0; i < s.capacities.size(); ++i) {
			if (reply.outputs[i].size() > s.capacities[i]) { valid = false; break; }
		}
	}
	// A malformed reply is still drainable host-owned data. Do not leave an
	// unread packet behind or make its bytes visible to a public caller.
	Check(s.runtime->core.EndInvokeRead(id, s.handle, s.ticket, true)); unreserve.active = false;
	Check(s.runtime->core.ReleaseInvokeCaller(id, s.handle, s.ticket));
	s.ticket.reset(); s.capacities.clear(); s.phase = RetainedPhase::Connected;
	if (!valid && output) { s.retiring = true; return BadMessage; }
	if (output) { *output = std::move(reply); }
	return 0;
}
int AdvanceRetirement(RetainedState& s, Identity id) {
	s.retiring = true;
	if (s.phase == RetainedPhase::Connecting) {
		int32_t accepted = 0;
		const int error = RetainedConnectResult(s, accepted);
		if (error) { return error; }
	}
	if (s.phase == RetainedPhase::Invoking) {
		const int error = RetainedConsume(s, id, nullptr);
		if (error == WouldBlock) { return error; }
		if (error) {
			// Only a completed transport failure uses connected-client force.
			// Pending connects/requests never lose their caller observation.
			Check(s.runtime->core.ForceTeardown(id, s.handle));
			s.phase = RetainedPhase::Terminating;
		}
	}
	if (s.phase == RetainedPhase::Connected) {
		Ticket ticket;
		Check(s.runtime->core.Disconnect(id, s.handle, ticket));
		s.ticket = std::move(ticket); s.phase = RetainedPhase::Disconnecting;
	}
	if (s.phase == RetainedPhase::Disconnecting) {
		int32_t result = 0;
		const int error = RetainedDisconnectResult(s, result);
		if (error) { return error; }
	}
	if (s.phase == RetainedPhase::Terminating && s.ticket) {
		Check(s.runtime->core.ReleaseInvokeCaller(id, s.handle, s.ticket));
		s.ticket.reset(); s.capacities.clear();
	}
	Check(s.runtime->core.DestroyClient(id, s.handle));
	s.phase = RetainedPhase::Retired;
	std::lock_guard<std::mutex> lock(s.runtime->mutex);
	s.runtime->retained.erase(s.token);
	return 0;
}
#endif
} // namespace

int Install(Hooks hooks) noexcept {
	return Safe([&] {
#if KYTY_IPMI_HAS_RETAINED
		if (bool(hooks.capture_dispatch_thread) != bool(hooks.inspect_dispatch_thread)) { return Guest(Invalid); }
#endif
		if (!hooks.owner || !hooks.identity || !hooks.copy_in || !hooks.copy_out || !hooks.authorize_registration ||
		    !hooks.authorize_connect || !hooks.invoke_guest || !hooks.cooperate) { return Guest(Invalid); }
		auto runtime = std::make_shared<Runtime>(std::move(hooks));
		std::lock_guard<std::mutex> lock(global_mutex);
		if (installed) { return Guest(Busy); }
		installed = std::move(runtime); return 0;
	});
}
int Uninstall() noexcept {
	std::shared_ptr<Runtime> retired;
	{
		std::lock_guard<std::mutex> lock(global_mutex);
		if (!installed) { return 0; }
		if (installed.use_count() != 1) { return Guest(Busy); }
		{ std::lock_guard<std::mutex> objects(installed->mutex); if (!installed->objects.empty()) { return Guest(Busy); } }
		retired = std::move(installed);
	}
	return 0;
}
int RetryRetirements() noexcept {
	return Safe([&] {
		auto r = Current(); const auto id = r->Id();
		std::lock_guard<std::mutex> lock(r->mutex);
		int error = 0;
		for (auto i = r->objects.begin(); i != r->objects.end();) {
			const auto& o = *i->second;
			if (!o.retirement) { ++i; continue; }
			// Session rollback is receiver-thread bound, not an arbitrary host
			// cleanup identity. Retain rather than impersonate its old caller.
			if (o.owner.pid != id.pid || (o.kind == ObjectKind::Session && !(o.owner == id))) {
				error = Busy; ++i; continue;
			}
			const auto result = r->Release(id, o);
			if (result.syscall_errno) { error = result.syscall_errno; ++i; }
			else { i = r->objects.erase(i); }
		}
		return Guest(error);
	});
}
bool Installed() noexcept { std::lock_guard<std::mutex> lock(global_mutex); return bool(installed); }

#if KYTY_IPMI_HAS_RETAINED
namespace Lifecycle {
class ServerState final {
public:
	std::shared_ptr<Runtime> runtime;
	std::shared_ptr<Object> object;
	Identity owner;
	uint64_t native_address;
	Handle handle;
	ServerState(std::shared_ptr<Runtime> r, std::shared_ptr<Object> o, Identity id):
	    runtime(std::move(r)), object(std::move(o)), owner(id), native_address(object->native), handle(object->handle) {}
	// No retirement in the destructor. Object/storage/Runtime ownership is only
	// released; allocator tokens schedule their own explicit reclamation.
};
namespace {
std::shared_ptr<Runtime> TryCurrent() {
	std::unique_lock lock(global_mutex, std::try_to_lock);
	if (!lock.owns_lock()) { throw Fault {Busy}; }
	if (!installed) { throw Fault {Unsupported}; }
	return installed;
}
template<class F> int WithServer(const ServerLease& lease, F&& operation) {
	const auto pin = lease;
	if (!pin) { return Invalid; }
	auto r = TryCurrent();
	if (r != pin->runtime) { return NotFound; }
	const auto id = r->Id(); // Actual caller; no captured-identity execution.
	if (!(id == pin->owner) || !(id == pin->object->owner)) { return Invalid; }
	auto& o = *pin->object;
	std::unique_lock shutdown(o.shutdown_mutex, std::try_to_lock);
	if (!shutdown.owns_lock()) { return Busy; }
	ServerSnapshot snapshot;
	{
		std::unique_lock lock(r->mutex, std::try_to_lock);
		if (!lock.owns_lock()) { return Busy; }
		if (o.kind != ObjectKind::Server || o.native != pin->native_address || o.handle != pin->handle) { return NotFound; }
		const auto entry = r->objects.find(pin->native_address);
		const bool exact = entry != r->objects.end() && entry->second == pin->object;
		// Registry absence/replacement alone is NEVER destruction evidence.
		// Generic Erase handles provisional rollback; PruneSessions only erases
		// sessions; RetryRetirements only handles failed provisional rollback.
		// Published server proof is set by DestroyServer only after Core success
		// and erasure of this exact shared Object, under these same locks.
		if (!o.destroyed && (!exact || !o.published || o.retirement)) { return NotFound; }
		if (o.destroyed && exact) { return BadMessage; }
		snapshot.native_address = pin->native_address;
		snapshot.handle = pin->handle;
		snapshot.published = exact && !o.destroyed;
		snapshot.destroyed = o.destroyed;
		snapshot.stopping = o.stopping;
		snapshot.dispatching = o.dispatching;
		snapshot.stop_succeeded = snapshot.stopping;
	}
	return operation(*r, o, id, snapshot);
}
} // namespace

int AcquireServer(uint64_t native_address, Handle expected_handle, ServerLease& output) noexcept {
	return HostSafe([&] {
		if (!native_address || !expected_handle) { return int(Invalid); }
		auto r = TryCurrent();
		const auto id = r->Id();
		std::shared_ptr<Object> object;
		{
			std::unique_lock lock(r->mutex, std::try_to_lock);
			if (!lock.owns_lock()) { return int(Busy); }
			const auto entry = r->objects.find(native_address);
			if (entry == r->objects.end()) { return int(NotFound); }
			const auto& o = *entry->second;
			if (o.kind != ObjectKind::Server || !o.published || o.retirement || o.destroyed ||
			    o.native != native_address || o.handle != expected_handle) { return int(NotFound); }
			if (!(o.owner == id)) { return int(Invalid); }
			object = entry->second;
		}
		// Allocate and release any previous output outside adapter locks.
		ServerLease token = std::make_shared<ServerState>(r, std::move(object), id);
		const int result = WithServer(token, [](Runtime&, Object&, Identity, const ServerSnapshot& state) {
			return state.published && !state.destroyed ? 0 : int(NotFound);
		});
		if (result == 0) { output = std::move(token); }
		return result;
	});
}
int InspectServer(const ServerLease& lease, ServerSnapshot& output) noexcept {
	return HostSafe([&] {
		return WithServer(lease, [&](Runtime&, Object&, Identity, const ServerSnapshot& state) {
			output = state; return 0;
		});
	});
}
int InspectDispatcher(const ServerLease& lease, DispatcherSnapshot& output) noexcept {
	return HostSafe([&] {
		const auto pin = lease;
		DispatcherSnapshot observed;
		const int admitted = WithServer(pin, [&](Runtime&, Object& o, Identity, const ServerSnapshot&) {
			if (o.inspecting_dispatcher.exchange(true)) { return int(Busy); }
			observed.receipt = o.dispatcher_receipt;
			if (observed.receipt.token) { observed.binding = DispatcherBinding::Bound; }
			return 0;
		});
		if (admitted) { return admitted; }
		struct End { Object& object; ~End() { object.inspecting_dispatcher = false; } } end {*pin->object};
		if (observed.binding == DispatcherBinding::Bound) {
			auto& r = *pin->runtime;
			if (!r.hooks.inspect_dispatch_thread) { return int(Unsupported); }
			Check(r.hooks.inspect_dispatch_thread(observed.receipt, observed.proof));
			const auto& p = observed.proof;
			const bool joiner_valid = (p.join_source.pid > 0 && p.join_source.tid > 0) ||
			    (p.join_source.pid == -1 && p.join_source.tid == -1);
			const bool origin_valid = p.join_origin == DispatcherJoinOrigin::Explicit ||
			    p.join_origin == DispatcherJoinOrigin::DetachedCollector || p.join_origin == DispatcherJoinOrigin::StartupRollback;
			if ((p.successfully_joined && (!p.native_exited || !joiner_valid || !origin_valid)) ||
			    (!p.successfully_joined && (p.join_source.pid != 0 || p.join_source.tid != 0 ||
			                               p.join_origin != DispatcherJoinOrigin::None))) { return int(BadMessage); }
			const int committed = WithServer(pin, [&](Runtime&, Object& o, Identity, const ServerSnapshot& state) {
				const auto& previous = o.dispatcher_proof;
				if ((previous.native_exited && !p.native_exited) || (previous.successfully_joined && !p.successfully_joined) ||
				    (previous.successfully_joined && (!(previous.join_source == p.join_source) || previous.join_origin != p.join_origin)) ||
				    (state.dispatching && p.native_exited)) { return int(BadMessage); }
				o.dispatcher_proof = p; return 0;
			});
			if (committed) { return committed; }
		}
		// May release the caller's old token. Never do that under adapter locks.
		output = std::move(observed); return 0;
	});
}
int RequestStop(const ServerLease& lease, StopResult& output) noexcept {
	return HostSafe([&] {
		return WithServer(lease, [&](Runtime& r, Object& o, Identity id, ServerSnapshot state) {
			if (!state.published || state.destroyed) { return int(NotFound); }
			const int error = HostSafe([&] { return StopServer(r, o, id); });
			state.stopping = o.stopping;
			state.stop_succeeded = state.stopping;
			state.dispatching = o.dispatching;
			output = {error, state};
			return 0;
		});
	});
}
} // namespace Lifecycle
#endif

int KYTY_SYSV_ABI CreateServer(void** out, const ServerConfig* config, void* argument, void* storage) noexcept {
	return Safe([&] {
		if (!out || !config || !storage) { return Guest(Invalid); }
		auto r = Current(); auto cfg = r->Read<ServerConfig>(config);
		if (std::memcmp(cfg.name, Name, sizeof(Name)) || !cfg.handler || cfg.multi > 1 || cfg.cross_no_cap > 1 || cfg.restricted > 1 ||
		    cfg.input_size > Budget || cfg.output_size > Budget) { return Guest(Invalid); }
		if (cfg.restricted && !cfg.cross_no_cap) { return Guest(Invalid); }
		// The native multi-dispatch factory has a separate service-name whitelist.
		// Each exact service route admits its proven ShellCore configuration only.
		if (cfg.multi || cfg.restricted) { return Guest(Unsupported); }
		Check(r->hooks.authorize_registration(r->Id(), true));
		Publication p(r, ObjectKind::Server, storage);
		p.object->handler = Address(cfg.handler); p.object->argument = Address(argument);
		p.object->multi = cfg.multi != 0;
		p.object->work_size = std::max(cfg.input_size, cfg.output_size) + 0x100;
		LibKernel::Ipmi::ServerConfig native;
		native.native_object = Address(storage); native.handler = Address(cfg.handler); native.user_argument = Address(argument);
		native.argument_limit = cfg.input_size; native.transfer_limit = cfg.output_size;
		native.multi = cfg.multi; native.cross_no_cap = cfg.cross_no_cap; native.restricted = cfg.restricted;
		Check(r->core.RegisterServer(p.id, Name, native, p.object->handle, false)); p.Commit(out, ServerTable(), 0x30); return 0;
	});
}
int KYTY_SYSV_ABI CreateClient(void** out, const void* config, void* argument, void* storage) noexcept {
	return Safe([&] {
		if (!out || !config || !storage) { return Guest(Invalid); }
		auto r = Current(); std::array<uint8_t, 0x158> cfg {};
		r->Read(Address(config), cfg.data(), cfg.size());
		if (std::memcmp(cfg.data(), Name, sizeof(Name))) { return Guest(Invalid); }
		Check(r->hooks.authorize_registration(r->Id(), false));
		Config native;
		native.sync_slots = static_cast<uint32_t>(Get(cfg, 0x20, 4)); native.input_limit = Get(cfg, 0x28); native.output_limit = Get(cfg, 0x30);
		native.async_slots = static_cast<uint32_t>(Get(cfg, 0x38, 4)); native.async_input_limit = Get(cfg, 0x40); native.async_output_limit = Get(cfg, 0x48);
		native.event_count = static_cast<uint32_t>(Get(cfg, 0x1c, 4)); native.sync_flag = cfg[0x19];
		native.queue_count = static_cast<uint32_t>(Get(cfg, 0x50, 4));
		if (native.queue_count || native.input_limit > Budget || native.output_limit > Budget ||
		    native.async_input_limit > Budget || native.async_output_limit > Budget || native.sync_flag > 1) {
			return Guest(Unsupported);
		}
		native.native_object = Address(storage); native.user_argument = Address(argument);
		Publication p(r, ObjectKind::Client, storage);
		Check(r->core.RegisterClient(p.id, Name, native, p.object->handle)); p.Commit(out, ClientTable(), 0x10); return 0;
	});
}
#if KYTY_IPMI_HAS_RETAINED
namespace Retained {
int Register(Connection& output) noexcept {
	return HostSafe([&] {
		auto r = Current(); const auto id = r->Id();
		Check(r->hooks.authorize_registration(id, false));
		auto s = std::make_shared<RetainedState>(); s->runtime = r; s->owner = id;
		{
			std::lock_guard<std::mutex> lock(global_mutex);
			if (next_retained_token == UINT64_MAX) { return int(NoMemory); }
			s->token = next_retained_token++;
		}
		// Prepare the registry node before Core registration: publication below
		// cannot allocate/fail and strand an unowned Core client.
		std::map<Handle, std::shared_ptr<RetainedState>> prepared;
		prepared.emplace(0, s); auto node = prepared.extract(0);
		Config config;
		config.sync_slots = 1; config.input_limit = 0x2000; config.output_limit = 0x4000;
		config.event_count = 0; config.native_object = Address(s->backing.data());
		{
			std::lock_guard<std::mutex> lock(r->mutex);
			if (!r->retained.empty()) { return int(Busy); }
			Check(r->core.RegisterClient(id, Name, config, s->handle));
			s->backing[1] = s->handle;
			node.key() = s->token; r->retained.insert(std::move(node));
		}
		output = s->token; return 0;
	});
}
int BeginConnect(Connection connection, const Bytes& argument) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity id) {
		if (s.retiring || s.phase != RetainedPhase::Idle) { return int(Busy); }
		if (argument.size() > 0x2000) { return int(Invalid); }
		ConnectionInfo info; Check(s.runtime->core.Connection(id, s.handle, info));
		auto server = s.runtime->Find(reinterpret_cast<void*>(info.server_config.native_object), ObjectKind::Server);
		Check(s.runtime->hooks.authorize_connect(id, info));
		if (!info.server_config.multi && info.client_config.sync_slots >= 2) { return int(Invalid); }
		if (argument.size() + 0x154 > info.server_config.transfer_limit) { return int(NoMemory); }
		Ticket ticket;
		Check(s.runtime->core.Connect(id, s.handle, ticket, argument, info.server));
		s.ticket = std::move(ticket); s.phase = RetainedPhase::Connecting; return 0;
	});
}
int PollConnect(Connection connection, int32_t& acceptance) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity) {
		if (s.retiring) { return int(Busy); }
		if (s.phase != RetainedPhase::Connecting) { return int(Invalid); }
		return RetainedConnectResult(s, acceptance);
	});
}
int BeginInvoke(Connection connection, uint32_t method, const std::vector<Bytes>& inputs,
                const std::vector<uint64_t>& capacities) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity id) {
		if (s.retiring || s.phase != RetainedPhase::Connected) { return int(Busy); }
		if (inputs.size() > 10 || capacities.size() > 10) { return int(Invalid); }
		uint64_t total = 0;
		for (auto capacity: capacities) {
			if (capacity > Budget - total) { return int(Invalid); } total += capacity;
		}
		uint64_t input_total = 0;
		for (const auto& bytes: inputs) {
			if (bytes.size() > Budget - input_total) { return int(Invalid); } input_total += bytes.size();
		}
		ConnectionInfo info; Check(s.runtime->core.Connection(id, s.handle, info));
		if (0x18 + uint64_t(inputs.size() + capacities.size()) * 4 + total + input_total > info.server_config.transfer_limit) {
			return int(NoMemory);
		}
		// All adapter allocations precede publication to Core.
		auto saved = capacities; Ticket ticket;
		Check(s.runtime->core.Invoke(id, s.handle, method, inputs, capacities, ticket));
		s.capacities = std::move(saved); s.ticket = std::move(ticket); s.phase = RetainedPhase::Invoking; return 0;
	});
}
int PollInvoke(Connection connection) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity) {
		if (s.retiring) { return int(Busy); }
		if (s.phase != RetainedPhase::Invoking) { return int(Invalid); }
		auto status = s.ticket->PollStatus(); return status ? status->syscall_errno : int(WouldBlock);
	});
}
int ConsumeInvoke(Connection connection, Result& output) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity id) {
		if (s.retiring) { return int(Busy); }
		if (s.phase != RetainedPhase::Invoking) { return int(Invalid); }
		return RetainedConsume(s, id, &output);
	});
}
int BeginDisconnect(Connection connection) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity id) {
		if (s.retiring || s.phase != RetainedPhase::Connected) { return int(Busy); }
		Ticket ticket; Check(s.runtime->core.Disconnect(id, s.handle, ticket));
		s.ticket = std::move(ticket); s.phase = RetainedPhase::Disconnecting; return 0;
	});
}
int PollDisconnect(Connection connection, int32_t& result) noexcept {
	return WithRetained(connection, [&](RetainedState& s, Identity) {
		if (s.retiring) { return int(Busy); }
		if (s.phase != RetainedPhase::Disconnecting) { return int(Invalid); }
		return RetainedDisconnectResult(s, result);
	});
}
int Retire(Connection connection) noexcept {
	return WithRetained(connection, [](RetainedState& s, Identity id) { return AdvanceRetirement(s, id); });
}
int RetryRetirements() noexcept {
	return HostSafe([&] {
		auto r = Current(); r->Id();
		std::vector<Handle> pending;
		{
			std::lock_guard<std::mutex> lock(r->mutex);
			for (const auto& entry: r->retained) {
				if (entry.second->retiring.load()) { pending.push_back(entry.first); }
			}
		}
		int error = 0;
		for (auto handle: pending) {
			const int result = WithRetained(handle, [&](RetainedState& s, Identity actual) {
				return s.retiring ? AdvanceRetirement(s, actual) : 0;
			});
			if (result && result != NotFound) { error = result == Invalid ? int(Busy) : result; }
		}
		return error;
	});
}
} // namespace Retained
#endif
} // namespace Libs::KYTY_IPMI_ADAPTER_NAMESPACE

#undef KYTY_IPMI_HAS_RETAINED
#undef KYTY_IPMI_RETAINED_SELECT
#undef KYTY_IPMI_RETAINED_SELECT_
#undef KYTY_IPMI_RETAINED_NpNativeTransport
#undef KYTY_IPMI_RETAINED_AppDbNativeTransport
#undef KYTY_IPMI_RETAINED_LoginMgrNativeTransport
