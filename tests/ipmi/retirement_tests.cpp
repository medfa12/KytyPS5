// White-box failure injection: exercise the production rollback path with a
// real Core reservation. No production fault-injection API is exposed.
#include "libs/loginMgrNativeTransport.cpp"

#include <cstdlib>
#include <iostream>

namespace L = Libs::LoginMgrNativeTransport;
using namespace Libs::LibKernel::Ipmi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
constexpr int GuestError(unsigned n) { return static_cast<int>(0x80020000u | n); }

int main() {
	Identity identity {100, 1};
	L::Hooks hooks;
	auto pin = std::make_shared<int>(1); std::weak_ptr<int> retained = pin;
	hooks.owner = pin; pin.reset();
	hooks.identity = [&] { return identity; };
	hooks.copy_in = [](uint64_t p, void* out, size_t n) { std::memcpy(out, reinterpret_cast<void*>(p), n); return 0; };
	hooks.copy_out = [](uint64_t, const void*, size_t) { return 14; };
	hooks.authorize_registration = [](Identity, bool) { return 0; };
	hooks.authorize_connect = [](Identity, const ConnectionInfo&) { return 0; };
	hooks.invoke_guest = [](uint64_t, const uint64_t*, size_t, int64_t&) { return 0x4e; };
	hooks.cooperate = [] { return 0x4e; };
	CHECK(L::Install(std::move(hooks)) == 0);
	auto runtime = L::Current();
	std::array<uint64_t, 6> backing {}; void* output = reinterpret_cast<void*>(0x1234);
	Handle handle = 0;
	{
		L::Publication publication(runtime, ObjectKind::Server, backing.data());
		CHECK(runtime->core.RegisterServer(identity, L::ServiceName, 1, publication.object->handle).syscall_errno == 0);
		handle = publication.object->handle;
		CHECK(runtime->core.Wake(identity, handle).syscall_errno == 0);
		Dispatch pending;
		CHECK(runtime->core.BeginReceive(identity, handle, pending).syscall_errno == 0);
		CHECK(runtime->core.SealServer(identity, handle).syscall_errno == Busy);
		try { publication.Commit(&output, L::ServerTable(), 0x30); CHECK(false); }
		catch (const L::Fault& fault) { CHECK(fault.error == 14); }
	} // DestroyServer fails Busy: rollback must retain the address and owner.
	CHECK(output == reinterpret_cast<void*>(0x1234));
	CHECK(runtime->objects.size() == 1 && runtime->objects.begin()->second->retirement);
	CHECK(!runtime->objects.begin()->second->published);
	auto original = runtime->objects.begin()->second;
	try { L::Publication duplicate(runtime, ObjectKind::Server, backing.data()); CHECK(false); }
	catch (const L::Fault& fault) { CHECK(fault.error == Busy); }
	CHECK(runtime->objects.begin()->second == original);
	CHECK(L::RetryRetirements() == GuestError(Busy));
	CHECK(runtime->core.EndReceive(identity, handle, false).syscall_errno == 0);
	identity = {200, 1}; CHECK(L::RetryRetirements() == GuestError(Busy));
	runtime.reset(); original.reset();
	CHECK(L::Uninstall() == GuestError(Busy) && !retained.expired());
	identity = {100, 2}; CHECK(L::RetryRetirements() == 0);
	// A failed session release additionally requires the original receiver,
	// even when a different thread has the same owning PID.
	runtime = L::Current();
	Handle server = 0, client = 0;
	CHECK(runtime->core.RegisterServer(identity, L::ServiceName, 1, server).syscall_errno == 0);
	CHECK(runtime->core.RegisterClient({200, 1}, L::ServiceName, Config {}, client).syscall_errno == 0);
	Ticket connect;
	CHECK(runtime->core.Connect({200, 1}, client, connect).syscall_errno == 0);
	Dispatch dispatch;
	CHECK(runtime->core.Receive(identity, server, dispatch).syscall_errno == 0);
	{
		L::Publication publication(runtime, ObjectKind::Session, backing.data());
		publication.object->parent = server;
		CHECK(runtime->core.CreateSession(identity, L::Address(backing.data()), 0, publication.object->handle, server).syscall_errno == 0);
		CHECK(runtime->core.Accept(identity, client, 0).syscall_errno == 0);
		try { publication.Commit(&output, L::SessionTable(), 0x10); CHECK(false); }
		catch (const L::Fault& fault) { CHECK(fault.error == 14); }
	}
	CHECK(L::RetryRetirements() == GuestError(Unsupported));
	CHECK(runtime->core.ForceTeardown({200, 1}, client).syscall_errno == 0);
	CHECK(runtime->core.Receive(identity, server, dispatch).syscall_errno == 0);
	identity = {100, 3}; CHECK(L::RetryRetirements() == GuestError(Busy));
	identity = {100, 2}; CHECK(L::RetryRetirements() == 0);
	CHECK(runtime->core.DestroyClient({200, 1}, client).syscall_errno == 0);
	CHECK(runtime->core.DestroyServer(identity, server).syscall_errno == 0);
	runtime.reset();
	CHECK(L::Uninstall() == 0 && retained.expired());
	std::cout << "Failed rollback retains backing/owner until authorized retry succeeds\n";
}
