#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/graphicsObserve.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/openxrHeadset.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/nativeVideo.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"
#include "loader/runtimeChildClient.h"
#include "loader/runtimeChildFrames.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick = 0;
	bool          busy         = false;
	bool          hdr          = false;
	bool          blank        = false;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	bool                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage(CommandScheduler& scheduler);
	[[nodiscard]] bool   PrepareSystemOverlay();
	bool     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               std::span<const Presenter::Layer> overlays, bool draw_system_overlay,
	                               Presenter::Frame* video, NativeVideoRect video_rect);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();
	void RecordReadback(vk::CommandBuffer command, vk::Buffer destination);
	[[nodiscard]] vk::Extent2D Extent() const { return m_extent; }
	[[nodiscard]] bool HdrActive() const { return m_hdr_active; }

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] bool IsHdrSupported() const noexcept { return m_hdr_supported.load(); }
	[[nodiscard]] PresentationOutputSnapshot OutputSnapshot() const { return m_output.Snapshot(); }
	void ObserveMinimized(bool minimized) { m_output.SetMinimized(minimized); }
	bool SetHdr(bool hdr) {
		const bool changed = m_hdr_active != (hdr && IsHdrSupported());
		m_hdr_requested = hdr;
		return changed;
	}

private:
	vk::SurfaceFormatKHR SelectFormat();
	void Destroy();
	void DrawLayer(vk::CommandBuffer command, const Presenter::Layer& layer,
	               const vk::Rect2D& rect);

	WindowContext&   m_window;
	PresentationOutputState m_output;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	std::atomic_bool m_hdr_supported {false};
	bool             m_hdr_requested = false;
	bool             m_hdr_active    = false;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<uint64_t>          m_image_acquired_ticks;
	std::vector<vk::Semaphore>     m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx), frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		for (uint32_t attempt = 0; !swapchain.Create(); attempt++) {
			// Initial creation may fail under host memory pressure; park the
			// presenter thread here instead of killing the run. Retried below
			// whenever a frame is presented.
			if ((attempt % 6) == 0) {
				LOGF("swapchain creation deferred (attempt %u); game continues headless\n",
				     attempt);
			}
			Common::Thread::SleepMicro(5000 * 1000);
		}
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
		// Observe only this host window. The callback changes cached status,
		// not dimensions, rendering, guest events, or protected-output policy.
		window_id = SDL_GetWindowID(owner.window);
		swapchain.ObserveMinimized((SDL_GetWindowFlags(owner.window) & SDL_WINDOW_MINIMIZED) != 0);
		watch_installed = SDL_AddEventWatch(ObserveWindowEvent, this);
		if (!watch_installed) {
			LOGF("presentation status observer unavailable: %s\n", SDL_GetError());
		}
	}
	~Impl() {
		// SDL serializes removal with dispatch. Retire userdata before any
		// of the cached state or swapchain members are destroyed.
		if (watch_installed) SDL_RemoveEventWatch(ObserveWindowEvent, this);
		present_scheduler.Wait(native_upload_tick);
		native_upload.reset();
		present_scheduler.Wait(remote_upload_tick);
		remote_upload.reset();
		child_download.reset(); // Every readback is completed before Publish.
	}
	static bool SDLCALL ObserveWindowEvent(void* userdata, SDL_Event* event) {
		auto& self = *static_cast<Impl*>(userdata);
		if (event->type < SDL_EVENT_WINDOW_FIRST || event->type > SDL_EVENT_WINDOW_LAST ||
		    event->window.windowID != self.window_id) return true;
		if (event->type == SDL_EVENT_WINDOW_MINIMIZED) {
			self.swapchain.ObserveMinimized(true);
		} else if (event->type == SDL_EVENT_WINDOW_RESTORED ||
		           event->type == SDL_EVENT_WINDOW_MAXIMIZED) {
			self.swapchain.ObserveMinimized(false);
			// Reuse the existing retained-frame refresh/recovery path; do not
			// create a separate media renderer or mark output usable here.
			self.scanout_dirty.store(true, std::memory_order_release);
		}
		return true;
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		Common::DebugCounters::Add(Common::DebugCounters::Counter::SwapchainRecreations);
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		// A minimized surface has no swapchain yet. Keep preparing guest frames
		// in the last valid format until presentation can be recreated.
		if (swapchain.Format() != vk::Format::eUndefined) {
			frames.SetFormat(swapchain.Format());
		}
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		// ps5-vsh chainSUI306: unbounded-by-design (one line per flip, ~100/run):
		// image identity decides draw/present alias splits.
		{
			static std::atomic_uint s_find {0};
			const auto fn = s_find.fetch_add(1);
			if (fn < 200 || (fn % 500) == 0) {
				LOGF("ps5-vsh: videoout-find #%u req=0x%llx img=%u\n", fn,
				     (unsigned long long)info.data.address, image_id.index);
			}
		}
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}
	void Present();
	void PrepareNativeVideo();
	void PrepareRemoteFrame();

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	ScanoutLayers<Frame>   layers;
	std::unique_ptr<Buffer> native_upload;
	uint64_t native_upload_tick = 0;
	Frame* native_frame = nullptr;
	Frame* child_guest_frame = nullptr; // Tagged only by an actual guest VideoOut submission after entry.
	std::unique_ptr<Buffer> remote_upload, child_download;
	uint64_t remote_upload_tick = 0;
	Frame* remote_frame = nullptr;
	Loader::RuntimeChild::FramePixels remote_pixels;
	Loader::RuntimeChild::Key remote_key;
	std::atomic<uint64_t> presented_remote_revision {0};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool capture_for_test = false;
	Loader::RuntimeChild::FramePixels captured_for_test;
