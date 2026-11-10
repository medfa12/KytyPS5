#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

// ps5-vsh: SceSysCore.elf (system-core daemon) audio/status peripheral close-out.
// Raw request keys verified verbatim (NID[Lib][LibVer]):
//   SxQprgjttKE/FZXcY33Bxjc/NUWqWguYcNQ/U0ivfdKFZbA/YeBSNVAELe4/rcgv2ciDrtc/
//   tQpOPpYwv7o [AudioIn_v1][AudioIn_v1.1] (7, sceAudioIn*)
//   +YKXDq904dc/1OgP3KmjtKo/71jwRArZoEE/ElSFxgvFp0I/OrwJMVPsokQ/WAQKYsTN6Hs/
//   dhUy8EiYeJs/qILoZjAOv5c/tG7CO3ypKf4 [AudioOut2_v1][AudioOut2_v1.1] (9,
//   sceAudioOut2Mbus*)
//   XEGAcOsiVfQ/ZmPBwBD2tIY/dm0L3LVgQ+M/PMYiKWBa-84/m1Wi0Yp38v0/xVRs9kzSP7I
//     [AvSetting_v1][AvSetting_v1.1] (6; PMYiKWBa-84/m1Wi0Yp38v0/xVRs9kzSP7I
//     UNKNOWN, no nids.csv row)
//   NVITuK83Z7o/v9XTZHXoJyQ [Camera_v1][Camera_v1.1] (2, sceCamera*)
//   OF8D1S5kgMw [Dipsw_v1][Dipsw_v1.1] (1, sceKernelUnsetDipsw)
//   Ac6aYjr1pY8 [Vcodec_v1][Vcodec_v1.1] (1, UNKNOWN)
//   fFLgmNUpChg [Rtc_v1][Rtc_v1.1] (1, sceRtcSetConf)
//   -0FdPC78x7U/AAk6w60xP3A [SysUtil_v1][SysUtil_v1.1] (2, sceSysUtilSend*)
//   OxyZ9gt9hto [RegMgr_v1][RegMgr_v1.1] (1, sceRegMgrBackupPullDataGkp)
// Absence check: every NID above grepped over src/libs -> all 30 clean, 0 skips.
// Semantics: safe tracers, no hardware present. Presence queries fail soft,
// inits/setup/setters succeed-empty (return 0, caller ignores output).
// PRINT_NAME + LOGF NID+name+args, uint64_t x4 args, null-safe, no guest writes
// at all, never halt. C++ NIDs starting with '+' or containing '+'/'-'
// stay verbatim inside LIB_FUNC("...") string literals; C++ function names use
// the sce names, and unknown tracers use LogStub_Unk<sanitized NID> with
// sanitizing (+ -> P, - -> M): +YKXDq904dc stays sce-named (no C++ NID
// needed), dm0L3LVgQ+M stays sce-named, PMYiKWBa-84 -> PMYiKWBaM84,
// -0FdPC78x7U stays sce-named, Ac6aYjr1pY8/m1Wi0Yp38v0/xVRs9kzSP7I need no
// sanitizing (no '+'/'-').

