#include "loader/runtimeChildInput.h"
#include <cmath>
#include <new>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::RuntimeChild {
namespace {
bool Center(unsigned i, uint8_t value) { return i < 4 ? value >= 112 && value <= 144 : value <= 16; }
#if defined(_WIN32)
struct Shared {
	volatile LONG lock = 0;
	uint32_t magic = 0x50414432;
	Key key;
	uint32_t first = 0, count = 0;
	uint64_t input_serial = 0, output_serial = 0;
	uint64_t ps_pulses = 0;
	std::array<PadReport, 64> reports {};
	PadOutput output;
};
struct TryLock {
	Shared* state;
	explicit TryLock(void* view): state(static_cast<Shared*>(view)) {
		if (state && InterlockedCompareExchange(&state->lock, 1, 0)) { state = nullptr; }
	}
	~TryLock() { if (state) { InterlockedExchange(&state->lock, 0); } }
};
#endif
}
InputMailbox::~InputMailbox() {
#if defined(_WIN32)
	if (view_) { UnmapViewOfFile(view_); }
	if (handle_) { CloseHandle(reinterpret_cast<HANDLE>(handle_)); }
#endif
}
bool InputMailbox::Create(const Key& key) {
#if defined(_WIN32)
	if (view_ || handle_ || !key.Valid()) { return false; }
	SECURITY_ATTRIBUTES security {sizeof(security), nullptr, FALSE};
	const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
	if (!mapping) { return false; }
	auto* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!view) { CloseHandle(mapping); return false; }
	auto* state = new(view) Shared; state->key = key;
	view_ = view; handle_ = reinterpret_cast<uint64_t>(mapping); return true;
#else
	return false;
#endif
}
bool InputMailbox::Adopt(uint64_t handle, const Key& key) {
#if defined(_WIN32)
	if (view_ || handle_ || !handle || !key.Valid()) { return false; }
	auto* view = MapViewOfFile(reinterpret_cast<HANDLE>(handle), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!view) { return false; }
	auto* state = static_cast<Shared*>(view);
	if (state->magic != 0x50414432 || state->key != key) { UnmapViewOfFile(view); return false; }
	view_ = view; handle_ = handle; return true;
#else
	return false;
#endif
}
bool InputMailbox::Push(PadReport report) {
#if defined(_WIN32)
	TryLock guard(view_); auto* s = guard.state; if (!s) { return false; }
	if (s->count == s->reports.size()) { s->first = (s->first + 1) % s->reports.size(); --s->count; }
	report.serial = ++s->input_serial;
	s->reports[(s->first + s->count++) % s->reports.size()] = report; return true;
#else
	return false;
#endif
}
bool InputMailbox::Pop(PadReport* report) {
#if defined(_WIN32)
	TryLock guard(view_); auto* s = guard.state; if (!s || !report || !s->count) { return false; }
	*report = s->reports[s->first]; s->first = (s->first + 1) % s->reports.size(); --s->count; return true;
#else
	return false;
#endif
}
bool InputMailbox::WriteOutput(PadOutput output) {
#if defined(_WIN32)
	TryLock guard(view_); auto* s = guard.state; if (!s) { return false; }
	output.serial = ++s->output_serial; s->output = output; return true;
#else
	return false;
#endif
}
bool InputMailbox::ReadOutput(PadOutput* output) {
#if defined(_WIN32)
	TryLock guard(view_); auto* s = guard.state;
	if (!s || !output || !s->output.serial || s->output.serial == output->serial) { return false; }
	*output = s->output; return true;
#else
	return false;
#endif
}
bool InputMailbox::PulsePs() {
#if defined(_WIN32)
	TryLock guard(view_); if (!guard.state) { return false; }
	++guard.state->ps_pulses; return true;
#else
	return false;
#endif
}
bool InputMailbox::ReadPs(uint64_t* pulses) {
#if defined(_WIN32)
	TryLock guard(view_); if (!guard.state || !pulses || *pulses == guard.state->ps_pulses) { return false; }
	*pulses = guard.state->ps_pulses; return true;
#else
	return false;
#endif
}
void PadFocusFilter::Focus(bool enabled, uint64_t epoch, const PadReport& held) {
	epoch_ = epoch; serial_ = 0; enabled_ = enabled; buttons_ = held.buttons;
	for (unsigned i = 0; i != axes_.size(); ++i) { axes_[i] = !Center(i, held.axes[i]); }
	for (unsigned i = 0; i != touch_.size(); ++i) { touch_[i] = held.touch[i].down != 0; }
}
bool PadFocusFilter::Apply(const PadReport& raw, PadReport* result) {
	if (!result || raw.epoch != epoch_ || !raw.serial || raw.serial <= serial_) { return false; }
	serial_ = raw.serial;
	if (!raw.eligible) {
		buttons_ = raw.buttons;
		for (unsigned i = 0; i != axes_.size(); ++i) { axes_[i] = !Center(i, raw.axes[i]); }
		for (unsigned i = 0; i != touch_.size(); ++i) { touch_[i] = raw.touch[i].down != 0; }
	}
	buttons_ &= raw.buttons;
	for (unsigned i = 0; i != axes_.size(); ++i) { if (Center(i, raw.axes[i])) { axes_[i] = false; } }
	for (unsigned i = 0; i != touch_.size(); ++i) { if (!raw.touch[i].down) { touch_[i] = false; } }
	*result = raw;
	if (!enabled_ || !raw.connected || !raw.eligible) {
		*result = PadReport {}; result->epoch = raw.epoch; result->serial = raw.serial; result->time = raw.time;
		result->connected = raw.connected; result->connections = raw.connections;
		return true;
	}
	result->buttons &= ~(buttons_ | 0x10000u);
	for (unsigned i = 0; i != axes_.size(); ++i) { if (axes_[i]) { result->axes[i] = i < 4 ? 128 : 0; } }
	for (unsigned i = 0; i != touch_.size(); ++i) { if (touch_[i]) { result->touch[i] = {}; } }
	for (float& f: result->accel) { if (!std::isfinite(f)) { f = 0; } }
	for (float& f: result->gyro) { if (!std::isfinite(f)) { f = 0; } }
	for (float& f: result->orientation) { if (!std::isfinite(f)) { f = 0; } }
	return true;
}
}
