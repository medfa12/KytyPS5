#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/videoOutSystemEvents.h"
#include "graphics/graphicsObserve.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/singleton.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/flipQueueSelection.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/renderDoc.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fmt/format.h>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
} // namespace Libs::Graphics

namespace Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

namespace EventQueue = LibKernel::EventQueue;

constexpr int      VIDEO_OUT_EVENT_FLIP                                 = 0;
constexpr int      VIDEO_OUT_EVENT_VBLANK                               = 1;
constexpr int      VIDEO_OUT_EVENT_PRE_VBLANK_START                     = 2;
constexpr int      VIDEO_OUT_EVENT_SET_MODE                             = 8;
constexpr int      VIDEO_OUT_TRUE                                       = 1;
constexpr int      VIDEO_OUT_FALSE                                      = 0;
constexpr int      VIDEO_OUT_BUS_TYPE_MAIN                              = 0;
constexpr int      VIDEO_OUT_BUS_TYPE_OVERLAY                           = 1;
constexpr int      VIDEO_OUT_BUS_TYPE_SUB                               = 2;
// ps5-vsh: 13.60's ScePssWaitVBlank registers a system vblank on this bus
// (observed arg 0x100); the reference validates it the same way.
constexpr int      VIDEO_OUT_BUS_TYPE_MAIN_SYSTEM                       = 256;
constexpr int      VIDEO_OUT_BUS_TYPE_HMD2_MAIN                          = 32;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC                            = 1;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC_MULTI                      = 4;
constexpr int      VIDEO_OUT_FLIP_MODE_SLAVE                            = 8;
constexpr int      VIDEO_OUT_FLIP_MODE_MASTER                           = 9;
// ps5-vsh: verified against the platform VideoOut API reference:
// flip modes VSYNC=1/ASAP=2/WINDOW=3/VSYNC_MULTI=4/SLAVE=8/MASTER=9, flip
// queue depth 16/port, slots 0-15, setIndex 0-3, bufferNum 2-16, category
// 0/1, option 8 = STRICT_COLORIMETRY (sRGB->BT.709 OETF), 32 = premult alpha.
// NOTE: validation below deliberately accepts buffer_num=1 (spec wants
// 2-16): our own Composite staged presenter registers a single buffer.
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLACK                         = -2;
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLANK                         = -1;
constexpr int      VIDEO_OUT_BUFFER_NUM_MAX                             = 16;
constexpr size_t   VIDEO_OUT_FLIP_QUEUE_CAPACITY                        = 16;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX                   = 4;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT                        = 0x0000000000000001ULL;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ                       = 0x000000000000000FULL;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_59_94HZ                       = 3;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_119_88HZ                      = 13;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED     = 0;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED       = 1;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_HMD2              = 0x10;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY = 8;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED = 32;

enum class VideoOutEventKind : uintptr_t {
	Flip           = VIDEO_OUT_EVENT_FLIP,
	Vblank         = VIDEO_OUT_EVENT_VBLANK,
	PreVblankStart = VIDEO_OUT_EVENT_PRE_VBLANK_START,
	OutputMode     = VIDEO_OUT_EVENT_SET_MODE,
};

enum class FlipRequestSource { Cpu, GpuEop };

struct VideoOutEventState;

struct VideoOutEventRegistration {
	EventQueue::KernelEqueue            handle = EventQueue::KERNEL_EQUEUE_INVALID;
	std::shared_ptr<VideoOutEventState> state;
	uint64_t                            generation = 0;
	VideoOutEventKind                   kind       = VideoOutEventKind::Flip;
};

using VideoOutEventRegistrationRef = std::shared_ptr<VideoOutEventRegistration>;
using VideoOutEventQueues          = std::vector<VideoOutEventRegistrationRef>;

struct VideoOutEventState {
	Common::Mutex       mutex;
	VideoOutEventQueues flip;
	VideoOutEventQueues pre_vblank;
	VideoOutEventQueues vblank;
	VideoOutEventQueues output_mode;
};

// ps5-vsh: system (pipeline-level) vblank events are independent of process-local
// video-out handles. PSM registers them on its own equeues - observed here as
// "Equeue create: ScePsmVBlankMainThr" + Am8Hlr7tlxA(eq, pipeline=0),
// "ScePsmVBlankCheckerThr" + Ek+VR4lcJQI(eq, bus=0), and "ScePssWaitVBlank" +
// Ek+VR4lcJQI(eq, bus=0x100=MAIN_SYSTEM) - and then blocks in KernelWaitEqueue
// every frame. Those imports used to be a VideoOutDiag no-op that returned OK and
// never triggered anything, so the waiting threads were starved forever.
// Mirrors the reference (voldemort src/graphics/presentation/videoOut.cpp:117-122).
static std::array<std::shared_ptr<VideoOutEventState>, 2> g_system_vblank_events {
    std::make_shared<VideoOutEventState>(), std::make_shared<VideoOutEventState>()};

static uintptr_t SystemVblankEventId(unsigned pipeline) {
	return (uint64_t {0x70} << 48) | (uint64_t {pipeline} << 40) | (uint64_t {1} << 32);
}

struct VideoOutBufferAttribute2 {
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

// PS5 layout
struct VideoOutFlipStatus {
	uint64_t count                    = 0;
	uint64_t processTime              = 0;
	uint64_t reserved0                = 0;
	int64_t  flipArg                  = 0;
	uint64_t reserved1                = 0;
	uint64_t processTimeCounter       = 0;
	int32_t  gcQueueNum               = 0;
	int32_t  flipPendingNum           = 0;
	int32_t  currentBuffer            = 0;
	uint32_t reserved2                = 0;
	uint64_t submitProcessTimeCounter = 0;
	uint64_t reserved3[7]             = {};
};

// PS5 layout
struct VideoOutVblankStatus {
	uint64_t count              = 0;
	uint64_t processTime        = 0;
	uint64_t reserved           = 0;
	uint64_t processTimeCounter = 0;
	uint8_t  flags              = 0;
	uint8_t  phase              = 0;
	uint8_t  pad1[6]            = {};
};

struct VideoOutOutputStatus {
	uint32_t resolution   = 0;
	uint32_t dynamicRange = 0;
	uint64_t refreshRate  = 0;
	uint64_t flags        = 0;
	uint64_t reserved[3]  = {};
};
static_assert(sizeof(VideoOutOutputStatus) == 0x30); // Also used by VideoOutVrrStatus.

struct VideoOutOutputOptions {
	uint32_t internalData[16] = {};
};

struct VideoOutColorSettings {
	float    gamma       = 1.0f;
	uint32_t reserved[3] = {};
};

struct VideoOutBuffers {
	const void* data;
	const void* metadata;
	const void* reserved[2];
};

struct VideoOutBuffer {
	int      group_index      = -1;
	uint64_t data_address     = 0;
	uint64_t metadata_address = 0;

	[[nodiscard]] bool Occupied() const noexcept { return group_index >= 0; }
};

struct BufferAttributeGroup {
	VideoOutBufferAttribute2 attribute {};
	int                      category = VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED;
	bool                     occupied = false;

	[[nodiscard]] Graphics::ImageInfo ImageInfo(const VideoOutBuffer& buffer) const;
};

struct VideoOutConfig {
	Common::Mutex                       mutex;
	Common::CondVar                     vblank_cond;
	std::shared_ptr<VideoOutEventState> events      = std::make_shared<VideoOutEventState>();
	uint32_t                            width       = 0;
	uint32_t                            height      = 0;
	uint64_t                            generation  = 0;
	bool                                opened      = false;
	bool                                closing     = false;
	int                                 bus         = 0;
	VideoOutConfig*                     master      = nullptr;
	uint32_t                            slave_count = 0;
	int                                 flip_rate   = 0;
	uint64_t                            output_mode = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	float                               gamma       = 1.0f;
	VideoOutFlipStatus                  flip_status;
	VideoOutVblankStatus                pre_vblank_status;
	VideoOutVblankStatus                vblank_status;
	std::array<VideoOutBuffer, VIDEO_OUT_BUFFER_NUM_MAX>                 buffers;
	std::array<BufferAttributeGroup, VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX> groups;
};

class FlipQueue {
public:
	explicit FlipQueue(Graphics::Presenter& presenter): m_presenter(presenter) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~FlipQueue();
	KYTY_CLASS_NO_COPY(FlipQueue);

	bool Reserve(VideoOutConfig& cfg, int index, int64_t flip_arg, FlipRequestSource source,
	             uint64_t& request_id);
	void Cancel(VideoOutConfig& cfg);
	void Prepare(uint64_t request_id, Graphics::CommandBuffer& buffer);
	void Complete(uint64_t request_id);
	void WaitForSubmitSlot(VideoOutConfig& cfg);
	bool Flip(uint32_t micros);
	void GetFlipStatus(VideoOutConfig& cfg, VideoOutFlipStatus& out);
	void Wait(VideoOutConfig& cfg, int index);

private:
	enum class RequestState { Reserved, Recording, Ready, Presenting };

	struct Request {
		uint64_t                    id;
		uint64_t                    group;
		VideoOutConfig*             cfg;
		VideoOutConfig*             master;
		uint64_t                    generation;
		int                         index;
		int64_t                     flip_arg;
		uint64_t                    submit_ptc;
		FlipRequestSource           source;
		RequestState                state;
		Graphics::Presenter::Frame* frame;
		bool                        premultiplied_alpha;
	};

	Graphics::Presenter& m_presenter;
	Common::Mutex        m_mutex;
	Common::CondVar      m_submit_cond_var;
	Common::CondVar      m_submit_slot_cond_var;
	Common::CondVar      m_done_cond_var;
	std::list<Request>   m_requests;
	std::list<Request>   m_cpu_requests;
	std::list<Request>   m_cancelled_requests;
	bool                 m_processing      = false;
	uint64_t             m_next_request_id = 1;
};

struct VideoOutDriver::Impl {
public:
	static constexpr std::array VIDEO_OUT_BUSES {VIDEO_OUT_BUS_TYPE_MAIN,
	                                             VIDEO_OUT_BUS_TYPE_OVERLAY, VIDEO_OUT_BUS_TYPE_SUB,
	                                             VIDEO_OUT_BUS_TYPE_HMD2_MAIN,
	                                             VIDEO_OUT_BUS_TYPE_MAIN_SYSTEM};
	static constexpr int VIDEO_OUT_NUM_MAX = VIDEO_OUT_BUSES.size() + 1;

	Impl(uint32_t width, uint32_t height, Graphics::Presenter& presenter)
	    : m_renderer(presenter.Renderer()), m_presenter(presenter), m_flip_queue(presenter) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		Init(width, height);
		m_present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
	}
	~Impl();
	KYTY_CLASS_NO_COPY(Impl);

