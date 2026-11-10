#include "libs/controller.h"

#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/dualSenseHaptics.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/libs.h"
#include "libs/padData.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeChildClient.h"
#include "graphics/presentation/window/runtimeWindowBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace Libs::Controller {

LIB_NAME("Pad", "Pad");

constexpr int PAD_HANDLE = 1;
static_assert(PAD_HANDLE + Config::MAX_PLAYERS <= PAD_HANDLE_VR_LEFT);

constexpr uint32_t RUMBLE_DURATION_MS = 0xffff;
constexpr uint32_t RELEASE_FLUSH_MS   = 50;

struct PadControllerInformation {
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
	uint8_t  reserve[8];
};

struct PadLightBarParam {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t reserve;
};

struct PadVibrationParam {
	uint8_t large_motor;
	uint8_t small_motor;
};

struct PadTriggerEffectCommand {
	uint32_t mode;
	uint8_t  reserve[4];
	uint8_t  data[48];
};

struct PadTriggerEffectParam {
	uint8_t                 trigger_mask;
	uint8_t                 reserve[7];
	PadTriggerEffectCommand command[2];
};

struct PadTriggerEffectStateInformation {
	int32_t state[2];
};

static_assert(sizeof(PadTriggerEffectCommand) == 56);
static_assert(sizeof(PadTriggerEffectParam) == 120);
static_assert(sizeof(PadTriggerEffectStateInformation) == 8);

struct DualSenseEffects {
	uint8_t enable_bits;
	uint8_t reserve[9];
	uint8_t right_trigger[11];
	uint8_t left_trigger[11];
};

static_assert(sizeof(DualSenseEffects) == 32);

struct ControllerState {
	struct Touch {
		uint8_t  id   = 0;
		bool     down = false;
		uint16_t x    = 0;
		uint16_t y    = 0;
	};

	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
	Touch    touch[2];
	std::array<float, 3> accel {0.0f, 1.0f, 0.0f};
	std::array<float, 3> gyro {};
	std::array<float, 4> orientation {0.0f, 0.0f, 0.0f, 1.0f};
};

namespace Child = Loader::RuntimeChild;
static std::atomic_bool g_runtime_parent {false}, g_runtime_focus {true};
static std::atomic<uint64_t> g_runtime_epoch {0};
static std::atomic<uint64_t> g_runtime_ps_until {0};
static Child::PadReport g_child_raw;
static Child::PadReport g_child_pad;
static Child::PadFocusFilter g_child_filter;
static Child::PadFocusFilter g_child_host_filter;
static uint64_t g_child_host_serial = 0;
static uint64_t g_child_last_input_ms = 0;
static int g_broker_device = -1;
static uint32_t g_broker_connections = 0;
static bool HostWindowEligible() {
	return !Graphics::RuntimeWindowBridgeActive() || Graphics::RuntimeWindowTreeForeground();
}
static ControllerState FromReport(const Child::PadReport& r) {
	ControllerState state; state.time = r.time; state.buttons = r.buttons;
	for (unsigned i = 0; i != r.axes.size(); ++i) { state.axes[i] = r.axes[i]; }
	for (unsigned i = 0; i != r.touch.size(); ++i) { state.touch[i] = {r.touch[i].id, r.touch[i].down != 0, r.touch[i].x, r.touch[i].y}; }
	state.accel = r.accel; state.gyro = r.gyro; state.orientation = r.orientation; return state;
}
static Child::PadReport ToReport(const ControllerState& state, bool connected, int count) {
	Child::PadReport r; r.time = state.time; r.buttons = state.buttons;
	r.connected = connected; r.connections = static_cast<uint32_t>(std::max(count, 0));
	for (unsigned i = 0; i != r.axes.size(); ++i) { r.axes[i] = static_cast<uint8_t>(std::clamp(state.axes[i], 0, 255)); }
	for (unsigned i = 0; i != r.touch.size(); ++i) { r.touch[i] = {state.touch[i].x, state.touch[i].y, state.touch[i].id, static_cast<uint8_t>(state.touch[i].down), 0}; }
	r.accel = state.accel; r.gyro = state.gyro; r.orientation = state.orientation; return r;
}

class GameController {
public:
	explicit GameController(int player): m_player(player) {}

	KYTY_CLASS_NO_COPY(GameController);

	[[nodiscard]] int Handle() const { return PAD_HANDLE + m_player; }

	int InitPort();
	int OpenPort(int user_id, int type, int index, bool shared = false);
	int GetPortHandle(int user_id, int type, int index);
	int ClosePort(int handle);
	int CheckPortHandle(int handle);

