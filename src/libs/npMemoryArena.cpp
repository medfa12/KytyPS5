#include "libs/npMemoryArena.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace Libs::NpMemoryArena {

bool RangeReadWrite(void* buffer, size_t size) {
	auto address = reinterpret_cast<uintptr_t>(buffer);
	if (address == 0 || size == 0 || size > UINTPTR_MAX - address) { return false; }
	const auto end = address + size;
#ifdef _WIN32
	while (address < end) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) { return false; }
		const auto protection = info.Protect & 0xffu;
		if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
		    protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
		if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= address) { return false; }
		address = base + info.RegionSize;
	}
	return true;
#elif defined(__APPLE__)
	while (address < end) {
		mach_vm_address_t base = address;
		mach_vm_size_t length = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const auto status = mach_vm_region(mach_task_self(), &base, &length, VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
		if (object != MACH_PORT_NULL) { mach_port_deallocate(mach_task_self(), object); }
		if (status != KERN_SUCCESS || base > address || length > UINTPTR_MAX - base || base + length <= address ||
		    (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) { return false; }
		address = base + length;
	}
	return true;
#else
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	char line[1024];
	while (address < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		uintptr_t base = 0, limit = 0;
		char permissions[5] {};
		if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &base, &limit, permissions) != 3 || limit <= address) { continue; }
		if (base > address || permissions[0] != 'r' || permissions[1] != 'w') { break; }
		address = limit;
	}
	std::fclose(maps);
	return address >= end;
#endif
}

bool Arena::Initialize(void* buffer, size_t size) {
	if (!blocks.empty()) { return false; }
	const auto base = reinterpret_cast<uintptr_t>(buffer);
	if (base == 0 || size > UINTPTR_MAX - base || base > UINTPTR_MAX - (Alignment - 1)) { return false; }
	const auto aligned = (base + Alignment - 1) & ~(uintptr_t {Alignment} - 1);
	const size_t padding = aligned - base;
	if (padding > size || size - padding < Alignment) { return false; }
	try {
		blocks.push_back({aligned, (size - padding) & ~(Alignment - 1), 0});
	} catch (const std::bad_alloc&) { return false; }
	return true;
}

void Arena::Reset() { if (!HasLeases()) { blocks.clear(); } }

Arena::Iterator Arena::Find(void* pointer) {
	const auto address = reinterpret_cast<uintptr_t>(pointer);
	return std::find_if(blocks.begin(), blocks.end(), [address](const Block& block) {
		return block.address == address && block.requested != 0;
	});
}

void Arena::Release(Iterator block) {
	block->requested = 0;
	block->pending_free = false;
	block->lease.reset();
	auto next = std::next(block);
	if (next != blocks.end() && next->requested == 0) {
		block->size += next->size;
		blocks.erase(next);
	}
	if (block != blocks.begin()) {
		auto previous = std::prev(block);
		if (previous->requested == 0) {
			previous->size += block->size;
			blocks.erase(block);
		}
	}
}

void* Arena::Allocate(size_t size) {
	Reclaim();
	if (next_generation == UINT64_MAX) { return nullptr; }
	const size_t requested = std::max<size_t>(size, 1);
	if (requested > SIZE_MAX - (Alignment - 1)) { return nullptr; }
	const size_t aligned = (requested + Alignment - 1) & ~(Alignment - 1);
	for (auto block = blocks.begin(); block != blocks.end(); ++block) {
		if (block->requested != 0 || block->size < aligned) { continue; }
		if (block->size > aligned) {
			try {
				blocks.insert(std::next(block), {block->address + aligned, block->size - aligned, 0});
			} catch (const std::bad_alloc&) { return nullptr; }
			block->size = aligned;
		}
		block->requested = requested;
		block->generation = ++next_generation;
		return reinterpret_cast<void*>(block->address);
	}
	return nullptr;
}

void* Arena::Reallocate(void* pointer, size_t size) {
	if (pointer == nullptr) { return Allocate(size); }
	auto block = Find(pointer);
	if (block == blocks.end()) { return nullptr; }
	if (block->pending_free || (block->lease && block->lease.use_count() > 1)) { return nullptr; }
	size = std::max<size_t>(size, 1); // Native realloc(ptr,0) shrinks but retains the owned allocation.
	if (size > SIZE_MAX - (Alignment - 1)) { return nullptr; }
	const size_t aligned = (size + Alignment - 1) & ~(Alignment - 1);
	if (aligned <= block->size) {
		// Split only after metadata allocation succeeds; failure leaves the old block intact.
		if (aligned < block->size) {
			try {
				auto tail = blocks.insert(std::next(block), {block->address + aligned, block->size - aligned, 0});
				block->size = aligned;
				Release(tail);
			} catch (const std::bad_alloc&) { return nullptr; }
		}
		block->requested = size;
		return pointer;
	}
	auto next = std::next(block);
	if (next != blocks.end() && next->requested == 0 && next->size >= aligned - block->size) {
		const size_t extra = aligned - block->size;
		next->address += extra;
		next->size -= extra;
		if (next->size == 0) { blocks.erase(next); }
		block->size = aligned;
		block->requested = size;
		return pointer;
	}
	void* replacement = Allocate(size);
	if (replacement != nullptr) {
		std::memcpy(replacement, pointer, std::min(size, block->requested));
		Release(block);
	}
	return replacement;
}

void Arena::Free(void* pointer) {
	auto block = Find(pointer);
	if (block != blocks.end()) {
		if (block->lease && block->lease.use_count() > 1) { block->pending_free = true; }
		else { Release(block); }
	}
}

std::shared_ptr<void> Arena::AcquireRange(uintptr_t address, size_t size) {
	if (size == 0 || size > UINTPTR_MAX - address) { return {}; }
	for (auto& block: blocks) {
		if (!block.pending_free && block.requested != 0 && address >= block.address &&
		    address - block.address < block.requested && size <= block.requested - (address - block.address)) {
			if (!block.lease) {
				block.lease = std::make_shared<AllocationGeneration>(AllocationGeneration {block.generation, block.address});
			}
			return block.lease;
		}
	}
	return {};
}

bool Arena::HasLeases() const {
	return std::any_of(blocks.begin(), blocks.end(), [](const Block& block) {
		return block.lease && block.lease.use_count() > 1;
	});
}
bool Arena::HasPendingFrees() const {
	return std::any_of(blocks.begin(), blocks.end(), [](const Block& block) { return block.pending_free; });
}
void Arena::Reclaim() {
	for (auto block = blocks.begin(); block != blocks.end();) {
		if (block->pending_free && (!block->lease || block->lease.use_count() == 1)) {
			Release(block);
			block = blocks.begin(); // Release may coalesce both neighbours.
		} else { ++block; }
	}
}

size_t Arena::LiveBlocks() const {
	return static_cast<size_t>(std::count_if(blocks.begin(), blocks.end(),
	    [](const Block& block) { return block.requested != 0; }));
}

} // namespace Libs::NpMemoryArena
