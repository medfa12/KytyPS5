#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

// ps5-vsh: AgcCompositor.elf (compositor daemon) peripheral close-out.
// Raw request keys verified verbatim (NID[Lib][LibVer]):
//   eTywOSWsEiI/cLxF1QtHch0/+X1Kgnn3bzg/ztqH5qNTpTk/68S3GKWhY6o/mxgMmR+1Kr0
//     [Camera_v1][Camera_v1.1] (6, sceCamera*)
//   E9scVxt0DNg [Hmd_v1][Hmd_v1.1] (1, sceHmdInternalCreateSharedMemory)
//   PWAUwOTHbbo/OXQ1EDf02XU/IAxfMTvFm0M/hfrnRTryKQQ/ng6eI8uA0h4/uKhwYoMsIsA/
//   EtslTnqO1xU/w2vEs2y5ZmE/CTM7QPLJF9Q [Hmd2_v1][Hmd2_v1.1] (9, sceHmd2Internal*,
//   CTM7QPLJF9Q UNKNOWN)
//   Fog5t22u4K0 [AvSetting_v1][AvSetting_v1.1] (1, sceAvControlSetSystemVideoMode)
//   JSNgID8Bupk [Dipsw_v1][Dipsw_v1.1] (1, sceKernelCheckDipsw)
//   ycczGaMXtnI/smBYHl9GB-g [SisrSys_v1][SisrSys_v1.1] (2, sceSisrSys*)
// Absence check: every NID above grepped over src/libs -> all 20 clean, 0 skips.
// Note: AvSetting EIZdWTT7Zdw/Im9QXnr0QK4/A143jTEYkNw/s2ODJNcQVkI (libC.cpp,
// libShellHits.cpp) and Dipsw PKg59uLpHNI/UtO0OHMCgmI (libShellHits.cpp) already
// exist and are NOT repeated here. Existing Dipsw impls return 0 (retail: flag
// off/absent), so sceKernelCheckDipsw below also returns 0 (flag off).
// Semantics: safe tracers, no hardware present. Presence queries return
// absent/0, init/open calls fail soft with an error (-1), close/stop/setup
// calls succeed-empty (return 0, caller ignores output). PRINT_NAME + LOGF
// NID+name+args, uint64_t x4 args, null-safe, no guest writes at all, never
// halt. C++ NIDs starting with '+' or containing '-' stay verbatim inside
// LIB_FUNC("...") string literals; C++ function names use the sce names
// (no sanitizing needed since no identifier is derived from a raw NID).

