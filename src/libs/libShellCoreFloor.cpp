#include "common/abi.h"
#include "common/assert.h"
#include "common/singleton.h"
#include "kernel/fileSystem.h"
#include "kernel/socketpairProvenance.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libGuestFile.h"
#include "libs/guestDirectory.h"
#include "kernel/eventQueue.h"
#include "kernel/eventFlag.h"
#include "libs/guestHeap.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "libs/libShellCoreFloor.h"
#include "libs/processSdk.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <cinttypes>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_set>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>
#endif

// ps5-vsh ShellCore boot STEP 1 — kernel/libc floor only.
//
// Covers the libkernel (181 NIDs) + libSceLibcInternal (190 NIDs) imports of
// SceShellCore.elf (_ProbeLogs/shellcore_imports_full.txt) that have no
// implementation anywhere under src/libs. Verified absent via per-NID grep
// before writing; NpCommon/NpManager/FsVsh/SysCore are owned by other steps
// and are NOT touched here.
//
// Conventions (match libC.cpp XlengthError region + libPs5VshVideo.cpp):
// - C++ EH personality/__cxa_*/_Unwind_Resume: LOGF THROW tracers that
//   return 0 and never unwind.
// - Queries: sceKernelGetCompiledSdkVersionByPid family returns the SDK 14.0
//   version int, GetAppState reports running, IsCloudkit reports false.
// - Everything is null-safe and host-safe: guest out-params are written only
//   after a writability probe, guest strings go through SEH-guarded host
//   calls, guest FILE* handles are never forwarded to host libc, and
//   terminate/abort-style entries never terminate the host.
// - LIB_VERSION tags use the exact Library-column names from the imports
//   file ("libkernel", "libSceLibcInternal") so resolution exact-matches
//   instead of relying on NID fallback.

namespace Libs {
namespace ShellCoreFloor {

namespace {

// SDK 14.0 version int: (major << 24) | (minor << 16).
constexpr uint32_t kSdk140 = 0x0E000000u;
// Floor firmware version reported by *SystemSwVersion-style outs (13.60).
constexpr uint32_t kFw1360 = 0x0D3C0000u;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// SEH-guarded host call: any guest-pointer fault inside expr falls back
// instead of crashing the host. POD-only call sites (no C++ unwinding).
#define FLOOR_SEH_RETURN(fallback, expr)                                                       \
	__try {                                                                                    \
		return (expr);                                                                         \
	} __except (EXCEPTION_EXECUTE_HANDLER) {                                                   \
		return (fallback);                                                                     \
	}
#else
#define FLOOR_SEH_RETURN(fallback, expr) return (expr);
#endif

// True when [v, v + n) is mapped with the requested access. Rejects null,
// wrap-around and low (never-a-guest-pointer) addresses before probing.
static bool FloorRangeOk(uint64_t v, size_t n, bool wantWrite) {
	if (v == 0 || n == 0) {
		return false;
	}
	if (v + n < v) {
		return false;
	}
	if (v < 0x10000u) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(v)), &info,
	                 sizeof(info)) == 0) {
		return false;
	}
	if (info.State != MEM_COMMIT) {
		return false;
	}
	const uint64_t base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
	const uint64_t size = static_cast<uint64_t>(info.RegionSize);
	if (v < base || v + n > base + size) {
		return false;
	}
	const DWORD p = info.Protect & 0xffu;
	if (wantWrite) {
		return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
		       p == PAGE_EXECUTE_WRITECOPY;
	}
	return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
	       p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
#else
	(void)wantWrite;
	return true;
#endif
}

static bool FloorWrite(uint64_t dst, const void* src, size_t n) {
	if (src == nullptr || !FloorRangeOk(dst, n, true)) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	__try {
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)), src, n);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
#else
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)), src, n);
	return true;
#endif
}

static bool FloorWriteU32(uint64_t dst, uint32_t v) {
	return FloorWrite(dst, &v, sizeof(v));
}

static bool FloorWriteU64(uint64_t dst, uint64_t v) {
	return FloorWrite(dst, &v, sizeof(v));
}

static bool FloorWriteByte(uint64_t dst, uint8_t v) {
	return FloorWrite(dst, &v, sizeof(v));
}

static uint64_t FloorCaller() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return reinterpret_cast<uint64_t>(_ReturnAddress());
#else
	return 0u;
#endif
}

// Bounded printable preview of a guest string. Never crashes (probe first).
static void FloorPreview(uint64_t v) {
	if (!FloorRangeOk(v, 1, false)) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	char   buf[48] = {};
	size_t len     = 0;
	__try {
		for (; len < sizeof(buf) - 1; ++len) {
			const char c = reinterpret_cast<const char*>(static_cast<uintptr_t>(v))[len];
			if (c == '\0' || c < 0x20 || c > 0x7e) {
				break;
			}
			buf[len] = c;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	buf[len] = '\0';
	if (len > 0) {
		LOGF("\t preview: \"%s\"\n", buf);
	}
#else
	LOGF("\t preview: <host-read skipped>\n");
#endif
}

static void FloorThrowLog(const char* nid, uint64_t obj) {
	LOGF("ps5-vsh: THROW %s(obj=0x%016" PRIx64 ") from guest 0x%016" PRIx64 "\n", nid, obj,
	     FloorCaller());
	FloorPreview(obj);
}

} // namespace

namespace KernelFloor {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KFailNeg1(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	LOGF("\t shellcore-floor libkernel fail-1 stub: a1=0x%016llx\n",
	     (unsigned long long)a1);
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<uint64_t>(-1);
}

static int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ReadlinkFail(const char* path, char* buf, uint64_t bufsize) {
	LOGF("\t readlink(%s) -> -1/EINVAL (no symlinks)\n", path != nullptr ? path : "<null>");
	(void)buf;
	(void)bufsize;
	return -1;
}

struct SceRtprio {
	uint16_t type;
	uint16_t prio;
};

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRtprioThread(int function, uint32_t /*lwpid*/, SceRtprio* rtp) {
	if (rtp != nullptr) {
		if (function == 0 || function == 2) {
			rtp->type = 2;   // RTP_PRIO_NORMAL
			rtp->prio = 120; // default normal priority
		}
	}
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	// ps5-vsh: log guest return address for attribution (shared stub serves
	// many NIDs; the ra identifies the caller). Deduplicate to prevent log flood.
	uint64_t ra = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ra = reinterpret_cast<uint64_t>(_ReturnAddress());
#endif
	static std::unordered_set<uint64_t> s_seen_ras;
	static std::mutex                   s_ra_mutex;
	bool is_new = false;
	{
		std::lock_guard<std::mutex> lock(s_ra_mutex);
		is_new = s_seen_ras.insert(ra).second;
	}
	if (is_new) {
		LOGF("\t shellcore-floor libkernel stub: ra=0x%016" PRIx64 " a1=0x%016" PRIx64
		     " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 "\n",
		     ra, a1, a2, a3, a4);
	}
	return 0;
}

} // namespace KernelFloor

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SocketpairObserved(
    int domain, int type, int protocol, int* output) {
	const uint64_t caller = KYTY_SPP_CALLER;
	LibKernel::FileSystem::SocketpairProvenance::SocketpairCall observation(
	    caller, static_cast<uint32_t>(domain), static_cast<uint32_t>(type),
	    static_cast<uint32_t>(protocol), reinterpret_cast<uint64_t>(output));
	const int result = Network::Net::Socketpair(domain, type, protocol, output);
	observation.Finish(static_cast<uint64_t>(static_cast<int64_t>(result)));
	return result;
}

namespace KernelFloor {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetAppInfo(uint64_t pid, void* out) {
	// ps5-vsh: G-MYv5erXaU sceKernelGetAppInfo. Port of the reference
	// behavior: 88-byte record, title ATTRIBUTE3 u32 at +0x4c and category
	// u32 at +0x50 from the queried process metadata; unknown/inactive pids
	// report ESRCH so enumeration loops terminate instead of spinning
	// on success-with-zeros. First 8 + every 1024th logged.
	static std::atomic_uint64_t s_n = 0;
	const uint64_t              n   = s_n.fetch_add(1, std::memory_order_relaxed);
	if (out == nullptr || !FloorRangeOk(reinterpret_cast<uint64_t>(out), 88, true)) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	Loader::GuestProcessIdentity identity;
	const bool ok = pid <= INT32_MAX && Loader::TryGetGuestProcessIdentity(static_cast<int64_t>(pid), &identity) &&
	                identity.category.has_value();
	if (n < 8 || (n % 1024) == 0) {
		LOGF("\t sceKernelGetAppInfo(pid=%llu) -> %s\n", (unsigned long long)pid,
		     ok ? "OK" : "ESRCH");
	}
	if (!ok) {
		return LibKernel::KERNEL_ERROR_ESRCH;
	}
	std::memset(out, 0, 88);
	// Preserve the declared word verbatim, including zero/unknown bits. Esvm's
	// +0x4d & 2 test observes ATTRIBUTE3 & 0x200, not an authinfo capability.
	const uint32_t attribute3 = identity.attribute3.value_or(0);
	const uint32_t category = *identity.category;
	std::memcpy(static_cast<char*>(out) + 0x4c, &attribute3, sizeof(attribute3));
	std::memcpy(static_cast<char*>(out) + 0x50, &category, sizeof(category));
	return 0;
}

// ps5-vsh: +xy9ORMbd8U sceKernelGetAppCategoryType (voldemort parity).
// Same category source as GetAppInfo; unknown pids report ESRCH.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetAppCategoryType(uint64_t pid, uint32_t* out) {
	PRINT_NAME();
	if (out == nullptr || !FloorRangeOk(reinterpret_cast<uint64_t>(out), sizeof(*out), true)) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	uint32_t category = 0;
	if (pid > INT32_MAX || !Loader::TryGetGuestProcessCategory(static_cast<int64_t>(pid), &category)) {
		return LibKernel::KERNEL_ERROR_ESRCH;
	}
	*out = static_cast<uint32_t>(category);
	LOGF("\t pid = %llu, category = 0x%08x\n", (unsigned long long)pid,
	     static_cast<unsigned>(category));
	return 0;
}

// get_authinfo(pid, 136-byte record): explicit PID lookup, independent of the
// accepting compositor thread. Only the boot ShellUI identity has the observed
// compositor-pool capability; runtime authority comes from executable metadata.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KGetAuthInfo(int pid, void* output) {
	PRINT_NAME();
	if (output == nullptr || !FloorRangeOk(reinterpret_cast<uint64_t>(output), 136, true)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}
	std::array<uint64_t, 17> record {};
	if (!Loader::TryGetGuestProcessAuthInfo(pid, &record)) {
		*Posix::GetErrorAddr() = Posix::POSIX_ESRCH;
		return -1;
	}
	std::memcpy(output, record.data(), sizeof(record));
	LOGF("\t get_authinfo pid=%d -> authority=0x%016" PRIx64 " caps=0x%016" PRIx64 "\n", pid,
	     record[0], record[1]);
	return 0;
}

// ps5-vsh: ePRSOZsroIQ sceKernelGetIpcPath (voldemort parity, verified in
// libkernel: writes "/system_tmp/" + name (truncated to 31 chars)). The
// previous KStub left AF_UNIX socket paths uninitialized, so compositor
// IPC peers disagreed on the path.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetIpcPath(const char* name, char* out) {
	PRINT_NAME();
	if (name == nullptr || out == nullptr ||
	    !FloorRangeOk(reinterpret_cast<uint64_t>(out), 12 + 0x1F + 1, true)) {
		return static_cast<int>(0x80022016u);
	}
	static constexpr char kPrefix[] = "/system_tmp/";
	std::memcpy(out, kPrefix, 12);
	std::strncpy(out + 12, name, 0x1F);
	out[12 + 0x1F] = '\0';
	LOGF("\t name = %s -> %s\n", name, out);
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KFalse(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor libkernel false-query: a1=0x%016" PRIx64 " -> 0\n", a1);
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KTrue(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor libkernel true-query: a1=0x%016" PRIx64 " -> 1\n", a1);
	(void)a2;
	(void)a3;
	(void)a4;
	return 1;
}

// sceKernelGetCompiledSdkVersion{,ByPid} family (+Prospero/Ps4 variants):
// return the SDK 14.0 version int directly (no out-param convention).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KSdkVersion(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor SDK version for pid/eboot 0x%016" PRIx64 " -> 0x%08x\n", a1,
	     kSdk140);
	(void)a2;
	(void)a3;
	(void)a4;
	return kSdk140;
}

// sceKernelGetAppState(pid, state*): report running (1), return success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KAppState(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor GetAppState pid=0x%016" PRIx64 " -> running\n", a1);
	(void)a3;
	(void)a4;
	FloorWriteU32(a2, 1);
	return 0;
}

// *SystemSwVersion / *UpdVersion / build-number outs: floor firmware word.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KSwVersion(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor SwVersion query -> 0x%08x\n", kFw1360);
	(void)a2;
	(void)a3;
	(void)a4;
	FloorWriteU32(a1, kFw1360);
	return 0;
}

// sceKernelGetThreadName(thread, buf, len?): bounded copy, never overruns.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KThreadName(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	static const char kName[] = "ShellCore";
	(void)a1;
	(void)a4;
	size_t cap = sizeof(kName);
	if (a3 != 0 && a3 < cap) {
		cap = static_cast<size_t>(a3);
	}
	if (cap > 0 && FloorRangeOk(a2, cap, true)) {
		char tmp[sizeof(kName)] = {};
		std::memcpy(tmp, kName, cap - 1);
		FloorWrite(a2, tmp, cap);
	}
	// ps5-vsh: ShellCore polls this in tight loops; log first call only.
	static bool s_thread_name_logged = false;
	if (!s_thread_name_logged) {
		s_thread_name_logged = true;
		LOGF("\t shellcore-floor GetThreadName -> \"%s\"\n", kName);
	}
	return 0;
}

// The guest VFS currently exposes files/directories, not symbolic-link nodes
// (readlink above rejects them). Use its real metadata/error contract for lstat
// too: Mono falls back to lstat after stat fails, so success without writing
// FileStat turns missing paths into files with uninitialized attributes.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CLstat(
    const char* path, LibKernel::FileSystem::FileStat* attributes) {
	return POSIX_CALL(LibKernel::FileSystem::KernelStat(path, attributes));
}

// ps5-vsh: access(path, mode) (voldemort parity). Stat-based, so the
// previous success-no-op (which claimed everything exists) no longer lies
// about missing paths.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CAccess(const char* path, int mode) {
	if (mode < 0 || (mode & ~7) != 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	LibKernel::FileSystem::FileStat attributes {};
	const auto result = LibKernel::FileSystem::KernelStat(path, &attributes);
	if (result != 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(result);
		return -1;
	}
	if (((mode & 4) != 0 && (attributes.st_mode & 0444) == 0) ||
	    ((mode & 2) != 0 && (attributes.st_mode & 0222) == 0) ||
	    ((mode & 1) != 0 && (attributes.st_mode & 0111) == 0)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EACCES;
		return -1;
	}
	return 0;
}

// sceKernelGetIdPs / GetOpenPsIdForSystem: zeroed 16-byte ID, never crashes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KIdPs(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	static const uint8_t kZero[16] = {};
	FloorWrite(a1, kZero, sizeof(kZero));
	return 0;
}

// sceKernelCreateUuidV1: deterministic fake UUID, never crashes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KUuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	static const uint8_t kUuid[16] = {0x53, 0x48, 0x45, 0x4c, 0x4c, 0x43, 0x4f, 0x52,
	                                  0x45, 0x46, 0x4c, 0x4f, 0x4f, 0x52, 0x00, 0x01};
	FloorWrite(a1, kUuid, sizeof(kUuid));
	return 0;
}

// sceKernelTerminateSysCore: report success but never terminate the host.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KTerminateSysCore(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor TerminateSysCore ignored: a1=0x%016" PRIx64 "\n", a1);
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

