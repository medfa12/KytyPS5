#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/presentation/videoOut.h"
#include "libs/libs.h"
#include "libs/composite.h"
#include "libs/mediaFramework.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#ifdef _WIN32
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif
#include <map>
#include <mutex>
#include <string>
#include <utility>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Libs {

// ps5-vsh: NPXS40038 imports 23 Composite v1.1 NIDs
// (Composite_v1|Composite_v1.1, from _ProbeLogs/npxs40038_v3.log
// `unresolved PLT import patched to stub` lines + _ProbeLogs/npxs40038_imports.txt).
// Host-backed surfaces (malloc registry + canvas table) in the VideoOut style:
// lifecycle mirrors graphics/presentation/videoOut.h VideoOutDriver +
// libs/agc.cpp Initialize (WindowInit -> VideoOutInit, host owns the backing)
// and Shutdown (VideoOutShutdown frees it) — here InitWithProcessOrder stores
// the config and Quit frees every registry block like VideoOutShutdown does.
// Canvas command setters record per-canvas state + LOGF; execution is an
// honest NO-OP until the presenter path exists.
// NOTE: dN3ooEQkn90 (SetSystemConfigWithMask) already stubbed in src/libs/libC.cpp —
// it is intentionally NOT duplicated here.

namespace Composite {

LIB_VERSION("Composite", 1, "Composite", 1, 1);

using NativeCanvasDraw = KYTY_SYSV_ABI int (*)(uint32_t*, int, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t);
static std::atomic<NativeCanvasDraw> g_nativeCanvasDraw {nullptr};
using NativeTexturePatch  = KYTY_SYSV_ABI int (*)(uint32_t*, int, uint64_t);
using NativeCanvasScratch = KYTY_SYSV_ABI uint64_t (*)();
static std::atomic<NativeTexturePatch>    g_nativeTexturePatch {nullptr};
static std::atomic<NativeCanvasScratch>   g_nativeCanvasScratch {nullptr};
void                                      SetNativeTexturePatch(uint64_t address) {
	g_nativeTexturePatch.store(reinterpret_cast<NativeTexturePatch>(address),
	                           std::memory_order_release);
}
void SetNativeCanvasScratch(uint64_t address) {
	g_nativeCanvasScratch.store(reinterpret_cast<NativeCanvasScratch>(address),
	                            std::memory_order_release);
}
void SetNativeVirtualCanvasDraw(uint64_t address) {
	g_nativeCanvasDraw.store(reinterpret_cast<NativeCanvasDraw>(address), std::memory_order_release);
}

namespace Comp {

// Audited 13.60 Composite +0x35e0/+0x3640: these are packed-id byte
// accessors, not service queries. Byte0 == 0xff is the unavailable sentinel.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MetaSlot(uint8_t* out, uint32_t id) {
	static std::atomic_uint calls {0};
	if (MediaFramework::TraceEnabled() && calls.fetch_add(1) < 12)
		Log::WriteToConsoleAndLog(fmt::sprintf("MediaCanvas: MetaSlot id=%08x\n", id));
	if (!out) return static_cast<int>(0x80d40006u);
	*out = static_cast<uint8_t>(id);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MetaFlags(uint8_t* out, uint32_t id) {
	if (!out) return static_cast<int>(0x80d40006u);
	*out = static_cast<uint8_t>(id >> 8);
	return 0;
}
static bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsVirtualCanvas(uint32_t id) {
	return (id & 0xff00u) == 0xff00u;
}
// PSM +0x1480a0 reserves exactly 32 bytes in its graphics command buffer.
// Execute at that point in GPU order, with the guest's active render target.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VirtualCanvasDraw(
    uint32_t* command, int meta, uint64_t scratch_offset, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
	if (!MediaFramework::OwnsCanvas(meta)) {
		if (auto native = g_nativeCanvasDraw.load(std::memory_order_acquire))
			return native(command, meta, scratch_offset, x, y, width, height);
		return static_cast<int>(0x80d40006u);
	}
	static std::atomic_uint calls {0};
	if (MediaFramework::TraceEnabled() && calls.fetch_add(1) < 12)
		Log::WriteToConsoleAndLog(fmt::sprintf("MediaCanvas: emit id=%08x rect=%u,%u %ux%u command=%p\n", meta, x, y, width, height, static_cast<void*>(command)));
	if (!command || !width || !height || width > 8192 || height > 8192) return static_cast<int>(0x80d40006u);
	const uint32_t packet[8] = {0xc0061000, MediaFramework::CanvasPacket,
	    static_cast<uint32_t>(meta), x, y, width, height, 0};
	std::memcpy(command, packet, sizeof(packet));
	return 0;
}

// Window backing the guest scribbles into: 1280 x 720 x 4 Bpp.
static constexpr uint32_t kCompWidth      = 1280;
static constexpr uint32_t kCompHeight     = 720;
static constexpr uint32_t kCompBpp        = 4;
static constexpr uint64_t kCompBackingSize =
    uint64_t(kCompWidth) * uint64_t(kCompHeight) * uint64_t(kCompBpp); // 3686400

struct CompSurface {
	uint64_t addr      = 0; // host address; guest-visible (unified address space)
	uint64_t size      = 0;
	uint32_t w         = 0;
	uint32_t h         = 0;
	bool     secondary = false;
};

static std::map<uint64_t, CompSurface> g_surfaces; // key: host addr
static std::mutex                      g_surfacesMutex;
static uint64_t                        g_systemAddr    = 0;
static uint64_t                        g_videoAddr     = 0;
static uint64_t                        g_lastSecondary = 0;

// Native 13.60 Composite +0x3f50..+0x3fa0 returns arena addresses/sizes
// directly. These are allocator arenas, not rendered pixel surfaces.
struct CompPool {
	void* address = nullptr;
	uint64_t size = 0;
	CompPool() = default;
	CompPool(const CompPool&) = delete;
	CompPool& operator=(const CompPool&) = delete;
	CompPool& operator=(CompPool&& other) noexcept {
		if (this != &other) {
			Reset();
			address = std::exchange(other.address, nullptr);
			size = std::exchange(other.size, 0);
		}
		return *this;
	}
	~CompPool() { Reset(); }
	void Reset() {
#ifdef _WIN32
		_aligned_free(address);
#else
		std::free(address);
#endif
		address = nullptr;
		size = 0;
	}
	bool Allocate(uint32_t bytes) {
		if (!bytes) { return true; }
		const auto aligned = (uint64_t(bytes) + 0x3fff) & ~uint64_t(0x3fff);
#ifdef _WIN32
		address = _aligned_malloc(static_cast<size_t>(aligned), 0x4000);
#else
		address = std::aligned_alloc(0x4000, static_cast<size_t>(aligned));
#endif
		if (!address) { return false; }
		std::memset(address, 0, static_cast<size_t>(aligned));
		size = bytes;
		return true;
	}
};
static CompPool g_pools[3]; // system, video, third native arena
static bool g_poolsInitialized = false;
static std::mutex g_poolLifecycleMutex;

struct CompCanvas {
	uint64_t    metaId = 0;
	std::string lastCommand;
	bool        touched = false;
	// ps5-vsh: Agc decode (libSceComposite.sprx layouts, RE spec):
	// SetAgcContext: a1 = canvas handle, a2 = Agc ctx id scalar.
	uint32_t agcCtxId      = 0;
	bool     hasAgcCtx     = false;
	// SetAgcComposite: a1 low byte = tag/canvas (>0xF rejected by FW),
	// a2 = guest ptr to 32B payload (internal split unrecovered: stored
	// verbatim + hexdumped so real titles resolve it empirically).
	uint8_t  compTag       = 0;
	uint8_t  compBlob[32]  = {};
	bool     hasComposite  = false;
	// SetPatch: a2 = u16 patch idx, a3 = guest rects ptr (layout unknown).
	uint16_t patchIdx      = 0;
	uint64_t patchPtr      = 0;
	bool     hasPatch      = false;
};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// ps5-vsh: identity-mapped guest reads must never fault (canary discipline).
// Page-walk probe; false when any page is uncommitted/unreadable.
static bool CompProbeGuest(uint64_t addr, size_t len) {
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
static bool CompProbeGuest(uint64_t addr, size_t len) {
	return addr != 0 && len != 0;
}
#endif

// PSM +0x39be1 patches an ordinary sampled T# via the native scratch arena.
// Supply host-owned media descriptors directly. Native AgcCompositor's
// processPatch type 0 validates the destination but performs no write; type 2
// would resolve this host-only metadata through its own table to a dummy image.
// The renderer resolves the tagged handle in GPU order.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI TexturePatch(uint32_t* command,
                                                                               int       meta,
                                                                               uint64_t  offset) {
	auto native = g_nativeTexturePatch.load(std::memory_order_acquire);
	if (!MediaFramework::OwnsCanvas(meta))
		return native ? native(command, meta, offset) : static_cast<int>(0x80d40006u);
	if (!command) return static_cast<int>(0x80d40006u);
	auto scratch = g_nativeCanvasScratch.load(std::memory_order_acquire);
	if (!scratch) return static_cast<int>(0x80d40006u);
	const uint64_t base = scratch();
	if (!base || offset > UINT32_MAX || base > UINT64_MAX - offset ||
	    !CompProbeGuest(base + offset, 32))
		return static_cast<int>(0x80d40006u);
	uint32_t descriptor[8] {};
	if (!MediaFramework::BuildCanvasTexture(meta, descriptor)) return static_cast<int>(0x80d40006u);
	std::memcpy(reinterpret_cast<void*>(base + offset), descriptor, sizeof(descriptor));
	const uint32_t patch[4] = {0, static_cast<uint32_t>(offset), 0, 0};
	std::memcpy(command, patch, sizeof(patch));
	static std::atomic_uint calls {0};
	if (MediaFramework::TraceEnabled() && calls.fetch_add(1) < 12)
		Log::WriteToConsoleAndLog(fmt::sprintf("MediaCanvas: texture id=%08x descriptor=%llx\n",
		                                       meta,
		                                       static_cast<unsigned long long>(base + offset)));
	return 0;
}

static std::string CompHex(const uint8_t* data, size_t len) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string          out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		out.push_back(kHex[(data[i] >> 4) & 0xf]);
		out.push_back(kHex[data[i] & 0xf]);
	}
	return out;
}

