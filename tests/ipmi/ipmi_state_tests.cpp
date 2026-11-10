#include "kernel/ipmi.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace Libs::LibKernel::Ipmi;
using namespace std::chrono_literals;
#define CHECK(expr) do { if (!(expr)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
static void Success(const Result& r) { CHECK(r.syscall_errno == 0); CHECK(r.operation_result == 0); }
static void ErrorIs(const Result& r, int e) { CHECK(r.syscall_errno == e); }
static void Consume(Core& core, Identity caller, Handle client, const Ticket& ticket) {
	Result response;
	Success(core.BeginInvokeRead(caller, client, ticket, response));
	Success(core.EndInvokeRead(caller, client, ticket, true));
	Success(core.ReleaseInvokeCaller(caller, client, ticket));
}

struct Fixture {
	Core core;
	Identity server {100, 1}, worker {100, 2}, worker2 {100, 3};
	Identity client {200, 1}, client2 {200, 2};
	Handle s = 0, c = 0, session = 0;
	Fixture() {
		Success(core.RegisterServer(server, "SceLoginMgrServer", 0x12345678, s));
		Config config; config.sync_slots = 2;
		Success(core.RegisterClient(client, "SceLoginMgrServer", config, c));
		Ticket connect; Success(core.Connect(client, c, connect)); CHECK(!connect->Poll());
		Dispatch d; Success(core.Receive(worker, s, d));
		CHECK(d.type == Dispatch::Type::Connect); CHECK(d.caller == client); CHECK(d.handler == 0x12345678);
		Handle untouched = 123;
		ErrorIs(core.CreateSession(worker2, 0x1000, 9, untouched), Invalid); CHECK(untouched == 123);
		Success(core.CreateSession(worker, 0x1000, 9, session));
		// Acceptance-time flag setting survives until the first client poll.
		Success(core.SetEvent(worker, session, 0, 0x80));
		ErrorIs(core.Accept(worker2, c, 0), Invalid); CHECK(!connect->Poll());
		Success(core.Accept(worker, c, 0)); CHECK(connect->Poll()); Success(*connect->Poll());
		CHECK(connect->Poll()->method_result == 0);
	}
};

static void CorrelationAndBounds() {
	Fixture f;
	Ticket a, b, third;
	Success(f.core.Invoke(f.client, f.c, 11, {{1, 2}}, {3}, a));
	Success(f.core.Invoke(f.client2, f.c, 22, {{3}}, {1}, b));
	ErrorIs(f.core.Invoke({200, 3}, f.c, 33, {}, {}, third), WouldBlock); CHECK(!third);
	Dispatch da, db;
	Success(f.core.Receive(f.worker, f.s, da)); Success(f.core.Receive(f.worker2, f.s, db));
	CHECK(da.method == 11 && db.method == 22); CHECK(da.inputs[0] == Bytes({1, 2}));
	CHECK(da.native_session == 0x1000 && da.argument == 9);
	ErrorIs(f.core.Reply({999, 2}, f.session, 0, {{7}}), NotFound);
	ErrorIs(f.core.Reply({100, 99}, f.session, 0, {{7}}), Invalid);
	ErrorIs(f.core.Reply(f.worker2, f.session, 0, {Bytes(4097)}), Invalid);
	CHECK(!a->Poll() && !b->Poll());
	// Reverse reply order proves no session FIFO / global pending reply.
	Success(f.core.Reply(f.worker2, f.session, -42, {{9}}));
	CHECK(!a->Poll()); CHECK(b->Poll()->method_result == -42);
	CHECK(b->Poll()->outputs[0] == Bytes({9}));
	Success(f.core.Reply(f.worker, f.session, 37, {{4, 5}}));
	CHECK(a->Poll()->outputs[0] == Bytes({4, 5})); CHECK(a->Poll()->method_result == 37);
	ErrorIs(f.core.Reply(f.worker, f.session, 0, {{6}}), Invalid);
}

