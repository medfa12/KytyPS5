// Included by the full-HLE test target: invoke real exports, not a model allocator.
namespace NpMemoryTest {
namespace Np = Libs::NpManagerMemory;
constexpr const char* Name = "NpManagerMemory";
using CreateKernel = int32_t (KYTY_SYSV_ABI *)(size_t, const char*);
using CreateBuffer = int32_t (KYTY_SYSV_ABI *)(size_t, void*, const char*);
using GetAllocator = Np::Allocator* (KYTY_SYSV_ABI *)();
using Destroy = int32_t (KYTY_SYSV_ABI *)();

template<class F> F Resolve(Loader::SymbolDatabase& symbols, const char* nid, const char* library = "NpManager") {
	const auto* symbol = symbols.Find({nid, library, 1, library, 1, 1, Loader::SymbolType::Func});
	Check(Name, symbol != nullptr, std::string("missing versioned export: ") + nid);
	return reinterpret_cast<F>(symbol->vaddr);
}

struct Api {
	Loader::SymbolDatabase symbols;
	CreateKernel create;
	CreateBuffer supplied;
	GetAllocator get;
	Destroy destroy, destroy_common;
	Np::AllocateCallback alloc;
	Np::ReallocateCallback realloc;
	Np::FreeCallback free;
	Api() {
		Libs::OfflineLogin::InitOfflineLogin_1(&symbols);
		Libs::LibNpCommon::InitNpCommon_1(&symbols);
		Libs::LibNpManager::InitNpManager_1(&symbols);
		create = Resolve<CreateKernel>(symbols, "fHGhS3uP52k");
		supplied = Resolve<CreateBuffer>(symbols, "v8+25H9WIX4");
		get = Resolve<GetAllocator>(symbols, "ukEeOizCkIU");
		destroy = Resolve<Destroy>(symbols, "4uhgVNAqiag");
		destroy_common = Resolve<Destroy>(symbols, "4uhgVNAqiag", "NpCommon");
		alloc = Resolve<Np::AllocateCallback>(symbols, "p0TfCdPEcsk");
		realloc = Resolve<Np::ReallocateCallback>(symbols, "PIYEFT1iG0Y");
		free = Resolve<Np::FreeCallback>(symbols, "8JX-S2ADen4");
		Check(Name, destroy == destroy_common, "Destroy aliases differ");
		Check(Name, symbols.FindByNid("p0TfCdPEcsk", Loader::SymbolType::Func)->vaddr == reinterpret_cast<uint64_t>(alloc) &&
		            symbols.FindByNid("8JX-S2ADen4", Loader::SymbolType::Func)->vaddr == reinterpret_cast<uint64_t>(free),
		      "earlier OfflineLogin fallback still bypasses manager ownership");
	}
	~Api() { (void)destroy(); }
};

uintptr_t last_mapping = 0;
size_t last_mapping_size = 0;
unsigned maps = 0, unmaps = 0;
bool notification_uninitialized = true;
void Mapped(uintptr_t address, size_t size) {
	last_mapping = address;
	last_mapping_size = size;
	++maps;
	// Re-entry proves the NP state lock is not held across the mapping backend.
	notification_uninitialized &= Np::GetAllocator() == nullptr;
}
void Unmapped(uintptr_t, size_t) {
	++unmaps;
	notification_uninitialized &= Np::GetAllocator() == nullptr;
}

struct DeleteRecord { void* raw = nullptr; void* context = nullptr; void* object = nullptr; unsigned calls = 0; bool uninitialized = false; uint32_t depth = 99; };
DeleteRecord delete_record;
void KYTY_SYSV_ABI RecordingFree(void* raw, void* context) {
	delete_record.raw = raw;
	delete_record.context = context;
	++delete_record.calls;
	delete_record.uninitialized = Libs::LibNpCommon::NpMutexIsInit(delete_record.object) == 0;
	delete_record.depth = Common::GetHostLockCriticalCounter()->load();
}
} // namespace NpMemoryTest

