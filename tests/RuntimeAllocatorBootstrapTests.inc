// Included inside Loader by runtimeLinker.cpp. Requires the parent's THREE exact
// returning pairs; no classification override, fake registration or firmware.
namespace RuntimeAllocatorBootstrapFixture {
using TraceInfo = ApplicationHeapTraceInfo;
using Setter = void(KYTY_SYSV_ABI*)(void* const*);
using Trace = void(KYTY_SYSV_ABI*)(TraceInfo*);
constexpr uint64_t ImageSize = 0xc000;
constexpr uint64_t VectorOffset = 0x4100;
constexpr uint64_t PacketOffset = 0x4400;
struct Packet {
	uint64_t before;
	TraceInfo info;
	uint64_t after;
};
static_assert(sizeof(Packet) == 48);
struct Invocation {
	RuntimeLinker* linker = nullptr;
	RuntimeGuestExecution* execution = nullptr;
	uint64_t entry = 0;
	void* argument = nullptr;
	uint64_t scalar = UINT64_MAX;
	unsigned continued = 0;
	unsigned destroyed = 0;
	unsigned malloc_calls = 0;
	unsigned memalign_calls = 0;
	unsigned free_calls = 0;
	uint64_t size = 0;
	uint64_t malloc_size = 0;
	uint64_t alignment = 0;
	void* allocation = nullptr;
	bool lock_free = true;
	bool (*probe)(RuntimeLinker&) = nullptr;
	uint64_t setter = 0;
	uint64_t trace = 0;
	void* const* replacement = nullptr;
	TraceInfo* bad_header = nullptr;
	std::atomic_bool* started = nullptr;
	uint64_t image = 0;
	Packet stack_packet {};
	RuntimeGuestResult stack_set;
	RuntimeGuestResult stack_query;
};
static thread_local Invocation* active = nullptr;
static uint64_t KYTY_SYSV_ABI GuestSet(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	if (call->started) { call->started->store(true, std::memory_order_release); }
	reinterpret_cast<Setter>(call->entry)(static_cast<void* const*>(call->argument));
	++call->continued;
	return 19;
}
static uint64_t KYTY_SYSV_ABI GuestTrace(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	if (call->started) { call->started->store(true, std::memory_order_release); }
	reinterpret_cast<Trace>(call->entry)(static_cast<TraceInfo*>(call->argument));
	++call->continued;
	return 23;
}
static uint64_t KYTY_SYSV_ABI GuestScalar(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	call->scalar = static_cast<uint32_t>(reinterpret_cast<int(KYTY_SYSV_ABI*)()>(call->entry)());
	++call->continued;
	return call->scalar;
}
static void* KYTY_SYSV_ABI Malloc(uint64_t size) {
	++active->malloc_calls;
	active->size = size;
	active->malloc_size = size;
	active->lock_free = active->lock_free && active->probe(*active->linker);
	if (active->replacement) {
		auto* next = active->replacement;
		active->replacement = nullptr;
		reinterpret_cast<Setter>(active->setter)(next); // Real registered reentry.
	}
	if (active->bad_header) { reinterpret_cast<Trace>(active->trace)(active->bad_header); }
	return active->allocation;
}
static void* KYTY_SYSV_ABI MallocAlternate(uint64_t size) {
	++active->malloc_calls;
	active->size = size + 1;
	active->malloc_size = size + 1;
	active->lock_free = active->lock_free && active->probe(*active->linker);
	return active->allocation;
}
static void KYTY_SYSV_ABI Free(void*) { if (active) { ++active->free_calls; } }
static int KYTY_SYSV_ABI Memalign(void** output, uint64_t alignment, uint64_t size) {
	++active->memalign_calls;
	active->size = size;
	active->alignment = alignment;
	active->lock_free = active->lock_free && active->probe(*active->linker);
	*output = active->allocation;
	return 0;
}
static uint64_t KYTY_SYSV_ABI ExerciseHost(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	struct Pin { Invocation* call; ~Pin() { ++call->destroyed; } } pin {call};
	if (call->linker->ApplicationHeapMalloc(317) != call->allocation) { return 0; }
	if (call->execution->Failure() || call->execution->StopRequested()) { return 0; }
	return call->linker->ApplicationHeapMemalign(256, 619) == call->allocation ? 7 : 0;
}
static uint64_t KYTY_SYSV_ABI GuestExercise(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	const auto value = reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(call->entry)(call);
	++call->continued;
	return value;
}
static uint64_t KYTY_SYSV_ABI TraceThenStopHost(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	struct Pin { Invocation* call; ~Pin() { ++call->destroyed; } } pin {call};
	reinterpret_cast<Trace>(call->trace)(static_cast<TraceInfo*>(call->argument));
	call->execution->RequestStop();
	return 31;
}
static Packet FreshPacket() {
	Packet packet;
	std::memset(&packet, 0xa5, sizeof(packet));
	packet.info.size = sizeof(TraceInfo);
	return packet;
}
static Packet ReadPacket(uint64_t base, uint64_t offset = PacketOffset) {
	Packet result;
	std::memcpy(&result, reinterpret_cast<void*>(base + offset), sizeof(result));
	return result;
}
static TraceInfo* ResetPacket(uint64_t base, uint64_t offset = PacketOffset) {
	const auto packet = FreshPacket();
	std::memcpy(reinterpret_cast<void*>(base + offset), &packet, sizeof(packet));
	return reinterpret_cast<TraceInfo*>(base + offset + offsetof(Packet, info));
}
static bool HeaderAndCanaries(const Packet& packet) {
	return packet.before == 0xa5a5a5a5a5a5a5a5ULL && packet.after == packet.before &&
	    packet.info.size == sizeof(TraceInfo) && packet.info.reserved == 0 && packet.info.mode == 0 &&
	    packet.info.mask && packet.info.table;
}
static std::array<void*, 10> Vector(uint64_t base, bool alternate = false) {
	std::array<void*, 10> api {};
	api[0] = reinterpret_cast<void*>(base + (alternate ? 0x160 : 0x100));
	api[1] = reinterpret_cast<void*>(base + 0x120);
	api[6] = reinterpret_cast<void*>(base + 0x140);
	api[8] = reinterpret_cast<void*>(base + 0x120); // Validate even unused nonnull slots.
	return api;
}
static void* const* StoreVector(uint64_t base, const std::array<void*, 10>& api, uint64_t offset = VectorOffset) {
	std::memcpy(reinterpret_cast<void*>(base + offset), api.data(), sizeof(api));
	return reinterpret_cast<void* const*>(base + offset);
}
static void* VectorArgument(uint64_t base, bool alternate = false, uint64_t offset = VectorOffset) {
	StoreVector(base, Vector(base, alternate), offset);
	return reinterpret_cast<void*>(base + offset);
}
static void* KYTY_SYSV_ABI OwnedStackCoordinator(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	auto vector = Vector(call->image);
	call->entry = call->setter;
	call->argument = vector.data();
	call->stack_set = call->execution->Invoke(GuestSet, call);
	auto packet = FreshPacket();
	call->entry = call->trace;
	call->argument = &packet.info;
	call->stack_query = call->execution->Invoke(GuestTrace, call);
	call->stack_packet = packet;
	call->argument = nullptr; // Do not leave a borrowed guest-stack header behind.
	return nullptr;
}
}

