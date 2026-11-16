#include "common/abi.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/libs.h"
#include "loader/childLaunch.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Libs {

namespace LncUtil {

LIB_VERSION("LncUtil", 1, "LncUtil", 1, 1);

namespace Lnc {

int KYTY_SYSV_ABI sceLncUtilGetSpmemAllocatorConfigValue(const char* key) {
	// Firmware 13.60 SystemService: export 0x5910 -> 0x69e0. One key
	// argument; searches 16 records (32-byte name + byte value), returning
	// zero for null/missing keys. No allocator overrides are configured here.
	(void)key;
	return 0;
}

struct AppEntry {
	std::string title_id;
	int         status = 0; // 0 = running
	// ps5-vsh: ShellCore-driven BigApp budget flow (NPXS40038 strings:
	// `BigAppGame suspending`, `EnsureBigAppBudget`, `lock/unlockBigApp`,
	// `acquire/releaseCpuBudget*`).
	bool suspended        = false;
	bool bigapp_locked    = false;
	int  bigapp_budget_mb = 0;
	int  cpu_budget_pct   = 0; // 0-100
	AppStatus app_status {};
	PendingLaunch launch;
	enum class Phase { Requested, Accepted, Running, Failed, Exited };
	Phase phase = Phase::Running;
	bool runtime_managed = false;
	int32_t launch_error = 0;
	int32_t app_local_pid = -1;
	std::string guest_path;
	std::string guest_sce_sys_dir;
	int32_t owner_program_id = -1;
	uint64_t owner_lifetime = 0;
	Loader::RuntimeChild::Key process_token {};
	enum class Close { None, Dispatching, Requested };
	Close close = Close::None;
	bool quiescent_exit_pending = false;
	int32_t quiescent_exit_result = 0;
};

// ps5-vsh: `vcn[%3d]` / `Disc Player vcn budget` /
// `beChangeVcnResource` / `ChangeVcnResource` (NPXS40038).
struct VcnBudget {
	int         vcn = -1;
	std::string owner;
	int         budgetMb = 0;
	bool        locked   = false;
};

// ps5-vsh: `Disc Player vcn budget` fallback (MB) when no budget is given.
constexpr int VCN_BUDGET_MB_DISC_PLAYER = 512;
static uint32_t                                 g_next_app_id = 100;
static std::unordered_map<std::string, uint32_t> g_by_title;
static std::unordered_map<uint32_t, AppEntry>    g_by_app_id;
static std::mutex                                g_app_mutex;
// Program IDs are monotonic loader identities, unlike the shared HLE PID.
static std::unordered_map<int32_t, uint32_t>       g_by_program;
static bool                                      g_vsh_runtime = false;
static int32_t                                   g_focused_app = -1;
static int32_t                                   g_controller_app = -1;
// One lock orders state changes and owned event snapshots together.
static std::deque<EventWire>                     g_shell_events;

static const Loader::Program* ApplicationOwner(const Loader::Program* program) {
	return program != nullptr && program->application_owner != nullptr ? program->application_owner : program;
}

template <size_t N>
static void CopyStatusString(char (&out)[N], const std::string& value) {
	// An overlong identity/version is unavailable, not a truncated identity.
	if (value.size() < N) {
		std::memcpy(out, value.c_str(), value.size() + 1);
	}
}

void RegisterProgramLaunch(const Loader::Program* program, const std::filesystem::path& param_json,
                           bool system_ex, uint32_t requested_app_id,
                           const std::filesystem::path& firmware_root) {
	if (program == nullptr) {
		return;
	}
	program = ApplicationOwner(program);
	AppStatus status {};
	std::string title;
	std::ifstream input(param_json, std::ios::binary);
	if (input) {
		const auto json = nlohmann::json::parse(input, nullptr, false);
		if (json.is_object()) {
			const auto string_value = [&](const char* key) -> std::string {
				const auto it = json.find(key);
				return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
			};
			title = string_value("titleId");
			CopyStatusString(status.app_ver, string_value("masterVersion"));
			CopyStatusString(status.content_version, string_value("contentVersion"));
			const auto category = json.find("applicationCategoryType");
			if (category != json.end() && category->is_number_integer()) {
				const auto value = category->get<int64_t>();
				if (value >= 0 && value <= UINT32_MAX) {
					status.app_category_type = static_cast<uint32_t>(value);
				}
			}
		}
	}
	// A generic staging directory (notably "sys") is not an application ID.
	// Only a title-shaped executable directory is a usable metadata fallback.
	if (title.empty() && program->file_name.filename() == "eboot.bin") {
		const auto parent = program->file_name.parent_path().filename().string();
		if (parent.size() == 9 && parent.starts_with("NPXS") &&
		    parent.find_first_not_of("0123456789", 4) == std::string::npos) {
			title = parent;
		}
	}
	if (title.size() >= sizeof(status.title_id) || title == "sys") {
		title.clear();
	}
	CopyStatusString(status.title_id, title);
	// Known firmware executable identities, not a category-to-type guess.
	// NPXS40087 contains the ShellUI AppMain/PSM application in FW 13.60.
	if (program->file_name.filename() == "SceShellCore.elf") {
		status.app_type = 6;
	} else if (program->file_name.filename() == "eboot.bin" && title == "NPXS40087") {
		status.app_type = 1;
	}
	// Staged executables retain the installation provenance of their title.
	// Consult the actual firmware installation, not an NPXS => system_ex rule.
	if (!system_ex && !firmware_root.empty() && title.size() == 9 && title.starts_with("NPXS") &&
	    title.find_first_not_of("0123456789", 4) == std::string::npos) {
		std::error_code ec;
		system_ex = std::filesystem::is_regular_file(
		    firmware_root / "system_ex_b" / "app" / title / "eboot.bin", ec);
	}
	status.is_system_ex_app = system_ex ? 1 : 0;
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	if (g_by_program.contains(program->unique_id)) {
		return; // Do not replace identity after guest caches may have observed it.
	}
	uint32_t app_id = 0;
	const auto pending = g_by_app_id.find(requested_app_id);
	if (requested_app_id != 0 && pending != g_by_app_id.end() &&
	    (title.empty() || pending->second.title_id == title)) {
		if (pending->second.process_token.Valid()) { return; } // External process has no in-parent Program owner.
		if (pending->second.runtime_managed && pending->second.owner_program_id != -1 &&
		    pending->second.owner_program_id != program->unique_id) { return; }
		app_id = requested_app_id;
		status.launch_request_app_id = pending->second.app_status.launch_request_app_id;
		status.app_attr = pending->second.app_status.app_attr;
		if (title.empty()) {
			title = pending->second.title_id;
			CopyStatusString(status.title_id, title);
		}
	} else {
		app_id = g_next_app_id++;
	}
	if (g_vsh_runtime && program->application_owner != nullptr) {
		status.app_type = 5; // Runtime application, not an arbitrary loaded module.
	}
	status.app_id = static_cast<int32_t>(app_id);
	auto& entry = g_by_app_id[app_id];
	entry.title_id = title;
	entry.app_status = status;
	entry.owner_program_id = program->unique_id;
	const auto execution = Loader::GetApplicationExecution(program);
	entry.owner_lifetime = execution ? execution->Owner().token : 0;
	g_by_program.emplace(program->unique_id, app_id);
	if (!title.empty()) {
		g_by_title[title] = app_id;
	}
	LOGF("Lnc register program=%d appId=%u title=%s type=%d category=0x%08x systemEx=%u\n",
	     program->unique_id, app_id, title.c_str(), status.app_type, status.app_category_type,
	     static_cast<unsigned>(status.is_system_ex_app));
}

static std::map<int, VcnBudget> g_vcn_table;
static std::mutex               g_vcn_mutex;
static int                      g_current_vcn = -1;

static const char* SafeStr(const char* s) {
	return s != nullptr ? s : "(null)";
}

// ps5-vsh: -------------------------------------------------------------------
// 13.60 LNC launch-control event receive (sceLncUtilGetEventForShellUI).
//
// `Sce.Vsh.LncUtilWrapper.EventMonitor` (13.60) drives the ShellUI's per-frame
// LNC FrameTask:
//
//   Update(): if (BlockUpdate || UpdateEvent() != 0) return;
//             Event e = default; GetEvent(ref e);
//             switch (e.eventType) { ...15 known types...; default: throw new InvalidOperationException(); }
//
// `UpdateEvent`/`GetEvent` are the guest `LncProxy` entry points; the proxy
// forwards the receive to libSceLncUtil's sceLncUtilGetEventForShellUI
// (NID ZucoOmNsb7w). A zero result *promises* that GetEvent will hand back one
// complete record, so while nothing is queued the nonblocking receive must
// report "empty" (EAGAIN) instead of success. This tree left the NID
// unresolved, the loader patched eboot.bin's PLT slot to a zero-returning stub,
// EventMonitor dispatched eventType == Invalid(0) into its `default:` arm, and
// the resulting InvalidOperationException escaped FrameTaskManager.Update ->
// UISystem.Update -> Application.Run and killed the process with a crash dump
// (nothing after that frame ever runs, so the UI/job-queue/scene pumps stop).
//
// Positive control: the working 4.03 reference resolves the very same NID
// (compare403.trace.log: "PS5 NID fallback: ZucoOmNsb7w -> ...[LncUtil_v1][LncUtil_v1.1]")
// and never logs an InvalidOperationException.
// ---------------------------------------------------------------------------

static EventWire Snapshot(const AppEntry& entry) {
	EventWire event {};
	const auto& status = entry.app_status;
	event.app_id = status.app_id;
	event.app_type = status.app_type;
	event.app_attribute = status.app_attr;
	event.launch_request_app_id = status.launch_request_app_id;
	CopyStatusString(event.title_id, entry.title_id);
	event.user_id = entry.launch.user_id;
	event.check_flags = entry.launch.param.check_flag;
	event.context_id = entry.launch.param.context_id;
	event.app_local_pid = entry.app_local_pid;
	event.app_category_type = status.app_category_type;
	event.app_boot_mode = status.app_boot_mode;
	event.app_boot_sub_mode = status.app_boot_sub_mode;
	for (const auto& arg: entry.launch.args) {
		const auto size = arg.size() + 1;
		if (size > sizeof(event.arguments) - static_cast<size_t>(event.argument_size)) { break; }
		std::memcpy(event.arguments + event.argument_size, arg.c_str(), size);
		event.argument_size += static_cast<int32_t>(size);
	}
	CopyStatusString(event.path, entry.guest_path);
	CopyStatusString(event.sce_sys_dir, entry.guest_sce_sys_dir);
	return event;
}

static void QueueLaunchStart(const AppEntry& entry) {
	auto event = Snapshot(entry);
	event.event_type = 1;
	// Verified in 13.60 Legacy::ApplicationCoordinator::AllocateApp +0xdc90:
	// EventArgs.AppId at +0x20 == -1 opens the container without Start (+0xb3a0).
	// OnAppLaunchStart +0xdf20 accepts nonnegative error_code, not constructor -1.
	event.app_id = event.app_local_pid = -1;
	g_shell_events.push_back(event);
}

// Per-frame receive, so the log is rate-limited: the first few calls (and every
// 600th afterwards) emit a line. A silent log afterwards is meaningful only
// because at least one "receive" line is guaranteed to appear first.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetEventForShellUI(void* event) {
	static std::atomic<uint32_t> calls {0};
	const uint32_t               n = calls.fetch_add(1, std::memory_order_relaxed);
	if (event == nullptr) {
		LOGF("\tsceLncUtilGetEventForShellUI: null output buffer -> 0x80940005\n");
		return (int)0x80940005;
	}
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	if (g_shell_events.empty()) {
		if (n < 4u || (n % 600u) == 0u) {
			LOGF("\tsceLncUtilGetEventForShellUI: #%u empty -> EAGAIN (0x%08x)\n", n,
			     (unsigned)LibKernel::KERNEL_ERROR_EAGAIN);
		}
		return LibKernel::KERNEL_ERROR_EAGAIN;
	}
	const EventWire& front = g_shell_events.front();
	LOGF("\tsceLncUtilGetEventForShellUI: #%u deliver type=%d app=%d pid=%d title=%s app_type=%d error=0x%08x existing=%d\n", n,
	     front.event_type, front.app_id, front.app_local_pid, front.title_id, front.app_type,
	     static_cast<unsigned>(front.error_code), front.existing_app_id);
	std::memcpy(event, &front, sizeof(EventWire));
	g_shell_events.pop_front();
	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// ps5-vsh: guest argv is identity-mapped; a stale pointer must never take
// the emulator down. Page-walk probe (mirrors libShellHits.cpp ProbeGuest).
static bool ProbeGuest(uint64_t addr, size_t len) {
	if (addr == 0 || len == 0) {
		return false;
	}
	const uint64_t end = addr + len;
	if (end < addr) {
		return false;
	}
	for (uint64_t p = addr; p < end;) {
		MEMORY_BASIC_INFORMATION mi {};
		if (VirtualQuery(reinterpret_cast<const void*>(p), &mi, sizeof(mi)) == 0 ||
		    mi.State != MEM_COMMIT) {
			return false;
		}
		const DWORD prot = mi.Protect & 0xffu;
		if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
		      prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
		      prot == PAGE_EXECUTE_WRITECOPY)) {
			return false;
		}
		const uint64_t region_end =
		    reinterpret_cast<uint64_t>(mi.BaseAddress) + mi.RegionSize;
		if (region_end <= p) {
			return false;
		}
		p = region_end;
	}
	return true;
}
#else
static bool ProbeGuest(uint64_t addr, size_t len) {
	return addr != 0 && len != 0;
}
#endif

