// Included inside Loader by runtimeLinker.cpp in the CPU fixture build.
// Actual tiny shared ELFs are mapped by LoadProgram; no file-backed guest code
// or initializer is executed. Owner executables and relocation probes are synthetic.
bool RuntimeLinker::TestAppLocalPrograms(std::string* failure) {
	RuntimeLinker linker;
	Program app_a, app_b, shell;
	struct Cleanup {
		RuntimeLinker& linker;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::vector<Program*> stack;
		std::filesystem::path root;
		~Cleanup() {
			RuntimeLinker::SetCurrentProgram(previous);
			auto& programs = linker.m_programs;
			for (auto* p: programs) {
				if (std::find(stack.begin(), stack.end(), p) == stack.end()) { RuntimeLinker::DeleteProgram(p); }
			}
			programs.clear();
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker, GetCurrentProgram(), {&app_a, &app_b, &shell}, std::filesystem::temp_directory_path() /
	    ("kyty_prepare_local_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
	const auto check = [&](bool ok, const std::string& message) {
		if (!ok && failure != nullptr) { *failure = message; }
		return ok;
	};
	const auto write = [&](const char* basename, const char* needed, bool shared = true) {
		const auto path = cleanup.root / basename;
		std::filesystem::create_directories(path.parent_path());
		std::array<uint8_t, 0x600> bytes {};
		Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\177ELF", 4);
		header.e_ident[EI_CLASS] = ELFCLASS64;
		header.e_ident[EI_DATA] = ELFDATA2LSB;
		header.e_ident[EI_VERSION] = EV_CURRENT;
		header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
		header.e_type = shared ? ET_DYNAMIC : ET_DYNEXEC;
		header.e_machine = EM_X86_64;
		header.e_version = EV_CURRENT;
		header.e_ehsize = sizeof(header);
		header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Elf64_Phdr);
		header.e_phnum = 3;
		std::array<Elf64_Phdr, 3> ph {};
		ph[0].p_type = PT_LOAD; ph[0].p_flags = PF_R | PF_W;
		ph[0].p_filesz = bytes.size(); ph[0].p_memsz = 0x4000; ph[0].p_align = 0x4000;
		ph[1].p_type = PT_DYNAMIC; ph[1].p_offset = 0x100;
		ph[2].p_type = PT_OS_DYNLIBDATA; ph[2].p_offset = 0x300; ph[2].p_filesz = 0x300;
		std::vector<Elf64_Dyn> dyn;
		const auto value = [&](Elf64_Sxword tag, uint64_t v) { Elf64_Dyn d {}; d.d_tag = tag; d.d_un.d_val = v; dyn.push_back(d); };
		value(DT_OS_STRTAB, 0); value(DT_OS_STRSZ, 0x100);
		value(DT_OS_SYMTAB, 0x100); value(DT_OS_SYMTABSZ, sizeof(Elf64_Sym)); value(DT_OS_SYMENT, sizeof(Elf64_Sym));
		value(DT_OS_PLTREL, DT_RELA); value(DT_OS_JMPREL, 0x200); value(DT_OS_RELA, 0x200); value(DT_OS_RELAENT, sizeof(Elf64_Rela));
		value(DT_INIT, 0x80); // Deliberately non-code: preparation MUST NOT call it.
		if (needed != nullptr) { value(DT_NEEDED, 1); std::memcpy(bytes.data() + 0x301, needed, std::strlen(needed) + 1); }
		value(DT_NULL, 0);
		ph[1].p_filesz = dyn.size() * sizeof(Elf64_Dyn);
		std::memcpy(bytes.data(), &header, sizeof(header));
		std::memcpy(bytes.data() + sizeof(header), ph.data(), sizeof(ph));
		std::memcpy(bytes.data() + 0x100, dyn.data(), ph[1].p_filesz);
		Common::File file;
		if (!file.Create(path)) { return false; }
		uint32_t written = 0;
		file.Write(bytes.data(), bytes.size(), &written);
		return written == bytes.size();
	};
	if (!check(write("eboot.bin", nullptr, false) && write("lib/provider.prx", "leaf.prx") && write("lib/leaf.prx", "provider.prx") &&
	           write("sce_module/adjacent.sprx", nullptr) && write("lib/unrequested.prx", nullptr) &&
	           write("assets/asset.prx", nullptr) && write("lib/notshared.prx", nullptr, false), "write ELF fixtures")) { return false; }
	std::filesystem::create_hard_link(cleanup.root / "lib/provider.prx", cleanup.root / "lib/alias.prx");
	// A mutable session instance at the SAME file must not satisfy A or B's map.
	SetCurrentProgram(nullptr);
	Program* session_loads[2] {};
	std::atomic<unsigned> ready {0};
	std::atomic<bool> begin {false};
	const auto load_session = [&](unsigned index, const char* name) {
		SetCurrentProgram(nullptr);
		ready.fetch_add(1);
		while (!begin.load()) { std::this_thread::yield(); }
		session_loads[index] = linker.LoadProgram(cleanup.root / name);
	};
	std::thread first(load_session, 0, "lib/provider.prx");
	std::thread second(load_session, 1, "lib/alias.prx");
	while (ready.load() != 2) { std::this_thread::yield(); }
	begin.store(true);
	first.join();
	second.join();
	auto* session = session_loads[0];
	if (!check(session && session == session_loads[1] && linker.m_programs.size() == 1 &&
	           linker.LoadProgram(cleanup.root / "lib/provider.prx") == session &&
	           !session->started && session->load_count == 0,
	           "concurrent boot-session loads must publish one producer without starting or acquiring it")) { return false; }
	for (auto* app: {&app_a, &app_b, &shell}) {
		app->rt = &linker;
		app->file_name = cleanup.root / (app == &shell ? "shell.bin" : "eboot.bin");
		app->elf = std::make_unique<Elf64>();
		app->elf->Open(cleanup.root / "eboot.bin");
		app->dynamic_info = std::make_unique<DynamicInfo>(*session->dynamic_info);
		app->dynamic_info->needed = {"provider.prx", "alias.prx", "libkernel.prx", "hle-only.prx"};
		app->export_symbols = std::make_unique<SymbolDatabase>();
		linker.m_programs.push_back(app);
	}
	app_a.application_owner = &app_a;
	app_b.application_owner = &app_b;
	std::vector<Program*> prepared;
	std::string error;
	// Prove owner/root are explicit, not inferred from TLS or m_programs[0].
	SetCurrentProgram(&app_b);
	if (!check(linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::AdjacentAndNeeded, &prepared, &error), error)) { return false; }
	if (!check(GetCurrentProgram() == &app_b && prepared.size() == 3, "scope restoration, adjacent semantics, cycle/hardlink dedup or dependency-only lib/ failed")) { return false; }
	Program *provider_a = nullptr, *leaf_a = nullptr;
	for (auto* p: prepared) {
		if (!check(p->application_owner == &app_a && p != session && !p->started && p->load_count == 0,
		           "prepared module borrowed foreign state, started guest code or acquired a guest load reference")) { return false; }
		if (p->file_name.filename() == "provider.prx") { provider_a = p; }
		if (p->file_name.filename() == "leaf.prx") { leaf_a = p; }
	}
	if (!check(provider_a && leaf_a, "transitive dependency missing")) { return false; }
	const auto count = linker.m_programs.size();
	if (!check(linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::AdjacentAndNeeded, &prepared, &error) && linker.m_programs.size() == count,
	           "repeated preparation must reuse owner instances")) { return false; }
	if (!check(linker.PrepareAppLocalPrograms(&app_b, cleanup.root, AppLocalProgramPolicy::NeededOnly, &prepared, &error) && prepared.size() == 2 &&
	           std::all_of(prepared.begin(), prepared.end(), [&](auto* p) { return p->application_owner == &app_b && p != provider_a && p != leaf_a && p != session; }),
	           "B borrowed A/session or needed-only preparation eagerly scanned adjacent modules")) { return false; }
	SetCurrentProgram(&app_a);
	if (!check(linker.FindProgramByFileName(cleanup.root / "lib/alias.prx") == provider_a, "owner hardlink lookup changed")) { return false; }
	app_a.dynamic_info->needed = {"../lib/provider.prx"};
	if (!check(!linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::NeededOnly, &prepared, &error), "traversal accepted")) { return false; }
	app_a.dynamic_info->needed = {"notshared.prx"};
	if (!check(!linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::NeededOnly, &prepared, &error), "non-shared ELF accepted")) { return false; }
	{
		Common::File invalid;
		if (!check(invalid.Create(cleanup.root / "lib/invalid.prx"), "create invalid ELF")) { return false; }
		std::array<uint8_t, 0x80> invalid_bytes {};
		invalid.Write(invalid_bytes.data(), invalid_bytes.size());
	}
	app_a.dynamic_info->needed = {"invalid.prx"};
	if (!check(!linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::NeededOnly, &prepared, &error), "invalid ELF accepted")) { return false; }
	app_a.dynamic_info->needed_valid = false;
	if (!check(!linker.PrepareAppLocalPrograms(&app_a, cleanup.root, AppLocalProgramPolicy::NeededOnly, &prepared, &error), "invalid DT_NEEDED reference accepted")) { return false; }
	app_a.dynamic_info->needed_valid = true;
	app_a.dynamic_info->needed = {"provider.prx"};
	// A newly mapped leaf's strong import participates in complete admission.
	std::array<Elf64_Sym, 2> symbols {};
	symbols[1].st_name = 1; symbols[1].st_info = (STB_GLOBAL << 4) | STT_FUNC;
	char names[] = "\0MissingLeaf#A#A";
	Elf64_Rela relocation {};
	relocation.r_info = (uint64_t {1} << 32) | R_X86_64_JUMP_SLOT;
	relocation.r_offset = 0x2000;
	auto& info = *leaf_a->dynamic_info;
	info.str_table = names; info.str_table_size = sizeof(names);
	info.symbol_table = symbols.data(); info.symbol_table_total_size = sizeof(symbols);
	info.jmprela_table = &relocation; info.jmprela_table_size = sizeof(relocation);
	info.import_modules = {{"A", 1, 1, "LeafModule"}};
	info.import_libs = {{"A", 1, "LeafLibrary"}};
	leaf_a->rela_bits.assign(1, 0);
	for (auto* p: linker.m_programs) {
		if (p->application_owner == &app_b && p->file_name.filename() == "provider.prx") {
			p->export_symbols->Add({"MissingLeaf", "LeafLibrary", 1, "LeafModule", 1, 1, SymbolType::Func}, p->base_vaddr + 0x80);
		}
	}
	if (!check(linker.Symbols()->FindByNid("MissingLeaf", SymbolType::Func) == nullptr, "private export published globally")) { return false; }
	uint64_t shell_slot = 0xfeedfacecafebeefull;
	Elf64_Rela shell_relocation = relocation;
	shell_relocation.r_offset = reinterpret_cast<uint64_t>(&shell_slot);
	*shell.dynamic_info = info;
	shell.dynamic_info->jmprela_table = &shell_relocation;
	shell.rela_bits.assign(1, 0);
	linker.RelocateProgram(&app_a);
	if (!check(shell_slot == 0xfeedfacecafebeefull && shell.rela_bits[0] == 0, "owned preparation changed live session relocation")) { return false; }
	if (!check(linker.GetUnresolvedApplicationImports(&app_a) == std::vector<std::string> {"leaf.prx: MissingLeaf#A#A"},
	           "transitive strong import escaped admission")) { return false; }
	for (auto* p: linker.m_programs) {
		if (!check(!p->started, "map/relocate/admission started an initializer")) { return false; }
	}
	return true;
}
