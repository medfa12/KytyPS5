#include "common/abi.h"
#include "kernel/fileSystem.h"
#include "libs/libRnpsAppMgr.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace Libs {

// ps5-vsh: libSceRnpsAppMgr (RNPS app management: install/update/events)
// Host-owned state (kyty REAL-service style, like libComposite.cpp surfaces):
// in-memory ShellUI event queue + last-call bookkeeping. Installed descriptors
// are read through the current guest mount mapping. Guest
// out-params are never written (layout unvalidated): GetAppInfoList only
// counts/logs REAL titles and returns 0.

namespace RnpsAppMgr {

LIB_VERSION("RnpsAppMgr", 1, "RnpsAppMgr", 1, 1);

namespace RnpsAppMgr {

static std::mutex              g_eventMutex;
static std::deque<std::string> g_events;
static std::string             g_lastUpdateUrl;
static std::string             g_lastCrlUrl;
static bool                    g_blocked         = false;
static bool                    g_destroyedBundle = false;

// The RNPS app tree comes from the current guest mount, including per-title
// overlays; never consult a different firmware tree on the host. The guest root
// is the path the ShellUI's own
// service reports for an entry from that tree (SceShellCore strings
// "./system_ex/rnps/apps." / "%s/application.ps.bundle").
static constexpr const char* kRnpsAppsGuestRoot = "/system_ex/rnps/apps";

static std::string ReadGuestString(uint64_t addr) {
	if (addr == 0) {
		return {};
	}
	const char* p = reinterpret_cast<const char*>(addr);
	size_t      n = 0;
	while (n < 4096 && p[n] != '\0') {
		++n;
	}
	return std::string(p, n);
}

static std::string PickJsonString(const std::string& text, const char* key) {
	const std::string k = std::string("\"") + key + "\"";
	const size_t      kp = text.find(k);
	if (kp == std::string::npos) {
		return {};
	}
	const size_t colon = text.find(':', kp + k.size());
	if (colon == std::string::npos) {
		return {};
	}
	const size_t q0 = text.find('"', colon + 1);
	if (q0 == std::string::npos) {
		return {};
	}
	const size_t q1 = text.find('"', q0 + 1);
	if (q1 == std::string::npos) {
		return {};
	}
	return text.substr(q0 + 1, q1 - q0 - 1);
}

// ps5-vsh: sceRnpsAppMgrGetAppInfo(2) record. Recovered from the managed
// interop structure in Sce.Vsh.RnpsAppMgrWrapper.dll (class .AppInfo, field
// order titleId, version, preinstalled, path, downloadUrl; constants
// MAX_TITLEID_SIZE=10, MAX_VERSION_SIZE=0x40, MAX_PATH_SIZE=0x40,
// MAX_URL_SIZE=0x400) and cross-checked against the only consumer, the
// 13.60 libNativeExtensions TwinTurbo bundle-root lookup:
//
//   f_1e2d0 @0x1e2d0   memset(out, 0, 0x48b); GetAppInfo(appId, out);
//                      if (ret < 0 && !out[0x4a]) { RemoveApp(appId, out); retry; }
//                      memcpy(user, out + 0x4b, 0x40)      } on ret >= 0
//   so sizeof(record) = 0x48b, preinstalled @0x4a, path @0x4b, all three of
//   which the declared constants reproduce exactly:
//       10 + 64 + 1 + 64 + 1024 = 1163 = 0x48b
// Path has no trailing slash: the consumer appends "/" then <name>.ps.bundle.hbc
// (0x33e87 writes word 0x2f), and the bundle files sit in the app dir.
static constexpr size_t kAppInfoSize        = 0x48b;
static constexpr size_t kAppInfoTitleIdOff  = 0x00;
static constexpr size_t kAppInfoTitleIdLen  = 10;
static constexpr size_t kAppInfoVersionOff  = 0x0a;
static constexpr size_t kAppInfoVersionLen  = 0x40;
static constexpr size_t kAppInfoPreinstOff  = 0x4a;
static constexpr size_t kAppInfoPathOff     = 0x4b;
static constexpr size_t kAppInfoPathLen     = 0x40;
// libNativeExtensions only treats 0x81850008 as "no usable path" and maps it to
// a null root; 0x81850003 is the client's own invalid-argument code.
static constexpr int kRnpsAppMgrInvalidArgument = static_cast<int>(0x81850003u);
static constexpr int kRnpsAppMgrAppNotInstalled = static_cast<int>(0x81850008u);

// Bounded copy of a NUL-terminated field, never leaving the record unterminated.
static void SetAppInfoField(uint8_t* record, size_t off, size_t len, const std::string& value) {
	const size_t n = (std::min)(value.size(), len - 1);
	std::memcpy(record + off, value.data(), n);
	record[off + n] = '\0';
}

// Safe no-op success that still shows causality: last-call state in the log.
#define KYTY_RNPSAPPMGR_NOOP_STATE(name)                                            \
	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI name(                                            \
	    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {                 \
		PRINT_NAME();                                                    \
		LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64                \
		     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64                 \
		     " blocked = %d destroyedBundle = %d updateUrl = '%s' "      \
		     "crlUrl = '%s' -> 0\n",                                     \
		     a0, a1, a2, a3, g_blocked ? 1 : 0,                          \
		     g_destroyedBundle ? 1 : 0, g_lastUpdateUrl.c_str(),          \
		     g_lastCrlUrl.c_str());                                      \
		(void)a0;                                                        \
		(void)a1;                                                        \
		(void)a2;                                                        \
		(void)a3;                                                        \
		return 0;                                                        \
	}

KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrCreateUpdateRequest)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrDestroyUpdateRequest)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrRequestUpdateCheck)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrGetUpdateProgress)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrCreateUrlRequest)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrRemoveApp)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrRemoveAppOnSystemShutdown)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrRemoveUfsImageOnSystemShutdown)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrRecoverUfsImage)
KYTY_RNPSAPPMGR_NOOP_STATE(SceRnpsAppMgrTerminate)

