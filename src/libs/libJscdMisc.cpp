#include "common/abi.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

// ps5-vsh: NPXS40000 JSCd (eboot.bin) boot1 unresolved-PLT coverage.
// Raw request keys verified from _ProbeLogs/jscd_boot1.log Relocate lines
// (NID[Lib][LibVer], exact guest REQUEST keys, used EXACTLY below):
//   JPI9m4ViLQ4[AppDbShellCoreClient_v1][AppDbShellCoreClient_v1.0] (18 total)
//   U8QGe+LlOC4[BgsStorage_v1][BgsStorage_v1.0] (14 total)
//   q8a6R8SJHms[SystemLogger2NativeQueueClient_v1][SystemLogger2NativeQueueClient_v1.1] (10)
//   CTKW9UibxEA[SystemLogger2Delivery_v1][SystemLogger2Delivery_v1.1] (6)
//   bI5AGFMydrA[Json2_v1][Json_v1.1] (8)
//   9KXSwPg4PlM[AppInstUtil_v1][AppInstUtil_v1.1] (8)
//   ymCrwbFD4fk[libkernel_v1][libkernel_v1.1] (5 PLT; task brief said 7, log holds 5)
//   d+JHa2D36oA[RnpsAppMgr_v1][RnpsAppMgr_v1.1] (7)
// NID extraction: py regex over 'unresolved PLT' lines, deduped per family.
// Absence check: every NID above grepped over src/libs AND src/kernel -> all
// 76 clean, 0 skips (no owner file:line; libkernel hits are genuinely new NIDs,
// e.g. Rnps neighbors 6UbbC7GA9MI/nfsELpNQ0fo/Yrht530r9FI differ by 1 char from
// existing KUbb../Hfs../Irht.. under other keys).
// Semantics: log-and-succeed only (PRINT_NAME + LOGF NID+name+args, return OK),
// null-safe, NO guest deref (raw u64 args only). AppDb/BgsStorage unknowns:
// plain success-0, no fake handles. Runs via InitJscdMisc_1 (see snippet at end;
// place AFTER existing calls so exact-key insert_or_assign wins).

