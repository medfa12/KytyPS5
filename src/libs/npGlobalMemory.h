#pragma once

#include "libs/npMemoryHeap.h"
#include <array>

namespace Libs::NpGlobalMemory {
using Allocator = NpMemoryHeap::Allocator;
using AllocatorEx = NpMemoryHeap::AllocatorEx;
constexpr uint64_t HeapSize = 0x80000;
constexpr int32_t AllocationError = static_cast<int32_t>(0x80550005u);

// Separate process-lifetime descriptors, refreshed on every Get. Neither getter
// initializes the heap. Ex preserves its trailing four padding bytes.
Allocator* KYTY_SYSV_ABI GetGlobalAllocator();
AllocatorEx* KYTY_SYSV_ABI GetGlobalAllocatorEx();
int32_t KYTY_SYSV_ABI ArchInit();
void KYTY_SYSV_ABI ArchTerm();

// Internal host hooks only. Arch owns prerequisites and duplicate-call policy.
// Callers must serialize lifecycle operations and exclude concurrent callbacks.
// These delegate every requested operation to the existing H lifecycle unchanged.
int32_t InitializeHeap();
void DestroyHeap();
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
uint32_t TestArchState();
void TestSetArchState(uint32_t value);
void TestSetClockFailure(bool fail);
std::array<uint32_t, 628> TestGeneratorState();
void TestSeedGenerator(uint32_t seed);
#endif
} // namespace Libs::NpGlobalMemory
