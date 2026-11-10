#include "kernel/pthread.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/emulatorConfig.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeCrtSync.h"
#include "loader/applicationImportBindings.h"
#include "loader/runtimeGuestExecution.h"
#include "loader/runtimeProcessIdentity.h"
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include "libs/libLncUtil.h"
#endif
#include "loader/timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#endif
#include <windows.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <pthread.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <fmt/format.h>
#include <pthread_time.h>
#elif !defined(__APPLE__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <csignal> // pthread_kill is declared here, not in <pthread.h>
#endif

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS && (defined(_M_X64) || defined(__x86_64__))
#include <x86intrin.h>
#endif

#ifdef pthread_attr_getguardsize
#undef pthread_attr_getguardsize
#endif

namespace Libs {

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void InitLibKernel_1(Loader::SymbolDatabase*);
#endif

namespace LibcInternalExt {
void RunThreadAtexitDestructors();
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
int KYTY_SYSV_ABI LibcInternalExtCxaThreadAtexit(void(KYTY_SYSV_ABI* destructor)(void*), void* object, void* module_id);
#endif
} // namespace LibcInternalExt

namespace LibKernel {

LIB_NAME("libkernel", "libkernel");

// macOS's <pthread.h>/<limits.h> define PTHREAD_STACK_MIN as a macro; the emulator
// wants its own guest-side value with the same name, so drop the host macro here.
#ifdef PTHREAD_STACK_MIN
#undef PTHREAD_STACK_MIN
#endif

constexpr int      KEYS_MAX                = 256;
constexpr int      DESTRUCTOR_ITERATIONS   = 4;
constexpr size_t   PTHREAD_STACK_DEFAULT   = 0x400000;
constexpr size_t   GUEST_PTHREAD_STACK_MIN = 0x4000;
constexpr size_t   PTHREAD_STACK_PAGE      = 0x4000;
constexpr size_t   PTHREAD_STACK_INITIAL   = 0x800000;
constexpr size_t   PTHREAD_STACK_EXTRA     = 0x400000;
constexpr uint64_t PTHREAD_STACK_TOP       = 0x7efff8000ull;
constexpr uint64_t PTHREAD_STACK_BOTTOM    = 0x0000040000ull;
constexpr uint32_t SIGNAL_APC_POLL_MICROS  = 10000;

static constexpr KernelClockid KERNEL_CLOCK_REALTIME          = 0;
static constexpr KernelClockid KERNEL_CLOCK_VIRTUAL           = 1;
static constexpr KernelClockid KERNEL_CLOCK_PROF              = 2;
static constexpr KernelClockid KERNEL_CLOCK_MONOTONIC         = 4;
static constexpr KernelClockid KERNEL_CLOCK_UPTIME            = 5;
static constexpr KernelClockid KERNEL_CLOCK_UPTIME_PRECISE    = 7;
static constexpr KernelClockid KERNEL_CLOCK_UPTIME_FAST       = 8;
static constexpr KernelClockid KERNEL_CLOCK_REALTIME_PRECISE  = 9;
static constexpr KernelClockid KERNEL_CLOCK_REALTIME_FAST     = 10;
static constexpr KernelClockid KERNEL_CLOCK_MONOTONIC_PRECISE = 11;
static constexpr KernelClockid KERNEL_CLOCK_MONOTONIC_FAST    = 12;
static constexpr KernelClockid KERNEL_CLOCK_SECOND            = 13;
static constexpr KernelClockid KERNEL_CLOCK_THREAD_CPUTIME_ID = 14;
static constexpr KernelClockid KERNEL_CLOCK_PROCTIME          = 15;
static constexpr KernelClockid KERNEL_CLOCK_EXT_NETWORK       = 16;
static constexpr KernelClockid KERNEL_CLOCK_EXT_DEBUG_NETWORK = 17;
static constexpr KernelClockid KERNEL_CLOCK_EXT_AD_NETWORK    = 18;
static constexpr KernelClockid KERNEL_CLOCK_EXT_RAW_NETWORK   = 19;

static constexpr int KERNEL_PTHREAD_MUTEX_ERRORCHECK = 1;
static constexpr int KERNEL_PTHREAD_MUTEX_RECURSIVE  = 2;
static constexpr int KERNEL_PTHREAD_MUTEX_NORMAL     = 3;
static constexpr int KERNEL_PTHREAD_MUTEX_ADAPTIVE   = 4;

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// OS-level thread id reported to the guest.
static uint64_t GetHostThreadId() {
#if defined(__APPLE__)
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	return tid;
#else
	return static_cast<uint64_t>(::syscall(SYS_gettid));
#endif
}
#endif

static uint64_t KernelHostTsc() {
#if defined(_M_X64) || defined(__x86_64__)
	return __rdtsc();
#else
	return Common::Timer::QueryPerformanceCounter();
#endif
}

static uint64_t KernelHostTscFrequency() {
	static const uint64_t frequency = [] {
#if defined(_M_X64) || defined(__x86_64__)
		const auto host_frequency = Common::Timer::QueryPerformanceFrequency();
		if (host_frequency == 0) {
			return uint64_t {1000000000};
		}

		KernelHostTsc();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		KernelHostTsc();

		const auto host_start = Common::Timer::QueryPerformanceCounter();
		const auto tsc_start  = KernelHostTsc();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto host_end = Common::Timer::QueryPerformanceCounter();
		const auto tsc_end  = KernelHostTsc();

		const auto host_delta = host_end - host_start;
		const auto tsc_delta  = tsc_end - tsc_start;
		if (host_delta == 0 || tsc_delta == 0) {
			return host_frequency;
		}

		auto freq = static_cast<uint64_t>(
		    (static_cast<long double>(tsc_delta) * static_cast<long double>(host_frequency)) /
		    static_cast<long double>(host_delta));
		static constexpr uint64_t ROUND_TO = 100000;
		const auto                mod      = freq % ROUND_TO;
		freq = (mod >= ROUND_TO / 2 ? freq - mod + ROUND_TO : freq - mod);

		return freq != 0 ? freq : host_frequency;
#else
		return Common::Timer::QueryPerformanceFrequency();
#endif
	}();

	return frequency;
}

// The console counter runs at 1.6 GHz and some games hardcode that rate instead of calling
// sceKernelGetTscFrequency(); --guest-tsc presents it on every host and the loader rewrites guest
// RDTSC/RDTSCP to the same product.
static constexpr uint64_t GUEST_TSC_FREQUENCY = 1600000000;

uint64_t KernelGuestTscMultiplier() {
	static const uint64_t multiplier =
	    ((GUEST_TSC_FREQUENCY << 32u) + KernelHostTscFrequency() / 2) / KernelHostTscFrequency();
	return multiplier;
}

static uint64_t KernelReadTscNative() {
	if (!Config::GuestTscEnabled()) {
		return KernelHostTsc();
	}
	return static_cast<uint64_t>(
	    (static_cast<unsigned __int128>(KernelHostTsc()) * KernelGuestTscMultiplier()) >> 32u);
}

static uint64_t KernelGetTscFrequencyNative() {
	return Config::GuestTscEnabled() ? GUEST_TSC_FREQUENCY : KernelHostTscFrequency();
}

static uint64_t KernelGetInitialTsc() {
	static const uint64_t initial_tsc = KernelReadTscNative();
	return initial_tsc;
}

static uint64_t KernelGetElapsedTsc() {
	const auto initial_tsc = KernelGetInitialTsc();
	const auto current_tsc = KernelReadTscNative();
	return current_tsc >= initial_tsc ? current_tsc - initial_tsc : 0;
}

static uint64_t KernelGetProcessTimeUsNative() {
	const auto frequency = KernelGetTscFrequencyNative();
	if (frequency == 0) {
		return static_cast<uint64_t>(Loader::Timer::GetTimeMs() * 1000.0);
	}

	const auto elapsed = KernelGetElapsedTsc();
	return static_cast<uint64_t>((static_cast<long double>(elapsed) * 1000000.0L) /
	                             static_cast<long double>(frequency));
}

static void KernelUsToTimespec(uint64_t us, KernelTimespec* tp) {
	EXIT_IF(tp == nullptr);
	tp->tv_sec  = static_cast<int64_t>(us / 1000000);
	tp->tv_nsec = static_cast<int64_t>((us % 1000000) * 1000);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static uint64_t KernelFiletimeTo100ns(FILETIME ft) {
	ULARGE_INTEGER v {};
	v.LowPart  = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;
	return v.QuadPart;
}

static void Kernel100nsToTimespec(uint64_t value, KernelTimespec* tp) {
	EXIT_IF(tp == nullptr);
	tp->tv_sec  = static_cast<int64_t>(value / 10000000);
	tp->tv_nsec = static_cast<int64_t>((value % 10000000) * 100);
}

static bool KernelRealtimeToTimespec(bool precise, KernelTimespec* tp) {
	EXIT_IF(tp == nullptr);

	FILETIME ft {};
	if (precise) {
		GetSystemTimePreciseAsFileTime(&ft);
	} else {
		GetSystemTimeAsFileTime(&ft);
	}

	static constexpr uint64_t WINDOWS_UNIX_EPOCH_DELTA_100NS = 116444736000000000ULL;
	const auto                value                          = KernelFiletimeTo100ns(ft);
	if (value < WINDOWS_UNIX_EPOCH_DELTA_100NS) {
		return false;
	}

	Kernel100nsToTimespec(value - WINDOWS_UNIX_EPOCH_DELTA_100NS, tp);
	return true;
}

static bool KernelMonotonicToTimespec(KernelTimespec* tp) {
	EXIT_IF(tp == nullptr);

	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	if (frequency == 0) {
		return false;
	}

	const auto counter = Common::Timer::QueryPerformanceCounter();
	tp->tv_sec         = static_cast<int64_t>(counter / frequency);
	tp->tv_nsec        = static_cast<int64_t>(((counter % frequency) * 1000000000ull) / frequency);
	return true;
}

static bool KernelClockGettimeSpecial(KernelClockid clock_id, KernelTimespec* tp, int* error) {
	EXIT_IF(tp == nullptr);
	EXIT_IF(error == nullptr);

	if (clock_id == KERNEL_CLOCK_PROCTIME) {
		KernelUsToTimespec(KernelGetProcessTimeUsNative(), tp);
		return true;
	}

	FILETIME create_time {};
	FILETIME exit_time {};
	FILETIME kernel_time {};
	FILETIME user_time {};

	switch (clock_id) {
		case KERNEL_CLOCK_REALTIME:
		case KERNEL_CLOCK_REALTIME_PRECISE:
			if (!KernelRealtimeToTimespec(true, tp)) {
				*error = KERNEL_ERROR_EFAULT;
			}
			return true;
		case KERNEL_CLOCK_REALTIME_FAST:
		case KERNEL_CLOCK_SECOND:
			if (!KernelRealtimeToTimespec(false, tp)) {
				*error = KERNEL_ERROR_EFAULT;
			}
			if (clock_id == KERNEL_CLOCK_SECOND) {
				tp->tv_nsec = 0;
			}
			return true;
		case KERNEL_CLOCK_MONOTONIC:
		case KERNEL_CLOCK_UPTIME:
		case KERNEL_CLOCK_UPTIME_PRECISE:
		case KERNEL_CLOCK_UPTIME_FAST:
		case KERNEL_CLOCK_MONOTONIC_PRECISE:
		case KERNEL_CLOCK_MONOTONIC_FAST:
		case KERNEL_CLOCK_EXT_NETWORK:
		case KERNEL_CLOCK_EXT_DEBUG_NETWORK:
		case KERNEL_CLOCK_EXT_AD_NETWORK:
		case KERNEL_CLOCK_EXT_RAW_NETWORK:
			if (!KernelMonotonicToTimespec(tp)) {
				*error = KERNEL_ERROR_EFAULT;
			}
			return true;
		case KERNEL_CLOCK_THREAD_CPUTIME_ID:
			if (!GetThreadTimes(GetCurrentThread(), &create_time, &exit_time, &kernel_time,
			                    &user_time)) {
				*error = KERNEL_ERROR_EFAULT;
				return true;
			}
			Kernel100nsToTimespec(
			    KernelFiletimeTo100ns(kernel_time) + KernelFiletimeTo100ns(user_time), tp);
			return true;
		case KERNEL_CLOCK_VIRTUAL:
			if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time,
			                     &user_time)) {
				*error = KERNEL_ERROR_EFAULT;
				return true;
			}
			Kernel100nsToTimespec(KernelFiletimeTo100ns(user_time), tp);
			return true;
		case KERNEL_CLOCK_PROF:
			if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time,
			                     &user_time)) {
				*error = KERNEL_ERROR_EFAULT;
				return true;
			}
			Kernel100nsToTimespec(KernelFiletimeTo100ns(kernel_time), tp);
			return true;
		default: return false;
	}
}
#else
static bool KernelClockGettimeSpecial(KernelClockid clock_id, KernelTimespec* tp, int* error) {
	EXIT_IF(tp == nullptr);
	EXIT_IF(error == nullptr);

	if (clock_id == KERNEL_CLOCK_PROCTIME) {
		KernelUsToTimespec(KernelGetProcessTimeUsNative(), tp);
		return true;
	}

	return false;
}
#endif

struct PthreadMutexPrivate {
	struct Waiter {
		Pthread                 thread = nullptr;
		std::condition_variable cv;
		Waiter*                 previous = nullptr;
		Waiter*                 next     = nullptr;
	};
	uint8_t                 reserved[256];
	std::string             name;
	std::mutex              m;
	Loader::GuestCallOwner wait_owner;
	Pthread                 owner     = nullptr;
	uint32_t                count     = 0;
	int                     type      = 1;
	int                     pprotocol = PTHREAD_PRIO_NONE;
	Waiter*                 first_waiter = nullptr;
	Waiter*                 last_waiter  = nullptr;
};

struct PthreadMutexattrPrivate {
	uint8_t             reserved[64];
	pthread_mutexattr_t p;
	int                 pprotocol;
	int                 type;
};

struct PthreadAttrPrivate {
	uint8_t        reserved[64];
	KernelCpumask  affinity;
	size_t         guard_size;
	void*          stack_addr;
	size_t         stack_size;
	bool           stack_user;
	uint64_t       stack_map_addr;
	size_t         stack_map_size;
	int            policy;
	int            guest_priority;
	int            inherit_sched;
	int            solosched;
	// Guest PROCESS=0 / SYSTEM=2; unrelated to affinity or solosched.
	int            scope;
	bool           detached;
	pthread_attr_t p;
};

struct PthreadGuestData {
	int32_t thread_id;
	uint8_t reserved[4092];
};

static_assert(sizeof(PthreadGuestData) == 4096);

class PthreadNativeReceiptState final {
public:
	PthreadNativeSnapshot identity; // Finalized before startup/receipt publication; immutable thereafter.
	Pthread record = nullptr; // Records never disappear; only dereferenced after pool + join-lock validation.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE handle = nullptr;
#endif
	std::atomic<PthreadNativeJoinSource> joined {PthreadNativeJoinSource::None};
	uint64_t joined_value = 0; // Published by joined.store(release), read only after acquire.
	int32_t joiner_pid = -1, joiner_tid = -1;
	~PthreadNativeReceiptState() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (handle) { CloseHandle(handle); }
#endif
	}
};
static std::atomic<uint64_t> g_native_receipt_generation {0};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static std::atomic<bool> g_native_receipt_exhaust_for_test {false};
[[maybe_unused]] static std::atomic<bool> g_native_receipt_duplicate_fail_for_test {false};
#endif
[[maybe_unused]] static uint64_t NextNativeReceiptGeneration() noexcept {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_native_receipt_exhaust_for_test.exchange(false)) { return 0; }
#endif
	auto previous = g_native_receipt_generation.load(std::memory_order_relaxed);
	while (previous != UINT64_MAX) {
		if (g_native_receipt_generation.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {
			return previous + 1;
		}
	}
	return 0;
}

struct PthreadPrivate {
	PthreadGuestData      guest;
	std::string           name;
	pthread_t             p;
	PthreadAttr           attr;
	pthread_entry_func_t  entry;
	bool                 runtime_coordinator = false;
	void*                 arg;
	int                   unique_id;
	std::atomic_bool      detached;
	std::atomic_bool      almost_done;
	Loader::Program*      counted_application = nullptr;
	std::atomic_bool      free;
	bool                 runtime_owned = false; // Immutable pool kind; no legacy async delivery can cross into shared reuse.
	std::atomic<bool>    stop_ready {false};
	Loader::GuestBoundaryStopState stop_boundary;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE               stop_handle = nullptr; // Actual native handle for legacy AND shared; stop_mutex pins it.
#endif
	// Serializes native join/reclaim. Guest callers never block acquiring this
	// host lock; a competing join is refused, not another unbounded wait.
	std::mutex           join_mutex;
	bool                 pool_managed = false; // Immutable; synthetic/main records are ineligible.
	std::atomic<bool>    join_reclaiming {false}; // Record unavailable during out-of-lock deleters.
	// Only the actual target writes these before termination. Native join is
	// the synchronization boundary for the sole reclaimer; registration must
	// never wait on join_mutex, which a joiner can hold while waiting for us.
	std::array<std::shared_ptr<void>, 16> join_lifetimes {};
	size_t               join_lifetime_count = 0;
	std::shared_ptr<PthreadNativeReceiptState> native_receipt;
	// Pin incarnation/stack/stop handle independently of a blocking native join.
	// Creators/reapers take join_mutex then this lock; stoppers take only this.
	std::mutex           stop_mutex;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	std::atomic<bool>    test_guest_join_polling {false};
	std::atomic<bool>    test_fail_native_join {false};
#endif
	// ps5-vsh: program scope inheritance (voldemort parity). Captured
	// from the creating thread; installed as this thread's TLS program
	// on entry so per-program mounts resolve for guest-spawned threads.
	Loader::Program* owner_program = nullptr;
	// Published before pthread_create (or main-thread registration), immutable
	// throughout the host thread's lifetime. Reset only after failed creation or
	// successful host join, before pool reuse. An on-target special APC can copy
	// this stable record even while RunThread is installing its separate TLS ptr.
	// Do not replace with atomic<shared_ptr>: its internal lock is not APC-safe.
	FileSystem::GuestFsContextPtr fs_context;
	// Immutable until host join. The Thread lease covers startup through cleanup;
	// the separate strong control pin survives cleanup until join/pool reuse.
	std::shared_ptr<Loader::RuntimeGuestExecution> execution;
	Loader::RuntimeGuestExecution::Lease execution_thread;
	std::mutex startup_mutex;
	std::condition_variable startup_cv;
	std::atomic<int> startup {0}; // 0 pending, 1 published, -1 refused
	Loader::RuntimeGuestResult guest_result;
	uint64_t              host_thread_id;
	std::shared_ptr<Common::HostLockCriticalCounter> host_lock_critical;
	uintptr_t             guest_host_rbx;
	uintptr_t             guest_host_rsp;
	uintptr_t             guest_host_rbp;
	uint64_t              cond_sequence = 0;
	std::condition_variable cond_cv;
	std::atomic<uint64_t> pending_signal_mask {0};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t guest_host_gs8;
	uintptr_t guest_host_gs10;
	std::atomic_bool guest_cancel_requested {false};
#endif
};

FileSystem::GuestFsContextPtr PthreadGetFsContext(Pthread thread) {
	// Caller must satisfy the live-target / pre-join lifetime contract in the
	// header. Concurrent readers only copy the immutable shared_ptr; there is
	// no shared_ptr-object lock for a special APC to reenter. The record pins
	// the control block throughout the copy, including during TLS installation.
	return thread != nullptr ? thread->fs_context : nullptr;
}

Loader::Program* PthreadGetOwnerProgram(Pthread thread) {
	// Called while the signal target is live (before launching the helper).
	return thread != nullptr ? thread->owner_program : nullptr;
}

static PthreadAttrPrivate* GetPthreadAttrValue(const PthreadAttr* attr, const char* func_name) {
	if (attr == nullptr) {
		return nullptr;
	}

	auto* attr_value = *attr;
	if (attr_value == nullptr || reinterpret_cast<uint64_t>(attr_value) < 0x10000u) {
		(void)func_name;
		return nullptr;
	}

	return attr_value;
}

struct PthreadRwlockPrivate {
	struct Reader {
		Pthread  thread = nullptr;
		uint32_t count  = 0;
		Loader::RuntimeCrt::ThreadIdentity incarnation;
	};

	uint8_t                 reserved[256];
	std::string             name;
	std::mutex              m;
	std::condition_variable cv;
	Loader::GuestCallOwner wait_owner;
	int                     type            = 1;
	Pthread                 writer          = nullptr;
	Loader::RuntimeCrt::ThreadIdentity writer_incarnation;
	uint32_t                writer_count    = 0;
	uint32_t                reader_count    = 0;
	uint32_t                waiting_writers = 0;
	std::vector<Reader>     readers;
};

struct PthreadRwlockattrPrivate {
	uint8_t              reserved[64];
	int                  type;
	pthread_rwlockattr_t p;
	Loader::GuestCallOwner wait_owner;
};

// Registry ownership is independent of guest handle words. Calls take a typed
// strong pin before dropping this lock; destroy refuses even an idle queued call.
static Loader::GuestExecutionMutex g_rwlock_registry_mutex;
static std::map<PthreadRwlock, std::shared_ptr<PthreadRwlockPrivate>> g_rwlocks;
static std::map<PthreadRwlockattr, std::unique_ptr<PthreadRwlockattrPrivate>> g_rwlock_attributes;
struct RwlockSlot {
	Loader::GuestCallOwner owner;
	PthreadRwlock value = nullptr;
};
// Retain private-image address authority after destroy/stop. Stack output slots
// are temporary handle transport, not immutable storage: actual native join can
// retire/reuse that stack while a copied handle still retains its typed owner.
static std::map<uint64_t, RwlockSlot> g_rwlock_slots;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
using RuntimeRwlockHook = void (*)(PthreadRwlock, Loader::RuntimeGuestExecution*);
static std::atomic<RuntimeRwlockHook> g_runtime_rwlock_pinned_for_test {nullptr};
static std::atomic<RuntimeRwlockHook> g_runtime_rwlock_publish_for_test {nullptr};
static std::atomic<RuntimeRwlockHook> g_runtime_rwlock_init_publish_for_test {nullptr};
static std::atomic<int> g_runtime_rwlock_attr_destroy_error_for_test {0};
using RuntimeRwlockattrHook = void (*)(PthreadRwlockattr, Loader::RuntimeGuestExecution*);
static std::atomic<RuntimeRwlockattrHook> g_runtime_rwlock_attr_init_publish_for_test {nullptr};
#endif

static int NativeRwlockattrDestroy(PthreadRwlockattr attr) noexcept {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (const int error = g_runtime_rwlock_attr_destroy_error_for_test.load(std::memory_order_acquire)) { return error; }
#endif
	// Audited bundled winpthread DLL, SHA256 78ebbc1480d62098aa66307db2ff139a
	// bc07c137bc188f5b73bf7b131492932c, RVA 0x525d: null-argument test and scalar
	// return ONLY. No dereference, call, wait, allocation, free or exception path.
	// It is therefore safe inside the short Windows stop-serialized publication.
	return pthread_rwlockattr_destroy(&attr->p);
}

struct PthreadCondattrPrivate {
	uint8_t            reserved[64];
	pthread_condattr_t p;
	KernelClockid      clock_id = KERNEL_CLOCK_REALTIME;
	Loader::GuestCallOwner wait_owner;
};

struct PthreadCondPrivate {
	uint8_t                 reserved[256];
	std::string             name;
	std::mutex              m;
	Loader::GuestCallOwner wait_owner;
	KernelClockid           clock_id = KERNEL_CLOCK_REALTIME;
	std::vector<Pthread>    waiters;
};

// Typed handles and call pins retain objects through notification and the
// waiter's final mutex reacquisition, not merely while listed in waiters.
static Loader::GuestExecutionMutex g_condition_registry_mutex;
static std::map<PthreadCond, std::shared_ptr<PthreadCondPrivate>> g_conditions;
static std::map<PthreadCondattr, std::unique_ptr<PthreadCondattrPrivate>> g_condition_attributes;
static std::shared_ptr<PthreadCondPrivate> PinCondition(const void* value) {
	std::lock_guard lock(g_condition_registry_mutex);
	const auto it = g_conditions.find(static_cast<PthreadCond>(const_cast<void*>(value)));
	return it == g_conditions.end() ? nullptr : it->second;
}

static void CondAddWaiter(PthreadCondPrivate* cond, Pthread thread) {
	EXIT_IF(cond == nullptr);
	EXIT_IF(thread == nullptr);

	cond->waiters.push_back(thread);
}

static void CondRemoveWaiter(PthreadCondPrivate* cond, Pthread thread) {
	EXIT_IF(cond == nullptr);
	EXIT_IF(thread == nullptr);

	std::erase(cond->waiters, thread);
}

static void CondWakeWaiter(PthreadCondPrivate* cond, Pthread thread) {
	EXIT_IF(cond == nullptr);

	if (thread == nullptr) {
		if (cond->waiters.empty()) {
			return;
		}
		thread = cond->waiters.front();
	}

	auto it = std::find(cond->waiters.begin(), cond->waiters.end(), thread);
	if (it == cond->waiters.end()) {
		return;
	}

	cond->waiters.erase(it);
	thread->cond_sequence++;
	// Notify while holding cond->m so the selected waiter cannot exit and be freed first.
	thread->cond_cv.notify_one();
}

void PthreadWakeForSignal(Pthread thread) {
	if (thread != nullptr) {
		thread->cond_cv.notify_one();
	}
}

void KernelDispatchPendingSignalForCurrentThread();

static void SchedulerBackoffOnce() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (SwitchToThread() == 0) {
		Sleep(0);
	}
#else
	std::this_thread::yield();
#endif
}

static void SleepMicroWithSignalPoll(uint64_t microseconds) {
	if (microseconds == 0) {
		KernelDispatchPendingSignalForCurrentThread();
		return;
	}

	while (microseconds > 0) {
		const auto step = std::min<uint64_t>(microseconds, SIGNAL_APC_POLL_MICROS);
		Common::Thread::SleepMicro(step);
		microseconds -= step;
		KernelDispatchPendingSignalForCurrentThread();
	}
}

static void SleepNanoWithSignalPoll(uint64_t nanoseconds) {
	if (nanoseconds == 0) {
		KernelDispatchPendingSignalForCurrentThread();
		return;
	}

	constexpr uint64_t poll_nanos = static_cast<uint64_t>(SIGNAL_APC_POLL_MICROS) * 1000ull;
	while (nanoseconds > 0) {
		const auto step = std::min<uint64_t>(nanoseconds, poll_nanos);
		Common::Thread::SleepNano(step);
		nanoseconds -= step;
		KernelDispatchPendingSignalForCurrentThread();
	}
}

struct PthreadStaticObject {
	enum class Type { Mutex, Cond, Rwlock };

	Type             type;
	uint64_t         vaddr;
	Loader::Program* program;
};

class PthreadStaticObjects {
public:
	PthreadStaticObjects() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~PthreadStaticObjects() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadStaticObjects);

	void* CreateObject(void* addr, PthreadStaticObject::Type type, void** initial_word = nullptr,
	                   Loader::RuntimeGuestExecution* execution = nullptr, int* status = nullptr);
	void  DeleteObjects(Loader::Program* program);

private:
	std::vector<PthreadStaticObject*> m_objects;
	Common::Mutex                     m_mutex;
};

struct PthreadSpecificValue {
	uint64_t generation = 0;
	void*    data       = nullptr;
};

struct PthreadKeyState {
	// Odd generations are allocated; deletion advances to the next even generation.
	std::atomic<uint64_t>         generation {0};
	pthread_key_destructor_func_t destructor = nullptr;
	std::shared_ptr<Loader::RuntimeGuestExecution> execution;
};

static std::array<PthreadKeyState, KEYS_MAX>                g_pthread_keys;
static std::mutex                                           g_pthread_keys_mutex;
static thread_local std::unique_ptr<PthreadSpecificValue[]> g_pthread_specific;

class PthreadPool {
public:
	PthreadPool() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~PthreadPool() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadPool);

	Pthread Create(bool runtime_owned = false);
	std::vector<Pthread> StopCandidates(bool* acquired);

	void FreeDetachedThreads();

	bool Contains(Pthread thread) {
		if (thread == nullptr) {
			return false;
		}
		Common::LockGuard lock(m_mutex);
		for (auto* p: m_threads) {
			if (p == thread) {
				return true;
			}
		}
		return false;
	}

	// ps5-vsh: poor-man's profiler. Snapshots (name, os thread id) under
	// the pool lock; the sampler suspends each thread, captures RIP and
	// resumes WITHOUT holding any emulator lock (a suspended thread may
	// hold them), then logs. Stale ids (exited threads) are skipped.
	struct ThreadSample {
		std::string name;
		uint64_t    os_thread_id = 0;
		uint64_t    rip          = 0;
		bool        ok           = false;
	};
	std::vector<ThreadSample> SampleAllThreads();

private:
	std::vector<Pthread> m_threads;
	Common::Mutex        m_mutex;
};

class PThreadContext {
public:
	PThreadContext() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~PThreadContext() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PThreadContext);

	PthreadAttr*       GetDefaultAttr() { return &m_default_attr; }
	void               SetDefaultAttr(PthreadAttr attr) { m_default_attr = attr; }
	PthreadCondattr*   GetDefaultCondattr() { return &m_default_condattr; }
	void               SetDefaultCondattr(PthreadCondattr attr) { m_default_condattr = attr; }
	PthreadMutexattr*  GetDefaultMutexattr() { return &m_default_mutexattr; }
	void               SetDefaultMutexattr(PthreadMutexattr attr) { m_default_mutexattr = attr; }
	PthreadRwlockattr* GetDefaultRwlockattr() { return &m_default_rwlockattr; }
	void               SetDefaultRwlockattr(PthreadRwlockattr attr) { m_default_rwlockattr = attr; }
	PthreadPool*       GetPthreadPool() { return m_pthread_pool; }
	void               SetPthreadPool(PthreadPool* pool) { m_pthread_pool = pool; }
	PthreadStaticObjects* GetPthreadStaticObjects() { return m_pthread_static_objects; }
	void SetPthreadStaticObjects(PthreadStaticObjects* objs) { m_pthread_static_objects = objs; }

	[[nodiscard]] thread_dtors_func_t GetThreadDtors() const { return m_thread_dtors; }
	void SetThreadDtors(thread_dtors_func_t dtors) { m_thread_dtors = dtors; }

private:
	// Common::Mutex           m_mutex;
	PthreadMutexattr      m_default_mutexattr      = nullptr;
	PthreadRwlockattr     m_default_rwlockattr     = nullptr;
	PthreadCondattr       m_default_condattr       = nullptr;
	PthreadAttr           m_default_attr           = nullptr;
	PthreadPool*          m_pthread_pool           = nullptr;
	PthreadStaticObjects* m_pthread_static_objects = nullptr;

	std::atomic<thread_dtors_func_t> m_thread_dtors = nullptr;
};

thread_local Pthread        g_pthread_self           = nullptr;
PThreadContext*             g_pthread_context        = nullptr;
thread_local uintptr_t      g_guest_entry_return_rsp = 0;
static std::atomic<int32_t> g_pthread_thread_id      = 0;

// Co-ELF entry threads are not pool records. Each has its own stable main
// record: a single global main pointer loses older, still-live co-ELFs as new
// entries start. Records intentionally remain allocated, like pool records,
// so validation of a stale guest pointer never dereferences freed memory.
struct ExternalMainRegistry {
	std::mutex mutex;
	std::unordered_set<Pthread> records;
};
static ExternalMainRegistry& ExternalMains() {
	static auto* registry = new ExternalMainRegistry; // TLS cleanup can run during process teardown.
	return *registry;
}
struct ExternalMainLifetime {
	Pthread record = nullptr;
	~ExternalMainLifetime() {
		if (!record) { return; }
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(record->stop_mutex);
		record->free.store(true, std::memory_order_release);
		record->stop_ready.store(false, std::memory_order_release);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (record->stop_handle) { CloseHandle(record->stop_handle); record->stop_handle = nullptr; }
#endif
	}
};
static thread_local ExternalMainLifetime g_external_main_lifetime;

