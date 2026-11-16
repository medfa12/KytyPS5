#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_

#include "common/abi.h"
#include "common/common.h"
#include "kernel/guestFsContext.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// IWYU pragma: no_include <pthread.h>

extern "C" {
struct sched_param;
}

namespace Loader {
struct Program;
class RuntimeGuestExecution;
} // namespace Loader

namespace Libs {

namespace LibKernel {

void Initialize();

struct PthreadLifecycle {
	static constexpr const char* name       = "Pthread";
	static constexpr auto        initialize = Libs::LibKernel::Initialize;
};

struct PthreadAttrPrivate;
struct PthreadPrivate;
struct PthreadMutexPrivate;
struct PthreadMutexattrPrivate;
struct PthreadRwlockPrivate;
struct PthreadRwlockattrPrivate;
struct PthreadCondattrPrivate;
struct PthreadCondPrivate;
struct PthreadBarrierPrivate;
struct PthreadBarrierattrPrivate;

struct KernelTimespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct KernelTimeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

struct KernelTimezone {
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};

struct KernelTimesec {
	int64_t  t;
	uint32_t west_sec;
	uint32_t dst_sec;
};

using PthreadAttr       = PthreadAttrPrivate*;
using Pthread           = PthreadPrivate*;
using KernelCpumask     = uint64_t;
using PthreadMutex      = PthreadMutexPrivate*;
using PthreadMutexattr  = PthreadMutexattrPrivate*;
using KernelSchedParam  = struct sched_param;
using PthreadRwlock     = PthreadRwlockPrivate*;
using PthreadRwlockattr = PthreadRwlockattrPrivate*;
using KernelUseconds    = unsigned int;
using PthreadCondattr   = PthreadCondattrPrivate*;
using PthreadCond       = PthreadCondPrivate*;
using PthreadBarrier    = PthreadBarrierPrivate*;
using PthreadBarrierattr = PthreadBarrierattrPrivate*;
using KernelClockid     = int32_t;
using PthreadKey        = int;

using pthread_entry_func_t          = KYTY_SYSV_ABI void* (*)(void*);
using thread_dtors_func_t           = KYTY_SYSV_ABI void (*)();
using pthread_key_destructor_func_t = KYTY_SYSV_ABI void (*)(void*);

void  PthreadInitSelfForMainThread();
// Host-only: retain on the actual current, published pool pthread through
// successful native join (including detached collection). Never adopts another
// thread's identity. BSD errno: EINVAL=22, ESRCH=3, ENOSPC=28; success=0.
// At most 16 distinct non-null lifetime pointers; repeated pointers deduplicate.
int PthreadRetainCurrentUntilJoin(std::shared_ptr<void> lifetime) noexcept;

class PthreadNativeReceiptState;
// Opaque type also permits lossless storage in a host shared_ptr<void> token.
using PthreadNativeReceipt = std::shared_ptr<PthreadNativeReceiptState>;
enum class PthreadNativePhase { Pending, NativeExited, Joined };
enum class PthreadNativeJoinSource { None, Explicit, DetachedCollector, StartupRollback };
struct PthreadNativeSnapshot {
	uint64_t generation = 0;
	uint64_t owner_cookie = 0; // Metadata only: never a dereferenceable/lifetime-validating Program handle.
	int32_t pid = -1, tid = -1;
	uint64_t native_tid = 0, entry = 0, argument = 0;
	bool detached_at_creation = false;
	PthreadNativePhase phase = PthreadNativePhase::Pending;
	PthreadNativeJoinSource join_source = PthreadNativeJoinSource::None;
	int32_t joiner_pid = -1, joiner_tid = -1; // Actual caller if available; collector never invents a guest identity.
	uint64_t joined_value = 0; // Valid only with Joined; metadata, not borrowed storage.
};
// Host-only BSD errno APIs. Windows legacy guest pthreads with a retained PID
// only; other platforms/kinds fail closed. Outputs stay unchanged on failure.
// Receipt outputs must be empty. No receipt owns/dereferences Program/provider.
int PthreadCreateWithNativeReceipt(Pthread* thread, const PthreadAttr* attr,
    pthread_entry_func_t entry, void* arg, const char* name, PthreadNativeReceipt& receipt) noexcept;
int PthreadCaptureCurrentNativeReceipt(PthreadNativeReceipt& receipt) noexcept;
int PthreadInspectNativeReceipt(const PthreadNativeReceipt& receipt, PthreadNativeSnapshot& output) noexcept;
// One atomic expected-incarnation check + genuine native join. Record-lock
// contention or an unsignaled original OS handle returns EBUSY (16); no wait for
// target exit. Already joined/reused/mismatched records return ESRCH (3), never
// a fabricated second join success. Current-self returns EDEADLK (11).
int PthreadJoinNativeReceipt(const PthreadNativeReceipt& receipt, Pthread expected_target, void** value) noexcept;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestPthreadJoinRetention(std::string* failure);
bool TestPthreadNativeReceipt(std::string* failure);
#endif
void* PthreadCreateMainGuestStack();
void  PthreadDeleteStaticObjects(Loader::Program* program);

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);
int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr,
                                   const char* name);
