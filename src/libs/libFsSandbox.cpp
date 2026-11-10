#include "common/abi.h"
#include "common/common.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

// ps5-vsh: ShellCore boot STEPS 2+3 — FsSandbox mounts + SysCore process mgmt.
//
// ShellCore's early mount storm (sceFsMountSaveData, trophy/download siblings,
// M2 local-sandbox mounts, LncService MountToFsSandbox family) succeeds without
// real PFS: every mount maps onto FileSystem::Mount (which auto-umounts the same
// point, so storm re-mounts are idempotent), backed by a writable skeleton dir
// that is CREATEd on demand. SysCore process queries report a single primary
// process; kill/raise paths are logged NO-OPs. All entry points are null-safe
// and fail-soft (return success even when the backing cannot be created).
//
// NID absence was verified against src/libs before adding (skipped: e-ODUaoHXiY,
// yvbO67OvrFc, 0YmjfSJ8X98, 9PCBPZcDU7Q, MSQ1n+tzSEA, j8aGZfdbhc8 — already in
// libC.cpp). New LIB_DEFINE is InitFsSandbox_1; wire-up snippet for
// src/libs/libs.cpp is appended at the end of this file.

namespace Libs {

namespace FsSandbox {

// (LIB_VERSION tags live on the three sub-namespaces below so each NID set
// resolves under its own lib/mod tags.)

namespace Detail {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool IsGuestReadable(const void* p, size_t size) {
	// ps5-vsh: every page in [p, p+size) must be committed + readable.
	// Null is never readable (null-safe: guest-null out-params/strings).
	if (p == nullptr || size == 0) {
		return false;
	}
	auto       addr = reinterpret_cast<uint64_t>(p);
	const auto end  = addr + size;
	while (addr < end) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & 0xFFu) == 0 ||
		    (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		const auto region_end =
		    reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		if (region_end <= addr) {
			return false;
		}
		addr = region_end;
	}
	return true;
}

static bool IsGuestWritable(const void* p, size_t size) {
	// ps5-vsh: mirror of libC.cpp's probe (every page committed + writable).
	if (p == nullptr || size == 0) {
		return false;
	}
	auto       addr = reinterpret_cast<uint64_t>(p);
	const auto end  = addr + size;
	while (addr < end) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT ||
		    (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
		                      PAGE_EXECUTE_WRITECOPY)) == 0) {
			return false;
		}
		const auto region_end =
		    reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		if (region_end <= addr) {
			return false;
		}
		addr = region_end;
	}
	return true;
}
#else
static bool IsGuestReadable(const void* p, size_t size) {
	return p != nullptr && size != 0; // identity-mapped guest memory
}
static bool IsGuestWritable(const void* p, size_t size) {
	return p != nullptr && size != 0;
}
#endif

static constexpr size_t kMaxGuestStr = 256;

static std::string SafeGuestStr(uint64_t addr) {
	// ps5-vsh: bounded, probed guest-string read. Never crashes: rejects null
	// and unreadable windows before touching identity-mapped memory.
	if (addr == 0 || !IsGuestReadable(reinterpret_cast<const void*>(addr), 1)) {
		return {};
	}
	if (!IsGuestReadable(reinterpret_cast<const void*>(addr), kMaxGuestStr)) {
		return {};
	}
	const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(addr));
	size_t      n = 0;
	while (n < kMaxGuestStr && s[n] != '\0') {
		++n;
	}
	if (n == 0 || n >= kMaxGuestStr) {
		return {};
	}
	return std::string(s, n);
}

static bool IsMountPoint(const std::string& s) {
	if (s.empty() || s.size() >= 128 || s[0] != '/') {
		return false;
	}
	for (char c : s) {
		if (c < 0x20 || c == '\\') {
			return false;
		}
	}
	return true;
}

static std::string SandboxTitle() {
	std::string t;
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", &t) && !t.empty()) {
		return t;
	}
	return "UNKNOWN";
}

// Mount strategy: prefer the boot's writable jail area (_FwData/<title> at
// /data, created by emulator.cpp from --fw-root TITLE_ID); otherwise CREATE the
// _SaveData/<title> (or per-family) skeleton dir in CWD. Fail-soft: always
// returns the backing path (Mount itself never throws; MountPoints::Mount
// auto-umounts the same point so the mount storm stays idempotent).
static std::filesystem::path SandboxBacking(const std::string& family,
                                            const std::filesystem::path* captured_data_root = nullptr) {
	const std::string title = SandboxTitle();
	const auto data = captured_data_root != nullptr ? *captured_data_root :
	                                                LibKernel::FileSystem::GetRealFilename("/data");
	std::filesystem::path backing;
	if (!data.empty()) {
		backing = data / "savedata" / family;
	} else {
		backing = std::filesystem::path("_SaveData") / title / family;
	}
	std::error_code ec;
	std::filesystem::create_directories(backing, ec);
	if (ec) {
		LOGF("\t skeleton create failed for %s (%s), mounting anyway\n",
		     backing.generic_string().c_str(), ec.message().c_str());
	}
	return backing;
}