// Bounded, null-terminated guest argv read for sceLncUtilLaunchApp.
static bool ReadGuestArgv(const char* const* argv, std::vector<std::string>* args) {
	if (argv == nullptr) { return true; }
	// Native libSceSystemService +0x76ef walks until a null pointer and
	// rejects more than 0x1000 bytes including string terminators (+0x7725).
	size_t total = 0;
	for (size_t i = 0; i <= 4096; ++i) {
		if (!ProbeGuest(reinterpret_cast<uint64_t>(argv + i), sizeof(*argv))) { return false; }
		const auto* value = argv[i];
		if (value == nullptr) { return true; }
		size_t length = 0;
		for (;;) {
			if (total == 4096 || !ProbeGuest(reinterpret_cast<uint64_t>(value + length), 1)) { return false; }
			++total;
			if (value[length] == '\0') { break; }
			++length;
		}
		args->emplace_back(value, length);
	}
	return false;
}

// ps5-vsh: ShellCore-driven child re-entry queue (Lnc half of spawnApp).
// sceLncUtilLaunchApp enqueues here; emulator.cpp drains via TakePendingLaunch
// -> Loader::RemountApp0 -> re-enter with Loader::BuildChildArgv-style args.
// NOTE: lib has no fw_root, so app0_dir is recorded EMPTY here and the
// consumer resolves it at consume time via Loader::ProbeChildTitle over
// <fw_root>/system_b/vsh/app/<T> then system_ex_b/app/<T> (NPXS system titles)
// plus Loader::IsLaunchableTitle check. Guest argv IS forwarded (bounded +
// probed reads in sceLncUtilLaunchApp).
static std::deque<PendingLaunch> g_pending;
static std::mutex                g_pending_mutex;
static uint64_t                  g_pending_seq = 1;
static std::function<int(const PendingLaunch&)> g_runtime_launch;
static std::function<int(const RuntimeCloseRequest&)> g_runtime_close;
static std::function<int(const RuntimeFocusRequest&)> g_runtime_focus;
static uint64_t g_focus_epoch = 0;
static std::mutex g_runtime_focus_mutex; // HOST transition serialization, never g_app_mutex.

