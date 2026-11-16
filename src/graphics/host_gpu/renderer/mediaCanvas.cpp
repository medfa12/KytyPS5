#include "common/logging/log.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "libs/mediaFramework.h"

#include <atomic>
#include <bit>
#include <cstring>

namespace Libs::Graphics {

void RenderExecutor::DrawMediaCanvas(CommandBuffer& buffer, int meta, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height) {
	static std::atomic_uint traces {0};
	const bool              trace = MediaFramework::TraceEnabled() && traces.fetch_add(1) < 12;
	if (trace)
		Log::WriteToConsoleAndLog(fmt::sprintf(
		    "MediaCanvas: execute id=%08x target=%llx rect=%u,%u %ux%u\n", meta,
		    static_cast<unsigned long long>(buffer.GetRegisters().GetRenderTarget(0).base.addr), x,
		    y, width, height));
	if (!width || !height || width > 8192 || height > 8192 ||
	    buffer.GetRegisters().GetRenderTarget(0).base.addr == 0)
		return;
	RenderColorInfo target;
	ResetBindings();
	ResolveRenderColorTarget(buffer, target, 0, 0, true, true);
	ResetBindings();
	const auto extent = target.Extent();
	if (trace)
		Log::WriteToConsoleAndLog(
		    fmt::sprintf("MediaCanvas: extent=%ux%u\n", extent.width, extent.height));
	if (x >= extent.width || y >= extent.height || width > extent.width - x ||
	    height > extent.height - y || target.desc.info.samples != 1 ||
	    target.desc.view_info.layer_count != 1)
		return;
	auto&      cache  = m_context.GetTextureCache();
	auto&      image  = cache.GetImage(target.image_id);
	const auto format = image.info.pixel_format;
	const bool bgra   = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
	const bool half   = format == vk::Format::eR16G16B16A16Sfloat;
	if (!bgra && !half && format != vk::Format::eR8G8B8A8Unorm &&
	    format != vk::Format::eR8G8B8A8Srgb) {
		LOGF("MediaCanvas: unsupported target format=%u\n", static_cast<unsigned>(format));
		return;
	}
	std::vector<uint8_t> rgba;
	if (!MediaFramework::CopyCanvasFrame(meta, width, height, rgba)) return;
	if (trace) Log::WriteToConsoleAndLog("MediaCanvas: decoded frame snapshot\n");
	const size_t size     = rgba.size() * (half ? 2 : 1);
	auto&        staging  = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Upload);
	auto [mapped, offset] = staging.Map(size, 16);
	if (half) {
		auto* out = reinterpret_cast<uint16_t*>(mapped);
		for (size_t i = 0; i < rgba.size(); ++i) {
			const uint32_t bits = std::bit_cast<uint32_t>(float(rgba[i]) / 255.0f);
			out[i] = rgba[i] == 0 ? 0 : static_cast<uint16_t>(((bits + 0x1000) >> 13) - 0x1c000);
		}
	} else {
		std::memcpy(mapped, rgba.data(), size);
		if (bgra)
			for (size_t i = 0; i < size; i += 4)
				std::swap(mapped[i], mapped[i + 2]);
	}
	staging.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset     = offset;
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, target.desc.view_info.base_level,
	                         target.desc.view_info.base_layer, 1};
	copy.imageOffset      = vk::Offset3D {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
	copy.imageExtent      = vk::Extent3D {width, height, 1};
	image.Upload(std::span(&copy, 1), staging.Handle(), offset, size);
	cache.MarkGpuWritten(target.image_id);
	static std::atomic_uint count {0};
	if (count.fetch_add(1) < 8)
		LOGF("MediaCanvas: uploaded meta=%08x rect=%u,%u %ux%u target=%llx\n", meta, x, y, width,
		     height, static_cast<unsigned long long>(target.desc.info.data.address));
}
} // namespace Libs::Graphics
