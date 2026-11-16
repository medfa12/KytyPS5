// Include after libs/npGlobalMemory.h and the full-HLE test support declarations.
// Run in its own focused mode: mapping callbacks are installed once per process.
namespace NpGlobalTest {
namespace Global = Libs::NpGlobalMemory;
namespace Heap = Libs::NpMemoryHeap;
namespace Manager = Libs::NpManagerMemory;
namespace Memory = Libs::LibKernel::Memory;
constexpr const char* Name = "NpGlobalMemory";
unsigned maps = 0, unmaps = 0;
uintptr_t last_mapping = 0;
bool reentered = true;
Global::Allocator* expected_basic = nullptr;
Global::AllocatorEx* expected_ex = nullptr;

bool Same(const Global::Allocator& a, const Global::Allocator& b) {
	return a.allocate == b.allocate && a.reallocate == b.reallocate && a.free == b.free && a.context == b.context;
}
void Reenter() {
	reentered &= Global::GetGlobalAllocator() == expected_basic && Global::GetGlobalAllocatorEx() == expected_ex;
}
void Mapped(uintptr_t address, size_t) {
	++maps;
	last_mapping = address;
	Reenter(); // Would deadlock if a global descriptor lock spanned mapping.
}
void Unmapped(uintptr_t, size_t) {
	++unmaps;
	Reenter();
}
size_t Available() {
	size_t size = 0;
	CheckOk(Name, Memory::KernelAvailableFlexibleMemorySize(&size), "available flexible memory");
	return size;
}
void Inactive(const Global::Allocator& descriptor) {
	Check(Name, descriptor.allocate(0x40, descriptor.context) == nullptr &&
	            descriptor.reallocate(nullptr, 0x40, descriptor.context) == nullptr,
	      "inactive global callbacks allocated or fell back to another heap");
	descriptor.free(nullptr, descriptor.context);
}
} // namespace NpGlobalTest

