#include "graphics/shader/shaderVertexBuffers.h"

#include <algorithm>

namespace Libs::Graphics {

bool ShaderBuildVertexBufferBindings(ShaderVertexInputInfo& info) {
	if (info.resources_num < 0 || info.resources_num > ShaderVertexInputInfo::RES_MAX) {
		return false;
	}

	info.buffers_num = 0;
	for (int ri = 0; ri < info.resources_num; ri++) {
		const auto& r           = info.resources[ri];
		auto&       destination = info.resources_dst[ri];
		int         bi          = 0;
		for (; bi < info.buffers_num; bi++) {
			auto& b = info.buffers[bi];
			// One host binding has one stride, fetch rate and bounded size (stride * records).
			// Overlapping descriptors with different record counts need separate bindings;
			// taking either count would truncate one view or extend the other. The renderer
			// may still share their backing allocation while retaining these binding bounds.
			if (b.stride == r.Stride() && b.fetch_index == destination.fetch_index &&
			    b.num_records == r.NumRecords()) {
				const uint64_t base = std::min(r.Base48(), b.addr);
				if (r.Base48() - base < b.stride && b.addr - base < b.stride) {
					b.addr = base;
					break;
				}
			}
		}

		if (bi == info.buffers_num) {
			// At most one binding per resource; the resource count was checked above.
			info.buffers[bi] = {.addr        = r.Base48(),
			                    .stride      = r.Stride(),
			                    .num_records = r.NumRecords(),
			                    .fetch_index = destination.fetch_index};
			info.buffers_num++;
		}
		destination.buffer_index = bi;
	}
	return true;
}

} // namespace Libs::Graphics