static std::map<uint64_t, CompCanvas> g_canvases; // key: canvas handle
static std::mutex                     g_canvasMutex;
static uint64_t                       g_nextCanvas   = 1;
static uint32_t                       g_processOrder = 0;

// REAL-service presenter handoff (staged slot + lazy present attempt): read
// src/graphics/presentation/videoOut.h (Open/Close/SubmitFlip/WaitVblank),
// src/libs/libVideoOut.cpp (VideoOutOpen(user,bus,index,param) -> handle;
// VideoOutSubmitFlip(handle,index,mode,arg) -> ReserveFlipRequest +
// SubmitFlipPreparation on a REGISTERED handle), src/libs/agc.cpp:63-73
// (WindowInit -> VideoOutInit(width,height,presenter) owns the driver;
// Shutdown tears it down), src/graphics/presentation/videoOut.cpp:291-294
// (DriverState() EXITs when g_video_out_driver == nullptr) + :1584-1599
// (SubmitFlip path). Safe-submit preconditions: VideoOutInit ran on the main
// thread (FlipQueue ctor requires it), a handle is open (user 0 / main bus /
// index 0), and buffers are registered with a known pixel_format, tiling 0,
// 64K-aligned footprint (ImageInfo validation EXITs otherwise).
// Blocker (resolved): EVERY VideoOut* entry (Open/SubmitFlip/Register/Close)
// calls DriverState() first, which EXITs when g_video_out_driver is null.
// videoOut.h/cpp now exports VideoOutIsInitialized() (null-check + ready
// flag set/cleared in VideoOutInit/VideoOutShutdown, no exit path), so
// PresenterAvailableSafe() below gates every VideoOut* call on it — calling
// VideoOutOpen blind headless would be DbgExit(321) death. The malloc backing
// here is 16B-aligned linear pixels, so each ready flush copies the staged
// surface into a dedicated 64K-aligned block sized for the tiled upload
// footprint (RegisterBuffers2 footprint validation would EXIT otherwise —
// every gate is pre-validated: tiler size/align + pixel-format decode +
// address alignment, fail-soft to staging on any doubt); SubmitFlip (CPU
// path) needs only the registered handle, no GPU CommandBuffer from us.
// Hence: Flush stages + attempts lazy present via TryPresentStaged(), which
// stays staging-only while PresenterAvailableSafe() is false (headless-safe),
// and logs first-frame-ready (dims+size) once so a real-window probe shows
// frames arrive. Never calls a VideoOut* entry while the flag is false;
// never EXITs.
static std::mutex       g_presentMutex;
static uint64_t         g_pendingAddr = 0; // host backing addr, 0 = slot empty
[[maybe_unused]] static uint64_t g_pendingSize = 0; // reserved for presenter poll
static uint64_t         g_pendingSeq  = 0; // increments per staged flush
static std::atomic_bool g_waitLogged {false};
static std::atomic_bool g_firstReadyLogged {false};
static int              g_voHandle = -1; // -1 = never opened; lazy VideoOut handle
static bool             g_voRegistered = false; // buffers registered on g_voHandle
static std::atomic_bool g_presentLogged {false}; // first real-submit log gate
static std::atomic_bool g_voWarned {false};      // open/register failure log gate
static void*            g_voBacking     = nullptr; // 64K-aligned upload block
static uint64_t         g_voBackingSize = 0;
static uint64_t         g_voBackOff     = 0; // flushes to skip before open/register retry

