// Included inside Loader by runtimeLinker.cpp in the CPU fixture build.
// Real files/hard links and production lookup, StartModule, and guest
// LoadStartModule/Dlsym exports; only already-mapped Program records and their
// initializer/export callbacks are synthetic. No firmware or graphics required.
namespace RuntimeProgramFileReuseFixture {
struct Pending {
	RuntimeLinker* linker;
	Program* program;
	int value;
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false, release = false, second_started = false, second_finished = false;
	unsigned calls = 0;
	int nested = 0;
};
static KYTY_SYSV_ABI int HeldInitialize(size_t args, const void* argp, module_func_t) {
	if (args != sizeof(Pending) || !argp) { return -1; }
	auto& state = *static_cast<Pending*>(const_cast<void*>(argp));
	int ignored = -1;
	state.nested = state.linker->LoadStartModuleReference(state.program, args, argp, &ignored);
	std::unique_lock lock(state.mutex);
	++state.calls;
	state.entered = true;
	state.changed.notify_all();
	state.changed.wait(lock, [&] { return state.release; });
	return state.value;
}
static KYTY_SYSV_ABI int Initialize(size_t args, const void* argp, module_func_t) {
	if (args != sizeof(unsigned) || argp == nullptr) { return -1; }
	++*static_cast<unsigned*>(const_cast<void*>(argp));
	return 0;
}
} // namespace RuntimeProgramFileReuseFixture