static void IndependentAcceptances() {
	Core core; Handle s, a, b, sa, sb;
	Identity owner {10, 1}, wa {10, 2}, wb {10, 3}, ca {20, 1}, cb {30, 1};
	Success(core.RegisterServer(owner, "service", 100, s));
	Success(core.RegisterClient(ca, "service", {}, a)); Success(core.RegisterClient(cb, "service", {}, b));
	Ticket ta, tb; Success(core.Connect(ca, a, ta)); Success(core.Connect(cb, b, tb));
	Dispatch da, db; Success(core.Receive(wa, s, da)); Success(core.Receive(wb, s, db));
	Success(core.CreateSession(wb, 2000, 2, sb)); Success(core.Accept(wb, b, 0));
	CHECK(tb->Poll() && !ta->Poll());
	Success(core.CreateSession(wa, 1000, 1, sa)); CHECK(sa != sb);
	ErrorIs(core.Accept(wa, b, 0), NotFound); CHECK(!ta->Poll());
	Success(core.Accept(wa, a, 0));
	Ticket call; Success(core.Invoke(ca, a, 1, {}, {}, call));
	Success(core.Receive(wa, s, da)); CHECK(da.native_session == 1000);
	ErrorIs(core.Reply(wa, sb, 0, {}), Invalid); CHECK(!call->Poll());
	Success(core.Reply(wa, sa, 0, {}));
}

static void DisconnectOwnershipAndRetirement() {
	Fixture f; Ticket invoke, disconnect;
	Success(f.core.Invoke(f.client, f.c, 1, {}, {}, invoke));
	ErrorIs(f.core.Disconnect(f.client, f.c, disconnect), Busy); CHECK(!disconnect);
	ErrorIs(f.core.DestroyClient(f.client, f.c), Busy);
	ErrorIs(f.core.DestroyServer(f.server, f.s), Busy);
	Dispatch d; Success(f.core.Receive(f.worker, f.s, d));
	ErrorIs(f.core.CloseSession(f.worker, f.session), Busy);
	Success(f.core.Reply(f.worker, f.session, 0, {}));
	ErrorIs(f.core.Disconnect(f.client, f.c, disconnect), Busy);
	Consume(f.core, f.client, f.c, invoke);
	Success(f.core.Disconnect(f.client, f.c, disconnect));
	Ticket resumed; Success(f.core.Disconnect(f.client, f.c, resumed)); CHECK(resumed == disconnect);
	Success(f.core.Receive(f.worker, f.s, d)); CHECK(d.type == Dispatch::Type::Disconnect);
	CHECK(d.native_session == 0x1000);
	ErrorIs(f.core.AcknowledgeDisconnect(f.worker, f.c, 0), Busy);
	Success(f.core.CloseSession(f.worker, f.session)); CHECK(!disconnect->Poll());
	ErrorIs(f.core.SetEvent(f.worker, f.session, 0, 1), NotFound);
	ErrorIs(f.core.AcknowledgeDisconnect(f.worker2, f.c, 0), Invalid);
	Success(f.core.AcknowledgeDisconnect(f.worker, f.c, 17));
	CHECK(disconnect->Poll()->method_result == 17);
	ErrorIs(f.core.Receive(f.worker, f.s, d), WouldBlock); // no duplicate disconnect
	Success(f.core.DestroyClient(f.client, f.c)); Success(f.core.DestroyServer(f.server, f.s));
	// Completed results remain readable after registry entries retire.
	CHECK(invoke->Poll()->method_result == 0); CHECK(disconnect->Poll()->method_result == 17);
	auto missing = f.core.DestroyClient(f.client, f.c);
	ErrorIs(missing, NotFound); CHECK(missing.operation_result == 0);
}

