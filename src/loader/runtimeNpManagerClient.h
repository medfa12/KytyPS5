#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>

namespace Libs::NpManagerClient { class Client; }

namespace Loader {
struct Program;

// Owner-bound query integration. Callback registration/pumping still requires
// a separately admitted guest callback boundary and is not exposed here.
namespace RuntimeNpManagerClient {
using Token = uint64_t;
struct Status {
	uint64_t transport_epoch = 0;
	int owner_pid = -1;
	int transport_tid = -1, delivery_tid = -1;
	bool attached = false, stop_requested = false, owner_lost = false;
	bool transport_returning = false, delivery_returning = false;
	bool coordinator_done = false;
	// Guest result from Client::Attach; zero is meaningful only with attached.
	int attachment_result = 0;
	// BSD errno from worker admission/transport setup; zero means no such error.
	int worker_error = 0;
};

// BSD errno throughout this control API. Start requires an ACTUAL guest pthread
// belonging to selected_owner, with that program current and source-bound live
// Core/UI credentials. Legacy cohosts only; application-owned execution is not
// admitted by this slice. One owner including its retiring workers at a time.
// Requires a live OPEN source-bound transport epoch. Closed epochs return Busy
// even after their selected registry has drained; bare native Install fixtures
// do not establish source-bound client admission.
// output is untouched on error. Duplicate/competing starts return Busy.
int StartForCurrentOwner(Program* selected_owner, Token& output) noexcept;
// Retains the run AND loader-clear gate through the query. An eligible actual
// owner may start attachment lazily and waits for its actual attach/error result,
// as the native accessor does. Unavailable/retiring/wrong-owner callers receive
// null; epoch closure interrupts a starting wait. Never falls back to Session().
// This is not a wall-clock bounded API: worker creation/native stalls can prevent
// the worker-owned deadlines from advancing. No guest timeout is invented.
std::shared_ptr<Libs::NpManagerClient::Client> AcquireQueryClient() noexcept;
// Explicit local-profile HLE selection. nullopt means native routing remains
// selected. Never reports SignedIn and never falls back from a native failure.
std::optional<int> QueryLocalSigninState(int user, uint32_t* output);
int InspectForCurrentOwner(Token token, Status& output) noexcept;

// Host lifecycle APIs: may request stop/reap without assuming a guest identity.
// Retirement closes public attachment immediately, but leaves the original
// transport worker alive to drain real pending acceptance/replies/flag aliases.
int RequestRetirement(Token token) noexcept;
// Owner teardown, distinct from retiring an individual client: permanently close
// the EXPECTED source-bound transport epoch, request stop on its selected run,
// then perform one Reap pass. Only the actual admitted transport Core owner may
// initiate/retry this API (the selected client may belong to UI). Zero means the
// registry is empty, NOT that native server stop/term/uninstall has happened.
// Closing survives empty registry and failed Uninstall. Native work continues on
// the original workers. Stale epochs cannot touch replacement runs.
int CloseAndReapForCurrentTransportOwner(uint64_t expected_epoch) noexcept;
// One bounded observation pass. WouldBlock retains the registry. Join is called
// only after BOTH workers' actual duplicated OS handles are signaled. Busy means
// another reap owns the pass. Success removes the token; no token is reused.
int Reap(Token token) noexcept;
bool HasLive() noexcept;

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
// Scheduling-only fixture seam. It cannot supply credentials, replies or results.
// Install/remove only with no client workers or consumers live.
enum class TestPoint { TransportCreate, TransportAdvance, ConsumerWaiting, ConsumerBeforeGrant };
using TestObserver = void (*)(TestPoint, Token, size_t queued_jobs);
void SetTestObserver(TestObserver observer) noexcept;
#endif

// INTEGRATION PRECONDITION: gate global RuntimeLinker::Clear and transport
// Uninstall while HasLive(). Retain the selected Program/mappings and credential
// identity through successful Reap; BootCredentialLifetime alone is NOT a mapping
// lease. Existing admitted-image unload gates still apply. Stop before revoking
// that identity. On actual owner loss, retain unresolved state and fail closed;
// never substitute a different thread to retire the original connection.
// Start and source-bound transport retirement serialize registry publication
// with RuntimeLinker::Clear under the loader mutex. The loader also confirms that
// the selected Program is still loaded before any dereference/admission.
} // namespace RuntimeNpManagerClient
} // namespace Loader