#undef KYTY_RNPSAPPMGR_NOOP_STATE

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrSetUpdateUrl(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	std::string url = ReadGuestString(a0 != 0 ? a0 : a1);
	if (!url.empty()) {
		g_lastUpdateUrl = url;
	}
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " updateUrl = '%s' -> 0\n",
	     a0, a1, a2, a3, g_lastUpdateUrl.c_str());
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrSetCrlUrl(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	std::string url = ReadGuestString(a0 != 0 ? a0 : a1);
	if (!url.empty()) {
		g_lastCrlUrl = url;
	}
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " crlUrl = '%s' -> 0\n",
	     a0, a1, a2, a3, g_lastCrlUrl.c_str());
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrBlockAppInstall(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	g_blocked = true;
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " blocked = 1 -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrUnblockAppInstall(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	g_blocked = false;
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " blocked = 0 -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrDestroyBundle(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	g_destroyedBundle = true;
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " destroyedBundle = 1 (safe no-op) -> 0\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrGetAppInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	if (a0 == 0 || a1 == 0) {
		LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " (null) -> 0x%08x\n", a0, a1,
		     kRnpsAppMgrInvalidArgument);
		return kRnpsAppMgrInvalidArgument;
	}
	const std::string title_id = ReadGuestString(a0);
	uint8_t*          record   = reinterpret_cast<uint8_t*>(a1);
	// The service owns the whole record: clear it so nothing we do not populate
	// (downloadUrl, reserved bytes) can leak stale guest bytes into the caller.
	std::memset(record, 0, kAppInfoSize);
	if (title_id.empty() || title_id.size() >= kAppInfoTitleIdLen) {
		LOGF("\t titleId = '%s' (empty or longer than %zu) -> 0x%08x\n", title_id.c_str(),
		     kAppInfoTitleIdLen - 1, kRnpsAppMgrInvalidArgument);
		return kRnpsAppMgrInvalidArgument;
	}
	try {
		namespace fs            = std::filesystem;
		const std::string guest_manifest = std::string(kRnpsAppsGuestRoot) + "/" + title_id + "/manifest.json";
		const auto resolved = LibKernel::FileSystem::ResolveGuestPath(guest_manifest.c_str());
		if (resolved.error != 0 || resolved.host.empty()) {
			return kRnpsAppMgrAppNotInstalled;
		}
		const fs::path manifest = resolved.host;
		const fs::path dir = manifest.parent_path();
		std::error_code ec;
		if (!fs::exists(manifest, ec) || ec) {
			LOGF("\t titleId = '%s' no manifest.json under '%s' -> 0x%08x\n",
			     title_id.c_str(), dir.string().c_str(), kRnpsAppMgrAppNotInstalled);
			return kRnpsAppMgrAppNotInstalled;
		}
		// Version is read from the installed descriptor, never invented.
		std::string version;
		{
			std::ifstream f(manifest);
			if (f.is_open()) {
				std::stringstream ss;
				ss << f.rdbuf();
				version = PickJsonString(ss.str(), "applicationVersion");
			}
		}
		// Everything under this root is a preinstalled (KI) title: it is the
		// read-only /system_ex tree, so preinstalled = 1 like the real service.
		const std::string path = std::string(kRnpsAppsGuestRoot) + "/" + title_id;
		SetAppInfoField(record, kAppInfoTitleIdOff, kAppInfoTitleIdLen, title_id);
		SetAppInfoField(record, kAppInfoVersionOff, kAppInfoVersionLen, version);
		record[kAppInfoPreinstOff] = 1;
		SetAppInfoField(record, kAppInfoPathOff, kAppInfoPathLen, path);
		LOGF("\t titleId = '%s' version = '%s' preinstalled = 1 path = '%s' -> 0\n",
		     title_id.c_str(), version.c_str(), path.c_str());
		return 0;
	} catch (...) {
		LOGF("\t titleId = '%s' scan failed -> 0x%08x\n", title_id.c_str(),
		     kRnpsAppMgrAppNotInstalled);
		return kRnpsAppMgrAppNotInstalled;
	}
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrGetAppInfoList(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	size_t count = 0;
	try {
		namespace fs                 = std::filesystem;
		const auto resolved = LibKernel::FileSystem::ResolveGuestPath(kRnpsAppsGuestRoot);
		if (resolved.error != 0 || resolved.host.empty()) {
			LOGF("\t appInfoList: guest app root unavailable, empty list -> 0\n");
			return 0;
		}
		const fs::path root = resolved.host;
		std::error_code ec;
		if (!fs::exists(root, ec) || ec) {
			LOGF("\t appInfoList: dump dir missing, empty list -> 0\n");
			return 0;
		}
		for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
			if (ec) {
				break;
			}
			try {
				if (!it->is_directory(ec) || ec) {
					continue;
				}
				const std::string guest_manifest = std::string(kRnpsAppsGuestRoot) + "/" +
				                                   it->path().filename().string() + "/manifest.json";
				const auto entry = LibKernel::FileSystem::ResolveGuestPath(guest_manifest.c_str());
				if (entry.error != 0 || entry.host.empty()) {
					continue;
				}
				const fs::path man = entry.host;
				if (!fs::exists(man, ec) || ec) {
					continue;
				}
				std::ifstream f(man);
				if (!f.is_open()) {
					continue;
				}
				std::stringstream ss;
				ss << f.rdbuf();
				const std::string text = ss.str();
				const std::string tid  = PickJsonString(text, "titleId");
				const std::string name = PickJsonString(text, "applicationName");
				++count;
				LOGF("\t appInfoList: titleId = '%s' name = '%s'\n", tid.c_str(),
				     name.c_str());
			} catch (...) {
				continue;
			}
		}
	} catch (...) {
		LOGF("\t appInfoList: scan failed, empty list -> 0\n");
		return 0;
	}
	if (a0 == 0) {
		LOGF("\t appInfoList: count = %zu (null out-param, no guest write) -> 0\n", count);
		return 0;
	}
	// Guest out-param layout is unvalidated: never write it, just report.
	LOGF("\t appInfoList: count = %zu a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " (no guest write) -> 0\n",
	     count, a0, a1, a2, a3);
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrHasEventForShellUI(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	size_t pending = 0;
	{
		std::lock_guard<std::mutex> lock(g_eventMutex);
		pending = g_events.size();
	}
	const int has = pending != 0 ? 1 : 0;
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " pending = %zu -> %d\n",
	     a0, a1, a2, a3, pending, has);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return has;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrGetEventForShellUI(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	std::string ev;
	size_t      remaining = 0;
	{
		std::lock_guard<std::mutex> lock(g_eventMutex);
		if (g_events.empty()) {
			LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " empty -> -1\n", a0, a1);
			(void)a2;
			(void)a3;
			return -1;
		}
		ev        = g_events.front();
		g_events.pop_front();
		remaining = g_events.size();
	}
	if (a0 == 0) {
		LOGF("\t null buffer, popped '%s' discarded remaining = %zu -> 0\n", ev.c_str(),
		     remaining);
		(void)a1;
		(void)a2;
		(void)a3;
		return 0;
	}
	const uint64_t cap = a1 != 0 ? a1 : a2;
	char*          out = reinterpret_cast<char*>(a0);
	if (cap == 0) {
		std::memcpy(out, ev.c_str(), ev.size() + 1);
	} else if (cap == 1) {
		out[0] = '\0';
	} else {
		const size_t n = (std::min)(ev.size(), static_cast<size_t>(cap - 1));
		std::memcpy(out, ev.data(), n);
		out[n] = '\0';
	}
	LOGF("\t popped '%s' cap = %llu remaining = %zu -> 0\n", ev.c_str(),
	     static_cast<unsigned long long>(cap), remaining);
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsAppMgrSetEventFlagForClients(uint64_t a0, uint64_t a1,
                                                             uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	std::string s = ReadGuestString(a0);
	if (s.empty()) {
		s = "rnps.event.flag";
	}
	size_t pending = 0;
	// ps5-vsh: same bound as the host Enqueue path (drop oldest past cap).
	static constexpr size_t kMaxShellUIEvents = 64;
	{
		std::lock_guard<std::mutex> lock(g_eventMutex);
		g_events.push_back(s);
		while (g_events.size() > kMaxShellUIEvents) {
			g_events.pop_front();
		}
		pending = g_events.size();
	}
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " pushed '%s' pending = %zu -> 0\n",
	     a0, a1, a2, a3, s.c_str(), pending);
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

} // namespace RnpsAppMgr

