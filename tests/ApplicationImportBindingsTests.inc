// Included in runtimeLinker.cpp: actual relocation slots, generated entries,
// private native mappings and the registered production HLE database.
namespace ApplicationImportFixture {
struct Call {
	uint64_t address = 0;
	RuntimeGuestExecution* execution = nullptr;
	bool stop = false;
	unsigned continued = 0;
	RuntimeGuestResult outcome;
	int32_t dynamic_handle = 0;
};
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Invoke(void* argument) {
	auto* call = static_cast<Call*>(argument);
	if (call->stop) { call->execution->RequestStop(); }
	using Function = uint64_t(KYTY_SYSV_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	const auto result = reinterpret_cast<Function>(call->address)(1, 2, 3, 4, 5, 6, 7, 8);
	++call->continued;
	return result;
}
static unsigned unknown_calls = 0;
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV InvokeDlsym(void* argument) {
	auto* call = static_cast<Call*>(argument);
	void* target = nullptr;
	const int status = reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t, const char*, void**)>(call->address)(
	    call->dynamic_handle, "bindnative", &target);
	if (status || !target) { return 0; }
	using Function = uint64_t(KYTY_SYSV_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	const auto result = reinterpret_cast<Function>(target)(1, 2, 3, 4, 5, 6, 7, 8);
	++call->continued;
	return result;
}
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV InvokeScalar(void* argument) {
	auto* call = static_cast<Call*>(argument);
	const auto result = reinterpret_cast<double(KYTY_SYSV_ABI*)(double)>(call->address)(0.5);
	++call->continued;
	uint64_t bits = 0;
	std::memcpy(&bits, &result, sizeof(bits));
	return bits;
}
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV InvokeVarargs(void* argument) {
	auto* call = static_cast<Call*>(argument);
	const int result = reinterpret_cast<int(KYTY_SYSV_ABI*)(const char*, ...)>(call->address)(
	    "gateway %d %d %d %d %d %d %d %.2f\n", 1, 2, 3, 4, 5, 6, 7, 1.25);
	++call->continued;
	return static_cast<uint64_t>(result);
}
static uint64_t KYTY_SYSV_ABI UnknownHost() { ++unknown_calls; return 19; }
static void* KYTY_SYSV_ABI ExitCoordinator(void* argument) {
	auto* call = static_cast<Call*>(argument);
	call->outcome = call->execution->Invoke(Invoke, call);
	return nullptr;
}
}

bool RuntimeLinker::TestApplicationImportBindings(std::string* failure) {
	using namespace ApplicationImportFixture;
	const auto check = [&](bool ok, const char* text) {
		if (!ok && failure) { *failure = text; }
		return ok;
	};
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	Program owners[5], providers[2];
	uint64_t slots[5][5] {};
	Elf64_Sym symbols[5][5] {};
	Elf64_Rela relocations[5][5] {};
	void* dynamic_native[2] {};
	Elf64_Rela empty {};
	struct Cleanup {
		RuntimeLinker& linker;
		std::vector<Program*>& programs;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::filesystem::path root;
		uint64_t code[2] {};
		~Cleanup() {
			RuntimeLinker::SetCurrentProgram(previous);
			for (auto* program: programs) {
				program->elf.reset();
				if (auto execution = GetApplicationExecution(program)) { execution->RequestStop(); }
			}
			programs.clear();
			for (auto address: code) { if (address) { Libs::LibKernel::Memory::FreeGuestMemory(address, 4096); } }
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker, linker.m_programs};
	cleanup.root = std::filesystem::temp_directory_path() /
	    ("kyty_application_imports_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root);
	const auto path = cleanup.root / "importer.sprx";
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
	segment.p_flags = PF_R;
	segment.p_memsz = 4096;
	Common::File file;
	if (!check(file.Create(path), "create import fixture ELF")) { return false; }
	uint32_t written = 0;
	file.Write(&header, sizeof(header), &written);
	file.Write(&segment, sizeof(segment), &written);
	file.Close();
	const auto provider_path = cleanup.root / "provider.sprx";
	segment.p_flags = PF_R | PF_X;
	if (!check(file.Create(provider_path), "create executable provider ELF")) { return false; }
	file.Write(&header, sizeof(header), &written);
	file.Write(&segment, sizeof(segment), &written);
	file.Close();
	const auto image = [&](Program& program, bool executable = false) {
		program.rt = &linker;
		program.file_name = executable ? provider_path : path;
		program.elf = std::make_unique<Elf64>();
		program.elf->Open(program.file_name);
		program.dynamic_info = std::make_unique<DynamicInfo>();
		program.export_symbols = std::make_unique<SymbolDatabase>();
		return program.elf->IsValid();
	};
	Libs::InitLibKernel_1(linker.Symbols());
	Libs::LibC::InitLibC_1(linker.Symbols());
	linker.Symbols()->Add({"unknownhost", "Fixture", 1, "Fixture", 1, 1, SymbolType::Func},
	    reinterpret_cast<uint64_t>(UnknownHost), "Fixture::UnknownHost");
	const char names[] = "bindnative#A#B\0BNowx2l588E#A#B\0unknownhost#A#B\0" "3kg7rT0NQIs#A#B\0"
	    "H8ya2H00jbI#A#B\0hcuQgD53UxM#A#B\0LwG8g3niqwA#A#B\0";
	for (unsigned i = 0; i != 5; ++i) {
		auto& owner = owners[i];
		owner.application_owner = &owner;
		owner.unique_id = 988000 + i;
		if (!check(image(owner) && InitializeApplicationExecution(&owner), "initialize actual importer owner")) { return false; }
		owner.base_vaddr = reinterpret_cast<uint64_t>(&slots[i][0]);
		owner.base_size = sizeof(slots[i]);
		auto& info = *owner.dynamic_info;
		info.str_table = const_cast<char*>(names);
		info.str_table_size = sizeof(names);
		info.import_libs.push_back({"A", 1, "Fixture"});
		info.import_modules.push_back({"B", 1, 1, "Fixture"});
		info.symbol_table = symbols[i];
		info.symbol_table_entry_size = sizeof(Elf64_Sym);
		info.symbol_table_total_size = sizeof(symbols[i]);
		info.rela_table = &empty;
		info.rela_table_entry_size = sizeof(Elf64_Rela);
		info.jmprela_table = relocations[i];
		info.jmprela_table_size = sizeof(relocations[i]);
		for (unsigned j = 0; j != 5; ++j) {
			symbols[i][j].st_info = (STB_GLOBAL << 4) | STT_FUNC;
			symbols[i][j].st_name = j == 0 ? 0 : sizeof("bindnative#A#B");
			relocations[i][j].r_offset = j * sizeof(uint64_t);
			relocations[i][j].r_info = (uint64_t(j) << 32) | R_X86_64_JUMP_SLOT;
		}
		symbols[i][2].st_name = sizeof("bindnative#A#B") + sizeof("BNowx2l588E#A#B") + sizeof("unknownhost#A#B") + sizeof("3kg7rT0NQIs#A#B");
		symbols[i][3].st_name = symbols[i][2].st_name + sizeof("H8ya2H00jbI#A#B");
		symbols[i][4].st_name = symbols[i][3].st_name + sizeof("hcuQgD53UxM#A#B");
		if (i == 3) { symbols[i][0].st_name = sizeof("bindnative#A#B") + sizeof("BNowx2l588E#A#B"); }
		if (i == 4) { symbols[i][0].st_name = sizeof("bindnative#A#B") + sizeof("BNowx2l588E#A#B") + sizeof("unknownhost#A#B"); }
		linker.m_programs.push_back(&owner);
		RuntimeLinker::SetCurrentProgram(&owner);
		RelocateRecord(0, &relocations[i][0], &owner, true);
		RelocateRecord(1, &relocations[i][1], &owner, true);
		RelocateRecord(2, &relocations[i][2], &owner, true);
		RelocateRecord(3, &relocations[i][3], &owner, true);
		RelocateRecord(4, &relocations[i][4], &owner, true);
		if (!check(slots[i][0] && slots[i][1] && owner.application_import_bindings->Published(),
		           "real relocation must publish immutable owner entries for missing and resolved functions")) { return false; }
	}
	if (!check(linker.GetApplicationHardImportBlockers(&owners[2]).empty() &&
	               linker.GetApplicationHardImportBlockers(&owners[3]).empty() &&
	               !linker.GetApplicationImportBlockers(&owners[3]).empty(),
	           "unused missing/unaudited functions have real deferred entries and are not hard ABI blockers")) { return false; }
	const auto saved_entry = slots[2][0];
	slots[2][0] = reinterpret_cast<uint64_t>(UnknownHost);
	const bool unpatched_blocked = !linker.GetApplicationHardImportBlockers(&owners[2]).empty();
	slots[2][0] = saved_entry;
	relocations[2][0].r_info = R_X86_64_64;
	relocations[2][0].r_addend = 1;
	const bool addend_blocked = !linker.GetApplicationHardImportBlockers(&owners[2]).empty();
	relocations[2][0].r_info = R_X86_64_JUMP_SLOT;
	relocations[2][0].r_addend = 0;
	symbols[2][0].st_info = (STB_GLOBAL << 4) | STT_NOTYPE;
	const bool untyped_blocked = !linker.GetApplicationHardImportBlockers(&owners[2]).empty();
	symbols[2][0].st_info = (STB_GLOBAL << 4) | STT_OBJECT;
	const bool data_blocked = !linker.GetApplicationHardImportBlockers(&owners[2]).empty();
	symbols[2][0].st_info = (STB_GLOBAL << 4) | STT_FUNC;
	if (!check(unpatched_blocked && addend_blocked && untyped_blocked && data_blocked &&
	               linker.GetApplicationHardImportBlockers(&owners[2]).empty(),
	           "hard admission rejects raw/tampered entries, function addends, untyped symbols and missing data")) { return false; }
	// Private providers appear after the relocation but before the first call.
	const uint8_t sum[] {0x48,0x89,0xf8,0x48,0x01,0xf0,0x48,0x01,0xd0,0x48,0x01,0xc8,
	    0x4c,0x01,0xc0,0x4c,0x01,0xc8,0x48,0x03,0x44,0x24,0x08,0x48,0x03,0x44,0x24,0x10,
	    0x48,0x83,0xc0,0x00,0xc3};
	for (unsigned i = 0; i != 2; ++i) {
		auto& provider = providers[i];
		if (!check(image(provider, true), "parse private provider ELF")) { return false; }
		cleanup.code[i] = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite, "application_import_fixture");
		if (!check(cleanup.code[i] != 0, "allocate private provider code")) { return false; }
		std::memcpy(reinterpret_cast<void*>(cleanup.code[i]), sum, sizeof(sum));
		reinterpret_cast<uint8_t*>(cleanup.code[i])[31] = i == 0 ? 0 : 100;
		if (!check(Libs::LibKernel::Memory::ProtectGuestMemory(cleanup.code[i], 4096, Common::VirtualMemory::Mode::ExecuteRead),
		           "protect private provider executable code")) { return false; }
		Common::VirtualMemory::FlushInstructionCache(cleanup.code[i], 4096);
		provider.base_vaddr = cleanup.code[i];
		provider.base_size = 4096;
		provider.application_owner = &owners[i];
		provider.unique_id = 989000 + i;
		provider.export_symbols->Add({"bindnative", "Fixture", 1, "Fixture", 1, 1, SymbolType::Func}, cleanup.code[i]);
		Libs::InitLibKernel_1(provider.export_symbols.get());
		provider.export_symbols->Add({"unknownhost", "Fixture", 1, "Fixture", 1, 1, SymbolType::Func},
		    reinterpret_cast<uint64_t>(UnknownHost), "Fixture::UnknownHost");
		provider.export_symbols->Add({"privateobject", "Fixture", 1, "Fixture", 1, 1, SymbolType::Object}, cleanup.code[i] + 512);
		provider.export_symbols->Add({"ambiguous", "Fixture", 1, "Fixture", 1, 1, SymbolType::NoType}, cleanup.code[i]);
		linker.m_programs.push_back(&provider);
	}
	RuntimeLinker::SetCurrentProgram(&owners[0]);
	providers[0].dynamic_info->export_libs.push_back({"K", 1, "libkernel"});
	const bool kernel_metadata_blocked = !linker.IsApplicationExecutableAddress(&owners[0], cleanup.code[0]) &&
	    !linker.GetApplicationHardImportBlockers(&owners[0]).empty() && !linker.PublishApplicationStopText(&owners[0]);
	providers[0].dynamic_info->export_libs.clear();
	if (!check(kernel_metadata_blocked && linker.LoadProgram(cleanup.root / "libkernel-refused.sprx") == nullptr &&
	               !owners[0].application_execution->Failure(),
	           "renamed native kernel exports and explicit kernel paths must not bypass shared provider policy")) { return false; }
	for (unsigned i = 0; i != 2; ++i) {
		RuntimeLinker::SetCurrentProgram(&owners[i]);
		if (!check(linker.IsApplicationExecutableAddress(&owners[i], cleanup.code[i]) &&
		               !linker.IsApplicationExecutableAddress(&owners[i], owners[i].base_vaddr) &&
		               !linker.IsApplicationExecutableAddress(&owners[i], cleanup.code[1 - i]),
		           "native dispatch requires this owner's executable ELF text")) { return false; }
		if (!check(linker.PublishApplicationStopText(&owners[i]), "publish actual private ELF executable ranges")) { return false; }
		const auto text = owners[i].application_execution->StopText();
		if (!check(text && text->ranges.size() == 1 && text->Contains(cleanup.code[i]) &&
		               text->Contains(cleanup.code[i] + 4095) && !text->Contains(cleanup.code[i] + 4096) &&
		               !text->Contains(cleanup.code[1 - i]) && !text->Contains(slots[i][0]) &&
		               !text->Contains(owners[i].base_vaddr),
		           "stop proof must exclude non-executable data, other owners and generated host import thunks")) { return false; }
		providers[i].base_size = 2048;
		const bool invalid_published = linker.PublishApplicationStopText(&owners[i]);
		providers[i].base_size = 4096;
		if (!check(!invalid_published && owners[i].application_execution->StopText() == text,
		           "out-of-mapping executable spans must not replace an existing valid stop proof")) { return false; }
		Call call {slots[i][0], owners[i].application_execution.get()};
		const auto first = call.execution->Invoke(Invoke, &call);
		const auto second = call.execution->Invoke(Invoke, &call);
		if (!check(first.Returned() && second.Returned() && first.call.value == 36 + i * 100 &&
		               second.call.value == first.call.value && call.continued == 2 &&
		               linker.GetApplicationImportBlockers(&owners[i]).empty(),
		           "first actual deferred invocation must preserve register/stack args and private-owner resolution")) { return false; }
		call.address = slots[i][1];
		const auto* registered = linker.Symbols()->FindByNid("BNowx2l588E", SymbolType::Func);
		const auto expected = reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)()>(registered->vaddr)();
		const auto host = call.execution->Invoke(Invoke, &call);
		if (!check(host.Returned() && host.call.value == expected && expected != 0 &&
		               HostCallReturnThunk::CurrentDepth() == 0 && call.execution->Outstanding() == 0,
		           "production registered HLE must return through its actual owner gateway with balanced leases")) { return false; }
		call.address = slots[i][2];
		const auto* sine = linker.Symbols()->FindByNid("H8ya2H00jbI", SymbolType::Func);
		const auto sine_expected = reinterpret_cast<double(KYTY_SYSV_ABI*)(double)>(sine->vaddr)(0.5);
		uint64_t sine_bits = 0;
		std::memcpy(&sine_bits, &sine_expected, sizeof(sine_bits));
		const auto scalar = call.execution->Invoke(InvokeScalar, &call);
		call.address = slots[i][3];
		const auto varargs = call.execution->Invoke(InvokeVarargs, &call);
		if (!check(scalar.Returned() && scalar.call.value == sine_bits && varargs.Returned() &&
		               varargs.call.value == std::strlen("gateway 1 2 3 4 5 6 7 1.25\n") &&
		               call.execution->Outstanding() == 0 && HostCallReturnThunk::CurrentDepth() == 0,
		           "registered scalar math and printf imports must preserve XMM arguments/results, varargs AL and overflow arguments")) { return false; }
		using namespace Libs::LibKernel;
		void* repeated = nullptr;
		if (!check(linker.ResolveApplicationDlsym(&providers[i], "bindnative", "not-a-matching-hash", &dynamic_native[i]) == OK &&
		               linker.ResolveApplicationDlsym(&providers[i], "ignored", "bindnative", &repeated) == OK &&
		               repeated == dynamic_native[i] && reinterpret_cast<uint64_t>(repeated) != cleanup.code[i],
		           "dynamic name/NID lookup must publish a stable owner-checked entry rather than a raw private address")) { return false; }
		call.address = reinterpret_cast<uint64_t>(dynamic_native[i]);
		const auto dynamic_result = call.execution->Invoke(Invoke, &call);
		void* dynamic_host = nullptr;
		if (!check(dynamic_result.Returned() && dynamic_result.call.value == 36 + i * 100 &&
		               linker.ResolveApplicationDlsym(&providers[i], "ignored", "BNowx2l588E", &dynamic_host) == OK &&
		               reinterpret_cast<uint64_t>(dynamic_host) != registered->vaddr,
		           "dynamic native lookup must retain owner isolation and wrap registered HLE targets")) { return false; }
		call.address = reinterpret_cast<uint64_t>(dynamic_host);
		const auto dynamic_hle = call.execution->Invoke(Invoke, &call);
		void* object = nullptr;
		if (!check(dynamic_hle.Returned() && dynamic_hle.call.value == expected &&
		               linker.ResolveApplicationDlsym(&providers[i], "ignored", "privateobject", &object) == OK &&
		               reinterpret_cast<uint64_t>(object) == cleanup.code[i] + 512,
		           "dynamic HLE must preserve results while private object lookup retains its data address")) { return false; }
		for (const auto* denied: {"unknownhost", "ambiguous"}) {
			void* output = reinterpret_cast<void*>(1);
			if (!check(linker.ResolveApplicationDlsym(&providers[i], "ignored", denied, &output) == KERNEL_ERROR_EOPNOTSUPP &&
			               output == nullptr && !call.execution->Failure(),
			           "optional dynamic probes must reject unaudited/ambiguous targets without failing the owner")) { return false; }
		}
		void* foreign = reinterpret_cast<void*>(1);
		if (!check(linker.ResolveApplicationDlsym(&providers[1 - i], "ignored", "bindnative", &foreign) == KERNEL_ERROR_EOPNOTSUPP &&
		               foreign == nullptr && !call.execution->Failure(),
		           "an explicit foreign module handle must not leak another owner's dynamic function")) { return false; }
		call.address = slots[i][4];
		call.dynamic_handle = providers[i].unique_id;
		const auto looked_up = call.execution->Invoke(InvokeDlsym, &call);
		if (!check(looked_up.Returned() && looked_up.call.value == 36 + i * 100 &&
		               call.execution->Outstanding() == 0 && HostCallReturnThunk::CurrentDepth() == 0,
		           "actual KernelDlsym import must hash/name-resolve and return a callable scoped pointer in the same guest invocation")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&owners[0]);
	providers[1].application_owner = nullptr;
	void* borrowed = reinterpret_cast<void*>(1);
	const auto borrowed_result = linker.ResolveApplicationDlsym(&providers[1], "ignored", "bindnative", &borrowed);
	providers[1].application_owner = &owners[1];
	if (!check(borrowed_result == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP && borrowed == nullptr,
	           "session-native dlsym providers require their own transition contract")) { return false; }
	for (unsigned i = 2; i != 4; ++i) {
		RuntimeLinker::SetCurrentProgram(&owners[i]);
		Call call {slots[i][0], owners[i].application_execution.get()};
		if (!check(!linker.GetApplicationImportBlockers(&owners[i]).empty(), "unsafe import must remain an admission blocker")) { return false; }
		const auto result = call.execution->Invoke(Invoke, &call);
		if (!check(result.call.tag == GuestCallOutcomeTag::Aborted && call.continued == 0 &&
		               call.execution->Failure() && call.execution->Outstanding() == 0 && unknown_calls == 0 &&
		               !owners[0].application_execution->Failure(),
		           "unresolved or unaudited target must fail only its owner, never return zero or invoke another owner's provider")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&owners[4]);
	Call exiting {slots[4][0], owners[4].application_execution.get()};
	Libs::LibKernel::Pthread exit_thread = nullptr;
	if (!check(linker.GetApplicationImportBlockers(&owners[4]).empty() &&
	               Libs::LibKernel::PthreadCreateRuntimeCoordinator(&exit_thread, nullptr, ExitCoordinator, &exiting, "import-exit-fixture") == 0,
	           "admit and create owner coordinator for the registered scoped pthread-exit adapter")) { return false; }
	void* exit_value = nullptr;
	if (!check(Libs::LibKernel::PthreadJoin(exit_thread, &exit_value) == 0 && exit_value == reinterpret_cast<void*>(1) &&
	               exiting.outcome.ThreadExited() && exiting.continued == 0 && !exiting.execution->Failure() &&
	               exiting.execution->Outstanding() == 0,
	           "production import thunk and HLE gateway must propagate ordinary pthread exit to real worker cleanup")) { return false; }
	RuntimeLinker::SetCurrentProgram(&owners[0]);
	Call stopped {slots[0][0], owners[0].application_execution.get(), true};
	const auto stopped_result = stopped.execution->Invoke(Invoke, &stopped);
	if (!check(stopped_result.call.tag == GuestCallOutcomeTag::Aborted && stopped.continued == 0 &&
	                 stopped_result.stopped && !stopped.execution->Failure() &&
	                 stopped.execution->Outstanding() == 0 && GuestCallBoundary::CurrentDepth() == 0 &&
	                 HostCallReturnThunk::CurrentDepth() == 0,
	             "cached native target must still honor owner stop before dispatch and leave clean transition TLS")) { return false; }
	RuntimeLinker::SetCurrentProgram(&owners[1]);
	Call dynamic_stopped {reinterpret_cast<uint64_t>(dynamic_native[1]), owners[1].application_execution.get(), true};
	const auto dynamic_stop = dynamic_stopped.execution->Invoke(Invoke, &dynamic_stopped);
	void* output = reinterpret_cast<void*>(1);
	return check(dynamic_stop.stopped && dynamic_stopped.continued == 0 && !dynamic_stopped.execution->Failure() &&
	                 dynamic_stopped.execution->Outstanding() == 0 &&
	                 linker.ResolveApplicationDlsym(&providers[1], "ignored", "bindnative", &output) == Libs::LibKernel::KERNEL_ERROR_ECANCELED &&
	                 output == nullptr,
	             "cached dynamic pointers and new lookup publication must both honor owner stop");
}
