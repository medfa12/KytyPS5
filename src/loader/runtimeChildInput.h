#pragma once
#include "loader/runtimeChildProtocol.h"
#include <array>

namespace Loader::RuntimeChild {
// Normalized HOST ABI, not a guest PadData or SDL structure. Exactly one pad.
struct PadReport {
	uint64_t epoch = 0, serial = 0, time = 0;
	uint32_t buttons = 0, connections = 0;
	std::array<uint8_t, 6> axes {128,128,128,128,0,0};
	uint8_t connected = 0, eligible = 1;
	struct Touch { uint16_t x = 0, y = 0; uint8_t id = 0, down = 0; uint16_t reserved = 0; };
	std::array<Touch, 2> touch {};
	std::array<float, 3> accel {0,1,0}, gyro {};
	std::array<float, 4> orientation {0,0,0,1};
};
struct PadOutput {
	uint64_t epoch = 0, serial = 0;
	uint8_t large_motor = 0, small_motor = 0, red = 0, green = 0, blue = 0, flags = 0;
	std::array<uint8_t, 22> triggers {}; // Already encoded DualSense effects, no guest pointers.
};
static_assert(sizeof(PadReport) == 96);
static_assert(sizeof(PadOutput) == 48);
// Nonblocking process-shared try-lock: no pipe writes, spin waits, or unbounded
// backlog. A crashed peer cannot wedge lifecycle/control. Contention drops a
// sample; 64-report history drops oldest on overflow. All copies under lock.
class InputMailbox {
public:
	InputMailbox() = default;
	~InputMailbox();
	InputMailbox(const InputMailbox&) = delete;
	InputMailbox& operator=(const InputMailbox&) = delete;
	bool Create(const Key& key);
	bool Adopt(uint64_t handle, const Key& key);
	uint64_t Handle() const { return handle_; }
	bool Push(PadReport report);
	bool Pop(PadReport* report);
	bool WriteOutput(PadOutput output);
	bool ReadOutput(PadOutput* output);
	bool PulsePs();
	bool ReadPs(uint64_t* pulses);
private:
	void* view_ = nullptr;
	uint64_t handle_ = 0;
};
// Epoch transitions drop history and suppress held buttons/axes/touches until
// release/center. System Guide/PS (0x10000) never enters ordinary app reports.
class PadFocusFilter {
public:
	void Focus(bool enabled, uint64_t epoch, const PadReport& held);
	bool Apply(const PadReport& raw, PadReport* result);
private:
	uint64_t epoch_ = 0, serial_ = 0;
	uint32_t buttons_ = 0;
	std::array<bool, 6> axes_ {};
	std::array<bool, 2> touch_ {};
	bool enabled_ = false;
};
}
