#include "libs/libs.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"
#include "loader/timer.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <new>
#include <thread>

namespace Libs {
namespace {

// ps5-vsh: last-HLE-call registry for the idle-thread watchdog below.
// Fixed slots, lock-free writes (plain stores of stable string literals +
// atomic time/tid). 2048 slots comfortably cover guest thread counts;
// collisions overwrite (LIFO wins) and are validated by tid on read.
struct HleCallSlot {
	std::atomic<uint64_t> tid {0};
	std::atomic<uint64_t> os_tid {0};
	const char*           library {nullptr};
	const char*           module {nullptr};
	const char*           function {nullptr};
	const char*           what {nullptr};
	std::atomic<uint64_t> entry_ms {0};
	std::atomic<uint64_t> exit_ms {0};
	std::atomic<uint64_t> reported_entry {0};
	std::atomic<uint64_t> reported_exit {0};
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> calls_snap {0};
	std::atomic<uint64_t> consec {0};
	std::atomic<uint64_t> consec_reported {0};
};

constexpr size_t kHleCallSlots = 2048;
HleCallSlot    g_hle_call_slots[kHleCallSlots];

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void SampleHostThread(uint64_t tid, uint64_t os_tid, const char* state) {
	if (os_tid == 0 || os_tid == static_cast<uint64_t>(::GetCurrentThreadId())) {
		return;
	}
	HANDLE h = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, static_cast<DWORD>(os_tid));
	if (h != nullptr) {
		if (::SuspendThread(h) != static_cast<DWORD>(-1)) {
			CONTEXT ctx {};
			ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (::GetThreadContext(h, &ctx)) {
				uint8_t code[16] = {};
				SIZE_T  code_read = 0;
				::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rip),
				                    code, sizeof(code), &code_read);

				uint64_t stack[4] = {};
				SIZE_T   stack_read = 0;
				::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp),
				                    stack, sizeof(stack), &stack_read);

				// The sampled thread may own the logger. Release only our suspend
				// count before entering any logging or formatting path.
				const DWORD resumed = ::ResumeThread(h);
				if (resumed == static_cast<DWORD>(-1) || resumed > 1) {
					::CloseHandle(h);
					return;
				}
				LOGF("ps5-vsh: sample host=%llu os=%llu (%s): RIP=0x%016llx RSP=0x%016llx RAX=0x%016llx RBX=0x%016llx\n"
				     "         code: %02x %02x %02x %02x %02x %02x %02x %02x\n"
				     "         ret0: 0x%016llx ret1: 0x%016llx ret2: 0x%016llx\n",
				     static_cast<unsigned long long>(tid),
				     static_cast<unsigned long long>(os_tid),
				     state,
				     static_cast<unsigned long long>(ctx.Rip),
				     static_cast<unsigned long long>(ctx.Rsp),
				     static_cast<unsigned long long>(ctx.Rax),
				     static_cast<unsigned long long>(ctx.Rbx),
				     code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
				     static_cast<unsigned long long>(stack[0]),
				     static_cast<unsigned long long>(stack[1]),
				     static_cast<unsigned long long>(stack[2]));
			} else {
				::ResumeThread(h);
			}
		}
		::CloseHandle(h);
	}
}
#endif

} // namespace

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestSampleHostThread(uint64_t os_tid) { SampleHostThread(0, os_tid, "test"); }
#endif

// ps5-vsh: per-thread HLE trace (main-thread hunt). 0 = off; otherwise
// every HLE call on that host thread logs (capped to keep volume sane).
std::atomic<uint64_t> g_trace_tid {0};
std::atomic<uint64_t> g_trace_left {1000000};