int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex);
// Shared guest lock/timedlock require a lifetime match and this segment's host
// return gateway BEFORE lazy initialization/shadow repair/acquisition. Terminal
// ECANCELED unlinks the waiter (or releases its FIFO-transferred lock), returning
// through host cleanup before the gateway suppresses guest continuation.
// Lazy shared initialization requires an owner-private image address; heap
// mutexes must first be explicitly initialized to establish their lifetime.
// Host/coordinator/session calls retain their ordinary mutex semantics.
int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex);

Pthread KYTY_SYSV_ABI PthreadSelf();
Pthread               PthreadSelfOrNull();
Pthread               PthreadSwapSelfForSignal(Pthread thread);
// Ownership getters are NOT arbitrary thread-table queries. Call on the live
// target (including its startup/cleanup or on-target special APC), or after
// successful creation while the caller guarantees no concurrent join/reclaim.
// The record is immutable from before pthread_create until host join completes.
// Do not read a joined/reusable record. Signal helpers capture on the target
// before dispatch and subsequently use only their captured ownership values.
FileSystem::GuestFsContextPtr PthreadGetFsContext(Pthread thread);
Loader::Program* PthreadGetOwnerProgram(Pthread thread);
int KYTY_SYSV_ABI     PthreadCreate(Pthread* thread, const PthreadAttr* attr,
                                    pthread_entry_func_t entry, void* arg, const char* name);
// Host-only runtime adapter. Its entry is C++ coordination on a guest stack,
// not a disposable guest segment; actual guest calls inside it own boundaries.
// Never export this entry-kind override to guest code.
int PthreadCreateRuntimeCoordinator(Pthread* thread, const PthreadAttr* attr,
                                    pthread_entry_func_t entry, void* arg, const char* name);
enum class PthreadOwnerStopStatus { InvalidOwner, Unsupported, Pending, Redirected, Idle, ControlError };
struct PthreadOwnerStopResult {
	PthreadOwnerStopStatus status = PthreadOwnerStopStatus::InvalidOwner;
	uint64_t lifetime = 0;
	uint32_t redirected = 0;
	uint32_t pending = 0;
};
// HOST ONLY, one bounded pass; close publication first, then redirect only exact
// private native text with initialized innermost boundary and pinned stack.
// Pending/ControlError retain all work/mappings. Not a drain/native join result.
PthreadOwnerStopResult PthreadRequestOwnerStop(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) noexcept;
enum class PthreadOwnerNativeDrainStatus { InvalidOwner, AdmissionOpen, Unsupported, Pending, Terminated, ControlError };
struct PthreadOwnerNativeDrainResult {
	PthreadOwnerNativeDrainStatus status = PthreadOwnerNativeDrainStatus::InvalidOwner;
	uint64_t lifetime = 0;
	uint32_t terminated = 0; // Signaled retained records observed; already reaped records are implicit.
	uint32_t pending = 0; // Conservative obligations, including startup/registry/record contention.
	explicit operator bool() const noexcept { return status == PthreadOwnerNativeDrainStatus::Terminated; }
};
// HOST ONLY, nonblocking native-completion gate. Does NOT request stop or reap.
// Requires closed admission AND zero leases (covering pre-record creation), then
// proves every matching nonfree runtime record's ORIGINAL retained native handle
// signaled under stop_mutex, independently of a blocking native join. A reaped or
// reused record can be skipped only because reclamation follows actual join.
// Only Terminated permits close publication. Pending/ControlError/Unsupported
// retain owner mappings/pins and are never logical-drain completion substitutes.
// Windows only; other platforms return Unsupported, never an OS-ID fallback.
PthreadOwnerNativeDrainResult PthreadOwnerNativeThreadsTerminated(
    const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) noexcept;