LIB_DEFINE(InitRnpsAppMgr_1) {
	{
		std::lock_guard<std::mutex> lock(RnpsAppMgr::g_eventMutex);
		if (RnpsAppMgr::g_events.empty()) {
			RnpsAppMgr::g_events.emplace_back("rnps.login.done uid=1000");
		}
	}
	LIB_FUNC("Jw7u7WgTeJs", RnpsAppMgr::SceRnpsAppMgrCreateUpdateRequest);
	LIB_FUNC("BlZothImIe4", RnpsAppMgr::SceRnpsAppMgrDestroyUpdateRequest);
	LIB_FUNC("O3BGBzl-Xx0", RnpsAppMgr::SceRnpsAppMgrRequestUpdateCheck);
	LIB_FUNC("IgMowNrTFnQ", RnpsAppMgr::SceRnpsAppMgrGetUpdateProgress);
	LIB_FUNC("CeoDV-N8yBY", RnpsAppMgr::SceRnpsAppMgrCreateUrlRequest);
	LIB_FUNC("ISNcXEI3HrQ", RnpsAppMgr::SceRnpsAppMgrSetUpdateUrl);
	LIB_FUNC("ELeJhwuG-q4", RnpsAppMgr::SceRnpsAppMgrSetCrlUrl);
	LIB_FUNC("6UbbC7GA9MI", RnpsAppMgr::SceRnpsAppMgrBlockAppInstall);
	LIB_FUNC("nfsELpNQ0fo", RnpsAppMgr::SceRnpsAppMgrUnblockAppInstall);
	LIB_FUNC("J55AwNbGHB0", RnpsAppMgr::SceRnpsAppMgrGetAppInfoList);
	// ps5-vsh: the 13.60 NID (verified from the ctype1 Relocate lines for both
	// importers: _ShellUIManaged/native/eboot.bin and libNativeExtensions.sprx).
	// libJscdMisc's Rnps block used to carry this as a no-op stub; it registers
	// *after* this file, so the stub would shadow a real implementation there.
	LIB_FUNC("JgBgvkAFMwc", RnpsAppMgr::SceRnpsAppMgrGetAppInfo);
	LIB_FUNC("Yrht530r9FI", RnpsAppMgr::SceRnpsAppMgrRemoveApp);
	LIB_FUNC("OjXJrpWKok8", RnpsAppMgr::SceRnpsAppMgrHasEventForShellUI);
	LIB_FUNC("Cqj2bpN3gcU", RnpsAppMgr::SceRnpsAppMgrGetEventForShellUI);
	LIB_FUNC("MJD4BrPh7pA", RnpsAppMgr::SceRnpsAppMgrSetEventFlagForClients);
	LIB_FUNC("Lvxc2gSxsRA", RnpsAppMgr::SceRnpsAppMgrDestroyBundle);
	LIB_FUNC("Dex0a4BzZiM", RnpsAppMgr::SceRnpsAppMgrRemoveAppOnSystemShutdown);
	LIB_FUNC("DsI+ycl8YLI", RnpsAppMgr::SceRnpsAppMgrRemoveUfsImageOnSystemShutdown);
	LIB_FUNC("D4UQhpiNn4E", RnpsAppMgr::SceRnpsAppMgrRecoverUfsImage);
	LIB_FUNC("BRScmxStBzg", RnpsAppMgr::SceRnpsAppMgrTerminate);
}

