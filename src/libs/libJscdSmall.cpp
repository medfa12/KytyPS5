#include "common/abi.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "libs/npMemoryArena.h"
#include "libs/npMemoryHeap.h"
#include "loader/symbolDatabase.h"
#include "kernel/fileSystem.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <cinttypes>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ps5-vsh: NPXS40000 JSCd (eboot.bin) boot3 unresolved-PLT close-out
// (_ProbeLogs/jscd_boot3.log `unresolved PLT import patched to stub` lines).
// Raw request keys verified verbatim from Relocate lines (NID[Lib][LibVer]):
//   PhlPIVLxvSo/cJqESX9JU98/gdBO0jjfQsg/omZKNhDRL9k/wCM+d+kYC-w/zgDNUrp0A+U
//     [MediaAppSharedStorageMc_v1][MediaAppSharedStorage_v1.0] (6, no nids.csv row)
//   0A0yznVbShk/1ORzOVJWfNg/6fELpICjpnk/7ejdChiWT8U/qrewaZ35Jns
//     [PubSubClientShared_v1][PubSubClientShared_v1.1] (5, ScePubSubClient*)
//   BDDZwF5kuTc/Wp8zHTocS5E/H6axEciG5Gw/OhsV6P0FD5A/W5SFqOnOVt0
//     [UsbStorageForShellUI_v1][UsbStorage_v1.0] (5; Init/Term named in nids.csv,
//     rest UNKNOWN)
//   odAzwN-S9G4/xY607JqjoPk/yB12TC6fbPo/yuQrMTiR28k/zj5wKSAva+Y
//     [NpManager_v1][NpManager_v1.1] (5, names in nids.csv)
//   2hlBNB96saE/81DkaQt6J30/KJdPcOGmK58/hOnIlcGrO6g [NpWebApi2_v1][NpWebApi2_v1.1]
//   dfXSH2Tsjkw/kZizwrFvWZY/kwW5qddf+Lo/uj86YxCYid0 [NpCommon_v1][NpCommon_v1.1]
//   BHUdZh6st9c/KHxNaWydXss/pv4ZWTRweyI [Remoteplay_v1][Remoteplay_v1.1]
//   MTEBJdi3YNQ/Syu+Sbd7R4s/Y00mcdiTiOA [UrlConfigResolver_v1][UrlConfigResolver_v1.1]
//   Dbbkj6YHWdo/Uxqkdta7wEg [Coredump_v1][libkernel_v1.1]
//   LN3Zcb72Q0c/Ot1DE3gif84 [Rtc_v1][Rtc_v1.1]
//   R4jTtQ-5puM/bfDcj2PJL6g [SystemLogger2_v1][SystemLogger2_v1.1]
//   RQPgtaOJdTk/aScB-WDl+L0 [NpGameIntent_v1][NpGameIntent_v1.1]
//   jSWBvOPfSOk/eJd+9F0v9hE [Notification_v1][Notification_v1.1]
//   xoRfQxAhHjk/n2fSRzuTEY4 [LibreSSl3_v1][LibreSSl3_v1.1]
//   7W6jR0LyiSg [Psm_v1][Psm_v1.1] (_ZN3sce3pss5orbis9framework8PsmEvent4PostE...)
//   D6ovvSecedE [Pad_v1][Pad_v1.1] (scePadRemoteControlInsertData)
//   JXlI9EZVjf4 [NetCtl_v1][NetCtl_v1.1] (sceNetCtlGetState2IpcInt)
// Absence check: every NID above grepped over src/libs -> all 50 clean, 0 skips.
// Already-registered (SKIPPED, present as LIB_FUNC in src/libs, not repeated here):
//   Sysmodule 39iV5E1HoCk/vXZhrtJxkGc (libSysmodule.cpp), SystemStateMgr
//   6gtqLPVTdJY/9WRHk1wQT08 (libJscdNet.cpp) + ze0ky5Q1yE8/wlxvESTUplk (libC.cpp),
//   Telemetry 83leZmsmmT4 (libJscdNet.cpp), RegMgr dKeshzt29G4 (libC.cpp),
//   Ssl 0K1yQ6Lv-Yc/hdpVEUDFW3s + Http2 3JCe3lCbQ8A/YiBUtz-pGkc + Net
//   K7RlrTkI-mw/dgJBaeJnGpo (libNet.cpp), SystemLogger2Delivery (log shows the 6
//   NIDs already resolved to Delivery* impls).
// Out of scope here (handled separately): Jsc 32ajtT0hAck x1, Jsi-0.70 x4,
// libhermes-0.11 WIDyseMUXEo x1, LibcInternal x5. Plus 1x libReactNative out of scope.
// Semantics: log-and-succeed (PRINT_NAME + LOGF NID+name+args, return 0),
// uint64_t x4 args, null-safe, no guest deref, except the audited USB-storage
// lifecycle/enumeration implementation below.

