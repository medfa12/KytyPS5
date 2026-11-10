#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTATIONOUTPUTSTATE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTATIONOUTPUTSTATE_H_

#include <cstdint>
#include <mutex>

namespace Libs::Graphics {

enum class PresentationAvailability : uint8_t {
	Uninitialized, Ready, Recovering, NoDrawable, CreateFailed, AcquireFailed, PresentFailed,
	SurfaceLost, ObserverUnavailable, Shutdown
};

// Last observed virtual-display attachment/resource state, NOT guest dimensions
// or a DRM/HDMI capability. Minimize and a lost Vulkan surface suspend host
// presentation, not the attached emulated monitor. Teardown disconnects it.
struct PresentationOutputSnapshot {
	bool connected = false;
	bool initialized = false; // Complete, nonzero-extent swapchain resources exist.
	bool presentable = false;
	bool hdr_supported = false;
	bool hdr_active = false;
	bool minimized = false; // Host visibility, never a physical disconnect.
	uint32_t drawable_width = 0;
	uint32_t drawable_height = 0;
	uint64_t generation = 0;
	PresentationAvailability availability = PresentationAvailability::Uninitialized;
};

// Shared by the real swapchain lifecycle and CPU-only transition tests. Its
// short lock protects only this value: never Vulkan calls, waits or callbacks.
class PresentationOutputState final {
public:
	[[nodiscard]] PresentationOutputSnapshot Snapshot() const {
		std::lock_guard lock(m_mutex);
		return m_state;
	}
	void BeginCreate() {
		std::lock_guard lock(m_mutex);
		ClearResources();
		m_state.connected = true; // Caller has a real Vulkan surface, not just a window.
		m_state.availability = PresentationAvailability::Recovering;
	}
	void Created(uint32_t width, uint32_t height, bool hdr_supported, bool hdr_active) {
		std::lock_guard lock(m_mutex);
		if (width == 0 || height == 0) {
			ClearResources();
			m_state.availability = PresentationAvailability::NoDrawable;
			return;
		}
		m_state.initialized = true;
		m_state.drawable_width = width;
		m_state.drawable_height = height;
		m_state.hdr_supported = hdr_supported;
		m_state.hdr_active = hdr_active;
		++m_state.generation;
		// Initial successful resource acquisition is enough to accept a first
		// flip. Recovery after a failed operation needs an actual good present.
		m_state.presentable = !m_needs_present && !m_state.minimized;
		m_state.availability = m_state.minimized ? PresentationAvailability::NoDrawable
		                      : m_needs_present ? PresentationAvailability::Recovering
		                                        : PresentationAvailability::Ready;
	}
	void CreationUnavailable(bool zero_extent) {
		std::lock_guard lock(m_mutex);
		ClearResources();
		m_state.availability = zero_extent ? PresentationAvailability::NoDrawable
		                                   : PresentationAvailability::CreateFailed;
	}
	void Failed(bool acquire, bool surface_lost) {
		std::lock_guard lock(m_mutex);
		m_needs_present = true;
		m_state.presentable = false;
		m_state.availability = surface_lost ? PresentationAvailability::SurfaceLost
		                         : acquire ? PresentationAvailability::AcquireFailed
		                                   : PresentationAvailability::PresentFailed;
	}
	void BeginRecovery(bool surface_lost) {
		std::lock_guard lock(m_mutex);
		m_needs_present = true;
		ClearResources();
		m_state.availability = surface_lost ? PresentationAvailability::SurfaceLost
		                                   : PresentationAvailability::Recovering;
	}
	void ResourcesDestroyed() {
		std::lock_guard lock(m_mutex);
		ClearResources();
		// Releasing a swapchain is not unplugging the emulated output endpoint.
		m_state.availability = PresentationAvailability::Recovering;
	}
	void Presented() {
		std::lock_guard lock(m_mutex);
		if (!m_state.connected || !m_state.initialized || m_state.minimized) return;
		m_needs_present = false;
		m_state.presentable = true;
		m_state.availability = PresentationAvailability::Ready;
	}
	void SetMinimized(bool minimized) {
		std::lock_guard lock(m_mutex);
		if (m_state.minimized == minimized) return;
		m_state.minimized = minimized;
		m_needs_present = true;
		m_state.presentable = false;
		m_state.availability = minimized ? PresentationAvailability::NoDrawable
		                                  : PresentationAvailability::Recovering;
	}
	void Shutdown() {
		std::lock_guard lock(m_mutex);
		const auto generation = m_state.generation;
		m_state = {};
		m_state.generation = generation;
		m_state.availability = PresentationAvailability::Shutdown;
		m_needs_present = false;
	}

private:
	void ClearResources() {
		m_state.initialized = false;
		m_state.presentable = false;
		m_state.hdr_supported = false;
		m_state.hdr_active = false;
		m_state.drawable_width = 0;
		m_state.drawable_height = 0;
	}
	mutable std::mutex m_mutex;
	PresentationOutputSnapshot m_state;
	bool m_needs_present = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTATIONOUTPUTSTATE_H_
