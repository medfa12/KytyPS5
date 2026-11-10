namespace NpHeapTest {
namespace Np = Libs::NpMemoryHeap;
constexpr const char* Name = "NpMemoryHeap";
struct Api {
	Loader::SymbolDatabase symbols;
	decltype(&Np::Init) init;
	decltype(&Np::Destroy) destroy;
	decltype(&Np::MakeAllocator) descriptor;
	decltype(&Np::MakeAllocatorEx) descriptor_ex;
	decltype(&Np::Malloc) alloc;
	decltype(&Np::Realloc) realloc;
	decltype(&Np::Free) free;

	template<class F> F Resolve(const char* nid, F expected) {
		const auto* exact = symbols.Find({nid, "NpCommon", 1, "NpCommon", 1, 1, Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(Name, exact != nullptr && fallback != nullptr && exact->vaddr == reinterpret_cast<uint64_t>(expected) &&
		            fallback->vaddr == exact->vaddr, std::string("heap registration/early fallback: ") + nid);
		return reinterpret_cast<F>(exact->vaddr);
	}
	Api() {
		// Production ordering, including early JscdSmall and final ShellHits overrides.
		Libs::JscdSmall::InitJscdSmall_1(&symbols);
		Libs::OfflineLogin::InitOfflineLogin_1(&symbols);
		Libs::LibNpCommon::InitNpCommon_1(&symbols);
		Libs::LibNpManager::InitNpManager_1(&symbols);
		Libs::ShellHits::InitShellHits_1(&symbols);
		init = Resolve("kZizwrFvWZY", &Np::Init);
		destroy = Resolve("dfXSH2Tsjkw", &Np::Destroy);
		descriptor = Resolve("FaMNvjMA6to", &Np::MakeAllocator);
		descriptor_ex = Resolve("xHAiSVEEjSI", &Np::MakeAllocatorEx);
		alloc = Resolve("kwW5qddf+Lo", &Np::Malloc);
		realloc = Resolve("wsfyvM+VbUk", &Np::Realloc);
		free = Resolve("p1vvpKGRXe4", &Np::Free);
	}
};
unsigned maps = 0, unmaps = 0;
uintptr_t last_mapping = 0;
Np::Handle empty_slot = nullptr;
bool reentered = true;
Np::Allocator callback_target {};
void KYTY_SYSV_ABI RecordingHeapFree(void* raw, void* context) {
	NpMemoryTest::RecordingFree(raw, context);
	callback_target.free(raw, context);
}
void Mapped(uintptr_t address, size_t) {
	++maps;
	last_mapping = address;
	reentered &= Np::Malloc(&empty_slot, 1) == nullptr;
	(void)Libs::NpManagerMemory::GetAllocator();
}
void Unmapped(uintptr_t, size_t) {
	++unmaps;
	reentered &= Np::Malloc(&empty_slot, 1) == nullptr;
	(void)Libs::NpManagerMemory::GetAllocator();
}
bool Inside(const Np::Heap& heap, void* pointer, size_t size) {
	const auto p = reinterpret_cast<uintptr_t>(pointer), base = reinterpret_cast<uintptr_t>(heap.mapping);
	return p >= base && p - base <= heap.size && size <= heap.size - (p - base);
}
struct GuardedHeap { uint64_t before = 0x1122334455667788; Np::Heap heap {}; uint64_t after = 0x8877665544332211; };
void Canaries(const GuardedHeap& heap) {
	Check(Name, heap.before == 0x1122334455667788 && heap.after == 0x8877665544332211, "H writes exceeded 32 bytes");
}
} // namespace NpHeapTest

void TestNpMemoryHeap() {
	using namespace NpHeapTest;
	namespace Manager = Libs::NpManagerMemory;
	namespace Memory = Libs::LibKernel::Memory;
	Api api;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	_putenv_s("KYTY_IPMI_OBSERVE", "1");
	Libs::IpmiObserve::Initialize();
#endif
	Memory::RegisterCallbacks(Mapped, Unmapped);
	Check(Name, api.init(nullptr, 1, nullptr) == Np::InvalidHeap, "null H must return 80558005");
	GuardedHeap one, two;
	CheckOk(Name, api.init(&one.heap, 0x4001, "heap-one"), "Init first heap");
	Check(Name, one.heap.size == 0x8000 && one.heap.reserved == 0 && one.heap.mapping != nullptr && one.heap.handle != nullptr,
	      "actual H layout/backing");
	CheckOk(Name, api.init(&two.heap, 0x4000, "heap-two"), "Init second heap");
	Check(Name, two.heap.mapping != one.heap.mapping && two.heap.handle != one.heap.handle, "heaps share backing or handle");
	Memory::VirtualQueryInfo info {};
	CheckOk(Name, Memory::KernelVirtualQuery(one.heap.mapping, 0, &info, sizeof(info)), "query heap backing");
	Check(Name, info.protection == 3 && info.is_flexible && info.end - info.start == 0x8000, "heap kernel map contract");
	struct { uint64_t before = 0x1234; Np::Allocator value {}; uint64_t after = 0x5678; } descriptor;
	api.descriptor(&one.heap, &descriptor.value);
	const auto copy = descriptor.value;
	Check(Name, copy.allocate != nullptr && copy.reallocate != nullptr && copy.free != nullptr && copy.context == &one.heap &&
	            descriptor.before == 0x1234 && descriptor.after == 0x5678, "basic descriptor fields/canaries");
	void* raw = copy.allocate(0x60, copy.context); // Native Object::new(0x50) requests raw 0x60.
	Check(Name, raw == one.heap.mapping && Inside(one.heap, raw, 0x60), "Push raw callback size/backing or unwanted header");
	std::memset(raw, 0x5a, 0x60);
	struct { uint64_t before = 0x1357; Np::AllocatorEx value {}; uint64_t after = 0x2468; } extended;
	extended.value.padding = 0xaabbccdd;
	api.descriptor_ex(&two.heap, static_cast<int32_t>(0x8055a004u), &extended.value);
	Check(Name, extended.value.allocator.context == &two.heap && extended.value.error == static_cast<int32_t>(0x8055a004u) &&
	            extended.value.padding == 0xaabbccdd && extended.before == 0x1357 && extended.after == 0x2468 &&
	            extended.value.allocator.allocate == copy.allocate && extended.value.allocator.reallocate == copy.reallocate &&
	            extended.value.allocator.free == copy.free, "Ex fields/error/padding or callback ABI");
	Np::Allocator null_context {};
	api.descriptor(nullptr, &null_context);
	Check(Name, null_context.context == nullptr && null_context.allocate == copy.allocate, "producer inspected null H");
	Np::AllocatorEx null_ex {};
	null_ex.padding = 0xfeedbeef;
	api.descriptor_ex(nullptr, -7, &null_ex);
	Check(Name, null_ex.allocator.context == nullptr && null_ex.error == -7 && null_ex.padding == 0xfeedbeef,
	      "Ex producer inspected null H or overwrote padding");
	void* other = extended.value.allocator.allocate(0x80, &two.heap);
	Check(Name, Inside(two.heap, other, 0x80), "Ex callback did not honor H context");
	// Manager uses the same arena mechanics but remains independent, even with a nonnull context.
	CheckOk(Name, Manager::CreateKernel(0x4000, "heap-manager"), "manager coexistence");
	const auto manager = *Manager::GetAllocator();
	void* manager_raw = manager.allocate(32, &one.heap);
	Check(Name, manager_raw != nullptr && !Inside(one.heap, manager_raw, 1) && !Inside(two.heap, manager_raw, 1), "manager routed to H");
	api.free(&two.heap.handle, raw);
	Check(Name, api.realloc(&two.heap.handle, raw, 0x100) == nullptr &&
	            manager.reallocate(raw, 0x100, nullptr) == nullptr && copy.reallocate(other, 0x100, &one.heap) == nullptr &&
	            copy.reallocate(manager_raw, 0x100, &one.heap) == nullptr, "cross-heap pointer accepted");
	Check(Name, copy.reallocate(raw, 0x60, copy.context) == raw && static_cast<uint8_t*>(raw)[0x5f] == 0x5a,
	      "foreign free damaged owner");
	Np::Handle copied_slot = one.heap.handle;
	void* through_slot = api.alloc(&copied_slot, 0x20);
	Check(Name, Inside(one.heap, through_slot, 0x20), "direct helper requires a slot, not H identity");
	api.free(&copied_slot, through_slot);
	Check(Name, api.alloc(&empty_slot, 16) == nullptr && api.realloc(&empty_slot, raw, 16) == nullptr, "zero handle fallback allocation");
	api.free(&empty_slot, raw);
	api.free(&one.heap.handle, nullptr);
	const bool old_direct = Libs::IsDirectTitleBoot();
	for (bool direct: {false, true}) {
		Libs::SetDirectTitleBoot(direct);
		void* p = api.alloc(&two.heap.handle, 0x20);
		Check(Name, Inside(two.heap, p, 0x20), "direct/cohost gate or escaped arena");
		api.free(&two.heap.handle, p);
	}
	Libs::SetDirectTitleBoot(old_direct);
	std::atomic<bool> parallel_ok {true};
	std::atomic<unsigned> ready {0};
	auto exercise = [&](Np::Allocator allocator, const Np::Heap* owner) {
		++ready;
		while (ready.load() != 3) { std::this_thread::yield(); }
		// Manager event22 has its existing budget; keep this fixture below the
		// shared Gateway cap while exercising per-heap callback truncation heavily.
		for (unsigned i = 0; i < (owner == nullptr ? 4u : 64u); ++i) {
			void* p = allocator.allocate(0x40, allocator.context);
			if (p == nullptr || (owner != nullptr && !Inside(*owner, p, 0x40))) { parallel_ok = false; return; }
			std::memset(p, 0x79, 0x40);
			void* q = allocator.reallocate(p, 0x80, allocator.context);
			if (q == nullptr || static_cast<uint8_t*>(q)[0x3f] != 0x79 || (owner != nullptr && !Inside(*owner, q, 0x80))) {
				parallel_ok = false;
				allocator.free(p, allocator.context);
				return;
			}
			allocator.free(q, allocator.context);
		}
	};
	std::thread first(exercise, copy, &one.heap), second(exercise, extended.value.allocator, &two.heap), third(exercise, manager, nullptr);
	first.join(); second.join(); third.join();
	Check(Name, parallel_ok, "concurrent heaps/manager ownership or realloc preservation");
	// Fill one real arena, force realloc failure, then retain zero-size ownership.
	copy.free(raw, copy.context);
	// A guest-style header fixture uses a recording free that re-enters this heap.
	// The deleting-dtor adapter must have released its host lock before invoking it.
	callback_target = copy;
	Np::Allocator recording = copy;
	recording.free = RecordingHeapFree;
	void* header = copy.allocate(0x60, copy.context);
	auto* descriptor_pointer = &recording;
	std::memcpy(header, &descriptor_pointer, sizeof(descriptor_pointer));
	void* object = static_cast<uint8_t*>(header) + 0x10;
	Libs::LibNpCommon::NpMutexCtor(object);
	CheckOk(Name, Libs::LibNpCommon::NpMutexInit(object, nullptr), "heap-owned mutex Init");
	NpMemoryTest::delete_record = {};
	NpMemoryTest::delete_record.object = object;
	Libs::LibNpCommon::NpMutexDeletingDtor(object);
	const auto& deletion = NpMemoryTest::delete_record;
	Check(Name, deletion.calls == 1 && deletion.raw == header && deletion.context == &one.heap && deletion.uninitialized && deletion.depth == 0,
	      "guest callback must see destroyed mutex and re-enter per-heap free unlocked");
	void* a = api.alloc(&one.heap.handle, 0x100);
	void* tail = api.alloc(&one.heap.handle, one.heap.size - 0x100);
	std::memset(a, 0x6b, 0x100);
	Check(Name, a == one.heap.mapping && tail != nullptr && api.alloc(&one.heap.handle, 1) == nullptr &&
	            api.realloc(&one.heap.handle, a, 0x200) == nullptr && static_cast<uint8_t*>(a)[0xff] == 0x6b,
	      "exhaustion or realloc failure preservation");
	Check(Name, api.realloc(&one.heap.handle, a, 0) == a && static_cast<uint8_t*>(a)[0] == 0x6b, "zero realloc released pointer");
	void* shrink_tail = api.alloc(&one.heap.handle, 0xf0);
	Check(Name, shrink_tail == static_cast<uint8_t*>(a) + 16, "zero realloc did not retain minimum owned block");
	api.free(&one.heap.handle, static_cast<uint8_t*>(a) + 1);
	Check(Name, api.realloc(&one.heap.handle, a, 1) == a, "interior free damaged ownership");
	api.free(&one.heap.handle, a);
	Check(Name, api.alloc(&one.heap.handle, 16) == a, "zero realloc ownership not releasable");
	// Mapping failure leaves old handle/mapping, but writes reserved/rounded size first.
	const Np::Heap saved = one.heap;
	one.heap.reserved = UINT64_MAX;
	Check(Name, api.init(&one.heap, 0x4001, nullptr) == Libs::LibKernel::KERNEL_ERROR_EFAULT &&
	            one.heap.size == 0x8000 && one.heap.reserved == 0 && one.heap.mapping == saved.mapping && one.heap.handle == saved.handle,
	      "mapping failure did not preserve native partial writes");
	Check(Name, api.init(&one.heap, UINT64_MAX, "wrap") == Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	            one.heap.size == 0 && one.heap.mapping == saved.mapping && one.heap.handle == saved.handle,
	      "rounding must wrap before backend validation");
	one.heap = saved; // Restore caller-owned layout after intentional partial-write tests.
	one.heap.mapping = reinterpret_cast<void*>(UINTPTR_MAX - 0x3fff);
	Check(Name, api.init(&one.heap, 0x4000, "map-no-space") == Libs::LibKernel::KERNEL_ERROR_ENOMEM &&
	            one.heap.size == 0x4000 && one.heap.reserved == 0 && one.heap.mapping == nullptr && one.heap.handle == saved.handle,
	      "mapping failure must retain backend output writes and old handle");
	one.heap = saved;
	const auto before_duplicate_unmaps = unmaps;
	CheckOk(Name, api.init(&one.heap, 0x4000, "duplicate"), "duplicate Init");
	Check(Name, unmaps == before_duplicate_unmaps && one.heap.handle != saved.handle && one.heap.mapping != saved.mapping,
	      "duplicate Init rejected or implicitly destroyed old resources");
	Check(Name, api.realloc(&copied_slot, a, 16) == a, "copied old slot lost old heap on duplicate Init");
	void* current = copy.allocate(32, copy.context);
	Check(Name, current == one.heap.mapping, "copied descriptor did not follow H current heap");
	CheckOk(Name, Memory::KernelVirtualQuery(saved.mapping, 0, &info, sizeof(info)), "duplicate retained old mapping");
	// Destroy affects current heap only, keeps size/reserved, and does not edit descriptors.
	one.heap.reserved = 0x12345678;
	const auto descriptor_before = descriptor.value;
	const auto current_size = one.heap.size;
	api.destroy(&one.heap);
	Check(Name, one.heap.size == current_size && one.heap.reserved == 0x12345678 && one.heap.handle == nullptr && one.heap.mapping == nullptr &&
	            std::memcmp(&descriptor.value, &descriptor_before, sizeof(descriptor_before)) == 0, "Destroy layout/descriptor lifetime");
	Check(Name, copy.allocate(16, copy.context) == nullptr && copy.reallocate(current, 16, copy.context) == nullptr,
	      "postdestroy descriptor fell back to manager");
	copy.free(current, copy.context);
	Check(Name, api.realloc(&copied_slot, a, 16) == a && api.realloc(&two.heap.handle, other, 0x80) == other &&
	            manager.reallocate(manager_raw, 32, nullptr) == manager_raw, "Destroy crossed heap boundary");
	CheckOk(Name, api.init(&one.heap, 0x4000, "reinit"), "reinit after Destroy");
	Check(Name, copy.allocate(16, copy.context) == one.heap.mapping, "descriptor did not resume on reinit");
	api.destroy(&one.heap);
	Np::Heap orphan = saved; // Explicit guest-style cleanup of the saved old heap, not implicit reclamation.
	api.destroy(&orphan);
	Check(Name, api.alloc(&copied_slot, 16) == nullptr, "retired handle resurrected");
	// Failed post-map creation clears exactly H, unmaps new backing, retains old copied handle.
	const auto old_two = two.heap;
	Np::Handle old_two_slot = old_two.handle;
	const auto before_failure_maps = maps, before_failure_unmaps = unmaps;
	Np::TestFailNextHeapCreation();
	Check(Name, api.init(&two.heap, 0x4000, "rollback") == Manager::HeapFailure, "postmap failure status");
	const Np::Heap zero {};
	Check(Name, std::memcmp(&two.heap, &zero, sizeof(zero)) == 0 && maps == before_failure_maps + 1 && unmaps == before_failure_unmaps + 1,
	      "postmap failure did not clear exactly H / unmap new backing");
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(last_mapping), 0, &info, sizeof(info)) != 0 &&
	            api.realloc(&old_two_slot, other, 0x80) == other, "rollback mapping survived or destroyed prior heap");
	Np::Heap old_two_cleanup = old_two;
	api.destroy(&old_two_cleanup);
	api.destroy(&two.heap); // Valid zero heap: no unmap, preserves remaining fields.
	Manager::Destroy();
	Canaries(one);
	Canaries(two);
	Check(Name, reentered && Common::GetHostLockCriticalCounter()->load() == 0, "map/unmap re-entry or suspension guard leak");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto* observation = Libs::IpmiObserve::GetBuffer();
	unsigned callbacks = 0;
	bool saw_raw = false, saw_ex = false;
	for (size_t i = 0; i < std::min<uint64_t>(observation->attempted[4].load(), Libs::IpmiObserve::CAPACITIES[4]); ++i) {
		const auto& r = observation->slots[Libs::IpmiObserve::OFFSETS[4] + i].record;
		if (r.event != 23) { continue; }
		callbacks += r.values[1] >= 4 && r.values[1] <= 6;
		saw_raw |= r.values[1] == 4 && r.values[4] == 0x60 && r.values[6] == reinterpret_cast<uint64_t>(raw) &&
		           r.values[8] == reinterpret_cast<uint64_t>(&one.heap);
		saw_ex |= r.values[1] == 3 && r.values[7] == 0x8055a004u;
	}
	Check(Name, callbacks == 16 && saw_raw && saw_ex && observation->dropped[4] == 0, "actual heap observations/bounded callback budget");
	std::printf("NP heap observer: %ls\n", Libs::IpmiObserve::OutputPath());
#endif
}
