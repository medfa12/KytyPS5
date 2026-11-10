#pragma once

#include "libs/npManagerClient.h"
#include "libs/npNativeTransport.h"
#include <chrono>
#include <functional>

namespace Libs::NpNativeManagerEndpoint {

class Coordinator;
// Mandatory runtime-owned hooks. They must be nonblocking, nonreentrant and must
// not invoke guest code. identity reads CURRENT guest identity, never a saved ID.
struct Hooks {
	std::shared_ptr<void> owner;
	std::function<LibKernel::Ipmi::Identity()> identity;
	std::function<bool()> owner_alive;
	// Install in the runtime's durable retirement registry BEFORE Create returns.
	// Keep this strong reference until Done(), including after owner loss. A lost
	// guest owner cannot be impersonated: unresolved resources block shutdown.
	std::function<bool(std::shared_ptr<Coordinator>)> retain;
	// KernelOpenEventFlag / KernelCloseEventFlag adapters, returning guest errors.
	// open must preserve a nonzero alias on any failure requiring close retry.
	std::function<int(const char*, uintptr_t&)> open_flag;
	// KernelPollEventFlag(alias, 1ull << channel, 0x22, &observed).
	// Translate kernel Busy to 1; 0 = signaled; negative = kernel error.
	std::function<int(uintptr_t, uint32_t)> poll_flag;
	std::function<int(uintptr_t)> close_flag;
	// Optional request-completion hooks, supplied together. Creation uses attr
	// 0x121 and initial bits 0. Preserve a nonzero creator on cleanup failure.
	std::function<int(const char*, uintptr_t&)> create_request_flag;
	// KernelClearEventFlag takes the bits to KEEP, not the selected bits to clear.
	std::function<int(uintptr_t, uint64_t)> clear_request_flag;
	// KernelPollEventFlag(creator, mask, 0x21); Busy -> 1, signal -> 0.
	std::function<int(uintptr_t, uint64_t)> poll_request_flag;
	std::function<int(uintptr_t)> delete_request_flag;
};

class Coordinator final {
public:
	~Coordinator();
	// Only the durable, actual guest owner calls Advance. No thread is created.
	// First turn registers the current PID/TID; all subsequent turns verify it.
	// Returns BSD errno; WouldBlock/Busy are retained progress, not cancellation.
	int Advance() noexcept;
	// Lock-free request only; actual cleanup remains on Advance's guest owner.
	void RequestRetirement() noexcept;
	// First terminal cause, independent of native cleanup progress. A timed-out
	// connect can retain a native ticket (Advance returns Busy/WouldBlock) long
	// after it has ceased to be a candidate for attachment. Lock-free so observers
	// never wait on Advance's mutex or impersonate its guest owner to drain it.
	int RetirementError() const noexcept;
	bool Done() const noexcept;
	// Atomic scheduler snapshot, excluding the active native operation. Never use
	// this potentially stale observation as an admission grant.
	size_t QueuedJobs() const noexcept;
	struct State;
private:
	explicit Coordinator(std::shared_ptr<State> state);
	std::shared_ptr<State> m;
	friend int Create(Hooks, std::shared_ptr<NpManagerClient::Endpoint>&,
	                  std::shared_ptr<Coordinator>&, std::chrono::milliseconds) noexcept;
};

// Asynchronous connect starts on Advance; attach only once Connected() is true.
// Timeout is a finite HOST observation deadline (1..1000ms), not native timeout
// ABI. Deadline expiry revokes the whole endpoint; the original ticket drains
// on its original actual guest worker. No autoattach or callback pump here.
// Public admission waits for lock contention within that same host deadline;
// expiry before publication submits nothing and does not revoke the connection.
// Completion/deadline observation never reacquires the coordinator lock;
// Connected is a lock-free lifecycle snapshot, not an admission grant, and
// Disconnect only requests retirement. A full job queue still returns Busy.
int Create(Hooks hooks, std::shared_ptr<NpManagerClient::Endpoint>& endpoint,
           std::shared_ptr<Coordinator>& coordinator,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) noexcept;

} // namespace Libs::NpNativeManagerEndpoint