namespace Libs {

namespace AgcPeriCamera {

LIB_VERSION("Camera", 1, "Camera", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraOpenByModuleId(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t eTywOSWsEiI sceCameraOpenByModuleId a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no camera hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraStartByHandle(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cLxF1QtHch0 sceCameraStartByHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no camera hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraStopByHandle(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t +X1Kgnn3bzg sceCameraStopByHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing running; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraCloseByHandle(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ztqH5qNTpTk sceCameraCloseByHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing open; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraIsConnectedForSystem(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 68S3GKWhY6o sceCameraIsConnectedForSystem a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // not connected
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraGetFrameData(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t mxgMmR+1Kr0 sceCameraGetFrameData a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no camera hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraUnk_RHYJ7GKOSMg(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
	PRINT_NAME();
	LOGF("\t RHYJ7GKOSMg SceCameraUnk a0 = 0x%016" PRIx64 "\n", a0);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no camera hardware present
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriCamera_1) {
	LIB_FUNC("eTywOSWsEiI", Impl::SceCameraOpenByModuleId);      // sceCameraOpenByModuleId
	LIB_FUNC("cLxF1QtHch0", Impl::SceCameraStartByHandle);       // sceCameraStartByHandle
	LIB_FUNC("+X1Kgnn3bzg", Impl::SceCameraStopByHandle);        // sceCameraStopByHandle
	LIB_FUNC("ztqH5qNTpTk", Impl::SceCameraCloseByHandle);       // sceCameraCloseByHandle
	LIB_FUNC("68S3GKWhY6o", Impl::SceCameraIsConnectedForSystem); // sceCameraIsConnectedForSystem
	LIB_FUNC("mxgMmR+1Kr0", Impl::SceCameraGetFrameData);        // sceCameraGetFrameData
	LIB_FUNC("RHYJ7GKOSMg", Impl::SceCameraUnk_RHYJ7GKOSMg);
}

} // namespace AgcPeriCamera

namespace AgcPeriHmd {

LIB_VERSION("Hmd", 1, "Hmd", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalCreateSharedMemory(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t E9scVxt0DNg sceHmdInternalCreateSharedMemory a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalGetDeviceStatus_1PNiQR_7L6k(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 1PNiQR-7L6k SceHmdInternalGetDeviceStatus a0 = 0x%016" PRIx64 "\n", a0);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // 0 = no HMD device connected
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriHmd_1) {
	LIB_FUNC("E9scVxt0DNg", Impl::SceHmdInternalCreateSharedMemory); // sceHmdInternalCreateSharedMemory
	LIB_FUNC("1PNiQR-7L6k", Impl::SceHmdInternalGetDeviceStatus_1PNiQR_7L6k); // sceHmdInternalGetDeviceStatus
}

} // namespace AgcPeriHmd

namespace AgcPeriHmd2 {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalCreateSharedMemory(uint64_t a0, uint64_t a1, uint64_t a2,
                                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t PWAUwOTHbbo sceHmd2InternalCreateSharedMemory a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageOpen(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t OXQ1EDf02XU sceHmd2InternalImageOpen a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageClose(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t IAxfMTvFm0M sceHmd2InternalImageClose a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing open; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageSetConfig(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t hfrnRTryKQQ sceHmd2InternalImageSetConfig a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageGetMemoryLayout(uint64_t a0, uint64_t a1, uint64_t a2,
                                                             uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ng6eI8uA0h4 sceHmd2InternalImageGetMemoryLayout a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageStop(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t uKhwYoMsIsA sceHmd2InternalImageStop a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing running; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalImageStart2(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	LOGF("\t EtslTnqO1xU sceHmd2InternalImageStart2 a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalGetOpticalParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t w2vEs2y5ZmE sceHmd2InternalGetOpticalParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkCTM7QPLJF9Q(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t CTM7QPLJF9Q Hmd2Unk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalUnk_3F5inHhKzXg(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 3F5inHhKzXg SceHmd2InternalUnk a0 = 0x%016" PRIx64 "\n", a0);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD2 hardware
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalUnk_EPeH4xuQ2XQ(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t EPeH4xuQ2XQ SceHmd2InternalUnk a0 = 0x%016" PRIx64 "\n", a0);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HMD2 hardware
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriHmd2_1) {
	LIB_FUNC("PWAUwOTHbbo", Impl::SceHmd2InternalCreateSharedMemory); // sceHmd2InternalCreateSharedMemory
	LIB_FUNC("OXQ1EDf02XU", Impl::SceHmd2InternalImageOpen);          // sceHmd2InternalImageOpen
	LIB_FUNC("IAxfMTvFm0M", Impl::SceHmd2InternalImageClose);         // sceHmd2InternalImageClose
	LIB_FUNC("hfrnRTryKQQ", Impl::SceHmd2InternalImageSetConfig);     // sceHmd2InternalImageSetConfig
	LIB_FUNC("ng6eI8uA0h4", Impl::SceHmd2InternalImageGetMemoryLayout); // sceHmd2InternalImageGetMemoryLayout
	LIB_FUNC("uKhwYoMsIsA", Impl::SceHmd2InternalImageStop);          // sceHmd2InternalImageStop
	LIB_FUNC("EtslTnqO1xU", Impl::SceHmd2InternalImageStart2);        // sceHmd2InternalImageStart2
	LIB_FUNC("w2vEs2y5ZmE", Impl::SceHmd2InternalGetOpticalParam);    // sceHmd2InternalGetOpticalParam
	LIB_FUNC("sXaPEPY-fLA", Impl::SceHmd2InternalGetOpticalParam);    // sceHmd2InternalGetOpticalParam (FW 13.60)
	LIB_FUNC("3F5inHhKzXg", Impl::SceHmd2InternalUnk_3F5inHhKzXg);
	LIB_FUNC("EPeH4xuQ2XQ", Impl::SceHmd2InternalUnk_EPeH4xuQ2XQ);
	LIB_FUNC("CTM7QPLJF9Q", Impl::LogStub_UnkCTM7QPLJF9Q); // name unresolved (no nids.csv row)
}

} // namespace AgcPeriHmd2

namespace AgcPeriAv {

LIB_VERSION("AvSetting", 1, "AvSetting", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAvControlSetSystemVideoMode(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Fog5t22u4K0 sceAvControlSetSystemVideoMode a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no display hardware to program
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriAv_1) {
	LIB_FUNC("Fog5t22u4K0", Impl::SceAvControlSetSystemVideoMode); // sceAvControlSetSystemVideoMode
}

} // namespace AgcPeriAv

namespace AgcPeriDipsw {

LIB_VERSION("Dipsw", 1, "Dipsw", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelCheckDipsw(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	// Matches existing Dipsw impls (libShellHits.cpp IsAssistMode/
	// IsDevelopmentMode): retail box, flag off.
	LOGF("\t JSNgID8Bupk sceKernelCheckDipsw a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // flag off
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriDipsw_1) {
	LIB_FUNC("JSNgID8Bupk", Impl::SceKernelCheckDipsw); // sceKernelCheckDipsw
}

} // namespace AgcPeriDipsw

namespace AgcPeriSisr {

LIB_VERSION("SisrSys", 1, "SisrSys", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSisrSysSetup(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ycczGaMXtnI sceSisrSysSetup a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing to set up; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSisrSysProcessUmdEvent(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t smBYHl9GB-g sceSisrSysProcessUmdEvent a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no UMD/status hardware; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitAgcPeriSisr_1) {
	LIB_FUNC("ycczGaMXtnI", Impl::SceSisrSysSetup);           // sceSisrSysSetup
	LIB_FUNC("smBYHl9GB-g", Impl::SceSisrSysProcessUmdEvent); // sceSisrSysProcessUmdEvent
}

} // namespace AgcPeriSisr

namespace AgcPeri {

LIB_DEFINE(InitAgcPeri_1) {
AgcPeriCamera::InitAgcPeriCamera_1(s);
AgcPeriHmd::InitAgcPeriHmd_1(s);
AgcPeriHmd2::InitAgcPeriHmd2_1(s);
AgcPeriAv::InitAgcPeriAv_1(s);
AgcPeriDipsw::InitAgcPeriDipsw_1(s);
AgcPeriSisr::InitAgcPeriSisr_1(s);
}

} // namespace AgcPeri

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp,
inside namespace Libs next to the other bare LIB_DEFINE lines):

LIB_DEFINE(InitAgcPeri_1);

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   InitAgcPeri_1(s);
*/
