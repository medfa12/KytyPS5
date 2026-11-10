#include "libs/audio.h"
#include "libs/audioPcmTrace.h"
#include "loader/runtimeChildClient.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/audio_internal.h"
#include "libs/controller.h"
#include "libs/dualSenseHaptics.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <magic_enum.hpp>
#include <mutex>
#include <vector>

namespace Libs::Audio {

namespace {

constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUDIO3D   = 126;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

constexpr uint32_t AUDIO_OUT_PARAM_FORMAT_MASK = 0x000000ffu;
constexpr uint64_t AUDIO_OUT_TARGET_LATENCY_US = 40000;

// Process-wide budgets also bound repeated capability-probe opens/closes.
struct AudioTraceBudget {
	static constexpr uint32_t REPORTS_MAX = 256;
	static constexpr uint32_t SAMPLES_MAX = 16 * 1024 * 1024;
	std::atomic<uint32_t> reports {0};
	std::atomic<uint32_t> samples {0};
	std::atomic<uint64_t> generation {0};

	uint32_t TakeReport() {
		auto count = reports.load(std::memory_order_relaxed);
		while (count < REPORTS_MAX) {
			if (reports.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
				return count + 1;
			}
		}
		return 0;
	}

	uint32_t TakeSamples(uint32_t requested) {
		auto count = samples.load(std::memory_order_relaxed);
		while (count < SAMPLES_MAX) {
			const auto take = std::min({requested, 8192u, SAMPLES_MAX - count});
			if (samples.compare_exchange_weak(count, count + take, std::memory_order_relaxed)) {
				return take;
			}
		}
		return 0;
	}
};

AudioTraceBudget g_audio_trace;

static bool AudioTraceEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_AUDIO_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static int64_t AudioTraceQueuedUs(int bytes, uint32_t bytes_per_frame, uint32_t rate) {
	if (bytes < 0 || bytes_per_frame == 0 || rate == 0) {
		return -1;
	}
	return static_cast<int64_t>(static_cast<uint64_t>(bytes) * 1000000ULL /
	                            (static_cast<uint64_t>(bytes_per_frame) * rate));
}

struct AudioTraceClockSample {
	uint64_t process_before_us;
	int64_t utc_us;
	uint64_t process_after_us;
};

template <typename ProcessClock, typename UtcClock>
static AudioTraceClockSample CaptureAudioTraceClockSample(ProcessClock process_clock, UtcClock utc_clock) {
	const auto before = process_clock();
	const auto utc = utc_clock();
	const auto after = process_clock();
	return {before, utc, after};
}

static void TraceAudioClockCorrelation() {
	static std::atomic_bool reported {false};
	if (!AudioTraceEnabled() || reported.exchange(true, std::memory_order_relaxed)) {
		return;
	}
	const auto sample = CaptureAudioTraceClockSample(
	    [] { return LibKernel::KernelGetProcessTime(); },
	    [] { return std::chrono::duration_cast<std::chrono::microseconds>(
	                    std::chrono::system_clock::now().time_since_epoch()).count(); });
	if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
		LOGF("AudioOut trace: seq=%u event=clock_correlation process_before_us=%llu utc_us=%lld "
		     "process_after_us=%llu\n", seq,
		     static_cast<unsigned long long>(sample.process_before_us),
		     static_cast<long long>(sample.utc_us),
		     static_cast<unsigned long long>(sample.process_after_us));
	}
}

constexpr int      AUDIO_IN_SILENT_STATE_DEVICE_NONE = 0x1;
constexpr uint32_t AUDIO_IN_GRAIN_MAX_ASYNC          = 384;

struct OutputClock {
	uint64_t next_time = 0;
	uint64_t remainder = 0;

	uint64_t Advance(uint64_t now, uint32_t frames, uint32_t rate) {
		// Preserve small scheduling errors; discard timing debt after a discontinuity.
		if (next_time == 0 || (now > next_time && now - next_time > 100000)) {
			next_time = now;
			remainder = 0;
		}
		const auto deadline = next_time;
		const auto duration = 1000000ULL * frames + remainder;
		next_time += duration / rate;
		remainder = duration % rate;
		return deadline;
	}
};

static bool audio_out_port_type_is_valid(int type) {
	return (type >= AUDIO_OUT_PORT_TYPE_MAIN && type <= AUDIO_OUT_PORT_TYPE_PADSPK) ||
	       type == AUDIO_OUT_PORT_TYPE_VIBRATION || type == AUDIO_OUT_PORT_TYPE_AUDIO3D ||
	       type == AUDIO_OUT_PORT_TYPE_AUX;
}

} // namespace

struct AudioInternal::NativeOutput::Impl {
	NativeOutputIdentity identity;
	mutable std::mutex mutex;
	std::atomic_bool retired {false};
	SDL_AudioStream* stream = nullptr;
	uint64_t generation = 0;
	uint32_t bytes_per_frame = 0, max_frames = 0;
	bool paused = true;
	bool input_ended = false;
	float requested_gain = 1.0f;
	void Close() {
		retired.store(true, std::memory_order_release);
		std::lock_guard lock(mutex);
		if (stream) SDL_DestroyAudioStream(stream);
		stream = nullptr;
	}
};

class Audio {
public:
	bool SetRuntimeFocus(bool enabled);
	using Format = AudioInternal::Format;

	class Id {
	public:
		explicit Id(int id): m_id(id - 1) {}
		[[nodiscard]] int  ToInt() const { return m_id + 1; }
		[[nodiscard]] bool IsValid() const { return m_id >= 0; }

		friend class Audio;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int audio_id) {
			Id r;
			r.m_id = audio_id;
			return r;
		}
		[[nodiscard]] int GetId() const { return m_id; }

		int m_id = -1;
	};

	struct OutputParam {
		Id           handle;
		const void*  data  = nullptr;
		const float* gains = nullptr;
	};

	Audio() = default;
	virtual ~Audio();

	KYTY_CLASS_NO_COPY(Audio);

	bool     WarmOutput();
	std::shared_ptr<AudioInternal::NativeOutput> OpenNativeOutput(AudioInternal::NativeOutputIdentity identity,
	    uint32_t rate, Format format, uint64_t generation, uint32_t max_queued_frames);
	Id       AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioOutClose(Id handle);
	bool     AudioOutValid(Id handle);
	bool     AudioOutHasDevice(Id handle);
	bool     AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume);
	uint32_t AudioOutOutputs(OutputParam* params, uint32_t num, bool blocking = true);
	bool     AudioOutGetStatus(Id handle, int* type, int* channels_num);

	Id       AudioInOpen(uint32_t samples_num, uint32_t freq, Format format, bool asynchronous);
	int      AudioInClose(Id handle);
	int      AudioInGetSilentState(Id handle);
	int      AudioInInput(Id handle, void* dest);

	static constexpr int OUT_PORTS_MAX = 32;
	static constexpr int IN_PORTS_MAX  = 8;