namespace RnpsAppMgrHost {

// Host-side ShellUI event channel (see libRnpsAppMgr.h). Same queue the
// guest HasEvent/GetEventForShellUI NIDs drain; lets the BGS dispatch path
// feed ShellCore-visible events without a guest caller.
void EnqueueShellUIEvent(const std::string& event) {
	size_t pending = 0;
	// ps5-vsh: bound the channel (no guest drains it yet; ShellCore polls
	// HasEvent/GetEventForShellUI when driven). Drop oldest past the cap.
	static constexpr size_t kMaxShellUIEvents = 64;
	{
		std::lock_guard<std::mutex> lock(RnpsAppMgr::g_eventMutex);
		RnpsAppMgr::g_events.push_back(event.empty() ? "rnps.event.flag" : event);
		while (RnpsAppMgr::g_events.size() > kMaxShellUIEvents) {
			RnpsAppMgr::g_events.pop_front();
		}
		pending = RnpsAppMgr::g_events.size();
	}
	LOGF("\t [RNPS host] EnqueueShellUIEvent '%s' pending = %zu\n", event.c_str(),
	     pending);
}

size_t PendingShellUIEvents() {
	std::lock_guard<std::mutex> lock(RnpsAppMgr::g_eventMutex);
	return RnpsAppMgr::g_events.size();
}

bool TryConsumeShellUIEvent(std::string* out) {
	std::lock_guard<std::mutex> lock(RnpsAppMgr::g_eventMutex);
	if (RnpsAppMgr::g_events.empty()) {
		return false;
	}
	if (out != nullptr) {
		*out = std::move(RnpsAppMgr::g_events.front());
	}
	RnpsAppMgr::g_events.pop_front();
	LOGF("\t [RNPS host] ConsumeShellUIEvent remaining = %zu\n",
	     RnpsAppMgr::g_events.size());
	return true;
}

} // namespace RnpsAppMgrHost

} // namespace RnpsAppMgr

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp (append at end of file) ----
//
// namespace RnpsAppMgr {
// LIB_DEFINE(InitRnpsAppMgr_1);
// } // namespace RnpsAppMgr
//
// ... and inside Libs::InitAll, alongside the other ps5-vsh entries:
//
//   RnpsAppMgr::InitRnpsAppMgr_1(s);