void SetRuntimeLaunchHandler(std::function<int(const PendingLaunch&)> handler) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_pending_mutex);
	g_runtime_launch = std::move(handler);
}

void SetRuntimeCloseHandler(std::function<int(const RuntimeCloseRequest&)> handler) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_pending_mutex);
	g_runtime_close = std::move(handler);
}

void SetRuntimeFocusHandler(std::function<int(const RuntimeFocusRequest&)> handler) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_pending_mutex); g_runtime_focus = std::move(handler);
}

bool ConfigureStandaloneRuntimeIdentity(uint32_t app_id) {
	if (!app_id || app_id > INT32_MAX) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	if (g_vsh_runtime || !g_by_app_id.empty() || !g_by_program.empty()) { return false; }
	g_next_app_id = app_id; return true;
}

bool RegisterRuntimeProcess(const Loader::RuntimeChild::Key& token, uint32_t category) {
	if (!token.Valid()) { return false; }
	Common::HostLockCriticalGuard critical; std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(token.app);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed ||
	    it->second.phase != AppEntry::Phase::Requested || it->second.launch.seq != token.sequence ||
	    it->second.owner_program_id != -1 || it->second.process_token.Valid()) { return false; }
	it->second.process_token = token; it->second.app_status.app_category_type = category; return true;
}

bool RuntimeProcessStarted(const Loader::RuntimeChild::Key& token, int32_t child_guest_pid) {
	if (!token.Valid() || child_guest_pid <= 0) { return false; }
	Common::HostLockCriticalGuard critical; std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(token.app);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed || it->second.process_token != token || it->second.phase != AppEntry::Phase::Accepted ||
	    it->second.close != AppEntry::Close::None) { return false; }
	auto& entry = it->second; entry.phase = AppEntry::Phase::Running; entry.status = 0; entry.app_local_pid = child_guest_pid;
	auto event = Snapshot(entry); event.event_type = 2; g_shell_events.push_back(event); return true;
}

void ConfigureVshRuntime(bool enabled) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	if (g_vsh_runtime == enabled) { return; }
	g_vsh_runtime = enabled;
	g_shell_events.clear();
	g_focused_app = g_controller_app = -1;
}

bool RegisterRuntimeLaunchPath(uint32_t app_id, const std::string& guest_executable_path,
                               const std::string& guest_sce_sys_dir) {
	if (guest_executable_path.empty() || guest_executable_path.front() != '/' || guest_executable_path.size() >= 1024 ||
	    guest_sce_sys_dir.empty() || guest_sce_sys_dir.front() != '/' || guest_sce_sys_dir.size() >= 256 ||
	    guest_executable_path.find('\0') != std::string::npos || guest_sce_sys_dir.find('\0') != std::string::npos) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(app_id);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed ||
	    it->second.phase != AppEntry::Phase::Requested || !it->second.guest_path.empty()) { return false; }
	it->second.guest_path = guest_executable_path;
	it->second.guest_sce_sys_dir = guest_sce_sys_dir;
	it->second.app_status.app_type = 5;
	return true;
}

bool RuntimeLaunchAccepted(uint32_t app_id) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(app_id);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed ||
	    it->second.phase != AppEntry::Phase::Requested) { return false; }
	it->second.phase = AppEntry::Phase::Accepted;
	QueueLaunchStart(it->second);
	return true;
}

bool RuntimeLaunchStarted(uint32_t app_id, int32_t app_local_pid) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(app_id);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed ||
	    it->second.phase != AppEntry::Phase::Accepted || it->second.process_token.Valid() || it->second.close == AppEntry::Close::Requested) { return false; }
	auto& entry = it->second;
	entry.phase = AppEntry::Phase::Running;
	entry.status = 0;
	entry.app_local_pid = app_local_pid;
	auto event = Snapshot(entry);
	event.event_type = 2;
	g_shell_events.push_back(event);
	return true;
}

