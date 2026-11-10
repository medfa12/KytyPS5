#include "common/abi.h"
#include "libs/errno.h"
#include "libs/guestHeap.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
}

namespace Libs {

// Forward declarations for the v1.0 / stub blocks defined later in this file.
// They are chained from the existing v1.1 INIT entry points below so that
// src/libs/libs.cpp does not need to change.
namespace VideoStreamingEngineV10 {
void InitVideoStreamingEngineV10_1(Loader::SymbolDatabase* s);
} // namespace VideoStreamingEngineV10
namespace SysmoduleStubs {
void InitSysmoduleStubs_1(Loader::SymbolDatabase* s);
} // namespace SysmoduleStubs
namespace LibcInternalStubs {
void InitLibcInternalStubs_1(Loader::SymbolDatabase* s);
} // namespace LibcInternalStubs
// Forward: VideoCoreIfMainLoop (VCI block below) pumps via the VSE10 player
// table defined later in this file. Declared here, defined at the end of VSE10.
namespace VideoStreamingEngineV10 {
namespace VSE10 {
int VideoMainLoopPump(uint64_t handle);
} // namespace VSE10
} // namespace VideoStreamingEngineV10
namespace UsbStorageStubs {
void InitUsbStorageStubs_1(Loader::SymbolDatabase* s);
} // namespace UsbStorageStubs

// ps5-vsh: NPXS40038 (SceVideoCore4K, VideoCore4K) needs these to get past
// its init gate. Real implementations come later (VSE + VideoCore + Hermes).
// All stubs report success so the applet proceeds instead of aborting.

namespace VideoCoreInterface {

LIB_VERSION("VideoCoreInterface", 1, "VideoCoreInterface", 1, 1);

namespace VCI {

	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoCoreIfInitializeInterface(uint64_t a1, uint64_t a2, uint64_t a3) {
		PRINT_NAME();
		LOGF("\t VideoCoreIfInitializeInterface a1=0x%016llx a2=0x%016llx a3=0x%016llx\n",
		     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
		     static_cast<unsigned long long>(a3));
		(void)a1;
		(void)a2;
		(void)a3;
		return 0;
	}

	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoCoreIfSetFunctionTableUnlimited(uint64_t a1, uint64_t a2) {
		PRINT_NAME();
		LOGF("\t VideoCoreIfSetFunctionTableUnlimited a1=0x%016llx a2=0x%016llx\n",
		     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2));
		// ps5-vsh: dump the table struct (guest stack): it is shared with
		// MainLoop's arg, so its layout is the engine contract.
		if (a1 != 0) {
			const uint64_t* q = reinterpret_cast<const uint64_t*>(a1);
			LOGF("\t table[0..15]: %016llx %016llx %016llx %016llx %016llx %016llx "
			     "%016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx "
			     "%016llx %016llx\n",
			     static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
			     static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
			     static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]),
			     static_cast<unsigned long long>(q[6]), static_cast<unsigned long long>(q[7]),
			     static_cast<unsigned long long>(q[8]), static_cast<unsigned long long>(q[9]),
			     static_cast<unsigned long long>(q[10]), static_cast<unsigned long long>(q[11]),
			     static_cast<unsigned long long>(q[12]), static_cast<unsigned long long>(q[13]),
			     static_cast<unsigned long long>(q[14]), static_cast<unsigned long long>(q[15]));
		}
		(void)a1;
		(void)a2;
		return 0;
	}

	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoCoreIfMainLoop(uint64_t a1) {
		PRINT_NAME();
		LOGF("\t VideoCoreIfMainLoop a1=0x%016llx\n", static_cast<unsigned long long>(a1));
	// Bounded REAL frame pump (defined on the VSE10 player table at the end of
	// this file): advances a Playing player by one frame quantum per iteration.
	// Returns 0 so the guest reaches its shutdown path + guest exit(0).
	return VideoStreamingEngineV10::VSE10::VideoMainLoopPump(a1);
}

} // namespace VCI

LIB_DEFINE(InitVideoCoreInterface_1) {
	LIB_FUNC("Te4fj9mHFME", VCI::VideoCoreIfInitializeInterface);
	LIB_FUNC("tXNLLCUCfOE", VCI::VideoCoreIfSetFunctionTableUnlimited);
	LIB_FUNC("a5Z4yyiPbK8", VCI::VideoCoreIfMainLoop);
}

} // namespace VideoCoreInterface

