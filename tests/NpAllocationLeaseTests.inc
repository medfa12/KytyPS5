namespace NpAllocationLeaseTest {
constexpr const char* Name = "NpAllocationLease";
namespace Np = Libs::NpMemoryHeap;
namespace Memory = Libs::LibKernel::Memory;
using Libs::LibKernel::KERNEL_ERROR_EBUSY;
unsigned gpu_changes = 0;
unsigned frees = 0;
void* reentry_address = nullptr;
unsigned reentry_attempts = 0;
bool reentry_denied = true;
void* retiring_address = nullptr;
uint64_t retiring_size = 0;
Np::Handle retiring_handle = nullptr;
unsigned retirement_reentries = 0;
void Mapped(uintptr_t, size_t) {}
void Gpu(bool, uint64_t, size_t) {
	++gpu_changes;
	if (retiring_address != nullptr) {
		void* address = retiring_address;
		retiring_address = nullptr; // One attempt, before the first host unmap.
		++retirement_reentries;
		Check(Name, Np::HasRetainedGeneration(retiring_handle), "immediate retirement was not published before observer");
		Np::Handle overlap = nullptr;
		Check(Name, Np::CreateHeap(&overlap, address, retiring_size, "np-retirement-overlap") < 0 && !overlap,
		      "observer adopted backing during immediate reclamation");
		Np::RetryRetirements(); // Must not recursively reclaim the same mapping.
	}
	if (reentry_address != nullptr) {
		std::shared_ptr<void> pin;
		++reentry_attempts;
		reentry_denied &= Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(reentry_address), 1, pin) != 0 && !pin;
	}
}
void Freed(uintptr_t, size_t) {
	++frees;
	// Reentry would deadlock if reclamation held either NP allocator lock.
	(void)Np::HasPendingRetirements();
	Np::RetryRetirements();
}
std::shared_ptr<void> Lease(void* address, size_t size) {
	std::shared_ptr<void> token;
	Check(Name, Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(address), size, token) == 0 && token,
	      "actual flexible allocation lease acquisition failed");
	return token;
}
} // namespace NpAllocationLeaseTest

