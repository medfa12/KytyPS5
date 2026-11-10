#include "libs/npMemoryHeap.h"

#include "common/threads.h"
#include "kernel/memory.h"
#include "libs/ipmiObserve.h"
#include "libs/npMemoryArena.h"
#include "libs/processSdk.h"
#include "libs/errno.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace Libs::NpMemoryHeap {
namespace {
struct State {
	std::mutex mutex;
	NpMemoryArena::Arena arena;
	bool active = true;
	uintptr_t backing = 0;
	uint64_t size = 0;
	uint64_t mapping_generation = 0;
	std::shared_ptr<void> mapping_pin;
	bool unmap_requested = false;
	bool reclaiming = false;
	size_t acquiring = 0;
};
std::mutex registry_mutex;
// Keep opaque handles stable, including retired handles, so stale slot values
// cannot alias a later State allocation. Duplicate Init intentionally orphans
// the previous mapping/state; native does not implicitly destroy it. The OS
// reclaims orphan mappings at process exit, not at a fabricated guest Destroy.
std::unordered_map<Handle, std::shared_ptr<State>> registry;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
std::atomic<bool> fail_next_heap {false};
#endif
struct StateLock {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock;
	explicit StateLock(std::mutex& mutex): lock(mutex) {}
};

std::shared_ptr<State> Lookup(Handle* slot, Handle* handle) {
	StateLock lock(registry_mutex);
	*handle = *slot;
	const auto it = registry.find(*handle);
	return it == registry.end() ? nullptr : it->second;
}

// Backing coverage/lifetime is established by the caller before entry. Only
// publication takes the registry lock; no mapping or guest callback runs here.
bool CreateState(Handle* slot, void* backing, uint64_t size) {
	try {
		auto state = std::make_shared<State>();
		state->backing = reinterpret_cast<uintptr_t>(backing);
		state->size = size;
		// Capture identity while temporarily pinned, before taking any NP lock.
		auto pin = LibKernel::Memory::AcquireFlexibleMappingLease(state->backing, size, 0, &state->mapping_generation);
		bool forced = false;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		forced = fail_next_heap.exchange(false);
#endif
		if (!forced && state->arena.Initialize(backing, size)) {
			StateLock lock(registry_mutex);
			for (const auto& entry: registry) {
				const auto& other = entry.second;
				StateLock state_lock(other->mutex);
				if (state->backing < other->backing + other->size && other->backing < state->backing + size &&
				    (other->mapping_pin || other->arena.HasLeases() || other->reclaiming ||
				     other->unmap_requested || other->acquiring != 0)) { return false; }
			}
			registry.emplace(state.get(), state);
			pin.reset();
			*slot = state.get();
			return true;
		}
	} catch (const std::bad_alloc&) {}
	return false;
}

Handle RetireState(Handle* slot, void* mapping = nullptr, uint64_t size = 0, bool* deferred = nullptr,
                   std::shared_ptr<State>* reclamation = nullptr) {
	Handle handle = nullptr;
	auto state = Lookup(slot, &handle);
	if (handle == nullptr) { return nullptr; } // Native zero-handle destroy only reads the slot.
	if (state != nullptr) {
		StateLock lock(state->mutex);
		state->active = false;
		if (mapping != nullptr &&
		    state->backing == reinterpret_cast<uintptr_t>(mapping) && state->size == size) {
			state->unmap_requested = true;
			if (deferred != nullptr) { *deferred = true; }
			// Publish ownership before unlocking, including the immediate path.
			// Reentrant observers and concurrent retries must see this reservation.
			if (reclamation != nullptr && !state->mapping_pin && state->acquiring == 0 && !state->reclaiming) {
				state->reclaiming = true;
				*reclamation = state;
			}
		}
		state->arena.Reset();
	}
	{
		StateLock lock(registry_mutex);
		*slot = nullptr;
	}
	return handle;
}

void Observe(uint64_t caller, uint32_t operation, const void* subject, Handle handle,
             uint64_t size, const void* input, const void* result, int32_t status = 0, uint64_t aux = 0) {
	IpmiObserve::NpHeapRecord(
	    {caller, operation, reinterpret_cast<uint64_t>(subject), reinterpret_cast<uint64_t>(handle), size,
	     reinterpret_cast<uint64_t>(input), reinterpret_cast<uint64_t>(result), static_cast<uint32_t>(status), aux});
}

void* Operate(Handle* slot, void* pointer, uint64_t size, uint32_t operation, uint64_t caller, void* context) {
	Handle handle = nullptr;
	auto state = Lookup(slot, &handle); // Registry lock released before taking state lock.
	void* result = nullptr;
	if (state != nullptr) {
		StateLock lock(state->mutex);
		if (state->active) {
			if (operation == 4) { result = state->arena.Allocate(size); }
			else if (operation == 5) { result = state->arena.Reallocate(pointer, size); }
			else if (pointer != nullptr) { state->arena.Free(pointer); }
		}
	}
	Observe(caller, operation, slot, handle, size, pointer, result, 0, reinterpret_cast<uint64_t>(context));
	return result;
}

void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AllocateCallback(size_t size, void* context) {
	return Operate(&static_cast<Heap*>(context)->handle, nullptr, size, 4, KYTY_IPMI_CALLER, context);
}
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ReallocateCallback(void* pointer, size_t size, void* context) {
	return Operate(&static_cast<Heap*>(context)->handle, pointer, size, 5, KYTY_IPMI_CALLER, context);
}
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FreeCallback(void* pointer, void* context) {
	(void)Operate(&static_cast<Heap*>(context)->handle, pointer, 0, 6, KYTY_IPMI_CALLER, context);
}
Allocator Descriptor(Heap* heap) { return {AllocateCallback, ReallocateCallback, FreeCallback, heap}; }
} // namespace

