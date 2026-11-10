// Exercise production output scheduling with a deterministic clock and consuming SDL queues.
#include "common/threads.h"
#include "libs/dualSenseHaptics.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AudioOutTimingTests: %s\n", message);
		std::abort();
	}
}

struct Stream {
	SDL_AudioSpec spec;
	uint64_t      frames = 0; // Millionths of a frame retain exact simulated consumption.
	uint64_t      updated;
	std::vector<uint8_t> pcm;
	SDL_AudioDeviceID device = 0;
	bool paused = true;
	float gain = 1;
	bool input_ended = false;
};

uint64_t                             now       = 1000000;
uint32_t                             oversleep = 0, processing = 0;
std::vector<uint32_t>                sleeps;
std::vector<std::unique_ptr<Stream>> streams;
bool                                 fail_open = false, fail_put = false, stalled = false;
bool                                 fail_init = false, fail_resume = false, fail_keepalive = false;
bool                                 fail_query = false, fail_clear = false;
bool                                 fail_end = false, retain_resampler_tail = false;
int                                  input_ends = 0;
int                                  audio_refs = 0, audio_inits = 0, audio_quits = 0;
int                                  physical_opens = 0, physical_closes = 0;
int                                  warm_opens = 0;
uint32_t                             cold_open_us = 0, cold_close_us = 0;
SDL_AudioDeviceID                     next_device = 1;
std::vector<SDL_AudioDeviceID>         logical_devices;
bool                                 pad_connected = false, pad_bluetooth = false;
uint64_t                             pad_queue_us = 0;
const float*                         queued_pad_gains = nullptr;
int                                  clears = 0;
std::binary_semaphore* held_put_entered = nullptr;
std::binary_semaphore* held_put_resume = nullptr;
std::binary_semaphore* held_destroy_entered = nullptr;
std::binary_semaphore* held_destroy_resume = nullptr;

Stream& GetStream(SDL_AudioStream* stream) {
	return *reinterpret_cast<Stream*>(stream);
}

void Drain(Stream& stream) {
	if (!stalled && !stream.paused) {
		const auto consumed = (now - stream.updated) * stream.spec.freq;
		const auto tail = retain_resampler_tail && !stream.input_ended
		                      ? std::min(stream.frames, uint64_t(7 * 1000000)) : 0;
		stream.frames -= std::min(stream.frames - tail, consumed);
	}
	stream.updated = now;
}
} // namespace

namespace Fake {
bool InitSubSystem(SDL_InitFlags) {
	if (fail_init) {
		return false;
	}
	++audio_refs;
	++audio_inits;
	return true;
}
void QuitSubSystem(SDL_InitFlags) {
	Check(streams.empty(), "SDL teardown raced a still-owned stream destruction");
	Check(audio_refs > 0, "audio subsystem reference underflow");
	--audio_refs;
	++audio_quits;
}
bool ResumeAudioStreamDevice(SDL_AudioStream* stream) {
	if (fail_resume) return false;
	Drain(GetStream(stream));
	GetStream(stream).paused = false;
	return true;
}
bool PauseAudioStreamDevice(SDL_AudioStream* stream) {
	Drain(GetStream(stream)); GetStream(stream).paused = true; return true;
}
bool SetAudioStreamGain(SDL_AudioStream* stream, float gain) {
	GetStream(stream).gain = gain; return true;
}

SDL_AudioDeviceID OpenAudioDevice(SDL_AudioDeviceID source, const SDL_AudioSpec* spec) {
	if (source == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK && spec == nullptr) {
		++warm_opens;
	}
	if (source != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
		Check(std::find(logical_devices.begin(), logical_devices.end(), source) != logical_devices.end(),
		      "keepalive was derived from an invalid logical device");
		Check(spec == nullptr, "keepalive changed format negotiation");
		if (fail_keepalive) {
			return 0;
		}
	}
	if (fail_open) {
		return 0;
	}
	if (logical_devices.empty()) {
		++physical_opens;
		now += cold_open_us;
	}
	const auto device = next_device++;
	logical_devices.push_back(device);
	return device;
}

void CloseAudioDevice(SDL_AudioDeviceID device) {
	const auto it = std::find(logical_devices.begin(), logical_devices.end(), device);
	Check(it != logical_devices.end(), "unknown logical device closed");
	logical_devices.erase(it);
	if (logical_devices.empty()) {
		++physical_closes;
		now += cold_close_us;
	}
}

SDL_AudioDeviceID GetAudioStreamDevice(SDL_AudioStream* stream) {
	return GetStream(stream).device;
}

SDL_AudioStream* OpenAudioDeviceStream(SDL_AudioDeviceID source, const SDL_AudioSpec* spec,
                                        SDL_AudioStreamCallback, void*) {
	Check(source == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
	      "port pinned a cached physical device instead of following the current default");
	const auto device = OpenAudioDevice(source, spec);
	if (device == 0) {
		return nullptr;
	}

	streams.push_back(std::make_unique<Stream>(Stream {*spec, 0, now, {}, device}));
	return reinterpret_cast<SDL_AudioStream*>(streams.back().get());
}

void DestroyAudioStream(SDL_AudioStream* stream) {
	if (held_destroy_entered) { held_destroy_entered->release(); held_destroy_resume->acquire(); }
	const auto it = std::find_if(streams.begin(), streams.end(), [stream](const auto& value) {
		return value.get() == &GetStream(stream);
	});
	Check(it != streams.end(), "unknown stream destroyed");
	const auto device = GetStream(stream).device;
	streams.erase(it);
	CloseAudioDevice(device);
}

int GetAudioStreamQueued(SDL_AudioStream* stream) {
	if (fail_query) return -1;
	auto& state = GetStream(stream);
	Drain(state);
	return static_cast<int>(state.frames / 1000000 * SDL_AUDIO_BYTESIZE(state.spec.format) *
	                        state.spec.channels);
}

bool PutAudioStreamData(SDL_AudioStream* stream, const void* data, int bytes) {
	if (held_put_entered) { held_put_entered->release(); held_put_resume->acquire(); }
	now += processing;
	if (fail_put) {
		return false;
	}
	auto& state = GetStream(stream);
	const auto* pcm = static_cast<const uint8_t*>(data);
	state.pcm.assign(pcm, pcm + bytes);
	Drain(state);
	state.frames += static_cast<uint64_t>(bytes) * 1000000 /
	                (SDL_AUDIO_BYTESIZE(state.spec.format) * state.spec.channels);
	return true;
}

bool ClearAudioStream(SDL_AudioStream* stream) {
	if (fail_clear) return false;
	auto& state   = GetStream(stream);
	state.frames  = 0;
	state.input_ended = false;
	state.updated = now;
	clears++;
	return true;
}
bool FlushAudioStream(SDL_AudioStream* stream) {
	if (fail_end) return false;
	GetStream(stream).input_ended = true;
	++input_ends;
	return true;
}
} // namespace Fake