void TestNpGlobalMemory() {
	using namespace NpGlobalTest;
	const auto initial_depth = Common::GetHostLockCriticalCounter()->load();
	const auto baseline = Available();
	auto* basic = Global::GetGlobalAllocator();
	auto* extended = Global::GetGlobalAllocatorEx();
	Check(Name, basic != nullptr && extended != nullptr && basic != &extended->allocator, "distinct persistent descriptors");
	const auto copy = *basic;
	const auto ex_copy = extended->allocator;
	auto* context = static_cast<Heap::Heap*>(copy.context);
	Check(Name, context != nullptr && Same(copy, ex_copy) && copy.allocate != nullptr && copy.reallocate != nullptr &&
	            copy.free != nullptr && context->size == 0 && context->reserved == 0 && context->mapping == nullptr &&
	            context->handle == nullptr, "preinit global H/callbacks");
	Check(Name, extended->error == Global::AllocationError, "Ex allocation-error metadata");
	Inactive(copy);
	Inactive(ex_copy);
	Check(Name, Available() == baseline && extended->error == Global::AllocationError,
	      "getter lazily mapped backing or inactive callbacks changed Ex error");

	// Each getter repopulates only its own descriptor; Ex's trailing bytes survive.
	extended->padding = 0x1234abcd;
	*basic = {};
	Check(Name, Global::GetGlobalAllocator() == basic && Same(*basic, copy), "basic getter did not restore fields");
	Check(Name, Same(extended->allocator, ex_copy) && extended->padding == 0x1234abcd, "basic getter changed Ex storage");
	extended->allocator = {};
	extended->error = 0;
	Check(Name, Global::GetGlobalAllocatorEx() == extended && Same(extended->allocator, ex_copy) &&
	            extended->error == Global::AllocationError && extended->padding == 0x1234abcd && Same(*basic, copy),
	      "Ex getter did not restore fields/error or overwrote padding/basic storage");
	expected_basic = basic;
	expected_ex = extended;
	Memory::RegisterCallbacks(Mapped, Unmapped);

	// Fail after a real map: backing must be returned and H cleared, while both
	// persistent descriptors and the Ex error remain usable as inactive metadata.
	const auto before_failure_maps = maps, before_failure_unmaps = unmaps;
	Heap::TestFailNextHeapCreation();
	Check(Name, Global::InitializeHeap() == Manager::HeapFailure, "post-map global heap failure status");
	const Heap::Heap empty {};
	Check(Name, std::memcmp(context, &empty, sizeof(empty)) == 0 && maps == before_failure_maps + 1 &&
	            unmaps == before_failure_unmaps + 1 && Available() == baseline, "global rollback leaked backing or H state");
	Memory::VirtualQueryInfo info {};
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(last_mapping), 0, &info, sizeof(info)) != 0,
	      "failed global mapping still published");
	Check(Name, Global::GetGlobalAllocator() == basic && Global::GetGlobalAllocatorEx() == extended &&
	            extended->error == Global::AllocationError && extended->padding == 0x1234abcd, "failure invalidated descriptors");
	Inactive(copy);
	Inactive(ex_copy);
	Check(Name, extended->error == Global::AllocationError && extended->padding == 0x1234abcd,
	      "failed-heap callbacks changed Ex metadata");

	CheckOk(Name, Global::InitializeHeap(), "initialize global heap");
	Check(Name, context->size == 0x80000 && context->reserved == 0 && context->mapping != nullptr && context->handle != nullptr &&
	            Available() == baseline - 0x80000, "global heap extent/backing");
	CheckOk(Name, Memory::KernelVirtualQuery(context->mapping, 0, &info, sizeof(info)), "query global backing");
	Check(Name, info.start == reinterpret_cast<uintptr_t>(context->mapping) && info.end - info.start == 0x80000 &&
	            info.protection == 3 && info.is_flexible && std::strcmp(info.name, "SceNpGlobalHeap") == 0,
	      "global map name/protection/size");
	Heap::Heap other {};
	CheckOk(Name, Heap::Init(&other, 0x4000, "global-test-other"), "initialize independent heap");
	Heap::Allocator other_descriptor {};
	Heap::MakeAllocator(&other, &other_descriptor);
	CheckOk(Name, Manager::CreateKernel(0x4000, "global-test-manager"), "initialize independent manager");
	auto* manager = Manager::GetAllocator();
	Check(Name, manager != nullptr && manager != basic && manager != &extended->allocator && manager->context == nullptr &&
	            other_descriptor.context != context && other.mapping != context->mapping && other.handle != context->handle &&
	            copy.allocate == other_descriptor.allocate && copy.reallocate == other_descriptor.reallocate &&
	            copy.free == other_descriptor.free, "global/manager/per-heap identities or heap callbacks differ");
	void* other_raw = other_descriptor.allocate(0x40, other_descriptor.context);
	void* manager_raw = manager->allocate(0x40, nullptr);
	Check(Name, other_raw != nullptr && manager_raw != nullptr && other_raw != manager_raw, "independent allocations failed");

	// Native Ex-new requests raw 0x40; the adapter must not add another header.
	void* raw = ex_copy.allocate(0x40, ex_copy.context);
	Check(Name, raw == context->mapping && raw != other_raw && raw != manager_raw, "native raw 0x40 request/header or ownership");
	std::memset(raw, 0x6a, 0x40);
	void* tail = copy.allocate(0x80000 - 0x40, copy.context);
	Check(Name, tail == static_cast<uint8_t*>(raw) + 0x40 && copy.allocate(1, copy.context) == nullptr &&
	            ex_copy.reallocate(raw, 0x80, ex_copy.context) == nullptr && static_cast<uint8_t*>(raw)[0x3f] == 0x6a,
	      "global arena is not bounded or failed realloc damaged allocation");
	other_descriptor.free(raw, other_descriptor.context);
	manager->free(raw, nullptr);
	Check(Name, other_descriptor.reallocate(raw, 0x40, &other) == nullptr &&
	            copy.reallocate(other_raw, 0x40, copy.context) == nullptr &&
	            copy.reallocate(manager_raw, 0x40, copy.context) == nullptr &&
	            copy.reallocate(raw, 0x40, copy.context) == raw, "cross-heap ownership accepted or foreign free damaged global heap");
	copy.free(tail, copy.context);

	const auto descriptor_before = *basic;
	const auto extended_before = *extended;
	void* old_mapping = context->mapping;
	Global::DestroyHeap(); // Outstanding global allocation is invalidated; descriptors survive.
	Check(Name, context->mapping == nullptr && context->handle == nullptr && context->size == 0x80000 &&
	            Same(*basic, descriptor_before) && std::memcmp(extended, &extended_before, sizeof(*extended)) == 0,
	      "Destroy changed descriptors or retained active H");
	Check(Name, Memory::KernelVirtualQuery(old_mapping, 0, &info, sizeof(info)) != 0, "Destroy retained global mapping");
	Check(Name, Global::GetGlobalAllocator() == basic && Global::GetGlobalAllocatorEx() == extended &&
	            extended->error == Global::AllocationError && extended->padding == 0x1234abcd, "post-Destroy descriptor identity/error");
	Inactive(copy);
	Inactive(ex_copy);
	Check(Name, copy.reallocate(raw, 0x40, copy.context) == nullptr, "retired global block reallocated");
	copy.free(raw, copy.context);
	Check(Name, extended->error == Global::AllocationError && extended->padding == 0x1234abcd,
	      "post-Destroy callbacks changed Ex metadata");
	Check(Name, other_descriptor.reallocate(other_raw, 0x40, &other) == other_raw &&
	            manager->reallocate(manager_raw, 0x40, nullptr) == manager_raw, "global Destroy affected other owners");

	CheckOk(Name, Global::InitializeHeap(), "reinitialize global heap");
	void* resumed = copy.allocate(0x40, copy.context);
	Check(Name, resumed == context->mapping && Global::GetGlobalAllocator() == basic &&
	            Global::GetGlobalAllocatorEx() == extended && extended->error == Global::AllocationError &&
	            extended->padding == 0x1234abcd, "copied descriptor did not follow stable H on reinit");
	ex_copy.free(resumed, ex_copy.context);
	Global::DestroyHeap();
	Heap::Destroy(&other);
	CheckOk(Name, Manager::Destroy(), "destroy independent manager");
	Check(Name, Available() == baseline && reentered && Common::GetHostLockCriticalCounter()->load() == initial_depth,
	      "global lifecycle leaked backing/critical scope or mapping getter re-entry failed");
}