namespace VideoStreamingEngine {

LIB_VERSION("VideoStreamingEngine", 1, "VideoStreamingEngine", 1, 1);

namespace VSE {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoStreamingEngineInitialize(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoStreamingEngineTerminate() {
	PRINT_NAME();
	return 0;
}

// Unknowns observed in NPXS40038 imports (npxs40038_imports.txt). Stubbed to
// unblock the loader; rename once real signatures are known.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoStreamingEngineUnknown0(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoStreamingEngineUnknown1(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoStreamingEngineUnknown2(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace VSE

LIB_DEFINE(InitVideoStreamingEngine_1) {
	LIB_FUNC("kruTFfx-BDM", VSE::VideoStreamingEngineInitialize);
	LIB_FUNC("bbKGMzUU5fA", VSE::VideoStreamingEngineTerminate);
	LIB_FUNC("KAkO+-Xbfws", VSE::VideoStreamingEngineUnknown0);
	LIB_FUNC("5Fg-EMN0TMw", VSE::VideoStreamingEngineUnknown1);
	LIB_FUNC("Svch6mxCneE", VSE::VideoStreamingEngineUnknown2);
	// v1.0 player NIDs live in their own LIB_VERSION block below.
	VideoStreamingEngineV10::InitVideoStreamingEngineV10_1(s);
}

} // namespace VideoStreamingEngine

namespace AgcVshStubs {

LIB_VERSION("AgcVsh", 1, "AgcVsh", 1, 1);

namespace AgcVsh {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcVshUnknown0(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcVshUnknown1(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AgcVshSetNop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace AgcVsh

LIB_DEFINE(InitAgcVshStubs_1) {
	LIB_FUNC("mzvvOZlxNag", AgcVsh::AgcVshUnknown0);
	LIB_FUNC("k0E7vkgqAuE", AgcVsh::AgcVshUnknown1);
	LIB_FUNC("K2mciNVxUCE", AgcVsh::AgcVshSetNop);
	// Sysmodule / LibcInternal / USB-storage stubs live in their own LIB_VERSION blocks below.
	SysmoduleStubs::InitSysmoduleStubs_1(s);
	LibcInternalStubs::InitLibcInternalStubs_1(s);
	UsbStorageStubs::InitUsbStorageStubs_1(s);
}

} // namespace AgcVshStubs

namespace VideoStreamingEngineV10 {

LIB_VERSION("VideoStreamingEngine", 1, "VideoStreamingEngine", 1, 0);

namespace VSE10 {

// ps5-vsh: VSE v1.0 player NIDs imported by NPXS40038. REAL stateful player:
// handle table + state machine + sample queue backed by host memory, fed into
// REAL FFmpeg demux/decode on host (in-memory AVIO over the queued bytes,
// host-backed pattern: libVideoDec2.cpp + videoDec2Decoder.h, player
// lifecycle: avPlayer.cpp). Guest memory is identity-mapped, so recorded
// sample ptrs are directly host-usable (same approach as LibcInternalStubs
// realloc forward). Decoded frames are counted and dropped — no presentation
// here (Composite owns surfaces).
//
// Arg convention (existing __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI (uint64_t x4) signatures are kept):
// a1 = player handle, a2 = guest ptr / immediate value, a3 = size / second value,
// a4 = flags. Return codes: 0 = OK (see libs/errno.h), nonzero = error.
// MediaKeys/Session + thumbnail trio + aux trio stay documented NO-OPs
// (no DRM backend) with opportunistic handle validation.

struct MediaSample {
	uint64_t data     = 0; // guest ptr (identity-mapped, host-usable)
	uint64_t size     = 0;
	uint64_t pts      = 0; // REAL timing: SetPtsDtsToMediaSample stores pts/dts...
	uint64_t dts      = 0;
	uint64_t duration = 0; // ...duration refines via Divide splits + demux packet attribution
	uint64_t source   = 0; // source-buffer tag (enqueue a2), matched by RemoveSourceBuffer
	uint64_t id       = 0; // monotonic host id, matched by DeleteMediaSample
};

// Last-good FFmpeg detection for one stream (fail-soft: kept across demux
// failures and served by the track-info getters until a better demux lands).
struct VseStreamInfo {
	bool     present      = false;
	int      codecId      = 0; // AVCodecID
	char     codecName[64] = {};
	uint32_t width        = 0; // video
	uint32_t height       = 0; // video
	uint32_t channels     = 0; // audio
	uint32_t sampleRate   = 0; // audio
	int      streamIndex  = -1;
};

struct VsePlayer {
	uint32_t handle = 0;
	enum class State : uint32_t { Idle = 0, Loaded, Playing, Paused, Stopped } state =
	    State::Idle;
	double   rate       = 1.0;
	bool     loop       = false;
	uint64_t rangeStart = 0;
	uint64_t rangeEnd   = 0;
	uint64_t position   = 0; // last Seek position
	uint64_t duration   = 0; // SetDuration fallback; getters prefer queue sums when known
	uint32_t width      = 3840; // VideoCore4K default until demux finds a video stream
	uint32_t height     = 2160;
	std::vector<MediaSample> samples; // queued input, cap 256, no presentation
	bool     eos           = false; // EndOfStream latched; Play restarts from head when loop else Stopped
	uint64_t playHead      = 0; // restart index after EOS + loop
	bool     demuxed       = false; // last Play-time FFmpeg demux succeeded
	char     demuxFormat[64] = {}; // container name from demux (e.g. "mov,mp4,m4a,3gp,3g2,mj2")
	uint64_t demuxDuration = 0; // container duration in ms, fallback when queue timing unknown
	VseStreamInfo video; // served by GetVideoTrackInfo/GetResolution/SelectVideoTrack
	VseStreamInfo audio; // served by GetAudioTrackInfo/EnableAudioTrack
	uint32_t selectedVideoTrack = 0;
	uint32_t selectedAudioTrack = 0;
	bool     audioEnabled        = true;
};

static constexpr size_t kMaxSamples = 256;

static std::map<uint32_t, VsePlayer> g_players;
static std::mutex                    g_players_mutex;
static uint32_t                      g_nextPlayerHandle = 1; // 0 = failure to the guest
static uint64_t                      g_nextSampleId     = 1; // 0 = invalid sample id

static const char* PlayerStateName(VsePlayer::State s) {
	switch (s) {
		case VsePlayer::State::Idle: return "Idle";
		case VsePlayer::State::Loaded: return "Loaded";
		case VsePlayer::State::Playing: return "Playing";
		case VsePlayer::State::Paused: return "Paused";
		case VsePlayer::State::Stopped: return "Stopped";
	}
	return "Unknown";
}

static void LogPlayerCall(const char* op, uint64_t handle, uint64_t ptr, uint64_t size,
                          uint64_t flags) {
	LOGF("\t %s: handle = %u ptr = 0x%016" PRIx64 " size = %llu flags = 0x%016" PRIx64 "\n",
	     op, static_cast<uint32_t>(handle), ptr, static_cast<unsigned long long>(size), flags);
}

// Caller must hold g_players_mutex.
static VsePlayer* FindPlayerLocked(uint32_t handle) {
	auto it = g_players.find(handle);
	return it != g_players.end() ? &it->second : nullptr;
}

static VsePlayer* LookupPlayer(uint64_t handle, const char* op) {
	auto* p = FindPlayerLocked(static_cast<uint32_t>(handle));
	if (p == nullptr) {
		LOGF("\t %s: unknown player handle %u\n", op, static_cast<uint32_t>(handle));
	}
	return p;
}

// Guest out-param write. Never crashes on null guest pointers: address 0 is
// guest-null, so validate before touching identity-mapped memory.
template <typename T>
static int WriteGuest(uint64_t guest_addr, T value) {
	if (guest_addr == 0) {
		LOGF("\t null guest out-param\n");
		return -1; // EINVAL-style
	}
	*reinterpret_cast<T*>(static_cast<uintptr_t>(guest_addr)) = value;
	return OK;
}

// Append a sample record; drops oldest + LOGFs when full (cap 256). Each
// record gets a monotonic host id and keeps its source-buffer tag so
// RemoveSourceBuffer/DeleteMediaSample can match it later.
static void EnqueueSampleLocked(VsePlayer& p, uint64_t ptr, uint64_t size, uint64_t pts,
                                uint64_t dts) {
	if (p.samples.size() >= kMaxSamples) {
		p.samples.erase(p.samples.begin());
		LOGF("\t player %u sample queue full (%llu), dropped oldest\n", p.handle,
		     static_cast<unsigned long long>(kMaxSamples));
	}
	MediaSample s;
	s.data   = ptr;
	s.size   = size;
	s.pts    = pts;
	s.dts    = dts;
	s.source = ptr; // buffer tag for RemoveSourceBuffer matching
	s.id     = g_nextSampleId++;
	if (s.id == 0) {
		s.id = g_nextSampleId++; // never hand out 0 (0 = invalid to the guest)
	}
	p.samples.push_back(s);
}

// Caller must hold g_players_mutex. a2 is a queue index when in range,
// otherwise a host sample id (or exact data ptr) for id-based removal.
static MediaSample* FindSampleLocked(VsePlayer& p, uint64_t indexOrId) {
	if (indexOrId < p.samples.size()) {
		return &p.samples[static_cast<size_t>(indexOrId)];
	}
	for (auto& s : p.samples) {
		if (s.id == indexOrId || s.data == indexOrId) {
			return &s;
		}
	}
	return nullptr;
}

// Documented NO-OP (no DRM/thumbnail/aux backend): log + OK, but validate the
// player handle when the caller passed one (first REAL validation in this file).
static int NoOpWithHandleCheck(const char* op, uint64_t handle) {
	if (handle != 0) {
		std::scoped_lock lock(g_players_mutex);
		if (LookupPlayer(handle, op) == nullptr) {
			return -1;
		}
	}
	LOGF("\t %s: NO-OP (no backend)\n", op);
	return OK;
}

static void SetPlayerState(VsePlayer& p, VsePlayer::State next, const char* op) {
	LOGF("\t player %u %s: %s -> %s\n", p.handle, op, PlayerStateName(p.state),
	     PlayerStateName(next));
	p.state = next;
}

// ---- REAL FFmpeg demux/decode on host (kytyps5 REAL-service style, like avPlayer) ----
// The VSE sample queue holds guest (identity-mapped) bytes. On Play the queued
// bytes are concatenated into a host buffer and opened through an in-memory
// AVIOContext, mirroring avPlayer's FileStreamer/host-buffer pattern. Detected
// container + stream info is stored on the player and served by
// GetVideoTrackInfo/GetAudioTrackInfo/GetResolution/SelectVideoTrack/
// EnableAudioTrack. Per-sample pts/dts/duration gaps are filled from REAL
// demuxed packet timing (mapped back by packet byte offset). A few video frames
// are decoded and dropped (counted only): no presentation happens here,
// Composite owns surfaces. Every failure path LOGFs and keeps prior state
// (fail-soft, the player keeps running).

struct VseMemReader {
	const uint8_t* data = nullptr;
	size_t         size = 0;
	size_t         pos  = 0;
};

static int VseMemRead(void* opaque, uint8_t* buf, int buf_size) {
	auto* r = static_cast<VseMemReader*>(opaque);
	if (r == nullptr || buf == nullptr || buf_size <= 0) {
		return AVERROR(EINVAL);
	}
	if (r->pos >= r->size) {
		return AVERROR_EOF;
	}
	const size_t left = r->size - r->pos;
	const auto   want = static_cast<size_t>(buf_size);
	const int    n    = static_cast<int>(left < want ? left : want);
	std::memcpy(buf, r->data + r->pos, static_cast<size_t>(n));
	r->pos += static_cast<size_t>(n);
	return n;
}

static int64_t VseMemSeek(void* opaque, int64_t offset, int whence) {
	auto* r = static_cast<VseMemReader*>(opaque);
	if (r == nullptr) {
		return AVERROR(EINVAL);
	}
	if ((whence & AVSEEK_SIZE) != 0) {
		return static_cast<int64_t>(r->size);
	}
	uint64_t base = 0;
	switch (whence & ~AVSEEK_FORCE) {
		case SEEK_SET: break;
		case SEEK_CUR: base = r->pos; break;
		case SEEK_END: base = r->size; break;
		default: return AVERROR(EINVAL);
	}
	uint64_t position = 0;
	if (offset < 0) {
		const uint64_t distance = static_cast<uint64_t>(0) - static_cast<uint64_t>(offset);
		if (distance > base) {
			return AVERROR(EINVAL);
		}
		position = base - distance;
	} else {
		const uint64_t distance = static_cast<uint64_t>(offset);
		if (base > r->size || distance > r->size - base) {
			return AVERROR(EINVAL);
		}
		position = base + distance;
	}
	r->pos = static_cast<size_t>(position);
	return static_cast<int64_t>(position);
}

static constexpr size_t kVseDemuxMaxBytes   = 32ull * 1024ull * 1024ull;
static constexpr int    kVseDemuxMaxPackets = 2048;
static constexpr int    kVseDemuxMaxFrames  = 4;

static uint64_t VseRescaleMs(int64_t v, AVRational tb) {
	if (v == AV_NOPTS_VALUE) {
		return 0;
	}
	const int64_t ms = av_rescale_q(v, tb, AVRational{1, 1000});
	return ms < 0 ? 0 : static_cast<uint64_t>(ms);
}

static const char* VseCodecName(int codecId) {
	const AVCodec* decoder =
	    avcodec_find_decoder(static_cast<AVCodecID>(codecId));
	return (decoder != nullptr && decoder->name != nullptr) ? decoder->name : "unknown";
}

// Queue-derived timing: sum of REAL per-sample durations when any are known,
// else the span of min/max pts, else the demuxed container duration, else the
// SetDuration fallback. Optional outs report the observed min pts / max end.
static uint64_t EffectiveDurationLocked(const VsePlayer& p, uint64_t* outMinPts,
                                        uint64_t* outMaxEnd) {
	uint64_t sum    = 0;
	bool     anyDur = false;
	uint64_t mn     = UINT64_MAX;
	uint64_t mx     = 0;
	bool     anyPts = false;
	for (const auto& s : p.samples) {
		if (s.duration != 0) {
			anyDur = true;
			sum += s.duration;
		}
		if (s.pts != 0 || s.dts != 0) {
			anyPts = true;
			const uint64_t lo = s.pts < s.dts ? s.pts : s.dts;
			const uint64_t hi = s.pts > s.dts ? s.pts : s.dts;
			if (lo < mn) {
				mn = lo;
			}
			uint64_t end = hi + s.duration;
			if (end < hi) {
				end = hi; // saturate, never wrap
			}
			if (end > mx) {
				mx = end;
			}
		}
	}
	if (outMinPts != nullptr) {
		*outMinPts = anyPts ? mn : 0;
	}
	if (outMaxEnd != nullptr) {
		*outMaxEnd = anyPts ? mx : 0;
	}
	if (anyDur) {
		return sum;
	}
	if (anyPts && mx > mn) {
		return mx - mn;
	}
	if (p.demuxDuration != 0) {
		return p.demuxDuration;
	}
	return p.duration;
}

// Caller must hold g_players_mutex. Concatenates queued sample bytes and runs
// REAL FFmpeg demux (+ a few counted-not-shown decoded frames) over them.
static void DemuxQueueLocked(VsePlayer& p) {
	if (p.samples.empty()) {
		LOGF("\t player %u demux: queue empty, nothing to probe\n", p.handle);
		return;
	}
	// 1. Gather queued bytes into one host buffer (cap 32MB, skip holes).
	std::vector<uint8_t> blob;
	std::vector<size_t>  blobStart; // per-contributor start offset, for packet attribution
	std::vector<size_t>  contribIdx; // queue index per contributor
	blobStart.reserve(p.samples.size());
	contribIdx.reserve(p.samples.size());
	for (size_t i = 0; i < p.samples.size(); ++i) {
		const auto& s = p.samples[i];
		if (s.size == 0 || s.data == 0) {
			LOGF("\t player %u demux: sample %llu skipped (size=%llu data=0x%016" PRIx64
			     ")\n",
			     p.handle, static_cast<unsigned long long>(i),
			     static_cast<unsigned long long>(s.size), s.data);
			continue;
		}
		if (blob.size() + static_cast<size_t>(s.size) > kVseDemuxMaxBytes ||
		    blob.size() + static_cast<size_t>(s.size) < blob.size()) {
			LOGF("\t player %u demux: truncated at %llu bytes (cap %llu)\n", p.handle,
			     static_cast<unsigned long long>(blob.size()),
			     static_cast<unsigned long long>(kVseDemuxMaxBytes));
			break;
		}
		blobStart.push_back(blob.size());
		contribIdx.push_back(i);
		const auto* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(s.data));
		blob.insert(blob.end(), src, src + static_cast<size_t>(s.size));
	}
	if (blob.empty()) {
		LOGF("\t player %u demux: no readable bytes, keep prior state\n", p.handle);
		return;
	}
	// 2. In-memory AVIO (avPlayer host-buffer pattern).
	VseMemReader reader{blob.data(), blob.size(), 0};
	uint8_t*     avioBuf = static_cast<uint8_t*>(av_malloc(4096));
	if (avioBuf == nullptr) {
		LOGF("\t player %u demux: av_malloc failed, keep prior state\n", p.handle);
		return;
	}
	AVIOContext* avio =
	    avio_alloc_context(avioBuf, 4096, 0, &reader, VseMemRead, nullptr, VseMemSeek);
	if (avio == nullptr) {
		av_free(avioBuf);
		LOGF("\t player %u demux: avio_alloc_context failed, keep prior state\n", p.handle);
		return;
	}
	AVFormatContext* fmt = avformat_alloc_context();
	if (fmt == nullptr) {
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		LOGF("\t player %u demux: avformat_alloc_context failed, keep prior state\n", p.handle);
		return;
	}
	fmt->pb                = avio;
	AVFormatContext* opened = fmt;
	if (const int rc = avformat_open_input(&opened, nullptr, nullptr, nullptr); rc < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE] = {};
		av_make_error_string(err, sizeof(err), rc);
		LOGF("\t player %u demux: avformat_open_input failed (%s), keep prior state\n",
		     p.handle, err);
		// open_input frees the context on failure (same assumption as avPlayer);
		// the custom AVIO buffer is still ours.
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		return;
	}
	fmt = opened;
	if (const int rc = avformat_find_stream_info(fmt, nullptr); rc < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE] = {};
		av_make_error_string(err, sizeof(err), rc);
		LOGF("\t player %u demux: avformat_find_stream_info failed (%s), keep prior state\n",
		     p.handle, err);
		avformat_close_input(&fmt);
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		return;
	}
	const char* formatName =
	    (fmt->iformat != nullptr && fmt->iformat->name != nullptr) ? fmt->iformat->name : "unknown";
	// 3. Detect the first video + first audio stream.
	int videoIdx = -1;
	int audioIdx = -1;
	for (unsigned i = 0; i < fmt->nb_streams; ++i) {
		AVStream* st = fmt->streams[i];
		if (st == nullptr || st->codecpar == nullptr) {
			continue;
		}
		if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoIdx < 0) {
			videoIdx = static_cast<int>(i);
		} else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioIdx < 0) {
			audioIdx = static_cast<int>(i);
		}
	}
	uint32_t videoW = 0;
	uint32_t videoH = 0;
	uint32_t audioCh = 0;
	if (videoIdx >= 0) {
		const auto* par = fmt->streams[videoIdx]->codecpar;
		videoW          = par->width > 0 ? static_cast<uint32_t>(par->width) : 0;
		videoH          = par->height > 0 ? static_cast<uint32_t>(par->height) : 0;
	}
	if (audioIdx >= 0) {
		const auto* par = fmt->streams[audioIdx]->codecpar;
		audioCh = par->ch_layout.nb_channels > 0 ? static_cast<uint32_t>(par->ch_layout.nb_channels)
		                                         : 0;
	}
	// Codec/format detection, logged per contributing sample.
	for (const size_t si : contribIdx) {
		const auto& s = p.samples[si];
		LOGF("\t player %u sample %llu (id=%llu %llu bytes): format=%s video=%s %ux%u "
		     "audio=%s ch=%u\n",
		     p.handle, static_cast<unsigned long long>(si),
		     static_cast<unsigned long long>(s.id), static_cast<unsigned long long>(s.size),
		     formatName,
		     videoIdx >= 0 ? VseCodecName(fmt->streams[videoIdx]->codecpar->codec_id) : "none",
		     videoW, videoH,
		     audioIdx >= 0 ? VseCodecName(fmt->streams[audioIdx]->codecpar->codec_id) : "none",
		     audioCh);
	}
	// 4. Store detection (served by track-info getters + GetResolution).
	if (videoIdx >= 0) {
		const auto* par    = fmt->streams[videoIdx]->codecpar;
		p.video.present    = true;
		p.video.codecId    = static_cast<int>(par->codec_id);
		std::snprintf(p.video.codecName, sizeof(p.video.codecName), "%s",
		              VseCodecName(par->codec_id));
		p.video.width       = videoW;
		p.video.height      = videoH;
		p.video.streamIndex = videoIdx;
		if (videoW != 0 && videoH != 0) {
			p.width  = videoW;
			p.height = videoH;
		}
	}
	if (audioIdx >= 0) {
		const auto* par    = fmt->streams[audioIdx]->codecpar;
		p.audio.present    = true;
		p.audio.codecId    = static_cast<int>(par->codec_id);
		std::snprintf(p.audio.codecName, sizeof(p.audio.codecName), "%s",
		              VseCodecName(par->codec_id));
		p.audio.channels    = audioCh;
		p.audio.sampleRate  = par->sample_rate > 0 ? static_cast<uint32_t>(par->sample_rate) : 0;
		p.audio.streamIndex = audioIdx;
	}
	p.demuxed = true;
	std::snprintf(p.demuxFormat, sizeof(p.demuxFormat), "%s", formatName);
	if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
		p.demuxDuration = static_cast<uint64_t>(fmt->duration / (AV_TIME_BASE / 1000));
	}
	// 5. Packet pass: REAL per-sample pts/dts/duration via packet byte-offset
	// attribution + decode a few video frames (counted, dropped — no presentation).
	const AVCodec*    vdec = nullptr;
	AVCodecContext*   vctx = nullptr;
	if (videoIdx >= 0) {
		vdec = avcodec_find_decoder(fmt->streams[videoIdx]->codecpar->codec_id);
		if (vdec != nullptr) {
			vctx = avcodec_alloc_context3(vdec);
			if (vctx != nullptr &&
			    (avcodec_parameters_to_context(vctx, fmt->streams[videoIdx]->codecpar) < 0 ||
			     avcodec_open2(vctx, vdec, nullptr) < 0)) {
				avcodec_free_context(&vctx);
				LOGF("\t player %u demux: video decoder open failed, timing only\n", p.handle);
			}
		}
	}
	AVPacket* pkt = av_packet_alloc();
	int       packets = 0;
	int       frames  = 0;
	int       frameW  = 0;
	int       frameH  = 0;
	int       framePix = -1;
	uint64_t  timed    = 0;
	if (pkt != nullptr) {
		while (packets < kVseDemuxMaxPackets && frames < kVseDemuxMaxFrames &&
		       av_read_frame(fmt, pkt) >= 0) {
			++packets;
			if (pkt->stream_index < 0 ||
			    static_cast<unsigned>(pkt->stream_index) >= fmt->nb_streams) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream* st = fmt->streams[pkt->stream_index];
			if (pkt->pos >= 0 && st != nullptr) {
				const auto pos = static_cast<uint64_t>(pkt->pos);
				for (size_t k = 0; k < contribIdx.size(); ++k) {
					const uint64_t start = static_cast<uint64_t>(blobStart[k]);
					const uint64_t end =
					    (k + 1 < contribIdx.size())
					        ? static_cast<uint64_t>(blobStart[k + 1])
					        : static_cast<uint64_t>(blob.size());
					if (pos >= start && pos < end) {
						auto& s = p.samples[contribIdx[k]];
						if (s.duration == 0 && pkt->duration > 0) {
							s.duration = VseRescaleMs(pkt->duration, st->time_base);
						}
						if (s.pts == 0 && s.dts == 0) {
							if (pkt->pts != AV_NOPTS_VALUE) {
								s.pts = VseRescaleMs(pkt->pts, st->time_base);
							}
							if (pkt->dts != AV_NOPTS_VALUE) {
								s.dts = VseRescaleMs(pkt->dts, st->time_base);
							}
						}
						++timed;
						break;
					}
				}
			}
			if (vctx != nullptr && pkt->stream_index == videoIdx) {
				if (avcodec_send_packet(vctx, pkt) >= 0) {
					for (;;) {
						AVFrame* f = av_frame_alloc();
						if (f == nullptr) {
							break;
						}
						if (const int r = avcodec_receive_frame(vctx, f); r == 0) {
							++frames;
							if (frames == 1) {
								frameW   = f->width;
								frameH   = f->height;
								framePix = f->format;
							}
							av_frame_free(&f);
							if (frames >= kVseDemuxMaxFrames) {
								break;
							}
						} else {
							av_frame_free(&f);
							break;
						}
					}
				}
			}
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
	}
	if (vctx != nullptr) {
		avcodec_free_context(&vctx);
	}
	LOGF("\t player %u demux ok: format=%s packets=%d timed=%llu frames(decoded, "
	     "dropped)=%d first=%dx%d pixfmt=%d\n",
	     p.handle, formatName, packets, static_cast<unsigned long long>(timed), frames, frameW,
	     frameH, framePix);
	avformat_close_input(&fmt);
	av_freep(&avio->buffer);
	avio_context_free(&avio);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseCreatePlayer(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("create", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	uint32_t handle = g_nextPlayerHandle++;
	if (handle == 0) {
		handle = g_nextPlayerHandle++; // never hand out 0 (0 = failure to the guest)
	}
	VsePlayer p;
	p.handle = handle;
	g_players.emplace(handle, std::move(p));
	LOGF("\t created player handle = %u\n", handle);
	return static_cast<int>(handle); // nonzero = success, 0 = failure
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseDeletePlayer(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("delete", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto it = g_players.find(static_cast<uint32_t>(a1));
	if (it == g_players.end()) {
		LOGF("\t delete: unknown player handle %u\n", static_cast<uint32_t>(a1));
		return -1;
	}
	g_players.erase(it);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseLoad(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("load", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "load");
	if (p == nullptr) {
		return -1;
	}
	p->eos      = false;
	p->playHead = 0;
	// Fresh content: last-good detection no longer describes the queue.
	p->demuxed       = false;
	p->demuxFormat[0] = '\0';
	p->demuxDuration = 0;
	p->video         = VseStreamInfo{};
	p->audio         = VseStreamInfo{};
	SetPlayerState(*p, VsePlayer::State::Loaded, "load"); // Idle -> Loaded
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VsePlay(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("play", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "play");
	if (p == nullptr) {
		return -1;
	}
	if (p->eos) {
		// EndOfStream latched: loop restarts from head, otherwise stay Stopped.
		if (p->loop) {
			p->eos      = false;
			p->playHead = 0;
			LOGF("\t player %u play: EOS restart from head (loop)\n", p->handle);
		} else {
			SetPlayerState(*p, VsePlayer::State::Stopped, "play-after-eos");
			return OK;
		}
	}
	if (!p->samples.empty()) {
		DemuxQueueLocked(*p); // REAL FFmpeg demux; fail-soft inside
	}
	SetPlayerState(*p, VsePlayer::State::Playing, "play");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VsePause(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("pause", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "pause");
	if (p == nullptr) {
		return -1;
	}
	SetPlayerState(*p, VsePlayer::State::Paused, "pause");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseStop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("stop", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "stop");
	if (p == nullptr) {
		return -1;
	}
	p->eos      = false;
	p->playHead = 0;
	SetPlayerState(*p, VsePlayer::State::Stopped, "stop");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseQueryVideoMemory(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("query-video-memory", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "query-video-memory");
	if (p == nullptr) {
		return -1;
	}
	uint64_t total = 0;
	for (const auto& s : p->samples) {
		total += s.size;
	}
	LOGF("\t player %u queued bytes = %llu in %llu samples\n", p->handle,
	     static_cast<unsigned long long>(total),
	     static_cast<unsigned long long>(p->samples.size()));
	return WriteGuest<uint64_t>(a2, total);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseAppendBuffer(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("append-buffer", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "append-buffer");
	if (p == nullptr) {
		return -1;
	}
	EnqueueSampleLocked(*p, a2, a3, 0, 0); // pts/dts filled by demux later
	LOGF("\t player %u queued %llu bytes, depth %llu\n", p->handle,
	     static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(p->samples.size()));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseAddSourceBuffer(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("add-source-buffer", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "add-source-buffer");
	if (p == nullptr) {
		return -1;
	}
	EnqueueSampleLocked(*p, a2, a3, 0, 0); // pts/dts filled by demux later
	LOGF("\t player %u queued %llu bytes, depth %llu\n", p->handle,
	     static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(p->samples.size()));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseEnqueueMediaSample(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("enqueue-media-sample", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "enqueue-media-sample");
	if (p == nullptr) {
		return -1;
	}
	EnqueueSampleLocked(*p, a2, a3, 0, 0); // pts/dts filled by demux later
	LOGF("\t player %u queued %llu bytes, depth %llu\n", p->handle,
	     static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(p->samples.size()));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSeek(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("seek", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "seek");
	if (p == nullptr) {
		return -1;
	}
	p->position = a2;
	LOGF("\t player %u position = %llu\n", p->handle, static_cast<unsigned long long>(a2));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseIsTransparentVideoMode(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetPlaybackRate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-playback-rate", a1, a2, a3, a4);
	double rate = 1.0; // a2 carries the IEEE-754 bits (no guest deref: never crashes)
	static_assert(sizeof(rate) == sizeof(a2), "rate bit-cast");
	std::memcpy(&rate, &a2, sizeof(rate));
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "set-playback-rate");
	if (p == nullptr) {
		return -1;
	}
	p->rate = rate;
	LOGF("\t player %u rate = %f\n", p->handle, rate);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetPlaybackRate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-playback-rate", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-playback-rate");
	if (p == nullptr) {
		return -1;
	}
	return WriteGuest<double>(a2, p->rate); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetLoop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-loop", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "set-loop");
	if (p == nullptr) {
		return -1;
	}
	p->loop = (a2 != 0);
	LOGF("\t player %u loop = %u\n", p->handle, p->loop ? 1u : 0u);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetLoop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-loop", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-loop");
	if (p == nullptr) {
		return -1;
	}
	return WriteGuest<uint32_t>(a2, p->loop ? 1u : 0u); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetPlaybackRange(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-playback-range", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "set-playback-range");
	if (p == nullptr) {
		return -1;
	}
	p->rangeStart = a2;
	p->rangeEnd   = a3;
	LOGF("\t player %u range = [%llu, %llu]\n", p->handle,
	     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetPlaybackRange(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-playback-range", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-playback-range");
	if (p == nullptr) {
		return -1;
	}
	if (WriteGuest<uint64_t>(a2, p->rangeStart) != OK) {
		return -1; // null guest ptr
	}
	return WriteGuest<uint64_t>(a3, p->rangeEnd); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetDuration(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-duration", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-duration");
	if (p == nullptr) {
		return -1;
	}
	uint64_t mn = 0;
	uint64_t mx = 0;
	const uint64_t dur =
	    EffectiveDurationLocked(*p, &mn, &mx); // queue-derived, not a stored constant
	LOGF("\t player %u duration = %llu (minPts=%llu maxEnd=%llu, %llu samples)\n", p->handle,
	     static_cast<unsigned long long>(dur), static_cast<unsigned long long>(mn),
	     static_cast<unsigned long long>(mx),
	     static_cast<unsigned long long>(p->samples.size()));
	return WriteGuest<uint64_t>(a2, dur); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetResolution(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-resolution", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-resolution");
	if (p == nullptr) {
		return -1;
	}
	if (WriteGuest<uint32_t>(a2, p->width) != OK) {
		return -1; // null guest ptr
	}
	LOGF("\t player %u resolution = %ux%u%s\n", p->handle, p->width, p->height,
	     p->demuxed ? " (demuxed)" : " (default)");
	return WriteGuest<uint32_t>(a3, p->height); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetLoadingProgress(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-loading-progress", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-loading-progress");
	if (p == nullptr) {
		return -1;
	}
	if (a2 == 0) {
		LOGF("\t null guest out-param\n");
		return -1;
	}
	// Progress derived from the queue (never a stored constant): empty = 0
	// unless EOS latched, demuxed/EOS = complete, otherwise byte-fraction.
	double progress = 0.0;
	if (p->samples.empty()) {
		progress = p->eos ? 1.0 : 0.0;
	} else if (p->eos || p->demuxed) {
		progress = 1.0;
	} else {
		uint64_t total = 0;
		for (const auto& s : p->samples) {
			total += s.size;
		}
		const double frac =
		    static_cast<double>(total) / static_cast<double>(1024ull * 1024ull);
		progress = 0.10 + 0.89 * (frac < 1.0 ? frac : 1.0);
	}
	LOGF("\t player %u loading progress = %f (%llu samples eos=%u demuxed=%u)\n", p->handle,
	     progress, static_cast<unsigned long long>(p->samples.size()), p->eos ? 1u : 0u,
	     p->demuxed ? 1u : 0u);
	return WriteGuest<double>(a2, progress); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseIsLiveStreaming(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("is-live-streaming", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "is-live-streaming");
	if (p == nullptr) {
		return -1;
	}
	if (a2 == 0) {
		LOGF("\t player %u is-live-streaming: null guest out-param\n", p->handle);
		return OK; // optional out-param: nothing to report through
	}
	return WriteGuest<uint32_t>(a2, 0); // not live
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSendAuxMessageSync(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("send-aux-message-sync", a1, a2, a3, a4);
	return NoOpWithHandleCheck("send-aux-message-sync", a1); // NO-OP: no aux backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSendAuxMessageForRepeatedResults(uint64_t a1, uint64_t a2, uint64_t a3,
                                                            uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("send-aux-message-for-repeated-results", a1, a2, a3, a4);
	return NoOpWithHandleCheck("send-aux-message-for-repeated-results", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseAbortAuxMessage(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("abort-aux-message", a1, a2, a3, a4);
	return NoOpWithHandleCheck("abort-aux-message", a1); // NO-OP: no aux backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetTimeProperty(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-time-property", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-time-property");
	if (p == nullptr) {
		return -1;
	}
	if (a3 == 0) {
		LOGF("\t null guest out-param\n");
		return -1;
	}
	// a2 selects the property; every value is computed from the queue.
	// 0 = position, 1 = duration, 2 = min pts, 3 = max end, 4 = buffered
	// bytes, 5 = sample count, default = position.
	uint64_t mn = 0;
	uint64_t mx = 0;
	const uint64_t dur = EffectiveDurationLocked(*p, &mn, &mx);
	uint64_t total = 0;
	for (const auto& s : p->samples) {
		total += s.size;
	}
	uint64_t value = p->position;
	switch (a2) {
		case 0: value = p->position; break;
		case 1: value = dur; break;
		case 2: value = mn; break;
		case 3: value = mx; break;
		case 4: value = total; break;
		case 5: value = static_cast<uint64_t>(p->samples.size()); break;
		default: value = p->position; break;
	}
	LOGF("\t player %u time-property %llu = %llu\n", p->handle,
	     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(value));
	return WriteGuest<uint64_t>(a3, value); // null guest ptr -> -1
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGenerateThumbnail(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("generate-thumbnail", a1, a2, a3, a4);
	return NoOpWithHandleCheck("generate-thumbnail", a1); // NO-OP: no thumbnail backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseReleaseThumbnailData(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("release-thumbnail-data", a1, a2, a3, a4);
	return NoOpWithHandleCheck("release-thumbnail-data", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseCancelThumbnail(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("cancel-thumbnail", a1, a2, a3, a4);
	return NoOpWithHandleCheck("cancel-thumbnail", a1); // NO-OP: no thumbnail backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseRemoveSourceBuffer(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("remove-source-buffer", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "remove-source-buffer");
	if (p == nullptr) {
		return -1;
	}
	if (a2 == 0) {
		LOGF("\t player %u remove-source-buffer: null source id\n", p->handle);
		return -1;
	}
	// Drop every sample enqueued from that source (data ptr, source tag, or id).
	const size_t before = p->samples.size();
	const auto   dropped = std::erase_if(p->samples, [&](const MediaSample& s) {
        return s.data == a2 || s.source == a2 || s.id == a2;
    });
	LOGF("\t player %u remove-source-buffer 0x%016" PRIx64 ": dropped %llu of %llu samples\n",
	     p->handle, a2, static_cast<unsigned long long>(dropped),
	     static_cast<unsigned long long>(before));
	return OK; // fail-soft: zero matches still succeeds
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseEndOfStream(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("end-of-stream", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "end-of-stream");
	if (p == nullptr) {
		return -1;
	}
	p->eos = true; // latched; Play restarts from head when loop else goes Stopped
	LOGF("\t player %u end-of-stream: EOS latched (loop=%u)\n", p->handle, p->loop ? 1u : 0u);
	if (p->state == VsePlayer::State::Playing || p->state == VsePlayer::State::Paused) {
		SetPlayerState(*p, VsePlayer::State::Stopped, "end-of-stream");
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetLiveSeekableRange(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseClearLiveSeekableRange(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetDuration(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-duration", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "set-duration");
	if (p == nullptr) {
		return -1;
	}
	p->duration = a2;
	LOGF("\t player %u duration = %llu\n", p->handle, static_cast<unsigned long long>(a2));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseAbortParsing(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("abort-parsing", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "abort-parsing");
	if (p == nullptr) {
		return -1;
	}
	// Clear the queue; the state machine stays (last-good demux info is kept
	// so track getters keep serving until the next successful Play).
	const auto queued = static_cast<unsigned long long>(p->samples.size());
	p->samples.clear();
	p->eos      = false;
	p->playHead = 0;
	LOGF("\t player %u abort-parsing: cleared %llu samples, state stays %s\n", p->handle,
	     queued, PlayerStateName(p->state));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseEnableAudioTrack(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("enable-audio-track", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "enable-audio-track");
	if (p == nullptr) {
		return -1;
	}
	// a2 = track id, a3 = nonzero enables (selecting a nonzero track enables too).
	p->selectedAudioTrack = static_cast<uint32_t>(a2);
	p->audioEnabled        = (a2 != 0) || (a3 != 0);
	LOGF("\t player %u audio track %u %s (detected: %s)\n", p->handle,
	     p->selectedAudioTrack, p->audioEnabled ? "enabled" : "disabled",
	     p->audio.present ? p->audio.codecName : "none");
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSelectVideoTrack(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("select-video-track", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "select-video-track");
	if (p == nullptr) {
		return -1;
	}
	p->selectedVideoTrack = static_cast<uint32_t>(a2);
	LOGF("\t player %u video track %u selected (detected: %s %ux%u)\n", p->handle,
	     p->selectedVideoTrack, p->video.present ? p->video.codecName : "none",
	     p->video.present ? p->video.width : 0, p->video.present ? p->video.height : 0);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetBufferedInfo(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-buffered-info", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-buffered-info");
	if (p == nullptr) {
		return -1;
	}
	uint64_t total = 0;
	for (const auto& s : p->samples) {
		total += s.size;
	}
	// a2 = sample-count out (required), a3 = queued-bytes out (optional).
	if (WriteGuest<uint64_t>(a2, static_cast<uint64_t>(p->samples.size())) != OK) {
		return -1; // null guest ptr
	}
	uint64_t mn = 0;
	uint64_t mx = 0;
	const uint64_t dur = EffectiveDurationLocked(*p, &mn, &mx);
	LOGF("\t player %u buffered: %llu samples, %llu bytes, duration=%llu minPts=%llu "
	     "maxEnd=%llu\n",
	     p->handle, static_cast<unsigned long long>(p->samples.size()),
	     static_cast<unsigned long long>(total), static_cast<unsigned long long>(dur),
	     static_cast<unsigned long long>(mn), static_cast<unsigned long long>(mx));
	if (a3 != 0) {
		*reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(a3)) = total;
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetAudioTrackInfo(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-audio-track-info", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-audio-track-info");
	if (p == nullptr) {
		return -1;
	}
	if (!p->audio.present) {
		LOGF("\t player %u audio track info: no audio stream demuxed yet\n", p->handle);
		return -1;
	}
	if (a3 == 0) {
		LOGF("\t null guest out-param\n");
		return -1;
	}
	// a2 = track id (logged), a3 = guest out struct {codecId, channels,
	// sampleRate}, a4 = byte-size cap (0 = full write).
	const uint64_t cap = (a4 != 0) ? a4 : 12;
	if (cap < 4) {
		LOGF("\t player %u audio track info: out struct too small (%llu)\n", p->handle,
		     static_cast<unsigned long long>(a4));
		return -1;
	}
	if (WriteGuest<uint32_t>(a3, static_cast<uint32_t>(p->audio.codecId)) != OK) {
		return -1; // null guest ptr
	}
	if (cap >= 8) {
		WriteGuest<uint32_t>(a3 + 4, p->audio.channels);
	}
	if (cap >= 12) {
		WriteGuest<uint32_t>(a3 + 8, p->audio.sampleRate);
	}
	LOGF("\t player %u audio track %llu: codec=%s ch=%u rate=%u (enabled=%u)\n", p->handle,
	     static_cast<unsigned long long>(a2), p->audio.codecName, p->audio.channels,
	     p->audio.sampleRate, p->audioEnabled ? 1u : 0u);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseGetVideoTrackInfo(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("get-video-track-info", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "get-video-track-info");
	if (p == nullptr) {
		return -1;
	}
	if (!p->video.present) {
		LOGF("\t player %u video track info: no video stream demuxed yet\n", p->handle);
		return -1;
	}
	if (a3 == 0) {
		LOGF("\t null guest out-param\n");
		return -1;
	}
	// a2 = track id (logged), a3 = guest out struct {codecId, width, height},
	// a4 = byte-size cap (0 = full write).
	const uint64_t cap = (a4 != 0) ? a4 : 12;
	if (cap < 4) {
		LOGF("\t player %u video track info: out struct too small (%llu)\n", p->handle,
		     static_cast<unsigned long long>(a4));
		return -1;
	}
	if (WriteGuest<uint32_t>(a3, static_cast<uint32_t>(p->video.codecId)) != OK) {
		return -1; // null guest ptr
	}
	if (cap >= 8) {
		WriteGuest<uint32_t>(a3 + 4, p->video.width);
	}
	if (cap >= 12) {
		WriteGuest<uint32_t>(a3 + 8, p->video.height);
	}
	LOGF("\t player %u video track %llu: codec=%s %ux%u (selected=%u)\n", p->handle,
	     static_cast<unsigned long long>(a2), p->video.codecName, p->video.width,
	     p->video.height, p->selectedVideoTrack);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetPtsDtsToMediaSample(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-pts-dts-to-media-sample", a1, a2, a3, a4);
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "set-pts-dts-to-media-sample");
	if (p == nullptr) {
		return -1;
	}
	auto* s = FindSampleLocked(*p, a2);
	if (s == nullptr) {
		LOGF("\t player %u set-pts-dts: no sample %llu (%llu queued)\n", p->handle,
		     static_cast<unsigned long long>(a2),
		     static_cast<unsigned long long>(p->samples.size()));
		return -1;
	}
	// a2 = queue index (or sample id), a3 = pts, a4 = dts — immediates only,
	// never dereferenced, so this never crashes. Duration is kept (refined by
	// Divide splits and demux packet attribution).
	s->pts = a3;
	s->dts = a4;
	LOGF("\t player %u sample id=%llu: pts=%llu dts=%llu (duration=%llu kept)\n", p->handle,
	     static_cast<unsigned long long>(s->id), static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(a4), static_cast<unsigned long long>(s->duration));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseDivideMediaSample(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("divide-media-sample", a1, a2, a3, a4);
	(void)a4;
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "divide-media-sample");
	if (p == nullptr) {
		return -1;
	}
	// Index-only (split position must stay stable): no id fallback here.
	if (a2 >= p->samples.size()) {
		LOGF("\t player %u divide: sample index %llu out of range (%llu queued)\n", p->handle,
		     static_cast<unsigned long long>(a2),
		     static_cast<unsigned long long>(p->samples.size()));
		return -1;
	}
	if (p->samples.size() >= kMaxSamples) {
		LOGF("\t player %u divide: queue full (%llu), no room to split\n", p->handle,
		     static_cast<unsigned long long>(kMaxSamples));
		return -1;
	}
	const size_t idx = static_cast<size_t>(a2);
	const MediaSample src = p->samples[idx]; // copy: insert may reallocate
	if (src.data == 0 || a3 == 0 || a3 >= src.size) {
		LOGF("\t player %u divide: bad offset %llu for sample %llu (%llu bytes)\n", p->handle,
		     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a2),
		     static_cast<unsigned long long>(src.size));
		return -1;
	}
	MediaSample tail;
	tail.data   = src.data + a3;
	tail.size   = src.size - a3;
	tail.source = src.source;
	tail.id     = g_nextSampleId++;
	if (tail.id == 0) {
		tail.id = g_nextSampleId++;
	}
	// Split timing proportionally by bytes (same units as the original stamps).
	tail.pts      = src.pts;
	tail.dts      = src.dts;
	tail.duration = 0;
	if (src.duration != 0) {
		const uint64_t second = src.duration * tail.size / src.size;
		tail.duration             = second;
		p->samples[idx].duration  = src.duration - second;
		tail.pts                  = src.pts + (src.duration - second);
		tail.dts                  = src.dts + (src.duration - second);
	}
	p->samples[idx].size = a3;
	p->samples.insert(p->samples.begin() + static_cast<std::ptrdiff_t>(idx) + 1, tail);
	LOGF("\t player %u divide: sample %llu (id=%llu) split at %llu into %llu+%llu bytes\n",
	     p->handle, static_cast<unsigned long long>(a2), static_cast<unsigned long long>(src.id),
	     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a3),
	     static_cast<unsigned long long>(tail.size));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseDeleteMediaSample(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("delete-media-sample", a1, a2, a3, a4);
	(void)a3;
	(void)a4;
	std::scoped_lock lock(g_players_mutex);
	auto* p = LookupPlayer(a1, "delete-media-sample");
	if (p == nullptr) {
		return -1;
	}
	auto* s = FindSampleLocked(*p, a2);
	if (s == nullptr) {
		LOGF("\t player %u delete: no sample %llu (%llu queued)\n", p->handle,
		     static_cast<unsigned long long>(a2),
		     static_cast<unsigned long long>(p->samples.size()));
		return -1;
	}
	const uint64_t erasedId = s->id;
	const auto     idx =
	    static_cast<size_t>(s - p->samples.data()); // pointer into our own vector
	p->samples.erase(p->samples.begin() + static_cast<std::ptrdiff_t>(idx));
	LOGF("\t player %u delete: removed sample id=%llu, %llu remain\n", p->handle,
	     static_cast<unsigned long long>(erasedId),
	     static_cast<unsigned long long>(p->samples.size()));
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseRequestMediaKeySystemAccess(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("request-media-key-system-access", a1, a2, a3, a4);
	return NoOpWithHandleCheck("request-media-key-system-access", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseDestroyMediaKeySystemAccess(uint64_t a1, uint64_t a2, uint64_t a3,
                                                       uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("destroy-media-key-system-access", a1, a2, a3, a4);
	return NoOpWithHandleCheck("destroy-media-key-system-access", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySystemAccessGetConfiguration(uint64_t a1, uint64_t a2,
                                                                 uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-system-access-get-configuration", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-system-access-get-configuration", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySystemAccessSetCdmStorageConfig(uint64_t a1, uint64_t a2,
                                                                    uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-system-access-set-cdm-storage-config", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-system-access-set-cdm-storage-config", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySystemAccessCreateMediaKeys(uint64_t a1, uint64_t a2,
                                                                uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-system-access-create-media-keys", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-system-access-create-media-keys", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySystemAccessDestroyMediaKeys(uint64_t a1, uint64_t a2,
                                                                 uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-system-access-destroy-media-keys", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-system-access-destroy-media-keys", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeysSetServerCertificate(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-keys-set-server-certificate", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-keys-set-server-certificate", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeysCreateSession(uint64_t a1, uint64_t a2, uint64_t a3,
                                                  uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-keys-create-session", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-keys-create-session", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeysDestroySession(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-keys-destroy-session", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-keys-destroy-session", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionSetEventHandler(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-set-event-handler", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-set-event-handler", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionGenerateRequest(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-generate-request", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-generate-request", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionUpdate(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-update", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-update", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionClose(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-close", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-close", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionLoad(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-load", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-load", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionRemove(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-remove", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-remove", a1); // NO-OP: no DRM backend
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionGetSessionId(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-get-session-id", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-get-session-id", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionGetExpiration(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-get-expiration", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-get-expiration", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseMediaKeySessionGetKeyStatuses(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("media-key-session-get-key-statuses", a1, a2, a3, a4);
	return NoOpWithHandleCheck("media-key-session-get-key-statuses", a1); // NO-OP
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VseSetMediaKeys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LogPlayerCall("set-media-keys", a1, a2, a3, a4);
	return NoOpWithHandleCheck("set-media-keys", a1); // NO-OP: no DRM backend
}

// Resident REAL frame pump for VideoCoreIfMainLoop (VCI block at the top of
// this file). Does NOT call into Composite (different lib, no header): the
// 1ms poll quantum is mirrored locally via Common::Thread::SleepMicro.
//
// Handle: a1 is a VSE player handle when nonzero; unknown/0 runs the resident
// headless engine loop (NPXS40038 session model: the real MainLoop blocks
// serving the engine, and returning ends the guest session, so this path
// never returns on its own — host window close tears the process down).
// Per iteration: advance a Playing player by one 1/60s frame quantum in
// position/duration state (ms units, matching demuxDuration/VseRescaleMs),
// SleepMicro(1000), LOGF progress every 60th iteration. The headless loop
// heartbeats every 600 ticks and adopts any player reaching Playing state.
// Exits on: EOS without loop, known-player 600-frame cap, player Stopped
// externally (or deleted). Returns 0.
int VideoMainLoopPump(uint64_t handle) {
	static constexpr int      kMaxFrames   = 600; // known player cap (10s @60fps)
	static constexpr uint32_t kPumpSleepUs = 1000; // local mirror of Composite 1ms poll
	static constexpr uint64_t kFrameStepMs   = 16; // ~1/60s in ms position units
	static constexpr int      kLogEvery      = 60; // progress cadence (10 lines max)

	const uint32_t h = static_cast<uint32_t>(handle);
	bool           known = false;
	{
		std::scoped_lock lock(g_players_mutex);
		known = (h != 0 && FindPlayerLocked(h) != nullptr);
	}
	if (!known) {
		// ps5-vsh: resident engine loop (NPXS40038 session model). The real
		// VideoCoreIfMainLoop blocks serving the engine; returning ends the
		// guest session (main() proceeds to Exit — verified: a bounded pump
		// makes main() log Exit and quit with no render thread, no USB, no
		// player). So the headless path never returns on its own: it pumps
		// a 1ms quantum forever (host window close tears the process down),
		// heartbeat-logging every 600 ticks, and adopts any player that
		// reaches Playing state in the meantime (e.g. a session established
		// after entry) by falling into the player pump below.
		LOGF("\t video-main-loop: engine resident (close window to stop)\n");
		int tick = 0;
		for (;;) {
			Common::Thread::SleepMicro(kPumpSleepUs);
			++tick;
			if (tick % (kLogEvery * 10) == 0) {
				LOGF("\t video-main-loop: resident tick %d\n", tick);
			}
			uint32_t adopted = 0;
			{
				std::scoped_lock lock(g_players_mutex);
				for (const auto& kv : g_players) {
					if (kv.second.state == VsePlayer::State::Playing) {
						adopted = kv.first;
						break;
					}
				}
			}
			if (adopted != 0) {
				LOGF("\t video-main-loop: adopting player %u\n", adopted);
				handle = adopted;
				known  = true;
				break;
			}
		}
	}
	for (int f = 1; f <= kMaxFrames; ++f) {
		bool     done    = false;
		uint64_t pos     = 0;
		uint64_t dur     = 0;
		bool     playing = false;
		{
			std::scoped_lock lock(g_players_mutex);
			VsePlayer*       p = FindPlayerLocked(h);
			if (p == nullptr) {
				done = true; // deleted externally: stop, never crash
			} else if (p->state == VsePlayer::State::Stopped) {
				done = true; // stopped externally (Stop / EOS latch): stop
				pos  = p->position;
				dur  = EffectiveDurationLocked(*p, nullptr, nullptr);
			} else {
				if (p->state == VsePlayer::State::Playing) {
					playing = true;
					if (p->eos) {
						if (p->loop) {
							p->eos      = false;
							p->playHead = 0;
							p->position = p->rangeStart; // restart head
						} else {
							SetPlayerState(*p, VsePlayer::State::Stopped,
							               "main-loop-eos");
							done = true;
						}
					} else {
						const uint64_t d = EffectiveDurationLocked(*p, nullptr, nullptr);
						uint64_t next = p->position + kFrameStepMs;
						if (next < p->position) {
							next = p->position; // saturate, never wrap
						}
						if (d != 0 && next >= d) {
							if (p->loop) {
								p->position = p->rangeStart; // restart head
								p->playHead = 0;
							} else {
								p->position = d;
								p->eos      = true;
								SetPlayerState(*p, VsePlayer::State::Stopped,
								               "main-loop-eos");
								done = true;
							}
						} else {
							p->position = next;
						}
					}
				}
				pos = p->position;
				dur = EffectiveDurationLocked(*p, nullptr, nullptr);
			}
		}
		Common::Thread::SleepMicro(kPumpSleepUs);
		if (f % kLogEvery == 0) {
			LOGF("\t video-main-loop: frame %d pos=%llu dur=%llu%s\n", f,
			     static_cast<unsigned long long>(pos), static_cast<unsigned long long>(dur),
			     playing ? "" : " (not playing)");
		}
		if (done) {
			break;
		}
	}
	return 0;
}

} // namespace VSE10

LIB_DEFINE(InitVideoStreamingEngineV10_1) {
	LIB_FUNC("VY4Mr66uSqA", VSE10::VseCreatePlayer);
	LIB_FUNC("TBPjAg6a2E4", VSE10::VseDeletePlayer);
	LIB_FUNC("iv5nnUPe-ls", VSE10::VseLoad);
	LIB_FUNC("VInB8YRmmZg", VSE10::VsePlay);
	LIB_FUNC("WFsVlc9eIQs", VSE10::VsePause);
	LIB_FUNC("IX7AgdtkNsI", VSE10::VseStop);
	LIB_FUNC("x32O4iKHZyA", VSE10::VseQueryVideoMemory);
	LIB_FUNC("s3oaAI0fKwU", VSE10::VseAppendBuffer);
	LIB_FUNC("ncXmalKo5TE", VSE10::VseAddSourceBuffer);
	LIB_FUNC("G9GFBN1xtH8", VSE10::VseEnqueueMediaSample);
	LIB_FUNC("EYLkNGuJu+k", VSE10::VseSeek);
	LIB_FUNC("vLAERdvUknc", VSE10::VseIsTransparentVideoMode);
	LIB_FUNC("eXS-hegAVDU", VSE10::VseSetPlaybackRate);
	LIB_FUNC("-RBoTh3JsaM", VSE10::VseGetPlaybackRate);
	LIB_FUNC("dC6mRLm1HQc", VSE10::VseSetLoop);
	LIB_FUNC("x7Vr+deBKo0", VSE10::VseGetLoop);
	LIB_FUNC("q04I0wREB5c", VSE10::VseSetPlaybackRange);
	LIB_FUNC("Xq9X3kt49Fk", VSE10::VseGetPlaybackRange);
	LIB_FUNC("tFgCMXYLEYo", VSE10::VseGetDuration);
	LIB_FUNC("f8LzETNbCYk", VSE10::VseGetResolution);
	LIB_FUNC("w6Y73Ubl0jw", VSE10::VseGetLoadingProgress);
	LIB_FUNC("tcZ2XNW8PO4", VSE10::VseIsLiveStreaming);
	LIB_FUNC("7i7Em8QPYIU", VSE10::VseSendAuxMessageSync);
	LIB_FUNC("u3lPXPjdo2k", VSE10::VseSendAuxMessageForRepeatedResults);
	LIB_FUNC("G7mvPDnYwQQ", VSE10::VseAbortAuxMessage);
	LIB_FUNC("p1jJjKjTEvU", VSE10::VseGetTimeProperty);
	LIB_FUNC("rcTQ2SilW60", VSE10::VseGenerateThumbnail);
	LIB_FUNC("bg-209imcPc", VSE10::VseReleaseThumbnailData);
	LIB_FUNC("ob+vzpK-ePI", VSE10::VseCancelThumbnail);
	LIB_FUNC("zNMGLpbvKpw", VSE10::VseRemoveSourceBuffer);
	LIB_FUNC("j4b+RLwmg3k", VSE10::VseEndOfStream);
	LIB_FUNC("UWEHx8qeF6Y", VSE10::VseSetLiveSeekableRange);
	LIB_FUNC("YZnFDSSQFE8", VSE10::VseClearLiveSeekableRange);
	LIB_FUNC("KjTwhkkB52s", VSE10::VseSetDuration);
	LIB_FUNC("fb3kqOU6eiI", VSE10::VseAbortParsing);
	LIB_FUNC("GFPBvjLfimA", VSE10::VseEnableAudioTrack);
	LIB_FUNC("nEAyXrcgaMY", VSE10::VseSelectVideoTrack);
	LIB_FUNC("Q2+ul4Jj7-4", VSE10::VseGetBufferedInfo);
	LIB_FUNC("6x9vvo92flo", VSE10::VseGetAudioTrackInfo);
	LIB_FUNC("5-P4R09btQQ", VSE10::VseGetVideoTrackInfo);
	LIB_FUNC("d3AkZn2emRY", VSE10::VseSetPtsDtsToMediaSample);
	LIB_FUNC("hZEb-EhU-4g", VSE10::VseDivideMediaSample);
	LIB_FUNC("SfDRCMJoaLE", VSE10::VseDeleteMediaSample);
	LIB_FUNC("Y-Bp-73pyIs", VSE10::VseRequestMediaKeySystemAccess);
	LIB_FUNC("NuTNdlvrugI", VSE10::VseDestroyMediaKeySystemAccess);
	LIB_FUNC("l42jJq+1X9M", VSE10::VseMediaKeySystemAccessGetConfiguration);
	LIB_FUNC("LQ9g9lYxgmc", VSE10::VseMediaKeySystemAccessSetCdmStorageConfig);
	LIB_FUNC("MSWZmqyr3eM", VSE10::VseMediaKeySystemAccessCreateMediaKeys);
	LIB_FUNC("A33uDRt2rQM", VSE10::VseMediaKeySystemAccessDestroyMediaKeys);
	LIB_FUNC("o9B+2IRDKow", VSE10::VseMediaKeysSetServerCertificate);
	LIB_FUNC("405M9x9WM80", VSE10::VseMediaKeysCreateSession);
	LIB_FUNC("tUuzW-ctqRY", VSE10::VseMediaKeysDestroySession);
	LIB_FUNC("mvDN856KXI8", VSE10::VseMediaKeySessionSetEventHandler);
	LIB_FUNC("9rLooB4jRvY", VSE10::VseMediaKeySessionGenerateRequest);
	LIB_FUNC("+gqhFOBhLj4", VSE10::VseMediaKeySessionUpdate);
	LIB_FUNC("RWCz4Gc+lW0", VSE10::VseMediaKeySessionClose);
	LIB_FUNC("sJbzLdIr3Ks", VSE10::VseMediaKeySessionLoad);
	LIB_FUNC("MB+kfLoZFWE", VSE10::VseMediaKeySessionRemove);
	LIB_FUNC("l8vw5ia8Wac", VSE10::VseMediaKeySessionGetSessionId);
	LIB_FUNC("ntQlwZUs6N4", VSE10::VseMediaKeySessionGetExpiration);
	LIB_FUNC("05Uv55TkoHc", VSE10::VseMediaKeySessionGetKeyStatuses);
	LIB_FUNC("sDX++cFuQiE", VSE10::VseSetMediaKeys);
}

} // namespace VideoStreamingEngineV10

namespace SysmoduleStubs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace Sysmod {

// NOTE: the real low-16-bit IsLoaded matching logic lives in
// src/libs/libSysmodule.cpp (SysmoduleIsLoadedId); this is just a log stub
// returning 0 so the VSH import resolves.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SysmoduleIsLoadedInternal(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Sysmod

LIB_DEFINE(InitSysmoduleStubs_1) {
	LIB_FUNC("ynFKQ5bfGks", Sysmod::SysmoduleIsLoadedInternal);
}

} // namespace SysmoduleStubs

namespace LibcInternalStubs {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

namespace LibcStub {

// Forward to host realloc; guest memory is identity-mapped so the host
// allocator is valid (same approach as libC.cpp mspace helpers).
static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LibcReallocForward(void* ptr, size_t size) {
	PRINT_NAME();
	LOGF("\t ptr = %p size = %llu\n", ptr, static_cast<unsigned long long>(size));
	return ::Libs::GuestHeap::Reallocate(ptr, size);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LibcStrncasecmp(const char* s1, const char* s2, size_t n) {
	PRINT_NAME();
	if (s1 == s2) {
		return 0;
	}
	if (s1 == nullptr) {
		return -1;
	}
	if (s2 == nullptr) {
		return 1;
	}
	LOGF("\t s1 = \"%s\" s2 = \"%s\" n = %llu\n", s1, s2,
	     static_cast<unsigned long long>(n));
#ifdef _WIN32
	return ::_strnicmp(s1, s2, n);
#else
	return ::strncasecmp(s1, s2, n);
#endif
}

} // namespace LibcStub

LIB_DEFINE(InitLibcInternalStubs_1) {
	LIB_FUNC("Y7aJ1uydPMo", LibcStub::LibcReallocForward);
	LIB_FUNC("pXvbDfchu6k", LibcStub::LibcStrncasecmp);
}

} // namespace LibcInternalStubs

namespace UsbStorageStubs {

// ps5-vsh: NPXS40038 imports SystemService USB-storage NIDs (RegisterCallback,
// UnregisterCallback, RequestMap, RequestUnmap). NID fallback resolves by NID,
// so the lib/module name here only needs to be plausible, not an exact match.
LIB_VERSION("SystemServiceUsbStorage", 1, "SystemServiceUsbStorage", 1, 1);

namespace UsbStorage {

static uint64_t g_usb_storage_callback = 0;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UsbStorageRegisterCallback(uint64_t a1, uint64_t a2, uint64_t a3,
                                                   uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	g_usb_storage_callback = a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UsbStorageUnregisterCallback(uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t previously registered callback = 0x%016" PRIx64 "\n", g_usb_storage_callback);
	g_usb_storage_callback = 0;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UsbStorageRequestMap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UsbStorageRequestUnmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace UsbStorage

LIB_DEFINE(InitUsbStorageStubs_1) {
	LIB_FUNC("rnvDWnAiM2M", UsbStorage::UsbStorageRegisterCallback);
	LIB_FUNC("cvTnVQBmRZA", UsbStorage::UsbStorageUnregisterCallback);
	LIB_FUNC("XTV1xv3H4qw", UsbStorage::UsbStorageRequestMap);
	LIB_FUNC("s8vuHbvbtis", UsbStorage::UsbStorageRequestUnmap);
}

} // namespace UsbStorageStubs

} // namespace Libs
