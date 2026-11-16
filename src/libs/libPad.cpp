#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/libMbusDevice.h"
#include "libs/padData.h"
#include "loader/symbolDatabase.h"

#include <SDL3/SDL.h>
#include <cstring>

namespace Libs {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetVibrationMode(int handle, int mode) {
	PRINT_NAME();

	LOGF("\t handle      = %d\n"
	     "\t mode        = %d\n",
	     handle, mode);

	return Controller::PadCheckHandle(handle);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(bool enabled) {
	PRINT_NAME();

	LOGF("\t enabled = %d\n", enabled ? 1 : 0);

	return OK;
}

struct PadDeviceClassExtendedInformation {
	int32_t device_class;
	uint8_t reserved[4];
	uint8_t class_data[12];
};

struct PadDeviceClassData {
	int32_t device_class;
	bool    data_valid;
	uint8_t reserved[3];
	union {
		struct {
			float    steering_wheel_angle;
			uint16_t steering_wheel;
			uint16_t accelerator_pedal;
			uint16_t brake_pedal;
			uint16_t clutch_pedal;
			uint16_t hand_brake;
			uint8_t  gear;
			uint8_t  reserved[1];
		} steering_wheel;
		struct {
			uint8_t data_len;
			uint8_t reserved[3];
			uint8_t data[12];
		} others;
		uint8_t raw[16];
	} class_data;
};

static_assert(sizeof(PadDeviceClassData) == 24);

static int KYTY_SYSV_ABI
PadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info) {
	PRINT_NAME();

	constexpr int pad_error_invalid_arg = -2137915391; /* 0x80920001 */

	if (const auto error = Controller::PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (info == nullptr) {
		return pad_error_invalid_arg;
	}

	std::memset(info, 0, sizeof(*info));

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadDeviceClassParseData(int handle, const Controller::PadData* data,
                                                 PadDeviceClassData* class_data) {
	PRINT_NAME();

	constexpr int pad_error_invalid_arg = -2137915391; /* 0x80920001 */

	LOGF("\t handle     = %d\n"
	     "\t data       = 0x%016" PRIx64 "\n"
	     "\t class_data = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(data), reinterpret_cast<uint64_t>(class_data));

	if (const auto error = Controller::PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (data == nullptr || class_data == nullptr) {
		return pad_error_invalid_arg;
	}

	std::memset(class_data, 0, sizeof(*class_data));
	class_data->device_class = 0; // SCE_PAD_DEVICE_CLASS_STANDARD
	class_data->data_valid   = data->connected;

	if (data->device_unique_data_len > 0) {
		const auto data_len =
		    (data->device_unique_data_len > 12 ? 12 : data->device_unique_data_len);
		class_data->device_class = -1; // SCE_PAD_DEVICE_CLASS_INVALID/unsupported device payload
		class_data->class_data.others.data_len = data_len;
		std::memcpy(class_data->class_data.others.data, data->device_unique_data, data_len);
	}

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetTiltCorrectionState(int handle, bool enabled) {
	PRINT_NAME();

	LOGF("\t handle  = %d\n"
	     "\t enabled = %d\n",
	     handle, enabled ? 1 : 0);

	return Controller::PadCheckHandle(handle);
}

// PPSA02385
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadOpenExtStub(int user_id, int type, int index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	return OK;
}

// PPSA02385
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadGetExtControllerInformationStub(int handle, void* info) {
	PRINT_NAME();

	LOGF("\t handle = %d\n"
	     "\t info   = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(info));

	return Controller::PadCheckHandle(handle);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadUnknownN3kSX62fgNo(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                               uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	LOGF("\t argc-style args = (0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64
	     ", 0x%016" PRIx64 ", 0x%016" PRIx64 ")\n",
	     arg0, arg1, arg2, arg3, arg4, arg5);

	if (arg0 >= 0x10000 && arg3 > 0 && arg3 <= 0x1000) {
		std::memset(reinterpret_cast<void*>(arg0), 0, static_cast<size_t>(arg3));
	}

	return OK;
}

// Device/open/read family verified against 13.60: the keyboard/mouse device
// uses the MBUS DualSense identity, supported-feature bits and the 112-byte
// privileged read layout.
namespace {
constexpr int kPadInvalidArg    = -2137915391; // 0x80920001
constexpr int kPadInvalidHandle = -2137915389; // 0x80920003

int GetPhysicalControllerId() {
	const int id = Controller::GetActiveControllerId();
	return id == Controller::HOST_INPUT_CONTROLLER_ID ? -1 : id;
}

int OpenPrimaryDevicePort() {
	return Controller::OpenSystemDevicePort();
}
} // namespace

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadDeviceOpen(uint64_t device_id, int type, int index) {
	const int id = GetPhysicalControllerId();
	// Bounded call logging: device-id space is guest-enumerated and was
	// previously invisible (both error paths returned silently).
	static int open_logs = 0;
	if (open_logs < 16) {
		open_logs++;
		LOGF("\t PadDeviceOpen call: device_id=0x%016" PRIx64 " type=%d index=%d phys=%d\n",
		     device_id, type, index, id);
	}
	if (id < 0) {
		// This is the same DualSense (MBUS type 22 / native Bond) advertised by
		// enumeration. Other device-class probes must not open aliases of it.
		if (device_id != Mbus::kVirtualPadDeviceId) { return kPadInvalidHandle; }
		if (type != Mbus::kVirtualPadOpenType || index != 0) { return kPadInvalidArg; }
		static int host_opens = 0;
		if (host_opens < 8) {
			host_opens++;
			LOGF("\t PadDeviceOpen: keyboard/mouse PS5 DualSense (device_id=0x%016" PRIx64
			     ") -> handle 1\n",
			     device_id);
		}
		return OpenPrimaryDevicePort();
	}
	if (device_id != static_cast<uint64_t>(id)) {
		return kPadInvalidHandle;
	}
	if (type != 0 || index != 0) {
		return kPadInvalidArg;
	}
	LOGF("\t PadDeviceOpen: physical controller %d -> handle 1\n", id);
	return OpenPrimaryDevicePort();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetUserNumber(int device_id, int number) {
	if (device_id < 0 || device_id != GetPhysicalControllerId()) {
		return kPadInvalidHandle;
	}
	if (number < 1 || number > 5) {
		return kPadInvalidArg;
	}
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(device_id));
	    pad != nullptr) {
		if (!SDL_SetGamepadPlayerIndex(pad, number - 1)) {
			LOGF("\t PadSetUserNumber: SDL player indicator unavailable: %s\n", SDL_GetError());
		}
	}
	LOGF("\t PadSetUserNumber: device=%d number=%d\n", device_id, number);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadGetCapability(uint64_t device_id, uint32_t* capability) {
	if (capability == nullptr) {
		return kPadInvalidArg;
	}
	const int id = GetPhysicalControllerId();
	if (id < 0) {
		if (device_id != Mbus::kVirtualPadDeviceId) { return kPadInvalidHandle; }
		// Supported features: motion, vibration, lightbar, player LED, touchpad.
		// Model identity is MBUS type 22 (DualSense), not this feature bitmask.
		*capability = 0x1f;
		return OK;
	}
	if (device_id != static_cast<uint64_t>(id)) {
		return kPadInvalidHandle;
	}
	// Motion, vibration, lightbar, player LED and touchpad.
	*capability = 0x1f;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadUiReadState(int handle, void* data) {
	if (data == nullptr) {
		return kPadInvalidArg;
	}
	Controller::PadData state {};
	const int result = Controller::PadReadSystemState(handle, &state);
	if (result != OK) {
		static thread_local unsigned failures = 0;
		if (failures++ < 8) {
			LOGF("\t PadUiReadState: handle=%d error=0x%08x\n", handle,
			     static_cast<uint32_t>(result));
		}
		return result;
	}
	// The privileged UI ABI is 112 bytes: the common pad prefix through the
	// timestamp, but connected_count is at byte 77 rather than byte 106.
	std::memset(data, 0, 112);
	std::memcpy(data, &state, 88);
	static_cast<uint8_t*>(data)[77] = state.connected_count;
	static thread_local uint32_t last_buttons   = UINT32_MAX;
	static thread_local int      last_connected = -1;
	if (last_buttons != state.buttons || last_connected != static_cast<int>(state.connected)) {
		LOGF("\t PadUiReadState: handle=%d connected=%d buttons=0x%08x\n", handle,
		     static_cast<int>(state.connected), state.buttons);
		last_buttons   = state.buttons;
		last_connected = static_cast<int>(state.connected);
	}
	return OK;
}

static int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadGetIdleCount(int handle) {
	(void)handle;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetProcessPrivilege(int privilege) {
	PRINT_NAME();
	LOGF("\t [DmBx8K+jDWw] scePadSetProcessPrivilege privilege=%d\n", privilege);
	return 0;
}

LIB_DEFINE(InitPad_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("hv1luiJrqQM", Controller::PadInit);
	LIB_FUNC("xk0AcarP3V4", Controller::PadOpen);
	LIB_FUNC("WFIiSfXGUq8", PadOpenExtStub);
	LIB_FUNC("u1GRHp+oWoY", Controller::PadGetHandle);
	LIB_FUNC("AcslpN1jHR8", PadDeviceClassGetExtendedInformation);
	LIB_FUNC("IHPqcbc0zCA", PadDeviceClassParseData);
	LIB_FUNC("clVvL4ZDntw", Controller::PadSetMotionSensorState);
	LIB_FUNC("r44mAxdSG+U", Controller::PadSetAngularVelocityDeadbandState);
	LIB_FUNC("rIZnR6eSpvk", Controller::PadResetOrientation);
	LIB_FUNC("vDLMoJLde8I", PadSetTiltCorrectionState);
	LIB_FUNC("gjP9-KQzoUk", Controller::PadGetControllerInformation);
	LIB_FUNC("fCWdlnmB1Ks", Controller::PadIsRemoteController);
	LIB_FUNC("hGbf2QTBmqc", PadGetExtControllerInformationStub);
	LIB_FUNC("2JgFB2n9oUM", Controller::PadSetTriggerEffect);
	LIB_FUNC("YndgXqQVV7c", Controller::PadReadState);
	LIB_FUNC("q1cHNfGycLI", Controller::PadRead);
	LIB_FUNC("yFVnOdGxvZY", Controller::PadSetVibration);
	LIB_FUNC("W2G-yoyMF5U", PadSetVibrationMode);
	LIB_FUNC("Yq0zOH7YNOM", PadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse);
	LIB_FUNC("znaWI0gpuo8", Controller::PadGetTriggerEffectState);
	LIB_FUNC("DscD1i9HX1w", Controller::PadResetLightBar);
	LIB_FUNC("RR4novUEENY", Controller::PadSetLightBar);
	LIB_FUNC("6ncge5+l5Qs", Controller::PadClose);
	LIB_FUNC("n3kSX62fgNo", PadUnknownN3kSX62fgNo);
	LIB_FUNC("d7bXuEBycDI", PadDeviceOpen); // scePadDeviceOpen
	LIB_FUNC("iOqpYvGEo6A", PadSetUserNumber); // scePadSetUserNumber
	LIB_FUNC("qtasqbvwgV4", PadGetCapability); // scePadGetCapability
	LIB_FUNC("FYqcaosYiIo", PadUiReadState); // scePadUiReadState
	LIB_FUNC("kiA9bZhbnAg", PadGetIdleCount); // scePadGetIdleCount
	LIB_FUNC("DmBx8K+jDWw", PadSetProcessPrivilege); // scePadSetProcessPrivilege
	LIB_FUNC("iA-DdobUen8", Controller::PadVrControllerReadState);
	LIB_FUNC("v8P+9PRqg10", Controller::PadVrControllerSetTriggerEffects);
}

namespace LibMouse {

LIB_VERSION("Mouse", 1, "Mouse", 1, 1);

namespace Mouse {

constexpr int MOUSE_ERROR_INVALID_ARG    = -2132869119; /* 0x80DF0001 */
constexpr int MOUSE_ERROR_INVALID_HANDLE = -2132869117; /* 0x80DF0003 */

struct MouseData {
	uint64_t timestamp;
	bool     connected;
	uint8_t  padding[3];
	uint32_t buttons;
	int32_t  x_axis;
	int32_t  y_axis;
	int32_t  wheel;
	int32_t  tilt;
	uint8_t  reserved[8];
};

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MouseInit() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MouseOpen(int user_id, int32_t type, int32_t index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %" PRId32 "\n"
	     "\t index   = %" PRId32 "\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	if (type != 0 || index < 0 || index >= 2) {
		return MOUSE_ERROR_INVALID_ARG;
	}

	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MouseClose(int32_t handle) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n", handle);

	if (handle <= 0) {
		return MOUSE_ERROR_INVALID_HANDLE;
	}

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MouseRead(int32_t handle, MouseData* data, int32_t num) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n"
	     "\t num    = %" PRId32 "\n",
	     handle, reinterpret_cast<uint64_t>(data), num);

	if (handle <= 0) {
		return MOUSE_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0) {
		return MOUSE_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(MouseData) * static_cast<size_t>(num));

	return 0;
}

} // namespace Mouse

LIB_DEFINE(InitMouse_1) {
	LIB_FUNC("cAnT0Rw-IwU", Mouse::MouseClose);
	LIB_FUNC("Qs0wWulgl7U", Mouse::MouseInit);
	LIB_FUNC("RaqxZIf6DvE", Mouse::MouseOpen);
	LIB_FUNC("x8qnXqh-tiM", Mouse::MouseRead);
}

} // namespace LibMouse

namespace LibKeyboard {

LIB_VERSION("Keyboard", 1, "Keyboard", 1, 1);

namespace Keyboard {

constexpr int KEYBOARD_ERROR_INVALID_ARG    = -2133196799; /* 0x80da0001 */
constexpr int KEYBOARD_ERROR_INVALID_HANDLE = -2133196797; /* 0x80da0003 */
constexpr int KEYBOARD_MAX_KEYCODES         = 16;
constexpr int KEYBOARD_MAX_DATA_NUM         = 16;
constexpr int KEYBOARD_MAPPING_101          = 0;
constexpr int KEYBOARD_MAPPING_106          = 1;

struct KeyboardData {
	uint64_t timestamp;
	bool     intercepted;
	uint8_t  reserve1[7];
	bool     connected;
	int32_t  length;
	uint32_t led;
	uint32_t modifier_key;
	uint16_t key_code[KEYBOARD_MAX_KEYCODES];
	uint8_t  reserve2[32];
};

struct KeyboardCharData {
	bool     processed;
	int32_t  length;
	uint16_t char_code;
	uint8_t  reserve[8];
};

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardInit() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardOpen(int user_id, int32_t type, int32_t index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %" PRId32 "\n"
	     "\t index   = %" PRId32 "\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	if (type != 0 || index < 0 || index >= 2) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardClose(int32_t handle) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n", handle);

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardRead(int32_t handle, KeyboardData* data, int32_t num) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n"
	     "\t num    = %" PRId32 "\n",
	     handle, reinterpret_cast<uint64_t>(data), num);

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0 || num > KEYBOARD_MAX_DATA_NUM) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(KeyboardData) * static_cast<size_t>(num));

	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardReadState(int32_t handle, KeyboardData* data) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(data));

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(KeyboardData));

	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KeyboardGetKey2Char(int32_t handle, int32_t arrange, uint32_t led,
                                             uint32_t modifier_key, uint16_t key_code,
                                             KeyboardCharData* char_data) {
	PRINT_NAME();

	LOGF("\t handle       = %" PRId32 "\n"
	     "\t arrange      = %" PRId32 "\n"
	     "\t led          = %" PRIu32 "\n"
	     "\t modifier_key = %" PRIu32 "\n"
	     "\t key_code     = %" PRIu16 "\n"
	     "\t char_data    = 0x%016" PRIx64 "\n",
	     handle, arrange, led, modifier_key, key_code, reinterpret_cast<uint64_t>(char_data));

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (char_data == nullptr ||
	    (arrange != KEYBOARD_MAPPING_101 && arrange != KEYBOARD_MAPPING_106)) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(char_data, 0, sizeof(KeyboardCharData));

	return OK;
}

} // namespace Keyboard

LIB_DEFINE(InitKeyboard_1) {
	LIB_FUNC("wadT3QBCGY0", Keyboard::KeyboardInit);
	LIB_FUNC("HJ+KnEHcaxI", Keyboard::KeyboardOpen);
	LIB_FUNC("0LWei+c7RNc", Keyboard::KeyboardClose);
	LIB_FUNC("6HpE68bzX6M", Keyboard::KeyboardReadState);
	LIB_FUNC("xybbGMCr738", Keyboard::KeyboardRead);
	LIB_FUNC("yO9JwdRhtSA", Keyboard::KeyboardGetKey2Char);
}

} // namespace LibKeyboard

} // namespace Libs
