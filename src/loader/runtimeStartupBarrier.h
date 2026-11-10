#pragma once

#include <string_view>

namespace Loader::StartupBarrier {

enum class Result { Disabled, Released, InvalidToken, SetupFailed, PublishFailed, TimedOut, WaitFailed };
enum class WaitResult { Released, TimedOut, Failed };
constexpr unsigned TimeoutMilliseconds = 120000;

// Injectable policy for offline tests. A backend owns all created resources and
// Close must also handle partially successful Create. No guest code is called.
struct Backend {
	virtual ~Backend() = default;
	virtual bool Create(std::string_view token) noexcept = 0;
	virtual bool PublishReady() noexcept = 0;
	virtual WaitResult Wait(unsigned milliseconds) noexcept = 0;
	virtual void Close() noexcept = 0;
};

inline Result Run(Backend& backend, bool selected, std::string_view token) {
	if (!selected) { return Result::Disabled; }
	if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
		return Result::InvalidToken;
	}
	struct Cleanup {
		Backend& backend;
		~Cleanup() { backend.Close(); }
	} cleanup {backend};
	if (!backend.Create(token)) { return Result::SetupFailed; }
	if (!backend.PublishReady()) { return Result::PublishFailed; }
	switch (backend.Wait(TimeoutMilliseconds)) {
		case WaitResult::Released: return Result::Released;
		case WaitResult::TimedOut: return Result::TimedOut;
		default: return Result::WaitFailed;
	}
}

// Opt-in host environment gate. Disabled performs no event creation or wait.
Result FromEnvironment();

} // namespace Loader::StartupBarrier
