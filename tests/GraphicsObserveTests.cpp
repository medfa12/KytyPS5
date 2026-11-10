#include "graphics/graphicsObserve.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace Libs::Graphics::Observe;

#define CHECK(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

int main() {
	auto buffer = std::make_unique<Buffer>();
	buffer->start_tick = 100;
	buffer->delay_ticks = 10;
	buffer->Append(Event::Emit, 99, {});
	buffer->Append(Event::Emit, 109, {});
	CHECK(buffer->skipped == 2 && buffer->eligible == 0);
	buffer->Append(Event::Emit, 110, {42});
	CHECK(buffer->slots[0].commit.load(std::memory_order_acquire) == 1);
	CHECK(buffer->slots[0].values[0] == 42);
	CHECK(buffer->totals[static_cast<size_t>(Event::Emit)] == 3);

	buffer = std::make_unique<Buffer>();
	// Simulate a producer suspended after reservation. Other producers must
	// publish independently, and readers must ignore this permanent hole.
	buffer->eligible.fetch_add(1);
	std::atomic<bool> done {false};
	std::thread reader([&] {
		while (!done.load(std::memory_order_acquire)) {
			for (size_t i = 1; i < CAPACITY; i++) {
				const auto& slot = buffer->slots[i];
				if (slot.commit.load(std::memory_order_acquire) == i + 1) {
					CHECK(slot.event == static_cast<uint64_t>(Event::Emit));
					CHECK(slot.values[2] == (slot.values[0] ^ slot.values[1]));
					CHECK(slot.values[12] == 0x123456789abcdef0ull);
				}
			}
		}
	});
	std::vector<std::thread> writers;
	for (uint64_t thread = 0; thread < 8; thread++) {
		writers.emplace_back([&, thread] {
			for (uint64_t i = 0; i < 2000; i++) {
				Values values {thread, i, thread ^ i};
				values[12] = 0x123456789abcdef0ull;
				buffer->Append(Event::Emit, 200, values);
			}
		});
	}
	for (auto& writer: writers) writer.join();
	done.store(true, std::memory_order_release);
	reader.join();
	CHECK(buffer->eligible == 16001);
	CHECK(buffer->dropped == 16001 - CAPACITY);
	CHECK(buffer->totals[static_cast<size_t>(Event::Emit)] == 16000);
	CHECK(buffer->slots[0].commit == 0);
	std::unordered_set<uint64_t> seen;
	for (size_t i = 1; i < CAPACITY; i++) {
		const auto& slot = buffer->slots[i];
		CHECK(slot.commit == i + 1);
		CHECK(seen.insert(slot.values[0] * 2000 + slot.values[1]).second);
	}
	const auto last = buffer->slots.back().values;
	buffer->Append(Event::Emit, 201, {999});
	CHECK(buffer->slots.back().values == last);
	CHECK(buffer->dropped == 16002 - CAPACITY);

#ifdef _WIN32
	_putenv_s("KYTY_GRAPHICS_OBSERVE", "");
	Initialize();
	CHECK(!Enabled());
	Record(Event::Emit, {});
	_putenv_s("KYTY_GRAPHICS_OBSERVE", "1");
	_putenv_s("KYTY_GRAPHICS_OBSERVE_DELAY", "invalid");
	Initialize();
	CHECK(!Enabled());
	_putenv_s("KYTY_GRAPHICS_OBSERVE_DELAY", "0");
	Initialize();
	CHECK(Enabled());
	Record(Event::Present, {123, 456});
	auto* mapped = g_buffer.load(std::memory_order_acquire);
	CHECK(mapped->ready.load(std::memory_order_acquire) == 1);
	CHECK(mapped->slots[0].commit == 1);
	CHECK(mapped->slots[0].values[0] == 123 && mapped->slots[0].values[1] == 456);
	std::printf("mapped capture test pid=%llu start_tick=%llu\n",
	            static_cast<unsigned long long>(mapped->pid),
	            static_cast<unsigned long long>(mapped->start_tick));
#endif
	std::puts("graphics observation tests passed");
}