namespace Common {
class TimingThread: public Thread {
public:
	static void SleepMicro(uint32_t micros) {
		sleeps.push_back(micros);
		now += micros + oversleep;
	}
};
} // namespace Common

#define Thread                      TimingThread
#define SDL_InitSubSystem           Fake::InitSubSystem
#define SDL_QuitSubSystem           Fake::QuitSubSystem
#define SDL_OpenAudioDeviceStream   Fake::OpenAudioDeviceStream
#define SDL_OpenAudioDevice         Fake::OpenAudioDevice
#define SDL_CloseAudioDevice        Fake::CloseAudioDevice
#define SDL_GetAudioStreamDevice    Fake::GetAudioStreamDevice
#define SDL_ResumeAudioStreamDevice Fake::ResumeAudioStreamDevice
#define SDL_PauseAudioStreamDevice  Fake::PauseAudioStreamDevice
#define SDL_SetAudioStreamGain      Fake::SetAudioStreamGain
#define SDL_DestroyAudioStream      Fake::DestroyAudioStream
#define SDL_GetAudioStreamQueued    Fake::GetAudioStreamQueued
#define SDL_PutAudioStreamData      Fake::PutAudioStreamData
#define SDL_ClearAudioStream        Fake::ClearAudioStream
#define SDL_FlushAudioStream        Fake::FlushAudioStream
#include "libs/audio.cpp"
#undef Thread
#undef SDL_InitSubSystem
#undef SDL_QuitSubSystem
#undef SDL_OpenAudioDeviceStream
#undef SDL_OpenAudioDevice
#undef SDL_CloseAudioDevice
#undef SDL_GetAudioStreamDevice
#undef SDL_ResumeAudioStreamDevice
#undef SDL_PauseAudioStreamDevice
#undef SDL_SetAudioStreamGain
#undef SDL_DestroyAudioStream
#undef SDL_GetAudioStreamQueued
#undef SDL_PutAudioStreamData
#undef SDL_ClearAudioStream
#undef SDL_FlushAudioStream

namespace Libs::Controller {
int GetActiveControllerId() {
	return 0;
}
float GetSettingScale(Setting) {
	return 1.0f;
}

namespace DualSenseHaptics {
Stream* Open(uint32_t, bool) {
	static int pad_stream;
	return pad_connected ? reinterpret_cast<Stream*>(&pad_stream) : nullptr;
}
void     Close(Stream*) {}
bool UsesBluetooth(const Stream* stream) {
	return stream != nullptr && pad_bluetooth;
}
uint64_t Queue(Stream* stream, int, const void*, uint32_t, uint32_t, bool, const int*, float, const float* gains) {
	queued_pad_gains = gains;
	return stream != nullptr ? pad_queue_us : 0;
}
} // namespace DualSenseHaptics
} // namespace Libs::Controller

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now;
}
} // namespace Libs::LibKernel

namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now) / 1000;
}
} // namespace Loader::Timer

namespace {
using Audio = Libs::Audio::Audio;

struct Fixture {
	Audio                   audio;
	std::array<float, 2048> pcm {};

	Fixture() {
		Check(streams.empty() && logical_devices.empty() && audio_refs == 0,
		      "stream/device/subsystem reference leaked between tests");
		now        = 1000000;
		processing = oversleep = 0;
		clears                 = 0;
		fail_open = fail_put = stalled = false;
		fail_init = fail_resume = fail_keepalive = false;
		fail_query = fail_clear = false;
		fail_end = retain_resampler_tail = false;
		input_ends = 0;
		audio_inits = audio_quits = physical_opens = physical_closes = 0;
		warm_opens = 0;
		cold_open_us = cold_close_us = 0;
		next_device = 1;
		pad_connected = pad_bluetooth = false;
		pad_queue_us = 0;
		queued_pad_gains = nullptr;
		sleeps.clear();
		held_put_entered = held_put_resume = nullptr;
		held_destroy_entered = held_destroy_resume = nullptr;
	}

	Audio::Id Open(uint32_t frames = 256, int type = 0) {
		const auto id = audio.AudioOutOpen(type, frames, 48000, Audio::Format::FloatStereo);
		Check(id.IsValid(), "port open failed");
		return id;
	}

