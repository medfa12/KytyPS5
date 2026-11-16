#include "kernel/semaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <intrin.h>
#endif

#include "kernel/semaphoreNative.inc"

namespace Libs::Posix {

LIB_NAME("Posix", "libkernel");

namespace {

constexpr uint16_t POSIX_SEM_MAGIC        = 0x09fa;
constexpr int      POSIX_SEM_VALUE_MAX    = 0x7fffffff;
constexpr uint32_t SIGNAL_APC_POLL_MICROS = 10000;

struct PosixSemGuest {
	uint16_t          magic;
	uint16_t          nameid;
	volatile uint32_t has_waiters;
	volatile uint32_t count;
	uint32_t          flags;
};

static_assert(sizeof(PosixSemGuest) == 16);

class PosixSemPrivate {
public:
	PosixSemPrivate(PosixSemGuest* guest, unsigned int value, uint32_t flags)
	    : m_guest(guest), m_count(static_cast<int>(value)) {
		SyncGuest(flags);
	}

	int Wait(uint32_t* micros) {
		Common::LockGuard lock(m_mutex);

		uint32_t      elapsed    = 0;
		const bool    timed_wait = (micros != nullptr);
		Common::Timer timer;
		timer.Start();

		while (m_count <= 0) {
			if (timed_wait && elapsed >= *micros) {
				SyncGuest();
				return POSIX_ETIMEDOUT;
			}

			m_waiters++;
			SyncGuest();

			if (timed_wait) {
				m_cond_var.WaitFor(&m_mutex, *micros - elapsed);
			} else {
				m_cond_var.WaitFor(&m_mutex, SIGNAL_APC_POLL_MICROS);
			}

			m_mutex.Unlock();
			LibKernel::KernelDispatchPendingSignalForCurrentThread();
			m_mutex.Lock();

			m_waiters--;
			elapsed = static_cast<uint32_t>(timer.GetTimeS() * 1000000.0);
		}

		m_count--;
		SyncGuest();

		return OK;
	}

	int TryWait() {
		Common::LockGuard lock(m_mutex);

		if (m_count <= 0) {
			SyncGuest();
			return POSIX_EAGAIN;
		}

		m_count--;
		SyncGuest();

		return OK;
	}

	int Post() {
		Common::LockGuard lock(m_mutex);

		if (m_count == POSIX_SEM_VALUE_MAX) {
			return POSIX_EOVERFLOW;
		}

		m_count++;
		SyncGuest();
		m_cond_var.Signal();

		return OK;
	}

