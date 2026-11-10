#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "libs/libs.h"
#include "libs/mediaFramework.h"
#include "libs/mediaFrameworkEsvm.h"
#include "libs/mediaFrameworkEsvmKeys.h"
#include "libs/mediaFrameworkEsvmSource.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace Libs::MediaFramework {
LIB_VERSION("MediaFrameworkInterface", 1, "MediaFrameworkInterface", 1, 1);

namespace {
// MediaFrameworkInterface exports use their own error namespace, not PSM's.
constexpr int Invalid = static_cast<int>(0x81790001u);
constexpr int Failed  = static_cast<int>(0x81790fffu);
using Clock           = std::chrono::steady_clock;
using Callback        = KYTY_SYSV_ABI void (*)(int, const void*, void*);
struct Callbacks {
	Callback function;
	void*    user;
	uint64_t mask;
};
static_assert(sizeof(Callbacks) == 24);

// Bounded streaming decoder: one decoded frame, one packet and FFmpeg's reorder
// buffers. In particular, a 4K looping clip is not cached as gigabytes of RGBA.
struct Decoder {
	AVFormatContext* format   = nullptr;
	AVCodecContext*  codec    = nullptr;
	AVFrame*         frame    = nullptr;
	AVFrame*         pending  = nullptr;
	AVPacket*        packet   = nullptr;
	SwsContext*      scaler   = nullptr;
	int              stream   = -1;
	bool             draining = false;
	double           pts = -1, duration = 0, frame_duration = 1.0 / 30.0;
	~Decoder() {
		sws_freeContext(scaler);
		av_packet_free(&packet);
		av_frame_free(&frame);
		av_frame_free(&pending);
		avcodec_free_context(&codec);
		avformat_close_input(&format);
	}
	bool Open(const std::string& path) {
		const int opened = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
		if (opened < 0) {
			LOGF("MediaFramework: open failed=%d path=%s\n", opened, path.c_str());
			return false;
		}
		if (avformat_find_stream_info(format, nullptr) < 0) return false;
		stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (stream < 0) {
			LOGF("MediaFramework: no video decoder=%d\n", stream);
			return false;
		}
		auto*       parameters = format->streams[stream]->codecpar;
		const auto* decoder    = avcodec_find_decoder(parameters->codec_id);
		if (!decoder || !(codec = avcodec_alloc_context3(decoder))) {
			LOGF("MediaFramework: unsupported codec=%d\n", static_cast<int>(parameters->codec_id));
			return false;
		}
		if (avcodec_parameters_to_context(codec, parameters) < 0) return false;
		codec->thread_count    = 2;
		const int codec_opened = avcodec_open2(codec, decoder, nullptr);
		if (codec_opened < 0) {
			LOGF("MediaFramework: codec open failed=%d\n", codec_opened);
			return false;
		}
		frame   = av_frame_alloc();
		pending = av_frame_alloc();
		packet  = av_packet_alloc();
		if (!frame || !pending || !packet || codec->width <= 0 || codec->height <= 0 ||
		    codec->width > 8192 || codec->height > 8192)
			return false;
		auto* video = format->streams[stream];
		duration    = video->duration > 0
		                  ? video->duration * av_q2d(video->time_base)
		                  : (format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0);
		const double fps = av_q2d(av_guess_frame_rate(format, video, nullptr));
		if (fps > 0) frame_duration = 1 / fps;
		return Next();
	}
	bool Next() {
		for (;;) {
			const int ret = avcodec_receive_frame(codec, pending);
			if (ret == 0) {
				std::swap(frame, pending);
				auto*      video = format->streams[stream];
				const auto stamp = frame->best_effort_timestamp;
				const auto start = video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time;
				pts = stamp == AV_NOPTS_VALUE ? std::max(0.0, pts + frame_duration)
				                              : (stamp - start) * av_q2d(video->time_base);
				return true;
			}
			if (ret != AVERROR(EAGAIN) || draining) {
				if (ret != AVERROR_EOF) LOGF("MediaFramework: receive failed=%d\n", ret);
				return false;
			}
			do {
				av_packet_unref(packet);
				const int read = av_read_frame(format, packet);
				if (read < 0) {
					if (read != AVERROR_EOF) return false;
					draining = true;
					return avcodec_send_packet(codec, nullptr) >= 0 && Next();
				}
			} while (packet->stream_index != stream);
			const int sent = avcodec_send_packet(codec, packet);
			if (sent < 0) {
				LOGF("MediaFramework: send failed=%d\n", sent);
				return false;
			}
		}
	}
	bool At(double seconds) {
		if (seconds + 1e-6 < pts) {
			auto*         video = format->streams[stream];
			const int64_t start = video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time;
			const auto    stamp = start + static_cast<int64_t>(seconds / av_q2d(video->time_base));
			if (av_seek_frame(format, stream, stamp, AVSEEK_FLAG_BACKWARD) < 0) return false;
			avcodec_flush_buffers(codec);
			draining = false;
			pts      = -1;
		}
		// Keep the last valid frame at EOF, including delayed HEVC pictures.
		while (pts + frame_duration <= seconds) {
			if (!Next()) break;
		}
		return pts >= 0;
	}
	bool Copy(uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
		if (!frame || !frame->data[0] || !w || !h || w > 8192 || h > 8192) return false;
		scaler = sws_getCachedContext(scaler, codec->width, codec->height,
		                              static_cast<AVPixelFormat>(frame->format), w, h,
		                              AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!scaler) return false;
		rgba.resize(size_t(w) * h * 4);
		uint8_t* planes[]  = {rgba.data(), nullptr, nullptr, nullptr};
		int      strides[] = {int(w * 4), 0, 0, 0};
		return sws_scale(scaler, frame->data, frame->linesize, 0, codec->height, planes, strides) ==
		       int(h);
	}
};

enum class Command { Load, Play, Pause, Stop, Seek };
struct Source;
thread_local Source* callback_source = nullptr;
struct Source {
	std::mutex               mutex;
	std::condition_variable  wake;
	std::deque<Command>      commands;
	std::unique_ptr<Decoder> decoder;
	Callbacks                callbacks {};
	LibKernel::Pthread       worker = nullptr;
	std::string              path;
	int                      id = 0, meta = 0, ready = 0, network = 0;
	int                      error      = 0;
	int                      display[2] = {};
	bool   quit = false, playing = false, loop = false, muted = false, bgm = false;
	double position = 0, duration = 0, rate = 1, volume = 1, loop_start = 0, loop_end = 0;
	Clock::time_point anchor = Clock::now();
	double            Time() const {
		double time =
		    position +
		    (playing ? std::chrono::duration<double>(Clock::now() - anchor).count() * rate : 0);
		const double end = loop_end > loop_start ? std::min(loop_end, duration) : duration;
		if (loop && end > loop_start && time >= end)
			time = loop_start + std::fmod(time - loop_start, end - loop_start);
		return std::clamp(time, 0.0, duration);
	}
	void Emit(int event) {
		// No source or registry locks across guest callbacks (callbacks query us).
		if (callbacks.function) callbacks.function(event, nullptr, callbacks.user);
	}
	void Run() {
		for (;;) {
			std::vector<int> events;
			{
				std::unique_lock lock(mutex);
				wake.wait_for(lock, std::chrono::milliseconds(33),
				              [&] { return quit || !commands.empty(); });
				if (quit) return;
				if (!commands.empty()) {
					const auto command = commands.front();
					commands.pop_front();
					if (TraceEnabled()) Log::WriteToConsoleAndLog(fmt::sprintf(
					    "MediaFramework: source=%d command=%d path=%s\n", id, static_cast<int>(command), path));
					if (command == Command::Load && !decoder) {
						auto next = std::make_unique<Decoder>();
						if (next->Open(path)) {
							duration = next->duration;
							ready    = 4;
							network  = 1;
							if (!display[0] || !display[1]) {
								display[0] = next->codec->width;
								display[1] = next->codec->height;
							}
							decoder = std::move(next);
							// PsmUtil maps 5/6/7/8 to metadata/data/canplay/canplaythrough.
							// Event 9 is Waiting and must not follow a successful load.
							events = {4, 0x1007, 0x1003, 5, 0x15, 0x1b, 6, 7, 8};
							LOGF("MediaFramework: loaded source=%d size=%dx%d duration=%.6f "
							     "path=%s\n",
							     id, decoder->codec->width, decoder->codec->height, duration,
							     path.c_str());
						} else {
							network = 3;
							error   = 4; // SrcNotSupported (no decodable local source).
							events  = {4, 0x13};
							LOGF("MediaFramework: load failed source=%d path=%s\n", id,
							     path.c_str());
						}
					} else if (command == Command::Play && decoder) {
						position = Time();
						anchor   = Clock::now();
						playing  = true;
						events   = {0x10};
					} else if (command == Command::Pause) {
						position = Time();
						playing  = false;
						events   = {0x11};
					} else if (command == Command::Stop) {
						playing  = false;
						position = 0;
						events   = {0x12};
					} else if (command == Command::Seek) {
						events = {0xb, 0xc};
					}
				}
				if (playing && decoder) {
					const double now = Time();
					if (!loop && now >= duration) {
						position = duration;
						playing  = false;
						events.push_back(0xd);
					} else
						events.push_back(0xf); // TimeUpdate; 0xc is Seeked.
				}
			}
			for (int event: events) {
				{
					std::lock_guard lock(mutex);
					if (quit) break;
				}
				Emit(event);
			}
		}
	}
	static KYTY_SYSV_ABI void* Entry(void* opaque) {
		std::unique_ptr<std::shared_ptr<Source>> owner(
		    static_cast<std::shared_ptr<Source>*>(opaque));
		auto source = *owner;
		owner.reset();
		callback_source = source.get();
		source->Run();
		callback_source = nullptr;
		return nullptr;
	}
};
std::mutex                             registry_mutex;
std::map<int, std::shared_ptr<Source>> sources;
int                                    next_id = 1;
std::shared_ptr<Source>                Find(int id) {
	std::lock_guard lock(registry_mutex);
	auto            it = sources.find(id);
	return it == sources.end() ? nullptr : it->second;
}
#define MF_ABI __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
#define MF_NATIVE_CONTROL(id, op, value) \
	if (IsEsvmServiceHandle(id)) { return EsvmControlSource(id, EsvmSourceControl::op, value, reinterpret_cast<uintptr_t>(__builtin_return_address(0))); }
#define MF_NATIVE_UNSUPPORTED(id) MF_NATIVE_CONTROL(id, Unsupported, 0)
#else
// This pre-existing standalone target includes this TU and tests only its
// local-file backend, with fake pthread/filesystem plumbing. No native service
// hooks in that isolated target; production/full-emulator fixtures use them.
#define MF_NATIVE_CONTROL(id, op, value)
#define MF_NATIVE_UNSUPPORTED(id)
#endif
int MF_ABI Initialize() {
	return 0;
}
int MF_ABI Add(const char* config, const Callbacks* callbacks, int* result) {
	if (!config || !result) return Invalid;
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
	if (!EsvmWritableGuest(result, sizeof(*result))) return Invalid;
#endif
	*result             = 0;
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
	char copied_uri[0x1000] {};
	size_t length = 0;
	if (!ReadMediaFrameworkUri(config, copied_uri, &length)) return Invalid;
#else
	const size_t length = strnlen(config, 0x1000);
#endif
	if (TraceEnabled()) {
		static std::atomic_uint calls {0};
		if (calls.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("MediaFramework: source-add leading_string_length=%zu callbacks=%p\n", length,
			     static_cast<const void*>(callbacks));
		}
	}
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
	int native_result = Invalid;
	if (TryAddEsvmSource(config, callbacks, result, &native_result)) return native_result;
#endif
	if (!length || length == 0x1000) return Invalid;
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
	std::string uri(copied_uri, length);
#else
	std::string uri(config, length);
#endif
	if (uri.starts_with("file://")) uri.erase(0, 7);
	if (!uri.starts_with('/') || uri.find("://") != std::string::npos) return Failed;
	auto source  = std::make_shared<Source>();
	source->path = LibKernel::FileSystem::GetRealFilename(uri).string();
	if (source->path.empty()) return Failed;
	// Reject an unavailable local source before publishing a handle or starting
	// callbacks. Decode/format errors for existing files remain asynchronous.
	std::error_code path_error;
	if (!std::filesystem::is_regular_file(source->path, path_error)) return Failed;
	if (callbacks) {
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
		if (!EsvmReadGuest(&source->callbacks, callbacks, sizeof(source->callbacks))) return Invalid;
#else
		source->callbacks = *callbacks;
#endif
	}
	{
		std::lock_guard lock(registry_mutex);
		if (next_id >= 0x80) return Failed;
		source->id = next_id++;
		// Composite +0x3700 identifies virtual canvases by byte1 == 0xff.
		// Byte0 must differ from the 0xff unavailable sentinel. The high byte
		// is our non-reused source generation; no kernel handle translation.
		source->meta = (source->id << 24) | 0xff7e;
		sources.emplace(source->id, source);
	}
	auto* owner = new std::shared_ptr<Source>(source);
	if (LibKernel::PthreadCreate(&source->worker, nullptr, Source::Entry, owner,
	                             "MediaFramework") != 0) {
		delete owner;
		std::lock_guard lock(registry_mutex);
		sources.erase(source->id);
		return Failed;
	}
	*result = source->id;
	LOGF("MediaFramework: added source=%d uri=%s\n", *result, uri.c_str());
	if (TraceEnabled()) Log::WriteToConsoleAndLog(fmt::sprintf("MediaFramework: source=%d uri=%s\n", *result, uri));
	return 0;
}
int MF_ABI Delete(int id) {
#ifndef KYTY_MEDIA_FRAMEWORK_TEST
	if (IsEsvmServiceHandle(id)) return EsvmDeleteSource(id, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
#endif
	std::shared_ptr<Source> source;
	{
		std::lock_guard lock(registry_mutex);
		auto            it = sources.find(id);
		if (it == sources.end()) return Invalid;
		source = it->second;
		sources.erase(it);
	}
	{
		std::lock_guard lock(source->mutex);
		source->quit = true;
	}
	source->wake.notify_all();
	if (callback_source == source.get())
		LibKernel::PthreadDetach(source->worker);
	else
		LibKernel::PthreadJoin(source->worker, nullptr);
	return 0;
}
int Queue(int id, Command command) {
	auto s = Find(id);
	if (!s) return Invalid;
	{
		std::lock_guard lock(s->mutex);
		if (s->quit) return Invalid;
		s->commands.push_back(command);
	}
	s->wake.notify_one();
	return 0;
}
int MF_ABI Load(int id) {
	MF_NATIVE_UNSUPPORTED(id);
	return Queue(id, Command::Load);
}
int MF_ABI Play(int id) {
	MF_NATIVE_CONTROL(id, Play, 0);
	return Queue(id, Command::Play);
}
int MF_ABI Pause(int id) {
	MF_NATIVE_CONTROL(id, Pause, 0);
	return Queue(id, Command::Pause);
}
int MF_ABI Stop(int id) {
	MF_NATIVE_UNSUPPORTED(id);
	return Queue(id, Command::Stop);
}
#define MF_GET(name, type, expression)                                                             \
	int MF_ABI name(int id, type* out) {                                                           \
		MF_NATIVE_UNSUPPORTED(id);                                                                 \
		auto s = Find(id);                                                                         \
		if (!s || !out) return Invalid;                                                            \
		std::lock_guard lock(s->mutex);                                                            \
		*out = (expression);                                                                       \
		return 0;                                                                                  \
	}
MF_GET(Network, int, s->network)
MF_GET(Ready, int, s->ready)
MF_GET(Duration, double, s->duration)
MF_GET(CurrentTime, double, s->Time())
MF_GET(Rate, double, s->rate)
MF_GET(Volume, double, s->volume)
MF_GET(Status, int, 0)
MF_GET(ClosedCaptions, uint8_t, 0)
#define MF_BOOL(name, field)                                                                       \
	int MF_ABI name(int id, bool value) {                                                          \
		MF_NATIVE_CONTROL(id, Mute, value);                                                         \
		auto s = Find(id);                                                                         \
		if (!s) return Invalid;                                                                    \
		std::lock_guard lock(s->mutex);                                                            \
		s->field = value;                                                                          \
		return 0;                                                                                  \
	}
MF_BOOL(SetMute, muted)
int MF_ABI SetBgm(int id, bool value) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s) return Invalid;
	std::lock_guard lock(s->mutex);
	s->bgm = value;
	return 0;
}
int MF_ABI SetLoop(int id, bool value) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s) return Invalid;
	std::lock_guard lock(s->mutex);
	s->position = s->Time();
	s->anchor   = Clock::now();
	s->loop     = value;
	return 0;
}
// PsmUtil +0x16e1b passes three int32 outputs, including the backend detail.
int MF_ABI GetError(int id, int* error, int* detail, int* code) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !error || !detail || !code) return Invalid;
	std::lock_guard lock(s->mutex);
	*error  = s->error;
	*detail = 0;
	*code   = s->error ? Failed : 0;
	return 0;
}
int MF_ABI SetRate(int id, double value) {
	MF_NATIVE_CONTROL(id, Rate, value);
	auto s = Find(id);
	if (!s || !std::isfinite(value) || value <= 0 || value > 16) return Invalid;
	std::lock_guard lock(s->mutex);
	s->position = s->Time();
	s->anchor   = Clock::now();
	s->rate     = value;
	return 0;
}
int MF_ABI SetVolume(int id, double value) {
	MF_NATIVE_CONTROL(id, Volume, value);
	auto s = Find(id);
	if (!s || !std::isfinite(value) || value < 0 || value > 1) return Invalid;
	std::lock_guard lock(s->mutex);
	s->volume = value;
	return 0;
}
int MF_ABI Seek(int id, double value) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !std::isfinite(value) || value < 0) return Invalid;
	{
		std::lock_guard lock(s->mutex);
		s->position = std::min(value, s->duration);
		s->anchor   = Clock::now();
	}
	return Queue(id, Command::Seek);
}
int MF_ABI LoopRange(int id, double start, double end) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !std::isfinite(start) || !std::isfinite(end) || start < 0 || end < start)
		return Invalid;
	std::lock_guard lock(s->mutex);
	s->loop_start = start;
	s->loop_end   = end;
	return 0;
}
int MF_ABI SetDisplay(int id, const int* size) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !size || size[0] < 0 || size[1] < 0) return Invalid;
	std::lock_guard lock(s->mutex);
	std::copy_n(size, 2, s->display);
	return 0;
}
int MF_ABI Display(int id, int* size) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !size) return Invalid;
	std::lock_guard lock(s->mutex);
	std::copy_n(s->display, 2, size);
	return 0;
}
int MF_ABI Resolution(int id, int* size) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !size) return Invalid;
	std::lock_guard lock(s->mutex);
	size[0] = s->decoder ? s->decoder->codec->width : 0;
	size[1] = s->decoder ? s->decoder->codec->height : 0;
	return 0;
}
int MF_ABI Canvas(int id, int* handle, int* type) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !handle || !type) return Invalid;
	std::lock_guard lock(s->mutex);
	*handle = s->decoder ? s->meta : -1;
	*type   = 0;
	return 0;
}
struct Ranges {
	double   pairs[512];
	uint64_t count;
};
static_assert(offsetof(Ranges, count) == 0x1000);
int MF_ABI Seekable(int id, Ranges* ranges) {
	MF_NATIVE_UNSUPPORTED(id);
	auto s = Find(id);
	if (!s || !ranges) return Invalid;
	std::lock_guard lock(s->mutex);
	*ranges = {};
	if (s->decoder) {
		ranges->count    = 1;
		ranges->pairs[1] = s->duration;
	}
	return 0;
}
} // namespace

