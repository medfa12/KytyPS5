#ifndef KYTY_GRAPHICS_OBSERVE_H
#define KYTY_GRAPHICS_OBSERVE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::Observe {

enum class Event : uint64_t {
	Shader = 1, Emit, Submit, Consume, Draw, Outcome, Resolve, Prepare, Present, Blank,
	Process, Dispatch, Indirect, Target, Texture,
};
enum class Outcome : uint64_t {
	ZeroCount = 1, Metadata, NoVertexShader, Topology, PrepareFailed, RectExports,
	Encoded, MeshZeroCount, PreparedReturned,
};

constexpr uint32_t CAPACITY = 4096;
constexpr uint64_t MAGIC = 0x3142534f5550474b; // little endian "KGPUOSB1"
using Values = std::array<uint64_t, 13>;

// Append-only: a suspended producer never blocks another producer or reader.
// Readers accept only commit == slot_index+1 (acquire when mapped in-process).
// No slot is reused. File/process snapshots may omit in-flight records.
struct alignas(64) Slot {
	std::atomic<uint64_t> commit {0};
	uint64_t event = 0;
	uint64_t tick = 0;
	Values values {};
};

struct Buffer {
	uint64_t magic = MAGIC;
	uint32_t version = 1;
	uint32_t capacity = CAPACITY;
	uint64_t pid = 0;
	uint64_t frequency = 0;
	uint64_t start_tick = 0;
	uint64_t delay_ticks = 0;
	std::atomic<uint64_t> eligible {0};
	std::atomic<uint64_t> dropped {0};
	std::atomic<uint64_t> skipped {0};
	std::array<std::atomic<uint64_t>, 16> totals {};
	std::atomic<uint64_t> ready {0};
	std::array<uint64_t, 6> reserved {};
	std::array<Slot, CAPACITY> slots {};

	void Append(Event event, uint64_t tick, const Values& values) noexcept {
		const auto kind = static_cast<uint64_t>(event);
		if (kind == 0 || kind >= totals.size()) {
			return;
		}
		totals[kind].fetch_add(1, std::memory_order_relaxed);
		if (tick < start_tick || tick - start_tick < delay_ticks) {
			skipped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto index = eligible.fetch_add(1, std::memory_order_relaxed);
		if (index >= CAPACITY) {
			dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto& slot = slots[index];
		slot.event = kind;
		slot.tick = tick;
		slot.values = values;
		slot.commit.store(index + 1, std::memory_order_release);
	}
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(Slot) == 128 && offsetof(Buffer, slots) == 256);
static_assert(sizeof(Buffer) == 524544);

extern std::atomic<Buffer*> g_buffer;
inline bool Enabled() noexcept { return g_buffer.load(std::memory_order_relaxed) != nullptr; }
// Called once during graphics initialization, before guest execution.
void Initialize() noexcept;
void Record(Event event, const Values& values = {}) noexcept;

} // namespace Libs::Graphics::Observe

#endif
