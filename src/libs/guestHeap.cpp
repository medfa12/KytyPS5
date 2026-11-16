#include "libs/guestHeap.h"

#include "common/virtualMemory.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Libs::GuestHeap {
namespace {

constexpr uint64_t kAddressLimit  = uint64_t {1} << 40;
constexpr size_t   kGranule       = 16;
constexpr size_t   kArenaMinimum  = size_t {16} << 20;
constexpr size_t   kArenaGranule  = 0x10000;

struct Block {
	uintptr_t base;  // start of the carved range
	size_t    total; // carved length (returned to the free lists)
	size_t    size;  // usable size reported to callers
};

std::mutex                              g_mutex;
std::map<uintptr_t, size_t>             g_free_by_address;
std::multimap<size_t, uintptr_t>        g_free_by_size;
std::unordered_map<uintptr_t, Block>    g_blocks;

void InsertFree(uintptr_t address, size_t size) {
	// Coalesce with neighbours so arenas do not fragment permanently.
	auto next = g_free_by_address.lower_bound(address);
	if (next != g_free_by_address.end() && address + size == next->first) {
		const auto [range_begin, range_end] = g_free_by_size.equal_range(next->second);
		for (auto it = range_begin; it != range_end; ++it) {
			if (it->second == next->first) {
				g_free_by_size.erase(it);
				break;
			}
		}
		size += next->second;
		next = g_free_by_address.erase(next);
	}
	if (next != g_free_by_address.begin()) {
		auto previous = std::prev(next);
		if (previous->first + previous->second == address) {
			const auto [range_begin, range_end] = g_free_by_size.equal_range(previous->second);
			for (auto it = range_begin; it != range_end; ++it) {
				if (it->second == previous->first) {
					g_free_by_size.erase(it);
					break;
				}
			}
			address = previous->first;
			size += previous->second;
			g_free_by_address.erase(previous);
		}
	}
	g_free_by_address.emplace(address, size);
	g_free_by_size.emplace(size, address);
}

bool AddArena(size_t needed) {
	const size_t arena_size =
	    std::max(kArenaMinimum, (needed + kArenaGranule - 1) & ~(kArenaGranule - 1));
	const uint64_t arena = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, arena_size, Common::VirtualMemory::Mode::ReadWrite, "guest-heap", false);
	if (arena == 0) {
		return false;
	}
	if (arena >= kAddressLimit || arena_size > kAddressLimit - arena) {
		(void)Libs::LibKernel::Memory::FreeGuestMemory(arena, arena_size);
		return false;
	}
	InsertFree(static_cast<uintptr_t>(arena), arena_size);
	return true;
}

} // namespace

void* Allocate(size_t size, size_t alignment) {
	if (alignment < kGranule) {
		alignment = kGranule;
	}
	if ((alignment & (alignment - 1)) != 0 || size > (SIZE_MAX / 2)) {
		return nullptr;
	}
	size = std::max<size_t>(size, 1);
	const size_t usable = (size + kGranule - 1) & ~(kGranule - 1);
	const size_t total  = usable + (alignment > kGranule ? alignment - kGranule : 0);

	std::lock_guard lock(g_mutex);
	for (int attempt = 0; attempt < 2; attempt++) {
		auto fit = g_free_by_size.lower_bound(total);
		if (fit != g_free_by_size.end()) {
			const size_t    free_size = fit->first;
			const uintptr_t base      = fit->second;
			g_free_by_size.erase(fit);
			g_free_by_address.erase(base);
			if (free_size > total) {
				InsertFree(base + total, free_size - total);
			}
			const uintptr_t result = (base + alignment - 1) & ~(uintptr_t {alignment} - 1);
			g_blocks.emplace(result, Block {base, total, usable});
			return reinterpret_cast<void*>(result);
		}
		if (attempt == 0 && !AddArena(total)) {
			return nullptr;
		}
	}
	return nullptr;
}

bool Release(void* ptr) {
	if (ptr == nullptr) {
		return false;
	}
	std::lock_guard lock(g_mutex);
	const auto it = g_blocks.find(reinterpret_cast<uintptr_t>(ptr));
	if (it == g_blocks.end()) {
		return false;
	}
	const Block block = it->second;
	g_blocks.erase(it);
	InsertFree(block.base, block.total);
	return true;
}

size_t BlockSize(const void* ptr) {
	std::lock_guard lock(g_mutex);
	const auto it = g_blocks.find(reinterpret_cast<uintptr_t>(ptr));
	return it != g_blocks.end() ? it->second.size : 0;
}

bool Owns(const void* ptr) {
	std::lock_guard lock(g_mutex);
	return g_blocks.find(reinterpret_cast<uintptr_t>(ptr)) != g_blocks.end();
}

void Free(void* ptr) {
	if (ptr != nullptr && !Release(ptr)) {
		std::free(ptr);
	}
}

void* Reallocate(void* ptr, size_t size) {
	if (ptr == nullptr) {
		return Allocate(size);
	}
	const size_t old_size = BlockSize(ptr);
	if (old_size == 0) {
		// CRT block from outside the guest heap: migrate it so the result is guest-range.
#ifdef _WIN32
		const size_t crt_size = _msize(ptr);
#else
		const size_t crt_size = 0;
#endif
		void* moved = Allocate(size);
		if (moved != nullptr) {
			std::memcpy(moved, ptr, std::min(crt_size, size));
			std::free(ptr);
		}
		return moved;
	}
	if (size <= old_size) {
		return ptr;
	}
	void* grown = Allocate(size);
	if (grown != nullptr) {
		std::memcpy(grown, ptr, old_size);
		Release(ptr);
	}
	return grown;
}

void* AllocateZeroed(size_t count, size_t size) {
	if (size != 0 && count > SIZE_MAX / size) {
		return nullptr;
	}
	void* p = Allocate(count * size);
	if (p != nullptr) {
		std::memset(p, 0, std::max<size_t>(count * size, 1));
	}
	return p;
}

} // namespace Libs::GuestHeap