	void Output(Audio::Id port, bool blocking = true) {
		Audio::OutputParam param {port, pcm.data()};
		audio.AudioOutOutputs(&param, 1, blocking);
	}

	void Prime(Audio::Id port) {
		const auto start = now;
		for (int i = 0; i < 8; i++) {
			Output(port);
		}
		Check(now == start, "priming was paced before the 40 ms cushion filled");
		Output(port);
		Check(now - start >= 5333 && now - start <= 5334,
		      "priming accumulated a multi-block deadline");
		Drain(*streams.front());
		Check(streams.front()->frames >= uint64_t {7 * 256} * 1000000,
		      "first paced output consumed the priming cushion");
	}
};

void TestPrimingAndSteadyCadence() {
	Fixture    f;
	const auto port = f.Open();
	f.Prime(port);
	processing = 125;
	oversleep  = 175;
	for (int i = 0; i < 10; i++) {
		f.Output(port);
	}
	const auto start = now;
	for (int i = 0; i < 3000; i++) {
		f.Output(port);
	}
	Check(now - start >= 15999999 && now - start <= 16000001,
	      "processing, sleep overshoot, or fractional periods accumulated timing drift");
}

void TestUnderrunAndStalledQueueRecovery() {
	Fixture    f;
	const auto port = f.Open();
	f.Prime(port);
	now += 1000000;
	f.Prime(port);

	// A stopped device must clear its stale queue and restart priming, not catch up old deadlines.
	stalled = true;
	for (int i = 0; i < 3 && clears == 0; i++) {
		f.Output(port);
	}
	Check(clears == 1, "stalled playback queue was not cleared");
	stalled          = false;
	const auto start = now;
	for (int i = 0; i < 7; i++) {
		f.Output(port);
	}
	Check(now == start, "queue clear did not restart unpaced priming");
	f.Output(port);
	Check(now - start >= 5333 && now - start <= 5334,
	      "queue clear retained an old pacing deadline");
}

void TestFallbackClockAndLongGap() {
	Fixture f;
	fail_open       = true;
	const auto port = f.Open();
	f.Output(port);
	const auto start = now;
	oversleep        = 175;
	for (int i = 0; i < 3000; i++) {
		now += 125;
		f.Output(port);
	}
	Check(now - start >= 16000174 && now - start <= 16000176,
	      "device-free output accumulated timing drift");
	now += 1000000;
	f.Output(port);
	const auto resumed = now;
	f.Output(port);
	Check(now - resumed >= 5333 && now - resumed <= 5509,
	      "long interruption caused a burst of device-free catch-up submissions");
}

void TestAsyncDoesNotAccumulateDeadlines() {
	Fixture    f;
	const auto port  = f.Open();
	const auto start = now;
	for (int i = 0; i < 8; i++) {
		f.Output(port, false);
	}
	Check(now == start, "asynchronous output slept");
	f.Output(port);
	Check(now - start <= 5334, "asynchronous submissions accumulated a blocking deadline");
	for (int i = 0; i < 3; i++) {
		f.Output(port);
	}
	now += 1000000;
	f.Output(port, false);
	const auto resumed = now;
	for (int i = 0; i < 7; i++) {
		f.Output(port);
	}
	Check(now == resumed, "async-to-blocking transition did not reset an emptied queue");
}

void TestSynchronizedBatchAndInactivePorts() {
	Fixture            f;
	const auto         vibration  = f.Open(256, 10);
	const auto         main       = f.Open();
	const auto         background = f.Open();
	Audio::OutputParam batch[] {
	    {vibration, f.pcm.data()}, {main, f.pcm.data()}, {background, f.pcm.data()}};
	const auto start = now;
	for (int i = 0; i < 8; i++) {
		f.audio.AudioOutOutputs(batch, 3);
	}
	Check(now == start, "vibration fallback slowed a device-backed batch's priming");
	for (int i = 0; i < 3; i++) {
		sleeps.clear();
		f.audio.AudioOutOutputs(batch, 3);
		Check(sleeps.size() == 1 && sleeps.front() >= 5333 && sleeps.front() <= 5334,
		      "synchronized streams accumulated separate pacing waits");
	}

	Audio::OutputParam inactive[] {{main, nullptr}, {vibration, f.pcm.data()}};
	now += 1000000;
	f.audio.AudioOutOutputs(inactive, 2);
	const auto resumed = now;
	f.audio.AudioOutOutputs(inactive, 2);
	Check(now - resumed >= 5333 && now - resumed <= 5334,
	      "a device without PCM disabled fallback pacing");
}

void TestFallbackUsesEachPortsPeriod() {
	Fixture f;
	fail_open                     = true;
	const auto         short_port = f.Open(128);
	const auto         long_port  = f.Open(256);
	Audio::OutputParam batch[] {{short_port, f.pcm.data()}, {long_port, f.pcm.data()}};
	f.audio.AudioOutOutputs(batch, 2);
	const auto start = now;
	f.audio.AudioOutOutputs(batch, 2);
	Check(now - start >= 5333 && now - start <= 5334,
	      "fallback batch used the first port's period for every port");
}

void TestFailedQueueUsesFallbackClock() {
	Fixture    f;
	const auto port = f.Open();
	fail_put        = true;
	f.Output(port);
	const auto start = now;
	for (int i = 0; i < 3; i++) {
		f.Output(port);
	}
	Check(now - start == 16000, "repeated queue failures reset fallback pacing");
}

void TestControllerSpeakerPacing() {
	Fixture f;
	fail_open     = true; // No PC audio device paces the pad speaker.
	pad_connected = true;
	pad_queue_us  = 80000;
	const auto speaker = f.Open(256, 4);

	pad_bluetooth = true;
	f.Output(speaker);
	const auto bluetooth_start = now;
	f.Output(speaker);
	Check(now - bluetooth_start >= 5333 && now - bluetooth_start <= 5334,
	      "Bluetooth speaker waited on its HID queue instead of the sample clock");

	pad_bluetooth = false;
	const auto usb_start = now;
	f.Output(speaker);
	Check(now - usb_start >= 5333 && now - usb_start <= 5334,
	      "stalled USB speaker waited longer than one audio block");
}

void TestInvalidBatchSize() {
	Libs::Audio::AudioOut::AudioOutOutputParam param {};
	for (const auto count: {0u, 33u}) {
		Check(Libs::Audio::AudioOut::AudioOutOutputs(&param, count) ==
		          Libs::Audio::AUDIO_OUT_ERROR_INVALID_SIZE,
		      "invalid batch size was not rejected before accessing ports");
	}
}

void TestZeroOutputFrequency() {
	Fixture f;
	Check(Libs::Audio::AudioOut::AudioOutOpen(0, 0, 0, 256, 0, 4) ==
	          Libs::Audio::AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ,
	      "public output open accepted zero frequency");
	Check(!f.audio.AudioOutOpen(10, 256, 0, Audio::Format::FloatStereo).IsValid(),
	      "internal output open accepted zero frequency");
	Check(f.Open().ToInt() == 1, "rejected output port occupied a handle");
}

template <typename T>
void CheckOutputSamples(std::initializer_list<T> expected, const char* message) {
	const auto& pcm = streams.back()->pcm;
	Check(pcm.size() == expected.size() * sizeof(T) &&
	          std::memcmp(pcm.data(), expected.begin(), pcm.size()) == 0, message);
}

void TestChannelGains() {
	Fixture f;
	const std::array pcm {0.25f, -0.5f, 0.75f, -1.0f};
	const auto stereo = f.audio.AudioOutOpen(0, 2, 48000, Audio::Format::FloatStereo);
	std::array gains {2.0f, 0.5f};
	Audio::OutputParam output {stereo, pcm.data(), gains.data()};
	for (int push = 0; push < 2; push++) {
		f.audio.AudioOutOutputs(&output, 1, false);
		CheckOutputSamples({0.5f, -0.25f, 1.5f, -0.5f}, "float channel gains were not applied independently");
	}
	const int volume[] {16384, 32768};
	f.audio.AudioOutSetVolume(stereo, 3, volume);
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.25f, -0.25f, 0.75f, -0.5f}, "channel gain did not combine with port volume");
	gains = {0.0f, 0.0f};
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.0f, -0.0f, 0.0f, -0.0f}, "zero channel gains did not mute");
	output.gains = nullptr;
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.125f, -0.5f, 0.375f, -1.0f}, "channel scaling changed original float PCM");

	const std::array<int16_t, 2> integer_pcm {20000, -20000};
	const auto integer = f.audio.AudioOutOpen(0, 1, 48000, Audio::Format::Signed16bitStereo);
	output = {integer, integer_pcm.data(), gains.data()};
	for (const auto gain: {2.0f, std::numeric_limits<float>::max()}) {
		gains = {gain, gain};
		f.audio.AudioOutOutputs(&output, 1, false);
		CheckOutputSamples<int16_t>({32767, -32768}, "integer gain overflowed instead of saturating");
	}
	gains = {0.5f, 0.25f};
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples<int16_t>({10000, -5000}, "integer channel gains changed original PCM");

	const std::array surround_pcm {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f};
	for (const auto format: {Audio::Format::Float8Ch, Audio::Format::Float12Ch}) {
		const auto surround = f.audio.AudioOutOpen(0, 1, 48000, format);
		output = {surround, surround_pcm.data(), surround_pcm.data()};
		f.audio.AudioOutOutputs(&output, 1, false);
		if (format == Audio::Format::Float8Ch) {
			CheckOutputSamples({1.0f, 4.0f, 9.0f, 16.0f, 49.0f, 64.0f, 25.0f, 36.0f},
			                   "channel reordering used destination channel gains");
		} else {
			CheckOutputSamples({82.0f, 104.0f, 9.0f, 16.0f, 170.0f, 208.0f, 25.0f, 36.0f},
			                   "height downmix lost per-channel gains");
		}
	}
	pad_connected = true;
	const auto speaker = f.Open(2, 4);
	gains = {0.75f, 0.25f};
	output = {speaker, pcm.data(), gains.data()};
	f.audio.AudioOutOutputs(&output, 1, false);
	Check(queued_pad_gains != nullptr && queued_pad_gains[0] == gains[0] && queued_pad_gains[1] == gains[1],
	      "connected controller did not receive channel gains");
}