	void SetDevices(int active_id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void RightStick(int id, int x, int y);
	void TouchPad(int id, int finger, bool down, float x, float y);
	void Motion(int id, Sensor sensor, const float* data, uint64_t time_us);
	void SetMotionSensorState(bool enable);
	void ResetOrientation();
	void ToggleGyroEmulation(int id);
	void ResetInputState();
	void GetConnectionInfo(bool* flag, int* count);
	void SetVibration(uint8_t large_motor, uint8_t small_motor);
	int  GetActiveControllerId();
	void SetLightBar(uint8_t r, uint8_t g, uint8_t b);
	bool SetTriggerEffect(const PadTriggerEffectParam& param);
	void  GetTriggerEffectState(PadTriggerEffectStateInformation* info);
	void  CycleSetting(Setting setting);
	float GetSettingScale(Setting setting) const;
	void ReadState(ControllerState* state, bool* flag, int* count);
	void ReadRawState(ControllerState* state, bool* flag, int* count);
	void RuntimeFocus(bool enabled, uint64_t epoch);
	void Inject(const Child::PadReport& report);
	void FlushChildOutput();
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;
	enum class PortState { Uninitialized, Closed, Open };

	int CheckPortHandleLocked(int handle) const;
	[[nodiscard]] bool PadOpenArgsAreValid(int user_id, int type, int index) const;

	const int m_player;

	void AddState();
	// Emulated tilt: the right stick drives pitch and roll instead of the stick axes.
	void UpdateEmulatedTilt();
	// Apply cached output without changing its game-requested lifetime; caller holds m_mutex.
	void ApplyVibration();
	bool SendTriggerEffect(const PadTriggerEffectParam& param);
	void UpdateTriggerEffectState();

	Common::Mutex    m_mutex;
	PortState        m_port_state      = PortState::Uninitialized;
	uint32_t         m_port_owners     = 0;
	int              m_active_id       = -1;
	bool             m_connected       = false;
	int              m_connected_count = 0;
	bool             m_motion_enabled  = true;
	uint64_t         m_gyro_time       = 0;
	bool             m_gyro_emulation  = false;
	uint64_t         m_emulated_time   = 0;
	std::array<int, 2> m_emulated_stick {128, 128};
	// Accelerometer gravity direction in the reset frame.
	std::array<float, 3> m_up_reference {0.0f, 1.0f, 0.0f};
	bool                 m_up_reference_valid = false;
	ControllerState  m_state;
	Child::PadOutput m_child_output;
	bool m_child_output_dirty = false;
	void PublishChildOutput(); // Caller holds m_mutex.
	Child::PadFocusFilter m_runtime_filter;
	uint64_t m_runtime_serial = 0, m_runtime_epoch = 0;
	bool m_window_eligible = true;
	ControllerState OrdinaryState();
	ControllerState  m_states[STATES_MAX];
	bool             m_obtained[STATES_MAX] {};
	uint32_t         m_states_num    = 0;
	uint32_t         m_first_state   = 0;
	uint8_t          m_next_touch_id = 1;
	// The game's last requests, re-sent when their intensity setting changes.
	std::array<uint8_t, 2> m_vibration {};
	uint64_t               m_vibration_until = 0;
	PadTriggerEffectParam  m_trigger_effect {};
	PadTriggerEffectStateInformation m_trigger_state {};
	// Setting changes share the output lock; audio only needs an atomic scale snapshot.
	std::array<std::atomic<uint32_t>, 3> m_setting_steps {};
};

static std::array<std::unique_ptr<GameController>, Config::MAX_PLAYERS> g_players;
// The embedded HWND receives keyboard/mouse events locally. Physical devices
// still belong exclusively to the parent; keep these two input sources separate
// so a broker sample cannot overwrite a local key press or mouse gesture.
static std::unique_ptr<GameController> g_child_host;

// Maps host gamepads to players; the keyboard follows the configured keyboard player.
static Common::Mutex g_router_mutex;
static std::vector<std::pair<int, int>> g_pad_players;

static uint32_t g_keyboard_player = 0;

static void RefreshPlayers();
static void StopHostPad(SDL_Gamepad* pad);

static int KeyboardPlayer() {
	return g_keyboard_player == 0 ? 0 : static_cast<int>(g_keyboard_player) - 1;
}

// Caller holds g_router_mutex.
static GameController* PlayerForDevice(int id) {
	if (id == HOST_INPUT_CONTROLLER_ID) {
		return g_players[KeyboardPlayer()].get();
	}
	for (const auto& [pad, player]: g_pad_players) {
		if (pad == id) {
			return player >= 0 ? g_players[player].get() : nullptr;
		}
	}
	return nullptr;
}

static GameController* PlayerForHandle(int handle) {
	if (handle >= PAD_HANDLE && handle < PAD_HANDLE + Config::MAX_PLAYERS) {
		return g_players[handle - PAD_HANDLE].get();
	}
	// The Sense controller ports belong to the initial user.
	return PadVrHand(handle) >= 0 ? g_players[0].get() : nullptr;
}

static void pad_fill_data(PadData* data, const ControllerState& state, bool connected,
                          int connected_count) {
	EXIT_IF(data == nullptr);

	std::memset(data, 0, sizeof(*data));

	data->buttons           = state.buttons;
	data->left_stick_x      = state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y      = state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x     = state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y     = state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2 = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2 = state.axes[static_cast<int>(Axis::TriggerRight)];
	data->acceleration_x     = state.accel[0];
	data->acceleration_y     = state.accel[1];
	data->acceleration_z     = state.accel[2];
	data->angular_velocity_x = state.gyro[0];
	data->angular_velocity_y = state.gyro[1];
	data->angular_velocity_z = state.gyro[2];
	data->orientation_x      = state.orientation[0];
	data->orientation_y      = state.orientation[1];
	data->orientation_z      = state.orientation[2];
	data->orientation_w      = state.orientation[3];
	for (const auto& touch: state.touch) {
		if (touch.down) {
			auto& output = data->touch_data.touch[data->touch_data.touch_num++];
			output.x     = touch.x;
			output.y     = touch.y;
			output.id    = touch.id;
		}
	}
	data->connected              = connected;
	data->timestamp              = state.time;
	data->connected_count        = static_cast<uint8_t>(std::min(connected_count, 255));
	data->device_unique_data_len = 0;
}

// In cycle order, starting at the PS5 default.
constexpr std::array SPEAKER_VOLUME = {1.0f, 0.0f, 0.02f, 0.12f, 0.42f};
constexpr std::array INTENSITY      = {1.0f, 0.0f, 0.33f, 0.66f};

void CycleSetting(Setting setting) {
	for (const auto& player: g_players) {
		if (player != nullptr) {
			player->CycleSetting(setting);
		}
	}
}

float GetSettingScale(Setting setting) {
	return g_players[0] != nullptr ? g_players[0]->GetSettingScale(setting) : 1.0f;
}

void GameController::CycleSetting(Setting setting) {
	Common::LockGuard lock(m_mutex);
	auto&             step = m_setting_steps[static_cast<size_t>(setting)];
	const auto count = setting == Setting::SpeakerVolume ? SPEAKER_VOLUME.size() : INTENSITY.size();
	step.store((step.load(std::memory_order_relaxed) + 1) % count, std::memory_order_relaxed);
	if (setting == Setting::VibrationIntensity) {
		ApplyVibration();
	} else if (setting == Setting::TriggerEffectIntensity) {
		(void)SendTriggerEffect(m_trigger_effect);
	}
}

float GameController::GetSettingScale(Setting setting) const {
	const auto step = m_setting_steps[static_cast<size_t>(setting)].load(std::memory_order_relaxed);
	if (setting == Setting::SpeakerVolume) {
		// The old speaker level matches the PS5 slider's midpoint. Give the upper
		// half of the global slider up to 6 dB of extra software gain.
		return SPEAKER_VOLUME[step] * (Config::GetControllerSpeakerVolume() / 50.0f);
	}
	if (setting == Setting::VibrationIntensity) {
		return INTENSITY[step] * (Config::GetControllerVibrationIntensity() / 100.0f);
	}
	return INTENSITY[step];
}

static uint8_t Scale(uint8_t value, float scale) {
	if (value == 0 || scale == 0.0f) {
		return 0;
	}
	return static_cast<uint8_t>(std::max(1L, std::lround(value * scale)));
}

static bool trigger_effect_zones(uint8_t* effect, const uint8_t* strengths, float scale,
                                 uint8_t type, uint8_t frequency = 0) {
	uint16_t active = 0;
	uint32_t packed = 0;
	for (int i = 0; i < 10; i++) {
		if (strengths[i] > 8) {
			return false;
		}
		const auto strength = Scale(strengths[i], scale);
		if (strength != 0) {
			active |= static_cast<uint16_t>(1u << i);
			packed |= static_cast<uint32_t>(strength - 1u) << (3 * i);
		}
	}

	effect[0] = active != 0 && (type != 0x26 || frequency != 0) ? type : 0x05;
	effect[1] = static_cast<uint8_t>(active);
	effect[2] = static_cast<uint8_t>(active >> 8u);
	effect[3] = static_cast<uint8_t>(packed);
	effect[4] = static_cast<uint8_t>(packed >> 8u);
	effect[5] = static_cast<uint8_t>(packed >> 16u);
	effect[6] = static_cast<uint8_t>(packed >> 24u);
	effect[9] = frequency;
	return true;
}

static bool trigger_effect_to_dualsense(const PadTriggerEffectCommand& command, uint8_t* effect,
                                        float scale) {
	std::memset(effect, 0, 11);
	effect[0] = 0x05;

	uint8_t strengths[10] = {};
	switch (command.mode) {
		case 0: return true;
		case 1:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, scale, 0x21);
		case 2: {
			const auto start    = command.data[0];
			const auto end      = command.data[1];
			const auto strength = command.data[2];
			if (start < 2 || start > 7 || end <= start || end > 8 || strength > 8) {
				return false;
			}
			const auto scaled = Scale(strength, scale);
			if (scaled == 0) {
				return true;
			}
			const uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
			effect[0]            = 0x25;
			effect[1]            = static_cast<uint8_t>(zones);
			effect[2]            = static_cast<uint8_t>(zones >> 8u);
			effect[3]            = scaled - 1u;
			return true;
		}
		case 3:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, scale, 0x26, command.data[2]);
		case 4: return trigger_effect_zones(effect, command.data, scale, 0x21);
		case 5: {
			const int start          = command.data[0];
			const int end            = command.data[1];
			const int start_strength = command.data[2];
			const int end_strength   = command.data[3];
			if (start > 8 || end <= start || end > 9 || start_strength < 1 || start_strength > 8 ||
			    end_strength < 1 || end_strength > 8) {
				return false;
			}
			for (int i = start; i < 10; i++) {
				const int delta  = (end_strength - start_strength) * (i - start);
				const int length = end - start;
				strengths[i]     = static_cast<uint8_t>(
				    i >= end ? end_strength
				             : start_strength +
				                   (delta + (delta < 0 ? -length / 2 : length / 2)) / length);
			}
			return trigger_effect_zones(effect, strengths, scale, 0x21);
		}
		case 6: return trigger_effect_zones(effect, command.data + 1, scale, 0x26, command.data[0]);
		default: return false;
	}
}

