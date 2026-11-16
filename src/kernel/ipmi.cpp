#include "ipmi.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <mutex>

namespace Libs::LibKernel::Ipmi {
Result Result::Success() { Result r; r.operation_result = 0; return r; }
Result Result::Failure(int error) { Result r; r.syscall_errno = error; return r; }

struct Completion::Impl {
	mutable std::mutex mutex;
	mutable std::condition_variable changed;
	std::optional<Result> result;
	Completion::ReplyState reply;
	Bytes reply_packet;
	std::shared_ptr<void> pin;
};
Completion::Completion(): m(std::make_unique<Impl>()) {}
Completion::~Completion() = default;
std::optional<Result> Completion::Poll() const {
	std::lock_guard<std::mutex> lock(m->mutex);
	return m->result;
}
std::optional<Result> Completion::PollStatus() const {
	std::lock_guard<std::mutex> lock(m->mutex);
	if (!m->result) { return {}; }
	const auto& stored = *m->result;
	Result status;
	status.syscall_errno = stored.syscall_errno;
	status.operation_result = stored.operation_result;
	status.method_result = stored.method_result;
	status.value = stored.value;
	status.received_size = stored.received_size;
	status.remaining_us = stored.remaining_us;
	return status;
}
std::optional<Result> Completion::WaitFor(std::chrono::milliseconds slice) const {
	std::unique_lock<std::mutex> lock(m->mutex);
	m->changed.wait_for(lock, slice, [&] { return m->result.has_value(); });
	return m->result;
}
Completion::ReplyState Completion::InspectReply() const {
	std::lock_guard<std::mutex> lock(m->mutex);
	return m->reply;
}
Bytes Completion::InspectReplyPacket() const {
	std::lock_guard<std::mutex> lock(m->mutex);
	return m->reply_packet;
}
Ticket Core::MakeTicket(const std::shared_ptr<void>& pin) {
	auto t = std::make_shared<Completion>(); t->m->pin = pin; return t;
}
void Core::Finish(const Ticket& t, Result result) {
	if (!t) { return; }
	{
		std::lock_guard<std::mutex> lock(t->m->mutex);
		if (t->m->result) { return; }
		t->m->result = std::move(result);
		// Break the pending operation's lifetime cycle only after publishing its
		// owned result. Dropping a caller's ticket must not cancel an operation.
		t->m->pin.reset();
	}
	t->m->changed.notify_all();
}

struct Core::Impl {
	enum class State { Idle, WaitingServer, Connecting, Connected, Disconnecting, Terminating };
	struct EventWait {
		uint64_t bits;
		uint32_t mode;
		std::optional<Clock::time_point> deadline;
		Ticket ticket;
	};
	struct Event { uint64_t pattern = 0; std::deque<EventWait> waiters; };
	struct QueueSend {
		Identity sender;
		Handle tag;
		Bytes frame;
		std::optional<Clock::time_point> deadline;
		Ticket ticket;
	};
	struct Queue {
		uint64_t capacity = 0, used = 0;
		Handle reader = 0;
		bool deferred_flush = false;
		std::deque<Bytes> frames;
		std::deque<QueueSend> writers;
	};
	struct Request {
		Identity caller {};
		std::optional<Identity> receiver;
		uint32_t method;
		uint32_t async_ticket = 0;
		uint32_t reply_method = 0;
		std::vector<Bytes> inputs;
		std::vector<uint64_t> capacities;
		Ticket ticket;
	};
	struct Client {
		int32_t pid;
		std::string name;
		Config config;
		Handle id = 0, server = 0, session = 0;
		uint64_t native = 0, argument = 0;
		Bytes connect_argument;
		State state = State::Idle;
		Identity connector {}, disconnector {};
		std::optional<Identity> accepting, disconnect_receiver;
		bool session_closed = false;
		Ticket server_wait, connect, disconnect;
		std::vector<std::shared_ptr<Request>> slots;
		std::vector<std::shared_ptr<Request>> async_slots;
		uint32_t next_async = 0;
		// Failed DATA copy releases the caller, not the unread response packet.
		// Future invocation semantics with such a packet are deliberately excluded.
		std::list<std::shared_ptr<Request>> response_packets;
		uint32_t active_receivers = 0;
		std::vector<Event> events;
		std::vector<Queue> queues;
	};
	struct Item {
		Dispatch::Type type;
		std::shared_ptr<Client> client;
		std::shared_ptr<Request> request;
	};
	struct Server {
		int32_t pid;
		std::string name;
		uint64_t handler;
		ServerConfig config;
		bool sealed = false, published = true;
		std::optional<Identity> reader;
		std::deque<Item> queue;
	};
	mutable std::mutex mutex;
	Handle next = 1;
	std::map<Handle, std::shared_ptr<Server>> servers;
	std::map<Handle, std::shared_ptr<Client>> clients;
	std::map<Handle, std::shared_ptr<Client>> sessions;
	struct ReceiveLease { Identity receiver; std::shared_ptr<Client> client; uint32_t index; };
	std::map<Handle, ReceiveLease> receives;
	// One uncompleted native dispatch per guest execution identity in this bounded core.
	std::map<std::pair<int32_t, int32_t>, Item> bindings;
	std::map<std::pair<int32_t, int32_t>, Handle> preparing;
	static auto Key(Identity id) { return std::make_pair(id.pid, id.tid); }
	void Publish(Handle handle, Server& server) {
		server.published = true;
		for (const auto& entry: clients) {
			auto& c = *entry.second;
			if (c.state == State::WaitingServer && c.name == server.name) {
				Result ready; ready.value = handle;
				Core::Finish(c.server_wait, std::move(ready));
			}
		}
	}
	static bool Valid(Identity id) { return id.pid > 0 && id.tid > 0; }
	static bool Mode(uint32_t mode) {
		return ((mode & 3) == 1 || (mode & 3) == 2) && (mode & 0x30) != 0x30 && !(mode & ~0x33u);
	}
	static bool Satisfied(uint64_t pattern, uint64_t bits, uint32_t mode) {
		return (mode & 3) == 1 ? (pattern & bits) == bits : (pattern & bits) != 0;
	}
	static Result Observe(Event& e, uint64_t bits, uint32_t mode) {
		auto r = Satisfied(e.pattern, bits, mode) ? Result::Success() : Result::Failure(Busy);
		r.value = e.pattern;
		if (r.syscall_errno == Ok) {
			if (mode & 0x10) { e.pattern = 0; }
			else if (mode & 0x20) { e.pattern &= ~bits; }
		}
		return r;
	}
	std::shared_ptr<Client> ClientFor(Identity id, Handle h) const {
		auto i = clients.find(h);
		return Valid(id) && i != clients.end() && i->second->pid == id.pid ? i->second : nullptr;
	}
	std::shared_ptr<Server> ServerFor(Identity id, Handle h) const {
		auto i = servers.find(h);
		return Valid(id) && i != servers.end() && i->second->pid == id.pid ? i->second : nullptr;
	}
	std::shared_ptr<Client> SessionFor(Identity id, Handle h) const {
		auto i = sessions.find(h);
		if (!Valid(id) || i == sessions.end()) { return nullptr; }
		return ServerFor(id, i->second->server) ? i->second : nullptr;
	}
	static bool Active(const Client& c) {
		return c.active_receivers != 0 || std::any_of(c.slots.begin(), c.slots.end(), [](const auto& r) { return bool(r); }) ||
		       std::any_of(c.async_slots.begin(), c.async_slots.end(), [](const auto& r) { return bool(r); });
	}
	static bool Waiting(const Client& c) {
		for (const auto& e: c.events) { if (!e.waiters.empty()) { return true; } }
		for (const auto& q: c.queues) { if (q.reader || !q.writers.empty()) { return true; } }
		return false;
	}
	static Result SendResult(const QueueSend& w, int error, Clock::time_point now) {
		auto r = error == Ok ? Result::Success() : Result::Failure(error);
		if (w.deadline) {
			auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(*w.deadline - now).count();
			r.remaining_us = static_cast<uint32_t>(std::max<int64_t>(0, remaining));
		}
		return r;
	}
	static void Flush(Queue& q, Clock::time_point now) {
		q.frames.clear(); q.used = 0; q.deferred_flush = false;
		for (auto& w: q.writers) { Core::Finish(w.ticket, SendResult(w, Cancelled, now)); }
		q.writers.clear();
	}
	static void Advance(Queue& q, Clock::time_point now) {
		// A timed out writer must not enqueue merely because space became available.
		for (auto i = q.writers.begin(); i != q.writers.end();) {
			if (i->deadline && now >= *i->deadline) {
				Core::Finish(i->ticket, SendResult(*i, WouldBlock, now)); i = q.writers.erase(i);
			} else { ++i; }
		}
		if (q.reader) { return; }
		if (q.deferred_flush) { Flush(q, now); return; }
		// Head-of-line writer priority: a small newcomer cannot bypass an older
		// reservation. Frame copy/commit is atomic here (host payload already owned).
		while (!q.writers.empty() && q.writers.front().frame.size() <= q.capacity - q.used) {
			auto w = std::move(q.writers.front()); q.writers.pop_front();
			q.used += w.frame.size(); q.frames.push_back(std::move(w.frame));
			Core::Finish(w.ticket, SendResult(w, Ok, now));
		}
	}
};

Core::Core(): m(std::make_unique<Impl>()) {}
Core::~Core() {
	// Host core abandonment is not guest process-exit emulation. Unblock tickets
	// explicitly as unsupported; never manufacture successful guest teardown.
	for (auto& entry: m->clients) {
		auto& c = *entry.second;
		Finish(c.server_wait, Result::Failure(Unsupported));
		Finish(c.connect, Result::Failure(Unsupported));
		Finish(c.disconnect, Result::Failure(Unsupported));
		for (auto& r: c.slots) { if (r) { Finish(r->ticket, Result::Failure(Unsupported)); } }
		for (auto& r: c.async_slots) { if (r) { Finish(r->ticket, Result::Failure(Unsupported)); } }
		for (auto& r: c.response_packets) { Finish(r->ticket, Result::Failure(Unsupported)); }
		for (auto& e: c.events) { for (auto& w: e.waiters) { Finish(w.ticket, Result::Failure(Unsupported)); } }
		for (auto& q: c.queues) { for (auto& w: q.writers) { Finish(w.ticket, Result::Failure(Unsupported)); } }
	}
}

Result Core::RegisterServer(Identity owner, const std::string& name, uint64_t handler, Handle& out) {
	ServerConfig config; config.handler = handler;
	return RegisterServer(owner, name, config, out);
}
Result Core::RegisterServer(Identity owner, const std::string& name, const ServerConfig& config, Handle& out, bool published) {
	std::lock_guard<std::mutex> lock(m->mutex);
	if (!Impl::Valid(owner) || name.empty() || name.size() > 24 || name.find('\0') != std::string::npos || !config.handler) {
		return Result::Failure(Invalid);
	}
	for (const auto& e: m->servers) { if (e.second->name == name) { return Result::Failure(Busy); } }
	if (m->next > INT32_MAX) { return Result::Failure(NoMemory); }
	auto s = std::make_shared<Impl::Server>(); s->pid = owner.pid; s->name = name; s->handler = config.handler; s->config = config;
	s->published = published;
	const auto handle = m->next;
	m->servers.emplace(handle, s);
	if (published) { m->Publish(handle, *s); }
	++m->next; out = handle; return Result::Success();
}
Result Core::PublishServer(Identity owner, Handle h) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(owner, h);
	if (!s) { return Result::Failure(NotFound); }
	if (s->sealed) { return Result::Failure(NotConnected); }
	if (!s->published) { m->Publish(h, *s); }
	return Result::Success();
}
Result Core::RegisterClient(Identity owner, const std::string& name, const Config& config, Handle& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	if (!Impl::Valid(owner) || name.empty() || name.size() > 24 || name.find('\0') != std::string::npos ||
	    !config.sync_slots || config.sync_slots > 16 || config.event_count > 16) { return Result::Failure(Invalid); }
	if (config.async_slots > 16) { return Result::Failure(Invalid); }
	if (config.queue_count > 32 || config.queue_capacities.size() != config.queue_count ||
	    std::any_of(config.queue_capacities.begin(), config.queue_capacities.end(), [](auto n) { return n < 8; })) {
		return Result::Failure(Invalid);
	}
	if (m->next > INT32_MAX) { return Result::Failure(NoMemory); }
	auto c = std::make_shared<Impl::Client>(); c->pid = owner.pid; c->name = name; c->config = config;
	c->slots.resize(config.sync_slots); c->events.resize(config.event_count);
	c->async_slots.resize(config.async_slots);
	c->queues.resize(config.queue_count);
	for (size_t i = 0; i < c->queues.size(); ++i) { c->queues[i].capacity = config.queue_capacities[i]; }
	const auto handle = m->next;
	c->id = handle;
	m->clients.emplace(handle, std::move(c));
	++m->next; out = handle; return Result::Success();
}
Result Core::Name(Identity owner, Handle h, std::array<char, 25>& out) const {
	std::lock_guard<std::mutex> lock(m->mutex);
	std::string name;
	if (auto s = m->ServerFor(owner, h)) { name = s->name; }
	else if (auto c = m->ClientFor(owner, h)) { name = c->name; }
	else { return Result::Failure(NotFound); }
	out.fill(0); std::copy(name.begin(), name.end(), out.begin()); return Result::Success();
}
std::array<char, 25> Core::TraceServerName(Handle h) const {
	std::array<char, 25> name {};
	std::lock_guard<std::mutex> lock(m->mutex);
	const auto server = m->servers.find(h);
	if (server != m->servers.end()) {
		std::copy(server->second->name.begin(), server->second->name.end(), name.begin());
	}
	return name;
}
Result Core::Inspect(Identity owner, Handle h, ObjectKind kind, ObjectInfo& out) const {
	std::lock_guard<std::mutex> lock(m->mutex);
	ObjectInfo info;
	if (kind == ObjectKind::Server) {
		auto s = m->ServerFor(owner, h); if (!s) { return Result::Failure(NotFound); }
		info = {s->config.native_object, s->config.user_argument, s->handler, s->name};
	} else if (kind == ObjectKind::Client) {
		auto c = m->ClientFor(owner, h); if (!c) { return Result::Failure(NotFound); }
		info = {c->config.native_object, c->config.user_argument, 0, c->name};
	} else {
		auto c = m->SessionFor(owner, h); if (!c) { return Result::Failure(NotFound); }
		info = {c->native, c->argument, m->servers.at(c->server)->handler, c->name};
	}
	out = std::move(info); return Result::Success();
}
Result Core::SessionPeerPid(Identity owner, Handle h, int32_t& out) const {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, h); if (!c) { return Result::Failure(NotFound); }
	// 13.60 809ef269 checks session-present bit20000, not connection high
	// state: peer PID remains readable during ordinary/forced close callbacks.
	if (c->state == Impl::State::Idle) { return Result::Failure(NotConnected); }
	out = c->pid; return Result::Success();
}
Result Core::Connection(Identity caller, Handle h, ConnectionInfo& out, Handle expected_server) const {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	auto s = std::find_if(m->servers.begin(), m->servers.end(), [&](const auto& e) {
		return e.second->published && e.second->name == c->name && (!expected_server || e.first == expected_server);
	});
	if (s == m->servers.end()) { return Result::Failure(NotFound); }
	if (s->second->sealed) { return Result::Failure(NotConnected); }
	out = {s->first, s->second->pid, s->second->config, c->config}; return Result::Success();
}
Result Core::WaitServer(Identity caller, Handle h, Ticket& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (c->state == Impl::State::WaitingServer && c->connector == caller) {
		out = c->server_wait; return Result::Success();
	}
	if (c->state != Impl::State::Idle) { return Result::Failure(Busy); }
	auto ticket = MakeTicket(c);
	c->state = Impl::State::WaitingServer; c->connector = caller; c->server_wait = ticket;
	for (const auto& entry: m->servers) {
		if (entry.second->published && !entry.second->sealed && entry.second->name == c->name) {
			Result ready; ready.value = entry.first; Finish(ticket, std::move(ready)); break;
		}
	}
	out = std::move(ticket); return Result::Success();
}
Result Core::AbandonServerWait(Identity caller, Handle h, const Ticket& ticket) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (!ticket || c->server_wait != ticket || !(c->connector == caller)) { return Result::Failure(Invalid); }
	if (c->state != Impl::State::WaitingServer) { return Result::Failure(Busy); }
	Finish(ticket, Result::Failure(Cancelled));
	c->server_wait.reset(); c->state = Impl::State::Idle;
	return Result::Success();
}
Result Core::Connect(Identity caller, Handle h, Ticket& out, Bytes argument, Handle expected_server, const Ticket& server_wait) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (server_wait) {
		if (c->state != Impl::State::WaitingServer || c->server_wait != server_wait || !(c->connector == caller)) {
			return Result::Failure(Invalid);
		}
		const auto ready = server_wait->PollStatus();
		if (!ready) { return Result::Failure(WouldBlock); }
		if (ready->syscall_errno) { return Result::Failure(ready->syscall_errno); }
		if (!expected_server || expected_server != ready->value) { return Result::Failure(NotFound); }
	} else if (c->state != Impl::State::Idle) { return Result::Failure(Busy); }
	auto s = std::find_if(m->servers.begin(), m->servers.end(), [&](const auto& e) {
		return e.second->published && e.second->name == c->name;
	});
	if (s == m->servers.end()) { return Result::Failure(NotFound); }
	if (s->second->sealed) { return Result::Failure(NotConnected); }
	if (expected_server && expected_server != s->first) { return Result::Failure(NotFound); }
	if (argument.size() > s->second->config.argument_limit) { return Result::Failure(Invalid); }
	// Allocate before publishing Connecting or the caller's ticket. In
	// particular a failed deque growth must not leave an undispatchable waiter
	// pinned to this client. The Core lock prevents receive before commit.
	auto ticket = MakeTicket(c);
	s->second->queue.push_back({Dispatch::Type::Connect, c, {}});
	c->connect_argument = std::move(argument);
	c->state = Impl::State::Connecting; c->server = s->first; c->connector = caller;
	c->session_closed = false;
	c->server_wait.reset();
	c->connect = ticket; out = std::move(ticket);
	return Result::Success();
}
Result Core::Receive(Identity receiver, Handle h, Dispatch& out) {
	Dispatch dispatch;
	auto result = BeginReceive(receiver, h, dispatch);
	if (result.syscall_errno) { return result; }
	result = EndReceive(receiver, h, true);
	if (!result.syscall_errno) { out = std::move(dispatch); }
	return result;
}
Result Core::BeginReceive(Identity receiver, Handle h, Dispatch& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(receiver, h); if (!s) { return Result::Failure(NotFound); }
	if (s->reader || m->bindings.count(Impl::Key(receiver)) || m->preparing.count(Impl::Key(receiver))) { return Result::Failure(Busy); }
	if (s->queue.empty()) { return Result::Failure(WouldBlock); }
	auto item = s->queue.front();
	if (item.type == Dispatch::Type::ForcedDisconnect && (Impl::Active(*item.client) || Impl::Waiting(*item.client))) {
		return Result::Failure(WouldBlock);
	}
	Dispatch d; d.type = item.type; d.handler = s->handler;
	if (item.client) {
		auto& c = *item.client; d.client = c.id; d.session = c.session; d.native_session = c.native;
		d.argument = c.argument; d.config = c.config;
		if (item.type == Dispatch::Type::Connect) { d.caller = c.connector; d.connect_argument = c.connect_argument; }
		else if (item.type == Dispatch::Type::Disconnect || item.type == Dispatch::Type::ForcedDisconnect) { d.caller = c.disconnector; }
		else {
			d.caller = item.request->caller;
			d.method = item.request->method; d.inputs = item.request->inputs; d.output_capacities = item.request->capacities;
			d.async_ticket = item.request->async_ticket;
		}
	}
	m->preparing.emplace(Impl::Key(receiver), h); s->reader = receiver;
	out = std::move(d); return Result::Success();
}
Result Core::EndReceive(Identity receiver, Handle h, bool consume) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(receiver, h); if (!s) { return Result::Failure(NotFound); }
	if (!s->reader || !(*s->reader == receiver)) { return Result::Failure(NotFound); }
	if (consume) {
		auto item = s->queue.front();
		if (item.client) {
			m->bindings.emplace(Impl::Key(receiver), item);
			if (item.type == Dispatch::Type::Connect) { item.client->accepting = receiver; }
			else if (item.type == Dispatch::Type::Disconnect || item.type == Dispatch::Type::ForcedDisconnect) { item.client->disconnect_receiver = receiver; }
			else { item.request->receiver = receiver; ++item.client->active_receivers; }
		}
		s->queue.pop_front();
	}
	s->reader.reset(); m->preparing.erase(Impl::Key(receiver)); return Result::Success();
}
Result Core::SealServer(Identity owner, Handle h) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(owner, h);
	if (!s) { return Result::Failure(NotFound); }
	if (s->reader) { return Result::Failure(Busy); }
	for (const auto& c: m->clients) {
		if (c.second->server == h) { return Result::Failure(Busy); }
	}
	s->sealed = true;
	return Result::Success();
}
Result Core::Wake(Identity owner, Handle h) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(owner, h); if (!s) { return Result::Failure(NotFound); }
	s->queue.push_back({Dispatch::Type::Wake, {}, {}}); return Result::Success();
}
Result Core::CreateSession(Identity receiver, uint64_t native, uint64_t argument, Handle& out, Handle expected_server) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Connect || !native) { return Result::Failure(Invalid); }
	auto c = i->second.client;
	if (expected_server && expected_server != c->server) { return Result::Failure(NotFound); }
	if (c->session) { return Result::Failure(Busy); }
	if (m->next > INT32_MAX) { return Result::Failure(NoMemory); }
	const auto session = m->next;
	m->sessions.emplace(session, c);
	++m->next;
	c->session = session; c->native = native; c->argument = argument;
	out = session; return Result::Success();
}
Result Core::Accept(Identity receiver, Handle client, int32_t result) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Connect) { return Result::Failure(Invalid); }
	auto c = i->second.client;
	if (c->id != client || !c->accepting || !(*c->accepting == receiver) || !m->ServerFor(receiver, c->server)) {
		return Result::Failure(NotFound);
	}
	if (result == 0 && !c->session) { return Result::Failure(Invalid); }
	if (result != 0) {
		// The accepting callback may already have freed its failed session's
		// storage. Roll back the association without dereferencing native memory.
		m->sessions.erase(c->session);
		c->session = 0; c->native = 0; c->argument = 0; c->server = 0;
		c->state = Impl::State::Idle;
		for (auto& e: c->events) { e.pattern = 0; }
		for (auto& q: c->queues) { Impl::Flush(q, Clock::now()); }
	} else { c->state = Impl::State::Connected; }
	c->connect_argument.clear(); c->accepting.reset(); m->bindings.erase(i);
	auto r = Result::Success(); r.method_result = result; Finish(c->connect, r); return Result::Success();
}
Result Core::Invoke(Identity caller, Handle h, uint32_t method, std::vector<Bytes> inputs,
                    std::vector<uint64_t> capacities, Ticket& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	for (const auto& request: c->response_packets) {
		const auto state = request->ticket->InspectReply();
		if (state.caller_released && !state.consumed) { return Result::Failure(Unsupported); }
	}
	if (inputs.size() > 10 || capacities.size() > 10) { return Result::Failure(Invalid); }
	uint64_t size = 0;
	for (const auto& b: inputs) { if (b.size() > c->config.input_limit - size) { return Result::Failure(Invalid); } size += b.size(); }
	size = 0;
	for (auto n: capacities) { if (n > c->config.output_limit - size) { return Result::Failure(Invalid); } size += n; }
	for (const auto& r: c->slots) { if (r && r->caller == caller) { return Result::Failure(Busy); } }
	auto slot = std::find(c->slots.begin(), c->slots.end(), nullptr);
	if (slot == c->slots.end()) { return Result::Failure(WouldBlock); }
	auto r = std::make_shared<Impl::Request>(); r->caller = caller; r->method = method;
	r->inputs = std::move(inputs); r->capacities = std::move(capacities);
	auto ticket = MakeTicket(c); r->ticket = ticket;
	c->response_packets.push_back(r); // retain before any publication/copy-fault cleanup
	try { m->servers.at(c->server)->queue.push_back({Dispatch::Type::Request, c, r}); }
	catch (...) { c->response_packets.pop_back(); throw; }
	*slot = r; out = std::move(ticket);
	return Result::Success();
}
Result Core::Reply(Identity receiver, Handle session, int32_t method, std::vector<Bytes> outputs) {
	auto result = PublishReply(receiver, session, method, std::move(outputs));
	return result.syscall_errno ? result : ReleaseReplyReceiver(receiver, session);
}
Result Core::PublishReply(Identity receiver, Handle session, int32_t method, std::vector<Bytes> outputs) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(receiver, session); if (!c) { return Result::Failure(NotFound); }
	if (c->state == Impl::State::Terminating) { return Result::Failure(NotConnected); }
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Request || i->second.client != c) { return Result::Failure(Invalid); }
	auto req = i->second.request;
	// Server publication is bounded by its negotiated session output budget,
	// not the individual caller's BufferInfo capacities/count. Those are checked
	// when the client receives the response, before touching its output buffers.
	if (outputs.size() > 10) { return Result::Failure(Invalid); }
	uint64_t total = 0;
	for (const auto& bytes: outputs) {
		if (bytes.size() > c->config.output_limit - total) { return Result::Failure(Invalid); }
		total += bytes.size();
	}
	const uint64_t prefix_size = 16 + outputs.size() * 4;
	if (total > UINT32_MAX - prefix_size) { return Result::Failure(Invalid); }
	const uint64_t packet_size = prefix_size + total;
	// Literal native packet evidence here covers zero/one output only. Multiple
	// outputs retain the typed response below; do not invent their wire packing.
	Bytes packet(outputs.size() <= 1 ? static_cast<size_t>(packet_size) : 0);
	const auto word = [&](size_t offset, uint32_t value) {
		for (size_t n = 0; n < 4; ++n) { packet[offset + n] = static_cast<uint8_t>(value >> (n * 8)); }
	};
	if (!packet.empty()) {
		word(0, 0x42); word(4, static_cast<uint32_t>(packet_size));
		word(8, static_cast<uint32_t>(method)); word(12, static_cast<uint32_t>(outputs.size()));
		if (!outputs.empty()) {
			word(16, static_cast<uint32_t>(outputs[0].size()));
			std::copy(outputs[0].begin(), outputs[0].end(), packet.begin() + 20);
		}
	}
	auto r = Result::Success(); r.method_result = method; r.outputs = std::move(outputs);
	{
		std::lock_guard<std::mutex> ticket_lock(req->ticket->m->mutex);
		if (req->ticket->m->result) { return Result::Failure(Busy); }
		req->ticket->m->result = std::move(r);
		req->ticket->m->reply_packet = std::move(packet);
		req->ticket->m->reply.published = true;
		req->ticket->m->pin.reset();
	}
	req->ticket->m->changed.notify_all();
	return Result::Success();
}
Result Core::ReleaseReplyReceiver(Identity receiver, Handle session) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(receiver, session); if (!c) { return Result::Failure(NotFound); }
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Request || i->second.client != c) { return Result::Failure(Invalid); }
	const auto& ticket = i->second.request->ticket;
	{
		std::lock_guard<std::mutex> ticket_lock(ticket->m->mutex);
		if (!ticket->m->reply.published && c->state != Impl::State::Terminating) { return Result::Failure(Busy); }
		ticket->m->reply.receiver_released = true;
	}
	--c->active_receivers;
	m->bindings.erase(i); return Result::Success();
}
Result Core::BeginInvokeRead(Identity caller, Handle client, const Ticket& ticket, Result& response) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	const auto slot = std::find_if(c->slots.begin(), c->slots.end(), [&](const auto& r) {
		return r && r->caller == caller && r->ticket == ticket;
	});
	if (slot == c->slots.end()) { return Result::Failure(NotFound); }
	std::lock_guard<std::mutex> ticket_lock(ticket->m->mutex);
	auto& state = ticket->m->reply;
	if (c->state == Impl::State::Terminating) { return Result::Failure(NotConnected); }
	if (!state.published) { return Result::Failure(WouldBlock); }
	if (state.read_reserved || state.consumed) { return Result::Failure(Busy); }
	response = *ticket->m->result;
	state.read_reserved = true;
	return Result::Success();
}
Result Core::AbortRequest(Identity receiver, Handle session, int error) {
	if (error <= 0) { return Result::Failure(Invalid); }
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(receiver, session); if (!c) { return Result::Failure(NotFound); }
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Request || i->second.client != c) {
		return Result::Failure(Invalid);
	}
	Finish(i->second.request->ticket, Result::Failure(error));
	--c->active_receivers; m->bindings.erase(i); return Result::Success();
}
Result Core::InspectInvoke(Identity caller, Handle client, Ticket& out) const {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	for (const auto& request: c->slots) {
		if (request && request->caller == caller) { out = request->ticket; return Result::Success(); }
	}
	return Result::Failure(NotFound);
}
Result Core::EndInvokeRead(Identity caller, Handle client, const Ticket& ticket, bool consume) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	const auto slot = std::find_if(c->slots.begin(), c->slots.end(), [&](const auto& r) {
		return r && r->caller == caller && r->ticket == ticket;
	});
	if (slot == c->slots.end()) { return Result::Failure(NotFound); }
	std::lock_guard<std::mutex> ticket_lock(ticket->m->mutex);
	auto& state = ticket->m->reply;
	if (!state.read_reserved) { return Result::Failure(Invalid); }
	state.read_reserved = false;
	state.consumed = consume;
	if (consume) { ticket->m->reply_packet.clear(); }
	return Result::Success();
}
Result Core::ReleaseInvokeCaller(Identity caller, Handle client, const Ticket& ticket) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	const auto slot = std::find_if(c->slots.begin(), c->slots.end(), [&](const auto& r) {
		return r && r->caller == caller && r->ticket == ticket;
	});
	if (slot == c->slots.end()) { return Result::Failure(NotFound); }
	std::lock_guard<std::mutex> ticket_lock(ticket->m->mutex);
	auto& state = ticket->m->reply;
	if (state.read_reserved) { return Result::Failure(Busy); }
	if (state.consumed || c->state == Impl::State::Terminating) { c->response_packets.remove(*slot); }
	state.caller_released = true;
	slot->reset();
	return Result::Success();
}
Result Core::Disconnect(Identity caller, Handle h, Ticket& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	// Explicit resume: the same guest thread gets the existing completion, never a second command.
	if (c->state == Impl::State::Disconnecting && c->disconnector == caller) {
		out = c->disconnect; return Result::Success();
	}
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (Impl::Active(*c) || Impl::Waiting(*c)) { return Result::Failure(Busy); }
	auto ticket = MakeTicket(c);
	m->servers.at(c->server)->queue.push_back({Dispatch::Type::Disconnect, c, {}});
	c->state = Impl::State::Disconnecting; c->disconnector = caller;
	c->disconnect = ticket; out = std::move(ticket);
	return Result::Success();
}
Result Core::CloseSession(Identity owner, Handle session) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, session);
	Result r = Result::Success();
	if (!c) { r.syscall_errno = NotFound; return r; }
	if (c->state == Impl::State::Connecting && c->accepting && *c->accepting == owner) {
		m->sessions.erase(session); c->session = 0; c->native = 0; c->argument = 0;
		return r;
	}
	// Ordinary close retains the222 binding; forced close retires its own
	// notification. Neither may free an active request/copy reservation.
	if (Impl::Active(*c) || Impl::Waiting(*c)) { r.syscall_errno = Busy; return r; }
	if ((c->state != Impl::State::Disconnecting && c->state != Impl::State::Terminating) || !c->disconnect_receiver) {
		r.syscall_errno = Unsupported; return r;
	}
	if (!(*c->disconnect_receiver == owner)) { return Result::Failure(Invalid); }
	m->sessions.erase(session); c->session_closed = true; c->session = 0; c->native = 0; c->argument = 0;
	if (c->state == Impl::State::Terminating) {
		m->bindings.erase(Impl::Key(owner));
		c->state = Impl::State::Idle; c->server = 0; c->disconnect_receiver.reset(); c->session_closed = false;
		c->response_packets.clear();
	}
	return r;
}
Result Core::AcknowledgeDisconnect(Identity receiver, Handle h, int32_t result) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::Disconnect || i->second.client->id != h) { return Result::Failure(Invalid); }
	auto c = i->second.client;
	if (!c->session_closed) { return Result::Failure(Busy); }
	auto r = Result::Success(); r.method_result = result; Finish(c->disconnect, r);
	c->state = Impl::State::Idle; c->server = 0; c->disconnect_receiver.reset(); c->session_closed = false;
	c->response_packets.clear();
	for (auto& e: c->events) { e.pattern = 0; }
	for (auto& q: c->queues) { Impl::Flush(q, Clock::now()); }
	m->bindings.erase(i); return Result::Success();
}
Result Core::DestroyClient(Identity owner, Handle h) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(owner, h); auto r = Result::Success();
	if (!c) { r.syscall_errno = NotFound; return r; }
	if (c->state == Impl::State::WaitingServer) {
		Finish(c->server_wait, Result::Failure(Cancelled));
		c->server_wait.reset(); c->state = Impl::State::Idle;
	}
	if (c->state != Impl::State::Idle || c->session || Impl::Active(*c) || Impl::Waiting(*c)) { r.syscall_errno = Busy; return r; }
	m->clients.erase(h); return r;
}
Result Core::DestroyServer(Identity owner, Handle h) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto s = m->ServerFor(owner, h); auto r = Result::Success();
	if (!s) { r.syscall_errno = NotFound; return r; }
	if (s->reader) { r.syscall_errno = Busy; return r; }
	for (const auto& e: m->clients) {
		if (e.second->server == h) {
			r.syscall_errno = e.second->session ? Busy : Unsupported; return r;
		}
	}
	m->servers.erase(h); return r;
}
Result Core::PollEvent(Identity caller, Handle h, uint32_t index, uint64_t bits, uint32_t mode) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (index >= c->events.size() || !Impl::Mode(mode)) { return Result::Failure(Invalid); }
	return Impl::Observe(c->events[index], bits, mode);
}
Result Core::WaitEvent(Identity caller, Handle h, uint32_t index, uint64_t bits, uint32_t mode,
                       std::optional<std::chrono::microseconds> timeout, Ticket& out, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, h); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (index >= c->events.size() || !Impl::Mode(mode) ||
	    (timeout && (timeout->count() < 0 || static_cast<uint64_t>(timeout->count()) > UINT32_MAX))) {
		return Result::Failure(Invalid);
	}
	// Allocate before Observe can consume event bits. Publish the caller ticket
	// only once waiter insertion/completion has succeeded.
	auto ticket = MakeTicket(c);
	auto& e = c->events[index]; auto r = Impl::Observe(e, bits, mode);
	if (r.syscall_errno == Ok) { Finish(ticket, r); }
	else if (timeout && timeout->count() == 0) { r.syscall_errno = TimedOut; Finish(ticket, r); }
	else {
		std::optional<Clock::time_point> deadline;
		if (timeout) { deadline = now + *timeout; }
		e.waiters.push_back({bits, mode, deadline, ticket});
	}
	out = std::move(ticket);
	return Result::Success();
}
Result Core::SetEvent(Identity owner, Handle session, uint32_t index, uint64_t bits) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, session); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected && c->state != Impl::State::Connecting) { return Result::Failure(NotConnected); }
	if (index >= c->events.size()) { return Result::Failure(Invalid); }
	auto& e = c->events[index]; e.pattern |= bits;
	for (auto i = e.waiters.begin(); i != e.waiters.end();) {
		auto r = Impl::Observe(e, i->bits, i->mode);
		if (r.syscall_errno == Ok) { Finish(i->ticket, r); i = e.waiters.erase(i); }
		else { ++i; }
	}
	return Result::Success();
}
Result Core::CancelEvent(Identity owner, Handle h, uint32_t index, uint64_t replacement) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(owner, h); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (index >= c->events.size()) { return Result::Failure(Invalid); }
	auto& e = c->events[index]; auto r = Result::Success();
	auto cancelled = Result::Failure(Cancelled); cancelled.value = e.pattern;
	for (auto& w: e.waiters) { ++r.value; Finish(w.ticket, cancelled); }
	e.pattern = replacement; e.waiters.clear(); return r;
}
Result Core::AbandonEventWait(Identity owner, Handle h, const Ticket& ticket) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(owner, h);
	if (!c) { return Result::Failure(NotFound); }
	if (!ticket) { return Result::Failure(Invalid); }
	for (auto& event: c->events) {
		for (auto i = event.waiters.begin(); i != event.waiters.end(); ++i) {
			if (i->ticket == ticket) {
				Finish(ticket, Result::Failure(Cancelled));
				event.waiters.erase(i);
				return Result::Success();
			}
		}
	}
	// A concurrent set, timeout or forced teardown already unlinked it.
	return ticket->PollStatus() ? Result::Success() : Result::Failure(Invalid);
}
void Core::ExpireEvents(Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	for (auto& c: m->clients) { for (auto& e: c.second->events) {
		for (auto i = e.waiters.begin(); i != e.waiters.end();) {
			if (i->deadline && now >= *i->deadline) {
				auto r = Result::Failure(TimedOut); r.value = e.pattern; Finish(i->ticket, r); i = e.waiters.erase(i);
			} else { ++i; }
		}
	} }
}
Result Core::Wait(const Ticket& t, const std::function<bool()>& interrupted) {
	if (!t) { return Result::Failure(Invalid); }
	for (;;) {
		ExpireEvents();
		ExpireQueues();
		if (auto r = t->Poll()) { return *r; }
		if (interrupted && interrupted()) {
			if (auto r = t->Poll()) { return *r; }
			return Result::Failure(Interrupted);
		}
		if (auto r = t->WaitFor(std::chrono::milliseconds(1))) { return *r; }
	}
}
Result Core::SendQueue(Identity owner, Handle session, uint32_t index, Bytes payload,
                       std::optional<std::chrono::microseconds> timeout, Ticket& out, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	if (payload.empty() || (timeout && (timeout->count() < 0 || static_cast<uint64_t>(timeout->count()) > UINT32_MAX))) {
		return Result::Failure(Invalid);
	}
	if (payload.size() >= 0x7fff9) { return Result::Failure(NoMemory); }
	auto c = m->SessionFor(owner, session); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connecting && c->state != Impl::State::Connected) {
		return Result::Failure(NotConnected);
	}
	if (index >= c->queues.size()) { return Result::Failure(Invalid); }
	auto& q = c->queues[index]; Impl::Advance(q, now);
	const auto total = payload.size() + 8;
	if (total > q.capacity) { return Result::Failure(NoMemory); }
	for (const auto& w: q.writers) { if (w.sender == owner) { return Result::Failure(Busy); } }
	Bytes frame(total);
	// Kernel queue frame: little-endian uint32 type=80, uint32 total_size.
	frame[0] = 0x80;
	for (unsigned i = 0; i < 4; ++i) { frame[4 + i] = static_cast<uint8_t>(total >> (8 * i)); }
	std::copy(payload.begin(), payload.end(), frame.begin() + 8);
	Impl::QueueSend w {owner, session, std::move(frame), {}, MakeTicket(c)};
	if (timeout) { w.deadline = now + *timeout; }
	out = w.ticket;
	if (!q.reader && q.writers.empty() && total <= q.capacity - q.used) {
		q.used += total; q.frames.push_back(std::move(w.frame));
		Finish(out, Impl::SendResult(w, Ok, now));
	} else if (timeout && timeout->count() == 0) {
		Finish(out, Impl::SendResult(w, WouldBlock, now));
	} else { q.writers.push_back(std::move(w)); }
	return Result::Success();
}

