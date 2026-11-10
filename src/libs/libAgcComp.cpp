#include "common/abi.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "libs/agcQueueIdentity.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ps5-vsh: AgcCompositor.elf (PS5 compositor daemon) unresolved-import close-out
// (14 unresolved NID imports). Raw request keys verified verbatim (NID[lib]):
//   AgcDriver_v1 [AgcDriver_v1.1] (12):
//     zP4ZNlXLBVg sceAgcDriverCreateQueue
//     XNbrdwCsZ9A sceAgcDriverDestroyQueue
//     NQLvUnSDyBU sceAgcDriverSetValueToSharedGds
//     SOo3Lwyglfo sceAgcDriverHp3dLockStart
//     u2g5VER9UnM sceAgcDriverHp3dLockEnd
//     e-YMQ+2tj9M sceAgcDriverGetTFRing
//     r28hEh6cNH0 sceAgcDriverGetHsOffchipParam
//     ftf-xlfBQpo sceAgcDriverSysIsGameClosed
//     kuE1uTiWfuk sceAgcDriverSysGetClientNumber
//     x3K61sY5m8Q sceAgcDriverSysEnableSubmitDone45Exception
//     1BUTwixUG5Y sceAgcDriverDebugHardwareStatus
//     EcaEt4nv8No sceAgcDriverSysGetGfxAppGpuResetStatus
//   AgcVsh_v1 [AgcVsh_v1.1] (2):
//     PxEFhy0d5v8 sceAgcDcbClearState
//     Aozdh0bCcO0 unknown AgcVsh func (no nids.csv row; name unresolved)
// Absence check: every NID above grepped over src/libs -> all 14 clean,
// 0 hits (no overlap with libAgcDriver.cpp Graphics5/Gr5Driver or
// libAgcVshBulk.cpp / libPs5VshVideo.cpp AgcVsh stubs).
// Semantics: tracers that return success (PRINT_NAME + LOGF NID+name+args),
// null-safe, no guest writes, never halt. Host-real behavior only where
// trivially safe: fake queue handles (static counter from 1), client number 1,
// not-closed / no-reset zeros.

namespace Libs {

namespace AgcComp {

namespace AgcCompDriver {

LIB_VERSION("AgcDriver", 1, "AgcDriver", 1, 1);

namespace {

// Fake queue handles: monotonically increasing nonzero IDs starting at 1.
// Never 0 (0 = invalid handle to the caller).
uint64_t g_agcCompQueueNext = 1;

} // namespace

// zP4ZNlXLBVg sceAgcDriverCreateQueue(index, out, ...): 0 = success and the
// created queue goes to *out (rsi). The guest DEREFERENCES the handle as an
// object (member write at +0x10 observed), so it must be real zeroed guest
// memory, not a cookie. 4KB RW each; never freed (driver lifetime).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverCreateQueue(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	uint64_t handle = 0;
	if (a1 != 0) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION mi {};
		bool writable = false;
		if (VirtualQuery(reinterpret_cast<const void*>(a1), &mi, sizeof(mi)) != 0 &&
		    mi.State == MEM_COMMIT) {
			const DWORD prot = mi.Protect & 0xffu;
			writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
			            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
		}
		if (writable) {
			handle = Libs::LibKernel::Memory::AllocateRuntimeMemory(
			    0, 0x1000, Common::VirtualMemory::Mode::ReadWrite, "agc-queue", false);
			if (handle != 0) {
				__try {
					*reinterpret_cast<uint64_t*>(a1) = handle;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					handle = 0;
				}
			}
		}
#else
		handle = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, 0x1000, Common::VirtualMemory::Mode::ReadWrite, "agc-queue", false);
		if (handle != 0 && a1 != 0) {
			*reinterpret_cast<uint64_t*>(a1) = handle;
		}
#endif
	}
	// Graphics-path ids get the driver context identity (+0 header, +4 queue id) that
	// AgcDriverSubmit*CommandBuffer* reads back; other ids keep the zeroed context.
	const bool graphics_identity =
	    handle != 0 && Libs::Graphics::AgcQueue::IsGraphicsCreateQueueId(a0);
	if (graphics_identity) {
		auto*          context  = reinterpret_cast<uint8_t*>(handle);
		const uint32_t header   = Libs::Graphics::AgcQueue::CONTEXT_HEADER_VALUE;
		const auto     queue_id = static_cast<uint32_t>(a0);
		std::memcpy(context + Libs::Graphics::AgcQueue::CONTEXT_HEADER_OFFSET, &header,
		            sizeof(header));
		std::memcpy(context + Libs::Graphics::AgcQueue::CONTEXT_QUEUE_ID_OFFSET, &queue_id,
		            sizeof(queue_id));
	}
	LOGF("\t zP4ZNlXLBVg sceAgcDriverCreateQueue a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " -> 0 (queue 0x%016llx %s, identity %s, eop_event_id %d)\n",
	     a0, a1, a2, a3, static_cast<unsigned long long>(handle),
	     handle != 0 ? "written" : "NOT written",
	     graphics_identity ? "graphics" : "unmodeled",
	     graphics_identity ? Libs::Graphics::AgcQueue::GraphicsEopEventIdForQueue(
	                             static_cast<uint32_t>(a0))
	                       : -1);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// XNbrdwCsZ9A sceAgcDriverDestroyQueue: accept any handle, report success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverDestroyQueue(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t XNbrdwCsZ9A sceAgcDriverDestroyQueue a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// NQLvUnSDyBU sceAgcDriverSetValueToSharedGds: log-only success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverSetValueToSharedGds(uint64_t a0, uint64_t a1, uint64_t a2,
                                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t NQLvUnSDyBU sceAgcDriverSetValueToSharedGds a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// SOo3Lwyglfo sceAgcDriverHp3dLockStart: log-only success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverHp3dLockStart(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	LOGF("\t SOo3Lwyglfo sceAgcDriverHp3dLockStart a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// u2g5VER9UnM sceAgcDriverHp3dLockEnd: log-only success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverHp3dLockEnd(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t u2g5VER9UnM sceAgcDriverHp3dLockEnd a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// e-YMQ+2tj9M sceAgcDriverGetTFRing: null ring; log-only success, no guest writes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverGetTFRing(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t e-YMQ+2tj9M sceAgcDriverGetTFRing (null ring) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// r28hEh6cNH0 sceAgcDriverGetHsOffchipParam: log-only success, no guest writes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverGetHsOffchipParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t r28hEh6cNH0 sceAgcDriverGetHsOffchipParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// ftf-xlfBQpo sceAgcDriverSysIsGameClosed: 0 = not closed.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverSysIsGameClosed(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ftf-xlfBQpo sceAgcDriverSysIsGameClosed (not closed) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// kuE1uTiWfuk sceAgcDriverSysGetClientNumber: 1 (we are client 0+1).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverSysGetClientNumber(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t kuE1uTiWfuk sceAgcDriverSysGetClientNumber a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 1\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 1;
}