static void PersistentEventsAndOrdering() {
	Fixture f;
	auto first = f.core.PollEvent(f.client, f.c, 0, 0x80, 2); Success(first); CHECK(first.value == 0x80);
	Success(f.core.SetEvent(f.worker, f.session, 0, 3));
	auto snapshot = f.core.PollEvent(f.client, f.c, 0, 1, 0x21); Success(snapshot); CHECK(snapshot.value == 0x83);
	auto miss = f.core.PollEvent(f.client, f.c, 0, 1, 1); ErrorIs(miss, Busy); CHECK(miss.value == 0x82);
	ErrorIs(f.core.PollEvent(f.client, f.c, 0, 1, 0x32), Invalid);
	Ticket a, b;
	Success(f.core.WaitEvent(f.client, f.c, 0, 4, 0x12, {}, a));
	Success(f.core.WaitEvent(f.client2, f.c, 0, 4, 2, {}, b));
	Success(f.core.SetEvent(f.worker, f.session, 0, 4));
	CHECK(a->Poll()->value == 0x86); CHECK(!b->Poll()); // first waiter clears all
	auto cancelled = f.core.CancelEvent(f.client, f.c, 0, 8); Success(cancelled); CHECK(cancelled.value == 1);
	CHECK(b->Poll()->syscall_errno == Cancelled); CHECK(b->Poll()->value == 0);
	Success(f.core.PollEvent(f.client, f.c, 0, 8, 2));
	// Cancellation does not overwrite an already completed wait.
	Success(f.core.CancelEvent(f.client, f.c, 0, 0)); CHECK(a->Poll()->syscall_errno == Ok);
	const auto now = Clock::now(); Ticket timeout;
	Success(f.core.WaitEvent(f.client, f.c, 0, 16, 2, 10us, timeout, now));
	f.core.ExpireEvents(now + 9us); CHECK(!timeout->Poll());
	f.core.ExpireEvents(now + 10us); ErrorIs(*timeout->Poll(), TimedOut);
	Success(f.core.SetEvent(f.worker, f.session, 0, 16)); ErrorIs(*timeout->Poll(), TimedOut);
	Success(f.core.PollEvent(f.client, f.c, 0, 16, 2)); // late set persists
	Ticket abandoned, survivor;
	Success(f.core.WaitEvent(f.client, f.c, 0, 32, 2, {}, abandoned));
	Success(f.core.WaitEvent(f.client2, f.c, 0, 32, 2, {}, survivor));
	ErrorIs(f.core.AbandonEventWait({999, 1}, f.c, abandoned), NotFound);
	CHECK(!abandoned->Poll());
	Success(f.core.AbandonEventWait(f.client, f.c, abandoned));
	ErrorIs(*abandoned->Poll(), Cancelled); CHECK(!survivor->Poll());
	Success(f.core.SetEvent(f.worker, f.session, 0, 32));
	Success(*survivor->Poll()); CHECK(survivor->Poll()->value == 48);
	Success(f.core.AbandonEventWait(f.client2, f.c, survivor));
	Success(*survivor->Poll()); // Cleanup cannot overwrite completed observation.
}

static void SetCancelRace() {
	Fixture f; Ticket t;
	Success(f.core.CancelEvent(f.client, f.c, 0, 0));
	Success(f.core.WaitEvent(f.client, f.c, 0, 1, 0x12, {}, t));
	std::atomic<int> ready {0}; std::atomic<bool> go {false};
	Result set, cancel;
	std::thread setter([&] { ++ready; while (!go.load()) { std::this_thread::yield(); }
		set = f.core.SetEvent(f.worker, f.session, 0, 1); });
	std::thread canceller([&] { ++ready; while (!go.load()) { std::this_thread::yield(); }
		cancel = f.core.CancelEvent(f.client, f.c, 0, 0); });
	while (ready.load() != 2) { std::this_thread::yield(); } go = true;
	setter.join(); canceller.join(); Success(set); Success(cancel);
	auto r = t->Poll(); CHECK(r);
	if (r->syscall_errno == Ok) {
		CHECK(r->value == 1 && cancel.value == 0);
		ErrorIs(f.core.PollEvent(f.client, f.c, 0, 1, 2), Busy);
	} else {
		CHECK(r->syscall_errno == Cancelled && r->value == 0 && cancel.value == 1);
		Success(f.core.PollEvent(f.client, f.c, 0, 1, 2));
	}
}

static void CooperativeWaitAndQueuedWake() {
	Fixture f; Ticket call;
	Success(f.core.Invoke(f.client, f.c, 1, {}, {}, call));
	// Reentrant pump enters Core. Holding any core lock across it would deadlock.
	auto result = f.core.Wait(call, [&] {
		Dispatch d; Success(f.core.Receive(f.worker, f.s, d));
		Success(f.core.Reply(f.worker, f.session, 55, {})); return false;
	});
	Success(result); CHECK(result.method_result == 55);
	Consume(f.core, f.client, f.c, call);
	Ticket disconnect; Success(f.core.Disconnect(f.client, f.c, disconnect));
	ErrorIs(f.core.Wait(disconnect, [] { return true; }), Interrupted);
	CHECK(!disconnect->Poll());
	Ticket same; Success(f.core.Disconnect(f.client, f.c, same)); CHECK(same == disconnect);
	Dispatch d; Success(f.core.Receive(f.worker, f.s, d));
	Success(f.core.CloseSession(f.worker, f.session)); Success(f.core.AcknowledgeDisconnect(f.worker, f.c, 0));
	Success(f.core.Wake(f.server, f.s)); Success(f.core.Wake(f.server, f.s));
	Success(f.core.Receive(f.worker, f.s, d)); CHECK(d.type == Dispatch::Type::Wake);
	Success(f.core.Receive(f.worker, f.s, d)); CHECK(d.type == Dispatch::Type::Wake);
	ErrorIs(f.core.Receive(f.worker, f.s, d), WouldBlock);
	ErrorIs(f.core.SealServer({999, 1}, f.s), NotFound);
	Success(f.core.SealServer(f.server, f.s));
	Ticket reconnect;
	ErrorIs(f.core.Connect(f.client, f.c, reconnect), NotConnected);
	CHECK(!reconnect);
	Success(f.core.DestroyClient(f.client, f.c));
	Success(f.core.DestroyServer(f.server, f.s));
}