int AcquireAllocationLease(uint64_t address, size_t size, std::shared_ptr<void>& lease) noexcept {
	// Never destroy a caller's previous token under an NP lock.
	lease.reset();
	if (address == 0 || size == 0 || size > UINT64_MAX - address) { return 22; } // EINVAL
	try {
		std::shared_ptr<State> candidate;
		{
			StateLock lock(registry_mutex);
			for (const auto& entry: registry) {
				const auto& state = entry.second;
				StateLock state_lock(state->mutex);
				if (state->active && address >= state->backing && address - state->backing < state->size &&
				    size <= state->size - (address - state->backing)) {
					if (candidate) { return 16; } // EBUSY: ambiguous overlapping arenas
					candidate = state;
				}
			}
		}
		if (!candidate || candidate->mapping_generation == 0) { return 45; } // BSD EOPNOTSUPP
		struct Acquisition {
			std::shared_ptr<State> state;
			~Acquisition() { if (state) { StateLock lock(state->mutex); --state->acquiring; } }
		} acquisition;
		{
			StateLock lock(candidate->mutex);
			if (!candidate->active || candidate->reclaiming) { return 16; }
			++candidate->acquiring;
			acquisition.state = candidate;
		}
		auto pin = LibKernel::Memory::AcquireFlexibleMappingLease(candidate->backing, candidate->size,
		    candidate->mapping_generation, nullptr);
		if (!pin) { return 14; } // EFAULT: backing changed or inaccessible
		struct Lease {
			std::shared_ptr<State> heap; // exact heap generation (stable registry identity)
			std::shared_ptr<void> allocation; // exact block generation; destroyed first
		};
		auto token = std::make_shared<Lease>();
		// Kernel pin is acquired before NP locking: callbacks may reenter NP from
		// the kernel operation lock. No NP lock ever waits for that lock.
		StateLock lock(registry_mutex);
		for (const auto& entry: registry) {
			const auto& other = entry.second;
			if (other == candidate) { continue; }
			StateLock state_lock(other->mutex);
			if ((other->active || other->mapping_pin || other->reclaiming || other->unmap_requested || other->acquiring != 0) &&
			    candidate->backing < other->backing + other->size && other->backing < candidate->backing + candidate->size) {
				return 16;
			}
		}
		StateLock state_lock(candidate->mutex);
		if (!candidate->active || candidate->reclaiming) { return 16; }
		token->allocation = candidate->arena.AcquireRange(address, size);
		if (!token->allocation) { return 14; }
		if (!candidate->mapping_pin) { candidate->mapping_pin = std::move(pin); }
		pin.reset(); // A redundant temporary pin must not escape publication.
		token->heap = candidate;
		lease = std::move(token);
		return 0;
	} catch (...) { return 12; } // ENOMEM; never unwind through transport hooks
}

