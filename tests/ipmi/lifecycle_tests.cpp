#include "kernel/ipmi.h"

#include <cstdlib>
#include <iostream>

using namespace Libs::LibKernel::Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace {
constexpr Identity server {100, 1}, receiver {100, 2}, other {100, 3}, caller {200, 1};
struct Fixture {
	Core core;
	Handle s = 0, c = 0, session = 0;
	Fixture() {
		Config config; config.async_slots = 2; config.async_input_limit = 32; config.async_output_limit = 32;
		config.queue_count = 1; config.queue_capacities = {16};
		CHECK(core.RegisterServer(server, "lifecycle", 1, s).syscall_errno == Ok);
		CHECK(core.RegisterClient(caller, "lifecycle", config, c).syscall_errno == Ok);
		Connect();
	}
	void Connect() {
		Ticket t; Dispatch d;
		CHECK(core.Connect(caller, c, t).syscall_errno == Ok);
		CHECK(core.Receive(receiver, s, d).syscall_errno == Ok);
		CHECK(core.CreateSession(receiver, 0x123400, 0x5678, session, s + 999).syscall_errno == NotFound);
		CHECK(core.CreateSession(receiver, 0x123400, 0x5678, session, s).syscall_errno == Ok);
		CHECK(core.Accept(receiver, c, 0).syscall_errno == Ok);
	}
	void CloseForced() {
		Dispatch d; CHECK(core.Receive(receiver, s, d).syscall_errno == Ok);
		CHECK(d.type == Dispatch::Type::ForcedDisconnect && d.native_session == 0x123400);
		int32_t pid = 0; CHECK(core.SessionPeerPid(receiver, session, pid).syscall_errno == Ok && pid == caller.pid);
		CHECK(core.CloseSession(other, session).syscall_errno == Invalid);
		CHECK(core.CloseSession(receiver, session).syscall_errno == Ok);
		CHECK(core.Receive(receiver, s, d).syscall_errno == WouldBlock);
		CHECK(core.DestroyClient(caller, c).syscall_errno == Ok);
		CHECK(core.DestroyServer(server, s).syscall_errno == Ok);
	}
};
void RejectedStorageIsRetired() {
	Core core; Handle s = 0, c = 0, session = 0; Ticket t; Dispatch d;
	CHECK(core.RegisterServer(server, "reject", 1, s).syscall_errno == Ok);
	CHECK(core.RegisterClient(caller, "reject", {}, c).syscall_errno == Ok);
	for (bool create: {false, true}) {
		CHECK(core.Connect(caller, c, t).syscall_errno == Ok);
		CHECK(core.Receive(receiver, s, d).syscall_errno == Ok);
		if (create) { CHECK(core.CreateSession(receiver, 0xdead, 0, session).syscall_errno == Ok); }
		CHECK(core.Accept(other, c, -42).syscall_errno == Invalid);
		CHECK(core.Accept(receiver, c, -42).syscall_errno == Ok);
		CHECK(t->PollStatus()->syscall_errno == Ok && t->PollStatus()->method_result == -42);
		ObjectInfo info; CHECK(core.Inspect(receiver, session, ObjectKind::Session, info).syscall_errno == NotFound);
		CHECK(core.Receive(receiver, s, d).syscall_errno == WouldBlock);
	}
	CHECK(core.DestroyClient(caller, c).syscall_errno == Ok);
	CHECK(core.DestroyServer(server, s).syscall_errno == Ok);
}
void AsyncConsumeAndLifetime() {
	Fixture f; uint32_t a = 999, b = 999, third = 999; Result result; Dispatch d;
	CHECK(f.core.InvokeAsync(caller, f.c, 0x30040, {{1,2}}, a).syscall_errno == Ok && a == 0);
	CHECK(f.core.InvokeAsync(caller, f.c, 0x30040, {}, b).syscall_errno == Ok && b == 1);
	CHECK(f.core.InvokeAsync(caller, f.c, 0, {}, third).syscall_errno == Busy && third == 999);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30041, a, {}, result).syscall_errno == WouldBlock);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30041, 1234, {}, result).syscall_errno == Invalid);
	CHECK(f.core.Receive(receiver, f.s, d).syscall_errno == Ok && d.async_ticket == a);
	CHECK(f.core.ReplyAsync({999,1}, f.session, 0x30041, a, -42, {}).syscall_errno == NotFound);
	CHECK(f.core.ReplyAsync(other, f.session, 0x30041, a, -42, {}).syscall_errno == Ok);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30040, a, {}, result).syscall_errno == BadMessage);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30041, a, {}, result).syscall_errno == Ok && result.method_result == -42);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30041, a, {}, result).syscall_errno == Invalid);
	CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Ok);
	CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Ok); // one notification
	CHECK(f.core.Receive(other, f.s, d).syscall_errno == WouldBlock); // callback still owns session
	CHECK(f.core.ReplyAsync(other, f.session, 0x30041, b, 0, {}).syscall_errno == NotConnected);
	CHECK(f.core.CollectAsync(caller, f.c, 0x30041, b, {}, result).syscall_errno == NotConnected);
	CHECK(f.core.ReleaseAsyncReceiver(receiver, f.session).syscall_errno == Ok);
	f.CloseForced();
}
void ForcedCopyAndReceiverDeferral() {
	Fixture f; Ticket request, event, writer, first; Dispatch d;
	CHECK(f.core.Invoke(caller, f.c, 3, {}, {4}, request).syscall_errno == Ok);
	CHECK(f.core.BeginReceive(receiver, f.s, d).syscall_errno == Ok);
	CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Busy); // reserved dispatch has not committed
	CHECK(!request->PollStatus());
	CHECK(f.core.EndReceive(receiver, f.s, true).syscall_errno == Ok);
	CHECK(f.core.WaitEvent(caller, f.c, 0, 1, 1, {}, event).syscall_errno == Ok);
	CHECK(f.core.SendQueue(other, f.session, 0, {1,2,3,4}, {}, first).syscall_errno == Ok);
	QueueReceive queue; CHECK(f.core.BeginQueueReceive(caller, f.c, 0, 4, queue).syscall_errno == Ok);
	CHECK(f.core.SendQueue(other, f.session, 0, {5,6,7,8}, {}, writer).syscall_errno == Ok && !writer->PollStatus());
	CHECK(f.core.PublishReply(receiver, f.session, 0, {{9,8,7,6}}).syscall_errno == Ok);
	Result response; CHECK(f.core.BeginInvokeRead(caller, f.c, request, response).syscall_errno == Ok);
	CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Ok);
	CHECK(event->PollStatus()->syscall_errno == Cancelled && writer->PollStatus()->syscall_errno == Cancelled);
	CHECK(f.core.Receive(other, f.s, d).syscall_errno == WouldBlock);
	CHECK(f.core.PublishReply(receiver, f.session, 0, {}).syscall_errno == NotConnected);
	CHECK(f.core.ReleaseReplyReceiver(receiver, f.session).syscall_errno == Ok);
	CHECK(f.core.ReleaseInvokeCaller(caller, f.c, request).syscall_errno == Busy);
	CHECK(f.core.EndInvokeRead(caller, f.c, request, true).syscall_errno == Ok);
	CHECK(f.core.ReleaseInvokeCaller(caller, f.c, request).syscall_errno == Ok);
	CHECK(f.core.Receive(other, f.s, d).syscall_errno == WouldBlock); // queue copy still pins storage
	CHECK(f.core.EndQueueReceive(caller, queue.lease).syscall_errno == Ok);
	f.CloseForced();
}
void AsyncCopyDeferralAndBoundaryFailure() {
	{
		Fixture f; uint32_t ticket = 99; Dispatch d; Result response;
		CHECK(f.core.InvokeAsync(caller, f.c, 7, {}, ticket).syscall_errno == Ok);
		CHECK(f.core.Receive(receiver, f.s, d).syscall_errno == Ok);
		CHECK(f.core.ReplyAsync(receiver, f.session, 8, ticket, -31, {}).syscall_errno == Ok);
		CHECK(f.core.ReleaseAsyncReceiver(receiver, f.session).syscall_errno == Ok);
		CHECK(f.core.BeginAsyncRead(caller, f.c, 8, ticket, {}, response).syscall_errno == Ok);
		CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Ok);
		CHECK(f.core.Receive(receiver, f.s, d).syscall_errno == WouldBlock);
		CHECK(f.core.EndAsyncRead(caller, f.c, ticket, true).syscall_errno == Ok);
		CHECK(f.core.ReleaseAsyncTicket(caller, f.c, ticket).syscall_errno == Ok);
		f.CloseForced();
	}
	{
		Fixture f; Ticket request; Dispatch d;
		CHECK(f.core.Invoke(caller, f.c, 7, {}, {}, request).syscall_errno == Ok);
		CHECK(f.core.Receive(receiver, f.s, d).syscall_errno == Ok);
		CHECK(f.core.AbortRequest(other, f.session, BadMessage).syscall_errno == Invalid);
		CHECK(f.core.AbortRequest(receiver, f.session, BadMessage).syscall_errno == Ok);
		CHECK(request->PollStatus()->syscall_errno == BadMessage && !request->PollStatus()->method_result);
		CHECK(f.core.ReleaseInvokeCaller(caller, f.c, request).syscall_errno == Ok);
		CHECK(f.core.ForceTeardown(caller, f.c).syscall_errno == Ok);
		f.CloseForced();
	}
}
} // namespace
int main() {
	RejectedStorageIsRetired(); AsyncConsumeAndLifetime(); ForcedCopyAndReceiverDeferral();
	AsyncCopyDeferralAndBoundaryFailure();
	std::cout << "IPMI lifecycle component checks passed\n";
}