static void UnsupportedIsNonDestructive() {
	Fixture f;
	ErrorIs(f.core.ForceTeardown({999, 1}, f.c), NotFound);
	ErrorIs(f.core.CloseSession(f.worker, f.session), Unsupported);
	Success(f.core.SetEvent(f.worker, f.session, 0, 1));
	Handle h = 777; Config config; config.async_slots = 1;
	config.async_input_limit = 17; config.async_output_limit = 19;
	Success(f.core.RegisterClient(f.client, "other", config, h)); CHECK(h != 777);
	std::array<char, 25> name; name.fill('x');
	ErrorIs(f.core.Name({300, 1}, f.c, name), NotFound); CHECK(name[0] == 'x');
	Success(f.core.Name(f.client, f.c, name)); CHECK(std::string(name.data()) == "SceLoginMgrServer"); CHECK(name[24] == 0);
	Ticket pending;
	{
		Core core; Handle s, c;
		Success(core.RegisterServer({1, 1}, "abandoned", 1, s));
		Success(core.RegisterClient({2, 1}, "abandoned", {}, c));
		Success(core.Connect({2, 1}, c, pending));
	}
	ErrorIs(*pending->Poll(), Unsupported); CHECK(!pending->Poll()->operation_result);
}

static void DroppedTicketDoesNotCancel() {
	Fixture f;
	Ticket t;
	Success(f.core.WaitEvent(f.client, f.c, 0, 0x100, 2, {}, t));
	t.reset();
	Ticket disconnect;
	ErrorIs(f.core.Disconnect(f.client, f.c, disconnect), Busy);
	auto cancelled = f.core.CancelEvent(f.client, f.c, 0, 0);
	Success(cancelled); CHECK(cancelled.value == 1);
	Success(f.core.Disconnect(f.client, f.c, disconnect));
	disconnect.reset();
	// The connection owns its original completion, even without a caller token.
	Success(f.core.Disconnect(f.client, f.c, disconnect)); CHECK(disconnect);
	Dispatch d; Success(f.core.Receive(f.worker, f.s, d));
	Success(f.core.CloseSession(f.worker, f.session));
	Success(f.core.AcknowledgeDisconnect(f.worker, f.c, 0));
	Success(*disconnect->Poll());
}

static void SameTidDifferentProcesses() {
	Core core;
	Handle s1, s2, c1, c2, session1, session2;
	Identity receiver1 {10, 7}, receiver2 {11, 7}, caller1 {20, 7}, caller2 {21, 7};
	Success(core.RegisterServer(receiver1, "first", 1, s1));
	Success(core.RegisterServer(receiver2, "second", 2, s2));
	Success(core.RegisterClient(caller1, "first", {}, c1));
	Success(core.RegisterClient(caller2, "second", {}, c2));
	Ticket a, b; Success(core.Connect(caller1, c1, a)); Success(core.Connect(caller2, c2, b));
	Dispatch d; Success(core.Receive(receiver1, s1, d)); Success(core.Receive(receiver2, s2, d));
	Success(core.CreateSession(receiver2, 2000, 2, session2)); Success(core.Accept(receiver2, c2, 0));
	CHECK(b->Poll() && !a->Poll());
	Success(core.CreateSession(receiver1, 1000, 1, session1)); Success(core.Accept(receiver1, c1, 0));
	Success(core.Invoke(caller1, c1, 1, {}, {}, a)); Success(core.Invoke(caller2, c2, 2, {}, {}, b));
	Success(core.Receive(receiver1, s1, d)); Success(core.Receive(receiver2, s2, d));
	ErrorIs(core.Reply(receiver2, session1, 0, {}), NotFound);
	Success(core.Reply(receiver2, session2, 2, {})); CHECK(!a->Poll());
	Success(core.Reply(receiver1, session1, 1, {}));
	CHECK(a->Poll()->method_result == 1 && b->Poll()->method_result == 2);
}