	int             Open(int bus_type, int index);
	bool            Close(int handle);
	VideoOutConfig* Get(int handle);
	VideoOutConfig* Get(int handle, uint64_t& generation);
	bool            IsOpened(int handle);
	VideoOutOutputSnapshot OutputSnapshot(bool detect_resolution);
	void            SetEndpointOutputMode(int bus, uint64_t mode);
	int             SetFlipMaster(int slave_handle, int master_handle);

	void                     Init(uint32_t width, uint32_t height);
	FlipQueue&               GetFlipQueue() { return m_flip_queue; }
	Graphics::RenderContext& Renderer() const noexcept { return m_renderer; }
	bool                     IsHdrSupported() const noexcept { return m_presenter.IsHdrSupported(); }

	void VblankBegin();
	void VblankEnd();
	void PresentThread(std::stop_token token);

private:
	Common::Mutex            m_mutex;
	VideoOutConfig           m_video_out_ctx[VIDEO_OUT_NUM_MAX];
	// Leaf relative to source/driver locks: they may precede this, never the reverse. Mode
	// configuration currently enters with a source mutex held, not m_mutex.
	Common::Mutex            m_output_mutex;
	VideoOutEndpointOutputState m_output;
	// ps5-vsh: scanout pulse counter handed to system vblank registrations.
	uint64_t                 m_system_vblank_count = 0;
	Graphics::RenderContext& m_renderer;
	Graphics::Presenter&     m_presenter;
	FlipQueue                m_flip_queue;
	std::jthread             m_present_thread;
};

static std::unique_ptr<VideoOutDriver> g_video_out_driver;
// Only the safe snapshot entry uses this lifetime pin. It never exposes a
// pointer that shutdown could retire after dropping the lock.
static std::mutex g_video_out_lifetime_mutex;

// Ready flag mirroring the agc.cpp:63-73 lifecycle (Initialize owns the driver,
// Shutdown tears it down). Set only inside VideoOutInit/VideoOutShutdown; read
// via VideoOutIsInitialized() without touching DriverState() (no exit path).
static std::atomic_bool g_video_out_ready {false};

static VideoOutDriver::Impl& DriverState() {
	EXIT_IF(g_video_out_driver == nullptr);
	return g_video_out_driver->State();
}

static uintptr_t VideoOutEventId(VideoOutEventKind kind) {
	return static_cast<uintptr_t>(kind);
}

static VideoOutEventQueues& VideoOutEventQueuesFor(VideoOutEventState& state,
                                                   VideoOutEventKind   kind) {
	switch (kind) {
		case VideoOutEventKind::Flip: return state.flip;
		case VideoOutEventKind::Vblank: return state.vblank;
		case VideoOutEventKind::PreVblankStart: return state.pre_vblank;
		case VideoOutEventKind::OutputMode: return state.output_mode;
	}
	EXIT("unsupported video-out event kind\n");
	return state.flip;
}

static intptr_t MakeVideoOutEventData(intptr_t current_data, void* trigger_data) {
	const uint64_t old_data = static_cast<uint64_t>(current_data);
	uint64_t       counter  = (old_data >> 12u) & 0xfu;
	if (counter != 0xfu) {
		counter++;
	}

	const uint64_t time    = LibKernel::KernelReadTsc() & 0xfffu;
	const uint64_t payload = static_cast<uint64_t>(reinterpret_cast<intptr_t>(trigger_data));

	return static_cast<intptr_t>(time | (counter << 12u) |
	                             ((payload & 0x0000ffffffffffffULL) << 16u));
}

static void ResetVideoOutEvent(EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void TriggerVideoOutEvent(EventQueue::KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    triggered_event.fflags < 0xfu ? triggered_event.fflags + 1u : triggered_event.fflags;
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
		return;
	}
	event->event     = triggered_event;
	event->triggered = true;
}

static void RemoveVideoOutEventQueue(EventQueue::KernelEqueue       eq,
                                     EventQueue::KernelEqueueEvent* event) {
	if (event == nullptr || event->filter.data == nullptr) {
		return;
	}

	auto* registration = static_cast<VideoOutEventRegistration*>(event->filter.data);
	auto  state        = registration->state;
	if (registration->handle != eq || !state) {
		return;
	}
	auto&             queues = VideoOutEventQueuesFor(*state, registration->kind);
	Common::LockGuard lock(state->mutex);
	const auto        entry =
	    std::find_if(queues.begin(), queues.end(), [registration](const auto& candidate) {
		    return candidate.get() == registration;
	    });
	if (entry != queues.end()) {
		queues.erase(entry);
	}
}

