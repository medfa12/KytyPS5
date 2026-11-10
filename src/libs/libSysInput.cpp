#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

// ps5-vsh: SceSysCore.elf (system-core daemon) input/HID/VR close-out.
// Raw request keys verified verbatim (NID[lib]):
//   Y1WkAv+vINM/ONyH3TlLDx0/AVtf1JiYnQI/aVvm-afVQlo/nYX3q3hser8/V3x+8S9k1vc/
//   u2GrRj85Riw/EfvzrKzujQE/akmC1nXbrLE/VBlu5blw5+Y/6NdxbfLzf0w/JRBFFug1N00/
//   Pc92TjYbXpc/37PCATQMR+0/IVjtw0AuSBE/JcPje9m3yyc/vY-muZt5ojc/6wfdbeqaQco/
//   8GZle24IAgw/Z82YcJH0-Z0/oKq4gXV9d0o/p2PN5EpVqH0/UQt115LnKms
//     [HidControl_v1][HidControl_v1.1] (23, sceHidControl*)
//   tul3-GzejQc/YWT1GrcWuQs/wZtLdyq2dKg
//     [BluetoothHid_v1][BluetoothHid_v1.1] (3, sceBluetoothHid*)
//   LkBkse9Pit0/hyATMTuQSoQ/v243mvYg0Y0/UTqrWB+1+SU/U9kPT4g1mFE/0z2qLqedQH0
//     [Hmd_v1][Hmd_v1.1] (6, sceHmdInternal*)
//   3pDi+jA6KJk/M+-H47IrheM/G3PNDE2VjfY/eBGK-o6E4RQ/-tfbonl+WQY
//     [Hmd2_v1][Hmd2_v1.1] (5, sceHmd2Internal*)
//   CfwUlQtCFi4/80XdmVYsNPA/gAHvg6JPIic
//     [Pad_v1][Pad_v1.1] (3, scePadMbusInit/scePadStartRecording/
//     scePadStopRecording)
// Absence check: every NID above grepped over src/libs -> all 40 clean, 0 skips.
// Semantics: safe tracers, no hardware present. Device queries report absent
// (-1), inits/connect/disconnect/set/bind/power/report calls succeed-empty
// (return 0, caller ignores output) so boot proceeds. PRINT_NAME + LOGF
// NID+name+args, uint64_t x4 args, null-safe, no guest writes at all, never
// halt. C++ NIDs starting with a digit or containing '+'/'-' stay verbatim
// inside LIB_FUNC("...") string literals; C++ function names use the sce name
// suffixed with the sanitized NID (+ -> P, - -> M), which keeps every
// identifier valid (all start with Sce, digits only appear after the '_').