private:
	struct PortOut {
		bool        used        = false;
		int         type        = 0;
		uint32_t    samples_num = 0;
		uint32_t    freq        = 0;
		Format      format      = Format::Unknown;
		OutputClock clock;
		bool        queue_primed = false;
		int         channels_num = 0;
		int         volume[12]   = {};
		uint64_t    trace_id = 0;
		int         trace_handle = 0;
		uint64_t    trace_open_us = 0;
		uint64_t    trace_frames = 0;
		uint64_t    trace_blocks = 0;
		AudioPcmTraceWindow trace_pcm;
		uint32_t    trace_reports = 0;
		uint32_t    trace_wait_reports = 0;
		uint32_t    trace_clock_reports = 0;
		bool        trace_nonzero = false;

		SDL_AudioStream*                      stream  = nullptr;
		Controller::DualSenseHaptics::Stream* haptics = nullptr;
	};

	struct PortIn {
		bool              used            = false;
		bool              busy            = false;
		bool              asynchronous    = false;
		uint32_t          samples_num     = 0;
		uint32_t          freq            = 0;
		uint32_t          bytes_per_frame = 0;
		uint64_t          last_input_time = 0;
		SDL_AudioStream*  stream          = nullptr;
		SDL_AudioDeviceID device          = 0;
	};

	PortIn* GetAudioInPort(Id handle); // Caller holds m_mutex.

	Common::Mutex m_mutex;
	PortOut       m_out_ports[OUT_PORTS_MAX];
	PortIn        m_in_ports[IN_PORTS_MAX];
	bool              m_output_initialized = false;
	SDL_AudioDeviceID m_output_keepalive = 0;
	// Retain closeable state even while the last public lease is destructing.
	// Shutdown and that destructor serialize on the same state's mutex before
	// SDL subsystem teardown can implicitly destroy any remaining streams.
	std::vector<std::shared_ptr<AudioInternal::NativeOutput::Impl>> m_native_outputs;

	static bool            FormatIsFloat(Format format);
	static bool            FormatIsStd(Format format);
	static uint32_t        BytesPerSample(Format format);
	static uint32_t        OutputChannels(const PortOut& port);
	static SDL_AudioFormat SdlFormat(Format format);
	bool                   InitializeSdlOutput(); // Caller holds m_mutex.
	bool                   OpenSdlDevice(PortOut* port); // Caller holds m_mutex.
	static void            CloseSdlDevice(PortOut* port);
	static void            OpenSdlDevice(PortIn* port, Format format);
	static void            CloseSdlDevice(PortIn* port);
	static const void*     PrepareOutputBuffer(const PortOut& port, const void* data,
	                                           std::vector<uint8_t>* buffer, float gain,
	                                           const float* gains);
	static bool QueueSdlAudio(PortOut* port, const void* data, bool blocking, float gain,
	                          const float* gains);
};

static Audio* g_audio = nullptr;
static std::atomic_bool g_runtime_audio_focus {true};
// Native stream acquisition cannot race destruction of the shared backend.
static std::mutex g_native_audio_lifecycle;

namespace AudioInternal {

NativeOutput::NativeOutput(std::shared_ptr<Impl> impl): m_impl(std::move(impl)) {}
NativeOutput::~NativeOutput() { Close(); }
void NativeOutput::Retire() { m_impl->retired.store(true, std::memory_order_release); }
void NativeOutput::Close() {
	m_impl->Close();
}
bool NativeOutput::IsClosed() const {
	std::lock_guard lock(m_impl->mutex);
	return m_impl->stream == nullptr;
}
NativeQueueResult NativeOutput::TryQueue(std::span<const uint8_t> pcm, uint32_t frames, uint64_t generation) {
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->retired.load(std::memory_order_acquire)) return NativeQueueResult::Retired;
	if (!m_impl->stream) return NativeQueueResult::Unavailable;
	if (!frames || m_impl->input_ended || generation != m_impl->generation || !pcm.data() ||
	    pcm.size() != uint64_t(frames) * m_impl->bytes_per_frame) return NativeQueueResult::Invalid;
	// Bundled SDL's resampler retains at most seven future-padding frames.
	// Leave room for that tail plus the next entire admitted block.
	if (frames > m_impl->max_frames - 8) return NativeQueueResult::Invalid;
	const auto queued = SDL_GetAudioStreamQueued(m_impl->stream);
	if (queued < 0) return NativeQueueResult::Failed;
	if (uint64_t(queued) + pcm.size() > uint64_t(m_impl->max_frames) * m_impl->bytes_per_frame)
		return NativeQueueResult::Backpressure;
	return SDL_PutAudioStreamData(m_impl->stream, pcm.data(), static_cast<int>(pcm.size()))
	           ? NativeQueueResult::Accepted : NativeQueueResult::Failed;
}
NativeOutputStatus NativeOutput::Status() const {
	std::lock_guard lock(m_impl->mutex);
	NativeOutputStatus result {NativeQueueResult::Unavailable, m_impl->generation, 0,
	                           m_impl->paused, m_impl->input_ended};
	if (m_impl->retired.load(std::memory_order_acquire)) { result.result = NativeQueueResult::Retired; return result; }
	if (!m_impl->stream) return result;
	const auto queued = SDL_GetAudioStreamQueued(m_impl->stream);
	result.result = queued < 0 ? NativeQueueResult::Failed : NativeQueueResult::Accepted;
	if (queued >= 0) result.queued_frames = static_cast<uint32_t>(queued) / m_impl->bytes_per_frame;
	return result;
}
bool NativeOutput::SetPaused(bool paused) {
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->retired.load(std::memory_order_acquire) || !m_impl->stream) return false;
	if (paused == m_impl->paused) return true;
	const bool result = paused ? SDL_PauseAudioStreamDevice(m_impl->stream) : SDL_ResumeAudioStreamDevice(m_impl->stream);
	if (result) m_impl->paused = paused;
	return result;
}
bool NativeOutput::SetGain(float gain) {
	if (!std::isfinite(gain) || gain < 0 || gain > 1) return false;
	std::lock_guard lock(m_impl->mutex);
	m_impl->requested_gain = gain;
	return !m_impl->retired.load(std::memory_order_acquire) && m_impl->stream &&
	       SDL_SetAudioStreamGain(m_impl->stream, g_runtime_audio_focus.load() ? gain : 0.0f);
}
bool NativeOutput::Flush(uint64_t generation) {
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->retired.load(std::memory_order_acquire) || !m_impl->stream ||
	    generation <= m_impl->generation || !SDL_ClearAudioStream(m_impl->stream)) return false;
	m_impl->generation = generation;
	m_impl->input_ended = false;
	return true;
}
bool NativeOutput::EndInput(uint64_t generation) {
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->retired.load(std::memory_order_acquire) || !m_impl->stream ||
	    generation != m_impl->generation) return false;
	if (m_impl->input_ended) return true;
	if (!SDL_FlushAudioStream(m_impl->stream)) return false;
	m_impl->input_ended = true;
	return true;
}
std::shared_ptr<NativeOutput> OpenNativeOutput(NativeOutputIdentity identity, uint32_t rate,
                                             Format format, uint64_t generation, uint32_t max_frames) {
	std::lock_guard lock(g_native_audio_lifecycle);
	return g_audio ? g_audio->OpenNativeOutput(identity, rate, format, generation, max_frames) : nullptr;
}

int AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format) {
	if (g_audio == nullptr) {
		return 0;
	}

	auto id = g_audio->AudioOutOpen(type, samples_num, freq, format);
	return id.IsValid() ? id.ToInt() : 0;
}

void AudioOutClose(int handle) {
	if (g_audio != nullptr && handle > 0) {
		(void)g_audio->AudioOutClose(Audio::Id(handle));
	}
}

bool AudioOutHasDevice(int handle) {
	return g_audio != nullptr && handle > 0 && g_audio->AudioOutHasDevice(Audio::Id(handle));
}

uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking) {
	if (g_audio == nullptr || params == nullptr || num == 0) {
		return 0;
	}

	std::vector<Audio::OutputParam> output_params;
	output_params.reserve(num);
	for (uint32_t i = 0; i < num; i++) {
		if (params[i].handle > 0 && params[i].data != nullptr) {
			output_params.push_back(
			    Audio::OutputParam {Audio::Id(params[i].handle), params[i].data, params[i].gains});
		}
	}

	if (output_params.empty()) {
		return 0;
	}

	return g_audio->AudioOutOutputs(output_params.data(),
	                                static_cast<uint32_t>(output_params.size()), blocking);
}

} // namespace AudioInternal

