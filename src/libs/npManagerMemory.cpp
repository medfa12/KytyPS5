#include "libs/npManagerMemory.h"

#include "common/threads.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "libs/ipmiObserve.h"
#include "libs/npMemoryArena.h"

#include <mutex>

namespace Libs::NpManagerMemory {
namespace {
constexpr size_t Page = 0x4000;
std::mutex mutex;
NpMemoryArena::Arena heap;
uintptr_t arena = 0;
size_t arena_size = 0;
uint32_t mode = 2; // 0: kernel mapping, 1: supplied buffer, 2: uninitialized
bool transition = false;
uint32_t diagnostic_count = 0; // Native U32 call counter; wrap is independent of ownership metadata.
// Native consumers use full-width, identity-mapped pointers. Keep this writable
// host-image storage alive across Destroy/reinit, independently of arena mappings.
alignas(16) Allocator descriptor {};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool fail_next_heap = false;
#endif

struct StateLock {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock {mutex};
}; // unlock before leaving suspension protection; never spans kernel mapping calls

bool Active() { return mode <= 1 && !transition; }

int32_t BeginCreate(bool invalid) {
	StateLock lock;
	if (invalid) { return InvalidArgument; }
	if (mode != 2 || transition) { return AlreadyInitialized; }
	transition = true;
	return 0;
}

bool MakeHeap(void* buffer, size_t size, uint32_t new_mode) {
	StateLock lock;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (fail_next_heap) { fail_next_heap = false; return false; }
#endif
	if (!heap.Initialize(buffer, size)) { return false; }
	arena = reinterpret_cast<uintptr_t>(buffer);
	arena_size = size;
	mode = new_mode;
	diagnostic_count = 0;
	descriptor = {Allocate, Reallocate, Free, nullptr};
	transition = false;
	return true;
}

void EndFailedCreate() {
	StateLock lock;
	transition = false;
}

} // namespace

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateKernel(size_t size, const char* name) {
	int32_t result = BeginCreate(name == nullptr);
	if (result == 0) {
		void* mapping = nullptr;
		if (size > SIZE_MAX - (Page - 1)) {
			result = LibKernel::KERNEL_ERROR_EINVAL;
		} else {
			const size_t rounded = (size + Page - 1) & ~(Page - 1);
			// No NP lock is held while the kernel takes its operation/range locks
			// or invokes allocation notifications.
			result = LibKernel::Memory::KernelMapNamedFlexibleMemory(&mapping, rounded, 3, 0, name);
			if (result == 0 && !MakeHeap(mapping, rounded, 0)) {
				result = HeapFailure;
				(void)LibKernel::Memory::KernelMunmap(reinterpret_cast<uintptr_t>(mapping), rounded);
			}
		}
		if (result != 0) { EndFailedCreate(); }
	}
	IpmiObserve::NpMemoryCreated(KYTY_IPMI_CALLER, size, name, nullptr, 0, result);
	return result;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateBuffer(size_t size, void* buffer, const char* name) {
	int32_t result = BeginCreate(buffer == nullptr || name == nullptr);
	// Keep the OS coverage query outside StateLock while transition excludes publication.
	if (result == 0 && (!NpMemoryArena::RangeReadWrite(buffer, size) || !MakeHeap(buffer, size, 1))) {
		result = HeapFailure;
		EndFailedCreate();
	}
	IpmiObserve::NpMemoryCreated(KYTY_IPMI_CALLER, size, name, buffer, 1, result);
	return result;
}

Allocator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetAllocator() {
	Allocator* result = nullptr;
	{
		StateLock lock;
		if (Active()) { result = &descriptor; }
	}
	IpmiObserve::Emit(IpmiObserve::Bank::Gateway, IpmiObserve::Event::NpAllocatorResult,
	                  {KYTY_IPMI_CALLER, reinterpret_cast<uint64_t>(result)});
	return result;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Destroy() {
	uintptr_t old_arena = 0;
	size_t old_size = 0;
	uint32_t old_mode = 2;
	{
		StateLock lock;
		if (arena == 0 || transition) { return NotInitialized; }
		if (mode > 1) { return InvalidArgument; }
		old_arena = arena;
		old_size = arena_size;
		old_mode = mode;
		transition = true;
		heap.Reset(); // Destroy invalidates outstanding allocations as native does.
		arena = 0;
		arena_size = 0;
		mode = 2;
		descriptor = {};
		diagnostic_count = 0;
	}
	if (old_mode == 0) { (void)LibKernel::Memory::KernelMunmap(old_arena, old_size); }
	EndFailedCreate();
	return 0;
}

void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Allocate(size_t size, void* context) {
	void* result = nullptr;
	uint64_t count = 0;
	{
		StateLock lock;
		if (Active()) {
			result = heap.Allocate(size);
			if (result != nullptr) { ++diagnostic_count; }
		}
		count = diagnostic_count;
	}
	IpmiObserve::NpMemoryCallback(KYTY_IPMI_CALLER, 0, size, nullptr, context, result, count);
	return result;
}

void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Reallocate(void* pointer, size_t size, void* context) {
	void* result = nullptr;
	uint64_t count = 0;
	{
		StateLock lock;
		if (Active()) { result = heap.Reallocate(pointer, size); }
		count = diagnostic_count;
	}
	IpmiObserve::NpMemoryCallback(KYTY_IPMI_CALLER, 1, size, pointer, context, result, count);
	return result;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Free(void* pointer, void* context) {
	uint64_t count = 0;
	{
		StateLock lock;
		if (Active()) {
			heap.Free(pointer);
			--diagnostic_count; // Includes null/foreign pointers; never used to decide ownership.
		}
		count = diagnostic_count;
	}
	IpmiObserve::NpMemoryCallback(KYTY_IPMI_CALLER, 2, 0, pointer, context, nullptr, count);
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
Snapshot TestSnapshot() {
	StateLock lock;
	return {arena, arena_size, heap.LiveBlocks(), diagnostic_count, mode,
	    Common::GetHostLockCriticalCounter()->load()};
}
void TestFailNextHeapCreation() { StateLock lock; fail_next_heap = true; }
#endif
} // namespace Libs::NpManagerMemory