namespace Libs {

namespace SysInputHidControl {

LIB_VERSION("HidControl", 1, "HidControl", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlInit_Y1WkAvPvINM(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Y1WkAv+vINM sceHidControlInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HID hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlConnectHandle_ONyH3TlLDx0(uint64_t a0, uint64_t a1,
                                                                uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ONyH3TlLDx0 sceHidControlConnectHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HID hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlDisconnectHandle_AVtf1JiYnQI(uint64_t a0, uint64_t a1,
                                                                   uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t AVtf1JiYnQI sceHidControlDisconnectHandle a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing connected; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlConnectPort_aVvmMafVQlo(uint64_t a0, uint64_t a1,
                                                              uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t aVvm-afVQlo sceHidControlConnectPort a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HID hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlDisconnectPort_nYX3q3hser8(uint64_t a0, uint64_t a1,
                                                                 uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t nYX3q3hser8 sceHidControlDisconnectPort a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing connected; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlDisconnectDevice_V3xP8S9k1vc(uint64_t a0, uint64_t a1,
                                                                   uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t V3x+8S9k1vc sceHidControlDisconnectDevice a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing connected; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetDeviceInfo_u2GrRj85Riw(uint64_t a0, uint64_t a1,
                                                                uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t u2GrRj85Riw sceHidControlGetDeviceInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetDeviceName_EfvzrKzujQE(uint64_t a0, uint64_t a1,
                                                                uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t EfvzrKzujQE sceHidControlGetDeviceName a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetBatteryState_akmC1nXbrLE(uint64_t a0, uint64_t a1,
                                                                  uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t akmC1nXbrLE sceHidControlGetBatteryState a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetPswBatteryState_VBlu5blw5PY(uint64_t a0, uint64_t a1,
                                                                     uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t VBlu5blw5+Y sceHidControlGetPswBatteryState a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetPswState_6NdxbfLzf0w(uint64_t a0, uint64_t a1,
                                                              uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 6NdxbfLzf0w sceHidControlGetPswState a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetPswDeviceIdFromIdxAdd_JRBFFug1N00(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t JRBFFug1N00 sceHidControlGetPswDeviceIdFromIdxAdd a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlGetSparkState_Pc92TjYbXpc(uint64_t a0, uint64_t a1,
                                                                 uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Pc92TjYbXpc sceHidControlGetSparkState a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no HID hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlIsForceUpdateNeeded_37PCATQMRP0(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 37PCATQMR+0 sceHidControlIsForceUpdateNeeded a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no update pending
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetHandleState_IVjtw0AuSBE(uint64_t a0, uint64_t a1,
                                                                 uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t IVjtw0AuSBE sceHidControlSetHandleState a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HID hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetApplicationFocus_JcPje9m3yyc(uint64_t a0, uint64_t a1,
                                                                       uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t JcPje9m3yyc sceHidControlSetApplicationFocus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no input hardware to program
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetProcessFocus_vYMmuZt5ojc(uint64_t a0, uint64_t a1,
                                                                   uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t vY-muZt5ojc sceHidControlSetProcessFocus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no input hardware to program
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetUserColor_6wfdbeqaQco(uint64_t a0, uint64_t a1,
                                                                uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 6wfdbeqaQco sceHidControlSetUserColor a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no input hardware to program
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetRemoteMergeProcess_8GZle24IAgw(uint64_t a0, uint64_t a1,
                                                                         uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 8GZle24IAgw sceHidControlSetRemoteMergeProcess a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no input hardware to program
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetCollaboInputSettingOverrideProcess_Z82YcJH0MZ0(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t Z82YcJH0-Z0 sceHidControlSetCollaboInputSettingOverrideProcess a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no input hardware to program
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetVibrationTriggerEffectWeakForce_oKq4gXV9d0o(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t oKq4gXV9d0o sceHidControlSetVibrationTriggerEffectWeakForce a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no rumble hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlSetForceAdvancedVibrationMode_p2PN5EpVqH0(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t p2PN5EpVqH0 sceHidControlSetForceAdvancedVibrationMode a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no rumble hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHidControlEnableMicBeamForming_UQt115LnKms(uint64_t a0, uint64_t a1,
                                                                        uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t UQt115LnKms sceHidControlEnableMicBeamForming a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no mic hardware; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysInputHidControl_1) {
	LIB_FUNC("Y1WkAv+vINM", Impl::SceHidControlInit_Y1WkAvPvINM);   // sceHidControlInit
	LIB_FUNC("ONyH3TlLDx0", Impl::SceHidControlConnectHandle_ONyH3TlLDx0); // sceHidControlConnectHandle
	LIB_FUNC("AVtf1JiYnQI", Impl::SceHidControlDisconnectHandle_AVtf1JiYnQI); // sceHidControlDisconnectHandle
	LIB_FUNC("aVvm-afVQlo", Impl::SceHidControlConnectPort_aVvmMafVQlo); // sceHidControlConnectPort
	LIB_FUNC("nYX3q3hser8", Impl::SceHidControlDisconnectPort_nYX3q3hser8); // sceHidControlDisconnectPort
	LIB_FUNC("V3x+8S9k1vc", Impl::SceHidControlDisconnectDevice_V3xP8S9k1vc); // sceHidControlDisconnectDevice
	LIB_FUNC("u2GrRj85Riw", Impl::SceHidControlGetDeviceInfo_u2GrRj85Riw); // sceHidControlGetDeviceInfo
	LIB_FUNC("EfvzrKzujQE", Impl::SceHidControlGetDeviceName_EfvzrKzujQE); // sceHidControlGetDeviceName
	LIB_FUNC("akmC1nXbrLE", Impl::SceHidControlGetBatteryState_akmC1nXbrLE); // sceHidControlGetBatteryState
	LIB_FUNC("VBlu5blw5+Y", Impl::SceHidControlGetPswBatteryState_VBlu5blw5PY); // sceHidControlGetPswBatteryState
	LIB_FUNC("6NdxbfLzf0w", Impl::SceHidControlGetPswState_6NdxbfLzf0w); // sceHidControlGetPswState
	LIB_FUNC("JRBFFug1N00", Impl::SceHidControlGetPswDeviceIdFromIdxAdd_JRBFFug1N00); // sceHidControlGetPswDeviceIdFromIdxAdd
	LIB_FUNC("Pc92TjYbXpc", Impl::SceHidControlGetSparkState_Pc92TjYbXpc); // sceHidControlGetSparkState
	LIB_FUNC("37PCATQMR+0", Impl::SceHidControlIsForceUpdateNeeded_37PCATQMRP0); // sceHidControlIsForceUpdateNeeded
	LIB_FUNC("IVjtw0AuSBE", Impl::SceHidControlSetHandleState_IVjtw0AuSBE); // sceHidControlSetHandleState
	LIB_FUNC("JcPje9m3yyc", Impl::SceHidControlSetApplicationFocus_JcPje9m3yyc); // sceHidControlSetApplicationFocus
	LIB_FUNC("vY-muZt5ojc", Impl::SceHidControlSetProcessFocus_vYMmuZt5ojc); // sceHidControlSetProcessFocus
	LIB_FUNC("6wfdbeqaQco", Impl::SceHidControlSetUserColor_6wfdbeqaQco); // sceHidControlSetUserColor
	LIB_FUNC("8GZle24IAgw", Impl::SceHidControlSetRemoteMergeProcess_8GZle24IAgw); // sceHidControlSetRemoteMergeProcess
	LIB_FUNC("Z82YcJH0-Z0", Impl::SceHidControlSetCollaboInputSettingOverrideProcess_Z82YcJH0MZ0); // sceHidControlSetCollaboInputSettingOverrideProcess
	LIB_FUNC("oKq4gXV9d0o", Impl::SceHidControlSetVibrationTriggerEffectWeakForce_oKq4gXV9d0o); // sceHidControlSetVibrationTriggerEffectWeakForce
	LIB_FUNC("p2PN5EpVqH0", Impl::SceHidControlSetForceAdvancedVibrationMode_p2PN5EpVqH0); // sceHidControlSetForceAdvancedVibrationMode
	LIB_FUNC("UQt115LnKms", Impl::SceHidControlEnableMicBeamForming_UQt115LnKms); // sceHidControlEnableMicBeamForming
}

} // namespace SysInputHidControl

namespace SysInputBluetoothHid {

LIB_VERSION("BluetoothHid", 1, "BluetoothHid", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceBluetoothHidInit_tul3MGzejQc(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t tul3-GzejQc sceBluetoothHidInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no BT hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceBluetoothHidGetDeviceInfo_YWT1GrcWuQs(uint64_t a0, uint64_t a1,
                                                                  uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t YWT1GrcWuQs sceBluetoothHidGetDeviceInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return -1; // no BT hardware present
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceBluetoothHidDisconnectDevice_wZtLdyq2dKg(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t wZtLdyq2dKg sceBluetoothHidDisconnectDevice a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing connected; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysInputBluetoothHid_1) {
	LIB_FUNC("tul3-GzejQc", Impl::SceBluetoothHidInit_tul3MGzejQc); // sceBluetoothHidInit
	LIB_FUNC("YWT1GrcWuQs", Impl::SceBluetoothHidGetDeviceInfo_YWT1GrcWuQs); // sceBluetoothHidGetDeviceInfo
	LIB_FUNC("wZtLdyq2dKg", Impl::SceBluetoothHidDisconnectDevice_wZtLdyq2dKg); // sceBluetoothHidDisconnectDevice
}

} // namespace SysInputBluetoothHid

namespace SysInputHmd {

LIB_VERSION("Hmd", 1, "Hmd", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalSetPortConnection_LkBkse9Pit0(uint64_t a0, uint64_t a1,
                                                                     uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t LkBkse9Pit0 sceHmdInternalSetPortConnection a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalSetDeviceConnection_hyATMTuQSoQ(uint64_t a0, uint64_t a1,
                                                                        uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t hyATMTuQSoQ sceHmdInternalSetDeviceConnection a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalSetPortStatus_v243mvYg0Y0(uint64_t a0, uint64_t a1,
                                                                 uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t v243mvYg0Y0 sceHmdInternalSetPortStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalBindDeviceWithUserId_UTqrWBP1PSU(uint64_t a0, uint64_t a1,
                                                                         uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t UTqrWB+1+SU sceHmdInternalBindDeviceWithUserId a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalSetHmuPowerControl_U9kPT4g1mFE(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t U9kPT4g1mFE sceHmdInternalSetHmuPowerControl a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmdInternalSetCrashReportCommand_0z2qLqedQH0(uint64_t a0, uint64_t a1,
                                                                          uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 0z2qLqedQH0 sceHmdInternalSetCrashReportCommand a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, log args; no HMD hardware to report
}

} // namespace Impl

LIB_DEFINE(InitSysInputHmd_1) {
	LIB_FUNC("LkBkse9Pit0", Impl::SceHmdInternalSetPortConnection_LkBkse9Pit0); // sceHmdInternalSetPortConnection
	LIB_FUNC("hyATMTuQSoQ", Impl::SceHmdInternalSetDeviceConnection_hyATMTuQSoQ); // sceHmdInternalSetDeviceConnection
	LIB_FUNC("v243mvYg0Y0", Impl::SceHmdInternalSetPortStatus_v243mvYg0Y0); // sceHmdInternalSetPortStatus
	LIB_FUNC("UTqrWB+1+SU", Impl::SceHmdInternalBindDeviceWithUserId_UTqrWBP1PSU); // sceHmdInternalBindDeviceWithUserId
	LIB_FUNC("U9kPT4g1mFE", Impl::SceHmdInternalSetHmuPowerControl_U9kPT4g1mFE); // sceHmdInternalSetHmuPowerControl
	LIB_FUNC("0z2qLqedQH0", Impl::SceHmdInternalSetCrashReportCommand_0z2qLqedQH0); // sceHmdInternalSetCrashReportCommand
}

} // namespace SysInputHmd

namespace SysInputHmd2 {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalSetPortConnection_3pDiPjA6KJk(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 3pDi+jA6KJk sceHmd2InternalSetPortConnection a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalSetDeviceConnection_MPMH47IrheM(uint64_t a0, uint64_t a1,
                                                                         uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t M+-H47IrheM sceHmd2InternalSetDeviceConnection a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalSetPowerOff_G3PNDE2VjfY(uint64_t a0, uint64_t a1,
                                                                 uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t G3PNDE2VjfY sceHmd2InternalSetPowerOff a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // already off; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalBindDeviceWithUserId_eBGKMo6E4RQ(uint64_t a0, uint64_t a1,
                                                                          uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t eBGK-o6E4RQ sceHmd2InternalBindDeviceWithUserId a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceHmd2InternalSetPortStatus_MtfbonlPWQY(uint64_t a0, uint64_t a1,
                                                                   uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t -tfbonl+WQY sceHmd2InternalSetPortStatus a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no HMD hardware; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysInputHmd2_1) {
	LIB_FUNC("3pDi+jA6KJk", Impl::SceHmd2InternalSetPortConnection_3pDiPjA6KJk); // sceHmd2InternalSetPortConnection
	LIB_FUNC("M+-H47IrheM", Impl::SceHmd2InternalSetDeviceConnection_MPMH47IrheM); // sceHmd2InternalSetDeviceConnection
	LIB_FUNC("G3PNDE2VjfY", Impl::SceHmd2InternalSetPowerOff_G3PNDE2VjfY); // sceHmd2InternalSetPowerOff
	LIB_FUNC("eBGK-o6E4RQ", Impl::SceHmd2InternalBindDeviceWithUserId_eBGKMo6E4RQ); // sceHmd2InternalBindDeviceWithUserId
	LIB_FUNC("-tfbonl+WQY", Impl::SceHmd2InternalSetPortStatus_MtfbonlPWQY); // sceHmd2InternalSetPortStatus
}

} // namespace SysInputHmd2

namespace SysInputPad {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePadMbusInit_CfwUlQtCFi4(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	LOGF("\t CfwUlQtCFi4 scePadMbusInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no pad hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePadStartRecording_80XdmVYsNPA(uint64_t a0, uint64_t a1, uint64_t a2,
                                                           uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 80XdmVYsNPA scePadStartRecording a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no pad hardware; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePadStopRecording_gAHvg6JPIic(uint64_t a0, uint64_t a1, uint64_t a2,
                                                          uint64_t a3) {
	PRINT_NAME();
	LOGF("\t gAHvg6JPIic scePadStopRecording a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing recording; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysInputPad_1) {
	LIB_FUNC("CfwUlQtCFi4", Impl::ScePadMbusInit_CfwUlQtCFi4);       // scePadMbusInit
	LIB_FUNC("80XdmVYsNPA", Impl::ScePadStartRecording_80XdmVYsNPA); // scePadStartRecording
	LIB_FUNC("gAHvg6JPIic", Impl::ScePadStopRecording_gAHvg6JPIic);  // scePadStopRecording
}

} // namespace SysInputPad

namespace SysInput {

LIB_DEFINE(InitSysInput_1) {
SysInputHidControl::InitSysInputHidControl_1(s);
SysInputBluetoothHid::InitSysInputBluetoothHid_1(s);
SysInputHmd::InitSysInputHmd_1(s);
SysInputHmd2::InitSysInputHmd2_1(s);
SysInputPad::InitSysInputPad_1(s);
}

} // namespace SysInput

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp,
inside namespace Libs next to the other bare LIB_DEFINE lines):

LIB_DEFINE(InitSysInput_1);

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   InitSysInput_1(s);
*/