void TraceHostThread(uint64_t tid) {
	g_trace_tid.store(tid, std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_no_foreground_user {false};
std::atomic<bool> g_direct_title_boot {false};
} // namespace

void SetNoForegroundUser(bool v) {
	g_no_foreground_user.store(v, std::memory_order_relaxed);
	LOGF("ps5-vsh: boot-flags: NoForegroundUser=%d\n", v ? 1 : 0);
}

bool NoForegroundUser() {
	return g_no_foreground_user.load(std::memory_order_relaxed);
}

void SetDirectTitleBoot(bool v) {
	g_direct_title_boot.store(v, std::memory_order_relaxed);
	LOGF("ps5-vsh: boot-flags: DirectTitleBoot=%d\n", v ? 1 : 0);
}

bool IsDirectTitleBoot() {
	return g_direct_title_boot.load(std::memory_order_relaxed);
}

void NoteHleCall(const char* library, const char* module, const char* function) {
	const uint64_t tid = static_cast<uint64_t>(Common::Thread::GetThreadIdUnique());
	auto&          slot = g_hle_call_slots[tid % kHleCallSlots];
	slot.tid.store(tid, std::memory_order_relaxed);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (slot.os_tid.load(std::memory_order_relaxed) == 0) {
		slot.os_tid.store(static_cast<uint64_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
	}
#endif
	slot.library = library;
	slot.module  = module;
	if (slot.function == function) {
		slot.consec.fetch_add(1, std::memory_order_relaxed);
	} else {
		slot.function = function;
		slot.consec.store(1, std::memory_order_relaxed);
		slot.consec_reported.store(0, std::memory_order_relaxed);
	}
	slot.what     = nullptr;
	slot.entry_ms.store(static_cast<uint64_t>(Loader::Timer::GetTimeMs()),
	                    std::memory_order_relaxed);
	slot.calls.fetch_add(1, std::memory_order_relaxed);
	const uint64_t trace_tid = g_trace_tid.load(std::memory_order_relaxed);
	if (trace_tid != 0 && trace_tid == tid) {
		const uint64_t c = slot.consec.load(std::memory_order_relaxed);
		if (c <= 10 || (c % 1000 == 0)) {
			LOGF("ps5-vsh: trace-main host=%llu (x%llu): %s::%s::%s\n",
			     static_cast<unsigned long long>(tid),
			     static_cast<unsigned long long>(c),
			     library, module, function);
		}
	}
}

void NoteHleReturn() {
	const uint64_t tid = static_cast<uint64_t>(Common::Thread::GetThreadIdUnique());
	auto&          slot = g_hle_call_slots[tid % kHleCallSlots];
	if (slot.tid.load(std::memory_order_relaxed) == tid) {
		slot.exit_ms.store(static_cast<uint64_t>(Loader::Timer::GetTimeMs()),
		                   std::memory_order_relaxed);
	}
}

HleSlowCallGuard::HleSlowCallGuard(const char* func_, const char* what_)
    : func(func_), what(what_),
      t0(static_cast<uint64_t>(Loader::Timer::GetTimeMs())) {
	// Publish the waited object for the watchdog's blocked-in reports.
	// Also refresh entry state: without this, a thread blocked in a
	// guard-only HLE (no PRINT_NAME) keeps its previous slot identity.
	const uint64_t tid = static_cast<uint64_t>(Common::Thread::GetThreadIdUnique());
	auto&          slot = g_hle_call_slots[tid % kHleCallSlots];
	if (slot.tid.load(std::memory_order_relaxed) == tid) {
		slot.function = func_;
		slot.what     = what_;
		slot.entry_ms.store(t0, std::memory_order_relaxed);
	}
}

HleSlowCallGuard::~HleSlowCallGuard() {
	NoteHleReturn();
	const uint64_t took =
	    static_cast<uint64_t>(Loader::Timer::GetTimeMs()) - t0;
	if (took > 5000) {
		// NOTE: no guest thread name here (libs.cpp stays decoupled from
		// kernel/pthread.h); host id via watchdog + object name suffice.
		try {
			LOGF("ps5-vsh: slow-hle: %s(%s) blocked %llums\n", func,
			     what != nullptr ? what : "?",
			     static_cast<unsigned long long>(took));
		} catch (const std::exception&) {
			// Logging-only boundary: preserve the HLE result; fatal asserts still abort.
		}
	}
}

static void StartIdleThreadWatchdog() {
	// Reports threads with no HLE call for >20s (parked mains, deadlocks,
	// exited threads). Each park reports once per last-call timestamp.
	std::thread([] {
		LOGF("ps5-vsh: idle-thread watchdog armed (5s threshold)\n");
		uint64_t last_scan_ms = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			const uint64_t now = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
			const bool         first_scan =
			    last_scan_ms == 0; // skip rates on the first pass: the delta
			                       // covers process start (init burst artifact).
			const double dt_s = first_scan ? 5.0 : (now - last_scan_ms) / 1000.0;
			last_scan_ms      = now;
			// Report hot loopers (spin/poll/sleep loops burn here).
			for (auto& slot : g_hle_call_slots) {
				const uint64_t tid = slot.tid.load(std::memory_order_relaxed);
				if (tid == 0) {
					continue;
				}
				const uint64_t c  = slot.calls.load(std::memory_order_relaxed);
				const uint64_t cs = slot.calls_snap.load(std::memory_order_relaxed);
				if (c < cs) {
					slot.calls_snap.store(c, std::memory_order_relaxed);
					continue;
				}
				const uint64_t delta = c - cs;
				slot.calls_snap.store(c, std::memory_order_relaxed);
				const uint64_t rate =
				    dt_s > 0.0 ? static_cast<uint64_t>(delta / dt_s) : 0;
				if (!first_scan && rate > 500) {
					LOGF("ps5-vsh: hot-loop: host=%llu %s::%s::%s ~%llu/s\n",
					     static_cast<unsigned long long>(tid),
					     slot.library != nullptr ? slot.library : "?",
					     slot.module != nullptr ? slot.module : "?",
					     slot.function != nullptr ? slot.function : "?",
					     static_cast<unsigned long long>(rate));
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					SampleHostThread(tid, slot.os_tid.load(std::memory_order_relaxed), "hot-loop");
#endif
				}
			}
			for (auto& slot : g_hle_call_slots) {
				const uint64_t tid = slot.tid.load(std::memory_order_relaxed);
				if (tid == 0) {
					continue;
				}
				// Sustained same-function loops (sleep-loops, quiet spins):
				// hot-loop catches rate, this catches repetition. Report on
				// doubling counts to avoid 15s spam for steady loops.
				const uint64_t consec = slot.consec.load(std::memory_order_relaxed);
				const uint64_t rep_consec =
				    slot.consec_reported.load(std::memory_order_relaxed);
				if (consec > 2000 && consec >= rep_consec * 2) {
					slot.consec_reported.store(consec, std::memory_order_relaxed);
					LOGF("ps5-vsh: loop-detect: host=%llu %s::%s::%s x%llu consecutive\n",
					     static_cast<unsigned long long>(tid),
					     slot.library != nullptr ? slot.library : "?",
					     slot.module != nullptr ? slot.module : "?",
					     slot.function != nullptr ? slot.function : "?",
					     static_cast<unsigned long long>(consec));
				}
			}
			for (auto& slot : g_hle_call_slots) {
				const uint64_t tid = slot.tid.load(std::memory_order_relaxed);
				if (tid == 0) {
					continue;
				}
				const uint64_t entry = slot.entry_ms.load(std::memory_order_relaxed);
				const uint64_t exit  = slot.exit_ms.load(std::memory_order_relaxed);
				if (entry > exit) {
					// Still inside the call: genuine block.
					const uint64_t threshold = (tid == 6 ? 3000 : 5000);
					if (now < entry + threshold ||
					    slot.reported_entry.load(std::memory_order_relaxed) == entry) {
						continue;
					}
					slot.reported_entry.store(entry, std::memory_order_relaxed);
					LOGF("ps5-vsh: blocked-in: host=%llu %s::%s::%s(%s) for %llus\n",
					     static_cast<unsigned long long>(tid),
					     slot.library != nullptr ? slot.library : "?",
					     slot.module != nullptr ? slot.module : "?",
					     slot.function != nullptr ? slot.function : "?",
					     slot.what != nullptr ? slot.what : "?",
					     static_cast<unsigned long long>((now - entry) / 1000));
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					SampleHostThread(tid, slot.os_tid.load(std::memory_order_relaxed), "blocked-in");
#endif
				} else {
					// Returned long ago: guest-spinning or exited.
					const uint64_t threshold = (tid == 6 ? 3000 : 5000);
					if (now < exit + threshold ||
					    slot.reported_exit.load(std::memory_order_relaxed) == exit) {
						continue;
					}
					slot.reported_exit.store(exit, std::memory_order_relaxed);
					LOGF("ps5-vsh: idle-since: host=%llu last=%s::%s::%s idle=%llus\n",
					     static_cast<unsigned long long>(tid),
					     slot.library != nullptr ? slot.library : "?",
					     slot.module != nullptr ? slot.module : "?",
					     slot.function != nullptr ? slot.function : "?",
					     static_cast<unsigned long long>((now - exit) / 1000));
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					SampleHostThread(tid, slot.os_tid.load(std::memory_order_relaxed), "idle-since");
#endif
				}
			}
		}
	}).detach();
}

namespace LibContentDelete {
LIB_DEFINE(InitContentDelete_1);
} // namespace LibContentDelete

namespace LibContentExport {
LIB_DEFINE(InitContentExport_1);
} // namespace LibContentExport

namespace LibContentSearch {
LIB_DEFINE(InitContentSearch_1);
} // namespace LibContentSearch

namespace VideoDec2 {
LIB_DEFINE(InitVideoDec2_1);
} // namespace VideoDec2

namespace Vdecsw {
LIB_DEFINE(InitVdecsw_1);
} // namespace Vdecsw

namespace LibMouse {
LIB_DEFINE(InitMouse_1);
} // namespace LibMouse

namespace LibKeyboard {
LIB_DEFINE(InitKeyboard_1);
} // namespace LibKeyboard

namespace Ime {
LIB_DEFINE(InitPlatform_1_Ime);
} // namespace Ime

namespace LibUlt {
LIB_DEFINE(InitUlt_1);
} // namespace LibUlt

namespace LibPsml {
LIB_DEFINE(InitPsml_1);
} // namespace LibPsml

namespace LibCes {
LIB_DEFINE(InitCes_1);
} // namespace LibCes

namespace LibC {
LIB_DEFINE(InitLibC_1);
} // namespace LibC

namespace LibRegMgr {
LIB_DEFINE(InitRegMgr_1);
} // namespace LibRegMgr

namespace LibSystemStateMgr {
LIB_DEFINE(InitSystemStateMgr_1);
} // namespace LibSystemStateMgr

namespace LibAvSetting {
LIB_DEFINE(InitAvSetting_1);
} // namespace LibAvSetting

namespace LibVrTracker2 {
LIB_DEFINE(InitVrTracker2_1);
} // namespace LibVrTracker2

namespace LibFsInternalForVsh {
LIB_DEFINE(InitFsInternalForVsh_1);
} // namespace LibFsInternalForVsh

namespace LibNetBandwidth {
LIB_DEFINE(InitNetBandwidth_1);
} // namespace LibNetBandwidth

namespace LibNpCommon {
LIB_DEFINE(InitNpCommon_1);
} // namespace LibNpCommon

namespace LibKernelTelemetry {
LIB_DEFINE(InitKernelTelemetry_1);
} // namespace LibKernelTelemetry

namespace LibIpmiConfig {
LIB_DEFINE(InitIpmiConfig_1);
} // namespace LibIpmiConfig

namespace LibJson2Iter {
LIB_DEFINE(InitJson2Iter_1);
} // namespace LibJson2Iter

namespace LibHttpCache {
LIB_DEFINE(InitHttpCache_1);
} // namespace LibHttpCache

namespace LibLibcFs {
LIB_DEFINE(InitLibcFs_1);
} // namespace LibLibcFs

namespace LibIpmiClient {
LIB_DEFINE(InitIpmiClient_1);
} // namespace LibIpmiClient

namespace LibNpManager {
LIB_DEFINE(InitNpManager_1);
} // namespace LibNpManager

namespace LibNpWebApi2 {
LIB_DEFINE(InitNpWebApi2_1);
} // namespace LibNpWebApi2

namespace LibSysCore {
LIB_DEFINE(InitSysCore_1);
} // namespace LibSysCore

namespace LibAppMessaging {
LIB_DEFINE(InitAppMessaging_1);
} // namespace LibAppMessaging

namespace LibNetConfig {
LIB_DEFINE(InitNetConfig_1);
} // namespace LibNetConfig

namespace LibNetCtl {
LIB_DEFINE(InitNetCtl_1);
} // namespace LibNetCtl

namespace LibHttp {
LIB_DEFINE(InitHttp_1);
} // namespace LibHttp

namespace LibAudioOut {
LIB_DEFINE(InitAudioOut_1);
} // namespace LibAudioOut

namespace LibMbus {
LIB_DEFINE(InitMbus_1);
} // namespace LibMbus

namespace LibAbstractStorage {
LIB_DEFINE(InitAbstractStorage_1);
} // namespace LibAbstractStorage

namespace LibLibcMem {
LIB_DEFINE(InitLibcMem_1);
} // namespace LibLibcMem

namespace LibCompositor {
LIB_DEFINE(InitCompositor_1);
} // namespace LibCompositor

namespace LibAmpr {
LIB_DEFINE(InitAmpr_1);
} // namespace LibAmpr

namespace Coredump {
LIB_DEFINE(InitCoredump_1);
} // namespace Coredump

namespace LibRazorCpu {
LIB_DEFINE(InitRazorCpu_1);
} // namespace LibRazorCpu

namespace Fiber {
LIB_DEFINE(InitFiber_1);
} // namespace Fiber

namespace LibRtc {
LIB_DEFINE(InitRtc_1);
} // namespace LibRtc

namespace LibGen5 {
LIB_DEFINE(InitVideoOut_1);
namespace VrrStatus {
LIB_DEFINE(InitVideoOutVrrStatus_1);
} // namespace VrrStatus
} // namespace LibGen5

namespace MetadataReaderWriter {
LIB_DEFINE(InitMetadataReaderWriter_1);
} // namespace MetadataReaderWriter

namespace VideoRecordingP {
LIB_DEFINE(InitVideoRecordingP_1);
} // namespace VideoRecordingP

namespace VideoCoreInterface {
LIB_DEFINE(InitVideoCoreInterface_1);
} // namespace VideoCoreInterface

namespace VideoStreamingEngine {
LIB_DEFINE(InitVideoStreamingEngine_1);
} // namespace VideoStreamingEngine

namespace AgcVshStubs {
LIB_DEFINE(InitAgcVshStubs_1);
} // namespace AgcVshStubs

namespace LncUtil {
LIB_DEFINE(InitLncUtil_1);
} // namespace LncUtil

namespace RnpsBundle {
LIB_DEFINE(InitRnpsBundle_1);
} // namespace RnpsBundle

namespace RnpsAppMgr {
LIB_DEFINE(InitRnpsAppMgr_1);
} // namespace RnpsAppMgr

namespace BgsRpc {
LIB_DEFINE(InitBgsRpc_1);
} // namespace BgsRpc

namespace MbusWire {
LIB_DEFINE(InitMbusWire_1);
} // namespace MbusWire

namespace ShellFocus {
LIB_DEFINE(InitShellFocus_1);
} // namespace ShellFocus

namespace MediaSrvSysSvc {
LIB_DEFINE(InitMediaSrvSysSvc_1);
} // namespace MediaSrvSysSvc

namespace MediaSrvKernel {
LIB_DEFINE(InitMediaSrvKernel_1);
} // namespace MediaSrvKernel

namespace MediaSrvMedia {
LIB_DEFINE(InitMediaSrvMedia_1);
} // namespace MediaSrvMedia

namespace JscdUser {
LIB_DEFINE(InitJscdUser_1);
} // namespace JscdUser

namespace JscdNet {
LIB_DEFINE(InitJscdNet_1);
} // namespace JscdNet

namespace JscdMisc {
LIB_DEFINE(InitJscdMisc_1);
} // namespace JscdMisc

namespace Jsc {
LIB_DEFINE(InitJsc_1);
} // namespace Jsc

namespace JscdSmall {
LIB_DEFINE(InitJscdSmall_1);
} // namespace JscdSmall

namespace JscdTail {
LIB_DEFINE(InitJscdTail_1);
} // namespace JscdTail

namespace AgcPeri {
LIB_DEFINE(InitAgcPeri_1);
} // namespace AgcPeri

namespace AgcCore {
LIB_DEFINE(InitAgcCore_1);
} // namespace AgcCore

namespace AgcComp {
LIB_DEFINE(InitAgcComp_1);
} // namespace AgcComp

namespace SysAudio {
LIB_DEFINE(InitSysAudio_1);
} // namespace SysAudio

namespace SysInput {
LIB_DEFINE(InitSysInput_1);
} // namespace SysInput

namespace SysNet {
LIB_DEFINE(InitSysNet_1);
} // namespace SysNet

namespace SysKern {
LIB_DEFINE(InitSysKern_1);
} // namespace SysKern

namespace SysProc {
LIB_DEFINE(InitSysProc_1);
} // namespace SysProc

namespace AgcVshBulk {
LIB_DEFINE(InitAgcVshBulk_1);
} // namespace AgcVshBulk

namespace Composite {
LIB_DEFINE(InitComposite_1);
} // namespace Composite
namespace MediaFramework {
LIB_DEFINE(InitMediaFramework_1);
}

namespace HermesVm {
LIB_DEFINE(InitHermesVm_1);
} // namespace HermesVm

namespace SceJsi {
LIB_DEFINE(InitSceJsi_1);
} // namespace SceJsi

namespace ShellCoreFloor {
LIB_DEFINE(InitShellCoreFloor_1);
} // namespace ShellCoreFloor

namespace FsSandbox {
LIB_DEFINE(InitFsSandbox_1);
} // namespace FsSandbox

namespace OfflineLogin {
LIB_DEFINE(InitOfflineLogin_1);
} // namespace OfflineLogin

namespace ShellHits {
// ps5-vsh: runs LAST in InitAll so its exact-key registrations override
// older LogStubs via insert_or_assign (exact + NID-fallback paths).
LIB_DEFINE(InitShellHits_1);
} // namespace ShellHits

LIB_DEFINE(InitAppContent_1);
LIB_DEFINE(InitAudio_1);
LIB_DEFINE(InitBluetoothHid_1);
LIB_DEFINE(InitConvertKeycode_1);
LIB_DEFINE(InitDbgAddressSanitizer_1);
LIB_DEFINE(InitDeviceService_1);
LIB_DEFINE(InitDialog_1);
LIB_DEFINE(InitFont_1);
LIB_DEFINE(InitFontFt_1);
LIB_DEFINE(InitAgcDriver_1);
LIB_DEFINE(InitHmd2_1);
LIB_DEFINE(InitJpegDec_1);
LIB_DEFINE(InitJpegEnc_1);
LIB_DEFINE(InitVrTracker2_1);
LIB_DEFINE(InitLibKernel_1);
LIB_DEFINE(InitNet_1);
LIB_DEFINE(InitNpManagerClient_1);
LIB_DEFINE(InitPad_1);
LIB_DEFINE(InitPlayGo_1);
LIB_DEFINE(InitPngDec_1);
LIB_DEFINE(InitPngEnc_1);
LIB_DEFINE(InitPlatform_1);
LIB_DEFINE(InitRudp_1);
LIB_DEFINE(InitSaveData_1);
LIB_DEFINE(InitShare_1);
LIB_DEFINE(InitSysmodule_1);
LIB_DEFINE(InitSystemService_1);
LIB_DEFINE(InitTextToSpeech2_1);
LIB_DEFINE(InitUsbd_1);
LIB_DEFINE(InitUserService_1);
LIB_DEFINE(InitWebBrowserDialog_1);

void InitAll(Loader::SymbolDatabase* s) {
	InitAudio_1(s);
	InitConvertKeycode_1(s);
	LibAmpr::InitAmpr_1(s);
	InitAppContent_1(s);
	VideoCoreInterface::InitVideoCoreInterface_1(s);
	VideoStreamingEngine::InitVideoStreamingEngine_1(s);
	AgcVshStubs::InitAgcVshStubs_1(s);
	LncUtil::InitLncUtil_1(s);
	RnpsBundle::InitRnpsBundle_1(s);
	RnpsAppMgr::InitRnpsAppMgr_1(s);
	BgsRpc::InitBgsRpc_1(s);
	MbusWire::InitMbusWire_1(s);
	ShellFocus::InitShellFocus_1(s);
	MediaSrvSysSvc::InitMediaSrvSysSvc_1(s);
	MediaSrvKernel::InitMediaSrvKernel_1(s);
	MediaSrvMedia::InitMediaSrvMedia_1(s);
	JscdUser::InitJscdUser_1(s);
	JscdNet::InitJscdNet_1(s);
	JscdMisc::InitJscdMisc_1(s);
	Jsc::InitJsc_1(s);
	JscdSmall::InitJscdSmall_1(s);
	JscdTail::InitJscdTail_1(s);
	AgcPeri::InitAgcPeri_1(s);
	AgcCore::InitAgcCore_1(s);
	AgcComp::InitAgcComp_1(s);
	SysAudio::InitSysAudio_1(s);
	SysInput::InitSysInput_1(s);
	SysNet::InitSysNet_1(s);
	SysKern::InitSysKern_1(s);
	SysProc::InitSysProc_1(s);
	AgcVshBulk::InitAgcVshBulk_1(s);
	Composite::InitComposite_1(s);
	MediaFramework::InitMediaFramework_1(s);
	HermesVm::InitHermesVm_1(s);
	SceJsi::InitSceJsi_1(s);
	ShellCoreFloor::InitShellCoreFloor_1(s);
	FsSandbox::InitFsSandbox_1(s);
	OfflineLogin::InitOfflineLogin_1(s);
	Coredump::InitCoredump_1(s);
	LibContentDelete::InitContentDelete_1(s);
	LibContentExport::InitContentExport_1(s);
	LibContentSearch::InitContentSearch_1(s);
	LibC::InitLibC_1(s);
	LibRegMgr::InitRegMgr_1(s);
	LibSystemStateMgr::InitSystemStateMgr_1(s);
	LibAvSetting::InitAvSetting_1(s);
	LibVrTracker2::InitVrTracker2_1(s);
	LibFsInternalForVsh::InitFsInternalForVsh_1(s);
	LibNetBandwidth::InitNetBandwidth_1(s);
	LibNpCommon::InitNpCommon_1(s);
	LibKernelTelemetry::InitKernelTelemetry_1(s);
	LibIpmiConfig::InitIpmiConfig_1(s);
	LibJson2Iter::InitJson2Iter_1(s);
	LibHttpCache::InitHttpCache_1(s);
	LibLibcFs::InitLibcFs_1(s);
	LibIpmiClient::InitIpmiClient_1(s);
	LibNpManager::InitNpManager_1(s);
	LibNpWebApi2::InitNpWebApi2_1(s);
	LibSysCore::InitSysCore_1(s);
	LibAppMessaging::InitAppMessaging_1(s);
	LibNetConfig::InitNetConfig_1(s);
	LibNetCtl::InitNetCtl_1(s);
	LibHttp::InitHttp_1(s);
	LibAudioOut::InitAudioOut_1(s);
	LibMbus::InitMbus_1(s);
	LibAbstractStorage::InitAbstractStorage_1(s);
	LibLibcMem::InitLibcMem_1(s);
	LibCompositor::InitCompositor_1(s);
	LibCes::InitCes_1(s);
	InitDbgAddressSanitizer_1(s);
	LibRazorCpu::InitRazorCpu_1(s);
	InitDialog_1(s);
	Fiber::InitFiber_1(s);
	InitFont_1(s);
	InitFontFt_1(s);
	InitAgcDriver_1(s);
	InitBluetoothHid_1(s);
	InitDeviceService_1(s);
	InitUsbd_1(s);
	InitHmd2_1(s);
	InitJpegDec_1(s);
	InitJpegEnc_1(s);
	InitVrTracker2_1(s);
	InitLibKernel_1(s);
	LibMouse::InitMouse_1(s);
	LibKeyboard::InitKeyboard_1(s);
	Ime::InitPlatform_1_Ime(s);
	InitNet_1(s);
	InitNpManagerClient_1(s);
	InitPad_1(s);
	InitPlayGo_1(s);
	LibPsml::InitPsml_1(s);
	InitPngDec_1(s);
	InitPngEnc_1(s);
	InitPlatform_1(s);
	InitRudp_1(s);
	LibRtc::InitRtc_1(s);
	InitSaveData_1(s);
	InitShare_1(s);
	InitSysmodule_1(s);
	InitSystemService_1(s);
	InitTextToSpeech2_1(s);
	LibUlt::InitUlt_1(s);
	InitUserService_1(s);
	VideoDec2::InitVideoDec2_1(s);
	Vdecsw::InitVdecsw_1(s);
	LibGen5::InitVideoOut_1(s);
	LibGen5::VrrStatus::InitVideoOutVrrStatus_1(s);
	MetadataReaderWriter::InitMetadataReaderWriter_1(s);
	VideoRecordingP::InitVideoRecordingP_1(s);
	InitWebBrowserDialog_1(s);
	ShellHits::InitShellHits_1(s);

	StartIdleThreadWatchdog();
}

namespace LibContentExport {

LIB_VERSION("ContentExport", 1, "ContentExport", 1, 1);

namespace ContentExport {

constexpr int CONTENT_EXPORT_ERROR_INVALID_PARAM = -2137182186; /* 0x809D3016 */

struct ContentExportInitParam2 {
	void*   malloc_func;
	void*   free_func;
	void*   user_data;
	size_t  buffer_size;
	int64_t reserved0;
	int64_t reserved1;
};

static bool g_initialized = false;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ContentExportInit2(const ContentExportInitParam2* init_param) {
	PRINT_NAME();

	LOGF("\t init_param  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));

	if (init_param == nullptr) {
		return CONTENT_EXPORT_ERROR_INVALID_PARAM;
	}

	LOGF("\t malloc_func = 0x%016" PRIx64 "\n"
	     "\t free_func   = 0x%016" PRIx64 "\n"
	     "\t user_data   = 0x%016" PRIx64 "\n"
	     "\t buffer_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(init_param->malloc_func),
	     reinterpret_cast<uint64_t>(init_param->free_func),
	     reinterpret_cast<uint64_t>(init_param->user_data),
	     static_cast<uint64_t>(init_param->buffer_size));

	g_initialized = true;

	return OK;
}

} // namespace ContentExport

LIB_DEFINE(InitContentExport_1) {
	LIB_FUNC("0GnN4QCgIfs", ContentExport::ContentExportInit2);
}

} // namespace LibContentExport

namespace LibContentSearch {

LIB_VERSION("ContentSearch", 1, "ContentSearch", 1, 0);

namespace ContentSearch {

constexpr int CONTENT_SEARCH_ERROR_INVALID_PARAM = -2137190397; /* 0x809D1003 */

struct ContentSearchInitParam {
	size_t memory_size;
};

static bool g_initialized = false;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ContentSearchInit(const ContentSearchInitParam* init_param) {
	PRINT_NAME();

	LOGF("\t init_param  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));

	if (init_param == nullptr) {
		return CONTENT_SEARCH_ERROR_INVALID_PARAM;
	}

	LOGF("\t memory_size = %" PRIu64 "\n", static_cast<uint64_t>(init_param->memory_size));

	g_initialized = true;

	return OK;
}

} // namespace ContentSearch

LIB_DEFINE(InitContentSearch_1) {
	LIB_FUNC("dPj4ZtRcIWk", ContentSearch::ContentSearchInit);
}

} // namespace LibContentSearch

namespace LibContentDelete {

LIB_VERSION("ContentDelete", 1, "ContentDelete", 1, 1);

namespace ContentDelete {

constexpr int CONTENT_DELETE_ERROR_INVALID_PARAM = -2137174015; /* 0x809D5001 */

struct ContentDeleteInitParam {
	char   reserved1[4];
	size_t heap_size;
	char   reserved2[32];
};

static bool g_initialized = false;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ContentDeleteInitialize(const ContentDeleteInitParam* init_param) {
	PRINT_NAME();

	LOGF("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));

	if (init_param == nullptr) {
		return CONTENT_DELETE_ERROR_INVALID_PARAM;
	}

	LOGF("\t heap_size  = %" PRIu64 "\n", static_cast<uint64_t>(init_param->heap_size));

	g_initialized = true;

	return OK;
}

} // namespace ContentDelete

LIB_DEFINE(InitContentDelete_1) {
	LIB_FUNC("zoxb0wEChEM", ContentDelete::ContentDeleteInitialize);
}

} // namespace LibContentDelete

} // namespace Libs