LIB_DEFINE(InitShellCoreFloorKernel_1) {
	// SDK version direct-returns (SDK 14.0).
	LIB_FUNC("8BrtU9maWG4", KSdkVersion); // sceKernelGetCompiledSdkVersionByPid
	LIB_FUNC("3vD7TSeebMs", KSdkVersion); // sceKernelGetProsperoCompiledSdkVersionByPid
	LIB_FUNC("Ha-dfYigiss", KSdkVersion); // sceKernelGetPs4CompiledSdkVersionByPid
	LIB_FUNC("WB66evu8bsU", LibKernel::KernelGetCompiledSdkVersion); // sceKernelGetCompiledSdkVersion
	LIB_FUNC("sp+h-CJV1Ns", KSdkVersion); // sceKernelGetPs4CompiledSdkVersion
	// App-state query (running).
	LIB_FUNC("WYCNbP+Q9kc", KAppState); // sceKernelGetAppState
	// False queries (not this hardware / feature off).
	LIB_FUNC("rm1EC-NwS6A", KFalse); // sceKernelIsCloudkit
	LIB_FUNC("XyW5rUlKP1w", KFalse); // sceKernelIsCloudDevkit
	LIB_FUNC("lFf3UU811v4", KFalse); // sceKernelIsGenuineDevKit
	LIB_FUNC("S4XWKGFp+bc", KFalse); // sceKernelIsGenuineTestKit
	LIB_FUNC("1CVP7E-WFKk", KFalse); // sceKernelIsTestKit
	LIB_FUNC("QEvYMQNJGMU", KFalse); // sceKernelIsSpmemAvailable
	LIB_FUNC("rNRtm1uioyY", KFalse); // sceKernelHasNeoMode
	LIB_FUNC("7lZJYy0+evk", KFalse); // sceKernelUseCronosKura
	LIB_FUNC("ind6aqzel+Y", KFalse); // sceKernelUseCronosStreaming
	LIB_FUNC("+iVwFsjXaFQ", KFalse); // sceKernelDebugGpuPaDebugIsInProgress
	LIB_FUNC("TUZlvaB9rlg", KFalse); // sceKernelTitleWorkaroundIsEnabled2
	LIB_FUNC("GST42pfUfMc", KFalse); // sceKernelTitleWorkdaroundIsEnabled
	LIB_FUNC("1yca4VvfcNA", KFalse); // sceKernelTitleWorkaroundIsEnabled
	LIB_FUNC("bmQaivctXo4", KFalse); // sceKernelGetSafemode
	LIB_FUNC("NMf5gHfsrYs", KFalse); // sceKernelGetUpdatemode
	LIB_FUNC("GrdiysPCp+M", KFalse); // sceKernelGetUniversalMode
	LIB_FUNC("CQ1bzp1ba0k", KFalse); // sceKernelGetBackupRestoreModeOfNextBoot
	// True queries (retail console).
	LIB_FUNC("ts25M0mp2uY", KTrue); // sceKernelIsGenuineCEX
	LIB_FUNC("H54cSDvjryI", KTrue); // sceKernelIsGenuineKratosCex
	LIB_FUNC("8aCOCGoRkUI", KTrue); // sceKernelIsCEX
	// Rcmgr feature flags (all off on the floor).
	LIB_FUNC("tFD04DtJY+g", KFalse); // sceKernelGetBetaUpdateTestForRcmgr
	LIB_FUNC("JVYt1hlHSvo", KFalse); // sceKernelGetDebugMenuMiniModeForRcmgr
	LIB_FUNC("qYwuXe0hTHs", KFalse); // sceKernelGetDebugMenuModeForPsmForRcmgr
	LIB_FUNC("cVC4JXxpb-s", KFalse); // sceKernelGetFakeFinalizeMenuForRcmgr
	LIB_FUNC("LJVcP+0wRIk", KFalse); // sceKernelGetFlagedUpdaterForRcmgr
	LIB_FUNC("we5t5V4Fc70", KFalse); // sceKernelGetForceUpdateModeForRcmgr
	LIB_FUNC("C2ltEJILIGE", KFalse); // sceKernelGetPsmIntdevModeForRcmgr
	LIB_FUNC("ZotSeVujZaU", KFalse); // sceKernelGetPsnAccessTraceLogForRcmgr
	LIB_FUNC("58GfHBfwpOY", KFalse); // sceKernelGetQafExpirationTimeNotafterForRcmgr
	LIB_FUNC("QZ3WWIhtNbE", KFalse); // sceKernelGetQafExpirationTimeNotbeforeForRcmgr
	LIB_FUNC("H0hj8V6fJwk", KFalse); // sceKernelGetQafGenerationForRcmgr
	LIB_FUNC("qJnZcaDrZvI", KFalse); // sceKernelGetQafNameForRcmgr
	LIB_FUNC("3KIbxxVQv84", KFalse); // sceKernelGetSpecialIForRcmgr
	LIB_FUNC("fFMg-cBHBBY", KFalse); // sceKernelGetSystemLevelDebuggerModeForRcmgr
	// Utoken flags (all off on the floor).
	LIB_FUNC("ToKKfC8tKIk", KFalse); // sceKernelGetUtokenDataExecutionForRcmgr
	LIB_FUNC("8PGQdZFioFQ", KFalse); // sceKernelGetUtokenExpirationTimeNotafterForRcmgr
	LIB_FUNC("Htwoe5tvNYg", KFalse); // sceKernelGetUtokenExpirationTimeNotbeforeForRcmgr
	LIB_FUNC("MaCKSOQDtyU", KFalse); // sceKernelGetUtokenFakeSharefactoryForRcmgr
	LIB_FUNC("0yVJNcMqyXk", KFalse); // sceKernelGetUtokenFlagedUpdaterForRcmgr
	LIB_FUNC("+XuZD9HV0p4", KFalse); // sceKernelGetUtokenNpEnvSwitchingForRcmgr
	LIB_FUNC("VbrTamW6dJs", KFalse); // sceKernelGetUtokenSaveDataRepairForRcmgr
	LIB_FUNC("0NyJTCJWuuk", KFalse); // sceKernelGetUtokenStoreModeForRcmgr
	LIB_FUNC("Vo1qeT4JY6I", KFalse); // sceKernelGetUtokenUseSoftwagnerForRcmgr
	LIB_FUNC("T1t68JwPqME", KFalse); // sceKernelGetUtokenWeakenedPortRestrictionForRcmgr
	LIB_FUNC("b0cryxaTM4k", KFalse); // sceKernelGetUtokenUseSoftwagnerForAcmgr
	// System/build version outs (floor firmware word).
	LIB_FUNC("Mv1zUObHvXI", KSwVersion); // sceKernelGetSystemSwVersion
	LIB_FUNC("aML18Z0J0t0", KSwVersion); // sceKernelGetProsperoSystemSwVersion
	LIB_FUNC("e+OCXNBQRAA", KSwVersion); // sceKernelGetPs4SystemSwVersion
	LIB_FUNC("3EDFoWECKOg", KSwVersion); // sceKernelGetSystemSwBeta
	LIB_FUNC("rTNYCkfforg", KSwVersion); // sceKernelGetUpdVersion
	LIB_FUNC("CL9DIlKFCnA", KSwVersion); // sceKernelGetAutoUpdateVersion
	LIB_FUNC("Oi7Mkc2qiew", KSwVersion); // sceKernelGetBuildNumber
	LIB_FUNC("mpbGISNJ6go", KSwVersion); // sceKernelGetSystemExVersion
	// Misc state queries (0, no struct writes: layouts unknown).
	LIB_FUNC("9urVLs6ovQk", KStub); // sceKernelGetMemoryPstate
	LIB_FUNC("W+BEVqLrDXo", KStub); // sceKernelGetNandInfo
	LIB_FUNC("ou2GyAJwJ+Q", KStub); // sceKernelGetSubsysId
	LIB_FUNC("Ai+CmAO2w54", KStub); // sceKernelGetProcessAbiType
	// App info (0, no struct writes: layouts unknown).
	LIB_FUNC("d7xu4QDJw8Q", KStub); // sceKernelGetAppInfo2
	LIB_FUNC("G-MYv5erXaU", KernelGetAppInfo); // sceKernelGetAppInfo
	LIB_FUNC("zsMzX135ofU", KStub); // sceKernelGetAppInfoByAppId
	LIB_FUNC("pyAyDVpDOhw", KStub); // sceKernelGetProductCode
	LIB_FUNC("+xy9ORMbd8U", KernelGetAppCategoryType); // sceKernelGetAppCategoryType (real)
	// Identity queries.
	LIB_FUNC("lZG-WlKxRqQ", KIdPs); // sceKernelGetIdPs
	LIB_FUNC("ul57hvm6mBc", KIdPs); // sceKernelGetOpenPsIdForSystem
	LIB_FUNC("p2xgfB-30g8", KStub); // get_self_auth_info
	LIB_FUNC("igMefp4SAv0", KGetAuthInfo); // get_authinfo
	LIB_FUNC("4vkZwDJbMx0", KThreadName); // sceKernelGetThreadName
	LIB_FUNC("2I2RV6LyNng", KernelRtprioThread); // rtprio_thread
	// Lifecycle (never act, just succeed).
	LIB_FUNC("hUN72ocX6gM", KTerminateSysCore); // sceKernelTerminateSysCore
	LIB_FUNC("qIjSKZfWk5I", KStub); // sceKernelSpawn
	LIB_FUNC("teiItL2boFw", KStub); // sceKernelReleaseFlexibleMemory
	LIB_FUNC("kc+LEEIYakc", KStub); // sceKernelMapNamedSystemFlexibleMemory
	LIB_FUNC("mTBZfEal2Bw", KStub); // mlock
	LIB_FUNC("OG4RsDwLguo", KStub); // munlock
	// Filesystem / posix surface (success no-ops for the floor).
	LIB_FUNC("VW3TVZiM4-E", LibKernel::FileSystem::KernelFtruncate); // sceKernelFtruncate (real)
	LIB_FUNC("WlyEA-sLDf0", KStub); // sceKernelTruncate
	LIB_FUNC("0Cq8ipKr9n0", KStub); // sceKernelUtimes
	LIB_FUNC("GDuV00CHrUg", KStub); // utimes
	LIB_FUNC("fgIsQ10xYVA", KStub); // sceKernelChmod
	LIB_FUNC("UtszJWHrDcA", KStub); // sceKernelFchmod
	LIB_FUNC("n01yNbQO5W4", KStub); // fchmod
	LIB_FUNC("ruyD9QkP8dI", KStub); // fchown
	LIB_FUNC("+2HAuc7yVbc", KStub); // chown
	LIB_FUNC("NN01qLRhiqU", KStub); // rename
	LIB_FUNC("waIF+5fE5HQ", KStub); // symlink
	LIB_FUNC("pd02UI9TbOA", ReadlinkFail); // readlink -> -1 (not a symlink)
	LIB_FUNC("c7ZnT7V1B98", KStub); // rmdir
	LIB_FUNC("DRGXpDDh8Ng", CLstat); // lstat (guest VFS metadata and POSIX errors)
	LIB_FUNC("tpThrBsInto", KStub); // unmount
	LIB_FUNC("3YnXspQNyw8", KStub); // nmount
	LIB_FUNC("EARPL-r8jf0", KStub); // getfsstat
	LIB_FUNC("ngZnQWyUvGI", KStub); // statfs
	LIB_FUNC("tPWsbOUGO8k", KStub); // shm_unlink
	LIB_FUNC("0wu33hunNdE", LibKernel::KernelSleep); // sleep (real, voldemort parity)
	LIB_FUNC("W0xkN0+ZkCE", KStub); // kill
	LIB_FUNC("8vE6Z6VEYyk", CAccess); // access (stat-based, voldemort parity)
	LIB_FUNC("tvpHe5kBO4E", KStub); // geteuid
	// Sockets / aio (success no-ops).
	LIB_FUNC("MZb0GKT3mo8", SocketpairObserved); // genuine pair + optional provenance observation
	LIB_FUNC("aNeavPDNKzA", KStub); // sendmsg
	LIB_FUNC("hI7oVeOluPM", KStub); // recvmsg
	LIB_FUNC("kaLFzWxqkFY", KStub); // aio_read
	LIB_FUNC("IiofFKEgSew", KStub); // aio_write
	LIB_FUNC("j64iXjsjnCY", KStub); // aio_fsync
	LIB_FUNC("52r68vPvrBA", KStub); // aio_suspend
	LIB_FUNC("phnjLKz4njM", KStub); // aio_cancel
	// Dynamic linking (NULL handles = clean failure, never crash).
	LIB_FUNC("oiT0vgzmW0M", KStub); // dlclose
	LIB_FUNC("ucFJiTO1EUw", KStub); // dlerror
	LIB_FUNC("UteVS6B1ZrU", KStub); // dlopen
	LIB_FUNC("biY+kcVB5D4", KStub); // dlsym
	// IPC / events / notifications.
	LIB_FUNC("ePRSOZsroIQ", KernelGetIpcPath); // sceKernelGetIpcPath (real path, voldemort parity)
	LIB_FUNC("UmXngHKB6is", KStub); // sceKernelDeleteFileEvent
	LIB_FUNC("cG3t15OK4Ow", KStub); // sceKernelAddFileEvent
	LIB_FUNC("VHCS3rCd0PM", ::Libs::LibKernel::EventQueue::KernelAddReadEvent); // sceKernelAddReadEvent
	LIB_FUNC("JxJ4tfgKlXA", ::Libs::LibKernel::EventQueue::KernelDeleteReadEvent); // sceKernelDeleteReadEvent
	LIB_FUNC("s9-RaxukuzQ", ::Libs::LibKernel::EventFlag::KernelCloseEventFlag);
	LIB_FUNC("YWQFUyXIVdU", KStub); // sceKernelDeleteTimerEvent
	LIB_FUNC("zl7hupSO0C0", KStub); // sceKernelSendNotificationRequest
	LIB_FUNC("I111PPn2g18", KStub); // sceKernelWriteSdkEventLog
	LIB_FUNC("hjErHT1CwJM", KStub); // sceKernelNotifySystemSuspendResumeProgress
	// App mount / FGC / AC bookkeeping.
	LIB_FUNC("CYIzY6OXyAs", KStub); // sceKernelEndAppMount2
	LIB_FUNC("HRDq-RDIGmI", KStub); // sceKernelBeginAppMount2
	LIB_FUNC("M+vuTJQ3F8Y", KStub); // sceKernelAddFGCInfo
	LIB_FUNC("ZGfk6FwQozw", KStub); // sceKernelDeleteFGCInfo
	LIB_FUNC("ImXI9qdLfc0", KStub); // sceKernelDeleteACInfo
	LIB_FUNC("KpCv0rernZQ", KStub); // sceKernelAddACInfo
	LIB_FUNC("mpTUqam2SgY", KStub); // sceKernelSetAcKeyRegResult
	LIB_FUNC("JlybWYbzKcY", KStub); // sceKernelSetPackageInfo
	// Mode setters (accept and ignore).
	LIB_FUNC("KCwXUKT269I", KStub); // sceKernelSetCpumodeGame
	LIB_FUNC("2mH42a-kAiQ", KStub); // sceKernelSetSafemodeDirect
	LIB_FUNC("As-JdqyUuMs", KStub); // sceKernelSetSafemode
	LIB_FUNC("9xRIkqnNibE", KStub); // sceKernelSetBackupRestoreMode
	LIB_FUNC("+rSNKzU+aNQ", KStub); // sceKernelSetDataTransferMode
	LIB_FUNC("cxdklXtQcqA", KStub); // sceKernelSetTimezoneInfo
	// UUID.
	LIB_FUNC("8PS6UDzj-xY", KUuid); // sceKernelCreateUuidV1
	// ICC / indicator stubs (all no-ops).
	LIB_FUNC("-uPq82VbRMI", KStub); // sceKernelIccGetBDDriveChuckingState
	LIB_FUNC("1ns3nDImmno", KStub); // sceKernelIccIsExtBDDriveAttached
	LIB_FUNC("zLEuSU+hl-w", KStub); // sceKernelIccGetBDPowerState
	LIB_FUNC("1wxFFk3xIhA", KStub); // sceKernelIccControlBDPowerState
	LIB_FUNC("3B8820wLyCk", KStub); // sceKernelHwGetBdDriveInfo
	LIB_FUNC("pv+JNjlZJgI", KStub); // sceKernelIccDisableExtBDDriveOperation
	LIB_FUNC("PGdGn+-opZg", KStub); // sceKernelIccSetPriorityBuzzerEx
	LIB_FUNC("qbx9P0mc2Bk", KStub); // sceKernelIccGetBuzzerOffSetting
	LIB_FUNC("Mrc6Jc4kC1A", KStub); // sceKernelIccIsThermalAlert
	LIB_FUNC("8TqMzIggthc", KStub); // sceKernelIccGetErrLog
	LIB_FUNC("ZqxPUMisNkY", KStub); // sceKernelIccControlUSBPowerState
	LIB_FUNC("0gkGdPk8dXw", KStub); // sceKernelIccReadPowerBootMessage
	LIB_FUNC("gQXxz1IoL5U", KStub); // sceKernelIccIndicatorShutdown
	LIB_FUNC("Eye1lgCj9yo", KStub); // sceKernelIccIndicatorStandbyShutdown
	LIB_FUNC("qWoGe2XqwVw", KStub); // sceKernelIccIndicatorBootDone
	LIB_FUNC("98LV9dS2Fvw", KStub); // sceKernelIccIndicatorStandby
	LIB_FUNC("7Q-U-8yN1Hk", KStub); // sceKernelIccIndicatorStandbyBoot
	// Heap telemetry (0, no-op).
	LIB_FUNC("m5zDvD-LLjo", KStub); // sceKernelKernelHeapUsage
	LIB_FUNC("rd23OgUSfZQ", KStub); // sceKernelKernelHeapGetCount
	LIB_FUNC("zx9n-9bt7IA", KStub); // sceKernelGetResidentFmemCount2
	// Unresolved-name imports (observation stubs, log + 0).
	LIB_FUNC("YP7qj+uSJwo", KStub); // ?
	LIB_FUNC("LPXIR37gw3c", KStub); // ?
	LIB_FUNC("Dv8In6FvzuM", KStub); // ?
	LIB_FUNC("940JfV9yJto", KStub); // ?
	LIB_FUNC("3jw-vq4gYLs", KStub); // ?
	LIB_FUNC("9KpeTiVh5Hk", KStub); // ?
	LIB_FUNC("l-QGmU3VU+Q", KStub); // ?
	LIB_FUNC("J2dqh+RUwEY", KStub); // ?
	LIB_FUNC("gBiaAGKEvHM", KStub); // ?
	LIB_FUNC("e7oZmNPlBbU", KStub); // ?
	LIB_FUNC("2YWCKF9-3gM", KStub); // ?
	LIB_FUNC("5KHFhRhBR78", KStub); // ?
	LIB_FUNC("JcG0kUg4TrE", KStub); // ?
	LIB_FUNC("XvONqaRmAPE", KStub); // ?
	LIB_FUNC("jMuNoBRCPUg", KStub); // ?
	LIB_FUNC("2fPLezHAV2E", KStub); // ?
	LIB_FUNC("pp+G+UZhiqY", KStub); // ?
	LIB_FUNC("h35fpAz5tf4", KStub); // ?
	LIB_FUNC("r6qUkFmQFr0", KStub); // ?
	LIB_FUNC("qe5JmcCj4zg", KStub); // ?
	LIB_FUNC("yWOJkfd-DhA", KStub); // ?
	LIB_FUNC("OZBJnWxt7d4", KStub); // ?
}

} // namespace KernelFloor