void TestTraceQueueUnitsAndBudgets() {
	using Libs::Audio::AudioTraceBudget;
	using Libs::Audio::AudioTraceQueuedUs;
	Check(AudioTraceQueuedUs(8192, 32, 48000) == 5333,
	      "8-channel float trace queue used host/stereo byte units");
	Check(AudioTraceQueuedUs(65536, 32, 48000) == 42666 &&
	          AudioTraceQueuedUs(73728, 32, 48000) == 48000,
	      "trace queue duration did not match the eight/nine-block cushion");
	Check(AudioTraceQueuedUs(4096, 4, 48000) == 21333 &&
	          AudioTraceQueuedUs(0, 4, 48000) == 0,
	      "S16 stereo trace queue duration was incorrect");
	Check(AudioTraceQueuedUs(-1, 4, 48000) == -1 &&
	          AudioTraceQueuedUs(1, 0, 48000) == -1 &&
	          AudioTraceQueuedUs(1, 4, 0) == -1,
	      "invalid trace queue information was reported as a valid duration");
	Check(AudioTraceQueuedUs(std::numeric_limits<int>::max(), 1, 1) == 2147483647000000LL,
	      "trace queue duration overflowed before conversion");
	AudioTraceBudget budget;
	for (uint32_t i = 1; i <= AudioTraceBudget::REPORTS_MAX; ++i) {
		Check(budget.TakeReport() == i, "trace event sequence was not monotonic");
	}
	Check(budget.TakeReport() == 0 && budget.TakeReport() == 0,
	      "process trace report budget did not stay exhausted");
	Check(budget.TakeSamples(16384) == 8192 && budget.TakeSamples(512) == 512,
	      "PCM scan budget did not cap each block prefix");
	budget.samples.store(AudioTraceBudget::SAMPLES_MAX - 3);
	Check(budget.TakeSamples(128) == 3 && budget.TakeSamples(128) == 0,
	      "PCM scan exceeded the process-wide sample budget");
}