// Immutable pool-kind marker, including reusable/stale records: shared records
// NEVER become legacy records or vice versa. Invalid handles fail closed (true).
bool PthreadIsRuntimeOwned(Pthread thread);
// Live actual target / captured helper record only, same lifetime contract as
// ownership getters above. No registry lock/TLS allocation from an APC.
bool PthreadIsRuntimeOwnedOnTarget(Pthread thread) noexcept;
// HOST ONLY. Windows legacy admission duplicates the actual native handle under
// stop_mutex, independently of a blocking native join. Never reopen a numeric ID
// or borrow pthread_gethandle: pthread_join may already be reclaiming it.
// Non-Windows admission returns EOPNOTSUPP; existing POSIX signal paths are unchanged.
class PthreadLegacyNativeTarget final {
public:
	PthreadLegacyNativeTarget() noexcept = default;
	~PthreadLegacyNativeTarget();
	PthreadLegacyNativeTarget(const PthreadLegacyNativeTarget&) = delete;
	PthreadLegacyNativeTarget& operator=(const PthreadLegacyNativeTarget&) = delete;
	void* Handle() const noexcept { return handle_; } // Borrowed Windows HANDLE.
	uint64_t ThreadId() const noexcept { return thread_id_; }
	uint64_t StackAddress() const noexcept { return stack_address_; }
	uint64_t StackSize() const noexcept { return stack_size_; }
	bool IsHostLockCritical() const noexcept;
	// With the record still pinned, reserve a signal without registry locking.
	// Returns true ONLY if this operation introduced the pending bit; enqueue
	// failure must not clear an older accepted/coalesced reservation.
	bool QueueSignal(int signum) noexcept;
	// Release before a direct guest call or suspension. Thereafter use ONLY the
	// captured metadata/owned handle, never the recyclable Pthread record.
	void UnlockRecord() noexcept;
	void* ReleaseHandle() noexcept; // Transfer ownership; caller must CloseHandle.
private:
	friend int PthreadPinLegacyNativeTarget(Pthread thread, PthreadLegacyNativeTarget* target);
	std::unique_lock<std::mutex> record_lock_;
	Pthread record_ = nullptr;
	void* handle_ = nullptr;
	uint64_t thread_id_ = 0;
	uint64_t stack_address_ = 0;
	uint64_t stack_size_ = 0;
	std::shared_ptr<std::atomic<uint32_t>> critical_;
};
// Kernel error convention. Shared/invalid targets fail before any mutation.
int PthreadPinLegacyNativeTarget(Pthread thread, PthreadLegacyNativeTarget* target);
int KYTY_SYSV_ABI     PthreadDetach(Pthread thread);
// Runtime guest joins only target the same execution lifetime, are boundedly
// stop-aware, and leave value untouched on ECANCELED. Boundary-free host/runtime
// coordinator calls remain actual native joins (including after owner stop).
int KYTY_SYSV_ABI     PthreadJoin(Pthread thread, void** value);
int KYTY_SYSV_ABI     PthreadCancel(Pthread thread);
int KYTY_SYSV_ABI     PthreadKill(Pthread thread, int sig);
int KYTY_SYSV_ABI     PthreadSetcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI     PthreadSetcanceltype(int type, int* old_type);
int KYTY_SYSV_ABI     PthreadGetprio(Pthread thread, int* prio);
int KYTY_SYSV_ABI     PthreadSetprio(Pthread thread, int prio);
void KYTY_SYSV_ABI    PthreadTestcancel();
int KYTY_SYSV_ABI     PthreadSetaffinity(Pthread thread, KernelCpumask mask);
// Runtime owners request a typed actual-thread exit at the innermost boundary.
// An active host gateway gets a normal return, then transfers after host cleanup.
// Coordinators inspect RuntimeGuestResult::ThreadExited() and return normally.
// Legacy owners keep thread-root exit; do not mark this function noreturn.
void KYTY_SYSV_ABI    PthreadExit(void* value);
int KYTY_SYSV_ABI     PthreadEqual(Pthread thread1, Pthread thread2);
int KYTY_SYSV_ABI     PthreadGetname(Pthread thread, char* name);
int KYTY_SYSV_ABI     PthreadRename(Pthread thread, const char* name);
void KYTY_SYSV_ABI    PthreadYield();
int KYTY_SYSV_ABI     PthreadGetthreadid();
int KYTY_SYSV_ABI     PthreadGetaffinity(Pthread thread, KernelCpumask* mask);
uint64_t              PthreadGetHostThreadId(Pthread thread);
bool                  PthreadIsHostLockCritical(Pthread thread);
void                  PthreadWakeForSignal(Pthread thread);
// ps5-vsh: co-hosted ELF mains run on raw host threads (not guest
// pthreads), invisible to the pool sampler. Registered at entry so the
// sampler (below) covers them too.
void                  NoteCoElfThread(const std::string& name, uint64_t os_thread_id);
// ps5-vsh: poor-man's profiler (see PthreadPool::SampleAllThreads).
// Snapshots every guest thread's RIP; logs name + rip (+ owning module).
void                  PthreadLogThreadSample();
void                  PthreadQueuePendingSignal(Pthread thread, int signum);
bool                  PthreadHasPendingSignal(Pthread thread, int signum);
bool                  PthreadTakePendingSignal(Pthread thread, int signum);
bool PthreadGetGuestStack(Pthread thread, uint64_t* stack_addr, uint64_t* stack_size);
int KYTY_SYSV_ABI KernelRaiseException(Pthread thread, int signum);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestGuestStackOwnerLifecycle(uint64_t* first_address, uint64_t* second_address,
                                  uint64_t* map_size);