	int GetValue() {
		Common::LockGuard lock(m_mutex);

		return m_count;
	}

private:
	void SyncGuest(uint32_t flags = std::numeric_limits<uint32_t>::max()) {
		if (m_guest != nullptr) {
			m_guest->magic       = POSIX_SEM_MAGIC;
			m_guest->nameid      = 0;
			m_guest->has_waiters = (m_waiters > 0 ? 1u : 0u);
			m_guest->count       = static_cast<uint32_t>(m_count);
			if (flags != std::numeric_limits<uint32_t>::max()) {
				m_guest->flags = flags;
			}
		}
	}

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	PosixSemGuest*  m_guest   = nullptr;
	int             m_count   = 0;
	uint32_t        m_waiters = 0;
};

Common::Mutex                                        g_posix_sem_mutex;
std::unordered_map<PosixSemGuest*, PosixSemPrivate*> g_posix_sems;

static int PosixToKernel(int posix_errno) {
	return (posix_errno >= 0 && posix_errno <= 102 ? posix_errno - 2147352576
	                                               : LibKernel::KERNEL_ERROR_EINVAL);
}

static PosixSemPrivate* GetSem(void* sem) {
	if (sem == nullptr) {
		return nullptr;
	}

	Common::LockGuard lock(g_posix_sem_mutex);

	const auto it = g_posix_sems.find(static_cast<PosixSemGuest*>(sem));
	return (it != g_posix_sems.end() ? it->second : nullptr);
}

static int SetErrnoReturn(int posix_errno) {
	*GetErrorAddr() = posix_errno;
	return -1;
}

static int SemInitImpl(void* sem, int pshared, unsigned int value, const char* name) {
	if (sem == nullptr || value > POSIX_SEM_VALUE_MAX) {
		return POSIX_EINVAL;
	}

	auto* guest = static_cast<PosixSemGuest*>(sem);
	auto* obj   = new PosixSemPrivate(guest, value, static_cast<uint32_t>(pshared));

	{
		Common::LockGuard lock(g_posix_sem_mutex);

		if (auto it = g_posix_sems.find(guest); it != g_posix_sems.end()) {
			delete it->second;
			it->second = obj;
		} else {
			g_posix_sems.insert({guest, obj});
		}
	}

	static std::atomic<uint32_t> log_count {0};
	const auto                   count = log_count.fetch_add(1);
	if (count < 32) {
		LOGF("\t POSIX semaphore init: sem=%p %s, value = %u, pshared = %d\n", sem,
		     (name != nullptr ? name : "sem"), value, pshared);
	}

	return OK;
}

static int SemDestroyImpl(void* sem) {
	if (sem == nullptr) {
		return POSIX_EINVAL;
	}

	auto*            guest = static_cast<PosixSemGuest*>(sem);
	PosixSemPrivate* obj   = nullptr;

	{
		Common::LockGuard lock(g_posix_sem_mutex);

		const auto it = g_posix_sems.find(guest);
		if (it == g_posix_sems.end()) {
			return POSIX_EINVAL;
		}

		obj = it->second;
		g_posix_sems.erase(it);
	}

	delete obj;
	guest->magic       = 0;
	guest->has_waiters = 0;
	guest->count       = 0;
	guest->flags       = 0;

	return OK;
}

static int SemTimedwaitImpl(void* sem, uint32_t* micros) {
	Libs::NoteHleCall("libkernel", "libkernel", "SemTimedwaitImpl");
	HLE_SLOW_GUARD("posix-sem");
	// ps5-vsh: posix-sem waiter census (diagnostic). Central entry for
// sem_wait/reltimedwait/timedwait + PthreadSem* wrappers. First + every
// 64th entry logs waiter thread name + guest sem addr + timeout so
// parked threads (silent host waits) become attributable. Entry-only;
// no behavior change.
	static std::atomic<uint64_t> s_psem_wait_probe {0};
	const uint64_t               psem_probe_n =
	    s_psem_wait_probe.fetch_add(1, std::memory_order_relaxed);
	if (psem_probe_n == 0 || (psem_probe_n % 64ull) == 0) {
		char waiter[64] = "?";
		if (auto* self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
			if (Libs::LibKernel::PthreadGetname(self, waiter) != OK) {
				waiter[0] = '?';
				waiter[1] = '\0';
			}
		}
		if (micros == nullptr) {
			LOGF("ps5-vsh: posix-sem-wait #%llu thread=%s sem=%p timo=inf\n",
			     psem_probe_n, waiter, sem);
		} else {
			LOGF("ps5-vsh: posix-sem-wait #%llu thread=%s sem=%p timo=%u\n",
			     psem_probe_n, waiter, sem, *micros);
		}
	}

	const auto tid = Common::Thread::GetThreadIdUnique();
	if (tid == 6) {
		static std::atomic<uint64_t> s_t6_sem_count {0};
		const uint64_t n = s_t6_sem_count.fetch_add(1, std::memory_order_relaxed);
		if (n < 50 || (n % 1000 == 0)) {
			void* ra = __builtin_return_address(0);
			LOGF("ps5-vsh: t6 SemTimedwaitImpl ENTER #%llu ra=%p sem=%p timo=%u\n",
			     (unsigned long long)n, ra, sem, (micros ? *micros : 9999999));
		}
	}

	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		LOGF("ps5-vsh: posix-sem-wait MISS sem=%p (never init)\n", sem);
		return POSIX_EINVAL;
	}