enum TriggerState : int32_t {
	TRIGGER_STATE_OFF               = 0,
	TRIGGER_STATE_FEEDBACK_STANDBY  = 1,
	TRIGGER_STATE_FEEDBACK_ACTIVE   = 2,
	TRIGGER_STATE_WEAPON_STANDBY    = 3,
	TRIGGER_STATE_WEAPON_PULLING    = 4,
	TRIGGER_STATE_WEAPON_FIRED      = 5,
	TRIGGER_STATE_VIBRATION_STANDBY = 6,
	TRIGGER_STATE_VIBRATION_ACTIVE  = 7,
};

static int32_t TriggerEffectState(const PadTriggerEffectCommand& command, int value,
                                  int32_t previous) {
	// Trigger positions use nonuniform travel ranges, with endpoints at 0 and 255.
	const int pos = value == 0 ? 0 : value == 255 ? 9 : 1 + value / 32;
	switch (command.mode) {
		case 1: // feedback: resistance from position data[0], strength data[1]
			return command.data[1] != 0 && pos >= command.data[0] ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                                                      : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 2: // weapon: resistance from data[0], released at data[1]
			if (value < command.data[0] * 32) {
				return TRIGGER_STATE_WEAPON_STANDBY;
			}
			return previous == TRIGGER_STATE_WEAPON_FIRED ||
			               value >= std::min(command.data[1] * 32, 254)
			           ? TRIGGER_STATE_WEAPON_FIRED
			           : TRIGGER_STATE_WEAPON_PULLING;
		case 3: // vibration from position data[0], amplitude data[1], frequency data[2]
			return command.data[1] != 0 && command.data[2] != 0 && pos >= command.data[0]
			           ? TRIGGER_STATE_VIBRATION_ACTIVE
			           : TRIGGER_STATE_VIBRATION_STANDBY;
		case 4: // multiple-position feedback: strength per position
			return command.data[pos] != 0 ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                              : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 5: // slope feedback between data[0] and data[1]
			return pos >= command.data[0] && pos < command.data[1] ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                                                       : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 6: // multiple-position vibration: frequency data[0], amplitude per position
			return command.data[0] != 0 && command.data[1 + pos] != 0
			           ? TRIGGER_STATE_VIBRATION_ACTIVE
			           : TRIGGER_STATE_VIBRATION_STANDBY;
		default: return TRIGGER_STATE_OFF;
	}
}

void Initialize() {
	EXIT_IF(g_players[0] != nullptr);
	g_broker_device = -1; g_broker_connections = 0;
	g_runtime_ps_until.store(0);

	for (int i = 0; i < Config::MAX_PLAYERS; i++) {
		g_players[i] = std::make_unique<GameController>(i);
	}
	g_keyboard_player = Config::GetKeyboardPlayer();
	if (!Loader::IsRuntimeChild()) { RefreshPlayers(); }
	else {
		g_child_raw = {}; g_child_pad = {}; g_child_host_serial = 0; g_child_last_input_ms = 0;
		g_child_host = std::make_unique<GameController>(0);
		g_child_host->SetDevices(HOST_INPUT_CONTROLLER_ID);
		g_child_host_filter.Focus(false, 0, {});
	}
	if (g_runtime_parent.load()) { for (const auto& player: g_players) { player->RuntimeFocus(true, 0); } }
}

void Shutdown() {
	EmergencyShutdown();
	g_child_host.reset();
	for (auto& player: g_players) {
		player.reset();
	}
}

void EmergencyShutdown() {
	Common::LockGuard lock(g_router_mutex);
	DualSenseHaptics::Shutdown();
	std::vector<SDL_Gamepad*> pads;
	for (const auto& [id, player]: g_pad_players) {
		if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id))) {
			StopHostPad(pad);
			pads.push_back(pad);
		}
	}
	if (!pads.empty()) {
		SDL_Delay(RELEASE_FLUSH_MS);
		for (auto* pad: pads) {
			SDL_CloseGamepad(pad);
		}
	}
	g_pad_players.clear();
	for (const auto& player: g_players) {
		if (player != nullptr) {
			player->SetDevices(-1);
		}
	}
}

void GameController::SetDevices(int active_id) {
	Common::LockGuard lock(m_mutex);
	if (m_active_id == active_id) {
		return;
	}
	if (active_id != HOST_INPUT_CONTROLLER_ID && active_id != -1) {
		if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(active_id));
		    pad != nullptr) {
			if (const auto& color = Config::GetControllerColor()) {
				(void)SDL_SetGamepadLED(pad, (*color)[0], (*color)[1], (*color)[2]);
			}
			for (auto sensor: {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO}) {
				if (SDL_GamepadHasSensor(pad, sensor) &&
				    !SDL_SetGamepadSensorEnabled(pad, sensor, true)) {
					LOGF("\t enabling controller sensor failed: %s\n", SDL_GetError());
				}
			}
		}
	}

	const bool new_connected = active_id != -1;
	if (!m_connected && new_connected) {
		m_connected_count++;
	}
	m_active_id     = active_id;
	m_connected     = new_connected;
	m_state         = {};
	m_gyro_time     = 0;
	m_gyro_emulation = false;
	m_emulated_time  = 0;
	m_emulated_stick = {128, 128};
	m_up_reference_valid = false;
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
	m_vibration          = {};
	m_vibration_until    = 0;
	m_trigger_effect     = {};
	m_trigger_state      = {};
}

// Input that changes no button or touch contact updates the newest unread sample, so a burst of
// stick or sensor events between two reads cannot push unread button or touch changes out.
void GameController::AddState() {
	const auto ordinary = OrdinaryState();
	if (m_states_num != 0) {
		const auto newest = (m_first_state + m_states_num - 1) % STATES_MAX;
		auto&      last   = m_states[newest];
		if (!m_obtained[newest] && last.buttons == ordinary.buttons &&
		    last.touch[0].down == ordinary.touch[0].down &&
		    last.touch[1].down == ordinary.touch[1].down) {
			last = ordinary;
			return;
		}
	}

	if (m_states_num >= STATES_MAX) {
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	const auto index  = (m_first_state + m_states_num) % STATES_MAX;
	m_states[index]   = ordinary;
	m_obtained[index] = false;
	m_states_num++;
}

void GameController::Button(int id, uint32_t button, bool down) {
	Common::LockGuard lock(m_mutex);

	// Keyboard events are routed to the configured player's port.
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		m_state.buttons = down ? m_state.buttons | button : m_state.buttons & ~button;

		AddState();
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));

		if (m_gyro_emulation && (axis == Controller::Axis::RightX || axis == Controller::Axis::RightY)) {
			m_emulated_stick[axis == Controller::Axis::RightY ? 1 : 0] = value;
			UpdateEmulatedTilt();
			return;
		}

		m_state.axes[axis_id] = value;

		uint32_t trigger = 0;
		if (axis == Controller::Axis::TriggerLeft) {
			trigger = PAD_BUTTON_L2;
		} else if (axis == Controller::Axis::TriggerRight) {
			trigger = PAD_BUTTON_R2;
		}
		if (trigger != 0) {
			m_state.buttons = value > 0 ? m_state.buttons | trigger : m_state.buttons & ~trigger;
			UpdateTriggerEffectState();
		}

		AddState();
	}
}

