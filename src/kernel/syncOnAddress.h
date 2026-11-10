#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_

#include "common/abi.h"
#include "common/common.h"

#include <chrono>

namespace Libs::LibKernel::SyncOnAddress {

using signal_poll_func_t = void (*)();
// Unlike signal_poll_func_t, these callbacks must not enter guest code. A guest
// signal handler may longjmp; delivery belongs after all waiter state is gone.
using signal_pending_func_t = bool (*)();
using budget_clock_func_t = uint64_t (*)(); // Nanoseconds, guest clock15 in export adapters.

int Wait8(volatile uint8_t* address, uint8_t expected, uint32_t* timeout_micros,
          const char* name, signal_pending_func_t signal_pending = nullptr,
          budget_clock_func_t budget_clock = nullptr);
int Wait16(volatile uint16_t* address, uint16_t expected, uint32_t* timeout_micros,
           const char* name, signal_pending_func_t signal_pending = nullptr,
           budget_clock_func_t budget_clock = nullptr);

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll = nullptr);
int Wake(volatile void* address, int32_t count);
// Native export result: number selected, or a kernel error. Wake() retains its
// status-only contract and its legacy address validation for _umtx_op callers.
int WakeNative(volatile void* address, int32_t count);

} // namespace Libs::LibKernel::SyncOnAddress

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_ */
