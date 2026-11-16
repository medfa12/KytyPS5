#include "kernel/syncOnAddress.h"

#include "common/threads.h"
#include "libs/errno.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel::SyncOnAddress {

namespace {

constexpr uint32_t SIGNAL_POLL_MICROS = 10000;

using Clock = std::chrono::steady_clock;

template <typename T>
[[nodiscard]] bool IsValidWaitAddress(const volatile T* address) {
	return address != nullptr && (reinterpret_cast<uintptr_t>(address) & (alignof(T) - 1u)) == 0;
}

[[nodiscard]] bool IsValidWakeAddress(const volatile void* address) {
	return address != nullptr &&
	       (reinterpret_cast<uintptr_t>(address) & (alignof(uint32_t) - 1u)) == 0;
}

template <typename T>
[[nodiscard]] T ReadWord(const volatile T* address) {
	return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

struct WaitDeadline {
	bool              finite = false;
	Clock::time_point end {};
};

[[nodiscard]] WaitDeadline MakeDeadline(std::chrono::nanoseconds timeout) {
	const auto now = Clock::now();
	const auto remaining = Clock::time_point::max() - now;
	return {true, timeout >= remaining ? Clock::time_point::max() : now + timeout};
}

[[nodiscard]] WaitDeadline MakeDeadline(const uint32_t* timeout_micros) {
	if (timeout_micros == nullptr) {
		return {};
	}
	return MakeDeadline(std::chrono::microseconds(*timeout_micros));
}

[[nodiscard]] uint32_t GetWaitSliceMicros(const WaitDeadline& deadline, bool first_wait) {
	if (!deadline.finite) {
		return SIGNAL_POLL_MICROS;
	}

	const auto now = Clock::now();
	if (now >= deadline.end) {
		return first_wait ? 0u : UINT32_MAX;
	}

	const auto remaining =
	    std::chrono::ceil<std::chrono::microseconds>(deadline.end - now).count();
	return static_cast<uint32_t>(std::min<int64_t>(remaining, SIGNAL_POLL_MICROS));
}

void PollSignals(signal_poll_func_t signal_poll) {
	if (signal_poll != nullptr) {
		signal_poll();
	}
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)

template <typename T>
int WaitLinux(volatile T* address, T expected, const WaitDeadline& deadline,
              signal_poll_func_t signal_poll) {
	bool       first_wait = true;

	for (;;) {
		if (ReadWord(address) != expected) {
			return OK;
		}
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		const timespec timeout = {
		    .tv_sec  = static_cast<time_t>(slice_micros / 1000000u),
		    .tv_nsec = static_cast<long>(slice_micros % 1000000u) * 1000L,
		};

		long result     = 0;
		int  wait_error = 0;
		result          = syscall(SYS_futex, const_cast<T*>(address), FUTEX_WAIT_PRIVATE,
		                          static_cast<uint32_t>(expected), &timeout, nullptr, 0);
		if (result != 0) {
			wait_error = errno;
		}
		if (result == 0 || wait_error == EAGAIN) {
			return OK;
		}
		if (wait_error != ETIMEDOUT && wait_error != EINTR) {
			return KERNEL_ERROR_EINVAL;
		}

		PollSignals(signal_poll);
		if (deadline.finite && Clock::now() >= deadline.end) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		first_wait = false;
	}
}

int WakeLinux(volatile void* address, int32_t count) {
	// The legacy FUTEX_WAKE path may wake one waiter when count is zero.
	if (count == 0) {
		return OK;
	}

	const auto result = syscall(SYS_futex, const_cast<void*>(address), FUTEX_WAKE_PRIVATE, count,
	                            nullptr, nullptr, 0);
	return result < 0 ? KERNEL_ERROR_EINVAL : static_cast<int>(result);
}

#endif

struct PortableWaiter {
	Common::CondVar condition;
	bool            wake_requested = false;
};

struct PortableAddressEntry {
	Common::Mutex              mutex;
	std::list<PortableWaiter*> waiters;
};

struct PortableAddressRegistry {
	std::mutex                                                           mutex;
	std::unordered_map<uintptr_t, std::shared_ptr<PortableAddressEntry>> entries;
};

PortableAddressRegistry& GetPortableRegistry() {
	static PortableAddressRegistry registry;
	return registry;
}

std::shared_ptr<PortableAddressEntry> RegisterPortableWaiter(volatile void*  address,
                                                             PortableWaiter* waiter) {
	auto&           registry = GetPortableRegistry();
	// Allocate before taking the entry mutex. A failed list allocation must
	// never leave a locked entry or a pointer to a destroyed stack waiter.
	std::list<PortableWaiter*> pending {waiter};
	std::lock_guard registry_lock(registry.mutex);
	const auto key = reinterpret_cast<uintptr_t>(address);
	auto it = registry.entries.find(key);
	if (it == registry.entries.end()) {
		auto created = std::make_shared<PortableAddressEntry>();
		it = registry.entries.emplace(key, std::move(created)).first;
	}
	auto entry = it->second;
	entry->mutex.Lock();
	entry->waiters.splice(entry->waiters.end(), pending);
	return entry;
}

void UnregisterPortableWaiter(volatile void*                               address,
                              const std::shared_ptr<PortableAddressEntry>& entry,
                              PortableWaiter*                              waiter) {
	entry->waiters.remove(waiter);
	const bool empty = entry->waiters.empty();
	entry->mutex.Unlock();
	if (!empty) {
		return;
	}

	auto&           registry = GetPortableRegistry();
	std::lock_guard registry_lock(registry.mutex);
	entry->mutex.Lock();
	const auto it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it != registry.entries.end() && it->second == entry && entry->waiters.empty()) {
		registry.entries.erase(it);
	}
	entry->mutex.Unlock();
}

template <typename T>
int WaitPortable(volatile T* address, T expected, const WaitDeadline& deadline,
                 signal_poll_func_t signal_poll, signal_pending_func_t signal_pending = nullptr) {
	PortableWaiter waiter;
	auto           entry      = RegisterPortableWaiter(address, &waiter);
	bool           first_wait = true;
	int            result     = OK;

	while (ReadWord(address) == expected && !waiter.wake_requested) {
		if (signal_pending != nullptr && signal_pending()) {
			result = KERNEL_ERROR_EINTR;
			break;
		}
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		if (slice_micros == 0) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}

		(void)waiter.condition.WaitFor(&entry->mutex, slice_micros);
		entry->mutex.Unlock();
		PollSignals(signal_poll);
		entry->mutex.Lock();
		if (signal_pending != nullptr && signal_pending()) {
			result = KERNEL_ERROR_EINTR;
			break;
		}

		if (deadline.finite && Clock::now() >= deadline.end && ReadWord(address) == expected &&
		    !waiter.wake_requested) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		first_wait = false;
	}

