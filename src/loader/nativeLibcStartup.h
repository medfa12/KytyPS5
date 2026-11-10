#pragma once

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Loader::NativeLibcStartup {

using Entry = KYTY_SYSV_ABI void (*)();
inline constexpr size_t LeafRva = 0x63f0;
inline constexpr size_t LeafSize = 0x62;

// Trusted readable host span, not a guest-pointer probe. offset is an offset
// into this span (ELF file offset is not necessarily equal to the native RVA).
// A match establishes only the98-byte leaf profile, not whole-provider identity.
bool MatchesLeafDigest(std::span<const uint8_t> readable, size_t offset,
                       std::span<const uint8_t, 32> expected) noexcept;
bool MatchesLeafProfile(std::span<const uint8_t> readable, size_t offset) noexcept;

enum class Result {
	Ok, Disabled, InvalidOwner, NullEndpoint, UnverifiedProfile,
	StartupAlreadyAdmitted, AlreadyBound, ReplacementRejected, AlreadyCompleted,
	Invoking, Invalidated, NotBound, InvalidInvocation, NotReturned, AlreadyInvoked,
	UnrelatedProvider,
};
enum class Phase { Fresh, Bound, Invoking, Completed, Invalidated };

class State;

// One-use handoff, kept alive by the caller through Complete. Invoke accesses
// only this ticket; call it outside the external loader lock. A returned ticket
// is the proof required by Complete; a callback's RAX is never interpreted.
class PreparedCall final {
public:
	PreparedCall() = default;
	PreparedCall(const PreparedCall&) = delete;
	PreparedCall& operator=(const PreparedCall&) = delete;
	PreparedCall(PreparedCall&&) = delete;
	PreparedCall& operator=(PreparedCall&&) = delete;
	Result Invoke(); // Deliberately does not catch/translate native faults or exceptions.

private:
	friend class State;
	const State* m_state = nullptr;
	int32_t m_owner = -1;
	Entry m_entry = nullptr;
	bool m_invoked = false, m_returned = false, m_completed = false;
};

// One per loader SESSION, not per Program/provider. No reset/rearm API and no
// own mutex. All State methods/queries require the caller's existing loader
// mutex (or equivalent exclusive serialization). Do not destroy while in flight.
class State final {
public:
	explicit State(std::string_view environment_value = {}) noexcept: m_enabled(environment_value == "1") {}
	State(const State&) = delete;
	State& operator=(const State&) = delete;
	State(State&&) = delete;
	State& operator=(State&&) = delete;

	// owner is Program::unique_id, never a guest PID. profile_verified means the
	// integration has verified the selected provider, endpoint and leaf profile.
	Result Bind(int32_t owner, Entry entry, bool profile_verified) noexcept;
	Result Prepare(PreparedCall& call) noexcept;
	Result Complete(PreparedCall& call) noexcept;
	// Record EVERY admitted initializer/application entry, including providers
	// later unloaded. Invoking rejects admission; otherwise history is sticky.
	Result RecordStartupAdmission() noexcept;
	// Must precede unmap. Invoking means unload admission is NOT safe: caller
	// must fail closed/defer and retry after completion, not unmap the ticket's
	// code. This is an internal integration result, not native EBUSY or a pin API.
	Result InvalidateProvider(int32_t owner) noexcept;

	bool Enabled() const noexcept { return m_enabled; }
	Phase GetPhase() const noexcept { return m_phase; }
	bool StartupAdmitted() const noexcept { return m_startup_admitted; }
	bool InvocationCompleted() const noexcept { return m_completed; }
	int32_t Owner() const noexcept { return m_owner; }

private:
	const bool m_enabled;
	Phase m_phase = Phase::Fresh;
	int32_t m_owner = -1;
	Entry m_entry = nullptr;
	PreparedCall* m_call = nullptr;
	bool m_startup_admitted = false, m_completed = false;
};

} // namespace Loader::NativeLibcStartup
