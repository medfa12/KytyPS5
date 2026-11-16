#pragma once

#include "libs/npManagerMemory.h"
#include <memory>

namespace Libs::NpMemoryHeap {
using Handle = void*;
struct MappingRecord {
	uint64_t size;
	uint64_t reserved;
	void* mapping;
};
static_assert(sizeof(MappingRecord) == 24 && offsetof(MappingRecord, mapping) == 0x10);
struct Heap {
	uint64_t size;
	uint64_t reserved;
	void* mapping;
	Handle handle;
};
using Allocator = NpManagerMemory::Allocator;
struct AllocatorEx {
	Allocator allocator;
	int32_t error;
	uint32_t padding;
};
static_assert(sizeof(Heap) == 32 && offsetof(Heap, mapping) == 0x10 && offsetof(Heap, handle) == 0x18);
static_assert(sizeof(AllocatorEx) == 40 && offsetof(AllocatorEx, error) == 0x20);
constexpr int32_t InvalidHeap = static_cast<int32_t>(0x80558005u);

// Host-only, opt-in retention. Success is BSD errno 0 and a nonempty token;
// unsupported backing/ranges fail closed. Destruction only drops host ownership.
int AcquireAllocationLease(uint64_t address, size_t size, std::shared_ptr<void>& lease) noexcept;
bool HasPendingRetirements();
void RetryRetirements(); // Call outside transport/allocator locks; may unmap/call kernel observers.
int RetryAllocationRetirements() noexcept; // BSD errno: 0 drained, 16 pending, 12 allocation failure.
bool HasRetainedGeneration(Handle handle);
// Retire and unmap outside NP locks, retaining the exact managed generation on
// failure. Returns kernel status; EBUSY means physical retirement is pending.
int RetireHeapMapping(Handle* slot, void* mapping, uint64_t size);

// Standalone native helpers: mapping ownership and arena ownership are separate.
// Callers serialize lifecycle/slot changes and keep backing mapped/RW until all
// arena users have finished. Create failure preserves the prior slot value.
int32_t KYTY_SYSV_ABI MapMemory(uint64_t size, const char* name, void** optional_base, MappingRecord* record);
int32_t KYTY_SYSV_ABI CreateHeap(Handle* slot, void* backing, uint64_t size, const char* name);
void KYTY_SYSV_ABI DestroyHeap(Handle* slot); // Retires the arena only; never unmaps backing.
int32_t KYTY_SYSV_ABI UnmapMemory(MappingRecord* record);

int32_t KYTY_SYSV_ABI Init(Heap* heap, uint64_t size, const char* name);
void KYTY_SYSV_ABI Destroy(Heap* heap);
void KYTY_SYSV_ABI MakeAllocator(Heap* heap, Allocator* out);
void KYTY_SYSV_ABI MakeAllocatorEx(Heap* heap, int32_t error, AllocatorEx* out);
// Direct helpers take the ADDRESS of a handle slot, not Heap* or the handle value.
void* KYTY_SYSV_ABI Malloc(Handle* slot, uint64_t size);
void* KYTY_SYSV_ABI Realloc(Handle* slot, void* pointer, uint64_t size);
void KYTY_SYSV_ABI Free(Handle* slot, void* pointer);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestFailNextHeapCreation();
#endif
} // namespace Libs::NpMemoryHeap
