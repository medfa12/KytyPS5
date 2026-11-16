// Isolated lifecycle regression. Compile the actual production Coordinator and
// runtime Run policy in this TU. A scheduling-only Retained test backend never
// accepts a connection, produces a service result, or invokes guest code.
#include "loader/runtimeNpManagerClient.cpp"
#include "libs/npNativeManagerEndpoint.cpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace Test {
namespace I = Libs::LibKernel::Ipmi;
namespace E = Libs::NpNativeManagerEndpoint;
bool pending = true;
int retire_result = I::Busy;
unsigned retired_calls = 0, method_calls = 0;
void Require(bool value, const char* message) {
	if (!value) { std::fprintf(stderr, "NP coordinator regression: %s\n", message); std::exit(3); }
}
}

namespace Libs::NpNativeTransport::Retained {
int Register(Connection& output) noexcept { output = 1; return 0; }
int BeginConnect(Connection c, const LibKernel::Ipmi::Bytes&) noexcept { Test::Require(c == 1, "connection token"); return 0; }
int PollConnect(Connection, int32_t&) noexcept { return Test::I::WouldBlock; } // never accepts
int Retire(Connection) noexcept { ++Test::retired_calls; return Test::pending ? Test::retire_result : 0; }
int BeginInvoke(Connection, uint32_t, const std::vector<LibKernel::Ipmi::Bytes>&, const std::vector<uint64_t>&) noexcept {
	++Test::method_calls; return Test::I::Invalid;
}
int PollInvoke(Connection) noexcept { ++Test::method_calls; return Test::I::Invalid; }
int ConsumeInvoke(Connection, LibKernel::Ipmi::Result&) noexcept { ++Test::method_calls; return Test::I::Invalid; }
}

int main(int argc, char** argv) {
	using namespace Test;
	const bool replay_old = argc == 2 && std::string_view(argv[1]) == "--old-policy";
	for (int cleanup : {I::Busy, I::WouldBlock}) {
		pending = true; retire_result = cleanup; retired_calls = method_calls = 0;
		auto run = std::make_shared<Loader::RuntimeNpManagerClient::Run>();
		std::shared_ptr<E::Coordinator> retained;
		E::Hooks hooks;
		hooks.owner = std::make_shared<int>(1);
		hooks.identity = [] { return I::Identity {2, 3}; };
		hooks.owner_alive = [] { return true; };
		hooks.retain = [&](auto c) { retained = c; return true; };
		hooks.open_flag = [](const char*, uintptr_t&) { return I::Invalid; };
		hooks.poll_flag = [](uintptr_t, uint32_t) { return 1; };
		hooks.close_flag = [](uintptr_t) { return 0; };
		std::shared_ptr<Libs::NpManagerClient::Endpoint> endpoint;
		std::shared_ptr<E::Coordinator> coordinator;
		Require(E::Create(std::move(hooks), endpoint, coordinator, std::chrono::milliseconds(1)) == 0, "create coordinator");
		run->coordinator.store(coordinator); run->endpoint.store(endpoint);
		Require(coordinator == retained && !endpoint->Connected(), "durable unpublished endpoint");
		Require(coordinator->Advance() == I::WouldBlock, "begin actual Coordinator connect path");
		Require(coordinator->RetirementError() == 0 && !run->stop.load(), "pending alone is not failure");
		run->ObserveCoordinatorProgress(I::WouldBlock, coordinator);
		Require(!run->stop.load() && run->error.load() == 0, "do not fail a healthy pending connection");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const int progress = coordinator->Advance();
		Require(progress == cleanup && !coordinator->Done() && retired_calls > 0, "timeout retains original pending connection");
		Require(coordinator->RetirementError() == I::TimedOut, "real Coordinator deadline latched timeout");
		if (replay_old) {
			// Exact former production decision: Busy/WouldBlock hid terminal error.
			if (progress && progress != I::WouldBlock && progress != I::Busy) { run->Fail(progress); }
		} else { run->ObserveCoordinatorProgress(progress, coordinator); }
		Require(run->stop.load() && run->error.load() == I::TimedOut, "terminal timeout was not propagated");
		Require(!run->attached.load() && run->coordinator.load() == coordinator && run->endpoint.load() == endpoint,
		        "consumer failure must not drop retained workers or endpoint");
		Require(!coordinator->Done() && !endpoint->Connected() && method_calls == 0, "no cleanup or service success fabricated");
		Require(coordinator->Advance() == cleanup && !coordinator->Done(), "retirement continues after consumer failure");
		pending = false;
		Require(coordinator->Advance() == 0 && coordinator->Done(), "late cleanup completes on original coordinator");
		run->ObserveCoordinatorProgress(0, coordinator);
		Require(run->error.load() == I::TimedOut && coordinator->RetirementError() == I::TimedOut, "first cause remains sticky");
		std::printf("timeout propagated with cleanup=%d; retained until original cleanup; no service calls\n", cleanup);
	}
	// A parent-requested stop must not become an invented worker failure.
	{
		pending = true;
		auto run = std::make_shared<Loader::RuntimeNpManagerClient::Run>();
		std::shared_ptr<E::Coordinator> coordinator;
		E::Hooks hooks;
		hooks.owner = run; hooks.identity = [] { return I::Identity {2, 3}; };
		hooks.owner_alive = [] { return true; }; hooks.retain = [](auto) { return true; };
		hooks.open_flag = [](const char*, uintptr_t&) { return I::Invalid; };
		hooks.poll_flag = [](uintptr_t, uint32_t) { return 1; }; hooks.close_flag = [](uintptr_t) { return 0; };
		std::shared_ptr<Libs::NpManagerClient::Endpoint> endpoint;
		Require(E::Create(std::move(hooks), endpoint, coordinator, std::chrono::milliseconds(1000)) == 0, "normal stop coordinator");
		run->coordinator.store(coordinator); run->endpoint.store(endpoint);
		run->Stop(); run->ObserveCoordinatorProgress(I::Busy, coordinator);
		Require(run->stop.load() && run->error.load() == 0, "planned stop incorrectly promoted to worker error");
		pending = false; Require(coordinator->Advance() == 0 && coordinator->Done(), "normal retirement");
		run->coordinator.store({}); run->endpoint.store({}); // break test-only owner cycle
	}
	std::puts("NP coordinator terminal/progress distinction passed");
}
