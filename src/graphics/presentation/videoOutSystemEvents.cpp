#include "graphics/presentation/videoOutSystemEvents.h"
#include "graphics/presentation/videoOut.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

namespace {
namespace EventQueue = LibKernel::EventQueue;
constexpr int       HEAD_MAIN = 0;
constexpr int       HEAD_VR = 1;
constexpr uint64_t  DEFAULT_OUTPUT_MODE = 1;

struct Registration {
	EventQueue::KernelEqueue queue;
};

struct HeadState {
	// Serialize add/publish/reset so registration cannot miss a concurrent mode
	// change. Equeue callbacks are stateless and never take this mutex.
	Common::Mutex mutex;
	uint64_t mode = DEFAULT_OUTPUT_MODE;
	// The equeue owns each registration. Deleting it (or the entire queue)
	// expires the weak entry, including a delete racing with registration.
	std::vector<std::weak_ptr<Registration>> registrations;
};

// Native PSM subscribes to MAIN and VR even with no headset attached. These
// output heads exist independently of scanout ports and window mirror planes.
// The native head number is also part of the kernel event identity: registering
// both on the same equeue must not replace the other's userdata or callbacks.
std::array<HeadState, 2> g_heads;

intptr_t EncodeModeEvent(intptr_t previous, uint64_t mode) {
	const uint64_t counter = std::min(((static_cast<uint64_t>(previous) >> 12u) & 0xfu) + 1u,
	                                  uint64_t {0xf});
	return static_cast<intptr_t>((LibKernel::KernelReadTsc() & 0xfffu) | (counter << 12u) |
	                             ((mode & 0x0000ffffffffffffULL) << 16u));
}

void ResetEvent(EventQueue::KernelEqueueEvent* event) {
	event->triggered = false;
	event->event.fflags = 0;
	event->event.data = 0;
}

void TriggerEvent(EventQueue::KernelEqueueEvent* event, void* data) {
	auto next = event->event;
	next.fflags = std::min(next.fflags + 1u, 0xfu);
	next.data = EncodeModeEvent(next.data, reinterpret_cast<uintptr_t>(data));
	if (event->triggered) {
		event->pending_events.push_back(next);
	} else {
		event->event = next;
		event->triggered = true;
	}
}

} // namespace

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutSysAddSetModeEvent2(EventQueue::KernelEqueue eq, int head, void* udata) {
	PRINT_NAME();
	if (head != HEAD_MAIN && head != HEAD_VR) return VIDEO_OUT_ERROR_INVALID_VALUE;
	if (!EventQueue::KernelPinEqueue(eq)) return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	auto& state = g_heads[head];
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(state.mutex);
	std::erase_if(state.registrations, [](const auto& entry) { return entry.expired(); });
	std::shared_ptr<Registration> registration;
	for (const auto& weak: state.registrations) {
		if (auto entry = weak.lock(); entry && entry->queue == eq) {
			registration = std::move(entry);
			break;
		}
	}
	if (!registration) {
		registration = std::make_shared<Registration>(Registration {eq});
		state.registrations.push_back(registration);
	}
	EventQueue::KernelEqueueEvent event {};
	event.triggered = true;
	event.event.ident = VideoOutSystemSetModeEventId(head);
	event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata = udata;
	event.event.fflags = 1;
	event.event.data = EncodeModeEvent(0, state.mode);
	event.filter.owner = registration;
	event.filter.reset_func = ResetEvent;
	event.filter.trigger_func = TriggerEvent;
	const int result = EventQueue::KernelAddEvent(eq, event);
	LOGF("video-out: system set-mode register head=%d eq=%lld mode=0x%llx -> %d\n", head,
	     static_cast<long long>(eq), static_cast<unsigned long long>(state.mode), result);
	return result == LibKernel::KERNEL_ERROR_EBADF ? VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE : result;
}

void VideoOutPublishSystemOutputMode(int bus, uint64_t mode) {
	// Application MAIN, app overlay and AGC MAIN_SYSTEM feed the physical MAIN
	// output. HMD mode changes feed VR even when mirrored in the host window.
	int head = HEAD_MAIN;
	if (bus == 32) {
		head = HEAD_VR;
	} else if (bus != 0 && bus != 1 && bus != 256) {
		return;
	}
	auto& state = g_heads[head];
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(state.mutex);
	state.mode = mode;
	std::erase_if(state.registrations, [](const auto& entry) { return entry.expired(); });
	for (const auto& weak: state.registrations) {
		const auto registration = weak.lock();
		if (!registration) continue;
		const auto result = EventQueue::KernelTriggerEvent(
		    registration->queue, VideoOutSystemSetModeEventId(head), EventQueue::KERNEL_EVFILT_VIDEO_OUT,
		    reinterpret_cast<void*>(mode));
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
		                     result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

void VideoOutResetSystemOutputEvents() {
	Common::HostLockCriticalGuard critical;
	for (unsigned head = 0; head < g_heads.size(); ++head) {
		auto& state = g_heads[head];
		Common::LockGuard lock(state.mutex);
		state.mode = DEFAULT_OUTPUT_MODE;
		for (const auto& weak: state.registrations) {
			const auto registration = weak.lock();
			if (!registration) continue;
			const auto result = EventQueue::KernelDeleteEvent(
			    registration->queue, VideoOutSystemSetModeEventId(head), EventQueue::KERNEL_EVFILT_VIDEO_OUT);
			EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
			                     result != LibKernel::KERNEL_ERROR_ENOENT);
		}
		state.registrations.clear();
	}
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutGetEventId(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();
	if (ev == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) return VIDEO_OUT_ERROR_INVALID_EVENT;
	switch (ev->ident) {
		// Existing port-event identities remain supported.
		case 0:
		case 1:
		case 2:
		case 8: return static_cast<int>(ev->ident);
		// Native GetEventId (0x127e0) decodes system event type 0x73 as SET_MODE=8.
		case VideoOutSystemSetModeEventId(HEAD_MAIN):
		case VideoOutSystemSetModeEventId(HEAD_VR): return 8;
		default: return VIDEO_OUT_ERROR_INVALID_EVENT;
	}
}

} // namespace Libs::VideoOut