// sRGB 8-bit attribute matching RegisterBuffers2's ImageInfo expectations
// (VIDEO_OUT_FORMAT_POLICIES[0]: R8G8B8A8 sRGB, 4 Bpp — byte-compatible with
// the 1280x720x4 staged linear scribble). Tiling is always 0; the driver
// interprets the footprint as a render-target tile (videoOut.cpp:569).
static constexpr uint64_t kVoPixelFormat   = 0x8000000022000000ULL;
static constexpr uint64_t kVoAlign          = 65536;
static constexpr int      kVoFlipModeVsync = 1;
static constexpr uint64_t kVoRetryEvery    = 120; // flushes between open/register retries

// Layout mirror of VideoOutBufferAttribute2 (videoOut.cpp:99-112). The real
// struct lives in videoOut.cpp (forward-declared in videoOut.h), so the
// attribute is built in this mirror (filled via the real
// VideoOutSetBufferAttribute2 entry) and passed with reinterpret_cast. The
// static_asserts lock the mirror to the exact guest ABI offsets.
struct VoBufferAttribute2 {
	uint32_t reserved0;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint64_t option;
	uint64_t pixel_format;
	uint64_t dcc_cb_register_clear_color;
	uint32_t dcc_control;
	uint32_t pad0;
	uint64_t reserved1[3];
};
static_assert(sizeof(VoBufferAttribute2) == 80);
static_assert(offsetof(VoBufferAttribute2, tiling_mode) == 4);
static_assert(offsetof(VoBufferAttribute2, width) == 12);
static_assert(offsetof(VoBufferAttribute2, pitch_in_pixel) == 20);
static_assert(offsetof(VoBufferAttribute2, option) == 24);
static_assert(offsetof(VoBufferAttribute2, pixel_format) == 32);
static_assert(offsetof(VoBufferAttribute2, dcc_cb_register_clear_color) == 40);
static_assert(offsetof(VoBufferAttribute2, dcc_control) == 48);
static_assert(offsetof(VoBufferAttribute2, reserved1) == 56);

// Layout mirror of VideoOutBuffers (videoOut.cpp:158-162): {data, metadata,
// reserved[2]}.
struct VoBuffers {
	const void* data;
	const void* metadata;
	const void* reserved[2];
};
static_assert(sizeof(VoBuffers) == 32);

static void* VoAllocAligned(uint64_t size) {
#ifdef _WIN32
	return _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(kVoAlign));
#else
	return std::aligned_alloc(static_cast<size_t>(kVoAlign), static_cast<size_t>(size));
#endif
}

static void VoFreeAligned(void* p) {
	if (p == nullptr) {
		return;
	}
#ifdef _WIN32
	_aligned_free(p);
#else
	std::free(p);
#endif
}

// Try/catch-free, EXIT-free availability probe. Consults ONLY the ready
// flag — never a VideoOut* entry — so it is safe headless (driver null) and
// can never trip the DriverState() EXIT.
static bool PresenterAvailableSafe() noexcept {
	return VideoOut::VideoOutIsInitialized();
}