static void EnsureShellSandbox(); // defined below; pre-backs /original_user, /user, /system_data

static int MountBacking(const char* op, const std::string& point, const std::string& family) {
	EnsureShellSandbox(); // storm-idempotent: same-point remount is a no-op
	const auto backing = SandboxBacking(family);
	LibKernel::FileSystem::Mount(backing, point);
	LOGF("\t %s: %s -> %s\n", op, point.c_str(), backing.generic_string().c_str());
	return OK;
}

// ps5-vsh: ShellCore ConsoleSandboxManager + USER/system_data stat-storm backing
// (shellcore_boot1.log: distinct `KernelStat ... file not found` under
// /original_user, /user, /system_data just before the null+0x18 death).
// Boots without explicit shell-root mounts leave KernelStat/Mkdir/Open(O_CREAT)
// unresolved. Launcher-selected roots take precedence over this fallback.
// FileSystem::KernelOpen with O_CREAT already creates parents + the file, so
// only the missing mount + skeleton dirs block it (hence the CsbMgr
// `Create New File` -> WriteFile 0x80020002 cascade on
// local_sandbox_info.json). Fix: pre-create the skeleton tree under
// _SaveData/<title>/mirror/... (or /data/savedata when the fw jail exists,
// via SandboxBacking) and mount each unresolved top-level root. Re-entry
// preserves existing mappings; creation LOGF is first-create-only via a set.
static std::mutex g_shellBackingMutex;
static std::unordered_set<std::string> g_shellBackingLogged;

static void LogBackingFirst(const std::string& what) {
	std::lock_guard<std::mutex> lock(g_shellBackingMutex);
	if (g_shellBackingLogged.insert(what).second) {
		LOGF("\t sandbox skeleton: %s\n", what.c_str());
	}
}

// Host backing for an exact guest path under the mirror family.
static std::filesystem::path MirrorBackingFor(const std::string& guest,
                                              const std::filesystem::path* captured_data_root = nullptr) {
	auto backing = SandboxBacking("mirror", captured_data_root);
	std::string rel = guest;
	while (!rel.empty() && rel.front() == '/') {
		rel.erase(rel.begin());
	}
	while (!rel.empty() && rel.back() == '/') {
		rel.pop_back();
	}
	if (!rel.empty()) {
		backing /= rel;
	}
	return backing;
}

struct ShellFileSeed {
	const char* guest;
	const char* content; // smallest-plausible initial bytes; guest truncates on write
};

