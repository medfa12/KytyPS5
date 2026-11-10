#include "graphics/shader/shaderVertexBuffers.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexBufferTests: failed: %s\n", text);
		std::abort();
	}
}

void Add(ShaderVertexInputInfo& info, uint64_t base, uint32_t stride, uint32_t records,
         uint32_t fetch_index = 0) {
	Check(info.resources_num < ShaderVertexInputInfo::RES_MAX, "test fixture capacity exceeded");
	const int index = info.resources_num++;
	auto&     r     = info.resources[index];
	r.fields[1]     = stride << 16u;
	r.UpdateAddress48(base);
	r.fields[2] = records;
	r.fields[3] = (static_cast<uint32_t>(Prospero::BufferFormat::k32Float) << 12u) |
	              DstSel(4, 0, 0, 1) | (2u << 28u);
	info.resources_dst[index] = {.register_start = index * 4,
	                             .registers_num  = 1,
	                             .attr_id        = index + 7,
	                             .fetch_index    = fetch_index,
	                             .buffer_index   = -1};
}

void BuildAndCheck(ShaderVertexInputInfo& info) {
	const auto before = info;
	Check(ShaderBuildVertexBufferBindings(info), "valid vertex descriptors were rejected");
	Check(info.resources_num == before.resources_num, "resource count changed");
	for (int i = 0; i < info.resources_num; i++) {
		const auto& r  = info.resources[i];
		const auto& rd = info.resources_dst[i];
		for (int word = 0; word < 4; word++) {
			Check(r.fields[word] == before.resources[i].fields[word], "descriptor was rewritten");
		}
		const auto& previous = before.resources_dst[i];
		Check(rd.register_start == previous.register_start && rd.registers_num == previous.registers_num &&
		          rd.attr_id == previous.attr_id && rd.fetch_index == previous.fetch_index,
		      "attribute identity, destination or fetch rate changed");
		Check(rd.buffer_index >= 0 && rd.buffer_index < info.buffers_num, "invalid binding index");
		const auto& b = info.buffers[rd.buffer_index];
		Check(b.stride == r.Stride() && b.num_records == r.NumRecords() && b.fetch_index == rd.fetch_index,
		      "binding changed a descriptor's stride, record bound or vertex/instance indexing");
		Check(b.addr <= r.Base48(), "rebasing underflowed the attribute offset");

		// pipelineCache builds attribute offsets as Base48 - binding.addr. renderDraw sizes
		// strided bindings as uint64_t(stride) * records and passes each size separately to
		// bindVertexBuffers2, even if overlapping bindings share one backing allocation.
		const uint64_t offset = r.Base48() - b.addr;
		if (b.stride != 0) {
			Check(uint64_t {b.stride} * b.num_records == r.GetSize(), "binding byte bound changed");
			Check(b.addr + offset == r.Base48(), "first vertex fetch address changed");
			if (r.NumRecords() != 0) {
				const uint64_t last = uint64_t {r.NumRecords() - 1u} * r.Stride();
				Check(b.addr + offset + last == r.Base48() + last, "last vertex fetch address changed");
			}
		}
	}
}

void TestUnequalCounts() {
	// Same address and nearby addresses, in both count orders: neither min nor max
	// is a valid replacement for the descriptors' independent record bounds.
	for (const uint32_t first: {4u, 9u}) {
		for (const uint64_t delta: {uint64_t {0}, uint64_t {8}}) {
			ShaderVertexInputInfo info {};
			Add(info, 0x100000008ull, 32, first);
			Add(info, 0x100000008ull + delta, 32, first == 4 ? 9 : 4);
			BuildAndCheck(info);
			Check(info.buffers_num == 2, "unequal record counts were merged");
			Check(info.resources_dst[0].buffer_index == 0 && info.resources_dst[1].buffer_index == 1,
			      "unequal-count descriptors lost their independent bindings");
			Check(info.buffers[0].addr == info.resources[0].Base48() &&
			          info.buffers[1].addr == info.resources[1].Base48(),
			      "separate views were spuriously rebased");
		}
	}

	ShaderVertexInputInfo empty_view {};
	Add(empty_view, 0x2000, 32, 0);
	Add(empty_view, 0x2000, 32, 4);
	BuildAndCheck(empty_view);
	Check(empty_view.buffers_num == 2 && empty_view.buffers[0].num_records == 0,
	      "empty descriptor acquired the non-empty alias's bound");
}

