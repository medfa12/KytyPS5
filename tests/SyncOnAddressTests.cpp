#include "kernel/syncOnAddress.h"

#include "libs/errno.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// The normal target remains a small common-only executable. A full-emulator
// test target may compile this same source with this define and call
// RunSyncOnAddressGuestTests(); no second main or CMake change is required.
#if defined(KYTY_SYNC_ON_ADDRESS_GUEST_TESTS)
#include "kernel/pthread.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include <csetjmp>
namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
#endif

namespace {

using Libs::LibKernel::SyncOnAddress::Wait32;
using Libs::LibKernel::SyncOnAddress::Wait64;
using Libs::LibKernel::SyncOnAddress::Wake;
using Libs::LibKernel::SyncOnAddress::Wait8;
using Libs::LibKernel::SyncOnAddress::Wait16;
using Libs::LibKernel::SyncOnAddress::WakeNative;

std::atomic<int> g_signal_poll_count{0};

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "SyncOnAddressTests: failed: %s\n", text);
    std::abort();
  }
}

template <typename T> void Store(T *address, T value) {
  std::atomic_ref<T>(*address).store(value, std::memory_order_release);
}

void CountSignalPoll() {
  g_signal_poll_count.fetch_add(1, std::memory_order_relaxed);
}

void TestInvalidAddress() {
  uint32_t timeout = 1;
  Check(Wait32(nullptr, 0, &timeout) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wait32 rejects a null address");
  Check(Wait64(nullptr, 0, &timeout) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wait64 rejects a null address");
  Check(Wake(nullptr, 1) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wake rejects a null address");

  alignas(uint64_t) uint8_t bytes[16] = {};
  auto *misaligned = reinterpret_cast<uint32_t *>(bytes + 1);
  Check(Wait32(misaligned, 0, &timeout) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wait32 rejects a misaligned address");
  Check(Wait64(reinterpret_cast<uint64_t *>(bytes + 4), 0, &timeout) ==
            Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wait64 rejects a misaligned address");
  Check(Wake(misaligned, 1) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wake rejects a misaligned address");
  uint64_t aligned = 0;
  Check(Wake(&aligned, -1) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "wake rejects a negative count");
}

void TestMismatchReturnsImmediately() {
  uint32_t word = 7;
  uint64_t word64 = UINT64_C(0x100000000);
  uint32_t timeout = 500000;
  const auto start = std::chrono::steady_clock::now();
  Check(Wait32(&word, 6, &timeout, CountSignalPoll) == OK,
        "mismatch succeeds without parking");
  Check(Wait64(&word64, 0, &timeout, CountSignalPoll) == OK,
        "wait64 compares all 64 bits");
  Check(std::chrono::steady_clock::now() - start <
            std::chrono::milliseconds(100),
        "mismatched value is a fast path");
  Check(g_signal_poll_count.load(std::memory_order_relaxed) >= 2,
        "mismatch remains a guest signal safe-point");
}

void TestTimeout() {
  uint32_t word = 0;
  uint32_t timeout = 20000;
  const auto start = std::chrono::steady_clock::now();
  Check(Wait32(&word, 0, &timeout) == Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
        "matching value times out");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Check(elapsed >= std::chrono::milliseconds(10),
        "timeout does not return too early");
  Check(elapsed < std::chrono::milliseconds(500), "timeout remains bounded");

  timeout = 0;
  Check(Wait32(&word, 0, &timeout) == Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
        "zero timeout polls a matching value");
  word = 1;
  Check(Wait32(&word, 0, &timeout) == OK,
        "zero timeout succeeds for a mismatched value");
}

void TestValueChangeAndWake() {
  uint64_t word = 0;
  uint32_t timeout = 1000000;
  std::atomic<bool> ready{false};
  int result = Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT;

  std::thread waiter([&] {
    ready.store(true, std::memory_order_release);
    result = Wait64(&word, 0, &timeout);
  });
  while (!ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  Store(&word, UINT64_C(0x100000000));
  Check(Wake(&word, 1) == OK, "wake-one succeeds");
  waiter.join();
  Check(result == OK, "a value change plus wake releases the waiter");
}

void TestNanosecondTimeout() {
  uint64_t word = 0;
  const auto start = std::chrono::steady_clock::now();
  Check(Wait64(&word, 0, std::chrono::milliseconds(1)) ==
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
        "64-bit timespec wait expires");
  Check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1),
        "timespec wait preserves its deadline");
  Check(Wait64(&word, 0, std::chrono::nanoseconds(0)) ==
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
        "zero timespec polls a matching value");
  Check(Wait64(&word, 1, std::chrono::nanoseconds(0)) == OK,
        "zero timespec succeeds on mismatch");

  g_signal_poll_count.store(0, std::memory_order_relaxed);
  std::atomic<bool> returned{false};
  std::thread waker([&] {
    while (g_signal_poll_count.load(std::memory_order_relaxed) < 2 &&
           !returned.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    Store(&word, UINT64_C(0x100000000));
    Check(Wake(&word, 1) == OK, "wake releases a long timespec wait");
  });
  const int result = Wait64(&word, 0, std::chrono::nanoseconds::max(), CountSignalPoll);
  returned.store(true, std::memory_order_release);
  waker.join();
  Check(result == OK, "long timespec wait does not overflow its deadline");
  Check(g_signal_poll_count.load(std::memory_order_relaxed) > 1,
        "timespec wait continues polling guest signals");
}

void TestWakeOneThenAll() {
  constexpr int WAITER_COUNT = 4;
  uint32_t word = 0;
  uint32_t timeout = 1000000;
  std::atomic<int> ready{0};
  std::atomic<int> returned{0};
  int results[WAITER_COUNT] = {};
  std::vector<std::thread> waiters;

  for (int i = 0; i < WAITER_COUNT; i++) {
    waiters.emplace_back([&, i] {
      ready.fetch_add(1, std::memory_order_release);
      results[i] = Wait32(&word, 0, &timeout);
      returned.fetch_add(1, std::memory_order_release);
    });
  }
  while (ready.load(std::memory_order_acquire) != WAITER_COUNT) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(Wake(&word, 1) == OK, "wake-one succeeds with multiple waiters");
  const auto one_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (returned.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < one_deadline) {
    std::this_thread::yield();
  }
  Check(returned.load(std::memory_order_acquire) == 1,
        "wake-one releases exactly one waiter");

  Check(Wake(&word, 2) == OK, "wake-two succeeds");
  const auto two_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (returned.load(std::memory_order_acquire) < 3 &&
         std::chrono::steady_clock::now() < two_deadline) {
    std::this_thread::yield();
  }
  Check(returned.load(std::memory_order_acquire) == 3,
        "wake-two releases exactly two more waiters");

  Check(Wake(&word, INT_MAX) == OK, "wake-all succeeds");
  for (auto &waiter : waiters) {
    waiter.join();
  }
  Check(returned.load(std::memory_order_acquire) == WAITER_COUNT,
        "wake-all releases the remaining waiters");
  for (int result : results) {
    Check(result == OK, "explicitly woken waiters return success");
  }
}

void TestAddressesAreIsolated() {
  uint32_t first = 0;
  uint32_t second = 0;
  uint32_t first_timeout = 1000000;
  uint32_t second_timeout = 1000000;
  std::atomic<int> ready{0};
  std::atomic<bool> first_returned{false};
  std::atomic<bool> second_returned{false};
  int first_result = 0;
  int second_result = 0;

  std::thread first_waiter([&] {
    ready.fetch_add(1, std::memory_order_release);
    first_result = Wait32(&first, 0, &first_timeout);
    first_returned.store(true, std::memory_order_release);
  });
  std::thread second_waiter([&] {
    ready.fetch_add(1, std::memory_order_release);
    second_result = Wait32(&second, 0, &second_timeout);
    second_returned.store(true, std::memory_order_release);
  });
  while (ready.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(Wake(&first, 1) == OK, "first address wakes");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(first_returned.load(std::memory_order_acquire),
        "first address waiter returned");
  Check(!second_returned.load(std::memory_order_acquire),
        "waking one address does not release another address");
  Check(Wake(&second, 1) == OK, "second address wakes");

  first_waiter.join();
  second_waiter.join();
  Check(first_result == OK && second_result == OK,
        "isolated waiters return success");
}

void TestCompareRegisterWakeRace() {
  for (int i = 0; i < 100; i++) {
    uint32_t word = 0;
    uint32_t timeout = 500000;
    std::atomic<bool> ready{false};
    int result = Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT;
    std::thread waiter([&] {
      ready.store(true, std::memory_order_release);
      result = Wait32(&word, 0, &timeout);
    });
    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    Store(&word, uint32_t{1});
    (void)Wake(&word, 1);
    waiter.join();
    Check(result == OK, "compare/register/wake race never loses progress");
  }
}

void TestWakeZeroIsNoOp() {
  constexpr int WAITER_COUNT = 2;
  uint32_t word = 0;
  uint32_t timeout = 1000000;
  std::atomic<int> ready{0};
  std::atomic<int> returned{0};
  int results[WAITER_COUNT] = {};
  std::vector<std::thread> waiters;

  for (int i = 0; i < WAITER_COUNT; i++) {
    waiters.emplace_back([&, i] {
      ready.fetch_add(1, std::memory_order_release);
      results[i] = Wait32(&word, 0, &timeout);
      returned.fetch_add(1, std::memory_order_release);
    });
  }
  while (ready.load(std::memory_order_acquire) != WAITER_COUNT) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(Wake(&word, 0) == OK, "zero-count wake succeeds");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Check(returned.load(std::memory_order_acquire) == 0,
        "zero-count wake releases no waiters");
  Check(Wake(&word, INT_MAX) == OK, "wake-all succeeds after zero-count wake");
  for (auto &waiter : waiters) {
    waiter.join();
  }
  for (int result : results) {
    Check(result == OK, "wake-all releases waiters after zero-count no-op");
  }
}

std::atomic<int> g_narrow_parked{0};
std::atomic<bool> g_interrupt{false};
thread_local bool g_observed_park = false;

bool ObserveNarrowPark() {
  if (!g_observed_park) {
    g_observed_park = true;
    g_narrow_parked.fetch_add(1, std::memory_order_release);
  }
  return g_interrupt.load(std::memory_order_acquire);
}

template <typename Predicate> void Await(Predicate predicate, const char* text) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool ready = predicate();
  while (!ready && std::chrono::steady_clock::now() < end) {
    std::this_thread::yield();
    ready = predicate();
  }
  Check(ready, text);
}

uint64_t ScriptedBudgetClock() {
  static thread_local uint64_t now = 0;
  now += 12345000; // 12345 microseconds per observation, independent of scheduling.
  return now;
}

void TestNarrowValidationAndBudgets() {
  using namespace Libs::LibKernel;
  alignas(8) uint8_t bytes[8] = {0xff, 7, 0, 0, 0xff, 0xff, 0xff, 0xff};
  auto* half = reinterpret_cast<uint16_t*>(bytes + 2);
  uint32_t budget = 0;
  Check(Wait8(bytes + 1, 6, &budget, "byte") == KERNEL_ERROR_ETIMEDOUT && budget == 0,
        "native zero duration precedes byte mismatch");
  Check(Wait16(half, 1, &budget, "half") == KERNEL_ERROR_ETIMEDOUT,
        "native zero duration precedes halfword mismatch");
  Check(Wait8(nullptr, 0, &budget, nullptr) == KERNEL_ERROR_ETIMEDOUT,
        "zero duration precedes raw syscall null-address validation");
  for (unsigned tag = 1; tag < 16; ++tag) {
    auto* bad = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(tag) << 60u);
    Check(Wait8(bad, 0, &budget, nullptr) == KERNEL_ERROR_EFAULT && budget == 0,
          "every high nibble faults before zero-timeout precheck");
    budget = 777;
    Check(Wait16(reinterpret_cast<volatile uint16_t*>(bad), 0, &budget, nullptr) == KERNEL_ERROR_EFAULT && budget == 777,
          "tag fault preserves the mutable budget");
    budget = 0;
  }
  budget = 50000;
  Check(Wait8(nullptr, 0, &budget, nullptr) == KERNEL_ERROR_EFAULT && budget == 50000,
        "null byte address faults without consuming budget");
  Check(Wait8(bytes + 1, 6, &budget, "byte", nullptr, ScriptedBudgetClock) == OK && budget == 37655,
        "successful byte mismatch subtracts elapsed guest-clock microseconds");
  budget = 100;
  Check(Wait16(half, 1, &budget, "half", nullptr, ScriptedBudgetClock) == OK && budget == 0,
        "successful halfword wait saturates the remaining budget at zero");
  budget = 20000;
  const auto start = std::chrono::steady_clock::now();
  Check(Wait16(half, 0, &budget, "half") == KERNEL_ERROR_ETIMEDOUT && budget == 0,
        "matching halfword really waits and clears expired budget");
  Check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(20),
        "narrow wait keeps its real deadline across polling slices");
  Check(WakeNative(reinterpret_cast<void*>(UINT64_C(0x8000000000000000)), -1) == 0,
        "native bit63 wake short circuit precedes count validation");
  Check(WakeNative(bytes + 1, 0) == 0 && WakeNative(bytes + 1, 4) == 0,
        "native unaligned byte wake reports zero with no waiters");
  Check(Wake(bytes, 1) == OK, "internal wake remains status-only");
}

void TestNarrowExactWidthAtPageEnd() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  SYSTEM_INFO info {};
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  auto* bytes = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(bytes != nullptr, "allocate narrow guard-page fixture");
  DWORD previous = 0;
  Check(VirtualProtect(bytes + page, page, PAGE_NOACCESS, &previous) != 0, "protect adjacent page");
#else
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* bytes = static_cast<uint8_t*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(bytes != MAP_FAILED, "allocate narrow guard-page fixture");
  Check(mprotect(bytes + page, page, PROT_NONE) == 0, "protect adjacent page");
#endif
  bytes[page - 1] = 7;
  uint32_t budget = 100000;
  Check(Wait8(bytes + page - 1, 6, &budget, "page-byte") == OK,
        "byte comparison does not read into an inaccessible neighboring page");
  auto* half = reinterpret_cast<uint16_t*>(bytes + page - 2);
  Store(half, uint16_t{0x8765});
  budget = 100000;
  Check(Wait16(half, 0x8764, &budget, "page-half") == OK,
        "halfword comparison neither widens nor ignores its high byte");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Check(VirtualFree(bytes, 0, MEM_RELEASE) != 0, "release narrow guard-page fixture");
#else
  Check(munmap(bytes, page * 2) == 0, "release narrow guard-page fixture");
#endif
#endif
}

void TestNarrowWakeCountAndNeighborIsolation() {
  alignas(8) uint8_t bytes[8] {};
  constexpr int count = 4;
  std::atomic<int> returned{0};
  int results[count] {};
  uint32_t budgets[count] = {1000000, 1000000, 1000000, 1000000};
  g_narrow_parked.store(0);
  g_interrupt.store(false);
  std::vector<std::thread> waiters;
  for (int i = 0; i < count; ++i) {
    waiters.emplace_back([&, i] {
      results[i] = Wait8(bytes + 1, 0, &budgets[i], "same-byte", ObserveNarrowPark);
      returned.fetch_add(1, std::memory_order_release);
    });
  }
  Await([] { return g_narrow_parked.load(std::memory_order_acquire) == count; }, "byte waiters are registered");
  Store(bytes, uint8_t{1});
  Store(bytes + 2, uint8_t{2});
  Check(WakeNative(bytes + 2, INT_MAX) == 0, "adjacent byte address has its own wake key");
  Check(WakeNative(bytes + 1, 0) == 0, "zero native wake selects no waiters");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Check(returned.load(std::memory_order_acquire) == 0, "neighbor mutation and notification do not release target-byte waiters");
  Check(WakeNative(bytes + 1, 1) == 1, "native byte wake-one returns one, not status zero");
  Await([&] { return returned.load(std::memory_order_acquire) == 1; }, "exactly one byte waiter returns");
  Check(WakeNative(bytes + 1, 2) == 2, "native byte wake-two returns two");
  Await([&] { return returned.load(std::memory_order_acquire) == 3; }, "exactly two additional byte waiters return");
  Check(WakeNative(bytes + 1, INT_MAX) == 1, "wake-all reports only the remaining waiter");
  for (auto& thread : waiters) thread.join();
  for (int i = 0; i < count; ++i) {
    Check(results[i] == OK && budgets[i] > 0 && budgets[i] < 1000000,
          "explicit byte wake succeeds with a genuinely reduced budget");
  }
  Check(WakeNative(bytes + 1, 1) == 0, "released waiter registry is cleaned up");
}

void TestMixedWidthsShareWakeBudget() {
  alignas(8) uint64_t word = 0;
  std::atomic<int> returned{0};
  g_narrow_parked.store(0);
  g_signal_poll_count.store(0);
  uint32_t budgets[3] = {1000000, 1000000, 1000000};
  int results[3] {};
  std::thread byte([&] {
    results[0] = Wait8(reinterpret_cast<uint8_t*>(&word), 0, &budgets[0], "mixed8", ObserveNarrowPark);
    returned.fetch_add(1, std::memory_order_release);
  });
  std::thread half([&] {
    results[1] = Wait16(reinterpret_cast<uint16_t*>(&word), 0, &budgets[1], "mixed16", ObserveNarrowPark);
    returned.fetch_add(1, std::memory_order_release);
  });
  std::thread wide([&] {
    results[2] = Wait32(reinterpret_cast<uint32_t*>(&word), 0, &budgets[2], CountSignalPoll);
    returned.fetch_add(1, std::memory_order_release);
  });
  Await([] { return g_narrow_parked.load(std::memory_order_acquire) == 2 &&
                    g_signal_poll_count.load(std::memory_order_acquire) > 0; }, "mixed-width queues are waiting");
  Check(WakeNative(&word, 1) == 1, "one budget shared by portable narrow and legacy wide queues");
  Await([&] { return returned.load(std::memory_order_acquire) >= 1; }, "mixed wake makes progress");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Check(returned.load(std::memory_order_acquire) == 1, "mixed wake-one does not wake one from each backend");
  int selected = 1;
  Await([&] {
    const int n = WakeNative(&word, 2);
    Check(n >= 0 && n <= 2 && selected + n <= 3, "mixed queue wake counts never exceed the remaining budget");
    selected += n;
    return selected == 3;
  }, "remaining mixed waiters are selected");
  byte.join(); half.join(); wide.join();
  Check(returned.load() == 3 && results[0] == OK && results[1] == OK && results[2] == OK,
        "mixed byte/halfword/futex waiters all finish successfully");
}

void TestNarrowInterruptedBudgetAndCleanup() {
  uint16_t half = 0;
  uint32_t budget = 500000;
  g_narrow_parked.store(0);
  g_interrupt.store(false);
  int result = -1;
  std::thread thread([&] { result = Wait16(&half, 0, &budget, "interrupted", ObserveNarrowPark); });
  Await([] { return g_narrow_parked.load(std::memory_order_acquire) == 1; }, "interruptible halfword waiter parks");
  g_interrupt.store(true, std::memory_order_release);
  thread.join();
  Check(result == OK && budget == 500000, "raw EINTR is suppressed and leaves the entire original budget unchanged");
  Check(WakeNative(&half, 1) == 0, "interrupted waiter is removed before guest signal delivery");
  g_interrupt.store(false);
}

void TestNarrowCompareEnqueueWakeRaces() {
  for (int i = 0; i < 100; ++i) {
    alignas(4) uint8_t bytes[4] {};
    uint32_t budget = 500000;
    std::atomic<bool> started{false};
    int result = -1;
    std::thread thread([&] {
      started.store(true, std::memory_order_release);
      result = (i & 1) != 0 ? Wait8(bytes + 1, 0, &budget, "race8") :
          Wait16(reinterpret_cast<uint16_t*>(bytes + 2), 0, &budget, "race16");
    });
    Await([&] { return started.load(std::memory_order_acquire); }, "narrow race thread starts");
    if ((i & 1) != 0) {
      Store(bytes + 1, uint8_t{1});
      (void)WakeNative(bytes + 1, 1);
    } else {
      Store(reinterpret_cast<uint16_t*>(bytes + 2), uint16_t{0x100});
      (void)WakeNative(bytes + 2, 1);
    }
    thread.join();
    Check(result == OK && budget > 0, "narrow compare/enqueue/wake race never loses a wake or reads the wrong width");
  }
}

void TestInfiniteNarrowWait() {
  alignas(4) uint8_t bytes[4] {};
  g_narrow_parked.store(0);
  int result = -1;
  std::thread thread([&] { result = Wait8(bytes + 3, 0, nullptr, "infinite", ObserveNarrowPark); });
  Await([] { return g_narrow_parked.load(std::memory_order_acquire) == 1; }, "infinite byte waiter is registered");
  Check(WakeNative(bytes + 3, 1) == 1, "unaligned infinite byte waiter receives a real counted notification");
  thread.join();
  Check(result == OK, "null-timeout narrow wait succeeds only after notification");
}

#if defined(KYTY_SYNC_ON_ADDRESS_GUEST_TESTS)
using GuestWait8 = int (KYTY_SYSV_ABI *)(volatile uint8_t*, uint8_t, uint32_t*, const char*);
using GuestWake = int (KYTY_SYSV_ABI *)(volatile void*, int32_t);
struct GuestSignalState {
  GuestWait8 wait = nullptr;
  GuestWake wake = nullptr;
  alignas(4) uint8_t bytes[4] {};
  uint32_t budget = 500000;
  uint32_t* budget_address = &budget;
  int result = -1;
  bool jump = false;
  bool jumped = false;
  // This worker executes on a guest stack. The Windows CRT longjmp attempts
  // host SEH unwinding there and raises STATUS_BAD_STACK. Model guest libc's
  // same-stack nonlocal transfer without invoking the host unwinder.
  std::jmp_buf jump_target;
  Libs::LibKernel::Pthread handler_self = nullptr;
  std::atomic<bool> called{false};
};
std::atomic<GuestSignalState*> g_guest_signal_state{nullptr};

void KYTY_SYSV_ABI AddressWaitGuestSignalHandler(int, void*) {
  auto& state = *g_guest_signal_state.load(std::memory_order_acquire);
  state.handler_self = Libs::LibKernel::PthreadSelfOrNull();
  Check(*state.budget_address == 500000, "signal callback sees the original unchanged wait budget");
  Check(state.wake(state.bytes + 1, 1) == 0,
        "real guest signal dispatch happens after waiter registry cleanup");
  state.called.store(true, std::memory_order_release);
  if (state.jump) {
#if defined(_WIN32) && defined(_M_X64)
    // A zero Frame selects the CRT's register/stack restore without walking
    // Windows unwind metadata for guest frames. Keep its full register save;
    // the compiler's reduced builtin jump buffer is insufficient across ABIs.
    reinterpret_cast<_JUMP_BUFFER*>(state.jump_target)->Frame = 0;
#endif
    std::longjmp(state.jump_target, 1);
  }
}

void* KYTY_SYSV_ABI AddressWaitGuestSignalWorker(void* pointer) {
  auto& state = *static_cast<GuestSignalState*>(pointer);
  if (setjmp(state.jump_target) == 0) {
    Libs::LibKernel::PthreadQueuePendingSignal(Libs::LibKernel::PthreadSelfOrNull(), 30);
    state.result = state.wait(state.bytes + 1, 0, state.budget_address, "SceLibcV2AtomicWait");
  } else {
    state.jumped = true;
  }
  return nullptr;
}

void TestGuestSignalSuppression(Loader::SymbolDatabase& symbols, GuestWait8 wait, GuestWake wake) {
  using Install = int (KYTY_SYSV_ABI *)(int, void*);
  using Remove = int (KYTY_SYSV_ABI *)(int);
  Loader::SymbolResolve install_query {"WkwEd3N7w0Y", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
  Loader::SymbolResolve remove_query {"Qhv5ARAoOEc", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
  const auto* install_symbol = symbols.FindRegistration(install_query, "LibKernel::KernelInstallExceptionHandler");
  const auto* remove_symbol = symbols.FindRegistration(remove_query, "LibKernel::KernelRemoveExceptionHandler");
  Check(install_symbol != nullptr && remove_symbol != nullptr, "signal fixture resolves exact handler exports");
  const auto install = reinterpret_cast<Install>(install_symbol->vaddr);
  const auto remove = reinterpret_cast<Remove>(remove_symbol->vaddr);
  Check(install(30, reinterpret_cast<void*>(&AddressWaitGuestSignalHandler)) == OK, "install an unused guest exception handler");
  for (bool jump : {false, true}) {
    GuestSignalState state;
    state.wait = wait; state.wake = wake; state.jump = jump;
    g_guest_signal_state.store(&state, std::memory_order_release);
    Libs::LibKernel::Pthread worker = nullptr;
    Check(Libs::LibKernel::PthreadCreate(&worker, nullptr, AddressWaitGuestSignalWorker, &state, "narrow-address-signal") == OK,
          "signal suppression fixture creates a real guest pthread");
    Check(Libs::LibKernel::PthreadJoin(worker, nullptr) == OK, "signal suppression guest pthread joins");
    Check(state.called.load(std::memory_order_acquire) && state.handler_self == worker && state.budget == 500000,
          "queued guest signal runs on the waiting guest pthread and preserves its full budget");
    Check(jump ? state.jumped && state.result == -1 : !state.jumped && state.result == OK,
          "guest handler may return or longjmp without leaving waiter state or a locked registry");
    Check(wake(state.bytes + 1, 1) == 0, "guest signal cleanup leaves no stale waiter");
    g_guest_signal_state.store(nullptr, std::memory_order_release);
  }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  SYSTEM_INFO info {};
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  auto* readonly_budget = static_cast<uint32_t*>(VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(readonly_budget != nullptr, "allocate read-only guest budget fixture");
  *readonly_budget = 500000;
  DWORD previous = 0;
  Check(VirtualProtect(readonly_budget, page, PAGE_READONLY, &previous) != 0, "make guest budget read-only");
#else
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* readonly_budget = static_cast<uint32_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(readonly_budget != MAP_FAILED, "allocate read-only guest budget fixture");
  *readonly_budget = 500000;
  Check(mprotect(readonly_budget, page, PROT_READ) == 0, "make guest budget read-only");
#endif
  GuestSignalState state;
  state.wait = wait; state.wake = wake; state.budget_address = readonly_budget;
  g_guest_signal_state.store(&state, std::memory_order_release);
  Libs::LibKernel::Pthread worker = nullptr;
  Check(Libs::LibKernel::PthreadCreate(&worker, nullptr, AddressWaitGuestSignalWorker, &state, "narrow-readonly-budget") == OK &&
        Libs::LibKernel::PthreadJoin(worker, nullptr) == OK, "read-only interruption fixture runs on a real guest pthread");
  Check(state.result == OK && state.called.load(std::memory_order_acquire) && *readonly_budget == 500000,
        "EINTR does not even attempt to write back an unchanged budget");
  Check(wait(state.bytes + 1, 1, readonly_budget, "readonly-budget") == Libs::LibKernel::KERNEL_ERROR_EFAULT &&
        *readonly_budget == 500000, "successful wait copyout cannot bypass guest read-only page protection");
  g_guest_signal_state.store(nullptr, std::memory_order_release);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Check(VirtualFree(readonly_budget, 0, MEM_RELEASE) != 0, "free read-only budget fixture");
#else
  Check(munmap(readonly_budget, page) == 0, "free read-only budget fixture");
#endif
#endif
  Check(remove(30) == OK, "remove guest signal fixture handler");
}
#endif

} // namespace

#if !defined(KYTY_SYNC_ON_ADDRESS_GUEST_TESTS)
int main() {
  TestInvalidAddress();
  TestMismatchReturnsImmediately();
  TestTimeout();
  TestValueChangeAndWake();
  TestNanosecondTimeout();
  TestWakeOneThenAll();
  TestAddressesAreIsolated();
  TestCompareRegisterWakeRace();
  TestWakeZeroIsNoOp();
  TestNarrowValidationAndBudgets();
  TestNarrowExactWidthAtPageEnd();
  TestNarrowWakeCountAndNeighborIsolation();
  TestMixedWidthsShareWakeBudget();
  TestNarrowInterruptedBudgetAndCleanup();
  TestNarrowCompareEnqueueWakeRaces();
  TestInfiniteNarrowWait();
  std::printf("SyncOnAddressTests: all passed\n");
  return 0;
}
#else
void RunSyncOnAddressGuestTests() {
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  using Wait8Abi = int (KYTY_SYSV_ABI *)(volatile uint8_t*, uint8_t, uint32_t*, const char*);
  using Wait16Abi = int (KYTY_SYSV_ABI *)(volatile uint16_t*, uint16_t, uint32_t*, const char*);
  using WakeAbi = int (KYTY_SYSV_ABI *)(volatile void*, int32_t);
  const char* nids[] = {"dFym5b41JLU", "XgVs4jRehcY", "q2y-wDIVWZA"};
  const char* names[] = {"Posix::KernelSyncOnAddressWait8", "Posix::KernelSyncOnAddressWait16", "Posix::KernelSyncOnAddressWake"};
  uint64_t addresses[3] {};
  for (int i = 0; i < 3; ++i) {
    Loader::SymbolResolve query {nids[i], "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    const auto* symbol = symbols.FindRegistration(query, names[i]);
    Check(symbol != nullptr, "narrow wait/wake exact firmware NID and library/module versions resolve");
    addresses[i] = symbol->vaddr;
    query.library_version = 2;
    Check(symbols.FindRegistration(query, names[i]) == nullptr, "no narrow wait library version fallback");
    query.library_version = 1;
    query.module_version_minor = 0;
    Check(symbols.FindRegistration(query, names[i]) == nullptr, "no narrow wait module version fallback");
    query.module_version_minor = 1;
    query.type = Loader::SymbolType::Object;
    Check(symbols.FindRegistration(query, names[i]) == nullptr, "narrow wait exports are functions, not objects");
  }
  const auto wait8 = reinterpret_cast<Wait8Abi>(addresses[0]);
  const auto wait16 = reinterpret_cast<Wait16Abi>(addresses[1]);
  const auto wake = reinterpret_cast<WakeAbi>(addresses[2]);
  alignas(4) uint8_t bytes[4] {};
  uint32_t budget = 0;
  Check(wait8(bytes + 1, 1, &budget, "SceLibcV2AtomicWait") == Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
        "resolved four-argument byte ABI preserves native zero-duration order");
  budget = 10000;
  Check(wait8(reinterpret_cast<volatile uint8_t*>(UINT64_C(0x1000000000000000)), 0, &budget, "tagged") ==
            Libs::LibKernel::KERNEL_ERROR_EFAULT && budget == 10000,
        "resolved byte ABI preserves the full budget on native top-nibble rejection");
  Check(wait8(bytes + 1, 0, reinterpret_cast<uint32_t*>(1), "bad-budget") == Libs::LibKernel::KERNEL_ERROR_EFAULT,
        "resolved byte ABI fault-contains guest timeout copyin");
  budget = 10000;
  Check(wait16(reinterpret_cast<uint16_t*>(bytes + 2), 1, &budget, "SceLibcV2AtomicWait") == OK && budget <= 10000,
        "resolved four-argument halfword ABI consumes a mutable microsecond budget");
  Check(wake(bytes + 1, 1) == 0, "resolved native wake accepts an unaligned byte address and reports the count");
  g_narrow_parked.store(0);
  int result = -1;
  std::thread thread([&] { result = Wait8(bytes + 1, 0, nullptr, "guest", ObserveNarrowPark); });
  Await([] { return g_narrow_parked.load(std::memory_order_acquire) == 1; }, "resolved wake fixture parks");
  Check(wake(bytes + 1, 1) == 1, "resolved native export returns one for a selected waiter");
  thread.join();
  Check(result == OK, "resolved wake really releases the waiter");
  TestGuestSignalSuppression(symbols, wait8, wake);
}
#endif