bool HasRetainedGeneration(Handle handle) {
	StateLock lock(registry_mutex);
	const auto it = registry.find(handle);
	if (it == registry.end()) { return false; }
	StateLock state_lock(it->second->mutex);
	return it->second->mapping_pin || it->second->arena.HasLeases() || it->second->reclaiming ||
	       it->second->unmap_requested || it->second->acquiring != 0;
}

int RetireHeapMapping(Handle* slot, void* mapping, uint64_t size) {
	bool deferred = false;
	std::shared_ptr<State> reclamation;
	RetireState(slot, mapping, size, &deferred, &reclamation);
	if (mapping == nullptr) { return 0; }
	if (reclamation) {
		const int result = LibKernel::Memory::UnmapFlexibleMappingGeneration(
		    reclamation->backing, reclamation->size, reclamation->mapping_generation);
		StateLock lock(reclamation->mutex);
		if (result == 0) { reclamation->unmap_requested = false; }
		reclamation->reclaiming = false;
		return result;
	}
	if (deferred) { return LibKernel::KERNEL_ERROR_EBUSY; }
	// Mapping-only rollback has no published heap generation; its caller retains
	// the mapping record on failure.
	return LibKernel::Memory::KernelMunmap(reinterpret_cast<uintptr_t>(mapping), size);
}

bool HasPendingRetirements() {
	StateLock lock(registry_mutex);
	for (const auto& entry: registry) {
		StateLock state_lock(entry.second->mutex);
		const auto& state = *entry.second;
		if ((!state.active && state.acquiring != 0) ||
		    (state.mapping_pin && (!state.active || !state.arena.HasLeases())) ||
		    state.unmap_requested || state.arena.HasPendingFrees() || state.reclaiming) { return true; }
	}
	return false;
}

void RetryRetirements() {
	std::vector<std::shared_ptr<State>> states;
	{
		StateLock lock(registry_mutex);
		for (const auto& entry: registry) { states.push_back(entry.second); }
	}
	for (const auto& state: states) {
		std::shared_ptr<void> pin;
		bool unmap = false;
		{
			StateLock lock(state->mutex);
			state->arena.Reclaim();
			if (state->arena.HasLeases() || state->reclaiming || state->acquiring != 0) { continue; }
			if (state->active) { state->mapping_pin.reset(); continue; }
			if (!state->active) { state->arena.Reset(); }
			if (!state->mapping_pin && !state->unmap_requested) { continue; }
			state->reclaiming = true;
			pin = state->mapping_pin;
			unmap = state->unmap_requested;
		}
		// Retained on failure; callbacks may safely reenter registry/heap APIs.
		const bool done = !unmap || (pin
		    ? LibKernel::Memory::UnmapFlexibleMappingLease(pin, state->backing, state->size)
		    : LibKernel::Memory::UnmapFlexibleMappingGeneration(state->backing, state->size, state->mapping_generation)) == 0;
		{
			StateLock lock(state->mutex);
			if (done) { state->mapping_pin.reset(); state->unmap_requested = false; }
			else if (pin) { state->mapping_pin = pin; }
			state->reclaiming = false;
		}
	}
}

