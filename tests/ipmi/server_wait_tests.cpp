#include "kernel/ipmi.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace Libs::LibKernel::Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr Identity Client {200, 1}, OtherThread {200, 2}, Server {100, 1};

int main() {
	Core core;
	Handle client = 0, server = 0;
	CHECK(!core.RegisterClient(Client, "SceAppDbIpcmsg", {}, client).syscall_errno);
	Ticket ready, duplicate, accepted;
	CHECK(!core.WaitServer(Client, client, ready).syscall_errno && !ready->PollStatus());
	CHECK(!core.WaitServer(Client, client, duplicate).syscall_errno && duplicate == ready);
	CHECK(core.WaitServer(OtherThread, client, duplicate).syscall_errno == Busy);
	CHECK(core.Connect(Client, client, accepted).syscall_errno == Busy && !accepted);
	CHECK(core.Connect(Client, client, accepted, {}, 1, ready).syscall_errno == WouldBlock);
	CHECK(core.AbandonServerWait(OtherThread, client, ready).syscall_errno == Invalid);
	ConnectionInfo connection;
	CHECK(core.Connection(Client, client, connection).syscall_errno == NotFound);
	ServerConfig cfg; cfg.handler = 0x1234;
	CHECK(!core.RegisterServer(Server, "unrelated", cfg, server).syscall_errno && !ready->PollStatus());
	CHECK(!core.DestroyServer(Server, server).syscall_errno);
	CHECK(!core.RegisterServer(Server, "SceAppDbIpcmsg", cfg, server, false).syscall_errno);
	CHECK(!ready->PollStatus() && core.Connection(Client, client, connection).syscall_errno == NotFound);
	CHECK(!core.DestroyServer(Server, server).syscall_errno && !ready->PollStatus()); // factory rollback
	CHECK(!core.RegisterServer(Server, "SceAppDbIpcmsg", cfg, server, false).syscall_errno);
	CHECK(core.PublishServer(Client, server).syscall_errno == NotFound);
	CHECK(!core.PublishServer(Server, server).syscall_errno);
	auto observed = ready->PollStatus();
	CHECK(observed && !observed->syscall_errno && observed->value == server);
	CHECK(!observed->method_result && !observed->operation_result); // readiness, not acceptance
	CHECK(!core.PublishServer(Server, server).syscall_errno);
	CHECK(!core.Connection(Client, client, connection, server).syscall_errno);
	CHECK(connection.server_pid == Server.pid);
	CHECK(core.Connect(OtherThread, client, accepted, {}, server, ready).syscall_errno == Invalid);
	CHECK(!core.Connect(Client, client, accepted, {1, 2, 3}, server, ready).syscall_errno);
	CHECK(!accepted->PollStatus());
	CHECK(core.AbandonServerWait(Client, client, ready).syscall_errno == Invalid);
	CHECK(core.SealServer(Server, server).syscall_errno == Busy);
	Dispatch dispatch;
	CHECK(!core.Receive(Server, server, dispatch).syscall_errno);
	CHECK(dispatch.type == Dispatch::Type::Connect && dispatch.caller == Client && dispatch.connect_argument == Bytes({1, 2, 3}));
	CHECK(!accepted->PollStatus() && core.Accept(Server, client, 0).syscall_errno == Invalid);
	Handle session = 0;
	CHECK(!core.CreateSession(Server, 0x2345, 0, session).syscall_errno);
	CHECK(!accepted->PollStatus());
	CHECK(!core.Accept(Server, client, -42).syscall_errno);
	CHECK(accepted->PollStatus()->method_result == -42);
	CHECK(!core.DestroyClient(Client, client).syscall_errno);
	CHECK(!core.DestroyServer(Server, server).syscall_errno);

	// A completed lookup is bound to one generation. Replacement must pass a
	// fresh authorization/lookup, never inherit the old service's permission.
	CHECK(!core.RegisterClient(Client, "SceAppDbIpcmsg", {}, client).syscall_errno);
	CHECK(!core.RegisterServer(Server, "SceAppDbIpcmsg", cfg, server).syscall_errno);
	CHECK(!core.WaitServer(Client, client, ready).syscall_errno);
	const auto old_server = server;
	CHECK(!core.DestroyServer(Server, server).syscall_errno);
	CHECK(!core.RegisterServer(Server, "SceAppDbIpcmsg", cfg, server).syscall_errno && server != old_server);
	CHECK(ready->PollStatus()->value == old_server);
	CHECK(core.Connection(Client, client, connection, old_server).syscall_errno == NotFound);
	CHECK(core.Connect(Client, client, accepted, {}, server, ready).syscall_errno == NotFound);
	CHECK(core.Connect(Client, client, accepted, {}, old_server, ready).syscall_errno == NotFound);
	CHECK(!core.AbandonServerWait(Client, client, ready).syscall_errno);
	CHECK(!core.WaitServer(Client, client, ready).syscall_errno && ready->PollStatus()->value == server);
	CHECK(!core.SealServer(Server, server).syscall_errno);
	CHECK(core.Connect(Client, client, accepted, {}, server, ready).syscall_errno == NotConnected);
	CHECK(!core.AbandonServerWait(Client, client, ready).syscall_errno);
	CHECK(!core.WaitServer(Client, client, ready).syscall_errno && !ready->PollStatus());
	CHECK(!core.DestroyClient(Client, client).syscall_errno && ready->PollStatus()->syscall_errno == Cancelled);
	CHECK(!core.DestroyServer(Server, server).syscall_errno);

	// Cancellation leaves no name waiter and permits fresh use of the client.
	CHECK(!core.RegisterClient(Client, "SceAppDbIpcmsg", {}, client).syscall_errno);
	CHECK(!core.WaitServer(Client, client, ready).syscall_errno);
	CHECK(!core.AbandonServerWait(Client, client, ready).syscall_errno && ready->PollStatus()->syscall_errno == Cancelled);
	CHECK(!core.WaitServer(Client, client, duplicate).syscall_errno && duplicate != ready);
	CHECK(core.AbandonServerWait(Client, client, ready).syscall_errno == Invalid);
	CHECK(!core.DestroyClient(Client, client).syscall_errno && duplicate->PollStatus()->syscall_errno == Cancelled);

	// Publication and waiter enrollment race under the same core mutex; neither
	// ordering may lose notification. No service handler is needed for readiness.
	for (int i = 0; i < 40; ++i) {
		Core race; Handle c = 0, s = 0;
		CHECK(!race.RegisterClient(Client, "race", {}, c).syscall_errno);
		std::atomic_bool start {false}; Ticket wait;
		std::thread publisher([&] {
			while (!start) { std::this_thread::yield(); }
			CHECK(!race.RegisterServer(Server, "race", 1, s).syscall_errno);
		});
		start = true;
		CHECK(!race.WaitServer(Client, c, wait).syscall_errno);
		const auto result = race.Wait(wait);
		publisher.join();
		CHECK(!result.syscall_errno && result.value == s && !result.method_result);
		CHECK(!race.DestroyClient(Client, c).syscall_errno && !race.DestroyServer(Server, s).syscall_errno);
	}
	Ticket abandoned;
	{
		Core temporary; Handle c = 0;
		CHECK(!temporary.RegisterClient(Client, "absent", {}, c).syscall_errno);
		CHECK(!temporary.WaitServer(Client, c, abandoned).syscall_errno);
	}
	CHECK(abandoned->PollStatus()->syscall_errno == Unsupported);
	std::cout << "IPMI server publication, generation and cancellation checks passed\n";
}