namespace LibcFloor {

LIB_VERSION("libSceLibcInternal", 1, "libSceLibcInternal", 1, 1);

// ---- data objects for vtable/typeinfo/facet-id imports ----
static uint64_t g_vtableBadArrayNewLength[4] = {};
static uint64_t g_vtableSiClassTypeInfo[4]   = {};
static uint64_t g_vtableBadCast[4]           = {};
static uint64_t g_vtableVmiClassTypeInfo[4]  = {};
static uint64_t g_vtableClassTypeInfo[4]     = {};
static uint64_t g_vtableBadAlloc[4]          = {};
static uint64_t g_typeinfoInt[4]             = {};
static uint64_t g_nothrow[2]                = {};
static uint64_t g_facetIdNumPut[2]          = {};
static uint64_t g_facetIdNumPunct[2]        = {};
static uint64_t g_facetIdCollate[2]         = {};
static uint64_t g_facetIdNumGet[2]          = {};
static uint64_t g_facetIdCodecvtCc[2]       = {};
static uint64_t g_facetIdCodecvtWc[2]       = {};
static uint64_t g_localeIdCnt[2]            = {};
// ps5-vsh: null-ios stream objects. The guest treats cout/cerr/cin as full
// basic_ios: vtable[-3] (offset-to-top at -0x18) selects member offsets up
// to +0x68, with explicit null branches when streambuf members are 0.
// An 8-qword blob is TOO SMALL: +0x48/+0x50 reads escape into adjacent
// statics, returning small-int garbage (crash at 0x5abe reading 0x2 via
// the "[ERROR]"→cerr log path) instead of 0. 24 qwords (0xC0) keeps every
// observed member access in-bounds and zero, so the guest takes its
// null-safe paths. Each stream gets its own vtable mem (slots 4..11 point
// at CStub, so virtual calls log-and-return-0); vtable[-3] stays 0.
static uint64_t g_cinMem[16]  = {};
static uint64_t g_cinObj[24]  = {};
static uint64_t g_coutMem[16] = {};
static uint64_t g_coutObj[24] = {};
// ps5-vsh: cerr object with a callable vtable. An all-zero blob crashes the
// guest's ostream machinery (it reads vtable-0x18 and calls through slots),
// so the vtable lives inside zero padding (negative reads stay in-bounds)
// with every slot pointing at CStub (log-and-0). Output is discarded, but
// formatting never faults.
static uint64_t g_cerrMem[16] = {};
static uint64_t g_cerrObj[24] = {};
// ps5-vsh: JSCd object audit (36 ZERO objects, jscd_audit10.log): C++ RTTI
// vtables/typeinfos the guest compares/copies during locale/iostream init.
// Unique non-zero blobs (vtable ABI: [0]=0, [1]=typeinfo ptr, [2+]=slots;
// typeinfo: [0]=vtable, [1]=name ptr). Zero would alias distinct types.
static uint64_t g_tiConstCharPtr[4]   = {0xC001, 0xC002, 0xC003, 0xC004};
static uint64_t g_vtPtrTypeInfo[4]    = {0, 0xD001, 0xD002, 0xD003};
static uint64_t g_vtFnTypeInfo[4]     = {0, 0xD011, 0xD012, 0xD013};
static uint64_t g_vtBadException[4]   = {0, 0xD021, 0xD022, 0xD023};
static uint64_t g_tiBadException[4]   = {0xE001, 0xE002, 0xE003, 0xE004};
static uint64_t g_tiVoid[4]           = {0xE011, 0xE012, 0xE013, 0xE014};

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

// ps5-vsh: minimal fake streambuf for the null-ios objects. The guest's
// ostream paths unconditionally dereference the rdbuf (e.g. 0x48f4 calls
// vtable+0x50 without a null check, 0x4971 calls +0x20 when the put-area
// is empty). The fake reports an empty put-area (scratch zeros, so loops
// are skipped) and full success on sputn (returns the requested count, so
// `cmp expected,rax` passes and the guest exits the write path cleanly).
static uint64_t g_putScratch[8]  = {};
static uint64_t g_fakeBufVt[16]  = {};
static uint64_t g_fakeBuf[16]    = {};
// ps5-vsh: one guest path (0x5790) double-derefs the buffer vtable:
// rdi=[buf]=bufVt, rax=[bufVt]=slot0, call [slot0+0x10]. Slot0 must
// therefore be a TABLE whose slots are callable (CStub logs the hit),
// not a function (whose +0x10 would be code bytes as a wild target).
// A second site calls [slot0+0x18], so the whole table is stubs.
static uint64_t g_indirect[8]          = {};
static const char kStreamLocaleName[]  = "C";
static uint64_t g_streamLocaleImpl[16] = {};
static uint64_t g_streamLocaleCell     = 0;
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SputnOk(uint64_t a1, uint64_t a2, uint64_t n) {
	PRINT_NAME();
	(void)a1;
	// ps5-vsh: JSCd triage - the guest prints its own init diagnostics
	// through cerr; capture the bytes (cap 256) instead of discarding.
	std::string view;
	if (a2 != 0 && n > 0 && n < 4096 && FloorRangeOk(a2, (size_t)n, false)) {
		view.assign(reinterpret_cast<const char*>(a2), (size_t)(n > 256 ? 256 : n));
	}
	LOGF("\t fakebuf sputn(n=%llu) -> %llu [%s]\n",
	     static_cast<unsigned long long>(n), static_cast<unsigned long long>(n),
	     view.c_str());
	return n;
}
static void CerrInit() {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;
	for (int i = 4; i < 12; i++) {
		g_cerrMem[i] = reinterpret_cast<uint64_t>(&CStub);
		g_coutMem[i] = reinterpret_cast<uint64_t>(&CStub);
		g_cinMem[i] = reinterpret_cast<uint64_t>(&CStub);
	}
	g_cerrObj[0] = reinterpret_cast<uint64_t>(&g_cerrMem[4]);
	g_coutObj[0] = reinterpret_cast<uint64_t>(&g_coutMem[4]);
	g_cinObj[0] = reinterpret_cast<uint64_t>(&g_cinMem[4]);
	// ps5-vsh: fake streambuf wiring (see decl): all vtable slots log-and-0
	// except +0x50 (index 10) which reports full write success; the buffer
	// claims an empty put-area via zeroed scratch so byte loops are skipped.
	for (int i = 0; i < 16; i++) {
		g_fakeBufVt[i] = reinterpret_cast<uint64_t>(&CStub);
	}
	g_fakeBufVt[10] = reinterpret_cast<uint64_t>(&SputnOk);
	for (int i = 0; i < 8; i++) {
		g_indirect[i] = reinterpret_cast<uint64_t>(&CStub);
	}
	g_fakeBufVt[0] = reinterpret_cast<uint64_t>(&g_indirect[0]);
	g_fakeBuf[0]    = reinterpret_cast<uint64_t>(&g_fakeBufVt[0]);
	g_fakeBuf[8]    = reinterpret_cast<uint64_t>(&g_putScratch[0]); // +0x40 put_next_ptr
	g_fakeBuf[12]   = reinterpret_cast<uint64_t>(&g_putScratch[1]); // +0x60 put_end_ptr
	g_cerrObj[9]    = reinterpret_cast<uint64_t>(&g_fakeBuf[0]);   // +0x48 _Mystrbuf
	g_coutObj[9]    = reinterpret_cast<uint64_t>(&g_fakeBuf[0]);
	g_cinObj[9]     = reinterpret_cast<uint64_t>(&g_fakeBuf[0]);
	// ps5-vsh: ios_base+0x40 is _Ploc (std::locale* -> _Locimp*), NOT a streambuf.
	// Point it at a classic _Locimp with callable vtable (retain=+0x10, release=+0x18),
	// facet_count=0 (+0x18), transparent=0 (+0x24), and name="C" (+0x28).
	g_streamLocaleImpl[0] = reinterpret_cast<uint64_t>(&g_indirect[0]);
	g_streamLocaleImpl[1] = 1;
	g_streamLocaleImpl[2] = 0;
	g_streamLocaleImpl[3] = 0;
	g_streamLocaleImpl[4] = 0;
	g_streamLocaleImpl[5] = reinterpret_cast<uint64_t>(kStreamLocaleName);
	g_streamLocaleCell    = reinterpret_cast<uint64_t>(&g_streamLocaleImpl[0]);
	g_cerrObj[8] = reinterpret_cast<uint64_t>(&g_streamLocaleCell); // +0x40 _Ploc
	g_coutObj[8] = reinterpret_cast<uint64_t>(&g_streamLocaleCell);
	g_cinObj[8]  = reinterpret_cast<uint64_t>(&g_streamLocaleCell);
	reinterpret_cast<uint8_t*>(g_cerrObj)[0x58] = ' ';
	reinterpret_cast<uint8_t*>(g_coutObj)[0x58] = ' ';
	reinterpret_cast<uint8_t*>(g_cinObj)[0x58]  = ' ';
		LOGF("\t ios-addrs: CStub=%016llx SputnOk=%016llx fakeBuf=%016llx fakeBufVt=%016llx\n"
	     "\t   cerrObj=%016llx coutObj=%016llx cinObj=%016llx cerrMem=%016llx\n",
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&CStub)),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&SputnOk)),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_fakeBuf[0])),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_fakeBufVt[0])),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_cerrObj[0])),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_coutObj[0])),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_cinObj[0])),
	     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&g_cerrMem[0])));
	// ps5-vsh: temp triage - dump CStub code bytes (guest calls [CStub+0x10]).
	const uint8_t* cb = reinterpret_cast<const uint8_t*>(&CStub);
	LOGF("\t CStub-bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	     cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7], cb[8], cb[9],
	     cb[10], cb[11]);
	LOGF("\t CStub-bytes+12: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	     cb[12], cb[13], cb[14], cb[15], cb[16], cb[17], cb[18], cb[19], cb[20],
	     cb[21], cb[22], cb[23]);
}

static int64_t  g_badoff                    = -1;
static uint64_t g_lconvBlob[16]             = {};
static uint8_t  g_cxaScratch[256]           = {};
static const char g_emptyStr[1]             = {0};

// Generic log-and-0 observation stub.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor libc stub: a1=0x%016" PRIx64 " a2=0x%016" PRIx64
	     " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return 0;
}

// ps5-vsh: I0vuMo7qix8 std::setw (MSVC _Smanip<streamsize>). The guest calls
// the returned _Pfun immediately (call *%rax with the adjusted stream), so a
// log-and-0 scalar return is a fatal null call. Return the address of CStub
// itself as _Pfun: same-process SysV HLE addresses are directly guest
// callable, and the apply becomes a logged no-op. Width rides in the
// guest's own rdx (untouched across our return).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSetw(uint64_t width) {
	PRINT_NAME();
	const uint64_t apply = reinterpret_cast<uint64_t>(&CStub);
	LOGF("\t setw(%llu) -> _Smanip{pfn=0x%016llx}\n",
	     static_cast<unsigned long long>(width),
	     static_cast<unsigned long long>(apply));
	return apply;
}

// ps5-vsh: per-NID named stubs (JSCd crash triage): each logs its NID +
// guest return address so shared-CStub call sites are attributable.
#define FLOOR_NAMED_STUB(nid)                                                  \
    static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub_##nid(uint64_t a1, uint64_t a2,        \
                                              uint64_t a3, uint64_t a4) {      \
        PRINT_NAME();                                                          \
        uint64_t ra = 0;                                                       \
        (void)ra;                                                              \
        LOGF("\t floor-named %s: ra=0x%016llx a1=0x%016llx a2=0x%016llx\n",   \
             #nid,                                                             \
             static_cast<unsigned long long>(                                   \
                 KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS                        \
                     ? reinterpret_cast<uint64_t>(_ReturnAddress())             \
                     : 0u),                                                     \
             static_cast<unsigned long long>(a1),                               \
             static_cast<unsigned long long>(a2));                              \
        (void)a3;                                                              \
        (void)a4;                                                              \
        return 0;                                                              \
    }
// init_once_complete must be nonzero (guest: 0 -> link_alternate_names_and_abort
// -> ud2). The init work happens inline; complete() only publishes it.
#define FLOOR_NAMED_STUB1(nid)                                                 \
    static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub1_##nid(uint64_t a1, uint64_t a2,       \
                                               uint64_t a3, uint64_t a4) {     \
        PRINT_NAME();                                                          \
        const uint64_t v1 = Libs::IsDirectTitleBoot() ? 1 : 0;                 \
        LOGF("\t floor-named1 %s: ra=0x%016llx a1=0x%016llx a2=0x%016llx -> %llu\n", \
             #nid,                                                             \
             static_cast<unsigned long long>(                                   \
                 KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS                        \
                     ? reinterpret_cast<uint64_t>(_ReturnAddress())             \
                     : 0u),                                                     \
             static_cast<unsigned long long>(a1),                               \
             static_cast<unsigned long long>(a2),                               \
             static_cast<unsigned long long>(v1));                              \
        (void)a3;                                                              \
        (void)a4;                                                              \
        return v1;                                                             \
    }

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CNonZero(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t shellcore-floor libc nonzero stub: a1=0x%016" PRIx64 " -> 1\n", a1);
	(void)a2;
	(void)a3;
	(void)a4;
	return 1;
}

// C++ EH tracer without message (personality, _Unwind_Resume, __cxa_*,
// dtors, dynamic_cast-fail): log + 0, never unwind (libC XlengthError style).
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void FloorTrapPostCheck();
#endif

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CThrowVoid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// The throw is the first guest->host transition after the bundle fread, so
	// it is the natural place to ask whether the armed taps are still there.
	FloorTrapPostCheck();
#endif
	LOGF("ps5-vsh: THROW eh-tracer(obj=0x%016" PRIx64 ") from guest 0x%016" PRIx64 "\n", a1,
	     FloorCaller());
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

// Fault-guarded read of a guest qword (0 when unmapped).
static uint64_t FloorReadU64(uint64_t p) {
	if (!FloorRangeOk(p, 8, false)) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(p)));
}

// Bounded, fault-guarded copy of a type_info's mangled name. Itanium
// __class_type_info is { vptr, const char* name, ... }; a name that is not
// NUL-terminated inside the mapping is reported as absent rather than followed.
static bool FloorTypeInfoNameCopy(uint64_t ti, char* out, size_t cap) {
	if (cap != 0) {
		out[0] = '\0';
	}
	if (ti == 0 || cap < 2 || !FloorRangeOk(ti, 16, false)) {
		return false;
	}
	const uint64_t np = FloorReadU64(ti + 8);
	if (!FloorRangeOk(np, cap, false)) {
		return false;
	}
	FLOOR_SEH_RETURN(false, ([np, out, cap]() -> bool {
		const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(np));
		for (size_t i = 0; i + 1 < cap; i++) {
			const char c = p[i];
			out[i] = c;
			if (c == '\0') {
				return true;
			}
		}
		out[cap - 1] = '\0';
		return false;
	})());
}

enum class FloorRttiKind { Unsupported, Class, Single, Multiple };

// Identify the dynamic type of the type_info object through its vtable's RTTI,
// not through payload pointer plausibility. In particular, VMI {flags=0,
// count=3} at +0x10 is 0x300000000, a perfectly valid Hermes heap address.
// These exact ABI type names and address-point headers are present in both
// 13.60 and 4.03 native libc. No function in the guest vtable is called here.
static FloorRttiKind FloorRttiGetKind(uint64_t ti) {
	if (!FloorRangeOk(ti, 16, false)) {
		return FloorRttiKind::Unsupported;
	}
	const uint64_t vptr = FloorReadU64(ti);
	if (vptr < 16 || !FloorRangeOk(vptr - 16, 16, false) || FloorReadU64(vptr - 16) != 0) {
		return FloorRttiKind::Unsupported;
	}
	char kind[96];
	if (!FloorTypeInfoNameCopy(FloorReadU64(vptr - 8), kind, sizeof(kind))) {
		return FloorRttiKind::Unsupported;
	}
	if (std::strcmp(kind, "N10__cxxabiv117__class_type_infoE") == 0) {
		return FloorRttiKind::Class;
	}
	if (std::strcmp(kind, "N10__cxxabiv120__si_class_type_infoE") == 0) {
		return FloorRttiKind::Single;
	}
	if (std::strcmp(kind, "N10__cxxabiv121__vmi_class_type_infoE") == 0) {
		return FloorRttiKind::Multiple;
	}
	return FloorRttiKind::Unsupported;
}

// Is `want` (a mangled class name) `ti` itself, or one of its bases?
// *off receives the byte offset of that base subobject inside the complete
// object that `ti` describes. Record layouts (Itanium C++ ABI):
//   __si_class_type_info : { vptr, name, base* }                  base at 0
//   __vmi_class_type_info: { vptr, name, flags, count, base*[] },
//                          each entry { base*, offset_flags }, off = flags >> 8
// Only unambiguous public, nonvirtual inheritance is supported. Virtual bases,
// repeated/diamond layouts and unknown flags are rejected rather than guessed.
static bool FloorRttiFindBase(uint64_t ti, const char* want, int depth, int64_t* off) {
	if (ti == 0 || depth > 8) {
		return false;
	}
	const auto kind = FloorRttiGetKind(ti);
	if (kind == FloorRttiKind::Unsupported) {
		return false;
	}
	char name[96];
	if (!FloorTypeInfoNameCopy(ti, name, sizeof(name)) || name[0] == '\0') {
		return false;
	}
	if (std::strcmp(name, want) == 0) {
		*off = 0;
		return true;
	}
	if (kind == FloorRttiKind::Class || !FloorRangeOk(ti, 24, false)) {
		return false;
	}
	const uint64_t v10 = FloorReadU64(ti + 0x10);
	if (kind == FloorRttiKind::Single) {
		return FloorRttiFindBase(v10, want, depth + 1, off);
	}
	const uint32_t count = static_cast<uint32_t>(v10 >> 32);
	if (static_cast<uint32_t>(v10) != 0 || count == 0 || count > 16 ||
	    !FloorRangeOk(ti, 24 + 16ull * count, false)) {
		return false;
	}
	bool found = false;
	int64_t result = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint64_t base = FloorReadU64(ti + 0x18 + 16ull * i);
		const int64_t  ofl  = static_cast<int64_t>(FloorReadU64(ti + 0x18 + 16ull * i + 8));
		if ((ofl & 1) != 0 || (ofl & 0xfc) != 0 || ofl < 0) {
			return false;
		}
		if ((ofl & 2) == 0) {
			// A private/nonpublic path cannot satisfy this cast.
			continue;
		}
		int64_t sub = 0;
		if (FloorRttiFindBase(base, want, depth + 1, &sub)) {
			if (found || sub < 0 || sub > INT64_MAX - (ofl >> 8)) {
				return false;
			}
			result = (ofl >> 8) + sub;
			found = true;
		}
	}
	if (found) {
		*off = result;
	}
	return found;
}

// __dynamic_cast (NID hMAe+TWS9mQ), Itanium ABI: a1 = sub, a2 = src typeinfo,
// a3 = dst typeinfo, a4 = hint. This retained HLE searches the object's dynamic
// type (vptr[-2] = offset-to-top, vptr[-1] = type_info*) with base offsets.
// It predates native JSI RTTI resolution and also retains the 4.03 JSC fallback
// below. Do not globally replace it with native __dynamic_cast without auditing
// those callers and the remaining shadowed RTTI objects.
// Observed caller: the 13.60 JS executor casts its jsi::Runtime* to
// facebook::hermes::HermesRuntime (nativeext 0x23d3e). Failing that cast sends
// the guest into native __cxa_bad_cast; run11's initiating std::bad_cast came
// from misclassifying the VMI count word as a readable SI base pointer.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI DynCastProbe(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	char        dst_buf[96];
	char        src_buf[96];
	const bool  have_dst = FloorTypeInfoNameCopy(a3, dst_buf, sizeof(dst_buf));
	const char* dst      = have_dst ? dst_buf : "<bad-dst-ti>";
	const char* src      = FloorTypeInfoNameCopy(a2, src_buf, sizeof(src_buf)) ? src_buf : "<bad-src-ti>";

	if (a1 == 0) {
		LOGF("ps5-vsh: DYNCAST null src_obj src=%s dst=%s from guest 0x%016llx -> 0\n", src, dst,
		     static_cast<unsigned long long>(FloorCaller()));
		return 0;
	}

	// Offset-to-top and type_info of the object's dynamic type.
	const uint64_t vptr  = FloorReadU64(a1);
	int64_t        offtt = 0;
	uint64_t       dt    = 0;
	if (FloorRangeOk(vptr, 24, false)) {
		offtt = static_cast<int64_t>(FloorReadU64(vptr - 16));
		dt    = FloorReadU64(vptr - 8);
	}
	char        dyn_buf[96];
	const char* dyn = FloorTypeInfoNameCopy(dt, dyn_buf, sizeof(dyn_buf)) ? dyn_buf : "<bad-dyn-ti>";
	if (offtt < -0x100000 || offtt > 0x100000) {
		// Not a plausible subobject offset: refuse to do pointer arithmetic on
		// it, but keep the value in the log.
		LOGF("ps5-vsh: DYNCAST src_obj=0x%016llx src=%s dyn=%s dst=%s offt=0x%llx implausible, "
		     "assuming 0 from guest 0x%016llx\n",
		     static_cast<unsigned long long>(a1), src, dyn, dst,
		     static_cast<unsigned long long>(offtt),
		     static_cast<unsigned long long>(FloorCaller()));
		offtt = 0;
	}

	if (have_dst) {
		int64_t off = 0;
		if (FloorRttiFindBase(dt, dst, 0, &off)) {
			const uint64_t res =
			    static_cast<uint64_t>(static_cast<int64_t>(a1 - static_cast<uint64_t>(offtt)) + off);
			LOGF("ps5-vsh: DYNCAST ok src_obj=0x%016llx src=%s dyn=%s dst=%s offt=%lld off=%lld "
			     "from guest 0x%016llx -> 0x%016llx\n",
			     static_cast<unsigned long long>(a1), src, dyn, dst,
			     static_cast<long long>(offtt), static_cast<long long>(off),
			     static_cast<unsigned long long>(FloorCaller()),
			     static_cast<unsigned long long>(res));
			return res;
		}
	}

	// Historical rule, kept as a fallback for the 4.03 JSC path: single
	// inheritance jsi::Runtime -> JSCRuntime at offset 0.
	if (have_dst && std::strcmp(dst, "N8facebook3jsc10JSCRuntimeE") == 0) {
		LOGF("ps5-vsh: DYNCAST -> JSCRuntime fallback (src_obj=0x%016llx dyn=%s) from guest 0x%016llx\n",
		     static_cast<unsigned long long>(a1), dyn,
		     static_cast<unsigned long long>(FloorCaller()));
		return a1;
	}

	LOGF("ps5-vsh: DYNCAST src_obj=0x%016llx src=%s dyn=%s dst=%s hint=%llu from guest 0x%016llx -> 0\n",
	     static_cast<unsigned long long>(a1), src, dyn, dst,
	     static_cast<unsigned long long>(a4), static_cast<unsigned long long>(FloorCaller()));
	(void)a4;
	return 0;
}
// ---------------------------------------------------------------------------
// ps5-vsh: guest C++ exception unwinding, stage 1 (diagnose).
//
// Every exception entry point the guest uses (__cxa_throw,
// __gxx_personality_v0, _Unwind_Resume, __cxa_begin/end_catch, ...) is an
// import that kyty HLEs, and nothing in kyty runs the guest's DWARF unwinder.
// When __cxa_throw returns instead of transferring control, the guest executes
// the ud2/int3 padding after the call as code and limps into the next function
// with garbage registers - observed as `vucomisd xmm0,[r12]` with r12 = -1 in
// libSceFollyShared (a folly::ConversionError thrown from the double->int64
// conversion checker at rva 0x129d0).
//
// Stage 1 replaces the limp with a faithful *diagnosis*: __cxa_throw walks the
// guest frame chain, resolves each frame's FDE in .eh_frame, parses that frame's
// LSDA and reports the landing pad that would run (cleanup) or that catches the
// thrown type, then stops the emulator with a readable message. No control
// transfer yet - _Unwind_Resume and the __cxa_* helpers stay no-ops.
//
// Tables are read from guest memory at runtime (modules are identity mapped and
// already relocated), so pcrel-encoded pointers resolve directly and the RELA
// tables are never consulted.
// ---------------------------------------------------------------------------

