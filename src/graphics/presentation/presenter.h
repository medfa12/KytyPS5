#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"
#include "graphics/presentation/scanoutLayers.h"
#include "graphics/presentation/presentationOutputState.h"

#include <memory>
#include <span>
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include "loader/runtimeChildFrames.h"
#include <array>
#endif

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct WindowContext;

class Presenter final {
public:
	struct Frame;
	using Layer = ScanoutLayers<Frame>::Layer;

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame&         PrepareFrame(CommandBuffer& command, const ImageInfo& info,
	                                          bool hdr = false, bool headset = false);
	[[nodiscard]] Frame&         PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                               CommandBuffer* producer = nullptr);
	[[nodiscard]] bool           PresentLastFrame();
	[[nodiscard]] bool           IsGuestPaused() const noexcept;
	[[nodiscard]] bool           IsHdrSupported() const noexcept;
	// Copies only cached lifecycle state under a short lock; never waits on
	// present_mutex, a GPU fence, queue submission, or a guest callback.
	[[nodiscard]] PresentationOutputSnapshot GetOutputSnapshot() const;
	[[nodiscard]] bool           NeedsSystemOverlayRefresh() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	// Host idle fill is not a guest MAIN plane and must not retain one.
	void                         PresentIdleFrame();
	void                         Present(std::span<const Layer> layers);
	void                         ClearLayer(int bus);
	void                         Discard(Frame& frame);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	Frame& PrepareSolidForTesting(uint32_t width, uint32_t height, std::array<float, 4> color);
	Loader::RuntimeChild::FramePixels CaptureComposedForTesting();
#endif

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