void Initialize() {
	std::lock_guard native_lock(g_native_audio_lifecycle);
	EXIT_IF(g_audio != nullptr);

	g_audio = new Audio;
	TraceAudioClockCorrelation();
	(void)g_audio->WarmOutput();
}

void Shutdown() {
	std::lock_guard native_lock(g_native_audio_lifecycle);
	delete g_audio;
	g_audio = nullptr;
}

bool SetRuntimeAudioFocus(bool enabled) {
	std::lock_guard native_lock(g_native_audio_lifecycle);
	g_runtime_audio_focus.store(enabled);
	return !g_audio || g_audio->SetRuntimeFocus(enabled);
}
bool Audio::SetRuntimeFocus(bool enabled) {
	Common::LockGuard lock(m_mutex);
	bool result = true;
	for (auto& port: m_out_ports) {
		if (port.stream) { result = SDL_SetAudioStreamGain(port.stream, enabled ? 1.0f : 0.0f) && result; }
		// Drop queued physical controller PCM on focus loss; regular playback
		// queues KEEP draining. The next focused write reopens the controller path.
		if (!enabled && port.haptics) { Controller::DualSenseHaptics::Close(port.haptics); port.haptics = nullptr; }
	}
	for (const auto& state: m_native_outputs) {
		std::lock_guard stream_lock(state->mutex);
		if (state->stream) { result = SDL_SetAudioStreamGain(state->stream, enabled ? state->requested_gain : 0.0f) && result; }
	}
	return result;
}

Audio::~Audio() {
	for (auto& state: m_native_outputs) state->Close();
	for (auto& port: m_out_ports) {
		CloseSdlDevice(&port);
	}
	for (auto& port: m_in_ports) {
		CloseSdlDevice(&port);
	}
	if (m_output_keepalive != 0) {
		SDL_CloseAudioDevice(m_output_keepalive);
	}
	if (m_output_initialized) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

bool Audio::FormatIsFloat(Format format) {
	switch (format) {
		case Format::FloatMono:
		case Format::FloatStereo:
		case Format::Float8Ch:
		case Format::Float8ChStd:
		case Format::Float12Ch: return true;
		default: return false;
	}
}

bool Audio::FormatIsStd(Format format) {
	return (format == Format::Signed16bit8ChStd || format == Format::Float8ChStd);
}

uint32_t Audio::BytesPerSample(Format format) {
	return FormatIsFloat(format) ? sizeof(float) : sizeof(int16_t);
}

uint32_t Audio::OutputChannels(const PortOut& port) {
	// SDL only takes up to 8 channels. Keep the guest buffer's channel count separate.
	return std::min(port.channels_num, 8);
}

SDL_AudioFormat Audio::SdlFormat(Format format) {
	return FormatIsFloat(format) ? SDL_AUDIO_F32 : SDL_AUDIO_S16;
}

bool Audio::InitializeSdlOutput() {
	if (!m_output_initialized) {
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			LOGF("AudioOut: SDL audio init failed: %s\n", SDL_GetError());
			return false;
		}
		m_output_initialized = true;
	}
	return true;
}

bool Audio::WarmOutput() {
	const auto start = AudioTraceEnabled() ? LibKernel::KernelGetProcessTime() : 0;
	Common::LockGuard lock(m_mutex);
	if (m_output_keepalive != 0) {
		return true;
	}
	// Common runtime initialization precedes Graphics and guest execution. Open
	// a real default-following device now, without any guest port or PCM queue.
	// SDL chooses its actual format; later guest streams still specify theirs.
	if (InitializeSdlOutput()) {
		m_output_keepalive = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
		if (m_output_keepalive == 0) {
			LOGF("AudioOut: SDL playback warmup failed: %s\n", SDL_GetError());
		}
	}
	if (AudioTraceEnabled()) {
		if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
			const auto now = LibKernel::KernelGetProcessTime();
			LOGF("AudioOut trace: seq=%u event=warm t_us=%llu elapsed_us=%llu device=%d\n",
			     seq, static_cast<unsigned long long>(now),
			     static_cast<unsigned long long>(now - start), m_output_keepalive != 0 ? 1 : 0);
		}
	}
	// Failure is nonfatal; the ordinary port-open path retries initialization
	// and real stream/device creation, including its existing keepalive fallback.
	return m_output_keepalive != 0;
}

std::shared_ptr<AudioInternal::NativeOutput> Audio::OpenNativeOutput(
    AudioInternal::NativeOutputIdentity identity, uint32_t rate, Format format,
    uint64_t generation, uint32_t max_frames) {
	if (identity.owner <= 0 || identity.context <= 0 || identity.decoder <= 0 || !identity.epoch ||
	    !generation || rate < 7350 || rate > 192000 || max_frames < 16 || max_frames > rate) return nullptr;
	const int channels = format == Format::FloatMono || format == Format::Signed16bitMono ? 1 :
	                     format == Format::FloatStereo || format == Format::Signed16bitStereo ? 2 : 0;
	if (!channels) return nullptr;
	Common::LockGuard lock(m_mutex);
	std::erase_if(m_native_outputs, [](const auto& state) {
		std::lock_guard lock(state->mutex); return state->stream == nullptr;
	});
	if (m_native_outputs.size() >= 8 || !InitializeSdlOutput()) return nullptr;
	auto state = std::make_shared<AudioInternal::NativeOutput::Impl>();
	state->identity = identity;
	state->generation = generation;
	state->bytes_per_frame = BytesPerSample(format) * channels;
	state->max_frames = max_frames;
	auto output = std::shared_ptr<AudioInternal::NativeOutput>(new AudioInternal::NativeOutput(std::move(state)));
	const SDL_AudioSpec spec {SdlFormat(format), channels, static_cast<int>(rate)};
	// SDL creates this independent logical playback stream paused. It follows
	// the same default physical device kept warm by the existing shared backend.
	output->m_impl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!output->m_impl->stream) return nullptr;
	if (!SDL_SetAudioStreamGain(output->m_impl->stream, g_runtime_audio_focus.load() ? 1.0f : 0.0f)) { return nullptr; }
	if (!m_output_keepalive) {
		m_output_keepalive = SDL_OpenAudioDevice(SDL_GetAudioStreamDevice(output->m_impl->stream), nullptr);
	}
	m_native_outputs.push_back(output->m_impl);
	return output;
}

