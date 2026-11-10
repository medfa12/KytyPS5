#include "libs/npManagerService.h"

#include "common/threads.h"
#include "common/logging/log.h"
#include "kernel/eventFlag.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>

namespace Libs::NpManagerService {
namespace {
namespace Np = NpManagerClient;
namespace Flags = LibKernel::EventFlag;
constexpr int NoService = static_cast<int>(0x80020002u);
constexpr int NotConnected = static_cast<int>(0x80020039u);
constexpr int Unsupported = static_cast<int>(0x8002004eu);
constexpr int Invalid = static_cast<int>(0x80020016u);
constexpr int NoMemory = static_cast<int>(0x8002000cu);
constexpr int Busy = static_cast<int>(0x80020010u);
constexpr int InvalidRequest = static_cast<int>(0x80559d04u);
constexpr int InvalidSubscription = static_cast<int>(0x80559d03u);
constexpr size_t QueueCapacity = 20;
constexpr size_t BatchSize = 0x1e8;
using Record = std::array<uint8_t, 24>;

struct Queue {
	bool subscribed = false;
	std::array<Record, QueueCapacity> records {};
	size_t first = 0, count = 0;
};
struct Connection {
	bool connected = false;
	void* guest = nullptr;
	std::array<char, 32> name {};
	Flags::KernelEventFlag creator = nullptr;
	Flags::KernelEventFlag alias = nullptr;
	std::array<Queue, 2> queues {};
	// Adapter-owned fetched records. Retained across bounded callback pumps.
	std::array<Record, QueueCapacity> batch {};
	size_t next = 0, count = 0;
	uint32_t batch_channel = 0, more = 0;
};
struct Broker {
	std::mutex mutex;
	bool running = false;
	uint64_t next_id = 1;
	std::array<std::shared_ptr<Connection>, 16> clients {};
};
Broker& State() { static Broker broker; return broker; }

// Nonblocking kernel flag operations do not dispatch guest callbacks. Protect
// these short host transitions from guest-thread suspension as well as races.
struct Lock {
	std::optional<Common::HostLockCriticalGuard> critical;
	std::unique_lock<std::mutex> lock {State().mutex, std::defer_lock};
	int error = 0;
	Lock() noexcept {
		try { critical.emplace(); lock.lock(); }
		catch (const std::bad_alloc&) { error = NoMemory; }
		catch (const std::system_error&) { error = Np::TransportIoError; }
	}
};

int Index(uint32_t channel) { return channel == 4 ? 0 : channel == 7 ? 1 : -1; }

int RetireLocked(const std::shared_ptr<Connection>& c) {
	if (!c) { return 0; }
	c->connected = false;
	int result = 0;
	if (c->alias) {
		result = Flags::KernelCloseEventFlag(c->alias);
		if (result == 0) { c->alias = nullptr; }
	}
	if (c->creator) {
		const int deleted = Flags::KernelDeleteEventFlag(c->creator);
		if (deleted == 0) { c->creator = nullptr; }
		if (result == 0) { result = deleted; }
	}
	c->queues = {};
	c->next = c->count = c->more = 0;
	// A failed kernel cleanup must retain its handle and broker slot for retry.
	// Revocation is immediate, but cannot be mistaken for resource retirement.
	if (!c->alias && !c->creator) {
		for (auto& slot: State().clients) { if (slot == c) { slot.reset(); break; } }
	}
	return result;
}

int AcceptLocked(void* guest, std::shared_ptr<Connection>& out) {
	auto& broker = State();
	if (!broker.running) { return NoService; }
	const auto slot = std::find(broker.clients.begin(), broker.clients.end(), nullptr);
	if (slot == broker.clients.end()) { return Busy; }
	if (broker.next_id == UINT64_MAX) { return Busy; }
	auto candidate = std::make_shared<Connection>();
	candidate->guest = guest;
	std::snprintf(candidate->name.data(), candidate->name.size(), "KytyNpMgr%016" PRIx64, broker.next_id++);
	// ShellCore cd36e4..cd36f3 creates the per-session named event flag with 0x121.
	const int result = Flags::KernelCreateEventFlag(&candidate->creator, candidate->name.data(), 0x121, 0, nullptr);
	if (result != 0) { return result; }
	candidate->connected = true;
	*slot = candidate;
	out = std::move(candidate);
	return 0;
}

std::shared_ptr<Connection> GuestLocked(void* guest) {
	if (!guest) { return {}; }
	for (const auto& c: State().clients) { if (c && c->guest == guest) { return c; } }
	return {};
}

// Synchronous host dispatch. Transport and method errors remain separate.
// All descriptors are validated before queue changes or successful output writes.
int InvokeLocked(Connection& c, uint32_t method, std::span<const DataInfo> in,
                 std::span<BufferInfo> out, int& service) {
	if (!c.connected) { return NotConnected; }
	for (auto& b: out) { b.written = 0; }
	service = InvalidRequest;
	const auto input = [&](size_t size) {
		return size == 0 ? in.empty() : in.size() == 1 && in[0].data && in[0].size == size;
	};
	const auto output = [&](size_t index, size_t size) {
		return index < out.size() && out[index].buf && out[index].capacity >= size;
	};
	if (method == 0) {
		if (!input(0) || out.size() != 1 || !output(0, 32)) { return 0; }
		std::memcpy(out[0].buf, c.name.data(), 32);
		out[0].written = 32;
		service = 0;
		return 0;
	}
	if (method == 0x13 || method == 0x38) {
		if (!input(method == 0x13 ? 8 : 4) || out.size() != 1 || !output(0, method == 0x13 ? 20 : 4)) { return 0; }
		// A working local service transport is not a working account backend.
		service = Np::NotInitialized;
		return 0;
	}
	if (method < 1 || method > 4) { service = Unsupported; return 0; }
	if (!input(method == 1 ? 8 : 4)) { return 0; }
	uint32_t channel = 0;
	std::memcpy(&channel, in[0].data, 4);
	const int index = Index(channel);
	if (index < 0) { return 0; }
	auto& queue = c.queues[index];
	if (method == 1) {
		uint32_t capacity = 0;
		std::memcpy(&capacity, static_cast<const uint8_t*>(in[0].data) + 4, 4);
		if (!out.empty() || capacity != QueueCapacity) { return 0; }
		if (queue.subscribed) { service = InvalidSubscription; return 0; }
		queue = {};
		queue.subscribed = true;
		service = 0;
	} else if (method == 2) {
		if (!out.empty()) { return 0; }
		if (!queue.subscribed) { service = InvalidSubscription; return 0; }
		queue = {};
		c.more &= ~(1u << channel);
		// A previously fetched record belongs to the retired subscription too.
		if (c.batch_channel == channel) { c.next = c.count = 0; }
		service = 0;
	} else if (method == 3) {
		if (out.size() != 2 || !output(0, BatchSize) || !output(1, 4)) { return 0; }
		if (!queue.subscribed) { service = InvalidSubscription; return 0; }
		std::array<uint8_t, BatchSize> batch {};
		const uint32_t count = static_cast<uint32_t>(queue.count);
		std::memcpy(batch.data(), &count, 4);
		for (size_t i = 0; i < queue.count; ++i) {
			std::memcpy(batch.data() + 8 + i * 24, queue.records[(queue.first + i) % QueueCapacity].data(), 24);
		}
		const uint32_t remaining = 0;
		std::memcpy(out[0].buf, batch.data(), batch.size());
		std::memcpy(out[1].buf, &remaining, 4);
		out[0].written = batch.size(); out[1].written = 4;
		// cd4050 copies/unlinks each record; no later acknowledgment is required.
		queue.first = queue.count = 0;
		service = 0;
	} else {
		if (!out.empty()) { return 0; }
		// cd29e0 -> cd3ea0 -> EventFlag::Set. NOT a dequeue acknowledgment.
		service = Flags::KernelSetEventFlag(c.creator, uint64_t {1} << channel);
	}
	return 0;
}

class Endpoint final: public Np::Endpoint {
public:
	std::shared_ptr<Connection> connection;
	~Endpoint() override { Disconnect(); }
	bool Connected() const noexcept override {
		Lock lock;
		if (lock.error) { return false; }
		return connection && connection->connected;
	}
	Np::Reply Invoke(uint32_t method, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		Lock lock;
		if (lock.error) { return {lock.error, Np::NotInitialized, 0}; }
		if (!connection) { return {NotConnected, Np::NotInitialized, 0}; }
		DataInfo in {input.data(), input.size()};
		BufferInfo out {output.data(), output.size(), 0};
		int service = Np::NotInitialized;
		const int transport = InvokeLocked(*connection, method,
		    input.empty() ? std::span<const DataInfo> {} : std::span<const DataInfo> {&in, 1},
		    output.empty() ? std::span<BufferInfo> {} : std::span<BufferInfo> {&out, 1}, service);
		return {transport, service, static_cast<size_t>(out.written)};
	}
	int OpenEvents(const char* name) noexcept override {
		Lock lock;
		if (lock.error) { return lock.error; }
		if (!connection || !connection->connected) { return NotConnected; }
		if (!name || std::strcmp(name, connection->name.data()) != 0) { return Invalid; }
		if (connection->alias) { return Busy; }
		return Flags::KernelOpenEventFlag(&connection->alias, name);
	}
	int Poll(Np::Event& event) noexcept override {
		Lock lock;
		if (lock.error) { return lock.error; }
		if (!connection || !connection->connected) { return NotConnected; }
		auto& c = *connection;
		if (!c.alias) { return Np::NotInitialized; }
		if (c.next == c.count) {
			c.next = c.count = 0;
			for (uint32_t channel: {4u, 7u}) {
				if (!c.queues[Index(channel)].subscribed) { continue; }
				const uint32_t bit = 1u << channel;
				if (!(c.more & bit)) {
					uint64_t observed = 0;
					const int result = Flags::KernelPollEventFlag(c.alias, bit, 0x22, &observed);
					if (result == Busy) { continue; }
					if (result != 0) { return result; }
				}
				std::array<uint8_t, BatchSize> batch {};
				int32_t remaining = 0, service = Np::NotInitialized;
				DataInfo in {&channel, 4};
				BufferInfo out[2] {{batch.data(), batch.size(), 0}, {&remaining, 4, 0}};
				const int result = InvokeLocked(c, 3, {&in, 1}, out, service);
				if (result != 0 || service != 0) { return result != 0 ? result : service; }
				int32_t count = 0;
				std::memcpy(&count, batch.data(), 4);
				if (out[0].written != BatchSize || out[1].written != 4 || count < 0 || count > 20 || remaining < 0) {
					return Np::TransportIoError;
				}
				c.more = (c.more & ~bit) | (remaining > 0 ? bit : 0);
				for (int i = 0; i < count; ++i) { std::memcpy(c.batch[i].data(), batch.data() + 8 + i * 24, 24); }
				c.count = static_cast<size_t>(count);
				c.batch_channel = channel;
				if (count != 0) { break; }
			}
		}
		if (c.next == c.count) { return 1; }
		event.channel = c.batch_channel;
		event.size = event.payload.size();
		std::copy_n(c.batch[c.next++].begin(), event.payload.size(), event.payload.begin());
		return 0;
	}
	void Disconnect() noexcept override {
		Lock lock;
		if (lock.error) { return; } // broker retains the connection for lifecycle StopService retry
		RetireLocked(connection);
		connection.reset();
	}
};
} // namespace

int StartUnavailableService() noexcept {
	{
		Lock lock;
		if (lock.error) { return lock.error; }
		if (State().running || std::any_of(State().clients.begin(), State().clients.end(),
		    [](const auto& c) { return c != nullptr; })) { return Busy; }
		State().running = true;
	}
	LOGF("NP broker: registered HLE SceNpMgrIpc; account backend unavailable; no native service acceptance\n");
	return 0;
}

int StopService() noexcept {
	Lock lock;
	if (lock.error) { return lock.error; }
	State().running = false;
	int result = 0;
	for (auto& slot: State().clients) {
		const auto retained = slot;
		const int retired = RetireLocked(retained);
		if (result == 0) { result = retired; }
	}
	return result;
}

bool Running() noexcept { Lock lock; return !lock.error && State().running; }

int ConnectEndpoint(std::shared_ptr<Np::Endpoint>& output) noexcept {
	try {
		auto endpoint = std::make_shared<Endpoint>();
		int result;
		{
			Lock lock;
			if (lock.error) { return lock.error; }
			result = AcceptLocked(nullptr, endpoint->connection);
		}
		if (result != 0) { return result; }
		output = std::move(endpoint);
		return 0;
	} catch (const std::bad_alloc&) { return NoMemory; }
}

int ConnectGuest(void* client, const void* argument, uint64_t size, int* acceptance) noexcept {
	if (!client || argument || size != 0 || !acceptance) { return Invalid; }
	try {
		int result;
		{
			Lock lock;
			if (lock.error) { return lock.error; }
			if (GuestLocked(client)) { return Busy; }
			std::shared_ptr<Connection> connection;
			result = AcceptLocked(client, connection);
		}
		if (result == 0) { *acceptance = 0; }
		return result;
	} catch (const std::bad_alloc&) { return NoMemory; }
}

int InvokeGuest(void* client, uint32_t method, const DataInfo* input, uint32_t input_count,
                int* service_result, BufferInfo* output, uint32_t output_count) noexcept {
	if (!service_result || input_count > 1 || output_count > 2 || (input_count && !input) || (output_count && !output)) {
		return Invalid;
	}
	// Snapshot guest descriptors/bytes before taking the broker lock. No guest
	// memory access, fault recovery, or guest callback occurs inside dispatch.
	DataInfo saved_input {};
	std::array<uint8_t, 8> input_bytes {};
	if (input_count) {
		std::memcpy(&saved_input, input, sizeof(saved_input));
		if (saved_input.size > input_bytes.size() || (saved_input.size && !saved_input.data)) { return Invalid; }
		if (saved_input.size) { std::memcpy(input_bytes.data(), saved_input.data, saved_input.size); }
		saved_input.data = input_bytes.data();
	}
	std::array<BufferInfo, 2> saved_output {}, local_output {};
	std::array<std::array<uint8_t, BatchSize>, 2> output_bytes {};
	for (size_t i = 0; i < output_count; ++i) {
		std::memcpy(&saved_output[i], output + i, sizeof(BufferInfo));
		if (saved_output[i].capacity && !saved_output[i].buf) { return Invalid; }
		local_output[i] = {output_bytes[i].data(), (std::min)(saved_output[i].capacity, uint64_t {BatchSize}), 0};
	}
	int method_result = Np::NotInitialized;
	int result;
	{
		Lock lock;
		if (lock.error) { return lock.error; }
		const auto c = GuestLocked(client);
		if (!c) { return NotConnected; }
		result = InvokeLocked(*c, method, {&saved_input, input_count}, {local_output.data(), output_count}, method_result);
	}
	if (result != 0) { return result; }
	for (size_t i = 0; i < output_count; ++i) {
		if (method_result == 0 && local_output[i].written) {
			std::memcpy(saved_output[i].buf, output_bytes[i].data(), local_output[i].written);
		}
		output[i].written = local_output[i].written;
	}
	*service_result = method_result;
	return 0;
}

int DisconnectGuest(void* client) noexcept {
	Lock lock;
	if (lock.error) { return lock.error; }
	const auto c = GuestLocked(client);
	if (!c) { return NotConnected; }
	return RetireLocked(c);
}

int PublishEvent(uint32_t channel, std::span<const uint8_t, 24> record) noexcept {
	const int index = Index(channel);
	if (index < 0) { return InvalidRequest; }
	Lock lock;
	if (lock.error) { return lock.error; }
	if (!State().running) { return NoService; }
	for (const auto& c: State().clients) {
		if (!c || !c->queues[index].subscribed) { continue; }
		auto& q = c->queues[index];
		const int result = Flags::KernelSetEventFlag(c->creator, uint64_t {1} << channel);
		if (result != 0) { return result; }
		// cd3f10 drops the oldest record when the subscription's queue is full.
		if (q.count == QueueCapacity) { q.first = (q.first + 1) % QueueCapacity; --q.count; }
		std::copy(record.begin(), record.end(), q.records[(q.first + q.count) % QueueCapacity].begin());
		++q.count;
	}
	return 0;
}
} // namespace Libs::NpManagerService