namespace Libs {

namespace JscdSmall {

namespace MediaAppShared {

LIB_VERSION("MediaAppSharedStorageMc", 1, "MediaAppSharedStorage", 1, 0);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkPhlPIVLxvSo(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t PhlPIVLxvSo MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkcJqESX9JU98(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t cJqESX9JU98 MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkgdBO0jjfQsg(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t gdBO0jjfQsg MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkomZKNhDRL9k(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t omZKNhDRL9k MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkwCM_d_kYC_w(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t wCM+d+kYC-w MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkzgDNUrp0A_U(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t zgDNUrp0A+U MediaAppSharedStorageMcUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallMediaAppShared_1) {
	LIB_FUNC("PhlPIVLxvSo", Impl::LogStub_UnkPhlPIVLxvSo); // name unresolved (no nids.csv row)
	LIB_FUNC("cJqESX9JU98", Impl::LogStub_UnkcJqESX9JU98); // name unresolved (no nids.csv row)
	LIB_FUNC("gdBO0jjfQsg", Impl::LogStub_UnkgdBO0jjfQsg); // name unresolved (no nids.csv row)
	LIB_FUNC("omZKNhDRL9k", Impl::LogStub_UnkomZKNhDRL9k); // name unresolved (no nids.csv row)
	LIB_FUNC("wCM+d+kYC-w", Impl::LogStub_UnkwCM_d_kYC_w); // name unresolved (no nids.csv row)
	LIB_FUNC("zgDNUrp0A+U", Impl::LogStub_UnkzgDNUrp0A_U); // name unresolved (no nids.csv row)
}

} // namespace MediaAppShared

namespace PubSubClientShared {

LIB_VERSION("PubSubClientShared", 1, "PubSubClientShared", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePubSubClientUnsubscribe(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 0A0yznVbShk ScePubSubClientUnsubscribe a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePubSubClientSubscribe(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 1ORzOVJWfNg ScePubSubClientSubscribe a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePubSubClientCreate(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 6fELpICjpnk ScePubSubClientCreate a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePubSubClientPublish(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 7ejdChiWT8U ScePubSubClientPublish a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePubSubClientDestroy(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t qrewaZ35Jns ScePubSubClientDestroy a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallPubSubClientShared_1) {
	LIB_FUNC("0A0yznVbShk", Impl::ScePubSubClientUnsubscribe); // ScePubSubClientUnsubscribe
	LIB_FUNC("1ORzOVJWfNg", Impl::ScePubSubClientSubscribe); // ScePubSubClientSubscribe
	LIB_FUNC("6fELpICjpnk", Impl::ScePubSubClientCreate); // ScePubSubClientCreate
	LIB_FUNC("7ejdChiWT8U", Impl::ScePubSubClientPublish); // ScePubSubClientPublish
	LIB_FUNC("qrewaZ35Jns", Impl::ScePubSubClientDestroy); // ScePubSubClientDestroy
}

} // namespace PubSubClientShared

namespace UsbStorageForShellUI {

LIB_VERSION("UsbStorageForShellUI", 1, "UsbStorage", 1, 0);

namespace Impl {

constexpr int32_t kUsbInvalid = static_cast<int32_t>(0x80f40002u);
constexpr int32_t kUsbStatus = static_cast<int32_t>(0x80f40009u);
constexpr size_t kUsbDeviceCapacity = 8;
// Session-owned empty storage-device set: no guest USB-storage attachment
// backend exists yet. Host USB devices and filesystem mounts are not devices
// registered with this service. Readiness is independent of device presence.
static std::atomic<bool> g_usb_storage_initialized {false};

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUsbStorageInit(void* thread_attr) {
	PRINT_NAME();
	// 13.60 +0x9810: ScePthreadAttr* is optional; repeated init succeeds.
	// This empty backend needs no callback thread and does not consume the attr.
	LOGF("\t sceUsbStorageInit thread_attr=%p\n", thread_attr);
	g_usb_storage_initialized.store(true);
	return 0;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUsbStorageTerm() {
	PRINT_NAME();
	// 13.60 +0x9da0 -> +0x1440: no client is STATUS, not success.
	return g_usb_storage_initialized.exchange(false) ? 0 : kUsbStatus;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUsbStorageGetDeviceList(
    uint32_t* device_ids, int32_t* count) {
	PRINT_NAME();
	// 13.60 +0xb910 -> +0x25c0; 4.03 +0xb350 -> +0x2da0:
	// two output pointers, fixed eight-ID capacity, status checked first.
	// The signed count is output-only (Gallery deliberately initializes it -1).
	if (!g_usb_storage_initialized.load()) { return kUsbStatus; }
	if (!NpMemoryArena::RangeReadWrite(device_ids, kUsbDeviceCapacity * sizeof(*device_ids)) ||
	    !NpMemoryArena::RangeReadWrite(count, sizeof(*count))) { return kUsbInvalid; }
	// ShellCore 13.60 +0x41e69b/+0x41e6ea: an empty guest list writes count=0
	// and returns success without touching unused ID slots. Use fault-contained
	// copyout after the full preflight (including split-page count outputs).
	const int32_t empty_count = 0;
	return LibKernel::FileSystem::CopyPathResultToGuest(count, &empty_count, sizeof(empty_count)) ? 0 : kUsbInvalid;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkH6axEciG5Gw(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t H6axEciG5Gw UsbStorageForShellUIUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkOhsV6P0FD5A(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t OhsV6P0FD5A UsbStorageForShellUIUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkW5SFqOnOVt0(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t W5SFqOnOVt0 UsbStorageForShellUIUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallUsbStorageForShellUI_1) {
	LIB_FUNC("BDDZwF5kuTc", Impl::SceUsbStorageInit); // sceUsbStorageInit
	LIB_FUNC("Wp8zHTocS5E", Impl::SceUsbStorageTerm); // sceUsbStorageTerm
	LIB_FUNC("mryrNITeYvI", Impl::SceUsbStorageGetDeviceList);
	LIB_FUNC("H6axEciG5Gw", Impl::LogStub_UnkH6axEciG5Gw); // name unresolved (no nids.csv row)
	LIB_FUNC("OhsV6P0FD5A", Impl::LogStub_UnkOhsV6P0FD5A); // name unresolved (no nids.csv row)
	LIB_FUNC("W5SFqOnOVt0", Impl::LogStub_UnkW5SFqOnOVt0); // name unresolved (no nids.csv row)
}

} // namespace UsbStorageForShellUI

namespace NpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpManagerIntGetBcAccessToken(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t odAzwN-S9G4 sceNpManagerIntGetBcAccessToken a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpManagerIntGetTitleToken(uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t xY607JqjoPk sceNpManagerIntGetTitleToken a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpAsmClientExtCreateRequest(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t yB12TC6fbPo sceNpAsmClientExtCreateRequest a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpAsmClientExtDeleteRequest(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t yuQrMTiR28k sceNpAsmClientExtDeleteRequest a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpAsmClientExtGetPlayingTitleNpTitleToken(uint64_t a1, uint64_t a2,
                                                                      uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t zj5wKSAva+Y sceNpAsmClientExtGetPlayingTitleNpTitleToken a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNpManager_1) {
	LIB_FUNC("odAzwN-S9G4", Impl::SceNpManagerIntGetBcAccessToken); // sceNpManagerIntGetBcAccessToken
	LIB_FUNC("xY607JqjoPk", Impl::SceNpManagerIntGetTitleToken); // sceNpManagerIntGetTitleToken
	LIB_FUNC("yB12TC6fbPo", Impl::SceNpAsmClientExtCreateRequest); // sceNpAsmClientExtCreateRequest
	LIB_FUNC("yuQrMTiR28k", Impl::SceNpAsmClientExtDeleteRequest); // sceNpAsmClientExtDeleteRequest
	LIB_FUNC("zj5wKSAva+Y", Impl::SceNpAsmClientExtGetPlayingTitleNpTitleToken); // sceNpAsmClientExtGetPlayingTitleNpTitleToken
}

} // namespace NpManager

namespace NpWebApi2 {

LIB_VERSION("NpWebApi2", 1, "NpWebApi2", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpWebApi2IntPushEventCreateCtxIndFilter(uint64_t a1, uint64_t a2,
                                                                    uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 2hlBNB96saE sceNpWebApi2IntPushEventCreateCtxIndFilter a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpWebApi2IntPushEventCreateDestPs4CtxIndFilter(uint64_t a1, uint64_t a2,
                                                                            uint64_t a3,
                                                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 81DkaQt6J30 sceNpWebApi2IntPushEventCreateDestPs4CtxIndFilter a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpWebApi2PushEventDeleteFilter(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t KJdPcOGmK58 sceNpWebApi2PushEventDeleteFilter a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpWebApi2PushEventUnregisterCallback(uint64_t a1, uint64_t a2,
                                                                 uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t hOnIlcGrO6g sceNpWebApi2PushEventUnregisterCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNpWebApi2_1) {
	LIB_FUNC("2hlBNB96saE", Impl::SceNpWebApi2IntPushEventCreateCtxIndFilter); // sceNpWebApi2IntPushEventCreateCtxIndFilter
	LIB_FUNC("81DkaQt6J30", Impl::SceNpWebApi2IntPushEventCreateDestPs4CtxIndFilter); // sceNpWebApi2IntPushEventCreateDestPs4CtxIndFilter
	LIB_FUNC("KJdPcOGmK58", Impl::SceNpWebApi2PushEventDeleteFilter); // sceNpWebApi2PushEventDeleteFilter
	LIB_FUNC("hOnIlcGrO6g", Impl::SceNpWebApi2PushEventUnregisterCallback); // sceNpWebApi2PushEventUnregisterCallback
}

} // namespace NpWebApi2

namespace NpCommon {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpHeapGetStat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpUserGetUserIdList(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	// ps5-vsh: offline bring-up (voldemort libNpCommon rule): no NP-signed-in
	// users -> count 0, success. The RifManager then skips per-user
	// entitlement enumeration. Scoped to direct-title boot (chain never calls
	// it; keeps legacy unresolved-stub behavior there).
	if (!Libs::IsDirectTitleBoot()) {
		return 0;
	}
	int* count = reinterpret_cast<int*>(a3);
	if (count != nullptr) {
		*count = 0;
	}
	(void)a1;
	(void)a2;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpStrToUInt32(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t uj86YxCYid0 sceNpStrToUInt32 a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNpCommon_1) {
	LIB_FUNC("gMlY6eewr-c", NpMemoryHeap::MapMemory);
	LIB_FUNC("B+yGIX1+BTI", NpMemoryHeap::CreateHeap);
	LIB_FUNC("wVdn78HKc30", NpMemoryHeap::DestroyHeap);
	LIB_FUNC("vjwlDmsGtME", NpMemoryHeap::UnmapMemory);
	LIB_FUNC("dfXSH2Tsjkw", NpMemoryHeap::Destroy);
	LIB_FUNC("kZizwrFvWZY", NpMemoryHeap::Init);
	LIB_FUNC("FaMNvjMA6to", NpMemoryHeap::MakeAllocator);
	LIB_FUNC("xHAiSVEEjSI", NpMemoryHeap::MakeAllocatorEx);
	LIB_FUNC("kwW5qddf+Lo", NpMemoryHeap::Malloc);
	LIB_FUNC("uj86YxCYid0", Impl::SceNpStrToUInt32); // sceNpStrToUInt32
	LIB_FUNC("p1vvpKGRXe4", NpMemoryHeap::Free);
	LIB_FUNC("wsfyvM+VbUk", NpMemoryHeap::Realloc);
	LIB_FUNC("GjdHoPpS+W0", Impl::SceNpHeapGetStat); // sceNpHeapGetStat
	LIB_FUNC("Gh74vNl06sg", Impl::SceNpUserGetUserIdList); // sceNpUserGetUserIdList
}

} // namespace NpCommon

namespace Remoteplay {

LIB_VERSION("Remoteplay", 1, "Remoteplay", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRemoteplayUnregistCallback(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t BHUdZh6st9c sceRemoteplayUnregistCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRemoteplayNotifyCanWakeup(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	LOGF("\t KHxNaWydXss sceRemoteplayNotifyCanWakeup a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRemoteplayRegistCallback(uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t pv4ZWTRweyI sceRemoteplayRegistCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallRemoteplay_1) {
	LIB_FUNC("BHUdZh6st9c", Impl::SceRemoteplayUnregistCallback); // sceRemoteplayUnregistCallback
	LIB_FUNC("KHxNaWydXss", Impl::SceRemoteplayNotifyCanWakeup); // sceRemoteplayNotifyCanWakeup
	LIB_FUNC("pv4ZWTRweyI", Impl::SceRemoteplayRegistCallback); // sceRemoteplayRegistCallback
}

} // namespace Remoteplay

namespace UrlConfigResolver {

LIB_VERSION("UrlConfigResolver", 1, "UrlConfigResolver", 1, 1);

namespace Impl {

static constexpr int kUrlCfgBufferShort = static_cast<int>(0x80BA8090);

static int UrlCfgWriteCStr(const char* s, void* buf, uint32_t bufSize, uint32_t* outSize) {
	const uint32_t need = static_cast<uint32_t>(std::strlen(s) + 1); // include NUL terminator
	if (outSize != nullptr) {
		*outSize = need;
	}
	if (buf == nullptr || bufSize < need) {
		return kUrlCfgBufferShort;
	}
	std::memcpy(buf, s, need);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverGetAppConfig(const char* app, int flags, void* buf, uint32_t bufSize, uint32_t* outSize, uint64_t a6) {
	PRINT_NAME();
	(void)a6;
	if (buf != nullptr && bufSize > 0) { std::memset(buf, 0, bufSize); }
	if (outSize != nullptr) { *outSize = 0; }
	if (app == nullptr) { return -2135261056; }
	LOGF("\t sceUrlConfigResolverGetAppConfig app = %s\n", app);

	std::filesystem::path real_path = Libs::LibKernel::FileSystem::GetRealFilename("/system_data/priv/cache/urlconfig/urlconfig_cache.json");
	if (!std::filesystem::exists(real_path)) {
		real_path = "_SystemData/priv/cache/urlconfig/urlconfig_cache.json";
	}
	std::ifstream cache(real_path);
	if (!cache) {
		LOGF("\t urlconfig_cache.json not found at %s!\n", real_path.string().c_str());
		return -2135261054;
	}
	const auto root = nlohmann::json::parse(cache, nullptr, false);
	if (!root.is_object()) {
		return -2135261054;
	}
	const auto payload = root.find(root.contains("urlconfig") ? "urlconfig" : "response");
	if (payload == root.end() || !payload->is_object()) { return -2135261054; }
	const auto& config = *payload;
	if (!config.contains("scheme") || !config["scheme"].is_object()) { return -2135261053; }

	std::string selector = "default";
	const auto selected = [&](const nlohmann::json& value) -> const nlohmann::json* {
		auto it = value.find(selector);
		if (it != value.end() && !it->is_null()) { return &*it; }
		it = value.find("default");
		return it != value.end() && !it->is_null() ? &*it : nullptr;
	};
	const auto* schemes = selected(config["scheme"]);
	if (schemes == nullptr || !schemes->is_object()) { return -2135261051; }
	if (!config.contains("app") || !config["app"].is_object()) { return -2135261046; }

	const std::string uri(app);
	const auto colon = uri.find(':');
	const nlohmann::json* route = nullptr;
	const auto lookup = [&](const std::string& key) -> const nlohmann::json* {
		auto it = schemes->find(key);
		return it != schemes->end() ? &*it : nullptr;
	};
	if (colon != std::string::npos && colon + 1 < uri.size()) {
		std::string prefix = uri.substr(0, uri.find('?', colon + 1));
		while (!prefix.empty()) {
			route = lookup(prefix);
			if (route != nullptr) { break; }
			const auto slash = prefix.rfind('/');
			if (slash == std::string::npos) { break; }
			prefix.resize(slash);
		}
	}
	if (route == nullptr) { route = lookup(uri.substr(0, colon)); }
	if (route == nullptr && (flags & 0xff) != 0) { route = lookup("default"); }
	if (route == nullptr) {
		LOGF("\t Route not found for uri: %s\n", app);
		return -2135261049;
	}
	if (!route->is_string() || !route->get_ref<const std::string&>().starts_with("app:")) {
		return -2135261047;
	}
	const auto& apps = config["app"];
	auto entry = apps.find(route->get_ref<const std::string&>().substr(4));
	if (entry == apps.end()) { entry = apps.find("default"); }
	if (entry == apps.end() || !entry->is_object()) { return -2135261044; }
	const auto* result = selected(*entry);
	if (result == nullptr || !result->is_object()) { return -2135261042; }

	const auto json_str = result->dump();
	LOGF("\t UrlConfig result for '%s': %s\n", app, json_str.c_str());
	return UrlCfgWriteCStr(json_str.c_str(), buf, bufSize, outSize);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverGetSysVer(void* buf, uint32_t bufSize, uint32_t* outSize, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	return UrlCfgWriteCStr("09.000.000", buf, bufSize, outSize);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverGetDeviceId(void* buf, uint32_t bufSize, uint32_t* outSize, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	return UrlCfgWriteCStr("0000000000000000000000000000000000000000", buf, bufSize, outSize);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverUpdate(uint64_t a1) {
	PRINT_NAME();
	(void)a1;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverIsLoaded() {
	PRINT_NAME();
	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverGetCacheVersion(uint32_t* outVersion) {
	PRINT_NAME();
	if (outVersion == nullptr) {
		return static_cast<int>(0x80BA8080);
	}
	*outVersion = 0x06400000;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceUrlConfigResolverGetRelease(void* buf, uint32_t bufSize, uint32_t* outSize, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	return UrlCfgWriteCStr("release", buf, bufSize, outSize);
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallUrlConfigResolver_1) {
	LIB_FUNC("MTEBJdi3YNQ", Impl::SceUrlConfigResolverGetAppConfig); // sceUrlConfigResolverGetAppConfig
	LIB_FUNC("Syu+Sbd7R4s", Impl::SceUrlConfigResolverGetSysVer); // sceUrlConfigResolverGetSysVer
	LIB_FUNC("Y00mcdiTiOA", Impl::SceUrlConfigResolverGetDeviceId); // sceUrlConfigResolverGetDeviceId
	LIB_FUNC("DrRUD+rZV28", Impl::SceUrlConfigResolverUpdate); // sceUrlConfigResolverUpdate
	LIB_FUNC("C4AAV-A4pnA", Impl::SceUrlConfigResolverIsLoaded); // sceUrlConfigResolverIsLoaded
	LIB_FUNC("kBo7ItYgSPo", Impl::SceUrlConfigResolverGetCacheVersion); // sceUrlConfigResolverGetCacheVersion
	LIB_FUNC("a3kUt2XrhOs", Impl::SceUrlConfigResolverGetRelease); // sceUrlConfigResolverGetRelease
}

} // namespace UrlConfigResolver

namespace Coredump {

LIB_VERSION("Coredump", 1, "libkernel", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCoredumpWriteUserData(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t Dbbkj6YHWdo sceCoredumpWriteUserData a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCoredumpSetUserDataType(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t Uxqkdta7wEg sceCoredumpSetUserDataType a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallCoredump_1) {
	LIB_FUNC("Dbbkj6YHWdo", Impl::SceCoredumpWriteUserData); // sceCoredumpWriteUserData
	LIB_FUNC("Uxqkdta7wEg", Impl::SceCoredumpSetUserDataType); // sceCoredumpSetUserDataType
}

} // namespace Coredump

namespace Rtc {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRtcGetCurrentAdNetworkTick(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t LN3Zcb72Q0c sceRtcGetCurrentAdNetworkTick a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRtcGetCurrentDebugNetworkTick(uint64_t a1, uint64_t a2, uint64_t a3,
                                                          uint64_t a4) {
	PRINT_NAME();
	LOGF("\t Ot1DE3gif84 sceRtcGetCurrentDebugNetworkTick a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallRtc_1) {
	LIB_FUNC("LN3Zcb72Q0c", Impl::SceRtcGetCurrentAdNetworkTick); // sceRtcGetCurrentAdNetworkTick
	LIB_FUNC("Ot1DE3gif84", Impl::SceRtcGetCurrentDebugNetworkTick); // sceRtcGetCurrentDebugNetworkTick
}

} // namespace Rtc

namespace SystemLogger2 {

LIB_VERSION("SystemLogger2", 1, "SystemLogger2", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSystemLogger2SetRnpsEventAsync(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t R4jTtQ-5puM sceSystemLogger2SetRnpsEventAsync a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSystemLogger2Terminate(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t bfDcj2PJL6g sceSystemLogger2Terminate a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallSystemLogger2_1) {
	LIB_FUNC("R4jTtQ-5puM", Impl::SceSystemLogger2SetRnpsEventAsync); // sceSystemLogger2SetRnpsEventAsync
	LIB_FUNC("bfDcj2PJL6g", Impl::SceSystemLogger2Terminate); // sceSystemLogger2Terminate
}

} // namespace SystemLogger2

namespace NpGameIntent {

LIB_VERSION("NpGameIntent", 1, "NpGameIntent", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpGameIntentRemoveLaunchCallback(uint64_t a1, uint64_t a2,
                                                             uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t RQPgtaOJdTk sceNpGameIntentRemoveLaunchCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNpGameIntentRequest(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t aScB-WDl+L0 sceNpGameIntentRequest a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNpGameIntent_1) {
	LIB_FUNC("RQPgtaOJdTk", Impl::SceNpGameIntentRemoveLaunchCallback); // sceNpGameIntentRemoveLaunchCallback
	LIB_FUNC("aScB-WDl+L0", Impl::SceNpGameIntentRequest); // sceNpGameIntentRequest
}

} // namespace NpGameIntent

namespace Notification {

LIB_VERSION("Notification", 1, "Notification", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNotificationSend(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t jSWBvOPfSOk sceNotificationSend a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNotificationSendById(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LOGF("\t eJd+9F0v9hE sceNotificationSendById a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNotification_1) {
	LIB_FUNC("jSWBvOPfSOk", Impl::SceNotificationSend); // sceNotificationSend
	LIB_FUNC("eJd+9F0v9hE", Impl::SceNotificationSendById); // sceNotificationSendById
}

} // namespace Notification

namespace LibreSSl3 {

LIB_VERSION("LibreSSl3", 1, "LibreSSl3", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceLibreSslInitEx2(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t xoRfQxAhHjk sceLibreSslInitEx2 a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceLibreSslTerm(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t n2fSRzuTEY4 sceLibreSslTerm a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallLibreSSl3_1) {
	LIB_FUNC("xoRfQxAhHjk", Impl::SceLibreSslInitEx2); // sceLibreSslInitEx2
	LIB_FUNC("n2fSRzuTEY4", Impl::SceLibreSslTerm); // sceLibreSslTerm
}

} // namespace LibreSSl3

namespace Psm {

LIB_VERSION("Psm", 1, "Psm", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PsmEventPost(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t 7W6jR0LyiSg PsmEventPost a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallPsm_1) {
	// _ZN3sce3pss5orbis9framework8PsmEvent4PostERKNS2_12PsmEventDataE
	LIB_FUNC("7W6jR0LyiSg", Impl::PsmEventPost);
}

} // namespace Psm

namespace Pad {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePadRemoteControlInsertData(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LOGF("\t D6ovvSecedE scePadRemoteControlInsertData a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallPad_1) {
	LIB_FUNC("D6ovvSecedE", Impl::ScePadRemoteControlInsertData); // scePadRemoteControlInsertData
}

} // namespace Pad

namespace NetCtl {

LIB_VERSION("NetCtl", 1, "NetCtl", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetCtlGetState2IpcInt(int* state) {
	// 13.60 NetCtl +3430 -> IPC 0x20009 has one four-byte output.
	// ShellUI +8fb0 exposes it as lx::lxNetCtl::GetState; Sce.Vsh.Lx.NetState
	// has the same 0/1/2/3 disconnected/connecting/IP-obtaining/obtained values.
	return Network::NetCtl::NetCtlGetState(state);
}

} // namespace Impl

LIB_DEFINE(InitJscdSmallNetCtl_1) {
	LIB_FUNC("JXlI9EZVjf4", Impl::SceNetCtlGetState2IpcInt); // sceNetCtlGetState2IpcInt
}

} // namespace NetCtl

LIB_DEFINE(InitJscdSmall_1) {
	JscdSmall::MediaAppShared::InitJscdSmallMediaAppShared_1(s);
	JscdSmall::PubSubClientShared::InitJscdSmallPubSubClientShared_1(s);
	JscdSmall::UsbStorageForShellUI::InitJscdSmallUsbStorageForShellUI_1(s);
	JscdSmall::NpManager::InitJscdSmallNpManager_1(s);
	JscdSmall::NpWebApi2::InitJscdSmallNpWebApi2_1(s);
	JscdSmall::NpCommon::InitJscdSmallNpCommon_1(s);
	JscdSmall::Remoteplay::InitJscdSmallRemoteplay_1(s);
	JscdSmall::UrlConfigResolver::InitJscdSmallUrlConfigResolver_1(s);
	JscdSmall::Coredump::InitJscdSmallCoredump_1(s);
	JscdSmall::Rtc::InitJscdSmallRtc_1(s);
	JscdSmall::SystemLogger2::InitJscdSmallSystemLogger2_1(s);
	JscdSmall::NpGameIntent::InitJscdSmallNpGameIntent_1(s);
	JscdSmall::Notification::InitJscdSmallNotification_1(s);
	JscdSmall::LibreSSl3::InitJscdSmallLibreSSl3_1(s);
	JscdSmall::Psm::InitJscdSmallPsm_1(s);
	JscdSmall::Pad::InitJscdSmallPad_1(s);
	JscdSmall::NetCtl::InitJscdSmallNetCtl_1(s);
}

} // namespace JscdSmall

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp):

namespace JscdSmall {
LIB_DEFINE(InitJscdSmall_1);
} // namespace JscdSmall

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   JscdSmall::InitJscdSmall_1(s);
*/
