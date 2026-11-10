#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "emulator.h"
#include "graphics/presentation/systemOverlay.h"
#include "libs/dialog.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/libSystemService.h"
#include "libs/libs.h"
#include "loader/runtimeGuestExecution.h"
#include "loader/runtimeChildClient.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <cstring>
#include <string>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Libs {

LIB_VERSION("SystemService", 1, "SystemService", 1, 1);

namespace SystemService {

[[maybe_unused]] constexpr int PARAM_ID_LANG                = 1;
[[maybe_unused]] constexpr int PARAM_ID_DATE_FORMAT         = 2;
[[maybe_unused]] constexpr int PARAM_ID_TIME_FORMAT         = 3;
[[maybe_unused]] constexpr int PARAM_ID_TIME_ZONE           = 4;
[[maybe_unused]] constexpr int PARAM_ID_SUMMERTIME          = 5;
[[maybe_unused]] constexpr int PARAM_ID_SYSTEM_NAME         = 6;
[[maybe_unused]] constexpr int PARAM_ID_GAME_PARENTAL_LEVEL = 7;
[[maybe_unused]] constexpr int PARAM_ID_CC_ENABLE           = 100;
[[maybe_unused]] constexpr int PARAM_ID_SCREEN_READER       = 208;
constexpr int                  PARAM_ID_LANG_PS5            = 400; // PPSA01325
[[maybe_unused]] constexpr int PARAM_ID_ENTER_BUTTON_ASSIGN = 1000;

[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_YYYYMMDD = 0;
[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_DDMMYYYY = 1;
[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_MMDDYYYY = 2;

[[maybe_unused]] constexpr int PARAM_TIME_FORMAT_12HOUR = 0;
[[maybe_unused]] constexpr int PARAM_TIME_FORMAT_24HOUR = 1;

[[maybe_unused]] constexpr int PARAM_CC_DISABLED = 0;
[[maybe_unused]] constexpr int PARAM_CC_ENABLED  = 1;

[[maybe_unused]] constexpr int MAX_SYSTEM_NAME_LENGTH = 65;

[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_OFF     = 0;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL01 = 1;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL02 = 2;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL03 = 3;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL04 = 4;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL05 = 5;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL06 = 6;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL07 = 7;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL08 = 8;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL09 = 9;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL10 = 10;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL11 = 11;

[[maybe_unused]] constexpr int PARAM_ENTER_BUTTON_ASSIGN_CIRCLE = 0;
[[maybe_unused]] constexpr int PARAM_ENTER_BUTTON_ASSIGN_CROSS  = 1;

static std::mutex          g_event_mutex;
static std::deque<int32_t> g_events;

namespace {
constexpr int32_t EntitlementUpdate = 0x10000003;
struct ApplicationEvents {
	Loader::GuestCallOwner owner;
	std::weak_ptr<Loader::RuntimeGuestExecution> execution;
	std::deque<int32_t> events;
};
// Non-reusing execution token, not Program*. Weak snapshots do not pin retired
// executables. Explicit release removes entries; registration also sweeps expired
// snapshots if an abandoned host preparation never reached its cleanup hook.
static std::unordered_map<uint64_t, ApplicationEvents> g_application_events;

template <typename Operation>
int WithApplicationEvents(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution, Operation&& operation) {
	if (!execution) { return SYSTEM_SERVICE_ERROR_UNAVAILABLE; }
	auto lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
	if (!lease) { return SYSTEM_SERVICE_ERROR_UNAVAILABLE; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_event_mutex);
	const auto owner = execution->Owner();
	const auto it = g_application_events.find(owner.token);
	if (it == g_application_events.end() || it->second.owner.identity != owner.identity ||
	    it->second.execution.lock() != execution) { return SYSTEM_SERVICE_ERROR_UNAVAILABLE; }
	int result = SYSTEM_SERVICE_ERROR_UNAVAILABLE;
	// Only short host-memory changes here, never guest output writes/callbacks.
	// Stop and event publication/consumption have a single linearization point.
	try {
		lease.Publish([&] { result = operation(it->second.events); });
	} catch (const std::bad_alloc&) {
		return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
	}
	return result;
}

template <typename Operation>
int WithCurrentEvents(Operation&& operation) {
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (program != nullptr && program->application_owner != nullptr) {
		return WithApplicationEvents(Loader::GetApplicationExecution(program), std::forward<Operation>(operation));
	}
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_event_mutex);
	return operation(g_events); // Preserve the legacy null/direct/firmware session.
}
} // namespace

bool RegisterApplicationEvents(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) {
	if (!execution || !execution->Owner().identity || !execution->Owner().token) { return false; }
	auto lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
	if (!lease) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_event_mutex);
	for (auto it = g_application_events.begin(); it != g_application_events.end();) {
		if (it->second.execution.expired()) { it = g_application_events.erase(it); } else { ++it; }
	}
	const auto owner = execution->Owner();
	const auto it = g_application_events.find(owner.token);
	if (it != g_application_events.end()) {
		return it->second.owner.identity == owner.identity && it->second.execution.lock() == execution &&
		       lease.Publish([] {});
	}
	return lease.Publish([&] { g_application_events.emplace(owner.token, ApplicationEvents {owner, execution, {}}); });
}

bool ReleaseApplicationEvents(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) {
	if (!execution || !execution->StopRequested() || execution->Outstanding() != 0) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_event_mutex);
	const auto owner = execution->Owner();
	const auto it = g_application_events.find(owner.token);
	if (it == g_application_events.end()) { return true; }
	if (it->second.owner.identity != owner.identity || it->second.execution.lock() != execution) { return false; }
	g_application_events.erase(it);
	return true;
}

bool NotifyApplicationEntitlementUpdate(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) {
	return WithApplicationEvents(execution, [](auto& events) {
		events.push_back(EntitlementUpdate);
		return OK;
	}) == OK;
}

void NotifyEntitlementUpdate() {
	// Keep the existing host API; TLS determines the source subscription. An app
	// update must not enter the shell queue, including after stop or failed setup.
	(void)WithCurrentEvents([](auto& events) {
		events.push_back(EntitlementUpdate);
		return OK;
	});
}

struct SystemServiceDisplaySafeAreaInfo {
	float   ratio;
	uint8_t reserved[128];
};

struct SystemServiceHdrToneMapLuminance {
	float max_full_frame_tone_map_luminance;
	float max_tone_map_luminance;
	float min_tone_map_luminance;
};

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceHideSplashScreen() {
	PRINT_NAME();
	Graphics::HideSplashScreen();
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceLoadExec(const char* path, const char* const argv[]) {
	PRINT_NAME();
	return Emulator::LoadExec(path, argv);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceParamGetInt(int param_id, int* value) {
	PRINT_NAME();

	if (value == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	int v = 0;

	switch (param_id) {
		case PARAM_ID_LANG:
		case PARAM_ID_LANG_PS5: v = static_cast<int>(Config::GetConsoleLanguage()); break;
		case PARAM_ID_DATE_FORMAT: v = PARAM_DATE_FORMAT_DDMMYYYY; break;
		case PARAM_ID_TIME_FORMAT: v = PARAM_TIME_FORMAT_24HOUR; break;
		case PARAM_ID_TIME_ZONE: v = +180; break;
		case PARAM_ID_SUMMERTIME: v = 0; break;
		case PARAM_ID_GAME_PARENTAL_LEVEL: v = PARAM_GAME_PARENTAL_OFF; break;
		case PARAM_ID_CC_ENABLE: v = PARAM_CC_DISABLED; break;
		case PARAM_ID_SCREEN_READER: v = 0; break;
		case PARAM_ID_ENTER_BUTTON_ASSIGN: v = PARAM_ENTER_BUTTON_ASSIGN_CROSS; break;
		default:
			LOGF_COLOR(Log::Color::Yellow, " unsupported param_id %d, using 0\n", param_id);
			break;
	}

	LOGF(" %d = %d\n", param_id, v);

	*value = v;

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceParamGetString(int param_id, char* buf, size_t buf_size) {
	PRINT_NAME();

	// Native 13.60 +4bd7..+4bf7: only selector 6, minimum capacity 65.
	if (buf == nullptr || buf_size < MAX_SYSTEM_NAME_LENGTH) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	const char* value = nullptr;
	switch (param_id) {
		case PARAM_ID_SYSTEM_NAME: value = "Kyty"; break;
		default: return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	const auto len = std::strlen(value);
	if (len + 1 > buf_size) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	std::memcpy(buf, value, len + 1);
	buf[MAX_SYSTEM_NAME_LENGTH - 1] = '\0';
	LOGF(" %d = %s\n", param_id, value);

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceReceiveEvent(SystemServiceEvent* event) {
	PRINT_NAME();

	if (event == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	int32_t event_type = -1;
	const int result = WithCurrentEvents([&](auto& events) {
		if (events.empty()) { return SYSTEM_SERVICE_ERROR_NO_EVENT; }
		event_type = events.front();
		events.pop_front();
		return OK;
	});
	if (result != OK && result != SYSTEM_SERVICE_ERROR_NO_EVENT) { return result; }
	event->event_type = event_type;
	std::memset(event->data, 0, sizeof(event->data));
	if (result == OK) { LOGF("\t SystemService event = 0x%08x\n", static_cast<uint32_t>(event_type)); }
	return result;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceGetStatus(SystemServiceStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	const auto dialog = Dialog::SystemDialog::GetVisualState();
	const bool vsh_overlaid =
	    LncUtil::Lnc::IsSystemUiOverlaid(Loader::RuntimeLinker::GetCurrentProgram());
	SystemServiceStatus result;
	result.is_system_ui_overlaid      = dialog.active || vsh_overlaid;
	// Hiding the embedded child on Home is background execution. Controller
	// focus alone does not imply an overlay; that needs a real visual producer.
	result.is_in_background_execution = dialog.background || vsh_overlaid || Loader::RuntimeChildInBackground();
	const int event_result = WithCurrentEvents([&](auto& events) {
		result.event_num = static_cast<int32_t>(events.size());
		return OK;
	});
	if (event_result != OK) { return event_result; }
	*status = result;
	return OK;
}

static int KYTY_SYSV_ABI
SystemServiceGetDisplaySafeAreaInfo(SystemServiceDisplaySafeAreaInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	info->ratio = 1.0f;

	return OK;
}

static int KYTY_SYSV_ABI
SystemServiceGetHdrToneMapLuminance(SystemServiceHdrToneMapLuminance* luminance) {
	PRINT_NAME();

	if (luminance == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	luminance->max_full_frame_tone_map_luminance = 80.0f;
	luminance->max_tone_map_luminance            = 1000.0f;
	luminance->min_tone_map_luminance            = 0.0f;

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceGetNoticeScreenSkipFlag(bool* value) {
	PRINT_NAME();

	if (value == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	*value = Config::SkipNoticeScreen();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceDisableNoticeScreenSkipFlagAutoSet() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceSetNoticeScreenSkipFlag() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServicePowerTick() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceDisableMusicPlayer() {
	return OK;
}

// The counterpart of sceSystemServiceDisableMusicPlayer(). A title can disable the system
// background music player, and this call re-enables it. It's implemented whit a stub and returns 0.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceSystemServiceReenableMusicPlayer() {
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceReportAbnormalTermination(const void* info) {
	PRINT_NAME();

	LOGF("\t info = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));

	return OK;
}

// ps5-vsh: NPXS40038 (SceVideoCore4K) probes USB mass storage on startup.
// There is no USB backend, but reporting success gets the applet past its
// init gate instead of aborting the title.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceUsbStorageInit() {
	PRINT_NAME();
	return OK;
}

static void KYTY_SYSV_ABI SystemServiceInitializePlayerDialogParam() {
	PRINT_NAME();
}

static int KYTY_SYSV_ABI SystemServiceLaunchPlayerDialog() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceUsbStorageTerm() {
	PRINT_NAME();

	return OK;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// ps5-vsh: bounded guest-string probe (canary discipline: never fault on a
// stale pointer; same shape as libShellHits.cpp ProbeGuest).
static bool DeeplinkProbe(uint64_t addr, size_t len) {
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
static bool DeeplinkProbe(uint64_t addr, size_t len) {
	return addr != 0 && len != 0;
}
#endif

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceLaunchSystemDeeplink(int user_id, const char* param) {
	PRINT_NAME();

	// ps5-vsh: Q5icOdSR3v8. Launches a system feature via a JSON param
	// string (e.g. {"functionName":"gamehelp",...}). No launcher backend
	// exists yet, so validated calls report success without launching.
	if (user_id != Config::GetUserId()) {
		LOGF("\t user_id = %d (want %d) -> INCORRECT_USER_ID\n", user_id,
		     static_cast<int>(Config::GetUserId()));
		return SYSTEM_SERVICE_ERROR_INCORRECT_USER_ID;
	}
	if (param == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
	// Bounded NUL scan (INVALID_PARAM_LEN when over-long, per reference).
	static constexpr size_t kMaxParamLen = 16u * 1024u;
	const uint64_t          base         = reinterpret_cast<uint64_t>(param);
	size_t                  len          = 0;
	while (len < kMaxParamLen && DeeplinkProbe(base + len, 1) &&
	       reinterpret_cast<const char*>(base)[len] != '\0') {
		len++;
	}
	if (len >= kMaxParamLen) {
		LOGF("\t param over %llu bytes -> INVALID_PARAM_LEN\n",
		     static_cast<unsigned long long>(kMaxParamLen));
		return SYSTEM_SERVICE_ERROR_INVALID_PARAM_LEN;
	}
	if (!DeeplinkProbe(base, len + 1)) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
	const std::string json(reinterpret_cast<const char*>(base), len);
	// Naive functionName pick for the log (no JSON dep in this TU).
	std::string feature;
	const char* key = "\"functionName\"";
	const size_t kp = json.find(key);
	if (kp != std::string::npos) {
		const size_t colon = json.find(':', kp + 8);
		if (colon != std::string::npos) {
			const size_t q0 = json.find('"', colon + 1);
			if (q0 != std::string::npos) {
				const size_t q1 = json.find('"', q0 + 1);
				if (q1 != std::string::npos) {
					feature = json.substr(q0 + 1, q1 - q0 - 1);
				}
			}
		}
	}
	LOGF("\t user_id = %d feature = \"%s\" json %.200s -> OK (no launcher backend)\n",
	     user_id, feature.c_str(), json.c_str());
	return OK;
}

} // namespace SystemService

namespace SystemGesture {

LIB_VERSION("SystemGesture", 1, "SystemGesture", 1, 1);

constexpr int SYSTEM_GESTURE_INPUT_TOUCH_PAD = 0;

constexpr int SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT     = -2138505215; /* 0x80890001 */
constexpr int SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND = -2138505211; /* 0x80890005 */
constexpr int SYSTEM_GESTURE_ERROR_INVALID_HANDLE       = -2138505210; /* 0x80890006 */

constexpr int32_t SYSTEM_GESTURE_HANDLE = 1;

struct SystemGestureVector2 {
	float x;
	float y;
};

struct SystemGesturePrimitiveTouchEvent {
	int32_t              event_state;
	uint16_t             primitive_id;
	uint8_t              is_updated;
	uint8_t              reserved0;
	SystemGestureVector2 pressed_position;
	SystemGestureVector2 current_position;
	SystemGestureVector2 delta_vector;
	uint64_t             delta_time;
	uint64_t             elapsed_time;
	uint8_t              reserve[32];
};

struct SystemGestureRectangle {
	float   x;
	float   y;
	float   width;
	float   height;
	uint8_t reserve[8];
};

struct SystemGestureTouchRecognizer {
	uint64_t reserve[361];
};

struct SystemGestureTouchRecognizerInformation {
	int32_t                gesture_type;
	SystemGestureRectangle rectangle;
	uint64_t               updated_time;
	uint8_t                reserve[256];
};

struct SystemGestureTouchEvent {
	uint8_t reserve[168];
};

static bool SystemGestureIsValidHandle(int32_t gesture_handle) {
	return gesture_handle == SYSTEM_GESTURE_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureInitializePrimitiveTouchRecognizer(const void*) {
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureFinalizePrimitiveTouchRecognizer() {
	return OK;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureOpen(int32_t input_type, const void*) {
	if (input_type != SYSTEM_GESTURE_INPUT_TOUCH_PAD) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	return SYSTEM_GESTURE_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureClose(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureResetPrimitiveTouchRecognizer(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureUpdatePrimitiveTouchRecognizer(int32_t gesture_handle,
                                                                     const void*) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEvents(
    int32_t gesture_handle, SystemGesturePrimitiveTouchEvent* event_buffer,
    uint32_t capacity_of_buffer, uint32_t* number_of_event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (number_of_event == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	*number_of_event = 0;
	if (event_buffer != nullptr && capacity_of_buffer != 0) {
		std::memset(event_buffer, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventsCount(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? 0 : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventByIndex(
    int32_t gesture_handle, uint32_t, SystemGesturePrimitiveTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventByPrimitiveID(
    int32_t gesture_handle, uint16_t, SystemGesturePrimitiveTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int KYTY_SYSV_ABI
SystemGestureCreateTouchRecognizer(int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer,
                                   int32_t, const SystemGestureRectangle*, const void*) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	std::memset(recognizer, 0, sizeof(SystemGestureTouchRecognizer));

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureAppendTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureRemoveTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureResetTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer != nullptr) {
		std::memset(recognizer, 0, sizeof(SystemGestureTouchRecognizer));
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetTouchRecognizerInformation(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer,
    SystemGestureTouchRecognizerInformation* information) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || information == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	std::memset(information, 0, sizeof(SystemGestureTouchRecognizerInformation));

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureUpdateTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureUpdateAllTouchRecognizer(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureUpdateTouchRecognizerRectangle(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer,
    const SystemGestureRectangle* rectangle) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || rectangle == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetTouchEvents(int32_t gesture_handle,
                                                     const SystemGestureTouchRecognizer* recognizer,
                                                     SystemGestureTouchEvent* event_buffer,
                                                     uint32_t                 capacity_of_buffer,
                                                     uint32_t*                number_of_event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || number_of_event == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	*number_of_event = 0;
	if (event_buffer != nullptr && capacity_of_buffer != 0) {
		std::memset(event_buffer, 0, sizeof(SystemGestureTouchEvent));
	}

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetTouchEventsCount(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? 0 : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetTouchEventByIndex(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer, uint32_t,
    SystemGestureTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGestureTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemGestureGetTouchEventByEventID(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer, uint32_t,
    SystemGestureTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGestureTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

LIB_DEFINE(InitSystemGesture_1) {
	LIB_FUNC("qpo-mEOwje0", SystemGesture::SystemGestureOpen);
	LIB_FUNC("j4yXIA2jJ68", SystemGesture::SystemGestureClose);
	LIB_FUNC("3pcAvmwKCvM", SystemGesture::SystemGestureInitializePrimitiveTouchRecognizer);
	LIB_FUNC("3QYCmMlOlCY", SystemGesture::SystemGestureFinalizePrimitiveTouchRecognizer);
	LIB_FUNC("o11J529VaAE", SystemGesture::SystemGestureResetPrimitiveTouchRecognizer);
	LIB_FUNC("GgFMb22sbbI", SystemGesture::SystemGestureUpdatePrimitiveTouchRecognizer);
	LIB_FUNC("L8YmemOeSNY", SystemGesture::SystemGestureGetPrimitiveTouchEvents);
	LIB_FUNC("JhwByySf9FY", SystemGesture::SystemGestureGetPrimitiveTouchEventsCount);
	LIB_FUNC("KAeP0+cQPVU", SystemGesture::SystemGestureGetPrimitiveTouchEventByIndex);
	LIB_FUNC("yBaQ0h9m1NM", SystemGesture::SystemGestureGetPrimitiveTouchEventByPrimitiveID);
	LIB_FUNC("FWF8zkhr854", SystemGesture::SystemGestureCreateTouchRecognizer);
	LIB_FUNC("1MMK0W-kMgA", SystemGesture::SystemGestureAppendTouchRecognizer);
	LIB_FUNC("ELvBVG-LKT0", SystemGesture::SystemGestureRemoveTouchRecognizer);
	LIB_FUNC("oBuH3zFWYIg", SystemGesture::SystemGestureResetTouchRecognizer);
	LIB_FUNC("0KrW5eMnrwY", SystemGesture::SystemGestureGetTouchRecognizerInformation);
	LIB_FUNC("j4h82CQWENo", SystemGesture::SystemGestureUpdateTouchRecognizer);
	LIB_FUNC("wPJGwI2RM2I", SystemGesture::SystemGestureUpdateAllTouchRecognizer);
	LIB_FUNC("4WOA1eTx3V8", SystemGesture::SystemGestureUpdateTouchRecognizerRectangle);
	LIB_FUNC("fLTseA7XiWY", SystemGesture::SystemGestureGetTouchEvents);
	LIB_FUNC("h8uongcBNVs", SystemGesture::SystemGestureGetTouchEventsCount);
	LIB_FUNC("TSKvgSz5ChU", SystemGesture::SystemGestureGetTouchEventByIndex);
	LIB_FUNC("lpsXm7tzeoc", SystemGesture::SystemGestureGetTouchEventByEventID);
}

} // namespace SystemGesture

namespace SystemServiceYouTubeDeviceAuth {
LIB_VERSION("SystemServiceYouTubeDeviceAuth", 1, "SystemService", 1, 1);

static int KYTY_SYSV_ABI GetCertificationScope(char* output, size_t size) {
	// 13.60 +0x3980 / 4.03 +0x39f0: a constant scope getter, separate
	// from the service-backed certification signing operation.
	constexpr char scope[] = "1p-cobalt-ps5";
	if (output == nullptr || size < sizeof(scope)) { return static_cast<int>(0x80a10003u); }
	std::memcpy(output, scope, sizeof(scope));
	return 0;
}

LIB_DEFINE(InitDeviceAuthScope) {
	LIB_FUNC("KgiwSrROsIQ", GetCertificationScope);
}
static int KYTY_SYSV_ABI SignCertificationMessage(const char* algorithm, size_t algorithm_size,
                                                 const void* message, size_t message_size,
                                                 void* digest, size_t digest_capacity) {
	// 13.60 +0x39c0 / 4.03 +0x3a20 validate byte spans, then obtain the
	// signing service. Its absence maps to INTERNAL before touching output.
	if (algorithm == nullptr || algorithm_size < 11 || message == nullptr || message_size == 0 ||
	    message_size > 128 || digest == nullptr || digest_capacity < 32 ||
	    std::memcmp(algorithm, "HMAC-SHA256", 11) != 0) {
		return static_cast<int>(0x80a10003u);
	}
	return static_cast<int>(0x80a10001u);
}
LIB_DEFINE(InitDeviceAuthSigning) {
	LIB_FUNC("Oxh0CSsTZ78", SignCertificationMessage);
}
} // namespace SystemServiceYouTubeDeviceAuth

LIB_DEFINE(InitSystemService_1) {
	LIB_FUNC("JoBqSQt1yyA", SystemService::SystemServiceLoadExec);
	LIB_FUNC("Vo5V8KAwCmk", SystemService::SystemServiceHideSplashScreen);
	LIB_FUNC("fZo48un7LK4", SystemService::SystemServiceParamGetInt);
	LIB_FUNC("SsC-m-S9JTA", SystemService::SystemServiceParamGetString);
	LIB_FUNC("656LMQSrg6U", SystemService::SystemServiceReceiveEvent);
	LIB_FUNC("rPo6tV8D9bM", SystemService::SystemServiceGetStatus);
	LIB_FUNC("1n37q1Bvc5Y", SystemService::SystemServiceGetDisplaySafeAreaInfo);
	LIB_FUNC("mPpPxv5CZt4", SystemService::SystemServiceGetHdrToneMapLuminance);
	LIB_FUNC("3RQ5aQfnstU", SystemService::SystemServiceGetNoticeScreenSkipFlag);
	LIB_FUNC("8Lo6Zv94aho", SystemService::SystemServiceDisableNoticeScreenSkipFlagAutoSet);
	LIB_FUNC("Q3utJvma4Mo", SystemService::SystemServiceSetNoticeScreenSkipFlag);
	LIB_FUNC("XbbJC3E+L5M", SystemService::SystemServicePowerTick);
	LIB_FUNC("x1UB9bwDSOw", SystemService::SystemServiceDisableMusicPlayer);
	LIB_FUNC("9kPCz7Or+1Y", SystemService::sceSystemServiceReenableMusicPlayer);
	LIB_FUNC("3s8cHiCBKBE", SystemService::SystemServiceReportAbnormalTermination);
	// ps5-vsh: NPXS40038 (SceVideoCore4K) probes USB storage on startup.
	// No USB mass-storage backend exists, but reporting success keeps the
	// applet past its init gate.
	LIB_FUNC("vv9n14vQtmo", SystemService::SystemServiceUsbStorageInit);
	LIB_FUNC("jQcQgA0GJ6k", SystemService::SystemServiceUsbStorageTerm);
	// ps5-vsh: Q5icOdSR3v8 sceSystemServiceLaunchSystemDeeplink (NID from
	// NID db, libSceSystemService; validated log-and-succeed, no launcher).
	LIB_FUNC("Q5icOdSR3v8", SystemService::SystemServiceLaunchSystemDeeplink);
	LIB_FUNC("m5CYKX20wfg", SystemService::SystemServiceInitializePlayerDialogParam);
	LIB_FUNC("uaieF+glFPs", SystemService::SystemServiceLaunchPlayerDialog);
	SystemGesture::InitSystemGesture_1(s);
	SystemServiceYouTubeDeviceAuth::InitDeviceAuthScope(s);
	SystemServiceYouTubeDeviceAuth::InitDeviceAuthSigning(s);
}

} // namespace Libs