bool RuntimeLinker::TestRuntimeAllocatorBootstrap(std::string* failure) {
	using namespace RuntimeAllocatorBootstrapFixture;
	using namespace Libs::LibKernel;
	const auto check = [&](bool condition, const char* message) {
		if (!condition && failure) { *failure = message; }
		return condition;
	};
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	// A/B, their modules, session, fresh A, nested failure, rejection cases,
	// stopped-before-first-call, serialized-stop races, concurrent publication,
	// unavailable control, unused-slot rejection, noaccess, readonly output,
	// native-kernel provider and an owner targeting that provider.
	constexpr unsigned Count = 31;
	Program programs[Count];
	std::vector<std::unique_ptr<HostCallReturnThunk>> gateways;
	struct Cleanup {
		RuntimeLinker& linker;
		Program* programs;
		std::vector<Program*> original;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::filesystem::path root;
		~Cleanup() {
			active = nullptr;
			RuntimeLinker::SetCurrentProgram(previous);
			for (unsigned i = 0; i != Count; ++i) {
				if (auto execution = GetApplicationExecution(&programs[i])) { execution->RequestStop(); }
			}
			linker.m_programs = std::move(original);
			for (unsigned i = 0; i != Count; ++i) {
				programs[i].application_import_bindings.reset();
				programs[i].elf.reset();
				if (programs[i].base_vaddr) { Memory::FreeGuestMemory(programs[i].base_vaddr, ImageSize); }
			}
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker, programs, linker.m_programs};
	cleanup.root = std::filesystem::absolute("_TempData") /
	    ("allocator-bootstrap-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root);
	const auto path = cleanup.root / "allocator.prx";
	Elf64_Ehdr header {};
	std::memcpy(header.e_ident, "\177ELF", 4);
	header.e_ident[EI_CLASS] = ELFCLASS64; header.e_ident[EI_DATA] = ELFDATA2LSB;
	header.e_ident[EI_VERSION] = EV_CURRENT; header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
	header.e_type = ET_DYNAMIC; header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
	header.e_ehsize = sizeof(header); header.e_phoff = sizeof(header);
	header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 3;
	Elf64_Phdr segments[3] {};
	for (unsigned i = 0; i != 3; ++i) {
		segments[i].p_type = PT_LOAD;
		segments[i].p_vaddr = i * 0x4000;
		segments[i].p_memsz = 0x4000;
		segments[i].p_flags = i == 0 ? PF_R | PF_X : i == 1 ? PF_R | PF_W : PF_R;
	}
	Common::File file;
	if (!check(file.Create(path), "create bounded allocator fixture ELF metadata")) { return false; }
	uint32_t written = 0;
	file.Write(&header, sizeof(header), &written); file.Write(segments, sizeof(segments), &written); file.Close();
	Libs::InitLibKernel_1(linker.Symbols());
	Libs::LibC::InitLibC_1(linker.Symbols());
	const std::array<const char*, 3> nids {"p5EcQeEeJAE", "NWtTN10cJzE", "jh+8XiK4LeE"};
	const std::array<const char*, 3> expressions {"LibKernel::KernelRtldSetApplicationHeapAPI",
	    "LibcInternalExt::LibcHeapGetTraceInfo", "Memory::KernelIsAddressSanitizerEnabled"};
	std::array<SymbolRecord, 3> registrations;
	for (unsigned j = 0; j != 3; ++j) {
		const auto* record = linker.Symbols()->FindRegistration({nids[j], j == 1 ? "LibcInternalExt" : "libkernel", 1,
		    j == 1 ? "LibcInternal" : "libkernel", 1, 1, SymbolType::Func}, expressions[j]);
		if (!check(record != nullptr, "all THREE exact production allocator registrations must exist")) { return false; }
		registrations[j] = *record;
	}
	for (unsigned i = 0; i != Count; ++i) {
		auto& program = programs[i];
		program.rt = &linker;
		program.unique_id = 990200 + i;
		program.file_name = path;
		program.application_owner = i == 4 ? nullptr : i == 2 ? &programs[0] : i == 3 ? &programs[1] :
		    i == 20 ? &programs[21] : &program;
		program.dynamic_info = std::make_unique<DynamicInfo>();
		program.dynamic_info->import_libs = {{"A", 1, "libkernel"}, {"B", 1, "LibcInternalExt"}};
		program.dynamic_info->import_modules = {{"B", 1, 1, "libkernel"}, {"C", 1, 1, "LibcInternal"}};
		program.elf = std::make_unique<Elf64>(); program.elf->Open(path);
		if (!check(program.elf->IsValid(), "parse allocator fixture private image segments")) { return false; }
		program.base_vaddr = Memory::AllocateProgramMemory(0, ImageSize,
		    Common::VirtualMemory::Mode::ExecuteReadWrite, "allocator_bootstrap_fixture");
		program.base_size = program.mapped_size = ImageSize;
		if (!check(program.base_vaddr != 0, "allocate trusted fixture text/data/read-only ranges")) { return false; }
		// Trusted fixture RX stubs, NEVER a guest executable-allocation permission.
		const std::array<uint64_t, 4> functions {reinterpret_cast<uint64_t>(Malloc), reinterpret_cast<uint64_t>(Free),
		    reinterpret_cast<uint64_t>(Memalign), reinterpret_cast<uint64_t>(MallocAlternate)};
		for (unsigned j = 0; j != functions.size(); ++j) {
			auto* code = reinterpret_cast<uint8_t*>(program.base_vaddr + 0x100 + j * 0x20);
			code[0] = 0x48; code[1] = 0xb8; std::memcpy(code + 2, &functions[j], 8); code[10] = 0xff; code[11] = 0xe0;
		}
		StoreVector(program.base_vaddr, Vector(program.base_vaddr));
		ResetPacket(program.base_vaddr);
		Memory::SetProgramMemoryProtection(program.base_vaddr, 0x4000, Common::VirtualMemory::Mode::ExecuteRead);
		Memory::SetProgramMemoryProtection(program.base_vaddr + 0x4000, 0x4000, Common::VirtualMemory::Mode::ReadWrite);
		Memory::SetProgramMemoryProtection(program.base_vaddr + 0x8000, 0x4000, Common::VirtualMemory::Mode::Read);
		Common::VirtualMemory::FlushInstructionCache(program.base_vaddr, 0x4000);
		if (program.application_owner == &program && i != 16 &&
		    !check(InitializeApplicationExecution(&program), "initialize actual allocator owner control")) { return false; }
		linker.m_programs.push_back(&program);
	}
	programs[20].file_name = "libkernel-fixture.prx";
	const auto probe = +[](RuntimeLinker& runtime) {
		if (Common::GetHostLockCriticalCounter()->load() != 0) { return false; }
		bool acquired = false;
		std::thread worker([&] {
			acquired = runtime.m_mutex.TryLock();
			if (acquired) { runtime.m_mutex.Unlock(); }
		});
		worker.join();
		return acquired;
	};
	uint64_t entries[Count][3] {};
	const std::array<const char*, 3> raw_names {"p5EcQeEeJAE#A#B", "NWtTN10cJzE#B#C", "jh+8XiK4LeE#A#B"};
	for (unsigned i = 0; i != Count; ++i) {
		if (i == 4 || i == 16 || i == 20) { continue; }
		programs[i].application_import_bindings = std::make_unique<ApplicationImportBindings>(&programs[i]);
		for (unsigned j = 0; j != 3; ++j) {
			if (!check(programs[i].application_import_bindings->BindResolved(nids[j], registrations[j]) != 0,
			           "parent must integrate the THREE exact returning pairs; fixture does not override production admission")) { return false; }
			const auto slot = programs[i].base_vaddr + 0x4000 + j * sizeof(uint64_t);
			entries[i][j] = programs[i].application_import_bindings->Bind(slot, raw_names[j]);
			if (!check(entries[i][j] != 0, "bind actual raw import identity and immutable first-call resolver")) { return false; }
			std::memcpy(reinterpret_cast<void*>(slot), &entries[i][j], sizeof(uint64_t));
		}
	}
	const auto invoke = [&](unsigned index, unsigned kind, void* argument, Invocation* supplied = nullptr) {
		RuntimeLinker::SetCurrentProgram(&programs[index]);
		Invocation local;
		auto& call = supplied ? *supplied : local;
		call.linker = &linker; call.execution = GetApplicationExecution(&programs[index]).get();
		call.entry = entries[index][kind]; call.argument = argument;
		active = &call;
		const auto result = call.execution->Invoke(kind == 0 ? GuestSet : kind == 1 ? GuestTrace : GuestScalar, &call);
		active = nullptr;
		return result;
	};
	const auto gateway = [&](unsigned owner, uint64_t target) {
		const auto execution = GetApplicationExecution(&programs[owner]);
		auto thunk = HostCallReturnThunk::Create(execution->Boundary(), target, execution.get(),
		    RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
		if (!thunk) { return uint64_t {0}; }
		const auto address = thunk->GetAddress(); gateways.push_back(std::move(thunk)); return address;
	};
	RuntimeLinker::SetCurrentProgram(&programs[4]);
	Packet session = FreshPacket();
	reinterpret_cast<Trace>(registrations[1].vaddr)(&session.info);
	if (!check(HeaderAndCanaries(session), "session trace header is deterministic and preserves surrounding canaries")) { return false; }
	std::array<uint64_t, ApplicationImportData::HeapTraceTableCount> session_table;
	std::memcpy(session_table.data(), reinterpret_cast<void*>(session.info.table), sizeof(session_table));
	const auto session_mask = *reinterpret_cast<uint64_t*>(session.info.mask);
	const auto old_malloc = linker.m_application_heap_malloc;
	const auto old_free = linker.m_application_heap_free;
	const auto old_memalign = linker.m_application_heap_posix_memalign;
	struct RestoreSession {
		RuntimeLinker& linker;
		application_heap_malloc_func_t malloc;
		application_heap_free_func_t free;
		application_heap_posix_memalign_func_t memalign;
		~RestoreSession() {
			linker.m_application_heap_malloc = malloc; linker.m_application_heap_free = free;
			linker.m_application_heap_posix_memalign = memalign;
		}
	} restore_session {linker, old_malloc, old_free, old_memalign};
	auto session_vector = Vector(programs[4].base_vaddr);
	reinterpret_cast<Setter>(registrations[0].vaddr)(session_vector.data());
	const auto session_malloc = linker.m_application_heap_malloc;
	const auto session_free = linker.m_application_heap_free;
	const auto session_memalign = linker.m_application_heap_posix_memalign;
	session_vector.fill(nullptr);
	Invocation session_heap;
	session_heap.linker = &linker; session_heap.probe = probe;
	session_heap.allocation = reinterpret_cast<void*>(programs[4].base_vaddr + 0x4700);
	active = &session_heap;
	const bool session_callbacks = linker.ApplicationHeapMalloc(41) == session_heap.allocation &&
	    linker.ApplicationHeapMemalign(128, 83) == session_heap.allocation;
	active = nullptr;
	if (!check(session_callbacks && session_heap.malloc_calls == 1 && session_heap.memalign_calls == 1 && session_heap.lock_free &&
	    session_heap.size == 83 && session_heap.alignment == 128,
	    "session setter also copies its borrowed vector and retains direct typed callbacks without locks")) { return false; }
	TraceInfo traces[2];
	for (unsigned i = 0; i != 2; ++i) {
		Invocation setter;
		const auto set = invoke(i, 0, VectorArgument(programs[i].base_vaddr), &setter);
		if (!check(set.Returned() && setter.continued == 1 && setter.malloc_calls == 0 && setter.memalign_calls == 0 && setter.free_calls == 0 &&
		    programs[i].application_heap_malloc == reinterpret_cast<application_heap_malloc_func_t>(programs[i].base_vaddr + 0x100),
		    "real registered deferred setter copies owner callbacks and invokes NONE during registration")) { return false; }
		const auto queried = invoke(i + 2, 1, ResetPacket(programs[i + 2].base_vaddr));
		const auto packet = ReadPacket(programs[i + 2].base_vaddr);
		if (!check(queried.Returned() && HeaderAndCanaries(packet), "private module query publishes the complete 32-byte owner header")) { return false; }
		traces[i] = packet.info;
		if (!check(traces[i].mask != session.info.mask && traces[i].table != session.info.table &&
		    *reinterpret_cast<uint64_t*>(traces[i].mask) == 0, "shared trace cells are fresh guest storage, not session globals")) { return false; }
		for (unsigned j = 0; j != ApplicationImportData::HeapTraceTableCount; ++j) {
			if (!check(reinterpret_cast<uint64_t*>(traces[i].table)[j] == 0, "all owner trace table cells initialize to zero")) { return false; }
		}
		void* begin = nullptr; void* end = nullptr; int prot = 0;
		if (!check(Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(traces[i].table), &begin, &end, &prot) == 0 &&
		    prot == 3 && traces[i].table + 64 * sizeof(uint64_t) <= reinterpret_cast<uint64_t>(end), "trace cells have real non-executable RW guest backing")) { return false; }
		const auto lifetime = programs[i].application_execution->Owner();
		if (!check(!programs[i].application_import_data->ContainsObjectAddress(lifetime, traces[i].mask),
		    "heap mask remains API-returned data, not an import-object whitelist extension")) { return false; }
		for (uint64_t byte = 0; byte != 64 * sizeof(uint64_t); ++byte) {
			if (!check(!programs[i].application_import_data->ContainsObjectAddress(lifetime, traces[i].table + byte),
			    "API-returned trace cells never become audited import objects")) { return false; }
		}
		Invocation scalar;
		if (!check(invoke(i, 2, nullptr, &scalar).Returned() && scalar.scalar == 0 && scalar.continued == 1,
		    "actual registered scalar no-ASan query returns zero through production admission/gateway")) { return false; }
	}
	if (!check(traces[0].mask != traces[1].mask && traces[0].table != traces[1].table, "two live owners never alias trace cells")) { return false; }
	*reinterpret_cast<uint64_t*>(traces[0].mask) = 0x1234;
	reinterpret_cast<uint64_t*>(traces[0].table)[63] = programs[0].base_vaddr + 0x4700;
	if (!check(invoke(0, 1, ResetPacket(programs[0].base_vaddr)).Returned() &&
	    ReadPacket(programs[0].base_vaddr).info.mask == traces[0].mask && *reinterpret_cast<uint64_t*>(traces[0].mask) == 0x1234 &&
	    reinterpret_cast<uint64_t*>(traces[0].table)[63] == programs[0].base_vaddr + 0x4700 &&
	    *reinterpret_cast<uint64_t*>(traces[1].mask) == 0 && reinterpret_cast<uint64_t*>(traces[1].table)[63] == 0,
	    "requery does not reset guest mutations or affect another owner's cells")) { return false; }
	// Destroy the borrowed vector contents AFTER the registered setter returns.
	std::memset(reinterpret_cast<void*>(programs[0].base_vaddr + VectorOffset), 0xcc, 80);
	Invocation heap;
	heap.linker = &linker; heap.execution = programs[0].application_execution.get(); heap.probe = probe;
	heap.allocation = reinterpret_cast<void*>(programs[0].base_vaddr + 0x4700);
	heap.entry = gateway(0, reinterpret_cast<uint64_t>(ExerciseHost));
	heap.setter = entries[0][0]; heap.trace = entries[0][1];
	if (!check(heap.entry != 0, "create real allocator-consumer host gateway")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[2]); active = &heap;
	const auto callbacks = heap.execution->Invoke(GuestExercise, &heap);
	active = nullptr;
	if (!check(callbacks.Returned() && callbacks.call.value == 7 && heap.malloc_calls == 1 && heap.memalign_calls == 1 &&
	    heap.size == 619 && heap.alignment == 256 && heap.destroyed == 1 && heap.lock_free,
	    "copied vector survives mutation; typed nested callbacks preserve args/results and hold no loader/critical lock")) { return false; }
	// Replace the vector from inside malloc: no loader lock held over callback.
	heap.replacement = StoreVector(programs[0].base_vaddr, Vector(programs[0].base_vaddr, true), 0x4200);
	heap.continued = 0;
	active = &heap;
	const auto replaced = heap.execution->Invoke(GuestExercise, &heap);
	active = nullptr;
	if (!check(replaced.Returned() && heap.lock_free && heap.replacement == nullptr &&
	    programs[0].application_heap_malloc == reinterpret_cast<application_heap_malloc_func_t>(programs[0].base_vaddr + 0x160),
	    "registered reentrant replacement publishes a new snapshot without invalidating the active callback")) { return false; }
	active = &heap;
	const auto new_callback = heap.execution->Invoke(GuestExercise, &heap);
	active = nullptr;
	if (!check(new_callback.Returned() && new_callback.call.value == 7 && heap.malloc_calls == 3 && heap.malloc_size == 318 &&
	    heap.memalign_calls == 3 && heap.lock_free, "subsequent allocation invokes the replacement, not the mutated original vector")) { return false; }
	Invocation heap_b;
	heap_b.linker = &linker; heap_b.execution = programs[1].application_execution.get(); heap_b.probe = probe;
	heap_b.allocation = reinterpret_cast<void*>(programs[1].base_vaddr + 0x4700);
	heap_b.entry = gateway(1, reinterpret_cast<uint64_t>(ExerciseHost));
	if (!check(heap_b.entry != 0, "create independent B allocator-consumer gateway")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[3]); active = &heap_b;
	const auto callbacks_b = heap_b.execution->Invoke(GuestExercise, &heap_b);
	active = nullptr;
	if (!check(callbacks_b.Returned() && callbacks_b.call.value == 7 && heap_b.malloc_calls == 1 && heap_b.memalign_calls == 1 &&
	    heap_b.lock_free && programs[0].application_heap_malloc != programs[1].application_heap_malloc,
	    "B private-module allocator calls stay independent of A replacement and session callbacks")) { return false; }
	// NULL clears all three slots atomically while live, and never calls free.
	if (!check(invoke(1, 0, nullptr).Returned() && !programs[1].application_heap_malloc && !programs[1].application_heap_free &&
	    !programs[1].application_heap_posix_memalign, "registered NULL setter clears only B's callback snapshot")) { return false; }
	auto optional_memalign = Vector(programs[1].base_vaddr);
	optional_memalign[6] = nullptr;
	if (!check(invoke(1, 0, const_cast<void*>(static_cast<const void*>(StoreVector(programs[1].base_vaddr, optional_memalign)))).Returned() &&
	    programs[1].application_heap_malloc && !programs[1].application_heap_posix_memalign &&
	    !linker.ApplicationHeapMemalign(64, 8), "optional NULL memalign cannot borrow the session allocator")) { return false; }
	// A real nested trace getter refuses malformed output; host RAII still ends.
	if (!check(invoke(6, 0, VectorArgument(programs[6].base_vaddr)).Returned(),
	    "prime nested-failure owner through the registered setter")) { return false; }
	Invocation nested;
	nested.linker = &linker; nested.execution = programs[6].application_execution.get(); nested.probe = probe;
	nested.allocation = reinterpret_cast<void*>(programs[6].base_vaddr + 0x4700);
	nested.entry = gateway(6, reinterpret_cast<uint64_t>(ExerciseHost)); nested.trace = entries[6][1];
	if (!check(nested.entry != 0, "create nested-failure host consumer gateway")) { return false; }
	nested.bad_header = ResetPacket(programs[6].base_vaddr); nested.bad_header->size = 31;
	const auto bad_packet = ReadPacket(programs[6].base_vaddr);
	RuntimeLinker::SetCurrentProgram(&programs[6]); active = &nested;
	const auto nested_failure = nested.execution->Invoke(GuestExercise, &nested);
	active = nullptr;
	const auto after_bad = ReadPacket(programs[6].base_vaddr);
	if (!check(nested.entry && !nested_failure.Returned() && nested_failure.failure.reason == GuestFailureReason::InvalidTransition &&
	    nested.destroyed == 1 && nested.continued == 0 && nested.memalign_calls == 0 && nested.lock_free &&
	    !programs[6].application_import_data &&
	    std::memcmp(&bad_packet, &after_bad, sizeof(Packet)) == 0,
	    "nested registered getter failure preserves output, ends host RAII and suppresses outer guest continuation")) { return false; }
	// Each rejection uses a fresh owner: failures are sticky, never reset for a test.
	for (unsigned index: {7u, 8u, 9u, 10u, 17u, 18u, 21u, 23u, 24u, 26u, 29u, 30u}) {
		if (!check(invoke(index, 0, VectorArgument(programs[index].base_vaddr)).Returned(),
		    "prime rejection owner with a complete checked callback snapshot")) { return false; }
		auto vector = Vector(programs[index].base_vaddr);
		if (index == 7) { vector[0] = reinterpret_cast<void*>(Malloc); }
		if (index == 8) { vector[1] = reinterpret_cast<void*>(programs[0].base_vaddr + 0x120); }
		if (index == 9) { vector[6] = reinterpret_cast<void*>(programs[index].base_vaddr + 0x4800); }
		if (index == 17) { vector[8] = reinterpret_cast<void*>(Free); }
		if (index == 21) { vector[0] = reinterpret_cast<void*>(programs[20].base_vaddr + 0x100); }
		if (index == 30) { vector[1] = nullptr; }
		auto* input = StoreVector(programs[index].base_vaddr, vector);
		if (index == 10) { input = reinterpret_cast<void* const*>(programs[index].base_vaddr + 0x7ff0); }
		if (index == 18) { input = reinterpret_cast<void* const*>(UINT64_MAX - 31); }
		if (index == 23) { input = reinterpret_cast<void* const*>(programs[0].base_vaddr + VectorOffset); }
		if (index == 24) { input = vector.data(); } // Host stack is readable, but not guest-owned.
		if (index == 26) {
			Memory::SetProgramMemoryProtection(programs[index].base_vaddr, 0x4000, Common::VirtualMemory::Mode::Read);
		}
		if (index == 29 && !check(Common::VirtualMemory::Protect(programs[index].base_vaddr + 0x4000, 0x4000,
		    Common::VirtualMemory::Mode::NoAccess), "make a real unreadable private vector page")) { return false; }
		const auto outcome = invoke(index, 0, const_cast<void*>(static_cast<const void*>(input)));
		if (index == 29 && !check(Common::VirtualMemory::Protect(programs[index].base_vaddr + 0x4000, 0x4000,
		    Common::VirtualMemory::Mode::ReadWrite), "restore fixture page after refused unreadable vector")) { return false; }
		if (!check(!outcome.Returned() && outcome.failure.reason == GuestFailureReason::InvalidTransition &&
		    programs[index].application_heap_malloc == reinterpret_cast<application_heap_malloc_func_t>(programs[index].base_vaddr + 0x100) &&
		    programs[index].application_heap_free == reinterpret_cast<application_heap_free_func_t>(programs[index].base_vaddr + 0x120) &&
		    programs[index].application_heap_posix_memalign == reinterpret_cast<application_heap_posix_memalign_func_t>(programs[index].base_vaddr + 0x140),
		    "host/foreign/data/unused-slot/kernel/nonexecutable targets and bad vector spans preserve the prior snapshot")) { return false; }
	}
	// Complete writable-span validation precedes header read/allocation/output.
	for (unsigned index: {11u, 12u, 19u, 25u, 27u}) {
		Packet host_packet = FreshPacket();
		const auto host_before = host_packet;
		auto* output = ResetPacket(programs[index].base_vaddr);
		if (index == 11) { output = nullptr; }
		if (index == 12) { output = reinterpret_cast<TraceInfo*>(programs[0].base_vaddr + PacketOffset + 8); }
		if (index == 19) { output = reinterpret_cast<TraceInfo*>(programs[index].base_vaddr + 0x8000); }
		if (index == 25) { output = reinterpret_cast<TraceInfo*>(programs[index].base_vaddr + 0x7ff0); }
		if (index == 27) { output = &host_packet.info; }
		const auto packet = ReadPacket(programs[index].base_vaddr);
		const auto foreign = ReadPacket(programs[0].base_vaddr);
		const auto outcome = invoke(index, 1, output);
		const auto after = ReadPacket(programs[index].base_vaddr);
		const auto after_foreign = ReadPacket(programs[0].base_vaddr);
		if (!check(!outcome.Returned() && outcome.failure.reason == GuestFailureReason::InvalidTransition &&
		    !programs[index].application_import_data && std::memcmp(&packet, &after, sizeof(Packet)) == 0 &&
		    std::memcmp(&foreign, &after_foreign, sizeof(Packet)) == 0 && std::memcmp(&host_before, &host_packet, sizeof(Packet)) == 0,
		    "NULL/foreign/readonly/truncated/host output refuses before allocation or output/canary mutation")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&programs[16]);
	reinterpret_cast<Trace>(registrations[1].vaddr)(reinterpret_cast<TraceInfo*>(1));
	reinterpret_cast<Setter>(registrations[0].vaddr)(reinterpret_cast<void* const*>(1));
	if (!check(!programs[16].application_import_data && !programs[16].application_heap_malloc,
	    "unavailable shared control never borrows session cells or callbacks")) { return false; }
	programs[13].application_execution->RequestStop();
	RuntimeLinker::SetCurrentProgram(&programs[13]);
	reinterpret_cast<Trace>(registrations[1].vaddr)(reinterpret_cast<TraceInfo*>(1));
	reinterpret_cast<Setter>(registrations[0].vaddr)(reinterpret_cast<void* const*>(1));
	if (!check(!programs[13].application_import_data && !programs[13].application_heap_malloc &&
	    !programs[13].application_execution->Failure(), "stop before first call refuses WITHOUT reading bad input, allocation or failure")) { return false; }
	Invocation stopped_scalar;
	const auto scalar_refused = invoke(13, 2, nullptr, &stopped_scalar);
	if (!check(!scalar_refused.Returned() && scalar_refused.stopped && stopped_scalar.continued == 0 && stopped_scalar.scalar == UINT64_MAX,
	    "stopped scalar invocation suppresses result assignment and guest continuation at execution admission")) { return false; }
	// Concurrent registered setters/first trace queries share ONE storage object
	// and publish whole snapshots. Immutable vector inputs and disjoint output
	// packets avoid manufacturing a guest data race in this fixture.
	void* concurrent_vectors[2] {VectorArgument(programs[22].base_vaddr), VectorArgument(programs[22].base_vaddr, true, 0x4200)};
	TraceInfo* concurrent_outputs[2] {ResetPacket(programs[22].base_vaddr), ResetPacket(programs[22].base_vaddr, 0x4500)};
	std::atomic_bool concurrent_ok {true};
	std::thread publishers[2];
	for (unsigned index = 0; index != 2; ++index) {
		publishers[index] = std::thread([&, index] {
			RuntimeLinker::SetCurrentProgram(&programs[22]);
			for (unsigned iteration = 0; iteration != 32; ++iteration) {
				Invocation set;
				set.entry = entries[22][0]; set.argument = concurrent_vectors[index];
				Invocation query;
				query.entry = entries[22][1]; query.argument = concurrent_outputs[index];
				if (!programs[22].application_execution->Invoke(GuestSet, &set).Returned() || set.continued != 1 ||
				    !programs[22].application_execution->Invoke(GuestTrace, &query).Returned() || query.continued != 1) {
					concurrent_ok.store(false); break;
				}
			}
			RuntimeLinker::SetCurrentProgram(nullptr);
		});
	}
	for (auto& publisher: publishers) { publisher.join(); }
	const auto concurrent_a = ReadPacket(programs[22].base_vaddr);
	const auto concurrent_b = ReadPacket(programs[22].base_vaddr, 0x4500);
	if (!check(concurrent_ok.load() && HeaderAndCanaries(concurrent_a) && HeaderAndCanaries(concurrent_b) &&
	    concurrent_a.info.mask == concurrent_b.info.mask && concurrent_a.info.table == concurrent_b.info.table &&
	    (programs[22].application_heap_malloc == reinterpret_cast<application_heap_malloc_func_t>(programs[22].base_vaddr + 0x100) ||
	     programs[22].application_heap_malloc == reinterpret_cast<application_heap_malloc_func_t>(programs[22].base_vaddr + 0x160)) &&
	    programs[22].application_heap_free == reinterpret_cast<application_heap_free_func_t>(programs[22].base_vaddr + 0x120) &&
	    programs[22].application_heap_posix_memalign == reinterpret_cast<application_heap_posix_memalign_func_t>(programs[22].base_vaddr + 0x140),
	    "concurrent registered replacement/query publishes one stable store and a coherent callback snapshot")) { return false; }
	// The real native caller uses a stack-local trace header. Exercise that
	// origin, not just the private RW image path used by the canary tests above.
	Invocation stack_call;
	stack_call.image = programs[28].base_vaddr; stack_call.execution = programs[28].application_execution.get();
	stack_call.setter = entries[28][0]; stack_call.trace = entries[28][1];
	RuntimeLinker::SetCurrentProgram(&programs[28]);
	Pthread stack_thread = nullptr;
	if (!check(PthreadCreateRuntimeCoordinator(&stack_thread, nullptr, OwnedStackCoordinator, &stack_call, "allocator-stack-fixture") == 0,
	    "create actual owned pthread/guest-stack allocator fixture")) { return false; }
	if (!check(PthreadJoin(stack_thread, nullptr) == 0 && stack_call.stack_set.Returned() && stack_call.stack_query.Returned() &&
	    HeaderAndCanaries(stack_call.stack_packet) && programs[28].application_heap_malloc ==
	    reinterpret_cast<application_heap_malloc_func_t>(programs[28].base_vaddr + 0x100),
	    "registered setter/query accept the actual current owner's pinned guest stack and finish actual native join")) { return false; }
	// Serialize a real registered invocation behind the loader, stop first, then
	// release. No sleeps/test hook inside production publication are required.
	for (unsigned index: {14u, 15u}) {
		std::atomic_bool started {false};
		Invocation racing;
		racing.linker = &linker; racing.execution = programs[index].application_execution.get(); racing.started = &started;
		racing.entry = entries[index][index == 14 ? 0 : 1];
		racing.argument = index == 14 ? VectorArgument(programs[index].base_vaddr) :
		    static_cast<void*>(ResetPacket(programs[index].base_vaddr));
		const auto before = ReadPacket(programs[index].base_vaddr);
		RuntimeGuestResult outcome;
		linker.m_mutex.Lock();
		std::thread worker([&] {
			RuntimeLinker::SetCurrentProgram(&programs[index]);
			outcome = racing.execution->Invoke(index == 14 ? GuestSet : GuestTrace, &racing);
			RuntimeLinker::SetCurrentProgram(nullptr);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
		const bool entered = started.load(std::memory_order_acquire);
		racing.execution->RequestStop();
		linker.m_mutex.Unlock();
		worker.join();
		const auto after = ReadPacket(programs[index].base_vaddr);
		if (!check(entered && !outcome.Returned() && outcome.stopped && racing.continued == 0 &&
		    !programs[index].application_heap_malloc && !programs[index].application_import_data &&
		    std::memcmp(&before, &after, sizeof(Packet)) == 0,
		    "stop winning loader/publication race prevents registered setter/getter output or storage publication")) { return false; }
	}
	// Stop AFTER successful registered output: pins survive, outer guest does not.
	Invocation stopping;
	stopping.linker = &linker; stopping.execution = programs[0].application_execution.get(); stopping.trace = registrations[1].vaddr;
	stopping.argument = ResetPacket(programs[0].base_vaddr);
	stopping.entry = gateway(0, reinterpret_cast<uint64_t>(TraceThenStopHost));
	if (!check(stopping.entry != 0, "create post-publication stop gateway")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[0]);
	const auto stopped = stopping.execution->Invoke(GuestExercise, &stopping);
	const auto lifetime = stopping.execution->Owner();
	TraceInfo retained;
	if (!check(!stopped.Returned() && stopped.stopped && !stopped.failure && stopping.destroyed == 1 && stopping.continued == 0 &&
	    programs[0].application_import_data->HeapTraceInfo(lifetime, &retained) && retained.mask == traces[0].mask &&
	    *reinterpret_cast<uint64_t*>(retained.mask) == 0x1234, "post-publication stop completes host cleanup and retains copied guest cells")) { return false; }
	const auto frozen = ReadPacket(programs[0].base_vaddr);
	reinterpret_cast<Trace>(registrations[1].vaddr)(reinterpret_cast<TraceInfo*>(stopping.argument));
	reinterpret_cast<Setter>(registrations[0].vaddr)(nullptr);
	const auto frozen_after = ReadPacket(programs[0].base_vaddr);
	if (!check(std::memcmp(&frozen, &frozen_after, sizeof(Packet)) == 0 && programs[0].application_heap_malloc &&
	    !linker.ApplicationHeapMalloc(999), "stopped calls cannot reset output/cells/callback snapshot or enter callbacks")) { return false; }
	if (!check(invoke(5, 1, ResetPacket(programs[5].base_vaddr)).Returned(), "fresh same-file relaunch queries registered trace output")) { return false; }
	const auto relaunched = ReadPacket(programs[5].base_vaddr).info;
	if (!check(relaunched.mask != retained.mask && relaunched.table != retained.table && *reinterpret_cast<uint64_t*>(relaunched.mask) == 0 &&
	    !programs[0].application_import_data->HeapTraceInfo(programs[5].application_execution->Owner(), &retained),
	    "new lifetime gets fresh cells and cannot query the retired store with its token")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[4]);
	Packet session_after = FreshPacket(); reinterpret_cast<Trace>(registrations[1].vaddr)(&session_after.info);
	if (!check(HeaderAndCanaries(session_after) && session_after.info.mask == session.info.mask && session_after.info.table == session.info.table &&
	    *reinterpret_cast<uint64_t*>(session.info.mask) == session_mask &&
	    std::memcmp(reinterpret_cast<void*>(session.info.table), session_table.data(), sizeof(session_table)) == 0 &&
	    linker.m_application_heap_malloc == session_malloc && linker.m_application_heap_free == session_free &&
	    linker.m_application_heap_posix_memalign == session_memalign, "all shared owner operations preserve session cells and callbacks")) { return false; }
	for (unsigned i = 0; i != Count; ++i) {
		if (auto execution = GetApplicationExecution(&programs[i]); execution &&
		    !check(execution->Outstanding() == 0, "allocator fixture leaves no guest/callback/host work leases")) { return false; }
	}
	return check(GuestCallBoundary::CurrentDepth() == 0 && HostCallReturnThunk::CurrentDepth() == 0,
	    "allocator fixture returns through every production boundary/gateway");
}