bool RuntimeLinker::TestProgramFileReuse(std::string* failure) {
	namespace Fs = Libs::LibKernel::FileSystem;
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure != nullptr) { *failure = message; }
		return ok;
	};
	if (!check(linker.m_programs.empty() && !linker.NativeSslSelected() &&
	               !linker.m_ssl_smoke_prepared && !linker.m_native_libc_startup.Enabled(),
	           "module reuse fixture requires a fresh default loader process")) { return false; }
	Program shared, different, pending_ok, pending_error, app_a, private_a, app_b, private_b;
	struct Cleanup {
		std::vector<Program*>& programs;
		bool& prepared;
		Program*& pinned_libc;
		Program*& pinned_ssl;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::filesystem::path root;
		std::vector<uint64_t> mappings;
		~Cleanup() {
			for (auto* p: programs) { p->elf.reset(); }
			programs.clear(); // Stack records must never reach DeleteProgram.
			for (auto address: mappings) { Libs::LibKernel::Memory::FreeGuestMemory(address, 4096); }
			prepared = false;
			pinned_libc = nullptr;
			pinned_ssl = nullptr;
			Fs::Shutdown();
			RuntimeLinker::SetCurrentProgram(previous);
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker.m_programs, linker.m_ssl_smoke_prepared,
	           linker.m_ssl_smoke_libc, linker.m_ssl_smoke_ssl};
	cleanup.root = std::filesystem::temp_directory_path() /
	    ("kyty_module_reuse_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	SetCurrentProgram(nullptr);
	Libs::LibKernel::Initialize(); // StopModule retires the real pthread static-object registry.
	Fs::Initialize();
	std::filesystem::create_directories(cleanup.root / "firmware");
	std::filesystem::create_directories(cleanup.root / "staged");
	std::filesystem::create_directories(cleanup.root / "different");
	Fs::Mount(cleanup.root, "/module-reuse");
	const auto source = cleanup.root / "firmware/libSceComposite.sprx";
	const auto alias = cleanup.root / "staged/libSceComposite.sprx";
	const auto distinct = cleanup.root / "different/libSceComposite.sprx";
	const auto pinned_alias = cleanup.root / "different/pinned-alias.sprx";
	Elf64_Ehdr header {};
	std::memcpy(header.e_ident, "\177ELF", 4);
	header.e_ident[EI_CLASS] = ELFCLASS64;
	header.e_ident[EI_DATA] = ELFDATA2LSB;
	header.e_ident[EI_VERSION] = EV_CURRENT;
	header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
	header.e_type = ET_DYNAMIC;
	header.e_machine = EM_X86_64;
	header.e_version = EV_CURRENT;
	header.e_ehsize = sizeof(header);
	header.e_phoff = sizeof(header);
	header.e_phentsize = sizeof(Elf64_Phdr);
	header.e_phnum = 1;
	Elf64_Phdr segment {};
	segment.p_type = PT_LOAD;
	segment.p_flags = PF_R | PF_X;
	segment.p_memsz = 4096;
	Common::File file;
	if (!check(file.Create(source), "create shared ELF fixture")) { return false; }
	uint32_t written_header = 0, written_segment = 0;
	file.Write(&header, sizeof(header), &written_header);
	file.Write(&segment, sizeof(segment), &written_segment);
	file.Close();
	if (!check(written_header == sizeof(header) && written_segment == sizeof(segment),
	           "write shared ELF fixture")) { return false; }
	std::error_code error;
	std::filesystem::create_hard_link(source, alias, error);
	if (!check(!error && std::filesystem::equivalent(source, alias), "create real hard-link alias")) { return false; }
	std::filesystem::copy_file(source, distinct);
	if (!check(!std::filesystem::equivalent(source, distinct), "copy must be a distinct file despite identical bytes")) { return false; }
	std::filesystem::create_hard_link(distinct, pinned_alias, error);
	if (!check(!error, "create pinned-provider alias")) { return false; }

	SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	const auto* load_symbol = symbols.FindByNid("wzvqT4UqKX8", SymbolType::Func);
	const auto* dlsym_symbol = symbols.FindByNid("LwG8g3niqwA", SymbolType::Func);
	if (!check(load_symbol && dlsym_symbol, "resolve production guest loader exports")) { return false; }
	const auto load = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, size_t, const void*, uint32_t, const void*, int*)>(load_symbol->vaddr);
	const auto dlsym = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, const char*, void**)>(dlsym_symbol->vaddr);
	unsigned starts[4] {};
	Elf64_Sym empty_symbol {};
	Elf64_Rela empty_relocation {};
	const auto publish = [&](Program& p, int id, const std::filesystem::path& path, unsigned index,
	                         bool start = true, module_ini_fini_func_t initializer = &RuntimeProgramFileReuseFixture::Initialize) {
		p.rt = &linker;
		p.unique_id = id;
		p.file_name = path;
		p.elf = std::make_unique<Elf64>();
		p.elf->Open(path);
		if (!check(p.elf->IsValid() && p.elf->IsShared(), "parse shared ELF fixture")) { return false; }
		p.base_vaddr = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite, "module_reuse_fixture");
		if (!check(p.base_vaddr != 0, "allocate actual private provider instructions")) { return false; }
		cleanup.mappings.push_back(p.base_vaddr);
		p.base_size = 4096;
		uint8_t code[] {0xb8, 0, 0, 0, 0, 0xc3}; // mov eax, module identity; ret
		std::memcpy(code + 1, &id, sizeof(id));
		std::memcpy(reinterpret_cast<void*>(p.base_vaddr), code, sizeof(code));
		uint8_t init[] {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
		const auto target = reinterpret_cast<uint64_t>(initializer);
		std::memcpy(init + 2, &target, sizeof(target));
		std::memcpy(reinterpret_cast<void*>(p.base_vaddr + 32), init, sizeof(init));
		if (!check(Libs::LibKernel::Memory::ProtectGuestMemory(p.base_vaddr, 4096, Common::VirtualMemory::Mode::ExecuteRead) &&
		               Common::VirtualMemory::FlushInstructionCache(p.base_vaddr, 4096), "seal provider instructions")) { return false; }
		p.dynamic_info = std::make_unique<DynamicInfo>();
		// Reuse now rechecks owner relocation before startup completion. Model
		// valid empty relocation tables instead of absent parser metadata.
		p.dynamic_info->symbol_table_entry_size = sizeof(Elf64_Sym);
		p.dynamic_info->rela_table_entry_size = sizeof(Elf64_Rela);
		p.dynamic_info->symbol_table = &empty_symbol;
		p.dynamic_info->rela_table = p.dynamic_info->jmprela_table = &empty_relocation;
		p.dynamic_info->init_vaddr = 32;
		p.export_symbols = std::make_unique<SymbolDatabase>();
		// Distinct addresses let handle-scoped dlsym detect a provider switch.
		p.export_symbols->Add({"EcTHemmz6n4", "libSceComposite", 1, "libSceComposite", 1, 1, SymbolType::Func},
		                      p.base_vaddr, "rendering-time fixture");
		linker.m_programs.push_back(&p);
		p.load_count = start ? 1 : 0;
		return !start || check(linker.StartModule(&p, sizeof(unsigned), &starts[index], nullptr) == 0 && starts[index] == 1,
		             "production StartModule must invoke each new instance once");
	};
	const auto reuse = [&](Program& p, const char* guest_path, unsigned index) {
		int result = -1;
		const auto before = p.load_count;
		const auto size_before = linker.m_programs.size();
		const auto handle = load(guest_path, sizeof(unsigned), &starts[index], 0, nullptr, &result);
		void* address = nullptr;
		if (!check(handle == p.unique_id && result == 0 && p.load_count == before + 1 &&
		                 linker.m_programs.size() == size_before && starts[index] == 1,
		             "LoadStartModule alias must reuse handle, retain once-only init and increment reference count")) { return false; }
		const auto status = dlsym(handle, "sceCompsoitorGetProcessRenderingTime", &address);
		const auto execution = GetApplicationExecution(GetCurrentProgram());
		if (execution && !p.application_owner) {
			return check(status == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP && !address && !execution->Failure(),
			             "pinned session reference reuse must not grant shared apps unreviewed raw native calls");
		}
		if (!check(status == 0 && address, "SystemMonitor dlsym must resolve from the reused instance")) { return false; }
		if (execution) {
			const auto called = execution->InvokeCallback(reinterpret_cast<uint64_t>(address), nullptr, true);
			return check(reinterpret_cast<uint64_t>(address) != p.base_vaddr && called.Returned() &&
			                 called.call.value == static_cast<uint64_t>(p.unique_id),
			             "shared dlsym must call the reused private provider through its owner-checked entry");
		}
		return check(reinterpret_cast<uint64_t>(address) == p.base_vaddr,
		             "session dlsym must retain the reused provider's direct native address");
	};
	if (!publish(shared, 710001, source, 0)) { return false; }
	if (!check(linker.FindProgramByFileName(alias) == &shared, "null-owner lookup must recognize hard-link alias") ||
	    !reuse(shared, "/module-reuse/staged/libSceComposite.sprx", 0) ||
	    !reuse(shared, "/module-reuse/firmware/libSceComposite.sprx", 0)) { return false; }
	if (!check(linker.FindProgramByFileName(distinct) == nullptr,
	           "same basename and identical bytes must not merge distinct files")) { return false; }
	// A launch manifest can explicitly alias a copied managed dependency to
	// the firmware producer without rewriting the stage or weakening identity.
	Fs::Mount(cleanup.root / "different", "/common_ex/lib");
	Fs::Mount(source, "/common_ex/lib/libSceComposite.sprx");
	if (!reuse(shared, "/common_ex/lib/libSceComposite.sprx", 0) ||
	    !reuse(shared, "/abcdef0123456789/common_ex/lib/libSceComposite.sprx", 0) ||
	    !check(!std::filesystem::equivalent(source, distinct), "mount alias must preserve the copied stage file")) { return false; }
	if (!publish(different, 710002, distinct, 1) ||
	    !reuse(different, "/module-reuse/different/libSceComposite.sprx", 1)) { return false; }
	if (!check(linker.FindProgramByFileName(cleanup.root / "missing/libSceComposite.sprx") == nullptr,
	           "failed identity query must not fall back to basename")) { return false; }
	const auto completion = [&](Program& p, const char* filename, int id, int value) {
		const auto path = cleanup.root / filename;
		std::filesystem::copy_file(source, path);
		if (!publish(p, id, path, 0, false, &RuntimeProgramFileReuseFixture::HeldInitialize)) { return false; }
		RuntimeProgramFileReuseFixture::Pending state {&linker, &p, value};
		int handles[2] {-1, -1}, results[2] {-1, -1};
		std::thread workers[2];
		struct Join {
			RuntimeProgramFileReuseFixture::Pending& state;
			std::thread (&workers)[2];
			~Join() {
				{ std::lock_guard lock(state.mutex); state.release = true; state.changed.notify_all(); }
				for (auto& worker: workers) { if (worker.joinable()) { worker.join(); } }
			}
		} join {state, workers};
		const auto invoke = [&](unsigned index) {
			SetCurrentProgram(nullptr);
			if (index) { std::lock_guard lock(state.mutex); state.second_started = true; state.changed.notify_all(); }
			const auto guest = std::string("/module-reuse/") + filename;
			handles[index] = load(guest.c_str(), sizeof(state), &state, 0, nullptr, &results[index]);
			if (index) { std::lock_guard lock(state.mutex); state.second_finished = true; state.changed.notify_all(); }
		};
		workers[0] = std::thread(invoke, 0);
		bool entered = false, waiting = false, ready_early = false;
		{
			std::unique_lock lock(state.mutex);
			entered = state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.entered; });
		}
		if (entered) {
			workers[1] = std::thread(invoke, 1);
			std::unique_lock lock(state.mutex);
			waiting = state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.second_started; });
			ready_early = state.changed.wait_for(lock, std::chrono::milliseconds(30), [&] { return state.second_finished; });
			std::lock_guard module_lock(p.module_mutex);
			if (!check(!p.started && !p.initializer_returned && p.load_count == 0,
			           "pending producer must not publish successful startup or a load reference")) { return false; }
		}
		{ std::lock_guard lock(state.mutex); state.release = true; state.changed.notify_all(); }
		for (auto& worker: workers) { if (worker.joinable()) { worker.join(); } }
		const int expected = value < 0 ? Libs::LibKernel::KERNEL_ERROR_ENOEXEC : id;
		int replay_result = -1;
		const auto replay = load((std::string("/module-reuse/") + filename).c_str(), sizeof(state), &state, 0, nullptr, &replay_result);
		return check(entered && waiting && !ready_early && state.calls == 1 &&
		             state.nested == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
		             handles[0] == expected && handles[1] == expected && results[0] == value && results[1] == value &&
		             replay == expected && replay_result == value && p.load_count == (value < 0 ? 0u : 3u) &&
		             p.started == (value >= 0) && p.initializer_returned,
		             "load handles must follow once-only native completion, retain its result and reject self-reentry");
	};
	if (!completion(pending_ok, "pending-ok.sprx", 710003, 37) ||
	    !completion(pending_error, "pending-error.sprx", 710004, -123)) { return false; }
	// Finalization closes acquisition before the callback, and cannot be
	// mistaken for cached initialization or another thread's completed stop.
	{
		const auto* stop_symbol = symbols.FindByNid("QKd0qM58Qes", SymbolType::Func);
		if (!check(stop_symbol, "resolve production stop/unload export")) { return false; }
		const auto unload = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, size_t, const void*, uint32_t, const void*, int*)>(stop_symbol->vaddr);
		int ignored = -1;
		if (!check(unload(pending_ok.unique_id, 0, nullptr, 0, nullptr, &ignored) == 0 &&
		           unload(pending_ok.unique_id, 0, nullptr, 0, nullptr, &ignored) == 0 && pending_ok.load_count == 1,
		           "nonfinal releases must retain the producer")) { return false; }
		pending_ok.dynamic_info->fini_vaddr = 32;
		RuntimeProgramFileReuseFixture::Pending state {&linker, &pending_ok, -9};
		bool finalized = false;
		int stop_result = 0;
		std::thread worker([&] { SetCurrentProgram(nullptr); stop_result = linker.StopModule(&pending_ok, sizeof(state), &state, nullptr, &finalized); });
		bool entered;
		{ std::unique_lock lock(state.mutex); entered = state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.entered; }); }
		int load_result = -1;
		const int during = load("/module-reuse/pending-ok.sprx", 0, nullptr, 0, nullptr, &load_result);
		bool competing_finalized = true;
		const int competing = linker.StopModule(&pending_ok, 0, nullptr, nullptr, &competing_finalized);
		{ std::lock_guard lock(state.mutex); state.release = true; state.changed.notify_all(); }
		worker.join();
		const int after = load("/module-reuse/pending-ok.sprx", 0, nullptr, 0, nullptr, &load_result);
		bool replay_finalized = true;
		const int replay_stop = linker.StopModule(&pending_ok, 0, nullptr, nullptr, &replay_finalized);
		if (!check(entered && state.calls == 1 && state.nested == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
		           during == Libs::LibKernel::KERNEL_ERROR_EBUSY && competing == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
		           !competing_finalized && finalized && stop_result == -9 && after == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
		           replay_stop == Libs::LibKernel::KERNEL_ERROR_EBUSY && !replay_finalized &&
		           pending_ok.module_state == ModuleExecutionState::Stopped && !pending_ok.started && pending_ok.load_count == 1,
		           "stopping/stopped producer must not revive, rerun finalizer, or reuse another caller's completion")) { return false; }
	}
	app_a.unique_id = 710010;
	app_a.rt = app_b.rt = &linker;
	app_a.application_owner = &app_a;
	private_a.application_owner = &app_a;
	app_b.unique_id = 710020;
	app_b.application_owner = &app_b;
	private_b.application_owner = &app_b;
	if (!check(InitializeApplicationExecution(&app_a) && InitializeApplicationExecution(&app_b),
	           "initialize shared owner execution lifetimes")) { return false; }
	SetCurrentProgram(&app_a);
	if (!check(linker.FindProgramByFileName(source) == nullptr && linker.FindProgramByFileName(alias) == nullptr,
	           "application must not reuse mutable session state even by exact path")) { return false; }
	if (!publish(private_a, 710011, source, 2)) { return false; }
	SetCurrentProgram(&app_b);
	if (!check(linker.FindProgramByFileName(alias) == nullptr,
	           "application B must not reuse application A or session instance")) { return false; }
	if (!publish(private_b, 710021, alias, 3)) { return false; }
	// Opposite path spelling in each owner; a session provider loaded first
	// must not win over the requesting owner's hard-link-equivalent instance.
	SetCurrentProgram(&private_a);
	if (!check(linker.FindProgramByFileName(alias) == &private_a, "A must select its own instance") ||
	    !reuse(private_a, "/module-reuse/staged/libSceComposite.sprx", 2)) { return false; }
	SetCurrentProgram(&private_b);
	if (!check(linker.FindProgramByFileName(source) == &private_b, "B must select its own instance") ||
	    !reuse(private_b, "/module-reuse/firmware/libSceComposite.sprx", 3)) { return false; }
	SetCurrentProgram(nullptr);
	if (!check(linker.FindProgramByFileName(alias) == &shared, "null owner must still select session instance")) { return false; }
	SetCurrentProgram(&shared);
	if (!check(linker.FindProgramByFileName(alias) == &shared,
	           "a session module requester must retain null-owner reuse behavior")) { return false; }
	// Simulate the explicit pinned provider identities, not a filename heuristic.
	linker.m_ssl_smoke_prepared = true;
	SetCurrentProgram(&app_a);
	for (auto** pinned: {&linker.m_ssl_smoke_libc, &linker.m_ssl_smoke_ssl}) {
		*pinned = &different;
		if (!check(linker.FindProgramByFileName(distinct) == &different &&
		               linker.FindProgramByFileName(pinned_alias) == &different,
		           "explicit pinned session provider must remain reusable by an application") ||
		    !reuse(different, "/module-reuse/different/pinned-alias.sprx", 1)) { return false; }
		*pinned = nullptr;
	}
	if (!check(linker.FindProgramByFileName(distinct) == nullptr,
	           "prepared SSL must not make unrelated session modules globally reusable")) { return false; }
	return true;
}
