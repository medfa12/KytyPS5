#pragma once

#include "common/abi.h"

#include <cstddef>
#include <cstdint>

namespace Libs::NpManagerMemory {

using AllocateCallback = void* (KYTY_SYSV_ABI *)(size_t, void*);
using ReallocateCallback = void* (KYTY_SYSV_ABI *)(void*, size_t, void*);
using FreeCallback = void (KYTY_SYSV_ABI *)(void*, void*);
struct Allocator {
	AllocateCallback allocate;
	ReallocateCallback reallocate;
	FreeCallback free;
	void* context;
};
static_assert(sizeof(Allocator) == 32 && offsetof(Allocator, context) == 24);

constexpr int32_t AlreadyInitialized = static_cast<int32_t>(0x80550001u);
constexpr int32_t NotInitialized = static_cast<int32_t>(0x80550002u);
constexpr int32_t InvalidArgument = static_cast<int32_t>(0x80550003u);
constexpr int32_t HeapFailure = static_cast<int32_t>(0x8055800au);

int32_t KYTY_SYSV_ABI CreateKernel(size_t size, const char* name);
int32_t KYTY_SYSV_ABI CreateBuffer(size_t size, void* buffer, const char* name);
Allocator* KYTY_SYSV_ABI GetAllocator();
int32_t KYTY_SYSV_ABI Destroy();
void* KYTY_SYSV_ABI Allocate(size_t size, void* context);
void* KYTY_SYSV_ABI Reallocate(void* pointer, size_t size, void* context);
void KYTY_SYSV_ABI Free(void* pointer, void* context);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct Snapshot {
	uintptr_t base;
	size_t size;
	size_t live_blocks;
	uint64_t diagnostic_count;
	uint32_t mode;
	uint32_t critical_depth;
};
Snapshot TestSnapshot();
void TestFailNextHeapCreation();
#endif
} // namespace Libs::NpManagerMemory