bool Audio::OpenSdlDevice(PortOut* port) {
	EXIT_IF(port == nullptr);
	if (!InitializeSdlOutput()) {
		return false;
	}

	SDL_AudioSpec desired {};
	desired.freq     = static_cast<int>(port->freq);
	desired.format   = SdlFormat(port->format);
	desired.channels = static_cast<int>(OutputChannels(*port));

	port->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, nullptr,
	                                         nullptr);
	if (port->stream == nullptr) {
		LOGF("AudioOut: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
		return false;
	}
	if (!SDL_SetAudioStreamGain(port->stream, g_runtime_audio_focus.load() ? 1.0f : 0.0f) || !SDL_ResumeAudioStreamDevice(port->stream)) {
		LOGF("AudioOut: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
		CloseSdlDevice(port);
		return false;
	}
	if (m_output_keepalive == 0) {
		// A separate, unbound logical device keeps SDL's physical playback device
		// warm across guest close/open gaps. Deriving it from this successful
		// stream preserves the negotiated device and its default-migration policy.
		// Each port still owns an independent stream/queue in its requested format.
		m_output_keepalive = SDL_OpenAudioDevice(SDL_GetAudioStreamDevice(port->stream), nullptr);
		if (m_output_keepalive == 0) {
			// Playback already works; a keepalive failure must not reject it. Retry
			// the optimization at a subsequent open, without touching active ports.
			LOGF("AudioOut: SDL playback keepalive open failed: %s\n", SDL_GetError());
		}
	}

	LOGF("AudioOut: opened SDL stream (%d Hz, %d ch, format 0x%04x)\n", desired.freq,
	     desired.channels, static_cast<unsigned>(desired.format));
	return true;
}

void Audio::CloseSdlDevice(PortOut* port) {
	EXIT_IF(port == nullptr);
	Controller::DualSenseHaptics::Close(port->haptics);
	port->haptics = nullptr;

	if (port->stream != nullptr) {
		SDL_DestroyAudioStream(port->stream);
	}
	port->stream = nullptr;
}

const void* Audio::PrepareOutputBuffer(const PortOut& port, const void* data,
                                       std::vector<uint8_t>* buffer, float gain,
                                       const float* gains) {
	EXIT_IF(data == nullptr);
	EXIT_IF(buffer == nullptr);

	const auto frames           = port.samples_num;
	const auto channels         = static_cast<uint32_t>(port.channels_num);
	const auto output_channels  = OutputChannels(port);
	const auto bytes_per_sample = BytesPerSample(port.format);
	const bool reorder          = channels >= 8 && !FormatIsStd(port.format);

	std::array<double, 12> scales {};
	bool                   volume_changed = false;
	for (uint32_t ch = 0; ch < channels; ch++) {
		scales[ch] = (static_cast<double>(port.volume[ch]) / 32768.0) * gain *
		             (gains != nullptr ? gains[ch] : 1.0f);
		volume_changed |= scales[ch] != 1.0;
	}

	if (!volume_changed && !reorder) {
		return data;
	}

	buffer->resize(frames * output_channels * bytes_per_sample);

	// SDL wants back speakers before side speakers; non-STD PCM has them reversed.
	static constexpr uint32_t SDL_8CH_MAP[8] = {0, 1, 2, 3, 6, 7, 4, 5};

	if (FormatIsFloat(port.format)) {
		auto*       dst = reinterpret_cast<float*>(buffer->data());
		const auto* src = static_cast<const float*>(data);

		for (uint32_t frame = 0; frame < frames; frame++) {
			for (uint32_t ch = 0; ch < output_channels; ch++) {
				const auto src_ch = reorder ? SDL_8CH_MAP[ch] : ch;
				dst[frame * output_channels + ch] =
				    static_cast<float>(src[frame * channels + src_ch] * scales[src_ch]);
			}
			if (channels == 12) {
				// Add the four top channels to their front/back channels, turning 12 into 8.
				// SDL handles the rest if the output device has fewer channels.
				static constexpr uint32_t HEIGHT_DST[4] = {0, 1, 4, 5};
				for (uint32_t ch = 0; ch < 4; ch++) {
					dst[frame * output_channels + HEIGHT_DST[ch]] +=
					    static_cast<float>(src[frame * channels + 8 + ch] * scales[8 + ch]);
				}
			}
		}
	} else {
		auto*       dst = reinterpret_cast<int16_t*>(buffer->data());
		const auto* src = static_cast<const int16_t*>(data);

		for (uint32_t frame = 0; frame < frames; frame++) {
			for (uint32_t ch = 0; ch < output_channels; ch++) {
				const auto src_ch = reorder ? SDL_8CH_MAP[ch] : ch;
				const double sample = src[frame * channels + src_ch] * scales[src_ch];
				dst[frame * output_channels + ch] = static_cast<int16_t>(
				    std::clamp(sample, -32768.0, 32767.0));
			}
		}
	}

	return buffer->data();
}

bool Audio::QueueSdlAudio(PortOut* port, const void* data, bool blocking, float gain,
                          const float* gains) {
	EXIT_IF(port == nullptr);

	if (port->stream == nullptr || data == nullptr) {
		return false;
	}

	std::vector<uint8_t> prepared_buffer;
	const void*          prepared_data = PrepareOutputBuffer(*port, data, &prepared_buffer, gain, gains);
	const auto           output_channels = OutputChannels(*port);
	const auto           prepared_size =
	    BytesPerSample(port->format) * output_channels * port->samples_num;
	const bool trace = port->trace_id != 0 &&
	                   g_audio_trace.reports.load(std::memory_order_relaxed) < AudioTraceBudget::REPORTS_MAX;
	const auto trace_entry_us = trace ? LibKernel::KernelGetProcessTime() : 0;
	const auto trace_queue_entry = trace ? SDL_GetAudioStreamQueued(port->stream) : -1;
	bool trace_cleared = false;
	bool trace_clear_ok = false;
	bool trace_underrun = false;

	uint32_t min_queued_size = 0;
	if (blocking) {
		const auto buffer_us = port->freq != 0 ? (1000000ULL * port->samples_num) / port->freq : 0;
		const auto buffers =
		    buffer_us != 0
		        ? static_cast<uint32_t>((AUDIO_OUT_TARGET_LATENCY_US + buffer_us - 1) / buffer_us)
		        : 2u;
		min_queued_size           = prepared_size * std::clamp(buffers, 2u, 16u);
		const auto wait_start      = LibKernel::KernelGetProcessTime();
		auto       queued          = SDL_GetAudioStreamQueued(port->stream);
		while (queued > static_cast<int>(min_queued_size)) {
			if (LibKernel::KernelGetProcessTime() - wait_start > 200000) {
				trace_clear_ok = SDL_ClearAudioStream(port->stream);
				trace_cleared = true;
				port->queue_primed = false;
				break;
			}
			Common::Thread::SleepMicro(1000);
			queued = SDL_GetAudioStreamQueued(port->stream);
		}
		if (queued < static_cast<int>(prepared_size)) {
			if (port->queue_primed) {
				trace_underrun = true;
				Common::DebugCounters::Add(Common::DebugCounters::Counter::AudioUnderruns);
			}
			port->queue_primed = false;
		}
	}

	const auto trace_queue_before = trace ? SDL_GetAudioStreamQueued(port->stream) : -1;
	const auto trace_submit_us = trace ? LibKernel::KernelGetProcessTime() : 0;
	const auto trace_wait_us = trace_submit_us - trace_entry_us;
	uint64_t trace_after_us = 0;
	auto trace_report = [&](const char* event, int queued_after, bool first_nonzero,
	                        uint32_t scanned_samples, float peak) {
		const auto seq = g_audio_trace.TakeReport();
		if (seq == 0) {
			return;
		}
		const auto bytes_per_frame = BytesPerSample(port->format) * output_channels;
		LOGF("AudioOut trace: seq=%u event=%s t_us=%llu after_put_us=%llu port=%llu handle=%d type=%d format=%u "
		     "rate=%u channels=%u sample_bytes=%u frames=%u block=%llu submitted_frames=%llu open_elapsed_us=%llu "
		     "blocking=%d primed=%d queue_entry_bytes=%d queue_before_bytes=%d queue_before_us=%lld "
		     "queue_after_bytes=%d queue_after_us=%lld pre_put_us=%llu clear=%d clear_ok=%d underrun=%d "
		     "first_nonzero_observed=%d seen_nonzero=%d scanned_samples=%u scan_complete=%d scanned_peak=%.6f\n",
		     seq, event, static_cast<unsigned long long>(trace_submit_us),
		     static_cast<unsigned long long>(trace_after_us),
		     static_cast<unsigned long long>(port->trace_id), port->trace_handle, port->type,
		     static_cast<unsigned>(port->format), port->freq, output_channels,
		     BytesPerSample(port->format), port->samples_num,
		     static_cast<unsigned long long>(port->trace_blocks),
		     static_cast<unsigned long long>(port->trace_frames),
		     static_cast<unsigned long long>(trace_submit_us - port->trace_open_us),
		     blocking ? 1 : 0, port->queue_primed ? 1 : 0, trace_queue_entry, trace_queue_before,
		     static_cast<long long>(AudioTraceQueuedUs(trace_queue_before, bytes_per_frame, port->freq)),
		     queued_after,
		     static_cast<long long>(AudioTraceQueuedUs(queued_after, bytes_per_frame, port->freq)),
		     static_cast<unsigned long long>(trace_wait_us), trace_cleared ? 1 : 0,
		     trace_clear_ok ? 1 : 0, trace_underrun ? 1 : 0,
		     first_nonzero ? 1 : 0, port->trace_nonzero ? 1 : 0,
		     scanned_samples, scanned_samples == port->samples_num * output_channels ? 1 : 0, peak);
	};
	const bool put_ok = SDL_PutAudioStreamData(port->stream, prepared_data, static_cast<int>(prepared_size));
	trace_after_us = trace ? LibKernel::KernelGetProcessTime() : 0;
	if (!put_ok) {
		LOGF("AudioOut: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
		if (trace) {
			trace_report("put_failed", SDL_GetAudioStreamQueued(port->stream), false, 0, 0.0f);
		}
		port->queue_primed = false;
		return false;
	}
	// Inspect only a bounded prefix of post-volume/reordered PCM. This is
	// submission evidence, not an SDL-consumed or speaker-presented timestamp.
	if (trace) {
		const auto queued_after = SDL_GetAudioStreamQueued(port->stream);
		const auto scanned_samples = !port->trace_nonzero
		    && port->trace_blocks % 128 == 0
		    ? g_audio_trace.TakeSamples(port->samples_num * output_channels) : 0;
		bool nonzero = false;
		float peak = 0.0f;
		for (uint32_t index = 0; index < scanned_samples; ++index) {
			const float sample = FormatIsFloat(port->format)
			    ? static_cast<const float*>(prepared_data)[index]
			    : static_cast<const int16_t*>(prepared_data)[index] / 32768.0f;
			peak = std::max(peak, std::fabs(sample));
			nonzero |= sample != 0.0f;
		}
		++port->trace_blocks;
		port->trace_frames += port->samples_num;
		if (FormatIsFloat(port->format) && port->trace_pcm.Scan()) {
			AudioPcmSummary input, output;
			const auto input_count = std::min(port->samples_num * static_cast<uint32_t>(port->channels_num), 8192u);
			const auto output_count = std::min(port->samples_num * output_channels, 8192u);
			for (uint32_t i = 0; i < input_count; ++i) input.Add(static_cast<const float*>(data)[i]);
			for (uint32_t i = 0; i < output_count; ++i) output.Add(static_cast<const float*>(prepared_data)[i]);
			if (port->trace_pcm.Report(output)) {
				if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
					LOGF("AudioOut trace: seq=%u event=pcm t_us=%llu port=%llu block=%llu input_scanned=%u input_nonfinite=%u input_above_one=%u input_peak=%.9g input_rms=%.9g input_hash=%llu output_scanned=%u output_nonfinite=%u output_above_one=%u output_peak=%.9g output_rms=%.9g output_hash=%llu queue_bytes=%d pre_put_us=%llu\n",
					     seq, static_cast<unsigned long long>(trace_submit_us), static_cast<unsigned long long>(port->trace_id),
					     static_cast<unsigned long long>(port->trace_blocks), input.count, input.nonfinite, input.above_one,
					     input.peak, input.Rms(), static_cast<unsigned long long>(input.hash), output.count, output.nonfinite,
					     output.above_one, output.peak, output.Rms(), static_cast<unsigned long long>(output.hash), queued_after,
					     static_cast<unsigned long long>(trace_wait_us));
				}
			}
		}
		const bool first_nonzero = nonzero && !port->trace_nonzero;
		port->trace_nonzero |= nonzero;
		const bool summary = port->trace_reports < 4 &&
		                     port->trace_frames >= uint64_t {port->trace_reports + 1} * port->freq;
		const bool wait = trace_wait_us >= 1000 && port->trace_wait_reports < 4;
		if (port->trace_blocks == 1 || first_nonzero || summary || wait || trace_cleared || trace_underrun) {
			trace_report("put", queued_after, first_nonzero, scanned_samples, peak);
		}
		if (summary) {
			++port->trace_reports;
		}
		if (wait) {
			++port->trace_wait_reports;
		}
	}
	// Unpaced priming must not advance the playback deadline into the future.
	if (!port->queue_primed) {
		port->clock = {};
	}
	if (blocking && !port->queue_primed &&
	    SDL_GetAudioStreamQueued(port->stream) >= static_cast<int>(min_queued_size)) {
		port->queue_primed = true;
	}

	return true;
}

Audio::Id Audio::AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format) {
	if (freq == 0) {
		return Id::Invalid();
	}

	const bool trace = AudioTraceEnabled() &&
	                   (type == AUDIO_OUT_PORT_TYPE_MAIN || type == AUDIO_OUT_PORT_TYPE_BGM);
	const auto trace_start = trace ? LibKernel::KernelGetProcessTime() : 0;
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < OUT_PORTS_MAX; id++) {
		if (!m_out_ports[id].used) {
			auto& port = m_out_ports[id];

			port.used        = true;
			port.type        = type;
			port.samples_num = samples_num;
			port.freq        = freq;
			port.format      = format;
			if (trace) {
				port.trace_id = g_audio_trace.generation.fetch_add(1, std::memory_order_relaxed) + 1;
				port.trace_handle = id + 1;
				port.trace_open_us = trace_start;
			}

			switch (format) {
				case Format::Signed16bitMono:
				case Format::FloatMono: port.channels_num = 1; break;
				case Format::Signed16bitStereo:
				case Format::FloatStereo: port.channels_num = 2; break;
				case Format::Signed16bit8Ch:
				case Format::Float8Ch:
				case Format::Signed16bit8ChStd:
				case Format::Float8ChStd: port.channels_num = 8; break;
				case Format::Float12Ch: port.channels_num = 12; break;
				default: EXIT("unknown format");
			}

			for (int i = 0; i < port.channels_num; i++) {
				port.volume[i] = 32768;
			}

			// Pad speaker ports keep the main output too; it plays them whenever no DualSense does.
			if (type != AUDIO_OUT_PORT_TYPE_VIBRATION) {
				OpenSdlDevice(&port);
			}
			if (!Loader::IsRuntimeChild() && g_runtime_audio_focus.load() && (type == AUDIO_OUT_PORT_TYPE_VIBRATION || type == AUDIO_OUT_PORT_TYPE_PADSPK)) {
				port.haptics =
				    Controller::DualSenseHaptics::Open(freq, type == AUDIO_OUT_PORT_TYPE_PADSPK);
			}
			if (trace) {
				if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
					const auto now = LibKernel::KernelGetProcessTime();
					LOGF("AudioOut trace: seq=%u event=open t_us=%llu port=%llu handle=%d type=%d "
					     "format=%u rate=%u channels=%u frames=%u elapsed_us=%llu device=%d\n",
					     seq, static_cast<unsigned long long>(now),
					     static_cast<unsigned long long>(port.trace_id), port.trace_handle, type,
					     static_cast<unsigned>(format), freq, OutputChannels(port), samples_num,
					     static_cast<unsigned long long>(now - trace_start), port.stream != nullptr ? 1 : 0);
				}
			}

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

bool Audio::AudioOutClose(Id handle) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		const auto trace_start = port.trace_id != 0 ? LibKernel::KernelGetProcessTime() : 0;
		CloseSdlDevice(&port);
		if (port.trace_id != 0) {
			if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
				const auto now = LibKernel::KernelGetProcessTime();
				LOGF("AudioOut trace: seq=%u event=close t_us=%llu port=%llu handle=%d "
				     "elapsed_us=%llu submitted_frames=%llu blocks=%llu seen_nonzero=%d\n",
				     seq, static_cast<unsigned long long>(now),
				     static_cast<unsigned long long>(port.trace_id), port.trace_handle,
				     static_cast<unsigned long long>(now - trace_start),
				     static_cast<unsigned long long>(port.trace_frames),
				     static_cast<unsigned long long>(port.trace_blocks), port.trace_nonzero ? 1 : 0);
			}
		}
		port = {};

		return true;
	}

	return false;
}