void TestNpManagerMemory() {
	using namespace NpMemoryTest;
	namespace Memory = Libs::LibKernel::Memory;
	Api api;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	_putenv_s("KYTY_IPMI_OBSERVE", "1");
	Libs::IpmiObserve::Initialize();
	Check(Name, Libs::IpmiObserve::Enabled(), "production observer not enabled");
#endif
	Check(Name, api.get() == nullptr && api.destroy() == Np::NotInitialized, "preinit lifecycle");
	Check(Name, api.alloc(16, nullptr) == nullptr && api.realloc(nullptr, 16, nullptr) == nullptr, "preinit callbacks");
	api.free(reinterpret_cast<void*>(1), nullptr);
	Check(Name, Np::TestSnapshot().diagnostic_count == 0, "preinit free changed counter");
	Check(Name, api.create(0x4000, nullptr) == Np::InvalidArgument, "null name precedence");
	Check(Name, api.supplied(0x100, nullptr, "np-test") == Np::InvalidArgument, "null buffer");
	Check(Name, api.create(0, "np-test") == Libs::LibKernel::KERNEL_ERROR_EINVAL, "zero size substituted");
	Check(Name, api.create(SIZE_MAX, "np-test") == Libs::LibKernel::KERNEL_ERROR_EINVAL, "mapping round overflow");
	const char long_name[] = "0123456789012345678901234567890123456789";
	Check(Name, api.create(0x4000, long_name) == Libs::LibKernel::KERNEL_ERROR_ENAMETOOLONG,
	      "mapping error must propagate unchanged");

	Memory::RegisterCallbacks(Mapped, Unmapped);
	Np::TestFailNextHeapCreation();
	Check(Name, api.create(0x4000, "np-rollback") == Np::HeapFailure, "heap failure result");
	Check(Name, maps == 1 && unmaps == 1 && api.get() == nullptr && notification_uninitialized,
	      "heap creation failure did not release mapping / mapping re-entry");
	Memory::VirtualQueryInfo query {};
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(last_mapping), 0, &query, sizeof(query)) != 0,
	      "failed heap left a registered mapping");

	void* fourth = reinterpret_cast<void*>(uintptr_t {0x1122334455667788ull});
#if defined(__x86_64__) || defined(_M_X64)
	// Actual guest-style call with an unrelated fourth GP argument.
	Xbyak::CodeGenerator call;
	call.sub(call.rsp, 8);
	call.mov(call.rdi, 0x4001);
	call.mov(call.rsi, reinterpret_cast<uint64_t>("np-kernel"));
	call.mov(call.rdx, 0x1234);
	call.mov(call.rcx, reinterpret_cast<uint64_t>(&fourth));
	call.mov(call.rax, reinterpret_cast<uint64_t>(api.create));
	call.call(call.rax);
	call.add(call.rsp, 8);
	call.ret();
	call.ready();
	using Entry = int32_t (KYTY_SYSV_ABI *)();
	CheckOk(Name, call.getCode<Entry>()(), "CreateKernel guest call");
#else
	CheckOk(Name, api.create(0x4001, "np-kernel"), "CreateKernel");
#endif
	Check(Name, fourth == reinterpret_cast<void*>(uintptr_t {0x1122334455667788ull}), "Create wrote uncontracted fourth argument");
	auto* persistent = api.get();
	Check(Name, persistent != nullptr && persistent == api.get(), "descriptor must be persistent");
	Np::Allocator copy = *persistent;
	Check(Name, copy.allocate == api.alloc && copy.reallocate == api.realloc && copy.free == api.free && copy.context == nullptr,
	      "descriptor callback ABI or null context");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Np::Allocator readable {};
	Check(Name, Libs::IpmiObserve::Read(persistent, &readable, sizeof(readable)) &&
	            std::memcmp(&copy, &readable, sizeof(copy)) == 0, "descriptor is not guest-readable full-width storage");