static void FailRuntimeLaunch(AppEntry& entry, int32_t error_code, int32_t existing_app_id = -1) {
	if (entry.phase == AppEntry::Phase::Requested) { QueueLaunchStart(entry); }
	entry.phase = AppEntry::Phase::Failed;
	entry.status = 2;
	entry.launch_error = error_code;
	auto event = Snapshot(entry);
	event.event_type = 2;
	event.app_id = event.app_local_pid = -1;
	event.error_code = error_code;
	event.existing_app_id = existing_app_id;
	g_shell_events.push_back(event);
}

int RuntimeLaunchConflict(uint32_t app_id, const Loader::RuntimeChild::Key& existing) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto next = g_by_app_id.find(app_id), current = g_by_app_id.find(existing.app);
	if (!g_vsh_runtime || !existing.Valid() || app_id == existing.app || next == g_by_app_id.end() || current == g_by_app_id.end() ||
	    !next->second.runtime_managed || next->second.phase != AppEntry::Phase::Requested || next->second.process_token.Valid() ||
	    next->second.app_status.app_type != 5 || !current->second.runtime_managed || current->second.process_token != existing ||
	    current->second.phase != AppEntry::Phase::Running || current->second.close != AppEntry::Close::None) {
		return LibKernel::KERNEL_ERROR_EBUSY;
	}
	// 13.60 ReactNativeApps AutoLaunch +17520 jump table: 80940010
	// invokes KillAppAsync(ExistingAppId), then retries; 8094000c exits
	// the launch flow while AppSystem selects the already-running app.
	const int result = static_cast<int>(next->second.title_id == current->second.title_id ? 0x8094000cu : 0x80940010u);
	FailRuntimeLaunch(next->second, result, static_cast<int32_t>(existing.app));
	return result;
}

bool RuntimeLaunchFailed(uint32_t app_id, int32_t error_code) {
	if (error_code >= 0) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(app_id);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed || it->second.process_token.Valid()) { return false; }
	auto& entry = it->second;
	if (entry.phase != AppEntry::Phase::Requested && entry.phase != AppEntry::Phase::Accepted) { return false; }
	FailRuntimeLaunch(entry, error_code);
	return true;
}

// g_app_mutex held; caller has established actual owner quiescence.
static void CompleteQuiescentExit(uint32_t app_id, AppEntry& entry, int32_t result) {
	entry.phase = AppEntry::Phase::Exited;
	entry.status = 2;
	if (g_focused_app == static_cast<int32_t>(app_id)) { g_focused_app = -1; }
	if (g_controller_app == static_cast<int32_t>(app_id)) { g_controller_app = -1; }
	if (g_vsh_runtime && entry.runtime_managed) {
		auto event = Snapshot(entry);
		event.event_type = 4;
		event.error_code = result;
		g_shell_events.push_back(event);
	}
}

// Includes accepted-before-entry cancellation WITHOUT manufacturing Running,
// AppKillEnd or a successful launch. Called only after actual process retirement.
static void CompleteProcessRetirement(uint32_t app_id, AppEntry& entry, int32_t result) {
	if (entry.phase == AppEntry::Phase::Running) { CompleteQuiescentExit(app_id, entry, result); return; }
	if (entry.phase == AppEntry::Phase::Requested) { QueueLaunchStart(entry); }
	entry.phase = AppEntry::Phase::Failed; entry.status = 2;
	entry.launch_error = result < 0 ? result : LibKernel::KERNEL_ERROR_ECANCELED;
	auto event = Snapshot(entry); event.event_type = 2; event.app_id = event.app_local_pid = -1; event.error_code = entry.launch_error;
	g_shell_events.push_back(event);
}

bool RuntimeProcessRetired(const Loader::RuntimeChild::Key& token, int32_t result) {
	if (!token.Valid()) { return false; }
	Common::HostLockCriticalGuard critical; std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(token.app);
	if (!g_vsh_runtime || it == g_by_app_id.end() || !it->second.runtime_managed || it->second.process_token != token || it->second.quiescent_exit_pending ||
	    it->second.phase == AppEntry::Phase::Failed || it->second.phase == AppEntry::Phase::Exited) { return false; }
	auto& entry = it->second;
	if (entry.close == AppEntry::Close::Dispatching) { entry.quiescent_exit_pending = true; entry.quiescent_exit_result = result; }
	else { CompleteProcessRetirement(token.app, entry, result); }
	return true;
}

bool RuntimeLaunchQuiescentExit(uint32_t app_id, int32_t result) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	const auto it = g_by_app_id.find(app_id);
	if (it == g_by_app_id.end() || it->second.process_token.Valid() || it->second.phase != AppEntry::Phase::Running ||
	    it->second.quiescent_exit_pending) { return false; }
	auto& entry = it->second;
	if (entry.close == AppEntry::Close::Dispatching) {
		// A backend may complete synchronously or race its request return. Keep
		// KillStart before KillEnd without holding the registry lock in backend code.
		entry.quiescent_exit_pending = true;
		entry.quiescent_exit_result = result;
	} else {
		CompleteQuiescentExit(app_id, entry, result);
	}
	return true;
}