static void EnsureShellSandbox() {
	// Directory-style entries: every distinct KernelStat `file not found`
	// from shellcore_boot1.log under the shell roots (/app0 + bundle paths
	// skipped). The /<FsSandboxRandomWord>/priv/mms point embeds the
	// per-boot random word, so it cannot be pre-backed statically.
	static constexpr const char* kShellDirs[] = {
	    "/original_user",
	    "/original_user/local_sandbox",
	    "/original_user/local_sandbox2",
	    "/original_user/local_sandbox2/tmp_sb",
	    "/original_user/local_sandbox2/sb001",
	    "/user",
	    "/user/TRASH",
	    "/user/temp",
	    "/user/temp.old",
	    "/user/temp/common_temp",
	    "/user/temp/common_temp/telemetry",
	    "/user/temp/share",
	    "/user/temp/gls/screenshot",
	    "/user/temp/fsroot_temp",
	    "/user/priv",
	    "/user/common",
	    "/user/common/playready",
	    "/user/av_contents",
	    "/user/av_contents/content_tmp",
	    "/user/av_contents/photo",
	    "/user/av_contents/video",
	    "/user/av_contents/thumbnails",
	    "/user/av_contents/sdr",
	    "/user/av_contents/extra",
	    "/user/music_player_service",
	    "/user/appmeta",
	    "/user/bgft",
	    "/user/data",
	    "/user/home",
	    "/user/system",
	    "/user/system/share",
	    "/user/system/webkit",
	    "/user/system/webkit/shell",
	    "/user/system/webkit/webbrowser",
	    "/user/savedata",
	    "/user/sshared",
	    "/user/catalog_downloader/appmeta",
	    "/system_data",
	    "/system_data/TRASH",
	    "/system_data/common",
	    "/system_data/common/mms",
	    "/system_data/common/video_sdk",
	    "/system_data/game/temp",
	    "/system_data/httpcache",
	    "/system_data/httpcache/nobackup",
	    "/system_data/music_core",
	    "/system_data/nobackup/datatransfer",
	    "/system_data/nobackup/systembackup",
	    "/system_data/nobackup/temp.old",
	    "/system_data/nobackup/temp/becore",
	    "/system_data/nobackup/temp/sqlite/other",
	    "/system_data/nobackup/temp/sqlite/system",
	    "/system_data/playgo",
	    "/system_data/priv",
	    "/system_data/priv/becore",
	    "/system_data/priv/becore/temp.old",
	    "/system_data/priv/bgdc",
	    "/system_data/priv/cache",
	    "/system_data/priv/cache/profile",
	    "/system_data/priv/cache/urlconfig",
	    "/system_data/priv/drm",
	    "/system_data/priv/error",
	    "/system_data/priv/error/history",
	    "/system_data/priv/gls",
	    "/system_data/priv/hid_config_service",
	    "/system_data/priv/home",
	    "/system_data/priv/ime_data",
	    "/system_data/priv/messages",
	    "/system_data/priv/messages/temp",
	    "/system_data/priv/messages/ui",
	    "/system_data/priv/mms",
	    "/system_data/priv/mms/nobackup",
	    "/system_data/priv/mms/nobackup/sqlite",
	    "/system_data/priv/music_player",
	    "/system_data/priv/nui_data",
	    "/system_data/priv/nui_data/voice",
	    "/system_data/priv/party",
	    "/system_data/priv/ps4appcategory",
	    "/system_data/priv/ps4sisrctl",
	    "/system_data/priv/remoteplay",
	    "/system_data/priv/settings_app",
	    "/system_data/priv/sticker",
	    "/system_data/priv/system_log",
	    "/system_data/priv/system_logger2",
	    "/system_data/priv/system_logger2/nobackup",
	    "/system_data/priv/system_logger2/nobackup/database",
	    "/system_data/priv/voice_and_agent",
	    "/system_data/priv/vrtracker2",
	    "/system_data/priv/vshlog",
	    "/system_data/priv/workaround_ctl",
	    "/system_data/savedata",
	    "/system_data/settings",
	    "/system_data/eap/rodata/home",
	};
	static constexpr ShellFileSeed kShellFiles[] = {
	    {"/original_user/local_sandbox2/local_sandbox_info.json", "{}"},
	    // ps5-vsh: user_list.dat deliberately NOT seeded (fresh boot = no
	    // users -> guest takes its initial-setup branch; an empty seeded
	    // file misreads as "setup done with zero users" and parks login).
	};
	static constexpr const char* kShellRoots[] = {
	    "/original_user", "/user", "/system_data",
	};

	// Only unresolved paths need a fallback skeleton. Existing boot-selected
	// roots/submounts (including read-only ones) own their contents; do not seed
	// either their selected backing or an unused shared mirror for those paths.
	static std::once_flag s_seedOnce;
	std::call_once(s_seedOnce, [] {
		std::error_code ec;
		for (const char* dir : kShellDirs) {
			if (!LibKernel::FileSystem::GetRealFilename(dir).empty()) { continue; }
			const auto backing = MirrorBackingFor(dir);
			ec.clear();
			const bool created = std::filesystem::create_directories(backing, ec);
			if (ec) {
				LOGF("\t skeleton create failed for %s (%s), mounting anyway\n",
				     backing.generic_string().c_str(), ec.message().c_str());
				continue;
			}
			if (created) {
				LogBackingFirst(std::string("mkdir ") + dir);
			}
		}
		for (const auto& seed : kShellFiles) {
			if (!LibKernel::FileSystem::GetRealFilename(seed.guest).empty()) { continue; }
			const std::string guest = seed.guest;
			const auto        slash = guest.rfind('/');
			if (slash != std::string::npos && slash > 0) {
				ec.clear();
				std::filesystem::create_directories(
				    MirrorBackingFor(guest.substr(0, slash)), ec);
			}
			const auto host = MirrorBackingFor(guest);
			std::error_code existsEc;
			if (!std::filesystem::exists(host, existsEc)) {
				std::ofstream out(host, std::ios::binary | std::ios::trunc);
				if (out) {
					out << seed.content;
				}
				LogBackingFirst(std::string("seed ") + guest);
			}
		}
	});

	// Mount only unresolved roots. Mount replaces an existing exact mount, so
	// calling it on a selected root would discard both its backing and RO flag.
	// This loop still runs after the once-only seed pass for fresh filesystems.
	for (const char* root : kShellRoots) {
		if (!LibKernel::FileSystem::GetRealFilename(root).empty()) { continue; }
		const auto backing = MirrorBackingFor(root);
		std::error_code ec;
		std::filesystem::create_directories(backing, ec);
		LibKernel::FileSystem::Mount(backing, root);
		LogBackingFirst(std::string(root) + " -> " + backing.generic_string());
	}
}

