#include "common/abi.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <new>

// ps5-vsh: SceSysCore.elf libkernel close-out batch (trivially-safe only).
// Raw request keys verified verbatim (NID[Lib][LibVer]):
//   Unknown (pure tracers, no nids.csv row, all return 0):
//     5EThc7n+PpM/7BUHmsuhcq4/9ncS0kCRRSA/AhBZlHQYeA8/B70jHzr4Jj4/C3xgqN95IoE/
//     Cz48pcLnLCY/LVRydXoXt7U/PTDuAbCYfm0/R3QLLyp9MEU/W1Cg5WPD4IQ/c1xs7KJ+uf0/
//     cH6vM6sF5YA/cvFtKaAYbHE/guq+otVpgj0/hmqw8GlN+tI/yzvrk3YrPlE (17)
//   POSIX/termios/cpuset process queries (return 0 unless noted):
//     0Ma-LQjv6So cpuset_setaffinity/BiHHifIvX88 cpuset_setid/
//     DGY7qWLcHGA cpuset_getid/itioQwmJGys cpuset/5kBfZ40bAuk tcgetattr/
//     C1MQ6OaXRjo cfsetspeed/FxHW5NuEXsY tcflush/La7xW-jcZwQ tcsetattr/
//     S76vfDNu6eI tcsetsid/6mMQ1MSPW-Q chdir/AKknh8Cx-V8 chroot/
//     Q-AesC937KM swapon/Y2OqwJQ3lr8 sync/edJoqV2FIWs setsid/
//     gokWod7GAH4 getsid/ZtQi1QZANgc ptrace (-1)/9Iem9Wk07xU __sys_rdup/
//     bSDxEpGzmUE rfork_thread (-1)/Tzmvl2b3kLA openintr (-1)/
//     x-v44qRnzVA sceKernelAprCtrl/4pYihoPggn8 __inet_ntop/
//     fyPeCKJ94Hg __inet_pton (22)
// Absence check: every NID above grepped over src/libs -> all 39 clean, 0 skips.
// Semantics: safe tracers, no guest writes at all. Success-empty returns 0
// (caller ignores output on a PC); ptrace returns -1 (no tracing, EPERM-like);
// rfork_thread returns -1 (no rfork, caller falls back); openintr returns -1
// (no interruptible-open path). PRINT_NAME + LOGF NID+name+args, uint64_t x4
// args, null-safe, never halt.
// Explicitly NOT covered here (handled separately): NotifyAppStateChanged,
// NotifyAppEvent, SetAppState, GetAppState-variants, Budget*, Sandbox*,
// CanvasMap*, Suspend/Resume/Restore/Save, GetChildApp/GetParentApp,
// TerminateProcess, Icc*, ToolMemory*, Knobs, CpuUsage*, ResourceLimit/One,
// SocClock/Sensor/Temperature/CpuCoreClock, DirectMemoryQueryForId,
// M2/Kratos/Sandbox queries, switch/neomode clocks, Rdup, KqueueFromEqueue,
// TraceAutoCapture*, mdbg_*, ipmimgr_call, prepareTo*, SetMemoryPstate,
// GetProcessType*, is_development_mode, SetCoredumpAppInfo, Reserve2mbPage,
// GetToolMemoryRange, SetGpuCu, NotifyAppStateChanged.
// C++ names sanitize NIDs (+ -> P, - -> M) with a leading tag so no identifier
// starts with a digit; LIB_FUNC keys keep the raw NID verbatim.