void GameController::RightStick(int id, int x, int y) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();
		if (m_gyro_emulation) {
			m_emulated_stick = {x, y};
			UpdateEmulatedTilt();
			return;
		}
		m_state.axes[static_cast<int>(Axis::RightX)] = x;
		m_state.axes[static_cast<int>(Axis::RightY)] = y;
		AddState();
	}
}

void GameController::TouchPad(int id, int finger, bool down, float x, float y) {
	if (finger < 0 || finger >= 2) {
		return;
	}

	Common::LockGuard lock(m_mutex);
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		auto& touch  = m_state.touch[finger];
		m_state.time = LibKernel::KernelGetProcessTime();
		if (down && !touch.down) {
			touch.id        = m_next_touch_id;
			m_next_touch_id = m_next_touch_id == 127 ? 1 : m_next_touch_id + 1;
		}
		touch.down = down;
		touch.x    = static_cast<uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1920.0f);
		touch.y    = static_cast<uint16_t>(std::clamp(y, 0.0f, 1.0f) * 943.0f);
		if (id == HOST_INPUT_CONTROLLER_ID) {
			m_state.buttons = down ? m_state.buttons | PAD_BUTTON_TOUCH_PAD
			                       : m_state.buttons & ~PAD_BUTTON_TOUCH_PAD;
		}
		AddState();
	}
}

namespace {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

Vec3 QuatRotate(const Quat& q, const Vec3& v) {
	const Vec3 t {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
	              2.0f * (q[0] * v[1] - q[1] * v[0])};
	return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]),
	        v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
	        v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

Quat QuatConjugate(const Quat& q) {
	return {-q[0], -q[1], -q[2], q[3]};
}