// Guest layout of mount calls is not fully known, so the mount point is the
// first plausible absolute-path arg; otherwise the per-family default.
static std::string PickPoint(uint64_t a1, uint64_t a2, const char* dflt) {
	for (uint64_t a : {a1, a2}) {
		const auto s = SafeGuestStr(a);
		if (IsMountPoint(s)) {
			return s;
		}
	}
	return dflt;
}

template <typename T>
static bool WriteGuest(uint64_t addr, T value) {
	if (!IsGuestWritable(reinterpret_cast<const void*>(addr), sizeof(T))) {
		LOGF("\t null/unwritable guest out-param\n");
		return false;
	}
	*reinterpret_cast<T*>(static_cast<uintptr_t>(addr)) = value;
	return true;
}

} // namespace Detail

// Dynamic backing for unmounted guest paths (per-boot random-word sandboxes
// like /<hex>/priv/...): O_CREAT under an unmounted root must land somewhere
// writable, not ENOENT. Jailed under the mirror family; rejects escapes.
std::filesystem::path BackingForUnmounted(const std::string& guest) {
	return BackingForUnmounted(guest, false);
}

std::filesystem::path BackingForUnmounted(const std::string& guest, bool create) {
	return BackingForUnmounted(guest, create, LibKernel::FileSystem::GetRealFilename("/data"));
}

std::filesystem::path BackingForUnmounted(const std::string& guest, bool create,
                                         const std::filesystem::path& captured_data_root) {
	// Scoped to direct-title boot: the chain keeps exact ENOENT semantics
	// (creating files under unmounted paths changed ShellCore's
	// existence-probing branches and destabilized it). O_CREAT callers
	// bypass the scope gate (HW creates on demand); sanitization and
	// jail below still apply.
	if (!create && !IsDirectTitleBoot()) {
		return {};
	}
	if (guest.empty() || guest.size() > 256 || guest[0] != '/') {
		return {};
	}
	for (char c : guest) {
		if (c < 0x20 || c == '\\' || c == '\0') {
			return {};
		}
	}
	if (guest.find("..") != std::string::npos) {
		return {};
	}
	return Detail::MirrorBackingFor(guest, &captured_data_root);
}

void EnsureLicenseSeed(const std::string& guest) {
	EnsureLicenseSeed(guest, LibKernel::FileSystem::GetRealFilename("/data"));
}

void EnsureLicenseSeed(const std::string& guest, const std::filesystem::path& captured_data_root) {
	// Scoped to direct-title boot (same reason as BackingForUnmounted).
	if (!IsDirectTitleBoot()) {
		return;
	}
	// License/entitlement DBs by path fragment or basename: the entitlement
	// daemon stats/opens these before any CREATE (mold paths like
	// /mnt/ntb/... carry no priv/license fragment). Without a seed they stay
	// ENOENT and its CSI aborts. Everything else keeps exact semantics.
	const bool licensePath = guest.find("priv/license") != std::string::npos;
	auto basename = guest.substr(guest.find_last_of('/') + 1);
	const bool licenseBase = basename == "entitlement4.db" || basename == "subscription.db" ||
	                         basename == "entitlement.db";
	if (!licensePath && !licenseBase) {
		return;
	}
	if (guest.size() < 4 || guest.compare(guest.size() - 3, 3, ".db") != 0) {
		return;
	}
	const auto backing = BackingForUnmounted(guest, false, captured_data_root);
	if (backing.empty()) {
		return;
	}
	std::error_code ec;
	if (std::filesystem::exists(backing, ec) && std::filesystem::file_size(backing, ec) > 0) {
		return;
	}
	std::filesystem::create_directories(backing.parent_path(), ec);
	if (ec) {
		return;
	}
	// Seed a valid empty sqlite DB so existence/size/header checks pass;
	// queries then see zero rows instead of ENOENT.
	static const char* kTemplate = "_TempData/empty.db";
	std::filesystem::copy_file(kTemplate, backing,
	                           std::filesystem::copy_options::overwrite_existing, ec);
	if (!ec) {
		LOGF("\t license seed: %s -> %s\n", guest.c_str(),
		     backing.generic_string().c_str());
	}
}