bool TestRuntimeGuestThreads(std::string* failure);
bool TestRuntimeGuestWaits(std::string* failure);
bool TestRuntimeCrtConditionWrappers(std::string* failure);
bool TestRuntimeRwlockLifecycle(std::string* failure);
bool TestRuntimeGuestStop(std::string* failure);
bool TestRuntimeGuestMutexStop(std::string* failure);
bool TestRuntimeGuestStopServices(Pthread target);
bool TestRuntimeNativeTargetPins(std::string* failure);
bool TestRuntimeNativeDrain(std::string* failure);
bool TestRuntimeNativeTargetPinServices(Pthread ended_legacy, void* shared_native_handle);
#endif
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
bool PthreadKillHost(Pthread thread, int host_signal);
#endif
int PthreadGetPriorityForKernel(Pthread thread);
int PthreadGetCurrentPriorityForKernel();

int KYTY_SYSV_ABI          KernelUsleep(KernelUseconds microseconds);
unsigned int KYTY_SYSV_ABI KernelSleep(unsigned int seconds);
int KYTY_SYSV_ABI          KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);

int KYTY_SYSV_ABI   PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI   PthreadKeyDelete(PthreadKey key);
int KYTY_SYSV_ABI   PthreadSetspecific(PthreadKey key, void* value);
void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key);

void KYTY_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors);

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched);
int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI PthreadAttrGetsolosched(const PthreadAttr* attr, int* solosched);
int KYTY_SYSV_ABI PthreadAttrGetscope(const PthreadAttr* attr, int* scope);
int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr,
                                      void** __restrict stack_addr, size_t* __restrict stack_size);
int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr);
int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask);
int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state);
int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI PthreadAttrSetsolosched(PthreadAttr* attr, int solosched);
int KYTY_SYSV_ABI PthreadAttrSetscope(PthreadAttr* attr, int scope);
int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size);
int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr);
int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
int TestPthreadAttrCopy(PthreadAttr* dst, const PthreadAttr* src);
#endif