static void TriggerVideoOutEvents(VideoOutConfig& video_out, VideoOutEventKind kind,
                                  void* trigger_data) {
	VideoOutEventQueues queues;
	{
		Common::LockGuard lock(video_out.events->mutex);
		queues = VideoOutEventQueuesFor(*video_out.events, kind);
	}
	for (const auto& registration: queues) {
		if (!registration || registration->generation != video_out.generation) {
			continue;
		}
		const auto result =
		    EventQueue::KernelTriggerEvent(registration->handle, VideoOutEventId(kind),
		                                   EventQueue::KERNEL_EVFILT_VIDEO_OUT, trigger_data);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
		                     result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

static void DeleteVideoOutEvents(const VideoOutEventQueues& queues, VideoOutEventKind kind) {
	for (const auto& registration: queues) {
		if (!registration) {
			continue;
		}
		const auto result = EventQueue::KernelDeleteEvent(
		    registration->handle, VideoOutEventId(kind), EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
		                     result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

// ps5-vsh: one pulse per scanout tick for the system vblank registrations above.
// No generation filter: these belong to no video-out handle.
static void TriggerSystemVblankEvents(uint64_t count) {
	for (unsigned pipeline = 0; pipeline < g_system_vblank_events.size(); ++pipeline) {
		auto          state = g_system_vblank_events[pipeline];
		VideoOutEventQueues queues;
		{
			Common::LockGuard lock(state->mutex);
			queues = state->vblank;
		}
		if (queues.empty()) {
			continue;
		}
		// ps5-vsh positive control: prove the pulse actually reaches the
		// registrations. Bounded to the first pulse per pipeline (the trigger
		// itself is on the scanout clock, so no per-tick I/O).
		static std::atomic<bool> pulse_logged[2] {false, false};
		if (!pulse_logged[pipeline].exchange(true, std::memory_order_relaxed)) {
			LOGF("ps5-vsh: first system vblank pulse pipeline=%u count=%llu regs=%zu\n", pipeline,
			     static_cast<unsigned long long>(count), queues.size());
		}
		for (const auto& registration: queues) {
			if (!registration) {
				continue;
			}
			const auto result = EventQueue::KernelTriggerEvent(
			    registration->handle, SystemVblankEventId(pipeline),
			    EventQueue::KERNEL_EVFILT_VIDEO_OUT, reinterpret_cast<void*>(count));
			EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
			                     result != LibKernel::KERNEL_ERROR_ENOENT);
		}
	}
}

static bool IsValidSystemVblankBus(int bus) {
	return bus == VIDEO_OUT_BUS_TYPE_MAIN || bus == VIDEO_OUT_BUS_TYPE_OVERLAY ||
	       bus == VIDEO_OUT_BUS_TYPE_SUB || bus == VIDEO_OUT_BUS_TYPE_MAIN_SYSTEM;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutSysAddVblankEvent2(EventQueue::KernelEqueue eq, unsigned pipeline, void* udata) {
	if (pipeline >= g_system_vblank_events.size()) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (eq == EventQueue::KERNEL_EQUEUE_INVALID || !EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto                         state = g_system_vblank_events[pipeline];
	VideoOutEventRegistrationRef registration;
	bool                         add_queue = false;
	{
		Common::LockGuard lock(state->mutex);
		const auto        existing = std::find_if(state->vblank.begin(), state->vblank.end(),
                                                  [eq](const auto& candidate) {
			                                          return candidate->handle == eq;
		                                          });
		if (existing != state->vblank.end()) {
			registration = *existing;
		} else {
			registration = std::make_shared<VideoOutEventRegistration>(VideoOutEventRegistration {
			    .handle = eq, .state = state, .generation = 0, .kind = VideoOutEventKind::Vblank});
			state->vblank.push_back(registration);
			add_queue = true;
		}
	}

	EventQueue::KernelEqueueEvent event {};
	event.event.ident              = SystemVblankEventId(pipeline);
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.filter.data              = registration.get();
	event.filter.owner             = registration;
	event.filter.delete_event_func = RemoveVideoOutEventQueue;
	event.filter.reset_func        = ResetVideoOutEvent;
	event.filter.trigger_func      = TriggerVideoOutEvent;

	const int result = EventQueue::KernelAddEvent(eq, event);
	if (result != OK && add_queue) {
		Common::LockGuard lock(state->mutex);
		const auto        added = std::find(state->vblank.begin(), state->vblank.end(), registration);
		if (added != state->vblank.end()) {
			state->vblank.erase(added);
		}
	}
	// ps5-vsh: registration census (a handful of calls per boot, not a hot path).
	LOGF("ps5-vsh: system vblank register pipeline=%u eq=%d udata=0x%llx -> %d\n", pipeline,
	     static_cast<int>(eq), static_cast<unsigned long long>(reinterpret_cast<uint64_t>(udata)),
	     result);
	return result == LibKernel::KERNEL_ERROR_EBADF ? VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE : result;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutSysDeleteVblankEvent2(EventQueue::KernelEqueue eq, unsigned pipeline) {
	if (pipeline >= g_system_vblank_events.size()) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (eq == EventQueue::KERNEL_EQUEUE_INVALID || !EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	const int result = EventQueue::KernelDeleteEvent(eq, SystemVblankEventId(pipeline),
	                                                 EventQueue::KERNEL_EVFILT_VIDEO_OUT);
	if (result == LibKernel::KERNEL_ERROR_EBADF) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	return result == LibKernel::KERNEL_ERROR_ENOENT ? OK : result;
}

// The legacy entries take a bus, but the native library always registers pipeline 0.
__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutSysAddVblankEvent(EventQueue::KernelEqueue eq, int bus, void* udata) {
	if (!IsValidSystemVblankBus(bus)) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	return VideoOutSysAddVblankEvent2(eq, 0, udata);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutSysDeleteVblankEvent(EventQueue::KernelEqueue eq, int bus) {
	if (!IsValidSystemVblankBus(bus)) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	return VideoOutSysDeleteVblankEvent2(eq, 0);
}

static int RegisterVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind,
                                 void* udata) {
	uint64_t generation = 0;
	auto*    video_out  = DriverState().Get(handle, generation);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(video_out->mutex);
	if (!video_out->opened || video_out->closing || video_out->generation != generation) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (kind == VideoOutEventKind::OutputMode) {
		LOGF("\t eq     = 0x%016" PRIx64 "\n"
		     "\t handle = %d\n"
		     "\t udata  = 0x%016" PRIx64 "\n",
		     static_cast<uint64_t>(eq), handle, reinterpret_cast<uint64_t>(udata));
	}
	if (eq == EventQueue::KERNEL_EQUEUE_INVALID) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	if (!EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto        event_state         = video_out->events;
	auto&       queues              = VideoOutEventQueuesFor(*event_state, kind);
	const bool  initially_triggered = kind == VideoOutEventKind::OutputMode;
	void* const initial_trigger_data =
	    initially_triggered ? reinterpret_cast<void*>(video_out->output_mode) : nullptr;

	EventQueue::KernelEqueueEvent event {};
	event.triggered    = initially_triggered;
	event.event.ident  = VideoOutEventId(kind);
	event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata  = udata;
	event.event.fflags = initially_triggered ? 1u : 0u;
	event.event.data   = initially_triggered ? MakeVideoOutEventData(0, initial_trigger_data) : 0;
	event.filter.delete_event_func = RemoveVideoOutEventQueue;
	event.filter.reset_func        = ResetVideoOutEvent;
	event.filter.trigger_func      = TriggerVideoOutEvent;

	VideoOutEventRegistrationRef registration;
	bool                         add_queue = false;
	{
		Common::LockGuard event_lock(event_state->mutex);
		const auto        existing =
		    std::find_if(queues.begin(), queues.end(), [&](const auto& candidate) {
			    return candidate->handle == eq && candidate->generation == generation;
		    });
		if (existing != queues.end()) {
			registration = *existing;
		} else {
			registration = std::make_shared<VideoOutEventRegistration>(VideoOutEventRegistration {
			    .handle = eq, .state = event_state, .generation = generation, .kind = kind});
			queues.push_back(registration);
			add_queue = true;
		}
	}
	event.filter.data  = registration.get();
	event.filter.owner = registration;
	const int result   = EventQueue::KernelAddEvent(eq, event);
	if (result != OK && add_queue) {
		Common::LockGuard event_lock(event_state->mutex);
		const auto        added = std::find(queues.begin(), queues.end(), registration);
		if (added != queues.end()) {
			queues.erase(added);
		}
	}
	return result == LibKernel::KERNEL_ERROR_EBADF ? VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE : result;
}

static int DeleteVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind) {
	uint64_t generation = 0;
	auto*    video_out  = DriverState().Get(handle, generation);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	Common::LockGuard lock(video_out->mutex);
	if (!video_out->opened || video_out->closing || video_out->generation != generation) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	const int result = EventQueue::KernelDeleteEvent(eq, VideoOutEventId(kind),
	                                                 EventQueue::KERNEL_EVFILT_VIDEO_OUT);
	if (result == LibKernel::KERNEL_ERROR_EBADF) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	return result == LibKernel::KERNEL_ERROR_ENOENT ? OK : result;
}

static bool IsFlipDueLocked(const VideoOutConfig& cfg, uint64_t generation) {
	if (!cfg.opened || cfg.closing || cfg.generation != generation) {
		return false;
	}
	const int interval = cfg.flip_rate + 1;

	return interval <= 1 || (cfg.vblank_status.count % static_cast<uint64_t>(interval)) == 0;
}

static bool IsValidBufferIndex(int index) {
	return index >= VIDEO_OUT_BUFFER_INDEX_BLACK && index < VIDEO_OUT_BUFFER_NUM_MAX;
}

static bool IsSpecialBufferIndex(int index) {
	return index == VIDEO_OUT_BUFFER_INDEX_BLANK || index == VIDEO_OUT_BUFFER_INDEX_BLACK;
}

static bool IsValidFlipMode(int mode) {
	return (mode >= VIDEO_OUT_FLIP_MODE_VSYNC && mode <= VIDEO_OUT_FLIP_MODE_VSYNC_MULTI) ||
	       mode == VIDEO_OUT_FLIP_MODE_SLAVE || mode == VIDEO_OUT_FLIP_MODE_MASTER;
}

// Set once the headset port shows eye images. The window then shows them instead of the main port,
// so the picture is visible without a headset.
static std::atomic<bool> g_headset_mirror {false};

static int ReserveFlipRequest(VideoOutDriver::Impl& driver, int handle, int index, int flip_mode,
                              int64_t flip_arg, FlipRequestSource source, uint64_t& request_id) {
	auto* video_out = driver.Get(handle);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!IsValidFlipMode(flip_mode)) {
		return VIDEO_OUT_ERROR_INVALID_FLIP_MODE;
	}
	if (!IsValidBufferIndex(index)) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	Common::LockGuard lock(video_out->mutex);
	if ((video_out->master != nullptr) != (flip_mode == VIDEO_OUT_FLIP_MODE_SLAVE) ||
	    (video_out->slave_count != 0) != (flip_mode == VIDEO_OUT_FLIP_MODE_MASTER)) {
		return VIDEO_OUT_ERROR_INVALID_FLIP_MODE;
	}
	// The headset port can flip before its reprojection buffers are registered; those show blank.
	if (video_out->closing ||
	    (!IsSpecialBufferIndex(index) && !video_out->buffers[index].Occupied() &&
	     video_out->bus != VIDEO_OUT_BUS_TYPE_HMD2_MAIN)) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!driver.GetFlipQueue().Reserve(*video_out, index, flip_arg, source, request_id)) {
		return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}
	return OK;
}

Graphics::ImageInfo BufferAttributeGroup::ImageInfo(const VideoOutBuffer& buffer) const {
	const auto compression = Graphics::ClassifyVideoOutCompression(
	    category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED, buffer.metadata_address,
	    attribute.dcc_control, attribute.dcc_cb_register_clear_color);
	if (attribute.reserved0 != 0 || attribute.aspect_ratio != 0 || attribute.width == 0 ||
	    attribute.height == 0 || attribute.width > 16384 || attribute.height > 16384 ||
	    attribute.pitch_in_pixel != 0 ||
	    (attribute.option & ~(VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY |
	                          VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED)) != 0 ||
	    (attribute.tiling_mode != 0 && attribute.tiling_mode != 4) || attribute.pad0 != 0 ||
	    attribute.reserved1[0] != 0 || attribute.reserved1[1] != 0 ||
	    attribute.reserved1[2] != 0 || buffer.data_address == 0) {
		// ps5-vsh TEMP triage (AgcCompositor 4K buffers): name the failing checks.
		LOGF("ps5-vsh: surface-attr triage reserved0=%llu aspect=%u wh=%ux%u pitch=%u opt=0x%llx "
		     "tiling=%u pad0=%u r1=%u,%u,%u data=0x%llx compr=%d dcc=0x%x meta=0x%llx clear=0x%llx pixfmt=0x%016llx\n",
		     static_cast<unsigned long long>(attribute.reserved0), attribute.aspect_ratio,
		     attribute.width, attribute.height, attribute.pitch_in_pixel,
		     static_cast<unsigned long long>(attribute.option), attribute.tiling_mode,
		     attribute.pad0, attribute.reserved1[0], attribute.reserved1[1],
		     attribute.reserved1[2],
		     static_cast<unsigned long long>(buffer.data_address),
		     static_cast<int>(compression), attribute.dcc_control,
		     static_cast<unsigned long long>(buffer.metadata_address),
		     static_cast<unsigned long long>(attribute.dcc_cb_register_clear_color),
		     attribute.pixel_format);
		EXIT("unsupported or invalid video-out surface attributes\n");
	}
	Graphics::VideoOutPixelFormatInfo pixel_format {};
	if (!Graphics::DecodeVideoOutPixelFormat(attribute.pixel_format, pixel_format)) {
		EXIT("unsupported video-out pixel format: 0x%016" PRIx64 "\n", attribute.pixel_format);
	}
	const auto          tile_mode = Graphics::Prospero::TileMode::kRenderTarget;
	Graphics::ImageInfo info {};
	info.data            = {buffer.data_address, 0};
	info.pixel_format    = pixel_format.format;
	info.guest_format    = pixel_format.guest_format;
	info.type            = Graphics::Prospero::ImageType::kColor2D;
	info.extent          = {attribute.width, attribute.height, 1};
	info.resources       = {1, 1};
	info.bytes_per_block = pixel_format.bytes_per_element;
	info.samples         = 1;
	info.tile_mode       = tile_mode;
	info.bgra16          = pixel_format.bgra16;
	info.UpdateSize();
	if (info.data.size == 0 || info.tiling.block.block_size != 65536 ||
	    (buffer.data_address & 65535u) != 0) {
		EXIT("invalid video-out surface footprint or alignment\n");
	}
	if (compression != Graphics::VideoOutCompression::Uncompressed) {
		Graphics::TileSizeAlign dcc_size {};
		if (!Graphics::TileGetDccSize(attribute.width, attribute.height, 1,
		                              pixel_format.bytes_per_element, 1, tile_mode, dcc_size)) {
			EXIT("invalid video-out DCC footprint\n");
		}
		info.metadata.range       = {buffer.metadata_address, dcc_size.size};
		info.metadata.kind        = Graphics::ImageMetadataKind::Dcc;
		info.metadata.control     = attribute.dcc_control;
		info.metadata.compression = compression;
	}
	Graphics::ImageOps::Validate(info);
	if (!Graphics::IsSupportedVideoOutFormat(info)) {
		EXIT("unsupported normalized video-out format\n");
	}
	return info;
}

VideoOutDriver::VideoOutDriver(uint32_t width, uint32_t height, Graphics::Presenter& presenter)
    : m_impl(std::make_unique<Impl>(width, height, presenter)) {}

VideoOutDriver::~VideoOutDriver() = default;

VideoOutDriver::Impl& VideoOutDriver::State() noexcept {
	return *m_impl;
}

VideoOutDriver& VideoOutInit(uint32_t width, uint32_t height, Graphics::Presenter& presenter) {
	std::lock_guard lifetime_lock(g_video_out_lifetime_mutex);
	EXIT_IF(g_video_out_driver != nullptr);
	g_video_out_driver = std::make_unique<VideoOutDriver>(width, height, presenter);
	g_video_out_ready.store(true, std::memory_order_release);
	return *g_video_out_driver;
}

void VideoOutShutdown() {
	// Clear first so any concurrent VideoOutIsInitialized() probe fails safe
	// while teardown is in flight.
	std::unique_ptr<VideoOutDriver> retired;
	{
		std::lock_guard lifetime_lock(g_video_out_lifetime_mutex);
		g_video_out_ready.store(false, std::memory_order_release);
		retired = std::move(g_video_out_driver);
	}
	// No snapshot/lifetime lock across joins, renderer teardown or event work.
	retired.reset();
	VideoOutResetSystemOutputEvents();
	// ps5-vsh: the system vblank registrations outlive the driver, so drop them
	// here (reference videoOut.cpp:748-762).
	for (unsigned pipeline = 0; pipeline < g_system_vblank_events.size(); ++pipeline) {
		auto          state = g_system_vblank_events[pipeline];
		VideoOutEventQueues queues;
		{
			Common::LockGuard lock(state->mutex);
			queues = state->vblank;
		}
		for (const auto& registration: queues) {
			if (!registration) {
				continue;
			}
			(void)EventQueue::KernelDeleteEvent(registration->handle, SystemVblankEventId(pipeline),
			                                    EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		}
	}
}

bool VideoOutIsInitialized() noexcept {
	return g_video_out_ready.load(std::memory_order_acquire);
}

VideoOutOutputSnapshot VideoOutGetOutputSnapshot() {
	if (!g_video_out_ready.load(std::memory_order_acquire)) return {};
	Common::HostLockCriticalGuard critical;
	int32_t attribute3 = 0;
	Loader::SystemContentParamSfoGetInt("ATTRIBUTE3", &attribute3);
	std::lock_guard lifetime_lock(g_video_out_lifetime_mutex);
	if (!g_video_out_driver || !g_video_out_ready.load(std::memory_order_acquire)) return {};
	return g_video_out_driver->State().OutputSnapshot((attribute3 & 4) != 0);
}

VideoOutDriver::Impl::~Impl() {
	if (m_present_thread.joinable()) {
		m_present_thread.request_stop();
		m_present_thread.join();
	}
	for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
		if (m_video_out_ctx[handle].master != nullptr) {
			(void)Close(handle);
		}
	}
	for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
		(void)Close(handle);
	}
}

void VideoOutDriver::Impl::Init(uint32_t width, uint32_t height) {
	m_output = {width, height, VIDEO_OUT_OUTPUT_MODE_DEFAULT};
	for (auto& ctx: m_video_out_ctx) {
		ctx.width  = width;
		ctx.height = height;
	}
}

// Native AGC opens (user=255, bus=256, index=0). MAIN_SYSTEM owns a separate
// port, queue and retained scanout plane alongside application MAIN.
int VideoOutDriver::Impl::Open(int bus_type, int index) {
	const auto bus = std::find(VIDEO_OUT_BUSES.begin(), VIDEO_OUT_BUSES.end(), bus_type);
	if (bus == VIDEO_OUT_BUSES.end()) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	EXIT_NOT_IMPLEMENTED(index != 0);
	Common::LockGuard lock(m_mutex);

	const int handle = static_cast<int>(bus - VIDEO_OUT_BUSES.begin()) + 1;
	if (m_video_out_ctx[handle].opened) {
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}
	auto&             config = m_video_out_ctx[handle];
	Common::LockGuard config_lock(config.mutex);

	{
		Common::LockGuard event_lock(config.events->mutex);
		EXIT_IF(!config.events->flip.empty());
		EXIT_IF(!config.events->pre_vblank.empty());
		EXIT_IF(!config.events->vblank.empty());
		EXIT_IF(!config.events->output_mode.empty());
	}
	EXIT_IF(config.flip_rate != 0);
	for (const auto& buffer: config.buffers) {
		EXIT_IF(buffer.Occupied());
	}
	for (const auto& group: config.groups) {
		EXIT_IF(group.occupied);
	}

	config.closing = false;
	config.opened  = true;
	config.bus     = bus_type;
	if (++config.generation == 0) {
		EXIT("video-out port generation wrapped\n");
	}
	config.output_mode               = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	config.flip_status               = VideoOutFlipStatus();
	config.flip_status.flipArg       = -1;
	config.flip_status.currentBuffer = -1;
	config.flip_status.count         = 0;
	config.pre_vblank_status         = VideoOutVblankStatus();
	config.vblank_status             = VideoOutVblankStatus();

	return handle;
}

bool VideoOutDriver::Impl::Close(int handle) {
	Common::LockGuard lock(m_mutex);

	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return false;
	}

	auto&               config = m_video_out_ctx[handle];
	VideoOutEventQueues flip_events;
	VideoOutEventQueues pre_vblank_events;
	VideoOutEventQueues vblank_events;
	VideoOutEventQueues output_mode_events;
	VideoOutConfig* master = nullptr;
	{
		Common::LockGuard config_lock(config.mutex);
		if (config.closing) {
			return false;
		}
		EXIT_IF(config.slave_count != 0);
		config.opened  = false;
		config.closing = true;
		if (++config.generation == 0) {
			EXIT("video-out port generation wrapped\n");
		}
		{
			Common::LockGuard event_lock(config.events->mutex);
			flip_events        = std::move(config.events->flip);
			pre_vblank_events  = std::move(config.events->pre_vblank);
			vblank_events      = std::move(config.events->vblank);
			output_mode_events = std::move(config.events->output_mode);
		}
		config.flip_rate   = 0;
		master             = config.master;
		config.master      = nullptr;

		for (const auto& buffer: config.buffers) {
			if (buffer.Occupied() &&
			    (buffer.group_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
			     buffer.data_address == 0 || !config.groups[buffer.group_index].occupied)) {
				EXIT("inconsistent registered video-out buffer state\n");
			}
		}
		for (auto& buffer: config.buffers) {
			buffer = VideoOutBuffer {};
		}
		for (auto& group: config.groups) {
			group = BufferAttributeGroup {};
		}
		config.vblank_cond.SignalAll();
	}
	if (master != nullptr) {
		Common::LockGuard master_lock(master->mutex);
		master->slave_count--;
	}
	m_flip_queue.Cancel(config);
	if (config.bus == VIDEO_OUT_BUS_TYPE_HMD2_MAIN) {
		g_headset_mirror = false;
	}
	m_presenter.ClearLayer(config.bus);
	DeleteVideoOutEvents(flip_events, VideoOutEventKind::Flip);
	DeleteVideoOutEvents(pre_vblank_events, VideoOutEventKind::PreVblankStart);
	DeleteVideoOutEvents(vblank_events, VideoOutEventKind::Vblank);
	DeleteVideoOutEvents(output_mode_events, VideoOutEventKind::OutputMode);
	return true;
}

// Legacy handle-zero compatibility for port APIs. Physical-head event APIs
// use independent output state and must never resolve through this alias.
VideoOutConfig* VideoOutDriver::Impl::Get(int handle) {
	Common::LockGuard lock(m_mutex);
	if (handle == 0) {
		static std::atomic<bool> s_head0 {false};
		bool expected = false;
		if (s_head0.compare_exchange_strong(expected, true)) {
			LOGF("ps5-vsh: video-out head 0 -> main alias\n");
		}
		handle = 1;
	}
	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return nullptr;
	}

	return m_video_out_ctx + handle;
}

VideoOutConfig* VideoOutDriver::Impl::Get(int handle, uint64_t& generation) {
	Common::LockGuard lock(m_mutex);
	if (handle == 0) {
		handle = 1;
	}
	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return nullptr;
	}

	auto*             config = m_video_out_ctx + handle;
	Common::LockGuard config_lock(config->mutex);
	if (!config->opened || config->closing) {
		return nullptr;
	}
	generation = config->generation;
	return config;
}

bool VideoOutDriver::Impl::IsOpened(int handle) {
	Common::LockGuard lock(m_mutex);

	return handle > 0 && handle < VIDEO_OUT_NUM_MAX && m_video_out_ctx[handle].opened;
}

VideoOutOutputSnapshot VideoOutDriver::Impl::OutputSnapshot(bool detect_resolution) {
	Common::LockGuard driver_lock(m_mutex);
	// These are the ordinary application's MAIN and AGC's MAIN_SYSTEM,
	// respectively, not a handle-zero compatibility alias or HMD mirror.
	constexpr auto main_index = 1;
	constexpr auto system_index = VIDEO_OUT_NUM_MAX - 1;
	auto& main = m_video_out_ctx[main_index];
	auto& system = m_video_out_ctx[system_index];
	Common::LockGuard main_lock(main.mutex);
	Common::LockGuard system_lock(system.mutex);
	Common::LockGuard output_lock(m_output_mutex);
	const VideoOutPortOutputState ports[] = {
	    {main.opened && !main.closing, main.bus},
	    {system.opened && !system.closing, system.bus}};
	// GetOutputSnapshot copies a tiny cached value under its own lock, never
	// the present_mutex or Vulkan queue lock. Source/endpoint locks remain held
	// until that copy completes, giving a joint source/endpoint/presenter observation.
	return ComposeVideoOutOutputSnapshot(true, m_presenter.GetOutputSnapshot(), m_output, ports,
	                                     std::size(ports), detect_resolution,
	                                     Config::GetVblankFrequency());
}

void VideoOutDriver::Impl::SetEndpointOutputMode(int bus, uint64_t mode) {
	// Do not acquire m_mutex or any source mutex here: ConfigureOutput holds
	// its source lock, while OutputSnapshot orders driver -> sources -> output.
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_output_mutex);
	m_output.SetOutputMode(bus, mode);
}

int VideoOutDriver::Impl::SetFlipMaster(int slave_handle, int master_handle) {
	Common::LockGuard lock(m_mutex);
	if (slave_handle <= 0 || slave_handle >= VIDEO_OUT_NUM_MAX ||
	    master_handle <= 0 || master_handle >= VIDEO_OUT_NUM_MAX ||
	    !m_video_out_ctx[slave_handle].opened || !m_video_out_ctx[master_handle].opened) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto& slave  = m_video_out_ctx[slave_handle];
	auto& master = m_video_out_ctx[master_handle];
	if (&slave == &master) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
	}
	Common::LockGuard first_lock(m_video_out_ctx[std::min(slave_handle, master_handle)].mutex);
	Common::LockGuard second_lock(m_video_out_ctx[std::max(slave_handle, master_handle)].mutex);
	if (slave.master != nullptr || slave.slave_count != 0 || master.master != nullptr) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
	}
	if (slave.flip_status.flipPendingNum != 0 || master.flip_status.flipPendingNum != 0) {
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}
	slave.master = &master;
	master.slave_count++;
	return OK;
}

