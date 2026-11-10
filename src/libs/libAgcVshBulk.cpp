#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Libs {

// ps5-vsh: bulk AgcVsh stubs for NPXS40038 (SceAgcVsh, AgcVsh_v1 / AgcVsh_v1.1).
// Derived from _ProbeLogs/npxs40038_v3.log (`unresolved PLT import patched to
// stub` lines containing `AgcVsh_v1`): 79 distinct NIDs. Names come from
// _ProbeLogs/npxs40038_imports.txt (NID -> sceAgc* mapping); all 79 mapped.
// Excludes the 3 NIDs already stubbed in src/libs/libPs5VshVideo.cpp
// (AgcVshStubs: mzvvOZlxNag, k0E7vkgqAuE, K2mciNVxUCE) and the 156 NIDs owned
// by src/libs/libAgcDriver.cpp (Graphics5) -- verified 0 overlap on both.
// All stubs are log-and-return-0; real implementations (sizes, Dcb/Acb
// builders) come later.

namespace AgcVshBulk {

LIB_VERSION("AgcVsh", 1, "AgcVsh", 1, 1);

namespace Agc {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcquireMemSetEngine(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbRewind(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbRewindGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbAtomicMem(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbAtomicMemGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbAtomicGds(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbAtomicGdsGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbDmaDataGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbDispatchIndirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbEventWriteGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbMemSemaphore(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbPrimeUtcl2(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbPrimeUtcl2GetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbPushMarkerSpan(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbQueueEndOfShaderActionGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbSetFlip(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbSetMarkerSpan(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbSetWorkloadComplete(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbSetWorkloadsActive(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbSetWorkloadStreamInactive(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAcbWaitUntilSafeForRendering(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAsyncWriteDataPatchSetAddressOrOffset(uint64_t a1, uint64_t a2,
                                                                 uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAsyncWriteDataPatchSetCachePolicy(uint64_t a1, uint64_t a2,
                                                             uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAsyncWriteDataPatchSetDst(uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcAsyncRewindPatchSetRewindState(uint64_t a1, uint64_t a2,
                                                          uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcBranchPatchSetThenTarget(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcBranchPatchSetElseTarget(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcBranchPatchSetCompareAddress(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbBranchGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbCondWrite(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbCondWriteGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                              uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbMemsetExclusive(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbSetUcRegistersDirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbSetShRegistersDirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbSetUcRegisterRangeDirectGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcCbSetUcRegisterRangeDirect(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbAtomicMem(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbAtomicMemGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbAtomicGds(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbAtomicGdsGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbBeginOcclusionQueryGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbDmaDataGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbDrawIndexIndirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbDrawIndexIndirectMultiGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbDrawIndirectMultiGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbEndOcclusionQueryGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbGetLodStatsGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbMemSemaphore(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbPrimeUtcl2(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbPrimeUtcl2GetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbPushMarkerSpan(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbQueueEndOfShaderActionGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetBaseDispatchIndirectArgsGetSize(uint64_t a1, uint64_t a2,
                                                                 uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetBaseDrawIndirectArgsGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetBoolPredicationEnableGetSize(uint64_t a1, uint64_t a2,
                                                              uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetCxRegistersIndirectGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetIndexBufferGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetIndexCountGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetIndexIndirectArgs(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetIndexIndirectArgsGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                          uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetIndexSizeGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetMarkerSpan(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetPredicationDisableGetSize(uint64_t a1, uint64_t a2,
                                                           uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetShRegisterDirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetShRegistersIndirectGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetUcRegisterDirectGetSize(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetUcRegistersIndirectGetSize(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetWorkloadStreamInactive(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbStallCommandBufferParserGetSize(uint64_t a1, uint64_t a2,
                                                              uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbSetZPassPredicationEnableGetSize(uint64_t a1, uint64_t a2,
                                                               uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	LOGF("\t size 0 (real size implemented later)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcGetSemaphoreLabel(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcGetGsPrimPayload(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcGetShaderInstrumentation(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcQueueEndOfPipeActionPatchGcrCntl(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcQueueEndOfPipeActionPatchType(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcSetShaderInstrumentation(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcWaitRegMemPatchCompareFunction(uint64_t a1, uint64_t a2, uint64_t a3,
                                                          uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcWriteDataPatchSetCachePolicy(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcWriteDataPatchSetDst(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Agc

LIB_DEFINE(InitAgcVshBulk_1) {
	LIB_FUNC("W0WEyog0f74", Agc::AgcAcquireMemSetEngine);                 // sceAgcAcquireMemSetEngine
	LIB_FUNC("DwICrVxerkY", Agc::AgcAcbRewind);                           // sceAgcAcbRewind
	LIB_FUNC("0ZOG0jc9nRg", Agc::AgcAcbRewindGetSize);                     // sceAgcAcbRewindGetSize
	LIB_FUNC("XKKuA6VkSRc", Agc::AgcAcbAtomicMem);                         // sceAgcAcbAtomicMem
	LIB_FUNC("da1Sm8-QDoU", Agc::AgcAcbAtomicMemGetSize);                  // sceAgcAcbAtomicMemGetSize
	LIB_FUNC("gQkqkLttcpw", Agc::AgcAcbAtomicGds);                         // sceAgcAcbAtomicGds
	LIB_FUNC("hcIxS8pmXF4", Agc::AgcAcbAtomicGdsGetSize);                  // sceAgcAcbAtomicGdsGetSize
	LIB_FUNC("M0ttm8h7SKA", Agc::AgcAcbDmaDataGetSize);                    // sceAgcAcbDmaDataGetSize
	LIB_FUNC("PxKWV2fVAps", Agc::AgcAcbDispatchIndirectGetSize);           // sceAgcAcbDispatchIndirectGetSize
	LIB_FUNC("Y-5vneiBtzk", Agc::AgcAcbEventWriteGetSize);                 // sceAgcAcbEventWriteGetSize
	LIB_FUNC("q4VuU-QsLOE", Agc::AgcAcbMemSemaphore);                      // sceAgcAcbMemSemaphore
	LIB_FUNC("szG7hz2yEhA", Agc::AgcAcbPrimeUtcl2);                         // sceAgcAcbPrimeUtcl2
	LIB_FUNC("eCjKaqeeQ5s", Agc::AgcAcbPrimeUtcl2GetSize);                  // sceAgcAcbPrimeUtcl2GetSize
	LIB_FUNC("8Kly1JrJUlw", Agc::AgcAcbPushMarkerSpan);                    // sceAgcAcbPushMarkerSpan
	LIB_FUNC("F8NLhWvFemI", Agc::AgcAcbQueueEndOfShaderActionGetSize);     // sceAgcAcbQueueEndOfShaderActionGetSize
	LIB_FUNC("ebixW91gpPw", Agc::AgcAcbSetFlip);                           // sceAgcAcbSetFlip
	LIB_FUNC("pxx-GoOSdw4", Agc::AgcAcbSetMarkerSpan);                     // sceAgcAcbSetMarkerSpan
	LIB_FUNC("opR1JeJZCBU", Agc::AgcAcbSetWorkloadComplete);               // sceAgcAcbSetWorkloadComplete
	LIB_FUNC("rVOmPz2RBlg", Agc::AgcAcbSetWorkloadsActive);                // sceAgcAcbSetWorkloadsActive
	LIB_FUNC("FcgdDM3MB+k", Agc::AgcAcbSetWorkloadStreamInactive);         // sceAgcAcbSetWorkloadStreamInactive
	LIB_FUNC("GPbUp9jXQa8", Agc::AgcAcbWaitUntilSafeForRendering);         // sceAgcAcbWaitUntilSafeForRendering
	LIB_FUNC("d4NZIlguzv0", Agc::AgcAsyncWriteDataPatchSetAddressOrOffset); // sceAgcAsyncWriteDataPatchSetAddressOrOffset
	LIB_FUNC("y5K5tPktiL8", Agc::AgcAsyncWriteDataPatchSetCachePolicy);    // sceAgcAsyncWriteDataPatchSetCachePolicy
	LIB_FUNC("EJBA4dbmvfg", Agc::AgcAsyncWriteDataPatchSetDst);            // sceAgcAsyncWriteDataPatchSetDst
	LIB_FUNC("eWaWyFegzgQ", Agc::AgcAsyncRewindPatchSetRewindState);       // sceAgcAsyncRewindPatchSetRewindState
	LIB_FUNC("7Wa3aeJgeVU", Agc::AgcBranchPatchSetThenTarget);             // sceAgcBranchPatchSetThenTarget
	LIB_FUNC("rP5xLdOf26k", Agc::AgcBranchPatchSetElseTarget);             // sceAgcBranchPatchSetElseTarget
	LIB_FUNC("GXBlM-ekzrI", Agc::AgcBranchPatchSetCompareAddress);         // sceAgcBranchPatchSetCompareAddress
	LIB_FUNC("uZW-mqsxkrM", Agc::AgcCbBranchGetSize);                      // sceAgcCbBranchGetSize
	LIB_FUNC("7toV+elXqNM", Agc::AgcCbCondWrite);                          // sceAgcCbCondWrite
	LIB_FUNC("FuVbkyKlf+s", Agc::AgcCbCondWriteGetSize);                   // sceAgcCbCondWriteGetSize
	LIB_FUNC("6nths4DHNrs", Agc::AgcCbMemsetExclusive);                    // sceAgcCbMemsetExclusive
	LIB_FUNC("TGEZzUWLbrc", Agc::AgcCbSetUcRegistersDirectGetSize);       // sceAgcCbSetUcRegistersDirectGetSize
	LIB_FUNC("yUBESvCCJ4I", Agc::AgcCbSetShRegistersDirectGetSize);       // sceAgcCbSetShRegistersDirectGetSize
	LIB_FUNC("JOWmDrl+j20", Agc::AgcCbSetUcRegisterRangeDirectGetSize);   // sceAgcCbSetUcRegisterRangeDirectGetSize
	LIB_FUNC("MDLD5Ly94Xk", Agc::AgcCbSetUcRegisterRangeDirect);           // sceAgcCbSetUcRegisterRangeDirect
	LIB_FUNC("1-gUn1PI4Sw", Agc::AgcDcbAtomicMem);                         // sceAgcDcbAtomicMem
	LIB_FUNC("oz6zQq1JwCE", Agc::AgcDcbAtomicMemGetSize);                  // sceAgcDcbAtomicMemGetSize
	LIB_FUNC("zARR5aCmkoY", Agc::AgcDcbAtomicGds);                         // sceAgcDcbAtomicGds
	LIB_FUNC("1tB0xkLNjcw", Agc::AgcDcbAtomicGdsGetSize);                  // sceAgcDcbAtomicGdsGetSize
	LIB_FUNC("ms1xVoZ-Vwc", Agc::AgcDcbBeginOcclusionQueryGetSize);        // sceAgcDcbBeginOcclusionQueryGetSize
	LIB_FUNC("2ccJz9LQI+w", Agc::AgcDcbDmaDataGetSize);                    // sceAgcDcbDmaDataGetSize
	LIB_FUNC("mStuvI0zOtc", Agc::AgcDcbDrawIndexIndirectGetSize);         // sceAgcDcbDrawIndexIndirectGetSize
	LIB_FUNC("r98I08t+LOg", Agc::AgcDcbDrawIndexIndirectMultiGetSize);    // sceAgcDcbDrawIndexIndirectMultiGetSize
	LIB_FUNC("pYoKs3lPy88", Agc::AgcDcbDrawIndirectMultiGetSize);         // sceAgcDcbDrawIndirectMultiGetSize
	LIB_FUNC("P1CugZ99Uzc", Agc::AgcDcbEndOcclusionQueryGetSize);          // sceAgcDcbEndOcclusionQueryGetSize
	LIB_FUNC("rUuVjyR+Rd4", Agc::AgcDcbGetLodStatsGetSize);                // sceAgcDcbGetLodStatsGetSize
	LIB_FUNC("G0jrLdvEqDw", Agc::AgcDcbMemSemaphore);                      // sceAgcDcbMemSemaphore
	LIB_FUNC("jt3pl7EN17o", Agc::AgcDcbPrimeUtcl2);                        // sceAgcDcbPrimeUtcl2
	LIB_FUNC("KjPeVduz6jU", Agc::AgcDcbPrimeUtcl2GetSize);                 // sceAgcDcbPrimeUtcl2GetSize
	LIB_FUNC("N7M3+N-sBCw", Agc::AgcDcbPushMarkerSpan);                    // sceAgcDcbPushMarkerSpan
	LIB_FUNC("zg6u-N6Otxs", Agc::AgcDcbQueueEndOfShaderActionGetSize);    // sceAgcDcbQueueEndOfShaderActionGetSize
	LIB_FUNC("9S4noWrUI0s", Agc::AgcDcbSetBaseDispatchIndirectArgsGetSize); // sceAgcDcbSetBaseDispatchIndirectArgsGetSize
	LIB_FUNC("MMlmJAL7N5w", Agc::AgcDcbSetBaseDrawIndirectArgsGetSize);    // sceAgcDcbSetBaseDrawIndirectArgsGetSize
	LIB_FUNC("yheJGN-ay+A", Agc::AgcDcbSetBoolPredicationEnableGetSize);  // sceAgcDcbSetBoolPredicationEnableGetSize
	LIB_FUNC("GBCh3zCihoU", Agc::AgcDcbSetCxRegistersIndirectGetSize);    // sceAgcDcbSetCxRegistersIndirectGetSize
	LIB_FUNC("j4emHHndCPY", Agc::AgcDcbSetIndexBufferGetSize);             // sceAgcDcbSetIndexBufferGetSize
	LIB_FUNC("mljzuGDZRQ4", Agc::AgcDcbSetIndexCountGetSize);              // sceAgcDcbSetIndexCountGetSize
	LIB_FUNC("0o3VDdtA6nM", Agc::AgcDcbSetIndexIndirectArgs);              // sceAgcDcbSetIndexIndirectArgs
	LIB_FUNC("AFIh8SQkYlQ", Agc::AgcDcbSetIndexIndirectArgsGetSize);       // sceAgcDcbSetIndexIndirectArgsGetSize
	LIB_FUNC("ca4KPvp0qLQ", Agc::AgcDcbSetIndexSizeGetSize);               // sceAgcDcbSetIndexSizeGetSize
	LIB_FUNC("BYcSvEsINWU", Agc::AgcDcbSetMarkerSpan);                     // sceAgcDcbSetMarkerSpan
	LIB_FUNC("vLrBL8DQiz8", Agc::AgcDcbSetPredicationDisableGetSize);     // sceAgcDcbSetPredicationDisableGetSize
	LIB_FUNC("QhPDD513V0w", Agc::AgcDcbSetShRegisterDirectGetSize);       // sceAgcDcbSetShRegisterDirectGetSize
	LIB_FUNC("nNlUtdDDvZ0", Agc::AgcDcbSetShRegistersIndirectGetSize);    // sceAgcDcbSetShRegistersIndirectGetSize
	LIB_FUNC("aP1Ki9G3++4", Agc::AgcDcbSetUcRegisterDirectGetSize);       // sceAgcDcbSetUcRegisterDirectGetSize
	LIB_FUNC("UQGTw4xRlcM", Agc::AgcDcbSetUcRegistersIndirectGetSize);    // sceAgcDcbSetUcRegistersIndirectGetSize
	LIB_FUNC("FneFypEDRgY", Agc::AgcDcbSetWorkloadStreamInactive);         // sceAgcDcbSetWorkloadStreamInactive
	LIB_FUNC("+u6dKSLWM2o", Agc::AgcDcbStallCommandBufferParserGetSize);  // sceAgcDcbStallCommandBufferParserGetSize
	LIB_FUNC("XN+Iuu7XsM8", Agc::AgcDcbSetZPassPredicationEnableGetSize); // sceAgcDcbSetZPassPredicationEnableGetSize
	LIB_FUNC("hFQ9pUxoLQ4", Agc::AgcGetSemaphoreLabel);                    // sceAgcGetSemaphoreLabel
	LIB_FUNC("ICkECTBxrMw", Agc::AgcGetGsPrimPayload);                     // sceAgcGetGsPrimPayload
	LIB_FUNC("SwI6QxqwAC0", Agc::AgcGetShaderInstrumentation);             // sceAgcGetShaderInstrumentation
	LIB_FUNC("J8YCgfKAMQs", Agc::AgcQueueEndOfPipeActionPatchGcrCntl);    // sceAgcQueueEndOfPipeActionPatchGcrCntl
	LIB_FUNC("T9fjQIINoeE", Agc::AgcQueueEndOfPipeActionPatchType);        // sceAgcQueueEndOfPipeActionPatchType
	LIB_FUNC("RTpj-tIlvZc", Agc::AgcSetShaderInstrumentation);             // sceAgcSetShaderInstrumentation
	LIB_FUNC("n485EBnIWmk", Agc::AgcWaitRegMemPatchCompareFunction);      // sceAgcWaitRegMemPatchCompareFunction
	LIB_FUNC("eAy8eGNsCuU", Agc::AgcWriteDataPatchSetCachePolicy);         // sceAgcWriteDataPatchSetCachePolicy
	LIB_FUNC("tmy-+rBpspY", Agc::AgcWriteDataPatchSetDst);                 // sceAgcWriteDataPatchSetDst
}

} // namespace AgcVshBulk

} // namespace Libs

/*
REGISTRATION SNIPPET for src/libs/libs.cpp (append to the AgcVshStubs block area):

namespace AgcVshBulk {
LIB_DEFINE(InitAgcVshBulk_1);
} // namespace AgcVshBulk

... and inside InitAll(Loader::SymbolDatabase* s), next to the AgcVsh line:

	AgcVshStubs::InitAgcVshStubs_1(s);
	AgcVshBulk::InitAgcVshBulk_1(s);

(exact lines to add:
  namespace AgcVshBulk {
  LIB_DEFINE(InitAgcVshBulk_1);
  } // namespace AgcVshBulk
and:
  	AgcVshBulk::InitAgcVshBulk_1(s);
)
*/