namespace NpGlobalTest {
struct RegisteredApi {
	Loader::SymbolDatabase symbols;
	using Init = int32_t (KYTY_SYSV_ABI *)();
	using Term = void (KYTY_SYSV_ABI *)();
	Init init = nullptr;
	Term term = nullptr;
	decltype(&Global::GetGlobalAllocator) basic = nullptr;
	decltype(&Global::GetGlobalAllocatorEx) extended = nullptr;

	uint64_t Resolve(const char* nid, uint64_t expected) {
		const auto* exact = symbols.Find({nid, "NpCommon", 1, "NpCommon", 1, 1, Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(Name, exact != nullptr && fallback != nullptr && exact->vaddr == expected && fallback->vaddr == expected,
		      std::string("global/Arch exact/fallback registration: ") + nid);
		return exact->vaddr;
	}
	RegisteredApi() {
		// Production order: both the early libC registration and final ShellHits
		// override must select the same handler, including first-insertion fallback.
		Libs::JscdSmall::InitJscdSmall_1(&symbols);
		Libs::OfflineLogin::InitOfflineLogin_1(&symbols);
		Libs::LibNpCommon::InitNpCommon_1(&symbols);
		Libs::LibNpManager::InitNpManager_1(&symbols);
		Libs::ShellHits::InitShellHits_1(&symbols);
		init = reinterpret_cast<Init>(Resolve("jGF+MaB4b-M", reinterpret_cast<uint64_t>(&Global::ArchInit)));
		term = reinterpret_cast<Term>(Resolve("UskWpVWxSvg", reinterpret_cast<uint64_t>(&Global::ArchTerm)));
		basic = reinterpret_cast<decltype(basic)>(Resolve("DI5n4aOdxmk", reinterpret_cast<uint64_t>(&Global::GetGlobalAllocator)));
		extended = reinterpret_cast<decltype(extended)>(Resolve("P4YpPziLBd4", reinterpret_cast<uint64_t>(&Global::GetGlobalAllocatorEx)));
	}
};
} // namespace NpGlobalTest

void TestNpGlobalMemoryRegistrations() {
	using namespace NpGlobalTest;
	RegisteredApi api;
	Check(Name, api.basic() == Global::GetGlobalAllocator() && api.extended() == Global::GetGlobalAllocatorEx(),
	      "registered getters returned different descriptor identities");
}

// Run AFTER TestNpGlobalMemory in the same focused mode. Reuse its one-time
// mapping callback registration, but take fresh memory/counter baselines here.
void TestNpGlobalArchLifecycle() {
	using namespace NpGlobalTest;
	RegisteredApi api;
	struct RestoreClock {
		~RestoreClock() { Global::TestSetClockFailure(false); }
	} restore_clock;
	const auto baseline = Available();
	const auto initial_depth = Common::GetHostLockCriticalCounter()->load();
	const auto initial_maps = maps, initial_unmaps = unmaps;
	auto* basic = api.basic();
	auto* extended = api.extended();
	Check(Name, basic == expected_basic && extended == expected_ex && reentered,
	      "Arch fixture requires direct-owner test callback setup");
	const auto descriptor = *basic;
	const auto ex_descriptor = extended->allocator;
	auto* context = static_cast<Heap::Heap*>(descriptor.context);
	Check(Name, Global::TestArchState() == 0 && context != nullptr && context->mapping == nullptr && context->handle == nullptr,
	      "Arch must start at state zero with inactive global H");
	extended->padding = 0x89abcdef;
	const auto extended_before = *extended;
	auto descriptors_survive = [&] {
		// Check storage before getters can repopulate it and hide lifecycle writes.
		Check(Name, Same(*basic, descriptor) && std::memcmp(extended, &extended_before, sizeof(*extended)) == 0,
		      "Arch lifecycle changed persistent descriptor storage");
		Check(Name, api.basic() == basic && api.extended() == extended && Same(*basic, descriptor) &&
		            Same(extended->allocator, ex_descriptor) && extended->error == Global::AllocationError &&
		            extended->padding == 0x89abcdef, "Arch getter identity/context/error/padding");
	};
	Memory::VirtualQueryInfo info {};
	auto mapped_global = [&](void* mapping) {
		CheckOk(Name, Memory::KernelVirtualQuery(mapping, 0, &info, sizeof(info)), "Arch global mapping query");
		const auto base = reinterpret_cast<uintptr_t>(mapping);
		// Adjacent identically named flexible maps may coalesce in VirtualQuery;
		// each H extent and the backing charge are checked separately below.
		Check(Name, info.start <= base && info.end >= base && info.end - base >= Global::HeapSize &&
		            info.protection == 3 && info.is_flexible && std::strcmp(info.name, "SceNpGlobalHeap") == 0,
		      "Arch global backing contract");
	};

	// A negative post-map heap result propagates without setting the state or
	// keeping the new backing. This calls the actual registered Arch entry point.
	Heap::TestFailNextHeapCreation();
	Check(Name, api.init() == Manager::HeapFailure && Global::TestArchState() == 0,
	      "Arch post-map failure status/state");
	const Heap::Heap empty {};
	Check(Name, std::memcmp(context, &empty, sizeof(empty)) == 0 && Available() == baseline &&
	            maps == initial_maps + 1 && unmaps == initial_unmaps + 1,
	      "Arch failed initialization leaked backing or H state");
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(last_mapping), 0, &info, sizeof(info)) != 0,
	      "Arch failed mapping remains published");
	descriptors_survive();
	Inactive(descriptor);
	Inactive(ex_descriptor);