void VideoOutDriver::Impl::VblankBegin() {
	Common::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.pre_vblank_status.count++;
			ctx.pre_vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.pre_vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.pre_vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEvents(ctx, VideoOutEventKind::PreVblankStart,
			                      reinterpret_cast<void*>(ctx.pre_vblank_status.count));
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutDriver::Impl::VblankEnd() {
	Common::LockGuard lock(m_mutex);
	// ps5-vsh: one pulse per scanout clock, regardless of the number of client
	// handles (reference videoOut.cpp:973-976).
	TriggerSystemVblankEvents(++m_system_vblank_count);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.vblank_status.count++;
			ctx.vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEvents(ctx, VideoOutEventKind::Vblank,
			                      reinterpret_cast<void*>(ctx.vblank_status.count));
			ctx.vblank_cond.SignalAll();
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutDriver::Impl::PresentThread(std::stop_token token) {	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	EXIT_IF(frequency == 0);

	int64_t total_wait = 0;
	while (!token.stop_requested()) {
		const auto sleep_begin = Common::Timer::QueryPerformanceCounter();
		if (total_wait > 0) {
			const auto remaining_us =
			    (static_cast<uint64_t>(total_wait) * 1000000u + frequency - 1) / frequency;
			Common::Thread::SleepMicro(static_cast<uint32_t>(
			    std::clamp<uint64_t>(remaining_us, 1, std::numeric_limits<uint32_t>::max())));
		}
		if (token.stop_requested()) {
			break;
		}
		const auto frame_begin = Common::Timer::QueryPerformanceCounter();
		total_wait -= static_cast<int64_t>(frame_begin - sleep_begin);

		const auto refresh = std::max(Config::GetVblankFrequency(), 1u);
		const auto period  = std::max(frequency / refresh, uint64_t {1});

		if (m_presenter.IsGuestPaused()) {
			(void)m_presenter.PresentLastFrame();
			const auto frame_end = Common::Timer::QueryPerformanceCounter();
			total_wait +=
			    static_cast<int64_t>(period) - static_cast<int64_t>(frame_end - frame_begin);
			continue;
		}

		VblankBegin();
		bool presented = m_flip_queue.Flip(0);
		if (!presented && m_presenter.NeedsSystemOverlayRefresh()) {
			if (m_presenter.PresentLastFrame()) {
				presented = true;
			} else {
				m_presenter.PresentIdleFrame();
				presented = true;
				// ps5-vsh: blank-vs-content census.
				static std::atomic_uint64_t s_blank1 {0};
				if ((++s_blank1 % 30) == 0) {
					LOGF("ps5-vsh: present blank(no-overlay-refresh) x%llu\n",
					     (unsigned long long)s_blank1.load());
				}
			}
		}
		if (!presented && total_wait < 0) {
			bool     any_open = false;
			{
				Common::LockGuard lock(m_mutex);
				for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
					any_open |= m_video_out_ctx[handle].opened;
				}
			}
			if (!any_open) {
				m_presenter.PresentIdleFrame();
				// ps5-vsh: blank-vs-content census.
				static std::atomic_uint64_t s_blank2 {0};
				if ((++s_blank2 % 30) == 0) {
					LOGF("ps5-vsh: present blank(no-open-output) x%llu\n",
					     (unsigned long long)s_blank2.load());
				}
			}
		}
		VblankEnd();

		const auto frame_end = Common::Timer::QueryPerformanceCounter();
		total_wait += static_cast<int64_t>(period) - static_cast<int64_t>(frame_end - frame_begin);
		if (total_wait < 0) {
			Common::DebugCounters::Add(Common::DebugCounters::Counter::LateVblanks);
		}
	}
}

bool FlipQueue::Reserve(VideoOutConfig& cfg, int index, int64_t flip_arg, FlipRequestSource source,
                        uint64_t& request_id) {
	Common::LockGuard lock(m_mutex);

	// A newer slave request replaces the one not yet captured by a master flip.
	if (cfg.master != nullptr) {
		for (auto* queue: {&m_requests, &m_cpu_requests}) {
			for (auto it = queue->begin(); it != queue->end();) {
				if (it->cfg != &cfg || it->group != 0) {
					++it;
					continue;
				}
				cfg.flip_status.flipPendingNum--;
				cfg.flip_status.gcQueueNum -= it->source == FlipRequestSource::GpuEop;
				if (it->state == RequestState::Ready) {
					m_presenter.Discard(*it->frame);
					it = queue->erase(it);
				} else {
					auto cancelled = it++;
					m_cancelled_requests.splice(m_cancelled_requests.end(), *queue, cancelled);
				}
			}
		}
		m_done_cond_var.SignalAll();
	}
	if (cfg.flip_status.flipPendingNum >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::FlipsRejected);
		return false;
	}
	auto& pending = source == FlipRequestSource::GpuEop ? m_requests : m_cpu_requests;

	Request r {};
	r.id         = m_next_request_id++;
	r.group      = cfg.master == nullptr ? r.id : 0;
	r.cfg        = &cfg;
	r.master     = cfg.master;
	r.generation = cfg.generation;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.submit_ptc = LibKernel::KernelGetProcessTimeCounter();
	r.source     = source;
	r.state      = RequestState::Reserved;
	if (cfg.slave_count != 0) {
		for (auto* queue: {&m_requests, &m_cpu_requests}) {
			for (auto& slave: *queue) {
				if (slave.master == &cfg && slave.group == 0) {
					slave.group = r.id;
				}
			}
		}
	}

	pending.push_back(r);
	request_id = r.id;
	Common::DebugCounters::Add(Common::DebugCounters::Counter::FlipsRequested);

	cfg.flip_status.flipPendingNum++;
	cfg.flip_status.submitProcessTimeCounter = r.submit_ptc;
	if (source == FlipRequestSource::GpuEop) {
		cfg.flip_status.gcQueueNum++;
	}

	return true;
}