#endif
	NativeVideoSnapshot native_snapshot;
	std::atomic<uint64_t> presented_native_revision {NativeVideoRevision()};
	std::atomic_bool      scanout_dirty {false};
	std::atomic<uint64_t> presented_overlay_revision {0};
	SDL_WindowID window_id = 0;
	bool watch_installed = false;
};

vk::SurfaceFormatKHR Swapchain::SelectFormat() {
	const auto& formats    = m_window.surface_capabilities.formats;
	auto&       graphics   = m_window.graphic_ctx;
	const auto  hdr_format = std::ranges::find_if(formats, [&](const auto& candidate) {
		return candidate.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT &&
		       IsHdrSwapchainFormat(candidate.format) &&
		       static_cast<bool>(
		           graphics.GetFormatProperties(candidate.format).optimalTilingFeatures &
		           vk::FormatFeatureFlagBits::eBlitDst);
	});
	m_hdr_supported        = Config::HdrEnabled() && hdr_format != formats.end();
	m_hdr_active           = m_hdr_requested && IsHdrSupported();
	if (m_hdr_active) {
		return *hdr_format;
	}
	if (Config::HdrEnabled() && !IsHdrSupported()) {
		LOGF("HDR10 surface unavailable; using SDR output\n");
	}
	if (formats.size() == 1 && formats.front().format == vk::Format::eUndefined) {
		return {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	}
	for (const auto& candidate: formats) {
		if (candidate.colorSpace != vk::ColorSpaceKHR::eSrgbNonlinear) {
			continue;
		}
		switch (candidate.format) {
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eR8G8B8A8Unorm: return candidate;
			default: break;
		}
	}
	EXIT("no supported UNORM swapchain format\n");
}

bool Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);
	m_output.BeginCreate();

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	if (m_window_extent.width == 0 || m_window_extent.height == 0) {
		m_output.CreationUnavailable(true);
		return false;
	}
	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	// Win32 can report a zero currentExtent while minimized even when SDL's
	// last drawable size is nonzero. Never create/acquire such a swapchain:
	// FitAspect would underflow its centered offset and produce a zero-height
	// viewport (observed dividing by zero inside Intel's vkCmdSetViewport).
	// Leaving the handle null lets the existing present retry restore it.
	if (m_extent.width == 0 || m_extent.height == 0) {
		m_output.CreationUnavailable(true);
		return false;
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	const auto format = SelectFormat();
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	bool require_readback = Loader::RuntimeChildComposedFrames();
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	require_readback = true;
#endif
	if (require_readback) {
		if (!(surface.capabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc)) {
			EXIT("runtime frame transport requires swapchain transfer-source support\n");
		}
		create_info.imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
	}
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
	const auto result = graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle);
	if (result != vk::Result::eSuccess || m_handle == nullptr) {
		LOGF("vkCreateSwapchainKHR failed: %s; will retry on next present\n", vk::to_string(result).c_str());
		m_handle = nullptr;
		m_output.CreationUnavailable(false);
		return false;
	}
	LOGF("Vulkan presentation: %s\n", m_hdr_active ? "HDR10 (PQ)" : "SDR");

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_image_acquired_ticks.assign(m_images.size(), 0);
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
	m_output.Created(m_extent.width, m_extent.height, IsHdrSupported(), m_hdr_active);
	return true;
}