static void ReceiveReservationOwnership() {
	Core core; Handle server, client, session = 999;
	Identity owner {10, 1}, worker {10, 2}, other {10, 3}, caller {20, 1};
	Success(core.RegisterServer(owner, "reserved", 100, server));
	Success(core.RegisterClient(caller, "reserved", {}, client));
	Ticket connect; Success(core.Connect(caller, client, connect, {1, 0, 3}));
	Dispatch dispatch; Success(core.BeginReceive(worker, server, dispatch));
	CHECK(dispatch.connect_argument == Bytes({1, 0, 3}));
	ErrorIs(core.BeginReceive(other, server, dispatch), Busy);
	ErrorIs(core.EndReceive(other, server, true), NotFound);
	ErrorIs(core.EndReceive({11, worker.tid}, server, false), NotFound);
	ErrorIs(core.CreateSession(worker, 1000, 1, session), Invalid); CHECK(session == 999);
	ErrorIs(core.DestroyServer(owner, server), Busy); CHECK(!connect->Poll());
	Success(core.EndReceive(worker, server, false));
	Success(core.Receive(other, server, dispatch));
	CHECK(dispatch.connect_argument == Bytes({1, 0, 3}));
	Success(core.CreateSession(other, 1000, 1, session));
	Success(core.Accept(other, client, 0)); Success(*connect->Poll());
}

static void IndependentReplyAndCallerRelease() {
	for (bool client_first: {false, true}) {
		Fixture f; Ticket call, other, blocked, reassigned;
		Success(f.core.Invoke(f.client, f.c, 0x160008, {}, {24}, call));
		Dispatch dispatch; Success(f.core.Receive(f.worker, f.s, dispatch));
		Bytes data(24, 0x37);
		Success(f.core.PublishReply(f.worker, f.session, 0, {data}));
		CHECK(call->Poll() && call->InspectReply().published);
		CHECK(!call->InspectReply().caller_released && !call->InspectReply().receiver_released);
		const auto packet = call->InspectReplyPacket();
		const Bytes header {0x42,0,0,0, 0x2c,0,0,0, 0,0,0,0, 1,0,0,0, 0x18,0,0,0};
		CHECK(packet.size() == 44 && std::equal(header.begin(), header.end(), packet.begin()));
		CHECK(Bytes(packet.begin() + 20, packet.end()) == data);
		ErrorIs(f.core.Invoke(f.client, f.c, 2, {}, {}, blocked), Busy);
		ErrorIs(f.core.Receive(f.worker, f.s, dispatch), Busy);
		if (client_first) {
			Consume(f.core, f.client, f.c, call);
			CHECK(call->InspectReply().caller_released && !call->InspectReply().receiver_released);
			ErrorIs(f.core.Disconnect(f.client, f.c, blocked), Busy);
			Success(f.core.Invoke(f.client, f.c, 3, {}, {}, reassigned));
			ErrorIs(f.core.Receive(f.worker, f.s, dispatch), Busy);
		} else {
			Success(f.core.ReleaseReplyReceiver(f.worker, f.session));
			CHECK(call->InspectReply().receiver_released && !call->InspectReply().caller_released);
			ErrorIs(f.core.Invoke(f.client, f.c, 2, {}, {}, blocked), Busy);
			// A free server worker can serve a different caller before completion.
			Success(f.core.Invoke(f.client2, f.c, 2, {}, {}, other));
			Success(f.core.Receive(f.worker, f.s, dispatch));
			Success(f.core.Reply(f.worker, f.session, 7, {}));
			Consume(f.core, f.client2, f.c, other);
			Consume(f.core, f.client, f.c, call);
		}
		if (client_first) {
			Success(f.core.ReleaseReplyReceiver(f.worker, f.session));
			Success(f.core.Receive(f.worker, f.s, dispatch)); CHECK(dispatch.method == 3);
			Success(f.core.Reply(f.worker, f.session, 0, {}));
			Consume(f.core, f.client, f.c, reassigned);
		}
		CHECK(call->InspectReply().consumed && call->InspectReply().receiver_released);
		CHECK(call->InspectReplyPacket().empty());
		Result response;
		ErrorIs(f.core.BeginInvokeRead(f.client, f.c, call, response), NotFound);
	}
}

void QueueTests();
int main() {
	CorrelationAndBounds(); IndependentAcceptances(); DisconnectOwnershipAndRetirement();
	PersistentEventsAndOrdering(); SetCancelRace(); CooperativeWaitAndQueuedWake(); UnsupportedIsNonDestructive();
	DroppedTicketDoesNotCancel(); SameTidDifferentProcesses();
	ReceiveReservationOwnership();
	IndependentReplyAndCallerRelease();
	QueueTests();
	std::cout << "IPMI bounded state tests passed\n";
}