static bool IsValidPthread(Pthread thread) {
	if (thread == nullptr || reinterpret_cast<uintptr_t>(thread) < 0x10000ULL ||
	    reinterpret_cast<uintptr_t>(thread) == ~0ULL ||
	    (reinterpret_cast<uintptr_t>(thread) & 7ULL) != 0) {
		return false;
	}
	if (g_pthread_self != nullptr && thread == g_pthread_self) {
		return !thread->join_reclaiming;
	}
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(ExternalMains().mutex);
		if (ExternalMains().records.contains(thread)) {
			return !thread->free.load(std::memory_order_acquire) && !thread->join_reclaiming;
		}
	}
	if (g_pthread_context != nullptr) {
		auto* pool = g_pthread_context->GetPthreadPool();
		if (pool != nullptr) {
			return pool->Contains(thread) && !thread->join_reclaiming;
		}
	}
	return false;
}

static void CurrentNativeJoinIdentity(int32_t& pid, int32_t& tid) noexcept {
	// A selected Program is not ownership (notably on the main launcher).
	// Reuse actual-current capture's native-thread and immutable owner/PID
	// validation, outside record locks; never combine scoped PID with own TID.
	pid = -1;
	tid = -1;
	try {
		PthreadNativeReceipt receipt;
		if (PthreadCaptureCurrentNativeReceipt(receipt) == 0) {
			pid = receipt->identity.pid;
			tid = receipt->identity.tid;
		}
	} catch (...) { pid = -1; tid = -1; } // Identity unavailable never becomes a guessed guest caller.
}

static Common::Mutex g_guest_stack_mutex;
static uint64_t      g_guest_stack_last = 0;
struct CachedGuestStack {
	uint64_t address;
	size_t   map_size;
	size_t   guard_size;
};
static std::vector<CachedGuestStack> g_guest_stack_cache;

static size_t RoundStackSize(size_t size) {
	return ((size + PTHREAD_STACK_PAGE - 1) / PTHREAD_STACK_PAGE) * PTHREAD_STACK_PAGE;
}

static int CreateGuestStack(PthreadAttr attr) {
	if (attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (attr->stack_addr != nullptr) {
		attr->guard_size     = 0;
		attr->stack_user     = true;
		attr->stack_map_addr = 0;
		attr->stack_map_size = 0;
		return OK;
	}

	const auto stack_size = RoundStackSize(attr->stack_size);
	const auto guard_size = RoundStackSize(attr->guard_size);
	const auto map_size   = stack_size + guard_size;

	uint64_t stack_addr = 0;
	bool     cached     = false;
	{
		Common::LockGuard lock(g_guest_stack_mutex);

		auto cached_stack =
		    std::find_if(g_guest_stack_cache.begin(), g_guest_stack_cache.end(),
		                 [map_size, guard_size](const auto& stack) {
			                 return stack.map_size == map_size && stack.guard_size == guard_size;
		                 });
		if (cached_stack != g_guest_stack_cache.end()) {
			stack_addr = cached_stack->address;
			g_guest_stack_cache.erase(cached_stack);
			cached = true;
		} else {
			if (g_guest_stack_last == 0) {
				g_guest_stack_last = PTHREAD_STACK_TOP - PTHREAD_STACK_INITIAL - PTHREAD_STACK_PAGE;
			}
			if (map_size > g_guest_stack_last - PTHREAD_STACK_BOTTOM) {
				LOGF("guest-stack: range exhausted last=0x%llx size=0x%llx\n",
				     (unsigned long long)g_guest_stack_last, (unsigned long long)map_size);
				return KERNEL_ERROR_EAGAIN;
			}
			stack_addr = g_guest_stack_last - map_size;
			g_guest_stack_last -= map_size;
		}
	}

	int result = OK;
	if (!cached) {
		const auto requested = stack_addr;
		stack_addr = Memory::AllocateGuestStackMemory(
		    stack_addr, map_size, Common::VirtualMemory::Mode::ReadWrite, "stack");
		if (stack_addr == 0) {
			LOGF("guest-stack: allocation failed addr=0x%llx size=0x%llx\n",
			     (unsigned long long)requested, (unsigned long long)map_size);
			return KERNEL_ERROR_EAGAIN;
		}
	}

	if (guard_size != 0) {
		result = Memory::KernelMprotect(reinterpret_cast<void*>(stack_addr), guard_size, 0);
		if (result != OK) {
			LOGF("guest-stack: guard protection failed addr=0x%llx size=0x%llx result=0x%x\n",
			     (unsigned long long)stack_addr, (unsigned long long)guard_size, result);
			Memory::KernelMunmap(stack_addr, map_size);
			return KERNEL_ERROR_EAGAIN;
		}
	}

	attr->stack_addr     = reinterpret_cast<void*>(stack_addr + guard_size);
	attr->stack_size     = stack_size;
	attr->stack_user     = false;
	attr->stack_map_addr = stack_addr;
	attr->stack_map_size = map_size;

	std::memset(attr->stack_addr, 0, stack_size);

	return OK;
}

static void FreeGuestStack(PthreadAttr attr) {
	if (attr == nullptr || attr->stack_user || attr->stack_map_addr == 0 ||
	    attr->stack_map_size == 0) {
		return;
	}

	const auto guard_size = attr->stack_map_size - attr->stack_size;
	{
		Common::LockGuard lock(g_guest_stack_mutex);
		g_guest_stack_cache.push_back({attr->stack_map_addr, attr->stack_map_size, guard_size});
	}

	attr->stack_addr     = nullptr;
	attr->stack_map_addr = 0;
	attr->stack_map_size = 0;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestGuestStackOwnerLifecycle(uint64_t* first_address, uint64_t* second_address,
                                  uint64_t* map_size) {
	if (first_address == nullptr || second_address == nullptr || map_size == nullptr) {
		return false;
	}

	size_t flexible_before = 0;
	if (Memory::KernelAvailableFlexibleMemorySize(&flexible_before) != OK) {
		return false;
	}

	PthreadAttr attr = nullptr;
	if (PthreadAttrInit(&attr) != OK) {
		return false;
	}
	if (CreateGuestStack(attr) != OK) {
		PthreadAttrDestroy(&attr);
		return false;
	}
	*first_address = attr->stack_map_addr;
	*map_size      = attr->stack_map_size;
	const bool first_owned =
	    Memory::TestGuestAddressRangeIsOwned(*first_address, static_cast<uint64_t>(*map_size));
	uint64_t   backing_value = 0;
	const bool first_private =
	    !Memory::TryReadBacking(*first_address, &backing_value, sizeof(backing_value));
	size_t     flexible_during_first = 0;
	const bool first_capacity_unchanged =
	    Memory::KernelAvailableFlexibleMemorySize(&flexible_during_first) == OK &&
	    flexible_during_first == flexible_before;
	FreeGuestStack(attr);

	if (CreateGuestStack(attr) != OK) {
		PthreadAttrDestroy(&attr);
		return false;
	}
	*second_address = attr->stack_map_addr;
	const bool second_owned =
	    Memory::TestGuestAddressRangeIsOwned(*second_address, static_cast<uint64_t>(*map_size));
	const bool second_private =
	    !Memory::TryReadBacking(*second_address, &backing_value, sizeof(backing_value));
	size_t     flexible_during_second = 0;
	const bool second_capacity_unchanged =
	    Memory::KernelAvailableFlexibleMemorySize(&flexible_during_second) == OK &&
	    flexible_during_second == flexible_before;
	FreeGuestStack(attr);

	CachedGuestStack cached {};
	bool             found = false;
	{
		Common::LockGuard lock(g_guest_stack_mutex);
		const auto        entry = std::find_if(
		    g_guest_stack_cache.begin(), g_guest_stack_cache.end(),
		    [second_address](const auto& stack) { return stack.address == *second_address; });
		if (entry != g_guest_stack_cache.end()) {
			cached = *entry;
			g_guest_stack_cache.erase(entry);
			found = true;
		}
	}

	const bool unmapped = found && Memory::KernelMunmap(cached.address, cached.map_size) == OK;
	size_t     flexible_after = 0;
	const bool final_capacity_unchanged =
	    Memory::KernelAvailableFlexibleMemorySize(&flexible_after) == OK &&
	    flexible_after == flexible_before;
	return PthreadAttrDestroy(&attr) == OK && first_owned && first_private &&
	       first_capacity_unchanged && second_owned && second_private &&
	       second_capacity_unchanged && unmapped && final_capacity_unchanged;
}
#endif

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void* RunOnGuestStack(void* arg, pthread_entry_func_t func, void* stack_top) {
#if defined(__x86_64__) || defined(_M_X64)
	void*      ret = nullptr;
	const auto aligned_stack_top =
	    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
	const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
	const auto guest_rbp = guest_rsp;

	auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
	guest_root_frame[0]    = 0;
	guest_root_frame[1]    = 0;

	g_guest_entry_return_rsp = guest_rsp - sizeof(uint64_t);

	uintptr_t host_rsp = 0;
	uintptr_t host_rbp = 0;
	uintptr_t host_rbx = 0;
	asm volatile("movq %%rsp, %0\n\t"
	             "movq %%rbp, %1\n\t"
	             "movq %%rbx, %2\n\t"
	             : "=r"(host_rsp), "=r"(host_rbp), "=r"(host_rbx)
	             :
	             : "memory");

	if (g_pthread_self != nullptr) {
		g_pthread_self->guest_host_rbx = host_rbx;
#if defined(__APPLE__)
		g_pthread_self->guest_host_rsp = host_rsp - (2u * sizeof(uint64_t));
#else
		g_pthread_self->guest_host_rsp = host_rsp - (4u * sizeof(uint64_t));
#endif
		g_pthread_self->guest_host_rbp = host_rbp;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		uintptr_t host_gs8  = 0;
		uintptr_t host_gs10 = 0;
		asm volatile("movq %%gs:0x08, %0\n\t"
		             "movq %%gs:0x10, %1\n\t"
		             : "=r"(host_gs8), "=r"(host_gs10)
		             :
		             : "memory");
		g_pthread_self->guest_host_gs8  = host_gs8;
		g_pthread_self->guest_host_gs10 = host_gs10;
#endif
	}

	// The guest ABI expects the entry argument in rdi and a 16-byte aligned stack before call.
#if defined(__APPLE__)
	// Keep inputs out of r12/r13.
	register uintptr_t guest_rsp_reg asm("r14") = guest_rsp;
	register uintptr_t guest_rbp_reg asm("r15") = guest_rbp;
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rsp, %%r12\n\t"
	             "movq %%rbp, %%r13\n\t"
	             "movq %[guest_rsp], %%rsp\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%%rsi\n\t"
	             "movq %%r13, %%rbp\n\t"
	             "movq %%r12, %%rsp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             : "=a"(ret), "+D"(arg), "+S"(func)
	             : [guest_rsp] "r"(guest_rsp_reg), [guest_rbp] "r"(guest_rbp_reg)
	             : "cc", "memory", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
	               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	// PthreadExit resumes at this frame, so all four saved registers stay on the host stack.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Consume stack inputs from registers that the transition does not replace
	// before the switch. Plain "r" can allocate them to r12-r15; r14/r15 are
	// overwritten with TEB limits below, sending execution onto the host stack.
	register uintptr_t guest_rsp_reg asm("r8") = guest_rsp;
	register uintptr_t guest_rbp_reg asm("r9") = guest_rbp;
#endif
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "pushq %%r14\n\t"
	             "pushq %%r15\n\t"
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	             "movq %%gs:0x08, %%r14\n\t"
	             "movq %%gs:0x10, %%r15\n\t"
	             "xorq %%rcx, %%rcx\n\t"
	             "movq %%rcx, %%gs:0x08\n\t"
	             "movq %%rcx, %%gs:0x10\n\t"
#endif
	             "movq %%rsp, %%r12\n\t"
	             "movq %%rbp, %%r13\n\t"
	             "movq %[guest_rsp], %%rsp\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%%rsi\n\t"
	             "movq %%r13, %%rbp\n\t"
	             "movq %%r12, %%rsp\n\t"
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	             "movq %%r14, %%gs:0x08\n\t"
	             "movq %%r15, %%gs:0x10\n\t"
#endif
	             "popq %%r15\n\t"
	             "popq %%r14\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	             : "=a"(ret), "+D"(arg), "+S"(func),
	               [guest_rsp] "+r"(guest_rsp_reg), [guest_rbp] "+r"(guest_rbp_reg)
	             :
	             : "cc", "memory", "rcx", "rdx", "r10", "r11", "xmm0", "xmm1", "xmm2",
	               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	             : "=a"(ret), "+D"(arg), "+S"(func)
	             : [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
	             : "cc", "memory", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13", "xmm0",
	               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
	               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
#endif

	g_guest_entry_return_rsp = 0;
	if (g_pthread_self != nullptr) {
		g_pthread_self->guest_host_rbx = 0;
		g_pthread_self->guest_host_rsp = 0;
		g_pthread_self->guest_host_rbp = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		g_pthread_self->guest_host_gs8  = 0;
		g_pthread_self->guest_host_gs10 = 0;
#endif
	}

	return ret;
#else
	(void)stack_top;
	return func(arg);
#endif
}

static void UpdateCurrentThreadStackAttr(PthreadAttr* attr) {
	if (attr == nullptr || *attr == nullptr) {
		return;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ULONG_PTR low  = 0;
	ULONG_PTR high = 0;
	GetCurrentThreadStackLimits(&low, &high);

	if (low != 0 && high > low) {
		(*attr)->stack_addr = reinterpret_cast<void*>(low);
		(*attr)->stack_size = high - low;
		(*attr)->stack_user = true;
	}
#elif defined(__APPLE__)
	// macOS reports a stack top and size.
	void*        top  = pthread_get_stackaddr_np(pthread_self());
	const size_t size = pthread_get_stacksize_np(pthread_self());

	if (top != nullptr && size != 0) {
		(*attr)->stack_addr = static_cast<void*>(static_cast<uint8_t*>(top) - size);
		(*attr)->stack_size = size;
		(*attr)->stack_user = true;
	}
#else
	// Record the main thread's stack bounds.
	pthread_attr_t self_attr {};
	if (pthread_getattr_np(pthread_self(), &self_attr) == 0) {
		void*  base = nullptr;
		size_t size = 0;
		if (pthread_attr_getstack(&self_attr, &base, &size) == 0 && base != nullptr && size != 0) {
			(*attr)->stack_addr = base;
			(*attr)->stack_size = size;
			(*attr)->stack_user = true;
		}
		pthread_attr_destroy(&self_attr);
	}
#endif
}

static void FreeDetachedThreads(void* /*arg*/) {
	PRINT_NAME_ENABLE(false);

	auto* pthread_pool = g_pthread_context->GetPthreadPool();

	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10000));
		pthread_pool->FreeDetachedThreads();
	}
}

void PthreadDeleteStaticObjects(Loader::Program* program) {

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	pthread_static_objects->DeleteObjects(program);
}

void PthreadInitSelfForMainThread() {
	EXIT_IF(g_pthread_self != nullptr);

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (!Common::HostException::InitializeThreadSignalStack()) {
		EXIT("Failed to initialize the thread signal stack\n");
	}
#endif

	g_pthread_self = new PthreadPrivate {};
	g_pthread_self->owner_program = Loader::RuntimeLinker::GetCurrentProgram();
	g_pthread_self->fs_context = FileSystem::GetCurrentFsContext();
	g_pthread_self->host_lock_critical = Common::GetHostLockCriticalCounter();
	PthreadAttrInit(&g_pthread_self->attr);
	UpdateCurrentThreadStackAttr(&g_pthread_self->attr);
	g_pthread_self->p               = pthread_self();
	g_pthread_self->name            = "MainThread";
	g_pthread_self->guest.thread_id = ++g_pthread_thread_id;
	g_pthread_self->unique_id       = Common::Thread::GetThreadIdUnique();
	g_pthread_self->free            = false;
	g_pthread_self->detached        = false;
	g_pthread_self->almost_done     = false;
	g_pthread_self->entry           = nullptr;
	g_pthread_self->arg             = nullptr;

	uint64_t os_thread_id = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	os_thread_id = static_cast<uint64_t>(GetCurrentThreadId());
#else
	os_thread_id = GetHostThreadId();
#endif
	g_pthread_self->host_thread_id = os_thread_id;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
	    &g_pthread_self->stop_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		g_pthread_self->stop_ready.store(true, std::memory_order_release);
	}
#endif
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(ExternalMains().mutex);
		ExternalMains().records.insert(g_pthread_self);
	}
	g_external_main_lifetime.record = g_pthread_self;

	// ps5-vsh: trace MainThread HLE calls
	Libs::TraceHostThread(g_pthread_self->unique_id);

	// ps5-vsh TEMP DIAG: identify MainThread's OS TID to match against
	// external CPU sampling (spinner hunt).
	LOGF("ps5-vsh: MainThread os_thread_id = %" PRIu64 "\n", os_thread_id);

	LOGF("\tPthread main self: id = %d, os_thread_id = %" PRIu64 ", stack_addr = 0x%016" PRIx64
	     ", stack_size = %" PRIu64 "\n",
	     g_pthread_self->unique_id, os_thread_id,
	     reinterpret_cast<uint64_t>(g_pthread_self->attr->stack_addr),
	     static_cast<uint64_t>(g_pthread_self->attr->stack_size));
}

void* PthreadCreateMainGuestStack() {
	EXIT_IF(g_pthread_self == nullptr);
	EXIT_IF(g_pthread_self->attr == nullptr);

	if (g_pthread_self->attr->stack_map_addr != 0) {
		return reinterpret_cast<void*>(
		    (reinterpret_cast<uintptr_t>(g_pthread_self->attr->stack_addr) +
		     g_pthread_self->attr->stack_size) &
		    ~static_cast<uintptr_t>(0x0f));
	}

	g_pthread_self->attr->stack_addr     = nullptr;
	g_pthread_self->attr->stack_size     = PTHREAD_STACK_DEFAULT + PTHREAD_STACK_EXTRA;
	g_pthread_self->attr->stack_user     = false;
	g_pthread_self->attr->stack_map_addr = 0;
	g_pthread_self->attr->stack_map_size = 0;

	const auto result = CreateGuestStack(g_pthread_self->attr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	auto* stack_top =
	    reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(g_pthread_self->attr->stack_addr) +
	                             g_pthread_self->attr->stack_size) &
	                            ~static_cast<uintptr_t>(0x0f));

	LOGF("\tPthread main guest stack: stack_addr = 0x%016" PRIx64 ", stack_size = %" PRIu64
	     ", stack_top = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(g_pthread_self->attr->stack_addr),
	     static_cast<uint64_t>(g_pthread_self->attr->stack_size),
	     reinterpret_cast<uint64_t>(stack_top));

	return stack_top;
}

void Initialize() {
	PRINT_NAME_ENABLE(false);

	EXIT_IF(g_pthread_context != nullptr);

	g_pthread_context = new PThreadContext;

	g_pthread_context->SetPthreadStaticObjects(new PthreadStaticObjects);
	g_pthread_context->SetPthreadPool(new PthreadPool);
	Common::CondVar::SetWaitPollCallback(KernelDispatchPendingSignalForCurrentThread);

	PthreadMutexattr  default_mutexattr  = nullptr;
	PthreadRwlockattr default_rwlockattr = nullptr;
	PthreadCondattr   default_condattr   = nullptr;
	PthreadAttr       default_attr       = nullptr;

	PthreadAttrInit(&default_attr);
	PthreadMutexattrInit(&default_mutexattr);
	PthreadRwlockattrInit(&default_rwlockattr);
	PthreadCondattrInit(&default_condattr);

	g_pthread_context->SetDefaultMutexattr(default_mutexattr);
	g_pthread_context->SetDefaultRwlockattr(default_rwlockattr);
	g_pthread_context->SetDefaultCondattr(default_condattr);
	g_pthread_context->SetDefaultAttr(default_attr);

	PRINT_NAME_ENABLE(true);

	Common::Thread thread(FreeDetachedThreads, nullptr);
	thread.Detach();
}

static int PthreadAttrCopy(PthreadAttr* dst, const PthreadAttr* src) {
	if (dst == nullptr || *dst == nullptr || src == nullptr || *src == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	int              solosched     = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	int result = 0;

	result = (result == 0 ? PthreadAttrGetaffinity(src, &mask) : result);
	result = (result == 0 ? PthreadAttrGetdetachstate(src, &state) : result);
	result = (result == 0 ? PthreadAttrGetguardsize(src, &guard_size) : result);
	result = (result == 0 ? PthreadAttrGetinheritsched(src, &inherit_sched) : result);
	result = (result == 0 ? PthreadAttrGetschedparam(src, &param) : result);
	result = (result == 0 ? PthreadAttrGetschedpolicy(src, &policy) : result);
	result = (result == 0 ? PthreadAttrGetsolosched(src, &solosched) : result);
	result = (result == 0 ? PthreadAttrGetstackaddr(src, &stack_addr) : result);
	result = (result == 0 ? PthreadAttrGetstacksize(src, &stack_size) : result);

	result = (result == 0 ? PthreadAttrSetaffinity(dst, mask) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(dst, state) : result);
	result = (result == 0 ? PthreadAttrSetguardsize(dst, guard_size) : result);
	result = (result == 0 ? PthreadAttrSetinheritsched(dst, inherit_sched) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(dst, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(dst, policy) : result);
	result = (result == 0 ? PthreadAttrSetsolosched(dst, solosched) : result);
	result = (result == 0 ? PthreadAttrSetscope(dst, (*src)->scope) : result);
	if (stack_addr != nullptr) {
		result = (result == 0 ? PthreadAttrSetstackaddr(dst, stack_addr) : result);
	}
	if (stack_size != 0) {
		result = (result == 0 ? PthreadAttrSetstacksize(dst, stack_size) : result);
	}
	return result;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
int TestPthreadAttrCopy(PthreadAttr* dst, const PthreadAttr* src) {
	return PthreadAttrCopy(dst, src);
}
#endif

static void PthreadAttrDbgPrint(const PthreadAttr* src) {
	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	int              solosched     = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	PthreadAttrGetaffinity(src, &mask);
	PthreadAttrGetdetachstate(src, &state);
	PthreadAttrGetguardsize(src, &guard_size);
	PthreadAttrGetinheritsched(src, &inherit_sched);
	PthreadAttrGetschedparam(src, &param);
	PthreadAttrGetschedpolicy(src, &policy);
	PthreadAttrGetsolosched(src, &solosched);
	PthreadAttrGetstackaddr(src, &stack_addr);
	PthreadAttrGetstacksize(src, &stack_size);

	LOGF("\tcpu_mask       = 0x%" PRIx64 "\n"
	     "\tdetach_state   = %d\n"
	     "\tguard_size     = %" PRIu64 "\n"
	     "\tinherit_sched  = %d\n"
	     "\tsched_priority = %d\n"
	     "\tpolicy         = %d\n"
	     "\tsolosched      = %d\n"
	     "\tstack_addr     = 0x%016" PRIx64 "\n"
	     "\tstack_size    = %" PRIu64 "\n",
	     mask, state, guard_size, inherit_sched, param.sched_priority, policy, solosched,
	     reinterpret_cast<uint64_t>(stack_addr), static_cast<uint64_t>(stack_size));
}

static constexpr int32_t DST_NONE = 0;
static constexpr int32_t DST_MET  = 4;

static int32_t GetDstSeconds() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	TIME_ZONE_INFORMATION tzi {};
	const DWORD           result = GetTimeZoneInformation(&tzi);
	return (result == TIME_ZONE_ID_DAYLIGHT ? -tzi.DaylightBias * 60 : 0);
#else
	const std::time_t now = std::time(nullptr);
	std::tm           local_tm {};
	localtime_r(&now, &local_tm);
	return (local_tm.tm_isdst > 0 ? 3600 : 0);
#endif
}

static bool GetPosixClockId(KernelClockid clock_id, clockid_t* out) {
	EXIT_IF(out == nullptr);

	switch (clock_id) {
		case KERNEL_CLOCK_REALTIME:
		case KERNEL_CLOCK_REALTIME_PRECISE:
		case KERNEL_CLOCK_REALTIME_FAST:
		case KERNEL_CLOCK_SECOND: *out = CLOCK_REALTIME; return true;
		case KERNEL_CLOCK_MONOTONIC:
		case KERNEL_CLOCK_UPTIME:
		case KERNEL_CLOCK_UPTIME_PRECISE:
		case KERNEL_CLOCK_UPTIME_FAST:
		case KERNEL_CLOCK_MONOTONIC_PRECISE:
		case KERNEL_CLOCK_MONOTONIC_FAST:
		case KERNEL_CLOCK_EXT_NETWORK:
		case KERNEL_CLOCK_EXT_DEBUG_NETWORK:
		case KERNEL_CLOCK_EXT_AD_NETWORK:
		case KERNEL_CLOCK_EXT_RAW_NETWORK: *out = CLOCK_MONOTONIC; return true;
		default: return false;
	}
}

static bool NativeClockGettime(KernelClockid clock_id, KernelTimespec* tp) {
	EXIT_IF(tp == nullptr);

	int error = OK;
	if (KernelClockGettimeSpecial(clock_id, tp, &error)) {
		return error == OK;
	}

	clockid_t native_clock_id {};
	if (!GetPosixClockId(clock_id, &native_clock_id)) {
		return false;
	}

	timespec native_time {};
	if (::clock_gettime(native_clock_id, &native_time) != 0) {
		return false;
	}

	tp->tv_sec  = native_time.tv_sec;
	tp->tv_nsec = native_time.tv_nsec;
	return true;
}

static int PthreadMutexInitNamed(PthreadMutex* mutex, const PthreadMutexattr* attr,
                                 const char* name, int static_type = 0);
static int PthreadRwlockInitNamed(PthreadRwlock* rwlock, const PthreadRwlockattr* attr,
                                  const char* name);
static int PthreadCondInitNamed(PthreadCond* cond, const PthreadCondattr* attr, const char* name);

// ps5-vsh TEMP-DIAG (chainSUI129): trace the two loader-lock mutexes that
// produce the mono_os_mutex_unlock EPERM spin. Gated on guest addr: zero
// cost otherwise. Remove once the EPERM source is identified.
static bool Ps5VshTraceMutex(uint64_t vaddr) {
	return vaddr == 0x9604faa00ull || vaddr == 0x2e69e4000ull;
}
static uint64_t Ps5VshRa() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return reinterpret_cast<uint64_t>(_ReturnAddress());
#else
	return 0;
#endif
}

// ps5-vsh TEMP-DIAG (chainSUI129): guest return address of the current HLE
// call. Captured with __builtin_return_address(0) at HLE entry (where it
// yields the guest caller, cf. Equeue wait logs); stashed in TLS so deep
// helpers (lazy init, NativeMutexUnlock) can attribute. Cleared never:
// overwritten on every mutex HLE entry.
static thread_local uint64_t g_ps5vsh_guest_ra = 0;
static inline uint64_t Ps5VshGuestRa() {
	return g_ps5vsh_guest_ra;
}

// ps5-vsh (chainSUI137): set when CreateObject lazily creates a host mutex
// in the current call; cleared at every mutex HLE entry. Lets unlock treat
// EPERM-on-fresh-object as no-op success (guest heap reuse clobbered the
// word; HW never exhibits this state). Live-object double-unlocks still
// return EPERM.
static thread_local bool g_ps5vsh_mutex_fresh = false;

// ps5-vsh TEMP-DIAG (chainSUI135): passive lock/unlock op ring. An int3 on
// the lock PLT destabilized early boot, so pair locks with unlocks here:
// one cheap record per mutex HLE (same cost class as the existing census
// fetch_adds). On EPERM the tail is dumped: the lock/unlock imbalance is
// temporally adjacent. Remove with the other TEMP-DIAG once root-caused.
struct Ps5VshMutexOp {
	uint64_t vaddr;
	uint32_t op; // 0=lock 1=unlock 2=trylock 3=timedlock
	int32_t  result;
	uint32_t tid;
};
static constexpr uint64_t kPs5VshMutexOps = 4096;
static Ps5VshMutexOp              g_ps5vsh_mutex_ops[kPs5VshMutexOps] = {};
static std::atomic<uint64_t>      g_ps5vsh_mutex_op_idx {0};
static inline void Ps5VshRecordMutexOp(uint64_t vaddr, uint32_t op, int result) {
	const uint64_t n = g_ps5vsh_mutex_op_idx.fetch_add(1, std::memory_order_relaxed);
	auto&          slot = g_ps5vsh_mutex_ops[n % kPs5VshMutexOps];
	slot.vaddr         = vaddr;
	slot.op            = op;
	slot.result        = result;
	slot.tid           = Common::Thread::GetThreadIdUnique();
}

static void NativeMutexRemoveWaiter(PthreadMutexPrivate* mutex,
                                     PthreadMutexPrivate::Waiter* waiter) {
	if (waiter->previous != nullptr) {
		waiter->previous->next = waiter->next;
	} else {
		mutex->first_waiter = waiter->next;
	}
	if (waiter->next != nullptr) {
		waiter->next->previous = waiter->previous;
	} else {
		mutex->last_waiter = waiter->previous;
	}
}

static bool RuntimeWaitCanceled(const Loader::RuntimeGuestExecution* execution);
// Caller holds mutex->m. A FIFO-selected waiter has already been unlinked;
// cancellation must pass on its transferred ownership, not unlink it twice.
static void NativeMutexPassOwnershipLocked(PthreadMutexPrivate* mutex) {
	if (auto* waiter = mutex->first_waiter) {
		NativeMutexRemoveWaiter(mutex, waiter);
		mutex->owner = waiter->thread;
		mutex->count = 1;
		waiter->cv.notify_one();
	} else {
		mutex->owner = nullptr;
		mutex->count = 0;
	}
}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
using RuntimeMutexHandoffHook = void (*)(PthreadMutexPrivate*, Loader::RuntimeGuestExecution*);
static std::atomic<RuntimeMutexHandoffHook> g_runtime_mutex_handoff_for_test {nullptr};
#endif
static int NativeMutexLock(PthreadMutexPrivate* mutex, KernelUseconds* timeout_us,
                           Loader::RuntimeGuestExecution* execution = nullptr) {
	EXIT_IF(mutex == nullptr);

	auto* self = g_pthread_self;
	EXIT_NOT_IMPLEMENTED(self == nullptr);

	std::unique_lock lock(mutex->m);
	if (RuntimeWaitCanceled(execution)) { return ECANCELED; }

	if (mutex->owner == self) {
		if (mutex->type == 2) {
			if (mutex->count == UINT32_MAX) {
				return EAGAIN;
			}
			mutex->count++;
			return OK;
		}
		return EDEADLK;
	}

	if (mutex->owner == nullptr) {
		mutex->owner = self;
		mutex->count = 1;
		return OK;
	}
	if (timeout_us != nullptr && *timeout_us == 0) {
		return ETIMEDOUT;
	}

	// Guest mutex waits are not cancellation points, unlike the host condition wait.
	int previous_cancel_state = 0;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel_state);
	PthreadMutexPrivate::Waiter waiter;
	waiter.thread   = self;
	waiter.previous = mutex->last_waiter;
	if (mutex->last_waiter != nullptr) {
		mutex->last_waiter->next = &waiter;
	} else {
		mutex->first_waiter = &waiter;
	}
	mutex->last_waiter = &waiter;
	const auto deadline = timeout_us != nullptr
	                          ? std::chrono::steady_clock::now() +
	                                std::chrono::microseconds(*timeout_us)
	                          : std::chrono::steady_clock::time_point::max();
	int result = OK;
	while (mutex->owner != self) {
		if (RuntimeWaitCanceled(execution)) {
			NativeMutexRemoveWaiter(mutex, &waiter);
			result = ECANCELED;
			break;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			NativeMutexRemoveWaiter(mutex, &waiter);
			result = ETIMEDOUT;
			break;
		}
		waiter.cv.wait_until(lock, std::min(deadline, now +
		    std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)));
		if (mutex->owner != self && !execution) {
			lock.unlock();
			KernelDispatchPendingSignalForCurrentThread();
			lock.lock();
		}
	}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (result == OK && execution) {
		if (auto hook = g_runtime_mutex_handoff_for_test.load(std::memory_order_acquire)) { hook(mutex, execution); }
	}