	Global::TestSetClockFailure(true);
	const auto before_seed = Global::TestGeneratorState();
	CheckOk(Name, api.init(), "Arch state-zero initialization despite clock failure");
	const auto zero_seed = Global::TestGeneratorState();
	// Native SFMT19937 seed-zero known answers, not a second implementation of
	// the recurrence or parity certification in the test.
	Check(Name, zero_seed[0] == 624 && zero_seed[4] == 0 && zero_seed[5] == 1 &&
	            zero_seed[6] == 0x6c078967 && zero_seed[7] == 0x714acb41 && zero_seed[627] == 0x6b1a1930,
	      "ignored clock failure must seed the persistent SFMT19937 state with zero");
	Check(Name, zero_seed[1] == before_seed[1] && zero_seed[2] == before_seed[2] && zero_seed[3] == before_seed[3],
	      "generator seeding changed its 12-byte padding");
	Check(Name, Global::TestArchState() == 0 && context->size == Global::HeapSize && context->handle != nullptr &&
	            maps == initial_maps + 2 && unmaps == initial_unmaps + 1 && Available() == baseline - Global::HeapSize,
	      "successful ArchInit must leave state zero and create one heap");
	mapped_global(context->mapping);
	void* previous_raw = descriptor.allocate(0x40, descriptor.context);
	Check(Name, previous_raw == context->mapping, "Arch global raw allocation");
	std::memset(previous_raw, 0x73, 0x40);
	Heap::Heap previous = *context;
	Heap::Handle previous_slot = previous.handle;