static uint8_t FloorReadU8(uint64_t p) {
	if (!FloorRangeOk(p, 1, false)) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(p)));
}

static uint16_t FloorReadU16(uint64_t p) {
	if (!FloorRangeOk(p, 2, false)) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, *reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(p)));
}

static uint32_t FloorReadU32(uint64_t p) {
	if (!FloorRangeOk(p, 4, false)) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(p)));
}

// LEB128 readers over guest memory. *end (when given) receives the address of
// the first byte past the encoding.
static uint64_t FloorUleb(uint64_t p, uint64_t* end) {
	uint64_t v     = 0;
	unsigned shift = 0;
	for (int i = 0; i < 10; i++) {
		const uint8_t b = FloorReadU8(p++);
		v |= static_cast<uint64_t>(b & 0x7fu) << shift;
		if ((b & 0x80u) == 0) {
			break;
		}
		shift += 7;
	}
	if (end != nullptr) {
		*end = p;
	}
	return v;
}

static int64_t FloorSleb(uint64_t p, uint64_t* end) {
	int64_t  v     = 0;
	unsigned shift = 0;
	uint8_t  b     = 0;
	for (int i = 0; i < 10; i++) {
		b = FloorReadU8(p++);
		v |= static_cast<int64_t>(b & 0x7fu) << shift;
		shift += 7;
		if ((b & 0x80u) == 0) {
			break;
		}
	}
	if (shift < 64 && (b & 0x40u) != 0) {
		v |= -(static_cast<int64_t>(1) << shift);
	}
	if (end != nullptr) {
		*end = p;
	}
	return v;
}

static size_t FloorEhEncSize(uint8_t enc) {
	switch (enc & 0x0fu) {
	case 0x00: return 8;
	case 0x02: return 2;
	case 0x03: return 4;
	case 0x04: return 8;
	case 0x0a: return 2;
	case 0x0b: return 4;
	case 0x0c: return 8;
	default: return 0; // uleb/sleb entries cannot be indexed arithmetically
	}
}

// One DWARF encoded value. `field` is the address of its first byte, which is
// also the pcrel base used by this decoder. `image_base` is
// the module base used for the (rare) absolute/datarel forms.
static bool FloorEhEncRead(uint64_t field, uint8_t enc, uint64_t image_base, uint64_t* next, uint64_t* out) {
	if (enc == 0xff) {
		return false;
	}
	const uint8_t fmt = enc & 0x0fu;
	const uint8_t app = enc & 0x70u;
	uint64_t      p   = field;
	uint64_t      v   = 0;
	switch (fmt) {
	case 0x00: v = FloorReadU64(p); p += 8; break;
	case 0x01: v = FloorUleb(p, &p); break;
	case 0x02: v = FloorReadU16(p); p += 2; break;
	case 0x03: v = FloorReadU32(p); p += 4; break;
	case 0x04: v = FloorReadU64(p); p += 8; break;
	case 0x09: v = static_cast<uint64_t>(FloorSleb(p, &p)); break;
	case 0x0a: v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(FloorReadU16(p)))); p += 2; break;
	case 0x0b: v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(FloorReadU32(p)))); p += 4; break;
	case 0x0c: v = FloorReadU64(p); p += 8; break;
	default: return false;
	}
	switch (app) {
	case 0x00: v += image_base; break; // absptr: 0-based image address
	case 0x10: v += field; break;      // pcrel: from the encoding's own address
	case 0x30: v += image_base; break; // datarel: module/section base
	default: return false;             // funcrel and the rest are not emitted here
	}
	if ((enc & 0x80u) != 0) {
		v = FloorReadU64(v);
	}
	if (next != nullptr) {
		*next = p;
	}
	*out = v;
	return true;
}

struct FloorEhCie {
	uint64_t va       = 0;
	uint32_t len      = 0;
	uint8_t  version  = 0;
	bool     has_z    = false;
	char     aug[8]   = {};
	uint8_t  fde_enc  = 0x00;
	uint8_t  lsda_enc = 0xff;
	uint8_t  per_enc  = 0xff;
	uint64_t per_ptr  = 0;
};

static bool FloorEhParseCie(uint64_t va, FloorEhCie* cie) {
	cie->va            = va;
	const uint32_t len = FloorReadU32(va);
	if (len == 0 || len == 0xffffffffu) {
		return false;
	}
	cie->len          = len;
	uint64_t       p  = va + 4;
	const uint32_t id = FloorReadU32(p);
	p += 4;
	if (id != 0) {
		return false;
	}
	cie->version = FloorReadU8(p++);
	if (cie->version != 1 && cie->version != 3) {
		return false;
	}
	for (size_t i = 0; i + 1 < sizeof(cie->aug); i++) {
		const uint8_t c = FloorReadU8(p++);
		if (c == 0) {
			break;
		}
		cie->aug[i] = static_cast<char>(c);
		if (c == 'z') {
			cie->has_z = true;
		}
	}
	(void)FloorUleb(p, &p); // code alignment factor
	(void)FloorSleb(p, &p); // data alignment factor
	(void)FloorUleb(p, &p); // return address register
	if (!cie->has_z) {
		return true;
	}
	const uint64_t alen = FloorUleb(p, &p);
	const uint64_t aend = p + alen;
	for (const char* q = cie->aug; *q != '\0'; q++) {
		if (*q == 'z') {
			continue;
		}
		if (*q == 'L') {
			cie->lsda_enc = FloorReadU8(p++);
		} else if (*q == 'R') {
			cie->fde_enc = FloorReadU8(p++);
		} else if (*q == 'P') {
			cie->per_enc = FloorReadU8(p);
			p++;
			(void)FloorEhEncRead(p, cie->per_enc, 0, &p, &cie->per_ptr);
		} else if (*q == 'S') {
			// signal frame: no augmentation data
		} else {
			break; // unknown field: cannot size it, stop here
		}
	}
	return p <= aend;
}

struct FloorEhModule {
	bool     found         = false;
	uint64_t base          = 0;
	uint64_t hi            = 0;
	uint64_t eh_hdr        = 0;
	uint64_t eh_hdr_size   = 0;
	uint64_t eh_frame      = 0;
	uint64_t eh_frame_size = 0;
};

static bool FloorEhFindModule(uint64_t addr, FloorEhModule* out) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	if (rt == nullptr) {
		return false;
	}
	auto* pg = rt->FindProgramByAddr(addr);
	if (pg == nullptr || pg->elf == nullptr) {
		return false;
	}
	const auto* ehdr = pg->elf->GetEhdr();
	const auto* phdr = pg->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr) {
		return false;
	}
	constexpr uint32_t kPtGnuEhFrame = 0x6474e550;
	out->base                        = pg->base_vaddr;
	out->hi                          = pg->base_vaddr + pg->base_size_aligned;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == kPtGnuEhFrame) {
			out->eh_hdr      = pg->base_vaddr + phdr[i].p_vaddr;
			out->eh_hdr_size = phdr[i].p_memsz;
			break;
		}
	}
	if (out->eh_hdr == 0 || out->eh_hdr_size < 12 || FloorReadU8(out->eh_hdr) != 1) {
		return false;
	}
	uint64_t ef = 0;
	if (!FloorEhEncRead(out->eh_hdr + 4, FloorReadU8(out->eh_hdr + 1), pg->base_vaddr, nullptr, &ef)) {
		return false;
	}
	out->eh_frame = ef;
	if (out->eh_hdr > ef) {
		out->eh_frame_size = out->eh_hdr - ef;
	} else if (out->hi > ef) {
		out->eh_frame_size = out->hi - ef;
	}
	out->found = out->eh_frame_size > 16;
	return out->found;
}

struct FloorEhFde {
	bool     found    = false;
	uint64_t va       = 0;
	uint64_t func     = 0;
	uint64_t range    = 0;
	uint64_t lsda     = 0;
	uint8_t  lsda_enc = 0xff;
};

// Linear .eh_frame walk (entries are self-describing; no eh_frame_hdr table and
// no cache is needed on a fault path). `pc` is the return address minus one, the
// convention the personality routine uses.
static bool FloorEhFindFde(const FloorEhModule& m, uint64_t pc, FloorEhFde* out) {
	const uint64_t lo = m.eh_frame;
	const uint64_t hi = m.eh_frame + m.eh_frame_size;
	for (uint64_t va = lo; va + 8 <= hi;) {
		const uint32_t len = FloorReadU32(va);
		if (len == 0 || len == 0xffffffffu) {
			break;
		}
		const uint64_t next = va + 4 + len;
		if (next <= va || next > hi) {
			break;
		}
		const uint32_t id = FloorReadU32(va + 4);
		if (id != 0) {
			FloorEhCie cie {};
			if (FloorEhParseCie((va + 4) - id, &cie)) {
				uint64_t p     = va + 8;
				uint64_t func  = 0;
				uint64_t range = 0;
				if (FloorEhEncRead(p, cie.fde_enc, m.base, &p, &func) &&
				    FloorEhEncRead(p, static_cast<uint8_t>(cie.fde_enc & 0x0fu), 0, &p, &range) &&
				    func <= pc && pc - func < range) {
					uint64_t lsda = 0;
					if (cie.has_z) {
						const uint64_t alen = FloorUleb(p, &p);
						const uint64_t aend = p + alen;
						for (const char* q = cie.aug; *q != '\0'; q++) {
							if (*q == 'z') {
								continue;
							}
							if (*q == 'L') {
								(void)FloorEhEncRead(p, cie.lsda_enc, m.base, &p, &lsda);
							} else if (*q == 'P' || *q == 'R' || *q == 'S') {
								// no FDE-side augmentation data
							} else {
								break;
							}
						}
						if (p > aend) {
							lsda = 0;
						}
					}
					out->found    = true;
					out->va       = va;
					out->func     = func;
					out->range    = range;
					out->lsda     = lsda;
					out->lsda_enc = cie.lsda_enc;
					return true;
				}
			}
		}
		va = next;
	}
	return false;
}

struct FloorEhSite {
	uint64_t lp       = 0; // absolute landing pad; 0 = unwind-only
	uint64_t filter   = 0; // 0 = cleanup, >0 = type index + 1
	uint64_t catch_ti = 0; // resolved catch type_info; 0 = catch (...)
};

// LSDA call-site lookup for `pc` (function-relative to `func`, Itanium ABI):
//   lpstart_enc, [LPStart], ttype_enc, [ttype_offset], cs_enc, cs_len, sites[],
//   action table; the action chain then yields the type filter for the site.
static bool FloorEhFindSite(uint64_t lsda, uint64_t func, uint64_t pc, FloorEhSite* out) {
	uint64_t       p      = lsda;
	const uint8_t  lp_enc = FloorReadU8(p);
	p++;
	uint64_t lp_base = func;
	if (lp_enc != 0xff) {
		uint64_t nx = 0;
		uint64_t v  = 0;
		if (FloorEhEncRead(p, lp_enc, 0, &nx, &v)) {
			lp_base = v;
			p       = nx;
		}
	}
	const uint8_t ttype_enc = FloorReadU8(p);
	p++;
	uint64_t ttype_off = 0;
	if (ttype_enc != 0 && ttype_enc != 0xff) {
		ttype_off = FloorUleb(p, &p);
	}
	const uint8_t  cs_enc = FloorReadU8(p);
	p++;
	const uint64_t cs_len = FloorUleb(p, &p);
	const uint64_t cs_end = p + cs_len;
	if (pc < lp_base) {
		return false;
	}
	const uint64_t rel      = pc - lp_base;
	uint64_t       site_lp  = 0;
	uint64_t       site_act = 0;
	bool           have     = false;
	while (p + 1 <= cs_end) {
		uint64_t nx = 0;
		uint64_t st = 0;
		uint64_t ln = 0;
		uint64_t lp = 0;
		if (!FloorEhEncRead(p, cs_enc, 0, &nx, &st)) {
			break;
		}
		p = nx;
		if (!FloorEhEncRead(p, cs_enc, 0, &nx, &ln)) {
			break;
		}
		p = nx;
		if (!FloorEhEncRead(p, cs_enc, 0, &nx, &lp)) {
			break;
		}
		p = nx;
		const uint64_t act = FloorUleb(p, &p);
		if (st <= rel && rel < st + ln) {
			site_lp  = lp;
			site_act = act;
			have     = true;
			break;
		}
	}
	if (!have || site_lp == 0) {
		return false;
	}
	out->lp     = lp_base + site_lp;
	out->filter = 0;
	if (site_act == 0) {
		return true; // landing pad with no action: a cleanup
	}
	// Action chain (the action table starts right after the call-site table).
	uint64_t action = site_act;
	for (int guard = 0; action != 0 && guard < 32; guard++) {
		uint64_t       q      = cs_end + (action - 1);
		const uint64_t filter = FloorUleb(q, &q);
		const int64_t  disp   = FloorSleb(q, &q);
		if (filter != 0) {
			out->filter = filter;
			const size_t sz = FloorEhEncSize(ttype_enc);
			if (sz != 0) {
				uint64_t nx = 0;
				(void)FloorEhEncRead(lsda + ttype_off + (filter - 1) * sz, ttype_enc, 0, &nx,
				                     &out->catch_ti);
			}
			return true;
		}
		if (disp == 0) {
			break;
		}
		action = disp < 0 ? static_cast<uint64_t>(-disp)
		                  : static_cast<uint64_t>(static_cast<int64_t>(q) + disp - static_cast<int64_t>(cs_end) + 1);
	}
	out->filter = 0;
	return true;
}

// Itanium can_catch: the catch type matches when it *is* the thrown type or one
// of its bases (type info graph walked by mangled name; the same walk
// __dynamic_cast uses). A zero catch type_info is `catch (...)`.
static bool FloorEhCatchMatches(uint64_t thrown_ti, uint64_t catch_ti) {
	if (catch_ti == 0) {
		return true;
	}
	char want[96];
	if (!FloorTypeInfoNameCopy(catch_ti, want, sizeof(want))) {
		return false;
	}
	char have[96];
	if (FloorTypeInfoNameCopy(thrown_ti, have, sizeof(have)) && std::strcmp(have, want) == 0) {
		return true;
	}
	int64_t off = 0;
	return FloorRttiFindBase(thrown_ti, want, 0, &off);
}

