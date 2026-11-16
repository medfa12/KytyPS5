#pragma once

// Isolated, opt-in state model. No guest memory access, factory routing or syscall
// registration. Handles and structures here are NOT the firmware wire ABI.
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Libs::LibKernel::Ipmi {

using Handle = uint64_t;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
struct Identity {
	int32_t pid;
	int32_t tid;
	bool operator==(const Identity& other) const { return pid == other.pid && tid == other.tid; }
};

// BSD error values used by the verified paths. A future adapter must separately
// implement libkernel's errno convention and copyin/copyout error precedence.
enum Error : int { Ok = 0, NotFound = 2, NotConnected = 3, Interrupted = 4,
	NoMemory = 0x0c, Busy = 0x10, Invalid = 0x16, WouldBlock = 0x23,
	TimedOut = 0x3c, Unsupported = 0x4e, Cancelled = 0x55, BadMessage = 0x59 };
struct Result {
	int syscall_errno = Ok;
	std::optional<int32_t> operation_result;
	std::optional<int32_t> method_result;
	uint64_t value = 0; // event snapshot or newly created handle, on documented paths
	std::vector<Bytes> outputs;
	std::optional<uint64_t> received_size; // queue252: written only after consumption and successful length copy
	std::optional<uint32_t> remaining_us; // queue253 timeout in/out, absent for indefinite waits
	static Result Success();
	static Result Failure(int error);
};

class Completion final {
public:
	struct ReplyState {
		bool published = false, read_reserved = false, consumed = false;
		bool caller_released = false, receiver_released = false;
	};
	Completion();
	~Completion();
	std::optional<Result> Poll() const;
	// Completion metadata only; never copies/allocates output payloads. Used by
	// syscall waits before entering the guarded response-consumption phase.
	std::optional<Result> PollStatus() const;
	// A host wait slice only; never invokes guest callbacks or changes transport state.
	std::optional<Result> WaitFor(std::chrono::milliseconds slice) const;
	// Observation only; availability is not consumption or caller completion.
	ReplyState InspectReply() const;
	// Diagnostic snapshot of an unconsumed zero/one-output native type42 packet,
	// not a retry API. Multi-output wire packing is unverified (empty snapshot).
	Bytes InspectReplyPacket() const;
private:
	struct Impl;
	std::unique_ptr<Impl> m;
	friend class Core;
};
using Ticket = std::shared_ptr<Completion>;

struct Config {
	uint32_t sync_slots = 1;
	uint64_t input_limit = 4096;
	uint64_t output_limit = 4096;
	uint32_t event_count = 1;
	uint32_t async_slots = 0;
	uint32_t queue_count = 0;
	// Host channel byte capacities INCLUDING each message's 8-byte header.
	// Not native Config fields: the registration adapter must translate separately.
	std::vector<uint64_t> queue_capacities;
	uint64_t async_input_limit = 0, async_output_limit = 0;
	uint64_t native_object = 0, user_argument = 0;
	uint32_t sync_flag = 0;
};
struct ServerConfig {
	uint64_t native_object = 0, handler = 0, user_argument = 0;
	uint64_t argument_limit = 4096, transfer_limit = 2 * 1024 * 1024;
	uint32_t multi = 0, cross_no_cap = 0, restricted = 0;
};
enum class ObjectKind { Server, Client, Session };
struct ObjectInfo {
	uint64_t native_object = 0, user_argument = 0, handler = 0;
	std::string name;
};
struct ConnectionInfo {
	Handle server = 0;
	int32_t server_pid = 0;
	ServerConfig server_config;
	Config client_config;
};
struct QueueReceive {
	Handle lease = 0; // host-only transaction, bound to receiving guest PID/TID
	Bytes payload;
};
struct Dispatch {
	enum class Type { Connect = 1, Disconnect = 3, Request = 0x41, AsyncRequest = 0x43, ForcedDisconnect = 0xa0, Wake = 0xb0 };
	Type type = Type::Wake;
	Handle client = 0;
	Handle session = 0;
	Identity caller {};
	uint64_t handler = 0;
	uint64_t native_session = 0;
	uint64_t argument = 0;
	uint32_t method = 0;
	uint32_t async_ticket = 0;
	Config config {};
	std::vector<Bytes> inputs;
	std::vector<uint64_t> output_capacities;
	Bytes connect_argument;
};

class Core final {
public:
	Core();
	~Core();
	Core(const Core&) = delete;
	Core& operator=(const Core&) = delete;
	Result RegisterServer(Identity owner, const std::string& name, uint64_t handler, Handle& out);
	Result RegisterServer(Identity owner, const std::string& name, const ServerConfig& config, Handle& out, bool published = true);
	// Factories register provisionally, complete native copyout, then publish.
	// Only publication makes a server discoverable and wakes name waiters.
	Result PublishServer(Identity owner, Handle server);
	Result RegisterClient(Identity owner, const std::string& name, const Config& config, Handle& out);
	Result Name(Identity owner, Handle handle, std::array<char, 25>& out) const;
	// Diagnostic lookup only: live server kind, exact non-reused handle, no
	// owner-PID restriction. Missing/stale/wrong-kind handles yield an empty name.
	std::array<char, 25> TraceServerName(Handle handle) const;
	Result Inspect(Identity owner, Handle handle, ObjectKind kind, ObjectInfo& out) const;
	Result SessionPeerPid(Identity owner, Handle session, int32_t& out) const;
	Result Connection(Identity caller, Handle client, ConnectionInfo& out, Handle expected_server = 0) const;
	// Host-only prerequisite for indefinite connectWithWaitServer. Completion
	// carries a server generation in value, NOT a session or acceptance result.
	// Retains exclusive connect admission until Connect consumes this exact ticket
	// or AbandonServerWait releases it. No guest timeout/error ABI is inferred here.
	Result WaitServer(Identity caller, Handle client, Ticket& out);
	Result AbandonServerWait(Identity caller, Handle client, const Ticket& ticket);
	Result Connect(Identity caller, Handle client, Ticket& out, Bytes argument = {}, Handle expected_server = 0,
	               const Ticket& server_wait = {});
	Result Receive(Identity receiver, Handle server, Dispatch& out);
	// Reserve the queue head across unlocked adapter copies. Abort releases the
	// reservation without consuming or binding. These are host transactions only.
	Result BeginReceive(Identity receiver, Handle server, Dispatch& out);
	Result EndReceive(Identity receiver, Handle server, bool consume);
	Result Wake(Identity owner, Handle server);
	// Adapter shutdown gate: atomically refuse new connects only after all
	// attached clients/dispatch reservations drain. Busy retains the service.
	Result SealServer(Identity owner, Handle server);
	Result CreateSession(Identity receiver, uint64_t native_object, uint64_t argument, Handle& out, Handle expected_server = 0);
	Result Accept(Identity receiver, Handle client, int32_t result);
	Result Invoke(Identity caller, Handle client, uint32_t method, std::vector<Bytes> inputs,
	              std::vector<uint64_t> output_capacities, Ticket& out);
	Result Reply(Identity receiver, Handle session, int32_t method_result, std::vector<Bytes> outputs);
	// Separate native publication and worker release. Reply is their convenience
	// composition; neither releases the invoking thread's synchronous slot.
	Result PublishReply(Identity receiver, Handle session, int32_t method_result, std::vector<Bytes> outputs);
	Result ReleaseReplyReceiver(Identity receiver, Handle session);
	// Host dispatch-boundary failure, never a synthesized service reply.
	Result AbortRequest(Identity receiver, Handle session, int error);
	Result BeginInvokeRead(Identity caller, Handle client, const Ticket& ticket, Result& response);
	Result EndInvokeRead(Identity caller, Handle client, const Ticket& ticket, bool consume);
	Result ReleaseInvokeCaller(Identity caller, Handle client, const Ticket& ticket);
	// Read-only host observation of an occupied caller slot; no guest routing.
	Result InspectInvoke(Identity caller, Handle client, Ticket& out) const;
	// Typed async transport. Ticket zero is valid; successful collection consumes
	// one connection-scoped ticket. Pending/unknown are WouldBlock/Invalid.
	Result InvokeAsync(Identity caller, Handle client, uint32_t method, std::vector<Bytes> inputs, uint32_t& out);
	Result ReplyAsync(Identity owner, Handle session, uint32_t method, uint32_t ticket,
	                  int32_t result, std::vector<Bytes> outputs);
	Result ReleaseAsyncReceiver(Identity receiver, Handle session);
	Result CollectAsync(Identity caller, Handle client, uint32_t method, uint32_t ticket,
	                    const std::vector<uint64_t>& capacities, Result& out);
	Result BeginAsyncRead(Identity caller, Handle client, uint32_t method, uint32_t ticket,
	                      const std::vector<uint64_t>& capacities, Result& out);
	Result EndAsyncRead(Identity caller, Handle client, uint32_t ticket, bool consume);
	Result ReleaseAsyncTicket(Identity caller, Handle client, uint32_t ticket);
	// Start publication succeeded but ticket copyout failed. Native clears the
	// slot without undoing an already queued/dispatched request.
	Result ReleaseAsyncStartOnCopyFault(Identity caller, Handle client, uint32_t ticket);
	Result Disconnect(Identity caller, Handle client, Ticket& out);
	Result CloseSession(Identity owner, Handle session);
	Result AcknowledgeDisconnect(Identity receiver, Handle client, int32_t result);
	Result DestroyClient(Identity owner, Handle client);
	Result DestroyServer(Identity owner, Handle server);
	Result PollEvent(Identity caller, Handle client, uint32_t index, uint64_t bits, uint32_t mode);
	Result WaitEvent(Identity caller, Handle client, uint32_t index, uint64_t bits, uint32_t mode,
	                 std::optional<std::chrono::microseconds> timeout, Ticket& out,
	                 Clock::time_point now = Clock::now());
	Result SetEvent(Identity owner, Handle session, uint32_t index, uint64_t bits);
	Result CancelEvent(Identity owner, Handle client, uint32_t index, uint64_t replacement);
	// Host-boundary cancellation of ONE waiting call, without clearing its
	// event or waking unrelated callers. Completed observations are not undone.
	Result AbandonEventWait(Identity owner, Handle client, const Ticket& ticket);
	void ExpireEvents(Clock::time_point now = Clock::now());
	Result SendQueue(Identity owner, Handle session, uint32_t index, Bytes payload,
	                 std::optional<std::chrono::microseconds> timeout, Ticket& out,
	                 Clock::time_point now = Clock::now());
	Result PollQueue(Identity caller, Handle client, uint32_t index, uint64_t capacity,
	                 Clock::time_point now = Clock::now());
	// Adapter performs payload copy with no core locks held. An error aborts the
	// receive; a length-copy error is reported AFTER consumption, never requeued.
	Result BeginQueueReceive(Identity caller, Handle client, uint32_t index, uint64_t capacity,
	                         QueueReceive& out, Clock::time_point now = Clock::now());
	Result EndQueueReceive(Identity caller, Handle lease, int payload_copy_errno = 0,
	                       int length_copy_errno = 0, Clock::time_point now = Clock::now());
	// Internal channel primitives, NOT new guest syscalls. Send waiters carry the
	// session kid; receive252 is nonblocking and has no sleeping reader waiter.
	Result CancelQueueWaiters(Identity owner, Handle session, uint32_t index, Handle tag,
	                          Clock::time_point now = Clock::now());
	Result FlushQueue(Identity owner, Handle session, uint32_t index, Clock::time_point now = Clock::now());
	void ExpireQueues(Clock::time_point now = Clock::now());
	// Calls the supplied cooperative signal pump without any transport/ticket lock.
	// The caller must keep Core alive; interruption does not cancel/requeue a request.
	Result Wait(const Ticket& ticket, const std::function<bool()>& interrupted = {});
	// Attached connected/disconnecting clients only. Active dispatch/copy leases
	// defer forced notification and session retirement. Pending-connect and
	// process-wide exit remain outside this entry point.
	Result ForceTeardown(Identity owner, Handle handle);
private:
	struct Impl;
	std::unique_ptr<Impl> m;
	static Ticket MakeTicket(const std::shared_ptr<void>& pin);
	static void Finish(const Ticket& ticket, Result result);
};
} // namespace Libs::LibKernel::Ipmi