	// Success deliberately did not set state=1: a second call creates another
	// mapping and leaves the old resources alive, without implicit destruction.
	CheckOk(Name, api.init(), "duplicate ArchInit while state remains zero");
	Check(Name, Global::TestArchState() == 0 && context->size == Global::HeapSize &&
	            context->mapping != previous.mapping && context->handle != previous.handle &&
	            maps == initial_maps + 3 && unmaps == initial_unmaps + 1 && Available() == baseline - 2 * Global::HeapSize,
	      "duplicate ArchInit refused, reused or implicitly destroyed old resources");
	mapped_global(previous.mapping);
	mapped_global(context->mapping);
	Check(Name, Heap::Realloc(&previous_slot, previous_raw, 0x40) == previous_raw &&
	            static_cast<uint8_t*>(previous_raw)[0x3f] == 0x73, "duplicate ArchInit invalidated old heap ownership/data");
	void* current_raw = ex_descriptor.allocate(0x40, ex_descriptor.context);
	Check(Name, current_raw == context->mapping && current_raw != previous_raw, "copied descriptor did not follow current global H");
	descriptors_survive();

	// Explicit fixture cleanup only AFTER proving orphan survival. Preserve a
	// copied slot to verify retirement without changing the current global H.
	void* previous_mapping = previous.mapping;
	Heap::Destroy(&previous);
	Check(Name, unmaps == initial_unmaps + 2 && Available() == baseline - Global::HeapSize &&
	            Heap::Malloc(&previous_slot, 0x40) == nullptr &&
	            Memory::KernelVirtualQuery(previous_mapping, 0, &info, sizeof(info)) != 0 &&
	            descriptor.reallocate(current_raw, 0x40, descriptor.context) == current_raw,
	      "explicit orphan cleanup leaked or damaged current global heap");

	void* current_mapping = context->mapping;
	const auto before_zero_term = Global::TestGeneratorState();
	api.term(); // old=0: wrap to UINT32_MAX, but still perform cleanup.
	Check(Name, Global::TestArchState() == UINT32_MAX && context->mapping == nullptr && context->handle == nullptr &&
	            maps == initial_maps + 3 && unmaps == initial_unmaps + 3 && Available() == baseline &&
	            Memory::KernelVirtualQuery(current_mapping, 0, &info, sizeof(info)) != 0,
	      "ArchTerm old-zero wrap/cleanup");
	Check(Name, Global::TestGeneratorState() == before_zero_term && before_zero_term == zero_seed,
	      "ArchTerm old-zero cleared or changed persistent generator state");
	descriptors_survive();
	Inactive(descriptor);
	Inactive(ex_descriptor);
	const auto inactive_heap = *context;
	// Change the clock policy so an accidental real initialization on this fast
	// path would not merely recreate the same forced-zero generator state.
	Global::TestSetClockFailure(false);
	CheckOk(Name, api.init(), "ArchInit UINT32_MAX fast path");
	Check(Name, Global::TestArchState() == 0 && maps == initial_maps + 3 && unmaps == initial_unmaps + 3 &&
	            Available() == baseline && std::memcmp(context, &inactive_heap, sizeof(inactive_heap)) == 0,
	      "nonzero ArchInit must wrap state without touching heap");
	Check(Name, Global::TestGeneratorState() == before_zero_term, "nonzero ArchInit fast path reseeded generator state");
	descriptors_survive();
	Inactive(descriptor);

