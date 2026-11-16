#pragma once

#include "libs/npMemoryHeap.h"

namespace Libs::NpManagerIpcMemory {
using Allocator = NpManagerMemory::Allocator;
constexpr int32_t AlreadyInitialized = static_cast<int32_t>(0x80559d00u);
constexpr int32_t NotInitialized = static_cast<int32_t>(0x80559d01u);
constexpr int32_t InvalidMode = static_cast<int32_t>(0x80559d03u);
constexpr int32_t InvalidArgument = static_cast<int32_t>(0x80559d04u);

// Original 13.60 NpManager IPC memory, distinct from manager-global memory.
// CreateKernel returns status; it has no output-pointer argument.
int32_t KYTY_SYSV_ABI CreateKernel(size_t size, const char* name);
int32_t KYTY_SYSV_ABI CreateBuffer(size_t size, void* buffer, const char* name);
int32_t KYTY_SYSV_ABI Destroy();
Allocator* KYTY_SYSV_ABI GetAllocator();
void* KYTY_SYSV_ABI Allocate(size_t size, void* context);
void* KYTY_SYSV_ABI Reallocate(void* pointer, size_t size, void* context);
void KYTY_SYSV_ABI Free(void* pointer, void* context);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct Snapshot {
	void* base;
	size_t size;
	uint32_t mode, diagnostic_count, critical_depth;
};
Snapshot TestSnapshot();
#endif
} // namespace Libs::NpManagerIpcMemory
