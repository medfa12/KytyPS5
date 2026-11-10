#include "loader/runtimeStartupBarrier.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::StartupBarrier {
#if defined(_WIN32)
namespace {
class Events final: public Backend {
public:
	bool Create(std::string_view token) noexcept override {
		// Fixed-width validated token keeps names short and excludes separators.
		std::snprintf(prefix_, sizeof(prefix_), "Local\\KytyPreGuest-%lu-%.*s",
		              static_cast<unsigned long>(GetCurrentProcessId()), static_cast<int>(token.size()), token.data());
		char name[128] {};
		std::snprintf(name, sizeof(name), "%s-ready", prefix_);
		ready_ = CreateEventA(nullptr, TRUE, FALSE, name);
		if (!ready_ || GetLastError() == ERROR_ALREADY_EXISTS) { return false; }
		std::snprintf(name, sizeof(name), "%s-release", prefix_);
		release_ = CreateEventA(nullptr, TRUE, FALSE, name);
		return release_ && GetLastError() != ERROR_ALREADY_EXISTS;
	}
	bool PublishReady() noexcept override {
		// Mapping/relocation output must be observable before the ready signal.
		std::fprintf(stdout, "pre-guest-barrier: ready pid=%lu timeout_ms=%u phase=relocated-before-guest names=%s\n",
		             static_cast<unsigned long>(GetCurrentProcessId()), TimeoutMilliseconds, prefix_);
		if (std::fflush(nullptr) != 0) { return false; }
		return SetEvent(ready_) != FALSE;
	}
	WaitResult Wait(unsigned milliseconds) noexcept override {
		switch (WaitForSingleObject(release_, milliseconds)) {
			case WAIT_OBJECT_0: return WaitResult::Released;
			case WAIT_TIMEOUT: return WaitResult::TimedOut;
			default: return WaitResult::Failed;
		}
	}
	void Close() noexcept override {
		if (release_) { CloseHandle(release_); release_ = nullptr; }
		if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
	}
private:
	HANDLE ready_ = nullptr;
	HANDLE release_ = nullptr;
	char prefix_[96] {};
};
} // namespace
#endif

Result FromEnvironment() {
#if defined(_WIN32)
	char token[34] {};
	SetLastError(ERROR_SUCCESS);
	const DWORD size = GetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", token, sizeof(token));
	if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) { return Result::Disabled; }
	if (size != 32) { return Result::InvalidToken; }
	// Consume in this host so later child launches cannot accidentally inherit
	// a diagnostic stop intended only for this startup.
	if (!SetEnvironmentVariableA("KYTY_PRE_GUEST_GATE_TOKEN", nullptr)) { return Result::SetupFailed; }
	Events events;
	return Run(events, true, std::string_view(token, size));
#else
	return std::getenv("KYTY_PRE_GUEST_GATE_TOKEN") ? Result::SetupFailed : Result::Disabled;
#endif
}
} // namespace Loader::StartupBarrier