Swapchain::~Swapchain() {
	Destroy();
	m_output.Shutdown();
}

void Swapchain::Destroy() {
	m_output.ResourcesDestroyed();
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_image_acquired_ticks.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	m_output.BeginRecovery(surface_lost);
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	m_window.RefreshSurfaceCapabilities();
	if (!Create()) {
		// Transient host OOM (or a minimized window): leave the handle null and let
		// the next present retry. Acquire guards null; the frame is dropped meanwhile.
		LOGF("swapchain recreation failed; will retry on next present\n");
	}
}

bool Swapchain::NeedsResize() const {
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	if (m_handle == nullptr || m_frame_index >= m_image_acquired.size()) {
		// Preserve a more specific no-drawable/create-failure state until
		// recovery attempts to acquire real resources again.
		return Status::Recreate;
	}
	// Repeated frames can wrap the semaphore ring without acquiring a new source frame.
	// Wait for the submission that consumed this slot before signaling it again.
	scheduler.Wait(m_image_acquired_ticks[m_frame_index]);
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			m_output.Failed(true, false);
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			m_output.Failed(true, false);
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			m_output.Failed(true, false);
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			m_output.Failed(true, true);
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

static vk::Rect2D FitAspect(const vk::Extent3D& source, const vk::Extent2D& target) {
	const uint64_t src_w = std::max(source.width, 1u);
	const uint64_t src_h = std::max(source.height, 1u);
	uint32_t       w     = target.width;
	uint32_t       h     = target.height;
	if (src_w * h > src_h * w) {
		h = std::max<uint32_t>(1, static_cast<uint32_t>(src_h * w / src_w));
	} else {
		w = std::max<uint32_t>(1, static_cast<uint32_t>(src_w * h / src_h));
	}
	return {{static_cast<int32_t>((target.width - w) / 2),
	         static_cast<int32_t>((target.height - h) / 2)},
	        {w, h}};
}

void Swapchain::DrawLayer(vk::CommandBuffer command, const Presenter::Layer& layer,
                          const vk::Rect2D& rect) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	// The main bus ignores alpha.
	const uint32_t flags = (layer.premultiplied_alpha ? 1u : 0u) | (m_hdr_active ? 2u : 0u) |
	                       (frame.hdr ? 4u : 0u) |
	                       (frame.image.format == vk::Format::eR16G16B16A16Sfloat ? 8u : 0u) |
	                       (layer.bus == 0 ? 16u : 0u);
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(flags), &flags);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {static_cast<float>(rect.offset.x),
	                             static_cast<float>(rect.offset.y),
	                             static_cast<float>(rect.extent.width),
	                             static_cast<float>(rect.extent.height),
	                             0.0f,
	                             1.0f};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &rect);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