// Shared rwlocks/attrs bind immutable owner identity+token. Calls retain typed
// object pins through queueing/acquisition/release; destroy returns EBUSY while
// pinned or held. NULL static try/read/write are serialized, unlock never lazily
// creates. Shared writable outputs require private-image/current owned-stack
// spans; only image slot authority outlives stack retirement/native join.
// Terminal waits return encoded ECANCELED through host cleanup; lease-serialized
// publication cannot acquire/init/unlock/destroy after stop. No guest cleanup or
// robust owner-death release is inferred: abandoned/failed-release state stays
// retained. Boundary-free HOST coordination requires the matching current owner;
// real session objects keep their legacy lazy/shadow/static-cleanup path.
int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr,
                                    const char* name);
int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type);
int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type);

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clock_id);
int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread);
// Shared-runtime guest waits reject foreign/session-owned objects. Terminal
// ECANCELED after release leaves the mutex RELEASED, never falsely relocked or
// stolen from a peer. Only success/timeout restore its prior recursion depth.
// These HLEs must return through an audited stop gateway, not resume guest code
// on terminal cancellation. Host/session waits retain their original contract.
int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadCondTimedwaitAbs(PthreadCond* cond, PthreadMutex* mutex,
                                          const KernelTimespec* abstime);
int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);

int KYTY_SYSV_ABI      KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelGettimeofday(KernelTimeval* tp);
int KYTY_SYSV_ABI      KernelGettimezone(KernelTimezone* tz);
int KYTY_SYSV_ABI      KernelConvertLocaltimeToUtc(int64_t local_time, int64_t reserved,
                                                   int64_t* utc_time, KernelTimezone* timezone,
                                                   int32_t* dst_seconds);
int KYTY_SYSV_ABI      KernelConvertUtcToLocaltime(int64_t utc_time, int64_t* local_time,
                                                   KernelTimesec* st, uint64_t* dst_sec);
uint64_t KYTY_SYSV_ABI KernelGetTscFrequency();
// Guest TSC = host TSC * multiplier >> 32.
uint64_t               KernelGuestTscMultiplier();
uint64_t KYTY_SYSV_ABI KernelReadTsc();
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency();

} // namespace LibKernel

namespace Posix {

int KYTY_SYSV_ABI pthread_barrier_init(LibKernel::PthreadBarrier* barrier,
                                      const LibKernel::PthreadBarrierattr* attr, unsigned count);
int KYTY_SYSV_ABI pthread_barrier_wait(LibKernel::PthreadBarrier* barrier);
int KYTY_SYSV_ABI pthread_barrier_destroy(LibKernel::PthreadBarrier* barrier);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestPthreadBarrierArrivals(LibKernel::PthreadBarrier* barrier, unsigned* arrivals);
void TestPthreadBarrierFailNextAllocation();
void TestPthreadBarrierSetReleaseCallback(void (*callback)());
#endif

int KYTY_SYSV_ABI  pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr,
                                  LibKernel::pthread_entry_func_t entry, void* arg);
int KYTY_SYSV_ABI  pthread_create_name_np(LibKernel::Pthread*             thread,
                                          const LibKernel::PthreadAttr*   attr,
                                          LibKernel::pthread_entry_func_t entry, void* arg,
                                          const char* name);
int KYTY_SYSV_ABI  pthread_detach(LibKernel::Pthread thread);
void KYTY_SYSV_ABI pthread_exit(void* value);
LibKernel::Pthread KYTY_SYSV_ABI pthread_self();
int KYTY_SYSV_ABI                pthread_rename_np(LibKernel::Pthread thread, const char* name);
int KYTY_SYSV_ABI                pthread_getname_np(LibKernel::Pthread thread, char* name);
int KYTY_SYSV_ABI                pthread_setcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI                pthread_setprio(LibKernel::Pthread thread, int prio);
int KYTY_SYSV_ABI                pthread_getschedparam(LibKernel::Pthread thread, int* policy,
                                                       LibKernel::KernelSchedParam* param);
int KYTY_SYSV_ABI                pthread_setschedparam(LibKernel::Pthread thread, int policy,
                                                       const LibKernel::KernelSchedParam* param);