bool Audio::AudioOutValid(Id handle) {
	Common::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX &&
	        m_out_ports[handle.GetId()].used);
}

bool Audio::AudioOutHasDevice(Id handle) {
	Common::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX &&
	        m_out_ports[handle.GetId()].used && m_out_ports[handle.GetId()].stream != nullptr);
}

bool Audio::AudioOutGetStatus(Id handle, int* type, int* channels_num) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		*type         = port.type;
		*channels_num = port.channels_num;

		return true;
	}

	return false;
}

bool Audio::AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		for (int i = 0; i < port.channels_num; i++, bitflag >>= 1u) {
			auto bit = bitflag & 0x1u;

			if (bit == 1) {
				port.volume[i] = volume[i];

				LOGF("\t port.volume[%d] = volume[%d] (%d)\n", i, i, volume[i]);
			}
		}

		return true;
	}

	return false;
}

uint32_t Audio::AudioOutOutputs(OutputParam* params, uint32_t num, bool blocking) {
	EXIT_NOT_IMPLEMENTED(num == 0 || num > OUT_PORTS_MAX);
	EXIT_NOT_IMPLEMENTED(!AudioOutValid(params[0].handle));

	const auto&                     first_port = m_out_ports[params[0].handle.GetId()];
	std::array<bool, OUT_PORTS_MAX> paced {};
	bool                            any_device = false;
	uint64_t                        deadline   = 0;

	for (uint32_t i = 0; i < num; i++) {
		auto& port = m_out_ports[params[i].handle.GetId()];
		const float gain =
		    port.type == AUDIO_OUT_PORT_TYPE_PADSPK
		        ? Controller::GetSettingScale(Controller::Setting::SpeakerVolume)
		    : port.type == AUDIO_OUT_PORT_TYPE_VIBRATION
		        ? Controller::GetSettingScale(Controller::Setting::VibrationIntensity)
		        : 1.0f;

		uint64_t controller_queued_us = 0;
		bool controller_uses_bluetooth = false;
		if (!Loader::IsRuntimeChild()) {
			// Keep the stream alive against close and volume changes.
			Common::LockGuard lock(m_mutex);
			if (g_runtime_audio_focus.load() && !port.haptics && (port.type == AUDIO_OUT_PORT_TYPE_VIBRATION || port.type == AUDIO_OUT_PORT_TYPE_PADSPK)) {
				port.haptics = Controller::DualSenseHaptics::Open(port.freq, port.type == AUDIO_OUT_PORT_TYPE_PADSPK);
			}
			if (g_runtime_audio_focus.load() && port.haptics) {
			controller_queued_us = Controller::DualSenseHaptics::Queue(
			    port.haptics, Controller::GetActiveControllerId(), params[i].data, port.samples_num,
			    static_cast<uint32_t>(port.channels_num), FormatIsFloat(port.format), port.volume,
			    gain, params[i].gains);
			controller_uses_bluetooth =
			    Controller::DualSenseHaptics::UsesBluetooth(port.haptics);
			}
		}
		if (controller_queued_us == 0) {
			// No DualSense took it (e.g. it was unplugged); a pad speaker port plays on the main
			// output instead, and a vibration port has none.
			if (QueueSdlAudio(&port, params[i].data, blocking, gain, params[i].gains)) {
				any_device = true;
				paced[i]   = port.queue_primed;
			}
		} else {
			port.queue_primed = false;
			// Bluetooth HID has its own sender. Let the sample clock pace a batch
			// without another audio device, instead of waiting on the HID queue.
			if (port.type == AUDIO_OUT_PORT_TYPE_PADSPK && !controller_uses_bluetooth) {
				any_device = true;
				if (blocking && controller_queued_us > AUDIO_OUT_TARGET_LATENCY_US) {
					// Limit a stalled USB endpoint to one audio block of waiting.
					const uint64_t block_us = 1000000ULL * port.samples_num / port.freq;
					deadline = std::max(deadline, LibKernel::KernelGetProcessTime() +
					                                  std::min(controller_queued_us - AUDIO_OUT_TARGET_LATENCY_US,
					                                           block_us));
				}
			}
		}
	}

	// Submit the whole batch before pacing it. Audible devices own its cadence while
	// priming; silent/vibration-only batches use the same sample clock as SDL output.
	const auto now = LibKernel::KernelGetProcessTime();
	for (uint32_t i = 0; i < num; i++) {
		auto& port = m_out_ports[params[i].handle.GetId()];
		if (blocking && (paced[i] || !any_device)) {
			deadline = std::max(deadline, port.clock.Advance(now, port.samples_num, port.freq));
		} else {
			port.clock = {};
		}
		if (!blocking) {
			port.queue_primed = false;
		}
	}
	if (deadline > now) {
		Common::Thread::SleepMicro(deadline - now);
		for (uint32_t i = 0; AudioTraceEnabled() && i < num; i++) {
			auto& port = m_out_ports[params[i].handle.GetId()];
			if (port.trace_id != 0 && port.trace_clock_reports < 4) {
				if (const auto seq = g_audio_trace.TakeReport(); seq != 0) {
					const auto after = LibKernel::KernelGetProcessTime();
					LOGF("AudioOut trace: seq=%u event=pace t_us=%llu port=%llu handle=%d "
					     "requested_us=%llu elapsed_us=%llu batch_ports=%u\n", seq,
					     static_cast<unsigned long long>(after),
					     static_cast<unsigned long long>(port.trace_id), port.trace_handle,
					     static_cast<unsigned long long>(deadline - now),
					     static_cast<unsigned long long>(after - now), num);
				}
				++port.trace_clock_reports;
				break; // A batch sleeps once, not once per port.
			}
		}
	}

	return first_port.samples_num;
}

