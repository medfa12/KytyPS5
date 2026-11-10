#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSYSTEMEVENTS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSYSTEMEVENTS_H_

#include <cstdint>

namespace Libs::VideoOut {

// Native libSceVideoOut 13.60: SysAddSetModeEvent2 (0xdfc0 -> 0xdc40).
// Head validation accepts MAIN=0 and VR=1, independent of open scanout ports.
constexpr uintptr_t VideoOutSystemSetModeEventId(unsigned head) {
	return (uint64_t {0x73} << 48) | (uint64_t {head} << 40) | (uint64_t {1} << 32);
}

// Physical output notifications outlive individual application/system ports.
// Called after a successful guest output configuration, never for a flip.
void VideoOutPublishSystemOutputMode(int bus, uint64_t mode);
void VideoOutResetSystemOutputEvents();

} // namespace Libs::VideoOut

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTSYSTEMEVENTS_H_
