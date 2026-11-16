// Included by memory.cpp in Libs::LibKernel::Memory. No renderer or guest code
// execution: exercise production guest API boundaries under real owner scopes.
namespace RuntimeExecutableMemoryPolicyFixture {
static uint64_t gpu_events = 0;
static uint64_t alloc_events = 0;
static uint64_t free_events = 0;
static void GpuEvent(bool, uint64_t, size_t) { ++gpu_events; }
static void AllocEvent(uintptr_t, size_t) { ++alloc_events; }
static void FreeEvent(uintptr_t, size_t) { ++free_events; }
static auto Snapshot() {
	const auto s = GetDebugStats();
	return std::array<uint64_t, 21> {s.direct_total, s.direct_allocated, s.pooled_allocated,
	    s.automatic_allocated, s.direct_mapped, s.flexible_total, s.flexible_used,
	    s.pool_committed, s.cpu_page_entries, s.gpu_page_entries, s.cpu_page_entries_total,
	    s.gpu_page_entries_total, TestGuestBackingSize(), TestFlexibleBackingHighWater(),
	    TestDirectMappingCount(), TestFlexibleMappingCount(), TestBackingBytesZeroed(),
	    gpu_events, alloc_events, free_events, TestGuestFreeRangeVisits()};
}
}

bool TestRuntimeExecutableMemoryPolicy(std::string* failure) {
	using namespace RuntimeExecutableMemoryPolicyFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	if (!check(g_guest_address_space && g_virtual_ranges && g_physical_memory && g_flexible_memory &&
	               !g_gpu_resources && !g_test_gpu_range && !g_alloc_callback && !g_free_callback,
	           "executable-policy fixture requires initialized memory without renderer/callbacks")) { return false; }
	constexpr uint64_t page = 0x4000;
	Loader::Program owner, importer, uninitialized_owner, session;
	owner.application_owner = &owner;
	importer.application_owner = &owner;
	uninitialized_owner.application_owner = &uninitialized_owner;
	const auto previous = Loader::RuntimeLinker::GetCurrentProgram();
	struct Cleanup {
		Loader::Program* previous;
		Loader::Program* session;
		std::vector<uint64_t> mappings;
		int64_t physical = -1;
		~Cleanup() {
			Loader::RuntimeLinker::SetCurrentProgram(session);
			for (auto address: mappings) { KernelMunmap(address, page); }
			if (physical >= 0) { KernelCheckedReleaseDirectMemory(physical, page); }
			g_test_gpu_range = nullptr;
			g_alloc_callback = nullptr;
			g_free_callback = nullptr;
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} cleanup {previous, &session};
	gpu_events = alloc_events = free_events = 0;
	g_test_gpu_range = GpuEvent;
	g_alloc_callback = AllocEvent;
	g_free_callback = FreeEvent;
	Loader::RuntimeLinker::SetCurrentProgram(&owner);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	if (!check(Loader::InitializeApplicationExecution(&owner), "initialize real shared execution control")) { return false; }
#endif
	// The policy belongs at guest APIs, not the trusted allocator. These exact
	// executable loader/runtime helpers must still work under a shared scope.
	for (bool program: {false, true}) {
		const auto address = program ? AllocateProgramMemory(0, page, VirtualMemory::Mode::ExecuteReadWrite, "policy-loader") :
		    AllocateRuntimeMemory(0, page, VirtualMemory::Mode::ExecuteReadWrite, "policy-runtime");
		if (address) { cleanup.mappings.push_back(address); }
		if (!check(address != 0, "trusted executable allocation must remain legal under a shared ProgramScope")) { return false; }
		if (program) { SetProgramMemoryProtection(address, page, VirtualMemory::Mode::ExecuteRead); }
		if (!check(ProtectGuestMemory(address, page, VirtualMemory::Mode::ExecuteRead),
		           "trusted executable protection must remain legal under a shared ProgramScope")) { return false; }
	}
	void* data = nullptr;
	const auto mapped = KernelMapFlexibleMemory(&data, page, 3, 0);
	if (mapped == OK) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(data)); }
	if (!check(mapped == OK && data, "shared non-executable flexible mapping must remain available")) { return false; }
	if (!check(KernelMprotect(data, page, 1) == OK && KernelMtypeprotect(data, page, 0, 3) == OK,
	           "shared non-executable protection changes must remain available")) { return false; }
	*static_cast<uint64_t*>(data) = 0x45584543504f4c49ULL;
	if (!check(KernelAllocateMainDirectMemory(page, page, 0, &cleanup.physical) == OK,
	           "prepare real direct allocation for mapping refusal")) { return false; }
	const auto protection_is = [&](int expected) {
		void* begin = nullptr;
		void* end = nullptr;
		int protection = -1;
		return KernelQueryMemoryProtection(data, &begin, &end, &protection) == OK && protection == expected;
	};
	for (auto* current: {&owner, &importer, &uninitialized_owner}) {
		Loader::RuntimeLinker::SetCurrentProgram(current);
		for (int prot: {4, 5, 7, 0x37}) {
			const auto before = Snapshot();
			for (bool named: {false, true}) {
				void* output = nullptr;
				const auto result = named ? KernelMapNamedFlexibleMemory(&output, page, prot, 0, "policy-denied") :
				    KernelMapFlexibleMemory(&output, page, prot, 0);
				if (result == OK && output) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(output)); }
				if (!check(result == KERNEL_ERROR_EACCES && output == nullptr && Snapshot() == before,
				           "shared executable flexible map must preserve output, accounting, backing and GPU/hooks")) { return false; }
			}
			void* replacement = data;
			if (!check(KernelMapFlexibleMemory(&replacement, page, prot, 0x10) == KERNEL_ERROR_EACCES && replacement == data &&
			               protection_is(3) && *static_cast<uint64_t*>(data) == 0x45584543504f4c49ULL && Snapshot() == before,
			           "fixed executable replacement must preserve existing RW mapping and contents")) { return false; }
			for (int variant = 0; variant != 3; ++variant) {
				void* output = nullptr;
				const auto result = variant == 0 ? KernelMapDirectMemory(&output, page, prot, 0, cleanup.physical, page) :
				    variant == 1 ? KernelMapNamedDirectMemory(&output, page, prot, 0, cleanup.physical, page, "policy-direct") :
				    KernelMapDirectMemory2(&output, page, 0, prot, 0, cleanup.physical, page);
				if (result == OK && output) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(output)); }
				if (!check(result == KERNEL_ERROR_EACCES && !output && Snapshot() == before,
				           "existing direct-map execute refusal must precede mapping and GPU mutation")) { return false; }
			}
			if (!check(KernelMprotect(data, page, prot) == KERNEL_ERROR_EACCES &&
			               KernelMtypeprotect(data, page, 0, prot) == KERNEL_ERROR_EACCES &&
			               protection_is(3) && Snapshot() == before,
			           "shared executable protection must leave semantic and host RW protection unchanged")) { return false; }
			*static_cast<uint64_t*>(data) = 0x45584543504f4c49ULL; // still genuinely host-writable
			if (!check(MapAutomaticMemory(kExtendedMemoryBase, page, 0, prot) == KERNEL_ERROR_EACCES && Snapshot() == before,
			           "automatic executable map must refuse before physical reservation/accounting")) { return false; }
			if (!check(KernelMemoryPoolCommit(data, 0x10000, 0, prot, 0) == KERNEL_ERROR_EACCES && Snapshot() == before,
			           "shared executable pool commit must refuse before backing/accounting")) { return false; }
			for (int operation: {0, 2, 3, 4}) {
				KernelBatchMapEntry entries[2] {};
				entries[0].length = page;
				entries[0].protection = 3;
				entries[0].operation = 3; // would allocate RW before the denied entry
				entries[1].start = operation == 2 || operation == 4 ? data : nullptr;
				entries[1].offset = cleanup.physical;
				entries[1].length = page;
				entries[1].protection = static_cast<unsigned char>(prot);
				entries[1].operation = operation;
				int processed = 91;
				const auto result = KernelBatchMap2(entries, 2, &processed, 0);
				if (entries[0].start) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(entries[0].start)); }
				if ((operation == 0 || operation == 3) && entries[1].start) {
					cleanup.mappings.push_back(reinterpret_cast<uint64_t>(entries[1].start));
				}
				if (!check(result == KERNEL_ERROR_EACCES && processed == 91 && !entries[0].start &&
				               entries[1].start == (operation == 2 || operation == 4 ? data : nullptr) &&
				               protection_is(3) && Snapshot() == before,
				           "mixed batch must preflight executable operations before its first mutation or output write")) { return false; }
			}
			KernelBatchMapEntry legacy {};
			legacy.start = data; legacy.length = page; legacy.protection = static_cast<unsigned char>(prot); legacy.operation = 2;
			int processed = 73;
			if (!check(KernelBatchMap(&legacy, 1, &processed) == KERNEL_ERROR_EACCES && processed == 73 && Snapshot() == before,
			           "legacy batch wrapper must preserve executable exclusion and outputs")) { return false; }
			for (uint32_t operation: {1u, 3u, 4u}) {
				KernelMemoryPoolBatchEntry entries[2] {};
				entries[0].op = 3; entries[0].protect.addr = data; entries[0].protect.len = page; entries[0].protect.prot = 1;
				entries[1].op = operation;
				if (operation == 1) {
					entries[1].commit.addr = data; entries[1].commit.len = 0x10000; entries[1].commit.prot = static_cast<uint8_t>(prot);
				} else if (operation == 3) {
					entries[1].protect.addr = data; entries[1].protect.len = page; entries[1].protect.prot = static_cast<uint8_t>(prot);
				} else {
					entries[1].type_protect.addr = data; entries[1].type_protect.len = page;
					entries[1].type_protect.prot = static_cast<uint8_t>(prot);
				}
				processed = 81;
				if (!check(KernelMemoryPoolBatch(entries, 2, &processed, 0) == KERNEL_ERROR_EACCES && processed == 81 &&
				               protection_is(3) && Snapshot() == before,
				           "pool batch must reject execute before changing an earlier entry's protection")) { return false; }
			}
		}
	}
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	void* executable = nullptr;
	const auto session_result = KernelMapNamedFlexibleMemory(&executable, page, 7, 0, "policy-session");
	if (session_result == OK && executable) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(executable)); }
	if (!check(session_result == OK && executable && KernelMprotect(executable, page, 5) == OK &&
	               KernelMtypeprotect(executable, page, 0, 7) == OK,
	           "session executable flexible maps and protection must retain their existing behavior")) { return false; }
	KernelBatchMapEntry session_entries[2] {};
	for (auto& entry: session_entries) { entry.length = page; entry.protection = 3; entry.operation = 3; }
	session_entries[1].protection = 7;
	int session_processed = -1;
	const auto session_batch = KernelBatchMap2(session_entries, 2, &session_processed, 0);
	for (const auto& entry: session_entries) {
		if (entry.start) { cleanup.mappings.push_back(reinterpret_cast<uint64_t>(entry.start)); }
	}
	if (!check(session_batch == OK && session_processed == 2 && session_entries[0].start && session_entries[1].start,
	           "session mixed RW/executable batches must retain their existing processing")) { return false; }
	void* direct = nullptr;
	if (!check(KernelMapDirectMemory(&direct, page, 7, 0, cleanup.physical, page) == KERNEL_ERROR_EACCES && !direct &&
	               KernelMemoryPoolCommit(data, 0x10000, 0, 7, 0) == KERNEL_ERROR_EINVAL,
	           "existing session direct/pool executable errors must remain unchanged")) { return false; }
	return check(!owner.application_execution || !owner.application_execution->Failure(),
	             "typed memory-policy refusal must not poison the shared owner");
}
