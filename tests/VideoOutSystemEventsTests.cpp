#include "graphics/presentation/scanoutLayers.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/videoOutSystemEvents.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

// Isolate the clock and unrelated socket backend; the native VideoOut entry,
// registration lifetime and kernel equeue implementation below are production.
namespace Libs {
void NoteHleCall(const char*, const char*, const char*) {}
HleSlowCallGuard::HleSlowCallGuard(const char* func_, const char* what_)
    : func(func_), what(what_), t0(0) {}
HleSlowCallGuard::~HleSlowCallGuard() = default;
}
namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelReadTsc() { return 0x123; }
}
namespace Libs::Network::Net {
bool KYTY_SYSV_ABI IsSocket(int) { std::abort(); }
bool IsSocketReadReady(int) { std::abort(); }
}

namespace {
namespace EventQueue = Libs::LibKernel::EventQueue;
using namespace Libs::VideoOut;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "VideoOutSystemEventsTests: %s\n", message);
		std::abort();
	}
}

EventQueue::KernelEqueue CreateQueue() {
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "physical-main-output") == OK, "create queue");
	return queue;
}

void ExpectMode(EventQueue::KernelEqueue queue, uint64_t mode, void* udata, unsigned head = 0) {
	EventQueue::KernelEvent event {};
	Libs::LibKernel::KernelUseconds timeout = 0;
	int count = 0;
	Check(EventQueue::KernelWaitEqueue(queue, &event, 1, &count, &timeout) == OK && count == 1,
	      "a real set-mode event must be delivered");
	Check(event.ident == (head == 0 ? 0x0073000100000000ULL : 0x0073010100000000ULL) &&
	          event.filter == EventQueue::KERNEL_EVFILT_VIDEO_OUT && VideoOutGetEventId(&event) == 8,
	      "set-mode event identity/filter");
	Check((static_cast<uint64_t>(event.data) >> 16u) == mode, "mode payload encoding");
	Check(((static_cast<uint64_t>(event.data) >> 12u) & 0xfu) != 0, "event occurrence count");
	Check((static_cast<uint64_t>(event.data) & 0xfffu) == 0x123, "event timestamp encoding");
	Check(event.udata == udata, "event userdata");
}

void ExpectIdle(EventQueue::KernelEqueue queue) {
	EventQueue::KernelEvent event {};
	Libs::LibKernel::KernelUseconds timeout = 0;
	int count = 0;
	Check(EventQueue::KernelWaitEqueue(queue, &event, 1, &count, &timeout) ==
	          Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
	      "consumed or unregistered mode event must not remain ready");
}

void TestBootWithoutApplicationPort() {
	VideoOutResetSystemOutputEvents();
	auto queue = CreateQueue();
	void* udata = reinterpret_cast<void*>(0x4567);
	// No VideoOutDriver, presenter, GPU or application MAIN handle exists in
	// this executable. This is the failing native ShellUI call, unchanged.
	Check(VideoOutSysAddSetModeEvent2(queue, 0, udata) == OK,
	      "HEAD_MAIN registration must not look up application handle 1");
	ExpectMode(queue, 1, udata);
	ExpectIdle(queue);
	VideoOutPublishSystemOutputMode(256, 15);
	ExpectMode(queue, 15, udata);
	ExpectIdle(queue);

	auto late_queue = CreateQueue();
	Check(VideoOutSysAddSetModeEvent2(late_queue, 0, nullptr) == OK, "late subscriber");
	ExpectMode(late_queue, 15, nullptr);
	// A new application's output configuration reaches the same physical head.
	VideoOutPublishSystemOutputMode(0, 1);
	ExpectMode(queue, 1, udata);
	ExpectMode(late_queue, 1, nullptr);
	VideoOutPublishSystemOutputMode(32, 15);
	VideoOutPublishSystemOutputMode(2, 15);
	ExpectIdle(queue);
	ExpectIdle(late_queue);
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete queue");
	Check(EventQueue::KernelDeleteEqueue(late_queue) == OK, "delete late queue");
}

void TestNativePsmInitializeAndReceive() {
	VideoOutResetSystemOutputEvents();
	auto queue = CreateQueue();
	// libScePsm 13.60 initialization registers BOTH heads before publishing
	// this queue to ReceiveSetModeEvent. A failed second add deletes the queue.
	Check(VideoOutSysAddSetModeEvent2(queue, 0, reinterpret_cast<void*>(1)) == OK,
	      "PSM InitializeSetModeEvent MAIN registration");
	Check(VideoOutSysAddSetModeEvent2(queue, 1, reinterpret_cast<void*>(2)) == OK,
	      "PSM InitializeSetModeEvent VR registration must succeed without a headset port");
	EventQueue::KernelEvent events[8] {};
	Libs::LibKernel::KernelUseconds timeout = 0;
	int count = 0;
	Check(EventQueue::KernelWaitEqueue(queue, events, 8, &count, &timeout) == OK && count == 2,
	      "PSM ReceiveSetModeEvent must receive both heads instead of EBADF");
	Check(events[0].ident == 0x0073000100000000ULL && events[1].ident == 0x0073010100000000ULL,
	      "native system event identities distinguish MAIN and VR");
	Check(VideoOutGetEventId(&events[0]) == 8 && VideoOutGetEventId(&events[1]) == 8,
	      "both native identities decode as set-mode events");
	Check(events[0].filter == EventQueue::KERNEL_EVFILT_VIDEO_OUT &&
	          events[1].filter == EventQueue::KERNEL_EVFILT_VIDEO_OUT &&
	          events[0].udata == reinterpret_cast<void*>(1) &&
	          events[1].udata == reinterpret_cast<void*>(2),
	      "both native head registrations retain their filter and userdata");
	ExpectIdle(queue); // Native receiver treats ETIMEDOUT as false, not an exception.
	VideoOutPublishSystemOutputMode(256, 15);
	ExpectMode(queue, 15, reinterpret_cast<void*>(1));
	ExpectIdle(queue);
	VideoOutPublishSystemOutputMode(32, 1);
	ExpectMode(queue, 1, reinterpret_cast<void*>(2), 1);
	ExpectIdle(queue);
	// Removing MAIN must not invalidate the shared queue or its VR registration.
	Check(EventQueue::KernelDeleteEvent(queue, 0x0073000100000000ULL,
	                                  EventQueue::KERNEL_EVFILT_VIDEO_OUT) == OK,
	      "delete only MAIN from the dual-head queue");
	VideoOutPublishSystemOutputMode(0, 1);
	ExpectIdle(queue);
	VideoOutPublishSystemOutputMode(32, 15);
	ExpectMode(queue, 15, reinterpret_cast<void*>(2), 1);
	VideoOutResetSystemOutputEvents();
	ExpectIdle(queue);
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete PSM dual-head queue");
}