Result Core::BeginQueueReceive(Identity caller, Handle client, uint32_t index, uint64_t capacity,
                               QueueReceive& out, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	if (capacity >= UINT64_MAX - 7) { return Result::Failure(BadMessage); }
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (index >= c->queues.size()) { return Result::Failure(Invalid); }
	auto& q = c->queues[index]; Impl::Advance(q, now);
	if (q.reader || q.frames.empty()) { return Result::Failure(WouldBlock); }
	const auto& frame = q.frames.front();
	// Only SendQueue constructs frames. Keep explicit validation at the parser
	// boundary so malformed internal frames cannot become plausible payloads.
	if (frame.size() < 8) { return Result::Failure(BadMessage); }
	auto word = [&](size_t offset) {
		uint32_t n = 0;
		for (unsigned i = 0; i < 4; ++i) { n |= static_cast<uint32_t>(frame[offset + i]) << (8 * i); }
		return n;
	};
	if (word(0) != 0x80 || word(4) != frame.size()) { return Result::Failure(BadMessage); }
	if (frame.size() - 8 > capacity) { return Result::Failure(NoMemory); }
	QueueReceive receive;
	receive.payload.assign(frame.begin() + 8, frame.end());
	receive.lease = m->next++;
	m->receives.emplace(receive.lease, Impl::ReceiveLease {caller, c, index});
	q.reader = receive.lease;
	out = std::move(receive);
	return Result::Success();
}

