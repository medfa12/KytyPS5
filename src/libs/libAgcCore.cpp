#include "common/abi.h"
#include "kernel/eventQueue.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cmath>
#include <cstdint>

namespace Libs {

// ps5-vsh: AgcCompositor.elf boot1 unresolved-PLT close-out
// (_ProbeLogs/agc_boot1.log, 65 slots; Camera/Hmd/Av/Dipsw/Sisr/AgcDriver/
// AgcVsh covered by libAgcPeri.cpp + libAgcComp.cpp). This file: VideoOut
// (15), libkernel (10), RegMgr (2), Sysmodule-by-name (1), libc math (3).
// All NIDs verified absent from src/libs before adding. Tracers return
// success/empty unless noted; null-safe, no guest writes, never halt.

namespace AgcCoreVideoOut {

LIB_VERSION("VideoOut", 1, "VideoOut", 1, 1);

#define AGC_CORE_VO_STUB(func, nid)                                                 \
	static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2, \
	                                   uint64_t a3) {                        \
		PRINT_NAME();                                                    \
		LOGF("\t agc-videoout " #nid " -> 0\n");                         \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return 0;                                                        \
	}

AGC_CORE_VO_STUB(VoSysGetFlipStatus, 21sS8EU45cw) // sceVideoOutSysGetFlipStatus
AGC_CORE_VO_STUB(VoAddSetModeEvent, AjtlFa2HRws)  // sceVideoOutAddSetModeEvent
AGC_CORE_VO_STUB(VoCursorSetPos, N17AK88NdD0)     // sceVideoOutCursorSetPosition
AGC_CORE_VO_STUB(VoCursorMag2x, Se-Zs6koGzk)      // sceVideoOutCursorSet2xMagnify
AGC_CORE_VO_STUB(VoCursorEnable, UPZWCH8qTM4)     // sceVideoOutCursorEnable
AGC_CORE_VO_STUB(VoSysSetDrrPeg, cFy4rRDKZjQ)     // sceVideoOutSysSetDrrPeg
AGC_CORE_VO_STUB(VoCursorDisable, fEPEwROdCwM)    // sceVideoOutCursorDisable
AGC_CORE_VO_STUB(VoBlankAfterResume, zILy8t5bPo)  // sceVideoOutSetBlankAfterResume
AGC_CORE_VO_STUB(VoUnk_5AIEU9, Minus5AIEU9plus11g) // no nids.csv row
AGC_CORE_VO_STUB(VoUnk_4SZjpc4, 4SZjpc4U1es)       // no nids.csv row
AGC_CORE_VO_STUB(VoUnk_I7ms8Y, I7ms8YxoZio)       // no nids.csv row
AGC_CORE_VO_STUB(VoUnk_Ip78YC, Ip78YCQUrTo)       // no nids.csv row
AGC_CORE_VO_STUB(VoUnk_VKBIGf, VKBIGfxIuAA)       // no nids.csv row
AGC_CORE_VO_STUB(VoUnk_WUUufq, WUUplusufqgLJg)     // no nids.csv row
// pTxsULu4Nh0 queries the occupied privacy-mask buffer IDs (sceVideoOutSysGetPrivacyMaskStatus).
// AgcCompositor getPrivacyMaskAvailableId reads this bitmask after success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VoUnk_pTxsUL(uint64_t a0, uint64_t a1,
                                                                                   uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	if (a0 != 0) {
		*reinterpret_cast<uint64_t*>(a0) = 0;
	}
	return 0;
}

#undef AGC_CORE_VO_STUB

LIB_DEFINE(InitAgcCoreVideoOut_1) {
	LIB_FUNC("21sS8EU45cw", VoSysGetFlipStatus);
	LIB_FUNC("AjtlFa2HRws", VoAddSetModeEvent);
	LIB_FUNC("N17AK88NdD0", VoCursorSetPos);
	LIB_FUNC("Se-Zs6koGzk", VoCursorMag2x);
	LIB_FUNC("UPZWCH8qTM4", VoCursorEnable);
	LIB_FUNC("cFy4rRDKZjQ", VoSysSetDrrPeg);
	LIB_FUNC("fEPEwROdCwM", VoCursorDisable);
	LIB_FUNC("zILy8t+5bPo", VoBlankAfterResume);
	LIB_FUNC("-5AIEU9+11g", VoUnk_5AIEU9);
	LIB_FUNC("4SZjpc4U1es", VoUnk_4SZjpc4);
	LIB_FUNC("I7ms8YxoZio", VoUnk_I7ms8Y);
	LIB_FUNC("Ip78YCQUrTo", VoUnk_Ip78YC);
	LIB_FUNC("VKBIGfxIuAA", VoUnk_VKBIGf);
	LIB_FUNC("WUU+ufqgLJg", VoUnk_WUUufq);
	LIB_FUNC("pTxsULu4Nh0", VoUnk_pTxsUL);
}

} // namespace AgcCoreVideoOut

