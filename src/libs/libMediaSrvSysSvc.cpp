#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

// ps5-vsh: NPXS40039 MediaCoreServer service-loop coverage. These 20
// SystemService NIDs are PLT-imported by the title but never called yet;
// none is registered anywhere in src/libs (checked), so register
// log-and-succeed stubs here so future paths resolve instead of hitting
// the unresolved-import thunk. 19 names come from _ProbeLogs/nids.csv;
// a1TOY4fwA5Y has no csv row, so it gets a local unknown-stub body
// (LogUnknownStub lives in LibC without a header declaration, so it is
// not referenced cross-TU).

LIB_VERSION("SystemService", 1, "SystemService", 1, 1);

namespace MediaSrvSysSvc {

#define MEDIA_SRV_SYS_SVC_STUB(func, nid)                                                  \
	static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI func(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { \
		PRINT_NAME();                                                                      \
		LOGF("\t NID " nid " " #func "(%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64          \
		     ", %016" PRIx64 ") -> 0\n",                                                   \
		     a0, a1, a2, a3);                                                              \
		return 0;                                                                          \
	}

MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceAddLocalProcess, "0cl8SuwosPQ")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppTitleId, "0YmI09jOP-g")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceKillLocalProcess, "6jpZY0WUwLM")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceIsAppSuspended, "d-15YTCUMVU")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetSubAppTitleId, "Ewjywv3W4QA")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetRemoteplayStatus, "Getl+RqEhVQ")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceIsGameLiveStreamingOnAir, "HGA2SV-Gkq8")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceAppGetAppInstallStatus, "HOSnwjsiZMk")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetUIStatus, "J8t8t3eVoMg")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetSubAppTitleIdForSP, "Kc8KQ-sG2uA")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceLaunchApp, "l4FB3wNa-Ac")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetSpotifyTitleId, "MLIJkgKJV4k")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceKillApp, "N4RkyJh7FtA")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppIdOfRunningBigApp, "NwT2R5lo8P8")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppStatusListContainsDaemon, "O+90p3x1qu4")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppStatus, "t5ShV0jWEFE")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppId, "TtItikWwBgs")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppFocusedAppStatus, "xjE7xLfrLUk")
MEDIA_SRV_SYS_SVC_STUB(sceSystemServiceGetAppType, "YLbhAXS20C0")

// Unknown NID (no _ProbeLogs/nids.csv row): local log-and-0 with the NID
// in the log line, same observable behavior as the generic stub.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_Unka1TOY4fwA5Y(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	LOGF("\t NID a1TOY4fwA5Y (unknown)(%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64
	     ", %016" PRIx64 ") -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

LIB_DEFINE(InitMediaSrvSysSvc_1) {
	LIB_FUNC("0cl8SuwosPQ", MediaSrvSysSvc::sceSystemServiceAddLocalProcess);
	LIB_FUNC("0YmI09jOP-g", MediaSrvSysSvc::sceSystemServiceGetAppTitleId);
	LIB_FUNC("6jpZY0WUwLM", MediaSrvSysSvc::sceSystemServiceKillLocalProcess);
	LIB_FUNC("a1TOY4fwA5Y", MediaSrvSysSvc::LogStub_Unka1TOY4fwA5Y);
	LIB_FUNC("d-15YTCUMVU", MediaSrvSysSvc::sceSystemServiceIsAppSuspended);
	LIB_FUNC("Ewjywv3W4QA", MediaSrvSysSvc::sceSystemServiceGetSubAppTitleId);
	LIB_FUNC("Getl+RqEhVQ", MediaSrvSysSvc::sceSystemServiceGetRemoteplayStatus);
	LIB_FUNC("HGA2SV-Gkq8", MediaSrvSysSvc::sceSystemServiceIsGameLiveStreamingOnAir);
	LIB_FUNC("HOSnwjsiZMk", MediaSrvSysSvc::sceSystemServiceAppGetAppInstallStatus);
	LIB_FUNC("J8t8t3eVoMg", MediaSrvSysSvc::sceSystemServiceGetUIStatus);
	LIB_FUNC("Kc8KQ-sG2uA", MediaSrvSysSvc::sceSystemServiceGetSubAppTitleIdForSP);
	LIB_FUNC("l4FB3wNa-Ac", MediaSrvSysSvc::sceSystemServiceLaunchApp);
	LIB_FUNC("MLIJkgKJV4k", MediaSrvSysSvc::sceSystemServiceGetSpotifyTitleId);
	LIB_FUNC("N4RkyJh7FtA", MediaSrvSysSvc::sceSystemServiceKillApp);
	LIB_FUNC("NwT2R5lo8P8", MediaSrvSysSvc::sceSystemServiceGetAppIdOfRunningBigApp);
	LIB_FUNC("O+90p3x1qu4", MediaSrvSysSvc::sceSystemServiceGetAppStatusListContainsDaemon);
	LIB_FUNC("t5ShV0jWEFE", MediaSrvSysSvc::sceSystemServiceGetAppStatus);
	LIB_FUNC("TtItikWwBgs", MediaSrvSysSvc::sceSystemServiceGetAppId);
	LIB_FUNC("xjE7xLfrLUk", MediaSrvSysSvc::sceSystemServiceGetAppFocusedAppStatus);
	LIB_FUNC("YLbhAXS20C0", MediaSrvSysSvc::sceSystemServiceGetAppType);
}

} // namespace MediaSrvSysSvc

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp (paste, do not edit this file) ----
// LIB_DEFINE(InitMediaSrvSysSvc_1);
// ... in InitAll():  InitMediaSrvSysSvc_1(s);