	const int result = obj->Wait(micros);
	if (tid == 6) {
		static std::atomic<uint64_t> s_t6_sem_exit {0};
		const uint64_t n = s_t6_sem_exit.fetch_add(1, std::memory_order_relaxed);
		if (n < 50 || (n % 1000 == 0)) {
			LOGF("ps5-vsh: t6 SemTimedwaitImpl EXIT #%llu ret=%d\n", (unsigned long long)n, result);
		}
	}
	return result;
}

static bool AbsTimespecToMicros(const LibKernel::KernelTimespec* abstime, uint32_t* micros) {
	if (abstime == nullptr || micros == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000) {
		return false;
	}

	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettime(0, &now) != OK) {
		return false;
	}

	const int64_t sec_delta  = abstime->tv_sec - now.tv_sec;
	const int64_t nsec_delta = abstime->tv_nsec - now.tv_nsec;
	const int64_t usec_delta = sec_delta * 1000000 + nsec_delta / 1000;

	if (usec_delta <= 0) {
		*micros = 0;
	} else if (usec_delta > std::numeric_limits<uint32_t>::max()) {
		*micros = std::numeric_limits<uint32_t>::max();
	} else {
		*micros = static_cast<uint32_t>(usec_delta);
	}

	return true;
}

} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_init(void* sem, int pshared, unsigned int value) {
	PRINT_NAME();

	const int result = SemInitImpl(sem, pshared, value, nullptr);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_destroy(void* sem) {
	PRINT_NAME();

	const int result = SemDestroyImpl(sem);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_wait(void* sem) {
	const int result = SemTimedwaitImpl(sem, nullptr);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_trywait(void* sem) {
	if (Common::Thread::GetThreadIdUnique() == 6) {
		static std::atomic<uint64_t> s_t6_trywait {0};
		const uint64_t n = s_t6_trywait.fetch_add(1, std::memory_order_relaxed);
		if (n < 20 || (n % 1000 == 0)) {
			LOGF("ps5-vsh: t6 sem_trywait #%llu ra=%p sem=%p\n",
			     (unsigned long long)n, __builtin_return_address(0), sem);
		}
	}
	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		LOGF("ps5-vsh: posix-sem-trywait MISS sem=%p\n", sem);
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = obj->TryWait();
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, uint32_t usec) {
	const int result = SemTimedwaitImpl(sem, &usec);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_timedwait(void* sem, const LibKernel::KernelTimespec* abstime) {
	uint32_t micros = 0;
	if (!AbsTimespecToMicros(abstime, &micros)) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = SemTimedwaitImpl(sem, &micros);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_post(void* sem) {
	if (Common::Thread::GetThreadIdUnique() == 6) {
		static std::atomic<uint64_t> s_t6_post {0};
		const uint64_t n = s_t6_post.fetch_add(1, std::memory_order_relaxed);
		if (n < 20 || (n % 1000 == 0)) {
			LOGF("ps5-vsh: t6 sem_post #%llu ra=%p sem=%p\n",
			     (unsigned long long)n, __builtin_return_address(0), sem);
		}
	}
	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		LOGF("ps5-vsh: posix-sem-post MISS sem=%p\n", sem);
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = obj->Post();
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sem_getvalue(void* sem, int* value) {
	auto* obj = GetSem(sem);
	if (obj == nullptr || value == nullptr) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	*value = obj->GetValue();

	return 0;
}

} // namespace Libs::Posix

namespace Libs::LibKernel::Semaphore {

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemInit(void* sem, int flag, unsigned int value, const char* name) {
	PRINT_NAME();

	if (flag != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = Posix::SemInitImpl(sem, 0, value, name);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemDestroy(void* sem) {
	PRINT_NAME();

	const int result = Posix::SemDestroyImpl(sem);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemWait(void* sem) {
	const int result = Posix::SemTimedwaitImpl(sem, nullptr);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemTrywait(void* sem) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = obj->TryWait();
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemTimedwait(void* sem, KernelUseconds usec) {
	uint32_t  micros = usec;
	const int result = Posix::SemTimedwaitImpl(sem, &micros);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemPost(void* sem) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = obj->Post();
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSemGetvalue(void* sem, int* value) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr || value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*value = obj->GetValue();

	return OK;
}

} // namespace Libs::LibKernel::Semaphore