namespace Libs {

namespace SysKern {

LIB_VERSION("SysKern", 1, "libkernel", 1, 1);

namespace Impl {

// ---- unknown NIDs (pure tracers, no nids.csv row; return 0) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_5EThc7nPPpM(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 5EThc7n+PpM Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_7BUHmsuhcq4(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 7BUHmsuhcq4 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_9ncS0kCRRSA(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 9ncS0kCRRSA Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_AhBZlHQYeA8(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t AhBZlHQYeA8 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_B70jHzr4Jj4(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t B70jHzr4Jj4 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_C3xgqN95IoE(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t C3xgqN95IoE Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_Cz48pcLnLCY(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Cz48pcLnLCY Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_LVRydXoXt7U(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t LVRydXoXt7U Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_PTDuAbCYfm0(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t PTDuAbCYfm0 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_R3QLLyp9MEU(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t R3QLLyp9MEU Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_W1Cg5WPD4IQ(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t W1Cg5WPD4IQ Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_c1xs7KJPuf0(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t c1xs7KJ+uf0 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_cH6vM6sF5YA(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cH6vM6sF5YA Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_cvFtKaAYbHE(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cvFtKaAYbHE Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_guqPotVpgj0(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t guq+otVpgj0 Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_hmqw8GlNPtI(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t hmqw8GlN+tI Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unk_yzvrk3YrPlE(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t yzvrk3YrPlE Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no nids.csv row
}

// ---- POSIX/termios/cpuset process queries (success-empty on a PC) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Cpuset_setaffinity_0MaMLQjv6So(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 0Ma-LQjv6So cpuset_setaffinity a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Cpuset_setid_BiHHifIvX88(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t BiHHifIvX88 cpuset_setid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Cpuset_getid_DGY7qWLcHGA(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t DGY7qWLcHGA cpuset_getid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Cpuset_itioQwmJGys(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t itioQwmJGys cpuset a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Tcgetattr_5kBfZ40bAuk(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 5kBfZ40bAuk tcgetattr a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Cfsetspeed_C1MQ6OaXRjo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t C1MQ6OaXRjo cfsetspeed a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Tcflush_FxHW5NuEXsY(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	LOGF("\t FxHW5NuEXsY tcflush a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Tcsetattr_La7xWMjcZwQ(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t La7xW-jcZwQ tcsetattr a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Tcsetsid_S76vfDNu6eI(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t S76vfDNu6eI tcsetsid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Chdir_6mMQ1MSPWMQ(const char* path) {
	PRINT_NAME();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	try {
#endif
	return POSIX_CALL(LibKernel::FileSystem::ChangeDirectory(path));
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	} catch (const std::bad_alloc&) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return -1;
	}
#endif
}