static bool RecordingDevicePresent(SDL_AudioDeviceID device) {
	int                count   = 0;
	SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
	bool               present = false;
	for (int i = 0; i < count; i++) {
		if (devices[i] == device) {
			present = true;
			break;
		}
	}
	SDL_free(devices);
	return present;
}

void Audio::OpenSdlDevice(PortIn* port, Format format) {
	const auto& name = Config::GetAudioInputDevice();
	if (name.empty()) {
		return;
	}
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		LOGF("AudioIn: SDL init failed: %s\n", SDL_GetError());
		return;
	}
	int                device_count = 0;
	SDL_AudioDeviceID* devices      = SDL_GetAudioRecordingDevices(&device_count);
	SDL_AudioDeviceID  device       = 0;
	for (int i = 0; i < device_count; i++) {
		const char* device_name = SDL_GetAudioDeviceName(devices[i]);
		if (device_name != nullptr && name == device_name) {
			device = devices[i];
			break;
		}
	}
	SDL_free(devices);
	if (device == 0) {
		LOGF("AudioIn: cannot find '%s'; using silence\n", name.c_str());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return;
	}
	SDL_AudioSpec desired {};
	desired.freq     = static_cast<int>(port->freq);
	desired.format   = SdlFormat(format);
	desired.channels = static_cast<int>(port->bytes_per_frame / BytesPerSample(format));
	port->stream     = SDL_OpenAudioDeviceStream(device, &desired, nullptr, nullptr);
	if (port->stream == nullptr) {
		LOGF("AudioIn: cannot open '%s': %s; using silence\n", name.c_str(), SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return;
	}
	port->device = device;
	LOGF("AudioIn: opened '%s' (%u Hz, %d ch)\n", name.c_str(), port->freq, desired.channels);
}