void TestPlaybackLifetimeAcrossProbeGaps() {
	{
		Fixture f;
		cold_open_us = 600000;
		cold_close_us = 40000;
		const auto start = now;
		const auto probe = f.audio.AudioOutOpen(0, 256, 48000, Audio::Format::Float8Ch);
		Check(probe.IsValid() && now - start == cold_open_us,
		      "first playback open did not use the real cold device path");
		Check(f.audio.AudioOutClose(probe) && streams.empty() && logical_devices.size() == 1,
		      "closing a probe did not destroy its stream while retaining an unbound keepalive");
		const auto warm_start = now;
		const auto second_probe = f.audio.AudioOutOpen(0, 256, 48000, Audio::Format::Float8Ch);
		Check(f.audio.AudioOutClose(second_probe), "second probe close failed");
		const auto main = f.Open();
		Check(now == warm_start && audio_inits == 1 && physical_opens == 1 && physical_closes == 0,
		      "probe gaps repeated audio subsystem/physical device startup or shutdown");
		Check(streams.front()->spec.channels == 2 && streams.front()->spec.format == SDL_AUDIO_F32,
		      "stereo playback inherited the capability probe's eight-channel format");
		f.pcm[0] = 0.25f;
		f.Output(main);
		const auto main_device = streams.front()->device;
		const auto second = f.audio.AudioOutOpen(1, 128, 24000, Audio::Format::Signed16bitMono);
		Check(second.IsValid() && streams.size() == 2 &&
		          streams.back()->spec.channels == 1 && streams.back()->spec.freq == 24000 &&
		          streams.back()->spec.format == SDL_AUDIO_S16 && streams.back()->frames == 0 &&
		          streams.front()->device == main_device && streams.front()->frames != 0,
		      "independent ports lost their requested formats or shared PCM queues");
		Check(f.audio.AudioOutClose(second) && streams.size() == 1 &&
		          streams.front()->device == main_device,
		      "closing another port invalidated active playback");
		Check(f.audio.AudioOutClose(main) && streams.empty() && logical_devices.size() == 1,
		      "last guest close leaked PCM or released the warm device");
	}
	Check(logical_devices.empty() && audio_refs == 0 && audio_inits == 1 && audio_quits == 1 &&
	          physical_opens == 1 && physical_closes == 1,
	      "backend shutdown did not release exactly its owned keepalive/subsystem reference");
}

void TestPlaybackLifetimeFailureRecovery() {
	{
		Fixture f;
		fail_init = true;
		auto port = f.Open();
		Check(!f.audio.AudioOutHasDevice(port) && audio_refs == 0 && logical_devices.empty(),
		      "failed subsystem initialization fabricated a device or retained a reference");
		f.audio.AudioOutClose(port);
		fail_init = false;
		fail_open = true;
		port = f.Open();
		Check(!f.audio.AudioOutHasDevice(port) && audio_refs == 1 && logical_devices.empty(),
		      "failed stream open fabricated playback or lost the backend's subsystem reference");
		f.audio.AudioOutClose(port);
		fail_open = false;
		fail_resume = true;
		port = f.Open();
		Check(!f.audio.AudioOutHasDevice(port) && streams.empty() && logical_devices.empty(),
		      "failed stream resume leaked a stream/device");
		f.audio.AudioOutClose(port);
		fail_resume = false;
		fail_keepalive = true;
		port = f.Open();
		Check(f.audio.AudioOutHasDevice(port) && logical_devices.size() == 1,
		      "keepalive failure rejected an otherwise working playback stream");
		f.Output(port);
		f.audio.AudioOutClose(port);
		Check(logical_devices.empty(), "failed keepalive left a stale device handle");
		fail_keepalive = false;
		port = f.Open();
		Check(f.audio.AudioOutHasDevice(port) && logical_devices.size() == 2 && audio_inits == 1,
		      "subsequent open did not retry a failed playback/keepalive setup");
		// A failed later open must not release the retained default device or an active port.
		fail_open = true;
		const auto unavailable = f.Open();
		Check(!f.audio.AudioOutHasDevice(unavailable) && f.audio.AudioOutHasDevice(port) &&
		          logical_devices.size() == 2,
		      "an unavailable default during another open invalidated active playback");
		f.audio.AudioOutClose(unavailable);
		fail_open = false;
		const auto recovered = f.Open();
		Check(f.audio.AudioOutHasDevice(recovered) && logical_devices.size() == 3,
		      "default playback could not be reopened after temporary unavailability");
	}
	Check(streams.empty() && logical_devices.empty() && audio_refs == 0 && audio_quits == 1 &&
	          physical_opens == physical_closes,
	      "failure/retry paths leaked a stream/device/subsystem reference at shutdown");
}