#endif
	const auto kernel = Np::TestSnapshot();
	Check(Name, kernel.mode == 0 && kernel.size == 0x8000 && last_mapping_size == 0x8000 && kernel.critical_depth > 0,
	      "kernel backing size/mode or unprotected state lock");
	CheckOk(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(kernel.base), 0, &query, sizeof(query)), "query manager mapping");
	Check(Name, query.protection == 3 && query.is_flexible && query.end - query.start == 0x8000, "mapping backend contract");
	Check(Name, api.create(16, nullptr) == Np::InvalidArgument && api.create(16, "duplicate") == Np::AlreadyInitialized,
	      "name validation must precede duplicate detection");
	Check(Name, api.supplied(16, nullptr, "duplicate") == Np::InvalidArgument &&
	            api.supplied(16, &fourth, nullptr) == Np::InvalidArgument &&
	            api.supplied(16, &fourth, "duplicate") == Np::AlreadyInitialized, "buffer validation precedence");
	void* outstanding = copy.allocate(0x568, reinterpret_cast<void*>(1));
	Check(Name, outstanding != nullptr && reinterpret_cast<uintptr_t>(outstanding) >= kernel.base &&
	            reinterpret_cast<uintptr_t>(outstanding) + 0x568 <= kernel.base + kernel.size, "allocation escaped kernel arena");
	std::memset(outstanding, 0xa5, 0x568);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto* observations = Libs::IpmiObserve::GetBuffer();
	bool saw_create = false, saw_get = false, saw_allocation = false;
	for (size_t i = 0; i < std::min<uint64_t>(observations->attempted[4].load(), Libs::IpmiObserve::CAPACITIES[4]); ++i) {
		const auto& record = observations->slots[Libs::IpmiObserve::OFFSETS[4] + i].record;
		saw_create |= record.event == 19 && record.values[1] == 0x4001 && record.values[5] == 0 && record.values[15] == 1;
		saw_get |= record.event == 20 && record.values[1] == reinterpret_cast<uint64_t>(persistent);
		saw_allocation |= record.event == 22 && record.values[1] == 0 && record.values[2] == 0x568 &&
		                  record.values[5] == reinterpret_cast<uint64_t>(outstanding) && record.values[6] == 1;
	}
	Check(Name, saw_create && saw_get && saw_allocation, "actual manager exports did not emit Create/Get/0x568 results");
	std::printf("NP allocator observer: %ls\n", Libs::IpmiObserve::OutputPath());
#endif
	CheckOk(Name, api.destroy_common(), "Destroy with outstanding allocation");
	Check(Name, unmaps == 2 && notification_uninitialized && api.get() == nullptr &&
	            persistent->allocate == nullptr && persistent->context == nullptr, "kernel Destroy did not clear/release");
	Check(Name, Memory::KernelVirtualQuery(reinterpret_cast<void*>(kernel.base), 0, &query, sizeof(query)) != 0,
	      "kernel Destroy retained owned mapping");
	Check(Name, copy.allocate(16, nullptr) == nullptr && copy.reallocate(outstanding, 32, nullptr) == nullptr,
	      "copied callbacks survived uninitialized state");
	copy.free(outstanding, nullptr);

	alignas(16) std::array<uint8_t, 0x420> supplied;
	supplied.fill(0xcc);
	void* buffer = supplied.data() + 16;
	Check(Name, api.supplied(1, buffer, "too-small") == Np::HeapFailure, "tiny arena silently substituted");
	Check(Name, api.supplied(SIZE_MAX, buffer, "overflow") == Np::HeapFailure, "overflowing supplied range");
	{
		namespace Vm = Common::VirtualMemory;
		// This fixture validates arbitrary supplied host buffers, not guest VA
		// placement. Avoid competing with emulator reservations below 1 TiB.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto range = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 0x3000, MEM_RESERVE, PAGE_NOACCESS));
#else
		const auto range = Vm::Reserve(0, 0x3000);
#endif
		Check(Name, range != 0, "reserve supplied-buffer validation fixture");
		auto rejects = [&](uintptr_t address, size_t size) {
			Check(Name, api.supplied(size, reinterpret_cast<void*>(address), "invalid-arena") == Np::HeapFailure &&
			            api.get() == nullptr, "invalid supplied coverage published an allocator");
		};
		rejects(range, 0x1000); // Reserved, uncommitted storage.
		Check(Name, Vm::Commit(range, 0x3000, Vm::Mode::ReadWrite), "commit supplied fixture");
		Check(Name, Vm::Protect(range + 0x1000, 0x1000, Vm::Mode::Read), "readonly middle page");
		rejects(range + 0x1000, 0x1000);
		rejects(range + 0xff0, 0x1020); // RW endpoints are insufficient: the middle is read-only.
		Check(Name, Vm::Protect(range + 0x1000, 0x1000, Vm::Mode::NoAccess), "inaccessible middle page");
		rejects(range + 0xff0, 0x1020);
		Check(Name, Vm::Decommit(range + 0x1000, 0x1000), "decommit middle page");
		rejects(range, 0x3000);
		Check(Name, Vm::Free(range), "free supplied fixture");
		rejects(range, 0x3000); // Completely unmapped storage.
	}
	CheckOk(Name, api.supplied(0x400, buffer, "np-supplied"), "CreateBuffer");
	Check(Name, api.get() == persistent && Np::TestSnapshot().mode == 1, "descriptor identity across reinit");
	Check(Name, api.realloc(outstanding, 16, nullptr) == nullptr, "stale prior-heap block accepted");
	void* a = copy.allocate(0x100, nullptr);
	void* b = api.alloc(0x100, nullptr);
	void* c = copy.allocate(0x200, nullptr);
	Check(Name, a == buffer && b == supplied.data() + 0x110 && c == supplied.data() + 0x210,
	      "supplied arena addresses/bounds");
	std::memset(a, 0x5a, 0x100);
	Check(Name, api.alloc(1, nullptr) == nullptr && api.alloc(SIZE_MAX, nullptr) == nullptr, "real arena exhaustion/overflow");
	Check(Name, api.realloc(a, 0x180, nullptr) == nullptr, "exhausted realloc must fail");
	Check(Name, static_cast<uint8_t*>(a)[0] == 0x5a && static_cast<uint8_t*>(a)[0xff] == 0x5a &&
	            Np::TestSnapshot().live_blocks == 3 && Np::TestSnapshot().diagnostic_count == 3,
	      "failed realloc damaged original allocation or counters");
	api.free(static_cast<uint8_t*>(a) + 1, nullptr);
	api.free(reinterpret_cast<void*>(1), nullptr);
	api.free(nullptr, nullptr);
	Check(Name, Np::TestSnapshot().live_blocks == 3 && Np::TestSnapshot().diagnostic_count == 0,
	      "diagnostic free count was confused with ownership");
	Check(Name, api.realloc(static_cast<uint8_t*>(a) + 1, 16, nullptr) == nullptr, "interior realloc accepted");
	copy.free(b, nullptr);
	Check(Name, Np::TestSnapshot().diagnostic_count == UINT32_MAX && Np::TestSnapshot().live_blocks == 2,
	      "initialized free must wrap the native U32 diagnostic counter independently of ownership");
	void* reused = copy.allocate(0x100, nullptr);
	Check(Name, reused == b && Np::TestSnapshot().diagnostic_count == 0, "freed span reuse / U32 increment wrap");
	copy.free(reused, nullptr);
	Check(Name, copy.reallocate(a, 0x180, nullptr) == a && static_cast<uint8_t*>(a)[0xff] == 0x5a,
	      "adjacent growth/preservation");
	copy.free(c, nullptr);
	Check(Name, copy.reallocate(a, 0x80, nullptr) == a, "shrink");
	void* tail = copy.allocate(0x380, nullptr);
	Check(Name, tail == supplied.data() + 0x90, "shrink/coalescing did not recover capacity");
	Check(Name, supplied.front() == 0xcc && supplied[15] == 0xcc && supplied[0x410] == 0xcc && supplied.back() == 0xcc,
	      "supplied arena canary overwritten");
	CheckOk(Name, api.destroy(), "supplied Destroy with live blocks");
	Check(Name, unmaps == 2 && supplied[16] == 0x5a && Np::TestSnapshot().live_blocks == 0,
	      "caller-owned buffer unmapped or heap tracking retained");
	CheckOk(Name, api.supplied(0x400, buffer, "np-reinit"), "same buffer reinit");
	Check(Name, api.realloc(a, 16, nullptr) == nullptr && Np::TestSnapshot().live_blocks == 0,
	      "old allocation tracking resurrected in new heap");
	a = api.alloc(0x80, nullptr);
	b = api.alloc(0x80, nullptr);
	std::memset(a, 0x6b, 0x80);
	void* moved = api.realloc(a, 0x180, nullptr);
	Check(Name, moved == supplied.data() + 0x110 && static_cast<uint8_t*>(moved)[0] == 0x6b &&
	            static_cast<uint8_t*>(moved)[0x7f] == 0x6b && Np::TestSnapshot().live_blocks == 2 &&
	            Np::TestSnapshot().diagnostic_count == 2, "moving realloc preservation/count");
	Check(Name, api.alloc(0x80, nullptr) == a, "moving realloc did not release original span");
	const auto count_before = Np::TestSnapshot().diagnostic_count;
	Check(Name, api.realloc(nullptr, 16, nullptr) != nullptr &&
	            Np::TestSnapshot().diagnostic_count == count_before, "realloc(null) changed diagnostic count");
	const auto live_before = Np::TestSnapshot().live_blocks;
	Check(Name, api.realloc(b, 0, nullptr) == b &&
	            Np::TestSnapshot().diagnostic_count == count_before && Np::TestSnapshot().live_blocks == live_before,
	      "realloc zero must retain original minimal allocation and diagnostic count");
	void* after_zero = api.alloc(0x70, nullptr);
	Check(Name, after_zero == static_cast<uint8_t*>(b) + 16, "realloc zero lost ownership or did not recover tail");
	api.free(b, nullptr);
	Check(Name, api.alloc(16, nullptr) == b, "zero-size realloc block was not still owned and releasable");
	CheckOk(Name, api.destroy(), "final Destroy");
	Check(Name, Common::GetHostLockCriticalCounter()->load() == 0, "allocator leaked suspension protection");
}