namespace AgcCoreKernel {

LIB_VERSION("AgcCoreKernel", 1, "libkernel", 1, 1);

#define AGC_CORE_K_STUB(func, nid)                                                  \
	static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2, \
	                                   uint64_t a3) {                        \
		PRINT_NAME();                                                    \
		LOGF("\t agc-kernel " #nid " -> 0\n");                           \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return 0;                                                        \
	}

AGC_CORE_K_STUB(KSetProcessProperty, MinusW4xI5aVI8w) // sceKernelSetProcessProperty
AGC_CORE_K_STUB(KIsDebuggerAttached, K0ZTCJ-wFrM)     // sceKernelIsDebuggerAttached
AGC_CORE_K_STUB(KDebugGetAppStatus, XECwgo2nbek)       // sceKernelDebugGetAppStatus
AGC_CORE_K_STUB(KProtectForPID, cBp0Ad-24LA)          // sceKernelProtectDirectMemoryForPID
AGC_CORE_K_STUB(KAllocForApp, fNoviuIwykc)            // sceKernelAllocateDirectMemoryForApp
AGC_CORE_K_STUB(KGetAppCategory, fh-Bb3wl0Vs)         // sceKernelGetAppCategoryTypeByAppId
AGC_CORE_K_STUB(KProtectDirect, k1jIkFHa9OU)          // sceKernelProtectDirectMemory
AGC_CORE_K_STUB(KUnk_1v23w7, 1v23w7Rnkic)              // no nids.csv row
AGC_CORE_K_STUB(KUnk_TVB77y, TVB77yVaPlusRU)           // no nids.csv row

#undef AGC_CORE_K_STUB

LIB_DEFINE(InitAgcCoreKernel_1) {
	LIB_FUNC("-W4xI5aVI8w", KSetProcessProperty);
	LIB_FUNC("JxJ4tfgKlXA", ::Libs::LibKernel::EventQueue::KernelDeleteReadEvent);
	LIB_FUNC("K0ZTCJ-wFrM", KIsDebuggerAttached);
	LIB_FUNC("XECwgo2nbek", KDebugGetAppStatus);
	LIB_FUNC("cBp0Ad-24LA", KProtectForPID);
	LIB_FUNC("fNoviuIwykc", KAllocForApp);
	LIB_FUNC("fh-Bb3wl0Vs", KGetAppCategory);
	LIB_FUNC("k1jIkFHa9OU", KProtectDirect);
	LIB_FUNC("1v23w7Rnkic", KUnk_1v23w7);
	LIB_FUNC("TVB77yVa+RU", KUnk_TVB77y);
}

} // namespace AgcCoreKernel

namespace AgcCoreMisc {

LIB_VERSION("AgcCoreMisc", 1, "RegMgr", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegGetRealMachineType(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	LOGF("\t agc-regmgr x0oTVOySvTU sceRegMgrSrvGetRealMachineType -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegIsChange(uint64_t a0, uint64_t a1, uint64_t a2,
                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t agc-regmgr xhrI4zhlBuA sceRegMgrIsChange -> 0 (no change)\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegGetMachineType(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t agc-regmgr 04zv+0zNJkg sceRegMgrSrvGetMachineType -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegGetQAF(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t agc-regmgr wQlp94zzrWg sceRegMgrSrvGetQAFforReg -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitAgcCoreMisc_1) {
	LIB_FUNC("x0oTVOySvTU", RegGetRealMachineType);
	LIB_FUNC("xhrI4zhlBuA", RegIsChange);
	LIB_FUNC("04zv+0zNJkg", RegGetMachineType); // sceRegMgrSrvGetMachineType
	LIB_FUNC("wQlp94zzrWg", RegGetQAF);         // sceRegMgrSrvGetQAFforReg
}

} // namespace AgcCoreMisc

namespace AgcCoreSysmod {

LIB_VERSION("AgcCoreSysmod", 1, "Sysmodule", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnloadByName(uint64_t a0, uint64_t a1, uint64_t a2,
                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t agc-sysmod vpTHmA6Knvg sceSysmoduleUnloadModuleByNameInternal -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitAgcCoreSysmod_1) {
	LIB_FUNC("vpTHmA6Knvg", UnloadByName);
}

} // namespace AgcCoreSysmod

namespace AgcCoreMath {

LIB_VERSION("AgcCoreMath", 1, "LibcInternal", 1, 1);

static float __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Log2f(float x) {
	PRINT_NAME();
	return std::log2f(x);
}

static float __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Exp2f(float x) {
	PRINT_NAME();
	return std::exp2f(x);
}

static float __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Ldexpf(float x, int e) {
	PRINT_NAME();
	return std::ldexpf(x, e);
}

LIB_DEFINE(InitAgcCoreMath_1) {
	LIB_FUNC("hsi9drzHR2k", Log2f);  // log2f (real)
	LIB_FUNC("wuAQt-j+p4o", Exp2f);  // exp2f (real)
	LIB_FUNC("kn0yiYeExgA", Ldexpf); // ldexpf (real)
}

} // namespace AgcCoreMath

namespace AgcCore {

LIB_DEFINE(InitAgcCore_1) {
	AgcCoreVideoOut::InitAgcCoreVideoOut_1(s);
	AgcCoreKernel::InitAgcCoreKernel_1(s);
	AgcCoreMisc::InitAgcCoreMisc_1(s);
	AgcCoreSysmod::InitAgcCoreSysmod_1(s);
	AgcCoreMath::InitAgcCoreMath_1(s);
}

} // namespace AgcCore

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp ----
//
// namespace AgcCore {
// LIB_DEFINE(InitAgcCore_1);
// } // namespace AgcCore
//
// ... and inside Libs::InitAll:
//   AgcCore::InitAgcCore_1(s);
