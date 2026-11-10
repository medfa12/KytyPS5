#include "loader/runtimeStartupBarrier.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

using namespace Loader::StartupBarrier;
struct Fake final: Backend {
	bool create = true, publish = true;
	WaitResult wait = WaitResult::Released;
	int creates = 0, publishes = 0, waits = 0, closes = 0;
	bool Create(std::string_view) noexcept override { ++creates; return create; }
	bool PublishReady() noexcept override { ++publishes; return publish; }
	WaitResult Wait(unsigned duration) noexcept override { assert(duration == 120000); ++waits; return wait; }
	void Close() noexcept override { ++closes; }
};

int main() {
	constexpr auto token = "0123456789abcdef0123456789abcdef";
	{
		Fake f;
		assert(Run(f, false, "bad") == Result::Disabled);
		assert(f.creates == 0 && f.closes == 0 && f.waits == 0);
		for (auto bad: {"", "a", "0123456789abcdef0123456789abcdeg", "0123456789abcdef0123456789abcdef0"}) {
			assert(Run(f, true, bad) == Result::InvalidToken);
		}
		assert(f.creates == 0);
	}
	{
		Fake f; f.create = false;
		assert(Run(f, true, token) == Result::SetupFailed);
		assert(f.closes == 1 && f.publishes == 0 && f.waits == 0);
	}
	{
		Fake f; f.publish = false;
		assert(Run(f, true, token) == Result::PublishFailed);
		assert(f.closes == 1 && f.waits == 0);
	}
	for (auto wait: {WaitResult::Released, WaitResult::TimedOut, WaitResult::Failed}) {
		Fake f; f.wait = wait;
		const auto result = Run(f, true, token);
		assert(result == (wait == WaitResult::Released ? Result::Released :
		                  wait == WaitResult::TimedOut ? Result::TimedOut : Result::WaitFailed));
		assert(f.creates == 1 && f.publishes == 1 && f.waits == 1 && f.closes == 1);
	}
#if defined(_WIN32)
	// Self-process events only. No emulator, debugger, guest or child process.
	SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", nullptr);
	assert(FromEnvironment() == Result::Disabled);
	SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", "invalid");
	assert(FromEnvironment() == Result::InvalidToken);
	SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", token);
	const std::string prefix = "Local\\KytyPreGuest-" + std::to_string(GetCurrentProcessId()) + "-" + token;
	const auto ready_name = prefix + "-ready", release_name = prefix + "-release";
	{
		HANDLE stale = CreateEventA(nullptr, TRUE, TRUE, ready_name.c_str());
		assert(stale);
		assert(FromEnvironment() == Result::SetupFailed);
		CloseHandle(stale);
		SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", token);
		stale = CreateEventA(nullptr, TRUE, TRUE, release_name.c_str());
		assert(stale);
		assert(FromEnvironment() == Result::SetupFailed);
		assert(OpenEventA(SYNCHRONIZE, FALSE, ready_name.c_str()) == nullptr);
		CloseHandle(stale);
	}
	std::atomic<bool> passed {false};
	SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", token);
	Result result = Result::WaitFailed;
	std::thread worker([&] { result = FromEnvironment(); passed = true; });
	HANDLE ready = nullptr;
	const auto deadline = GetTickCount64() + 5000;
	while (!ready && GetTickCount64() < deadline) {
		ready = OpenEventA(SYNCHRONIZE, FALSE, ready_name.c_str());
		if (!ready) { Sleep(1); }
	}
	assert(ready && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
	assert(!passed.load());
	HANDLE release = OpenEventA(EVENT_MODIFY_STATE, FALSE, release_name.c_str());
	assert(release && SetEvent(release));
	worker.join();
	assert(result == Result::Released && passed.load());
	CloseHandle(release); CloseHandle(ready);
	assert(OpenEventA(SYNCHRONIZE, FALSE, ready_name.c_str()) == nullptr);
	assert(OpenEventA(SYNCHRONIZE, FALSE, release_name.c_str()) == nullptr);
	SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", nullptr);
#endif
	std::puts("StartupBarrier: disabled/invalid, partial setup, publication failure, bounded wait outcomes, stale events and same-process release/cleanup passed");
}