Result Core::EndQueueReceive(Identity caller, Handle lease, int payload_error, int length_error, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto i = m->receives.find(lease);
	if (i == m->receives.end()) { return Result::Failure(NotFound); }
	if (!(i->second.receiver == caller) || payload_error < 0 || length_error < 0) { return Result::Failure(Invalid); }
	auto c = i->second.client; // pin through lease removal and any completion wakes
	auto& q = c->queues[i->second.index];
	const uint64_t length = q.frames.front().size() - 8;
	Result result;
	if (payload_error) {
		// Abort receive: release ownership without removing the message.
		result = Result::Failure(payload_error == NoMemory ? BadMessage : payload_error);
	} else {
		q.used -= q.frames.front().size(); q.frames.pop_front();
		result = length_error ? Result::Failure(length_error) : Result::Success();
		if (!length_error) { result.received_size = length; }
	}
	q.reader = 0; m->receives.erase(i);
	// Deferred whole-channel cancellation wins over capacity admission on release.
	if (q.deferred_flush) { Impl::Flush(q, now); }
	else { Impl::Advance(q, now); }
	return result;
}

Result Core::PollQueue(Identity caller, Handle client, uint32_t index, uint64_t capacity, Clock::time_point now) {
	QueueReceive receive;
	auto result = BeginQueueReceive(caller, client, index, capacity, receive, now);
	if (result.syscall_errno) { return result; }
	result = EndQueueReceive(caller, receive.lease, 0, 0, now);
	if (result.syscall_errno == Ok) { result.outputs.push_back(std::move(receive.payload)); }
	return result;
}