// __cxa_throw(obj, type_info*, void (*)(void*)): no unwinder exists yet, so walk
// the frames, report where the exception would stop and halt loudly instead of
// returning into the guest's ud2/int3 padding.
[[maybe_unused]] static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CxaThrow(uint64_t obj, uint64_t tinfo, uint64_t dtor, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	const uint64_t ra = FloorCaller();
	char           thrown[96];
	const char*    thrown_name =
	    FloorTypeInfoNameCopy(tinfo, thrown, sizeof(thrown)) ? thrown : "<bad-ti>";
	LOGF("ps5-vsh: CXXTHROW obj=0x%016" PRIx64 " ti=0x%016" PRIx64 " type=%s dtor=0x%016" PRIx64
	     " from guest 0x%016" PRIx64 "\n",
	     obj, tinfo, thrown_name, dtor, ra);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// rbp of the guest frame that called us: callee-saved, so it is either
	// untouched by this HLE (then it is still the guest value) or saved exactly
	// one word below our own return-address slot.
	CONTEXT ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	RtlCaptureContext(&ctx);
	const uint64_t ra_slot_addr = reinterpret_cast<uint64_t>(_AddressOfReturnAddress());
	uint64_t       guest_rbp    = ctx.Rbp;
	if (ra_slot_addr >= 16 && ctx.Rbp + 8 == ra_slot_addr) {
		guest_rbp = FloorReadU64(ctx.Rbp);
	}
#else
	uint64_t guest_rbp = 0;
#endif

	bool     have_catch  = false;
	uint64_t catch_lp    = 0;
	uint64_t catch_filt  = 0;
	int      frames      = 0;
	uint64_t fp          = guest_rbp;
	for (; frames < 32; frames++) {
		const uint64_t ip = FloorReadU64(fp + 8);
		if (ip < 0x10000u) {
			break;
		}
		FloorEhModule m {};
		if (!FloorEhFindModule(ip, &m)) {
			LOGF("ps5-vsh: CXXTHROW frame %d ip=0x%016" PRIx64 " outside every module\n", frames,
			     ip);
			break;
		}
		const uint64_t pc = ip - 1;
		FloorEhFde     fde {};
		if (!FloorEhFindFde(m, pc, &fde)) {
			LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
			     " ip=0x%016" PRIx64 " no FDE\n",
			     frames, m.base, pc - m.base, ip);
		} else if (fde.lsda == 0) {
			LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
			     " ip=0x%016" PRIx64 " func=0x%06" PRIx64 " no LSDA\n",
			     frames, m.base, pc - m.base, ip, fde.func - m.base);
		} else {
			FloorEhSite site {};
			if (!FloorEhFindSite(fde.lsda, fde.func, pc, &site)) {
				LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
				     " func=0x%06" PRIx64 " lsda=0x%06" PRIx64 " no call site\n",
				     frames, m.base, pc - m.base, fde.func - m.base, fde.lsda - m.base);
			} else if (site.lp == 0) {
				LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
				     " func=0x%06" PRIx64 " unwind-only\n",
				     frames, m.base, pc - m.base, fde.func - m.base);
			} else if (site.filter == 0) {
				LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
				     " func=0x%06" PRIx64 " cleanup lp=0x%06" PRIx64 "\n",
				     frames, m.base, pc - m.base, fde.func - m.base, site.lp - m.base);
			} else {
				char        cbuf[96];
				const char* cname =
				    FloorTypeInfoNameCopy(site.catch_ti, cbuf, sizeof(cbuf)) ? cbuf : "<bad-ti>";
				const bool match = FloorEhCatchMatches(tinfo, site.catch_ti);
				LOGF("ps5-vsh: CXXTHROW frame %d mod=0x%08" PRIx64 " rva=0x%06" PRIx64
				     " func=0x%06" PRIx64 " handler filter=%" PRIu64 " type=%s match=%d"
				     " lp=0x%06" PRIx64 "\n",
				     frames, m.base, pc - m.base, fde.func - m.base, site.filter, cname,
				     match ? 1 : 0, site.lp - m.base);
				if (match) {
					have_catch = true;
					catch_lp   = site.lp;
					catch_filt = site.filter;
					break;
				}
			}
		}
		const uint64_t next_fp = FloorReadU64(fp);
		if (next_fp <= fp) {
			LOGF("ps5-vsh: CXXTHROW frame %d frame chain broken (fp=0x%016" PRIx64
			     " next=0x%016" PRIx64 ")\n",
			     frames, fp, next_fp);
			break;
		}
		fp = next_fp;
	}

	LOGF("ps5-vsh: CXXTHROW RESULT type=%s frames=%d catch=%d filter=%" PRIu64 " lp=0x%016" PRIx64
	     "\n",
	     thrown_name, frames, have_catch ? 1 : 0, catch_filt, catch_lp);
	EXIT("ps5-vsh: guest C++ throw of %s - no unwinder yet (see CXXTHROW lines)\n", thrown_name);
	EXIT_HALT();
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CThrowMsg(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	FloorThrowLog("eh-throw", a1);
	return 0;
}

// __cxa_allocate_exception: hand out bounded host scratch (never null for
// small requests) so the guest can populate it before __cxa_throw.
[[maybe_unused]] static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CThrowAlloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("ps5-vsh: THROW __cxa_allocate_exception(%llu)\n",
	     static_cast<unsigned long long>(a1));
	if (a1 == 0 || a1 > sizeof(g_cxaScratch)) {
		return 0;
	}
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_cxaScratch[0]));
}

// exception::what(): trapping with null would crash; return static "".
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CWhat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_emptyStr[0]));
}

// operator delete family: accept and ignore (the host heap is never
// corrupted; small floor leaks are acceptable).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CDelete(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

// Aligned/nothrow new[] + aligned_alloc: host malloc (16-aligned on Win64).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CAlignedAlloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	const size_t n = a1 != 0 ? static_cast<size_t>(a1) : 1;
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(::Libs::GuestHeap::Allocate(n))));
}

// localeconv: static zero blob beats a null deref.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CLocaleconv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_lconvBlob[0]));
}

// ---- SEH-guarded narrow-string forwards (null-safe, never crash) ----
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrcspn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(::strcspn(reinterpret_cast<const char*>(
	                              static_cast<uintptr_t>(a1)),
	                          reinterpret_cast<const char*>(static_cast<uintptr_t>(a2)))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrspn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(::strspn(reinterpret_cast<const char*>(
	                              static_cast<uintptr_t>(a1)),
	                          reinterpret_cast<const char*>(static_cast<uintptr_t>(a2)))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrcasecmp(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		LOGF("\t shellcore-floor strcasecmp with null\n");
		return 0;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(static_cast<uint32_t>(::_stricmp(
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)),
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a2))))));
#else
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(static_cast<uint32_t>(::strcasecmp(
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)),
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a2))))));
#endif
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrnlen(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(::strnlen(reinterpret_cast<const char*>(
	                              static_cast<uintptr_t>(a1)),
	                          static_cast<size_t>(a2))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrnstr(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 0;
	}
	// No strnstr on MSVC: bounded substring search (SEH-guarded at call site).
	FLOOR_SEH_RETURN(0, ([a1, a2, a3]() -> uint64_t {
		const char*  s    = reinterpret_cast<const char*>(static_cast<uintptr_t>(a1));
		const char*  f    = reinterpret_cast<const char*>(static_cast<uintptr_t>(a2));
		const size_t n    = static_cast<size_t>(a3);
		const size_t flen = ::strlen(f);
		if (flen == 0) {
			return a1;
		}
		if (flen > n) {
			return 0;
		}
		for (size_t i = 0; i + flen <= n; ++i) {
			if (std::memcmp(s + i, f, flen) == 0) {
				return a1 + i;
			}
		}
		return static_cast<uint64_t>(0);
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMemchr(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
	                              ::memchr(reinterpret_cast<const void*>(static_cast<uintptr_t>(a1)),
	                                       static_cast<int>(a2), static_cast<size_t>(a3)))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrcoll(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(static_cast<uint32_t>(::strcoll(
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)),
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a2))))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrxfrm(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a2 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(::strxfrm(
	                              a1 != 0 ? reinterpret_cast<char*>(static_cast<uintptr_t>(a1))
	                                        : nullptr,
	                              reinterpret_cast<const char*>(static_cast<uintptr_t>(a2)),
	                              static_cast<size_t>(a3))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrlcat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0 || a2 == 0 || a3 == 0) {
		return 0;
	}
	auto*       dst  = reinterpret_cast<char*>(static_cast<uintptr_t>(a1));
	const char* src  = reinterpret_cast<const char*>(static_cast<uintptr_t>(a2));
	const size_t cap = static_cast<size_t>(a3);
	FLOOR_SEH_RETURN(0, ([dst, src, cap]() -> uint64_t {
		const size_t dlen = ::strnlen(dst, cap);
		const size_t slen = ::strlen(src);
		if (dlen < cap) {
			const size_t room = cap - dlen - 1;
			const size_t take = slen < room ? slen : room;
			std::memcpy(dst + dlen, src, take);
			dst[dlen + take] = '\0';
		}
		return static_cast<uint64_t>(dlen + slen);
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrcatS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0 || a3 == 0 || a2 == 0) {
		return 22; // EINVAL, no writes
	}
	auto*       dst  = reinterpret_cast<char*>(static_cast<uintptr_t>(a1));
	const char* src  = reinterpret_cast<const char*>(static_cast<uintptr_t>(a3));
	const size_t cap = static_cast<size_t>(a2);
	FLOOR_SEH_RETURN(22, ([dst, src, cap]() -> uint64_t {
		const size_t dlen = ::strnlen(dst, cap);
		const size_t slen = ::strlen(src);
		if (dlen >= cap || slen >= cap - dlen) {
			return 22; // ERANGE, dst untouched on overflow
		}
		std::memcpy(dst + dlen, src, slen + 1);
		return 0;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrncatS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	if (a1 == 0 || a3 == 0 || a2 == 0) {
		return 22; // EINVAL, no writes
	}
	auto*       dst   = reinterpret_cast<char*>(static_cast<uintptr_t>(a1));
	const char* src   = reinterpret_cast<const char*>(static_cast<uintptr_t>(a3));
	const size_t cap  = static_cast<size_t>(a2);
	const size_t maxn = static_cast<size_t>(a4);
	FLOOR_SEH_RETURN(22, ([dst, src, cap, maxn]() -> uint64_t {
		const size_t dlen = ::strnlen(dst, cap);
		size_t       slen = ::strnlen(src, maxn);
		if (dlen >= cap || slen >= cap - dlen) {
			return 22; // ERANGE, dst untouched on overflow
		}
		std::memcpy(dst + dlen, src, slen);
		dst[dlen + slen] = '\0';
		return 0;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrtoll(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2, a3]() -> uint64_t {
		char*      end = nullptr;
		const auto v =
		    ::strtoll(reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)), &end,
		              static_cast<int>(a3));
		FloorWriteU64(a2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end)));
		return static_cast<uint64_t>(v);
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrtoull(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2, a3]() -> uint64_t {
		char*      end = nullptr;
		const auto v =
		    ::strtoull(reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)), &end,
		               static_cast<int>(a3));
		FloorWriteU64(a2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end)));
		return static_cast<uint64_t>(v);
	})());
}

// _Stolx/_Stoulx/_Stollx/_Stoullx: return the parsed 64-bit value, errno via
// guarded out-param, endptr translated (identity-mapped) when writable.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrToLong(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	if (a1 == 0) {
		FloorWriteU32(a4, 22); // EINVAL
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2, a3, a4]() -> uint64_t {
		char*      end = nullptr;
		const auto v =
		    ::strtoll(reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)), &end,
		              static_cast<int>(a3));
		FloorWriteU64(a2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end)));
		FloorWriteU32(a4, 0);
		return static_cast<uint64_t>(v);
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CToFloat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2]() -> uint64_t {
		char*     end = nullptr;
		const auto f =
		    ::strtof(reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)), &end);
		FloorWriteU64(a2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end)));
		uint32_t bits = 0;
		std::memcpy(&bits, &f, sizeof(bits));
		return bits;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CToDouble(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2]() -> uint64_t {
		char*     end = nullptr;
		const auto d =
		    ::strtod(reinterpret_cast<const char*>(static_cast<uintptr_t>(a1)), &end);
		FloorWriteU64(a2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end)));
		uint64_t bits = 0;
		std::memcpy(&bits, &d, sizeof(bits));
		return bits;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CBasenameR(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (a1 == 0 || a2 == 0 || a3 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2, a3]() -> uint64_t {
		const char* path = reinterpret_cast<const char*>(static_cast<uintptr_t>(a3));
		const char* base = path;
		for (const char* p = path; *p != '\0'; ++p) {
			if (*p == '/') {
				base = p + 1;
			}
		}
		const size_t cap = static_cast<size_t>(a2);
		size_t       len = ::strlen(base);
		if (len >= cap) {
			len = cap - 1;
		}
		char tmp[260] = {};
		std::memcpy(tmp, base, len);
		if (!FloorWrite(a1, tmp, len + 1)) {
			return 0;
		}
		return a1;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStrdup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
	                              ::strdup(reinterpret_cast<const char*>(
	                                  static_cast<uintptr_t>(a1))))));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CRealpath(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, a2]() -> uint64_t {
		const char* path = reinterpret_cast<const char*>(static_cast<uintptr_t>(a1));
		if (a2 != 0 && FloorRangeOk(a2, 260, true)) {
			char tmp[260] = {};
			std::strncpy(tmp, path, sizeof(tmp) - 1);
			FloorWrite(a2, tmp, sizeof(tmp));
			return a2;
		}
		return static_cast<uint64_t>(
		    reinterpret_cast<uintptr_t>(::strdup(path))); // else heap copy
	})());
}

static char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CGetcwd(char* buffer, size_t size) {
	PRINT_NAME();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	try {
#endif
	if (buffer != nullptr && size < 2) {
		*Posix::GetErrorAddr() = size == 0 ? Posix::POSIX_EINVAL : Posix::POSIX_ERANGE;
		return nullptr;
	}
	std::string cwd;
	const int error = LibKernel::FileSystem::GetGuestCurrentDirectory(&cwd);
	if (error != OK) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(error);
		return nullptr;
	}
	const size_t needed = cwd.size() + 1;
	const bool allocated = buffer == nullptr;
	if (allocated) {
		// Firmware 13.60 +78785: null buffer ignores size, starts at 0x400,
		// and doubles as needed. This export belongs to the HLE libc domain:
		// its returned storage must be released by HLE libc free/GuestHeap.
		size = 1024;
		while (size < needed) {
			if (size > SIZE_MAX / 2) { *Posix::GetErrorAddr() = Posix::POSIX_ENOMEM; return nullptr; }
			size *= 2;
		}
		buffer = static_cast<char*>(GuestHeap::Allocate(size));
		if (buffer == nullptr) { *Posix::GetErrorAddr() = Posix::POSIX_ENOMEM; return nullptr; }
	} else if (size < needed) {
		*Posix::GetErrorAddr() = Posix::POSIX_ERANGE;
		return nullptr;
	}
	if (!LibKernel::FileSystem::CopyPathResultToGuest(buffer, cwd.c_str(), needed)) {
		if (allocated) { GuestHeap::Free(buffer); }
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return nullptr;
	}
	return buffer;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	} catch (const std::bad_alloc&) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return nullptr;
	}
#endif
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CBzero(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 0;
	}
	const size_t n = static_cast<size_t>(a2);
	if (!FloorRangeOk(a1, n, true)) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, ([a1, n]() -> uint64_t {
		std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(a1)), 0, n);
		return 0;
	})());
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMemmoveS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	if (a1 == 0 || a3 == 0) {
		return 22; // EINVAL
	}
	const size_t cap = static_cast<size_t>(a2);
	const size_t n   = static_cast<size_t>(a4);
	if (n > cap || !FloorRangeOk(a1, n, true) || !FloorRangeOk(a3, n, false)) {
		return 22; // EINVAL/ERANGE, dst untouched
	}
	FLOOR_SEH_RETURN(22, ([a1, a3, n]() -> uint64_t {
		::memmove(reinterpret_cast<void*>(static_cast<uintptr_t>(a1)),
		          reinterpret_cast<const void*>(static_cast<uintptr_t>(a3)), n);
		return 0;
	})());
}

// filesystem::_Current_get: report root into the caller's buffer.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CCurrentGet(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	static const char kRoot[] = "/";
	if (!FloorWrite(a1, kRoot, sizeof(kRoot))) {
		return 22; // EINVAL
	}
	return 0;
}

// ---- guest-FILE* shims: fread/fclose/fseek/ftell/getc serve real files
// opened via _Fiopen (same GuestFile shims); foreign pointers fail soft
// exactly as before (0 / EOF). ----
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFclose(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<uint64_t>(Libs::GuestFile::Close(reinterpret_cast<void*>(a1)));
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFgets(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0; // NULL = EOF, buffer untouched
}

// ps5-vsh diagnostic: the guest reads host.ps.bundle.hbc through libc++'s
// filebuf, so the interesting question is *which* guest function issued the
// read. The guest is frame-pointer compiled, so walk its saved-RBP chain from
// this HLE entry (same capture trick as CxaThrow) and print every return
// address module-relative. A handful of walks only, to keep the log small.
static void FloorLogGuestFrames(const char* tag, const void* stream, uint64_t guest_ret) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static int budget = [] {
		const char* e = std::getenv("KYTY_FLOOR_WALK");
		return (e == nullptr || e[0] == '0') ? 0 : 6;
	}();
	if (budget <= 0) {
		return;
	}
	FloorEhModule m {};
	if (!FloorEhFindModule(guest_ret, &m)) {
		LOGF("ps5-vsh: %s stream=%p ret=0x%016" PRIx64 " (no module)\n", tag, stream, guest_ret);
		Log::Flush();
		return;
	}
	budget--;
	LOGF("ps5-vsh: %s stream=%p mod=0x%08" PRIx64 " ret_rva=0x%06" PRIx64 "\n", tag, stream,
	     m.base, guest_ret - m.base);
	const auto* slot = static_cast<const uint64_t*>(_AddressOfReturnAddress());
	const uint64_t slot_addr = reinterpret_cast<uint64_t>(slot);
	uint64_t    last = 0;
	for (int i = 0; i < 128; i++) {
		uint64_t v = 0;
		__try {
			v = slot[i];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			LOGF("ps5-vsh:   scan stopped at slot %d\n", i);
			break;
		}
		if (v < m.base || v >= m.hi || v == last) {
			continue;
		}
		last = v;
		LOGF("ps5-vsh:   slot[%d] rva=0x%06" PRIx64 "\n", i, v - m.base);
		// ps5-vsh: if this is the builder's return from istream::read, the
		// frame pointer is one word below the return-address slot; its locals
		// hold the size it computed ([rbp-0x88]) and the raw tellg halves
		// ([rbp-0x80] = low, [rbp-0x78] = high).
		if (v == m.base + 0x1f275) {
			const uint64_t rbp = slot_addr + static_cast<uint64_t>(i) * 8 - 8;
			__try {
				const uint64_t bfp = *reinterpret_cast<const uint64_t*>(rbp);
				const uint64_t bra = *reinterpret_cast<const uint64_t*>(bfp + 8);
				const uint64_t sz  = *reinterpret_cast<const uint64_t*>(bfp - 0x88);
				const uint64_t lo  = *reinterpret_cast<const uint64_t*>(bfp - 0x80);
				const uint64_t hi  = *reinterpret_cast<const uint64_t*>(bfp - 0x78);
				const uint64_t str = *reinterpret_cast<const uint64_t*>(bfp - 0xd8);
				LOGF("ps5-vsh:   builder rbp=0x%016" PRIx64 " ra=0x%016" PRIx64
				     " size=0x%016" PRIx64 " tellg_lo=0x%016" PRIx64
				     " tellg_hi=0x%016" PRIx64 " stream=0x%016" PRIx64 "\n",
				     bfp, bra, sz, lo, hi, str);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				LOGF("ps5-vsh:   builder frame read faulted (rbp=0x%016" PRIx64 ")\n", rbp);
			}
		}
	}
	Log::Flush();
#else
	(void)tag;
	(void)stream;
	(void)guest_ret;
#endif
}

// ps5-vsh: the bundle-read diagnostics below (trace/taps/guards/watchdog) were
// built to answer "does the guest resume on the pushed return address after
// CFread?". They are inert unless KYTY_FLOOR_TRAPS is set so a normal run is
// not perturbed: the watchdog suspends the guest thread, and the stack walk
// takes RuntimeLinker::m_mutex *before* CFread returns.
static bool FloorTrapEnabled() {
	static const bool on = [] {
		const char* e = std::getenv("KYTY_FLOOR_TRAPS");
		return e != nullptr && e[0] != '0';
	}();
	return on;
}

// ps5-vsh: mutex-free trace for the RNPS bundle read path. The shared LOGF
// stream is buffered (2 s flush) and a crash/hang loses the tail, which is
// exactly the region we need; this file is flushed per line and never takes
// the log mutex.
static void BundleTrace(const char* what, uint64_t a, uint64_t b, uint64_t c) {
	if (!FloorTrapEnabled()) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FILE* f = std::fopen("_Runs/bundle_trace.txt", "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "pid=%lu tid=%lu %s a=%llu b=%llu c=0x%016" PRIx64 "\n",
	             static_cast<unsigned long>(GetCurrentProcessId()),
	             static_cast<unsigned long>(GetCurrentThreadId()), what,
	             static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), c);
	std::fflush(f);
	std::fclose(f);