float Length(const Vec3& v) {
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void GameController::Motion(int id, Sensor sensor, const float* data, uint64_t time_us) {
	Common::LockGuard lock(m_mutex);
	if (id != m_active_id || !m_motion_enabled) {
		return;
	}

	// Convert acceleration to G; angular velocity is already in rad/s.
	if (sensor == Sensor::Accel) {
		for (int i = 0; i < 3; i++) {
			m_state.accel[i] = data[i] / SDL_STANDARD_GRAVITY;
		}
		// Capture gravity near 1 G to limit interference from linear acceleration.
		const float magnitude = Length(m_state.accel);
		if (!m_up_reference_valid && magnitude > 0.9f && magnitude < 1.1f) {
			const Vec3 up {m_state.accel[0] / magnitude, m_state.accel[1] / magnitude,
			               m_state.accel[2] / magnitude};
			m_up_reference       = QuatRotate(m_state.orientation, up);
			m_up_reference_valid = true;
		}
	} else {
		std::copy_n(data, 3, m_state.gyro.begin());
		// Mahony tilt correction; heading is unobservable and reported gyro values stay raw.
		auto        rate      = m_state.gyro;
		const float magnitude = Length(m_state.accel);
		if (m_up_reference_valid && magnitude > 0.8f && magnitude < 1.2f) {
			const Vec3 measured {m_state.accel[0] / magnitude, m_state.accel[1] / magnitude,
			                     m_state.accel[2] / magnitude};
			const Vec3 predicted = QuatRotate(QuatConjugate(m_state.orientation), m_up_reference);
			rate[0] += measured[1] * predicted[2] - measured[2] * predicted[1];
			rate[1] += measured[2] * predicted[0] - measured[0] * predicted[2];
			rate[2] += measured[0] * predicted[1] - measured[1] * predicted[0];
		}
		// Do not extrapolate a single sample across lost reports (e.g. loss of window focus).
		constexpr uint64_t max_gyro_interval_us = 100000;
		if (m_gyro_time != 0 && time_us > m_gyro_time &&
		    time_us - m_gyro_time <= max_gyro_interval_us) {
			const float dt         = static_cast<float>(time_us - m_gyro_time) * 0.000001f;
			const float speed      = Length(rate);
			const float half_angle = speed * dt * 0.5f;
			const float scale      = speed > 0.0f ? std::sin(half_angle) / speed : 0.0f;
			const float x          = rate[0] * scale;
			const float y          = rate[1] * scale;
			const float z          = rate[2] * scale;
			const float w          = std::cos(half_angle);
			const auto  q          = m_state.orientation;
			// Accumulate body-local rotation relative to connection / orientation reset.
			m_state.orientation = {q[3] * x + q[0] * w + q[1] * z - q[2] * y,
			                       q[3] * y - q[0] * z + q[1] * w + q[2] * x,
			                       q[3] * z + q[0] * y - q[1] * x + q[2] * w,
			                       q[3] * w - q[0] * x - q[1] * y - q[2] * z};
			float length        = 0.0f;
			for (float value: m_state.orientation) {
				length += value * value;
			}
			length = std::sqrt(length);
			for (float& value: m_state.orientation) {
				value /= length;
			}
		}
		m_gyro_time = time_us;
	}
	m_state.time = LibKernel::KernelGetProcessTime();
	AddState();
}

void GameController::SetMotionSensorState(bool enable) {
	Common::LockGuard lock(m_mutex);
	if (m_motion_enabled != enable) {
		m_motion_enabled     = enable;
		m_gyro_time          = 0;
		m_state.accel        = {0.0f, 1.0f, 0.0f};
		m_state.gyro         = {};
		m_up_reference_valid = false;
		m_state.time         = LibKernel::KernelGetProcessTime();
		AddState();
	}
}

static Quat QuatMultiply(const Quat& a, const Quat& b) {
	return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

void GameController::UpdateEmulatedTilt() {
	constexpr float max_angle = 0.7853982f; // 45 degrees at full stick deflection
	const auto      angle     = [](int value) {
        return std::clamp((value - 128) / 127.0f, -1.0f, 1.0f) * max_angle;
	};
	const float roll  = angle(m_emulated_stick[0]);
	const float pitch = angle(m_emulated_stick[1]);

	const Quat q_pitch {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
	const Quat q_roll {0.0f, 0.0f, std::sin(-roll * 0.5f), std::cos(-roll * 0.5f)};
	Quat       q = QuatMultiply(q_roll, q_pitch);

	const uint64_t now = LibKernel::KernelGetProcessTime();
	Vec3           rate {};
	if (m_emulated_time != 0 && now > m_emulated_time) {
		const float dt = std::clamp(static_cast<float>(now - m_emulated_time) * 0.000001f,
		                            0.001f, 0.1f);
		auto delta = QuatMultiply(QuatConjugate(m_state.orientation), q);
		if (delta[3] < 0.0f) {
			for (auto& v: delta) {
				v = -v;
			}
		}
		rate = {2.0f * delta[0] / dt, 2.0f * delta[1] / dt, 2.0f * delta[2] / dt};
	}
	m_emulated_time = now;

	m_state.time        = now;
	m_state.orientation = q;
	m_state.accel       = QuatRotate(QuatConjugate(q), {0.0f, 1.0f, 0.0f});
	m_state.gyro        = rate;
	AddState();
	// Angular velocity is only nonzero while the tilt changes.
	m_state.gyro = {};
}

void GameController::ToggleGyroEmulation(int id) {
	Common::LockGuard lock(m_mutex);
	if (id != HOST_INPUT_CONTROLLER_ID && id != m_active_id) {
		return;
	}
	// Pads with a real gyroscope report it directly.
	if (m_active_id != HOST_INPUT_CONTROLLER_ID) {
		auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
		if (pad != nullptr && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
			return;
		}
	}

	m_gyro_emulation = !m_gyro_emulation;
	m_emulated_time  = 0;
	m_state.time     = LibKernel::KernelGetProcessTime();
	if (m_gyro_emulation) {
		m_emulated_stick = {m_state.axes[static_cast<int>(Axis::RightX)],
		                    m_state.axes[static_cast<int>(Axis::RightY)]};
		m_state.axes[static_cast<int>(Axis::RightX)] = 128;
		m_state.axes[static_cast<int>(Axis::RightY)] = 128;
		UpdateEmulatedTilt();
	} else {
		m_state.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
		m_state.accel       = {0.0f, 1.0f, 0.0f};
		m_state.gyro        = {};
		m_state.axes[static_cast<int>(Axis::RightX)] = m_emulated_stick[0];
		m_state.axes[static_cast<int>(Axis::RightY)] = m_emulated_stick[1];
		AddState();
	}
	LOGF("Emulated gyro tilt: %s\n", m_gyro_emulation ? "enabled" : "disabled");
}

void GameController::ResetOrientation() {
	Common::LockGuard lock(m_mutex);
	m_state.orientation  = {0.0f, 0.0f, 0.0f, 1.0f};
	m_gyro_time          = 0;
	m_up_reference_valid = false;
	m_state.time        = LibKernel::KernelGetProcessTime();
	AddState();
}

void GameController::ResetInputState() {
	Common::LockGuard lock(m_mutex);
	m_state.buttons = 0;
	std::fill_n(m_state.axes, 4, 128);
	std::fill_n(m_state.axes + 4, 2, 0);
	UpdateTriggerEffectState();
	for (auto& touch: m_state.touch) {
		touch = {};
	}
	m_state.time    = LibKernel::KernelGetProcessTime();
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
	AddState();
}

static void StopHostPad(SDL_Gamepad* pad) {
	if (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		DualSenseEffects effect {};
		effect.enable_bits     = 0x0c;
		effect.right_trigger[0] = 0x05;
		effect.left_trigger[0]  = 0x05;
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
	(void)SDL_RumbleGamepad(pad, 0, 0, 0);
	(void)SDL_SetGamepadLED(pad, 0, 0, 0);
}

void GameController::SetVibration(uint8_t large_motor, uint8_t small_motor) {
	Common::LockGuard lock(m_mutex);
	m_vibration       = {large_motor, small_motor};
	m_vibration_until = SDL_GetTicks() + RUMBLE_DURATION_MS;
	ApplyVibration();
}

void GameController::ApplyVibration() {
	const auto now = SDL_GetTicks();
	const auto duration =
	    m_vibration_until > now ? static_cast<uint32_t>(m_vibration_until - now) : 0;
	const auto scale       = duration != 0 ? GetSettingScale(Setting::VibrationIntensity) : 0.0f;
	const auto large_motor = Scale(m_vibration[0], scale);
	const auto small_motor = Scale(m_vibration[1], scale);
	if (Loader::IsRuntimeChild()) {
		m_child_output.large_motor = large_motor; m_child_output.small_motor = small_motor; m_child_output.flags |= 1;
		m_child_output_dirty = true; PublishChildOutput();
		return;
	}
	if (g_runtime_parent.load() && (!g_runtime_focus.load() || !HostWindowEligible())) { return; }
	// The DualSense haptics backend drives a single pad, so only player 1 uses it.
	if (m_player == 0 &&
	    DualSenseHaptics::SetVibration(m_active_id, large_motor, small_motor, duration)) {
		return;
	}

	if (m_active_id == HOST_INPUT_CONTROLLER_ID) {
		return;
	}

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad == nullptr) {
		return;
	}

	const auto large = static_cast<uint16_t>(large_motor * 0x101U);
	const auto small = static_cast<uint16_t>(small_motor * 0x101U);
	if (!SDL_RumbleGamepad(pad, large, small, duration)) {
		LOGF("\t rumble failed: %s\n", SDL_GetError());
	}
}

int GameController::GetActiveControllerId() {
	Common::LockGuard lock(m_mutex);
	return m_active_id;
}

void GameController::SetLightBar(uint8_t r, uint8_t g, uint8_t b) {
	Common::LockGuard lock(m_mutex);
	if (Loader::IsRuntimeChild()) {
		m_child_output.red = r; m_child_output.green = g; m_child_output.blue = b; m_child_output.flags |= 2;
		m_child_output_dirty = true; PublishChildOutput();
		return;
	}
	if (g_runtime_parent.load() && (!g_runtime_focus.load() || !HostWindowEligible())) { return; }
	if (const auto& color = Config::GetControllerColor()) {
		r = (*color)[0];
		g = (*color)[1];
		b = (*color)[2];
	}
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	    pad != nullptr) {
		(void)SDL_SetGamepadLED(pad, r, g, b);
	}
}

bool GameController::SetTriggerEffect(const PadTriggerEffectParam& param) {
	Common::LockGuard lock(m_mutex);
	if (!SendTriggerEffect(param)) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if ((param.trigger_mask & (1u << i)) != 0) {
			m_trigger_effect.trigger_mask |= static_cast<uint8_t>(1u << i);
			m_trigger_effect.command[i] = param.command[i];
		}
	}
	UpdateTriggerEffectState();
	return true;
}

void GameController::GetTriggerEffectState(PadTriggerEffectStateInformation* info) {
	Common::LockGuard lock(m_mutex);
	*info = m_trigger_state;
}

void GameController::UpdateTriggerEffectState() {
	// Track every input event so a weapon remains fired even when the game polls after release
	// starts.
	for (int i = 0; i < 2; i++) {
		m_trigger_state.state[i] = TriggerEffectState(
		    m_trigger_effect.command[i], m_state.axes[static_cast<int>(Axis::TriggerLeft) + i],
		    m_trigger_state.state[i]);
	}
}

bool GameController::SendTriggerEffect(const PadTriggerEffectParam& param) {
	if ((param.trigger_mask & ~0x03u) != 0) {
		return false;
	}

	DualSenseEffects effect {};
	const auto       scale = GetSettingScale(Setting::TriggerEffectIntensity);
	if ((param.trigger_mask & 0x01u) != 0) {
		effect.enable_bits |= 0x08;
		if (!trigger_effect_to_dualsense(param.command[0], effect.left_trigger, scale)) {
			return false;
		}
	}
	if ((param.trigger_mask & 0x02u) != 0) {
		effect.enable_bits |= 0x04;
		if (!trigger_effect_to_dualsense(param.command[1], effect.right_trigger, scale)) {
			return false;
		}
	}
	if (effect.enable_bits == 0) {
		return true;
	}
	if (Loader::IsRuntimeChild()) {
		if (effect.enable_bits & 8) { std::copy_n(effect.left_trigger, 11, m_child_output.triggers.begin()); }
		if (effect.enable_bits & 4) { std::copy_n(effect.right_trigger, 11, m_child_output.triggers.begin() + 11); }
		m_child_output.flags |= 4;
		m_child_output_dirty = true; PublishChildOutput();
		return true;
	}
	if (g_runtime_parent.load() && (!g_runtime_focus.load() || !HostWindowEligible())) { return true; }

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad != nullptr && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
	return true;
}

void GameController::GetConnectionInfo(bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = OrdinaryState();
}

ControllerState GameController::OrdinaryState() {
	if (!g_runtime_parent.load()) { return m_state; }
	const bool eligible = HostWindowEligible();
	if (eligible != m_window_eligible) {
		m_window_eligible = eligible; m_states_num = m_first_state = 0;
		m_runtime_filter.Focus(g_runtime_focus.load(), m_runtime_epoch, ToReport(m_state, m_connected, m_connected_count));
	}
	auto raw = ToReport(m_state, m_connected, m_connected_count); raw.epoch = m_runtime_epoch; raw.serial = ++m_runtime_serial;
	raw.eligible = eligible;
	Child::PadReport filtered;
	return m_runtime_filter.Apply(raw, &filtered) ? FromReport(filtered) : ControllerState {};
}
void GameController::ReadRawState(ControllerState* state, bool* flag, int* count) {
	Common::LockGuard lock(m_mutex); *state = m_state; *flag = m_connected; *count = m_connected_count;
}
void GameController::RuntimeFocus(bool enabled, uint64_t epoch) {
	Common::LockGuard lock(m_mutex);
	m_runtime_epoch = epoch; m_runtime_serial = 0; m_states_num = m_first_state = 0;
	m_child_output = {}; m_child_output.epoch = epoch;
	m_child_output_dirty = false;
	m_runtime_filter.Focus(enabled, epoch, ToReport(m_state, m_connected, m_connected_count));
}
void GameController::Inject(const Child::PadReport& report) {
	Common::LockGuard lock(m_mutex); m_state = FromReport(report); m_connected = report.connected != 0;
	m_connected_count = static_cast<int>(std::min<uint32_t>(report.connections, INT32_MAX)); AddState();
}
void GameController::PublishChildOutput() {
	if (m_player != 0 || !g_runtime_focus.load()) { return; }
	m_child_output.epoch = g_runtime_epoch.load();
	m_child_output_dirty = !Loader::SendRuntimeChildPadOutput(m_child_output);
}
void GameController::FlushChildOutput() {
	Common::LockGuard lock(m_mutex); if (m_child_output_dirty) { PublishChildOutput(); }
}

void ConfigureRuntimeControllerRole(bool parent) {
	g_runtime_parent.store(parent); g_runtime_focus.store(!Loader::IsRuntimeChild());
}
void PulseRuntimeSystemPs() {
	// The child sends a pulse, not a held pad bit. Keep it observable across
	// shell polling ticks without sleeping the broker or leaking PS to the app.
	if (g_runtime_parent.load()) { g_runtime_ps_until.store(SDL_GetTicks() + 150); }
}
static void StopRuntimePadOutputLocked() {
	if (!g_players[0] || Loader::IsRuntimeChild()) { return; }
	const auto id = g_players[0]->GetActiveControllerId();
	(void)DualSenseHaptics::SetVibration(id, 0, 0, 0);
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id))) { StopHostPad(pad); }
}
void StopRuntimePadOutput() { Common::LockGuard lock(g_router_mutex); StopRuntimePadOutputLocked(); }
void SetRuntimeControllerFocus(bool enabled, uint64_t epoch) {
	Common::LockGuard lock(g_router_mutex);
	g_runtime_epoch = epoch; g_runtime_focus.store(enabled);
	for (const auto& player: g_players) { if (player) { player->RuntimeFocus(enabled, epoch); } }
	if (Loader::IsRuntimeChild()) {
		auto held = g_child_raw;
		// The baseline travels atomically with Focus, not through a mailbox
		// sample that could be dropped/overwritten at exactly the handoff.
		Loader::RuntimeChildFocusHeld(&held);
		g_child_filter.Focus(enabled, epoch, held);
		g_child_pad = {};
		ControllerState host_state; bool host_connected = false; int host_count = 0;
		if (g_child_host) { g_child_host->ReadRawState(&host_state, &host_connected, &host_count); }
		g_child_host_filter.Focus(enabled, epoch, ToReport(host_state, host_connected, host_count));
		g_child_host_serial = 0;
		if (g_players[0]) { auto neutral = Child::PadReport {}; neutral.connected = g_child_raw.connected; neutral.connections = g_child_raw.connections; g_players[0]->Inject(neutral); }
		Child::PadOutput stopped; stopped.epoch = epoch; Loader::SendRuntimeChildPadOutput(stopped);
	} else { StopRuntimePadOutputLocked(); }
}
Child::PadReport RuntimePadSnapshot() {
	Common::LockGuard lock(g_router_mutex);
	ControllerState state; bool connected = false; int count = 0;
	if (g_players[0]) { g_players[0]->ReadRawState(&state, &connected, &count); }
	// Windows may retain keyboard focus on the parent HWND while the embedded
	// app is visible. Forward its virtual keyboard/mouse pad just like Home;
	// marking it disconnected discards every mapped key in the child filter.
	// SetDevices already neutralizes the old physical state on unplug/fallback.
	const int device = connected && g_players[0] ? g_players[0]->GetActiveControllerId() : -1;
	if (device != -1 && device != g_broker_device && g_broker_connections != UINT32_MAX) { ++g_broker_connections; }
	g_broker_device = device;
	auto report = ToReport(state, connected, count); report.connections = g_broker_connections;
	report.eligible = HostWindowEligible(); return report;
}
// Caller holds g_router_mutex. Publish on each local event as well as each
// broker report, preserving press/release history even between pump ticks.
static void PublishRuntimeChildInput() {
	if (!g_players[0] || !g_child_host) { return; }
	ControllerState state; bool connected = false; int count = 0;
	g_child_host->ReadRawState(&state, &connected, &count);
	auto host = ToReport(state, connected, count);
	host.epoch = g_runtime_epoch.load(); host.serial = ++g_child_host_serial;
	const bool heartbeat = g_child_raw.serial && SDL_GetTicks() - g_child_last_input_ms <= 250;
	// Eligibility comes from the parent's foreground-root snapshot, just like
	// physical input. The child never owns a separate top-level foreground HWND.
	host.eligible = heartbeat && g_child_raw.eligible;
	Child::PadReport local;
	if (!g_child_host_filter.Apply(host, &local)) { return; }
	auto merged = g_child_pad;
	if (!heartbeat) { merged = {}; }
	else {
		merged.connected = merged.connected || local.connected;
		merged.connections = std::max(merged.connections, local.connections);
		merged.buttons |= local.buttons;
		for (unsigned i = 0; i != local.axes.size(); ++i) {
			if (local.axes[i] != (i < 4 ? 128 : 0)) { merged.axes[i] = local.axes[i]; }
		}
		for (unsigned i = 0; i != local.touch.size(); ++i) {
			if (local.touch[i].down) { merged.touch[i] = local.touch[i]; }
		}
	}
	merged.time = LibKernel::KernelGetProcessTime();
	g_players[0]->Inject(merged);
}
void PumpRuntimeChildController() {
	if (!Loader::IsRuntimeChild()) {
		static bool was_eligible = true;
		if (g_runtime_parent.load()) {
			const bool eligible = HostWindowEligible();
			if (!eligible && was_eligible) { StopRuntimePadOutput(); }
			was_eligible = eligible;
		}
		return;
	}
	Common::LockGuard lock(g_router_mutex);
	if (!g_players[0]) { return; }
	g_players[0]->FlushChildOutput();
	Child::PadReport raw, filtered;
	for (unsigned i = 0; i != 64 && Loader::PollRuntimeChildPad(&raw); ++i) {
		if (g_child_filter.Apply(raw, &filtered)) {
			g_child_raw = raw; g_child_pad = filtered; g_child_last_input_ms = SDL_GetTicks();
			PublishRuntimeChildInput();
		}
	}
	PublishRuntimeChildInput();
}
void ApplyRuntimePadOutput(const Child::PadOutput& output, uint64_t epoch, bool enabled) {
	Common::LockGuard lock(g_router_mutex);
	if (!enabled || output.epoch != epoch || Loader::IsRuntimeChild() || !g_players[0] || !HostWindowEligible()) { return; }
	const int id = g_players[0]->GetActiveControllerId();
	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
	if (output.flags & 1) {
		if (!DualSenseHaptics::SetVibration(id, output.large_motor, output.small_motor, RUMBLE_DURATION_MS) && pad) {
			(void)SDL_RumbleGamepad(pad, uint16_t(output.large_motor * 257u), uint16_t(output.small_motor * 257u), RUMBLE_DURATION_MS);
		}
	}
	if (pad && (output.flags & 2)) { (void)SDL_SetGamepadLED(pad, output.red, output.green, output.blue); }
	if (pad && (output.flags & 4) && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		DualSenseEffects effect {}; effect.enable_bits = 12;
		std::copy_n(output.triggers.begin(), 11, effect.left_trigger); std::copy_n(output.triggers.begin() + 11, 11, effect.right_trigger);
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > STATES_MAX);

	Common::LockGuard lock(m_mutex);
	// Observe OS-root foreground transitions before consuming any old history.
	if (g_runtime_parent.load()) { (void)OrdinaryState(); }

	*flag  = m_connected;
	*count = m_connected_count;

	int ret_num = 0;

	if (m_connected) {
		if (m_states_num != 0) {
			for (uint32_t i = 0; i < m_states_num; i++) {
				if (ret_num >= states_num) {
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_obtained[index]) {
					m_obtained[index] = true;

					states[ret_num++] = m_states[index];
				}
			}
		}
	}

	return ret_num;
}