void TestCompatibleMergeAndMapping() {
	ShaderVertexInputInfo info {};
	Add(info, 0x1008, 32, 4);
	Add(info, 0x100c, 32, 9);
	Add(info, 0x1000, 32, 4);
	Add(info, 0x1004, 32, 9);
	// Per-attribute formats and channel mappings remain independent of bindings.
	info.resources[2].fields[3] = (static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) << 12u) |
	                            DstSel(4, 1, 0, 1) | (2u << 28u);
	BuildAndCheck(info);
	Check(info.buffers_num == 2, "compatible equal-count views did not merge");
	Check(info.resources_dst[0].buffer_index == 0 && info.resources_dst[1].buffer_index == 1 &&
	          info.resources_dst[2].buffer_index == 0 && info.resources_dst[3].buffer_index == 1,
	      "interleaved count groups mapped attributes to the wrong binding");
	Check(info.buffers[0].addr == 0x1000 && info.buffers[0].num_records == 4 &&
	          info.buffers[1].addr == 0x1004 && info.buffers[1].num_records == 9,
	      "compatible merge changed group bases or counts");
	Check(info.resources[0].Base48() - info.buffers[0].addr == 8 &&
	          info.resources[1].Base48() - info.buffers[1].addr == 8,
	      "earlier attributes did not retain their offsets after rebasing");
	BuildAndCheck(info);
	Check(info.buffers_num == 2, "rebuilding accumulated stale bindings");
}

void TestIncompatibleBindings() {
	ShaderVertexInputInfo info {};
	Add(info, 0x3000, 32, 8);
	Add(info, 0x3000, 32, 8, 1);
	Add(info, 0x3000, 16, 8);
	Add(info, 0x3020, 32, 8); // Exactly one stride away is not an attribute offset.
	BuildAndCheck(info);
	Check(info.buffers_num == 4, "different stride, fetch rate or vertex origin was merged");
	for (int i = 0; i < info.resources_num; i++) {
		Check(info.resources_dst[i].buffer_index == i, "incompatible view mapped to an earlier binding");
	}

	ShaderVertexInputInfo constant {};
	Add(constant, 0x4000, 0, 1);
	Add(constant, 0x4000, 0, 1);
	BuildAndCheck(constant);
	Check(constant.buffers_num == 2, "zero-stride grouping behavior changed");
}

void TestWideDescriptorsAndCapacity() {
	ShaderVertexInputInfo wide {};
	constexpr uint64_t base = 0x1234fffffff0ull;
	Add(wide, base + 8, 0x3fff, UINT32_MAX);
	Add(wide, base, 0x3fff, UINT32_MAX);
	Add(wide, base + 4, 0x3fff, UINT32_MAX - 1u);
	BuildAndCheck(wide);
	Check(wide.buffers_num == 2 && wide.buffers[0].addr == base,
	      "wide equal-count descriptors failed to merge or unequal counts merged");
	Check(wide.resources[0].GetSize() == uint64_t {0x3fff} * UINT32_MAX &&
	          wide.resources[0].GetSize() > UINT32_MAX,
	      "large descriptor extent overflowed 32 bits");

	ShaderVertexInputInfo full {};
	for (int i = 0; i < ShaderVertexInputInfo::RES_MAX; i++) {
		Add(full, 0x5000, 32, static_cast<uint32_t>(i + 1));
	}
	BuildAndCheck(full);
	Check(full.buffers_num == ShaderVertexInputInfo::RES_MAX, "maximum independent binding count failed");
	for (int i = 0; i < full.resources_num; i++) {
		Check(full.resources_dst[i].buffer_index == i, "full binding array mapping was corrupted");
	}
	full.resources_num = 0;
	BuildAndCheck(full);
	Check(full.buffers_num == 0, "empty input retained stale bindings");

	for (const int count: {-1, ShaderVertexInputInfo::RES_MAX + 1}) {
		ShaderVertexInputInfo invalid {};
		invalid.resources_num = count;
		invalid.buffers_num   = 7;
		Check(!ShaderBuildVertexBufferBindings(invalid), "invalid resource count was accepted");
		Check(invalid.buffers_num == 7, "invalid count changed bindings before rejection");
	}
}

} // namespace

int main() {
	TestUnequalCounts();
	TestCompatibleMergeAndMapping();
	TestIncompatibleBindings();
	TestWideDescriptorsAndCapacity();
	std::puts("ShaderVertexBufferTests: all cases passed");
	return 0;
}