void Audio::CloseSdlDevice(PortIn* port) {
	if (port->stream != nullptr) {
		SDL_DestroyAudioStream(port->stream);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		port->stream = nullptr;
		port->device = 0;
	}
}

Audio::Id Audio::AudioInOpen(uint32_t samples_num, uint32_t freq, Format format, bool asynchronous) {
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < IN_PORTS_MAX; id++) {
		if (!m_in_ports[id].used) {
			auto& port = m_in_ports[id];

			port                 = {};
			port.used            = true;
			port.asynchronous    = asynchronous;
			port.samples_num     = samples_num;
			port.freq            = freq;
			port.bytes_per_frame = BytesPerSample(format);
			if (format == Format::Signed16bitStereo || format == Format::FloatStereo) {
				port.bytes_per_frame *= 2;
			}
			OpenSdlDevice(&port, format);

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

Audio::PortIn* Audio::GetAudioInPort(Id handle) {
	if (handle.GetId() < 0 || handle.GetId() >= IN_PORTS_MAX ||
	    !m_in_ports[handle.GetId()].used) {
		return nullptr;
	}
	return &m_in_ports[handle.GetId()];
}

int Audio::AudioInClose(Id handle) {
	Common::LockGuard lock(m_mutex);
	auto* port = GetAudioInPort(handle);
	if (port == nullptr) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}
	if (port->busy) {
		return AUDIO_IN_ERROR_BUSY;
	}
	CloseSdlDevice(port);
	*port = {};
	return OK;
}

int Audio::AudioInGetSilentState(Id handle) {
	Common::LockGuard lock(m_mutex);
	auto*             port = GetAudioInPort(handle);
	if (port == nullptr) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}
	if (port->stream == nullptr || !RecordingDevicePresent(port->device)) {
		return AUDIO_IN_SILENT_STATE_DEVICE_NONE;
	}
	return 0;
}

int Audio::AudioInInput(Id handle, void* dest) {
	PortIn snapshot;
	{
		Common::LockGuard lock(m_mutex);
		auto*             port = GetAudioInPort(handle);
		if (port == nullptr) {
			return AUDIO_IN_ERROR_INVALID_HANDLE;
		}
		if (port->busy) {
			return AUDIO_IN_ERROR_BUSY;
		}
		port->busy = true;
		snapshot   = *port;
	}
	bool failed = snapshot.stream != nullptr && dest != nullptr &&
	              (!RecordingDevicePresent(snapshot.device) ||
	               !SDL_ResumeAudioStreamDevice(snapshot.stream));

	const uint64_t block_time = (1000000ULL * snapshot.samples_num) / snapshot.freq;
	uint64_t       wait_time  = 0;
	if (snapshot.asynchronous) {
		if (dest != nullptr) {
			wait_time = block_time;
		}
	} else if (snapshot.last_input_time != 0 && (snapshot.stream == nullptr || dest == nullptr)) {
		const auto now  = LibKernel::KernelGetProcessTime();
		const auto next = snapshot.last_input_time + block_time;
		if (next > now) {
			wait_time = next - now;
		}
	}
	Common::Thread::SleepMicro(wait_time);
	uint32_t frames = snapshot.samples_num;
	bool     timed_out = false;
	if (dest != nullptr) {
		if (snapshot.stream != nullptr && !failed) {
			const int requested = static_cast<int>(frames * snapshot.bytes_per_frame);
			const auto deadline = LibKernel::KernelGetProcessTime() +
			                      std::max<uint64_t>(20000, block_time * 4);
			const auto device = SDL_GetAudioStreamDevice(snapshot.stream);
			int        available = SDL_GetAudioStreamAvailable(snapshot.stream);
			while (!snapshot.asynchronous && available >= 0 &&
			       available < requested &&
			       device != 0 && !SDL_AudioDevicePaused(device) &&
			       LibKernel::KernelGetProcessTime() < deadline) {
				Common::Thread::SleepMicro(1000);
				available = SDL_GetAudioStreamAvailable(snapshot.stream);
			}
			failed = available < 0 || device == 0 || SDL_AudioDevicePaused(device);
			timed_out = !snapshot.asynchronous && available < requested;
			if (!failed && !timed_out) {
				if (snapshot.asynchronous) {
					frames = AUDIO_IN_GRAIN_MAX_ASYNC;
				}
				const int bytes = SDL_GetAudioStreamData(
				    snapshot.stream, dest, static_cast<int>(frames * snapshot.bytes_per_frame));
				failed = bytes < 0;
				if (!failed) {
					frames = static_cast<uint32_t>(bytes) / snapshot.bytes_per_frame;
				}
			}
		}
		if (snapshot.stream == nullptr || failed || timed_out) {
			std::memset(dest, 0, frames * snapshot.bytes_per_frame);
		}
	} else if (snapshot.stream != nullptr) {
		SDL_PauseAudioStreamDevice(snapshot.stream);
		SDL_ClearAudioStream(snapshot.stream);
	}
	{
		Common::LockGuard lock(m_mutex);
		auto&             port = m_in_ports[handle.GetId()];
		if (failed) {
			LOGF("AudioIn: capture stopped; using silence\n");
			CloseSdlDevice(&port);
		}
		port.last_input_time = dest != nullptr ? LibKernel::KernelGetProcessTime() : 0;
		port.busy            = false;
	}
	return dest != nullptr ? static_cast<int>(frames) : 0;
}

namespace AudioOut {

LIB_NAME("AudioOut", "AudioOut");

struct AudioOutOutputParam {
	int         handle;
	const void* ptr;
};

struct AudioOutPortState {
	uint16_t output;
	uint8_t  channel;
	uint8_t  reserved1[1];
	int16_t  volume;
	uint16_t reroute_counter;
	uint64_t flag;
	uint64_t reserved2[2];
};

struct AudioOutSystemState {
	float    loudness;
	uint8_t  reserved8[4];
	uint64_t reserved64[3];
};

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutInit() {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                               uint32_t param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t len     = %u\n"
	     "\t freq    = %u\n",
	     user_id, type, index, len, freq);

	if (!audio_out_port_type_is_valid(type)) {
		return AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
	}
	if (freq == 0) {
		return AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
	}
	EXIT_NOT_IMPLEMENTED(index != 0);

	Audio::Format format       = Audio::Format::Unknown;
	const auto    format_param = param & AUDIO_OUT_PARAM_FORMAT_MASK;

	switch (format_param) {
		case 0: format = Audio::Format::Signed16bitMono; break;
		case 1: format = Audio::Format::Signed16bitStereo; break;
		case 2: format = Audio::Format::Signed16bit8Ch; break;
		case 3: format = Audio::Format::FloatMono; break;
		case 4: format = Audio::Format::FloatStereo; break;
		case 5: format = Audio::Format::Float8Ch; break;
		case 6: format = Audio::Format::Signed16bit8ChStd; break;
		case 7: format = Audio::Format::Float8ChStd; break;
		default:;
	}

