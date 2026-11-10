#include "kernel/ipmi.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace Libs::LibKernel::Ipmi;
using namespace std::chrono_literals;
#define CHECK(expr) do { if (!(expr)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
static void Success(const Result& r) { CHECK(r.syscall_errno == Ok && r.operation_result == 0); }
static void ErrorIs(const Result& r, int error) { CHECK(r.syscall_errno == error); }

struct QueueFixture {
	Core core;
	Identity server {100, 1}, sender {100, 2}, sender2 {100, 3}, sender3 {100, 4};
	Identity client {200, 1}, reader2 {200, 2};
	Handle s = 0, c = 0, session = 0;
	Clock::time_point now = Clock::now();
	explicit QueueFixture(std::vector<uint64_t> capacities = {16}) {
		Success(core.RegisterServer(server, "UserService", 0x1234, s));
		Config config; config.queue_count = static_cast<uint32_t>(capacities.size()); config.queue_capacities = capacities;
		Success(core.RegisterClient(client, "UserService", config, c));
		Ticket connect; Success(core.Connect(client, c, connect));
		Dispatch d; Success(core.Receive(sender, s, d)); CHECK(d.config.queue_capacities == capacities);
		Success(core.CreateSession(sender, 0x1000, 0, session));
		Success(core.Accept(sender, c, 0));
	}
	Ticket Send(Identity from, Bytes payload, std::optional<std::chrono::microseconds> timeout = {}, uint32_t index = 0) {
		Ticket t; Success(core.SendQueue(from, session, index, std::move(payload), timeout, t, now)); return t;
	}
	Result Poll(uint64_t capacity = 8, uint32_t index = 0) { return core.PollQueue(client, c, index, capacity, now); }
};

static void QueueFramingAndShortBuffers() {
	QueueFixture f;
	// A genuine transport payload, not a synthesized notification: exactly eight
	// opaque bytes supplied by the sender. The internal eight-byte header also
	// occupies capacity, but is never exposed as part of the returned payload.
	const Bytes payload {0x00, 0x01, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00};
	auto sent = f.Send(f.sender, payload, 0us); Success(*sent->Poll());
	CHECK(sent->Poll()->remaining_us == 0);
	auto full = f.Send(f.sender2, {99}, 0us); ErrorIs(*full->Poll(), WouldBlock);
	CHECK(!full->Poll()->operation_result && !full->Poll()->method_result);
	QueueReceive unchanged {777, {88}};
	ErrorIs(f.core.BeginQueueReceive(f.client, f.c, 0, 7, unchanged, f.now), NoMemory);
	CHECK(unchanged.lease == 777 && unchanged.payload == Bytes({88}));
	ErrorIs(f.Poll(0), NoMemory);
	auto got = f.Poll(8); Success(got); CHECK(got.received_size == 8);
	CHECK(got.outputs.size() == 1 && got.outputs[0] == payload);
	CHECK(!got.method_result);
	auto empty = f.Poll(); ErrorIs(empty, WouldBlock); CHECK(!empty.received_size && empty.outputs.empty());
	Success(*f.Send(f.sender, payload)->Poll());
	ErrorIs(f.Poll(UINT64_MAX - 7), BadMessage);
	CHECK(f.Poll().outputs[0] == payload); // overflow rejected without consumption
	QueueFixture small({15}); Ticket t;
	ErrorIs(small.core.SendQueue(small.sender, small.session, 0, payload, {}, t, small.now), NoMemory); CHECK(!t);
	ErrorIs(f.core.SendQueue(f.sender, f.session, 0, {}, {}, t, f.now), Invalid);
	ErrorIs(f.core.SendQueue(f.sender, f.session, 0, Bytes(0x7fff9), {}, t, f.now), NoMemory);
	QueueFixture maximum({0x80000});
	Success(*maximum.Send(maximum.sender, Bytes(0x7fff8, 7), 0us)->Poll());
	auto max_read = maximum.Poll(0x7fff8); Success(max_read); CHECK(max_read.received_size == 0x7fff8);
	CHECK(max_read.outputs[0].front() == 7 && max_read.outputs[0].back() == 7);
}

