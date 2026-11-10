#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs {

struct VaContext;
struct VaList;

using guest_printf_std_func_t   = KYTY_FORMAT_PRINTF(1, 2) KYTY_SYSV_ABI
    int (*)(const char* str, ...);
using guest_printf_ctx_func_t   = int (*)(VaContext* c);
using guest_snprintf_ctx_func_t = int (*)(VaContext* c);
using guest_vprintf_func_t      = int (*)(const char* str, VaList* c);
using guest_vsnprintf_func_t    = int (*)(char* s, size_t n, const char* format, VaList* c);

guest_printf_std_func_t   GetGuestPrintfStdFunc();
guest_printf_ctx_func_t   GetGuestPrintfCtxFunc();
guest_snprintf_ctx_func_t GetGuestSnprintfCtxFunc();
guest_vprintf_func_t      GetGuestVprintfFunc();
guest_vsnprintf_func_t    GetGuestVsnprintfFunc();

// ps5-vsh chainSUI149: entry-stack parity ring for the host printf-context
// entries (misaligned guest stacks fault their prologue movaps). Dumped by
// the VEH crash handler; absence of a record for the fatal call + a
// prologue-pc crash proves misaligned entry.
void Ps5VshDumpPrintfEntrySp();

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_ */