// Keep every opened pad, including those waiting for a free player slot.
// Caller holds g_router_mutex.
static void RefreshPlayers() {
	if (Loader::IsRuntimeChild()) { return; }
	std::array<bool, Config::MAX_PLAYERS> taken {};
	const int keyboard = KeyboardPlayer();
	if (g_keyboard_player != 0) {
		taken[keyboard] = true;
	}
	for (auto& [id, player]: g_pad_players) {
		if (g_keyboard_player != 0 && player == keyboard) {
			player = -1;
		}
		if (player >= 0) {
			taken[player] = true;
		}
	}
	for (auto& [id, player]: g_pad_players) {
		if (player >= 0) {
			continue;
		}
		const auto free = std::find(taken.begin(), taken.end(), false);
		if (free != taken.end()) {
			player = static_cast<int>(free - taken.begin());
			*free  = true;
			LOGF("Controller %d assigned to player %d\n", id, player + 1);
		}
	}
	std::array<int, Config::MAX_PLAYERS> active;
	active.fill(-1);
	active[keyboard] = HOST_INPUT_CONTROLLER_ID;
	for (const auto& [id, player]: g_pad_players) {
		if (player >= 0) {
			active[player] = id;
		}
	}
	// Stop old owners before configuring new owners of the same physical pad.
	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		const int previous = g_players[player]->GetActiveControllerId();
		if (previous != active[player]) {
			if (player == 0) {
				(void)DualSenseHaptics::SetVibration(previous, 0, 0, 0);
			}
			if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(previous))) {
				StopHostPad(pad);
			}
		}
	}
	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		g_players[player]->SetDevices(active[player]);
	}
}