namespace Libs {

namespace SysAudioAudioIn {

LIB_VERSION("AudioIn", 1, "AudioIn", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInInit(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t SxQprgjttKE sceAudioInInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no audio hardware; init succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetDeviceMuteStatus(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t FZXcY33Bxjc sceAudioInSetDeviceMuteStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing to mute; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetPortStatuses(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t NUWqWguYcNQ sceAudioInSetPortStatuses a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no ports present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetSparkParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t U0ivfdKFZbA sceAudioInSetSparkParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no DSP present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetConnections(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
	PRINT_NAME();
	LOGF("\t YeBSNVAELe4 sceAudioInSetConnections a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no routes present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetDevConnection(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	LOGF("\t rcgv2ciDrtc sceAudioInSetDevConnection a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no device present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioInSetPortConnections(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	LOGF("\t tQpOPpYwv7o sceAudioInSetPortConnections a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no ports present; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioAudioIn_1) {
	LIB_FUNC("SxQprgjttKE", Impl::SceAudioInInit);               // sceAudioInInit
	LIB_FUNC("FZXcY33Bxjc", Impl::SceAudioInSetDeviceMuteStatus); // sceAudioInSetDeviceMuteStatus
	LIB_FUNC("NUWqWguYcNQ", Impl::SceAudioInSetPortStatuses);    // sceAudioInSetPortStatuses
	LIB_FUNC("U0ivfdKFZbA", Impl::SceAudioInSetSparkParam);      // sceAudioInSetSparkParam
	LIB_FUNC("YeBSNVAELe4", Impl::SceAudioInSetConnections);     // sceAudioInSetConnections
	LIB_FUNC("rcgv2ciDrtc", Impl::SceAudioInSetDevConnection);   // sceAudioInSetDevConnection
	LIB_FUNC("tQpOPpYwv7o", Impl::SceAudioInSetPortConnections); // sceAudioInSetPortConnections
}

} // namespace SysAudioAudioIn

namespace SysAudioAudioOut2 {

LIB_VERSION("AudioOut2", 1, "AudioOut2", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusInit(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t +YKXDq904dc sceAudioOut2MbusInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no audio hardware; init succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetConnections(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 1OgP3KmjtKo sceAudioOut2MbusSetConnections a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no routes present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetMorpheusParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 71jwRArZoEE sceAudioOut2MbusSetMorpheusParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no DSP present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetCaesarParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ElSFxgvFp0I sceAudioOut2MbusSetCaesarParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no DSP present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetDeviceConnections(uint64_t a0, uint64_t a1,
                                                              uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t OrwJMVPsokQ sceAudioOut2MbusSetDeviceConnections a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no devices present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetSparkParam(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t WAQKYsTN6Hs sceAudioOut2MbusSetSparkParam a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no DSP present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetHeadphoneOutMode(uint64_t a0, uint64_t a1,
                                                             uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t dhUy8EiYeJs sceAudioOut2MbusSetHeadphoneOutMode a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no headphone hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusSetPortStatuses(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t qILoZjAOv5c sceAudioOut2MbusSetPortStatuses a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no ports present; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOut2MbusUserLoginFor3DAudio(uint64_t a0, uint64_t a1,
                                                             uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t tG7CO3ypKf4 sceAudioOut2MbusUserLoginFor3DAudio a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no 3D-audio session; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioAudioOut2_1) {
	LIB_FUNC("+YKXDq904dc", Impl::SceAudioOut2MbusInit);                 // sceAudioOut2MbusInit
	LIB_FUNC("1OgP3KmjtKo", Impl::SceAudioOut2MbusSetConnections);       // sceAudioOut2MbusSetConnections
	LIB_FUNC("71jwRArZoEE", Impl::SceAudioOut2MbusSetMorpheusParam);     // sceAudioOut2MbusSetMorpheusParam
	LIB_FUNC("ElSFxgvFp0I", Impl::SceAudioOut2MbusSetCaesarParam);       // sceAudioOut2MbusSetCaesarParam
	LIB_FUNC("OrwJMVPsokQ", Impl::SceAudioOut2MbusSetDeviceConnections); // sceAudioOut2MbusSetDeviceConnections
	LIB_FUNC("WAQKYsTN6Hs", Impl::SceAudioOut2MbusSetSparkParam);        // sceAudioOut2MbusSetSparkParam
	LIB_FUNC("dhUy8EiYeJs", Impl::SceAudioOut2MbusSetHeadphoneOutMode);  // sceAudioOut2MbusSetHeadphoneOutMode
	LIB_FUNC("qILoZjAOv5c", Impl::SceAudioOut2MbusSetPortStatuses);      // sceAudioOut2MbusSetPortStatuses
	LIB_FUNC("tG7CO3ypKf4", Impl::SceAudioOut2MbusUserLoginFor3DAudio);  // sceAudioOut2MbusUserLoginFor3DAudio
}

} // namespace SysAudioAudioOut2

namespace SysAudioAvSetting {

LIB_VERSION("AvSetting", 1, "AvSetting", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAvSettingDriverUpdateStatus(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t XEGAcOsiVfQ sceAvSettingDriverUpdateStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no display hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAvSettingGetCurrentOutputMode_(uint64_t a0, uint64_t a1, uint64_t a2,
                                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ZmPBwBD2tIY sceAvSettingGetCurrentOutputMode_ a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no display present; fail soft, no guest write
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAvSettingDriverChangeConnectionStatus(uint64_t a0, uint64_t a1,
                                                                  uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t dm0L3LVgQ+M sceAvSettingDriverChangeConnectionStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no display hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkPMYiKWBaM84(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t PMYiKWBa-84 AvSettingUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // name unresolved (no nids.csv row); tracer succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_Unkm1Wi0Yp38v0(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t m1Wi0Yp38v0 AvSettingUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // name unresolved; tracer succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkxVRs9kzSP7I(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t xVRs9kzSP7I AvSettingUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // name unresolved; tracer succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioAvSetting_1) {
	LIB_FUNC("XEGAcOsiVfQ", Impl::SceAvSettingDriverUpdateStatus);         // sceAvSettingDriverUpdateStatus
	LIB_FUNC("ZmPBwBD2tIY", Impl::SceAvSettingGetCurrentOutputMode_);     // sceAvSettingGetCurrentOutputMode_
	LIB_FUNC("dm0L3LVgQ+M", Impl::SceAvSettingDriverChangeConnectionStatus); // sceAvSettingDriverChangeConnectionStatus
	LIB_FUNC("PMYiKWBa-84", Impl::LogStub_UnkPMYiKWBaM84); // name unresolved (no nids.csv row)
	LIB_FUNC("m1Wi0Yp38v0", Impl::LogStub_Unkm1Wi0Yp38v0); // name unresolved (no nids.csv row)
	LIB_FUNC("xVRs9kzSP7I", Impl::LogStub_UnkxVRs9kzSP7I); // name unresolved (no nids.csv row)
}

} // namespace SysAudioAvSetting

namespace SysAudioCamera {

LIB_VERSION("Camera", 1, "Camera", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraSetProcessFocusByHandle(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t NVITuK83Z7o sceCameraSetProcessFocusByHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no camera hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCameraWaitForLukeReady(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	LOGF("\t v9XTZHXoJyQ sceCameraWaitForLukeReady a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no camera hardware; fail soft, no wait
}

} // namespace Impl

LIB_DEFINE(InitSysAudioCamera_1) {
	LIB_FUNC("NVITuK83Z7o", Impl::SceCameraSetProcessFocusByHandle); // sceCameraSetProcessFocusByHandle
	LIB_FUNC("v9XTZHXoJyQ", Impl::SceCameraWaitForLukeReady);         // sceCameraWaitForLukeReady
}

} // namespace SysAudioCamera

namespace SysAudioDipsw {

LIB_VERSION("Dipsw", 1, "Dipsw", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelUnsetDipsw(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	LOGF("\t OF8D1S5kgMw sceKernelUnsetDipsw a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // retail box, flag already off; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioDipsw_1) {
	LIB_FUNC("OF8D1S5kgMw", Impl::SceKernelUnsetDipsw); // sceKernelUnsetDipsw
}

} // namespace SysAudioDipsw

namespace SysAudioVcodec {

LIB_VERSION("Vcodec", 1, "Vcodec", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkAc6aYjr1pY8(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Ac6aYjr1pY8 VcodecUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // name unresolved; tracer succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioVcodec_1) {
	LIB_FUNC("Ac6aYjr1pY8", Impl::LogStub_UnkAc6aYjr1pY8); // name unresolved (no nids.csv row)
}

} // namespace SysAudioVcodec

namespace SysAudioRtc {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRtcSetConf(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t fFLgmNUpChg sceRtcSetConf a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no RTC hardware to program
}

} // namespace Impl

LIB_DEFINE(InitSysAudioRtc_1) {
	LIB_FUNC("fFLgmNUpChg", Impl::SceRtcSetConf); // sceRtcSetConf
}

} // namespace SysAudioRtc

namespace SysAudioSysUtil {

LIB_VERSION("SysUtil", 1, "SysUtil", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSysUtilSendTrcCheckNotificationRequest(uint64_t a0, uint64_t a1,
                                                                  uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t -0FdPC78x7U sceSysUtilSendTrcCheckNotificationRequest a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // notification queued-empty; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSysUtilSendNotificationRequest(uint64_t a0, uint64_t a1, uint64_t a2,
                                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t AAk6w60xP3A sceSysUtilSendNotificationRequest a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // notification queued-empty; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioSysUtil_1) {
	LIB_FUNC("-0FdPC78x7U", Impl::SceSysUtilSendTrcCheckNotificationRequest); // sceSysUtilSendTrcCheckNotificationRequest
	LIB_FUNC("AAk6w60xP3A", Impl::SceSysUtilSendNotificationRequest);         // sceSysUtilSendNotificationRequest
}

} // namespace SysAudioSysUtil

namespace SysAudioRegMgr {

LIB_VERSION("RegMgr", 1, "RegMgr", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRegMgrBackupPullDataGkp(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	LOGF("\t OxyZ9gt9hto sceRegMgrBackupPullDataGkp a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no backup store; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysAudioRegMgr_1) {
	LIB_FUNC("OxyZ9gt9hto", Impl::SceRegMgrBackupPullDataGkp); // sceRegMgrBackupPullDataGkp
}

} // namespace SysAudioRegMgr

namespace SysAudio {

LIB_DEFINE(InitSysAudio_1) {
SysAudioAudioIn::InitSysAudioAudioIn_1(s);
SysAudioAudioOut2::InitSysAudioAudioOut2_1(s);
SysAudioAvSetting::InitSysAudioAvSetting_1(s);
SysAudioCamera::InitSysAudioCamera_1(s);
SysAudioDipsw::InitSysAudioDipsw_1(s);
SysAudioVcodec::InitSysAudioVcodec_1(s);
SysAudioRtc::InitSysAudioRtc_1(s);
SysAudioSysUtil::InitSysAudioSysUtil_1(s);
SysAudioRegMgr::InitSysAudioRegMgr_1(s);
}

} // namespace SysAudio

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp,
inside namespace Libs next to the other bare LIB_DEFINE lines):

LIB_DEFINE(InitSysAudio_1);

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   InitSysAudio_1(s);
*/