// Still a legacy success-only stub. GuestFsContext's fixed / root does NOT
// implement chroot, jail confinement, or native directory-object semantics.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Chroot_AKknh8CxMV8(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t AKknh8Cx-V8 chroot a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Swapon_QMAesC937KM(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Q-AesC937KM swapon a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Sync_Y2OqwJQ3lr8(uint64_t a0, uint64_t a1, uint64_t a2,
                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Y2OqwJQ3lr8 sync a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Setsid_edJoqV2FIWs(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t edJoqV2FIWs setsid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Getsid_gokWod7GAH4(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t gokWod7GAH4 getsid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Ptrace_ZtQi1QZANgc(uint64_t a0, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ZtQi1QZANgc ptrace a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no tracing; EPERM-like
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Sys_rdup_9Iem9Wk07xU(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 9Iem9Wk07xU __sys_rdup a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Rfork_thread_bSDxEpGzmUE(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t bSDxEpGzmUE rfork_thread a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no rfork; caller falls back
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Openintr_Tzmvl2b3kLA(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Tzmvl2b3kLA openintr a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no interruptible-open path
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelAprCtrl_xMv44qRnzVA(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	LOGF("\t x-v44qRnzVA sceKernelAprCtrl a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Inet_ntop_4pYihoPggn8(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 4pYihoPggn8 __inet_ntop a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Inet_pton_fyPeCKJ94Hg(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t fyPeCKJ94Hg __inet_pton a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty on a PC
}

} // namespace Impl

LIB_DEFINE(InitSysKern_1) {
	LIB_FUNC("5EThc7n+PpM", Impl::Unk_5EThc7nPPpM); // no nids.csv row
	LIB_FUNC("7BUHmsuhcq4", Impl::Unk_7BUHmsuhcq4); // no nids.csv row
	LIB_FUNC("9ncS0kCRRSA", Impl::Unk_9ncS0kCRRSA); // no nids.csv row
	LIB_FUNC("AhBZlHQYeA8", Impl::Unk_AhBZlHQYeA8); // no nids.csv row
	LIB_FUNC("B70jHzr4Jj4", Impl::Unk_B70jHzr4Jj4); // no nids.csv row
	LIB_FUNC("C3xgqN95IoE", Impl::Unk_C3xgqN95IoE); // no nids.csv row
	LIB_FUNC("Cz48pcLnLCY", Impl::Unk_Cz48pcLnLCY); // no nids.csv row
	LIB_FUNC("LVRydXoXt7U", Impl::Unk_LVRydXoXt7U); // no nids.csv row
	LIB_FUNC("PTDuAbCYfm0", Impl::Unk_PTDuAbCYfm0); // no nids.csv row
	LIB_FUNC("R3QLLyp9MEU", Impl::Unk_R3QLLyp9MEU); // no nids.csv row
	LIB_FUNC("W1Cg5WPD4IQ", Impl::Unk_W1Cg5WPD4IQ); // no nids.csv row
	LIB_FUNC("c1xs7KJ+uf0", Impl::Unk_c1xs7KJPuf0); // no nids.csv row
	LIB_FUNC("cH6vM6sF5YA", Impl::Unk_cH6vM6sF5YA); // no nids.csv row
	LIB_FUNC("cvFtKaAYbHE", Impl::Unk_cvFtKaAYbHE); // no nids.csv row
	LIB_FUNC("guq+otVpgj0", Impl::Unk_guqPotVpgj0); // no nids.csv row
	LIB_FUNC("hmqw8GlN+tI", Impl::Unk_hmqw8GlNPtI); // no nids.csv row
	LIB_FUNC("yzvrk3YrPlE", Impl::Unk_yzvrk3YrPlE); // no nids.csv row
	LIB_FUNC("0Ma-LQjv6So", Impl::Cpuset_setaffinity_0MaMLQjv6So); // cpuset_setaffinity
	LIB_FUNC("BiHHifIvX88", Impl::Cpuset_setid_BiHHifIvX88); // cpuset_setid
	LIB_FUNC("DGY7qWLcHGA", Impl::Cpuset_getid_DGY7qWLcHGA); // cpuset_getid
	LIB_FUNC("itioQwmJGys", Impl::Cpuset_itioQwmJGys); // cpuset
	LIB_FUNC("5kBfZ40bAuk", Impl::Tcgetattr_5kBfZ40bAuk); // tcgetattr
	LIB_FUNC("C1MQ6OaXRjo", Impl::Cfsetspeed_C1MQ6OaXRjo); // cfsetspeed
	LIB_FUNC("FxHW5NuEXsY", Impl::Tcflush_FxHW5NuEXsY); // tcflush
	LIB_FUNC("La7xW-jcZwQ", Impl::Tcsetattr_La7xWMjcZwQ); // tcsetattr
	LIB_FUNC("S76vfDNu6eI", Impl::Tcsetsid_S76vfDNu6eI); // tcsetsid
	LIB_FUNC("6mMQ1MSPW-Q", Impl::Chdir_6mMQ1MSPWMQ); // chdir
	LIB_FUNC("AKknh8Cx-V8", Impl::Chroot_AKknh8CxMV8); // chroot
	LIB_FUNC("Q-AesC937KM", Impl::Swapon_QMAesC937KM); // swapon
	LIB_FUNC("Y2OqwJQ3lr8", Impl::Sync_Y2OqwJQ3lr8); // sync
	LIB_FUNC("edJoqV2FIWs", Impl::Setsid_edJoqV2FIWs); // setsid
	LIB_FUNC("gokWod7GAH4", Impl::Getsid_gokWod7GAH4); // getsid
	LIB_FUNC("ZtQi1QZANgc", Impl::Ptrace_ZtQi1QZANgc); // ptrace
	LIB_FUNC("9Iem9Wk07xU", Impl::Sys_rdup_9Iem9Wk07xU); // __sys_rdup
	LIB_FUNC("bSDxEpGzmUE", Impl::Rfork_thread_bSDxEpGzmUE); // rfork_thread
	LIB_FUNC("Tzmvl2b3kLA", Impl::Openintr_Tzmvl2b3kLA); // openintr
	LIB_FUNC("x-v44qRnzVA", Impl::SceKernelAprCtrl_xMv44qRnzVA); // sceKernelAprCtrl
	LIB_FUNC("4pYihoPggn8", Impl::Inet_ntop_4pYihoPggn8); // __inet_ntop
	LIB_FUNC("fyPeCKJ94Hg", Impl::Inet_pton_fyPeCKJ94Hg); // __inet_pton
}

} // namespace SysKern

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp,
inside namespace Libs next to the other bare LIB_DEFINE lines):

LIB_DEFINE(InitSysKern_1);

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   InitSysKern_1(s);
*/