void Connect(int id) {
	if (Loader::IsRuntimeChild()) { return; }
	Common::LockGuard lock(g_router_mutex);
	if (std::any_of(g_pad_players.begin(), g_pad_players.end(),
	                [id](const auto& pad) { return pad.first == id; })) {
		return;
	}
	g_pad_players.emplace_back(id, -1);
	RefreshPlayers();
}

void Disconnect(int id) {
	Common::LockGuard lock(g_router_mutex);
	std::erase_if(g_pad_players, [id](const auto& pad) { return pad.first == id; });
	RefreshPlayers();
}

void CycleKeyboardPlayer() {
	Common::LockGuard lock(g_router_mutex);
	g_keyboard_player = (g_keyboard_player + 1) % (Config::MAX_PLAYERS + 1);
	RefreshPlayers();
	if (g_keyboard_player == 0) {
		LOGF("Keyboard shared with player 1\n");
	} else {
		LOGF("Keyboard assigned to player %u\n", g_keyboard_player);
	}
}

void SetButton(int id, uint32_t button, bool down) {
	Common::LockGuard lock(g_router_mutex);
	if (Loader::IsRuntimeChild()) {
		if (id == HOST_INPUT_CONTROLLER_ID && g_child_host) {
			g_child_host->Button(id, button, down); PublishRuntimeChildInput();
		}
		return;
	}
	if (auto* player = PlayerForDevice(id)) {
		player->Button(id, button, down);
	}
}

void SetAxis(int id, Axis axis, int value) {
	Common::LockGuard lock(g_router_mutex);
	if (Loader::IsRuntimeChild()) {
		if (id == HOST_INPUT_CONTROLLER_ID && g_child_host) {
			g_child_host->Axis(id, axis, value); PublishRuntimeChildInput();
		}
		return;
	}
	if (auto* player = PlayerForDevice(id)) {
		player->Axis(id, axis, value);
	}
}

void SetRightStick(int id, int x, int y) {
	Common::LockGuard lock(g_router_mutex);
	if (Loader::IsRuntimeChild()) {
		if (id == HOST_INPUT_CONTROLLER_ID && g_child_host) {
			g_child_host->RightStick(id, x, y); PublishRuntimeChildInput();
		}
		return;
	}
	if (auto* player = PlayerForDevice(id)) {
		player->RightStick(id, x, y);
	}
}

void SetTouchPad(int id, int finger, bool down, float x, float y) {
	Common::LockGuard lock(g_router_mutex);
	if (Loader::IsRuntimeChild()) {
		if (id == HOST_INPUT_CONTROLLER_ID && g_child_host) {
			g_child_host->TouchPad(id, finger, down, x, y); PublishRuntimeChildInput();
		}
		return;
	}
	if (auto* player = PlayerForDevice(id)) {
		player->TouchPad(id, finger, down, x, y);
	}
}

void SetSensor(int id, Sensor sensor, const float* data, uint64_t time_us) {
	if (Loader::IsRuntimeChild()) { return; }
	Common::LockGuard lock(g_router_mutex);
	if (auto* player = PlayerForDevice(id)) {
		player->Motion(id, sensor, data, time_us);
	}
}

void ToggleGyroEmulation(int id) {
	Common::LockGuard lock(g_router_mutex);
	if (auto* player = PlayerForDevice(id)) {
		player->ToggleGyroEmulation(id);
	}
}

void ResetInputState() {
	Common::LockGuard lock(g_router_mutex);
	if (g_child_host) { g_child_host->ResetInputState(); }
	for (const auto& player: g_players) {
		if (player != nullptr) {
			player->ResetInputState();
		}
	}
}

int GetActiveControllerId() {
	return g_players[0] != nullptr ? g_players[0]->GetActiveControllerId() : -1;
}

bool IsPlayerConnected(int player) {
	if (player < 0 || player >= Config::MAX_PLAYERS || g_players[player] == nullptr) {
		return false;
	}
	return g_players[player]->GetActiveControllerId() != -1;
}

bool GameController::PadOpenArgsAreValid(int user_id, int type, int index) const {
	// The host keyboard and gamepads of a player feed one standard port for that player's user.
	const bool personal = user_id == Config::GetPlayerUserId(m_player) && (type == 0 || type == 2);
	const bool system_remote = m_player == 0 && user_id == 0xff && type == 16;
	return index == 0 && (personal || system_remote);
}

