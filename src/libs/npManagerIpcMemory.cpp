#include "libs/npManagerIpcMemory.h"

#include "common/threads.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <mutex>
#include <new>

namespace Libs::NpManagerIpcMemory {
namespace {
std::mutex mutex;
NpMemoryHeap::Handle heap = nullptr;
NpMemoryHeap::Handle retained_generation = nullptr;
NpMemoryHeap::MappingRecord mapping {};
void* base = nullptr;
size_t capacity = 0;
uint32_t mode = 2; // 0: owned mapping; 1: caller backing; 2: inactive
uint32_t diagnostic_count = 0; // Native wrapping call counter, not ownership.
bool transition = false;
alignas(16) Allocator descriptor {};

struct StateLock {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock {mutex};
};
bool Active() { return mode <= 1 && !transition; }

int32_t ReleaseMapping(NpMemoryHeap::MappingRecord& owned) {
	if (owned.mapping == nullptr) { return 0; }
	const auto result = LibKernel::Memory::KernelMunmap(reinterpret_cast<uintptr_t>(owned.mapping), owned.size);
	if (result == 0) { owned = {}; }
	return result;
}

// Lifecycle exclusion spans mapping/coverage operations without holding a host
// lock over kernel callbacks. Ordinary callbacks hold StateLock through the
// bounded heap operation, so Destroy cannot retire/unmap an in-flight block.
struct Creation {
	NpMemoryHeap::Handle handle = nullptr;
	NpMemoryHeap::MappingRecord owned {};
	~Creation() {
		if (handle != nullptr) { NpMemoryHeap::DestroyHeap(&handle); }
		(void)ReleaseMapping(owned);
		StateLock lock;
		// Failed rollback remains owned and blocks replacement until Destroy
		// can actually release it. Never lose the last mapping record.
		if (owned.mapping != nullptr) { mapping = owned; }
		transition = false;
	}
};

int32_t Create(size_t size, void* buffer, const char* name, uint32_t new_mode) {
	{
		StateLock lock;
		if (name == nullptr || (new_mode == 1 && buffer == nullptr)) { return InvalidArgument; }
		if (mode != 2 || transition || mapping.mapping != nullptr ||
		    NpMemoryHeap::HasRetainedGeneration(retained_generation)) { return AlreadyInitialized; }
		transition = true;
	}
	Creation pending;
	try {
		if (new_mode == 0) {
			const auto result = NpMemoryHeap::MapMemory(size, name, &buffer, &pending.owned);
			if (result < 0) { return result; }
			size = pending.owned.size;
		}
		const auto result = NpMemoryHeap::CreateHeap(&pending.handle, buffer, size, name);
		if (result < 0) { return result; }
		StateLock lock;
		heap = pending.handle;
		mapping = pending.owned;
		base = buffer;
		capacity = size;
		mode = new_mode;
		diagnostic_count = 0;
		descriptor = {Allocate, Reallocate, Free, nullptr};
		pending.handle = nullptr;
		pending.owned = {};
		// Creation releases transition after this lock; no partially initialized
		// descriptor is returned by GetAllocator during mapping notifications.
		return result;
	} catch (const std::bad_alloc&) { return NpManagerMemory::HeapFailure; }
}
} // namespace

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateKernel(size_t size, const char* name) {
	return Create(size, nullptr, name, 0);
}
int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateBuffer(size_t size, void* buffer, const char* name) {
	return Create(size, buffer, name, 1);
}
Allocator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetAllocator() {
	StateLock lock;
	return Active() ? &descriptor : nullptr;
}
int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Destroy() {
	NpMemoryHeap::Handle retired = nullptr;
	NpMemoryHeap::MappingRecord owned {};
	bool retry_retained = false;
	{
		StateLock lock;
		if (transition) { return NotInitialized; }
		if (heap == nullptr && mapping.mapping == nullptr) {
			if (!NpMemoryHeap::HasRetainedGeneration(retained_generation)) { return NotInitialized; }
			retry_retained = true;
		}
		if (heap != nullptr && mode > 1) { return InvalidMode; }
		transition = true;
		retired = retry_retained ? retained_generation : heap;
		if (retired != nullptr) { retained_generation = retired; }
		owned = mapping;
		heap = nullptr;
		mapping = {};
		base = nullptr;
		capacity = 0;
		mode = 2;
		diagnostic_count = 0;
		descriptor = {};
	}
	if (retry_retained) {
		// A failed/deferred unmap transferred its mapping record to the heap's
		// generation registry. Retry there, never through a stale raw address.
		const int retry = NpMemoryHeap::RetryAllocationRetirements();
		const bool pending = NpMemoryHeap::HasRetainedGeneration(retired);
		StateLock lock;
		transition = false;
		return !pending ? 0 : retry == 12 ? LibKernel::KERNEL_ERROR_ENOMEM : LibKernel::KERNEL_ERROR_EBUSY;
	}
	// Transfer ownership atomically with logical retirement. The stable global
	// descriptor cannot be replaced while this exact generation remains retained.
	const auto generation = retired;
	const auto result = NpMemoryHeap::RetireHeapMapping(&retired, owned.mapping, owned.size);
	if (result == 0 || NpMemoryHeap::HasRetainedGeneration(generation)) { owned = {}; }
	StateLock lock;
	mapping = owned;
	transition = false;
	return result;
}
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Allocate(size_t size, void*) {
	StateLock lock;
	if (!Active()) { return nullptr; }
	void* result = NpMemoryHeap::Malloc(&heap, size);
	if (result != nullptr) { ++diagnostic_count; }
	return result;
}
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Reallocate(void* pointer, size_t size, void*) {
	StateLock lock;
	return Active() ? NpMemoryHeap::Realloc(&heap, pointer, size) : nullptr;
}
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Free(void* pointer, void*) {
	StateLock lock;
	if (Active()) {
		NpMemoryHeap::Free(&heap, pointer);
		--diagnostic_count;
	}
}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
Snapshot TestSnapshot() {
	StateLock lock;
	return {base, capacity, mode, diagnostic_count, Common::GetHostLockCriticalCounter()->load()};
}
#endif
} // namespace Libs::NpManagerIpcMemory