// x3K61sY5m8Q sceAgcDriverSysEnableSubmitDone45Exception: log-only success.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverSysEnableSubmitDone45Exception(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t x3K61sY5m8Q sceAgcDriverSysEnableSubmitDone45Exception a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// 1BUTwixUG5Y sceAgcDriverDebugHardwareStatus: log-only success, no guest writes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverDebugHardwareStatus(uint64_t a0, uint64_t a1, uint64_t a2,
                                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 1BUTwixUG5Y sceAgcDriverDebugHardwareStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// EcaEt4nv8No sceAgcDriverSysGetGfxAppGpuResetStatus: 0 = no reset.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDriverSysGetGfxAppGpuResetStatus(uint64_t a0, uint64_t a1,
                                                                  uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t EcaEt4nv8No sceAgcDriverSysGetGfxAppGpuResetStatus (no reset) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitAgcCompDriver_1) {
	LIB_FUNC("zP4ZNlXLBVg", AgcDriverCreateQueue);                 // sceAgcDriverCreateQueue
	LIB_FUNC("XNbrdwCsZ9A", AgcDriverDestroyQueue);                // sceAgcDriverDestroyQueue
	LIB_FUNC("NQLvUnSDyBU", AgcDriverSetValueToSharedGds);         // sceAgcDriverSetValueToSharedGds
	LIB_FUNC("SOo3Lwyglfo", AgcDriverHp3dLockStart);               // sceAgcDriverHp3dLockStart
	LIB_FUNC("u2g5VER9UnM", AgcDriverHp3dLockEnd);                 // sceAgcDriverHp3dLockEnd
	LIB_FUNC("e-YMQ+2tj9M", AgcDriverGetTFRing);                   // sceAgcDriverGetTFRing
	LIB_FUNC("r28hEh6cNH0", AgcDriverGetHsOffchipParam);           // sceAgcDriverGetHsOffchipParam
	LIB_FUNC("ftf-xlfBQpo", AgcDriverSysIsGameClosed);             // sceAgcDriverSysIsGameClosed
	LIB_FUNC("kuE1uTiWfuk", AgcDriverSysGetClientNumber);          // sceAgcDriverSysGetClientNumber
	LIB_FUNC("x3K61sY5m8Q", AgcDriverSysEnableSubmitDone45Exception); // sceAgcDriverSysEnableSubmitDone45Exception
	LIB_FUNC("1BUTwixUG5Y", AgcDriverDebugHardwareStatus);         // sceAgcDriverDebugHardwareStatus
	LIB_FUNC("EcaEt4nv8No", AgcDriverSysGetGfxAppGpuResetStatus);  // sceAgcDriverSysGetGfxAppGpuResetStatus
}

} // namespace AgcCompDriver

namespace AgcCompVsh {

LIB_VERSION("AgcVsh", 1, "AgcVsh", 1, 1);

// PxEFhy0d5v8 sceAgcDcbClearState: log-only success, no guest writes.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcDcbClearState(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t PxEFhy0d5v8 sceAgcDcbClearState a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

// Aozdh0bCcO0: unknown AgcVsh func -- the name is unresolved (no nids.csv row
// for this NID). The single observed call site tests the return for nonzero
// (je fail), so return 1. Null-safe, no guest writes, never halts.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcVshUnknownAozdh0bCcO0(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Aozdh0bCcO0 AgcVsh-unknown (name unresolved, no nids.csv row) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 1\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 1;
}

LIB_DEFINE(InitAgcCompVsh_1) {
	LIB_FUNC("PxEFhy0d5v8", AgcDcbClearState);          // sceAgcDcbClearState
	LIB_FUNC("Aozdh0bCcO0", AgcVshUnknownAozdh0bCcO0);  // unknown AgcVsh func (name unresolved)
}

} // namespace AgcCompVsh

LIB_DEFINE(InitAgcComp_1) {
	AgcCompDriver::InitAgcCompDriver_1(s);
	AgcCompVsh::InitAgcCompVsh_1(s);
}

} // namespace AgcComp

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp ----
//
// namespace AgcComp {
// LIB_DEFINE(InitAgcComp_1);
// } // namespace AgcComp
//
// ... and inside Libs::InitAll:
//   AgcComp::InitAgcComp_1(s);