void TestPlaybackLifetimePreservesOtherAudioOwners() {
	SDL_AudioDeviceID external = 0;
	{
		Fixture f;
		Check(Fake::InitSubSystem(SDL_INIT_AUDIO), "external audio owner initialization failed");
		const SDL_AudioSpec spec {SDL_AUDIO_S16, 2, 48000};
		external = Fake::OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
		Check(f.audio.WarmOutput(), "warmup could not coexist with an external audio owner");
		const auto port = f.Open();
		Check(f.audio.AudioOutClose(port) && logical_devices.size() == 2,
		      "closing a guest port released another owner's device or its own keepalive");
	}
	Check(audio_refs == 1 && audio_inits == 2 && audio_quits == 1 &&
	          logical_devices.size() == 1 && logical_devices.front() == external && physical_closes == 0,
	      "audio backend shutdown released another owner's subsystem/device reference");
	Fake::CloseAudioDevice(external);
	Fake::QuitSubSystem(SDL_INIT_AUDIO);
	Check(audio_refs == 0 && logical_devices.empty() && physical_closes == 1,
	      "external audio owner could not cleanly close after backend shutdown");
}

void TestRuntimeInitializeWarmsBeforeGuestPorts() {
	Fixture f;
	cold_open_us = 600000;
	const auto start = now;
	Libs::Audio::Initialize();
	Check(now - start == cold_open_us && streams.empty() && logical_devices.size() == 1 &&
	          audio_refs == 1 && physical_opens == 1 && warm_opens == 1,
	      "common Audio initialization did not pay the cold startup before guest port creation");
	const auto guest_start = now;
	const auto handle = Libs::Audio::AudioInternal::AudioOutOpen(
	    0, 256, 48000, Audio::Format::FloatStereo);
	Check(handle == 1 && now == guest_start && streams.size() == 1 &&
	          streams.front()->spec.channels == 2 && streams.front()->spec.format == SDL_AUDIO_F32 &&
	          streams.front()->frames == 0 && physical_opens == 1,
	      "warmup fabricated a guest port/PCM or changed the real guest stream format");
	Libs::Audio::AudioInternal::AudioOutClose(handle);
	Check(streams.empty() && logical_devices.size() == 1,
	      "guest close released the initialized warm device");
	Libs::Audio::Shutdown();
	Check(logical_devices.empty() && audio_refs == 0 && audio_quits == 1 && physical_closes == 1,
	      "initialized warm device was not released at common Audio shutdown");
}

void TestWarmOutputRetryAndIdempotentLifetime() {
	{
		Fixture f;
		fail_init = true;
		Check(!f.audio.WarmOutput() && audio_refs == 0 && logical_devices.empty(),
		      "unavailable warm initialization fabricated a device or owned reference");
		fail_init = false;
		fail_open = true;
		Check(!f.audio.WarmOutput() && audio_refs == 1 && logical_devices.empty(),
		      "failed warm device open fabricated success or lost the initialized subsystem");
		fail_open = false;
		const auto port = f.Open();
		Check(port.ToInt() == 1 && f.audio.AudioOutHasDevice(port) && logical_devices.size() == 2 &&
		          audio_inits == 1 && warm_opens == 1,
		      "ordinary first guest open did not recover from unavailable warm playback");
		Check(f.audio.WarmOutput() && f.audio.WarmOutput() && logical_devices.size() == 2 && warm_opens == 1,
		      "warmup duplicated an existing keepalive or device open");
	}
	Check(logical_devices.empty() && audio_refs == 0 && physical_opens == 1 && physical_closes == 1,
	      "warmup failure/retry leaked playback resources");
	{
		Fixture f;
		Check(f.audio.WarmOutput() && f.audio.WarmOutput() && warm_opens == 1 && streams.empty(),
		      "warmup without guest ports created PCM streams or reopened the warm device");
	}
	Check(logical_devices.empty() && audio_refs == 0 && audio_quits == 1 && physical_closes == 1,
	      "shutdown without ever opening a guest port leaked the warm device");
}

void TestTraceClockCorrelationReadOrder() {
	uint32_t reads = 0;
	const auto sample = Libs::Audio::CaptureAudioTraceClockSample(
	    [&]() -> uint64_t {
		    Check(reads == 0 || reads == 2, "process clock did not bracket the UTC read");
		    return reads++ == 0 ? 100 : 104;
	    },
	    [&]() -> int64_t {
		    Check(reads++ == 1, "UTC clock was not read between the two process-clock samples");
		    return 1793790000000000LL;
	    });
	Check(reads == 3 && sample.process_before_us == 100 && sample.process_after_us == 104 &&
	          sample.utc_us == 1793790000000000LL,
	      "clock correlation lost its exact values or sampling uncertainty bounds");
}

