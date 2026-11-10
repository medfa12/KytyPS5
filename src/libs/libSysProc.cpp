#include "common/abi.h"
#include "libs/libs.h"
#include "libs/ipmiObserve.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

// ps5-vsh: kqueue bridge lives in libC.cpp (LibcInternal).
namespace LibcInternal {
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KqueueFdForEqueue(int64_t eq, int* out_kq);
}

// ps5-vsh: SceSysCore.elf boot1 unresolved-PLT close-out, process-core batch
// (libkernel NIDs not covered by libSysKern.cpp). Conventions: per-area
// namespaces with LIB_VERSION("SysProc",1,"libkernel",1,1), master
// LIB_DEFINE(InitSysProc_1) + footer snippet. All impls PRINT_NAME()+LOGF,
// null-safe, no guest writes, never halt. Only success-empty (0), explicit
// failure (-1/ESRCH-shaped), or fake handles where the guest stores them.

namespace SysProcState {

LIB_VERSION("SysProc", 1, "libkernel", 1, 1);

#define SYSPROC_OK(func, nid)                                                    \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2,  \
	                              uint64_t a3) {                         \
		PRINT_NAME();                                                    \
		LOGF("\t sysproc " #nid " -> 0\n");                              \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return 0;                                                        \
	}

SYSPROC_OK(NotifyAppStateChanged, 2z1Tkrqh8jE) // sceKernelNotifyAppStateChanged
SYSPROC_OK(NotifyAppEvent, v3LXmnWVWXk)         // sceKernelNotifyAppEvent
SYSPROC_OK(SetAppState, QU57kCwF81U)           // sceKernelSetAppState
SYSPROC_OK(SetAppInfo, CkomU+GYCQ8)            // sceKernelSetAppInfo
SYSPROC_OK(SetKnobs, kyifJypWYrM)              // sceKernelSetKnobs
SYSPROC_OK(SetMemoryPstate, A1zOC17L80g)       // sceKernelSetMemoryPstate
SYSPROC_OK(SetGpuCu, 4nAp4pZgV1A)              // sceKernelSetGpuCu
SYSPROC_OK(SetCoredumpAppInfo, 9LBcV3rS10Q)    // sceKernelSetCoredumpAppInfo
SYSPROC_OK(SetBaseModeClock, V0ey5Vz5F7k)      // sceKernelSetBaseModeClock
SYSPROC_OK(SetNeoModeClock, EA4bmgm02o8)       // sceKernelSetNeoModeClock
SYSPROC_OK(SwitchToNeoMode, +DVcq4YF6Xg)       // sceKernelSwitchToNeoMode
SYSPROC_OK(SwitchToBaseMode, KK6B72YC-js)      // sceKernelSwitchToBaseMode
SYSPROC_OK(SwitchToNativeMode, Op5E4tzSoiU)    // sceKernelSwitchToNativeMode
SYSPROC_OK(SuspendProcess, NvRYDns8Ydc)        // sceKernelSuspendProcess
SYSPROC_OK(ResumeProcess, iZ5it+3LGQM)         // sceKernelResumeProcess
SYSPROC_OK(RestoreApp, Bc0S7C2y0Ng)            // sceKernelRestoreApp
SYSPROC_OK(SaveApp, uq1imV74tBs)               // placeholder, replaced below
SYSPROC_OK(TerminateProcess, VinlCA8Vkg8)      // sceKernelTerminateProcess (no-op)
SYSPROC_OK(SetSuspendState, ihf-xuMiekk)       // sceKernelSetSuspendState
SYSPROC_OK(PrepareToSuspend, 5ifAYsFspMM)      // sceKernelPrepareToSuspendProcess
SYSPROC_OK(PrepareToResume, -ajib2lKj-c)       // sceKernelPrepareToResumeProcess
SYSPROC_OK(CreateBudget, BVafm-M0GNw)          // sceKernelCreateBudget
SYSPROC_OK(DeleteBudget, vs+KPSoE7AM)          // sceKernelDeleteBudget
SYSPROC_OK(SetBudget, nzuKTmqQFXY)             // sceKernelSetBudget
SYSPROC_OK(GetBudget, OX9wtyjVAJY)             // sceKernelGetBudget
SYSPROC_OK(AddToCanvasMap, 5-rYyD+-vHQ)        // sceKernelAddProcessToCanvasMap
SYSPROC_OK(DeleteFromCanvasMap, iSGc7kcf8X4)   // sceKernelDeleteProcessFromCanvasMap
SYSPROC_OK(Reserve2mbPage, VjWR6g9qL2k)        // sceKernelReserve2mbPage
SYSPROC_OK(GetToolMemoryRange, dkBx0YqFQ+Y)    // sceKernelGetToolMemoryRange
SYSPROC_OK(AllocateToolMemory, 45Yurf7lZmU)    // sceKernelAllocateToolMemory
SYSPROC_OK(ReleaseToolMemory, gO98NioN5FM)     // sceKernelReleaseToolMemory
SYSPROC_OK(GetDefaultToolMemorySize, IdQuLJhnPAQ) // sceKernelGetDefaultToolMemorySize
SYSPROC_OK(GetCpuUsageProc, VS45LnNqyCo)       // sceKernelGetCpuUsageProc
SYSPROC_OK(GetCpuUsageProc2, QbdkmhQQfIM)      // sceKernelGetCpuUsageProc2
SYSPROC_OK(GetResourceLimit, S2-4Sh3qnm8)      // sceKernelGetResourceLimit
SYSPROC_OK(GetResourceOne, TQxbrxPdIxU)        // sceKernelGetResourceOne
SYSPROC_OK(GetSocClock, tSFK-c43YAI)           // sceKernelGetSocClock
SYSPROC_OK(GetSocSensorTemp, zzKyvoELf4I)      // sceKernelGetSocSensorTemperature
SYSPROC_OK(GetCpuCoreClock, 8Ejov5Yq8ck)       // sceKernelGetCpuCoreClock
SYSPROC_OK(GetCpuTemperature, qiL4fFObAxM)     // sceKernelGetCpuTemperature
SYSPROC_OK(DirectMemoryQueryForId, ZB9HZnouJrc) // sceKernelDirectMemoryQueryForId
SYSPROC_OK(IsM2Attached, mC57yegC040)          // sceKernelIsM2DeviceAttached (absent)
SYSPROC_OK(IsKratos, miPa6vSAqos)               // sceKernelIsKratos (absent)
SYSPROC_OK(IsInSandbox, xeu-pV8wkKs)           // sceKernelIsInSandbox (no)
SYSPROC_OK(TraceCreate, A3MDcIJe1tg)           // sceKernelTraceAutoCaptureCreate
SYSPROC_OK(TraceDelete, 4uDeONzVc04)           // sceKernelTraceAutoCaptureDelete
SYSPROC_OK(TraceStart, SoN+jQ6T788)            // sceKernelTraceAutoCaptureStart
SYSPROC_OK(TraceStop, TqLPe9eIbng)             // sceKernelTraceAutoCaptureStop
SYSPROC_OK(MdbgCall, 7KuwdAt0kQQ)              // mdbg_call
SYSPROC_OK(MdbgService, LYbbZxXNAns)           // mdbg_service
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IpmimgrCall(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	const auto caller = KYTY_IPMI_CALLER;
	IpmiObserve::Emit(IpmiObserve::Bank::Gateway, IpmiObserve::Event::GatewayEnter, {a0, a1, a2, a3, caller});
	PRINT_NAME();
	LOGF("\t sysproc Hk7iHmGxB18 -> 0\n");
	IpmiObserve::Emit(IpmiObserve::Bank::Gateway, IpmiObserve::Event::GatewayExit, {a0, a1, a2, a3, caller, 0});
	return 0;
}
SYSPROC_OK(BeginAppMount, H69MYqpNjCc)         // sceKernelBeginAppMount
SYSPROC_OK(GetParentApp, T8ZlojCdON0)          // sceKernelGetParentApp
SYSPROC_OK(GetProcessType, +g+UP8Pyfmo)        // sceKernelGetProcessType
SYSPROC_OK(GetProcessTypeOfBudget, 4P1e01mbenA) // sceKernelGetProcessTypeOfBudget
SYSPROC_OK(IsDevelopmentMode, O-hEvSnv2o4)     // __sys_is_development_mode (retail)
SYSPROC_OK(SandboxPath, ASVb7Y-pR4Y)               // sceKernelSandboxPath (no write)

#undef SYSPROC_OK

LIB_DEFINE(InitSysProcState_1) {
	LIB_FUNC("2z1Tkrqh8jE", NotifyAppStateChanged);
	LIB_FUNC("v3LXmnWVWXk", NotifyAppEvent);
	LIB_FUNC("QU57kCwF81U", SetAppState);
	LIB_FUNC("CkomU+GYCQ8", SetAppInfo);
	LIB_FUNC("kyifJypWYrM", SetKnobs);
	LIB_FUNC("A1zOC17L80g", SetMemoryPstate);
	LIB_FUNC("4nAp4pZgV1A", SetGpuCu);
	LIB_FUNC("9LBcV3rS10Q", SetCoredumpAppInfo);
	LIB_FUNC("V0ey5Vz5F7k", SetBaseModeClock);
	LIB_FUNC("EA4bmgm02o8", SetNeoModeClock);
	LIB_FUNC("+DVcq4YF6Xg", SwitchToNeoMode);
	LIB_FUNC("KK6B72YC-js", SwitchToBaseMode);
	LIB_FUNC("Op5E4tzSoiU", SwitchToNativeMode);
	LIB_FUNC("NvRYDns8Ydc", SuspendProcess);
	LIB_FUNC("iZ5it+3LGQM", ResumeProcess);
	LIB_FUNC("Bc0S7C2y0Ng", RestoreApp);
	LIB_FUNC("uq1imV74tBs", SaveApp);
	LIB_FUNC("VinlCA8Vkg8", TerminateProcess);
	LIB_FUNC("ihf-xuMiekk", SetSuspendState);
	LIB_FUNC("5ifAYsFspMM", PrepareToSuspend);
	LIB_FUNC("-ajib2lKj-c", PrepareToResume);
	LIB_FUNC("BVafm-M0GNw", CreateBudget);
	LIB_FUNC("vs+KPSoE7AM", DeleteBudget);
	LIB_FUNC("nzuKTmqQFXY", SetBudget);
	LIB_FUNC("OX9wtyjVAJY", GetBudget);
	LIB_FUNC("5-rYyD+-vHQ", AddToCanvasMap);
	LIB_FUNC("iSGc7kcf8X4", DeleteFromCanvasMap);
	LIB_FUNC("VjWR6g9qL2k", Reserve2mbPage);
	LIB_FUNC("dkBx0YqFQ+Y", GetToolMemoryRange);
	LIB_FUNC("45Yurf7lZmU", AllocateToolMemory);
	LIB_FUNC("gO98NioN5FM", ReleaseToolMemory);
	LIB_FUNC("IdQuLJhnPAQ", GetDefaultToolMemorySize);
	LIB_FUNC("VS45LnNqyCo", GetCpuUsageProc);
	LIB_FUNC("QbdkmhQQfIM", GetCpuUsageProc2);
	LIB_FUNC("S2-4Sh3qnm8", GetResourceLimit);
	LIB_FUNC("TQxbrxPdIxU", GetResourceOne);
	LIB_FUNC("tSFK-c43YAI", GetSocClock);
	LIB_FUNC("zzKyvoELf4I", GetSocSensorTemp);
	LIB_FUNC("8Ejov5Yq8ck", GetCpuCoreClock);
	LIB_FUNC("qiL4fFObAxM", GetCpuTemperature);
	LIB_FUNC("ZB9HZnouJrc", DirectMemoryQueryForId);
	LIB_FUNC("mC57yegC040", IsM2Attached);
	LIB_FUNC("miPa6vSAqos", IsKratos);
	LIB_FUNC("xeu-pV8wkKs", IsInSandbox);
	LIB_FUNC("A3MDcIJe1tg", TraceCreate);
	LIB_FUNC("4uDeONzVc04", TraceDelete);
	LIB_FUNC("SoN+jQ6T788", TraceStart);
	LIB_FUNC("TqLPe9eIbng", TraceStop);
	LIB_FUNC("7KuwdAt0kQQ", MdbgCall);
	LIB_FUNC("LYbbZxXNAns", MdbgService);
	LIB_FUNC("Hk7iHmGxB18", IpmimgrCall);
	LIB_FUNC("H69MYqpNjCc", BeginAppMount);
	LIB_FUNC("T8ZlojCdON0", GetParentApp);
	LIB_FUNC("+g+UP8Pyfmo", GetProcessType);
	LIB_FUNC("4P1e01mbenA", GetProcessTypeOfBudget);
	LIB_FUNC("O-hEvSnv2o4", IsDevelopmentMode);
	LIB_FUNC("ASVb7Y-pR4Y", SandboxPath);
}

} // namespace SysProcState