#else
	(void)what;
	(void)a;
	(void)b;
	(void)c;
#endif
}

// ps5-vsh diagnostic (RNPS bundle read), kept as an opt-in probe: one-shot int3
// taps on the guest's libc++ filebuf::xsgetn chunk loop (libNativeExtensions),
// plus PAGE_GUARD on the code pages and a DR0 execute breakpoint. Resolved the
// open question: the guest *does* resume at the pushed return address and does
// issue fread(1 x 4095) then fread(1 x 2129) for a 6224-byte read. The earlier
// "no second read / no trap" observation was caused by FloorLogGuestFrames
// blocking on RuntimeLinker::m_mutex inside CFread before it returned.
// Faithful: replaced instructions are restored, no guest state is altered.
static std::atomic<uint64_t> g_trapAddr[8];
static uint8_t               g_trapOrig[8] {};
static std::atomic<int>      g_trapArmed {0};

static void TrapTrace(const char* fmt, ...) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FILE* f = std::fopen("_Runs/bundle_trap.txt", "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "pid=%lu tid=%lu ", static_cast<unsigned long>(GetCurrentProcessId()),
	             static_cast<unsigned long>(GetCurrentThreadId()));
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fflush(f);
	std::fclose(f);
#else
	(void)fmt;
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static std::atomic<uint64_t> g_selfTestAddr {0};
static std::atomic<uint64_t> g_guardPage[2];
static std::atomic<uint64_t> g_bundleRet {0};
static std::atomic<uint64_t> g_bundleBase {0};
static std::atomic<uint64_t> g_armAddr[8];

// noinline: the address registered for the self-test must be the address that
// actually executes, otherwise the probe silently falls through to the
// emulator's unhandled-breakpoint path (observed in rnpstrap3).
static __attribute__((noinline)) void FloorTrapSelfTestProbe() {
	__debugbreak();
}