	LOGF("\t param   = %u (format=%u, %s)\n", param, format_param, magic_enum::enum_name(format));

	EXIT_NOT_IMPLEMENTED(format == Audio::Format::Unknown);

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioOutOpen(type, len, freq, format);

	if (!id.IsValid()) {
		return AUDIO_OUT_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutClose(int handle) {
	PRINT_NAME();

	if (!g_audio->AudioOutClose(Audio::Id(handle))) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state) {
	PRINT_NAME();

	int type         = 0;
	int channels_num = 0;

	if (!g_audio->AudioOutGetStatus(Audio::Id(handle), &type, &channels_num)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	state->reroute_counter = 0;
	state->volume          = 127;

	switch (type) {
		case AUDIO_OUT_PORT_TYPE_MAIN:
		case AUDIO_OUT_PORT_TYPE_BGM:
		case AUDIO_OUT_PORT_TYPE_AUDIO3D:
			state->output  = 1;
			state->channel = (channels_num > 2 ? 2 : channels_num);
			break;
		case AUDIO_OUT_PORT_TYPE_VOICE:
		case AUDIO_OUT_PORT_TYPE_PERSONAL:
			state->output  = 0x40;
			state->channel = 1;
			break;
		case AUDIO_OUT_PORT_TYPE_PADSPK:
		case AUDIO_OUT_PORT_TYPE_VIBRATION:
			state->output  = 4;
			state->channel = 1;
			break;
		case AUDIO_OUT_PORT_TYPE_AUX:
			state->output  = 0x80;
			state->channel = 0;
			break;
		default: EXIT("unknown port type: %d\n", type);
	}

	LOGF("\t output  = %" PRIu16 "\n"
	     "\t channel = %" PRIu8 "\n",
	     state->output, state->channel);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutGetSystemState(AudioOutSystemState* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return AUDIO_OUT_ERROR_INVALID_POINTER;
	}

	// Loudness is undefined until measurement starts; no measurement is emulated.
	*state = {};

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol) {
	PRINT_NAME();

	LOGF("\t handle = %d\n"
	     "\t flag   = %u\n",
	     handle, flag);

	EXIT_IF(g_audio == nullptr);
	EXIT_NOT_IMPLEMENTED(vol == nullptr);

	if (!g_audio->AudioOutSetVolume(Audio::Id(handle), flag, vol)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param == nullptr);
	if (num == 0 || num > Audio::OUT_PORTS_MAX) {
		return AUDIO_OUT_ERROR_INVALID_SIZE;
	}

	Audio::OutputParam params[Audio::OUT_PORTS_MAX];

	EXIT_IF(g_audio == nullptr);

	for (uint32_t i = 0; i < num; i++) {
		params[i].handle = Audio::Id(param[i].handle);
		params[i].data   = param[i].ptr;

		if (!g_audio->AudioOutValid(params[i].handle)) {
			return AUDIO_OUT_ERROR_INVALID_PORT;
		}
	}

	return static_cast<int>(g_audio->AudioOutOutputs(params, num));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr) {
	if (ptr == nullptr) {
		static uint8_t silence[65536] = {};
		ptr                         = silence;
	}

	Audio::OutputParam params[1];

	EXIT_IF(g_audio == nullptr);

	params[0].handle = Audio::Id(handle);
	params[0].data   = ptr;

	if (!g_audio->AudioOutValid(params[0].handle)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return static_cast<int>(g_audio->AudioOutOutputs(params, 1));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutOpenPsm(int user_id, int type, int index, uint32_t unk, uint32_t lenfmt,
                                  uint32_t freq) {
	PRINT_NAME();
	LOGF("\t psm: unk=%u lenfmt=%u freq=%u\n", unk, lenfmt, freq);
	return AudioOutOpen(user_id, type, index, lenfmt == 0 ? 256 : lenfmt, freq == 0 ? 48000 : freq,
	                    lenfmt & AUDIO_OUT_PARAM_FORMAT_MASK);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutOpenEx(int user_id, int type, uint64_t index, uint32_t device_id,
                                 uint32_t len, uint32_t freq, uint32_t param) {
	PRINT_NAME();
	LOGF("\t device_id = %u, index = %llu\n", device_id,
	     static_cast<unsigned long long>(index));
	if (index != 0) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}
	if (device_id != 0) {
		return AUDIO_OUT_ERROR_INVALID_ARG;
	}
	return AudioOutOpen(user_id, type, 0, len, freq, param);
}

} // namespace AudioOut

namespace AudioIn {

LIB_NAME("AudioIn", "AudioIn");

static int OpenPort(int user_id, int type, int index, uint32_t len, uint32_t freq,
                    uint32_t param, bool asynchronous) {
	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t len     = %u\n"
	     "\t freq    = %u\n",
	     user_id, type, index, len, freq);

	if (type != 0 && type != 1) {
		return AUDIO_IN_ERROR_INVALID_TYPE;
	}
	if (index != 0) {
		return AUDIO_IN_ERROR_INVALID_PARAM;
	}
	if (len != 128 && (asynchronous || len != 256)) {
		return AUDIO_IN_ERROR_INVALID_SIZE;
	}
	if (freq != 48000 && (asynchronous || freq != 16000)) {
		return AUDIO_IN_ERROR_INVALID_FREQ;
	}

	Audio::Format format = Audio::Format::Unknown;

	switch (param) {
		case 1: format = Audio::Format::Signed16bitMono; break;
		case 2: format = Audio::Format::Signed16bitStereo; break;
		case 0x10:
		case 0x11: format = Audio::Format::FloatMono; break;
		case 0x12: format = Audio::Format::FloatStereo; break;
		default: return AUDIO_IN_ERROR_INVALID_PARAM;
	}

	LOGF("\t param   = %u (%s)\n", param, magic_enum::enum_name(format));

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioInOpen(len, freq, format, asynchronous);

	if (!id.IsValid()) {
		return AUDIO_IN_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioInOpen(int user_id, int type, int index, uint32_t len,
                              uint32_t freq, uint32_t param) {
	PRINT_NAME();
	return OpenPort(user_id, type, index, len, freq, param, false);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioInHqOpen(int user_id, int type, int index, uint32_t len,
                                uint32_t freq, uint32_t param) {
	PRINT_NAME();
	return OpenPort(user_id, type, index, len, freq, param, true);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioInClose(int handle) {
	PRINT_NAME();
	EXIT_IF(g_audio == nullptr);
	if (handle <= 0) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}
	return g_audio->AudioInClose(Audio::Id(handle));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioInInput(int handle, void* dest) {
	PRINT_NAME();
	EXIT_IF(g_audio == nullptr);
	if (handle <= 0) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return g_audio->AudioInInput(Audio::Id(handle), dest);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioInGetSilentState(int handle) {
	PRINT_NAME();

	EXIT_IF(g_audio == nullptr);

	if (handle <= 0) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return g_audio->AudioInGetSilentState(Audio::Id(handle));
}

} // namespace AudioIn

namespace VoiceQoS {

LIB_NAME("VoiceQoS", "VoiceQoS");

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type) {
	PRINT_NAME();

	LOGF("\t mem_block = %016" PRIx64 "\n"
	     "\t mem_size = %" PRIu32 "\n"
	     "\t app_type = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(mem_block), mem_size, app_type);

	return OK;
}

} // namespace VoiceQoS

} // namespace Libs::Audio