// Lazy present: called from Flush with a valid staged surface. Headless-safe:
// when PresenterAvailableSafe() is false, stages only + emits first-frame-ready
// once. When ready: VideoOutOpen(0, main-bus 0, index 0) once ->
// RegisterBuffers2(handle, 64K-aligned backing, 1280x720 sRGB attribute) once
// -> VideoOutSubmitFlip(handle, 0, VSYNC(1), seq) per frame. Any validation
// doubt or non-OK return falls back to staging (pending slot retained; the
// 16ms wait quantum in WaitEndOfRendering is kept). Quit closes the handle if
// opened (guarded) and frees the backing. Holds g_presentMutex throughout;
// nests g_surfacesMutex only for the staged-bytes copy.
static void TryPresentStaged(uint64_t addr, uint64_t size, uint64_t seq) {
	if (addr == 0 || size == 0) {
		return;
	}
	if (!PresenterAvailableSafe()) {
		if (!g_firstReadyLogged.exchange(true)) {
			LOGF("\t composite first-frame-ready: %" PRIu32 "x%" PRIu32 "x%" PRIu32
			     " size = %" PRIu64 " seq = %llu addr = 0x%016" PRIx64
			     " (staged; presenter unavailable headless, no submit attempted)\n",
			     kCompWidth, kCompHeight, kCompBpp, size,
			     static_cast<unsigned long long>(seq), addr);
		}
		return;
	}
	std::lock_guard<std::mutex> lock(g_presentMutex);
	if ((g_voHandle < 0 || !g_voRegistered) && g_voBackOff > 0) {
		--g_voBackOff;
		return;
	}
	// 1. Open once (user 0, main bus 0, index 0). A busy port (the game owns
	// it) or any other failure backs off and stays staged.
	if (g_voHandle < 0) {
		const int handle = VideoOut::VideoOutOpen(0, 0, 0, nullptr);
		if (handle <= 0) {
			g_voBackOff = kVoRetryEvery;
			if (!g_voWarned.exchange(true)) {
				LOGF("\t composite present: VideoOutOpen failed rc = %d "
				     "(staged; retrying every %llu flushes)\n",
				     handle, static_cast<unsigned long long>(kVoRetryEvery));
			}
			return;
		}
		g_voHandle = handle;
		LOGF("\t composite present: VideoOutOpen ok handle = %d (user 0, bus 0, index 0)\n",
		     g_voHandle);
	}
	// 2. Register once: pre-validate the exact ImageInfo EXIT gates, then
	// allocate the 64K-aligned upload block sized for the tiled footprint.
	if (!g_voRegistered) {
		Graphics::TileSizeAlign total {};
		Graphics::TileGetTextureTotalSize(Graphics::Prospero::BufferFormat::k8_8_8_8Srgb,
		                                   kCompWidth, kCompHeight, 1, 1,
		                                   Graphics::Prospero::TileMode::kRenderTarget, false,
		                                   total);
		Graphics::VideoOutPixelFormatInfo fmt {};
		if (total.size == 0 || total.align != kVoAlign ||
		    !Graphics::DecodeVideoOutPixelFormat(kVoPixelFormat, fmt)) {
			g_voBackOff = kVoRetryEvery;
			return; // tiler/ABI doubt: stay staged, retry later
		}
		if (g_voBacking == nullptr) {
			uint64_t need = size;
			if (need < total.size) {
				need = total.size; // presenter upload reads total.size bytes
			}
			need = (need + kVoAlign - 1) & ~(kVoAlign - 1); // aligned_alloc: size % align == 0
			void* backing = VoAllocAligned(need);
			if (backing == nullptr ||
			    (reinterpret_cast<uint64_t>(backing) & (kVoAlign - 1)) != 0) {
				VoFreeAligned(backing);
				g_voBackOff = kVoRetryEvery;
				return;
			}
			std::memset(backing, 0, static_cast<size_t>(need));
			g_voBacking     = backing;
			g_voBackingSize = need;
		}
		VoBufferAttribute2 attr {};
		VideoOut::VideoOutSetBufferAttribute2(
		    reinterpret_cast<VideoOut::VideoOutBufferAttribute2*>(&attr), kVoPixelFormat, 0,
		    kCompWidth, kCompHeight, 0, 0, 0);
		VoBuffers bufs {};
		bufs.data     = g_voBacking;
		bufs.metadata = nullptr;
		const int rc = VideoOut::VideoOutRegisterBuffers2(
		    g_voHandle, 0, 0, reinterpret_cast<const VideoOut::VideoOutBuffers*>(&bufs), 1,
		    reinterpret_cast<const VideoOut::VideoOutBufferAttribute2*>(&attr), 0, nullptr);
		if (rc != 0) {
			VoFreeAligned(g_voBacking);
			g_voBacking     = nullptr;
			g_voBackingSize = 0;
			g_voBackOff     = kVoRetryEvery;
			if (!g_voWarned.exchange(true)) {
				LOGF("\t composite present: RegisterBuffers2 failed rc = %d "
				     "(staged; backing dropped, retrying every %llu flushes)\n",
				     rc, static_cast<unsigned long long>(kVoRetryEvery));
			}
			return;
		}
		g_voRegistered = true;
		LOGF("\t composite present: RegisterBuffers2 ok handle = %d backing = 0x%016" PRIx64
		     " size = %" PRIu64 " (64K-aligned, 1280x720 sRGB fmt 0x%016" PRIx64 ")\n",
		     g_voHandle, reinterpret_cast<uint64_t>(g_voBacking), g_voBackingSize,
		     kVoPixelFormat);
	}
	// 3. Refresh the upload block from the staged surface, then submit.
	{
		std::lock_guard<std::mutex> slock(g_surfacesMutex);
		auto                        it = g_surfaces.find(addr);
		if (it == g_surfaces.end()) {
			return; // surface went away (Quit raced); stay staged
		}
		uint64_t copySize = size;
		if (copySize > it->second.size) {
			copySize = it->second.size;
		}
		if (copySize > g_voBackingSize) {
			copySize = g_voBackingSize;
		}
		std::memcpy(g_voBacking, reinterpret_cast<const void*>(addr),
		            static_cast<size_t>(copySize));
		// ps5-vsh: offscreen frame dump (KYTY_RN_DUMPFRAME=1). Writes the
		// submitted upload block as raw RGBA for offline verification.
		// One-shot (first submit only).
		{
			static bool s_dumped = false;
			if (!s_dumped) {
				s_dumped = true;
				if (std::getenv("KYTY_RN_DUMPFRAME") != nullptr) {
					const char* output = std::getenv("KYTY_RN_DUMPFRAME_PATH");
					FILE* f = std::fopen(output != nullptr && output[0] != '\0' ? output : "frame0.rgba", "wb");
					if (f != nullptr) {
						std::fwrite(g_voBacking, 1,
						            static_cast<size_t>(copySize) > 3686400
						                ? 3686400
						                : static_cast<size_t>(copySize),
						            f);
						std::fclose(f);
						LOGF("\t composite frame dumped (%llu bytes)\n",
						     static_cast<unsigned long long>(copySize));
					}
				}
			}
		}
	}
	const int rc =
	    VideoOut::VideoOutSubmitFlip(g_voHandle, 0, kVoFlipModeVsync, static_cast<int64_t>(seq));
	if (rc != 0) {
		return; // queue full / transient: stays staged, retried next flush
	}
	if (g_pendingSeq == seq) {
		g_pendingAddr = 0; // presenter owns the frame; WaitEndOfRendering returns at once
		g_pendingSize = 0;
	}
	if (!g_presentLogged.exchange(true)) {
		LOGF("\t composite presented: frame #%llu handle = %d addr = 0x%016" PRIx64
		     " size = %" PRIu64 " (VideoOutSubmitFlip path)\n",
		     static_cast<unsigned long long>(seq), g_voHandle, addr, size);
	}
}