void TestNativeOutputLease() {
	using namespace Libs::Audio::AudioInternal;
	using Result = NativeQueueResult;
	Fixture fixture;
	Libs::Audio::Initialize();
	const NativeOutputIdentity identity {100, 65542, 65544, 1};
	Check(!OpenNativeOutput(identity, 48000, Format::Float8Ch, 1, 960), "native unsupported layout rejected");
	fail_open = true;
	Check(!OpenNativeOutput(identity, 48000, Format::FloatStereo, 1, 960), "native no-device is not silent success");
	fail_open = false;
	auto stream = OpenNativeOutput(identity, 48000, Format::FloatStereo, 1, 960);
	Check(stream && streams.size() == 1 && physical_opens == 1 && stream->Status().paused,
	      "native lease uses shared warm device, starts paused, has independent queue");
	std::vector<uint8_t> pcm(480 * 2 * sizeof(float), 0);
	pcm[17] = 0x34;
	Check(stream->TryQueue(pcm, 480, 1) == Result::Accepted && streams.front()->pcm == pcm,
	      "native exact PCM bytes accepted without block padding");
	now += 1000000;
	Check(stream->Status().queued_frames == 480, "paused native stream does not consume queued PCM");
	Check(stream->TryQueue(pcm, 480, 1) == Result::Accepted &&
	      stream->TryQueue(pcm, 480, 1) == Result::Backpressure && sleeps.empty(),
	      "native bounded queue never waits or clears unplayed data");
	Check(stream->SetPaused(false), "native resume");
	now += 10000;
	Check(stream->Status().queued_frames == 480 && stream->TryQueue(pcm, 480, 1) == Result::Accepted,
	      "actual consumption frees bounded capacity");
	Check(stream->SetPaused(true) && stream->SetGain(.25f) && streams.front()->gain == .25f,
	      "native controls target only owned stream");
	Check(!stream->SetGain(std::numeric_limits<float>::quiet_NaN()), "native nonfinite gain rejected");
	Check(stream->Flush(2) && stream->Status().queued_frames == 0 &&
	      stream->TryQueue(pcm, 480, 1) == Result::Invalid, "native flush clears queue and rejects old generation");
	fail_put = true;
	Check(stream->TryQueue(pcm, 480, 2) == Result::Failed && stream->Status().queued_frames == 0,
	      "SDL submission failure is not native queue acceptance");
	fail_put = false; fail_query = true;
	Check(stream->TryQueue(pcm, 480, 2) == Result::Failed, "queue-query failure cannot evade capacity bound");
	fail_query = false; fail_clear = true;
	Check(!stream->Flush(3) && stream->Status().generation == 2, "failed clear cannot advance generation");
	fail_clear = false;
	Check(stream->TryQueue(std::span(pcm).first(8), 1, 2) == Result::Accepted && streams.front()->pcm.size() == 8,
	      "short final PCM block is not padded or overread");
	stream->Retire();
	Check(stream->TryQueue(pcm, 480, 2) == Result::Retired && !stream->SetPaused(false),
	      "retired lease rejects new work");
	stream->Close();
	auto replacement = OpenNativeOutput({101, 65545, 65546, 2}, 48000, Format::FloatMono, 1, 960);
	Check(replacement && stream->IsClosed() && stream->TryQueue(pcm, 480, 2) == Result::Retired,
	      "old native lease cannot target replacement owner's stream");
	const auto guest = AudioOutOpen(0, 256, 48000, Format::FloatStereo);
	Check(guest == 1, "native leases do not consume guest handle identities");
	AudioOutClose(guest);
	Libs::Audio::Shutdown();
	Check(replacement->IsClosed() && replacement->Status().result == Result::Retired &&
	      streams.empty() && logical_devices.empty() && audio_refs == 0,
	      "shared shutdown closes retained native leases before audio subsystem teardown");
}

void TestNativeOutputRetirementConcurrency() {
	using namespace Libs::Audio::AudioInternal;
	Fixture fixture;
	Libs::Audio::Initialize();
	auto stream = OpenNativeOutput({100, 1, 2, 1}, 48000, Format::FloatMono, 1, 960);
	Check(stream != nullptr, "concurrent native lease acquired");
	std::binary_semaphore entered {0}, resume {0}, closed {0};
	held_put_entered = &entered; held_put_resume = &resume;
	NativeQueueResult queued = NativeQueueResult::Failed;
	std::array<uint8_t, 4> sample {};
	std::thread producer([&] { queued = stream->TryQueue(sample, 1, 1); });
	Check(entered.try_acquire_for(std::chrono::seconds(2)), "producer entered real queue call");
	stream->Retire(); // Must not block on the queue operation held above.
	std::thread closer([&] { stream->Close(); closed.release(); });
	Check(!closed.try_acquire_for(std::chrono::milliseconds(20)), "close waits for admitted queue operation");
	resume.release();
	producer.join();
	Check(closed.try_acquire_for(std::chrono::seconds(2)), "close drains after producer returns");
	closer.join();
	held_put_entered = held_put_resume = nullptr;
	Check(queued == NativeQueueResult::Accepted && stream->IsClosed() &&
	      stream->TryQueue(sample, 1, 1) == NativeQueueResult::Retired,
	      "in-flight admission finishes, retired lease never queues into a destroyed stream");
	Libs::Audio::Shutdown();
}

void TestNativeOutputLastOwnerShutdown() {
	using namespace Libs::Audio::AudioInternal;
	Fixture fixture;
	Libs::Audio::Initialize();
	auto stream = OpenNativeOutput({100, 1, 2, 1}, 48000, Format::FloatMono, 1, 960);
	std::weak_ptr<NativeOutput> weak = stream;
	std::binary_semaphore entered {0}, resume {0}, shutdown_started {0}, shutdown_done {0};
	held_destroy_entered = &entered; held_destroy_resume = &resume;
	std::thread last_owner([lease = std::move(stream)]() mutable { lease.reset(); });
	Check(entered.try_acquire_for(std::chrono::seconds(2)) && weak.expired(),
	      "last public lease expires while SDL destruction remains in flight");
	std::thread shutdown([&] {
		shutdown_started.release(); Libs::Audio::Shutdown(); shutdown_done.release();
	});
	shutdown_started.acquire();
	Check(!shutdown_done.try_acquire_for(std::chrono::milliseconds(20)),
	      "backend teardown waits for last-owner destructor even after weak expiration");
	resume.release();
	last_owner.join();
	Check(shutdown_done.try_acquire_for(std::chrono::seconds(2)), "shutdown drains closeable state");
	shutdown.join();
	held_destroy_entered = held_destroy_resume = nullptr;
	Check(streams.empty() && audio_refs == 0 && logical_devices.empty(), "last-owner teardown leaves no resources");
}