void TestNpMutexNativeDestructors() {
	using namespace NpMemoryTest;
	Api api;
	using Ctor = void (KYTY_SYSV_ABI *)(void*);
	using Init = int (KYTY_SYSV_ABI *)(void*, const uint32_t*);
	const auto ctor = Resolve<Ctor>(api.symbols, "O1AvlQU33pI", "NpCommon");
	const auto init = Resolve<Init>(api.symbols, "aTNOl9EB4V4", "NpCommon");
	const char* old_env = std::getenv("KYTY_NATIVE_NP_OWNERSHIP");
	const std::string saved = old_env == nullptr ? "" : old_env;
	const char* old_queue_env = std::getenv("KYTY_NATIVE_NP_WORKQUEUE");
	const std::string saved_queue = old_queue_env == nullptr ? "" : old_queue_env;
	const char* old_callout_env = std::getenv("KYTY_NATIVE_NP_CALLOUT");
	const std::string saved_callout = old_callout_env == nullptr ? "" : old_callout_env;
	const char* old_http_pool_env = std::getenv("KYTY_NATIVE_NP_HTTP_POOL");
	const std::string saved_http_pool = old_http_pool_env == nullptr ? "" : old_http_pool_env;
	const char* old_alternate_env = std::getenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP");
	const std::string saved_alternate = old_alternate_env == nullptr ? "" : old_alternate_env;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	_putenv_s("KYTY_NATIVE_NP_ALTERNATE_STARTUP", "");
	_putenv_s("KYTY_NATIVE_NP_OWNERSHIP", "1");
	_putenv_s("KYTY_NATIVE_NP_WORKQUEUE", "");
	_putenv_s("KYTY_NATIVE_NP_CALLOUT", "");
	_putenv_s("KYTY_NATIVE_NP_HTTP_POOL", "");
#else
	setenv("KYTY_NATIVE_NP_OWNERSHIP", "1", 1);
	unsetenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP");
	unsetenv("KYTY_NATIVE_NP_WORKQUEUE");
	unsetenv("KYTY_NATIVE_NP_CALLOUT");
	unsetenv("KYTY_NATIVE_NP_HTTP_POOL");
#endif
	alignas(16) std::array<uint8_t, 0x30> storage {};
	Np::Allocator allocator {nullptr, nullptr, RecordingFree, &storage};
	auto* descriptor = &allocator;
	std::memcpy(storage.data(), &descriptor, sizeof(descriptor));
	void* object = storage.data() + 0x10;
	ctor(object);
	CheckOk(Name, init(object, nullptr), "native-owned mutex Init");
	const auto* table = *reinterpret_cast<const uintptr_t**>(object);
	delete_record = {};
	delete_record.object = object;
	reinterpret_cast<Ctor>(table[1])(object);
	Check(Name, delete_record.calls == 1 && delete_record.raw == storage.data() &&
	            delete_record.context == &storage && delete_record.uninitialized && delete_record.depth == 0,
	      "deleting dtor must clear state then call descriptor free(raw,ctx) unlocked");
	alignas(16) std::array<uint8_t, 0x18> embedded {};
	ctor(embedded.data());
	CheckOk(Name, init(embedded.data(), nullptr), "embedded mutex Init");
	table = *reinterpret_cast<const uintptr_t**>(embedded.data());
	reinterpret_cast<Ctor>(table[0])(embedded.data());
	Check(Name, delete_record.calls == 1 && Libs::LibNpCommon::NpMutexIsInit(embedded.data()) == 0,
	      "nondeleting dtor freed embedded mutex");
	const auto* native_table = table;
	const uintptr_t* legacy_table = nullptr;
	for (const char* value: {"", "0", "true", "01", "1 "}) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		_putenv_s("KYTY_NATIVE_NP_OWNERSHIP", value);
#else
		setenv("KYTY_NATIVE_NP_OWNERSHIP", value, 1);
#endif
		ctor(embedded.data());
		table = *reinterpret_cast<const uintptr_t**>(embedded.data());
		if (legacy_table == nullptr) { legacy_table = table; }
		Check(Name, table == legacy_table && table != native_table && table[0] == table[1] &&
		            table[0] != native_table[0] && table[1] != native_table[1], "ownership opt-in must be exact 1");
		for (size_t slot = 2; slot < 8; ++slot) {
			Check(Name, table[slot] == native_table[slot], "opt-in changed ordinary mutex slots");
		}
	}
	for (const char* value: {"", "0", "true", "01", "1 ", "1"}) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		_putenv_s("KYTY_NATIVE_NP_OWNERSHIP", "");
		_putenv_s("KYTY_NATIVE_NP_WORKQUEUE", value);
