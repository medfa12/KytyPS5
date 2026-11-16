#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SCANOUTLAYERS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SCANOUTLAYERS_H_

#include <array>
#include <utility>

namespace Libs::Graphics {

// Guest bus numbers are not host plane indices. SUB has no window scanout;
// HMD2_MAIN mirrors into the application plane, never the AGC system plane.
constexpr int ScanoutLayerIndex(int bus) {
	switch (bus) {
		case 0:
		case 32: return 0;
		case 1: return 1;
		case 256: return 2;
		default: return -1;
	}
}

// CPU-side retained ownership; callers serialize access and release returned
// frames through their frame pool (which waits for the last GPU submission).
template <typename Frame> class ScanoutLayers {
public:
	struct Layer {
		Frame* frame = nullptr;
		int    bus = 0;
		bool   premultiplied_alpha = false;
	};

	[[nodiscard]] const std::array<Layer, 3>& Ordered() const { return m_layers; }
	[[nodiscard]] bool HasFrames() const {
		for (const auto& layer: m_layers) {
			if (layer.frame != nullptr) return true;
		}
		return false;
	}
	[[nodiscard]] Frame* Replace(const Layer& layer) {
		const int index = ScanoutLayerIndex(layer.bus);
		if (index < 0) return layer.frame;
		return std::exchange(m_layers[index], layer).frame;
	}
	[[nodiscard]] Frame* Clear(int bus) {
		const int index = ScanoutLayerIndex(bus);
		// MAIN and HMD share a mirror plane, but closing either must not remove
		// a frame retained on behalf of the other port.
		if (index < 0 || m_layers[index].bus != bus) return nullptr;
		return std::exchange(m_layers[index], Layer {}).frame;
	}
	[[nodiscard]] Frame* ColorSource() const {
		// Keep blank/black flips in the current color space. A real application
		// frame owns output HDR; system-only scanout can select HDR as well.
		auto* frame = m_layers[0].frame != nullptr ? m_layers[0].frame : m_layers[2].frame;
		return frame != nullptr && !frame->blank ? frame : nullptr;
	}

private:
	std::array<Layer, 3> m_layers {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SCANOUTLAYERS_H_