namespace SysProcIds {

LIB_VERSION("SysProc", 1, "libkernel", 1, 1);

#define SYSPROC_FAIL(func, nid, rc)                                          \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2,  \
	                              uint64_t a3) {                         \
		PRINT_NAME();                                                    \
		LOGF("\t sysproc " #nid " -> %d\n", rc);                         \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return rc;                                                       \
	}

SYSPROC_FAIL(GetChildApp, tVCSUib8C4Y, -1) // sceKernelGetChildApp (none yet)
SYSPROC_FAIL(Rdup, LxIacrWiJ3w, -1)        // sceKernelRdup (no duplicate)
// sceKernelGetKqueueFromEqueue: real impl below (wraps the kevent bridge).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KqueueFromEqueue(uint64_t a0, uint64_t a1, uint64_t, uint64_t) {
	PRINT_NAME();
	const int ret = LibcInternal::KqueueFdForEqueue(static_cast<int64_t>(a0),
	                                                reinterpret_cast<int*>(a1));
	LOGF("\t sysproc QyrxcdBrb0M -> %d\n", ret);
	return ret;
}
SYSPROC_FAIL(Fdatasync, 30Rh4ixbKy4, -1)   // sceKernelFdatasync (no-op fail)
SYSPROC_FAIL(SetFsbase, 3SVaehJvYFk, -1)   // amd64_set_fsbase (no TLS base ctl)
SYSPROC_FAIL(Wait4, UFKprwxNhT8, -1)       // wait4 (no child wait)
SYSPROC_FAIL(Execve, kdguLiAheLI, -1)      // execve (no exec)
SYSPROC_FAIL(SetNameNp, oxMp8uPqa+U, -1)   // pthread_set_name_np (log-only below)