#endif
	if (result == OK && RuntimeWaitCanceled(execution)) {
		NativeMutexPassOwnershipLocked(mutex);
		result = ECANCELED;
	}
	lock.unlock();
	pthread_setcancelstate(previous_cancel_state, nullptr);
	return result;
}

static int NativeMutexTrylock(PthreadMutexPrivate* mutex) {
	EXIT_IF(mutex == nullptr);

	auto* self = g_pthread_self;
	EXIT_NOT_IMPLEMENTED(self == nullptr);

	std::unique_lock lock(mutex->m);

	if (mutex->owner == self) {
		if (mutex->type == 2) {
			if (mutex->count == UINT32_MAX) {
				return EAGAIN;
			}
			mutex->count++;
			return OK;
		}
		return EBUSY;
	}

	if (mutex->owner != nullptr) {
		return EBUSY;
	}

	mutex->owner = self;
	mutex->count = 1;
	return OK;
}

static int NativeMutexUnlock(PthreadMutexPrivate* mutex, uint32_t* recurse = nullptr) {
	EXIT_IF(mutex == nullptr);

	auto* self = g_pthread_self;
	EXIT_NOT_IMPLEMENTED(self == nullptr);

	std::unique_lock lock(mutex->m);

	if (mutex->owner != self) {
		uint64_t guest_word = 0;
		uint64_t guest_addr = 0;
		// ps5-vsh TEMP-DIAG: recover the guest variable backing this host
		// object by name (Static%016x). Content tells us what mono object
		// is really being unlocked.
		if (mutex->name.size() == 22 &&
		    mutex->name.compare(0, 6, "Static") == 0) {
			guest_addr = std::strtoull(mutex->name.c_str() + 6, nullptr, 16);
			if (Graphics::HostMemoryRangeIsReadable(guest_addr, 8)) {
				std::memcpy(&guest_word,
				            reinterpret_cast<const void*>(
				                static_cast<uintptr_t>(guest_addr)),
				            sizeof(guest_word));
			}
		}
		LOGF("ps5-vsh: EPERM unlock: mutex=%p name=%s type=%d count=%u owner=%p self=%p "
		     "tid=%d ra=0x%016llx guest=0x%016llx recurse=%d gaddr=%llx gword=%llx\n",
		     static_cast<const void*>(mutex), mutex->name.c_str(), mutex->type,
		     mutex->count, static_cast<const void*>(mutex->owner),
		     static_cast<const void*>(self), Common::Thread::GetThreadIdUnique(),
		     static_cast<unsigned long long>(Ps5VshRa()),
		     static_cast<unsigned long long>(Ps5VshGuestRa()), recurse != nullptr,
		     static_cast<unsigned long long>(guest_addr),
		     static_cast<unsigned long long>(guest_word));
		// ps5-vsh TEMP-DIAG: which stack does HLE run on + guest caller scan.
		// If HLE runs on the guest stack, upward qwords in the monosgen
		// range are guest return addresses (caller rvas).
		{
			const auto* frame =
			    reinterpret_cast<const uint64_t*>(&mutex);
			uint64_t hits[8] = {};
			int      nhits   = 0;
			for (int wi = 0; wi < 2048 && nhits < 8; wi++) {
				const uint64_t wa =
				    reinterpret_cast<uint64_t>(frame) + static_cast<uint64_t>(wi) * 8u;
				if (!Graphics::HostMemoryRangeIsReadable(wa, 8)) {
					break;
				}
				uint64_t wv = 0;
				std::memcpy(&wv, reinterpret_cast<const void*>(wa), 8);
				if (wv >= 0x960000000ull && wv < 0x9603d67aeull) {
					hits[nhits++] = wv;
				}
			}
			LOGF("ps5-vsh: EPERM frame=%llx hits=%d %llx %llx %llx %llx %llx %llx %llx %llx\n",
			     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(frame)),
			     nhits, static_cast<unsigned long long>(hits[0]),
			     static_cast<unsigned long long>(hits[1]),
			     static_cast<unsigned long long>(hits[2]),
			     static_cast<unsigned long long>(hits[3]),
			     static_cast<unsigned long long>(hits[4]),
			     static_cast<unsigned long long>(hits[5]),
			     static_cast<unsigned long long>(hits[6]),
			     static_cast<unsigned long long>(hits[7]));
		}
		// ps5-vsh TEMP-DIAG: dump this mutex's op history (lock/unlock
		// pairing). Ops: 0=lock 1=unlock 2=trylock 3=timedlock.
		if (guest_addr != 0) {
			const uint64_t total = g_ps5vsh_mutex_op_idx.load(std::memory_order_relaxed);
			const uint64_t back =
			    (total < 2048ull ? total : 2048ull);
			uint64_t shown = 0;
			for (uint64_t k = 0; k < back && shown < 40; k++) {
				const uint64_t       seq  = total - 1 - k;
				const Ps5VshMutexOp& op   = g_ps5vsh_mutex_ops[seq % kPs5VshMutexOps];
				if (op.vaddr != guest_addr) {
					continue;
				}
				LOGF("ps5-vsh: EPERM hist #%llu op=%u res=%d tid=%u\n",
				     static_cast<unsigned long long>(seq), op.op, op.result, op.tid);
				shown++;
			}
			Log::Flush();
		}
		return EPERM;
	}

	if (recurse != nullptr) {
		*recurse     = mutex->count;
		mutex->count = 1;
	}

	if (mutex->type == 2 && mutex->count > 1) {
		mutex->count--;
		return OK;
	}

	NativeMutexPassOwnershipLocked(mutex);

	return OK;
}

// A runtime coordinator's boundary-free HOST join/wait is not a guest wait.
// Its own guest callbacks still have boundaries and receive scoped cancellation.
static Loader::RuntimeGuestExecution* RuntimeGuestWaitExecution() {
	auto* self = g_pthread_self;
	return self && self->execution && Loader::GuestCallBoundary::CurrentDepth() ? self->execution.get() : nullptr;
}
static bool RuntimeWaitCanceled(const Loader::RuntimeGuestExecution* execution) {
	return execution && (execution->StopRequested() || execution->PendingThreadExit());
}
static Loader::GuestCallOwner CurrentWaitOwner() {
	auto execution = Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram());
	return execution ? execution->Owner() : Loader::GuestCallOwner {};
}
static bool RuntimeWaitOwnerMatches(const Loader::RuntimeGuestExecution* execution, Loader::GuestCallOwner owner) {
	return !execution || (execution->HasHostCallForCurrentBoundary() && execution->FailureFunction() && owner.identity == execution->Owner().identity &&
	                      owner.token == execution->Owner().token);
}

static int NativeMutexLockRecurse(PthreadMutexPrivate* mutex, uint32_t recurse,
                                  Loader::RuntimeGuestExecution* execution = nullptr) {
	EXIT_IF(mutex == nullptr);

	// Keep one FIFO waiter, rather than dropping/requeueing it each stop poll.
	int result = NativeMutexLock(mutex, nullptr, execution);
	if (result == OK && RuntimeWaitCanceled(execution)) {
		NativeMutexUnlock(mutex); // Acquired once, recursion not yet restored.
		return ECANCELED;
	}
	if (result == OK) {
		std::lock_guard lock(mutex->m);
		mutex->count = std::max<uint32_t>(recurse, 1);
	}
	return result;
}

static bool NativeCondDeadlineFromAbs(KernelClockid clock_id, const KernelTimespec* abstime,
                                      std::chrono::steady_clock::time_point* out) {
	if (abstime == nullptr || out == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000) {
		return false;
	}

	KernelTimespec now {};
	if (!NativeClockGettime(clock_id, &now)) {
		return false;
	}

	const auto host_now = std::chrono::steady_clock::now();
	if (abstime->tv_sec < now.tv_sec ||
	    (abstime->tv_sec == now.tv_sec && abstime->tv_nsec <= now.tv_nsec)) {
		*out = host_now;
		return true;
	}
	// Guest absolute deadlines may exceed the host clock's nanosecond range
	// (Esvm uses INT64_MAX microseconds). Normalize before converting units;
	// overflowing seconds -> nanoseconds otherwise turns a future wait into a
	// repeated immediate timeout. Saturation retains ordinary signal/APC wakes.
	auto seconds = abstime->tv_sec - now.tv_sec;
	auto nanos = abstime->tv_nsec - now.tv_nsec;
	if (nanos < 0) { --seconds; nanos += 1000000000; }
	const auto room = std::chrono::steady_clock::time_point::max() - host_now;
	if (seconds > std::chrono::duration_cast<std::chrono::seconds>(room).count()) {
		*out = std::chrono::steady_clock::time_point::max();
		return true;
	}
	const auto whole = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
	    std::chrono::seconds(seconds));
	const auto fraction = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
	    std::chrono::nanoseconds(nanos));
	*out = fraction >= room - whole ? std::chrono::steady_clock::time_point::max()
	                              : host_now + whole + fraction;
	return true;
}

namespace {
// ps5-vsh: live host pthread objects (mutex/cond/rwlock privates). CreateObject
// must not trust a guest variable holding garbage as a host object pointer;
// validation against this set turns wild values into fresh initialization.
Common::Mutex                         g_pthread_live_mutex;
std::unordered_set<const void*>       g_pthread_live_objects;
	} // namespace

// ps5-vsh (chainSUI141): shadow guest-addr -> last host object. The guest
// recycles heap blocks (munmap/mmap arenas, jemalloc reuse, vsnprintf
// scratch) while mono still references pthread words inside them. The
// word then holds freelist/string garbage and lazy reinit would reset a
// HELD mutex to free (downstream EBUSY-where-HW-succeeds, double-unlock
// EPERM, self-lock EDEADLK). HW keeps kernel mutex state outside user
// memory, so mirror that: on a dead word, reuse the shadow object
// (preserving owner/count) instead of resetting. Word-live always wins
// and refreshes the shadow; explicit destroy erases it.
namespace {
struct PthreadShadowEntry {
	const void*               object;
	PthreadStaticObject::Type type;
};
Common::Mutex                                        g_pthread_shadow_mutex;
std::unordered_map<uint64_t, PthreadShadowEntry>     g_pthread_shadow;
} // namespace

static void PthreadShadowPut(uint64_t vaddr, PthreadStaticObject::Type type,
                             const void* object) {
	if (vaddr == 0 || object == nullptr) {
		return;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_shadow_mutex);
	g_pthread_shadow[vaddr] = PthreadShadowEntry{object, type};
}

static bool PthreadShadowGet(uint64_t vaddr, PthreadStaticObject::Type type,
                             const void** object_out) {
	if (vaddr == 0 || object_out == nullptr) {
		return false;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_shadow_mutex);
	const auto it = g_pthread_shadow.find(vaddr);
	if (it == g_pthread_shadow.end() || it->second.type != type) {
		return false;
	}
	*object_out = it->second.object;
	return *object_out != nullptr;
}

static void** PthreadShadowSlot(uint64_t vaddr, PthreadStaticObject::Type type) {
	if (vaddr == 0) {
		return nullptr;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_shadow_mutex);
	const auto it = g_pthread_shadow.find(vaddr);
	if (it == g_pthread_shadow.end() || it->second.type != type || it->second.object == nullptr) {
		return nullptr;
	}
	return const_cast<void**>(&it->second.object);
}

// ps5-vsh chainSUI211: guest-word preservation for dual-use mutex words.
// Mono locks klass-adjacent words whose content is live guest data (e.g.
// klass->parent at 0x2e6e84000); writing our host pointer there derails
// mono's klass-byte hashing (0x1ea4cd bitmap OOB). Sony keeps mutex state
// kernel-side and never writes such words, so neither do we: addrs in this
// set get their guest word restored on HLE exit; ops resolve the host
// object via the shadow map (stable across restores). Marked when
// CreateObject displaces a non-live pointer-sized word (genuine BSS zeros
// and small-int statics keep the legacy write-through).
static bool PthreadHostObjectAlive(const void* p);
Common::Mutex g_pthread_preserved_mutex;
std::unordered_set<uint64_t> g_pthread_preserved;
static void PthreadPreservedMark(uint64_t vaddr) {
	if (vaddr == 0) {
		return;
	}
	Common::LockGuard lock(g_pthread_preserved_mutex);
	if (g_pthread_preserved.insert(vaddr).second) {
		LOGF("ps5-vsh: preserved mutex word at %llx\n",
		     (unsigned long long)vaddr);
	}
}
static bool PthreadPreservedHas(uint64_t vaddr) {
	if (vaddr == 0) {
		return false;
	}
	Common::LockGuard lock(g_pthread_preserved_mutex);
	return g_pthread_preserved.count(vaddr) != 0;
}
struct PthreadGuestWordGuard {
	uint64_t vaddr = 0;
	void*    saved = nullptr;
	void*    host  = nullptr;
	bool     armed = false;
	PthreadGuestWordGuard() = default;
	PthreadGuestWordGuard(uint64_t a, void* s, void* h) { arm(a, s, h); }
	void arm(uint64_t a, void* s, void* h) {
		if (a != 0 && s != nullptr && s != h && h != nullptr &&
		    PthreadHostObjectAlive(h) && PthreadPreservedHas(a)) {
			vaddr = a;
			saved = s;
			host  = h;
			armed = true;
		}
	}
	~PthreadGuestWordGuard() {
		if (!armed) {
			return;
		}
		if (!Graphics::HostMemoryRangeIsReadable(vaddr, 8)) {
			return;
		}
		void* cur = nullptr;
		std::memcpy(&cur, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(vaddr)), sizeof(cur));
		// Restore only if nobody else changed the word since (guest
		// rewrites and concurrent ops win over our restore).
		if (cur == host) {
			std::memcpy(reinterpret_cast<void*>(
			    static_cast<uintptr_t>(vaddr)), &saved, sizeof(saved));
		}
	}
};

static void PthreadShadowErase(uint64_t vaddr) {
	if (vaddr == 0) {
		return;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_shadow_mutex);
	g_pthread_shadow.erase(vaddr);
}

static void PthreadHostObjectAdd(const void* p) {
	if (p == nullptr) {
		return;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_live_mutex);
	g_pthread_live_objects.insert(p);
}

static void PthreadHostObjectRemove(const void* p) {
	if (p == nullptr) {
		return;
	}
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_live_mutex);
	g_pthread_live_objects.erase(p);
}

static bool PthreadHostObjectAlive(const void* p) {
	if (p == nullptr) {
		return false;
	}
	// Cover acquisition too: Windows can be suspended after taking its lock
	// bit but before publishing owner metadata inside RtlEnterCriticalSection.
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_pthread_live_mutex);
	return g_pthread_live_objects.count(p) != 0;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestPthreadLiveLockWithCallback(void (*callback)(), bool before_acquire, bool shadow) {
	Common::HostLockCriticalGuard critical;
	if (before_acquire) {
		callback();
	}
	Common::LockGuard lock(shadow ? g_pthread_shadow_mutex : g_pthread_live_mutex);
	if (!before_acquire) {
		callback();
	}
}
#endif

void* PthreadStaticObjects::CreateObject(void* addr, PthreadStaticObject::Type type,
                                        void** initial_word, Loader::RuntimeGuestExecution* execution, int* status) {
	if (status) { *status = KERNEL_ERROR_EINVAL; }
	const auto reject = [status](int error) { if (status) { *status = error; } };
	if (execution) {
		// No legacy lazy-init/shadow repair without the exact current lifetime
		// and its returning host barrier, even for an uncontended acquisition.
		if ((type != PthreadStaticObject::Type::Mutex && type != PthreadStaticObject::Type::Cond) ||
		    !RuntimeWaitOwnerMatches(execution, CurrentWaitOwner())) {
			reject(KERNEL_ERROR_EOPNOTSUPP); return nullptr;
		}
		if (RuntimeWaitCanceled(execution)) { reject(KERNEL_ERROR_ECANCELED); return nullptr; }
	}
	const auto admitted_mutex = [&](const void* object) {
		if (type == PthreadStaticObject::Type::Cond) {
			const auto condition = PinCondition(object);
			if (!condition || !RuntimeWaitOwnerMatches(execution, condition->wait_owner)) {
				reject(KERNEL_ERROR_EOPNOTSUPP); return false;
			}
			if (RuntimeWaitCanceled(execution)) { reject(KERNEL_ERROR_ECANCELED); return false; }
			return true;
		}
		if (!execution) { return true; }
		if (!RuntimeWaitOwnerMatches(execution, static_cast<const PthreadMutexPrivate*>(object)->wait_owner)) {
			reject(KERNEL_ERROR_EOPNOTSUPP); return false;
		}
		if (RuntimeWaitCanceled(execution)) { reject(KERNEL_ERROR_ECANCELED); return false; }
		return true;
	};
	if (initial_word != nullptr) {
		*initial_word = nullptr;
	}
	if (addr == nullptr) {
		return addr;
	}
	if (execution && ((reinterpret_cast<uintptr_t>(addr) % alignof(void*)) != 0 ||
	    !Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(addr), sizeof(void*)))) {
		reject(KERNEL_ERROR_EFAULT); return nullptr;
	}

	auto*      current       = static_cast<void**>(addr);
	auto       current_ref   = std::atomic_ref<void*>(*current);
	auto*      current_value = current_ref.load(std::memory_order_acquire);
	// Reuse the required guest-word load for preservation at the call site.
	// Capture before validation, lazy initialization, or shadow-slot selection;
	// the locked recheck below must not replace this initial observation.
	if (initial_word != nullptr) {
		*initial_word = current_value;
	}
	const bool adaptive_static_mutex =
	    (type == PthreadStaticObject::Type::Mutex &&
	     current_value == reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

	// ps5-vsh: a guest variable holding garbage (never initialized, stale heap)
	// must not be trusted as a host object pointer: locking it crashes inside
	// MSVCP (mtx_lock on wild memory). Only live host objects pass through;
	// everything else falls through to fresh initialization below.
	if (current_value != nullptr && !adaptive_static_mutex &&
	    !PthreadHostObjectAlive(current_value)) {
		LOGF("\tstatic object at %p holds non-live value %p; reinitializing\n", addr,
		     current_value);
		current_value = nullptr;
	}

	if (current_value != nullptr && !adaptive_static_mutex) {
		if (!admitted_mutex(current_value)) { return nullptr; }
		if (type == PthreadStaticObject::Type::Cond) { return addr; }
		PthreadShadowPut(reinterpret_cast<uint64_t>(addr), type, current_value);
		if (void** slot = PthreadShadowSlot(reinterpret_cast<uint64_t>(addr), type); slot != nullptr) {
			return slot;
		}
		return addr;
	}

	Common::LockGuard lock(m_mutex);

	current_value = current_ref.load(std::memory_order_acquire);
	const bool adaptive_static_mutex_after_lock =
	    (type == PthreadStaticObject::Type::Mutex &&
	     current_value == reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

	if (current_value != nullptr && !adaptive_static_mutex_after_lock &&
	    PthreadHostObjectAlive(current_value)) {
		if (!admitted_mutex(current_value)) { return nullptr; }
		if (type == PthreadStaticObject::Type::Cond) { return addr; }
		PthreadShadowPut(reinterpret_cast<uint64_t>(addr), type, current_value);
		if (void** slot = PthreadShadowSlot(reinterpret_cast<uint64_t>(addr), type); slot != nullptr) {
			return slot;
		}
		return addr;
	}

	// ps5-vsh (chainSUI141): dead word, but the shadow remembers a host
	// object for this guest address (heap reuse clobbered the word). Reuse
	// it — preserving owner/count — instead of resetting a held lock.
	const void* shadow_object = nullptr;
	if (PthreadShadowGet(reinterpret_cast<uint64_t>(addr), type, &shadow_object) &&
	    PthreadHostObjectAlive(shadow_object)) {
		if (!admitted_mutex(shadow_object)) { return nullptr; }
		if (type == PthreadStaticObject::Type::Cond) {
			if (execution && !Loader::RuntimeCrt::WritableSpan(Loader::RuntimeCrt::Context {}, addr, sizeof(void*))) {
				reject(KERNEL_ERROR_EOPNOTSUPP); return nullptr;
			}
			auto publish = [&] { current_ref.store(const_cast<void*>(shadow_object), std::memory_order_release); };
			if (execution) {
				auto lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
				if (!lease || !lease.Publish(publish)) { reject(KERNEL_ERROR_ECANCELED); return nullptr; }
			} else { publish(); }
			return addr;
		}
		LOGF("\tstatic object at %p reuses shadow object %p (word was clobbered)\n",
		     addr, shadow_object);
		if (type == PthreadStaticObject::Type::Mutex && current_value != nullptr &&
		    reinterpret_cast<uint64_t>(current_value) >= 0x10000u) {
			PthreadPreservedMark(reinterpret_cast<uint64_t>(addr));
		} else if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(addr), 8)) {
			current_ref.store(const_cast<void*>(shadow_object),
			                  std::memory_order_release);
		}
		if (void** slot = PthreadShadowSlot(reinterpret_cast<uint64_t>(addr), type); slot != nullptr) {
			return slot;
		}
		return addr;
	}

	auto* rt      = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto  vaddr   = reinterpret_cast<uint64_t>(addr);
	auto* program = rt->FindProgramByAddr(vaddr);
	if (execution && (!program || Loader::GetApplicationExecution(program).get() != execution)) {
		// An uninitialized heap/session/foreign word carries no lifetime proof.
		// Explicitly initialized owned heap objects above remain supported; lazy
		// initialization requires this owner's private image mapping.
		reject(KERNEL_ERROR_EOPNOTSUPP); return nullptr;
	}
	if (RuntimeWaitCanceled(execution)) { reject(KERNEL_ERROR_ECANCELED); return nullptr; }

	std::string name = fmt::format("Static{:016x}", vaddr);

	// ps5-vsh (chainSUI137): this call creates a brand-new host object: the
	// guest never locked this incarnation (fresh static, or clobbered word
	// after guest heap reuse). Record it so an immediately-following unlock
	// EPERM can be treated as a no-op success instead of killing boot with
	// a state HW never exhibits. Mutex type only; cleared at every mutex
	// HLE entry so it strictly means "created by THIS call".
	if (type == PthreadStaticObject::Type::Mutex) {
		g_ps5vsh_mutex_fresh = true;
	}

	// ps5-vsh chainSUI211: InitNamed below overwrites the guest word; if the
	// displaced word looks like live guest data, preserve it immediately and
	// return the host shadow slot so guest memory is never left displaced.
	const bool preserve_displaced_word =
	    (type == PthreadStaticObject::Type::Mutex && current_value != nullptr &&
	     !adaptive_static_mutex_after_lock && !PthreadHostObjectAlive(current_value) &&
	     reinterpret_cast<uint64_t>(current_value) >= 0x10000u);
	if (preserve_displaced_word) {
		PthreadPreservedMark(vaddr);
	}
	int result = OK;
	switch (type) {
		case PthreadStaticObject::Type::Mutex:
			result = PthreadMutexInitNamed(static_cast<PthreadMutex*>(addr), nullptr, name.c_str(),
			                               adaptive_static_mutex_after_lock ? 4 : 0);
			break;
		case PthreadStaticObject::Type::Cond:
			result = PthreadCondInitNamed(static_cast<PthreadCond*>(addr), nullptr, name.c_str());
			break;
		case PthreadStaticObject::Type::Rwlock:
			result =
			    PthreadRwlockInitNamed(static_cast<PthreadRwlock*>(addr), nullptr, name.c_str());
			break;
		default: EXIT("unknown type: %d\n", static_cast<int>(type));
	}

	if (execution && result != OK) { reject(result); return nullptr; }
	EXIT_NOT_IMPLEMENTED(result != OK);
	if (preserve_displaced_word && Graphics::HostMemoryRangeIsReadable(vaddr, 8)) {
		current_ref.store(current_value, std::memory_order_release);
	}

	void* ret_slot = type == PthreadStaticObject::Type::Cond ? addr : PthreadShadowSlot(vaddr, type);
	if (ret_slot == nullptr) {
		ret_slot = addr;
	}

	// Heap-backed lazy pthread objects are valid. Initialize them without requiring
	// an owning ELF segment; only segment-backed objects need module-unload cleanup bookkeeping.
	if (program == nullptr) {
		return ret_slot;
	}

	auto* obj    = new PthreadStaticObject;
	obj->program = program;
	obj->type    = type;
	obj->vaddr   = vaddr;

	const auto it = std::find(m_objects.begin(), m_objects.end(), nullptr);
	if (it != m_objects.end()) {
		*it = obj;
	} else {
		m_objects.push_back(obj);
	}

	return ret_slot;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
// Expose the resolver result without exporting the private shadow-slot address.
PthreadMutex TestResolvePthreadMutexInitialWord(PthreadMutex* mutex, void** initial_word) {
	auto* slot = static_cast<PthreadMutex*>(g_pthread_context->GetPthreadStaticObjects()->CreateObject(
	    mutex, PthreadStaticObject::Type::Mutex, initial_word));
	return slot != nullptr ? *slot : nullptr;
}
#endif

void PthreadStaticObjects::DeleteObjects(Loader::Program* program) {
	Common::LockGuard lock(m_mutex);

	for (auto& obj: m_objects) {
		if (obj != nullptr && obj->program == program) {
			int result = OK;
			switch (obj->type) {
				case PthreadStaticObject::Type::Mutex:
					result = PthreadMutexDestroy(reinterpret_cast<PthreadMutex*>(obj->vaddr));
					break;
				case PthreadStaticObject::Type::Cond:
					result = PthreadCondDestroy(reinterpret_cast<PthreadCond*>(obj->vaddr));
					break;
				case PthreadStaticObject::Type::Rwlock:
					result = PthreadRwlockDestroy(reinterpret_cast<PthreadRwlock*>(obj->vaddr));
					break;
				default: EXIT("unknown type: %d\n", static_cast<int>(obj->type));
			}

			EXIT_NOT_IMPLEMENTED(result != OK);

			delete obj;
			obj = nullptr;
		}
	}
}

Pthread PthreadPool::Create(bool runtime_owned) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_threads) {
		if (p->free && p->runtime_owned == runtime_owned) {
			EXIT_IF(p->execution_thread || p->execution);
			EXIT_IF(p->join_reclaiming || p->join_lifetime_count != 0);
			EXIT_IF(p->native_receipt); // Joined receipt was detached before publishing free.
			p->fs_context.reset(); // Reclaimed record; no live target may read it.
			// Do not expose the previous incarnation's OS ID while the new
			// binding is being initialized. RunThread publishes the new ID after
			// installing the pre-created binding; existing signal admission rejects 0.
			p->host_thread_id = 0;
			p->free = false;
			return p;
		}
	}

	auto* ret = new PthreadPrivate {};
	ret->runtime_owned = runtime_owned;
	ret->pool_managed = true;

	ret->free        = false;
	ret->detached    = false;
	ret->almost_done = false;
	ret->attr        = nullptr;

	m_threads.push_back(ret);

	return ret;
}
std::vector<Pthread> PthreadPool::StopCandidates(bool* acquired) {
	*acquired = false;
	if (!m_mutex.TryLock()) { return {}; }
	struct Unlock { Common::Mutex* mutex; ~Unlock() { mutex->Unlock(); } } unlock {&m_mutex};
	*acquired = true;
	std::vector<Pthread> result;
	for (auto* thread: m_threads) { if (thread->runtime_owned && !thread->free) { result.push_back(thread); } }
	return result; // Records themselves are never deleted; lock stop_mutex before reading their incarnation.
}
bool PthreadIsRuntimeOwned(Pthread thread) {
	return !thread || !IsValidPthread(thread) || thread->runtime_owned;
}
bool PthreadIsRuntimeOwnedOnTarget(Pthread thread) noexcept { return thread && thread->runtime_owned; }

PthreadLegacyNativeTarget::~PthreadLegacyNativeTarget() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); }
#endif
}
bool PthreadLegacyNativeTarget::IsHostLockCritical() const noexcept {
	return critical_ && critical_->load(std::memory_order_seq_cst) != 0;
}
bool PthreadLegacyNativeTarget::QueueSignal(int signum) noexcept {
	if (!record_ || !record_lock_.owns_lock() || signum < 0 || signum >= 64) { return false; }
	const auto mask = 1ull << static_cast<unsigned>(signum);
	return (record_->pending_signal_mask.fetch_or(mask, std::memory_order_acq_rel) & mask) == 0;
}
void PthreadLegacyNativeTarget::UnlockRecord() noexcept {
	record_ = nullptr;
	if (record_lock_.owns_lock()) { record_lock_.unlock(); }
}
void* PthreadLegacyNativeTarget::ReleaseHandle() noexcept { return std::exchange(handle_, nullptr); }
int PthreadPinLegacyNativeTarget(Pthread thread, PthreadLegacyNativeTarget* target) {
	if (!target || target->handle_ || target->record_lock_.owns_lock()) { return KERNEL_ERROR_EINVAL; }
	if (!thread || !IsValidPthread(thread)) { return KERNEL_ERROR_ESRCH; }
	if (thread->runtime_owned) { return KERNEL_ERROR_EOPNOTSUPP; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::unique_lock pin(thread->stop_mutex);
	if (thread->free || !thread->stop_ready.load(std::memory_order_acquire) || !thread->stop_handle) {
		return KERNEL_ERROR_ESRCH;
	}
	HANDLE handle = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), thread->stop_handle, GetCurrentProcess(), &handle,
	    0, FALSE, DUPLICATE_SAME_ACCESS)) { return KERNEL_ERROR_ESRCH; }
	target->handle_ = handle;
	target->thread_id_ = thread->host_thread_id;
	target->critical_ = thread->host_lock_critical;
	if (thread->attr) {
		target->stack_address_ = reinterpret_cast<uint64_t>(thread->attr->stack_addr);
		target->stack_size_ = thread->attr->stack_size;
	}
	target->record_ = thread;
	target->record_lock_ = std::move(pin);
	return OK;
#else
	return KERNEL_ERROR_EOPNOTSUPP;
#endif
}

static int JoinPthread(Pthread thread, void** value, bool detached_only, void* caller, void* caller_frame,
                       const PthreadNativeReceipt* expected = nullptr);

void PthreadPool::FreeDetachedThreads() {
	std::vector<Pthread> candidates;
	try {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		candidates = m_threads; // Records never disappear; validate the current incarnation under join_mutex.
	} catch (const std::bad_alloc&) { return; }
	for (auto* p: candidates) { (void)JoinPthread(p, nullptr, true, nullptr, nullptr); }
}

std::vector<PthreadPool::ThreadSample> PthreadPool::SampleAllThreads() {
	std::vector<ThreadSample> out;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Snapshot values (not pointers) under lock; pool entries can be
	// recycled the moment we release it.
	{
		Common::LockGuard lock(m_mutex);
		for (const auto* p: m_threads) {
			if (p == nullptr || p->free || p->host_thread_id == 0) {
				continue;
			}
			ThreadSample s;
			s.name         = p->name;
			s.os_thread_id = p->host_thread_id;
			out.push_back(std::move(s));
		}
	}
	const uint64_t self = static_cast<uint64_t>(GetCurrentThreadId());
	for (auto& s: out) {
		if (s.os_thread_id == self) {
			continue;
		}
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
		                      FALSE, static_cast<DWORD>(s.os_thread_id));
		if (h == nullptr) {
			continue;
		}
		if (SuspendThread(h) == static_cast<DWORD>(-1)) {
			CloseHandle(h);
			continue;
		}
		CONTEXT ctx {};
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (GetThreadContext(h, &ctx) != 0) {
			s.rip = static_cast<uint64_t>(ctx.Rip);
			s.ok  = true;
		}
		ResumeThread(h);
		CloseHandle(h);
	}
