#pragma once

#include "common/abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace Libs::VideoOut { struct VideoOutOutputSnapshot; }

namespace Libs::MediaFramework {

// Observed 13.60 MediaFramework ABI, independent of the FFmpeg canvas API.
struct EsvmInit24 {
	uint32_t size;
	uint32_t reserved;
	uint64_t affinity;
	int32_t priority;
	uint32_t reserved2;
};
static_assert(sizeof(EsvmInit24) == 24);

struct EsvmResult56 {
	int32_t result;
	uint32_t reserved;
	std::array<uint8_t, 48> payload;
};
static_assert(sizeof(EsvmResult56) == 56);
static_assert(offsetof(EsvmResult56, payload) == 8);

// One coherent, owner-aware presentation read, supplied by the graphics owner.
// These are native RAW payloads, not VideoOutOutputStatus copied indiscriminately.
// Output: 48 bytes (mode fields, flags including actual HDCP authentication).
// Port: 32 bytes (including independent disconnected/unusable flag bits).
// A disconnected/minimized/lost output is a valid snapshot with unavailable raw
// state, NOT callback failure. false means no truthful backend snapshot exists.
struct EsvmPresentationSnapshot {
	int32_t output_result = 0;
	int32_t port_result = 0;
	std::array<uint8_t, 48> output_status {};
	std::array<uint8_t, 32> port_status {};
};
using EsvmSnapshotReader = bool (*)(int owner_pid, EsvmPresentationSnapshot* out);
// Pure encoding of one actual graphics snapshot, separately testable without GPU.
EsvmPresentationSnapshot EncodeEsvmPresentationSnapshot(const VideoOut::VideoOutOutputSnapshot& snapshot);
bool ReadEsvmGraphicsSnapshot(int owner_pid, EsvmPresentationSnapshot* out);
// Session-lifetime function pointer, no captured context. nullptr fails closed.
// Called without MediaFramework locks; replacement/owner retirement invalidates
// in-flight reads before any guest output is committed.
// Production defaults to ReadEsvmGraphicsSnapshot; this seam permits explicit
// unavailable/provider-replacement tests without changing graphics state.
void SetEsvmSnapshotReader(EsvmSnapshotReader reader);

int KYTY_SYSV_ABI EsvmInitialize(const EsvmInit24* config, bool reserved_false);
int KYTY_SYSV_ABI EsvmRegisterClient(const char* name);
int KYTY_SYSV_ABI EsvmUnregisterClient(const char* name);
int KYTY_SYSV_ABI EsvmTerminate();
int KYTY_SYSV_ABI EsvmGetStatus(uint32_t selector, EsvmResult56* out);

// Internal admission lease for protocol services. A matching epoch requires an
// initialized owner AND actual completed citroncore registration resources.
int GetEsvmClientEpoch(int owner_pid, uint64_t* epoch);
int GetEsvmClientLease(int owner_pid, uint64_t* epoch, std::shared_ptr<std::recursive_mutex>* dispatch);
// All keys/source dispatch acquisitions participate in one thread-wide scope.
// A reentrant lifecycle/drain call must never wait for a foreign owner gate.
class EsvmDispatchLock final {
public:
	explicit EsvmDispatchLock(std::recursive_mutex& mutex, bool wait = true);
	~EsvmDispatchLock();
	EsvmDispatchLock(const EsvmDispatchLock&) = delete;
	EsvmDispatchLock& operator=(const EsvmDispatchLock&) = delete;
	bool owns_lock() const { return gate.owns_lock(); }
	void lock();
	void unlock();
private:
	std::unique_lock<std::recursive_mutex> gate;
};
bool EsvmDispatchScopeActive();
// Routine drains try/defer; blocking is honored only outside ALL dispatch scopes.
bool DrainRetiredEsvmServices(bool wait = false);
// Shared non-reusing service namespace, disjoint from local-file source IDs.
// Object type/owner validation still belongs to each typed registry.
int AllocateEsvmServiceHandle();
bool IsEsvmServiceHandle(int handle);
bool EsvmReadGuest(void* destination, const void* source, size_t bytes);
bool EsvmWritableGuest(void* destination, size_t bytes);
bool EsvmStoreGuest(void* destination, const void* source, size_t bytes);

// Parent must call after guest threads drain, OUTSIDE the process-identity lock,
// on owner retirement (PID 100 also supports direct-game shutdown). Returns false
// if kernel cleanup failed; retained resources are retried, never forgotten.
// Outside dispatch scopes this also waits for admitted keys/source callbacks.
// Reentrant calls only invalidate and opportunistically drain; outer exits retry.
bool ReleaseEsvmOwner(int owner_pid);
// Host shutdown: close admission before releasing private HLE resources. Safe
// with outstanding snapshot reads; repeated calls retry retained cleanup.
// Reentrant shutdown closes admission but defers worker join to an outside-scope
// retry. Its return value reports cleanup errors, not reentrant quiescence.
bool ShutdownEsvmService();

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
enum class EsvmResourceFailure { None, ShmOpen, ShmResize, ShmMap, FirstSemaphore, SecondSemaphore, ReceiverSetup };
void EsvmFailNextResourceForTest(EsvmResourceFailure failure);
struct EsvmServiceResources {
	size_t sessions = 0;
	size_t initialized = 0;
	size_t clients = 0;
	size_t mappings = 0;
	size_t semaphores = 0;
	size_t descriptors = 0;
};
EsvmServiceResources EsvmResourcesForTest();
#endif
} // namespace Libs::MediaFramework
