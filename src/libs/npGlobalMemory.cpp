#include "libs/npGlobalMemory.h"

#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/ipmiObserve.h"

#include <atomic>
#include <mutex>

namespace Libs::NpGlobalMemory {
namespace {
NpMemoryHeap::Heap heap {};
alignas(16) Allocator allocator {};
alignas(16) AllocatorEx allocator_ex {};
std::mutex descriptor_mutex;
std::atomic<uint32_t> observations {0};
std::mutex arch_mutex;
uint32_t arch_state = 0;
// SFMT19937: output index, three untouched padding words, 624 state words.
alignas(16) std::array<uint32_t, 628> generator {};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool clock_failure = false;
#endif

struct DescriptorLock {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock {descriptor_mutex};
};

struct ArchLock {
	Common::HostLockCriticalGuard critical;
	std::lock_guard<std::mutex> lock {arch_mutex};
};

void SeedGenerator(uint32_t seed) {
	ArchLock lock;
	auto* words = generator.data() + 4;
	words[0] = seed;
	for (uint32_t i = 1; i < 624; ++i) {
		const uint32_t previous = words[i - 1];
		words[i] = 0x6c078965u * (previous ^ (previous >> 30)) + i;
	}
	generator[0] = 624;
	uint32_t parity = (words[0] & 1u) ^ (words[3] & 0x13c9e684u);
	parity ^= parity >> 16;
	parity ^= parity >> 8;
	parity ^= parity >> 4;
	parity ^= parity >> 2;
	parity ^= parity >> 1;
	if ((parity & 1u) == 0) { words[0] ^= 1u; }
}

void Observe(uint64_t caller, uint32_t operation, const void* result) {
	if (!IpmiObserve::Enabled()) { return; }
	auto count = observations.load(std::memory_order_relaxed);
	do {
		if (count >= 8) { return; }
	} while (!observations.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
	IpmiObserve::Emit(IpmiObserve::Bank::Gateway, IpmiObserve::Event::NpGlobalResult,
	    {caller, operation, reinterpret_cast<uint64_t>(result), reinterpret_cast<uint64_t>(&heap),
	     heap.size, reinterpret_cast<uint64_t>(heap.mapping), reinterpret_cast<uint64_t>(heap.handle),
	     operation == 1 ? static_cast<uint32_t>(AllocationError) : 0});
}
} // namespace

Allocator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetGlobalAllocator() {
	Allocator prepared {};
	// Existing producer observations run outside the descriptor lock. The producer
	// does not inspect H, so this also works before Init and after Destroy.
	NpMemoryHeap::MakeAllocator(&heap, &prepared);
	{
		DescriptorLock lock;
		allocator = prepared;
	}
	Observe(KYTY_IPMI_CALLER, 0, &allocator);
	return &allocator;
}

AllocatorEx* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetGlobalAllocatorEx() {
	AllocatorEx prepared {};
	NpMemoryHeap::MakeAllocatorEx(&heap, AllocationError, &prepared);
	{
		DescriptorLock lock;
		allocator_ex.allocator = prepared.allocator;
		allocator_ex.error = prepared.error; // Write 36 bytes, never overwrite padding.
	}
	Observe(KYTY_IPMI_CALLER, 1, &allocator_ex);
	return &allocator_ex;
}

int32_t InitializeHeap() { return NpMemoryHeap::Init(&heap, HeapSize, "SceNpGlobalHeap"); }
void DestroyHeap() { NpMemoryHeap::Destroy(&heap); }

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ArchInit() {
	{
		ArchLock lock;
		if (arch_state != 0) {
			++arch_state; // Native wrapping uint32 increment, including UINT32_MAX -> 0.
			return 0;
		}
	}
	// Native prerequisite init is literally return 0. Read clock4 without any
	// NP host lock; its failure leaves the zero time used by the seed initializer.
	LibKernel::KernelTimespec time {};
	uint64_t microseconds = 0;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool fail = false;
	{ ArchLock lock; fail = clock_failure; }
	if (!fail && LibKernel::KernelClockGettime(4, &time) == 0) {
#else
	if (LibKernel::KernelClockGettime(4, &time) == 0) {
#endif
		microseconds = static_cast<uint64_t>(time.tv_sec) * 1000000 + static_cast<uint64_t>(time.tv_nsec) / 1000;
	}
	SeedGenerator(static_cast<uint32_t>(microseconds));
	const int32_t result = InitializeHeap();
	// Both native cleanup hooks are no-ops; preserve the heap failure. Notably,
	// this firmware does NOT set arch_state=1 after the first successful Init.
	return result < 0 ? result : 0;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ArchTerm() {
	uint32_t old = 0;
	{
		ArchLock lock;
		old = arch_state;
		arch_state = old - 1;
	}
	if (static_cast<int32_t>(old) > 1) { return; }
	DestroyHeap();
	// Generator termination and prerequisite termination are native no-ops.
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
uint32_t TestArchState() { ArchLock lock; return arch_state; }
void TestSetArchState(uint32_t value) { ArchLock lock; arch_state = value; }
void TestSetClockFailure(bool fail) { ArchLock lock; clock_failure = fail; }
std::array<uint32_t, 628> TestGeneratorState() { ArchLock lock; return generator; }
void TestSeedGenerator(uint32_t seed) { SeedGenerator(seed); }
#endif
} // namespace Libs::NpGlobalMemory