	CheckOk(Name, api.init(), "ArchInit real path after wrap to zero");
	Check(Name, Global::TestArchState() == 0 && context->size == Global::HeapSize &&
	            maps == initial_maps + 4 && unmaps == initial_unmaps + 3 &&
	            Available() == baseline - Global::HeapSize, "ArchInit failed to resume real initialization after wrap");
	mapped_global(context->mapping);
	void* retained = descriptor.allocate(0x40, descriptor.context);
	Check(Name, retained == context->mapping, "Arch reinitialized allocation");
	const auto retained_heap = *context;
	const auto retained_generator = Global::TestGeneratorState();
	Global::TestSetArchState(2);
	api.term(); // old=2: decrement only, retaining mapping and live allocations.
	Check(Name, Global::TestArchState() == 1 && maps == initial_maps + 4 && unmaps == initial_unmaps + 3 &&
	            Available() == baseline - Global::HeapSize && std::memcmp(context, &retained_heap, sizeof(retained_heap)) == 0 &&
	            descriptor.reallocate(retained, 0x40, descriptor.context) == retained,
	      "ArchTerm old-two must skip cleanup");
	Check(Name, Global::TestGeneratorState() == retained_generator, "ArchTerm old-two changed generator state");
	mapped_global(context->mapping);
	descriptors_survive();
	api.term(); // old=1: decrement to zero and clean up.
	Check(Name, Global::TestArchState() == 0 && context->mapping == nullptr && context->handle == nullptr &&
	            maps == initial_maps + 4 && unmaps == initial_unmaps + 4 && Available() == baseline &&
	            Memory::KernelVirtualQuery(retained_heap.mapping, 0, &info, sizeof(info)) != 0,
	      "ArchTerm old-one must destroy current heap");
	Check(Name, Global::TestGeneratorState() == retained_generator, "ArchTerm old-one cleared or changed generator state");
	descriptors_survive();
	Inactive(descriptor);
	Inactive(ex_descriptor);

	CheckOk(Name, api.init(), "ArchInit before signed-negative Term");
	void* negative_mapping = context->mapping;
	Global::TestSetArchState(UINT32_MAX);
	api.term(); // signed old=-1 must clean up, unlike an unsigned >1 test.
	Check(Name, Global::TestArchState() == UINT32_MAX - 1 && context->mapping == nullptr && context->handle == nullptr &&
	            maps == initial_maps + 5 && unmaps == initial_unmaps + 5 && Available() == baseline &&
	            Memory::KernelVirtualQuery(negative_mapping, 0, &info, sizeof(info)) != 0,
	      "ArchTerm negative original state must decrement and destroy the heap");
	Global::TestSetArchState(0);
	descriptors_survive();
	Inactive(descriptor);

	// Seed 2 requires SFMT period certification to flip word0 from 2 to 3.
	// Known words distinguish the correction from ordinary MT seed expansion.
	Global::TestSeedGenerator(2);
	const auto certified = Global::TestGeneratorState();
	Check(Name, certified[0] == 624 && certified[4] == 3 && certified[7] == 0xa2f2c655 && certified[627] == 0x2da04eb3,
	      "SFMT seed-two period certification correction");
	Check(Name, reentered && Common::GetHostLockCriticalCounter()->load() == initial_depth,
	      "Arch map/unmap getter re-entry or critical-scope leak");
}