bool TraceEnabled() {
	static const bool enabled = std::getenv("KYTY_MEDIA_TRACE") != nullptr;
	return enabled;
}

bool CopyCanvasFrame(int meta, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba) {
	auto source = Find(static_cast<uint32_t>(meta) >> 24);
	if (!source || source->meta != meta) return false;
	std::lock_guard lock(source->mutex);
	return !source->quit && source->decoder && source->decoder->At(source->Time()) &&
	       source->decoder->Copy(width, height, rgba);
}

bool OwnsCanvas(int meta) {
	const auto      value = static_cast<uint32_t>(meta);
	const auto      id    = value >> 24;
	std::lock_guard lock(registry_mutex);
	return (value & 0xffffffu) == 0xff7eu && id > 0 && id < static_cast<uint32_t>(next_id);
}

int CanvasFromTextureAddress(uint64_t address) {
	if ((address & 0xffffff00000000ffull) != 0xfe0000000000ull) return 0;
	const auto meta = static_cast<int>((address >> 8) & 0xffffffffu);
	return OwnsCanvas(meta) ? meta : 0;
}

bool IsCanvasLive(int meta) {
	auto source = Find(static_cast<uint32_t>(meta) >> 24);
	if (!source || source->meta != meta) return false;
	std::lock_guard lock(source->mutex);
	return !source->quit && source->decoder != nullptr;
}