void KYTY_SYSV_ABI               pthread_yield();
int KYTY_SYSV_ABI                sched_get_priority_max(int policy);
int KYTY_SYSV_ABI                sched_get_priority_min(int policy);
int KYTY_SYSV_ABI                pthread_join(LibKernel::Pthread thread, void** value);
int KYTY_SYSV_ABI                pthread_kill(LibKernel::Pthread thread, int sig);
int KYTY_SYSV_ABI                pthread_attr_init(LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI                pthread_attr_destroy(LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_get_np(LibKernel::Pthread thread, LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_getdetachstate(const LibKernel::PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI pthread_attr_getguardsize(const LibKernel::PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI pthread_attr_getinheritsched(const LibKernel::PthreadAttr* attr,
                                               int*                          inherit_sched);
int KYTY_SYSV_ABI pthread_attr_getschedparam(const LibKernel::PthreadAttr* attr,
                                             LibKernel::KernelSchedParam*  param);
int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const LibKernel::PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI pthread_attr_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                        void** __restrict stack_addr,
                                        size_t* __restrict stack_size);
int KYTY_SYSV_ABI pthread_attr_getstacksize(const LibKernel::PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI pthread_attr_setdetachstate(LibKernel::PthreadAttr* attr, int state);
int KYTY_SYSV_ABI pthread_attr_setguardsize(LibKernel::PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI pthread_attr_setinheritsched(LibKernel::PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI pthread_attr_setschedparam(LibKernel::PthreadAttr*            attr,
                                             const LibKernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_attr_setschedpolicy(LibKernel::PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI pthread_attr_setscope(LibKernel::PthreadAttr* attr, int scope);
int KYTY_SYSV_ABI pthread_attr_setstack(LibKernel::PthreadAttr* attr, void* addr, size_t size);
int KYTY_SYSV_ABI pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size);
int KYTY_SYSV_ABI pthread_cond_broadcast(LibKernel::PthreadCond* cond);
int KYTY_SYSV_ABI pthread_cond_signal(LibKernel::PthreadCond* cond);
int KYTY_SYSV_ABI pthread_cond_init(LibKernel::PthreadCond*           cond,
                                    const LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_init(LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_destroy(LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_setclock(LibKernel::PthreadCondattr* attr,
                                            LibKernel::KernelClockid    clock_id);
int KYTY_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_cond_timedwait(LibKernel::PthreadCond*          cond,
                                         LibKernel::PthreadMutex*         mutex,
                                         const LibKernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI pthread_cond_reltimedwait_np(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex,
                                               LibKernel::KernelUseconds usec);
// Shared once requires this owner's registered host-return boundary and private
// pinned control/callback. Contended terminal stop unlinks without signals;
// incomplete/actual-thread-exiting initializer resets for retry, never DONE.
int KYTY_SYSV_ABI pthread_once(void* once_control, void(KYTY_SYSV_ABI* init_routine)());
int KYTY_SYSV_ABI pthread_mutex_lock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_mutex_trylock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_mutex_timedlock(LibKernel::PthreadMutex*         mutex,
                                          const LibKernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI pthread_mutex_unlock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_init(LibKernel::PthreadRwlock*           rwlock,
                                      const LibKernel::PthreadRwlockattr* attr);
int KYTY_SYSV_ABI pthread_key_create(LibKernel::PthreadKey*                   key,
                                     LibKernel::pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI pthread_key_delete(LibKernel::PthreadKey key);
int KYTY_SYSV_ABI pthread_setspecific(LibKernel::PthreadKey key, void* value);
void* KYTY_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key);
int KYTY_SYSV_ABI   pthread_mutex_destroy(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_mutex_init(LibKernel::PthreadMutex*           mutex,
                                       const LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI   pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol);
int KYTY_SYSV_ABI   pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutex_setname_np(LibKernel::PthreadMutex* mutex, const char* name);
int KYTY_SYSV_ABI   pthread_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                     void** __restrict stack_addr, size_t* __restrict stack_size);
int64_t KYTY_SYSV_ABI lseek(int d, int64_t offset, int whence);

} // namespace Posix

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_ */