#else
		unsetenv("KYTY_NATIVE_NP_OWNERSHIP");
		setenv("KYTY_NATIVE_NP_WORKQUEUE", value, 1);
#endif
		ctor(embedded.data());
		table = *reinterpret_cast<const uintptr_t**>(embedded.data());
		Check(Name, table == (std::strcmp(value, "1") == 0 ? native_table : legacy_table),
		      "queue opt-in must imply native ownership only for exact 1");
	}
	ctor(object);
	CheckOk(Name, init(object, nullptr), "queue-owned mutex Init");
	table = *reinterpret_cast<const uintptr_t**>(object);
	reinterpret_cast<Ctor>(table[1])(object);
	Check(Name, delete_record.calls == 2 && delete_record.raw == storage.data() &&
	            delete_record.uninitialized && delete_record.depth == 0,
	      "queue-only mode must preserve raw-header deletion and unlocked callback");
	for (const char* value: {"", "0", "true", "01", "1 ", "1"}) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		_putenv_s("KYTY_NATIVE_NP_WORKQUEUE", "");
		_putenv_s("KYTY_NATIVE_NP_CALLOUT", value);
#else
		unsetenv("KYTY_NATIVE_NP_WORKQUEUE");
		setenv("KYTY_NATIVE_NP_CALLOUT", value, 1);
#endif
		ctor(embedded.data());
		table = *reinterpret_cast<const uintptr_t**>(embedded.data());
		Check(Name, table == (std::strcmp(value, "1") == 0 ? native_table : legacy_table),
		      "callout opt-in must imply native ownership only for exact 1");
	}
	ctor(object);
	CheckOk(Name, init(object, nullptr), "callout-owned mutex Init");
	table = *reinterpret_cast<const uintptr_t**>(object);
	reinterpret_cast<Ctor>(table[1])(object);
	Check(Name, delete_record.calls == 3 && delete_record.raw == storage.data() &&
	            delete_record.uninitialized && delete_record.depth == 0,
	      "callout-only mode must preserve raw-header deletion and unlocked callback");
	for (const char* value: {"", "0", "true", "01", "1 ", "1"}) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		_putenv_s("KYTY_NATIVE_NP_CALLOUT", "");
		_putenv_s("KYTY_NATIVE_NP_HTTP_POOL", value);