void TestNativeOutputResamplerEndInput() {
	using namespace Libs::Audio::AudioInternal;
	using Result = NativeQueueResult;
	Fixture fixture;
	Libs::Audio::Initialize();
	retain_resampler_tail = true;
	Check(!OpenNativeOutput({100, 1, 2, 1}, 7350, Format::FloatMono, 1, 7),
	      "capacity too small for retained resampler tail rejected");
	auto stream = OpenNativeOutput({100, 1, 2, 1}, 7350, Format::FloatMono, 1, 1032);
	Check(stream && stream->SetPaused(false), "actual 7350 Hz AAC rate supported");
	std::vector<uint8_t> block(1024 * sizeof(float));
	Check(stream->TryQueue(block, 1024, 1) == Result::Accepted, "queue AAC block with resampler headroom");
	now += 1000000;
	Check(stream->Status().queued_frames == 7 && stream->TryQueue(block, 1024, 1) == Result::Accepted,
	      "retained tail cannot permanently block the next whole block");
	std::vector<uint8_t> oversized(1025 * sizeof(float));
	Check(stream->TryQueue(oversized, 1025, 1) == Result::Invalid, "oversized atomic block must be split");
	now += 1000000;
	fail_end = true;
	Check(!stream->EndInput(1) && !stream->Status().input_ended, "failed drain marker cannot report ended input");
	fail_end = false;
	Check(!stream->EndInput(2) && stream->EndInput(1) && stream->EndInput(1) && input_ends == 1 && clears == 0,
	      "end-input validates generation, flushes resampler once, never clears PCM");
	Check(stream->TryQueue(block, 1024, 1) == Result::Invalid, "end marker closes input admission");
	now += 1000000;
	Check(stream->Status().queued_frames == 0, "finite resampler tail is released");
	Check(stream->Flush(2) && !stream->Status().input_ended && stream->TryQueue(block, 1024, 2) == Result::Accepted,
	      "discontinuity clears EOS and opens a new generation");
	stream->Retire();
	Check(!stream->EndInput(2), "retirement closes EOS admission");
	Libs::Audio::Shutdown();
}

void TestRealSdlFiniteResamplerTail() {
	// Exercise the linked SDL converter, without a physical device or the fake
	// consumer, to verify the retained-tail assumption used by native admission.
	const SDL_AudioSpec input {SDL_AUDIO_F32, 1, 7350}, output {SDL_AUDIO_F32, 1, 48000};
	auto* stream = SDL_CreateAudioStream(&input, &output);
	Check(stream != nullptr, "real SDL resampler created");
	std::array<float, 1024> pcm {};
	pcm.fill(.25f);
	Check(SDL_PutAudioStreamData(stream, pcm.data(), sizeof(pcm)), "real resampler accepts finite PCM");
	std::array<float, 8192> converted {};
	const auto first = SDL_GetAudioStreamData(stream, converted.data(), sizeof(converted));
	Check(first > 0 && SDL_GetAudioStreamAvailable(stream) == 0, "real converter reaches withheld finite tail");
	const auto retained = SDL_GetAudioStreamQueued(stream);
	Check(retained > 0 && retained <= 7 * int(sizeof(float)), "real retained input fits reserved headroom");
	Check(SDL_FlushAudioStream(stream) && SDL_GetAudioStreamAvailable(stream) > 0,
	      "real end-input releases remaining resampled samples");
	const auto tail = SDL_GetAudioStreamData(stream, converted.data(), sizeof(converted));
	Check(tail > 0 && SDL_GetAudioStreamAvailable(stream) == 0 && SDL_GetAudioStreamQueued(stream) == 0,
	      "real finite PCM drains without clear or fabricated padding submission");
	SDL_DestroyAudioStream(stream);
}
} // namespace

int main() {
	TestPrimingAndSteadyCadence();
	TestUnderrunAndStalledQueueRecovery();
	TestFallbackClockAndLongGap();
	TestAsyncDoesNotAccumulateDeadlines();
	TestSynchronizedBatchAndInactivePorts();
	TestFallbackUsesEachPortsPeriod();
	TestFailedQueueUsesFallbackClock();
	TestControllerSpeakerPacing();
	TestInvalidBatchSize();
	TestZeroOutputFrequency();
	TestChannelGains();
	TestTraceQueueUnitsAndBudgets();
	TestPlaybackLifetimeAcrossProbeGaps();
	TestPlaybackLifetimeFailureRecovery();
	TestPlaybackLifetimePreservesOtherAudioOwners();
	TestRuntimeInitializeWarmsBeforeGuestPorts();
	TestWarmOutputRetryAndIdempotentLifetime();
	TestTraceClockCorrelationReadOrder();
	TestNativeOutputLease();
	TestNativeOutputRetirementConcurrency();
	TestNativeOutputLastOwnerShutdown();
	TestNativeOutputResamplerEndInput();
	TestRealSdlFiniteResamplerTail();
	Check(streams.empty() && logical_devices.empty() && audio_refs == 0, "output resources leaked");
	std::puts("AudioOutTimingTests: all cases passed");
}