namespace Libs {
namespace JscdMisc {

// Generic named stub: log NID + name + raw args, succeed, never touch guest.
#define JSCD_MISC_NAMED_STUB(Func, NidStr, FullName)                                                 \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Func(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,          \
	                              uint64_t a4, uint64_t a5) {                                  \
		PRINT_NAME();                                                                      \
		LOGF("\t jscd-misc: NID=%s %s(a0=0x%016" PRIx64 " a1=0x%016" PRIx64                    \
		     " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64                        \
		     " a5=0x%016" PRIx64 ")\n",                                                      \
		     NidStr, FullName, a0, a1, a2, a3, a4, a5);                                       \
		return OK;                                                                       \
	}

// Unknown NID: log NID + raw args, succeed, never touch guest.
#define JSCD_MISC_UNK_STUB(Func, NidStr)                                                             \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Func(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,          \
	                              uint64_t a4, uint64_t a5) {                                  \
		PRINT_NAME();                                                                      \
		LOGF("\t jscd-misc-unk: NID=%s(a0=0x%016" PRIx64 " a1=0x%016" PRIx64                   \
		     " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64                        \
		     " a5=0x%016" PRIx64 ")\n",                                                      \
		     NidStr, a0, a1, a2, a3, a4, a5);                                                \
		return OK;                                                                       \
	}

namespace AppDb {

LIB_VERSION("AppDbShellCoreClient", 1, "AppDbShellCoreClient", 1, 0);

// ps5-vsh: JSCd audit object 7U22Y3NBs0A (48B per sprx export, name unknown).
// Zeroed blob of exact size.
static uint64_t g_appdb48[6] = {};

JSCD_MISC_UNK_STUB(LogStub_Unk_JPI9m4ViLQ4, "JPI9m4ViLQ4")
JSCD_MISC_UNK_STUB(LogStub_Unk_5g80tF6RtUM, "5g80tF6RtUM")
JSCD_MISC_UNK_STUB(LogStub_Unk_Aw2fJZ_TNcM, "Aw2fJZ+TNcM")
JSCD_MISC_UNK_STUB(LogStub_Unk_ZWqfj9VyL0I, "ZWqfj9VyL0I")
JSCD_MISC_UNK_STUB(LogStub_Unk_1rdpvtPYXUg, "1rdpvtPYXUg")
JSCD_MISC_UNK_STUB(LogStub_Unk_0PevQGrqaXQ, "0PevQGrqaXQ")
JSCD_MISC_UNK_STUB(LogStub_Unk_faNkQhATfVw, "faNkQhATfVw")
JSCD_MISC_UNK_STUB(LogStub_Unk_SlsrwedLC34, "SlsrwedLC34")
JSCD_MISC_UNK_STUB(LogStub_Unk_G6QGW4amFBc, "G6QGW4amFBc")
JSCD_MISC_UNK_STUB(LogStub_Unk_gyhDBiNNvlg, "gyhDBiNNvlg")
JSCD_MISC_UNK_STUB(LogStub_Unk_DdycECIaV2o, "DdycECIaV2o")
JSCD_MISC_UNK_STUB(LogStub_Unk_Hk3ZVT3vbzI, "Hk3ZVT3vbzI")
JSCD_MISC_UNK_STUB(LogStub_Unk_WUIPL_WE6g0, "WUIPL-WE6g0")
JSCD_MISC_UNK_STUB(LogStub_Unk_j5q3FIFFuDk, "j5q3FIFFuDk")
JSCD_MISC_UNK_STUB(LogStub_Unk_5rwApEXmVrc, "5rwApEXmVrc")
JSCD_MISC_UNK_STUB(LogStub_Unk_dr9_2YPkvUw, "dr9-2YPkvUw")
JSCD_MISC_UNK_STUB(LogStub_Unk_Bj0hSEaglms, "Bj0hSEaglms")
JSCD_MISC_UNK_STUB(LogStub_Unk_J6LN2x1jm0k, "J6LN2x1jm0k")

static void InitAppDb(Loader::SymbolDatabase* s) {
	LIB_FUNC("JPI9m4ViLQ4", LogStub_Unk_JPI9m4ViLQ4); // unknown AppDbShellCoreClient stub
	LIB_FUNC("5g80tF6RtUM", LogStub_Unk_5g80tF6RtUM); // unknown AppDbShellCoreClient stub
	LIB_FUNC("Aw2fJZ+TNcM", LogStub_Unk_Aw2fJZ_TNcM); // unknown AppDbShellCoreClient stub
	LIB_FUNC("ZWqfj9VyL0I", LogStub_Unk_ZWqfj9VyL0I); // unknown AppDbShellCoreClient stub
	LIB_FUNC("1rdpvtPYXUg", LogStub_Unk_1rdpvtPYXUg); // unknown AppDbShellCoreClient stub
	LIB_FUNC("0PevQGrqaXQ", LogStub_Unk_0PevQGrqaXQ); // unknown AppDbShellCoreClient stub
	LIB_FUNC("faNkQhATfVw", LogStub_Unk_faNkQhATfVw); // unknown AppDbShellCoreClient stub
	LIB_FUNC("SlsrwedLC34", LogStub_Unk_SlsrwedLC34); // unknown AppDbShellCoreClient stub
	LIB_FUNC("G6QGW4amFBc", LogStub_Unk_G6QGW4amFBc); // unknown AppDbShellCoreClient stub
	LIB_FUNC("gyhDBiNNvlg", LogStub_Unk_gyhDBiNNvlg); // unknown AppDbShellCoreClient stub
	LIB_FUNC("DdycECIaV2o", LogStub_Unk_DdycECIaV2o); // unknown AppDbShellCoreClient stub
	LIB_FUNC("Hk3ZVT3vbzI", LogStub_Unk_Hk3ZVT3vbzI); // unknown AppDbShellCoreClient stub
	LIB_FUNC("WUIPL-WE6g0", LogStub_Unk_WUIPL_WE6g0); // unknown AppDbShellCoreClient stub
	LIB_FUNC("j5q3FIFFuDk", LogStub_Unk_j5q3FIFFuDk); // unknown AppDbShellCoreClient stub
	LIB_FUNC("5rwApEXmVrc", LogStub_Unk_5rwApEXmVrc); // unknown AppDbShellCoreClient stub
	LIB_FUNC("dr9-2YPkvUw", LogStub_Unk_dr9_2YPkvUw); // unknown AppDbShellCoreClient stub
	LIB_FUNC("Bj0hSEaglms", LogStub_Unk_Bj0hSEaglms); // unknown AppDbShellCoreClient stub
	LIB_FUNC("J6LN2x1jm0k", LogStub_Unk_J6LN2x1jm0k); // unknown AppDbShellCoreClient stub
	LIB_OBJECT("7U22Y3NBs0A", &g_appdb48[0]); // 48B data object (audit10)
}

} // namespace AppDb

namespace Bgs {

LIB_VERSION("BgsStorage", 1, "BgsStorage", 1, 0);

JSCD_MISC_UNK_STUB(LogStub_Unk_U8QGe_LlOC4, "U8QGe+LlOC4")
JSCD_MISC_UNK_STUB(LogStub_Unk_JK67MBtUF5A, "JK67MBtUF5A")
JSCD_MISC_UNK_STUB(LogStub_Unk_UIBQpJITFwo, "UIBQpJITFwo")
JSCD_MISC_UNK_STUB(LogStub_Unk_kS74ICPn0MQ, "kS74ICPn0MQ")
JSCD_MISC_UNK_STUB(LogStub_Unk_BauWWA9gkBc, "BauWWA9gkBc")
JSCD_MISC_UNK_STUB(LogStub_Unk_rpT7IeQJt8M, "rpT7IeQJt8M")
JSCD_MISC_UNK_STUB(LogStub_Unk_11_Z9cwWDR0, "11+Z9cwWDR0")
JSCD_MISC_UNK_STUB(LogStub_Unk_EbYx033ydKc, "EbYx033ydKc")
JSCD_MISC_UNK_STUB(LogStub_Unk_L8eSXdS8H2A, "L8eSXdS8H2A")
JSCD_MISC_UNK_STUB(LogStub_Unk_UIxiHHQRDH0, "UIxiHHQRDH0")
JSCD_MISC_UNK_STUB(LogStub_Unk_YxExcYiqRY4, "YxExcYiqRY4")
JSCD_MISC_UNK_STUB(LogStub_Unk_Ke8CKO5MNEM, "Ke8CKO5MNEM")
JSCD_MISC_UNK_STUB(LogStub_Unk_qCHuyQMNC3Q, "qCHuyQMNC3Q")
JSCD_MISC_UNK_STUB(LogStub_Unk_PQqTvPT8G2E, "PQqTvPT8G2E")

static void InitBgs(Loader::SymbolDatabase* s) {
	LIB_FUNC("U8QGe+LlOC4", LogStub_Unk_U8QGe_LlOC4); // unknown BgsStorage stub
	LIB_FUNC("JK67MBtUF5A", LogStub_Unk_JK67MBtUF5A); // unknown BgsStorage stub
	LIB_FUNC("UIBQpJITFwo", LogStub_Unk_UIBQpJITFwo); // unknown BgsStorage stub
	LIB_FUNC("kS74ICPn0MQ", LogStub_Unk_kS74ICPn0MQ); // unknown BgsStorage stub
	LIB_FUNC("BauWWA9gkBc", LogStub_Unk_BauWWA9gkBc); // unknown BgsStorage stub
	LIB_FUNC("rpT7IeQJt8M", LogStub_Unk_rpT7IeQJt8M); // unknown BgsStorage stub
	LIB_FUNC("11+Z9cwWDR0", LogStub_Unk_11_Z9cwWDR0); // unknown BgsStorage stub
	LIB_FUNC("EbYx033ydKc", LogStub_Unk_EbYx033ydKc); // unknown BgsStorage stub
	LIB_FUNC("L8eSXdS8H2A", LogStub_Unk_L8eSXdS8H2A); // unknown BgsStorage stub
	LIB_FUNC("UIxiHHQRDH0", LogStub_Unk_UIxiHHQRDH0); // unknown BgsStorage stub
	LIB_FUNC("YxExcYiqRY4", LogStub_Unk_YxExcYiqRY4); // unknown BgsStorage stub
	LIB_FUNC("Ke8CKO5MNEM", LogStub_Unk_Ke8CKO5MNEM); // unknown BgsStorage stub
	LIB_FUNC("qCHuyQMNC3Q", LogStub_Unk_qCHuyQMNC3Q); // unknown BgsStorage stub
	LIB_FUNC("PQqTvPT8G2E", LogStub_Unk_PQqTvPT8G2E); // unknown BgsStorage stub
}

} // namespace Bgs

namespace Sl2Queue {

LIB_VERSION("SystemLogger2NativeQueueClient", 1, "SystemLogger2NativeQueueClient", 1, 1);

JSCD_MISC_NAMED_STUB(NativeQueueGetEvent, "q8a6R8SJHms",
                     "sceSystemLogger2NativeQueueClientGetEvent")
JSCD_MISC_NAMED_STUB(NativeQueueGetEvent2, "YmAOA8iYW8w",
                     "sceSystemLogger2NativeQueueClientGetEvent2")
JSCD_MISC_NAMED_STUB(NativeQueueInitialize, "cUL8DS6BoQU",
                     "sceSystemLogger2NativeQueueClientInitialize")
JSCD_MISC_NAMED_STUB(NativeQueueTerminate, "qjM4x55l-XY",
                     "sceSystemLogger2NativeQueueClientTerminate")
JSCD_MISC_NAMED_STUB(NativeQueueSetDeliveryEnable, "FJyVzN8qkOo",
                     "sceSystemLogger2NativeQueueClientSetDeliveryEnable")
JSCD_MISC_NAMED_STUB(NativeQueueSetNativeQueueConfig, "obQpnx50kEk",
                     "sceSystemLogger2NativeQueueClientSetNativeQueueConfig")
JSCD_MISC_NAMED_STUB(NativeQueueSetBatchDeliveryResult, "4fg-S8skme4",
                     "sceSystemLogger2NativeQueueClientSetBatchDeliveryResult")
JSCD_MISC_NAMED_STUB(NativeQueueCheckData, "X33t3dyMmLg",
                     "sceSystemLogger2NativeQueueClientCheckData")
JSCD_MISC_NAMED_STUB(NativeQueueCheckAndMaskData, "libpnl6UIGg",
                     "sceSystemLogger2NativeQueueClientCheckAndMaskData")
JSCD_MISC_NAMED_STUB(NativeQueueSetEventFilter, "Br+PQRiVa6E",
                     "sceSystemLogger2NativeQueueClientSetEventFilter")

static void InitSl2Queue(Loader::SymbolDatabase* s) {
	LIB_FUNC("q8a6R8SJHms", NativeQueueGetEvent);
	LIB_FUNC("YmAOA8iYW8w", NativeQueueGetEvent2);
	LIB_FUNC("cUL8DS6BoQU", NativeQueueInitialize);
	LIB_FUNC("qjM4x55l-XY", NativeQueueTerminate);
	LIB_FUNC("FJyVzN8qkOo", NativeQueueSetDeliveryEnable);
	LIB_FUNC("obQpnx50kEk", NativeQueueSetNativeQueueConfig);
	LIB_FUNC("4fg-S8skme4", NativeQueueSetBatchDeliveryResult);
	LIB_FUNC("X33t3dyMmLg", NativeQueueCheckData);
	LIB_FUNC("libpnl6UIGg", NativeQueueCheckAndMaskData);
	LIB_FUNC("Br+PQRiVa6E", NativeQueueSetEventFilter);
}

} // namespace Sl2Queue

namespace Sl2Delivery {

LIB_VERSION("SystemLogger2Delivery", 1, "SystemLogger2Delivery", 1, 1);

JSCD_MISC_NAMED_STUB(DeliveryGetBatchResult, "CTKW9UibxEA",
                     "sceSystemLogger2DeliveryGetBatchResult")
JSCD_MISC_NAMED_STUB(DeliveryInitialize, "P-FtLXCs94E", "sceSystemLogger2DeliveryInitialize")
JSCD_MISC_NAMED_STUB(DeliveryTerminate, "tV3N4LyIutE", "sceSystemLogger2DeliveryTerminate")
JSCD_MISC_NAMED_STUB(DeliverySetBatchDeliveryEvent, "rgrhz3d6cx8",
                     "sceSystemLogger2DeliverySetBatchDeliveryEvent")
JSCD_MISC_NAMED_STUB(DeliverySuspend, "Ag1sXZLkTjA", "sceSystemLogger2DeliverySuspend")
JSCD_MISC_NAMED_STUB(DeliveryResume, "8Hc2nBp2nG4", "sceSystemLogger2DeliveryResume")

static void InitSl2Delivery(Loader::SymbolDatabase* s) {
	LIB_FUNC("CTKW9UibxEA", DeliveryGetBatchResult);
	LIB_FUNC("P-FtLXCs94E", DeliveryInitialize);
	LIB_FUNC("tV3N4LyIutE", DeliveryTerminate);
	LIB_FUNC("rgrhz3d6cx8", DeliverySetBatchDeliveryEvent);
	LIB_FUNC("Ag1sXZLkTjA", DeliverySuspend);
	LIB_FUNC("8Hc2nBp2nG4", DeliveryResume);
}

} // namespace Sl2Delivery

namespace Json {

LIB_VERSION("Json2", 1, "Json", 1, 1);

// bI5AGFMydrA JsonArrayCopyCtor, iAIYn4oAWvI JsonArrayIteratorArrow, GEylwSsiGG8 JsonArrayIteratorInc,
// and sn4HNCtNRzY JsonValueGetUInteger are implemented in libJson2.cpp
JSCD_MISC_NAMED_STUB(JsonObjectErase, "3ZhL-MkICwI", "sce::Json::Object::erase")
JSCD_MISC_NAMED_STUB(JsonInitializerCtor, "6qFqND4iwPA", "sce::Json::Initializer::Initializer")
JSCD_MISC_NAMED_STUB(JsonInitializerDtor, "qQZGdYkEytk", "sce::Json::Initializer::~Initializer")
JSCD_MISC_NAMED_STUB(JsonStringSize, "UVBkipSD2sw", "sce::Json::String::size")

static void InitJson(Loader::SymbolDatabase* s) {
	LIB_FUNC("3ZhL-MkICwI", JsonObjectErase);
	LIB_FUNC("6qFqND4iwPA", JsonInitializerCtor);
	LIB_FUNC("qQZGdYkEytk", JsonInitializerDtor);
	LIB_FUNC("UVBkipSD2sw", JsonStringSize);
}

} // namespace Json

namespace AppInst {

LIB_VERSION("AppInstUtil", 1, "AppInstUtil", 1, 1);

JSCD_MISC_NAMED_STUB(AppGetAppInstallStatus, "9KXSwPg4PlM", "sceAppInstUtilAppGetAppInstallStatus")
JSCD_MISC_NAMED_STUB(AppInstInitialize, "540lotO7oHE", "sceAppInstUtilInitialize")
JSCD_MISC_NAMED_STUB(AppInstTerminate, "kLLazhNh6d4", "sceAppInstUtilTerminate")
JSCD_MISC_NAMED_STUB(AppUnInstallForRemote, "O-Q3I-ZthsY", "sceAppInstUtilAppUnInstallForRemote")
JSCD_MISC_NAMED_STUB(AppUnInstallForRemote2, "lY+UDLhYjhA", "sceAppInstUtilAppUnInstallForRemote2")
JSCD_MISC_NAMED_STUB(GetInstallStatus, "1wsyLPfjnmg", "sceAppInstUtilGetInstallStatus")
JSCD_MISC_NAMED_STUB(GetNeededStorageSize, "XtygktjmmuA", "sceAppInstUtilGetNeededStorageSize")
JSCD_MISC_NAMED_STUB(AppGetStorageDestType, "FXr8AvwdsXM", "sceAppInstUtilAppGetStorageDestType")

static void InitAppInst(Loader::SymbolDatabase* s) {
	LIB_FUNC("9KXSwPg4PlM", AppGetAppInstallStatus);
	LIB_FUNC("540lotO7oHE", AppInstInitialize);
	LIB_FUNC("kLLazhNh6d4", AppInstTerminate);
	LIB_FUNC("O-Q3I-ZthsY", AppUnInstallForRemote);
	LIB_FUNC("lY+UDLhYjhA", AppUnInstallForRemote2);
	LIB_FUNC("1wsyLPfjnmg", GetInstallStatus);
	LIB_FUNC("XtygktjmmuA", GetNeededStorageSize);
	LIB_FUNC("FXr8AvwdsXM", AppGetStorageDestType);
}

} // namespace AppInst

namespace Kernel {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

JSCD_MISC_NAMED_STUB(PthreadSetschedotherNp, "ymCrwbFD4fk", "pthread_setschedother_np")
JSCD_MISC_NAMED_STUB(GetResidentFmemCount, "a7DM+5cDkAg", "sceKernelGetResidentFmemCount")
JSCD_MISC_NAMED_STUB(SetPhysFmemLimit, "BDLSyH7y6Mo", "sceKernelSetPhysFmemLimit")
JSCD_MISC_NAMED_STUB(InternalGetMapStatistics, "n2nyhRL5uUg", "sceKernelInternalGetMapStatistics")
JSCD_MISC_NAMED_STUB(RandomizedPath, "p2suRCR4KqE", "sceKernelRandomizedPath")

static void InitKernel(Loader::SymbolDatabase* s) {
	LIB_FUNC("ymCrwbFD4fk", PthreadSetschedotherNp);
	LIB_FUNC("a7DM+5cDkAg", GetResidentFmemCount);
	LIB_FUNC("BDLSyH7y6Mo", SetPhysFmemLimit);
	LIB_FUNC("n2nyhRL5uUg", InternalGetMapStatistics);
	LIB_FUNC("p2suRCR4KqE", RandomizedPath);
}

} // namespace Kernel

namespace Rnps {

LIB_VERSION("RnpsAppMgr", 1, "RnpsAppMgr", 1, 1);

JSCD_MISC_NAMED_STUB(IsBgsInstallReady, "d+JHa2D36oA", "sceRnpsAppMgrIsBgsInstallReady")

// 13.60 libSceRnpsAppMgr: 0x1b50 forwards one 0x80-byte output via
// IPC 0x3000c (0xb10). The ShellUI native binding is directly to this NID;
// the managed wrapper's two ByValTStr[64] fields have the same layout.
struct MboInfo {
	char version[64];
	char path[64];
};
static_assert(sizeof(MboInfo) == 0x80 && offsetof(MboInfo, path) == 0x40);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetMboInfo(MboInfo* info) {
	PRINT_NAME();
	if (info == nullptr) {
		return static_cast<int>(0x81850003u);
	}

	// SceShellCore BuildMboInfo (0x88e300) builds the PUP entry from
	// appsRoot + "/NPXS40195" even without an OTA MBO. Its manifest reader
	// (0x88efc0) returns an empty version on read/parse failure. GetMboInfo
	// (0x894bf0) copies version/path in that order and returns success.
	// This is the bundled entry, matching the current PUP-backed app service.
	static constexpr char path[] = "/system_ex/rnps/apps/NPXS40195";
	static_assert(sizeof(path) <= sizeof(info->path));
	*info = {};
	std::memcpy(info->path, path, sizeof(path));
	std::ifstream manifest(Libs::LibKernel::FileSystem::GetRealFilename(
	    "/system_ex/rnps/apps/NPXS40195/manifest.json"));
	if (manifest) {
		const auto root = nlohmann::json::parse(manifest, nullptr, false);
		if (root.is_object()) {
			const auto version = root.find("applicationVersion");
			if (version != root.end() && version->is_string()) {
				const auto& text = version->get_ref<const std::string&>();
				const size_t length = text.size() < sizeof(info->version) - 1 ? text.size() : sizeof(info->version) - 1;
				std::memcpy(info->version, text.data(), length);
			}
		}
	}
	LOGF("\t sceRnpsAppMgrGetMboInfo version='%s' path='%s'\n", info->version, info->path);
	return 0;
}
JSCD_MISC_NAMED_STUB(BlockAppInstall, "6UbbC7GA9MI", "sceRnpsAppMgrBlockAppInstall")
JSCD_MISC_NAMED_STUB(UnblockAppInstall, "nfsELpNQ0fo", "sceRnpsAppMgrUnblockAppInstall")
JSCD_MISC_NAMED_STUB(IsMboUpdated, "1kmIv01KxE0", "sceRnpsAppMgrIsMboUpdated")
JSCD_MISC_NAMED_STUB(RemoveApp, "Yrht530r9FI", "sceRnpsAppMgrRemoveApp")
// ps5-vsh: sceRnpsAppMgrGetAppInfo ("JgBgvkAFMwc") used to be a no-op stub here.
// It is the TwinTurbo bundle-root source for the ShellUI RN host
// (libNativeExtensions 0x1e2d0 -> 0x4b path), so it now has a real record
// implementation in libRnpsAppMgr.cpp (registered by InitRnpsAppMgr_1, which
// libs.cpp runs before JscdMisc's init, so there is no shadowing either way).

static void InitRnps(Loader::SymbolDatabase* s) {
	LIB_FUNC("d+JHa2D36oA", IsBgsInstallReady);
	LIB_FUNC("9sYn+PCy608", GetMboInfo);
	LIB_FUNC("6UbbC7GA9MI", BlockAppInstall);
	LIB_FUNC("nfsELpNQ0fo", UnblockAppInstall);
	LIB_FUNC("1kmIv01KxE0", IsMboUpdated);
	LIB_FUNC("Yrht530r9FI", RemoveApp);
}

} // namespace Rnps

LIB_DEFINE(InitJscdMisc_1) {
	JscdMisc::AppDb::InitAppDb(s);
	JscdMisc::Bgs::InitBgs(s);
	JscdMisc::Sl2Queue::InitSl2Queue(s);
	JscdMisc::Sl2Delivery::InitSl2Delivery(s);
	JscdMisc::Json::InitJson(s);
	JscdMisc::AppInst::InitAppInst(s);
	JscdMisc::Kernel::InitKernel(s);
	JscdMisc::Rnps::InitRnps(s);
}

} // namespace JscdMisc

} // namespace Libs

// --- libs.cpp registration (paste manually; this file does NOT self-register) ---
// LIB_DEFINE(InitJscdMisc_1);
// InitJscdMisc_1(s);