bool Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      std::span<const Presenter::Layer> overlays, bool draw_system_overlay,
                                      Presenter::Frame* video, NativeVideoRect video_rect) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command      = command.Handle();
	const auto* aspect_source = source;
	for (const auto& overlay: overlays) {
		if (aspect_source == nullptr && (video == nullptr || overlay.bus != 256))
			aspect_source = overlay.frame;
	}
	const auto aspect = aspect_source != nullptr ? aspect_source->image.extent
	                                             : vk::Extent3D {3840, 2160, 1};
	const auto game_rect = !Config::StretchEnabled() && (aspect_source != nullptr || video != nullptr)
	                           ? FitAspect(aspect, m_extent) : vk::Rect2D {{0, 0}, m_extent};
	const auto fitted = video != nullptr ? FitNativeVideo(video_rect, video->image.extent.width,
	    video->image.extent.height, {game_rect.offset.x, game_rect.offset.y,
	                               game_rect.extent.width, game_rect.extent.height})
	                                    : NativeVideoPixelRect {};
	// MAIN is genuinely opaque and covers this whole application viewport.
	// Native video has its own ownership; it never replaces a retained guest bus.
	const bool draw_video = source == nullptr && video != nullptr && fitted.width && fitted.height;
	const bool convert_source  = source != nullptr && source->hdr && !m_hdr_active;
	const bool blit_source     = source != nullptr && !convert_source;
	const bool draw_overlay    = std::ranges::any_of(overlays, [](const auto& layer) {
		return layer.frame != nullptr;
	});
	const bool draw_attachment = draw_video || convert_source || draw_overlay || draw_system_overlay;
	if (draw_video) {
		video->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead);
	}
	if (blit_source) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (convert_source) {
		source->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::AccessFlagBits2::eShaderRead);
	}
	for (const auto& overlay: overlays) {
		if (overlay.frame != nullptr) {
			overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
			                       vk::AccessFlagBits2::eShaderRead);
		}
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	if (!blit_source || game_rect.extent != m_extent) {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
		if (blit_source) {
			vk::ImageMemoryBarrier after_clear = to_transfer;
			after_clear.srcAccessMask          = vk::AccessFlagBits::eTransferWrite;
			after_clear.oldLayout              = vk::ImageLayout::eTransferDstOptimal;
			vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			                           vk::PipelineStageFlagBits::eTransfer,
			                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1,
			                           &after_clear);
		}
	}
	if (blit_source) {
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
		region.srcOffsets[1].z               = 1;
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[0].x = game_rect.offset.x;
		region.dstOffsets[0].y = game_rect.offset.y;
		region.dstOffsets[1].x = game_rect.offset.x + static_cast<int>(game_rect.extent.width);
		region.dstOffsets[1].y = game_rect.offset.y + static_cast<int>(game_rect.extent.height);
		region.dstOffsets[1].z               = 1;
		vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		// Each pass blends over the color writes of the previous one.
		to_present.srcAccessMask      = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.oldLayout          = vk::ImageLayout::eColorAttachmentOptimal;
		const auto attachment_barrier = [&] {
			vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
			                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
			                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
			                           &to_present);
		};
		if (draw_video) {
			DrawLayer(vk_command, {video, 0, false},
			          {{fitted.x, fitted.y}, {fitted.width, fitted.height}});
			attachment_barrier();
		}
		if (convert_source) {
			DrawLayer(vk_command, {source, 0, false}, game_rect);
			if (draw_overlay || draw_system_overlay) {
				attachment_barrier();
			}
		}
		for (const auto& overlay: overlays) {
			if (overlay.frame == nullptr) continue;
			// App overlays follow the application viewport. AGC is a separate
			// scanout surface, fitted to the host window independently.
			const auto rect = overlay.bus == 256
			                      ? (!Config::StretchEnabled()
			                             ? FitAspect(overlay.frame->image.extent, m_extent)
			                             : vk::Rect2D {{0, 0}, m_extent})
			                      : game_rect;
			DrawLayer(vk_command, overlay, rect);
			attachment_barrier();
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
	return draw_video;
}

