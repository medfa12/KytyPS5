#include "graphics/presentation/videoOutStatus.h"

#include "libs/errno.h"

#include <cstdint>
#include <cstring>

namespace Libs::VideoOut {

VideoOutOutputSnapshot ComposeVideoOutOutputSnapshot(
    bool initialized, const Graphics::PresentationOutputSnapshot& presentation,
    const VideoOutEndpointOutputState& endpoint,
    const VideoOutPortOutputState* ports, size_t port_count, bool detect_resolution,
    uint32_t vblank_frequency) {
	VideoOutOutputSnapshot result {};
	result.initialized = initialized;
	result.presentation = presentation;
	result.connected = initialized && presentation.connected;
	if (!initialized) return result;
	const VideoOutPortOutputState* selected = nullptr;
	for (size_t i = 0; ports != nullptr && i < port_count; ++i) {
		const auto& port = ports[i];
		if (!port.opened || (port.bus != 0 && port.bus != 256)) continue;
		if (selected == nullptr || port.bus == 0) selected = &port;
	}
	if (selected != nullptr) {
		result.port_open = true;
		result.bus = selected->bus;
	}
	result.guest_width = endpoint.width;
	result.guest_height = endpoint.height;
	result.output_mode = endpoint.output_mode;
	const bool mode_valid = endpoint.width != 0 && endpoint.height != 0 && endpoint.output_mode != 0;
	result.usable = result.connected && presentation.initialized && presentation.presentable &&
	                mode_valid;
	if (mode_valid) {
		// Same guest resolution-detection policy as VideoOutGetOutputStatus,
		// never the host's independently scaled/minimized window dimensions.
		result.resolution = detect_resolution && endpoint.width < 3840 && endpoint.height < 2160
		                        ? 1u : 2u;
		// Report selected host output color space, not merely HDR support.
		result.dynamic_range = presentation.initialized && presentation.hdr_active ? 2u : 1u;
		result.refresh_rate = endpoint.output_mode == 15 || vblank_frequency >= 119 ? 13u : 3u;
	}
	return result;
}

// 13.60 libSceVideoOut 0x15110 (local4.03 0x12a00), d7eh3psk1J8.
__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutPortStatusIsUsable(const void* status) {
	if (status == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	uint32_t flags = 0;
	std::memcpy(&flags, status, sizeof(flags));
	return static_cast<int>((~flags) & 1u);
}

// 13.60 0x15130, q9HFykGcuWU. Only the first byte is read by firmware.
__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutPortStatusIsDeviceConnected(const void* status) {
	if (status == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	return (*static_cast<const uint8_t*>(status) & 0x20u) == 0;
}

// 13.60 0x172f0, v9gmbZSAovI. memcpy also permits unaligned guest payloads.
__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutOutputStatusIsHdcp14Enabled(const void* status) {
	if (status == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	uint64_t flags = 0;
	std::memcpy(&flags, static_cast<const uint8_t*>(status) + 0x10, sizeof(flags));
	return (flags & 0xb00000000ull) == 0x300000000ull;
}

// 13.60 0x172c0, ys239q8VgXY.
__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int
VideoOutOutputStatusIsHdcp23Enabled(const void* status) {
	if (status == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	uint64_t flags = 0;
	std::memcpy(&flags, static_cast<const uint8_t*>(status) + 0x10, sizeof(flags));
	return (flags & 0xb00000000ull) == 0xb00000000ull;
}

} // namespace Libs::VideoOut