static int RequestRuntimeClose(int32_t app_id, int32_t reason, bool force) {
	LOGF("LNC close request: app=%d reason=%d force=%d\n", app_id, reason, force);
	std::function<int(const RuntimeCloseRequest&)> handler;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_pending_mutex);
		handler = g_runtime_close;
	}
	RuntimeCloseRequest request;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_app_mutex);
		const auto it = g_by_app_id.find(static_cast<uint32_t>(app_id));
		if (app_id <= 0 || it == g_by_app_id.end()) { return static_cast<int>(0x80940005); }
		auto& entry = it->second;
		// Never close ShellUI, a session daemon, or an arbitrary module by shared PID.
		if (!g_vsh_runtime || !entry.runtime_managed) { return LibKernel::KERNEL_ERROR_ENOTSUP; }
		if (entry.phase == AppEntry::Phase::Exited) { return 0; }
		if (entry.phase == AppEntry::Phase::Failed) { return entry.launch_error; }
		if (entry.close == AppEntry::Close::Requested) { return 0; }
		if (entry.close == AppEntry::Close::Dispatching) { return LibKernel::KERNEL_ERROR_EBUSY; }
		if (!handler) { return LibKernel::KERNEL_ERROR_ENOTSUP; }
		request = {static_cast<uint32_t>(app_id), entry.launch.seq, entry.owner_program_id,
		           entry.owner_lifetime, reason, force};
		request.process_token = entry.process_token;
		entry.close = AppEntry::Close::Dispatching;
	}
	// Guest/HLE backend work must execute without LNC locks or suspension guards.
	int result;
	try { result = handler(request); }
	catch (...) { result = LibKernel::KERNEL_ERROR_EIO; }
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_app_mutex);
		const auto it = g_by_app_id.find(request.app_id);
		if (it == g_by_app_id.end() || it->second.launch.seq != request.launch_seq) {
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		auto& entry = it->second;
		entry.close = result < 0 ? AppEntry::Close::None : AppEntry::Close::Requested;
		if (result >= 0 && entry.phase == AppEntry::Phase::Running) {
			auto event = Snapshot(entry);
			event.event_type = 3; // AppKillStart; completion is exclusively quiescent.
			g_shell_events.push_back(event);
		}
		if (entry.quiescent_exit_pending) {
			entry.quiescent_exit_pending = false;
			if (entry.process_token.Valid()) { CompleteProcessRetirement(request.app_id, entry, entry.quiescent_exit_result); }
			else { CompleteQuiescentExit(request.app_id, entry, entry.quiescent_exit_result); }
		}
	}
	return result < 0 ? result : 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilKillApp(int32_t app_id) {
	return RequestRuntimeClose(app_id, 0, false);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilKillAppWithReason(int32_t app_id, int32_t reason) {
	return RequestRuntimeClose(app_id, reason, false);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilForceKillApp(int32_t app_id) {
	return RequestRuntimeClose(app_id, 0, true);
}

void RuntimeLaunchExited(uint32_t app_id) {
	RuntimeLaunchQuiescentExit(app_id);
}

static int SetFocus(int32_t app_id, uint64_t flags, uint64_t context_id, bool controller) {
	LOGF("Lnc focus request: app=%d controller=%d flags=0x%llx context=0x%llx\n", app_id, controller,
	    static_cast<unsigned long long>(flags), static_cast<unsigned long long>(context_id));
	std::function<int(const RuntimeFocusRequest&)> backend;
	{
		Common::HostLockCriticalGuard critical; std::lock_guard lock(g_pending_mutex); backend = g_runtime_focus;
	}
	RuntimeFocusRequest request;
	std::unique_lock<std::mutex> transaction(g_runtime_focus_mutex, std::defer_lock);
	if (backend) {
		transaction.lock();
		{
			Common::HostLockCriticalGuard critical; std::lock_guard lock(g_app_mutex);
			if (!g_vsh_runtime) { return 0; }
			const auto it = g_by_app_id.find(static_cast<uint32_t>(app_id));
			// ShellUI is an actual registered in-parent application, not an
			// external process token. Its controller focus must reach the host
			// broker too (Control Center retains the external display focus).
			const bool shell = it != g_by_app_id.end() && !it->second.runtime_managed &&
			    it->second.owner_program_id >= 0 && it->second.app_status.app_type == 1;
			if (app_id != -1 && (it == g_by_app_id.end() || it->second.phase != AppEntry::Phase::Running ||
			    (!shell && !it->second.process_token.Valid()))) {
				LOGF("Lnc focus refused: app=%d controller=%d (no running owner)\n", app_id, controller);
				return static_cast<int>(0x80940005);
			}
			request = {shell ? -1 : app_id, controller, ++g_focus_epoch,
			    app_id == -1 || shell ? Loader::RuntimeChild::Key {} : it->second.process_token};
		}
		// Never wait for a child/UI acknowledgement while holding LNC locks.
		int status = LibKernel::KERNEL_ERROR_EIO;
		try { status = backend(request); } catch (...) {}
		LOGF("Lnc focus result: app=%d controller=%d epoch=%llu result=0x%08x\n", app_id, controller,
		    static_cast<unsigned long long>(request.epoch), static_cast<unsigned>(status));
		if (status < 0) { return status; }
	}
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	if (backend && request.epoch != g_focus_epoch) { return LibKernel::KERNEL_ERROR_ECANCELED; }
	if (!g_vsh_runtime) { return 0; } // Preserve direct-game behavior.
	const auto it = g_by_app_id.find(static_cast<uint32_t>(app_id));
	if (app_id != -1 && (it == g_by_app_id.end() || it->second.phase != AppEntry::Phase::Running)) {
		return static_cast<int>(0x80940005);
	}
	auto event = app_id == -1 ? EventWire {} : Snapshot(it->second);
	event.app_id = app_id;
	event.check_flags = flags;
	event.context_id = context_id;
	event.event_type = controller ? 0x1003 : 0x1001;
	g_shell_events.push_back(event);
	(controller ? g_controller_app : g_focused_app) = app_id;
	// Ownership is committed synchronously. This is not application startup or exit.
	++event.event_type;
	g_shell_events.push_back(event);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilSetAppFocus(
    int32_t app_id, uint64_t flags, uint64_t context_id) {
	return SetFocus(app_id, flags, context_id, false);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilSetControllerFocus(int32_t app_id) {
	return SetFocus(app_id, 0, 0, true);
}

int32_t GetFocusedAppId() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	return g_vsh_runtime ? g_focused_app : -1;
}

int32_t GetControllerFocusedAppId() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	return g_vsh_runtime ? g_controller_app : -1;
}

static bool QueryFocus(const Loader::Program* program, bool controller) {
	// Firmware/direct-game programs have no runtime application owner.
	if (program == nullptr || program->application_owner == nullptr) { return controller; }
	program = ApplicationOwner(program);
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	if (!g_vsh_runtime) { return controller; }
	const auto owner = g_by_program.find(program->unique_id);
	const int32_t focus = controller ? g_controller_app : g_focused_app;
	const auto entry = owner != g_by_program.end() ? g_by_app_id.find(owner->second) : g_by_app_id.end();
	// No target means system observation only, never a broadcast grant to runtime
	// clients. Registration alone (before entry or after exit) cannot own focus.
	const bool owns = focus >= 0 && owner != g_by_program.end() &&
	                  focus == static_cast<int32_t>(owner->second) && entry != g_by_app_id.end() &&
	                  entry->second.runtime_managed && entry->second.phase == AppEntry::Phase::Running;
	return controller ? owns : !owns;
}

bool HasControllerFocus(const Loader::Program* program) {
	return QueryFocus(program, true);
}

bool IsSystemUiOverlaid(const Loader::Program* program) {
	return QueryFocus(program, false);
}

void EnqueuePendingLaunch(const std::string& titleId,
                          const std::filesystem::path& app0_dir,
                           const std::vector<std::string>& args, uint32_t appId) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_pending_mutex);
	PendingLaunch pl;
	pl.titleId  = titleId;
	pl.app0_dir = app0_dir;
	pl.args     = args;
	pl.appId    = appId;
	pl.seq      = g_pending_seq++;
	g_pending.push_back(std::move(pl));
}

bool TakePendingLaunch(PendingLaunch* out) {
	if (out == nullptr) {
		return false;
	}
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_pending_mutex);
	if (g_pending.empty()) {
		return false;
	}
	*out = std::move(g_pending.front());
	g_pending.pop_front();
	return true;
}

