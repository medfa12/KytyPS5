#pragma once

#include <cstdint>
#include <memory>

namespace Loader::RuntimeNpQuiescence {
// Disabled unless explicitly called by a host fixture/integration. IPC ONLY:
// neither NativeExited nor Finished certifies callback/producer quiescence.
class State;
using OwnedState = std::shared_ptr<State>;
enum class Progress { Pending, NativeExited };
// BSD errno; outputs unchanged on error. Begin requires an empty output and the
// actual admitted Core creator pthread. Closing the epoch is irreversible.
// Exactly one successful Begin per installed generation; duplicates return Busy,
// including after Finish. Registry strongly retains an active state until Finish.
// Losing all public handles fails closed (pins prevent retirement); no recovery
// API is provided. Failed Begin does not consume the generation reservation.
int Begin(uint64_t epoch, uint64_t native_server_address, uint64_t expected_handle, OwnedState& output) noexcept;
// One bounded pass; never waits for running threads or invokes guest code.
// Client drain may join exited client workers and reclaim their stacks. Never
// joins the native DISPATCHER or destroys the native server.
int Advance(const OwnedState& state, Progress& output) noexcept;
// Caller supplies the ORIGINAL operation's actual returned result, not a retry
// or an aggregate term result that hides inner failures. These only observe and
// consume evidence, never perform/replay native cleanup or rewrite its result.
// Each success is one-shot. Errors leave the continuation stage unconsumed.
int ObserveJoin(const OwnedState& state, int actual_join_result) noexcept;
int ObserveDestroy(const OwnedState& state, int actual_destroy_result) noexcept;
// Releases this handle only after join + exact destroy evidence. All copies must
// be released before transport retirement; no automatic retirement in destructor.
int Finish(OwnedState& state) noexcept;
}
