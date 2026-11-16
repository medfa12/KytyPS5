#pragma once

#include "common/abi.h"
#include "kernel/ipmi.h"

namespace Libs::NpNativeTransport {

// Host provider receipt, not a guest pthread pointer or a native global. The
// provider pins one actual thread incarnation; generation is nonzero/non-reused.
// Token destruction must defer reclamation, never synchronously unmap or invoke
// guest code. A retained ServerLease keeps this token even after native destroy.
struct DispatcherReceipt {
	uint64_t generation = 0;
	LibKernel::Ipmi::Identity identity {};
	std::shared_ptr<void> token;
};
enum class DispatcherJoinOrigin { None, Explicit, DetachedCollector, StartupRollback };
struct DispatcherProof {
	bool native_exited = false;
	bool successfully_joined = false;
	// Actual successful join/collector identity, supplied by the provider, never
	// the inspecting caller. Zero until joined; positive PID/TID when available,
	// {-1,-1} when the actual joiner has no retained guest identity.
	LibKernel::Ipmi::Identity join_source {};
	DispatcherJoinOrigin join_origin = DispatcherJoinOrigin::None;
};

// Runtime integration is explicit. All hooks return BSD errno, not guest error
// words, and run without adapter/Core locks. Retain the installed owner until
// Uninstall succeeds. No identity, authorization or guest-execution fallback.
struct Hooks {
	std::shared_ptr<void> owner;
	std::function<LibKernel::Ipmi::Identity()> identity;
	std::function<int(uint64_t, void*, size_t)> copy_in;
	std::function<int(uint64_t, const void*, size_t)> copy_out;
	std::function<int(LibKernel::Ipmi::Identity, bool)> authorize_registration;
	std::function<int(LibKernel::Ipmi::Identity, const LibKernel::Ipmi::ConnectionInfo&)> authorize_connect;
	// Execute exactly one handler through the runtime's pinned guest boundary,
	// on the calling dispatcher guest thread. args include the native `this`.
	// Do not retain args, borrowed buffers, or the callback's result reference.
	std::function<int(uint64_t, const uint64_t*, size_t, int64_t&)> invoke_guest;
	// Cooperative wait only. MUST NOT dispatch service handlers on client threads.
	std::function<int()> cooperate;
	// Production integration must pin the containing allocation and backing
	// generation before native storage/work publication. Optional only for isolated
	// host fixtures whose storage lifetime is externally guaranteed. A successful
	// acquisition supplies a nonempty lease; destruction must not run guest code
	// or unmap synchronously under adapter locks.
	std::function<int(uint64_t, size_t, std::shared_ptr<void>&)> retain_storage;
	// Production dispatchers retain the adapter/provider on their actual guest
	// pthread until successful native join, including the epilogue after Run.
	// Optional only in isolated fixtures with externally guaranteed code lifetime.
	std::function<int(std::shared_ptr<void>)> retain_dispatch_thread;
	// Install both or neither. Mandatory for production receipt observation;
	// absent only in isolated fixtures (which report Unbound, never exit/join).
	// Capture on the ACTUAL Run caller and retain Runtime through native join.
	// Repeated capture on that same incarnation returns the same generation AND
	// shared token ownership. Both hooks are bounded, host-only, outside locks.
	std::function<int(std::shared_ptr<void>, DispatcherReceipt&)> capture_dispatch_thread;
	std::function<int(const DispatcherReceipt&, DispatcherProof&)> inspect_dispatch_thread;
};
int Install(Hooks hooks) noexcept;
int Uninstall() noexcept; // Busy while any object or API call remains live.
// Retry failed factory rollback on the actual owning guest identity; Busy
// retains backing/owner when its receiver or another live resource is needed.
int RetryRetirements() noexcept;
bool Installed() noexcept;

// NP-only host lifecycle observation. No guest callbacks, cooperate, cancellation,
// join, or synthesized native-exit proof. All statuses below are BSD errno.
namespace Lifecycle {
class ServerState;
using ServerLease = std::shared_ptr<const ServerState>;
struct ServerSnapshot {
	uint64_t native_address = 0;
	LibKernel::Ipmi::Handle handle = 0;
	bool published = false;
	bool destroyed = false; // Exact successful Core destroy AND registry erasure only.
	bool stopping = false;
	bool dispatching = false;
	bool stop_succeeded = false; // Prior successful native Stop, not a rewritten Busy result.
};
struct StopResult {
	int stop_errno = 0; // Actual native Stop semantics, including Busy when already stopping.
	ServerSnapshot snapshot;
};
enum class DispatcherBinding { Unbound, Bound };
struct DispatcherSnapshot {
	DispatcherBinding binding = DispatcherBinding::Unbound;
	DispatcherReceipt receipt;
	DispatcherProof proof;
};
// expected_handle is the admitted native object's handle (+8), not a fresh
// lookup by address. Both must match one currently published server generation.
// The actual current PID/TID must be its creator, and must remain identical on
// every operation. Output arguments are unchanged on host-API failure.
int AcquireServer(uint64_t native_address, LibKernel::Ipmi::Handle expected_handle, ServerLease& output) noexcept;
int InspectServer(const ServerLease& lease, ServerSnapshot& output) noexcept;
// Exact lease/creator validation as InspectServer, including destroyed objects.
// Unbound is explicit and is never native-exit/join evidence. Provider proof is
// validated monotonically; regression/inconsistent join evidence is BadMessage.
// All failures (including contention/provider Busy) preserve output. Hooks run
// without adapter locks and may reenter other host lifecycle observations.
int InspectDispatcher(const ServerLease& lease, DispatcherSnapshot& output) noexcept;
// Return 0 means Stop was attempted and output is valid, NOT that Stop succeeded.
// Adapter lock contention returns Busy without attempting Stop. Core Stop uses
// its existing bounded host synchronization, never waits for IPC or invokes hooks.
int RequestStop(const ServerLease& lease, StopResult& output) noexcept;
}

struct ServerConfig {
	uint64_t input_size, output_size;
	void* handler;
	uint8_t cross_no_cap;
	char name[25];
	uint8_t multi, restricted;
};
static_assert(sizeof(ServerConfig) == 0x38 && offsetof(ServerConfig, multi) == 0x32);
struct DataInfo { const void* data; uint64_t size; };
struct BufferInfo { void* data; uint64_t capacity, written; };
static_assert(sizeof(DataInfo) == 16 && sizeof(BufferInfo) == 24);

// Exact NP factory route; no automatic HLE service registration. Native object
// sizes are server0x30/client0x10/session0x10, written in caller-owned storage.
int KYTY_SYSV_ABI CreateServer(void** out, const ServerConfig* config, void* argument, void* storage) noexcept;
int KYTY_SYSV_ABI CreateClient(void** out, const void* config, void* argument, void* storage) noexcept;

// Private host transactions, NOT native vtable/timeout ABI. Every call returns
// BSD errno (WouldBlock means pending), and uses the actual current guest
// PID/TID. Only the registering thread may advance this connection. No call
// waits for IPC completion, cooperates, invokes guest code, or retains borrowed buffers.
namespace Retained {
using Connection = uint64_t;
// One retained manager connection per runtime (including retiring connections).
// Tokens are never reused across installations. Fixed manager negotiation:
// one sync slot, input0x2000/output0x4000, no
// async slots/queues/IPMI events. Notification uses a separate named flag.
int Register(Connection& output) noexcept;
int BeginConnect(Connection connection, const LibKernel::Ipmi::Bytes& argument = {}) noexcept;
int PollConnect(Connection connection, int32_t& acceptance) noexcept;
int BeginInvoke(Connection connection, uint32_t method,
                const std::vector<LibKernel::Ipmi::Bytes>& inputs,
                const std::vector<uint64_t>& capacities) noexcept;
// Observation only: no output payload copies or consumption. Repeated polls
// observe the same request. BeginInvoke remains Busy until ConsumeInvoke.
int PollInvoke(Connection connection) noexcept;
int ConsumeInvoke(Connection connection, LibKernel::Ipmi::Result& output) noexcept;
int BeginDisconnect(Connection connection) noexcept;
int PollDisconnect(Connection connection, int32_t& result) noexcept;
// Abandons public observation and advances retirement by one bounded step.
// Connecting cannot be cancelled: late rejection is destroyed; late acceptance
// is disconnected. Invokes retain their slot until drained (or supported forced
// teardown after a terminal transport failure). Busy/WouldBlock retains ALL
// backing and the installed runtime pin. Retry on the SAME actual guest thread.
// Zero means destroyed; subsequent calls with that token return NotFound.
int Retire(Connection connection) noexcept;
// Retry only previously requested retirements owned by the current thread.
// Foreign-owner retirements remain retained and cause Busy. Dropping a token is
// not cleanup: explicit Retire/retry is mandatory before runtime shutdown.
int RetryRetirements() noexcept;
}
} // namespace Libs::NpNativeTransport
