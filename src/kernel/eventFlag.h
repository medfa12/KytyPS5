#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_

#include "common/abi.h"
#include "common/common.h"
#include "kernel/pthread.h"

namespace Libs::LibKernel::EventFlag {

class KernelEventFlagPrivate;

using KernelEventFlag = KernelEventFlagPrivate*;

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* param);
int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef);
int KYTY_SYSV_ABI KernelOpenEventFlag(KernelEventFlag* out, const char* name);
int KYTY_SYSV_ABI KernelCloseEventFlag(KernelEventFlag ef);
int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat, KernelUseconds* timeout);
int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat);
int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern);
int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern);
int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern,
                                         int* num_wait_threads);

// Retains the object atomically with the name lookup; does not manufacture an alias.
int SetEventFlagByName(const char* name, uint64_t bits);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
enum class EventFlagHookPoint { None, Retained, Enrolled, Cancelled, Retired, AfterPark, Dispatch };
enum class EventFlagFailure { None, Prepare, Diagnostic, Park, Store };
void EventFlagHookForTest(EventFlagHookPoint point, void (*hook)(void*), void* context);
void EventFlagFailForTest(EventFlagFailure point);
EventFlagFailure EventFlagFailurePendingForTest();
// One-shot callback in the dispatcher exception boundary, outside all locks.
void EventFlagDispatchForTest(void (*hook)(void*), void* context);
int EventFlagObjectsForTest();
int EventFlagWaitersForTest(KernelEventFlag handle);
#endif

} // namespace Libs::LibKernel::EventFlag

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_ */