int GameController::InitPort() {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		m_port_state = PortState::Closed;
	}
	return OK;
}

int GameController::OpenPort(int user_id, int type, int index, bool shared) {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (!PadOpenArgsAreValid(user_id, type, index)) {
		return PAD_ERROR_INVALID_ARG;
	}
	if ((m_port_state == PortState::Open && !shared) || m_port_owners == UINT32_MAX) {
		return PAD_ERROR_ALREADY_OPENED;
	}
	m_port_state = PortState::Open;
	++m_port_owners;
	return Handle();
}

int GameController::GetPortHandle(int user_id, int type, int index) {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	return m_port_state == PortState::Open && PadOpenArgsAreValid(user_id, type, index)
	           ? Handle()
	           : PAD_ERROR_NO_HANDLE;
}

int GameController::CheckPortHandleLocked(int handle) const {
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	return m_port_state == PortState::Open && handle == Handle() ? OK : PAD_ERROR_INVALID_HANDLE;
}

int GameController::CheckPortHandle(int handle) {
	Common::LockGuard lock(m_mutex);
	return CheckPortHandleLocked(handle);
}

int GameController::ClosePort(int handle) {
	Common::LockGuard lock(m_mutex);
	if (const auto error = CheckPortHandleLocked(handle); error != OK) {
		return error;
	}
	if (--m_port_owners == 0) {
		m_port_state = PortState::Closed;
	}
	return OK;
}

int PadCheckHandle(int handle) {
	if (g_players[0] == nullptr) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (PadVrHand(handle) >= 0) {
		return PadVrCheckHandle(handle);
	}
	auto* player = PlayerForHandle(handle);
	return player != nullptr ? player->CheckPortHandle(handle) : PAD_ERROR_INVALID_HANDLE;
}

// Handle 0 is never valid, so this only reports whether scePadInit has run.
bool PadInitialized() {
	return g_players[0] != nullptr && g_players[0]->CheckPortHandle(0) != PAD_ERROR_NOT_INITIALIZED;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadInit() {
	PRINT_NAME();
	for (const auto& player: g_players) {
		(void)player->InitPort();
	}
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	if (const int hand = PadVrPortHand(type); hand >= 0) {
		return PadVrPort(user_id, hand, index, true);
	}
	const int player = user_id == 0xff && type == 16 ? 0 : Config::GetPlayerFromUserId(user_id);
	return player >= 0 ? g_players[player]->OpenPort(user_id, type, index) : PAD_ERROR_INVALID_ARG;
}

int OpenSystemDevicePort() {
	// PSM's device-class probes and the co-hosted shells independently own
	// opens on this system port. Closing a probe must not close every reader.
	return g_players[0] != nullptr ? g_players[0]->OpenPort(0xff, 16, 0, true)
	                               : PAD_ERROR_NOT_INITIALIZED;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n",
	     user_id, type, index);

	if (const int hand = PadVrPortHand(type); hand >= 0) {
		return PadVrPort(user_id, hand, index, false);
	}
	const int player = user_id == 0xff && type == 16 ? 0 : Config::GetPlayerFromUserId(user_id);
	return player >= 0 ? g_players[player]->GetPortHandle(user_id, type, index)
	                   : PAD_ERROR_NO_HANDLE;
}

int KYTY_SYSV_ABI PadClose(int handle) {
	PRINT_NAME();
	LOGF("\t PadClose: handle = %d\n", handle);
	if (PadVrHand(handle) >= 0) {
		return PadVrClose(handle);
	}
	auto* player = PlayerForHandle(handle);
	if (player == nullptr) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	return player->ClosePort(handle);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));
	PlayerForHandle(handle)->SetMotionSensorState(enable);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadResetOrientation(int handle) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	PlayerForHandle(handle)->ResetOrientation();
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	int  connected_count = 0;
	bool connected       = false;
	PlayerForHandle(handle)->GetConnectionInfo(&connected, &connected_count);

	std::memset(info, 0, sizeof(*info));

	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
	info->connection_type       = 0;
	info->connected_count       = static_cast<uint8_t>(std::min(connected_count, 255));
	info->connected             = connected;
	info->device_class          = 0;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadIsRemoteController(int handle, bool* is_remote) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (is_remote == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	*is_remote = false;
	return OK;
}

static void ReadNeutralSnapshot(GameController* player, PadData* data) {
	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;
	player->GetConnectionInfo(&connected, &connected_count);
	state.time = LibKernel::KernelGetProcessTime();
	pad_fill_data(data, state, connected, connected_count);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadReadSystemState(int handle, PadData* data) {
	if (const auto error = PadCheckHandle(handle); error != OK) { return error; }
	if (!data) { return PAD_ERROR_INVALID_ARG; }
	ControllerState state; bool connected = false; int count = 0;
	PlayerForHandle(handle)->ReadState(&state, &connected, &count);
	if (g_runtime_parent.load() && HostWindowEligible()) {
		ControllerState raw; PlayerForHandle(handle)->ReadRawState(&raw, &connected, &count);
		state.buttons = (state.buttons & ~PAD_BUTTON_PS) | (raw.buttons & PAD_BUTTON_PS);
		if (SDL_GetTicks() < g_runtime_ps_until.load()) { state.buttons |= PAD_BUTTON_PS; }
	}
	pad_fill_data(data, state, connected, count); return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadReadState(int handle, PadData* data) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	// LNC resolves the TLS program's application owner and gates this policy on
	// VSH runtime mode. Firmware/system-device readers (including PadUiReadState)
	// remain live so ShellUI can observe the PS button while an app has focus.
	if (!LncUtil::Lnc::HasControllerFocus(Loader::RuntimeLinker::GetCurrentProgram())) {
		ReadNeutralSnapshot(PlayerForHandle(handle), data);
		return OK;
	}

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	PlayerForHandle(handle)->ReadState(&state, &connected, &connected_count);

	pad_fill_data(data, state, connected, connected_count);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(num < 1 || num > 64);
	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(PadData) * static_cast<size_t>(num));

	// ReadStates marks shared reports obtained. A background reader must never
	// call it, even if its results would subsequently be neutralized.
	if (!LncUtil::Lnc::HasControllerFocus(Loader::RuntimeLinker::GetCurrentProgram())) {
		ReadNeutralSnapshot(PlayerForHandle(handle), data);
		return 1;
	}

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64]      = {};

	auto* player  = PlayerForHandle(handle);
	int   ret_num = player->ReadStates(states, num, &connected, &connected_count);

	if (!connected || ret_num == 0) {
		if (connected) {
			player->ReadState(&states[0], &connected, &connected_count);
		}
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++) {
		pad_fill_data(&data[i], states[i], connected, connected_count);
	}

	return ret_num;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param) {
	PRINT_NAME();
	Common::LockGuard lock(g_router_mutex);

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	LOGF("\t large_motor = %d\n"
	     "\t small_motor = %d\n",
	     static_cast<int>(param->large_motor), static_cast<int>(param->small_motor));

	PlayerForHandle(handle)->SetVibration(param->large_motor, param->small_motor);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadResetLightBar(int handle) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param) {
	PRINT_NAME();
	Common::LockGuard lock(g_router_mutex);

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	PlayerForHandle(handle)->SetLightBar(param->r, param->g, param->b);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const PadTriggerEffectParam* param) {
	PRINT_NAME();
	Common::LockGuard lock(g_router_mutex);

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	return PlayerForHandle(handle)->SetTriggerEffect(*param) ? OK : PAD_ERROR_INVALID_ARG;
}

int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	PlayerForHandle(handle)->GetTriggerEffectState(info);

	return OK;
}

} // namespace Libs::Controller