static LONG CALLBACK FloorTrapVeh(EXCEPTION_POINTERS* ep) {
	auto* rec = ep->ExceptionRecord;
	if (rec == nullptr) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	auto* ctx = ep->ContextRecord;
	const uint64_t addr = reinterpret_cast<uint64_t>(rec->ExceptionAddress);
	constexpr DWORD kSingleStep = 0x80000004u; // EXCEPTION_SINGLE_STEP (#DB)
	if (rec->ExceptionCode == kSingleStep) {
		if ((ctx->Dr6 & 0x1u) != 0) {
			TrapTrace("hwbp hit rip=0x%016" PRIx64 " dr0=0x%016" PRIx64 " rsp=0x%016" PRIx64
			          " rax=0x%016" PRIx64,
			          addr, ctx->Dr0, ctx->Rsp, ctx->Rax);
			ctx->Dr0 = 0;
			ctx->Dr6 = 0;
			ctx->Dr7 &= ~1ull;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (rec->ExceptionCode == EXCEPTION_BREAKPOINT) {
		const uint64_t st = g_selfTestAddr.load(std::memory_order_relaxed);
		if (st != 0 && addr >= st && addr < st + 16) {
			TrapTrace("selftest hit rip=0x%016" PRIx64 " (veh works)", addr);
			ctx->Rip = addr + 1;
			g_selfTestAddr.store(0, std::memory_order_relaxed);
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		int idx = -1;
		for (int i = 0; i < 8; i++) {
			if (g_trapAddr[i].load(std::memory_order_relaxed) == addr) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		TrapTrace("trap idx=%d rip=0x%016" PRIx64 " rax=0x%016" PRIx64 " rbx=0x%016" PRIx64
		          " rcx=0x%016" PRIx64 " rdx=0x%016" PRIx64 " rsi=0x%016" PRIx64
		          " rdi=0x%016" PRIx64 " r8=0x%016" PRIx64 " r12=0x%016" PRIx64
		          " r13=0x%016" PRIx64 " r14=0x%016" PRIx64 " r15=0x%016" PRIx64
		          " rsp=0x%016" PRIx64 " rbp=0x%016" PRIx64,
		          idx, addr, ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->R8,
		          ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->Rsp, ctx->Rbp);
		DWORD old = 0;
		if (VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old) != 0) {
			*reinterpret_cast<uint8_t*>(addr) = g_trapOrig[idx];
			VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
		}
		g_trapAddr[idx].store(0, std::memory_order_relaxed);
		ctx->Rip = addr;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	constexpr DWORD kGuard = 0x80000001u; // STATUS_GUARD_PAGE_VIOLATION
	if (rec->ExceptionCode == kGuard) {
		for (int i = 0; i < 2; i++) {
			if ((addr & ~0xfffull) == g_guardPage[i].load(std::memory_order_relaxed)) {
				TrapTrace("guard page[%d] entered rip=0x%016" PRIx64 " rax=0x%016" PRIx64
				          " rbx=0x%016" PRIx64 " r12=0x%016" PRIx64 " r13=0x%016" PRIx64
				          " r14=0x%016" PRIx64 " r15=0x%016" PRIx64 " rsp=0x%016" PRIx64,
				          i, addr, ctx->Rax, ctx->Rbx, ctx->R12, ctx->R13, ctx->R14, ctx->R15,
				          ctx->Rsp);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// The guest executes natively on the calling host thread, so suspending that
// thread and reading its CONTEXT yields the guest's own RIP/RSP/registers. The
// bundle read finishes inside this CFread call; a few seconds later this tells
// us whether the guest actually resumed at 0xA302 at all.
static void WatchTrace(const char* fmt, ...) {
	FILE* f = std::fopen("_Runs/bundle_watch.txt", "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "pid=%lu ", static_cast<unsigned long>(GetCurrentProcessId()));
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fflush(f);
	std::fclose(f);
}

// Non-invasive: VirtualQuery reports PAGE_GUARD until the page is actually
// touched, and the kernel clears the bit on the first violation. So a page that
// still reads 0x140 cannot have been entered, and one that reads 0x40 was
// entered (or had its protection rewritten) without us seeing an exception.
static void WatchPageState(int i, int p) {
	const uint64_t pg = g_guardPage[p].load(std::memory_order_relaxed);
	if (pg == 0) {
		return;
	}
	MEMORY_BASIC_INFORMATION mbi {};
	if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(pg)), &mbi, sizeof(mbi)) == 0) {
		WatchTrace("watchdog[%d] page[%d] 0x%016" PRIx64 " VirtualQuery failed err=%lu", i, p, pg,
		           static_cast<unsigned long>(GetLastError()));
		return;
	}
	const bool guarded = (mbi.Protect & PAGE_GUARD) != 0;
	WatchTrace("watchdog[%d] page[%d] 0x%016" PRIx64 " protect=0x%lx guard=%s state=0x%lx",
	           i, p, pg, static_cast<unsigned long>(mbi.Protect), guarded ? "SET" : "CLEARED",
	           static_cast<unsigned long>(mbi.State));
	if (!guarded) {
		// Safe to look at the opcode we planted without consuming a guard.
		const uint64_t arm = g_armAddr[0].load(std::memory_order_relaxed);
		if (arm != 0) {
			WatchTrace("watchdog[%d] byte@0x%016" PRIx64 " = %02x", i, arm,
			           *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(arm)));
		}
	}
}

static DWORD WINAPI FloorTrapWatchdog(LPVOID param) {
	const auto h = static_cast<HANDLE>(param);
	struct CloseTarget { HANDLE handle; ~CloseTarget() { CloseHandle(handle); } } target {h};
	const DWORD tid = GetThreadId(h);
	WatchTrace("watchdog for guest tid=%lu (watchdog tid=%lu)", tid,
	           static_cast<unsigned long>(GetCurrentThreadId()));
	for (int i = 0; i < 6; i++) {
		Sleep(1200);
		if (WaitForSingleObject(h, 0) != WAIT_TIMEOUT) { return 0; }
		DWORD exit_code = 0;
		GetExitCodeThread(h, &exit_code);
		const DWORD prev = SuspendThread(h);
		if (prev == static_cast<DWORD>(-1)) { return 0; }
		CONTEXT  c {};
		c.ContextFlags = CONTEXT_ALL;
		if (GetThreadContext(h, &c) != 0) {
			WatchTrace("watchdog[%d] prev_suspend=%lu exit=%lu rip=0x%016" PRIx64
			           " rsp=0x%016" PRIx64 " rbp=0x%016" PRIx64 " rax=0x%016" PRIx64
			           " rbx=0x%016" PRIx64 " r12=0x%016" PRIx64 " r13=0x%016" PRIx64
			           " r14=0x%016" PRIx64 " r15=0x%016" PRIx64,
			           i, static_cast<unsigned long>(prev), static_cast<unsigned long>(exit_code),
			           c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.R12, c.R13, c.R14, c.R15);
		} else {
			WatchTrace("watchdog[%d]: GetThreadContext failed err=%lu", i,
			           static_cast<unsigned long>(GetLastError()));
		}
		if (i == 0) {
			// Independent of any memory patch: a hardware execute breakpoint on
			// the pushed return address. If DR0 never fires either, the guest
			// genuinely never runs that instruction.
			const uint64_t arm = g_armAddr[0].load(std::memory_order_relaxed);
			CONTEXT d {};
			d.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (arm != 0 && GetThreadContext(h, &d) != 0) {
				d.Dr0 = arm;
				d.Dr7 = (d.Dr7 & ~0xffffull) | 0x1ull; // L0=1, RW0=exec, LEN0=1 byte
				d.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (SetThreadContext(h, &d) != 0) {
					WatchTrace("watchdog[0] hwbp armed dr0=0x%016" PRIx64 " dr7=0x%016" PRIx64, arm,
					           d.Dr7);
				} else {
					WatchTrace("watchdog[0] hwbp SetThreadContext failed err=%lu",
					           static_cast<unsigned long>(GetLastError()));
				}
			} else {
				WatchTrace("watchdog[0] hwbp not armed (arm=0x%016" PRIx64 " ctx err=%lu)", arm,
				           static_cast<unsigned long>(GetLastError()));
			}
		}
		ResumeThread(h);
		WatchPageState(i, 0);
		WatchPageState(i, 1);
		if (exit_code != 259 /* STILL_ACTIVE */) {
			return 0;
		}
	}
	return 0;
}

static void FloorTrapPostCheck() {
	static std::atomic<int> once {0};
	if (g_armAddr[0].load(std::memory_order_relaxed) == 0 || once.exchange(1) != 0) {
		return;
	}
	TrapTrace("post-check: base=0x%016" PRIx64 " ret=0x%016" PRIx64
	          " guard0=0x%016" PRIx64 " guard1=0x%016" PRIx64
	          " (guard still set => guest never entered that page)",
	          g_bundleBase.load(), g_bundleRet.load(), g_guardPage[0].load(),
	          g_guardPage[1].load());
	for (int i = 0; i < 6; i++) {
		const uint64_t a = g_armAddr[i].load(std::memory_order_relaxed);
		if (a == 0) {
			TrapTrace("post idx=%d consumed by trap", i);
			continue;
		}
		TrapTrace("post idx=%d addr=0x%016" PRIx64 " byte=%02x", i, a,
		          *reinterpret_cast<const uint8_t*>(a));
	}
}

static void FloorArmBundleTraps(uint64_t base, uint64_t ret) {
	if (g_trapArmed.exchange(1) != 0 || base == 0) {
		return;
	}
	// Diagnostics only. They rewrite guest opcodes and set guard pages, so they
	// stay inert unless explicitly requested.
	if (!FloorTrapEnabled()) {
		return;
	}
	g_bundleBase.store(base, std::memory_order_relaxed);
	g_bundleRet.store(ret, std::memory_order_relaxed);
	if (AddVectoredExceptionHandler(1, FloorTrapVeh) == nullptr) {
		TrapTrace("arm: AddVectoredExceptionHandler failed");
		return;
	}
	TrapTrace("arm base=0x%016" PRIx64 " ret=0x%016" PRIx64 " ret_rva=0x%06" PRIx64, base, ret,
	          ret - base);
	// Self-test the vectored handler first, so a silent "no trap fired" cannot
	// be confused with "the handler was never installed".
	g_selfTestAddr.store(reinterpret_cast<uint64_t>(&FloorTrapSelfTestProbe));
	TrapTrace("arm: selftest at 0x%016" PRIx64, g_selfTestAddr.load());
	FloorTrapSelfTestProbe();
	// ret: the instruction the guest resumes at after this fread (proves the tap
	// mechanism), then the four loop instructions that decide the second chunk.
	const uint64_t sites[6] = {ret,
	                            base + 0xa2e0,
	                            base + 0xa302,
	                            base + 0xa308,
	                            base + 0xa312,
	                            base + 0xa317};
	for (int i = 0; i < 6; i++) {
		const uint64_t a   = sites[i];
		DWORD          old = 0;
		if (VirtualProtect(reinterpret_cast<void*>(a), 1, PAGE_EXECUTE_READWRITE, &old) == 0) {
			TrapTrace("arm idx=%d addr=0x%016" PRIx64 " VirtualProtect failed (err=%lu)", i, a,
			          static_cast<unsigned long>(GetLastError()));
			continue;
		}
		g_trapOrig[i] = *reinterpret_cast<const uint8_t*>(a);
		g_trapAddr[i].store(a, std::memory_order_relaxed);
		g_armAddr[i].store(a, std::memory_order_relaxed);
		*reinterpret_cast<uint8_t*>(a) = 0xcc;
		VirtualProtect(reinterpret_cast<void*>(a), 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(a), 1);
		MEMORY_BASIC_INFORMATION mbi {};
		VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
		TrapTrace("arm idx=%d rva=0x%06" PRIx64 " addr=0x%016" PRIx64
		          " orig=%02x readback=%02x type=%lu protect=%lx",
		          i, a - base, a, g_trapOrig[i], *reinterpret_cast<const uint8_t*>(a),
		          static_cast<unsigned long>(mbi.Type), static_cast<unsigned long>(mbi.Protect));
	}
	// Independent of the opcode patch: watch the whole 4 KiB pages that hold the
	// return site and the builder's post-read site. If neither guard is ever
	// violated, the guest never resumes on either page after this fread, i.e.
	// 0xA302 was never reached no matter what the stack says.
	{
		const uint64_t pages[2] = {ret & ~0xfffull, (base + 0x1f275) & ~0xfffull};
		for (int i = 0; i < 2; i++) {
			g_guardPage[i].store(pages[i], std::memory_order_relaxed);
			DWORD old = 0;
			if (VirtualProtect(reinterpret_cast<void*>(pages[i]), 0x1000,
			                   PAGE_EXECUTE_READWRITE | PAGE_GUARD, &old) == 0) {
				TrapTrace("arm: guard page[%d] 0x%016" PRIx64 " failed (err=%lu)", i, pages[i],
				          static_cast<unsigned long>(GetLastError()));
			} else {
				TrapTrace("arm: guard page[%d] 0x%016" PRIx64 " armed (old=%lx)", i, pages[i],
				          static_cast<unsigned long>(old));
			}
		}
	}
	{
		LibKernel::PthreadLegacyNativeTarget target;
		if (LibKernel::PthreadPinLegacyNativeTarget(LibKernel::PthreadSelfOrNull(), &target) != 0) { return; }
		// Retain the actual legacy native object for the entire diagnostic;
		// a later numeric thread-ID reuse must never redirect context edits.
		target.UnlockRecord();
		const auto native = static_cast<HANDLE>(target.ReleaseHandle());
		DWORD  wd_tid = 0;
		HANDLE h      = CreateThread(
            nullptr, 0, FloorTrapWatchdog, native, 0, &wd_tid);
		if (h != nullptr) {
			CloseHandle(h);
			WatchTrace("watchdog started for tid=%lu", static_cast<unsigned long>(wd_tid));
		} else {
			CloseHandle(native);
			WatchTrace("watchdog: CreateThread failed err=%lu",
			           static_cast<unsigned long>(GetLastError()));
		}
	}
}
#endif

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFread(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	// fread(dst, size, count, stream).
	const char* bundle_path = Libs::GuestFile::ShimPath(reinterpret_cast<void*>(a4));
	const bool  is_bundle   = bundle_path != nullptr && std::strstr(bundle_path, ".ps.bundle") != nullptr;
	if (is_bundle) {
		BundleTrace("fread-enter", a2, a3, a1);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		{
			const auto     ret =
			    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
			FloorEhModule m {};
			if (FloorEhFindModule(ret, &m)) {
				FloorArmBundleTraps(m.base, ret);
			} else {
				TrapTrace("arm: no module for ret=0x%016" PRIx64, ret);
			}
		}
#endif
	}
	const size_t got = Libs::GuestFile::Read(reinterpret_cast<void*>(a4),
	                                         reinterpret_cast<void*>(a1),
	                                         static_cast<size_t>(a2),
	                                         static_cast<size_t>(a3));
	if (is_bundle) {
		BundleTrace("fread-exit", got, 0, a1);
	}
	LOGF("\t fread(%llu x %llu) -> %llu items (stream=%p ret=0x%016" PRIx64 ")\n",
	     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(got), reinterpret_cast<void*>(a4),
	     static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
	if (is_bundle) {
		// ps5-vsh: decisive probe for the bundle builder. a1 is the guest's own
		// destination buffer on the direct-read path (filebuf::xsgetn 0xA2FD),
		// so the head shown here is exactly what 0x1F060 will test against
		// "RNPSHEDR". Flush so the line survives a crash in the same second.
		uint64_t head = 0;
		if (a1 != 0) {
			__try {
				std::memcpy(&head, reinterpret_cast<const void*>(a1), sizeof(head));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				head = 0;
			}
		}
		LOGF("\t ps5-vsh: bundle fread dst=0x%016" PRIx64 " got=%llu head=%016" PRIx64
		     " tell=%lld\n",
		     a1, static_cast<unsigned long long>(got), head,
		     static_cast<long long>(Libs::GuestFile::Tell(reinterpret_cast<void*>(a4))));
		Log::Flush();
		FloorLogGuestFrames("fread-callers", reinterpret_cast<const void*>(a4),
		                    static_cast<uint64_t>(
		                        reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
	}
	return static_cast<uint64_t>(got);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFseek(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	// fseek(stream, offset, whence).
	(void)a4;
	const int64_t rc = Libs::GuestFile::Seek(reinterpret_cast<void*>(a1),
	                                        static_cast<long>(a2), static_cast<int>(a3));
	LOGF("\t fseek(stream=%p off=%lld whence=%d) -> %lld\n", reinterpret_cast<void*>(a1),
	     static_cast<long long>(static_cast<int64_t>(a2)), static_cast<int>(a3),
	     static_cast<long long>(rc));
	return static_cast<uint64_t>(rc);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFtell(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	// ftell(stream).
	(void)a2;
	(void)a3;
	(void)a4;
	const int64_t pos = Libs::GuestFile::Tell(reinterpret_cast<void*>(a1));
	LOGF("\t ftell(stream=%p) -> %lld\n", reinterpret_cast<void*>(a1),
	     static_cast<long long>(pos));
	return static_cast<uint64_t>(pos);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFgetc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	const int c = Libs::GuestFile::Getc(reinterpret_cast<void*>(a1));
	if (c < 0) {
		return static_cast<uint64_t>(-1); // EOF
	}
	return static_cast<uint64_t>(c);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFputc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	return a1; // echo: success
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFputs(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	if (a1 != 0) {
		FloorPreview(a1);
	}
	(void)a2;
	(void)a3;
	(void)a4;
	return 0; // success
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CUngetc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	return a1; // echo: success
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFeof(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 1; // EOF set: ends guest read loops hitting our 0-byte reads
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFerror(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0; // no error
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CRewind(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CSetvbuf(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFgetpos(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	// ps5-vsh: SHlt7EhOtqA = fgetpos. libc++'s filebuf::seekoff (which backs
	// istream::tellg) calls this, tests the int return for 0, then reads the
	// stored position back out of *a2. Returning 0 without writing *a2 (the old
	// no-op stub) made tellg return uninitialised stack: the RNPS bundle
	// builder then sized its read buffer from that garbage, hit the short-read
	// check and never reached the decrypt import. Write the real offset.
	(void)a3;
	(void)a4;
	const int64_t pos = Libs::GuestFile::Tell(reinterpret_cast<void*>(a1));
	if (const char* path = Libs::GuestFile::ShimPath(reinterpret_cast<void*>(a1));
	    path != nullptr && std::strstr(path, ".ps.bundle") != nullptr) {
		LOGF("\t ps5-vsh: bundle fgetpos stream=%p -> %lld\n", reinterpret_cast<void*>(a1),
		     static_cast<long long>(pos));
		Log::Flush();
	}
	if (pos < 0) {
		return 1; // fgetpos failure is nonzero; *a2 stays untouched
	}
	if (a2 != 0) {
		*reinterpret_cast<int64_t*>(a2) = pos;
	}
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CFsetpos(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	// ps5-vsh: 7PkSz+qnTto = fsetpos, the partner of fgetpos above
	// (filebuf::seekpos). Seek to the offset recorded in the fpos_t.
	(void)a3;
	(void)a4;
	if (a2 == 0) {
		return 1;
	}
	const int64_t pos = *reinterpret_cast<const int64_t*>(a2);
	const int     rc  =
	    Libs::GuestFile::Seek(reinterpret_cast<void*>(a1), static_cast<long>(pos), SEEK_SET);
	if (const char* path = Libs::GuestFile::ShimPath(reinterpret_cast<void*>(a1));
	    path != nullptr && std::strstr(path, ".ps.bundle") != nullptr) {
		LOGF("\t ps5-vsh: bundle fsetpos stream=%p pos=%lld -> %d\n", reinterpret_cast<void*>(a1),
		     static_cast<long long>(pos), rc);
		Log::Flush();
	}
	return rc == 0 ? 0 : 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CClosedir(void* stream) {
	return GuestDirectory::Close(stream);
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CReaddir(void* stream) {
	return GuestDirectory::Read(stream);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CWarnx(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t shellcore-floor warnx\n");
	FloorPreview(a1); // fmt only; varargs never parsed
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CPerror(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t shellcore-floor perror\n");
	FloorPreview(a1);
	return 0;
}

// ---- time stubs (zeroed data, never crash) ----
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CLocaltimeS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	if (a1 == 0 || a2 == 0) {
		return 22; // EINVAL
	}
	static const uint8_t kZeroTm[44] = {}; // portable struct-tm core
	if (!FloorWrite(a1, kZeroTm, sizeof(kZeroTm))) {
		return 22;
	}
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CGmtimeS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	return CLocaltimeS(a1, a2, a3, a4);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CTimespecGet(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	static const uint8_t kZeroTs[16] = {};
	FloorWrite(a1, kZeroTs, sizeof(kZeroTs));
	return 1; // TIME_UTC
}

// ---- misc ----
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CRandom(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<uint64_t>(static_cast<uint32_t>(::rand()));
}
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CAllocFail(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 12; // ENOMEM
}

// ps5-vsh: pure math forwards (voldemort parity). The log-and-0 stubs
// corrupted every computation (pow==0, atan2f==0); host libm is exact.
template <typename F>
static auto FloorMath(F operation) {
	const int saved_errno = errno;
	errno                 = 0;
	auto result           = operation();
	const int math_errno  = errno;
	errno                 = saved_errno;
	if (math_errno == EDOM) {
		*Posix::GetErrorAddr() = Posix::POSIX_EDOM;
	}
	if (math_errno == ERANGE) {
		*Posix::GetErrorAddr() = Posix::POSIX_ERANGE;
	}
	return result;
}

// Firmware 13.60 LibcInternal HUbZmOnT-Dg, RVA 0x49910: double y/x
// arrive in xmm0/xmm1 and the result is returned in xmm0.
static double __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMathAtan2(double y, double x) {
	return FloorMath([&] { return std::atan2(y, x); });
}

static float __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMathAtan2f(float y, float x) {
	return FloorMath([&] { return std::atan2(y, x); });
}

static float __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMathPowf(float x, float y) {
	return FloorMath([&] { return std::pow(x, y); });
}

static double __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMathPow(double x, double y) {
	return FloorMath([&] { return std::pow(x, y); });
}

// ps5-vsh: value classification must be exact, not log-and-0. libhermes
// imports __isnan (GfxAp9Xyiqs) from LibcInternal for ToIntegerOrInfinity;
// with the old 0 stub isnan(NaN) was false, so the trunc path returned NaN
// and every Array/String slice() with a defaulted start produced empty.
// These run on hot paths: no logging.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMathIsNan(double x) {
	return std::isnan(x) ? 1 : 0;
}

// ps5-vsh: wcslen on UTF-16 units (voldemort parity: this console's
// LibcInternal wchar_t is UTF-16). The log-and-0 stub reported every wide
// string empty.
static size_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CWcslen(const uint16_t* s) {
	if (s == nullptr) {
		return 0;
	}
	const auto* end = s;
	while (*end != 0) {
		++end;
	}
	return static_cast<size_t>(end - s);
}

// sceLibcMspaceCalloc(heap?, nmemb, size): host calloc, null-safe either way.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CMspaceCalloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a4;
	if (a2 == 0 || a3 == 0) {
		return 0;
	}
	FLOOR_SEH_RETURN(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
	                              ::calloc(static_cast<size_t>(a2), static_cast<size_t>(a3)))));
}

// snprintf_s: NUL-terminate when possible (varargs never parsed: floor).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CSnprintfS(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	FloorWriteByte(a1, 0);
	return 0;
}

// vswprintf: wide NUL when possible (guest wchar width unknown: 4 zero
// bytes covers both 16- and 32-bit layouts).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CSwprintf(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	FloorWriteU32(a1, 0);
	return 0;
}

FLOOR_NAMED_STUB(Q1BL70XVV0o)
FLOOR_NAMED_STUB(WkkeywLJcgU)
FLOOR_NAMED_STUB(0nV21JjYCH8)
FLOOR_NAMED_STUB(sZLrjxMyEx4)
FLOOR_NAMED_STUB(fnUEjBCNRVU)
FLOOR_NAMED_STUB(QJ5xVfKkni0)
FLOOR_NAMED_STUB(olQfMvTaj2Q)
FLOOR_NAMED_STUB(6paMAa4kTHk)
FLOOR_NAMED_STUB(gntpWkgb4sQ)
FLOOR_NAMED_STUB(77zPOH5zp5I)
// ps5-vsh gate1b: sUP1hBaouOw (_Getpctype) is deliberately NOT stubbed here -
// folly's JSON scanner needs the real LibcInternal table (see libC.cpp).
FLOOR_NAMED_STUB(HHXmPak83kQ)
FLOOR_NAMED_STUB(15TBQ3jmXvo)
FLOOR_NAMED_STUB(taOj6NIm7DM)
FLOOR_NAMED_STUB(LfjFksmiu4Y)
FLOOR_NAMED_STUB(ynwqogpIbL0)
FLOOR_NAMED_STUB(7KPYZMjSAto)
FLOOR_NAMED_STUB(4ITMYcbipA0)
FLOOR_NAMED_STUB(a9DvgsV9dWw)
FLOOR_NAMED_STUB(KSs6Q1X5ABk)
FLOOR_NAMED_STUB(2s6aLdPIA4I)
FLOOR_NAMED_STUB(SwcNvpMAf6c)
FLOOR_NAMED_STUB(PfAmL52MyfQ)
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub_2cXiHqvKFUM(LibKernel::Pthread* thread,
                                                                                    LibKernel::pthread_entry_func_t entry,
                                                                                    void* arg) {
	LOGF("ps5-vsh: _Thrd_start(entry=0x%016" PRIx64 ", arg=%p)\n",
	     reinterpret_cast<uint64_t>(entry), arg);
	int ret = LibKernel::PthreadCreate(thread, nullptr, entry, arg, "SceLibcI_Thr");
	return ret == 0 ? 0 : 4;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub_qnYxp6VUtbI(LibKernel::Pthread thread, int* code) {
	LOGF("ps5-vsh: _Thrd_join(thread=%p, code=%p)\n", static_cast<void*>(thread), static_cast<void*>(code));
	void* val = nullptr;
	int ret = LibKernel::PthreadJoin(thread, &val);
	if (ret == 0) {
		if (code != nullptr) {
			*code = static_cast<int>(reinterpret_cast<intptr_t>(val));
		}
		return 0;
	}
	return 4;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CStub_B7RFX6LbnZI(unsigned long ms) {
	if (ms > 0) {
		LibKernel::KernelUsleep(static_cast<uint64_t>(ms) * 1000u);
	}
}
FLOOR_NAMED_STUB(NFqelqHPxMk)
FLOOR_NAMED_STUB(S0FJMHONta8)
// 13.60 libSceLibcInternal: _Smtx_init/destroy/lock_exclusive/lock_shared
// at 0xda840/0xda8f0/0xda920/0xda950 forward the caller's handle SLOT
// directly to scePthreadRwlock*. Both unlocks (0xdaa80/0xdaab0) forward
// to scePthreadRwlockUnlock. libkernel 0x143de writes one pointer to that
// slot; there is no additional _Smtx allocation or indirection.
static_assert(sizeof(LibKernel::PthreadRwlock) == 8);

static int SmtxResult(int result) {
	// Match libc's return conversion, including its fallback for non-kernel
	// errors. These functions return errno values, not -1/set-errno or void.
	const auto error = static_cast<uint32_t>(result);
	return error == 0 ? 0 : ((error & 0xffff0000u) == 0x80020000u ? static_cast<int>(error & 0xffffu) : 1);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxInit(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockInit(lock, nullptr, nullptr));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxDestroy(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockDestroy(lock));
}

// Use the kernel's cooperative waits: they dispatch pending signals outside
// the rwlock's internal mutex, allowing suspended/GC-stopped guest threads
// to rendezvous while contending. No wrapper lock is held across the wait.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxLockExclusive(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockWrlock(lock));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxLockShared(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockRdlock(lock));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxUnlockExclusive(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockUnlock(lock));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SmtxUnlockShared(LibKernel::PthreadRwlock* lock) {
	return SmtxResult(LibKernel::PthreadRwlockUnlock(lock));
}
FLOOR_NAMED_STUB(EGHjTPlDWvA)
FLOOR_NAMED_STUB(rtuk8eFQQ1U)
FLOOR_NAMED_STUB(TpTMGpWLpEo)
FLOOR_NAMED_STUB(kzfKNNi3eew)
FLOOR_NAMED_STUB(aRtxlukU9WQ)
FLOOR_NAMED_STUB(5XVWOcxpuJ4)
FLOOR_NAMED_STUB(40rZnNs8g7A)
FLOOR_NAMED_STUB(NesIgTmfF0Q)
FLOOR_NAMED_STUB(BKidCxmLC5w)
FLOOR_NAMED_STUB(YZk9sHO0yNg)
FLOOR_NAMED_STUB(V02oFvPMJzA)
FLOOR_NAMED_STUB(aJKn6XP40Z8)
FLOOR_NAMED_STUB(HUbZmOnTMDg)
FLOOR_NAMED_STUB(EHMx713A99c)
FLOOR_NAMED_STUB(9LCjpWyQ5Zc)
FLOOR_NAMED_STUB(1D0H2KNjshE)
FLOOR_NAMED_STUB(dnaeGXbjP6E)
FLOOR_NAMED_STUB(aX8H2PBBlWE)
FLOOR_NAMED_STUB(p6lrRW8MMLY)
FLOOR_NAMED_STUB(mfHdJTIvhuo)
FLOOR_NAMED_STUB(OmG3YPCBLJs)
FLOOR_NAMED_STUB(CCMBLMBu9MI)
FLOOR_NAMED_STUB(KuOuD58hqn4)
FLOOR_NAMED_STUB(NDcSfcYZRC8)
FLOOR_NAMED_STUB(OGybVuPAhAY)
FLOOR_NAMED_STUB(YaXPSk7yIqY)
FLOOR_NAMED_STUB(2voXFbF1zkU)
FLOOR_NAMED_STUB(XQgLZdwqACM)
FLOOR_NAMED_STUB(z2t8wjOcyl8)
FLOOR_NAMED_STUB(LXwQTUlXqkY)
FLOOR_NAMED_STUB(JyYWGwYKbfc)
FLOOR_NAMED_STUB(nwYv13CyIIs)
FLOOR_NAMED_STUB(ZG4QXp4iMRk)
FLOOR_NAMED_STUB(1nTKA7pN1jw)

LIB_DEFINE(InitShellCoreFloorLibc_1) {
	CerrInit();
	// vtable / typeinfo data objects.
	LIB_OBJECT("Z+vcX3rnECg", &g_vtableBadArrayNewLength[0]); // _ZTVSt20bad_array_new_length
	// ps5-vsh: the C++ RTTI vtables are NO LONGER HLE'd here; they must come from
	// the real libSceLibcInternal.sprx, which exports all five (13.60 RVAs
	// 0x1ad7c8 / 0x1ad818 / 0x1ad880 / 0x1ad920 / 0x1ad740). Every guest
	// type_info object stores one of them as its `vptr`, and the real libc's
	// __dynamic_cast / __gxx_personality_v0 catch matching dispatches
	// __do_catch / __do_upcast / __do_dyncast *through that vptr*. Against the
	// old zero-filled HLE blobs those calls jumped into empty slots
	// (`rip=0` guest faults, and a null type_info reaching the comparison
	// helper at libc+0x33a59 that killed run libcfix1 in
	// ReactNative.PUI.PUIReactContext:UpdateAppData). Evidence: libcfix1's
	// `obj r14 @0x1060048208` (typeinfo for folly::ConversionError) has vptr
	// 0x141b981c0 == &g_vtableSiClassTypeInfo[0], per the relocation dump
	// `Relocate: [...] <- 0000000141b981c0, pZ9WXcClPO8[...][Object], Global,
	// &g_vtableSiClassTypeInfo[0]`. Nothing in kyty reads the g_vtable*TypeInfo
	// arrays (FloorRttiFindBase identifies their native RTTI kind), and the guest's own
	// __dynamic_cast still resolves to DynCastProbe below. Removed:
	//   pZ9WXcClPO8  _ZTVN10__cxxabiv120__si_class_type_infoE
	//   9ByRMdo7ywg  _ZTVN10__cxxabiv121__vmi_class_type_infoE
	//   byV+FWlAnB4  _ZTVN10__cxxabiv117__class_type_infoE
	//   aeHxLWwq0gQ  _ZTVN10__cxxabiv119__pointer_type_infoE
	//   CSEjkTYt5dw  _ZTVN10__cxxabiv124__function_type_infoE
	// NOTE (not done here, separate evidence needed): the type_info *objects*
	// still shadowed nearby (_ZTIi, _ZTIPKc, _ZTISt13bad_exception, _ZTIv) and
	// the _ZTVSt* vtables have the identical hazard - their vptr is an HLE blob -
	// so if the real machinery dispatches on one of those, unshadow them too.
	LIB_OBJECT("tVHE+C8vGXk", &g_vtableBadCast[0]); // _ZTVSt8bad_cast
	// 9ByRMdo7ywg (_ZTVN10__cxxabiv121__vmi_class_type_infoE) and byV+FWlAnB4
	// (_ZTVN10__cxxabiv117__class_type_infoE): resolved from the real libc, see
	// the RTTI-vtable note above.
	LIB_OBJECT("EMNG6cHitlQ", &g_vtableBadAlloc[0]); // _ZTVSt9bad_alloc
	// _ZTVSt9exception must resolve from native libc too. Hermes catches real
	// guest exceptions and calls what() at address-point +0x10; the old zero
	// HLE vtable sent that dispatch to PC=0 (run10, Hermes return RVA 0x1394a).
	// Verified 13.60 libc export RVA 0x1acc60, what() target RVA 0x1c4f0.
	LIB_OBJECT("St4apgcBNfo", &g_typeinfoInt[0]); // _ZTIi
	LIB_OBJECT("NLwJ3q+64bY", &g_nothrow[0]); // _ZSt7nothrow
	LIB_OBJECT("H8AprKeZtNg", stderr); // _Stderr
	LIB_OBJECT("T1cG+xkSd-g", &g_cinObj[0]); // cin object (null-ios, 24qw)
	LIB_OBJECT("9Qe7XFSQ4Lk", &g_coutObj[0]); // cout object (null-ios, 24qw)
	LIB_OBJECT("Jk7KrKMQzDw", &g_cerrObj[0]); // cerr object (callable vtable)
	LIB_OBJECT("jRuJ1frROF8", &g_badoff); // _BADOFF = -1
	LIB_OBJECT("3ZotGOwzi9k", &g_facetIdNumPut[0]); // num_put facet id
	LIB_OBJECT("5+FD4VpX+nw", &g_facetIdNumPunct[0]); // numpunct facet id
	LIB_OBJECT("UhVg8P8OCOQ", &g_facetIdCollate[0]); // collate facet id
	LIB_OBJECT("CvgG53ICQZ8", &g_facetIdNumGet[0]); // num_get facet id
	LIB_OBJECT("ef0jk36lGzM", &g_facetIdCodecvtCc[0]); // codecvt<char> facet id
	LIB_OBJECT("OVV3+UimpiY", &g_facetIdCodecvtWc[0]); // codecvt<wchar> facet id
	LIB_OBJECT("EIyErVBW9QI", &g_localeIdCnt[0]); // locale::id::_Id_cnt
	// ps5-vsh: JSCd RTTI objects (audit10: 16 ZERO slots across 6 NIDs).
	LIB_OBJECT("I4y33AOamns", &g_tiConstCharPtr[0]); // _ZTIPKc typeinfo const char*
	// aeHxLWwq0gQ (_ZTVN10__cxxabiv119__pointer_type_infoE) and CSEjkTYt5dw
	// (_ZTVN10__cxxabiv124__function_type_infoE): resolved from the real libc,
	// see the RTTI-vtable note above.
	LIB_OBJECT("Gvp-ypl9t5E", &g_vtBadException[0]); // vtable bad_exception
	LIB_OBJECT("Z7NWh8jD+Nw", &g_tiBadException[0]); // typeinfo bad_exception
	LIB_OBJECT("JrUnjJ-PCTg", &g_tiVoid[0]);         // typeinfo void
	// ps5-vsh: the C++ exception *mechanism* is no longer HLE'd here. The guest's
	// canonical implementation lives in the real libSceLibcInternal.sprx, which
	// the runner now co-hosts (/system/common/lib/libSceLibcInternal.sprx): the
	// imports below resolve to Sony's own __cxa_throw / __gxx_personality_v0 /
	// _Unwind_Resume / __cxa_{allocate_exception,begin_catch,end_catch,rethrow},
	// so the guest runs its real DWARF unwinder instead of a stub that returns
	// into the ud2/int3 padding (the `r12 = -1` limp). An HLE-resolved slot would
	// permanently shadow those exports, so these registrations stay removed:
	//   f1zwJ3jAI2k  _Unwind_Resume
	//   XwLA5cTHjt4  __gxx_personality_v0
	//   3cUUypQzMiI  __cxa_begin_catch
	//   lX+4FNUklF0  __cxa_end_catch
	//   ZL9FV4mJXxo  __cxa_rethrow
	//   vkuuLfhnSZI  __cxa_throw
	//   cfAXurvfl5o  __cxa_allocate_exception
	// CxaThrow (above) keeps the stage-1 frame/LSDA walk available as a
	// diagnostic if the real unwinder has to be backed out.
	// ps5-vsh: the *throw* side must also be Sony's, for the same reason. Every
	// registration in this group returned 0 instead of transferring control,
	// which is undefined behaviour for a non-returning function and corrupts
	// exception state: `std::exception::_Doraise` returning made callers
	// continue with a half-built exception, and the refcount/dtors then ran on
	// it. Symptom was a fault in __gxx_personality_v0 at
	// libSceLibcInternal+0x32C6F (`call [rcx+0x20]`, rcx = [catchType] = 0).
	// The real libc exports all of these, so they now resolve there:
	//   qKQiNX91IGo  __cxa_rethrow_primary_exception
	//   PsrRUg671K0  __cxa_increment_exception_refcount
	//   MQFPAqQPt1s  __cxa_decrement_exception_refcount
	//   XO3N4SBvCy0  bad_array_new_length dtor
	//   47RvLSo2HN8  bad_cast dtor
	//   khbdMADH4cQ  bad_alloc dtor
	//   MOBxtefPZUg  ~exception
	//   a2d1iefnHTk  Error_exception dtor
	//   EvS9U4rxAQk  Error_exception ctor
	//   4CI9DrkapVg  Error_exception copy ctor
	//   tyHd3P7oDrU  exception::_Doraise
	//   Q1BL70XVV0o  uncaught_exception
	//   sJ41UnZr6dQ / pDFe-IgbTPg / fPHZ-n-uRWQ / klLbZjteYdw / HFLNP4R4Elc /
	//   13wBGJrxnSc / P30mDR3cUlA / Dk+iDUjUdk8 / aFYb4pr4bjI / ZdF3BhEh5sk /
	//   okSBuQ1sJdQ / PA+HkSsicW8 / luv6hxzFAhY   (__throw_* / _X* family)
	// Keep HLE only where the real code would kill the host or is a probe:
	LIB_FUNC("hMAe+TWS9mQ", DynCastProbe); // __dynamic_cast (probe types, null = failed cast)
	LIB_FUNC("qYhnoevd9bI", CThrowMsg); // terminate (log only, never abort host)
	LIB_FUNC("3PxvyV7qPPQ", CWhat); // Error_exception::what -> ""
	LIB_FUNC("McsGnqV6yRE", CDelete); // sized delete nothrow
	LIB_FUNC("nwujzxOPXzQ", CDelete); // sized delete
	LIB_FUNC("s2eGsgUF9vk", CAlignedAlloc); // aligned new[] nothrow
	LIB_FUNC("2Btkg8k24Zg", CAlignedAlloc); // aligned_alloc
	// _Throw / _X* family also resolves to the real libc now: they must throw,
	// not return 0 (see the group note above).
	// Narrow strings (real, null-safe).
	LIB_FUNC("q0F6yS-rCms", CStrcspn); // strcspn
	LIB_FUNC("-kU6bB4M-+k", CStrspn); // strspn
	LIB_FUNC("AV6ipCNa4Rw", CStrcasecmp); // strcasecmp
	LIB_FUNC("DQbtGaBKlaw", CStrnlen); // strnlen_s
	LIB_FUNC("Xnrfb2-WhVw", CStrnstr); // strnstr
	LIB_FUNC("8u8lPzUEq+U", CMemchr); // memchr
	LIB_FUNC("ZyciM6GWVO8", CStrcoll); // _Strcoll
	LIB_FUNC("EvPpDIUyQPI", CStrxfrm); // _Strxfrm
	LIB_FUNC("ByfjUZsWiyg", CStrlcat); // strlcat
	LIB_FUNC("K+gcnFFJKVc", CStrcatS); // strcat_s
	LIB_FUNC("NC4MSB+BRQg", CStrncatS); // strncat_s
	LIB_FUNC("VOBg+iNwB-4", CStrtoll); // strtoll
	LIB_FUNC("5OqszGpy7Mg", CStrtoull); // strtoull
	LIB_FUNC("g-McpZfseZo", CStrtoull); // strtouq
	LIB_FUNC("q+9E0X3aWpU", CStrtoull); // _Stoull
	LIB_FUNC("bearuT1s7C0", CStrToLong); // _Stolx
	LIB_FUNC("l9qFfwjoLF0", CStrToLong); // _Stoulx
	LIB_FUNC("9kHKVS+3PzY", CStrToLong); // _Stollx
	LIB_FUNC("XORecIx0t+c", CStrToLong); // _Stoullx
	LIB_FUNC("CpWcnrEZbLA", CToFloat); // _Stof
	LIB_FUNC("c41UEHVtiEA", CToDouble); // _Stod
	LIB_FUNC("vsK6LzRtRLI", CBasenameR); // basename_r
	LIB_FUNC("g7zzzLDYGw0", CStrdup); // strdup
	LIB_FUNC("vhtcIgZG-Lk", CRealpath); // realpath
	LIB_FUNC("DYivN1nO-JQ", CGetcwd); // process-scoped HLE getcwd
	LIB_FUNC("9oiX1kyeedA", CBzero); // bzero
	LIB_FUNC("B59+zQQCcbU", CMemmoveS); // memmove_s
	LIB_FUNC("3y6Rg-iaDY8", CCurrentGet); // filesystem::_Current_get -> "/"
	LIB_FUNC("I0vuMo7qix8", SceSetw); // setw -> _Smanip{pfn} (real layout, below)
	LIB_FUNC("3BytPOQgVKc", CSnprintfS); // snprintf_s
	LIB_FUNC("u0XOsuOmOzc", CSwprintf); // vswprintf
	// Wide-char (UTF-16 units, voldemort parity).
	LIB_FUNC("WkkeywLJcgU", CWcslen); // wcslen (real)
	LIB_FUNC("0nV21JjYCH8", CStub_0nV21JjYCH8); // wcsncpy
	LIB_FUNC("sZLrjx-yEx4", CStub_sZLrjxMyEx4); // wcstombs_s
	LIB_FUNC("fnUEjBCNRVU", CStub_fnUEjBCNRVU); // wmemchr
	LIB_FUNC("QJ5xVfKkni0", CStub_QJ5xVfKkni0); // wmemcmp
	LIB_FUNC("olQf-vTaj2Q", CStub_olQfMvTaj2Q); // _Mbrtowc
	LIB_FUNC("6pa-Aa4kTHk", CStub_6paMAa4kTHk); // _Wcrtomb
	// Ctype tables: registered as call-safe func-0 (see note).
	LIB_FUNC("gntpWkgb4sQ", CStub_gntpWkgb4sQ); // _Tolower
	LIB_FUNC("77z+OH5zp5I", CStub_77zPOH5zp5I); // _Toupper
	// sUP1hBaouOw _Getpctype lives in LibcInternal (real table).
	LIB_FUNC("HHXmPak83kQ", CStub_HHXmPak83kQ); // _Getcvt
	LIB_FUNC("15TBQ3jmXvo", CStub_15TBQ3jmXvo); // _Getcoll
	LIB_FUNC("0hlfW1O4Aa4", CLocaleconv); // localeconv
	// Locale machinery (no-op success).
	LIB_FUNC("taOj6NIm7DM", CStub_taOj6NIm7DM); // _Locinfo_dtor
	LIB_FUNC("LfjFksmiu4Y", CStub_LfjFksmiu4Y); // _Addstd
	LIB_FUNC("ynwqogpIbL0", CStub_ynwqogpIbL0); // _New_Locimp
	LIB_FUNC("7KPYZ-jSAto", CStub_7KPYZMjSAto); // _Locimp_Addfac
	LIB_FUNC("4ITMYcbipA0", CStub_4ITMYcbipA0); // _Syserror_map
	LIB_FUNC("a9DvgsV9dWw", CStub_a9DvgsV9dWw); // _Syserror_map_search
	// Thread/mutex/cond primitives (success no-ops; never touch guest sync).
	LIB_FUNC("KSs6Q1X5ABk", CStub_KSs6Q1X5ABk); // _Cnd_destroy
	LIB_FUNC("2s6aLdPIA4I", CStub_2s6aLdPIA4I); // _Cnd_do_broadcast_at_thread_exit
	LIB_FUNC("SwcNvp-Af6c", CStub_SwcNvpMAf6c); // _Cnd_unregister_at_thread_exit
	LIB_FUNC("+fAmL52-yfQ", CStub_PfAmL52MyfQ); // _Cnd_register_at_thread_exit
	LIB_FUNC("qnYxp6VUtbI", CStub_qnYxp6VUtbI); // _Thrd_join
	LIB_FUNC("2cXiHqvKFUM", CStub_2cXiHqvKFUM); // _Thrd_start
	LIB_FUNC("B7RFX6LbnZI", CStub_B7RFX6LbnZI); // _Thrd_sleep_for (no sleep)
	LIB_FUNC("NFqelqH+xMk", CStub_NFqelqHPxMk); // _Mtx_timedlock
	LIB_FUNC("S0FJ-HONta8", CStub_S0FJMHONta8); // _Mtx_trylock
	LIB_FUNC("cIAHg-0N6Kg", SmtxDestroy); // _Smtx_destroy
	LIB_FUNC("6c2FkLIH5mE", SmtxInit); // _Smtx_init
	LIB_FUNC("5czuG5NjGx8", SmtxLockExclusive); // _Smtx_lock_exclusive
	LIB_FUNC("3PbA4AE88VQ", SmtxUnlockExclusive); // _Smtx_unlock_exclusive
	LIB_FUNC("u04PD3lnHUU", SmtxLockShared); // _Smtx_lock_shared
	LIB_FUNC("lt6pE0WHY94", SmtxUnlockShared); // _Smtx_unlock_shared
	LIB_FUNC("EGHjTPlDWvA", CNonZero); // init_once_begin (nonzero = run init)
	LIB_FUNC("rtuk8eFQQ1U", CStub_rtuk8eFQQ1U); // init_once_link_alternate_names_and_abort
	LIB_FUNC("TpT-GpWLpEo", CStub_TpTMGpWLpEo); // atomic_notify
	LIB_FUNC("kzfKNNi3eew", CStub_kzfKNNi3eew); // atomic_wait_64
	// STL internals (no-op success).
	LIB_FUNC("aRtxlukU9WQ", CStub_aRtxlukU9WQ); // find_trivial_1
	LIB_FUNC("5XVWOcxpuJ4", CStub_5XVWOcxpuJ4); // count_trivial_1
	LIB_FUNC("40rZnNs8g7A", CStub_40rZnNs8g7A); // bitset_to_string_1
	// reverse_trivially_swappable_1 (tJtD8WHnb04) resolves to native libc.
	// Hermes requires the actual [begin,end) byte reversal for radix conversion
	// and string hashing; a success-only override corrupts both consumers.
	LIB_FUNC("NesIgTmfF0Q", CStub_NesIgTmfF0Q); // bsearch (callback: never invoke)
	LIB_FUNC("BKidCxmLC5w", CStub_BKidCxmLC5w); // _Stoldx (long-double ABI: report 0)
	// Math (floor: +0.0; isfinite commonly-true).
	// frexp (kA-TdiOCsaY) resolves to native libc: Hermes requires its
	// exponent output to terminate non-decimal number conversion.
	LIB_FUNC("YZk9sHO0yNg", CStub_YZk9sHO0yNg); // frexpl
	LIB_FUNC("dhK16CKwhQg", CNonZero); // __isfinite
	LIB_FUNC("3-zCDXatSU4", CNonZero); // __isfinitel
	LIB_FUNC("V02oFv+-JzA", CStub_V02oFvPMJzA); // __isinf -> 0
	LIB_FUNC("GfxAp9Xyiqs", CMathIsNan); // __isnan (real: libhermes ToIntegerOrInfinity)
	LIB_FUNC("aJKn6X+40Z8", CStub_aJKn6XP40Z8); // ceill
	LIB_FUNC("HUbZmOnT-Dg", CMathAtan2); // atan2
	LIB_FUNC("EH-x713A99c", CMathAtan2f); // atan2f (real, voldemort parity)
	LIB_FUNC("9LCjpWyQ5Zc", CMathPow); // pow (real, voldemort parity)
	LIB_FUNC("1D0H2KNjshE", CMathPowf); // powf (real, voldemort parity)
	LIB_FUNC("dnaeGXbjP6E", CStub_dnaeGXbjP6E); // exp2
	LIB_FUNC("aX8H2+BBlWE", CStub_aX8H2PBBlWE); // ldexpl
	// Heap telemetry / mspace (no-op; mspace calloc backed by host).
	LIB_FUNC("qWESlyXMI3E", CAllocFail); // MspacePosixMemalign -> ENOMEM
	LIB_FUNC("LYo3GhIlB38", CMspaceCalloc); // MspaceCalloc
	LIB_FUNC("p6lrRW8-MLY", CStub_p6lrRW8MMLY); // MspaceReallocalign
	LIB_FUNC("mfHdJTIvhuo", CStub_mfHdJTIvhuo); // MspaceMallocStats
	LIB_FUNC("OmG3YPCBLJs", CStub_OmG3YPCBLJs); // PafMspaceMallocStatsFast
	LIB_FUNC("CC-BLMBu9-I", CStub_CCMBLMBu9MI); // malloc_stats
	LIB_FUNC("KuOuD58hqn4", CStub_KuOuD58hqn4); // malloc_stats_fast
	LIB_FUNC("NDcSfcYZRC8", CStub_NDcSfcYZRC8); // malloc_usable_size
	LIB_FUNC("OGybVuPAhAY", CStub_OGybVuPAhAY); // reallocalign
	LIB_FUNC("cVSk9y8URbc", LibcInternal::PosixMemalign); // posix_memalign (tracked, voldemort parity)
	// Filesystem (success no-ops; never touch host fs).
	LIB_FUNC("YaXPSk7yIqY", CStub_YaXPSk7yIqY); // filesystem::_Read_dir
	LIB_FUNC("2voXFbF1zkU", CStub_2voXFbF1zkU); // filesystem::_Close_dir
	LIB_FUNC("XQgLZdwqACM", CStub_XQgLZdwqACM); // filesystem::_Open_dir
	LIB_FUNC("z2t8wjOcyl8", CStub_z2t8wjOcyl8); // filesystem::_Remove_dir
	LIB_FUNC("LXwQTUlXqkY", CStub_LXwQTUlXqkY); // filesystem::_Lstat
	LIB_FUNC("JyYWGwYKbfc", CStub_JyYWGwYKbfc); // filesystem::_Unlink
	LIB_FUNC("nwYv13CyIIs", CStub_nwYv13CyIIs); // filesystem::_Chmod
	LIB_FUNC("ZG4QXp4iMRk", CStub_ZG4QXp4iMRk); // filesystem::_Copy_file
	// stdio on guest FILE* (never forward; EOF-flavored).
	LIB_FUNC("KdP-nULpuGw", CFgets); // fgets -> NULL
	LIB_FUNC("uodLYyUip20", CFclose); // fclose
	LIB_FUNC("aZK8lNei-Qw", CFputc); // fputc
	LIB_FUNC("-LFO7jhD5CE", CUngetc); // ungetc
	LIB_FUNC("AEuF3F2f8TA", CFgetc); // fgetc -> EOF
	LIB_FUNC("lbB+UlZqVG0", CFread); // fread -> 0
	LIB_FUNC("rQFVBXp-Cxg", CFseek); // fseek
	LIB_FUNC("SHlt7EhOtqA", CFgetpos); // fgetpos
	LIB_FUNC("7PkSz+qnTto", CFsetpos); // fsetpos
	LIB_FUNC("QMFyLoqNxIg", CSetvbuf); // setvbuf
	LIB_FUNC("Qazy8LmXTvw", CFtell); // ftell
	LIB_FUNC("AHxyhN96dy4", CFerror); // ferror -> 0
	LIB_FUNC("LxcEU+ICu8U", CFeof); // feof -> EOF
	LIB_FUNC("3QIPIh-GDjw", CRewind); // rewind
	LIB_FUNC("QrZZdJ8XsX0", CFputs); // fputs
	LIB_FUNC("XepdqehVYe4", CClosedir); // closedir
	LIB_FUNC("lybyyKtP54c", CReaddir); // readdir
	LIB_FUNC("aNJaYyn0Ujo", CWarnx); // warnx
	LIB_FUNC("EMutwaQ34Jo", CPerror); // perror
	// Time.
	LIB_FUNC("1nTKA7pN1jw", CStub_1nTKA7pN1jw); // utime
	LIB_FUNC("fiiNDnNBKVY", CLocaltimeS); // localtime_s
	LIB_FUNC("5bBacGLyLOs", CGmtimeS); // gmtime_s
	LIB_FUNC("yrx+40YKqiI", CTimespecGet); // timespec_get
	// Misc.
	LIB_FUNC("w1o05aHJT4c", CRandom); // random
}

} // namespace LibcFloor

LIB_DEFINE(InitShellCoreFloor_1) {
	KernelFloor::InitShellCoreFloorKernel_1(s);
	LibcFloor::InitShellCoreFloorLibc_1(s);
}

uint64_t Ps5VshFakeBufAddr() {
	return reinterpret_cast<uint64_t>(&LibcFloor::g_fakeBuf[0]);
}

} // namespace ShellCoreFloor
} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp (append only, STEP 1) ----
// namespace ShellCoreFloor {
// LIB_DEFINE(InitShellCoreFloor_1);
// } // namespace ShellCoreFloor
//
// In Libs::InitAll(), alongside the other Init* calls:
//   ShellCoreFloor::InitShellCoreFloor_1(s);