namespace FsInternalForVsh {

LIB_VERSION("FsInternalForVsh", 1, "FsInternalForVsh", 1, 1);

namespace FsVsh {

static int M2MetadataUnsupported() {
	const int fd = LibKernel::FileSystem::KernelOpen("/dev/m2.ctl", 0, 0);
	if (fd < 0) { return fd; }
	// Opening the controller does not imply attached media or verification.
	LibKernel::FileSystem::KernelClose(fd);
	return LibKernel::KERNEL_ERROR_EOPNOTSUPP;
}
// Retail libSceFsInternalForVsh +47b50 and retail kernel file +9c00f0.
// The absent-device ioctl returns ENODEV without writing any output buffers.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI M2StorageGetDeviceInfo(
    void* info, void* hash, void* controller, void* reserved, size_t reserved_size) {
	if (!info || reserved || reserved_size) { return LibKernel::KERNEL_ERROR_EINVAL; }
	const int fd = LibKernel::FileSystem::KernelOpen("/dev/m2.ctl", 0, 0);
	if (fd < 0) { return fd; }
	uint64_t envelope[] = {reinterpret_cast<uint64_t>(info), reinterpret_cast<uint64_t>(hash),
	                       reinterpret_cast<uint64_t>(controller), 0, 0x7800000001ULL};
	int result = LibKernel::KERNEL_ERROR_EOPNOTSUPP;
	LibKernel::FileSystem::TryM2Ioctl(fd, 0x80283208u, envelope, &result);
	LibKernel::FileSystem::KernelClose(fd);
	return result;
}

// Retail +45c20 validates flags; +45c60 opens the same controller before
// either verification ioctl. Metadata operations are not implemented.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI M2StorageVerifyMetadata(uint32_t flags, const void*) {
	if (flags >= 2) { return LibKernel::KERNEL_ERROR_EINVAL; }
	return M2MetadataUnsupported();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MountSaveData(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return Detail::MountBacking("sceFsMountSaveData",
	                            Detail::PickPoint(a1, a2, "/savedata0"), "savedata");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UmountSaveData(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	const auto point = Detail::PickPoint(a1, a2, "/savedata0");
	LibKernel::FileSystem::Umount(point); // no-op when absent; always success
	LOGF("\t sceFsUmountSaveData: %s\n", point.c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitMountSaveDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                              uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitMountSaveDataOpt: defaults already zeroed by guest -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitUmountSaveDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitUmountSaveDataOpt: defaults already zeroed by guest -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitCreatePfsSaveDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitCreatePfsSaveDataOpt: defaults already zeroed by guest -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreatePfsSaveDataImage(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	// No real PFS: the image IS the skeleton dir; ensure it exists + succeed.
	const auto title = Detail::SafeGuestStr(a1);
	LOGF("\t sceFsCreatePfsSaveDataImage: title = %s\n",
	     title.empty() ? "(default)" : title.c_str());
	(void)a2;
	(void)a3;
	(void)a4;
	std::error_code ec;
	std::filesystem::create_directories(Detail::SandboxBacking("savedata"), ec);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MountTrophyData(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return Detail::MountBacking("sceFsMountTrophyData",
	                            Detail::PickPoint(a1, a2, "/trophy0"), "trophy");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UmountTrophyData(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	const auto point = Detail::PickPoint(a1, a2, "/trophy0");
	LibKernel::FileSystem::Umount(point);
	LOGF("\t sceFsUmountTrophyData: %s\n", point.c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitMountTrophyDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitMountTrophyDataOpt -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitUmountTrophyDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitUmountTrophyDataOpt -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitCreatePfsTrophyDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitCreatePfsTrophyDataOpt -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreatePfsTrophyDataImage(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	std::error_code ec;
	std::filesystem::create_directories(Detail::SandboxBacking("trophy"), ec);
	LOGF("\t sceFsCreatePfsTrophyDataImage -> 0 (skeleton)\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MountDownloadData(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	// /download0 is pre-mounted by the boot; remounting the same backing is
	// idempotent. Prefer the guest point when it names another sandbox slot.
	return Detail::MountBacking("sceFsMountDownloadData",
	                            Detail::PickPoint(a1, a2, "/download0"), "download");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UmountDownloadData(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	const auto point = Detail::PickPoint(a1, a2, "/download0");
	// Never tear down the boot's /download0 jail dir itself: only release
	// guest-requested extra points; the base point remount is a no-op anyway.
	if (point != "/download0") {
		LibKernel::FileSystem::Umount(point);
	}
	LOGF("\t sceFsUmountDownloadData: %s\n", point.c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitMountDownloadDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitMountDownloadDataOpt -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI InitUmountDownloadDataOpt(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceFsInitUmountDownloadDataOpt -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI M2StorageLocalSandboxMount(uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return Detail::MountBacking("sceFsM2StorageLocalSandboxMount",
	                            Detail::PickPoint(a1, a2, "/m2sandbox0"), "m2sandbox");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI M2StorageLocalSandboxUmount(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	const auto point = Detail::PickPoint(a1, a2, "/m2sandbox0");
	LibKernel::FileSystem::Umount(point);
	LOGF("\t sceFsM2StorageLocalSandboxUnmount: %s\n", point.c_str());
	return OK;
}

} // namespace FsVsh

void InitFsInternalForVshSandbox_1(Loader::SymbolDatabase* s);

} // namespace FsInternalForVsh

namespace SysCore {

LIB_VERSION("SysCore", 1, "SysCore", 1, 1);

namespace Proc {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsPrimaryProcess(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceApplicationIsPrimaryProcess -> 1 (true)\n");
	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetProcs(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	// Layout assumed (pid_list, max, count_out): publish a single primary
	// entry through validated out-params only; never touch guest memory blind.
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	if (a1 != 0) {
		if (!Detail::WriteGuest<uint32_t>(a1, 0u) &&
		    !Detail::WriteGuest<uint64_t>(a1, 0u)) {
			LOGF("\t sceApplicationGetProcs: pid list not writable, count only\n");
		}
	}
	for (uint64_t out : {a3, a2, a4}) {
		if (out != 0 && (Detail::WriteGuest<uint32_t>(out, 1u) ||
		                  Detail::WriteGuest<uint64_t>(out, 1u))) {
			break;
		}
	}
	LOGF("\t sceApplicationGetProcs -> single primary entry\n");
	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BlockingKill3(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t sceApplicationBlockingKill3(pid=%llu, status=%llu) -> 0 NO-OP\n",
	     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2));
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RaiseException(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t sceApplicationRaiseException(pid=%llu, ex=%llu) -> 0 NO-OP\n",
	     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2));
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RaiseExceptionToLocalPid(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t sceApplicationRaiseExceptionToLocalPid -> 0 NO-OP\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RaiseExceptionToPid(uint64_t a1, uint64_t a2, uint64_t a3,
                                              uint64_t a4) {
	PRINT_NAME();
	LOGF("\t sceApplicationRaiseExceptionToPid -> 0 NO-OP\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetCoredumpState(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	if (a1 != 0) {
		Detail::WriteGuest<uint32_t>(a1, 0u); // 0 = no coredump in progress
	}
	LOGF("\t sceApplicationGetCoredumpState -> 0 (idle)\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetCoredumpDirAndFileName(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceApplicationGetCoredumpDirAndFileName -> 0 (no coredump)\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetAppInfoByAppId(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t sceApplicationGetAppInfoByAppId(appId=%llu) -> 0\n",
	     static_cast<unsigned long long>(a1));
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ContinueApp(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceApplicationContinue -> 0 NO-OP\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NotifyVshReady(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceApplicationNotifyVshReady -> 0\n");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SignalShellCoreHeartBeat(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t sceApplicationSignalShellCoreHeartBeat -> 0\n");
	return OK;
}

} // namespace Proc

void InitSysCoreSandbox_1(Loader::SymbolDatabase* s);

} // namespace SysCore

namespace LncService {

LIB_VERSION("LncService", 1, "LncService", 1, 1);

namespace Sbox {

// Nullfs-style alias: when the guest names an already-mounted source path
// (/system/priv, /webkit content, becore image), bind its host dir at the new
// point; otherwise fall back to the writable skeleton. Unknown arg order, so
// both string args are probed and classified by mount state.
static int MountAlias(const char* op, uint64_t a1, uint64_t a2, const char* family) {
	const auto s1 = Detail::SafeGuestStr(a1);
	const auto s2 = Detail::SafeGuestStr(a2);
	LOGF("\t %s: s1 = %s s2 = %s\n", op, s1.empty() ? "(non-str)" : s1.c_str(),
	     s2.empty() ? "(non-str)" : s2.c_str());
	std::string src, point;
	for (const auto& s : {s1, s2}) {
		if (!Detail::IsMountPoint(s)) {
			continue;
		}
		if (!LibKernel::FileSystem::GetRealFilename(s).empty() && src.empty()) {
			src = s; // already-mounted guest path -> alias source
		} else if (point.empty()) {
			point = s;
		}
	}
	if (point.empty()) {
		point = "/sandbox0";
	}
	if (!src.empty()) {
		const auto real = LibKernel::FileSystem::GetRealFilename(src);
		LibKernel::FileSystem::Mount(real, point);
		LOGF("\t %s: %s -> %s (alias of %s)\n", op, point.c_str(),
		     real.generic_string().c_str(), src.c_str());
	} else {
		Detail::MountBacking(op, point, family);
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MountToFsSandbox(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	return MountAlias("sceLncServiceMountToFsSandbox", a1, a2, "lnc");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MountToFsSandboxByTitleId(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	const auto title = Detail::SafeGuestStr(a1);
	LOGF("\t title = %s\n", title.empty() ? "(non-str)" : title.c_str());
	return MountAlias("sceLncServiceMountToFsSandboxByTitleId", a2, a3, "lnc");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnmountFromFsSandbox(uint64_t a1, uint64_t a2, uint64_t a3,
                                               uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	const auto point = Detail::PickPoint(a1, a2, "/sandbox0");
	LibKernel::FileSystem::Umount(point);
	LOGF("\t sceLncServiceUnmountFromFsSandbox: %s\n", point.c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnmountFromFsSandboxByTitleId(uint64_t a1, uint64_t a2,
                                                        uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	const auto title = Detail::SafeGuestStr(a1);
	const auto point = Detail::PickPoint(a2, a3, "/sandbox0");
	LibKernel::FileSystem::Umount(point);
	LOGF("\t sceLncServiceUnmountFromFsSandboxByTitleId: %s title = %s\n", point.c_str(),
	     title.empty() ? "(non-str)" : title.c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ForceUnmountFromFsSandbox(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	(void)a3;
	(void)a4;
	const auto point = Detail::PickPoint(a1, a2, "/sandbox0");
	LibKernel::FileSystem::Umount(point);
	LOGF("\t sceLncServiceForceUnmountFromFsSandbox: %s\n", point.c_str());
	return OK;
}

} // namespace Sbox

void InitLncServiceSandbox_1(Loader::SymbolDatabase* s);

} // namespace LncService

void FsInternalForVsh::InitFsInternalForVshSandbox_1(Loader::SymbolDatabase* s) {
	LIB_FUNC("NoXU3Apuy4I", FsVsh::M2StorageGetDeviceInfo); // sceFsM2StorageGetDeviceInfo
	LIB_FUNC("V6i6yMSwqo4", FsVsh::M2StorageVerifyMetadata); // sceFsM2StorageVerifyMetadata
	LIB_FUNC("c7YjLnWxw-4", FsVsh::MountSaveData);          // sceFsMountSaveData
	LIB_FUNC("Ck7qGp2MjI4", FsVsh::UmountSaveData);          // sceFsUmountSaveData
	LIB_FUNC("+mPswO7rxKs", FsVsh::InitMountSaveDataOpt);    // sceFsInitMountSaveDataOpt
	LIB_FUNC("udWWfVZ5cPg", FsVsh::InitUmountSaveDataOpt);   // sceFsInitUmountSaveDataOpt
	LIB_FUNC("ccVvHF4wZME", FsVsh::InitCreatePfsSaveDataOpt); // sceFsInitCreatePfsSaveDataOpt
	LIB_FUNC("oZf2EW1xS70", FsVsh::CreatePfsSaveDataImage);  // sceFsCreatePfsSaveDataImage
	LIB_FUNC("k0Kog-TlHYg", FsVsh::MountTrophyData);         // sceFsMountTrophyData
	LIB_FUNC("yXefJHaH0Kc", FsVsh::UmountTrophyData);         // sceFsUmountTrophyData
	LIB_FUNC("NoxZswTJHP0", FsVsh::InitMountTrophyDataOpt);  // sceFsInitMountTrophyDataOpt
	LIB_FUNC("bA31MLNLtWQ", FsVsh::InitUmountTrophyDataOpt); // sceFsInitUmountTrophyDataOpt
	LIB_FUNC("5RQCvNleU3I", FsVsh::InitCreatePfsTrophyDataOpt); // sceFsInitCreatePfsTrophyDataOpt
	LIB_FUNC("fU5fcZt5QEg", FsVsh::CreatePfsTrophyDataImage); // sceFsCreatePfsTrophyDataImage
	LIB_FUNC("bb5sduKHOTQ", FsVsh::MountDownloadData);       // sceFsMountDownloadData
	LIB_FUNC("rTu7zaYoryI", FsVsh::UmountDownloadData);       // sceFsUmountDownloadData
	LIB_FUNC("AoSlid9sAJE", FsVsh::InitMountDownloadDataOpt); // sceFsInitMountDownloadDataOpt
	LIB_FUNC("Qnu2x6d29lo", FsVsh::InitUmountDownloadDataOpt); // sceFsInitUmountDownloadDataOpt
	LIB_FUNC("eQDoY-eORKw", FsVsh::M2StorageLocalSandboxMount); // sceFsM2StorageLocalSandboxMount
	LIB_FUNC("84wJYcea6Ck", FsVsh::M2StorageLocalSandboxUmount); // sceFsM2StorageLocalSandboxUnmount
}

void SysCore::InitSysCoreSandbox_1(Loader::SymbolDatabase* s) {
	LIB_FUNC("UWQ3JZbb5bM", Proc::IsPrimaryProcess);      // sceApplicationIsPrimaryProcess -> 1
	LIB_FUNC("GdIm4Jm2mm8", Proc::GetProcs);              // sceApplicationGetProcs -> 1 entry
	LIB_FUNC("y55QWr8yzrY", Proc::BlockingKill3);         // sceApplicationBlockingKill3 NO-OP
	LIB_FUNC("IgqBZdkIvqM", Proc::RaiseException);        // sceApplicationRaiseException NO-OP
	LIB_FUNC("++DyHhMUeQA", Proc::RaiseExceptionToLocalPid); // sceApplicationRaiseExceptionToLocalPid
	LIB_FUNC("fQZV+2VLPWI", Proc::RaiseExceptionToPid);   // sceApplicationRaiseExceptionToPid
	LIB_FUNC("DfD47ZODK8Y", Proc::GetCoredumpState);      // sceApplicationGetCoredumpState
	LIB_FUNC("f+HdRtISAvw", Proc::GetCoredumpDirAndFileName); // sceApplicationGetCoredumpDirAndFileName
	LIB_FUNC("kMaBFIQNsSI", Proc::GetAppInfoByAppId);     // sceApplicationGetAppInfoByAppId
	LIB_FUNC("+7qLP7HjL+4", Proc::ContinueApp);           // sceApplicationContinue
	LIB_FUNC("dqwCM7RWifY", Proc::NotifyVshReady);        // sceApplicationNotifyVshReady
	LIB_FUNC("MW+T6ZCK7wo", Proc::SignalShellCoreHeartBeat); // sceApplicationSignalShellCoreHeartBeat
}

void LncService::InitLncServiceSandbox_1(Loader::SymbolDatabase* s) {
	LIB_FUNC("dZed2LcbR3Y", Sbox::MountToFsSandbox);            // sceLncServiceMountToFsSandbox
	LIB_FUNC("qBDiQoW03rg", Sbox::MountToFsSandboxByTitleId);   // sceLncServiceMountToFsSandboxByTitleId
	LIB_FUNC("GBZNk5MVT-k", Sbox::UnmountFromFsSandbox);         // sceLncServiceUnmountFromFsSandbox
	LIB_FUNC("a3c6x8b9TEA", Sbox::UnmountFromFsSandboxByTitleId); // sceLncServiceUnmountFromFsSandboxByTitleId
	LIB_FUNC("Zry4CQEzpsI", Sbox::ForceUnmountFromFsSandbox);   // sceLncServiceForceUnmountFromFsSandbox
}

LIB_DEFINE(InitFsSandbox_1) {
	Detail::EnsureShellSandbox(); // pre-mount before ShellCore's stat storm
	FsInternalForVsh::InitFsInternalForVshSandbox_1(s);
	SysCore::InitSysCoreSandbox_1(s);
	LncService::InitLncServiceSandbox_1(s);
}

} // namespace FsSandbox

} // namespace Libs

/*
Registration snippet for src/libs/libs.cpp (orchestrator applies; this file
itself is auto-compiled via the src-libs GLOB):

namespace FsSandbox {
LIB_DEFINE(InitFsSandbox_1);
} // namespace FsSandbox

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   FsSandbox::InitFsSandbox_1(s);
*/