bool HasPendingLaunch() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_pending_mutex);
	return !g_pending.empty();
}

void ClearPendingLaunches() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_pending_mutex);
	g_pending.clear();
}

size_t PollPendingLaunchCount() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_pending_mutex);
	return g_pending.size();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilLaunchApp(
    const char* titleId, const char* const* argv, const LaunchAppParam* param) {
	PRINT_NAME();
	if (titleId == nullptr || param == nullptr || !ProbeGuest(reinterpret_cast<uint64_t>(param), sizeof(*param))) {
		LOGF("LNC launch rejected: null title or unreadable parameter payload\n");
		return static_cast<int>(0x80940005);
	}
	for (size_t i = 0; i < 10; ++i) {
		if (!ProbeGuest(reinterpret_cast<uint64_t>(titleId + i), 1) || (titleId[i] == '\0') != (i == 9)) {
			LOGF("LNC launch rejected: title byte %zu is unreadable or has invalid termination\n", i);
			return static_cast<int>(0x80940005);
		}
	}
	std::vector<std::string> fwd_args;
	if (!ReadGuestArgv(argv, &fwd_args)) {
		LOGF("LNC launch rejected: invalid argv\n");
		return static_cast<int>(0x80940005);
	}
	const LaunchAppParam launch_param = *param;
	if (launch_param.size != LaunchAppParamAdvertisedSize) {
		LOGF("LNC launch rejected: parameter size=%u, expected=%u\n", launch_param.size, LaunchAppParamAdvertisedSize);
		return static_cast<int>(0x80940005);
	}
	std::string title(titleId, 9);
	LOGF("\ttitleId = %s, argv = %p, param = %p, user = 0x%x, flags = 0x%llx\n",
	     title.c_str(), static_cast<const void*>(argv), static_cast<const void*>(param),
	     param->user_id, (unsigned long long)param->check_flag);
	PendingLaunch request;
	request.titleId = title;
	request.args = std::move(fwd_args);
	request.param = launch_param;
	request.user_id = launch_param.user_id;
	std::function<int(const PendingLaunch&)> handler;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_pending_mutex);
		handler = g_runtime_launch;
		request.seq = g_pending_seq++;
	}
	uint32_t appId;
	bool vsh_runtime;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard<std::mutex> lock(g_app_mutex);
		appId = g_next_app_id++;
		request.appId = appId;
		vsh_runtime = g_vsh_runtime;
		g_by_title[title] = appId;
		auto& entry = g_by_app_id[appId];
		entry.title_id = title;
		entry.app_status.app_id = static_cast<int32_t>(appId);
		CopyStatusString(entry.app_status.title_id, title);
		entry.app_status.app_attr = static_cast<uint32_t>(launch_param.app_attr);
		const auto* caller = ApplicationOwner(Loader::RuntimeLinker::GetCurrentProgram());
		const auto owner = caller != nullptr ? g_by_program.find(caller->unique_id) : g_by_program.end();
		if (owner != g_by_program.end()) {
			entry.app_status.launch_request_app_id = static_cast<int32_t>(owner->second);
		}
		request.launch_request_app_id = entry.app_status.launch_request_app_id;
		entry.launch = request;
		entry.runtime_managed = vsh_runtime;
		if (vsh_runtime) {
			entry.phase = AppEntry::Phase::Requested;
			entry.status = 1;
		}
	}
	LOGF("\t-> appId = %u\n", (unsigned)appId);
	if (handler) {
		const int result = handler(request);
		LOGF("LNC launch result: title=%s app=%u result=0x%08x\n", title.c_str(), appId, static_cast<unsigned>(result));
		if (result < 0) {
			if (vsh_runtime) {
				RuntimeLaunchFailed(appId, result);
				return result;
			}
			Common::HostLockCriticalGuard critical;
			std::lock_guard lock(g_app_mutex);
			g_by_app_id.erase(appId);
			const auto by_title = g_by_title.find(title);
			if (by_title != g_by_title.end() && by_title->second == appId) { g_by_title.erase(by_title); }
			return result;
		}
		return static_cast<int>(appId);
	}
	if (vsh_runtime) {
		const int error = LibKernel::KERNEL_ERROR_ENOTSUP;
		RuntimeLaunchFailed(appId, error);
		return error;
	}
	// Child re-entry queue: app0_dir left EMPTY (no fw_root in-lib; consumer
	// resolves NPXS* via ProbeChildTitle at consume time). Guest argv is
	// forwarded (bounded + probed); the consumer feeds it to the child entry
	// so ShellCore-driven launches keep their args (e.g. NPXS40038
	// videoCoreName + buildType).
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_pending_mutex);
		g_pending.push_back(std::move(request));
	}
	return static_cast<int>(appId);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetResultLaunchAppByTitleId(const char* titleId,
                                                               uint32_t* out_appId) {
	PRINT_NAME();
	LOGF("\ttitleId = %s, out_appId = 0x%llx\n", SafeStr(titleId),
	     (unsigned long long)out_appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	if (titleId != nullptr) {
		auto it = g_by_title.find(titleId);
		if (it != g_by_title.end()) {
			const auto entry = g_by_app_id.find(it->second);
			if (g_vsh_runtime && entry != g_by_app_id.end()) {
				if (entry->second.phase == AppEntry::Phase::Failed) { return entry->second.launch_error; }
				if (entry->second.phase != AppEntry::Phase::Running) { return LibKernel::KERNEL_ERROR_EAGAIN; }
			}
			if (out_appId != nullptr) {
				*out_appId = it->second;
			}
			LOGF("\t-> appId = %u\n", (unsigned)it->second);
			LOGF("\tpending depth = %zu\n", PollPendingLaunchCount());
			return 0;
		}
	}
	if (g_vsh_runtime) { return static_cast<int>(0x80940001); }
	LOGF("\ttitle not launched yet, returning 0 (placeholder)\n");
	LOGF("\tpending depth = %zu\n", PollPendingLaunchCount());
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilIsAppLaunched(
    const char* title_id, uint8_t* out) {
	// 13.60 SystemService +6450 -> +6c70 -> +9730 validates title length 9;
	// LncUtilWrapper::_IsAppLaunched marshals an LPStr and an I1 out bool.
	if (title_id == nullptr || out == nullptr) { return static_cast<int>(0x80940005); }
	std::string title;
	for (size_t i = 0; i < 16; ++i) {
		if (!ProbeGuest(reinterpret_cast<uint64_t>(title_id + i), 1)) { return static_cast<int>(0x80940005); }
		if (title_id[i] == '\0') { break; }
		title += title_id[i];
	}
	if (title.size() != 9) { return static_cast<int>(0x80940005); }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_app_mutex);
	uint8_t launched = 0;
	// A rejected relaunch must not hide an already-running instance of the title.
	for (const auto& [id, entry]: g_by_app_id) {
		if (entry.title_id == title && entry.phase == AppEntry::Phase::Running) {
			launched = 1;
			break;
		}
	}
	*out = launched; // Exactly one byte, not an int/bool return value.
	return 0;
}

bool TryGetCurrentAppId(int32_t* out) {
	return TryGetProgramAppId(Loader::RuntimeLinker::GetCurrentProgram(), out);
}

bool TryGetProgramAppId(const Loader::Program* caller, int32_t* out) {
	if (out == nullptr) { return false; }
	caller = ApplicationOwner(caller);
	if (caller == nullptr) { return false; }
	int32_t app_id = 0;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard<std::mutex> lock(g_app_mutex);
		const auto owner = g_by_program.find(caller->unique_id);
		if (owner == g_by_program.end()) { return false; }
		const auto entry = g_by_app_id.find(owner->second);
		if (entry == g_by_app_id.end()) { return false; }
		app_id = entry->second.app_status.app_id;
	}
	*out = app_id;
	return true;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetAppStatus(AppStatus* out) {
	PRINT_NAME();
	// FW libSceSystemService +6370 -> +6b90 -> +8b40: null is
	// 80940005, uninitialized client is 80940001. +dfa0 sends IPC
	// 30010 with no inputs and one 0x40-byte output. Suspension is not
	// the return value of this API.
	if (out == nullptr) {
		return static_cast<int>(0x80940005);
	}
	const auto* caller = ApplicationOwner(Loader::RuntimeLinker::GetCurrentProgram());
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	const auto owner = caller != nullptr ? g_by_program.find(caller->unique_id) : g_by_program.end();
	if (owner == g_by_program.end()) {
		return static_cast<int>(0x80940001); // No initialized application context; output untouched.
	}
	const auto entry = g_by_app_id.find(owner->second);
	if (entry == g_by_app_id.end()) {
		return static_cast<int>(0x80940001);
	}
	std::memcpy(out, &entry->second.app_status, sizeof(*out));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetAppTitleId(uint32_t appId, char* out, uint64_t size) {
	PRINT_NAME();
	LOGF("\tappId = %u, out = 0x%llx, size = %llu\n", (unsigned)appId,
	     (unsigned long long)out, (unsigned long long)size);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	if (out != nullptr && size > 0) {
		const std::string& title = it->second.title_id;
		uint64_t           copy  = title.size() < (size - 1) ? (uint64_t)title.size() : size - 1;
		std::memcpy(out, title.c_str(), (size_t)copy);
		out[copy] = '\0';
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetAppCategoryType(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilBlockAppSuspend(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilUnblockAppSuspend(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	return 0;
}

// ps5-vsh: ShellCore-driven VCN budget table (`ChangeVcnResource`).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilChangeVcnResource(int vcn, const char* titleId) {
	PRINT_NAME();
	LOGF("\tvcn = %d, titleId = %s\n", vcn, SafeStr(titleId));
	std::string owner = titleId != nullptr ? titleId : "";
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard<std::mutex> lock(g_vcn_mutex);
		VcnBudget& b = g_vcn_table[vcn];
		b.vcn        = vcn;
		b.owner      = owner;
		g_current_vcn = vcn;
	}
	LOGF("\tvcn[%3d] owner=%s\n", vcn, owner.c_str());
	return 0;
}

// ps5-vsh: backend-endian alias (`beChangeVcnResource`).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilBeChangeVcnResource(int vcn, const char* titleId) {
	PRINT_NAME();
	return sceLncUtilChangeVcnResource(vcn, titleId);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilGetVcnResource() {
	PRINT_NAME();
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_vcn_mutex);
	if (g_current_vcn < 0) {
		LOGF("\tno vcn resource set yet, returning -1\n");
		return -1;
	}
	return g_current_vcn;
}

// ps5-vsh: `EnsureBigAppBudget`.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilEnsureBigAppBudget(uint32_t appId, int mb) {
	PRINT_NAME();
	LOGF("\tappId = %u, mb = %d\n", (unsigned)appId, mb);
	if (mb <= 0) {
		mb = VCN_BUDGET_MB_DISC_PLAYER;
		LOGF("\tmb <= 0, using Disc Player vcn budget %d MB\n", mb);
	}
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	it->second.bigapp_budget_mb = mb;
	return 0;
}

// ps5-vsh: `lockBigApp` (refuse double-lock with nonzero).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilLockBigApp(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	if (it->second.bigapp_locked) {
		LOGF("\tappId %u already locked, refusing double-lock\n", (unsigned)appId);
		return 1;
	}
	it->second.bigapp_locked = true;
	return 0;
}

