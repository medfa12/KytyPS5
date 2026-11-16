#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBLNCUTIL_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBLNCUTIL_H_

#include "common/abi.h"
#include "loader/runtimeChildProtocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Loader {
struct Program;
}

namespace Libs::LncUtil::Lnc {

// FW 13.60 LncUtilWrapper, Pack=4. The marshalled field payload is 36 bytes,
// but native LaunchAppParamInit (eboot+0x562ad0) advertises size 40. Keep
// that ABI version value separate from the field extent; no field exists +36.
// LaunchApp takes three arguments and returns the app ID (or a negative error);
// argv is null-terminated.
inline constexpr uint32_t LaunchAppParamAdvertisedSize = 40;
#pragma pack(push, 4)
struct LaunchAppParam {
	uint32_t size;
	int32_t user_id;
	int32_t app_attr;
	int32_t enable_crash_report;
	uint64_t check_flag;
	uint64_t context_id;
	uint8_t is_speculative_launch;
	uint8_t reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(LaunchAppParam) == 36);

// FW 13.60 LncUtilWrapper.AppStatus native marshalling layout.
#pragma pack(push, 4)
struct AppStatus {
	int32_t app_id = 0;
	int32_t launch_request_app_id = -1; // No requesting application for a host launch.
	int32_t app_type = 0; // Unknown; never infer ShellUI from a shared PID.
	uint32_t app_attr = 0; // LNC launch flags, NOT param.json attribute/attribute3.
	char title_id[16] {};
	uint32_t app_category_type = 0;
	int32_t app_boot_mode = -1; // Invalid: no boot-mode provider yet.
	int32_t app_boot_sub_mode = -1;
	char app_ver[6] {};
	char content_version[11] {};
	uint8_t is_system_ex_app = 0;
	uint8_t is_cdlg_initialized = 0; // No per-application CDLG initialization backend yet.
	uint8_t reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(AppStatus) == 0x40 && alignof(AppStatus) == 4);
static_assert(offsetof(AppStatus, app_id) == 0x00);
static_assert(offsetof(AppStatus, launch_request_app_id) == 0x04);
static_assert(offsetof(AppStatus, app_type) == 0x08);
static_assert(offsetof(AppStatus, app_attr) == 0x0c);
static_assert(offsetof(AppStatus, title_id) == 0x10);
static_assert(offsetof(AppStatus, app_category_type) == 0x20);
static_assert(offsetof(AppStatus, app_boot_mode) == 0x24);
static_assert(offsetof(AppStatus, app_boot_sub_mode) == 0x28);
static_assert(offsetof(AppStatus, app_ver) == 0x2c);
static_assert(offsetof(AppStatus, content_version) == 0x32);
static_assert(offsetof(AppStatus, is_system_ex_app) == 0x3d);
static_assert(offsetof(AppStatus, is_cdlg_initialized) == 0x3e);

// FW 13.60 EventMonitor.Event (Pack=4), not the 4.03 native record.
// Invalid IDs are -1; zero error_code means a successful transition.
#pragma pack(push, 4)
struct EventWire {
	int32_t event_type = 0;
	char title_id[16] {};
	int32_t app_id = -1;
	int32_t app_type = 0;
	uint32_t app_attribute = 0;
	int32_t launch_request_app_id = -1;
	int32_t user_id = -1;
	uint8_t arguments[4096] {};
	int32_t argument_size = 0;
	int32_t error_code = 0;
	int32_t error_data = 0;
	int32_t app_local_pid = -1;
	char path[1024] {};
	char sce_sys_dir[256] {};
	int32_t cause_code = 0;
	int32_t existing_app_id = -1;
	uint64_t check_flags = 0;
	uint64_t context_id = 0;
	int32_t coredump_reason = 0;
	char corefile_name[256] {};
	uint32_t exception_code = 0;
	char category[4] {};
	uint32_t app_category_type = 0;
	int32_t app_boot_mode = -1;
	int32_t app_boot_sub_mode = -1;
	int32_t is_crashed = 0;
};
#pragma pack(pop)
static_assert(sizeof(EventWire) == 5740 && alignof(EventWire) == 4);
static_assert(offsetof(EventWire, app_id) == 20);
static_assert(offsetof(EventWire, arguments) == 40);
static_assert(offsetof(EventWire, error_code) == 4140);
static_assert(offsetof(EventWire, path) == 4152);
static_assert(offsetof(EventWire, check_flags) == 5440);
static_assert(offsetof(EventWire, app_boot_mode) == 5728);
static_assert(offsetof(EventWire, is_crashed) == 5736);

// Host launch hook, before any guest module initialization. Metadata is read
// independently per executable, never from mutable global title state.
void RegisterProgramLaunch(const Loader::Program* program, const std::filesystem::path& param_json,
                           bool system_ex, uint32_t requested_app_id = 0,
                           const std::filesystem::path& firmware_root = {});
int KYTY_SYSV_ABI sceLncUtilGetAppStatus(AppStatus* out);
// Current TLS program's registered launch identity; failure leaves output untouched.
bool TryGetCurrentAppId(int32_t* out);
bool TryGetProgramAppId(const Loader::Program* program, int32_t* out);

// ShellCore-driven child re-entry queue (Lnc half of spawnApp).
// sceLncUtilLaunchApp enqueues; emulator drains via TakePendingLaunch.
struct PendingLaunch {
	std::string              titleId;
	std::filesystem::path    app0_dir;
	std::vector<std::string> args;
	uint32_t                 appId = 0;
	uint64_t                 seq   = 0;
	int32_t                  user_id = -1;
	LaunchAppParam           param {};
	int32_t                 launch_request_app_id = -1;
};

// The runtime host backend owns process creation; it receives no guest pointers.
void SetRuntimeLaunchHandler(std::function<int(const PendingLaunch&)> handler);
// Immutable close target. seq identifies the launch even before owner creation;
// owner identity/lifetime, when published, must match the backend's active run.
// A zero result accepts a request, NOT completed termination or quiescence.
struct RuntimeCloseRequest {
	uint32_t app_id = 0;
	uint64_t launch_seq = 0;
	int32_t owner_program_id = -1;
	uint64_t owner_lifetime = 0;
	int32_t reason = 0;
	bool force = false;
	Loader::RuntimeChild::Key process_token {}; // HOST-only; never the guest wire.
};
void SetRuntimeCloseHandler(std::function<int(const RuntimeCloseRequest&)> handler);
// Explicit session opt-in; a launch handler alone does not enable VSH behavior.
void ConfigureVshRuntime(bool enabled);
// Standalone child role, before the first program registration. No parent guest
// pointers or mutable profile state is imported into the independent process.
bool ConfigureStandaloneRuntimeIdentity(uint32_t app_id);
// A subprocess has no in-parent Program/execution. Bind its unguessable immutable
// host launch token before acceptance; notifications cannot match by PID alone.
bool RegisterRuntimeProcess(const Loader::RuntimeChild::Key& token, uint32_t category);
bool RuntimeProcessStarted(const Loader::RuntimeChild::Key& token, int32_t child_guest_pid);
bool RuntimeProcessRetired(const Loader::RuntimeChild::Key& token, int32_t result);
struct RuntimeFocusRequest {
	int32_t app_id = -1;
	bool controller = false;
	uint64_t epoch = 0;
	Loader::RuntimeChild::Key process_token {};
};
void SetRuntimeFocusHandler(std::function<int(const RuntimeFocusRequest&)> handler);
// Resolved native runtime plan, before Accepted: guest paths, never host paths.
// This explicitly classifies the plan as a BigApp; unclassified rejections remain Unknown.
bool RegisterRuntimeLaunchPath(uint32_t app_id, const std::string& guest_executable_path,
                               const std::string& guest_sce_sys_dir);
// All callback IDs are LNC app IDs (PendingLaunch::appId), never module IDs.
// Accepted: after path registration, before execution. Started: real guest entry.
// Failure: actual negative backend error. QuiescentExit: all guest threads stopped.
// Duplicate/out-of-order callbacks return false and produce no events.
bool RuntimeLaunchAccepted(uint32_t app_id);
bool RuntimeLaunchStarted(uint32_t app_id, int32_t app_local_pid = -1);
bool RuntimeLaunchFailed(uint32_t app_id, int32_t error_code);
// Native launch arbitration: preserve the existing process and let ShellUI
// choose focus or close/retry using the conflict event's existing_app_id.
int RuntimeLaunchConflict(uint32_t app_id, const Loader::RuntimeChild::Key& existing);
// result is the LNC teardown operation status, not the guest main() return code.
bool RuntimeLaunchQuiescentExit(uint32_t app_id, int32_t result = 0);
// Compatibility hook; must only be called at quiescent process exit.
void RuntimeLaunchExited(uint32_t app_id);
// Host consumers resolve application_owner before comparing LNC identities.
bool HasControllerFocus(const Loader::Program* program);
bool IsSystemUiOverlaid(const Loader::Program* program);
int32_t GetFocusedAppId();
int32_t GetControllerFocusedAppId();

void   EnqueuePendingLaunch(const std::string& titleId, const std::filesystem::path& app0_dir,
                            const std::vector<std::string>& args, uint32_t appId);
bool   TakePendingLaunch(PendingLaunch* out);
bool   HasPendingLaunch();
void   ClearPendingLaunches();
size_t PollPendingLaunchCount();

} // namespace Libs::LncUtil::Lnc

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBLNCUTIL_H_ */