void TestNpAllocationLease() {
	using namespace NpAllocationLeaseTest;
	Memory::RegisterCallbacks(Mapped, Freed);
	Memory::TestSetGpuRangeCallback(Gpu);
	Np::Heap heap {};
	CheckOk(Name, Np::Init(&heap, 0xc000, "np-lease"), "heap init");
	auto* a = static_cast<unsigned char*>(Np::Malloc(&heap.handle, 64));
	auto* b = static_cast<unsigned char*>(Np::Malloc(&heap.handle, 64));
	Check(Name, a && b, "heap allocations");
	std::memset(a, 0x5a, 64);
	std::memset(b, 0xa5, 64);
	auto first = Lease(a + 16, 32);
	auto second = Lease(a, 64);
	std::shared_ptr<void> rejected;
	Check(Name, Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(a + 63), 2, rejected) != 0 && !rejected,
	      "cross-allocation range accepted");
	Check(Name, Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(a), 0, rejected) != 0 && !rejected,
	      "empty range accepted");
	for (const auto size: {size_t {0}, size_t {16}, size_t {64}, size_t {4096}}) {
		Check(Name, Np::Realloc(&heap.handle, a, size) == nullptr && a[0] == 0x5a && a[63] == 0x5a,
		      "leased realloc changed the original allocation");
	}
	Np::Free(&heap.handle, a + 1); // Interior frees must not retire the allocation.
	auto third = Lease(a, 1);
	third.reset();
	Np::Free(&heap.handle, a);
	Check(Name, Np::HasPendingRetirements(), "deferred exact-base free was not retained");
	Check(Name, Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(a), 1, rejected) != 0,
	      "logically freed allocation admitted a new lease");
	first.reset();
	Np::RetryRetirements();
	void* fresh = Np::Malloc(&heap.handle, 64);
	Check(Name, fresh && fresh != a, "first release reused a multiply leased allocation");
	Np::Free(&heap.handle, fresh);
	const auto before_release = frees;
	{
		std::mutex transport_mutex;
		std::lock_guard lock(transport_mutex);
		second.reset(); // Must never unmap or call guest/kernel observers here.
	}
	Check(Name, frees == before_release, "token destructor reclaimed backing under transport lock");
	Np::RetryRetirements();
	Check(Name, Np::Malloc(&heap.handle, 64) == a, "final release did not permit exact span reuse");
	{
		auto live = Lease(b, 64);
	}
	Np::RetryRetirements();
	fresh = Np::Malloc(&heap.handle, 64);
	Check(Name, fresh != b && b[63] == 0xa5, "release freed an allocation with no free request");
	Np::Free(&heap.handle, fresh);
	auto retained = Lease(a, 64);
	const auto base = reinterpret_cast<uintptr_t>(heap.mapping);
	const auto length = heap.size;
	const auto old_handle = heap.handle;
	Np::Destroy(&heap);
	Check(Name, heap.handle == nullptr && heap.mapping == nullptr && Np::HasRetainedGeneration(old_handle),
	      "heap logical retirement lost its retained generation");
	const auto before_conflicts = gpu_changes;
	Check(Name, Memory::KernelMunmap(base, length) == KERNEL_ERROR_EBUSY, "unmap consumed a pin");
	Check(Name, Memory::KernelMprotect(reinterpret_cast<void*>(base), length, 1) == KERNEL_ERROR_EBUSY,
	      "protection removal accepted");
	void* fixed = reinterpret_cast<void*>(base);
	Check(Name, Memory::KernelMapNamedFlexibleMemory(&fixed, length, 3, 0x10, "np-conflict") == KERNEL_ERROR_EBUSY &&
	            fixed == reinterpret_cast<void*>(base), "fixed flexible replacement accepted");
	Check(Name, Memory::KernelReserveVirtualRange(&fixed, length, 0x10, 0x4000) < 0,
	      "fixed reservation replaced pinned backing");
	int64_t physical = 0;
	CheckOk(Name, Memory::KernelAllocateDirectMemory(0, Memory::KernelGetDirectMemorySize(), 0x4000, 0x4000, 0, &physical),
	        "direct allocation for replacement conflict");
	Check(Name, Memory::KernelMapDirectMemory(&fixed, 0x4000, 3, 0x10, physical, 0x4000) == KERNEL_ERROR_EBUSY,
	      "direct replacement accepted");
	CheckOk(Name, Memory::KernelReleaseDirectMemory(physical, 0x4000), "release direct test backing");
	Np::Handle overlap = nullptr;
	Check(Name, Np::CreateHeap(&overlap, reinterpret_cast<void*>(base), length, "np-overlap") < 0 && !overlap,
	      "new arena overlapped retained generation");
	Check(Name, gpu_changes == before_conflicts && a[0] == 0x5a && a[63] == 0x5a,
	      "rejected mutation partially changed GPU/CPU backing");
	Np::RetryRetirements();
	Check(Name, Np::HasRetainedGeneration(old_handle), "retry ignored outstanding allocation lease");
	Check(Name, Np::RetryAllocationRetirements() == 16, "pending retirement reported drained");
	retained.reset();
	Memory::TestFailGuestBackingStoreUnmapAfter(0);
	Check(Name, Np::RetryAllocationRetirements() == 16 && Np::HasRetainedGeneration(old_handle),
	      "failed physical reclamation lost retained ownership");
	Np::RetryRetirements();
	Check(Name, !Np::HasRetainedGeneration(old_handle) && Memory::TestPlaceholderRangeIsFree(base, length),
	      "final release/retry did not really unmap retired heap");

	// A hole and an unpinned prefix must not be partially consumed before a
	// pinned suffix is discovered by the sparse-unmap path.
	void* reservation = nullptr;
	CheckOk(Name, Memory::KernelReserveVirtualRange(&reservation, 0x10000, 0, 0x4000), "sparse fixture reservation");
	const auto sparse = reinterpret_cast<uintptr_t>(reservation);
	void* prefix = reservation;
	void* suffix = reinterpret_cast<void*>(sparse + 0x8000);
	CheckOk(Name, Memory::KernelMapNamedFlexibleMemory(&prefix, 0x4000, 3, 0x10, "np-prefix"), "prefix map");
	CheckOk(Name, Memory::KernelMapNamedFlexibleMemory(&suffix, 0x8000, 3, 0x10, "np-suffix"), "suffix map");
	CheckOk(Name, Memory::KernelMunmap(sparse + 0x4000, 0x4000), "make sparse hole");
	Np::Handle standalone = nullptr;
	CheckOk(Name, Np::CreateHeap(&standalone, static_cast<unsigned char*>(suffix) + 0x1000, 0x2000, "np-standalone"), "standalone heap");
	void* object = Np::Malloc(&standalone, 32);
	auto sparse_pin = Lease(object, 32);
	const auto before_sparse = gpu_changes;
	Check(Name, Memory::KernelMprotect(suffix, 0x4000, 1) == KERNEL_ERROR_EBUSY,
	      "pin did not cover the containing mapping outside heap bounds");
	Check(Name, Memory::KernelMunmap(sparse, 0x10000) == KERNEL_ERROR_EBUSY && gpu_changes == before_sparse,
	      "sparse unmap partially removed an unpinned prefix");
	Np::DestroyHeap(&standalone);
	sparse_pin.reset();
	Np::RetryRetirements();
	CheckOk(Name, Memory::KernelMunmap(sparse, 0x10000), "unmap sparse backing after drain");

	// A heap whose unleased mapping was replaced must never pin the new mapping.
	Np::Heap stale {};
	CheckOk(Name, Np::Init(&stale, 0x4000, "np-stale"), "stale heap init");
	object = Np::Malloc(&stale.handle, 32);
	fixed = stale.mapping;
	reentry_address = object;
	CheckOk(Name, Memory::KernelMapNamedFlexibleMemory(&fixed, stale.size, 3, 0x10, "np-new-generation"), "mapping replacement");
	reentry_address = nullptr;
	Check(Name, reentry_attempts != 0 && reentry_denied, "mutation observer admitted a lease between preflight and publication");
	Check(Name, Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(object), 32, rejected) != 0 && !rejected,
	      "stale heap pinned a reused mapping generation");
	const auto replacement_base = reinterpret_cast<uintptr_t>(fixed);
	const auto replacement_size = stale.size;
	static_cast<unsigned char*>(fixed)[0] = 0x39;
	Np::Destroy(&stale);
	uint64_t replacement_generation = 0;
	auto replacement_pin = Memory::AcquireFlexibleMappingLease(replacement_base, replacement_size, 0, &replacement_generation);
	Check(Name, replacement_pin && static_cast<unsigned char*>(fixed)[0] == 0x39,
	      "stale heap destruction unmapped the replacement generation");
	replacement_pin.reset();
	CheckOk(Name, Memory::KernelMunmap(replacement_base, replacement_size), "replacement mapping cleanup");

	// Immediate reclamation must reserve ownership before an observer can
	// publish a new arena over the still-readable original backing.
	Np::Heap immediate {};
	CheckOk(Name, Np::Init(&immediate, 0x4000, "np-immediate-retirement"), "immediate heap init");
	retiring_address = immediate.mapping;
	retiring_size = immediate.size;
	retiring_handle = immediate.handle;
	const auto immediate_base = reinterpret_cast<uintptr_t>(immediate.mapping);
	const auto before_reentries = retirement_reentries;
	Np::Destroy(&immediate);
	Check(Name, retirement_reentries == before_reentries + 1 &&
	            !Np::HasRetainedGeneration(retiring_handle) && Memory::TestPlaceholderRangeIsFree(immediate_base, retiring_size),
	      "immediate retirement reentry did not drain exactly once");

	// Split VirtualRanges so the first range commits before the second unmap
	// fails. Retry must accept the resulting hole and preserve a new generation
	// subsequently mapped into that hole.
	Np::Heap split {};
	CheckOk(Name, Np::Init(&split, 0x8000, "np-partial-retirement"), "split heap init");
	const auto split_base = reinterpret_cast<uintptr_t>(split.mapping);
	const auto split_handle = split.handle;
	CheckOk(Name, Memory::KernelMprotect(reinterpret_cast<void*>(split_base + 0x4000), 0x4000, 1), "split protection");
	Memory::TestFailGuestBackingStoreUnmapAfter(1);
	Np::Destroy(&split);
	Check(Name, Np::HasRetainedGeneration(split_handle) && Memory::TestPlaceholderRangeIsFree(split_base, 0x4000) &&
	            !Memory::TestPlaceholderRangeIsFree(split_base + 0x4000, 0x4000),
	      "partial unmap did not retain the surviving original span");
	fixed = reinterpret_cast<void*>(split_base);
	CheckOk(Name, Memory::KernelMapNamedFlexibleMemory(&fixed, 0x4000, 3, 0x10, "np-partial-replacement"), "replace retired prefix");
	static_cast<unsigned char*>(fixed)[0] = 0x71;
	Np::RetryRetirements();
	Check(Name, !Np::HasRetainedGeneration(split_handle) && Memory::TestPlaceholderRangeIsFree(split_base + 0x4000, 0x4000),
	      "partial retirement could not reclaim its surviving original span");
	replacement_pin = Memory::AcquireFlexibleMappingLease(split_base, 0x4000, 0, &replacement_generation);
	Check(Name, replacement_pin && static_cast<unsigned char*>(fixed)[0] == 0x71,
	      "partial retirement retry consumed a replacement generation");
	replacement_pin.reset();
	CheckOk(Name, Memory::KernelMunmap(split_base, 0x4000), "partial replacement cleanup");
	alignas(16) unsigned char host_backing[0x1000] {};
	Np::Handle host_heap = nullptr;
	CheckOk(Name, Np::CreateHeap(&host_heap, host_backing, sizeof(host_backing), "np-host"), "legacy host-backed heap");
	object = Np::Malloc(&host_heap, 32);
	Check(Name, object && Np::AcquireAllocationLease(reinterpret_cast<uintptr_t>(object), 32, rejected) == 45 && !rejected,
	      "unmanaged host backing received a fake mapping pin");
	Np::DestroyHeap(&host_heap);

	namespace Ipc = Libs::NpManagerIpcMemory;
	CheckOk(Name, Ipc::CreateKernel(0x4000, "np-ipc-retain"), "IPC heap create");
	object = Ipc::Allocate(32, nullptr);
	auto ipc_pin = Lease(object, 32);
	Check(Name, Ipc::Destroy() == KERNEL_ERROR_EBUSY, "IPC destroy claimed physical retirement");
	Check(Name, Ipc::CreateKernel(0x4000, "np-ipc-replace") == Ipc::AlreadyInitialized,
	      "global IPC descriptor replaced a retained generation");
	ipc_pin.reset();
	Np::RetryRetirements();
	CheckOk(Name, Ipc::CreateKernel(0x4000, "np-ipc-replace"), "IPC replacement after actual retirement");
	CheckOk(Name, Ipc::Destroy(), "IPC final destroy");
	Check(Name, Np::RetryAllocationRetirements() == 0, "cleanup retained an unreclaimed generation");
	Memory::TestSetGpuRangeCallback(nullptr);
}