static void QueueCopyCommitAndGuestOwnership() {
	QueueFixture f;
	Success(*f.Send(f.sender, {1, 2, 3})->Poll());
	QueueReceive lease;
	ErrorIs(f.core.BeginQueueReceive({999, 1}, f.c, 0, 8, lease, f.now), NotFound);
	Success(f.core.BeginQueueReceive(f.client, f.c, 0, 8, lease, f.now)); CHECK(lease.payload == Bytes({1, 2, 3}));
	QueueReceive other;
	ErrorIs(f.core.BeginQueueReceive(f.reader2, f.c, 0, 8, other, f.now), WouldBlock);
	ErrorIs(f.core.EndQueueReceive(f.reader2, lease.lease, 0, 0, f.now), Invalid);
	ErrorIs(f.core.EndQueueReceive({999, 1}, lease.lease, 0, 0, f.now), Invalid);
	Ticket disconnect; ErrorIs(f.core.Disconnect(f.client, f.c, disconnect), Busy);
	// Simulated copyout fault: abort keeps the original frame and releases owner.
	ErrorIs(f.core.EndQueueReceive(f.client, lease.lease, 0x0e, 0, f.now), 0x0e);
	Success(f.core.BeginQueueReceive(f.reader2, f.c, 0, 8, other, f.now)); CHECK(other.payload == lease.payload);
	ErrorIs(f.core.EndQueueReceive(f.reader2, other.lease, NoMemory, 0, f.now), BadMessage);
	Success(f.core.BeginQueueReceive(f.client, f.c, 0, 8, lease, f.now));
	// A failed length copy cannot resurrect a successfully copied/consumed frame.
	auto length_fault = f.core.EndQueueReceive(f.client, lease.lease, 0, 0x0e, f.now);
	ErrorIs(length_fault, 0x0e); CHECK(!length_fault.received_size && !length_fault.operation_result);
	ErrorIs(f.Poll(), WouldBlock);
	ErrorIs(f.core.EndQueueReceive(f.client, lease.lease, 0, 0, f.now), NotFound);
	ErrorIs(f.core.SendQueue(f.client, f.session, 0, {1}, {}, disconnect, f.now), NotFound);
	ErrorIs(f.core.SendQueue({999, 2}, f.session, 0, {1}, {}, disconnect, f.now), NotFound);
}

static void QueueTimedSendAndHeadOfLine() {
	QueueFixture f;
	Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
	auto timed = f.Send(f.sender2, Bytes(8, 2), 10us); CHECK(!timed->Poll());
	f.now += 9us;
	CHECK(f.Poll().outputs[0] == Bytes(8, 1)); Success(*timed->Poll()); CHECK(timed->Poll()->remaining_us == 1);
	CHECK(f.Poll().outputs[0] == Bytes(8, 2));
	Success(*f.Send(f.sender, Bytes(8, 3))->Poll());
	auto expires = f.Send(f.sender2, Bytes(8, 4), 10us);
	f.now += 10us; f.core.ExpireQueues(f.now);
	ErrorIs(*expires->Poll(), WouldBlock); CHECK(expires->Poll()->remaining_us == 0);
	CHECK(f.Poll().outputs[0] == Bytes(8, 3)); ErrorIs(f.Poll(), WouldBlock);
	// The same sender can issue a new operation after its old wait expires.
	auto indefinite = f.Send(f.sender2, Bytes(8, 5)); Success(*indefinite->Poll()); CHECK(!indefinite->Poll()->remaining_us);
	Ticket unchanged = indefinite;
	ErrorIs(f.core.SendQueue(f.sender, f.session, 0, {1}, -1us, unchanged, f.now), Invalid); CHECK(unchanged == indefinite);
	ErrorIs(f.core.SendQueue(f.sender, f.session, 0, {1}, std::chrono::microseconds(UINT64_C(1) << 32), unchanged, f.now), Invalid);

	QueueFixture fifo({40});
	Success(*fifo.Send(fifo.sender, Bytes(8, 1))->Poll()); // 16 framed bytes
	Success(*fifo.Send(fifo.sender, Bytes(16, 2))->Poll()); // 24 framed bytes
	auto large = fifo.Send(fifo.sender2, Bytes(16, 3));
	auto little = fifo.Send(fifo.sender3, {4});
	CHECK(!large->Poll() && !little->Poll());
	CHECK(fifo.Poll(16).outputs[0] == Bytes(8, 1));
	CHECK(!large->Poll() && !little->Poll()); // free16: small writer may not bypass head24
	CHECK(fifo.Poll(16).outputs[0] == Bytes(16, 2));
	Success(*large->Poll()); Success(*little->Poll());
	CHECK(fifo.Poll(16).outputs[0] == Bytes(16, 3)); CHECK(fifo.Poll(16).outputs[0] == Bytes({4}));
}