FlipQueue::~FlipQueue() {
	for (auto* queue: {&m_requests, &m_cpu_requests, &m_cancelled_requests}) {
		for (auto& request: *queue) {
			if (request.frame != nullptr) {
				m_presenter.Discard(*request.frame);
			}
		}
	}
}

void FlipQueue::Cancel(VideoOutConfig& cfg) {
	std::vector<Graphics::Presenter::Frame*> frames;
	m_mutex.Lock();
	while (m_processing && std::any_of(m_requests.begin(), m_requests.end(), [&cfg](const auto& r) {
		       return r.cfg == &cfg && r.state == RequestState::Presenting;
	       })) {
		m_done_cond_var.Wait(&m_mutex);
	}
	for (auto* queue: {&m_requests, &m_cpu_requests}) {
		for (auto it = queue->begin(); it != queue->end();) {
			if (it->cfg != &cfg) {
				++it;
				continue;
			}
			if (it->state == RequestState::Reserved || it->state == RequestState::Recording) {
				auto cancelled = it++;
				m_cancelled_requests.splice(m_cancelled_requests.end(), *queue, cancelled);
				continue;
			}
			if (it->state == RequestState::Presenting) {
				EXIT("video-out cancellation retained a presenting request\n");
			}
			if (it->frame != nullptr) {
				frames.push_back(it->frame);
			}
			it = queue->erase(it);
		}
	}
	m_done_cond_var.SignalAll();
	m_submit_slot_cond_var.SignalAll();
	m_submit_cond_var.SignalAll();
	m_mutex.Unlock();
	for (auto* frame: frames) {
		m_presenter.Discard(*frame);
	}
	Common::LockGuard lock(cfg.mutex);
	cfg.flip_status.flipPendingNum = 0;
	cfg.flip_status.gcQueueNum     = 0;
}

void FlipQueue::Prepare(uint64_t request_id, Graphics::CommandBuffer& buffer) {
	VideoOutConfig* cfg        = nullptr;
	uint64_t        generation = 0;
	int             index      = 0;
	{
		Common::LockGuard lock(m_mutex);
		auto request = std::find_if(m_requests.begin(), m_requests.end(),
		                            [request_id](const auto& r) { return r.id == request_id; });
		if (request == m_requests.end()) {
			auto pending = std::find_if(m_cpu_requests.begin(), m_cpu_requests.end(),
			                            [request_id](const auto& r) { return r.id == request_id; });
			if (pending == m_cpu_requests.end()) {
				auto cancelled =
				    std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
				                 [request_id](const auto& r) { return r.id == request_id; });
				if (cancelled == m_cancelled_requests.end() ||
				    cancelled->state != RequestState::Reserved) {
					EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
				}
				cancelled->state = RequestState::Recording;
				return;
			}
			request = m_requests.insert(m_requests.end(), *pending);
			m_cpu_requests.erase(pending);
		}
		if (request->state != RequestState::Reserved) {
			EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
		}
		request->state = RequestState::Recording;
		cfg            = request->cfg;
		generation     = request->generation;
		index          = request->index;
	}

	bool                special = IsSpecialBufferIndex(index);
	Graphics::ImageInfo source_info;
	uint32_t            width   = 0;
	uint32_t            height  = 0;
	bool                current = false;
	bool                premultiplied_alpha = false;
	bool                hdr                 = false;
	bool                headset             = false;
	{
		Common::LockGuard lock(cfg->mutex);
		current = cfg->opened && !cfg->closing && cfg->generation == generation;
		headset = cfg->bus == VIDEO_OUT_BUS_TYPE_HMD2_MAIN;
		special = special || (headset && !cfg->buffers[index].Occupied());
		if (current && special) {
			width  = cfg->width;
			height = cfg->height;
		} else if (current) {
			const auto& surface = cfg->buffers[index];
			if (!surface.Occupied()) {
				EXIT("cannot prepare flip from an unregistered surface, id=%" PRIu64
				     " index=%d\n",
				     request_id, index);
			}
			if (surface.group_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
			    !cfg->groups[surface.group_index].occupied) {
				EXIT("video-out surface references an unavailable attribute group, id=%" PRIu64
				     " index=%d group=%d\n",
				     request_id, index, surface.group_index);
			}
			const auto& group   = cfg->groups[surface.group_index];
			source_info         = group.ImageInfo(surface);
			hdr                 = Graphics::IsHdrVideoOutPixelFormat(group.attribute.pixel_format);
			premultiplied_alpha = (group.attribute.option &
			                       VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED) != 0;
		}
	}
	if (!current) {
		Common::LockGuard lock(m_mutex);
		const auto        request =
		    std::find_if(m_requests.begin(), m_requests.end(),
		                 [request_id](const auto& r) { return r.id == request_id; });
		if (request != m_requests.end()) {
			m_cancelled_requests.splice(m_cancelled_requests.end(), m_requests, request);
		}
		return;
	}
	Graphics::Presenter::Frame* frame = nullptr;
	if (special) {
		frame = &m_presenter.PrepareBlankFrame(width, height, index == VIDEO_OUT_BUFFER_INDEX_BLACK,
		                                       &buffer);
	} else {
		frame = &m_presenter.PrepareFrame(buffer, source_info, hdr, headset);
		if (headset) {
			g_headset_mirror = true;
		}
	}
	if (Graphics::Observe::Enabled()) {
		Graphics::Observe::Record(Graphics::Observe::Event::Prepare,
		                         {request_id, reinterpret_cast<uint64_t>(frame),
		                          static_cast<uint64_t>(index), static_cast<uint64_t>(special),
		                          source_info.data.address, source_info.data.size, width, height});
	}

	Common::LockGuard lock(m_mutex);
	Request*          prepared = nullptr;
	if (const auto request =
	        std::find_if(m_requests.begin(), m_requests.end(),
	                     [request_id](const auto& r) { return r.id == request_id; });
	    request != m_requests.end()) {
		prepared = &*request;
	} else if (const auto cancelled =
	               std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	           cancelled != m_cancelled_requests.end()) {
		prepared = &*cancelled;
	}
	if (prepared == nullptr || prepared->state != RequestState::Recording ||
	    prepared->frame != nullptr) {
		EXIT("video-out request changed while recording, id=%" PRIu64 "\n", request_id);
	}
	prepared->frame = frame;
	prepared->premultiplied_alpha = premultiplied_alpha;
}

