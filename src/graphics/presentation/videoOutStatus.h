#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSTATUS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSTATUS_H_

#include "common/abi.h"
#include "graphics/presentation/presentationOutputState.h"

#include <cstddef>
#include <cstdint>

namespace Libs::VideoOut {

struct VideoOutPortOutputState {
	bool opened = false; // Live generation and not closing, not merely a nonzero handle.
	int bus = -1;
};

// Configured shared MAIN display, independent of source handles/buffers and
// host drawable size. The driver initializes this before admitting source ports.
struct VideoOutEndpointOutputState {
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t output_mode = 0;

	// Match VideoOutPublishSystemOutputMode's physical MAIN routing. SUB and
	// HMD cannot change this endpoint, even when mirrored in the same window.
	// Caller serializes mutation with snapshot reads.
	void SetOutputMode(int bus, uint64_t mode) noexcept {
		if (bus == 0 || bus == 1 || bus == 256) output_mode = mode;
	}
};

// Host API, NOT a guest ABI structure or MediaFramework's Result56. Consumers
// must encode selector 0 (output fields) and selector 1 (raw port flags)
// separately, and perform their own initialized/client admission checks.
struct VideoOutOutputSnapshot {
	Graphics::PresentationOutputSnapshot presentation;
	bool initialized = false; // VideoOut driver lifetime, separate from swapchain initialization.
	bool port_open = false; // Selected MAIN/MAIN_SYSTEM source only; diagnostic, not admission.
	bool connected = false; // Attached virtual main display; independent of port/host surface usability.
	bool usable = false; // Connected + valid endpoint mode + initialized, presentable resources.
	int bus = -1;
	uint32_t guest_width = 0;
	uint32_t guest_height = 0;
	uint64_t output_mode = 0;
	uint32_t resolution = 0;
	uint32_t dynamic_range = 0;
	uint64_t refresh_rate = 0;
	uint64_t flags = 0; // No authenticated HDCP backend: all protected-output bits remain zero.
};

// Pure composition used by the actual locked driver query and CPU-only tests.
// MAIN takes precedence over MAIN_SYSTEM for source diagnostics only. No source
// handle can create or gate physical output readiness. Guest mode is the endpoint
// configuration, never source-buffer dimensions or the host drawable extent.
[[nodiscard]] VideoOutOutputSnapshot ComposeVideoOutOutputSnapshot(
    bool initialized, const Graphics::PresentationOutputSnapshot& presentation,
    const VideoOutEndpointOutputState& endpoint,
    const VideoOutPortOutputState* ports, size_t port_count, bool detect_resolution,
    uint32_t vblank_frequency);

// Safe before graphics initialization and during/after shutdown. Snapshotting
// pins the driver lifetime, copies source/endpoint configuration and cached presenter
// state under short locks, then releases all locks before returning.
[[nodiscard]] VideoOutOutputSnapshot VideoOutGetOutputSnapshot();

// Pure firmware payload interpreters. These do not query a device, admit a
// media client or assert a protected-output capability. The two port helpers
// take MediaFramework selector 1's raw payload; the HDCP helpers take selector
// 0's output payload. These payloads must not be interchanged.
KYTY_SYSV_ABI int VideoOutPortStatusIsUsable(const void* status);
KYTY_SYSV_ABI int VideoOutPortStatusIsDeviceConnected(const void* status);
KYTY_SYSV_ABI int VideoOutOutputStatusIsHdcp14Enabled(const void* status);
KYTY_SYSV_ABI int VideoOutOutputStatusIsHdcp23Enabled(const void* status);

} // namespace Libs::VideoOut

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSTATUS_H_
