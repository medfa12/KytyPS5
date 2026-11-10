#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_AGCQUEUEIDENTITY_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_AGCQUEUEIDENTITY_H_

#include <cstdint>

// sceAgcDriverCreateQueue queue identity and its graphics EOP interrupt source.
//
// Context layout (13.60 libSceAgcDriver.sprx, sceAgcDriverCreateQueue @0x2030): queue ids whose
// type field (bits 5..9) is zero take the graphics path, which fills a driver context with
// dword +0 = 0x38 and dword +4 = queue id and returns that context. Submission reads the id
// back from +4.
//
// EOP interrupt source: the driver does not encode it; sceAgcDriverAddEqEvent (@0x6540) only
// registers a kernel EVFILT_GRAPHICS (-14) event with ident = id. The single verified pair is
// AgcCompositor.elf: CreateQueue(3, ...) at 0x2ed0d, then AddEqEvent(eq, 8, nullptr) at
// 0x2eafd in the same init path. No other mapping is inferred: every other graphics queue
// keeps interrupt source 0.
namespace Libs::Graphics::AgcQueue {

constexpr uint32_t CONTEXT_HEADER_OFFSET   = 0;
constexpr uint32_t CONTEXT_HEADER_VALUE    = 0x38;
constexpr uint32_t CONTEXT_QUEUE_ID_OFFSET = 4;

constexpr uint32_t COMPUTE_QUEUE_BEGIN = 0x20;
constexpr uint32_t COMPUTE_QUEUE_END   = 0x58;

constexpr uint32_t COMPOSITOR_SYSTEM_QUEUE_ID      = 3;
constexpr int      COMPOSITOR_SYSTEM_EOP_EVENT_ID  = 8;
constexpr int      DEFAULT_GRAPHICS_EOP_EVENT_ID   = 0;

enum class Route { Graphics, Compute };

// Queue ids handled by the driver's graphics creation path (type field zero).
constexpr bool IsGraphicsCreateQueueId(uint64_t id) {
	return id < COMPUTE_QUEUE_BEGIN;
}

// Mirrors the existing submit routing: compute ring ids go to the compute processors,
// everything else to the graphics processor.
constexpr Route RouteForQueue(uint32_t queue) {
	return (queue >= COMPUTE_QUEUE_BEGIN && queue < COMPUTE_QUEUE_END) ? Route::Compute
	                                                                   : Route::Graphics;
}

constexpr int GraphicsEopEventIdForQueue(uint32_t queue) {
	return queue == COMPOSITOR_SYSTEM_QUEUE_ID ? COMPOSITOR_SYSTEM_EOP_EVENT_ID
	                                           : DEFAULT_GRAPHICS_EOP_EVENT_ID;
}

} // namespace Libs::Graphics::AgcQueue

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_AGCQUEUEIDENTITY_H_ */