	UnregisterPortableWaiter(address, entry, &waiter);
	return result;
}

int WakePortable(volatile void* address, int32_t count) {
	if (count == 0) {
		return OK;
	}
	auto&            registry = GetPortableRegistry();
	std::unique_lock registry_lock(registry.mutex);
	const auto       it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it == registry.entries.end()) {
		return OK;
	}
	auto entry = it->second;
	entry->mutex.Lock();
	registry_lock.unlock();

	int32_t woken = 0;
	for (auto* waiter: entry->waiters) {
		if (!waiter->wake_requested) {
			waiter->wake_requested = true;
			waiter->condition.Signal();
			if (++woken == count) {
				break;
			}
		}
	}
	entry->mutex.Unlock();
	return woken;
}

// One wake budget covers both queues. In particular an aligned byte waiter
// and a legacy Linux futex waiter must not each receive the full count.
int WakeCount(volatile void* address, int32_t count) {
	const int portable = WakePortable(address, count);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	if (portable < count && IsValidWakeAddress(address)) {
		const int futex = WakeLinux(address, count - portable);
		return futex < 0 ? futex : portable + futex;
	}
#endif
	return portable;
}

uint64_t SteadyNanos() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    Clock::now().time_since_epoch()).count());
}

template <typename T>
int WaitNarrow(volatile T* address, T expected, uint32_t* timeout_micros,
               signal_pending_func_t signal_pending, budget_clock_func_t budget_clock) {
	// Firmware helper order: top nibble, nonpositive duration, then syscall.
	// In particular a zero budget beats a value mismatch (and a null address).
	if ((reinterpret_cast<uintptr_t>(address) >> 60u) != 0) {
		return KERNEL_ERROR_EFAULT;
	}
	if (timeout_micros != nullptr && *timeout_micros == 0) {
		return KERNEL_ERROR_ETIMEDOUT;
	}
	if (address == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	const auto clock = budget_clock != nullptr ? budget_clock : SteadyNanos;
	const auto start = timeout_micros != nullptr ? clock() : 0;
	int result;
	try {
		result = WaitPortable(address, expected, MakeDeadline(timeout_micros), nullptr, signal_pending);
	} catch (const std::bad_alloc&) {
		return KERNEL_ERROR_ENOMEM;
	}
	const auto end = timeout_micros != nullptr ? clock() : 0;
	if (result == KERNEL_ERROR_EINTR) {
		return OK; // Native wrapper suppresses EINTR without modifying the budget.
	}
	if (timeout_micros != nullptr) {
		if (result == KERNEL_ERROR_ETIMEDOUT) {
			*timeout_micros = 0;
		} else if (result == OK) {
			const auto elapsed = end >= start ? (end - start) / 1000u : 0;
			*timeout_micros = elapsed >= *timeout_micros ? 0 : *timeout_micros - static_cast<uint32_t>(elapsed);
		}
	}
	return result;
}

template <typename T>
int WaitImpl(volatile T* address, T expected, const WaitDeadline& deadline,
             signal_poll_func_t signal_poll) {
	if (!IsValidWaitAddress(address)) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = OK;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	result = WaitLinux(address, expected, deadline, signal_poll);
#else
	result = WaitPortable(address, expected, deadline, signal_poll);
#endif
	PollSignals(signal_poll);
	return result;
}

} // namespace

int Wait8(volatile uint8_t* address, uint8_t expected, uint32_t* timeout_micros,
          const char* /*name*/, signal_pending_func_t signal_pending, budget_clock_func_t budget_clock) {
	return WaitNarrow(address, expected, timeout_micros, signal_pending, budget_clock);
}

int Wait16(volatile uint16_t* address, uint16_t expected, uint32_t* timeout_micros,
           const char* /*name*/, signal_pending_func_t signal_pending, budget_clock_func_t budget_clock) {
	return WaitNarrow(address, expected, timeout_micros, signal_pending, budget_clock);
}

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout), signal_poll);
}

int Wake(volatile void* address, int32_t count) {
	if (!IsValidWakeAddress(address) || count < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = WakeCount(address, count);
	return result < 0 ? result : OK;
}

int WakeNative(volatile void* address, int32_t count) {
	// Verified helper short-circuit: bit63 already set means no syscall, even
	// for a negative count. There is deliberately no 32-bit alignment check.
	if ((reinterpret_cast<uintptr_t>(address) >> 63u) != 0) {
		return 0;
	}
	// The firmware wrapper does not establish raw-kernel invalid-count/null
	// ordering. These are explicit HLE validation policies, not measured native
	// results; valid narrow addresses/counts use the real shared queues above.
	if (count < 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (address == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	return WakeCount(address, count);
}

} // namespace Libs::LibKernel::SyncOnAddress