// ps5-vsh: `unlockBigApp`.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilUnlockBigApp(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	if (!it->second.bigapp_locked) {
		LOGF("\tappId %u not locked, nothing to unlock\n", (unsigned)appId);
		return 1;
	}
	it->second.bigapp_locked = false;
	return 0;
}

static int ClampCpuPct(int pct) {
	if (pct < 0) {
		return 0;
	}
	if (pct > 100) {
		return 100;
	}
	return pct;
}

// ps5-vsh: `acquireCpuBudget*`.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilAcquireCpuBudget(uint32_t appId, int pct) {
	PRINT_NAME();
	LOGF("\tappId = %u, pct = %d\n", (unsigned)appId, pct);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	it->second.cpu_budget_pct = ClampCpuPct(pct);
	return 0;
}

// ps5-vsh: `releaseCpuBudget*`.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilReleaseCpuBudget(uint32_t appId, int pct) {
	PRINT_NAME();
	LOGF("\tappId = %u, pct = %d\n", (unsigned)appId, pct);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	it->second.cpu_budget_pct = ClampCpuPct(pct);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilIsBigAppLocked(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0\n");
		return 0;
	}
	return it->second.bigapp_locked ? 1 : 0;
}

// ps5-vsh: `BigAppGame suspending`.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilSuspendBigApp(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	it->second.suspended = true;
	LOGF("\tBigAppGame suspending appId = %u\n", (unsigned)appId);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilResumeBigApp(uint32_t appId) {
	PRINT_NAME();
	LOGF("\tappId = %u\n", (unsigned)appId);
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock(g_app_mutex);
	auto it = g_by_app_id.find(appId);
	if (it == g_by_app_id.end()) {
		LOGF("\tunknown appId, returning 0x80940001\n");
		return (int)0x80940001;
	}
	it->second.suspended = false;
	LOGF("\tBigAppGame resuming appId = %u\n", (unsigned)appId);
	return 0;
}