int RetryAllocationRetirements() noexcept {
	try {
		RetryRetirements();
		return HasPendingRetirements() ? 16 : 0;
	} catch (...) { return 12; }
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MapMemory(
    uint64_t size, const char* name, void** optional_base, MappingRecord* record) {
	if (record == nullptr) { return InvalidHeap; }
	record->reserved = 0;
	record->size = (size + uint64_t {0x3fff}) & ~uint64_t {0x3fff};
	const int32_t result = LibKernel::Memory::KernelMapNamedFlexibleMemory(&record->mapping, record->size, 3, 0, name);
	if (result >= 0 && optional_base != nullptr) { *optional_base = record->mapping; }
	return result;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateHeap(
    Handle* slot, void* backing, uint64_t size, const char* name) {
	// Native B+y calls mspace-create(name, backing, size, flags=0). Null names
	// select a default diagnostic name; they are not a failure condition.
	(void)name;
	uint32_t sdk = 0;
	const bool strict = LibKernel::KernelGetCompiledSdkVersion(&sdk) != 0 || sdk >= 0x03500000;
	const uint64_t overhead = strict ? 0x5a0 : 0x580;
	if (size <= overhead || size >= UINT64_C(0xffffffffffffc000) - overhead ||
	    (strict && ((reinterpret_cast<uintptr_t>(backing) & 7) != 0 || (size & 7) != 0)) ||
	    !NpMemoryArena::RangeReadWrite(backing, size)) {
		return NpManagerMemory::HeapFailure;
	}
	// No NP lock spans OS coverage queries. Native has no duplicate-slot check;
	// successful replacement leaves the old arena alive for retained handles.
	return CreateState(slot, backing, size) ? 0 : NpManagerMemory::HeapFailure;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI DestroyHeap(Handle* slot) {
	(void)RetireState(slot);
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnmapMemory(MappingRecord* record) {
	if (record == nullptr) { return InvalidHeap; }
	if (record->mapping != nullptr) {
		const auto result = LibKernel::Memory::KernelMunmap(reinterpret_cast<uintptr_t>(record->mapping), record->size);
		if (result == LibKernel::KERNEL_ERROR_EBUSY) { return result; } // Retain an outstanding pin conflict truthfully.
	}
	record->mapping = nullptr; // Native ignores unmap status, preserves size/reserved.
	return 0;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Init(Heap* heap, uint64_t size, const char* name) {
	if (heap == nullptr) {
		Observe(KYTY_IPMI_CALLER, 0, heap, nullptr, size, name, nullptr, InvalidHeap);
		return InvalidHeap;
	}
	// Unsigned rounding deliberately wraps, exactly as native. Do not clear the
	// old mapping/handle on mapping failure, or reject a duplicate initialization.
	heap->reserved = 0;
	heap->size = (size + uint64_t {0x3fff}) & ~uint64_t {0x3fff};
	int32_t result = LibKernel::Memory::KernelMapNamedFlexibleMemory(&heap->mapping, heap->size, 3, 0, name);
	if (result >= 0) {
		if (!CreateState(&heap->handle, heap->mapping, heap->size)) {
			result = NpManagerMemory::HeapFailure;
			if (heap->mapping != nullptr) {
				(void)LibKernel::Memory::KernelMunmap(reinterpret_cast<uintptr_t>(heap->mapping), heap->size);
			}
			*heap = {}; // Exactly 32 bytes; previous resources remain orphaned as native.
		}
	}
	Observe(KYTY_IPMI_CALLER, 0, heap, heap->handle, size, name, heap->mapping, result, heap->size);
	return result;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Destroy(Heap* heap) {
	const Handle handle = heap->handle;
	(void)RetireHeapMapping(&heap->handle, heap->mapping, heap->size);
	heap->mapping = nullptr;
	Observe(KYTY_IPMI_CALLER, 1, heap, handle, heap->size, nullptr, nullptr);
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MakeAllocator(Heap* heap, Allocator* out) {
	*out = Descriptor(heap); // Producers never inspect H, including null H.
	Observe(KYTY_IPMI_CALLER, 2, heap, nullptr, 32, nullptr, out);
}
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MakeAllocatorEx(Heap* heap, int32_t error, AllocatorEx* out) {
	out->allocator = Descriptor(heap);
	out->error = error; // Preserve bytes +0x24..+0x27.
	Observe(KYTY_IPMI_CALLER, 3, heap, nullptr, 40, nullptr, out, error);
}
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Malloc(Handle* slot, uint64_t size) {
	return Operate(slot, nullptr, size, 4, KYTY_IPMI_CALLER, nullptr);
}
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Realloc(Handle* slot, void* pointer, uint64_t size) {
	return Operate(slot, pointer, size, 5, KYTY_IPMI_CALLER, nullptr);
}
void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Free(Handle* slot, void* pointer) {
	(void)Operate(slot, pointer, 0, 6, KYTY_IPMI_CALLER, nullptr);
}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestFailNextHeapCreation() { fail_next_heap.store(true); }
#endif
} // namespace Libs::NpMemoryHeap