bool BuildCanvasTexture(int meta, uint32_t (&descriptor)[8]) {
	if (!OwnsCanvas(meta)) return false;
	uint32_t width = 1, height = 1;
	if (auto source = Find(static_cast<uint32_t>(meta) >> 24)) {
		std::lock_guard lock(source->mutex);
		if (!source->quit && source->decoder) {
			width  = source->decoder->codec->width;
			height = source->decoder->codec->height;
		}
	}
	if (!width || !height || width > 8192 || height > 8192) return false;
	const uint64_t address = 0xfe0000000000ull | (static_cast<uint64_t>(meta) << 8);
	// Gen5 T#: RGBA8 UNorm, identity component mapping, linear 2D, one mip.
	const uint32_t words[8] = {static_cast<uint32_t>(address >> 8),
	                           static_cast<uint32_t>(address >> 40) | (56u << 20) |
	                               (((width - 1) & 3u) << 30),
	                           ((width - 1) >> 2) | ((height - 1) << 14),
	                           0x90000fac,
	                           0,
	                           0,
	                           0,
	                           0};
	std::memcpy(descriptor, words, sizeof(words));
	return true;
}

#ifndef KYTY_MEDIA_FRAMEWORK_TEST
LIB_DEFINE(InitMediaFramework_1) {
	InitMediaFrameworkKeys_1(s);
	InitMediaFrameworkSource_1(s);
	LIB_FUNC("S8+GY2JSXnA", EsvmInitialize);
	LIB_FUNC("ilSc3Cc3kek", EsvmRegisterClient);
	LIB_FUNC("CwgReBYHFH4", EsvmUnregisterClient);
	LIB_FUNC("qHa1HbTmGeE", EsvmTerminate);
	LIB_FUNC("che6EGiH2gQ", EsvmGetStatus);
	LIB_FUNC("Ea8XATlrcRc", Initialize);
	LIB_FUNC("O9kOx8wYA-Y", Add);
	LIB_FUNC("kgxp80kqgrA", Delete);
	LIB_FUNC("HByoi0O45n8", Load);
	LIB_FUNC("XMjmgYrWzls", Play);
	LIB_FUNC("NlSpa23Wdh0", Pause);
	LIB_FUNC("chW+1VikTkM", Stop);
	LIB_FUNC("B+vEpGkNmgo", ClosedCaptions);
	LIB_FUNC("PmRWwXOXGYQ", Network);
	LIB_FUNC("tJhgreoG9iI", Ready);
	LIB_FUNC("dTmqebmcj6E", Duration);
	LIB_FUNC("Tqhqd4MWWSs", CurrentTime);
	LIB_FUNC("47Msr7Zjey8", Rate);
	LIB_FUNC("BJc0K1UBaYo", Volume);
	LIB_FUNC("+STGw1QE-8Q", Status);
	LIB_FUNC("c-AJTsu0v00", SetRate);
	LIB_FUNC("EkpnPaqIGhc", Seek);
	LIB_FUNC("IJGSva-bM-Q", SetLoop);
	LIB_FUNC("TS2pCViUFos", SetMute);
	LIB_FUNC("4PQhrMOSD6U", SetVolume);
	LIB_FUNC("BF2nUshxrY0", SetBgm);
	LIB_FUNC("qQhsC-aZkEE", SetDisplay);
	LIB_FUNC("gIdj5jMWfQw", Display);
	LIB_FUNC("5lNTWtwAD5Y", LoopRange);
	LIB_FUNC("NoZv1zNq3Ds", Canvas);
	LIB_FUNC("2cAOxQQ2eMM", Resolution);
	LIB_FUNC("bOvuQKpRZSo", Seekable);
	LIB_FUNC("Odxx6UPXBqU", GetError);
}
#endif
} // namespace Libs::MediaFramework