Result Core::CancelQueueWaiters(Identity owner, Handle session, uint32_t index, Handle tag, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, session); if (!c) { return Result::Failure(NotFound); }
	if (index >= c->queues.size()) { return Result::Failure(Invalid); }
	auto& q = c->queues[index]; auto result = Result::Success();
	// Cancellation is serialized against expiry/commit, not a sticky channel flag.
	for (auto i = q.writers.begin(); i != q.writers.end();) {
		if (i->tag == tag) {
			Finish(i->ticket, Impl::SendResult(*i, Cancelled, now));
			i = q.writers.erase(i); ++result.value;
		} else { ++i; }
	}
	Impl::Advance(q, now); return result;
}

Result Core::FlushQueue(Identity owner, Handle session, uint32_t index, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, session); if (!c) { return Result::Failure(NotFound); }
	if (index >= c->queues.size()) { return Result::Failure(Invalid); }
	auto& q = c->queues[index];
	if (q.reader) { q.deferred_flush = true; }
	else { Impl::Flush(q, now); }
	return Result::Success();
}

void Core::ExpireQueues(Clock::time_point now) {
	std::lock_guard<std::mutex> lock(m->mutex);
	for (auto& c: m->clients) { for (auto& q: c.second->queues) { Impl::Advance(q, now); } }
}