#else
	(void)out;
#endif
	return out;
}

static void DestructPthreadSpecific() {
	// ps5-vsh chainSUI173 TEMP-DIAG: who wipes whose table.
	{
		auto* self = LibKernel::PthreadSelfOrNull();
		uint64_t hostid = 0;
		#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		hostid = static_cast<uint64_t>(GetCurrentThreadId());
		#endif
		char tn[64] = "?";
		if (self != nullptr) {
			if (LibKernel::PthreadGetname(self, tn) != OK) {
				tn[0] = 0;
			}
		}
		static std::atomic<uint64_t> s_destruct {0};
		if (s_destruct.fetch_add(1, std::memory_order_relaxed) < 80) {
			LOGF("ps5-vsh: destruct-specific thread=%s(%p) host=%llu hadtable=%d\n",
			     tn, static_cast<const void*>(self),
			     static_cast<unsigned long long>(hostid),
			     g_pthread_specific != nullptr ? 1 : 0);
		}
	}
	if (g_pthread_specific == nullptr) {
		return;
	}

	std::unique_lock lock(g_pthread_keys_mutex);
	for (int iter = 0; iter < DESTRUCTOR_ITERATIONS; iter++) {
		bool called = false;
		for (int index = 0; index < KEYS_MAX; index++) {
			const auto value = std::exchange(g_pthread_specific[index], {});
			if (value.data == nullptr) {
				continue;
			}

			const auto& entry      = g_pthread_keys[index];
			const auto  generation = entry.generation.load(std::memory_order_relaxed);
			if ((generation & 1u) == 0 || generation != value.generation ||
			    entry.destructor == nullptr) {
				continue;
			}
			const auto destructor = entry.destructor;
			auto execution = entry.execution;
			lock.unlock();
			auto current = Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram());
			if (execution) {
				if (execution == current) { execution->InvokeCallback(reinterpret_cast<uint64_t>(destructor), value.data); }
			} else if (!current) { destructor(value.data); }
			lock.lock();
			called = true;
		}
		if (!called) {
			break;
		}
	}
	g_pthread_specific.reset();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr) {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr);

	*attr = new PthreadMutexattrPrivate {};

	int result = pthread_mutexattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadMutexattrSettype(attr, 1) : result);
	result = (result == 0 ? PthreadMutexattrSetprotocol(attr, 0) : result);

	switch (result) {
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr) {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_mutexattr_destroy(&(*attr)->p);

	if (result != 0) {
		// ps5-vsh: log-on-failure (SysCore eventQueuePush :126 hunt).
		uint64_t ra = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		ra = reinterpret_cast<uint64_t>(_ReturnAddress());
#endif
		LOGF("ps5-vsh: mutexattr destroy failed (attr=%p *attr=%p native=%d ra=0x%016" PRIx64
		     ")\n",
		     static_cast<const void*>(attr), static_cast<const void*>(*attr), result, ra);
	}

	delete *attr;
	*attr = nullptr;

	switch (result) {
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type) {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int ptype = PTHREAD_MUTEX_DEFAULT;
	switch (type) {
		case KERNEL_PTHREAD_MUTEX_ERRORCHECK: ptype = PTHREAD_MUTEX_ERRORCHECK; break;
		case KERNEL_PTHREAD_MUTEX_RECURSIVE: ptype = PTHREAD_MUTEX_RECURSIVE; break;
		case KERNEL_PTHREAD_MUTEX_NORMAL:
		case KERNEL_PTHREAD_MUTEX_ADAPTIVE: ptype = PTHREAD_MUTEX_NORMAL; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_mutexattr_settype(&(*attr)->p, ptype);

	if (result == 0) {
		(*attr)->type = type;
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexattrSetprotocol([[maybe_unused]] PthreadMutexattr* attr,
                                              int                                protocol) {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	[[maybe_unused]] int pprotocol = PTHREAD_PRIO_NONE;
	switch (protocol) {
		case 0: pprotocol = PTHREAD_PRIO_NONE; break;
		case 1: pprotocol = PTHREAD_PRIO_INHERIT; break;
		case 2: pprotocol = PTHREAD_PRIO_PROTECT; break;
		default: EXIT("invalid protocol: %d\n", protocol);
	}

	// protocol doesn't work in winpthreads
	int result         = 0; // pthread_mutexattr_setprotocol(&(*attr)->p, pprotocol);
	(*attr)->pprotocol = pprotocol;

	if (result == 0) {
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

static int PthreadMutexInitNamed(PthreadMutex* mutex, const PthreadMutexattr* attr,
                                 const char* name, int static_type) {
	if (name != nullptr && name[0] != '\0') {
		PRINT_NAME();
	}

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	if (mutex == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	PthreadMutexattrPrivate static_attr {};
	PthreadMutexattr        static_attr_ptr = nullptr;

	if (static_type != 0) {
		static_attr_ptr = &static_attr;
		pthread_mutexattr_init(&static_attr.p);
		PthreadMutexattrSettype(&static_attr_ptr, static_type);
		PthreadMutexattrSetprotocol(&static_attr_ptr, 0);
		attr = &static_attr_ptr;
	} else if (attr == nullptr) {

		attr = g_pthread_context->GetDefaultMutexattr();
	}

	auto* new_mutex = new PthreadMutexPrivate {};
	new_mutex->wait_owner = CurrentWaitOwner();
	PthreadHostObjectAdd(new_mutex);

	// ps5-vsh TEMP-DIAG (chainSUI129): who inits the loader-lock addrs.
	if (Ps5VshTraceMutex(reinterpret_cast<uint64_t>(mutex))) {
		LOGF("ps5-vsh: TRACE init guest=%llx static_type=%d tid=%d ra=0x%016llx "
		     "guest_ra=0x%016llx\n",
		     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(mutex)),
		     static_type, Common::Thread::GetThreadIdUnique(),
		     static_cast<unsigned long long>(Ps5VshRa()),
		     static_cast<unsigned long long>(Ps5VshGuestRa()));
	}

	new_mutex->name      = (name != nullptr ? name : "");
	new_mutex->type      = (*attr)->type;
	new_mutex->pprotocol = (*attr)->pprotocol;

	int result = 0;

	if (static_attr_ptr != nullptr) {
		pthread_mutexattr_destroy(&static_attr.p);
	}

	if (name != nullptr && name[0] != '\0') {
		LOGF("\tmutex init: %s, %d\n", new_mutex->name.c_str(), result);
	}

	std::atomic_ref<PthreadMutexPrivate*>(*mutex).store(new_mutex, std::memory_order_release);
	PthreadShadowPut(reinterpret_cast<uint64_t>(mutex),
	                 PthreadStaticObject::Type::Mutex, new_mutex);

	switch (result) {
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr,
                                   const char* name) {
	// ps5-vsh chainSUI219: guard across init (dual-use words survive init).
	void* ps5vsh_w0 = nullptr;
	if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(mutex), 8)) {
		std::memcpy(&ps5vsh_w0, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(reinterpret_cast<uint64_t>(mutex))), sizeof(ps5vsh_w0));
	}
	const int ps5vsh_r = PthreadMutexInitNamed(mutex, attr, name);
	void* ps5vsh_h = nullptr;
	if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(mutex), 8)) {
		std::memcpy(&ps5vsh_h, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(reinterpret_cast<uint64_t>(mutex))), sizeof(ps5vsh_h));
	}
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    ps5vsh_h);
	return ps5vsh_r;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex) {
	// Hot path for Python/Ren'Py startup; keep this quiet unless it fails.

	if (mutex == nullptr || *mutex == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	// ps5-vsh: never delete an object that isn't live (uninitialized guest var
	// holding garbage -> host heap corruption -> wild ntdll crashes later).
	if (!PthreadHostObjectAlive(*mutex)) {
		LOGF("\tmutex destroy: non-live object %p, refusing\n",
		     static_cast<const void*>(*mutex));
		return KERNEL_ERROR_EINVAL;
	}

	int result = ((*mutex)->owner == nullptr ? 0 : EBUSY);

	if (result != 0) {
		LOGF("\tmutex destroy: %s, %d\n", (*mutex)->name.c_str(), result);
		switch (result) {
			case EBUSY: return KERNEL_ERROR_EBUSY;
			case EINVAL:
			default: return KERNEL_ERROR_EINVAL;
		}
	}
	delete *mutex;
	PthreadHostObjectRemove(*mutex);
	PthreadShadowErase(reinterpret_cast<uint64_t>(mutex));
	*mutex = nullptr;

	switch (result) {
		case 0: return OK;

		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

// ps5-vsh: opt-in mutex-watch (KYTY_MUTEX_WATCH=1). Polls the lock in
// 100 ms timed slices via NativeMutexLock's timeout path (same cv wait
// and signal dispatch as the blocking call) so a wedged acquisition can
// name the mutex and its owner while still waiting. NativeMutexTrylock
// is not used: it conflates contention with a non-recursive self-lock
// (EBUSY for both), which the blocking call reports as EDEADLK.
static int Ps5VshMutexLockWatched(PthreadMutexPrivate* mutex, uint64_t guest_vaddr,
                                  uint64_t lock_start_ms, uint64_t guest_ra) {
	static std::atomic<uint64_t> s_watch_lines {0};
	KernelUseconds               poll_us     = 100000; // 100 ms
	uint64_t                     next_log_ms = lock_start_ms + 5000;
	for (;;) {
		const int result = NativeMutexLock(mutex, &poll_us);
		if (result != ETIMEDOUT) {
			return result;
		}
		// Timed-out slice: either contention (retry) or a non-recursive
		// self-lock, which the blocking call reports as EDEADLK.
		if (mutex != nullptr && mutex->owner == g_pthread_self) {
			return EDEADLK;
		}
		const uint64_t now_ms = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
		if (now_ms >= next_log_ms) {
			next_log_ms = now_ms + 10000;
			if (s_watch_lines.fetch_add(1, std::memory_order_relaxed) < 200) {
				LOGF("ps5-vsh: mutex-watch: host=%d guest=0x%llx mutex=%p name=%s "
				     "owner=%p waiting=%llus ra=0x%llx\n",
				     Common::Thread::GetThreadIdUnique(),
				     static_cast<unsigned long long>(guest_vaddr),
				     static_cast<const void*>(mutex),
				     (mutex != nullptr ? mutex->name.c_str() : "<null>"),
				     static_cast<const void*>(mutex != nullptr ? mutex->owner : nullptr),
				     static_cast<unsigned long long>((now_ms - lock_start_ms) / 1000),
				     static_cast<unsigned long long>(guest_ra));
			}
		}
	}
}

static std::shared_ptr<Loader::RuntimeGuestExecution> RuntimeMutexCallerExecution() {
	if (!Loader::GuestCallBoundary::CurrentDepth()) { return {}; } // Boundary-free HOST coordination.
	if (g_pthread_self && g_pthread_self->execution) { return g_pthread_self->execution; }
	// A runtime callback can also execute on a registered session/host thread.
	// Its active shared scope must not fall through to unrestricted legacy lock
	// semantics merely because that physical pthread has no execution binding.
	return Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram());
}

static int RuntimeMutexLock(PthreadMutex* mutex, KernelUseconds* timeout,
                            Loader::RuntimeGuestExecution* execution) {
	int status = KERNEL_ERROR_EINVAL;
	void* initial = nullptr;
	auto* slot = static_cast<PthreadMutex*>(g_pthread_context->GetPthreadStaticObjects()->CreateObject(
	    mutex, PthreadStaticObject::Type::Mutex, &initial, execution, &status));
	if (!slot) { return status; }
	auto* value = *slot;
	PthreadGuestWordGuard word_guard(reinterpret_cast<uint64_t>(slot), initial, value);
	int result = NativeMutexLock(value, timeout, execution);
	// Include fast/uncontended/recursive acquisition and stop arriving after
	// the native FIFO handoff check. Undo exactly THIS acquisition, never peer
	// ownership or an older recursive depth. No guest mutex re-acquisition.
	if (RuntimeWaitCanceled(execution)) {
		if (result == OK) { NativeMutexUnlock(value); }
		result = ECANCELED;
	}
	switch (result) {
		case 0: return OK;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex) {
	if (auto execution = RuntimeMutexCallerExecution()) { return RuntimeMutexLock(mutex, nullptr, execution.get()); }
	// PRINT_NAME();
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("mutex");

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	// ps5-vsh TEMP (retired): per-lock logging drowned the log (12M+
	// lines); re-enable locally when chasing a mutex bug.

	// ps5-vsh TEMP-DIAG (chainSUI129): trace loader-lock addrs only.
	const uint64_t trace_vaddr = reinterpret_cast<uint64_t>(mutex);
	const bool     do_trace    = Ps5VshTraceMutex(trace_vaddr);
	const uint64_t entry_ra    = do_trace ? Ps5VshRa() : 0;
	g_ps5vsh_guest_ra =
	    reinterpret_cast<uint64_t>(__builtin_return_address(0)); // NOLINT
	g_ps5vsh_mutex_fresh = false;
	// CreateObject captures the original guest word before it can displace it.
	void* ps5vsh_w0 = nullptr;
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, &ps5vsh_w0));

	if (mutex == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	// ps5-vsh: slow-lock detection. A lock held >5s means the owner is
	// dead or wedged (boot deadlock signature); log waiter + mutex once.
	const uint64_t lock_start_ms = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
	// ps5-vsh: opt-in mutex-watch (KYTY_MUTEX_WATCH=1), read once.
	static const bool s_mutex_watch = [] {
		const char* v = std::getenv("KYTY_MUTEX_WATCH");
		return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
	}();
	int result = 0;
	if (s_mutex_watch) {
		result = Ps5VshMutexLockWatched(*mutex, trace_vaddr, lock_start_ms,
		                                Ps5VshGuestRa());
	} else {
		result = NativeMutexLock(*mutex, nullptr);
	}
	const uint64_t lock_took_ms =
	    static_cast<uint64_t>(Loader::Timer::GetTimeMs()) - lock_start_ms;
	if (lock_took_ms > 5000) {
		uint64_t ra = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		ra = reinterpret_cast<uint64_t>(_ReturnAddress());
#endif
		LOGF("ps5-vsh: mutex lock blocked %llums (mutex=%p name=%s ra=0x%016" PRIx64 ")\n",
		     static_cast<unsigned long long>(lock_took_ms), static_cast<const void*>(mutex),
		     (*mutex)->name.c_str(), ra);
	}

	// ps5-vsh: spinner census (temporary). Lock/unlock logging was retired for
	// volume; throttled counting identifies lock-pair spins by name.
	static std::atomic<uint64_t> s_lock_calls {0};
	const uint64_t               n = s_lock_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: mutex lock call #%llu (%p -> %d)\n", n,
		     static_cast<const void*>(*mutex), result);
		LOGF("ps5-vsh: mutex lock call #%llu (%s -> %d)\n", n, (*mutex)->name.c_str(),
		     result);
	}

	// LOGF("\tmutex lock: %s, %d\n", (*mutex)->name.c_str(), result);

	if (do_trace) {
		LOGF("ps5-vsh: TRACE lock guest=%llx host=%p res=%d owner=%p self=%p tid=%d "
		     "ra=0x%016llx\n",
		     static_cast<unsigned long long>(trace_vaddr),
		     static_cast<const void*>(*mutex), result,
		     static_cast<const void*>((*mutex)->owner),
		     static_cast<const void*>(g_pthread_self),
		     Common::Thread::GetThreadIdUnique(),
		     static_cast<unsigned long long>(entry_ra));
	}
	Ps5VshRecordMutexOp(trace_vaddr, 0, result);

	switch (result) {
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex) {
	// PRINT_NAME();

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	// ps5-vsh TEMP-DIAG (chainSUI129): trace loader-lock addrs only.
	const uint64_t try_vaddr = reinterpret_cast<uint64_t>(mutex);
	const bool     try_trace = Ps5VshTraceMutex(try_vaddr);
	g_ps5vsh_guest_ra =
	    reinterpret_cast<uint64_t>(__builtin_return_address(0)); // NOLINT
	g_ps5vsh_mutex_fresh = false;
	// CreateObject captures the original guest word before it can displace it.
	void* ps5vsh_w0 = nullptr;
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, &ps5vsh_w0));

	if (mutex == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	int result = NativeMutexTrylock(*mutex);

	if (try_trace) {
		LOGF("ps5-vsh: TRACE trylock guest=%llx host=%p res=%d owner=%p self=%p tid=%d "
		     "ra=0x%016llx\n",
		     static_cast<unsigned long long>(try_vaddr),
		     static_cast<const void*>(*mutex), result,
		     static_cast<const void*>((*mutex)->owner),
		     static_cast<const void*>(g_pthread_self),
		     Common::Thread::GetThreadIdUnique(),
		     static_cast<unsigned long long>(Ps5VshRa()));
	}
	Ps5VshRecordMutexOp(try_vaddr, 2, result);

	// ps5-vsh: spinner census (temporary). Trylock is silent; a guest EBUSY-spin
	// burns a core invisibly. Throttled log carries the mutex name + result.
	static std::atomic<uint64_t> s_trylock_calls {0};
	const uint64_t               n = s_trylock_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: mutex trylock call #%llu (%s -> %d)\n", n, (*mutex)->name.c_str(),
		     result);
	}

	switch (result) {
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec) {
	if (auto execution = RuntimeMutexCallerExecution()) { return RuntimeMutexLock(mutex, &usec, execution.get()); }
	// PRINT_NAME();
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("mutex");

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	// ps5-vsh: log-on-failure needs the raw guest pointer; CreateObject
	// below overwrites it.
	void* raw_mutex = mutex;
	uint64_t ra     = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ra = reinterpret_cast<uint64_t>(_ReturnAddress());
#endif
	g_ps5vsh_guest_ra =
	    reinterpret_cast<uint64_t>(__builtin_return_address(0)); // NOLINT
	g_ps5vsh_mutex_fresh = false;

	// CreateObject captures the original guest word before it can displace it.
	void* ps5vsh_w0 = nullptr;
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, &ps5vsh_w0));

	if (mutex == nullptr) {
		LOGF("ps5-vsh: mutex timedlock EINVAL (null object for guest mutex=%p usec=%u "
		     "ra=0x%016" PRIx64 ")\n",
		     raw_mutex, usec, ra);
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	int result = NativeMutexLock(*mutex, &usec);

	Ps5VshRecordMutexOp(reinterpret_cast<uint64_t>(raw_mutex), 3, result);

	// ps5-vsh: spinner census (temporary). Timedlock(0) is trylock by another
	// name; looped, it burns invisibly.
	static std::atomic<uint64_t> s_timedlock_calls {0};
	const uint64_t               n = s_timedlock_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: mutex timedlock call #%llu (%s usec=%u)\n", n, (*mutex)->name.c_str(),
		     usec);
	}

	switch (result) {
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EINVAL:
		default:
			// ps5-vsh: log-on-failure. EINVAL here (e.g. SysCore Mbus
			// eventQueuePush) needs the mutex identity + timeout.
			uint64_t ra = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			ra = reinterpret_cast<uint64_t>(_ReturnAddress());
#endif
			LOGF("ps5-vsh: mutex timedlock EINVAL (mutex=%p name=%s usec=%u native=%d "
			     "ra=0x%016" PRIx64 ")\n",
			     static_cast<const void*>(mutex),
			     (*mutex != nullptr ? (*mutex)->name.c_str() : "<null>"), usec, result,
			     ra);
			return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex) {
	// PRINT_NAME();

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	// ps5-vsh TEMP-DIAG (chainSUI129): trace loader-lock addrs only.
	const uint64_t unlock_vaddr = reinterpret_cast<uint64_t>(mutex);
	const bool     unlock_trace = Ps5VshTraceMutex(unlock_vaddr);
	const uint64_t unlock_ra    = unlock_trace ? Ps5VshRa() : 0;
	g_ps5vsh_guest_ra =
	    reinterpret_cast<uint64_t>(__builtin_return_address(0)); // NOLINT
	g_ps5vsh_mutex_fresh = false;

	// ps5-vsh TEMP (retired): per-unlock logging drowned the log; see lock.

	// CreateObject captures the original guest word before it can displace it.
	void* ps5vsh_w0 = nullptr;
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, &ps5vsh_w0));

	if (mutex == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	int result = NativeMutexUnlock(*mutex);

	if (unlock_trace) {
		LOGF("ps5-vsh: TRACE unlock guest=%llx host=%p res=%d owner=%p self=%p tid=%d "
		     "ra=0x%016llx\n",
		     static_cast<unsigned long long>(unlock_vaddr),
		     static_cast<const void*>(*mutex), result,
		     static_cast<const void*>((*mutex)->owner),
		     static_cast<const void*>(g_pthread_self),
		     Common::Thread::GetThreadIdUnique(),
		     static_cast<unsigned long long>(unlock_ra));
	}
	Ps5VshRecordMutexOp(unlock_vaddr, 1, result);

	// ps5-vsh (chainSUI137): the host object was created fresh by this
	// call (guest word was NULL or clobbered by heap reuse): the guest
	// never locked this incarnation, so "unlock" is a no-op. HW never
	// exhibits this state (different heap layout); killing boot here is
	// purely our artifact. Genuine double-unlocks (live object, owner
	// mismatch) still return EPERM below.
	if (result == EPERM && g_ps5vsh_mutex_fresh) {
		g_ps5vsh_mutex_fresh = false;
		LOGF("ps5-vsh: masked fresh-object EPERM unlock (%s); returning OK\n",
		     (*mutex)->name.c_str());
		return OK;
	}

	// ps5-vsh (chainSUI138): consecutive-unlock mask. Class-init (F/W2)
	// demonstrably unlocks the same heap mutex twice with no lock between
	// (op-ring proof); the first EPERM is already masked above, the second
	// dies here. HW takes different branches (never double-unlocks), so
	// treat unlock-after-unlock as no-op. Only when the most recent op on
	// this mutex+thread is itself an unlock: any intervening lock/trylock
	// preserves EPERM.
	if (result == EPERM) {
		const uint64_t total = g_ps5vsh_mutex_op_idx.load(std::memory_order_relaxed);
		const uint64_t back  = (total < 512ull ? total : 512ull);
		const uint32_t self_tid = Common::Thread::GetThreadIdUnique();
		for (uint64_t k = 0; k < back; k++) {
			const uint64_t       seq = total - 1 - k;
			const Ps5VshMutexOp& op  = g_ps5vsh_mutex_ops[seq % kPs5VshMutexOps];
			if (op.vaddr != unlock_vaddr || op.tid != self_tid) {
				continue;
			}
			if (op.op == 1) {
				LOGF("ps5-vsh: masked consecutive-unlock EPERM (%s); returning OK\n",
				     (*mutex)->name.c_str());
				return OK;
			}
			break;
		}
	}

	// ps5-vsh: spinner census (temporary), paired with lock above.
	static std::atomic<uint64_t> s_unlock_calls {0};
	const uint64_t               m = s_unlock_calls.fetch_add(1, std::memory_order_relaxed);
	if ((m % 20000000ull) == 0) {
		LOGF("ps5-vsh: mutex unlock call #%llu (%p -> %d)\n", m,
		     static_cast<const void*>(*mutex), result);
		LOGF("ps5-vsh: mutex unlock call #%llu (%s -> %d)\n", m, (*mutex)->name.c_str(),
		     result);
	}

	if (result != 0) {
		LOGF("\tmutex unlock: %p, %d, thread_id = %d\n", static_cast<const void*>(*mutex),
		     result, Common::Thread::GetThreadIdUnique());
		LOGF("\tmutex unlock: %s, %d, thread_id = %d\n", (*mutex)->name.c_str(), result,
		     Common::Thread::GetThreadIdUnique());
	}

	switch (result) {
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr) {
	// PRINT_NAME();

	*attr = new PthreadAttrPrivate {};

	int result = pthread_attr_init(&(*attr)->p);

	(*attr)->affinity       = 0x7f;
	(*attr)->guard_size     = 0x1000;
	(*attr)->stack_addr     = nullptr;
	(*attr)->stack_size     = PTHREAD_STACK_DEFAULT;
	(*attr)->stack_user     = false;
	(*attr)->stack_map_addr = 0;
	(*attr)->stack_map_size = 0;
	// This backend's default is a host system thread. PROCESS can be retained
	// in attributes, but creation explicitly rejects that unsupported scope.
	(*attr)->scope          = 2;

	KernelSchedParam param;
	param.sched_priority = 700;

	result = (result == 0 ? PthreadAttrSetinheritsched(attr, 4) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(attr, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(attr, 1) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(attr, 0) : result);
	result = (result == 0 ? PthreadAttrSetstacksize(attr, PTHREAD_STACK_DEFAULT) : result);

	if (false && PRINT_NAME_ENABLED) {
		PthreadAttrDbgPrint(attr);
	}

	switch (result) {
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrDestroy");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_destroy(&attr_value->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetscope(PthreadAttr* attr, int scope) {
	auto* value = GetPthreadAttrValue(attr, "PthreadAttrSetscope");
	if (value == nullptr || (scope != 0 && scope != 2)) {
		return KERNEL_ERROR_EINVAL;
	}
	value->scope = scope;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetscope(const PthreadAttr* attr, int* scope) {
	auto* value = GetPthreadAttrValue(attr, "PthreadAttrGetscope");
	if (value == nullptr || scope == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	*scope = value->scope;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr) {
	// PRINT_NAME();

	if (thread == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrCopy(attr, &thread->attr);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask) {
	// PRINT_NAME();

	if (mask == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*mask = (*attr)->affinity;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state) {
	// PRINT_NAME();

	if (state == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	// int result = pthread_attr_getdetachstate(&(*attr)->p, state);
	int result = 0;

	*state = ((*attr)->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);

	switch (*state) {
		case PTHREAD_CREATE_JOINABLE: *state = 0; break;
		case PTHREAD_CREATE_DETACHED: *state = 1; break;
		default: EXIT("unknown state: %d\n", *state);
	}

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size) {
	// PRINT_NAME();

	if (guard_size == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*guard_size = (*attr)->guard_size;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched) {
	// PRINT_NAME();

	if (inherit_sched == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*inherit_sched = (*attr)->inherit_sched;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param) {
	// PRINT_NAME();

	if (param == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_getschedparam(&(*attr)->p, param);

	// Host priority mapping is lossy; return the exact guest value.
	param->sched_priority = (*attr)->guest_priority;

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy) {
	// PRINT_NAME();

	if (policy == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_getschedpolicy(&(*attr)->p, policy);

	switch (*policy) {
		case SCHED_OTHER: *policy = (*attr)->policy; break;
		case SCHED_FIFO: *policy = 1; break;
		case SCHED_RR: *policy = 3; break;
		default: EXIT("unknown policy: %d\n", *policy);
	}

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetsolosched(const PthreadAttr* attr, int* solosched) {
	// PRINT_NAME();

	if (solosched == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*solosched = (*attr)->solosched;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr,
                                      void** __restrict stack_addr, size_t* __restrict stack_size) {
	// PRINT_NAME();

	if (stack_size == nullptr || stack_addr == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*stack_addr = (*attr)->stack_addr;
	*stack_size = (*attr)->stack_size;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr) {
	// PRINT_NAME();

	if (stack_addr == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*stack_addr = (*attr)->stack_addr;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size) {
	// PRINT_NAME();

	if (stack_size == nullptr || attr == nullptr || *attr == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*stack_size = (*attr)->stack_size;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetaffinity");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->affinity = mask;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetdetachstate");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	int pstate = PTHREAD_CREATE_JOINABLE;
	switch (state) {
		case 0: pstate = PTHREAD_CREATE_JOINABLE; break;
		case 1: pstate = PTHREAD_CREATE_DETACHED; break;
		default: EXIT("unknown state: %d\n", state);
	}

	// int result = pthread_attr_setdetachstate(&(*attr)->p, pstate);
	int result = 0;

	attr_value->detached = (pstate == PTHREAD_CREATE_DETACHED);

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetguardsize");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->guard_size = guard_size;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetinheritsched");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	switch (inherit_sched) {
		case 0:
		case 4: break;
		default: EXIT("unknown inherit_sched: %d\n", inherit_sched);
	}

	// Keep this in Kyty state. winpthreads' inheritsched support is not needed for guest-visible
	// behavior, and some guest attr flows can leave the native attr in a shape that crashes inside
	// winpthreads.
	attr_value->inherit_sched = inherit_sched;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetschedparam");
	if (param == nullptr || attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	KernelSchedParam pparam {};
	if (param->sched_priority <= 478) {
		pparam.sched_priority = +2;
	} else if (param->sched_priority >= 733) {
		pparam.sched_priority = -2;
	} else {
		pparam.sched_priority = 0;
	}

	if (pthread_attr_setschedparam(&attr_value->p, &pparam) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
#endif

	attr_value->guest_priority = param->sched_priority;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetschedpolicy");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	// winpthreads supports only SCHED_OTHER policy
	int ppolicy = SCHED_OTHER;

	attr_value->policy = policy;

	int result = pthread_attr_setschedpolicy(&attr_value->p, ppolicy);

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetsolosched(PthreadAttr* attr, int solosched) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetsolosched");
	if (attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->solosched = solosched;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetstack");
	if (addr == nullptr || size < GUEST_PTHREAD_STACK_MIN || attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->stack_addr     = addr;
	attr_value->stack_size     = size;
	attr_value->stack_user     = true;
	attr_value->stack_map_addr = 0;
	attr_value->stack_map_size = 0;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetstackaddr");
	if (addr == nullptr || attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->stack_addr     = addr;
	attr_value->stack_user     = true;
	attr_value->stack_map_addr = 0;
	attr_value->stack_map_size = 0;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size) {
	// PRINT_NAME();

	auto* attr_value = GetPthreadAttrValue(attr, "PthreadAttrSetstacksize");
	if (stack_size < GUEST_PTHREAD_STACK_MIN || attr_value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	attr_value->stack_size = stack_size;

	return OK;
}

struct RwlockCall {
	std::shared_ptr<Loader::RuntimeGuestExecution> execution = RuntimeMutexCallerExecution();
	Loader::RuntimeGuestExecution::Lease lease;
	Loader::GuestCallOwner owner = CurrentWaitOwner();
	int status = OK;
	RwlockCall() {
		if (Loader::GuestCallBoundary::CurrentDepth() && Loader::RuntimeCrt::Context {}.Shared() && !execution) {
			status = KERNEL_ERROR_EOPNOTSUPP; return;
		}
		if (!execution) { return; } // Explicit boundary-free HOST coordination.
		if (!RuntimeWaitOwnerMatches(execution.get(), owner)) { status = KERNEL_ERROR_EOPNOTSUPP; return; }
		lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
		if (!lease || RuntimeWaitCanceled(execution.get())) { status = KERNEL_ERROR_ECANCELED; }
	}
	bool Owns(Loader::GuestCallOwner object_owner) const {
		return Loader::RuntimeCrt::SameOwner(owner, object_owner);
	}
	bool Span(void* slot, uint64_t size, bool writable = false) {
		if (!slot) { status = KERNEL_ERROR_EINVAL; return false; }
		if ((reinterpret_cast<uintptr_t>(slot) % (size == sizeof(void*) ? alignof(void*) : alignof(int))) ||
		    !Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(slot), size)) {
			status = KERNEL_ERROR_EFAULT; return false;
		}
		auto context = Loader::RuntimeCrt::Context {};
		if (!execution) { context.owner = nullptr; } // Trusted HOST spans, not a guest fallback.
		if (writable && !Loader::RuntimeCrt::WritableSpan(context, slot, size)) {
			status = KERNEL_ERROR_EOPNOTSUPP; return false;
		}
		return true;
	}
	template <typename F> bool Publish(F&& commit) {
		if (status != OK) { return false; }
		if (!execution) { commit(); return true; }
		if (RuntimeWaitCanceled(execution.get()) || !lease.Publish(std::forward<F>(commit))) {
			status = KERNEL_ERROR_ECANCELED; return false;
		}
		return true;
	}
};

// Created under the registry lock BEFORE the first registry-preparation allocation.
// Unwind/stop leaves existing tombstones untouched and removes only this
// attempt's unpublished registry/live/shadow state. Erase/restore need no new
// allocation, including when preparation itself failed with bad_alloc.
struct RwlockPreparation {
	uint64_t address;
	PthreadRwlock value;
	bool registered = false, live = false, inserted_slot = false, shadow = false, committed = false;
	bool previous_shadow = false;
	const void* previous_object = nullptr;
	~RwlockPreparation() {
		if (committed) { return; }
		if (shadow) {
			if (previous_shadow) { PthreadShadowPut(address, PthreadStaticObject::Type::Rwlock, previous_object); }
			else { PthreadShadowErase(address); }
		}
		if (inserted_slot) { g_rwlock_slots.erase(address); }
		if (live) { PthreadHostObjectRemove(value); }
		if (registered) { g_rwlocks.erase(value); }
	}
	RwlockSlot* Prepare(const std::shared_ptr<PthreadRwlockPrivate>& pin, Loader::GuestCallOwner owner,
	                    bool retain_slot, bool session_shadow = false) {
		registered = g_rwlocks.emplace(value, pin).second;
		live = true; PthreadHostObjectAdd(value);
		RwlockSlot* entry = nullptr;
		if (retain_slot) {
			auto [it, inserted] = g_rwlock_slots.try_emplace(address, RwlockSlot {owner});
			inserted_slot = inserted; entry = &it->second;
		}
		if (session_shadow) {
			previous_shadow = PthreadShadowGet(address, PthreadStaticObject::Type::Rwlock, &previous_object);
			shadow = true; PthreadShadowPut(address, PthreadStaticObject::Type::Rwlock, value);
		}
		return entry;
	}
};

static bool RwlockRetainsSlot(RwlockCall& call, const void* slot) {
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = linker->FindProgramByAddr(reinterpret_cast<uint64_t>(slot));
	if (program && program->application_owner) {
		auto execution = Loader::GetApplicationExecution(program);
		if (!execution || !call.Owns(execution->Owner())) { call.status = KERNEL_ERROR_EOPNOTSUPP; return false; }
		return true;
	}
	return !call.owner.identity; // Legacy session shadow; never a temporary shared stack.
}

// Shared guest-word loads, publication, lookup and removal use this one registry
// lock, including static initialization. No raw-pointer handoff to a waiter.
static std::shared_ptr<PthreadRwlockPrivate> ResolveRwlock(PthreadRwlock* slot, RwlockCall& call, bool lazy) {
	if (call.status != OK || !call.Span(slot, sizeof(*slot))) { return {}; }
	const bool retain_slot = RwlockRetainsSlot(call, slot);
	if (call.status != OK) { return {}; }
	if (!call.owner.identity && !call.execution && lazy) {
		// Keep session lazy/shadow repair and module-static cleanup bookkeeping.
		// Check retained address authority BEFORE the legacy resolver can mutate.
		{
			std::lock_guard lock(g_rwlock_registry_mutex);
			const auto authority = g_rwlock_slots.find(reinterpret_cast<uint64_t>(slot));
			const auto it = g_rwlocks.find(std::atomic_ref<PthreadRwlock>(*slot).load(std::memory_order_acquire));
			if ((authority != g_rwlock_slots.end() && !call.Owns(authority->second.owner)) ||
			    (it != g_rwlocks.end() && !call.Owns(it->second->wait_owner))) {
				call.status = KERNEL_ERROR_EOPNOTSUPP; return {};
			}
		}
		slot = static_cast<PthreadRwlock*>(g_pthread_context->GetPthreadStaticObjects()->CreateObject(
		    slot, PthreadStaticObject::Type::Rwlock));
		if (!slot) { call.status = KERNEL_ERROR_EINVAL; return {}; }
	}
	std::lock_guard lock(g_rwlock_registry_mutex);
	const auto address = reinterpret_cast<uint64_t>(slot);
	const auto authority = g_rwlock_slots.find(address);
	if (authority != g_rwlock_slots.end() && !call.Owns(authority->second.owner)) {
		call.status = KERNEL_ERROR_EOPNOTSUPP; return {};
	}
	auto handle = std::atomic_ref<PthreadRwlock>(*slot).load(std::memory_order_acquire);
	if (!handle && authority != g_rwlock_slots.end() && authority->second.value) {
		// Never silently replace a retained object whose guest word was clobbered.
		call.status = KERNEL_ERROR_EINVAL; return {};
	}
	if (handle) {
		const auto it = g_rwlocks.find(handle);
		if (it == g_rwlocks.end()) { call.status = KERNEL_ERROR_EINVAL; return {}; }
		if (!call.Owns(it->second->wait_owner)) { call.status = KERNEL_ERROR_EOPNOTSUPP; return {}; }
		return it->second;
	}
	if (!lazy) { call.status = KERNEL_ERROR_EINVAL; return {}; }
	if (!call.Span(slot, sizeof(*slot), true)) { return {}; }
	auto value = std::make_shared<PthreadRwlockPrivate>();
	value->wait_owner = call.owner;
	RwlockPreparation prepared {address, value.get()};
	auto* retained = prepared.Prepare(value, call.owner, retain_slot);
	if (!call.Publish([&]() noexcept {
		if (retained) { retained->value = value.get(); }
		std::atomic_ref<PthreadRwlock>(*slot).store(value.get(), std::memory_order_release);
	})) { return {}; }
	prepared.committed = true;
	return value;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(rwlock, sizeof(*rwlock), true)) { return call.status; }
	std::lock_guard registry_lock(g_rwlock_registry_mutex);
	const auto authority = g_rwlock_slots.find(reinterpret_cast<uint64_t>(rwlock));
	if (authority != g_rwlock_slots.end() && !call.Owns(authority->second.owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	const auto handle = std::atomic_ref<PthreadRwlock>(*rwlock).load(std::memory_order_acquire);
	if (!handle) { return authority != g_rwlock_slots.end() && authority->second.value ? KERNEL_ERROR_EINVAL : OK; }
	const auto it = g_rwlocks.find(handle);
	if (it == g_rwlocks.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (it->second.use_count() != 1) { return KERNEL_ERROR_EBUSY; }
	auto value = it->second;
	std::lock_guard object_lock(value->m);
	if (value->writer || value->reader_count || value->waiting_writers) { return KERNEL_ERROR_EBUSY; }
	if (!call.Publish([&]() noexcept {
		std::atomic_ref<PthreadRwlock>(*rwlock).store(nullptr, std::memory_order_release);
	})) { return call.status; }
	for (auto& [address, entry]: g_rwlock_slots) { if (entry.value == handle) { entry.value = nullptr; } }
	g_rwlocks.erase(it);
	PthreadHostObjectRemove(handle);
	PthreadShadowErase(reinterpret_cast<uint64_t>(rwlock));
	return OK;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

static int PthreadRwlockInitNamed(PthreadRwlock* rwlock, const PthreadRwlockattr* attr,
                                  const char* name) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(rwlock, sizeof(*rwlock), true)) { return call.status; }
	if (attr && !call.Span(const_cast<PthreadRwlockattr*>(attr), sizeof(*attr))) { return call.status; }
	const bool retain_slot = RwlockRetainsSlot(call, rwlock);
	if (call.status != OK) { return call.status; }
	auto value = std::make_shared<PthreadRwlockPrivate>();
	value->wait_owner = call.owner;
	if (name) {
		for (unsigned i = 0; i != 256; ++i) {
			if (!Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(name + i), 1)) { return KERNEL_ERROR_EFAULT; }
			if (!name[i]) { break; }
			if (i == 255) { return KERNEL_ERROR_EINVAL; }
			value->name.push_back(name[i]);
		}
	}
	std::lock_guard lock(g_rwlock_registry_mutex);
	if (attr) {
		const auto it = g_rwlock_attributes.find(*attr);
		if (it == g_rwlock_attributes.end()) { return KERNEL_ERROR_EINVAL; }
		if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
		value->type = it->second->type;
	}
	const auto address = reinterpret_cast<uint64_t>(rwlock);
	const auto authority = g_rwlock_slots.find(address);
	if (authority != g_rwlock_slots.end()) {
		if (!call.Owns(authority->second.owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
		if (authority->second.value) { return KERNEL_ERROR_EBUSY; }
	}
	const auto handle = std::atomic_ref<PthreadRwlock>(*rwlock).load(std::memory_order_acquire);
	if (const auto it = g_rwlocks.find(handle); it != g_rwlocks.end()) {
		return call.Owns(it->second->wait_owner) ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EOPNOTSUPP;
	}
	RwlockPreparation prepared {address, value.get()};
	auto* retained = prepared.Prepare(value, call.owner, retain_slot, !call.owner.identity);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto hook = g_runtime_rwlock_init_publish_for_test.load(std::memory_order_acquire)) { hook(value.get(), call.execution.get()); }
#endif
	if (!call.Publish([&]() noexcept {
		if (retained) { retained->value = value.get(); }
		std::atomic_ref<PthreadRwlock>(*rwlock).store(value.get(), std::memory_order_release);
	})) { return call.status; }
	prepared.committed = true;
	return OK;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr,
                                    const char* name) {
	return PthreadRwlockInitNamed(rwlock, attr, name);
}

static PthreadRwlockPrivate::Reader* RwlockFindReader(PthreadRwlock rwlock, Pthread thread,
                                                    const Loader::RuntimeCrt::ThreadIdentity& incarnation) noexcept {
	EXIT_IF(rwlock == nullptr);

	for (auto& reader: rwlock->readers) {
		if (reader.thread == thread && reader.incarnation == incarnation) {
			return &reader;
		}
	}

	return nullptr;
}

static void RwlockAddReader(PthreadRwlock rwlock, Pthread thread, const Loader::RuntimeCrt::ThreadIdentity& incarnation) {
	if (auto* reader = RwlockFindReader(rwlock, thread, incarnation); reader != nullptr) {
		reader->count++;
	} else {
		rwlock->readers.push_back({thread, 1, incarnation});
	}
	rwlock->reader_count++;
}

static bool RwlockRemoveReader(PthreadRwlock rwlock, Pthread thread, const Loader::RuntimeCrt::ThreadIdentity& incarnation) noexcept {
	for (auto it = rwlock->readers.begin(); it != rwlock->readers.end(); ++it) {
		if (it->thread == thread && it->incarnation == incarnation) {
			it->count--;
			rwlock->reader_count--;
			if (it->count == 0) {
				rwlock->readers.erase(it);
			}
			return true;
		}
	}

	return false;
}

static int RwlockAcquire(PthreadRwlock* slot, bool write, KernelUseconds* timeout_us, bool attempt = false) try {
	// Restore native cancellation LAST, after every call/object pin and lock has
	// unwound. Runtime stop instead polls and returns through ordinary host RAII.
	struct CancelState {
		int previous = 0;
		CancelState() { pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous); }
		~CancelState() { pthread_setcancelstate(previous, nullptr); }
	} cancel_state;
	RwlockCall call;
	auto pin = ResolveRwlock(slot, call, true);
	if (!pin) { return call.status; }
	auto* rwlock = pin.get();
	auto* self = g_pthread_self;
	if (!self) { return KERNEL_ERROR_EINVAL; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto hook = g_runtime_rwlock_pinned_for_test.load(std::memory_order_acquire)) { hook(rwlock, call.execution.get()); }
#endif
	const auto incarnation = Loader::RuntimeCrt::ActualThread(); // May allocate: NEVER inside Publish.
	const auto deadline = timeout_us ? std::chrono::steady_clock::now() + std::chrono::microseconds(*timeout_us)
	                                : std::chrono::steady_clock::time_point::max();
	std::unique_lock lock(rwlock->m);
	struct WriterWait {
		PthreadRwlock rwlock; bool registered = false;
		~WriterWait() {
			if (registered) { --rwlock->waiting_writers; rwlock->cv.notify_all(); }
		}
	} waiter {rwlock}; // Unregister under object lock on EVERY return path.
	for (;;) {
		if (RuntimeWaitCanceled(call.execution.get())) { return KERNEL_ERROR_ECANCELED; }
		const bool already_reader = RwlockFindReader(rwlock, self, incarnation) != nullptr;
		const bool own_writer = rwlock->writer == self && rwlock->writer_incarnation == incarnation;
		if (write && (own_writer || already_reader)) { return KERNEL_ERROR_EDEADLK; }
		const bool available = write ? !rwlock->writer && !rwlock->reader_count :
		    (!rwlock->writer && (rwlock->type == 2 || !rwlock->waiting_writers || already_reader)) || own_writer;
		if (available) {
			if (!write && rwlock->reader_count == UINT32_MAX) { return KERNEL_ERROR_EAGAIN; }
			if (!write && !already_reader) { rwlock->readers.reserve(rwlock->readers.size() + 1); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			if (auto hook = g_runtime_rwlock_publish_for_test.load(std::memory_order_acquire)) { hook(rwlock, call.execution.get()); }
#endif
			if (!call.Publish([&]() noexcept {
				if (write) { rwlock->writer = self; rwlock->writer_incarnation = incarnation; rwlock->writer_count = 1; }
				else { RwlockAddReader(rwlock, self, incarnation); } // Capacity reserved before Publish.
			})) { return call.status; }
			return OK;
		}
		if (attempt) { return KERNEL_ERROR_EBUSY; }
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) { return KERNEL_ERROR_ETIMEDOUT; }
		if (write && !waiter.registered) { ++rwlock->waiting_writers; waiter.registered = true; }
		rwlock->cv.wait_until(lock, std::min(deadline, now + std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)));
		if (!call.execution) {
			lock.unlock(); KernelDispatchPendingSignalForCurrentThread(); lock.lock();
		}
	}
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock) {
	return RwlockAcquire(rwlock, false, nullptr);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec) {
	return RwlockAcquire(rwlock, false, &usec);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec) {
	return RwlockAcquire(rwlock, true, &usec);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock) {
	return RwlockAcquire(rwlock, false, nullptr, true);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock) {
	return RwlockAcquire(rwlock, true, nullptr, true);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock) try {
	RwlockCall call;
	auto pin = ResolveRwlock(rwlock, call, false); // Unlock never creates/repairs.
	if (!pin) { return call.status; }
	auto* self = g_pthread_self;
	if (!self) { return KERNEL_ERROR_EINVAL; }
	const auto incarnation = Loader::RuntimeCrt::ActualThread();
	std::lock_guard lock(pin->m);
	if ((pin->writer != self || pin->writer_incarnation != incarnation) &&
	    !RwlockFindReader(pin.get(), self, incarnation)) { return KERNEL_ERROR_EPERM; }
	if (!call.Publish([&]() noexcept {
		if (!RwlockRemoveReader(pin.get(), self, incarnation)) { pin->writer = nullptr; pin->writer_incarnation.reset(); pin->writer_count = 0; }
		pin->cv.notify_all();
	})) { return call.status; }
	return OK;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock) {
	return RwlockAcquire(rwlock, true, nullptr);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(attr, sizeof(*attr), true)) { return call.status; }
	std::lock_guard lock(g_rwlock_registry_mutex);
	const auto it = g_rwlock_attributes.find(*attr);
	if (it == g_rwlock_attributes.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	int result = 0;
	if (!call.Publish([&]() noexcept {
		result = NativeRwlockattrDestroy(it->second.get());
		if (!result) { *attr = nullptr; }
	})) { return call.status; }
	if (result) { return KERNEL_ERROR_EINVAL; } // Failed release retains storage.
	g_rwlock_attributes.erase(it);
	return OK;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(attr, sizeof(*attr), true)) { return call.status; }
	std::lock_guard lock(g_rwlock_registry_mutex);
	if (const auto it = g_rwlock_attributes.find(*attr); it != g_rwlock_attributes.end()) {
		return call.Owns(it->second->wait_owner) ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EOPNOTSUPP;
	}
	auto prepared = std::make_unique<PthreadRwlockattrPrivate>();
	auto* value = prepared.get();
	// Reserve the retaining node BEFORE acquiring native state. Rollback never
	// needs an allocation to quarantine a failed native attribute release.
	g_rwlock_attributes.emplace(value, std::move(prepared));
	struct Preparation {
		PthreadRwlockattr value;
		bool initialized = false, committed = false;
		~Preparation() {
			if (!committed && (!initialized || !NativeRwlockattrDestroy(value))) { g_rwlock_attributes.erase(value); }
		}
	} rollback {value};
	const int result = pthread_rwlockattr_init(&value->p);
	if (result) { return result == ENOMEM ? KERNEL_ERROR_ENOMEM : KERNEL_ERROR_EINVAL; }
	rollback.initialized = true;
	value->type = 1; value->wait_owner = call.owner;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto hook = g_runtime_rwlock_attr_init_publish_for_test.load(std::memory_order_acquire)) { hook(value, call.execution.get()); }
#endif
	if (!call.Publish([&]() noexcept { *attr = value; })) { return call.status; }
	rollback.committed = true;
	return OK;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(attr, sizeof(*attr)) || !call.Span(type, sizeof(*type), true)) { return call.status; }
	std::lock_guard lock(g_rwlock_registry_mutex);
	const auto it = g_rwlock_attributes.find(*attr);
	if (it == g_rwlock_attributes.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	call.Publish([&]() noexcept { *type = it->second->type; });
	return call.status;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type) try {
	RwlockCall call;
	if (call.status != OK || !call.Span(attr, sizeof(*attr))) { return call.status; }
	if (type != 1 && type != 2) { return KERNEL_ERROR_EINVAL; }
	std::lock_guard lock(g_rwlock_registry_mutex);
	const auto it = g_rwlock_attributes.find(*attr);
	if (it == g_rwlock_attributes.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	call.Publish([&]() noexcept { it->second->type = type; });
	return call.status;
} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
  catch (const std::system_error&) { return KERNEL_ERROR_EINVAL; }

// Shared guest operations need their exact current boundary. Boundary-free
// host setup/teardown remains available to the loader and native-worker tests.
struct ConditionCall {
	std::shared_ptr<Loader::RuntimeGuestExecution> execution = RuntimeMutexCallerExecution();
	Loader::RuntimeGuestExecution::Lease lease;
	int status = OK;
	ConditionCall() {
		if (Loader::GuestCallBoundary::CurrentDepth() &&
		    Loader::RuntimeCrt::Context {}.Shared() && !execution) { status = KERNEL_ERROR_EOPNOTSUPP; return; }
		if (!execution) { return; }
		if (!RuntimeWaitOwnerMatches(execution.get(), CurrentWaitOwner())) { status = KERNEL_ERROR_EOPNOTSUPP; return; }
		lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
		if (!lease || RuntimeWaitCanceled(execution.get())) { status = KERNEL_ERROR_ECANCELED; }
	}
	bool Owns(Loader::GuestCallOwner owner) const { return RuntimeWaitOwnerMatches(execution.get(), owner); }
	bool Slot(void* slot) {
		if (!slot || !Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(slot), sizeof(void*))) {
			status = KERNEL_ERROR_EFAULT; return false;
		}
		if (execution && !Loader::RuntimeCrt::WritableSpan(Loader::RuntimeCrt::Context {}, slot, sizeof(void*))) {
			status = KERNEL_ERROR_EOPNOTSUPP; return false;
		}
		return true;
	}
	template <typename F> bool Publish(F&& commit) {
		if (status != OK) { return false; }
		if (!execution) { commit(); return true; }
		if (!lease.Publish(std::forward<F>(commit))) { status = KERNEL_ERROR_ECANCELED; return false; }
		return true;
	}
};

static std::shared_ptr<PthreadCondPrivate> ResolveCondition(PthreadCond* cond, ConditionCall& call) {
	if (call.status != OK) { return {}; }
	int status = KERNEL_ERROR_EINVAL;
	auto* slot = static_cast<PthreadCond*>(g_pthread_context->GetPthreadStaticObjects()->CreateObject(
	    cond, PthreadStaticObject::Type::Cond, nullptr, call.execution.get(), &status));
	if (!slot) { call.status = status; return {}; }
	auto value = PinCondition(std::atomic_ref<PthreadCond>(*slot).load(std::memory_order_acquire));
	if (!value || !call.Owns(value->wait_owner)) { call.status = KERNEL_ERROR_EOPNOTSUPP; return {}; }
	return value;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr) {
	PRINT_NAME();
	ConditionCall call;
	if (call.status != OK || !call.Slot(attr)) { return call.status; }
	std::lock_guard lock(g_condition_registry_mutex);
	const auto it = g_condition_attributes.find(*attr);
	if (it == g_condition_attributes.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	int result = 0;
	if (!call.Publish([&] {
		result = pthread_condattr_destroy(&it->second->p);
		if (!result) { *attr = nullptr; }
	})) { return call.status; }
	if (result) { return KERNEL_ERROR_EINVAL; }
	g_condition_attributes.erase(it);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr) {
	PRINT_NAME();
	ConditionCall call;
	if (call.status != OK || !call.Slot(attr)) { return call.status; }
	auto prepared = std::make_unique<PthreadCondattrPrivate>();
	const int result = pthread_condattr_init(&prepared->p);
	if (result) { return result == ENOMEM ? KERNEL_ERROR_ENOMEM : KERNEL_ERROR_EINVAL; }
	prepared->wait_owner = CurrentWaitOwner();
	std::lock_guard lock(g_condition_registry_mutex);
	if (g_condition_attributes.contains(*attr)) { pthread_condattr_destroy(&prepared->p); return KERNEL_ERROR_EBUSY; }
	auto* value = prepared.get();
	g_condition_attributes.emplace(value, std::move(prepared));
	if (!call.Publish([&] { *attr = value; })) {
		pthread_condattr_destroy(&value->p); g_condition_attributes.erase(value); return call.status;
	}
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clock_id) {
	PRINT_NAME();

	ConditionCall call;
	if (call.status != OK || !call.Slot(attr)) { return call.status; }
	std::lock_guard lock(g_condition_registry_mutex);
	const auto it = g_condition_attributes.find(*attr);
	if (it == g_condition_attributes.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	clockid_t pclock_id = {};
	if (!GetPosixClockId(clock_id, &pclock_id)) {
		LOGF("\tunknown clock_id: %d\n", clock_id);
		return KERNEL_ERROR_EINVAL;
	}

	int result = 0;
	if (!call.Publish([&] {
#if !defined(__APPLE__)
		result = pthread_condattr_setclock(&it->second->p, pclock_id);
#endif
		if (result == 0 || (result == EINVAL && pclock_id == CLOCK_MONOTONIC)) { it->second->clock_id = clock_id; }
	})) { return call.status; }

	LOGF("\tcondattr setclock: clock_id = %d, native = %d, result = %d\n", clock_id,
	     static_cast<int>(pclock_id), result);

	if (result == EINVAL && pclock_id == CLOCK_MONOTONIC) {
		LOGF("\tcondattr setclock: host rejected CLOCK_MONOTONIC, keeping emulated attribute\n");
		return OK;
	}

	switch (result) {
		case 0: return OK;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond) {
	ConditionCall call;
	auto value = ResolveCondition(cond, call);
	if (!value) { return call.status; }
	std::lock_guard lock(value->m);
	call.Publish([&] {
		for (auto* thread: value->waiters) {
			thread->cond_sequence++; thread->cond_cv.notify_one();
		}
		value->waiters.clear();
	});
	return call.status;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond) {
	PRINT_NAME();
	ConditionCall call;
	if (call.status != OK || !call.Slot(cond)) { return call.status; }
	std::unique_lock registry_lock(g_condition_registry_mutex);
	const auto it = g_conditions.find(std::atomic_ref<PthreadCond>(*cond).load(std::memory_order_acquire));
	if (it == g_conditions.end()) { return KERNEL_ERROR_EINVAL; }
	if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
	// A notified waiter still has a pin until its original wait call finishes.
	if (it->second.use_count() != 1) { return KERNEL_ERROR_EBUSY; }
	auto value = it->second;
	std::unique_lock object_lock(value->m);
	if (!value->waiters.empty()) { return KERNEL_ERROR_EBUSY; }
	if (!call.Publish([&] { std::atomic_ref<PthreadCond>(*cond).store(nullptr, std::memory_order_release); })) { return call.status; }
	g_conditions.erase(it);
	object_lock.unlock(); registry_lock.unlock();
	PthreadHostObjectRemove(value.get());
	PthreadShadowErase(reinterpret_cast<uint64_t>(cond));
	return OK;
}

static int PthreadCondInitNamed(PthreadCond* cond, const PthreadCondattr* attr, const char* name) {
	if (name != nullptr && name[0] != '\0') {
		PRINT_NAME();
	}

	ConditionCall call;
	if (call.status != OK || !call.Slot(cond)) { return call.status; }
	if (attr && !call.Slot(const_cast<PthreadCondattr*>(attr))) { return call.status; }
	auto value = std::make_shared<PthreadCondPrivate>();
	value->name = name ? name : "";
	value->wait_owner = CurrentWaitOwner();
	std::unique_lock lock(g_condition_registry_mutex);
	if (attr) {
		const auto it = g_condition_attributes.find(*attr);
		if (it == g_condition_attributes.end()) { return KERNEL_ERROR_EINVAL; }
		if (!call.Owns(it->second->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }
		value->clock_id = it->second->clock_id;
	}
	if (g_conditions.contains(std::atomic_ref<PthreadCond>(*cond).load(std::memory_order_acquire))) { return KERNEL_ERROR_EBUSY; }
	g_conditions.emplace(value.get(), value);
	PthreadHostObjectAdd(value.get());
	if (!call.Publish([&] { std::atomic_ref<PthreadCond>(*cond).store(value.get(), std::memory_order_release); })) {
		PthreadHostObjectRemove(value.get()); g_conditions.erase(value.get()); return call.status;
	}
	lock.unlock();
	PthreadShadowPut(reinterpret_cast<uint64_t>(cond), PthreadStaticObject::Type::Cond, value.get());
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr,
                                  const char* name) {
	return PthreadCondInitNamed(cond, attr, name);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond) {
	ConditionCall call;
	auto value = ResolveCondition(cond, call);
	if (!value) { return call.status; }
	std::lock_guard lock(value->m);
	call.Publish([&] { CondWakeWaiter(value.get(), nullptr); });
	return call.status;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread) {
	if (!thread) { return KERNEL_ERROR_EINVAL; }
	ConditionCall call;
	auto value = ResolveCondition(cond, call);
	if (!value) { return call.status; }
	std::lock_guard lock(value->m);
	const auto it = std::find(value->waiters.begin(), value->waiters.end(), thread);
	if (it == value->waiters.end()) { return OK; }
	if (call.execution && (*it)->execution != call.execution) { return KERNEL_ERROR_EOPNOTSUPP; }
	call.Publish([&] { CondWakeWaiter(value.get(), thread); });
	return call.status;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex,
                                       KernelUseconds usec) {
	// PRINT_NAME();
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("cond");

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	ConditionCall call;
	auto cond_pin = ResolveCondition(cond, call);
	if (!cond_pin) { return call.status; }
	auto* execution = call.execution.get();
	// ps5-vsh chainSUI211/219: snapshot the guest mutex word before
	// CreateObject may displace it; the guard restores it at scope exit.
	void* ps5vsh_w0 = nullptr;
	if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(mutex), 8)) {
		std::memcpy(&ps5vsh_w0, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(reinterpret_cast<uint64_t>(mutex))), sizeof(ps5vsh_w0));
	}
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, nullptr, execution, &call.status));

	if (mutex == nullptr) {
		return call.status;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	auto* cond_value  = cond_pin.get();
	auto* mutex_value = *mutex;
	if (!RuntimeWaitOwnerMatches(execution, cond_value->wait_owner) ||
	    !RuntimeWaitOwnerMatches(execution, mutex_value->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }

	if (mutex_value->owner != g_pthread_self) {
		return KERNEL_ERROR_EPERM;
	}

	std::unique_lock cond_lock(cond_value->m);
	auto*            thread          = g_pthread_self;
	const auto       thread_sequence = thread->cond_sequence;
	CondAddWaiter(cond_value, thread);

	// ps5-vsh: waiter census (diagnostic). First + every 64th entry logs the
	// waiter thread + cond/mutex names + timeout so parked threads (silent
	// host waits) become attributable. Entry-only; no behavior change.
	static std::atomic<uint64_t> s_cond_timedwait_probe {0};
	const uint64_t               cond_probe_n =
	    s_cond_timedwait_probe.fetch_add(1, std::memory_order_relaxed);
	if (cond_probe_n == 0 || (cond_probe_n % 64ull) == 0) {
		LOGF("ps5-vsh: cond-timedwait #%llu thread=%s cond=%s mutex=%s usec=%u\n",
		     cond_probe_n, (thread != nullptr ? thread->name.c_str() : "?"),
		     cond_value->name.c_str(), mutex_value->name.c_str(), usec);
	}

	uint32_t recurse = 0;
	int      result  = NativeMutexUnlock(mutex_value, &recurse);
	if (result != OK) {
		CondRemoveWaiter(cond_value, thread);
		return (result == EPERM ? KERNEL_ERROR_EPERM : KERNEL_ERROR_EINVAL);
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(usec);
	auto ready = [thread, thread_sequence] { return thread->cond_sequence != thread_sequence; };

	if (usec == 0) {
		result = ETIMEDOUT;
	} else {
		while (!ready() && !RuntimeWaitCanceled(execution)) {
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline) {
				break;
			}

			const auto remaining = deadline - now;
			const auto poll      = (remaining < std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)
			                            ? remaining
			                            : std::chrono::steady_clock::duration(
			                                  std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)));
			thread->cond_cv.wait_for(cond_lock, poll);

			if (!ready() && !RuntimeWaitCanceled(execution)) {
				cond_lock.unlock();
				KernelDispatchPendingSignalForCurrentThread();
				cond_lock.lock();
			}
		}

		result = (ready() ? OK : ETIMEDOUT);
	}
	if (RuntimeWaitCanceled(execution)) { result = ECANCELED; }
	CondRemoveWaiter(cond_value, thread);
	cond_lock.unlock();

	int lock_result = NativeMutexLockRecurse(mutex_value, recurse, execution);
	if (result == OK || lock_result != OK) {
		result = lock_result;
	}

	// LOGF("\tcond timedwait: %s, %d\n", (*cond)->name.c_str(), result);

	switch (result) {
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondTimedwaitAbs(PthreadCond* cond, PthreadMutex* mutex,
                                          const KernelTimespec* abstime) {
	// PRINT_NAME();

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	ConditionCall call;
	auto cond_pin = ResolveCondition(cond, call);
	if (!cond_pin) { return call.status; }
	auto* execution = call.execution.get();
	// ps5-vsh chainSUI211/219: snapshot the guest mutex word before
	// CreateObject may displace it; the guard restores it at scope exit.
	void* ps5vsh_w0 = nullptr;
	if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(mutex), 8)) {
		std::memcpy(&ps5vsh_w0, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(reinterpret_cast<uint64_t>(mutex))), sizeof(ps5vsh_w0));
	}
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, nullptr, execution, &call.status));

	if (mutex == nullptr) {
		return call.status;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	auto* cond_value  = cond_pin.get();
	auto* mutex_value = *mutex;
	if (!RuntimeWaitOwnerMatches(execution, cond_value->wait_owner) ||
	    !RuntimeWaitOwnerMatches(execution, mutex_value->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }

	std::chrono::steady_clock::time_point deadline {};
	if (!NativeCondDeadlineFromAbs(cond_value->clock_id, abstime, &deadline)) {
		return KERNEL_ERROR_EINVAL;
	}

	if (mutex_value->owner != g_pthread_self) {
		return KERNEL_ERROR_EPERM;
	}

	std::unique_lock cond_lock(cond_value->m);
	auto*            thread          = g_pthread_self;
	const auto       thread_sequence = thread->cond_sequence;
	CondAddWaiter(cond_value, thread);

	// ps5-vsh: waiter census (diagnostic). First + every 64th entry logs the
	// waiter thread + cond/mutex names + absolute timeout so parked threads
	// (silent host waits) become attributable. Entry-only; no behavior change.
	static std::atomic<uint64_t> s_cond_timedwait_abs_probe {0};
	const uint64_t               cond_abs_probe_n =
	    s_cond_timedwait_abs_probe.fetch_add(1, std::memory_order_relaxed);
	if (cond_abs_probe_n == 0 || (cond_abs_probe_n % 64ull) == 0) {
		LOGF("ps5-vsh: cond-timedwait-abs #%llu thread=%s cond=%s mutex=%s abs=%lld.%09lld\n",
		     cond_abs_probe_n, (thread != nullptr ? thread->name.c_str() : "?"),
		     cond_value->name.c_str(), mutex_value->name.c_str(),
		     static_cast<long long>(abstime->tv_sec),
		     static_cast<long long>(abstime->tv_nsec));
	}

	uint32_t recurse = 0;
	int      result  = NativeMutexUnlock(mutex_value, &recurse);
	if (result != OK) {
		CondRemoveWaiter(cond_value, thread);
		return (result == EPERM ? KERNEL_ERROR_EPERM : KERNEL_ERROR_EINVAL);
	}

	auto ready = [thread, thread_sequence] { return thread->cond_sequence != thread_sequence; };

	while (!ready() && !RuntimeWaitCanceled(execution)) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			break;
		}

		const auto remaining = deadline - now;
		const auto poll      = (remaining < std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)
		                            ? remaining
		                            : std::chrono::steady_clock::duration(
		                                  std::chrono::microseconds(SIGNAL_APC_POLL_MICROS)));
		thread->cond_cv.wait_for(cond_lock, poll);

		if (!ready() && !RuntimeWaitCanceled(execution)) {
			cond_lock.unlock();
			KernelDispatchPendingSignalForCurrentThread();
			cond_lock.lock();
		}
	}

	result = RuntimeWaitCanceled(execution) ? ECANCELED : (ready() ? OK : ETIMEDOUT);
	CondRemoveWaiter(cond_value, thread);
	cond_lock.unlock();

	int lock_result = NativeMutexLockRecurse(mutex_value, recurse, execution);
	if (result == OK || lock_result != OK) {
		result = lock_result;
	}

	switch (result) {
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex) {
	PRINT_NAME();

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	ConditionCall call;
	auto cond_pin = ResolveCondition(cond, call);
	if (!cond_pin) { return call.status; }
	auto* execution = call.execution.get();
	// ps5-vsh chainSUI211/219: snapshot the guest mutex word before
	// CreateObject may displace it; the guard restores it at scope exit.
	void* ps5vsh_w0 = nullptr;
	if (Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(mutex), 8)) {
		std::memcpy(&ps5vsh_w0, reinterpret_cast<const void*>(
		    static_cast<uintptr_t>(reinterpret_cast<uint64_t>(mutex))), sizeof(ps5vsh_w0));
	}
	mutex = static_cast<PthreadMutex*>(
	    pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex, nullptr, execution, &call.status));

	if (mutex == nullptr) {
		return call.status;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);
	PthreadGuestWordGuard ps5vsh_guard(reinterpret_cast<uint64_t>(mutex), ps5vsh_w0,
	                                    *mutex);

	auto* cond_value  = cond_pin.get();
	auto* mutex_value = *mutex;
	if (!RuntimeWaitOwnerMatches(execution, cond_value->wait_owner) ||
	    !RuntimeWaitOwnerMatches(execution, mutex_value->wait_owner)) { return KERNEL_ERROR_EOPNOTSUPP; }

	if (mutex_value->owner != g_pthread_self) {
		return KERNEL_ERROR_EPERM;
	}

	std::unique_lock cond_lock(cond_value->m);
	auto*            thread          = g_pthread_self;
	const auto       thread_sequence = thread->cond_sequence;
	CondAddWaiter(cond_value, thread);

	// ps5-vsh: waiter census (diagnostic). First + every 64th entry logs the
	// waiter thread + cond/mutex names so parked threads (silent host waits)
	// become attributable. Entry-only; no behavior change.
	static std::atomic<uint64_t> s_cond_wait_probe {0};
	const uint64_t               cond_wait_probe_n =
	    s_cond_wait_probe.fetch_add(1, std::memory_order_relaxed);
	if (cond_wait_probe_n == 0 || (cond_wait_probe_n % 64ull) == 0) {
		LOGF("ps5-vsh: cond-wait #%llu thread=%s cond=%s mutex=%s\n", cond_wait_probe_n,
		     (thread != nullptr ? thread->name.c_str() : "?"), cond_value->name.c_str(),
		     mutex_value->name.c_str());
	}

	HLE_SLOW_GUARD(cond_value->name.c_str());

	uint32_t recurse = 0;
	int      result  = NativeMutexUnlock(mutex_value, &recurse);
	if (result != OK) {
		CondRemoveWaiter(cond_value, thread);
		return (result == EPERM ? KERNEL_ERROR_EPERM : KERNEL_ERROR_EINVAL);
	}

	auto ready = [thread, thread_sequence] { return thread->cond_sequence != thread_sequence; };

	while (!ready() && !RuntimeWaitCanceled(execution)) {
		thread->cond_cv.wait_for(cond_lock, std::chrono::microseconds(SIGNAL_APC_POLL_MICROS));
		if (!ready() && !RuntimeWaitCanceled(execution)) {
			cond_lock.unlock();
			KernelDispatchPendingSignalForCurrentThread();
			cond_lock.lock();
		}
	}
	CondRemoveWaiter(cond_value, thread);
	cond_lock.unlock();

	result = NativeMutexLockRecurse(mutex_value, recurse, execution);

	switch (result) {
		case 0: return OK;
		case EPERM: return KERNEL_ERROR_EPERM;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

Pthread __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSelf() {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_pthread_self == nullptr);

	return g_pthread_self;
}

Pthread PthreadSelfOrNull() {
	return g_pthread_self;
}

int PthreadRetainCurrentUntilJoin(std::shared_ptr<void> lifetime) noexcept {
	if (!lifetime) { return 22; }
	auto* thread = g_pthread_self;
	if (!thread || !thread->pool_managed || thread->free || thread->join_reclaiming ||
	    thread->startup.load(std::memory_order_acquire) != 1 ||
	    thread->almost_done || !::pthread_equal(thread->p, ::pthread_self())) { return 3; }
	Common::HostLockCriticalGuard critical;
	// No callbacks, allocation, pool/stop/join lock, or captured guest identity.
	// Cleanup and native TLS teardown retain these pins until successful join.
	for (size_t i = 0; i < thread->join_lifetime_count; ++i) {
		if (thread->join_lifetimes[i].get() == lifetime.get()) { return 0; }
	}
	if (thread->join_lifetime_count == thread->join_lifetimes.size()) { return 28; }
	thread->join_lifetimes[thread->join_lifetime_count++] = std::move(lifetime);
	return 0;
}

int PthreadCaptureCurrentNativeReceipt(PthreadNativeReceipt& receipt) noexcept {
	if (receipt) { return 22; }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return 45;
#else
	try {
		auto* self = g_pthread_self;
		if (!self || !self->pool_managed || self->runtime_owned || self->free || self->join_reclaiming ||
		    self->almost_done || self->startup.load(std::memory_order_acquire) != 1 ||
		    !::pthread_equal(self->p, ::pthread_self())) { return 3; }
		// RunThread initialized the host-critical counter before admitting entry.
		Common::HostLockCriticalGuard critical;
		int pid = -1;
		if (!Loader::TryGetCurrentRetainedGuestProcessId(&pid)) { return 3; }
		const auto pin = self->native_receipt; // Current target cannot be joined/recycled while executing here.
		if (!pin || !pin->handle || pin->identity.pid != pid || pin->identity.tid != self->guest.thread_id ||
		    pin->identity.owner_cookie != reinterpret_cast<uint64_t>(Loader::RuntimeLinker::GetCurrentProgram()) ||
		    pin->identity.native_tid != GetCurrentThreadId()) { return 3; }
		receipt = pin;
		return 0;
	} catch (const std::bad_alloc&) { return 12; }
	catch (...) { return 5; }
#endif
}

int PthreadInspectNativeReceipt(const PthreadNativeReceipt& receipt, PthreadNativeSnapshot& output) noexcept {
	if (!receipt) { return 22; }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	(void)output;
	return 45;
#else
	const auto pin = receipt;
	if (!pin->handle || !pin->identity.generation) { return 3; }
	auto snapshot = pin->identity;
	const auto source = pin->joined.load(std::memory_order_acquire);
	const auto waited = WaitForSingleObject(pin->handle, 0);
	if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) { return 5; }
	if (source != PthreadNativeJoinSource::None) {
		if (waited != WAIT_OBJECT_0) { return 5; }
		snapshot.phase = PthreadNativePhase::Joined;
		snapshot.join_source = source;
		snapshot.joined_value = pin->joined_value;
		snapshot.joiner_pid = pin->joiner_pid;
		snapshot.joiner_tid = pin->joiner_tid;
	} else {
		snapshot.phase = waited == WAIT_OBJECT_0 ? PthreadNativePhase::NativeExited : PthreadNativePhase::Pending;
	}
	output = snapshot;
	return 0;
#endif
}

Pthread PthreadSwapSelfForSignal(Pthread thread) {
	auto* previous = g_pthread_self;
	g_pthread_self = thread;
	return previous;
}

uint64_t PthreadGetHostThreadId(Pthread thread) {
	return thread != nullptr ? thread->host_thread_id : 0;
}

bool PthreadIsHostLockCritical(Pthread thread) {
	return thread != nullptr && thread->host_lock_critical != nullptr &&
	       thread->host_lock_critical->load(std::memory_order_seq_cst) != 0;
}

namespace {
// ps5-vsh: co-elf main registry for the sampler (see NoteCoElfThread).
// Entries live until process exit (co-elf mains never return); stale ids
// (a recycled Windows TID) only cost one suspend/resume cycle.
struct CoElfThread {
	std::string name;
	uint64_t    os_thread_id = 0;
};
Common::Mutex              g_coelf_mutex;
std::vector<CoElfThread>   g_coelf_threads;
} // namespace

void NoteCoElfThread(const std::string& name, uint64_t os_thread_id) {
	Common::LockGuard lock(g_coelf_mutex);
	for (auto& t: g_coelf_threads) {
		if (t.os_thread_id == os_thread_id) {
			t.name = name;
			return;
		}
	}
	g_coelf_threads.push_back({name, os_thread_id});
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Returns RIP (+ optionally RSP/RBP) of a suspended-then-resumed thread.
// Reads the target's registers only; stack is walked after resume.
static bool SampleOneThread(uint64_t os_thread_id, uint64_t self_id, uint64_t* rip_out,
                            uint64_t* rsp_out = nullptr, uint64_t* rbp_out = nullptr) {
	if (os_thread_id == 0 || os_thread_id == self_id) {
		return false;
	}
	HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
	                      static_cast<DWORD>(os_thread_id));
	if (h == nullptr) {
		return false;
	}
	if (SuspendThread(h) == static_cast<DWORD>(-1)) {
		CloseHandle(h);
		return false;
	}
	CONTEXT ctx {};
	ctx.ContextFlags = CONTEXT_CONTROL;
	const bool ok = (GetThreadContext(h, &ctx) != 0);
	if (ok) {
		if (rip_out != nullptr) {
			*rip_out = static_cast<uint64_t>(ctx.Rip);
		}
		if (rsp_out != nullptr) {
			*rsp_out = static_cast<uint64_t>(ctx.Rsp);
		}
		if (rbp_out != nullptr) {
			*rbp_out = static_cast<uint64_t>(ctx.Rbp);
		}
	}
	ResumeThread(h);
	CloseHandle(h);
	return ok;
}
#endif

// ps5-vsh: SEH-guarded 16-byte read (kept free of C++ locals: __try
// forbids unwinding objects in the same function).
static bool ReadGuestPair(uint64_t addr, uint64_t out[2]) {
	__try {
		if (addr == 0) {
			return false;
		}
		out[0] = *reinterpret_cast<uint64_t*>(addr);
		out[1] = *reinterpret_cast<uint64_t*>(addr + 8);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// ps5-vsh: best-effort guest rbp-chain walk for the sampler (the target
// runs concurrently, so frames may tear — informative only).
static void LogGuestBacktrace(const char* name, uint64_t rbp, const char* reason = "sampler-bt") {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	LOGF("ps5-vsh: %s: %s", reason, name);
	for (int i = 0; i < 8; i++) {
		uint64_t frame[2] = {};
		if (!ReadGuestPair(rbp, frame)) {
			break;
		}
		const uint64_t ret = frame[1];
		std::string    where("?");
		if (const auto* p = rt->FindProgramByAddr(ret); p != nullptr) {
			char tmp[128] = {};
			std::snprintf(tmp, sizeof(tmp), "%s+0x%llx",
			              Common::PathToGenericString(p->file_name.filename()).c_str(),
			              (unsigned long long)(ret - p->base_vaddr));
			where = tmp;
		}
		LOGF(" [0x%llx %s]", (unsigned long long)ret, where.c_str());
		const uint64_t next = frame[0];
		if (next == 0 || next == rbp) {
			break;
		}
		rbp = next;
	}
	LOGF("\n");
}

void PthreadLogThreadSample() {
	if (g_pthread_context == nullptr || g_pthread_context->GetPthreadPool() == nullptr) {
		return;
	}
	auto samples = g_pthread_context->GetPthreadPool()->SampleAllThreads();
	auto*  rt    = Common::Singleton<Loader::RuntimeLinker>::Instance();
	for (const auto& s: samples) {
		if (!s.ok) {
			continue;
		}
		const std::string mod = [&] {
			if (const auto* p = rt->FindProgramByAddr(s.rip); p != nullptr) {
				return Common::PathToGenericString(p->file_name.filename());
			}
			return std::string("?");
		}();
		LOGF("ps5-vsh: sampler: %-28s os=%llu rip=0x%016llx %s\n", s.name.c_str(),
		     (unsigned long long)s.os_thread_id, (unsigned long long)s.rip, mod.c_str());
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// ps5-vsh: same treatment for registered co-elf mains (raw host
	// threads outside the pthread pool).
	const uint64_t self = static_cast<uint64_t>(GetCurrentThreadId());
	std::vector<CoElfThread> coelfs;
	{
		Common::LockGuard lock(g_coelf_mutex);
		coelfs = g_coelf_threads;
	}
	for (const auto& t: coelfs) {
		uint64_t rip = 0, rsp = 0, rbp = 0;
		if (!SampleOneThread(t.os_thread_id, self, &rip, &rsp, &rbp)) {
			continue;
		}
		const std::string mod = [&] {
			if (const auto* p = rt->FindProgramByAddr(rip); p != nullptr) {
				return Common::PathToGenericString(p->file_name.filename());
			}
			return std::string("?");
		}();
		LOGF("ps5-vsh: sampler: %-28s os=%llu rip=0x%016llx %s\n", t.name.c_str(),
		     (unsigned long long)t.os_thread_id, (unsigned long long)rip, mod.c_str());
		// ps5-vsh: native backtrace for co-elf mains (names the Mono
		// wait: eboot -> ... -> threadpool). RSP scan: any slot pointing
		// into a loaded program is a probable return address.
		if (t.name.starts_with("CoElf:") || t.name.starts_with("eboot")) {
			LogGuestBacktrace(t.name.c_str(), rbp);
			LOGF("ps5-vsh: sampler-bt: %s rsp-scan", t.name.c_str());
			for (uint64_t off = 0; off < 4096; off += 16) {
				uint64_t pair[2] = {};
				if (!ReadGuestPair(rsp + off, pair)) {
					break;
				}
				for (int k = 0; k < 2; k++) {
					if (const auto* p = rt->FindProgramByAddr(pair[k]);
					    p != nullptr) {
						LOGF(" rsp+0x%llx: 0x%llx %s+0x%llx", (unsigned long long)off,
						     (unsigned long long)pair[k],
						     Common::PathToGenericString(p->file_name.filename()).c_str(),
						     (unsigned long long)(pair[k] - p->base_vaddr));
					}
				}
			}
			LOGF("\n");
		}
	}
#endif
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// Raise a host signal on another guest thread.
bool PthreadKillHost(Pthread thread, int host_signal) {
	if (thread == nullptr || thread->free) {
		return false;
	}

	return ::pthread_kill(thread->p, host_signal) == 0;
}
#endif

void PthreadQueuePendingSignal(Pthread thread, int signum) {
	if (PthreadIsRuntimeOwned(thread)) { return; } // Never accept shared-owner global session signal work.
	if (thread == nullptr || signum < 0 || signum >= 64) {
		return;
	}

	thread->pending_signal_mask.fetch_or(1ull << static_cast<uint32_t>(signum),
	                                     std::memory_order_release);
}

bool PthreadHasPendingSignal(Pthread thread, int signum) {
	if (thread == nullptr || signum < 0 || signum >= 64) {
		return false;
	}

	const auto mask = 1ull << static_cast<uint32_t>(signum);
	return (thread->pending_signal_mask.load(std::memory_order_acquire) & mask) != 0;
}

bool PthreadTakePendingSignal(Pthread thread, int signum) {
	if (thread == nullptr || signum < 0 || signum >= 64) {
		return false;
	}

	const auto mask = 1ull << static_cast<uint32_t>(signum);
	return (thread->pending_signal_mask.fetch_and(~mask, std::memory_order_acq_rel) & mask) != 0;
}

bool PthreadGetGuestStack(Pthread thread, uint64_t* stack_addr, uint64_t* stack_size) {
	if (thread == nullptr || thread->attr == nullptr || stack_addr == nullptr ||
	    stack_size == nullptr) {
		return false;
	}

	auto* addr = thread->attr->stack_addr;
	auto  size = thread->attr->stack_size;
	if (addr == nullptr || size == 0) {
		return false;
	}

	*stack_addr = reinterpret_cast<uint64_t>(addr);
	*stack_size = static_cast<uint64_t>(size);
	return true;
}

int PthreadGetPriorityForKernel(Pthread thread) {
	if (thread == nullptr) {
		return 700;
	}

	sched_param param {};
	int         pol = 0;

	if (pthread_getschedparam(thread->p, &pol, &param) != 0) {
		return 700;
	}

	if (param.sched_priority <= -2) {
		return 767;
	}
	if (param.sched_priority >= +2) {
		return 256;
	}
	return 700;
}

int PthreadGetCurrentPriorityForKernel() {
	return PthreadGetPriorityForKernel(g_pthread_self);
}

static void CleanupThreadBody(void* arg) {
	auto* thread = static_cast<Pthread>(arg);

	// Never enter guest cleanup after owner failure/stop (including refused
	// startup). Registered libc atexit records are POD and their host TLS vector
	// is destroyed by normal native-thread teardown without invoking callbacks.
	const auto may_call_guest = [thread] {
		return thread->startup.load(std::memory_order_acquire) == 1 &&
		    (!thread->execution || (!thread->execution->StopRequested() && !thread->execution->PendingThreadExit()));
	};
	if (may_call_guest()) { LibcInternalExt::RunThreadAtexitDestructors(); }

	auto thread_dtors = thread->execution ? reinterpret_cast<thread_dtors_func_t>(thread->execution->ThreadDestructor()) :
	    g_pthread_context->GetThreadDtors();

	if (thread_dtors != nullptr && may_call_guest()) {
		if (thread->execution) { thread->execution->InvokeCallback(reinterpret_cast<uint64_t>(thread_dtors)); }
		else { thread_dtors(); }
	}

	if (may_call_guest()) { DestructPthreadSpecific(); }
	else { g_pthread_specific.reset(); }
	if (thread->execution) { thread->execution->ConsumeThreadExit(); } // Cleanup callback exit never escapes host teardown.

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	// Callback-backed TLS blocks need their own transition/lock audit even on
	// a normal return; retain those blocks for every shared execution for now.
	rt->DeleteTlss(thread->unique_id, !thread->execution);

	// Guest destructors above still see the launch context. The record retains
	// a strong reference until join; helper signal deliveries own their copies.
	FileSystem::SetCurrentFsContext(nullptr);
	auto* application = std::exchange(thread->counted_application, nullptr);
	LOGF("\tPthread run end: %s, id = %d\n", thread->name.c_str(), thread->unique_id);
	thread->almost_done = true;
	if (application != nullptr) {
		application->application_threads.fetch_sub(1);
	}
	// Logical guest cleanup only, NOT native termination. VSH must also use the
	// retained-handle native drain gate before publishing close. No guest/mapped
	// owner accesses follow; execution/fs pins remain through actual native join.
	thread->execution_thread.Reset();
}

static void* KYTY_SYSV_ABI CleanupThreadOnGuestStack(void* arg) {
	// HOST coordination, not a disposable guest segment. Each destructor call
	// below owns its own innermost boundary and returns through this host frame.
	CleanupThreadBody(arg);
	return nullptr;
}
static void CleanupThread(void* arg) {
	auto* thread = static_cast<Pthread>(arg);
	if (thread->execution) {
		// The worker/coordinator invocation has already returned off this stack.
		// Reuse the original, join-pinned guest stack so destructor private-text
		// loops satisfy exactly the same stop proof as ordinary guest callbacks.
		auto* top = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(thread->attr->stack_addr) +
		    thread->attr->stack_size) & ~uintptr_t {15});
		RunOnGuestStack(thread, CleanupThreadOnGuestStack, top);
	} else { CleanupThreadBody(arg); } // Preserve legacy native cleanup/exit semantics.
}

struct WorkerArguments {
	pthread_entry_func_t entry;
	void* argument;
};
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
static_assert(offsetof(WorkerArguments, argument) == 8);
// Only this trivial typed transition is beneath the disposable boundary.
__attribute__((naked)) static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV WorkerTransition(void*) {
	__asm__ volatile("movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "movq 8(%r10), %rdi\n\t"
	                 "xorl %ebp, %ebp\n\t"
	                 "jmp *%rax\n\t");
}
#endif

static KYTY_SYSV_ABI void* RunRuntimeWorker(void* argument) {
	auto* thread = static_cast<Pthread>(argument);
	WorkerArguments arguments {thread->entry, thread->arg};
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	thread->guest_result = thread->execution->Invoke(WorkerTransition, &arguments);
#else
	thread->guest_result.failure = thread->execution->Fail(Loader::GuestFailureReason::BoundaryUnavailable);
#endif
	return thread->guest_result.Returned() ? reinterpret_cast<void*>(thread->guest_result.call.value) :
	    reinterpret_cast<void*>(~uintptr_t {0});
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
using RuntimeNativeEpilogueHook = void (*)(Pthread);
static std::atomic<RuntimeNativeEpilogueHook> g_runtime_native_epilogue_for_test {nullptr};
static std::atomic<RuntimeNativeEpilogueHook> g_join_retention_epilogue_for_test {nullptr};
#endif
static void* RunThread(void* arg) {

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (!Common::HostException::InitializeThreadSignalStack()) {
		EXIT("Failed to initialize the thread signal stack\n");
	}
#endif

	auto* thread = static_cast<Pthread>(arg);
	void* ret    = nullptr;
	// Native creation and guest-visible publication are separate. Never wait
	// while holding the execution control's Publish mutex.
	{
		std::unique_lock lock(thread->startup_mutex);
		thread->startup_cv.wait(lock, [thread] { return thread->startup.load(std::memory_order_acquire) != 0; });
	}

	thread->unique_id = Common::Thread::GetThreadIdUnique();

	g_pthread_self = thread;
	thread->host_lock_critical = Common::GetHostLockCriticalCounter();
	// ps5-vsh: inherit the creating thread's program scope (voldemort
	// parity: SetCurrentProgram(thread->owner_program)). Null falls back
	// to the main program inside GetCurrentProgram.
	Loader::RuntimeLinker::SetCurrentProgram(thread->owner_program);
	FileSystem::SetCurrentFsContext(PthreadGetFsContext(thread));

	uint64_t os_thread_id = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	os_thread_id = static_cast<uint64_t>(GetCurrentThreadId());
#else
	os_thread_id = GetHostThreadId();
#endif
	thread->host_thread_id = os_thread_id;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
	    &thread->stop_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		if (thread->execution) { Loader::GuestCallBoundary::BindStopState(&thread->stop_boundary); }
		thread->stop_ready.store(true, std::memory_order_release);
	}
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (!thread->name.empty()) {
#if defined(__APPLE__)
		pthread_setname_np(thread->name.substr(0, 63).c_str());
#else
		pthread_setname_np(pthread_self(), thread->name.substr(0, 15).c_str());
#endif
	}
#endif

	LOGF("\tPthread run begin: %s, id = %d, os_thread_id = %" PRIu64 ", entry = 0x%016" PRIx64
	     ", arg = 0x%016" PRIx64 ", stack_addr = 0x%016" PRIx64 ", stack_size = %" PRIu64 "\n",
	     thread->name.c_str(), thread->unique_id, os_thread_id,
	     reinterpret_cast<uint64_t>(thread->entry), reinterpret_cast<uint64_t>(thread->arg),
	     reinterpret_cast<uint64_t>(thread->attr->stack_addr),
	     static_cast<uint64_t>(thread->attr->stack_size));

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_push(CleanupThread, thread);

	auto* stack_top = reinterpret_cast<void*>(
	    (reinterpret_cast<uintptr_t>(thread->attr->stack_addr) + thread->attr->stack_size) &
	    ~static_cast<uintptr_t>(0x0f));
	if (thread->startup.load(std::memory_order_acquire) == 1) {
		// The runtime coordinator is HOST C++ even on a guest stack. It owns its
		// individual guest boundaries; never put a root abort around its RAII.
		ret = thread->execution && !thread->runtime_coordinator ?
		    RunOnGuestStack(thread, RunRuntimeWorker, stack_top) :
		    RunOnGuestStack(thread->arg, thread->entry, stack_top);
	}
	if (thread->execution) {
		Loader::RuntimeCrt::AbandonCurrentThreadGuards(thread->execution);
		const auto exited = thread->execution->ConsumeThreadExit();
		if (thread->execution->Failure() || thread->execution->StopRequested()) {
			ret = reinterpret_cast<void*>(~uintptr_t {0});
		} else if (exited) { ret = reinterpret_cast<void*>(exited.value); }
	}

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_pop(1);
	if (thread->execution) {
		thread->guest_result.failure = thread->execution->Failure();
		thread->guest_result.stopped = thread->execution->StopRequested();
		if (thread->guest_result.failure || thread->guest_result.stopped) { ret = reinterpret_cast<void*>(~uintptr_t {0}); }
	}

	Loader::GuestCallBoundary::BindStopState(nullptr);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	// HOST epilogue only: cleanup/leases have finished, but neither native
	// return nor host TLS teardown has happened. Never enter guest code here.
	if (thread->execution) {
		if (auto hook = g_runtime_native_epilogue_for_test.load(std::memory_order_acquire)) { hook(thread); }
	}
	if (auto hook = g_join_retention_epilogue_for_test.load(std::memory_order_acquire)) { hook(thread); }
#endif
	return ret;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static void (*g_runtime_thread_before_publish)() = nullptr;
static bool g_runtime_thread_fail_native_create = false;
#endif

static int CreatePthread(Pthread* thread, const PthreadAttr* attr,
                         pthread_entry_func_t entry, void* arg, const char* name, bool runtime_coordinator,
                         PthreadNativeReceipt* receipt_output = nullptr) {
	PRINT_NAME();
	HLE_SLOW_GUARD("pthread-create");

	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* owner_program = Loader::RuntimeLinker::GetCurrentProgram();
	auto execution = Loader::GetApplicationExecution(owner_program);
	// Allocate the opaque control block before pool/record locks. Unknown-owner
	// legacy callers retain their old creation behavior but cannot capture a
	// fabricated PID100 receipt. Explicit receipt creation fails closed.
	std::shared_ptr<PthreadNativeReceiptState> pending_receipt;
	int32_t creator_pid = -1, creator_tid = -1;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	int receipt_pid = -1;
	if (!execution && owner_program && (!g_pthread_self || ::pthread_equal(g_pthread_self->p, ::pthread_self())) &&
	    Loader::TryGetCurrentRetainedGuestProcessId(&receipt_pid)) {
		try { pending_receipt = std::make_shared<PthreadNativeReceiptState>(); }
		catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
		pending_receipt->identity.generation = NextNativeReceiptGeneration();
		if (!pending_receipt->identity.generation) { return KERNEL_ERROR_EOVERFLOW; }
		pending_receipt->identity.owner_cookie = reinterpret_cast<uint64_t>(owner_program);
		pending_receipt->identity.pid = receipt_pid;
		pending_receipt->identity.entry = reinterpret_cast<uint64_t>(entry);
		pending_receipt->identity.argument = reinterpret_cast<uint64_t>(arg);
		CurrentNativeJoinIdentity(creator_pid, creator_tid);
	}
#endif
	if (receipt_output && !pending_receipt) { return KERNEL_ERROR_EOPNOTSUPP; }
	auto execution_thread = execution ? execution->TryAcquire(Loader::GuestWorkKind::Thread) :
	    Loader::RuntimeGuestExecution::Lease {};
	if (execution && !execution_thread) { return KERNEL_ERROR_ECANCELED; }

	auto* pthread_pool = g_pthread_context->GetPthreadPool();

	if (attr == nullptr) {
		attr = g_pthread_context->GetDefaultAttr();
	}
	if (GetPthreadAttrValue(attr, "PthreadCreate") == nullptr) {
		attr = g_pthread_context->GetDefaultAttr();
	}
	// Firmware accepts PROCESS at setter time. We have no process scheduler;
	// fail creation without allocating/publishing a thread, not silent SYSTEM.
	if ((*attr)->scope == 0) {
		return KERNEL_ERROR_EOPNOTSUPP;
	}

	PRINT_NAME_ENABLE(false);

	auto* created_thread = pthread_pool->Create(bool(execution));
	std::unique_lock record_lock(created_thread->join_mutex);
	std::unique_lock stop_record_lock(created_thread->stop_mutex);
	EXIT_IF(created_thread->native_receipt);
	created_thread->native_receipt = pending_receipt;
	created_thread->stop_ready.store(false, std::memory_order_relaxed);
	created_thread->stop_boundary.top.store(nullptr, std::memory_order_relaxed);
	created_thread->stop_boundary.host_boundary_depth.store(0, std::memory_order_relaxed);
	created_thread->stop_boundary.in_flight.store(false, std::memory_order_relaxed);
	created_thread->stop_boundary.sequence = 0;
	created_thread->pending_signal_mask.store(0, std::memory_order_relaxed);
	created_thread->execution = std::move(execution);
	created_thread->execution_thread = std::move(execution_thread);
	created_thread->startup.store(0, std::memory_order_relaxed);
	created_thread->guest_result = {};

	if (created_thread->attr != nullptr) {
		PthreadAttrDestroy(&created_thread->attr);
	}

	PthreadAttrInit(&created_thread->attr);

	int result = PthreadAttrCopy(&created_thread->attr, attr);
	if (result == 0) {
		// Translate guest SYSTEM=2 to the host constant (often 0), never pass 2.
		const int host_result = pthread_attr_setscope(&created_thread->attr->p, PTHREAD_SCOPE_SYSTEM);
		result = host_result == ENOTSUP ? KERNEL_ERROR_EOPNOTSUPP : host_result;
	}

	if (result == 0) {
		EXIT_IF(created_thread->free);

		if (created_thread->attr->stack_addr == nullptr) {
			created_thread->attr->stack_size += PTHREAD_STACK_EXTRA;
		}

		result = CreateGuestStack(created_thread->attr);
	}

	if (result == 0) {
		created_thread->name            = (name != nullptr ? name : "");
		created_thread->entry           = entry;
		created_thread->runtime_coordinator = runtime_coordinator;
		created_thread->arg             = arg;
		created_thread->guest.thread_id = ++g_pthread_thread_id;
		if (pending_receipt) {
			pending_receipt->record = created_thread;
			pending_receipt->identity.tid = created_thread->guest.thread_id;
			pending_receipt->identity.detached_at_creation = created_thread->attr->detached;
		}
		created_thread->almost_done     = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		created_thread->guest_cancel_requested = false;
#endif
		// A refused startup is joined by the creator, never by the collector.
		created_thread->detached        = false;
		created_thread->unique_id       = -1;
		// ps5-vsh: scope inheritance (see owner_program): the creator's
		// current program becomes this thread's program on entry.
		created_thread->owner_program = owner_program;
		created_thread->fs_context = FileSystem::GetCurrentFsContext();
		created_thread->counted_application = nullptr;

		#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (std::exchange(g_runtime_thread_fail_native_create, false)) { result = EAGAIN; }
		else
		#endif
		{ result = pthread_create(&created_thread->p, &created_thread->attr->p, RunThread, created_thread); }

		if (result == 0) {
			bool receipt_ready = true;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (pending_receipt) {
				// Child is parked on startup. Creator owns join_mutex and has not
				// published the record: duplicate the ORIGINAL native handle now.
				const auto original = static_cast<HANDLE>(::pthread_gethandle(created_thread->p));
				receipt_ready = original && original != INVALID_HANDLE_VALUE &&
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
				    !g_native_receipt_duplicate_fail_for_test.exchange(false) &&
#endif
				    DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &pending_receipt->handle,
				                    0, FALSE, DUPLICATE_SAME_ACCESS);
				if (receipt_ready) {
					pending_receipt->identity.native_tid = GetThreadId(pending_receipt->handle);
					receipt_ready = pending_receipt->identity.native_tid != 0;
				}
			}
#endif
			bool published = false;
			try {
				#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
				if (g_runtime_thread_before_publish) { g_runtime_thread_before_publish(); }
				#endif
				std::unique_lock startup_lock(created_thread->startup_mutex);
				// No fallible logging/allocation after publishing caller outputs or
				// admitting guest entry. Exceptions here still refuse/notify/join the
				// real parked native thread before any record can be reused.
				LOGF("\tthread create: %s, id = %d, %d\n", created_thread->name.c_str(),
				     created_thread->unique_id, result);
				PthreadAttrDbgPrint(&created_thread->attr);
				PRINT_NAME_ENABLE(true);
				const auto publish = [&] {
					created_thread->detached = created_thread->attr->detached;
					created_thread->counted_application = owner_program ? owner_program->application_owner : nullptr;
					if (created_thread->counted_application) {
						created_thread->counted_application->application_threads.fetch_add(1);
					}
					*thread = created_thread;
					if (receipt_output) { *receipt_output = pending_receipt; } // Required empty: no arbitrary deleter here.
					created_thread->startup.store(1, std::memory_order_release);
				};
				if (receipt_ready) {
					if (created_thread->execution) { published = created_thread->execution_thread.Publish(publish); }
					else { publish(); published = true; }
				}
			} catch (const std::bad_alloc&) { result = KERNEL_ERROR_ENOMEM; }
			catch (...) { result = KERNEL_ERROR_EIO; }
			if (!published) {
				std::lock_guard startup_lock(created_thread->startup_mutex);
				created_thread->startup.store(-1, std::memory_order_release);
			}
			created_thread->startup_cv.notify_one();
			if (published) { return OK; } // No record accesses after admitting startup.
			if (!published) {
				// The native thread owns cleanup, but never enters guest code. Join
				// outside Publish before reclaiming any stack/context/control pin.
				void* refused_value = nullptr;
				if (pthread_join(created_thread->p, &refused_value) != 0) {
					created_thread->detached = true; // Retain for the existing collector.
					return KERNEL_ERROR_ECANCELED;
				}
				created_thread->join_reclaiming = true;
				if (pending_receipt) {
					pending_receipt->joined_value = reinterpret_cast<uint64_t>(refused_value);
					pending_receipt->joiner_pid = creator_pid;
					pending_receipt->joiner_tid = creator_tid;
					pending_receipt->joined.store(PthreadNativeJoinSource::StartupRollback, std::memory_order_release);
				}
				if (result == 0) { result = KERNEL_ERROR_ECANCELED; }
			}
		}
	}

	if (result != 0) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (created_thread->stop_handle) { CloseHandle(created_thread->stop_handle); created_thread->stop_handle = nullptr; }
#endif
		created_thread->stop_ready.store(false, std::memory_order_release);
		FreeGuestStack(created_thread->attr);
		created_thread->execution_thread.Reset();
		created_thread->execution.reset();
		created_thread->fs_context.reset(); // No native thread can still be executing.
		created_thread->owner_program = nullptr;
		created_thread->almost_done = false;
		created_thread->detached = false;
		// Failed native creation/refused publication cannot run an admitted
		// entry. Never erase an unexpected lifetime under creator record locks.
		EXIT_IF(created_thread->join_lifetime_count != 0);
		created_thread->native_receipt.reset(); // pending_receipt keeps destruction outside record locks.
	}

	LOGF("\tthread create: %s, id = %d, %d\n", created_thread->name.c_str(),
	     created_thread->unique_id, result);

	PthreadAttrDbgPrint(&created_thread->attr);

	PRINT_NAME_ENABLE(true);
	if (result != 0) { created_thread->join_reclaiming = false; created_thread->free = true; }

	if (result < 0) {
		return result;
	}

	switch (result) {
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr* attr,
                                pthread_entry_func_t entry, void* arg, const char* name) {
	return CreatePthread(thread, attr, entry, arg, name, false);
}

int PthreadCreateWithNativeReceipt(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry,
                                  void* arg, const char* name, PthreadNativeReceipt& receipt) noexcept {
	if (!thread || !entry || receipt) { return 22; }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	(void)attr; (void)arg; (void)name;
	return 45;
#else
	try {
		const int result = CreatePthread(thread, attr, entry, arg, name, false, &receipt);
		return result == 0 ? 0 : KernelToPosix(result);
	}
	catch (const std::bad_alloc&) { return 12; }
	catch (...) { return 5; }
#endif
}

int PthreadCreateRuntimeCoordinator(Pthread* thread, const PthreadAttr* attr,
                                    pthread_entry_func_t entry, void* arg, const char* name) {
	return CreatePthread(thread, attr, entry, arg, name, true);
}

PthreadOwnerStopResult PthreadRequestOwnerStop(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) noexcept {
	if (!execution || !execution->Owner().identity || !execution->Owner().token) { return {}; }
	execution->RequestStop(); // Close publication BEFORE inspection/redirect.
	PthreadOwnerStopResult result {PthreadOwnerStopStatus::Unsupported, execution->Owner().token};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	// Atomic shared_ptr load and registry allocation/locking happen BEFORE any
	// suspend. While suspended: only pinned plain/atomic metadata and Win32 calls.
	auto text = execution->StopText();
	if (!text || !g_pthread_context) { return result; }
	try {
		bool acquired = false;
		auto candidates = g_pthread_context->GetPthreadPool()->StopCandidates(&acquired);
		if (!acquired) { result.status = PthreadOwnerStopStatus::Pending; ++result.pending; return result; }
		bool control_error = false;
		for (auto* thread: candidates) {
			std::unique_lock pin(thread->stop_mutex, std::try_to_lock);
			if (!pin) { ++result.pending; continue; }
			if (thread->free || thread->execution != execution) { continue; }
			if (!thread->stop_ready.load(std::memory_order_acquire) || !thread->stop_handle ||
			    thread->host_thread_id == GetCurrentThreadId() || thread->pending_signal_mask.load(std::memory_order_acquire) ||
			    thread->stop_boundary.in_flight.load(std::memory_order_acquire)) { ++result.pending; continue; }
			const auto handle = thread->stop_handle; // Actual duplicated handle, NEVER OpenThread(OS-id).
			const auto previous = SuspendThread(handle);
			if (previous == static_cast<DWORD>(-1)) { ++result.pending; continue; }
			bool redirected = false;
			CONTEXT context {};
			context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(handle, &context) && previous == 0 && text->Contains(context.Rip) &&
			    thread->host_lock_critical && thread->host_lock_critical->load(std::memory_order_seq_cst) == 0 &&
			    !(context.EFlags & (0x100u | 0x10000u)) && !(context.Dr7 & 0xffu)) {
				const auto begin = reinterpret_cast<uint64_t>(thread->attr->stack_addr);
				const auto size = static_cast<uint64_t>(thread->attr->stack_size);
				const auto landing = size <= UINT64_MAX - begin ? execution->Boundary()->PrepareAsyncStop(
				    thread->stop_boundary, thread->host_thread_id, context.Rsp, begin, begin + size) : Loader::GuestBoundaryStopLanding {};
				if (landing) {
					context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					context.Rip = landing.entry;
					context.R11 = landing.frame;
					context.EFlags &= ~0x400u; // Dedicated landing uses no interrupted stack/ABI call frame.
					thread->stop_boundary.in_flight.store(true, std::memory_order_release);
					redirected = SetThreadContext(handle, &context) != 0;
					if (!redirected) { thread->stop_boundary.in_flight.store(false, std::memory_order_release); }
				}
			}
			// Always release exactly OUR suspend contribution before unlocking,
			// logging, allocation or touching any owner/registry mutex.
			if (ResumeThread(handle) == static_cast<DWORD>(-1)) { control_error = true; }
			if (redirected) { ++result.redirected; } else { ++result.pending; }
		}
		result.status = control_error ? PthreadOwnerStopStatus::ControlError :
		    (result.redirected ? PthreadOwnerStopStatus::Redirected : (result.pending ? PthreadOwnerStopStatus::Pending : PthreadOwnerStopStatus::Idle));
	} catch (...) { result.status = PthreadOwnerStopStatus::Pending; ++result.pending; }
#endif
	return result;
}

PthreadOwnerNativeDrainResult PthreadOwnerNativeThreadsTerminated(
    const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) noexcept {
	if (!execution || !execution->Owner().identity || !execution->Owner().token) { return {}; }
	PthreadOwnerNativeDrainResult result {PthreadOwnerNativeDrainStatus::Unsupported, execution->Owner().token};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!execution->StopRequested()) { result.status = PthreadOwnerNativeDrainStatus::AdmissionOpen; return result; }
	// A Thread lease is acquired BEFORE pool insertion/native creation. With
	// admission closed, zero leases rules out an unsnapshotted future native
	// thread; a real thread retains its lease until its record is visible and
	// guest cleanup finishes. Non-thread leases conservatively keep this pending.
	if (execution->Outstanding() != 0) { result.status = PthreadOwnerNativeDrainStatus::Pending; ++result.pending; return result; }
	if (!g_pthread_context || !g_pthread_context->GetPthreadPool()) {
		result.status = PthreadOwnerNativeDrainStatus::ControlError; ++result.pending; return result;
	}
	try {
		bool acquired = false;
		auto candidates = g_pthread_context->GetPthreadPool()->StopCandidates(&acquired);
		if (!acquired) { result.status = PthreadOwnerNativeDrainStatus::Pending; ++result.pending; return result; }
		bool control_error = false;
		for (auto* thread: candidates) {
			// Pool entries never disappear. Never wait for join_mutex or perform a
			// join under the pool lock; this independent pin covers handle closure
			// and incarnation reuse even while pthread_join reclaims its own handle.
			std::unique_lock pin(thread->stop_mutex, std::try_to_lock);
			if (!pin) { ++result.pending; continue; }
			if (thread->free || thread->execution != execution) { continue; }
			if (!thread->stop_ready.load(std::memory_order_acquire)) {
				++result.pending;
				// Once cleanup is visible, publication cannot still be forthcoming.
				// Recheck after that acquire so an earlier 'not ready' sample cannot
				// misclassify a successful native-handle publication as failure.
				if (thread->almost_done.load(std::memory_order_acquire) &&
				    !thread->stop_ready.load(std::memory_order_acquire)) { control_error = true; }
				continue;
			}
			if (!thread->stop_handle) { ++result.pending; control_error = true; continue; }
			const auto waited = WaitForSingleObject(thread->stop_handle, 0);
			if (waited == WAIT_OBJECT_0) { ++result.terminated; }
			else {
				++result.pending;
				if (waited != WAIT_TIMEOUT) { control_error = true; }
			}
		}
		result.status = control_error ? PthreadOwnerNativeDrainStatus::ControlError :
		    (result.pending ? PthreadOwnerNativeDrainStatus::Pending : PthreadOwnerNativeDrainStatus::Terminated);
	} catch (const std::bad_alloc&) { result.status = PthreadOwnerNativeDrainStatus::Pending; ++result.pending; }
	catch (...) { result.status = PthreadOwnerNativeDrainStatus::ControlError; ++result.pending; }
#endif
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadDetach(Pthread thread) {
	PRINT_NAME();

	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	LOGF("\tthread detach: %s, %d\n", thread->name.c_str(), 0);

	thread->detached = true;

	return OK;
}

static int JoinPthread(Pthread thread, void** value, bool detached_only, void* caller, [[maybe_unused]] void* caller_frame,
                       const PthreadNativeReceipt* expected) {
	PRINT_NAME();
	HLE_SLOW_GUARD("join");

	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (expected && (!*expected || (*expected)->record != thread ||
	    (*expected)->joined.load(std::memory_order_acquire) != PthreadNativeJoinSource::None)) { return KERNEL_ERROR_ESRCH; }
	if (!IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	auto* execution = RuntimeGuestWaitExecution();
	if (thread == g_pthread_self) { return KERNEL_ERROR_EDEADLK; }
	if (RuntimeWaitCanceled(execution)) { return KERNEL_ERROR_ECANCELED; }
	int32_t joiner_pid = -1, joiner_tid = -1;
	CurrentNativeJoinIdentity(joiner_pid, joiner_tid);
	std::shared_ptr<PthreadNativeReceiptState> joined_receipt; // Destroy only after record locks leave scope.
	std::unique_lock join_lock(thread->join_mutex, std::defer_lock);
	if (execution || detached_only || expected) {
		if (!join_lock.try_lock()) { return KERNEL_ERROR_EBUSY; }
	} else { join_lock.lock(); } // Actual host/coordinator joins remain actual joins.
	if (thread->free) { return KERNEL_ERROR_ESRCH; }
	if (thread->join_reclaiming) { return KERNEL_ERROR_EBUSY; }
	if (expected) {
		if (thread->native_receipt != *expected) { return KERNEL_ERROR_ESRCH; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto waited = WaitForSingleObject((*expected)->handle, 0);
		if (waited == WAIT_TIMEOUT) { return KERNEL_ERROR_EBUSY; }
		if (waited != WAIT_OBJECT_0) { return KERNEL_ERROR_EIO; }
#else
		return KERNEL_ERROR_EOPNOTSUPP;
#endif
	}
	if (detached_only && (!thread->detached || !thread->almost_done)) { return KERNEL_ERROR_EBUSY; }
	if (execution && (!execution->FailureFunction() || thread->execution.get() != execution)) {
		return KERNEL_ERROR_EOPNOTSUPP; // Never stop or wait on another owner's lifetime.
	}

	// Log before the native wait as well as after it: a blocked join otherwise
	// hides which service worker is preventing native startup from advancing.
	if (PRINT_NAME_ENABLED) {
		LOGF("\tthread join begin: %s, entry=%p, caller=%p\n", thread->name.c_str(),
		     reinterpret_cast<void*>(thread->entry), caller);
		#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const char* diagnostic = std::getenv("KYTY_NATIVE_STARTUP_DIAGNOSTICS");
		if (diagnostic && std::string_view(diagnostic) == "1" && thread->name == "SceSaveDataWatchDog") {
			// Walk our current frame with the existing fault-contained, eight-frame
			// reader. This is frame-link evidence, not a scan for guessed return PCs.
			LogGuestBacktrace(thread->name.c_str(), reinterpret_cast<uint64_t>(caller_frame), "join-bt");
			auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
			const auto* core = rt->FindProgramByAddr(reinterpret_cast<uint64_t>(caller));
			if (core && core->boot_credential_admission && core->boot_credential_admission->IsCoreImage() &&
			    core->boot_credential_admission->IsLiveFor(core) && core->base_vaddr <= UINT64_MAX - 0x21321ef) {
				// Reviewed alternate Core: abb1a0 reads this service pointer;
				// aaf032..aaf067 rejects null service / +8 / +10 / +18 with
				// 8055db02. Observe only: never construct or repair native state.
				uint64_t slot[2] = {}, first[2] = {}, second[2] = {};
				const bool slot_ok = ReadGuestPair(core->base_vaddr + 0x21321e0, slot);
				const bool first_ok = slot_ok && slot[0] && slot[0] <= UINT64_MAX - 0xf && ReadGuestPair(slot[0], first);
				const bool second_ok = first_ok && slot[0] <= UINT64_MAX - 0x1f && ReadGuestPair(slot[0] + 0x10, second);
				LOGF("ps5-vsh: join-syndy-state: slot_read=%d service=0x%llx first_read=%d second_read=%d "
				     "member08=0x%llx member10=0x%llx member18=0x%llx (snapshot, not captured return)\n",
				     slot_ok, static_cast<unsigned long long>(slot[0]), first_ok, second_ok,
				     static_cast<unsigned long long>(first[1]), static_cast<unsigned long long>(second[0]),
				     static_cast<unsigned long long>(second[1]));
			} else {
				LOGF("ps5-vsh: join-syndy-state: skipped (caller is not live admitted alternate Core)\n");
			}
		}
		#endif
	}

	int result = 0;
	void* joined_value = nullptr;
	if (!execution) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (thread->test_fail_native_join.exchange(false)) { result = EBUSY; }
		else
#endif
		{ result = pthread_join(thread->p, &joined_value); }
	}
	else {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		// A cleanup/almost_done flag is NOT proof native termination completed.
		// Never follow it with an unbounded pthread_join on a guest HLE frame.
		std::mutex poll_mutex;
		std::condition_variable poll_cv;
		std::unique_lock poll_lock(poll_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		struct Polling {
			std::atomic<bool>& flag;
			~Polling() { flag.store(false, std::memory_order_release); }
		} polling {thread->test_guest_join_polling};
		thread->test_guest_join_polling.store(true, std::memory_order_release);
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto native_handle = static_cast<HANDLE>(::pthread_gethandle(thread->p));
		if (!native_handle || native_handle == INVALID_HANDLE_VALUE) { return KERNEL_ERROR_ESRCH; }
#endif
		for (;;) {
			if (RuntimeWaitCanceled(execution)) { result = ECANCELED; break; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			// _pthread_tryjoin may trust its early 'ended' flag. Check actual OS
			// termination instead; join_mutex pins this handle against reclamation.
			const auto waited = WaitForSingleObject(native_handle, 0);
			result = waited == WAIT_OBJECT_0 ? ::pthread_join(thread->p, &joined_value) :
			    (waited == WAIT_TIMEOUT ? EBUSY : ESRCH);
#else
			result = ::pthread_tryjoin_np(thread->p, &joined_value);
#endif
			if (result != EBUSY) { break; }
			poll_cv.wait_for(poll_lock, std::chrono::microseconds(SIGNAL_APC_POLL_MICROS),
			                 [execution] { return RuntimeWaitCanceled(execution); });
			if (!RuntimeWaitCanceled(execution)) { KernelDispatchPendingSignalForCurrentThread(); }
		}
#else
		return KERNEL_ERROR_EOPNOTSUPP; // No bounded native-join primitive audited here.
#endif
	}
	if (result == 0) {
		// Mark consumed before ANY fallible diagnostics/stack reclamation. A
		// cleanup exception must retain the record/pins, never rejoin a consumed
		// native handle or make this incarnation eligible for pool reuse.
		thread->join_reclaiming = true;
		// Successful native join is the sole writer. Its synchronization covers
		// guest cleanup AND native TLS destruction, not merely almost_done/Run.
		if (thread->native_receipt) {
			thread->native_receipt->joined_value = reinterpret_cast<uint64_t>(joined_value);
			thread->native_receipt->joiner_pid = joiner_pid;
			thread->native_receipt->joiner_tid = joiner_tid;
			const auto source = thread->startup.load(std::memory_order_acquire) != 1
			    ? PthreadNativeJoinSource::StartupRollback
			    : (detached_only ? PthreadNativeJoinSource::DetachedCollector : PthreadNativeJoinSource::Explicit);
			thread->native_receipt->joined.store(source, std::memory_order_release);
		}
	}
	const bool canceled = RuntimeWaitCanceled(execution);

	if (PRINT_NAME_ENABLED) {
		LOGF("\tthread join: %s, %d\n", thread->name.c_str(), result);
	}

	if (result == 0) {
		std::unique_lock stop_pin(thread->stop_mutex);
		FreeGuestStack(thread->attr);
		thread->stop_ready.store(false, std::memory_order_release);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (thread->stop_handle) { CloseHandle(thread->stop_handle); thread->stop_handle = nullptr; }
#endif

		// pthread_join has returned: no target APC can still be copying this
		// record. Already dispatched helpers use their own captured reference.
		thread->fs_context.reset();
		thread->execution_thread.Reset(); // Cleanup normally released this already.
		thread->execution.reset();
		thread->owner_program = nullptr;
		thread->almost_done = false;
		if (thread->join_lifetime_count != 0) {
			auto lifetimes = std::move(thread->join_lifetimes);
			thread->join_lifetime_count = 0;
			stop_pin.unlock();
			join_lock.unlock();
			// Arbitrary host deleters may reenter pthread APIs. Native termination
			// is proven, but free remains false until every deleter has returned.
			for (auto& lifetime: lifetimes) { lifetime.reset(); }
			join_lock.lock();
			stop_pin.lock();
		}
		joined_receipt = std::move(thread->native_receipt);
		thread->join_reclaiming = false;
		thread->free = true;
		if (!canceled && value) { *value = joined_value; }
	}
	if (canceled) { return KERNEL_ERROR_ECANCELED; }

	switch (result) {
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EOPNOTSUPP: return KERNEL_ERROR_EOPNOTSUPP;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadJoin(Pthread thread, void** value) {
	return JoinPthread(thread, value, false, __builtin_return_address(0), __builtin_frame_address(0));
}

int PthreadJoinNativeReceipt(const PthreadNativeReceipt& receipt, Pthread expected_target, void** value) noexcept {
	if (!receipt || !expected_target) { return 22; }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	(void)value;
	return 45;
#else
	const auto pin = receipt;
	try {
		const int result = JoinPthread(expected_target, value, false, __builtin_return_address(0),
		                               __builtin_frame_address(0), &pin);
		return result == 0 ? 0 : KernelToPosix(result);
	} catch (const std::bad_alloc&) { return 12; }
	catch (...) { return 5; }
#endif
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadCancel(Pthread thread) {
	PRINT_NAME();

	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	// A joined thread's native handle is gone; the guest may still hold its handle.
	if (thread->free) {
		return KERNEL_ERROR_ESRCH;
	}

	// Native nonlocal cancellation must not bypass live runtime boundaries.
	if (thread->execution) { return KERNEL_ERROR_EOPNOTSUPP; }
	int result = pthread_cancel(thread->p);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (result == 0) { thread->guest_cancel_requested.store(true, std::memory_order_release); }
#endif

	LOGF("\tthread cancel: %s, %d\n", thread->name.c_str(), result);

	switch (result) {
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadKill(Pthread thread, int sig) {
	PRINT_NAME();

	constexpr int POSIX_SIGUSR1    = 30;
	constexpr int POSIX_SIGNAL_MAX = 126;

	if (sig < 0 || sig > POSIX_SIGNAL_MAX) {
		return KERNEL_ERROR_EINVAL;
	}
	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!IsValidPthread(thread) || thread->free || thread->almost_done) {
		return KERNEL_ERROR_ESRCH;
	}

	if (sig == 0) {
		return OK;
	}
	return KernelRaiseException(thread, sig);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSetaffinity(Pthread thread, KernelCpumask mask) {
	PRINT_NAME();

	if (thread == nullptr || !IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	auto result = PthreadAttrSetaffinity(&thread->attr, mask);

	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetaffinity(Pthread thread, KernelCpumask* mask) {
	PRINT_NAME();

	if (thread == nullptr || !IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}
	if (mask == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetaffinity(&thread->attr, mask);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSetcancelstate(int state, int* old_state) {
	PRINT_NAME();

	int pstate = PTHREAD_CANCEL_DISABLE;

	switch (state) {
		case 0: pstate = PTHREAD_CANCEL_ENABLE; break;
		case 1: pstate = PTHREAD_CANCEL_DISABLE; break;
		default: EXIT("unknown state: %d", state);
	}

	int result = pthread_setcancelstate(pstate, old_state);

	LOGF("\tthread setcancelstate: %d\n", result);

	if (old_state != nullptr) {
		switch (*old_state) {
			case PTHREAD_CANCEL_ENABLE: *old_state = 0; break;
			case PTHREAD_CANCEL_DISABLE: *old_state = 1; break;
			default: EXIT("unknown old_state: %d", *old_state);
		}
	}

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSetcanceltype(int type, int* old_type) {
	PRINT_NAME();

	int ptype = PTHREAD_CANCEL_DEFERRED;

	switch (type) {
		case 0: ptype = PTHREAD_CANCEL_DEFERRED; break;
		case 2: ptype = PTHREAD_CANCEL_ASYNCHRONOUS; break;
		default: EXIT("unknown type: %d", type);
	}

	int result = pthread_setcanceltype(ptype, old_type);

	LOGF("\tthread setcanceltype: %d\n", result);

	if (old_type != nullptr) {
		switch (*old_type) {
			case PTHREAD_CANCEL_DEFERRED: *old_type = 0; break;
			case PTHREAD_CANCEL_ASYNCHRONOUS: *old_type = 2; break;
			default: EXIT("unknown type: %d", *old_type);
		}
	}

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetprio(Pthread thread, int* prio) {
	PRINT_NAME();

	if (thread == nullptr || !IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	EXIT_NOT_IMPLEMENTED(prio == nullptr);

	if (thread->free) {
		return KERNEL_ERROR_ESRCH;
	}

	*prio = thread->attr->guest_priority;
	LOGF("\t PthreadGetprio: %d, %d\n", thread->unique_id, *prio);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSetprio(Pthread thread, int prio) {
	PRINT_NAME();

	if (thread == nullptr || !IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	// A joined thread's native handle is gone; the guest may still hold its handle.
	if (thread->free) {
		return KERNEL_ERROR_ESRCH;
	}

	sched_param param {};
	int         pol = 0;

	int result = pthread_getschedparam(thread->p, &pol, &param);

	if (result != 0) {
		return KERNEL_ERROR_EINVAL;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (prio <= 478) {
		param.sched_priority = +2;
	} else if (prio >= 733) {
		param.sched_priority = -2;
	} else {
		param.sched_priority = 0;
	}

	if (pthread_setschedparam(thread->p, pol, &param) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
#endif

	thread->attr->guest_priority = prio;
	LOGF("\t PthreadSetprio: %d, %d\n", thread->unique_id, prio);
	return OK;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadTestcancel() {
	PRINT_NAME();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (g_pthread_self && g_pthread_self->execution) {
		// Native winpthreads cancellation cannot unwind this mixed-ABI stack.
		// Return through the owner gateway instead of using the root trampoline.
		if (g_pthread_self->guest_cancel_requested.load(std::memory_order_acquire)) {
			g_pthread_self->execution->Fail(Loader::GuestFailureReason::InvalidTransition);
		}
		return;
	}
	if (g_pthread_self != nullptr && g_pthread_self->guest_host_rsp != 0) {
		// Winpthreads implements cancellation with a host longjmp. Unwinding
		// from our separate guest stack raises STATUS_BAD_STACK. Use the same
		// return trampoline as scePthreadExit, then run cleanup on the host stack.
		int previous = PTHREAD_CANCEL_DISABLE;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
		if (previous == PTHREAD_CANCEL_ENABLE &&
		    g_pthread_self->guest_cancel_requested.load(std::memory_order_acquire)) {
			PthreadExit(reinterpret_cast<void*>(~uintptr_t {0})); // guest PTHREAD_CANCELED
			__builtin_unreachable();
		}
		pthread_setcancelstate(previous, nullptr);
		return;
	}
#endif
	pthread_testcancel();
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
// Return from all host helpers before the naked exit transition discards a guest
// segment. 1 means return to the host return barrier/coordinator; zero selects
// legacy session-only semantics. A mismatched active boundary is never scanned
// past and never sent to the old thread-root continuation.
__attribute__((naked)) static void KYTY_SYSV_ABI InvalidGuestExit() { __asm__ volatile("ud2\n\t"); }
extern "C" __attribute__((used, noinline)) uintptr_t KYTY_SYSV_ABI KytyPreparePthreadExit(void* value) {
	auto execution = Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram());
	if (!execution) {
		return Loader::GuestCallBoundary::CurrentDepth() ? reinterpret_cast<uintptr_t>(&InvalidGuestExit) : 0;
	}
	execution->RequestThreadExit(reinterpret_cast<uint64_t>(value));
	if (!Loader::GuestCallBoundary::CurrentDepth()) { return 1; } // Host coordinator is not disposable.
	const auto failure = execution->FailureFunction();
	if (!failure) { return reinterpret_cast<uintptr_t>(&InvalidGuestExit); }
	if (execution->HasHostCallForCurrentBoundary()) { return 1; }
	return reinterpret_cast<uintptr_t>(failure);
}

extern "C" void KYTY_SYSV_ABI KytyPthreadExitLegacy(void* value);
extern "C" __attribute__((naked, used)) void KYTY_SYSV_ABI KytyPthreadExitTransition(void*) {
	__asm__ volatile("pushq %rbp\n\t"
	                 "movq %rsp, %rbp\n\t"
	                 "andq $-16, %rsp\n\t"
	                 "subq $16, %rsp\n\t"
	                 "movq %rdi, 0(%rsp)\n\t"
	                 "call KytyPreparePthreadExit\n\t"
	                 "movq 0(%rsp), %rdi\n\t"
	                 "leave\n\t"
	                 "cmpq $1, %rax\n\t"
	                 "je 1f\n\t"
	                 "testq %rax, %rax\n\t"
	                 "jz KytyPthreadExitLegacy\n\t"
	                 "jmp *%rax\n\t"
	                 "1: ret\n\t");
}
__attribute__((naked)) void KYTY_SYSV_ABI PthreadExit(void*) {
	__asm__ volatile("jmp KytyPthreadExitTransition\n\t");
}
extern "C" void __attribute__((force_align_arg_pointer, used, noinline)) KYTY_SYSV_ABI KytyPthreadExitLegacy(void* value) {
#else
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadExit(void* value) {
#endif
	PRINT_NAME();

#if defined(__x86_64__) || defined(_M_X64)
	if (g_guest_entry_return_rsp != 0) {
		const auto return_rsp    = g_guest_entry_return_rsp;
		g_guest_entry_return_rsp = 0;

		if (g_pthread_self != nullptr && g_pthread_self->guest_host_rsp != 0) {
			const auto host_rbx = g_pthread_self->guest_host_rbx;
			const auto host_rsp = g_pthread_self->guest_host_rsp;
			const auto host_rbp = g_pthread_self->guest_host_rbp;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			const auto host_gs8  = g_pthread_self->guest_host_gs8;
			const auto host_gs10 = g_pthread_self->guest_host_gs10;
#endif

			asm volatile("movq %2, %%rbx\n\t"
			             "movq %3, %%r12\n\t"
			             "movq %4, %%r13\n\t"
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			             "movq %5, %%r14\n\t"
			             "movq %6, %%r15\n\t"
#endif
			             "movq %0, %%rax\n\t"
			             "movq %1, %%rsp\n\t"
			             "retq\n\t"
			             :
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			             : "m"(value), "m"(return_rsp), "m"(host_rbx), "m"(host_rsp), "m"(host_rbp),
			               "m"(host_gs8), "m"(host_gs10)
#else
			             : "m"(value), "m"(return_rsp), "m"(host_rbx), "m"(host_rsp), "m"(host_rbp)
#endif
			             : "rax", "memory");
			__builtin_unreachable();
		}
	}
#endif

	pthread_exit(value);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadEqual(Pthread thread1, Pthread thread2) {
	static std::atomic<uint32_t> log_count {0};

	const int result = (thread1 == thread2 ? 1 : 0);

	const auto count = log_count.fetch_add(1);
	if (count < 32 || (result != 0 && count < 128)) {
		LOGF("\tPthreadEqual: t1 = 0x%016" PRIx64 ", t2 = 0x%016" PRIx64 ", result = %d\n",
		     reinterpret_cast<uint64_t>(thread1), reinterpret_cast<uint64_t>(thread2), result);
	}

	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetname(Pthread thread, char* name) {
	PRINT_NAME();

	if (thread == nullptr || !IsValidPthread(thread)) {
		LOGF("ps5-vsh: pthread-getname(invalid thread %p)\n", static_cast<const void*>(thread));
		return KERNEL_ERROR_ESRCH;
	}

	if (name == nullptr) {
		LOGF("ps5-vsh: pthread-getname(null name buf) thread=%p\n",
		     static_cast<const void*>(thread));
		return KERNEL_ERROR_EFAULT;
	}

	// ps5-vsh: the camera thread path passes a non-Pthread pointer here
	// (guest-side id). Probe before touching thread->name; ESRCH on junk.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	{
		MEMORY_BASIC_INFORMATION mi {};
		if (VirtualQuery(thread, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT) {
			return KERNEL_ERROR_ESRCH;
		}
		__try {
			const char* n = thread->name.c_str();
			if (n == nullptr) {
				return KERNEL_ERROR_ESRCH;
			}
			MEMORY_BASIC_INFORMATION mi2 {};
			if (VirtualQuery(n, &mi2, sizeof(mi2)) == 0 || mi2.State != MEM_COMMIT) {
				return KERNEL_ERROR_ESRCH;
			}
			strncpy(name, n, 32);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return KERNEL_ERROR_ESRCH;
		}
		name[31] = '\0';
		return OK;
	}
#else
	strncpy(name, thread->name.c_str(), 32);
	name[31] = '\0';

	return OK;
#endif
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadRename(Pthread thread, const char* name) {
	PRINT_NAME();

	if (thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!IsValidPthread(thread)) {
		return KERNEL_ERROR_ESRCH;
	}

	if (name == nullptr) {
		return OK;
	}

	thread->name = std::string(name);

	// ps5-vsh: thread renames mark boot phases (MainThread renames itself per
	// subsystem init). Always loud; this is our phase tracker.
	LOGF("ps5-vsh: thread renamed to '%s'\n", thread->name.c_str());

	return OK;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadYield() {
	// ps5-vsh: spinner census (temporary). Yield is silent; a guest spin loop over
	// it burns a core invisibly. Count and report every 20M calls.
	static std::atomic<uint64_t> s_yield_calls {0};
	const uint64_t               n = s_yield_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: yield call #%llu\n", n);
	}
	SchedulerBackoffOnce();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetthreadid() {
	// PRINT_NAME();

	return g_pthread_self != nullptr ? g_pthread_self->guest.thread_id : 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp) {
	PRINT_NAME();

	if (tp == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (clock_id == KERNEL_CLOCK_PROCTIME || clock_id == KERNEL_CLOCK_THREAD_CPUTIME_ID ||
	    clock_id == KERNEL_CLOCK_VIRTUAL || clock_id == KERNEL_CLOCK_PROF) {
		const auto frequency = KernelGetTscFrequencyNative();
		tp->tv_sec           = 0;
		tp->tv_nsec          = static_cast<int64_t>(
		    frequency != 0 ? std::max<uint64_t>((1000000000 + (frequency >> 1)) / frequency, 1)
		                   : 1);
		return OK;
	}

	clockid_t pclock_id = {};
	if (!GetPosixClockId(clock_id, &pclock_id)) {
		LOGF("\tunknown clock_id: %d\n", clock_id);
		return KERNEL_ERROR_EINVAL;
	}

	timespec t {};

	int result = clock_getres(pclock_id, &t);

	tp->tv_sec  = t.tv_sec;
	tp->tv_nsec = t.tv_nsec;

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp) {
	// Called constantly by Python frame/timer code.

	if (tp == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	int special_error = OK;
	if (KernelClockGettimeSpecial(clock_id, tp, &special_error)) {
		return special_error;
	}

	clockid_t pclock_id = {};
	if (!GetPosixClockId(clock_id, &pclock_id)) {
		LOGF("\tunknown clock_id: %d\n", clock_id);
		return KERNEL_ERROR_EINVAL;
	}

	timespec t {};

	int result = clock_gettime(pclock_id, &t);

	tp->tv_sec  = t.tv_sec;
	tp->tv_nsec = t.tv_nsec;

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGettimeofday(KernelTimeval* tp) {
	// PRINT_NAME();
	// ps5-vsh: spinner census (temporary). Same throttled counting as yield.
	static std::atomic<uint64_t> s_gtod_calls {0};
	const uint64_t               n = s_gtod_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: gettimeofday call #%llu\n", n);
	}

	if (tp == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	int result = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FILETIME ft {};
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t ticks = ft.dwHighDateTime;
	ticks <<= 32;
	ticks |= ft.dwLowDateTime;
	ticks /= 10;
	ticks -= 11644473600000000ULL;
	tp->tv_sec  = static_cast<int64_t>(ticks / 1000000);
	tp->tv_usec = static_cast<int64_t>(ticks % 1000000);
#else
	struct timespec ts {};
	result = ::clock_gettime(CLOCK_REALTIME, &ts);
	if (result == 0) {
		tp->tv_sec  = static_cast<int64_t>(ts.tv_sec);
		tp->tv_usec = static_cast<int64_t>(ts.tv_nsec / 1000);
	}
#endif

	if (result == 0) {
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGettimezone(KernelTimezone* tz) {
	// Hot path in Unity's time/date code.

	if (tz == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	TIME_ZONE_INFORMATION tzi {};
	const DWORD           result = GetTimeZoneInformation(&tzi);

	tz->tz_minuteswest = tzi.Bias;
	tz->tz_dsttime     = (result == TIME_ZONE_ID_UNKNOWN ? DST_NONE : DST_MET);
#else
	const std::time_t now = std::time(nullptr);
	std::tm           local_tm {};
	std::tm           utc_tm {};
	localtime_r(&now, &local_tm);
	gmtime_r(&now, &utc_tm);
	const auto local = std::mktime(&local_tm);
	const auto utc   = std::mktime(&utc_tm);

	tz->tz_minuteswest = static_cast<int32_t>((utc - local) / 60);
	tz->tz_dsttime     = (local_tm.tm_isdst > 0 ? DST_MET : DST_NONE);
#endif

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelConvertLocaltimeToUtc(int64_t  local_time, int64_t /*reserved*/,
                                              int64_t* utc_time, KernelTimezone* timezone,
                                              int32_t* dst_seconds) {
	// Hot path in Unity's time/date code.

	if (timezone == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	KernelTimezone local_timezone {};
	auto           ret = KernelGettimezone(&local_timezone);
	if (ret != OK) {
		return ret;
	}

	const auto dst_sec = GetDstSeconds();
	*timezone          = local_timezone;

	if (utc_time != nullptr) {
		*utc_time = local_time + static_cast<int64_t>(local_timezone.tz_minuteswest) * 60 - dst_sec;
	}
	if (dst_seconds != nullptr) {
		*dst_seconds = dst_sec;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelConvertUtcToLocaltime(int64_t utc_time, int64_t* local_time,
                                              KernelTimesec* st, uint64_t* dst_sec) {
	// Hot path in Unity's time/date code.

	KernelTimezone tz {};
	auto           ret = KernelGettimezone(&tz);
	if (ret != OK) {
		return ret;
	}

	const auto dst  = static_cast<int64_t>(GetDstSeconds());
	const auto west = static_cast<int64_t>(tz.tz_minuteswest) * 60;
	if (local_time != nullptr) {
		*local_time = utc_time - west + dst;
	}
	if (st != nullptr) {
		st->t        = utc_time;
		st->west_sec = static_cast<uint32_t>(-west);
		st->dst_sec  = static_cast<uint32_t>(dst);
	}
	if (dst_sec != nullptr) {
		*dst_sec = static_cast<uint64_t>(dst);
	}

	return OK;
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetTscFrequency() {
	return KernelGetTscFrequencyNative();
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelReadTsc() {
	return KernelReadTscNative();
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetProcessTime() {
	const auto frequency = KernelGetTscFrequencyNative();
	if (frequency == 0) {
		return static_cast<uint64_t>(Loader::Timer::GetTimeMs() * 1000.0);
	}

	const auto elapsed = KernelGetElapsedTsc();
	return static_cast<uint64_t>((static_cast<long double>(elapsed) * 1000000.0L) /
	                             static_cast<long double>(frequency));
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetProcessTimeCounter() {
	return KernelGetElapsedTsc();
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency() {
	return KernelGetTscFrequencyNative();
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors) {
	PRINT_NAME();
	if (auto execution = Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram())) {
		if (!execution->RegisterThreadDestructor(reinterpret_cast<uint64_t>(dtors))) {
			execution->Fail(Loader::GuestFailureReason::InvalidTransition);
		}
		return;
	}

	// 13.60 libkernel rNhWz+lvOMU (RVA 0x7890) unconditionally stores
	// the process callback, including replacements and nullptr. Firmware libc
	// can register before an application's bundled libc during native startup.
	g_pthread_context->SetThreadDtors(dtors);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelUsleep(KernelUseconds microseconds) {
	NoteHleCall(g_library, g_module, __func__);
	// ps5-vsh: spinner census (temporary). Fully silent; usleep(0) poll loops burn
	// a core invisibly. Count and report.
	static std::atomic<uint64_t> s_usleep_calls {0};
	const uint64_t               n = s_usleep_calls.fetch_add(1, std::memory_order_relaxed);
	if ((n % 20000000ull) == 0) {
		LOGF("ps5-vsh: usleep call #%llu (%u us)\n", n, microseconds);
	}
	SleepMicroWithSignalPoll(microseconds);
	return OK;
}

unsigned int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSleep(unsigned int seconds) {
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("sleep");
	SleepMicroWithSignalPoll(static_cast<uint64_t>(seconds) * 1000000ull);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp) {
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("sleep");
	if (rqtp == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000) {
		return KERNEL_ERROR_EINVAL;
	}

	if (static_cast<uint64_t>(rqtp->tv_sec) >
	    (UINT64_MAX - static_cast<uint64_t>(rqtp->tv_nsec)) / 1000000000ull) {
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t nanos =
	    static_cast<uint64_t>(rqtp->tv_sec) * 1000000000ull + static_cast<uint64_t>(rqtp->tv_nsec);

	SleepNanoWithSignalPoll(nanos);

	if (rmtp != nullptr) {
		rmtp->tv_sec  = 0;
		rmtp->tv_nsec = 0;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor) {
	if (key == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto execution = Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram());
	auto lease = execution ? execution->TryAcquire(Loader::GuestWorkKind::Callback) : Loader::RuntimeGuestExecution::Lease {};
	if (execution && !lease) { return KERNEL_ERROR_ECANCELED; }
	std::lock_guard lock(g_pthread_keys_mutex);
	for (int index = 0; index < KEYS_MAX; index++) {
		auto&      entry      = g_pthread_keys[index];
		const auto generation = entry.generation.load(std::memory_order_relaxed);
		if ((generation & 1u) == 0) {
			const auto publish = [&] {
				entry.destructor = destructor;
				entry.execution = execution;
				entry.generation.store(generation + 1, std::memory_order_release);
				*key = index;
			};
			if (execution) { if (!lease.Publish(publish)) { return KERNEL_ERROR_ECANCELED; } }
			else { publish(); }
			return OK;
		}
	}
	return KERNEL_ERROR_EAGAIN;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadKeyDelete(PthreadKey key) {
	if (key < 0 || key >= KEYS_MAX) {
		return KERNEL_ERROR_EINVAL;
	}

	std::lock_guard lock(g_pthread_keys_mutex);
	auto&           entry      = g_pthread_keys[key];
	const auto      generation = entry.generation.load(std::memory_order_relaxed);
	if ((generation & 1u) == 0) {
		return KERNEL_ERROR_EINVAL;
	}
	entry.generation.store(generation + 1, std::memory_order_release);
	entry.destructor = nullptr;
	entry.execution.reset();
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSetspecific(PthreadKey key, void* value) {
	if (key < 0 || key >= KEYS_MAX) {
		return KERNEL_ERROR_EINVAL;
	}
	const auto generation = g_pthread_keys[key].generation.load(std::memory_order_acquire);
	if ((generation & 1u) == 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (g_pthread_specific == nullptr) {
		if (value == nullptr) {
			return OK;
		}
		g_pthread_specific.reset(new (std::nothrow) PthreadSpecificValue[KEYS_MAX] {});
		if (g_pthread_specific == nullptr) {
			return KERNEL_ERROR_ENOMEM;
		}
	}
	g_pthread_specific[key] = {generation, value};
	return OK;
}


void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key) {
	// ps5-vsh chainSUI197: TEMP-DIAG file side-logs removed (fopen-per-call
	// crashed inside ucrt). LOGF-only now.
	if (key < 0 || key >= KEYS_MAX || g_pthread_specific == nullptr) {
		// ps5-vsh chainSUI173 TEMP-DIAG: silent-null path triage.
		if (key == 2 || key == 4) {
			static std::atomic<uint64_t> s_specnull {0};
			if (s_specnull.fetch_add(1, std::memory_order_relaxed) < 60) {
				auto* self = LibKernel::PthreadSelfOrNull();
				uint64_t hostid = 0;
				#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				hostid = static_cast<uint64_t>(GetCurrentThreadId());
				#endif
				char tn[64] = "?";
				if (self != nullptr) {
					if (LibKernel::PthreadGetname(self, tn) != OK) {
						tn[0] = 0;
					}
				}
				LOGF("ps5-vsh: getspecific NULLTABLE thread=%s(%p) host=%llu key=%d table=%p\n",
				     tn, static_cast<const void*>(self),
				     static_cast<unsigned long long>(hostid),
				     static_cast<int>(key),
				     static_cast<const void*>(g_pthread_specific.get()));
			}
		}
		return nullptr;
	}
	const auto  generation = g_pthread_keys[key].generation.load(std::memory_order_acquire);
	const auto& value      = g_pthread_specific[key];
	if ((generation & 1u) != 0 && generation == value.generation) {

		return value.data;
	}
	// ps5-vsh chainSUI171: log misses for keys 2/4 only (hits are hot).
	if (key == 2 || key == 4) {
		static std::atomic<uint64_t> s_specmiss {0};
		if (s_specmiss.fetch_add(1, std::memory_order_relaxed) < 60) {
			char tn[64] = "?";
			if (auto* self = LibKernel::PthreadSelfOrNull(); self != nullptr) {
				if (LibKernel::PthreadGetname(self, tn) != OK) {
					tn[0] = 0;
				}
			}
			LOGF("ps5-vsh: getspecific MISS thread=%s key=%d geng=%llu stored=%llu\n",
			     tn, static_cast<int>(key),
			     static_cast<unsigned long long>(generation),
			     static_cast<unsigned long long>(value.generation));
		}
	}
	return nullptr;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include "../../tests/RuntimeGuestThreadTests.inc"
#include "../../tests/RuntimeGuestWaitTests.inc"
#include "../../tests/RuntimeCrtConditionTests.inc"
#include "../../tests/RuntimeRwlockLifecycleTests.inc"
#include "../../tests/RuntimeGuestStopTests.inc"
#include "../../tests/RuntimeNativeTargetPinTests.inc"
#include "../../tests/RuntimeGuestMutexStopTests.inc"
#include "../../tests/RuntimeNativeDrainTests.inc"
#include "../../tests/PthreadJoinRetentionTests.inc"
#include "../../tests/PthreadNativeReceiptTests.inc"
#endif

} // namespace LibKernel

namespace Posix {

LIB_NAME("Posix", "libkernel");

namespace {
// All guest slot access and barrier state transitions use this lock. Handles
// are never-dereferenced, never-reused tokens, so stale copies cannot acquire a
// newly allocated object at the same host address. A wait pins its object even
// after destroy removes the token and clears the original guest slot.
struct BarrierState {
	std::condition_variable cv;
	unsigned count;
	unsigned arrivals = 0;
	uint64_t generation = 0;
	explicit BarrierState(unsigned n): count(n) {}
};
std::mutex g_barrier_mutex;
std::unordered_map<LibKernel::PthreadBarrier, std::shared_ptr<BarrierState>> g_barriers;
uintptr_t g_next_barrier_token = 0x10000;

// A barrier is not a POSIX cancellation point. In particular, the host CV
// must not cancel out of a C++ frame holding the registry/pin on a guest stack.
// Restore only after all locks, critical guards and lifetime pins are gone.
struct BarrierCancelGuard {
	int previous = PTHREAD_CANCEL_DISABLE;
	BarrierCancelGuard() { ::pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous); }
	~BarrierCancelGuard() { ::pthread_setcancelstate(previous, nullptr); }
};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
thread_local bool g_barrier_fail_allocation = false;
std::atomic<void (*)()> g_barrier_release_callback {nullptr};
#endif
} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_barrier_init(
    LibKernel::PthreadBarrier* barrier, const LibKernel::PthreadBarrierattr* attr, unsigned count) {
	(void)attr; // Verified 13.60 body ignores attr, including non-null pointers.
	if (barrier == nullptr || count == 0) {
		return POSIX_EINVAL;
	}
	BarrierCancelGuard cancellation;
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_barrier_mutex);
	try {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (std::exchange(g_barrier_fail_allocation, false)) {
			return POSIX_ENOMEM;
		}
#endif
		if (g_next_barrier_token == UINTPTR_MAX) {
			return POSIX_ENOMEM; // Do not wrap tokens and introduce handle ABA.
		}
		auto state = std::make_shared<BarrierState>(count);
		auto token = reinterpret_cast<LibKernel::PthreadBarrier>(g_next_barrier_token);
		g_barriers.emplace(token, std::move(state));
		++g_next_barrier_token;
		*barrier = token; // Publish only after every allocation succeeds.
		return 0;
	} catch (const std::bad_alloc&) {
		return POSIX_ENOMEM;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_barrier_destroy(LibKernel::PthreadBarrier* barrier) {
	if (barrier == nullptr) {
		return POSIX_EINVAL;
	}
	BarrierCancelGuard cancellation;
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_barrier_mutex);
	const auto it = g_barriers.find(*barrier);
	if (it == g_barriers.end()) {
		return POSIX_EINVAL;
	}
	if (it->second->arrivals != 0) {
		return POSIX_EBUSY;
	}
	*barrier = nullptr;
	g_barriers.erase(it); // Released waiters own independent lifetime pins.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_barrier_wait(LibKernel::PthreadBarrier* barrier) {
	if (barrier == nullptr) {
		return POSIX_EINVAL;
	}
	BarrierCancelGuard cancellation;
	std::shared_ptr<BarrierState> state;
	uint64_t generation = 0;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_barrier_mutex);
		const auto it = g_barriers.find(*barrier);
		if (it == g_barriers.end()) {
			return POSIX_EINVAL;
		}
		state = it->second;
		generation = state->generation;
		if (++state->arrivals == state->count) {
			state->arrivals = 0;
			++state->generation;
			state->cv.notify_all();
			state.reset();
			return -1; // Serial-thread result, NOT POSIX_PTHREAD_CALL or errno.
		}
	}
	for (;;) {
		bool released = false;
		{
			// Cover lock acquisition AND CV re-acquisition. The guard spans only
			// one bounded sleep; dispatch guest signals outside it and the lock.
			Common::HostLockCriticalGuard critical;
			std::unique_lock lock(g_barrier_mutex);
			if (generation == state->generation) {
				state->cv.wait_for(lock, std::chrono::microseconds(LibKernel::SIGNAL_APC_POLL_MICROS));
			}
			released = generation != state->generation;
		}
		if (released) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			if (auto callback = g_barrier_release_callback.load(std::memory_order_acquire)) {
				callback();
			}
#endif
			Common::HostLockCriticalGuard critical;
			state.reset(); // Last pin may destroy the host CV after guest destroy.
			return 0;
		}
		LibKernel::KernelDispatchPendingSignalForCurrentThread();
	}
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestPthreadBarrierArrivals(LibKernel::PthreadBarrier* barrier, unsigned* arrivals) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_barrier_mutex);
	const auto it = g_barriers.find(*barrier);
	if (it == g_barriers.end()) {
		return false;
	}
	*arrivals = it->second->arrivals;
	return true;
}
void TestPthreadBarrierFailNextAllocation() { g_barrier_fail_allocation = true; }
void TestPthreadBarrierSetReleaseCallback(void (*callback)()) {
	g_barrier_release_callback.store(callback, std::memory_order_release);
}
#endif

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setscope(LibKernel::PthreadAttr* attr, int scope) {
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetscope(attr, scope));
}

struct PthreadOnce {
	int                     state;
	LibKernel::PthreadMutex mutex;
};

static_assert(sizeof(PthreadOnce) == 0x10);

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_once(void* once_control, void(KYTY_SYSV_ABI* init_routine)()) {
	PRINT_NAME();
	if (once_control == nullptr || init_routine == nullptr) {
		return POSIX_EINVAL;
	}
	return Loader::RuntimeCrt::Once(once_control, reinterpret_cast<uint64_t>(init_routine),
	    Loader::RuntimeCrt::SyncKind::PthreadOnce, nullptr, LibKernel::KernelDispatchPendingSignalForCurrentThread);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr,
                                 LibKernel::pthread_entry_func_t entry, void* arg) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCreate(thread, attr, entry, arg, ""));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_create_name_np(LibKernel::Pthread*             thread,
                                         const LibKernel::PthreadAttr*   attr,
                                         LibKernel::pthread_entry_func_t entry, void* arg,
                                         const char* name) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(
	    LibKernel::PthreadCreate(thread, attr, entry, arg, (name != nullptr ? name : "")));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_detach(LibKernel::Pthread thread) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadDetach(thread));
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
__attribute__((naked)) void KYTY_SYSV_ABI pthread_exit(void*) {
	__asm__ volatile("jmp KytyPthreadExitTransition\n\t");
}
#else
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_exit(void* value) {
	PRINT_NAME();

	LibKernel::PthreadExit(value);
}
#endif

LibKernel::Pthread __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_self() {
	PRINT_NAME();

	return LibKernel::PthreadSelf();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rename_np(LibKernel::Pthread thread, const char* name) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRename(thread, name));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_getname_np(LibKernel::Pthread thread, char* name) {
	PRINT_NAME();
	// BSD's two-argument export uses the same name buffer as scePthreadGetname,
	// but returns a positive POSIX pthread error rather than an encoded kernel error.
	return POSIX_PTHREAD_CALL(LibKernel::PthreadGetname(thread, name));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_setcancelstate(int state, int* old_state) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetcancelstate(state, old_state));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_setprio(LibKernel::Pthread thread, int prio) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetprio(thread, prio));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_getschedparam(LibKernel::Pthread thread, int* policy,
                                        LibKernel::KernelSchedParam* param) {
	PRINT_NAME();

	if (thread == nullptr || policy == nullptr || param == nullptr) {
		return POSIX_EINVAL;
	}

	int result = LibKernel::PthreadAttrGetschedpolicy(&thread->attr, policy);
	result = (result == OK ? LibKernel::PthreadAttrGetschedparam(&thread->attr, param) : result);

	return (result == OK ? 0 : LibKernel::KernelToPosix(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_setschedparam(LibKernel::Pthread thread, int policy,
                                        const LibKernel::KernelSchedParam* param) {
	PRINT_NAME();

	if (thread == nullptr || param == nullptr) {
		return POSIX_EINVAL;
	}

	LOGF("\t pthread_setschedparam: thread=%llx policy=%d prio=%d\n",
	     (unsigned long long)thread, policy, param->sched_priority);

	int result = LibKernel::PthreadAttrSetschedpolicy(&thread->attr, policy);
	result = (result == OK ? LibKernel::PthreadAttrSetschedparam(&thread->attr, param) : result);

	return (result == OK ? 0 : LibKernel::KernelToPosix(result));
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_yield() {
	PRINT_NAME();

	LibKernel::PthreadYield();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sched_get_priority_max(int policy) {
	PRINT_NAME();

	LOGF("\t policy = %d\n", policy);

	return 767;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sched_get_priority_min(int policy) {
	PRINT_NAME();

	LOGF("\t policy = %d\n", policy);

	return 256;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_join(LibKernel::Pthread thread, void** value) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadJoin(thread, value));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_kill(LibKernel::Pthread thread, int sig) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKill(thread, sig));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_init(LibKernel::PthreadAttr* attr) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrInit(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_destroy(LibKernel::PthreadAttr* attr) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrDestroy(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_get_np(LibKernel::Pthread thread, LibKernel::PthreadAttr* attr) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGet(thread, attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getdetachstate(const LibKernel::PthreadAttr* attr, int* state) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetdetachstate(attr, state));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getguardsize(const LibKernel::PthreadAttr* attr,
                                            size_t*                       guard_size) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetguardsize(attr, guard_size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getinheritsched(const LibKernel::PthreadAttr* attr,
                                               int*                          inherit_sched) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetinheritsched(attr, inherit_sched));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getschedparam(const LibKernel::PthreadAttr* attr,
                                             LibKernel::KernelSchedParam*  param) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetschedparam(attr, param));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getschedpolicy(const LibKernel::PthreadAttr* attr, int* policy) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetschedpolicy(attr, policy));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                        void** __restrict stack_addr,
                                        size_t* __restrict stack_size) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetstack(attr, stack_addr, stack_size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_getstacksize(const LibKernel::PthreadAttr* attr,
                                            size_t*                       stack_size) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetstacksize(attr, stack_size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setdetachstate(LibKernel::PthreadAttr* attr, int state) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetdetachstate(attr, state));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setguardsize(LibKernel::PthreadAttr* attr, size_t guard_size) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetguardsize(attr, guard_size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setinheritsched(LibKernel::PthreadAttr* attr, int inherit_sched) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetinheritsched(attr, inherit_sched));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setschedparam(LibKernel::PthreadAttr*            attr,
                                             const LibKernel::KernelSchedParam* param) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetschedparam(attr, param));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setschedpolicy(LibKernel::PthreadAttr* attr, int policy) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetschedpolicy(attr, policy));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setstack(LibKernel::PthreadAttr* attr, void* addr, size_t size) {
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetstack(attr, addr, size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetstacksize(attr, stack_size));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_broadcast(LibKernel::PthreadCond* cond) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondBroadcast(cond));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_signal(LibKernel::PthreadCond* cond) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondSignal(cond));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_init(LibKernel::PthreadCond*           cond,
                                    const LibKernel::PthreadCondattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondInit(cond, attr, nullptr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_condattr_setclock(LibKernel::PthreadCondattr* attr,
                                            LibKernel::KernelClockid    clock_id) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrSetclock(attr, clock_id));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_condattr_init(LibKernel::PthreadCondattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrInit(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_condattr_destroy(LibKernel::PthreadCondattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrDestroy(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondWait(cond, mutex));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_timedwait(LibKernel::PthreadCond*          cond,
                                         LibKernel::PthreadMutex*         mutex,
                                         const LibKernel::KernelTimespec* abstime) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondTimedwaitAbs(cond, mutex, abstime));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_cond_reltimedwait_np(
    LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex, LibKernel::KernelUseconds usec) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondTimedwait(cond, mutex, usec));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_lock(LibKernel::PthreadMutex* mutex) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexLock(mutex));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_trylock(LibKernel::PthreadMutex* mutex) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexTrylock(mutex));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_timedlock(LibKernel::PthreadMutex*         mutex,
                                          const LibKernel::KernelTimespec* abstime) {
	PRINT_NAME();

	if (abstime == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000) {
		return POSIX_PTHREAD_CALL(LibKernel::KERNEL_ERROR_EINVAL);
	}

	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettime(0, &now) != OK) {
		return POSIX_PTHREAD_CALL(LibKernel::KERNEL_ERROR_EINVAL);
	}

	int64_t sec_delta  = abstime->tv_sec - now.tv_sec;
	int64_t nsec_delta = abstime->tv_nsec - now.tv_nsec;
	if (nsec_delta < 0) {
		sec_delta--;
		nsec_delta += 1000000000;
	}

	uint64_t usec = 0;
	if (sec_delta > 0 || (sec_delta == 0 && nsec_delta > 0)) {
		usec = (static_cast<uint64_t>(sec_delta) > UINT32_MAX / 1000000ull
		            ? UINT32_MAX
		            : static_cast<uint64_t>(sec_delta) * 1000000ull);
		usec = std::min<uint64_t>(UINT32_MAX, usec + static_cast<uint64_t>(nsec_delta / 1000));
	}

	return POSIX_PTHREAD_CALL(
	    LibKernel::PthreadMutexTimedlock(mutex, static_cast<LibKernel::KernelUseconds>(usec)));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_unlock(LibKernel::PthreadMutex* mutex) {
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexUnlock(mutex));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock) {
	// Hot path for some PS5 titles; per-call name logging can dominate runtime.

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockRdlock(rwlock));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock) {
	// Hot path for some PS5 titles; per-call name logging can dominate runtime.

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockUnlock(rwlock));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock) {
	// Hot path for some PS5 titles; per-call name logging can dominate runtime.

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockWrlock(rwlock));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockDestroy(rwlock));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_rwlock_init(LibKernel::PthreadRwlock*           rwlock,
                                      const LibKernel::PthreadRwlockattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockInit(rwlock, attr, nullptr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_key_create(LibKernel::PthreadKey*                   key,
                                     LibKernel::pthread_key_destructor_func_t destructor) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyCreate(key, destructor));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_key_delete(LibKernel::PthreadKey key) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyDelete(key));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_setspecific(LibKernel::PthreadKey key, void* value) {
	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetspecific(key, value));
}

void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key) {
	return LibKernel::PthreadGetspecific(key);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_destroy(LibKernel::PthreadMutex* mutex) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexDestroy(mutex));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_init(LibKernel::PthreadMutex*           mutex,
                                     const LibKernel::PthreadMutexattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexInit(mutex, attr, nullptr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrInit(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSettype(attr, type));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSetprotocol(attr, protocol));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr) {
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrDestroy(attr));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_mutex_setname_np(LibKernel::PthreadMutex* mutex, const char* name) {
	// ps5-vsh: nTxZBp8YNGc (voldemort parity). Names a mutex; Mono names
	// its loader lock "__loader". Must NOT touch the mutex value (a prior
	// mis-map to a mutex factory corrupted named mutexes -> EDEADLK abort).
	if (mutex != nullptr && *mutex != nullptr) {
		(*mutex)->name = (name != nullptr ? name : "");
	}
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pthread_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                   void** __restrict stack_addr, size_t* __restrict stack_size) {
	PRINT_NAME();

	return pthread_attr_getstack(attr, stack_addr, stack_size);
}

} // namespace Posix

} // namespace Libs

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic pop
#endif
