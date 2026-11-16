#include "libs/mediaFrameworkEsvm.h"
#include "libs/mediaFramework.h"
#include "common/logging/log.h"

#include "graphics/presentation/videoOutStatus.h"
#include "loader/runtimeProcessIdentity.h"

#include <cstring>
#include <atomic>

namespace Libs::MediaFramework {

EsvmPresentationSnapshot EncodeEsvmPresentationSnapshot(const VideoOut::VideoOutOutputSnapshot& source) {
	EsvmPresentationSnapshot result {};
	// 13.60 selector-0 payload: u32 resolution, u32 dynamic range,
	// u64 refresh enumeration, u64 output flags; remaining reserved bytes zero.
	std::memcpy(result.output_status.data(), &source.resolution, sizeof(source.resolution));
	std::memcpy(result.output_status.data() + 4, &source.dynamic_range, sizeof(source.dynamic_range));
	std::memcpy(result.output_status.data() + 8, &source.refresh_rate, sizeof(source.refresh_rate));
	std::memcpy(result.output_status.data() + 0x10, &source.flags, sizeof(source.flags));
	// Independent active-low bits, exactly as the pure firmware interpreters
	// consume them. A valid snapshot of absent/lost/minimized output is NOT a
	// zero-filled port payload (which would falsely mean connected + usable).
	const uint32_t port_flags = (source.usable ? 0u : 1u) | (source.connected ? 0u : 0x20u);
	std::memcpy(result.port_status.data(), &port_flags, sizeof(port_flags));
	return result;
}

bool ReadEsvmGraphicsSnapshot(int owner_pid, EsvmPresentationSnapshot* out) {
	if (out == nullptr || owner_pid <= 0 || !Loader::IsGuestProcessIdentityActive(owner_pid)) { return false; }
	// Framework ownership/admission is checked by the service, not inferred from
	// VideoOut's global hardware snapshot. No framework locks span this call.
	const auto source = VideoOut::VideoOutGetOutputSnapshot();
	if (TraceEnabled()) {
		static std::atomic_uint samples {0};
		if (samples.fetch_add(1) < 32) {
			LOGF("MediaFramework output: pid=%d driver=%d connected=%d usable=%d port=%d bus=%d guest=%ux%u swapchain=%d presentable=%d state=%u drawable=%ux%u\n",
			     owner_pid, source.initialized, source.connected, source.usable, source.port_open, source.bus,
			     source.guest_width, source.guest_height, source.presentation.initialized, source.presentation.presentable,
			     static_cast<unsigned>(source.presentation.availability), source.presentation.drawable_width,
			     source.presentation.drawable_height);
		}
	}
	*out = EncodeEsvmPresentationSnapshot(source);
	return true;
}
} // namespace Libs::MediaFramework