#undef SYSPROC_FAIL

LIB_DEFINE(InitSysProcIds_1) {
	LIB_FUNC("tVCSUib8C4Y", GetChildApp);
	LIB_FUNC("LxIacrWiJ3w", Rdup);
	LIB_FUNC("QyrxcdBrb0M", KqueueFromEqueue);
	LIB_FUNC("30Rh4ixbKy4", Fdatasync);
	LIB_FUNC("3SVaehJvYFk", SetFsbase);
	LIB_FUNC("UFKprwxNhT8", Wait4);
	LIB_FUNC("kdguLiAheLI", Execve);
	LIB_FUNC("oxMp8uPqa+U", SetNameNp);
}

} // namespace SysProcIds

namespace SysProcIcc {

LIB_VERSION("SysProc", 1, "libkernel", 1, 1);

#define SYSPROC_ICC(func, nid)                                               \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2,  \
	                              uint64_t a3) {                         \
		PRINT_NAME();                                                    \
		LOGF("\t sysproc " #nid " -> 0 (no crash report)\n");            \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return 0;                                                        \
	}

SYSPROC_ICC(IccGetEmcCrashReport, 4NEVlgkAjNk)
SYSPROC_ICC(IccGetCrashNumBlocks, G0KaQ+fpNWY)
SYSPROC_ICC(IccClearCrashReport, qq-gRlUwTPk)
SYSPROC_ICC(IccGetBootTimestamp, kDfWvHZH3eM)

#undef SYSPROC_ICC

LIB_DEFINE(InitSysProcIcc_1) {
	LIB_FUNC("4NEVlgkAjNk", IccGetEmcCrashReport);
	LIB_FUNC("G0KaQ+fpNWY", IccGetCrashNumBlocks);
	LIB_FUNC("qq-gRlUwTPk", IccClearCrashReport);
	LIB_FUNC("kDfWvHZH3eM", IccGetBootTimestamp);
}

} // namespace SysProcIcc

namespace SysProc {

LIB_DEFINE(InitSysProc_1) {
	SysProcState::InitSysProcState_1(s);
	SysProcIds::InitSysProcIds_1(s);
	SysProcIcc::InitSysProcIcc_1(s);
}

} // namespace SysProc

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp ----
//
// namespace SysProc {
// LIB_DEFINE(InitSysProc_1);
// } // namespace SysProc
//
// ... and inside Libs::InitAll:
//   SysProc::InitSysProc_1(s);