// ps5-vsh: poll helper for the future emulator poll loop (LOCAL only).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceLncUtilPollPendingLaunchCount() {
	PRINT_NAME();
	size_t n = PollPendingLaunchCount();
	LOGF("\tpending depth = %zu\n", n);
	return (int)n;
}

} // namespace Lnc

namespace SystemServiceCloseExports {
// The native 13.60 library is LncUtil within the SystemService module. Keep
// exact registrations as well as the older standalone LncUtil-module aliases.
LIB_VERSION("LncUtil", 1, "SystemService", 1, 1);
static void Register(Loader::SymbolDatabase* s) {
	LIB_FUNC("SZ2uH5Abws8", Lnc::sceLncUtilKillApp);
	LIB_FUNC("RBlEzB--JeA", Lnc::sceLncUtilKillAppWithReason);
	LIB_FUNC("vquYrvImjPg", Lnc::sceLncUtilForceKillApp);
}
} // namespace SystemServiceCloseExports

LIB_DEFINE(InitLncUtil_1) {
	SystemServiceCloseExports::Register(s);
	LIB_FUNC("cHlIo6CoRTA", Lnc::sceLncUtilGetSpmemAllocatorConfigValue);
	LIB_FUNC("+nRJUD-7qCk", Lnc::sceLncUtilLaunchApp);
	// 13.60 SystemService +6280/+6550/+6680, native ShellUI import table.
	LIB_FUNC("SZ2uH5Abws8", Lnc::sceLncUtilKillApp);
	LIB_FUNC("RBlEzB--JeA", Lnc::sceLncUtilKillAppWithReason);
	LIB_FUNC("vquYrvImjPg", Lnc::sceLncUtilForceKillApp);
	LIB_FUNC("LZs6hfPMnso", Lnc::sceLncUtilGetResultLaunchAppByTitleId);
	LIB_FUNC("IGrJsPNL6n4", Lnc::sceLncUtilIsAppLaunched);
	// Verified 13.60 SystemService exports +0x64d0 / +0x64f0 and managed signatures.
	// The 4.03 reference's P7DYuX0G5TQ alias is not a 13.60 export.
	LIB_FUNC("v7DYuX0G5TQ", Lnc::sceLncUtilSetAppFocus);
	LIB_FUNC("X8gYbyLG1wk", Lnc::sceLncUtilSetControllerFocus);
	LIB_FUNC("DxRki7T2E44", Lnc::sceLncUtilGetAppStatus);
	LIB_FUNC("g0wTG9KImzI", Lnc::sceLncUtilGetAppTitleId);
	LIB_FUNC("LtGMfy0pCj4", Lnc::sceLncUtilGetAppCategoryType);
	LIB_FUNC("u1JVDP28ycg", Lnc::sceLncUtilBlockAppSuspend);
	LIB_FUNC("iRZduYIV1hs", Lnc::sceLncUtilUnblockAppSuspend);
	// ps5-vsh: launch-control event receive consumed by `LncProxy::UpdateEvent`
	// (guest eboot.bin) on behalf of EventMonitor.Update. Must report "empty"
	// (EAGAIN) rather than success while nothing is queued. Same NID the 4.03
	// reference resolves.
	LIB_FUNC("ZucoOmNsb7w", Lnc::sceLncUtilGetEventForShellUI);
	// LOCAL VCN/BigApp budget flow (ShellCore-driven; real Vcn NIDs come from a ShellCore/VSE export dump next round).
	LIB_FUNC("LOCALVcnC01", Lnc::sceLncUtilChangeVcnResource); // LOCAL
	LIB_FUNC("LOCALVcnBe1", Lnc::sceLncUtilBeChangeVcnResource); // LOCAL
	LIB_FUNC("LOCALVcnG01", Lnc::sceLncUtilGetVcnResource); // LOCAL
	LIB_FUNC("LOCALBgEns1", Lnc::sceLncUtilEnsureBigAppBudget); // LOCAL
	LIB_FUNC("LOCALBgLck1", Lnc::sceLncUtilLockBigApp); // LOCAL
	LIB_FUNC("LOCALBgUlk1", Lnc::sceLncUtilUnlockBigApp); // LOCAL
	LIB_FUNC("LOCALCpuAc1", Lnc::sceLncUtilAcquireCpuBudget); // LOCAL
	LIB_FUNC("LOCALCpuRl1", Lnc::sceLncUtilReleaseCpuBudget); // LOCAL
	LIB_FUNC("LOCALBgIsL1", Lnc::sceLncUtilIsBigAppLocked); // LOCAL
	LIB_FUNC("LOCALBgSus1", Lnc::sceLncUtilSuspendBigApp); // LOCAL
	LIB_FUNC("LOCALBgRes1", Lnc::sceLncUtilResumeBigApp); // LOCAL
	LIB_FUNC("LOCALPend01", Lnc::sceLncUtilPollPendingLaunchCount); // LOCAL
}

} // namespace LncUtil

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp):

namespace LncUtil {
LIB_DEFINE(InitLncUtil_1);
} // namespace LncUtil

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   LncUtil::InitLncUtil_1(s);

LIB_FUNC lines used:
	LIB_FUNC("+nRJUD-7qCk", Lnc::sceLncUtilLaunchApp);
	LIB_FUNC("LZs6hfPMnso", Lnc::sceLncUtilGetResultLaunchAppByTitleId);
	LIB_FUNC("IGrJsPNL6n4", Lnc::sceLncUtilIsAppLaunched);
	LIB_FUNC("DxRki7T2E44", Lnc::sceLncUtilGetAppStatus);
	LIB_FUNC("g0wTG9KImzI", Lnc::sceLncUtilGetAppTitleId);
	LIB_FUNC("LtGMfy0pCj4", Lnc::sceLncUtilGetAppCategoryType);
	LIB_FUNC("u1JVDP28ycg", Lnc::sceLncUtilBlockAppSuspend);
	LIB_FUNC("iRZduYIV1hs", Lnc::sceLncUtilUnblockAppSuspend);
*/

// emulator.cpp poll loop (orchestrator integrates):
// Libs::LncUtil::Lnc::PendingLaunch pl{};
// while (Libs::LncUtil::Lnc::TakePendingLaunch(&pl)) {
//   if (pl.app0_dir.empty()) Loader::ProbeChildTitle(fwAppDir, pl.titleId, &spec);
//   if (!Loader::RemountApp0(pl.app0_dir)) continue;
//   auto argv = Loader::BuildChildArgv(videoCore, buildType);
//   /* re-enter child with argv */
// }