void TestPlaneOwnershipIsIndependentOfHeadEvents() {
	VideoOutResetSystemOutputEvents();
	auto queue = CreateQueue();
	Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == OK, "register head");
	ExpectMode(queue, 1, nullptr);
	struct Frame { bool blank = false; } app, system;
	Libs::Graphics::ScanoutLayers<Frame> planes;
	Check(planes.Replace({&system, 256, true}) == nullptr, "retain system-only plane");
	Check(planes.Replace({&app, 0, false}) == nullptr, "retain independent application plane");
	Check(planes.Clear(0) == &app && planes.Ordered()[2].frame == &system,
	      "closing app plane retains system plane");
	VideoOutPublishSystemOutputMode(256, 15);
	ExpectMode(queue, 15, nullptr);
	Check(planes.Clear(256) == &system && !planes.HasFrames(), "clear system plane");
	VideoOutPublishSystemOutputMode(0, 1);
	ExpectMode(queue, 1, nullptr);
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete queue after plane changes");
}

void TestDuplicateDeleteAndShutdown() {
	VideoOutResetSystemOutputEvents();
	auto queue = CreateQueue();
	Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == OK, "register");
	VideoOutPublishSystemOutputMode(256, 15);
	void* udata = reinterpret_cast<void*>(0x8910);
	Check(VideoOutSysAddSetModeEvent2(queue, 0, udata) == OK, "duplicate register");
	ExpectMode(queue, 1, udata);
	ExpectMode(queue, 15, udata);
	ExpectIdle(queue);
	Check(EventQueue::KernelDeleteEvent(queue, 0x0073000100000000ULL,
	                                  EventQueue::KERNEL_EVFILT_VIDEO_OUT) == OK,
	      "delete registration through equeue");
	VideoOutPublishSystemOutputMode(256, 1);
	ExpectIdle(queue);
	Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == OK, "reregister after deletion");
	ExpectMode(queue, 1, nullptr);
	VideoOutPublishSystemOutputMode(0, 15);
	VideoOutResetSystemOutputEvents();
	ExpectIdle(queue);
	Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == OK, "reregister after shutdown");
	ExpectMode(queue, 1, nullptr);
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete reset queue");
	VideoOutPublishSystemOutputMode(256, 15); // Closed queues must have detached safely.
	VideoOutResetSystemOutputEvents();
}

void TestValidationAndConcurrentQueueClose() {
	Check(VideoOutGetEventId(nullptr) == VIDEO_OUT_ERROR_INVALID_ADDRESS, "event id null validation");
	EventQueue::KernelEvent event {};
	event.ident = 0x0073000100000000ULL;
	Check(VideoOutGetEventId(&event) == VIDEO_OUT_ERROR_INVALID_EVENT, "event id filter validation");
	event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	for (const uintptr_t id: {uintptr_t {0}, uintptr_t {1}, uintptr_t {2}, uintptr_t {8}}) {
		event.ident = id;
		Check(VideoOutGetEventId(&event) == static_cast<int>(id), "preserve port event decoding");
	}
	event.ident = 0x0073020100000000ULL;
	Check(VideoOutGetEventId(&event) == VIDEO_OUT_ERROR_INVALID_EVENT, "reject nonexistent head event");
	auto queue = CreateQueue();
	for (const int head: {-1, 2, 32, 256}) {
		Check(VideoOutSysAddSetModeEvent2(queue, head, nullptr) ==
		          VIDEO_OUT_ERROR_INVALID_VALUE, "reject invalid head / guest bus as head");
	}
	ExpectIdle(queue);
	Check(VideoOutSysAddSetModeEvent2(0, 0, nullptr) == VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE,
	      "reject invalid queue");
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete validation queue");
	Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE,
	      "reject stale queue");
	for (int iteration = 0; iteration < 32; ++iteration) {
		queue = CreateQueue();
		Check(VideoOutSysAddSetModeEvent2(queue, 0, nullptr) == OK, "register concurrent queue");
		std::jthread publish([] {
			for (int i = 0; i < 16; ++i) VideoOutPublishSystemOutputMode(256, (i & 1) ? 1 : 15);
		});
		Check(EventQueue::KernelDeleteEqueue(queue) == OK, "close during publish");
	}
	VideoOutResetSystemOutputEvents();
}
} // namespace

int main() {
	TestNativePsmInitializeAndReceive();
	TestBootWithoutApplicationPort();
	TestPlaneOwnershipIsIndependentOfHeadEvents();
	TestDuplicateDeleteAndShutdown();
	TestValidationAndConcurrentQueueClose();
	std::puts("VideoOutSystemEventsTests: all five cases passed (native entry + real CPU equeues)");
	return 0;
}