void Swapchain::RecordReadback(vk::CommandBuffer command, vk::Buffer destination) {
	vk::ImageMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eColorAttachmentWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
	barrier.oldLayout = vk::ImageLayout::ePresentSrcKHR;
	barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_images[m_image_index];
	barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
	    {}, 0, nullptr, 0, nullptr, 1, &barrier);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = {m_extent.width, m_extent.height, 1};
	command.copyImageToBuffer(m_images[m_image_index], vk::ImageLayout::eTransferSrcOptimal, destination, 1, &copy);
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
	barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
	    {}, 0, nullptr, 0, nullptr, 1, &barrier);
	vk::MemoryBarrier host {};
	host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	host.dstAccessMask = vk::AccessFlagBits::eHostRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	    {}, 1, &host, 0, nullptr, 0, nullptr);
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick                       = scheduler.Submit(submit);
	m_image_acquired_ticks[m_frame_index] = tick;
	return tick;
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			m_output.Failed(false, false);
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			m_output.Failed(false, false);
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			m_output.Failed(false, true);
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	m_output.Presented();
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info, bool hdr,
                                          bool headset) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	frame->hdr   = hdr;
	frame->blank = false;
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	// ps5-vsh chainSUI295: bounded present-resolve census (alias hit + gpu_mod at copy).
	// chainSUI306: + image identity + periodic sample (caps exhaust early).
	{
		static std::atomic_uint s_resolve {0};
		const auto rn = s_resolve.fetch_add(1);
		if (rn < 32 || (rn % 2000) == 0) {
			LOGF("ps5-vsh: present-resolve #%u req=0x%llx/0x%llx got=0x%llx/0x%llx copy=%ux%u<-%ux%u gpu_mod=%d img=%p\n",
			     rn, (unsigned long long)info.data.address, (unsigned long long)info.data.size,
			     (unsigned long long)image.info.data.address,
			     (unsigned long long)image.info.data.size, image.backing.extent.width,
			     image.backing.extent.height, info.extent.width, info.extent.height,
			     (int)image.IsGpuModified(), static_cast<const void*>(&image));
		}
	}
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	if (Observe::Enabled()) {
		Observe::Record(Observe::Event::Resolve,
		                {reinterpret_cast<uint64_t>(frame), reinterpret_cast<uint64_t>(&image),
		                 info.data.address, info.data.size, image.info.data.address,
		                 image.info.data.size, info.extent.width, info.extent.height,
		                 image.backing.extent.width, image.backing.extent.height,
		                 static_cast<uint64_t>(image.IsGpuModified())});
	}
	// Optional readback of the actual prepared GPU frame for rendering diagnostics.
	// The guest image and presentation flow are unaffected when capture is disabled.
	if (const char* directory = std::getenv("KYTY_FRAME_DUMP_DIR"); directory && *directory) {
		static uint32_t frame_number = 0;
		const auto number = ++frame_number;
		// ps5-vsh chainSUI306: KYTY_FRAME_DUMP_EVERY=N captures every Nth
		// frame (screen-equivalent evidence for idle force-presents, which
		// otherwise never hit the fixed sample numbers). Off when unset.
		static const uint64_t dump_every = [] {
			const char* e = std::getenv("KYTY_FRAME_DUMP_EVERY");
			return (e != nullptr && *e != '\0') ? std::strtoull(e, nullptr, 10) : 0;
		}();
		const bool capture = number == 1 || number == 30 || number == 60 || number == 90 ||
		                     number == 120 || number == 300 || number == 600 || number == 1200 ||
		                     (dump_every >= 1 && (number % dump_every) == 0);
		const bool bgra = frame_format == vk::Format::eB8G8R8A8Unorm;
		if (capture && (bgra || frame_format == vk::Format::eR8G8B8A8Unorm)) {
			const auto width = image.backing.extent.width;
			const auto height = image.backing.extent.height;
			const uint64_t bytes = uint64_t(width) * height * 4u;
			auto& download = m_impl->renderer.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
			auto [mapped, offset] = download.Map(bytes, 4u);
			EXIT_IF(mapped == nullptr);
			download.Commit();
			frame->Transit(buffer.Handle(), vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
			vk::BufferImageCopy region {};
			region.bufferOffset = offset;
			region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageExtent = {width, height, 1};
			buffer.Handle().copyImageToBuffer(frame->image.image, vk::ImageLayout::eTransferSrcOptimal,
			                                 download.Handle(), 1, &region);
			vk::BufferMemoryBarrier barrier {};
			barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
			barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = download.Handle();
			barrier.offset = offset;
			barrier.size = bytes;
			buffer.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
			const auto path = std::filesystem::path(directory) / ("frame_" + std::to_string(number) + ".ppm");
			m_impl->renderer.GetCommandScheduler().DeferPriorityOperation(
			    [&download, mapped, offset, bytes, width, height, bgra, number, path] {
				    download.Invalidate(offset, bytes);
				    std::error_code error;
				    std::filesystem::create_directories(path.parent_path(), error);
				    if (error) return;
				    std::ofstream output(path, std::ios::binary);
				    output << "P6\n" << width << ' ' << height << "\n255\n";
				    const auto* pixels = reinterpret_cast<const uint8_t*>(mapped);
				    std::vector<uint8_t> rgb(uint64_t(width) * height * 3u);
				    uint64_t nonblack = 0;
				    uint32_t max_rgb = 0;
				    for (uint64_t i = 0; i < uint64_t(width) * height; ++i) {
					    rgb[i * 3] = pixels[i * 4 + (bgra ? 2 : 0)];
					    rgb[i * 3 + 1] = pixels[i * 4 + 1];
					    rgb[i * 3 + 2] = pixels[i * 4 + (bgra ? 0 : 2)];
					    const uint8_t pixel_max = std::max({rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]});
					    if (pixel_max != 0) {
						    ++nonblack;
					    }
					    max_rgb = std::max<uint32_t>(max_rgb, pixel_max);
				    }
				    output.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
				    LOGF("ps5-vsh: frame-dump #%u (%ux%u) nonblack=%llu/%llu max_rgb=%u -> %s\n",
				         number, width, height, nonblack, uint64_t(width) * height, max_rgb,
				         path.string().c_str());
			    });
		}
	}
	if (headset) {
		// The frame holds the raw display-encoded pixels the headset needs.
		frame->Transit(buffer.Handle(), vk::ImageLayout::eTransferSrcOptimal,
		               vk::AccessFlagBits2::eTransferRead);
		Vr::OpenXrQueueEyeSource(buffer, frame->image);
	}
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	frame->hdr   = false;
	frame->blank = true;
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		Common::LockGuard lock(m_impl->present_mutex);
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	if (Observe::Enabled()) {
		Observe::Record(Observe::Event::Blank,
		                {reinterpret_cast<uint64_t>(frame), width, height,
		                 static_cast<uint64_t>(opaque), reinterpret_cast<uint64_t>(producer)});
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (!m_impl->layers.HasFrames() && !m_impl->scanout_dirty.load(std::memory_order_relaxed) &&
	    Loader::RuntimeChild::FrameSourceRevision() == m_impl->presented_remote_revision.load(std::memory_order_acquire) &&
	    m_impl->native_frame == nullptr && NativeVideoRevision() ==
	        m_impl->presented_native_revision.load(std::memory_order_acquire)) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::IsHdrSupported() const noexcept {
	return m_impl->swapchain.IsHdrSupported();
}

PresentationOutputSnapshot Presenter::GetOutputSnapshot() const {
	auto state = m_impl->swapchain.OutputSnapshot();
	if (!m_impl->watch_installed) {
		state.presentable = false;
		state.availability = PresentationAvailability::ObserverUnavailable;
	}
	return state;
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return m_impl->scanout_dirty.load(std::memory_order_acquire) || visual.active ||
	       Loader::RuntimeChild::FrameSourceRevision() != m_impl->presented_remote_revision.load(std::memory_order_acquire) ||
	       NativeVideoRevision() != m_impl->presented_native_revision.load(std::memory_order_acquire) ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::PresentIdleFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	m_impl->Present();
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		m_impl->frames.ValidateForPresent(layer.frame);
		if (auto* previous = m_impl->layers.Replace(layer); previous != nullptr) {
			m_impl->frames.Release(previous);
		}
		if (layer.frame && Loader::RuntimeChildEntryAdmitted()) { m_impl->child_guest_frame = layer.frame; }
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	Common::LockGuard lock(m_impl->present_mutex);
	if (auto* frame = m_impl->layers.Clear(bus); frame != nullptr) {
		if (m_impl->child_guest_frame == frame) { m_impl->child_guest_frame = nullptr; }
		m_impl->frames.Release(frame);
		m_impl->scanout_dirty.store(true, std::memory_order_release);
	}
}

void Presenter::Impl::PrepareNativeVideo() {
	auto next = ReadNativeVideoSnapshot();
	if (!next.frame || !next.stream->IsCurrent(next)) {
		if (native_frame) frames.Release(native_frame);
		native_frame = nullptr;
		native_snapshot = {};
		return;
	}
	if (native_frame && next.frame == native_snapshot.frame) {
		native_snapshot = std::move(next);
		return;
	}
	const auto& pixels = *next.frame;
	// This staging buffer belongs exclusively to the presentation scheduler.
	// Waiting its upload tick protects CPU writes independently of guest rendering.
	present_scheduler.Wait(native_upload_tick);
	if (!native_upload || native_upload->Size() < pixels.bytes) {
		native_upload = std::make_unique<Buffer>(window.graphic_ctx, present_scheduler,
		    MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc, pixels.bytes);
	}
	std::memcpy(native_upload->Mapped().data(), pixels.rgba.get(), pixels.bytes);
	native_upload->Flush(0, pixels.bytes);
	auto* prepared = frames.Acquire({pixels.width, pixels.height}, vk::Format::eR8G8B8A8Unorm);
	prepared->hdr = false;
	prepared->blank = false;
	prepared->Configure(window.graphic_ctx, {pixels.width, pixels.height}, vk::Format::eR8G8B8A8Unorm);
	auto& upload = present_scheduler.BeginCommand();
	prepared->Transit(upload.Handle(), vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::BufferImageCopy copy {};
	copy.bufferRowLength = pixels.stride / 4;
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = {pixels.width, pixels.height, 1};
	upload.Handle().copyBufferToImage(native_upload->Handle(), prepared->image.image,
	                                 vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	native_upload_tick = present_scheduler.Submit();
	prepared->present_tick = native_upload_tick;
	if (const char* trace = std::getenv("KYTY_MEDIA_TRACE"); trace && std::strcmp(trace, "1") == 0) {
		static std::atomic_uint reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("Native video: upload owner=%d context=%d decoder=%d generation=%llu serial=%llu "
			     "size=%ux%u pts=%lld has_pts=%d sequence=%u\n", next.stream->owner,
			     next.stream->context, next.stream->decoder, pixels.generation, next.serial,
			     pixels.width, pixels.height, pixels.pts, pixels.has_pts, pixels.sequence);
		}
	}
	if (native_frame) frames.Release(native_frame);
	native_frame = prepared;
	native_snapshot = std::move(next);
}

void Presenter::Impl::PrepareRemoteFrame() {
	const auto next = Loader::RuntimeChild::GetFrameSource();
	if (next.key != remote_key) {
		if (remote_frame) frames.Release(remote_frame);
		remote_frame = nullptr; remote_pixels = {}; remote_key = next.key;
	}
	// Retain the last application image while Home is visible, for resume even
	// if the guest has paused. Visibility is checked again at final composition.
	if (!next.mailbox || !next.visible || !next.mailbox->Read(&remote_pixels)) return;
	const auto& pixels = remote_pixels;
	present_scheduler.Wait(remote_upload_tick);
	if (!remote_upload || remote_upload->Size() < pixels.pixels.size()) {
		remote_upload = std::make_unique<Buffer>(window.graphic_ctx, present_scheduler,
		    MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc, pixels.pixels.size());
	}
	std::memcpy(remote_upload->Mapped().data(), pixels.pixels.data(), pixels.pixels.size());
	remote_upload->Flush(0, pixels.pixels.size());
	const auto format = static_cast<vk::Format>(pixels.format);
	auto* prepared = frames.Acquire({pixels.width, pixels.height}, format);
	prepared->hdr = pixels.hdr; prepared->blank = false;
	prepared->Configure(window.graphic_ctx, {pixels.width, pixels.height}, format);
	auto& upload = present_scheduler.BeginCommand();
	prepared->Transit(upload.Handle(), vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = {pixels.width, pixels.height, 1};
	upload.Handle().copyBufferToImage(remote_upload->Handle(), prepared->image.image, vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	remote_upload_tick = present_scheduler.Submit(); prepared->present_tick = remote_upload_tick;
	if (remote_frame) frames.Release(remote_frame);
	remote_frame = prepared;
}

void Presenter::Impl::Present() {
	KYTY_PROFILER_FUNCTION();
	const auto native_revision = NativeVideoRevision();
	const auto remote_revision = Loader::RuntimeChild::FrameSourceRevision();
	PrepareNativeVideo();
	PrepareRemoteFrame();

	const auto overlay_visual = GetSystemOverlayVisualState();
	auto ordered = layers.Ordered();
	const auto remote = Loader::RuntimeChild::GetFrameSource();
	if (remote_frame && remote.visible && remote.key == remote_key) ordered[0] = {remote_frame, 0, false};
	const auto* color_source = ordered[0].frame ? ordered[0].frame : layers.ColorSource();
	const bool hdr_changed = color_source != nullptr && swapchain.SetHdr(color_source->hdr);
	// Some window systems keep presenting an old swapchain after a resize.
	if (hdr_changed || swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = swapchain.AcquireNextImage(present_scheduler);
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		// present_mutex owns the prepared frames and this scheduler. Only queue submission
		// is shared with rendering, and it has its own queue_mutex.
		auto&      command = present_scheduler.BeginCommand();
		const bool draw_system_overlay = (overlay_visual.active || PerformanceOverlayEnabled()) &&
		                                 swapchain.PrepareSystemOverlay();
		// Flush/retirement during upload invalidates the snapshot before composition.
		auto* video = native_frame && native_snapshot.stream->IsCurrent(native_snapshot)
		                  ? native_frame : nullptr;
		const bool drew_video = swapchain.RecordPresentCommands(command, ordered[0].frame,
		    std::span(ordered).subspan(1), draw_system_overlay, video, native_snapshot.rect);
		const auto extent = swapchain.Extent();
		const size_t frame_bytes = size_t(extent.width) * extent.height * 4;
		const bool export_frame = Loader::RuntimeChildComposedFrames() && Loader::RuntimeChildEntryAdmitted() &&
		    (layers.HasFrames() || drew_video);
		bool capture_frame = export_frame;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		capture_frame |= capture_for_test;
#endif
		if (capture_frame) {
			if (extent.width > Loader::RuntimeChild::FrameMailbox::MaxWidth || extent.height > Loader::RuntimeChild::FrameMailbox::MaxHeight)
				EXIT("runtime frame transport extent exceeds 3840x2160\n");
			if (!child_download || child_download->Size() < frame_bytes)
				child_download = std::make_unique<Buffer>(window.graphic_ctx, present_scheduler, MemoryUsage::Download,
				    0, vk::BufferUsageFlagBits::eTransferDst, frame_bytes);
			swapchain.RecordReadback(command.Handle(), child_download->Handle());
		}
		const auto tick = swapchain.Submit(present_scheduler);
		bool exported = false;
		if (capture_frame) {
			present_scheduler.Wait(tick);
			child_download->Invalidate(0, frame_bytes);
			exported = export_frame && Loader::PublishRuntimeChildFrame(extent.width, extent.height, static_cast<uint32_t>(swapchain.Format()),
			    swapchain.HdrActive(), child_download->Mapped().first(frame_bytes));
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			if (capture_for_test) {
				captured_for_test.width = extent.width; captured_for_test.height = extent.height;
				captured_for_test.format = static_cast<uint32_t>(swapchain.Format());
				const auto data = child_download->Mapped().first(frame_bytes);
				captured_for_test.pixels.assign(data.begin(), data.end());
			}
#endif
		}
		if (drew_video) native_frame->present_tick = tick;
		for (const auto& layer: ordered) {
			if (layer.frame != nullptr) {
				layer.frame->present_tick = tick;
			}
		}
		status = swapchain.Present();
		if (Observe::Enabled()) {
			Observe::Record(Observe::Event::Present,
			                {reinterpret_cast<uint64_t>(ordered[0].frame),
			                 reinterpret_cast<uint64_t>(ordered[1].frame),
			                 static_cast<uint64_t>(status), attempt,
			                 static_cast<uint64_t>(overlay_visual.active),
			                 reinterpret_cast<uint64_t>(ordered[2].frame)});
		}
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		presented_native_revision.store(native_revision, std::memory_order_release);
		presented_remote_revision.store(remote_revision, std::memory_order_release);
		if (watch_installed && swapchain.OutputSnapshot().presentable) {
			AcknowledgeNativeVideoRevision(native_revision);
		}
		if (drew_video && watch_installed && swapchain.OutputSnapshot().presentable &&
		    native_snapshot.stream->Acknowledge(native_snapshot)) {
			if (const char* trace = std::getenv("KYTY_MEDIA_TRACE"); trace && std::strcmp(trace, "1") == 0) {
				static std::atomic_uint reports {0};
				if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
					LOGF("Native video: present-accepted owner=%d context=%d decoder=%d generation=%llu "
					     "serial=%llu pts=%lld has_pts=%d sequence=%u\n", native_snapshot.stream->owner,
					     native_snapshot.stream->context, native_snapshot.stream->decoder,
					     native_snapshot.frame->generation, native_snapshot.serial, native_snapshot.frame->pts,
					     native_snapshot.frame->has_pts, native_snapshot.frame->sequence);
				}
			}
		}
		scanout_dirty.store(false, std::memory_order_release);
		window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
		const bool retained_guest_frame = child_guest_frame && std::any_of(ordered.begin(), ordered.end(),
		    [&](const auto& layer) { return layer.frame == child_guest_frame; });
		if ((!Loader::RuntimeChildComposedFrames() || exported) &&
		    (retained_guest_frame || (drew_video && native_snapshot.frame && native_snapshot.serial))) {
			// Success means actual queue-present acceptance of guest content. Idle,
			// splash and host-overlay refreshes cannot manufacture this milestone.
			Loader::NotifyRuntimeChildPresent(window.loop.presented_frames.load(std::memory_order_relaxed));
			child_guest_frame = nullptr;
		}
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
	// Keep the existing retained-frame refresh path eligible for recovery
	// even if the guest waits for usability before submitting another flip.
	scanout_dirty.store(true, std::memory_order_release);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DroppedPresents);
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
Presenter::Frame& Presenter::PrepareSolidForTesting(uint32_t width, uint32_t height, std::array<float, 4> color) {
	Common::LockGuard lock(m_impl->present_mutex);
	auto* frame = m_impl->frames.Acquire({width, height}, vk::Format::eR8G8B8A8Unorm);
	frame->hdr = false; frame->blank = false;
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, vk::Format::eR8G8B8A8Unorm);
	vk::ClearColorValue clear {}; std::copy(color.begin(), color.end(), clear.float32.begin());
	frame->Clear(m_impl->present_scheduler.BeginCommand(), clear);
	frame->present_tick = m_impl->present_scheduler.Submit();
	return *frame;
}
Loader::RuntimeChild::FramePixels Presenter::CaptureComposedForTesting() {
	Common::LockGuard lock(m_impl->present_mutex);
	m_impl->captured_for_test = {}; m_impl->capture_for_test = true;
	m_impl->Present(); m_impl->capture_for_test = false;
	return std::move(m_impl->captured_for_test);
}
#endif

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