Result Core::InvokeAsync(Identity caller, Handle client, uint32_t method, std::vector<Bytes> inputs, uint32_t& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (inputs.size() > 10) { return Result::Failure(Invalid); }
	uint64_t total = inputs.size() * 4;
	if (total > c->config.async_input_limit) { return Result::Failure(Invalid); }
	for (const auto& b: inputs) {
		if (b.size() > c->config.async_input_limit - total) { return Result::Failure(Invalid); }
		total += b.size();
	}
	auto slot = std::find(c->async_slots.begin(), c->async_slots.end(), nullptr);
	if (slot == c->async_slots.end()) { return Result::Failure(Busy); }
	// Finite firmware counter. Do not overwrite a still-live ticket on wrap.
	for (const auto& r: c->async_slots) {
		if (r && r->async_ticket == c->next_async) { return Result::Failure(Busy); }
	}
	auto request = std::make_shared<Impl::Request>();
	request->caller = caller; request->method = method; request->inputs = std::move(inputs);
	request->async_ticket = c->next_async; request->ticket = MakeTicket(c);
	m->servers.at(c->server)->queue.push_back({Dispatch::Type::AsyncRequest, c, request});
	*slot = request; out = c->next_async;
	if (++c->next_async == 0xffff) { c->next_async = 0; }
	return Result::Success();
}
Result Core::ReplyAsync(Identity owner, Handle session, uint32_t method, uint32_t ticket,
                       int32_t result, std::vector<Bytes> outputs) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(owner, session); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	auto slot = std::find_if(c->async_slots.begin(), c->async_slots.end(), [&](const auto& r) { return r && r->async_ticket == ticket; });
	if (slot == c->async_slots.end()) { return Result::Failure(Invalid); }
	if (!(*slot)->receiver) { return Result::Failure(Invalid); }
	if ((*slot)->ticket->PollStatus()) { return Result::Failure(Busy); }
	if (outputs.size() > 10) { return Result::Failure(Invalid); }
	uint64_t total = outputs.size() * 4;
	if (total > c->config.async_output_limit) { return Result::Failure(Invalid); }
	for (const auto& b: outputs) {
		if (b.size() > c->config.async_output_limit - total) { return Result::Failure(Invalid); }
		total += b.size();
	}
	auto response = Result::Success(); response.method_result = result; response.outputs = std::move(outputs);
	(*slot)->reply_method = method; Finish((*slot)->ticket, std::move(response));
	return Result::Success();
}
Result Core::ReleaseAsyncReceiver(Identity receiver, Handle session) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->SessionFor(receiver, session); if (!c) { return Result::Failure(NotFound); }
	auto i = m->bindings.find(Impl::Key(receiver));
	if (i == m->bindings.end() || i->second.type != Dispatch::Type::AsyncRequest || i->second.client != c) {
		return Result::Failure(Invalid);
	}
	--c->active_receivers; m->bindings.erase(i); return Result::Success();
}
Result Core::CollectAsync(Identity caller, Handle client, uint32_t method, uint32_t ticket,
                         const std::vector<uint64_t>& capacities, Result& out) {
	auto r = BeginAsyncRead(caller, client, method, ticket, capacities, out);
	if (r.syscall_errno) { return r; }
	r = EndAsyncRead(caller, client, ticket, true);
	return r.syscall_errno ? r : ReleaseAsyncTicket(caller, client, ticket);
}
Result Core::BeginAsyncRead(Identity caller, Handle client, uint32_t method, uint32_t ticket,
                           const std::vector<uint64_t>& capacities, Result& out) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	if (c->state != Impl::State::Connected) { return Result::Failure(NotConnected); }
	if (capacities.size() > 10) { return Result::Failure(Invalid); }
	uint64_t total = capacities.size() * 4;
	if (total > c->config.async_output_limit) { return Result::Failure(Invalid); }
	for (auto n: capacities) {
		if (n > c->config.async_output_limit - total) { return Result::Failure(Invalid); }
		total += n;
	}
	auto slot = std::find_if(c->async_slots.begin(), c->async_slots.end(), [&](const auto& r) { return r && r->async_ticket == ticket; });
	if (slot == c->async_slots.end()) { return Result::Failure(Invalid); }
	const auto completion = (*slot)->ticket;
	std::lock_guard<std::mutex> ticket_lock(completion->m->mutex);
	auto& state = completion->m->reply;
	const auto& response = completion->m->result;
	if (!response || state.consumed) { return Result::Failure(WouldBlock); }
	if (state.read_reserved) { return Result::Failure(Busy); }
	if ((*slot)->reply_method != method || response->outputs.size() != capacities.size()) { return Result::Failure(BadMessage); }
	for (size_t n = 0; n < capacities.size(); ++n) {
		if (response->outputs[n].size() > capacities[n]) { return Result::Failure(BadMessage); }
	}
	out = *response; state.read_reserved = true; return Result::Success();
}
Result Core::EndAsyncRead(Identity caller, Handle client, uint32_t ticket, bool consume) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	auto slot = std::find_if(c->async_slots.begin(), c->async_slots.end(), [&](const auto& r) { return r && r->async_ticket == ticket; });
	if (slot == c->async_slots.end()) { return Result::Failure(Invalid); }
	const auto completion = (*slot)->ticket;
	std::lock_guard<std::mutex> ticket_lock(completion->m->mutex);
	auto& state = completion->m->reply;
	if (!state.read_reserved) { return Result::Failure(Invalid); }
	state.read_reserved = false; state.consumed = consume;
	// A forced teardown keeps the reserved slot until this final copy release.
	if (c->state == Impl::State::Terminating) { slot->reset(); }
	return Result::Success();
}
Result Core::ReleaseAsyncTicket(Identity caller, Handle client, uint32_t ticket) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	// Native re-resolves after copyout and still reports success if termination
	// already reset the slot. Connected unknown tickets remain invalid.
	if (c->state == Impl::State::Terminating || c->state == Impl::State::Idle) { return Result::Success(); }
	auto slot = std::find_if(c->async_slots.begin(), c->async_slots.end(), [&](const auto& r) { return r && r->async_ticket == ticket; });
	if (slot == c->async_slots.end()) { return Result::Failure(Invalid); }
	const auto state = (*slot)->ticket->InspectReply();
	if (state.read_reserved || !state.consumed) { return Result::Failure(Busy); }
	slot->reset(); return Result::Success();
}
Result Core::ReleaseAsyncStartOnCopyFault(Identity caller, Handle client, uint32_t ticket) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(caller, client); if (!c) { return Result::Failure(NotFound); }
	for (auto& r: c->async_slots) {
		if (r && r->async_ticket == ticket) { r.reset(); return Result::Success(); }
	}
	return Result::Failure(Invalid);
}
Result Core::ForceTeardown(Identity owner, Handle handle) {
	std::lock_guard<std::mutex> lock(m->mutex);
	auto c = m->ClientFor(owner, handle); if (!c) { return Result::Failure(NotFound); }
	if (c->state == Impl::State::Terminating) { return Result::Success(); }
	if (c->state != Impl::State::Connected && c->state != Impl::State::Disconnecting) { return Result::Failure(Unsupported); }
	auto s = m->servers.at(c->server);
	// A reserved dispatch copy and a running ordinary disconnect callback retain
	// their native storage. The adapter retries after their release; no guessed
	// pending-connect/process-exit transition is exposed here.
	if (s->reader || c->disconnect_receiver) { return Result::Failure(Busy); }
	// Allocate the notification before any externally visible cancellation.
	s->queue.push_back({Dispatch::Type::ForcedDisconnect, c, {}});
	c->state = Impl::State::Terminating; c->disconnector = owner;
	for (auto i = s->queue.begin(); i != s->queue.end();) {
		if (i->client == c && i->type != Dispatch::Type::ForcedDisconnect) { i = s->queue.erase(i); }
		else { ++i; }
	}
	Finish(c->disconnect, Result::Failure(NotConnected));
	for (auto& r: c->slots) {
		if (r) { Finish(r->ticket, Result::Failure(NotConnected)); }
	}
	for (auto& r: c->response_packets) { Finish(r->ticket, Result::Failure(NotConnected)); }
	for (auto& r: c->async_slots) {
		if (r) {
			Finish(r->ticket, Result::Failure(NotConnected));
			if (!r->ticket->InspectReply().read_reserved) { r.reset(); }
		}
	}
	for (auto& e: c->events) {
		for (auto& w: e.waiters) { auto r = Result::Failure(Cancelled); r.value = e.pattern; Finish(w.ticket, r); }
		e.waiters.clear(); e.pattern = 0;
	}
	for (auto& q: c->queues) {
		for (auto& w: q.writers) { Finish(w.ticket, Impl::SendResult(w, Cancelled, Clock::now())); }
		q.writers.clear();
		if (q.reader) { q.deferred_flush = true; } else { Impl::Flush(q, Clock::now()); }
	}
	return Result::Success();
}
} // namespace Libs::LibKernel::Ipmi