#else
		unsetenv("KYTY_NATIVE_NP_CALLOUT");
		setenv("KYTY_NATIVE_NP_HTTP_POOL", value, 1);
#endif
		ctor(embedded.data());
		table = *reinterpret_cast<const uintptr_t**>(embedded.data());
		Check(Name, table == (std::strcmp(value, "1") == 0 ? native_table : legacy_table),
		      "HTTP-pool opt-in must imply native ownership only for exact 1");
	}
	ctor(object);
	CheckOk(Name, init(object, nullptr), "HTTP-pool-owned mutex Init");
	table = *reinterpret_cast<const uintptr_t**>(object);
	reinterpret_cast<Ctor>(table[1])(object);
	Check(Name, delete_record.calls == 4 && delete_record.raw == storage.data() &&
	            delete_record.context == &storage && delete_record.uninitialized && delete_record.depth == 0,
	      "HTTP-pool-only mode must preserve raw-header deletion and unlocked callback");
	for (const char* value: {"", "0", "true", "01", "1 ", "1"}) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		_putenv_s("KYTY_NATIVE_NP_HTTP_POOL", "");
		_putenv_s("KYTY_NATIVE_NP_ALTERNATE_STARTUP", value);
#else
		unsetenv("KYTY_NATIVE_NP_HTTP_POOL");
		setenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP", value, 1);
#endif
		ctor(embedded.data());
		table = *reinterpret_cast<const uintptr_t**>(embedded.data());
		Check(Name, table == (std::strcmp(value, "1") == 0 ? native_table : legacy_table),
		      "standalone alternate selects header-aware destruction only for exact 1");
	}
	ctor(object);
	CheckOk(Name, init(object, nullptr), "alternate-owned mutex Init");
	table = *reinterpret_cast<const uintptr_t**>(object);
	reinterpret_cast<Ctor>(table[1])(object);
	Check(Name, delete_record.calls == 5 && delete_record.raw == storage.data() &&
	            delete_record.context == &storage && delete_record.uninitialized && delete_record.depth == 0,
	      "alternate-only mode preserves raw-header deletion and unlocked callback");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	_putenv_s("KYTY_NATIVE_NP_ALTERNATE_STARTUP", saved_alternate.c_str());
	_putenv_s("KYTY_NATIVE_NP_OWNERSHIP", saved.c_str());
	_putenv_s("KYTY_NATIVE_NP_WORKQUEUE", saved_queue.c_str());
	_putenv_s("KYTY_NATIVE_NP_CALLOUT", saved_callout.c_str());
	_putenv_s("KYTY_NATIVE_NP_HTTP_POOL", saved_http_pool.c_str());
#else
	if (old_alternate_env == nullptr) { unsetenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP"); }
	else { setenv("KYTY_NATIVE_NP_ALTERNATE_STARTUP", saved_alternate.c_str(), 1); }
	if (old_env == nullptr) { unsetenv("KYTY_NATIVE_NP_OWNERSHIP"); }
	else { setenv("KYTY_NATIVE_NP_OWNERSHIP", saved.c_str(), 1); }
	if (old_queue_env == nullptr) { unsetenv("KYTY_NATIVE_NP_WORKQUEUE"); }
	else { setenv("KYTY_NATIVE_NP_WORKQUEUE", saved_queue.c_str(), 1); }
	if (old_callout_env == nullptr) { unsetenv("KYTY_NATIVE_NP_CALLOUT"); }
	else { setenv("KYTY_NATIVE_NP_CALLOUT", saved_callout.c_str(), 1); }
	if (old_http_pool_env == nullptr) { unsetenv("KYTY_NATIVE_NP_HTTP_POOL"); }
	else { setenv("KYTY_NATIVE_NP_HTTP_POOL", saved_http_pool.c_str(), 1); }
#endif
}