void FlipQueue::Complete(uint64_t request_id) {
	m_mutex.Lock();
	auto request = std::find_if(m_requests.begin(), m_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	if (request != m_requests.end()) {
		if (request->state != RequestState::Recording || request->frame == nullptr) {
			m_mutex.Unlock();
			EXIT("completed GPU flip has no prepared recording, id=%" PRIu64 "\n", request_id);
		}
		request->state = RequestState::Ready;
		m_submit_cond_var.Signal();
		m_mutex.Unlock();
		return;
	}
	auto cancelled = std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
	                              [request_id](const auto& r) { return r.id == request_id; });
	if (cancelled == m_cancelled_requests.end() || cancelled->state != RequestState::Recording) {
		m_mutex.Unlock();
		EXIT("completed GPU flip has no prepared recording, id=%" PRIu64 "\n", request_id);
	}
	auto* frame = cancelled->frame;
	m_cancelled_requests.erase(cancelled);
	m_done_cond_var.SignalAll();
	m_mutex.Unlock();
	if (frame != nullptr) {
		m_presenter.Discard(*frame);
	}
}

void FlipQueue::WaitForSubmitSlot(VideoOutConfig& cfg) {
	Common::LockGuard lock(m_mutex);
	auto belongs_to_port = [&cfg](const auto& request) { return request.cfg == &cfg; };
	while (std::count_if(m_requests.begin(), m_requests.end(), belongs_to_port) +
	           std::count_if(m_cpu_requests.begin(), m_cpu_requests.end(), belongs_to_port) >=
	       VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		if (m_requests.empty()) {
			EXIT("video-out queue is saturated by CPU flips queued behind the current EOP\n");
		}
		m_submit_slot_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::Wait(VideoOutConfig& cfg, int index) {
	Common::LockGuard lock(m_mutex);

	auto has_request = [this, &cfg, index] {
		auto matches = [&cfg, index](const auto& r) { return r.cfg == &cfg && r.index == index; };
		return std::any_of(m_requests.begin(), m_requests.end(), matches) ||
		       std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(), matches);
	};
	while (has_request()) {
		m_done_cond_var.Wait(&m_mutex);
	}
}

bool FlipQueue::Flip(uint32_t micros) {
	KYTY_PROFILER_BLOCK("FlipQueue::Flip");

	m_mutex.Lock();
	if (m_requests.empty()) {
		m_submit_cond_var.WaitFor(&m_mutex, micros);
	}
	EXIT_IF(m_processing);
	std::array<Request, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> requests {};
	size_t count = 0;
	// One front-most group per port per vblank. Independent MAIN/SYSTEM
	// queues must neither halve each other's scanout rate nor block behind an
	// unready EOP on another port. Synchronized master/slave groups stay atomic.
	for (const auto& owner: m_requests) {
		if (!Graphics::IsFlipGroupReady(owner, m_requests, m_cpu_requests, [](const auto& r) {
			    return r.state == RequestState::Ready;
		    })) continue;
		const uint64_t group = owner.id;
		for (auto& request: m_requests) {
			if (request.group != group) continue;
			EXIT_IF(count == requests.size());
			request.state = RequestState::Presenting;
			requests[count++] = request;
		}
	}
	if (count == 0) {
		m_mutex.Unlock();
		return false;
	}
	m_processing = true;
	m_mutex.Unlock();

	// Lock each port in bus order, then present and complete the whole group at one Vblank.
	std::sort(requests.begin(), requests.begin() + count,
	          [](const auto& a, const auto& b) { return a.cfg->bus < b.cfg->bus; });
	for (size_t i = 0; i < count; i++) {
		requests[i].cfg->mutex.Lock();
	}
	std::array<bool, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> due {};
	for (size_t i = 0; i < count; i++) {
		due[i] = true;
		for (size_t j = 0; j < count; j++) {
			const auto& member = requests[j];
			if (member.group != requests[i].group) continue;
			due[i] &= member.cfg->opened && !member.cfg->closing &&
			          member.cfg->generation == member.generation;
			if (member.id == member.group) {
				due[i] &= IsFlipDueLocked(*member.cfg, member.generation);
			}
		}
	}
	const bool any_due = std::any_of(due.begin(), due.begin() + count, [](bool value) { return value; });
	std::array<Graphics::Presenter::Layer, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> layers {};
	size_t                      layer_count = 0;
	Graphics::Presenter::Frame* hidden      = nullptr; // The main port behind the headset image.
	for (size_t i = 0; i < count; i++) {
		auto& r = requests[i];
		if (!due[i]) continue;
		if (r.cfg->bus == VIDEO_OUT_BUS_TYPE_MAIN &&
		    g_headset_mirror.load(std::memory_order_relaxed)) {
			hidden = r.frame;
		} else {
			layers[layer_count++] = {r.frame, r.cfg->bus,
			                         r.premultiplied_alpha};
		}
	}
	if (any_due) {
		if (hidden != nullptr) {
			m_presenter.Discard(*hidden);
		}
		if (layer_count != 0) {
			m_presenter.Present(std::span(layers.data(), layer_count));
		}
		for (size_t i = 0; i < count; i++) {
			if (due[i] && requests[i].cfg->bus == VIDEO_OUT_BUS_TYPE_MAIN) {
				Graphics::PerformanceOverlayRecordFlip(requests[i].cfg->width, requests[i].cfg->height);
			}
		}
	}

	m_mutex.Lock();
	for (auto it = m_requests.begin(); it != m_requests.end();) {
		const auto selected = std::find_if(requests.begin(), requests.begin() + count,
		                                   [&](const auto& r) { return r.id == it->id; });
		if (selected == requests.begin() + count) {
			++it;
			continue;
		}
		if (!due[selected - requests.begin()]) {
			it++->state = RequestState::Ready;
			continue;
		}
		auto& r = *it;
		r.cfg->flip_status.count++;
		r.cfg->flip_status.processTime              = LibKernel::KernelGetProcessTime();
		r.cfg->flip_status.processTimeCounter       = LibKernel::KernelGetProcessTimeCounter();
		r.cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
		r.cfg->flip_status.flipArg                  = r.flip_arg;
		r.cfg->flip_status.currentBuffer            = r.index;
		r.cfg->flip_status.flipPendingNum--;
		r.cfg->flip_status.gcQueueNum -= r.source == FlipRequestSource::GpuEop;
		TriggerVideoOutEvents(*r.cfg, VideoOutEventKind::Flip, reinterpret_cast<void*>(r.flip_arg));
		it = m_requests.erase(it);
	}
	m_processing = false;
	m_done_cond_var.SignalAll();
	m_submit_slot_cond_var.SignalAll();
	m_mutex.Unlock();
	for (size_t i = count; i != 0; i--) {
		requests[i - 1].cfg->mutex.Unlock();
	}
	if (any_due) {
		Graphics::RenderDocOnGuestFlip(m_presenter.Renderer());
		if (Config::GraphicsDebugDumpEnabled() &&
		    Config::GetPrintfDirection() != Config::LogDirection::Silent) {
			LOGF("Flip done: %d\n", requests[0].index);
		}
		// ps5-vsh: blank-vs-content census (layers carry guest frames).
		{
			static std::atomic_uint64_t s_layered {0};
			if ((++s_layered % 10) == 0) {
				LOGF("ps5-vsh: present layered x%llu (last count=%zu)\n",
				     (unsigned long long)s_layered.load(), count);
			}
		}
	}
	return any_due;
}

void FlipQueue::GetFlipStatus(VideoOutConfig& cfg, VideoOutFlipStatus& out) {
	Common::LockGuard lock(cfg.mutex);

	out = cfg.flip_status;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSetFlipMaster(int slave_handle, int master_handle) {
	PRINT_NAME();
	return DriverState().SetFlipMaster(slave_handle, master_handle);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void VideoOutAddBufferAttributeOption(VideoOutBufferAttribute2* attribute,
                                                   uint64_t option) {
	PRINT_NAME();
	EXIT_IF(attribute == nullptr);
	attribute->option |= option;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutOpen(int user_id, int bus_type, int index, const void* param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 0);
	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	return DriverState().Open(bus_type, index);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutClose(int handle) {
	PRINT_NAME();

	return DriverState().Close(handle) ? OK : VIDEO_OUT_ERROR_INVALID_HANDLE;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute,
                                               uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option,
                                               uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	LOGF("\t pixel_format                = %016" PRIx64 "\n"
	     "\t tiling_mode                 = %" PRIu32 "\n"
	     "\t width                       = %" PRIu32 "\n"
	     "\t height                      = %" PRIu32 "\n"
	     "\t option                      = %016" PRIx64 "\n"
	     "\t dcc_control                 = %08" PRIx32 "\n"
	     "\t dcc_cb_register_clear_color = %016" PRIx64 "\n",
	     pixel_format, tiling_mode, width, height, option, dcc_control,
	     dcc_cb_register_clear_color);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute2));

	attribute->tiling_mode                 = tiling_mode;
	attribute->aspect_ratio                = 0;
	attribute->width                       = width;
	attribute->height                      = height;
	attribute->pitch_in_pixel              = 0;
	attribute->option                      = option;
	attribute->pixel_format                = pixel_format;
	attribute->dcc_cb_register_clear_color = dcc_cb_register_clear_color;
	attribute->dcc_control                 = dcc_control;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSetFlipRate(int handle, int rate) {
	PRINT_NAME();

	LOGF("\trate = %d\n", rate);

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (rate < 0 || rate > 2) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	Common::LockGuard lock(ctx->mutex);
	ctx->flip_rate = rate;

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutDeleteFlipEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Flip);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Flip, udata);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Vblank);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutDeletePreVblankStartEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                         void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Vblank, udata);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAddPreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                                 void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart, udata);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAddOutputModeEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                             void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::OutputMode, udata);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                           const VideoOutBuffers* buffers, int buffer_num,
                                           const VideoOutBufferAttribute2* attribute, int category,
                                           void* option) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (buffers == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
	    buffer_index_start < 0 || buffer_index_start >= VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_num < 1 || buffer_num > VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_index_start + buffer_num > VIDEO_OUT_BUFFER_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	LOGF("\t start_index    = %d\n"
	     "\t buffer_num     = %d\n"
	     "\t set_index      = %d\n"
	     "\t pixel_format   = 0x%016" PRIx64 "\n"
	     "\t tiling_mode    = %" PRIu32 "\n"
	     "\t aspect_ratio   = %" PRIu32 "\n"
	     "\t width          = %" PRIu32 "\n"
	     "\t height         = %" PRIu32 "\n"
	     "\t pitch_in_pixel = %" PRIu32 "\n"
	     "\t option         = %" PRIu64 "\n"
	     "\t category       = %d\n",
	     buffer_index_start, buffer_num, set_index, attribute->pixel_format, attribute->tiling_mode,
	     attribute->aspect_ratio, attribute->width, attribute->height, attribute->pitch_in_pixel,
	     attribute->option, category);

	if (option != nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	// The headset port takes reprojection buffers with a zeroed attribute. Each names its eye
	// texture, which is registered and flipped like a regular surface.
	const bool headset = ctx->bus == VIDEO_OUT_BUS_TYPE_HMD2_MAIN;
	if (headset != (category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_HMD2)) {
		return VIDEO_OUT_ERROR_INVALID_CATEGORY;
	}
	VideoOutBufferAttribute2                              headset_attribute {};
	std::array<VideoOutBuffers, VIDEO_OUT_BUFFER_NUM_MAX> headset_buffers {};
	if (headset) {
		for (int i = 0; i < buffer_num; i++) {
			Hmd2::ReprojectionSurface surface {};
			if (buffers[i].metadata != nullptr ||
			    !Hmd2::GetReprojectionSurface(buffers[i].data, surface) ||
			    (i != 0 && (surface.width != headset_attribute.width ||
			                surface.height != headset_attribute.height ||
			                surface.pixel_format != headset_attribute.pixel_format))) {
				return VIDEO_OUT_ERROR_INVALID_VALUE;
			}
			headset_attribute.pixel_format = surface.pixel_format;
			headset_attribute.width        = surface.width;
			headset_attribute.height       = surface.height;
			headset_buffers[i].data        = reinterpret_cast<const void*>(surface.address);
		}
		attribute = &headset_attribute;
		buffers   = headset_buffers.data();
		category  = VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED;
	}
	if (category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED &&
	    category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED) {
		return VIDEO_OUT_ERROR_INVALID_CATEGORY;
	}

	BufferAttributeGroup group {
	    .attribute = *attribute,
	    .category  = category,
	    .occupied  = true,
	};
	std::vector<VideoOutBuffer> registrations;
	registrations.reserve(static_cast<size_t>(buffer_num));

	for (int i = 0; i < buffer_num; i++) {
		LOGF("\t buffers[%d]: data=%p metadata=%p\n", i, buffers[i].data, buffers[i].metadata);
		if (buffers[i].reserved[0] != nullptr || buffers[i].reserved[1] != nullptr) {
			LOGF("\t buffers[%d]: ignoring reserved fields {%p, %p}\n", i, buffers[i].reserved[0],
			     buffers[i].reserved[1]);
		}
		const auto data_address     = reinterpret_cast<uint64_t>(buffers[i].data);
		const auto metadata_address = reinterpret_cast<uint64_t>(buffers[i].metadata);
		registrations.push_back({
		    .group_index      = set_index,
		    .data_address     = data_address,
		    .metadata_address = metadata_address,
		});
		(void)group.ImageInfo(registrations.back());
	}

	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (ctx->groups[set_index].occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	for (int i = 0; i < buffer_num; i++) {
		if (ctx->buffers[buffer_index_start + i].Occupied()) {
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}
	}

	ctx->groups[set_index] = group;
	for (int i = 0; i < buffer_num; i++) {
		ctx->buffers[buffer_index_start + i] = registrations[static_cast<size_t>(i)];
		const auto& buffer                   = registrations[static_cast<size_t>(i)];
		LOGF("\tbuffers[%d] = %016" PRIx64 " metadata = %016" PRIx64 " dcc = %08" PRIx32 "\n",
		     buffer_index_start + i, buffer.data_address, buffer.metadata_address,
		     attribute->dcc_control);
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index,
                                                       const VideoOutBufferAttribute2* attribute,
                                                       void*                           option) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	if (option != nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	Common::LockGuard lock(ctx->mutex);
	const auto&       current = ctx->groups[set_index];
	if (ctx->closing || !current.occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	BufferAttributeGroup replacement {
	    .attribute = *attribute,
	    .category  = current.category,
	    .occupied  = true,
	};
	for (const auto& buffer: ctx->buffers) {
		if (buffer.group_index == set_index) {
			(void)replacement.ImageInfo(buffer);
		}
	}
	ctx->groups[set_index] = replacement;

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutUnregisterBuffers(int handle, int set_index) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!ctx->groups[set_index].occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	ctx->groups[set_index] = BufferAttributeGroup {};
	for (auto& buffer: ctx->buffers) {
		if (buffer.group_index == set_index) {
			buffer = VideoOutBuffer {};
		}
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg) {
	PRINT_NAME();
	// ps5-vsh chainSUI295: bounded submit census (proves ShellUI ever presents).
	{
		static std::atomic_uint64_t s_submit {0};
		const auto n = ++s_submit;
		if (n <= 64 || (n % 1000) == 0) {
			LOGF("ps5-vsh: submit-flip #%llu handle=%d index=%d mode=%d arg=%lld\n",
			     (unsigned long long)n, handle, index, flip_mode, (long long)flip_arg);
		}
	}

	uint64_t  request_id = 0;
	const int result     = ReserveFlipRequest(DriverState(), handle, index, flip_mode, flip_arg,
	                                          FlipRequestSource::Cpu, request_id);
	if (result == VIDEO_OUT_ERROR_INVALID_FLIP_MODE) {
		LOGF("\t unsupported flip_mode = %d\n", flip_mode);
	}
	if (result != OK) {
		return result;
	}
	g_video_out_driver->SubmitFlipPreparation(request_id);

	return OK;
}

int VideoOutDriver::SubmitFlipFromGpu(Graphics::CommandBuffer& buffer, int handle, int index,
                                      int flip_mode, int64_t flip_arg, uint64_t& request_id) {
	EXIT_IF(buffer.IsInvalid());

	const int result = ReserveFlipRequest(*m_impl, handle, index, flip_mode, flip_arg,
	                                      FlipRequestSource::GpuEop, request_id);
	if (result != OK) {
		return result;
	}
	m_impl->GetFlipQueue().Prepare(request_id, buffer);

	return OK;
}

void VideoOutDriver::SubmitFlipPreparation(uint64_t request_id) {
	m_impl->Renderer().GetGpu().SubmitFlipPreparation(request_id);
}

void VideoOutDriver::PrepareFlip(uint64_t request_id, Graphics::CommandBuffer& buffer) {
	m_impl->GetFlipQueue().Prepare(request_id, buffer);
}

void VideoOutDriver::CompleteFlip(uint64_t request_id) {
	m_impl->GetFlipQueue().Complete(request_id);
}

void VideoOutDriver::WaitForSubmitSlot(int handle) {
	auto* cfg = m_impl->Get(handle);
	EXIT_IF(cfg == nullptr);
	m_impl->GetFlipQueue().WaitForSubmitSlot(*cfg);
}

void VideoOutDriver::WaitFlipDone(int handle, int index) {
	auto* ctx = m_impl->Get(handle);
	EXIT_IF(ctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!IsValidBufferIndex(index));
	m_impl->GetFlipQueue().Wait(*ctx, index);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	DriverState().GetFlipQueue().GetFlipStatus(*ctx, *status);

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n"
	     "\t submitProcessTimeCounter = %" PRIu64 "\n"
	     "\t flipArg = %" PRId64 "\n"
	     "\t gcQueueNum = %d\n"
	     "\t flipPendingNum = %d\n"
	     "\t currentBuffer = %d\n",
	     status->count, status->processTime, status->processTimeCounter,
	     status->submitProcessTimeCounter, status->flipArg, status->gcQueueNum,
	     status->flipPendingNum, status->currentBuffer);

	return OK;
}

// ps5-vsh chainSUI326: reads PSM SharedDmem globals + eboot backend (see
// call site for why). Read-only guest loads; caller bounds the rate.
static void SamplePsmSharedGlobals() {
	auto*    rt       = Common::Singleton<Loader::RuntimeLinker>::Instance();
	uint64_t base     = 0;
	uint64_t eboot    = 0;
	uint64_t agc_base = 0;
	if (rt != nullptr) {
		for (auto* p: rt->GetPrograms()) {
			if (p == nullptr) {
				continue;
			}
			const auto name = Common::PathToString(p->file_name.filename());
			if (name == "libScePsm.sprx") {
				base = p->base_vaddr;
			} else if (name == "eboot.bin") {
				eboot = p->base_vaddr;
			} else if (name == "AgcCompositor.elf") {
				agc_base = p->base_vaddr;
			}
		}
	}
	auto readq = [](uint64_t addr, uint64_t& out) {
		if (addr == 0 || !Libs::Graphics::HostMemoryRangeIsReadable(addr, 8)) {
			return false;
		}
		out = *reinterpret_cast<const uint64_t*>(addr);
		return true;
	};
	uint64_t   fp0 = 0, fp1 = 0, b38 = 0, b40 = 0, b48 = 0;
	const bool ok0 = readq(base + 0x445FD0, fp0);
	const bool ok1 = readq(base + 0x445FC8, fp1);
	const bool okb = readq(base + 0x42CE38, b38) && readq(base + 0x42CE40, b40) &&
	                 readq(base + 0x42CE48, b48);
	// ps5-vsh chainSUI329: contents decide the submit gate (decompiled
	// 0x1B62C0: submit iff (a8&2)==0 || (base && *base); the else-branch
	// needs eboot callbacks qword_446070/78 set and byte_42CE50==1).
	uint64_t   c38 = 0;
	const bool okc = okb && readq(b38, c38);
	uint64_t   cb70 = 0, cb78 = 0, fl50 = 0;
	const bool okcb = readq(base + 0x446070, cb70) && readq(base + 0x446078, cb78) &&
	                  readq(base + 0x42CE50, fl50);
	uint64_t   eb  = 0;
	const bool oke = readq(eboot + 0x26B7520, eb);

	uint64_t   psm_ctx = 0;
	uint64_t   flag236 = 0;
	const bool ok_ctx  = readq(base + 0x42A550, psm_ctx);
	const bool ok_f236 = ok_ctx && readq(psm_ctx + 236, flag236);

	uint64_t   agc_fl64 = 0;
	const bool ok_fl64  = readq(agc_base + 0x374170 + 64, agc_fl64);
	const auto byte66   = ok_fl64 ? static_cast<uint8_t>((agc_fl64 >> 16) & 0xFF) : 0;

	LOGF("ps5-vsh: psm-shared-globals base=0x%llx fp0=%d:0x%llx fp1=%d:0x%llx bases=%d:%llx,%llx,%llx contents=%d:0x%llx cb=%d:%llx,%llx,fl=%llx eboot=0x%llx backend=%d:0x%llx psm_ctx=%d:0x%llx f236=%d:0x%llx agc_fl66=%u\n",
	     (unsigned long long)base, ok0 ? 1 : 0, (unsigned long long)fp0,
	     ok1 ? 1 : 0, (unsigned long long)fp1, okb ? 1 : 0, (unsigned long long)b38,
	     (unsigned long long)b40, (unsigned long long)b48, okc ? 1 : 0,
	     (unsigned long long)c38, okcb ? 1 : 0, (unsigned long long)cb70,
	     (unsigned long long)cb78, (unsigned long long)fl50, (unsigned long long)eboot,
	     oke ? 1 : 0, (unsigned long long)eb, ok_ctx ? 1 : 0, (unsigned long long)psm_ctx,
	     ok_f236 ? 1 : 0, (unsigned long long)flag236, (unsigned)byte66);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle) {
	PRINT_NAME();
	// ps5-vsh chainSUI326: sample PSM SharedDmem globals at low thresholds
	// plus a periodic sample to trace PSM/AgcCompositor flip state in long runs.
	static std::atomic_uint64_t s_pending_n {0};
	const auto pn = s_pending_n.fetch_add(1, std::memory_order_relaxed);
	if (pn == 500 || pn == 2000 || pn == 5000 || (pn > 5000 && (pn % 2000) == 0)) {
		SamplePsmSharedGlobals();
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	VideoOutFlipStatus status {};
	DriverState().GetFlipQueue().GetFlipStatus(*ctx, status);

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("\t flipPendingNum = %d\n", status.flipPendingNum);
	}

	return status.flipPendingNum;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	*status = ctx->vblank_status;
	ctx->mutex.Unlock();

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n",
	     status->count, status->processTime, status->processTimeCounter);

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutGetEventData(const EventQueue::KernelEvent* ev, int64_t* data) {
	PRINT_NAME();

	if (ev == nullptr || data == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	uint64_t event_data = static_cast<uint64_t>(ev->data) >> 16u;
	if (ev->ident == VIDEO_OUT_EVENT_FLIP &&
	    (static_cast<uint64_t>(ev->data) & 0x8000000000000000ULL) != 0) {
		event_data |= 0xffff000000000000ULL;
	}

	*data = static_cast<int64_t>(event_data);

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutGetEventCount(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	return static_cast<int>((static_cast<uint64_t>(ev->data) >> 12u) & 0xfu);
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutWaitVblank(int handle) {
	PRINT_NAME();
	// ps5-vsh chainSUI312: bounded wait census (pre-OnLaunched stall gate).
	// Proves whether PSM render threads park here forever (lost wakeup).
	static std::atomic_uint64_t s_vbwait {0};
	const auto n = ++s_vbwait;
	const bool sample = (n <= 8 || (n % 500) == 0);

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	uint64_t waited_from = 0;
	uint64_t now_count   = 0;
	{
		Common::LockGuard lock(ctx->mutex);
		const auto        count = ctx->vblank_status.count;
		while (ctx->opened && ctx->vblank_status.count == count) {
			ctx->vblank_cond.Wait(&ctx->mutex);
		}
		// ps5-vsh chainSUI321 (audit): snapshot under lock, log after it
		// drops (VblankEnd needs this mutex to signal; never hold it
		// across I/O).
		waited_from = count;
		now_count   = ctx->vblank_status.count;
	}
	if (sample) {
		LOGF("ps5-vsh: wait-vblank #%llu handle=%d waited_count=%llu now=%llu\n",
		     (unsigned long long)n, handle, (unsigned long long)waited_from,
		     (unsigned long long)now_count);
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	int32_t attribute3 = 0;
	Loader::SystemContentParamSfoGetInt("ATTRIBUTE3", &attribute3);
	int32_t attribute = 0;
	const bool hdr_allowed = !Loader::SystemContentParamSfoGetInt("ATTRIBUTE", &attribute) ||
	                         (attribute & 0x20000000) != 0;
	ctx->mutex.Lock();
	// Primary output reports 4K unless param.json Video-out Info enables resolution detection.
	status->resolution =
	    ((attribute3 & 4) != 0 && ctx->width < 3840 && ctx->height < 2160 ? 1u : 2u);
	status->dynamicRange = hdr_allowed && DriverState().IsHdrSupported() ? 2u : 1u;
	status->refreshRate =
	    (ctx->output_mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ || Config::GetVblankFrequency() >= 119
	         ? VIDEO_OUT_REFRESH_RATE_119_88HZ
	         : VIDEO_OUT_REFRESH_RATE_59_94HZ);
	status->flags       = 0;
	status->reserved[0] = 0;
	status->reserved[1] = 0;
	status->reserved[2] = 0;
	ctx->mutex.Unlock();

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAllowOutputResolutionWqhdDetection(int handle) {
	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutVrrPegToFixedRate(int handle, uint64_t arg1, uint64_t arg2) {
	PRINT_NAME();

	static std::atomic_bool logged {false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("\t handle = %d\n"
		     "\t arg1   = 0x%016" PRIx64 "\n"
		     "\t arg2   = 0x%016" PRIx64 "\n",
		     handle, arg1, arg2);
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutVrrUnpegFromFixedRate(int handle) {
	PRINT_NAME();

	static std::atomic_bool logged {false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("\t handle = %d\n", handle);
	}

	return OK;
}

static int ValidateOutputConfig(int handle, uint64_t mode, const VideoOutOutputOptions* options,
                                void* reserved_ptr, uint64_t reserved) {
	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (reserved_ptr != nullptr || reserved != 0) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (options != nullptr) {
		for (auto v: options->internalData) {
			if (v != 0) {
				return VIDEO_OUT_ERROR_INVALID_OPTION;
			}
		}
	}

	if (mode != VIDEO_OUT_OUTPUT_MODE_DEFAULT && mode != VIDEO_OUT_OUTPUT_MODE_119_88HZ &&
	    mode != Hmd2::OUTPUT_MODE_89_91HZ && mode != Hmd2::OUTPUT_MODE_119_88HZ) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE;
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutInitializeOutputOptions(VideoOutOutputOptions* options) {
	PRINT_NAME();

	if (options == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	memset(options, 0, sizeof(VideoOutOutputOptions));

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutIsOutputSupported(int handle, uint64_t mode,
                                            const VideoOutOutputOptions* options,
                                            void* reserved_ptr, uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = ValidateOutputConfig(handle, mode, options, reserved_ptr, reserved);
	if (result != OK) {
		return result;
	}

	if (mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ) {
		return (Config::GetVblankFrequency() >= 119 ? VIDEO_OUT_TRUE : VIDEO_OUT_FALSE);
	}

	return VIDEO_OUT_TRUE;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutConfigureOutput(int handle, uint64_t mode,
                                          const VideoOutOutputOptions* options, void* reserved_ptr,
                                          uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = VideoOutIsOutputSupported(handle, mode, options, reserved_ptr, reserved);
	if (result < 0) {
		return result;
	}
	if (result == VIDEO_OUT_FALSE) {
		return VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	ctx->output_mode = mode;
	// Physical output mode survives closing/reopening a source port. Ordinary
	// OVERLAY configuration also targets MAIN; HMD/SUB remain separate outputs.
	DriverState().SetEndpointOutputMode(ctx->bus, mode);
	TriggerVideoOutEvents(*ctx, VideoOutEventKind::OutputMode,
	                      reinterpret_cast<void*>(ctx->output_mode));
	VideoOutPublishSystemOutputMode(ctx->bus, ctx->output_mode);
	ctx->mutex.Unlock();

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t top    = %d\n"
	     "\t bottom = %d\n",
	     top, bottom);

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutLatencyControlWaitBeforeInput(int handle) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutLatencyMeasureSetStartPoint(int handle, uint32_t point) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t point = %" PRIu32 "\n", point);

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma) {
	PRINT_NAME();

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (gamma < 0.1f || gamma > 2.0f) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	settings->gamma = gamma;
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings) {
	PRINT_NAME();

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	ctx->mutex.Lock();
	ctx->gamma = settings->gamma;
	ctx->mutex.Unlock();

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysSetDimmer(int handle, int dimmer) {
	PRINT_NAME();
	LOGF("\t handle = %d dimmer = %d\n", handle, dimmer);
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysGetPipelineStatus(int handle, void* status) {
	PRINT_NAME();
	LOGF("\t handle = %d status = %p\n", handle, status);
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysIsUserStatusVr(int user_id) {
	PRINT_NAME();
	LOGF("\t user_id = %d\n", user_id);
	return 0; // no VR headset in the emulator
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysGetVideoOutModeByBusSpecifier(int bus_specifier, void* mode) {
	PRINT_NAME();
	LOGF("\t bus_specifier = %d mode = %p\n", bus_specifier, mode);
	if (mode != nullptr) {
		uint8_t buffer[0x28] {};
		*reinterpret_cast<uint32_t*>(buffer + 0) = 0x20;
		buffer[6] = 14; // Mode format: 14 = 1080p standard
		*reinterpret_cast<uint64_t*>(buffer + 8) = 60; // Refresh rate
		*reinterpret_cast<uint32_t*>(buffer + 0x20) = 0; // System default mode
		std::memcpy(mode, buffer, sizeof(buffer));
	}
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysIsModeSupported(const void* mode) {
	PRINT_NAME();
	if (mode == nullptr) {
		return 0;
	}
	const auto* p = reinterpret_cast<const uint8_t*>(mode);
	uint8_t format = p[6];
	int supported = static_cast<uint8_t>(format - 12) < 6 ? 1 : 0;
	LOGF("\t format = %u -> %d\n", format, supported);
	return supported;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysModeIsSupported(const void* mode) {
	PRINT_NAME();
	if (mode == nullptr) {
		return 0;
	}
	uint32_t flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(mode) + 0x20);
	return ((~flags) & 6) == 0 ? 1 : 0;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysModeIs2(const void* mode) {
	PRINT_NAME();
	if (mode == nullptr) {
		return 0;
	}
	uint32_t flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(mode) + 0x20);
	return (flags & 6) == 2 ? 1 : 0;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysGetResolutionStatus2(uint32_t pipeline, void* status) {
	PRINT_NAME();
	if (pipeline >= 2) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	uint32_t record[16] {};
	record[0] = record[2] = 1920;
	record[1] = record[3] = 1080;
	record[4] = 9; // Native PSM refresh-rate enum: 60 Hz
	record[7] = 6; // Fixed output supports both required scanout capabilities
	std::memcpy(status, record, sizeof(record));
	return OK;
}

static int ValidateSystemStatusQuery(int user, uint32_t pipeline, int reserved,
                                    uint64_t reserved2) {
	if (user != 255 || pipeline >= 2 || reserved2 != 0) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (reserved != 0) {
		return static_cast<int>(0x8029001au);
	}
	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysIsUserStatusSystemDefault(int user, uint32_t pipeline,
    uint64_t unused, int reserved, uint64_t reserved2) {
	PRINT_NAME();
	(void)unused;
	const auto result = ValidateSystemStatusQuery(user, pipeline, reserved, reserved2);
	return result == OK ? 1 : result;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int VideoOutSysGetPrivacyMaskStatus(uint64_t* occupied_ids) {
	PRINT_NAME();
	if (occupied_ids == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	*occupied_ids = 0;
	return OK;
}

} // namespace Libs::VideoOut
