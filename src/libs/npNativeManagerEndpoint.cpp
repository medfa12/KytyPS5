#include "libs/npNativeManagerEndpoint.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>

namespace Libs::NpNativeManagerEndpoint {
namespace I = LibKernel::Ipmi;
namespace R = NpNativeTransport::Retained;
namespace N = NpManagerClient;
namespace {
int Guest(int e) { return e ? static_cast<int>(0x80020000u | unsigned(e)) : 0; }
int Index(uint32_t c) { return c == 4 ? 0 : c == 7 ? 1 : -1; }
uint32_t Word(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
struct Job {
	uint32_t method = 0, channel = 0;
	uint64_t generation = 0;
	bool open = false, notification = false;
	bool bind_request = false, poll_request = false;
	int request = 0;
	uint64_t mask = 0;
	// Single publisher (Advance under State::mutex). After release publication,
	// reply/output are immutable, including across repeated retirement turns.
	std::atomic_bool done {false};
	std::string name;
	std::vector<I::Bytes> inputs;
	std::vector<uint64_t> capacities;
	std::vector<I::Bytes> outputs;
	N::Reply reply;
	I::Clock::time_point deadline;
};
struct QueuedEvent { N::Event event; uint64_t generation; };
}

struct Coordinator::State {
	Hooks hooks;
	mutable std::mutex mutex;
	std::atomic_int retirement_request {0};
	std::chrono::milliseconds timeout;
	I::Clock::time_point connect_deadline;
	I::Identity identity {};
	R::Connection connection = 0;
	bool registered = false, connecting = false, retiring = false, done = false;
	// Native NpManager's +c930 accessor reads a published ready state, not the
	// availability of its worker's mutex. Public queries must remain initialized
	// while Advance owns that mutex to poll IPC or the completion event.
	std::atomic_bool connected {false};
	uintptr_t flag = 0;
	uintptr_t request_flag = 0;
	std::string request_name;
	std::string discovered;
	std::deque<std::shared_ptr<Job>> jobs;
	std::atomic_size_t queued_count {0};
	std::shared_ptr<Job> active;
	std::deque<QueuedEvent> events;
	bool subscribed[2] {}, more[2] {};
	uint64_t generation[2] {1, 1};
	unsigned next_channel = 0;
	void Request(int error) {
		int expected = 0; retirement_request.compare_exchange_strong(expected, error);
	}
	void Revoke(int error = I::NotConnected) {
		Request(error);
		retiring = true; connected = false; events.clear();
		const auto cancel = [error](const std::shared_ptr<Job>& j) {
			if (j && !j->done.load(std::memory_order_relaxed)) {
				j->reply = {Guest(error), N::NotInitialized, 0};
				j->done.store(true, std::memory_order_release);
			}
		};
		for (auto& j: jobs) { cancel(j); }
		jobs.clear();
		queued_count.store(0);
		cancel(active);
	}
	void Invalidate(int i) {
		++generation[i]; subscribed[i] = false; more[i] = false;
		events.erase(std::remove_if(events.begin(), events.end(), [i](const auto& e) {
			return Index(e.event.channel) == i;
		}), events.end());
	}
};

namespace {
class Endpoint final: public N::Endpoint {
	std::shared_ptr<Coordinator> coordinator;
	std::shared_ptr<Coordinator::State> s;
	N::Reply Submit(std::shared_ptr<Job> job, std::span<N::Endpoint::Output> outputs) {
		job->deadline = I::Clock::now() + s->timeout;
		std::unique_lock lock(s->mutex, std::defer_lock);
		// Contention is not a guest IPC failure. Wait only for admission, before
		// publishing this job, using the SAME deadline as completion. No native
		// request is retried, and no lock is held while waiting for the worker.
		while (!lock.try_lock()) {
			if (int error = s->retirement_request.load()) { return {Guest(error), N::NotInitialized, 0}; }
			if (I::Clock::now() >= job->deadline) { return {Guest(I::TimedOut), N::NotInitialized, 0}; }
			std::this_thread::yield();
		}
		if (I::Clock::now() >= job->deadline) { return {Guest(I::TimedOut), N::NotInitialized, 0}; }
		if (!s->connected || s->retiring || s->retirement_request.load()) { return {Guest(I::NotConnected), N::NotInitialized, 0}; }
		if (s->jobs.size() + bool(s->active) >= 8) { return {Guest(I::Busy), N::NotInitialized, 0}; }
		if (!job->open && job->method == 1) { job->generation = s->generation[Index(job->channel)]; }
		s->jobs.push_back(job);
		// Local unsubscribe invalidates even when the service subsequently rejects.
		if (!job->open && job->method == 2) { s->Invalidate(Index(job->channel)); }
		s->queued_count.store(s->jobs.size());
		lock.unlock();
		// Never reacquire the coordinator lock to observe completion or timeout.
		// Only the worker writes results; a timed-out observer leaves its job alone.
		for (;;) {
			if (I::Clock::now() >= job->deadline) {
				s->Request(I::TimedOut);
				return {Guest(I::TimedOut), N::NotInitialized, 0};
			}
			if (int error = s->retirement_request.load()) { return {Guest(error), N::NotInitialized, 0}; }
			if (job->done.load(std::memory_order_acquire)) { break; }
			std::this_thread::yield();
		}
		if (!job->reply.transport_result && !job->reply.service_result) {
			// Complete validated ALL descriptors before publishing done. Only this
			// caller touches public buffers, so timeout/retirement cannot write late.
			for (size_t i = 0; i < job->outputs.size(); ++i) {
				std::copy(job->outputs[i].begin(), job->outputs[i].end(), outputs[i].bytes.begin());
				outputs[i].written = job->outputs[i].size();
			}
		}
		return job->reply;
	}
public:
	Endpoint(std::shared_ptr<Coordinator> c, std::shared_ptr<Coordinator::State> state): coordinator(std::move(c)), s(std::move(state)) {}
	~Endpoint() override { Disconnect(); }
	bool Connected() const noexcept override {
		// An advisory lifecycle snapshot; Submit still checks admission under the
		// mutex. Disconnect/timeout revocation is visible without waiting for it.
		return s->connected.load() && !s->retirement_request.load();
	}
	N::Reply Invoke(uint32_t method, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		const std::array<std::span<const uint8_t>, 1> inputs {input};
		return InvokeInputs(method, input.empty() ? std::span<const std::span<const uint8_t>> {} : inputs, output);
	}
	N::Reply InvokeInputs(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	                      std::span<uint8_t> output) noexcept override {
		std::array<N::Endpoint::Output, 1> outputs {{{output, 0}}};
		return InvokeBuffers(method, inputs, output.empty() ? std::span<N::Endpoint::Output> {} : outputs);
	}
	N::Reply InvokeBuffers(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	                       std::span<N::Endpoint::Output> outputs) noexcept override {
		for (auto& output: outputs) { output.written = 0; }
		try {
			if (inputs.size() > 3 || outputs.size() > 2 || method == 3) {
				return {Guest(I::Unsupported), N::NotInitialized, 0};
			}
			size_t total = 0;
			for (const auto input: inputs) {
				if (input.size() > 0x2000 - total) { return {Guest(I::Unsupported), N::NotInitialized, 0}; }
				total += input.size();
			}
			total = 0;
			for (const auto& output: outputs) {
				if (output.bytes.size() > 0x4000 - total) { return {Guest(I::Unsupported), N::NotInitialized, 0}; }
				total += output.bytes.size();
			}
			auto j = std::make_shared<Job>(); j->method = method;
			if (method >= 1 && method <= 4) {
				if (inputs.size() != 1) { return {Guest(I::Invalid), N::NotInitialized, 0}; }
				const auto input = inputs[0];
				if (input.size() != (method == 1 ? 8u : 4u) || !outputs.empty()) { return {Guest(I::Invalid), N::NotInitialized, 0}; }
				j->channel = Word(input.data());
				if (Index(j->channel) < 0 || (method == 1 && Word(input.data() + 4) != 20)) { return {Guest(I::Invalid), N::NotInitialized, 0}; }
			}
			for (const auto input: inputs) { j->inputs.emplace_back(input.begin(), input.end()); }
			for (const auto& output: outputs) { j->capacities.push_back(output.bytes.size()); }
			return Submit(std::move(j), outputs);
		} catch (...) { coordinator->RequestRetirement(); return {Guest(I::NoMemory), N::NotInitialized, 0}; }
	}
	int OpenEvents(const char* name) noexcept override {
		try {
			if (!name) { return Guest(I::Invalid); }
			size_t n = 0; while (n < 32 && name[n]) { ++n; }
			if (!n || n == 32) { return Guest(I::Invalid); }
			auto j = std::make_shared<Job>(); j->open = true; j->name.assign(name, n);
			return Submit(std::move(j), {}).transport_result;
		} catch (...) { coordinator->RequestRetirement(); return Guest(I::NoMemory); }
	}
	int Poll(N::Event& event) noexcept override {
		std::unique_lock lock(s->mutex, std::try_to_lock);
		if (!lock.owns_lock()) { return 1; }
		if (!s->connected || s->retiring || s->retirement_request.load()) { return Guest(I::NotConnected); }
		while (!s->events.empty()) {
			auto e = s->events.front(); s->events.pop_front();
			int i = Index(e.event.channel);
			if (i >= 0 && s->subscribed[i] && e.generation == s->generation[i]) { event = e.event; return 0; }
		}
		return 1;
	}
	bool RequestEventsAvailable() const noexcept override { return bool(s->hooks.create_request_flag); }
	int BindRequest(int request, uint64_t mask) noexcept override {
		if (!RequestEventsAvailable()) { return N::NotInitialized; }
		if (request <= 0 || !mask || (mask & (mask - 1))) { return N::InvalidArgument; }
		try {
			auto j = std::make_shared<Job>(); j->bind_request = true; j->method = 8; j->request = request; j->mask = mask;
			std::array<uint8_t, 16> binding {};
			std::array<N::Endpoint::Output, 1> outputs {{{binding, 0}}};
			const auto reply = Submit(std::move(j), outputs);
			return reply.transport_result ? reply.transport_result : reply.service_result;
		} catch (...) { coordinator->RequestRetirement(); return Guest(I::NoMemory); }
	}
	int PollRequest(uint64_t mask) noexcept override {
		if (!RequestEventsAvailable()) { return N::NotInitialized; }
		if (!mask || (mask & (mask - 1))) { return N::InvalidArgument; }
		try {
			auto j = std::make_shared<Job>(); j->poll_request = true; j->mask = mask;
			const auto reply = Submit(std::move(j), {});
			return reply.transport_result ? reply.transport_result : reply.service_result;
		} catch (...) { coordinator->RequestRetirement(); return Guest(I::NoMemory); }
	}
	void Disconnect() noexcept override { coordinator->RequestRetirement(); }
};

int Complete(Coordinator::State& s, Job& j, I::Result& r) {
	if (r.syscall_errno) { return r.syscall_errno; }
	if (!r.method_result || (r.operation_result && *r.operation_result != 0)) { return I::BadMessage; }
	j.reply = {0, *r.method_result, 0};
	// Method5 returns its positive request ID in method_result. Validate output
	// shape on that successful path as well as on conventional zero-success.
	if (j.reply.service_result && !(j.method == 5 && j.reply.service_result > 0)) {
		return j.notification ? I::BadMessage : 0;
	}
	if (r.outputs.size() != j.capacities.size()) { return I::BadMessage; }
	for (size_t i = 0; i < r.outputs.size(); ++i) { if (r.outputs[i].size() > j.capacities[i]) { return I::BadMessage; } }
	if (j.bind_request) {
		if (r.outputs.size() != 1 || r.outputs[0].size() != 16) { return I::BadMessage; }
		uint64_t alias = 0, mask = 0;
		std::memcpy(&alias, r.outputs[0].data(), 8);
		std::memcpy(&mask, r.outputs[0].data() + 8, 8);
		if (!alias || mask != j.mask) { return I::BadMessage; }
	}
	if (j.notification) {
		// Retained outputs contain actual written bytes, not zero-filled capacities.
		if (r.outputs[0].size() != 0x1e8 || r.outputs[1].size() != 4) { return I::BadMessage; }
		uint32_t count = Word(r.outputs[0].data()), remaining = Word(r.outputs[1].data());
		if (count > 20 || remaining > INT32_MAX || (!count && remaining)) { return I::BadMessage; }
		int i = Index(j.channel);
		if (!s.subscribed[i] || s.generation[i] != j.generation) { return 0; }
		s.more[i] = remaining != 0;
		for (uint32_t n = 0; n < count; ++n) {
			N::Event e; e.channel = j.channel; e.size = 20;
			std::copy_n(r.outputs[0].data() + 8 + n * 24, 20, e.payload.begin());
			s.events.push_back({e, j.generation});
		}
	} else {
		if (j.method == 0) {
			if (r.outputs.size() != 1 || r.outputs[0].size() != 32) { return I::BadMessage; }
			const auto& output = r.outputs[0];
			auto end = std::find(output.begin(), output.end(), 0);
			if (end == output.end() || end == output.begin()) { return I::BadMessage; }
			s.discovered.assign(output.begin(), end);
		}
		j.outputs = std::move(r.outputs);
		for (const auto& output: j.outputs) { j.reply.written += output.size(); }
		if (j.method == 1) {
			int i = Index(j.channel);
			if (s.generation[i] == j.generation) { s.Invalidate(i); s.subscribed[i] = true; }
		}
	}
	return 0;
}
}

Coordinator::Coordinator(std::shared_ptr<State> state): m(std::move(state)) {}
Coordinator::~Coordinator() = default; // runtime registry owns until Done()
void Coordinator::RequestRetirement() noexcept { m->Request(I::NotConnected); }
int Coordinator::RetirementError() const noexcept { return m->retirement_request.load(); }
bool Coordinator::Done() const noexcept { std::lock_guard lock(m->mutex); return m->done; }
size_t Coordinator::QueuedJobs() const noexcept {
	return m->queued_count.load();
}

int Coordinator::Advance() noexcept {
	std::lock_guard lock(m->mutex);
	auto& s = *m;
	try {
		if (s.done) { return 0; }
		if (int error = s.retirement_request.load()) { s.Revoke(error); }
		if (!s.hooks.owner_alive()) { s.Revoke(); return I::NotConnected; }
		auto current = s.hooks.identity();
		if (current.pid <= 0 || current.tid <= 0 || (s.registered && !(current == s.identity))) {
			s.Revoke(); return I::Invalid;
		}
		const auto now = I::Clock::now();
		if (int error = s.retirement_request.load()) { s.Revoke(error); }
		if ((!s.connected && now >= s.connect_deadline) || (s.active && now >= s.active->deadline) ||
		    (!s.jobs.empty() && now >= s.jobs.front()->deadline)) { s.Revoke(I::TimedOut); }
		if (s.retiring) {
			// Keep the flag alias on close failure; native ticket retirement proceeds
			// independently. Neither failed close nor lost owner permits Done().
			int close = s.flag ? s.hooks.close_flag(s.flag) : 0;
			if (!close) { s.flag = 0; }
			int r = s.connection ? R::Retire(s.connection) : 0;
			if (!r || r == I::NotFound) { s.connection = 0; s.active.reset(); }
			// Server aliases must drain before deleting the creator. Failed deletion
			// remains in the durable registry and retries on the same owner worker.
			if (!s.connection && s.request_flag && !s.hooks.delete_request_flag(s.request_flag)) { s.request_flag = 0; }
			s.done = !s.connection && !s.flag && !s.request_flag;
			return s.done ? 0 : (r ? r : I::Busy);
		}
		if (!s.registered) {
			int r = R::Register(s.connection);
			if (r) { s.Revoke(r); return r; }
			s.identity = current; s.registered = true;
			r = R::BeginConnect(s.connection);
			if (r) { s.Revoke(r); return r; }
			s.connecting = true; return I::WouldBlock;
		}
		if (s.connecting) {
			int32_t accepted = 0; int r = R::PollConnect(s.connection, accepted);
			if (r == I::WouldBlock || r == I::Busy) { return r; }
			if (r || accepted) { s.Revoke(r ? r : I::NotConnected); return r ? r : I::NotConnected; }
			s.connecting = false; s.connected = true; return 0;
		}
		if (s.active) {
			int r = R::PollInvoke(s.connection);
			if (r == I::WouldBlock || r == I::Busy) { return r; }
			if (r) { s.Revoke(r); return r; }
			I::Result result; r = R::ConsumeInvoke(s.connection, result);
			if (r == I::WouldBlock || r == I::Busy) { return r; }
			if (!r) { r = Complete(s, *s.active, result); }
			if (r) { s.Revoke(r); return r; }
			s.active->done.store(true, std::memory_order_release); s.active.reset(); return 0;
		}
		std::shared_ptr<Job> j;
		if (!s.jobs.empty()) { j = s.jobs.front(); s.jobs.pop_front(); s.queued_count.store(s.jobs.size()); }
		else if (s.flag && s.events.size() <= 20) {
			for (unsigned n = 0; n < 2; ++n) {
				unsigned i = (s.next_channel + n) % 2; uint32_t channel = i ? 7 : 4;
				if (!s.subscribed[i]) { continue; }
				int r = s.more[i] ? 0 : s.hooks.poll_flag(s.flag, channel);
				if (r == 1) { continue; }
				if (r) { s.Revoke(I::BadMessage); return I::BadMessage; }
				j = std::make_shared<Job>(); j->notification = true; j->method = 3;
				j->channel = channel; j->generation = s.generation[i];
				j->inputs = {I::Bytes(4)}; std::memcpy(j->inputs[0].data(), &channel, 4);
				j->capacities = {0x1e8, 4}; j->deadline = now + s.timeout;
				s.next_channel = (i + 1) % 2; break;
			}
		}
		if (!j) { return 0; }
		// Retain even local jobs before hooks/allocation, so exceptions revoke the
		// observer instead of stranding an already-dequeued job.
		s.active = j;
		if (j->open) {
			int r = s.flag ? Guest(I::Busy) : j->name != s.discovered ? Guest(I::Invalid) : s.hooks.open_flag(j->name.c_str(), s.flag);
			if (!r && !s.flag) { r = Guest(I::BadMessage); }
			j->reply = {r, 0, 0}; j->done.store(true, std::memory_order_release);
			if (r && s.flag) { s.Revoke(); }
			s.active.reset();
			return 0;
		}
		if (j->poll_request) {
			const int r = s.request_flag ? s.hooks.poll_request_flag(s.request_flag, j->mask) : N::NotInitialized;
			j->reply = {0, r, 0}; j->done.store(true, std::memory_order_release); s.active.reset();
			return 0;
		}
		if (j->bind_request) {
			int r = 0;
			if (!s.request_flag) {
				char name[32] {};
				std::snprintf(name, sizeof(name), "SceNpMgrIpcC%08x", unsigned(s.identity.pid));
				s.request_name = name;
				r = s.hooks.create_request_flag(name, s.request_flag);
				if (!r && !s.request_flag) { r = Guest(I::BadMessage); }
				if (r && s.request_flag) { s.Revoke(); return I::BadMessage; }
			}
			if (!r) { r = s.hooks.clear_request_flag(s.request_flag, ~j->mask); }
			if (r) {
				j->reply = {r, N::NotInitialized, 0}; j->done.store(true, std::memory_order_release); s.active.reset(); return 0;
			}
			j->inputs = {I::Bytes(16), I::Bytes(s.request_name.begin(), s.request_name.end())};
			j->inputs[1].push_back(0);
			std::memcpy(j->inputs[0].data(), &j->request, 4);
			std::memcpy(j->inputs[0].data() + 8, &j->mask, 8);
			j->capacities = {16};
		}
		int r = R::BeginInvoke(s.connection, j->method, j->inputs, j->capacities);
		if (r) { s.Revoke(r); return r; }
		return I::WouldBlock;
	} catch (...) { s.Revoke(I::NoMemory); return I::NoMemory; }
}

int Create(Hooks hooks, std::shared_ptr<N::Endpoint>& endpoint, std::shared_ptr<Coordinator>& coordinator,
           std::chrono::milliseconds timeout) noexcept {
	if (!hooks.owner || !hooks.identity || !hooks.owner_alive || !hooks.retain || !hooks.open_flag ||
	    !hooks.poll_flag || !hooks.close_flag || timeout.count() < 1 || timeout.count() > 1000) { return I::Invalid; }
	const unsigned request_hooks = bool(hooks.create_request_flag) + bool(hooks.clear_request_flag) +
	    bool(hooks.poll_request_flag) + bool(hooks.delete_request_flag);
	if (request_hooks != 0 && request_hooks != 4) { return I::Invalid; }
	try {
		auto s = std::make_shared<Coordinator::State>(); s->hooks = std::move(hooks); s->timeout = timeout;
		s->connect_deadline = I::Clock::now() + timeout;
		auto c = std::shared_ptr<Coordinator>(new Coordinator(s));
		auto e = std::make_shared<Endpoint>(c, s);
		if (!s->hooks.retain(c)) { return I::Busy; }
		endpoint = std::move(e); coordinator = std::move(c); return 0;
	} catch (...) { return I::NoMemory; }
}
} // namespace Libs::NpNativeManagerEndpoint