static void QueueCancelScopeAndDeferredFlush() {
	QueueFixture f({32, 16});
	Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
	QueueReceive lease; Success(f.core.BeginQueueReceive(f.client, f.c, 0, 8, lease, f.now));
	// Even with free space, a receive owner serializes channel access.
	auto blocked = f.Send(f.sender2, Bytes(8, 2), 50us); CHECK(!blocked->Poll());
	ErrorIs(f.core.CancelQueueWaiters(f.client, f.session, 0, f.session, f.now), NotFound);
	auto wrong_tag = f.core.CancelQueueWaiters(f.sender, f.session, 0, f.c, f.now);
	Success(wrong_tag); CHECK(wrong_tag.value == 0 && !blocked->Poll());
	// queue253 passes session kid as the channel waiter tag, not client kid.
	f.now += 3us;
	auto cancelled = f.core.CancelQueueWaiters(f.sender, f.session, 0, f.session, f.now);
	CHECK(cancelled.value == 1); ErrorIs(*blocked->Poll(), Cancelled); CHECK(blocked->Poll()->remaining_us == 47);
	ErrorIs(f.core.EndQueueReceive(f.client, lease.lease, 0x0e, 0, f.now), 0x0e);
	CHECK(f.Poll().outputs[0] == Bytes(8, 1)); // targeted cancel preserved queued frame
	Success(*f.Send(f.sender, Bytes(8, 3))->Poll());
	Success(*f.Send(f.sender, Bytes(8, 9), {}, 1)->Poll());
	Success(f.core.BeginQueueReceive(f.client, f.c, 0, 8, lease, f.now));
	auto pending = f.Send(f.sender2, Bytes(8, 4));
	Success(f.core.FlushQueue(f.sender, f.session, 0, f.now)); CHECK(!pending->Poll());
	CHECK(lease.payload == Bytes(8, 3)); // active owner still has its pinned payload
	Success(f.core.EndQueueReceive(f.client, lease.lease, 0, 0, f.now));
	ErrorIs(*pending->Poll(), Cancelled); ErrorIs(f.Poll(), WouldBlock);
	CHECK(f.Poll(8, 1).outputs[0] == Bytes(8, 9)); // no cross-queue flush
	Success(*f.Send(f.sender, {5})->Poll()); CHECK(f.Poll().outputs[0] == Bytes({5})); // cancellation not sticky
}

static void QueueCancelVersusCapacityRace() {
	QueueFixture f;
	Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
	auto pending = f.Send(f.sender2, Bytes(8, 2));
	std::atomic<int> ready {0}; std::atomic<bool> go {false}; Result read, cancel;
	std::thread reader([&] { ++ready; while (!go.load()) { std::this_thread::yield(); } read = f.Poll(); });
	std::thread canceller([&] { ++ready; while (!go.load()) { std::this_thread::yield(); }
		cancel = f.core.CancelQueueWaiters(f.sender, f.session, 0, f.session, f.now); });
	while (ready.load() != 2) { std::this_thread::yield(); } go = true;
	reader.join(); canceller.join(); Success(read); Success(cancel);
	CHECK(read.outputs[0] == Bytes(8, 1)); CHECK(pending->Poll());
	if (pending->Poll()->syscall_errno == Ok) {
		CHECK(cancel.value == 0); CHECK(f.Poll().outputs[0] == Bytes(8, 2));
	} else {
		ErrorIs(*pending->Poll(), Cancelled); CHECK(cancel.value == 1); ErrorIs(f.Poll(), WouldBlock);
	}
}

