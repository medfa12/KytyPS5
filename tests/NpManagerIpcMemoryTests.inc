// Parent includes libs/npManagerIpcMemory.h at global scope; include this after
// NpManagerMemoryTests.inc. Run TestNpManagerIpcMemory after InitSubsystems in a
// dedicated mode. Requires the seven OfflineLogin binding replacements in the
// native NP lifecycle report; these checks must fail against the old stubs.
namespace NpIpcMemoryTest {
namespace Ipc = Libs::NpManagerIpcMemory;
namespace Manager = Libs::NpManagerMemory;
namespace Heap = Libs::NpMemoryHeap;
namespace Memory = Libs::LibKernel::Memory;
constexpr const char* Name = "NpManagerIpcMemory";

struct Api {
	Loader::SymbolDatabase symbols;
	decltype(&Ipc::CreateKernel) create;
	decltype(&Ipc::CreateBuffer) supplied;
	decltype(&Ipc::Destroy) destroy;
	decltype(&Ipc::GetAllocator) get;
	decltype(&Ipc::Allocate) allocate;
	decltype(&Ipc::Reallocate) reallocate;
	decltype(&Ipc::Free) free;

	template<class F> F Bind(const char* nid, F expected) {
		const auto* exact = symbols.Find({nid, "NpManager", 1, "NpManager", 1, 1, Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(Name, exact && fallback && exact->vaddr == reinterpret_cast<uint64_t>(expected) &&
		            fallback->vaddr == exact->vaddr, std::string("IPC binding mismatch: ") + nid);
		return reinterpret_cast<F>(exact->vaddr);
	}
	Api() {
		Libs::OfflineLogin::InitOfflineLogin_1(&symbols);
		Libs::LibNpManager::InitNpManager_1(&symbols);
		create = Bind("pHLjntY0psg", &Ipc::CreateKernel);
		supplied = Bind("UdhQmx64-uM", &Ipc::CreateBuffer);
		destroy = Bind("hyuye+88uPo", &Ipc::Destroy);
		get = Bind("V38nfJwXYhg", &Ipc::GetAllocator);
		allocate = Bind("VBZtcFn7+aY", &Ipc::Allocate);
		reallocate = Bind("TyACAxDH3Uw", &Ipc::Reallocate);
		free = Bind("VY8Xji9cAFA", &Ipc::Free);
	}
	~Api() { (void)destroy(); }
};

uintptr_t last_mapping = 0;
unsigned maps = 0, unmaps = 0;
bool unpublished = true;
void Mapped(uintptr_t address, size_t) {
	last_mapping = address;
	++maps;
	unpublished &= Ipc::GetAllocator() == nullptr;
}
void Unmapped(uintptr_t, size_t) {
	++unmaps;
	unpublished &= Ipc::GetAllocator() == nullptr;
}
} // namespace NpIpcMemoryTest

void TestNpManagerIpcMemory() {
	using namespace NpIpcMemoryTest;
	Api api;
	Check(Name, !api.get() && api.destroy() == Ipc::NotInitialized, "inactive lifecycle");
	Check(Name, !api.allocate(16, nullptr) && !api.reallocate(nullptr, 16, nullptr), "inactive allocations");
	api.free(reinterpret_cast<void*>(1), nullptr);
	Check(Name, api.create(0x18000, nullptr) == Ipc::InvalidArgument, "null name");
	Check(Name, api.supplied(0x1000, nullptr, "ipc") == Ipc::InvalidArgument, "null backing");
	Check(Name, api.create(0, "ipc") < 0 && !api.get(), "zero-sized mapping substituted");
	Check(Name, api.create(SIZE_MAX, "ipc") < 0 && !api.get(), "overflow-sized mapping substituted");

	// The existing manager heap remains live while the separate IPC heap fails,
	// initializes, exhausts and is destroyed.
	alignas(16) std::array<uint8_t, 0x2000> manager_backing {};
	CheckOk(Name, Manager::CreateBuffer(manager_backing.size(), manager_backing.data(), "np-manager"), "manager setup");
	struct Cleanup {
		// Memory callbacks are process-lifetime, single-registration hooks. This
		// dedicated test process installs static functions, not borrowed state.
		~Cleanup() { (void)Ipc::Destroy(); (void)Manager::Destroy(); }
	} cleanup;
	auto* manager = Manager::GetAllocator();
	void* manager_block = manager->allocate(32, nullptr);
	Check(Name, manager_block != nullptr, "manager allocation");
	std::memset(manager_block, 0x69, 32);
	maps = unmaps = 0; unpublished = true;
	Memory::RegisterCallbacks(Mapped, Unmapped);
	Heap::TestFailNextHeapCreation();
	Check(Name, api.create(0x18000, "np-ipc-failure") == Manager::HeapFailure, "heap failure propagation");
	Memory::VirtualQueryInfo query {};
	Check(Name, maps == 1 && unmaps == 1 && unpublished && !api.get() &&
	            Memory::KernelVirtualQuery(reinterpret_cast<void*>(last_mapping), 0, &query, sizeof(query)) != 0,
	      "failed IPC creation leaked/published or held its lock over mapping callbacks");

	void* fourth = reinterpret_cast<void*>(uintptr_t {0x1122334455667788ull});
#if defined(__x86_64__) || defined(_M_X64)
	Xbyak::CodeGenerator call;
	call.sub(call.rsp, 8);
	call.mov(call.rdi, 0x18000);
	call.mov(call.rsi, reinterpret_cast<uint64_t>("SceNpMgrIpcServer"));
	call.mov(call.rdx, 0x1234);
	call.mov(call.rcx, reinterpret_cast<uint64_t>(&fourth));
	call.mov(call.rax, reinterpret_cast<uint64_t>(api.create));
	call.call(call.rax);
	call.add(call.rsp, 8);
	call.ret();
	call.ready();
	using Entry = int32_t (KYTY_SYSV_ABI *)();
	CheckOk(Name, call.getCode<Entry>()(), "IPC create status ABI");
#else
	CheckOk(Name, api.create(0x18000, "SceNpMgrIpcServer"), "IPC create");
#endif
	Check(Name, fourth == reinterpret_cast<void*>(uintptr_t {0x1122334455667788ull}), "unexpected fourth-argument write");
	auto* descriptor = api.get();
	Check(Name, descriptor && descriptor != manager && descriptor == api.get(), "separate persistent IPC descriptor");
	const auto copy = *descriptor;
	Check(Name, copy.allocate == api.allocate && copy.reallocate == api.reallocate && copy.free == api.free &&
	            !copy.context, "IPC callback ABI");
	const auto state = Ipc::TestSnapshot();
	Check(Name, state.mode == 0 && state.size == 0x18000 && state.critical_depth > 0, "IPC backing contract");
	Check(Name, api.create(1, nullptr) == Ipc::InvalidArgument &&
	            api.create(1, "duplicate") == Ipc::AlreadyInitialized, "validation/duplicate precedence");
	void* block = copy.allocate(0x4000, reinterpret_cast<void*>(1));
	Check(Name, block && uintptr_t(block) >= uintptr_t(state.base) &&
	            uintptr_t(block) + 0x4000 <= uintptr_t(state.base) + state.size, "transfer block outside IPC heap");
	std::memset(block, 0xa5, 0x4000);
	Check(Name, !api.allocate(0x18001, nullptr) && !api.reallocate(block, 0x18001, nullptr) &&
	            static_cast<uint8_t*>(block)[0x3fff] == 0xa5, "bounded exhaustion/realloc preservation");
	CheckOk(Name, api.destroy(), "destroy outstanding IPC allocations");
	Check(Name, !api.get() && !descriptor->allocate && !copy.allocate(16, nullptr) &&
	            !copy.reallocate(block, 16, nullptr), "retired IPC callbacks remain active");
	copy.free(block, nullptr);
	Check(Name, unmaps == 2 && unpublished &&
	            Memory::KernelVirtualQuery(state.base, 0, &query, sizeof(query)) != 0, "IPC owned mapping retained");
	Check(Name, Manager::GetAllocator() == manager && static_cast<uint8_t*>(manager_block)[31] == 0x69,
	      "IPC lifecycle damaged manager-global memory");

	alignas(16) std::array<uint8_t, 0x2020> supplied;
	supplied.fill(0xcc);
	Check(Name, api.supplied(1, supplied.data() + 16, "tiny") == Manager::HeapFailure, "invalid supplied heap accepted");
	CheckOk(Name, api.supplied(0x2000, supplied.data() + 16, "ipc-pool"), "caller-backed IPC heap");
	Check(Name, api.get() == descriptor && Ipc::TestSnapshot().mode == 1, "reinit descriptor identity/mode");
	block = api.allocate(0x2000, nullptr);
	Check(Name, block == supplied.data() + 16 && !api.allocate(1, nullptr), "caller-backed bounds/exhaustion");
	api.free(block, nullptr);
	Check(Name, api.allocate(0x2000, nullptr) == block, "free did not reclaim bounded storage");
	CheckOk(Name, api.destroy(), "caller-backed destroy");
	Check(Name, unmaps == 2 && supplied.front() == 0xcc && supplied.back() == 0xcc,
	      "caller backing unmapped or guard bytes overwritten");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CheckOk(Name, api.create(0x18000, "ipc-retirement-retry"), "retirement setup");
	const auto retained = Ipc::TestSnapshot();
	Memory::TestFailGuestBackingStoreUnmapAfter(0);
	Check(Name, api.destroy() < 0 && !api.get() && !copy.allocate(16, nullptr),
	      "failed unmap reported retirement or left allocations active");
	Check(Name, api.create(0x18000, "replacement") == Ipc::AlreadyInitialized,
	      "replacement admitted while retired IPC mapping still owned");
	CheckOk(Name, api.destroy(), "retry failed IPC unmap");
	Check(Name, Memory::KernelVirtualQuery(retained.base, 0, &query, sizeof(query)) != 0,
	      "retirement retry lost backing mapping");
	CheckOk(Name, api.create(0x18000, "ipc-after-retirement"), "restart after retirement");
	CheckOk(Name, api.destroy(), "final retirement");
	Heap::TestFailNextHeapCreation();
	Memory::TestFailGuestBackingStoreUnmapAfter(0);
	Check(Name, api.create(0x18000, "ipc-rollback-retry") == Manager::HeapFailure && !api.get(),
	      "failed heap creation published allocator or lost its error");
	const auto rollback_mapping = last_mapping;
	Check(Name, api.create(0x18000, "replacement") == Ipc::AlreadyInitialized,
	      "failed rollback mapping did not block replacement");
	CheckOk(Name, api.destroy(), "retry failed creation rollback unmap");
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(rollback_mapping), 0, &query, sizeof(query)) != 0,
	      "failed creation rollback lost mapping ownership");
#endif
	manager->free(manager_block, nullptr);
}