// Locking order is always surfaces -> canvases. Helpers suffixed Locked
// require g_surfacesMutex to be held.
static uint64_t AllocSurfaceLocked(uint32_t w, uint32_t h, uint64_t size, bool secondary) {
	if (size == 0) {
		size = kCompBackingSize;
	}
	void* p = std::malloc(static_cast<size_t>(size));
	if (p == nullptr) {
		return 0;
	}
	std::memset(p, 0, static_cast<size_t>(size));
	uint64_t addr = reinterpret_cast<uint64_t>(p);
	g_surfaces.emplace(addr, CompSurface {addr, size, w, h, secondary});
	if (secondary) {
		g_lastSecondary = addr;
	}
	return addr;
}

static void FreeSurfaceLocked(uint64_t addr) {
	auto it = g_surfaces.find(addr);
	if (it == g_surfaces.end()) {
		return;
	}
	std::free(reinterpret_cast<void*>(it->first));
	g_surfaces.erase(it);
	if (g_systemAddr == addr) {
		g_systemAddr = 0;
	}
	if (g_videoAddr == addr) {
		g_videoAddr = 0;
	}
	if (g_lastSecondary == addr) {
		g_lastSecondary = 0;
	}
}

static int SetCanvasCommand(uint64_t handle, const char* name, uint64_t a2, uint64_t a3,
                            uint64_t a4) {
	std::lock_guard<std::mutex> lock(g_canvasMutex);
	CompCanvas&                 canvas = g_canvases[handle]; // insert-or-update: never crash
	canvas.lastCommand                = name;
	canvas.touched                    = true;
	LOGF("\t %s: canvas = %" PRIu64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 " (NO-OP execution, recorded; presenter comes later)\n",
	     name, handle, a2, a3, a4);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorQuit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	std::lock_guard lifecycle(g_poolLifecycleMutex);
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	// VideoOutShutdown-style teardown: free every host block, clear tables.
	size_t freed = 0;
	{
		std::lock_guard<std::mutex> lock(g_surfacesMutex);
		for (const auto& kv : g_surfaces) {
			std::free(reinterpret_cast<void*>(kv.first));
			++freed;
		}
		g_surfaces.clear();
		for (auto& pool : g_pools) { pool.Reset(); }
		g_poolsInitialized = false;
		g_systemAddr    = 0;
		g_videoAddr     = 0;
		g_lastSecondary = 0;
	}
	{
		std::lock_guard<std::mutex> lock(g_canvasMutex);
		g_canvases.clear();
		g_nextCanvas   = 1;
		g_processOrder = 0;
	}
	{
		std::lock_guard<std::mutex> lock(g_presentMutex);
		// Guarded close: only the handle WE opened, and only while the driver
		// is up — VideoOutClose is never called blind (it EXITs via
		// DriverState() with no driver). Driver Close() unregisters the
		// buffers itself. The aligned backing is always ours: free it even
		// when the driver is already gone.
		if (g_voHandle > 0) {
			if (VideoOut::VideoOutIsInitialized()) {
				(void)VideoOut::VideoOutClose(g_voHandle);
			}
			g_voHandle = -1;
		}
		VoFreeAligned(g_voBacking);
		g_voBacking     = nullptr;
		g_voBackingSize = 0;
		g_voRegistered  = false;
		g_voBackOff     = 0;
		g_pendingAddr   = 0;
		g_pendingSize   = 0;
		g_pendingSeq    = 0;
	}
	// Reset the once-flags for next init.
	g_firstReadyLogged.store(false);
	g_presentLogged.store(false);
	g_voWarned.store(false);
	LOGF("\t composite quit: freed %zu surface(s), cleared canvas table\n", freed);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorInitWithProcessOrder(
    uint32_t system_size, uint32_t video_size, uint32_t third_size, uint8_t order) {
	std::lock_guard lifecycle(g_poolLifecycleMutex);
	// Composite +0x4e15..+0x4e55: three byte sizes followed by process order.
	if (order > 3) { return static_cast<int>(0x80d40006u); }
	if (system_size > 0x20000000 || video_size > 0x20000000 || third_size > 0x20000000) {
		return static_cast<int>(0x80d40005u);
	}
	std::lock_guard<std::mutex> lock(g_surfacesMutex);
	if (g_poolsInitialized) { return static_cast<int>(0x80d40003u); }
	CompPool pending[3];
	if (!pending[0].Allocate(system_size) || !pending[1].Allocate(video_size) || !pending[2].Allocate(third_size)) {
		for (auto& pool : pending) { pool.Reset(); }
		return static_cast<int>(0x80d40005u);
	}
	for (size_t i = 0; i < 3; ++i) { g_pools[i] = std::move(pending[i]); }
	g_systemAddr = reinterpret_cast<uint64_t>(g_pools[0].address);
	g_videoAddr = reinterpret_cast<uint64_t>(g_pools[1].address);
	g_processOrder = order;
	g_poolsInitialized = true;
	LOGF("\t composite pools: system=%u video=%u third=%u order=%u\n", system_size, video_size, third_size, order);
	return 0;
}

template<size_t Index, bool Size>
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CompositorPoolValue() {
	std::lock_guard<std::mutex> lock(g_surfacesMutex);
	return Size ? g_pools[Index].size : reinterpret_cast<uint64_t>(g_pools[Index].address);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetMetaIdToCanvasHandle(uint64_t a1, uint64_t a2, uint64_t a3,
                                                              uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a3;
	(void)a4;
	std::lock_guard<std::mutex> lock(g_canvasMutex);
	g_canvases[a1].metaId                = a2; // insert-or-update: never crash
	LOGF("\t canvas %" PRIu64 ": meta id = %" PRIu64 "\n", a1, a2);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorGetCanvasHandle(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a2;
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return -1;
	}
	std::lock_guard<std::mutex> lock(g_canvasMutex);
	uint64_t                    handle = g_nextCanvas++;
	g_canvases.emplace(handle, CompCanvas {});
	*reinterpret_cast<uint64_t*>(a1) = handle;
	LOGF("\t canvas handle = %" PRIu64 "\n", handle);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetInvisibleCanvasCommand(uint64_t a1, uint64_t a2,
                                                                uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetInvisibleCanvasCommand", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetAgcCompositeCanvasCommand(uint64_t a1, uint64_t a2,
                                                                   uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	const int rc = SetCanvasCommand(a1, "SetAgcCompositeCanvasCommand", a2, a3, a4);
	// ps5-vsh: AgcComposite layout (libSceComposite.sprx RE): a1 low byte =
	// tag/canvas, a2 = guest ptr to 32B payload (internal split unrecovered).
	uint8_t blob[32] = {};
	const bool ok    = CompProbeGuest(a2, sizeof(blob)) &&
	                ([&] {
		                std::memcpy(blob, reinterpret_cast<const void*>(a2),
		                            sizeof(blob));
		                return true;
	                })();
	{
		std::lock_guard<std::mutex> lock(g_canvasMutex);
		CompCanvas&                 canvas = g_canvases[a1];
		canvas.compTag                   = static_cast<uint8_t>(a1 & 0xffu);
		canvas.hasComposite              = ok;
		if (ok) {
			std::memcpy(canvas.compBlob, blob, sizeof(canvas.compBlob));
		}
	}
	if (ok) {
		LOGF("\t AgcComposite: canvas = %" PRIu64 " tag = 0x%02x blob = %s\n", a1,
		     static_cast<unsigned>(a1 & 0xffu), CompHex(blob, sizeof(blob)).c_str());
	} else {
		LOGF("\t AgcComposite: canvas = %" PRIu64 " payload unreadable\n", a1);
	}
	return rc;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetEndOfFrameCommand(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetEndOfFrameCommand", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorFlush(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	size_t touched = 0;
	{
		std::lock_guard<std::mutex> lock(g_canvasMutex);
		for (auto& kv : g_canvases) {
			if (kv.second.touched) {
				++touched;
				kv.second.touched = false;
			}
		}
	}
	// Stage the touched canvases' backing toward presentation. Canvases carry
	// no surface addr (metaId only), so publish the latest flushed surface:
	// prefer video, then system, then secondary. Fail-soft when headless.
	uint64_t stagedAddr = 0;
	uint64_t stagedSize = 0;
	{
		std::lock_guard<std::mutex> lock(g_surfacesMutex);
		uint64_t                    pick = 0;
		if (g_videoAddr != 0 && g_surfaces.find(g_videoAddr) != g_surfaces.end()) {
			pick = g_videoAddr;
		} else if (g_systemAddr != 0 && g_surfaces.find(g_systemAddr) != g_surfaces.end()) {
			pick = g_systemAddr;
		} else if (g_lastSecondary != 0 &&
		           g_surfaces.find(g_lastSecondary) != g_surfaces.end()) {
			pick = g_lastSecondary;
		}
		if (pick != 0) {
			auto it = g_surfaces.find(pick);
			if (it != g_surfaces.end()) {
				stagedAddr = it->first;
				stagedSize = it->second.size;
			}
		}
	}
	uint64_t seq = 0;
	if (stagedAddr != 0) {
		std::lock_guard<std::mutex> lock(g_presentMutex);
		g_pendingAddr = stagedAddr;
		g_pendingSize = stagedSize;
		seq           = ++g_pendingSeq;
	}
	if (stagedAddr != 0) {
		LOGF("\t composite flush: %zu canvas(es) touched, staged frame #%llu "
		     "addr = 0x%016" PRIx64 " size = %" PRIu64 " (pending slot; presenter polls)\n",
		     touched, static_cast<unsigned long long>(seq), stagedAddr, stagedSize);
		TryPresentStaged(stagedAddr, stagedSize, seq);
	} else {
		LOGF("\t composite flush: %zu canvas(es) touched since last flush "
		     "(no backing yet; nothing staged)\n",
		     touched);
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorWaitEndOfRendering(uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	// REAL wait on the staged pending-frame slot: return at once when empty,
	// else poll until the presenter consumes it or one ~16ms vblank quantum
	// elapses. When presenting for real (VideoOutSubmitFlip path), a
	// successful submit clears the pending slot, so this returns at once;
	// headless the slot is only cleared by the quantum expiry (VideoOut*
	// waits would EXIT via DriverState(), so the 16ms quantum is retained).
	// First-use log only to avoid spam in frame loops.
	if (!g_waitLogged.exchange(true)) {
		LOGF("\t composite wait: staged-slot poll, ~16ms vblank quantum (first use)\n");
	}
	{
		std::lock_guard<std::mutex> lock(g_presentMutex);
		if (g_pendingAddr == 0) {
			return 0;
		}
	}
	const auto                              start = std::chrono::steady_clock::now();
	static constexpr std::chrono::microseconds kQuantum {16000};
	static constexpr uint32_t                   kPollUs {1000};
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(g_presentMutex);
			if (g_pendingAddr == 0) {
				break; // presenter consumed the staged frame
			}
		}
		if (std::chrono::steady_clock::now() - start >= kQuantum) {
			break; // vblank quantum elapsed; fail-soft return
		}
		Common::Thread::SleepMicro(kPollUs);
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSecondaryMemoryCreate(uint64_t a1, uint64_t a2, uint64_t a3,
                                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return -1;
	}
	std::lock_guard<std::mutex> lock(g_surfacesMutex);
	uint64_t addr = AllocSurfaceLocked(kCompWidth, kCompHeight, a2, true);
	if (addr == 0) {
		return -1;
	}
	*reinterpret_cast<uint64_t*>(a1) = addr;
	LOGF("\t secondary memory: addr = 0x%016" PRIx64 " size = %" PRIu64 "\n", addr,
	     g_surfaces[addr].size);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSecondaryMemoryRelease(uint64_t a1, uint64_t a2, uint64_t a3,
                                                             uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a2;
	(void)a3;
	(void)a4;
	if (a1 == 0) {
		return -1;
	}
	std::lock_guard<std::mutex> lock(g_surfacesMutex);
	if (g_surfaces.find(a1) == g_surfaces.end()) {
		LOGF("\t secondary release: unknown addr 0x%016" PRIx64 " (already released?)\n", a1);
		return 0;
	}
	FreeSurfaceLocked(a1);
	LOGF("\t secondary release: freed addr 0x%016" PRIx64 "\n", a1);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorReleaseLocalStall(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "ReleaseLocalStall", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetAgcContextCommand(uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	const int rc = SetCanvasCommand(a1, "SetAgcContextCommand", a2, a3, a4);
	// ps5-vsh: AgcContext layout (libSceComposite.sprx RE): a1 = canvas
	// handle, a2 = Agc ctx id scalar.
	{
		std::lock_guard<std::mutex> lock(g_canvasMutex);
		CompCanvas&                 canvas = g_canvases[a1];
		canvas.agcCtxId                 = static_cast<uint32_t>(a2 & 0xffffffffu);
		canvas.hasAgcCtx                = true;
	}
	LOGF("\t AgcContext: canvas = %" PRIu64 " ctxId = %u\n", a1,
	     static_cast<unsigned>(a2 & 0xffffffffu));
	return rc;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetLocalStallCommand(uint64_t a1, uint64_t a2, uint64_t a3,
                                                            uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetLocalStallCommand", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetPatchCommand(uint64_t a1, uint64_t a2, uint64_t a3,
                                                      uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	const int rc = SetCanvasCommand(a1, "SetPatchCommand", a2, a3, a4);
	// ps5-vsh: Patch layout (libSceComposite.sprx RE): a1 = canvas/handle,
	// a2 low 16 = patch idx, a3 = guest rects ptr (field map unrecovered).
	const bool ptrOk = CompProbeGuest(a3, 1);
	{
		std::lock_guard<std::mutex> lock(g_canvasMutex);
		CompCanvas&                 canvas = g_canvases[a1];
		canvas.patchIdx                 = static_cast<uint16_t>(a2 & 0xffffu);
		canvas.patchPtr                 = ptrOk ? a3 : 0;
		canvas.hasPatch                 = true;
	}
	LOGF("\t Patch: canvas = %" PRIu64 " idx = %u rects = 0x%016" PRIx64 "%s\n", a1,
	     static_cast<unsigned>(a2 & 0xffffu), a3, ptrOk ? "" : " (unreadable)");
	return rc;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetVirtualCanvasCommandFooter(uint64_t a1, uint64_t a2,
                                                                    uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetVirtualCanvasCommandFooter", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetVirtualCanvasAgcContextCommand(uint64_t a1, uint64_t a2,
                                                                        uint64_t a3,
                                                                        uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetVirtualCanvasAgcContextCommand", a2, a3, a4);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceCompositorSetVirtualCanvasPatchCommand(uint64_t a1, uint64_t a2,
                                                                   uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64
	     " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	return SetCanvasCommand(a1, "SetVirtualCanvasPatchCommand", a2, a3, a4);
}

} // namespace Comp

LIB_DEFINE(InitComposite_1) {
	LIB_FUNC("SpfMcnnpfDA", Comp::MetaSlot);
	LIB_FUNC("UEW01n5qkdA", Comp::MetaFlags);
	LIB_FUNC("Bt2J+lHKAbQ", Comp::IsVirtualCanvas);
	LIB_FUNC("+DMKcAbpcrQ", Comp::VirtualCanvasDraw);
	LIB_FUNC("yZcGEBwnDAM", Comp::TexturePatch);
	LIB_FUNC("y5IJdPxdBrE", Comp::SceCompositorQuit);
	LIB_FUNC("IUlpGnuoR1c", Comp::SceCompositorInitWithProcessOrder);
	LIB_FUNC("T6CVkdCDO7o", (Comp::CompositorPoolValue<0, false>));
	LIB_FUNC("N6ID0KNnzY8", (Comp::CompositorPoolValue<0, true>));
	LIB_FUNC("bxt+muwit0w", (Comp::CompositorPoolValue<1, false>));
	LIB_FUNC("FTQCTDU0b4g", (Comp::CompositorPoolValue<1, true>));
	LIB_FUNC("ReHQrFnGVxA", (Comp::CompositorPoolValue<2, false>));
	LIB_FUNC("5x5tC7zLwq4", (Comp::CompositorPoolValue<2, true>));
	LIB_FUNC("yDo24E2v2+Q", Comp::SceCompositorSetMetaIdToCanvasHandle);
	LIB_FUNC("N7PrM+lPMW0", Comp::SceCompositorGetCanvasHandle);
	LIB_FUNC("9sdvKfLR+mY", Comp::SceCompositorSetInvisibleCanvasCommand);
	LIB_FUNC("ourM2nlXyXw", Comp::SceCompositorSetAgcCompositeCanvasCommand);
	LIB_FUNC("m4cofzsxUWY", Comp::SceCompositorSetEndOfFrameCommand);
	LIB_FUNC("qZNF03+ghLI", Comp::SceCompositorFlush);
	LIB_FUNC("R00q9P3BKAc", Comp::SceCompositorWaitEndOfRendering);
	LIB_FUNC("TAlqzssvpes", Comp::SceCompositorSecondaryMemoryCreate);
	LIB_FUNC("gEME-XiMfm0", Comp::SceCompositorSecondaryMemoryRelease);
	LIB_FUNC("9zmcX1uKDGg", Comp::SceCompositorReleaseLocalStall);
	LIB_FUNC("OWU6H65+LLk", Comp::SceCompositorSetAgcContextCommand);
	LIB_FUNC("yW5gfrDFAoc", Comp::SceCompositorSetLocalStallCommand);
	LIB_FUNC("YzI2BOoDw+I", Comp::SceCompositorSetPatchCommand);
	LIB_FUNC("t8pR4ug-os0", Comp::SceCompositorSetVirtualCanvasCommandFooter);
	LIB_FUNC("aHuBE2C3JC4", Comp::SceCompositorSetVirtualCanvasAgcContextCommand);
	LIB_FUNC("n4Hk3OLW89E", Comp::SceCompositorSetVirtualCanvasPatchCommand);
}

namespace CompositeHost {

// Host-side canvas allocation for RNPS scenes (see libComposite.h). Same
// table + handle sequence the guest path uses; no guest pointers involved.
uint64_t CanvasForScene(const std::string& scene) {
	std::lock_guard<std::mutex> lock(Comp::g_canvasMutex);
	const uint64_t              handle = Comp::g_nextCanvas++;
	Comp::g_canvases.emplace(handle, Comp::CompCanvas {});
	LOGF("\t composite canvas %" PRIu64 " for scene %s\n", handle, scene.c_str());
	return handle;
}

// RNPS bring-up surface: 1280x720x4 linear RGBA owned by the host.
// Rasterizers (Hermes UI bridge, testcards) draw here; RnPresent stages +
// submits it through the normal TryPresentStaged path. Shares the guest
// VideoOut handle/queue (interleaves with guest flips; fine for bring-up).
uint64_t RnSurfaceAddr() {
	// Dedicated RN surface (never the guest's video/system backings).
	static uint64_t s_rnAddr = 0;
	std::lock_guard<std::mutex> lock(Comp::g_surfacesMutex);
	if (s_rnAddr == 0 || Comp::g_surfaces.find(s_rnAddr) == Comp::g_surfaces.end()) {
		s_rnAddr = Comp::AllocSurfaceLocked(Comp::kCompWidth, Comp::kCompHeight,
		                                     Comp::kCompBackingSize, false);
		LOGF("\t composite RN surface = 0x%016" PRIx64 "\n", s_rnAddr);
	}
	return s_rnAddr;
}

void RnPresent() {
	const uint64_t addr = RnSurfaceAddr();
	const uint64_t size = Comp::kCompBackingSize;
	if (addr == 0) {
		return;
	}
	uint64_t seq = 0;
	{
		std::lock_guard<std::mutex> lock(Comp::g_presentMutex);
		Comp::g_pendingAddr = addr;
		Comp::g_pendingSize = size;
		seq                 = ++Comp::g_pendingSeq;
	}
	Comp::TryPresentStaged(addr, size, seq);
}

} // namespace CompositeHost

} // namespace Composite

} // namespace Libs

// Registration snippet for src/libs/libs.cpp (add; do NOT edit this file's code):
//   namespace Composite {
//   LIB_DEFINE(InitComposite_1);
//   } // namespace Composite
// ... and inside Libs::InitAll(Loader::SymbolDatabase* s):
//   Composite::InitComposite_1(s);