static void QueueSetupAndLifetime() {
	Core core; Handle out = 777; Config bad; bad.queue_count = 1;
	ErrorIs(core.RegisterClient({1, 1}, "service", bad, out), Invalid); CHECK(out == 777);
	bad.queue_capacities = {7}; ErrorIs(core.RegisterClient({1, 1}, "service", bad, out), Invalid);
	bad.queue_count = 33; bad.queue_capacities.assign(33, 16);
	ErrorIs(core.RegisterClient({1, 1}, "service", bad, out), Invalid);
	Ticket abandoned;
	{
		QueueFixture f;
		Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
		abandoned = f.Send(f.sender2, Bytes(8, 2)); CHECK(!abandoned->Poll());
		Ticket no; ErrorIs(f.core.Disconnect(f.client, f.c, no), Busy);
		ErrorIs(f.core.ForceTeardown({999, 1}, f.c), NotFound); CHECK(!abandoned->Poll());
	}
	ErrorIs(*abandoned->Poll(), Unsupported); CHECK(!abandoned->Poll()->operation_result);
	QueueFixture f;
	Handle c2, session2; Identity caller2 {201, 1}, accept2 {100, 9};
	Config config; config.queue_count = 1; config.queue_capacities = {16};
	Success(f.core.RegisterClient(caller2, "UserService", config, c2));
	Ticket connected; Success(f.core.Connect(caller2, c2, connected));
	Dispatch d; Success(f.core.Receive(accept2, f.s, d));
	Success(f.core.CreateSession(accept2, 0x2000, 0, session2));
	// session-present send is supported during acceptance, before client poll is legal.
	Ticket sent; Success(f.core.SendQueue(accept2, session2, 0, Bytes(8, 9), {}, sent, f.now)); Success(*sent->Poll());
	ErrorIs(f.core.PollQueue(caller2, c2, 0, 8, f.now), NotConnected);
	Success(f.core.Accept(accept2, c2, 0));
	Ticket other_pending; Success(f.core.SendQueue(accept2, session2, 0, {8}, {}, other_pending, f.now));
	Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
	auto dropped = f.Send(f.sender2, {2}); dropped.reset();
	auto scoped = f.core.CancelQueueWaiters(f.sender, f.session, 0, f.session, f.now);
	CHECK(scoped.value == 1); CHECK(!other_pending->Poll());
	Success(f.core.FlushQueue(f.sender, f.session, 0, f.now)); CHECK(!other_pending->Poll());
	CHECK(f.core.PollQueue(caller2, c2, 0, 8, f.now).outputs[0] == Bytes(8, 9));
	Success(*other_pending->Poll());
	CHECK(f.core.PollQueue(caller2, c2, 0, 8, f.now).outputs[0] == Bytes({8}));
}

static void QueueCooperativeWaitAndReconnect() {
	QueueFixture f;
	Success(*f.Send(f.sender, Bytes(8, 1))->Poll());
	auto pending = f.Send(f.sender2, Bytes(8, 2));
	ErrorIs(f.core.Wait(pending, [] { return true; }), Interrupted); CHECK(!pending->Poll());
	auto complete = f.core.Wait(pending, [&] {
		CHECK(f.Poll().outputs[0] == Bytes(8, 1)); // enters core from unlocked signal pump
		return false;
	});
	Success(complete); CHECK(!complete.method_result);
	// A committed queued message does not prevent normal disconnect, but must
	// not leak into a subsequent connection's UserService notification stream.
	Ticket disconnect; Success(f.core.Disconnect(f.client, f.c, disconnect));
	// No new writer may strand close after receives become unavailable.
	Ticket rejected;
	ErrorIs(f.core.SendQueue(f.sender, f.session, 0, Bytes(8, 5), {}, rejected, f.now), NotConnected);
	CHECK(!rejected);
	ErrorIs(f.core.SendQueue(f.sender2, f.session, 0, Bytes(8, 6), {}, rejected, f.now), NotConnected);
	CHECK(!rejected);
	Dispatch d; Success(f.core.Receive(f.sender, f.s, d)); CHECK(d.type == Dispatch::Type::Disconnect);
	Success(f.core.CloseSession(f.sender, f.session));
	Success(f.core.AcknowledgeDisconnect(f.sender, f.c, 0));
	Ticket connect; Success(f.core.Connect(f.client, f.c, connect));
	Success(f.core.Receive(f.sender, f.s, d));
	const auto old_session = f.session;
	Success(f.core.CreateSession(f.sender, 0x2000, 0, f.session)); CHECK(f.session != old_session);
	Success(f.core.Accept(f.sender, f.c, 0));
	ErrorIs(f.Poll(), WouldBlock);
	Ticket stale; ErrorIs(f.core.SendQueue(f.sender, old_session, 0, {3}, {}, stale, f.now), NotFound);
	Success(*f.Send(f.sender, {4})->Poll()); CHECK(f.Poll().outputs[0] == Bytes({4}));
}

void QueueTests() {
	QueueFramingAndShortBuffers(); QueueCopyCommitAndGuestOwnership(); QueueTimedSendAndHeadOfLine();
	QueueCancelScopeAndDeferredFlush(); QueueCancelVersusCapacityRace(); QueueSetupAndLifetime();
	QueueCooperativeWaitAndReconnect();
}
