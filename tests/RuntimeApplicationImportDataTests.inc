// Included by runtimeLinker.cpp inside Loader. Actual LIB_OBJECT registrations,
// relocation writes, hard checker and registered KernelDlsym; no guest code run.
namespace ApplicationImportDataFixture {
static uint32_t unknown_host_object = 17;
constexpr std::array<const char*, 4> Nids {"f7uOxY9mM1U", "djxxOmW6-aw", "P330P3dFF68", "ZT4ODD2Ts9o"};
constexpr std::array<uint64_t, 4> Sizes {8, 8, 4, 4};
template <typename T> static T Read(uint64_t address) {
	T value {};
	std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	return value;
}
}

bool RuntimeLinker::TestRuntimeApplicationImportData(std::string* failure) {
	using namespace ApplicationImportDataFixture;
	using namespace Libs::LibKernel;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	Program programs[7]; // owners A/B, private A/B, session, relaunch A, unsupported importer
	Elf64_Sym symbols[7][4] {};
	Elf64_Rela relocations[7][4] {};
	struct Cleanup {
		RuntimeLinker& linker;
		Program* programs;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::vector<Program*> original;
		std::filesystem::path root;
		~Cleanup() {
			RuntimeLinker::SetCurrentProgram(previous);
			for (unsigned i = 0; i != 7; ++i) {
				if (auto execution = GetApplicationExecution(&programs[i])) { execution->RequestStop(); }
			}
			linker.m_programs = std::move(original);
			for (unsigned i = 0; i != 7; ++i) {
				if (programs[i].base_vaddr) { Memory::FreeGuestMemory(programs[i].base_vaddr, ApplicationImportData::MappingSize); }
				programs[i].elf.reset();
			}
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker, programs};
	cleanup.original = linker.m_programs;
	cleanup.root = std::filesystem::temp_directory_path() /
	    ("kyty_application_data_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root);
	Libs::InitLibKernel_1(linker.Symbols());
	Libs::LibC::InitLibC_1(linker.Symbols()); // also installs LibcInternal's distinct flag
	const char names[] = "f7uOxY9mM1U#K#K\0djxxOmW6-aw#K#K\0P330P3dFF68#C#C\0ZT4ODD2Ts9o#I#I\0appDataUnknown#K#K\0";
	const std::array<uint32_t, 5> offsets {0, sizeof("f7uOxY9mM1U#K#K"),
	    sizeof("f7uOxY9mM1U#K#K") + sizeof("djxxOmW6-aw#K#K"),
	    sizeof("f7uOxY9mM1U#K#K") + sizeof("djxxOmW6-aw#K#K") + sizeof("P330P3dFF68#C#C"),
	    sizeof("f7uOxY9mM1U#K#K") + sizeof("djxxOmW6-aw#K#K") + sizeof("P330P3dFF68#C#C") + sizeof("ZT4ODD2Ts9o#I#I")};
	const std::array<const char*, 7> filenames {"ApplicationA.bin", "ApplicationB.bin", "privateA.prx", "privateB.prx",
	    "ShellSession.bin", "ApplicationA.bin", "unsupported.prx"};
	for (unsigned i = 0; i != 7; ++i) {
		auto& program = programs[i];
		program.rt = &linker;
		program.unique_id = 989500 + i;
		program.application_owner = i == 0 || i == 1 || i == 5 ? &program :
		    i == 2 || i == 6 ? &programs[0] : i == 3 ? &programs[1] : nullptr;
		program.file_name = cleanup.root / filenames[i];
		Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\177ELF", 4);
		header.e_ident[EI_CLASS] = ELFCLASS64; header.e_ident[EI_DATA] = ELFDATA2LSB;
		header.e_ident[EI_VERSION] = EV_CURRENT; header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
		header.e_type = ET_DYNAMIC; header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
		header.e_ehsize = sizeof(header); header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 1;
		Elf64_Phdr segment {};
		segment.p_type = PT_LOAD; segment.p_flags = PF_R | PF_W;
		segment.p_memsz = ApplicationImportData::MappingSize;
		if (!Common::File::IsFileExisting(program.file_name)) {
			Common::File file;
			if (!check(file.Create(program.file_name), "create data relocation fixture ELF")) { return false; }
			uint32_t written = 0;
			file.Write(&header, sizeof(header), &written); file.Write(&segment, sizeof(segment), &written); file.Close();
		}
		program.elf = std::make_unique<Elf64>(); program.elf->Open(program.file_name);
		if (!check(program.elf->IsValid(), "parse real fixture ELF mapping metadata")) { return false; }
		program.base_vaddr = Memory::AllocateRuntimeMemory(0, ApplicationImportData::MappingSize,
		    Common::VirtualMemory::Mode::ReadWrite, "data_importer_fixture");
		program.base_size = program.mapped_size = ApplicationImportData::MappingSize;
		if (!check(program.base_vaddr != 0, "allocate real guest relocation slots")) { return false; }
		std::memset(reinterpret_cast<void*>(program.base_vaddr), 0xff, 8 * sizeof(uint64_t));
		program.export_symbols = std::make_unique<SymbolDatabase>();
		program.dynamic_info = std::make_unique<DynamicInfo>();
		auto& info = *program.dynamic_info;
		info.str_table = const_cast<char*>(names); info.str_table_size = sizeof(names);
		info.import_libs = {{"K", 1, "libkernel"}, {"C", 1, "libc"}, {"I", 1, "LibcInternal"}};
		info.import_modules = {{"K", 1, 1, "libkernel"}, {"C", 1, 1, "libc"}, {"I", 1, 1, "LibcInternal"}};
		info.symbol_table = symbols[i]; info.symbol_table_entry_size = sizeof(Elf64_Sym);
		info.symbol_table_total_size = sizeof(symbols[i]);
		info.rela_table = relocations[i]; info.rela_table_entry_size = sizeof(Elf64_Rela);
		info.rela_table_total_size = sizeof(relocations[i]);
		for (unsigned j = 0; j != 4; ++j) {
			symbols[i][j].st_name = offsets[j];
			symbols[i][j].st_info = ((i == 3 && j == 3 ? STB_WEAK : STB_GLOBAL) << 4) | STT_OBJECT;
			symbols[i][j].st_size = Sizes[j];
			relocations[i][j].r_offset = j * sizeof(uint64_t);
			relocations[i][j].r_info = (uint64_t(j) << 32) | (j < 2 ? R_X86_64_GLOB_DAT : R_X86_64_64);
		}
		if (program.application_owner == &program &&
		    !check(InitializeApplicationExecution(&program), "initialize actual data owner execution")) { return false; }
		// Add the unsupported importer only after proving ordinary hard admission.
		if (i != 6) { linker.m_programs.push_back(&program); }
	}
	Libs::InitLibKernel_1(programs[4].export_symbols.get());
	Libs::LibC::InitLibC_1(programs[4].export_symbols.get());
	std::array<SymbolRecord, 4> host;
	for (unsigned j = 0; j != 4; ++j) {
		const auto* registered = linker.Symbols()->FindByNid(Nids[j], SymbolType::Object);
		if (!check(registered != nullptr, "production object registration must exist")) { return false; }
		host[j] = *registered;
	}
	const auto session_guard = Read<uint64_t>(host[0].vaddr);
	const auto session_libc = Read<uint32_t>(host[2].vaddr);
	const auto session_internal = Read<uint32_t>(host[3].vaddr);
	const auto session_name_pointer = Read<uint64_t>(host[1].vaddr);
	std::array<char, ApplicationImportData::ProgramNameCapacity> session_name {};
	std::memcpy(session_name.data(), reinterpret_cast<const void*>(session_name_pointer), session_name.size());
	const auto* lookup = linker.Symbols()->FindByNid("LwG8g3niqwA", SymbolType::Func);
	if (!check(lookup != nullptr, "production KernelDlsym registration must exist")) { return false; }
	const auto dlsym = reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t, const char*, void**)>(lookup->vaddr);
	uint64_t pointers[6][4] {};
	for (unsigned i = 0; i != 5; ++i) {
		RuntimeLinker::SetCurrentProgram(&programs[i]);
		for (unsigned j = 0; j != 4; ++j) {
			if (!check(RelocateRecord(j, &relocations[i][j], &programs[i], false), "production object relocation must resolve")) { return false; }
			pointers[i][j] = Read<uint64_t>(programs[i].base_vaddr + j * sizeof(uint64_t));
			void* dynamic = nullptr;
			if (!check(dlsym(programs[4].unique_id, Nids[j], &dynamic) == OK &&
			               reinterpret_cast<uint64_t>(dynamic) == pointers[i][j],
			           "registered KernelDlsym must agree with actual data relocation (including session path)")) { return false; }
			if (!check(i == 4 ? pointers[i][j] == host[j].vaddr : pointers[i][j] != host[j].vaddr,
			           "only the session may retain the original registered host object address")) { return false; }
		}
	}
	for (unsigned j = 0; j != 4; ++j) {
		if (!check(pointers[0][j] == pointers[2][j] && pointers[1][j] == pointers[3][j] && pointers[0][j] != pointers[1][j],
		           "private modules must share their owner's objects, never another owner's cells")) { return false; }
	}
	for (unsigned i: {0u, 1u}) {
		if (!check(Read<uint64_t>(pointers[i][0]) == 0xDeadBeef00000007ULL &&
		               Read<uint32_t>(pointers[i][2]) == 1 && Read<uint32_t>(pointers[i][3]) == 1 && pointers[i][2] != pointers[i][3],
		           "owner guard must use legacy constant and libc/internal flags must be distinct uint32_t ones")) { return false; }
		const auto name = Read<uint64_t>(pointers[i][1]);
		if (!check(std::strcmp(reinterpret_cast<const char*>(name), filenames[i]) == 0 && name != session_name_pointer,
		           "progname must be a pinned guest pointer cell to the executable filename, not a private module or shell buffer")) { return false; }
		void* begin = nullptr; void* end = nullptr; int prot = -1;
		if (!check(Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(pointers[i][0]), &begin, &end, &prot) == OK && prot == 3 &&
		               name >= reinterpret_cast<uint64_t>(begin) && name + ApplicationImportData::ProgramNameCapacity <= reinterpret_cast<uint64_t>(end) &&
		               !linker.IsApplicationExecutableAddress(&programs[i], pointers[i][0]) &&
		               linker.GetApplicationHardImportBlockers(&programs[i]).empty(),
		           "audited objects must reside in private RW/non-executable guest storage and pass the real hard gate")) { return false; }
	}
	*reinterpret_cast<uint32_t*>(pointers[2][2]) = 0x12345678;
	*reinterpret_cast<uint32_t*>(pointers[2][3]) = 0x87654321;
	*reinterpret_cast<uint64_t*>(pointers[2][0]) = 0x1111222233334444ULL;
	*reinterpret_cast<char*>(Read<uint64_t>(pointers[2][1])) = 'X';
	if (!check(Read<uint32_t>(pointers[0][2]) == 0x12345678 && Read<uint32_t>(pointers[0][3]) == 0x87654321 &&
	               Read<uint32_t>(pointers[1][2]) == 1 && Read<uint32_t>(pointers[1][3]) == 1 &&
	               Read<uint64_t>(pointers[1][0]) == 0xDeadBeef00000007ULL,
	           "guest writes through a module must affect only its owner, including distinct libc/internal cells")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[0]);
	for (unsigned j = 0; j != 4; ++j) {
		auto relocation = relocations[0][j];
		relocation.r_info = (uint64_t(j) << 32) | R_X86_64_64;
		relocation.r_offset = (4 + j) * sizeof(uint64_t);
		relocation.r_addend = Sizes[j] - 1;
		if (!check(RelocateRecord(j, &relocation, &programs[0], false) &&
		               Read<uint64_t>(programs[0].base_vaddr + relocation.r_offset) == pointers[0][j] + Sizes[j] - 1,
		           "R_X86_64_64 must apply an in-object addend to the owner-local address")) { return false; }
		for (int64_t denied: std::array<int64_t, 3> {-1, static_cast<int64_t>(Sizes[j]), INT64_MAX}) {
			relocation.r_addend = denied;
			if (!check(!GetRelocationInfo(&relocation, &programs[0]).resolved,
			           "negative/end/overflow addends must not escape the exact object into neighboring cells or buffer")) { return false; }
		}
		const auto size = symbols[0][j].st_size;
		symbols[0][j].st_size = size + 1;
		const bool bad_size = !GetRelocationInfo(&relocations[0][j], &programs[0]).resolved &&
		    !linker.GetApplicationHardImportBlockers(&programs[0]).empty();
		symbols[0][j].st_size = 0;
		const bool unspecified = GetRelocationInfo(&relocations[0][j], &programs[0]).value == pointers[0][j];
		symbols[0][j].st_size = size;
		if (!check(bad_size && unspecified, "nonzero import sizes must match exactly; zero retains audited registration size")) { return false; }
	}
	const auto slot = pointers[0][0];
	std::memcpy(reinterpret_cast<void*>(programs[0].base_vaddr), &host[0].vaddr, sizeof(uint64_t));
	const bool raw_rejected = !linker.GetApplicationHardImportBlockers(&programs[0]).empty();
	std::memcpy(reinterpret_cast<void*>(programs[0].base_vaddr), &slot, sizeof(slot));
	if (!check(raw_rejected, "hard gate must reject a raw-host replacement of an audited private data relocation")) { return false; }
	const auto typed_info = symbols[0][0].st_info;
	symbols[0][0].st_info = (STB_GLOBAL << 4) | STT_NOTYPE;
	const bool untyped_rejected = !linker.GetApplicationHardImportBlockers(&programs[0]).empty();
	symbols[0][0].st_info = typed_info;
	if (!check(untyped_rejected, "an audited NID must not bypass the hard gate for an untyped relocation")) { return false; }
	for (unsigned variant = 0; variant != 4; ++variant) {
		auto forged = host[0];
		if (variant == 0) { forged.vaddr += 1; }
		if (variant == 1) { forged.dbg_name = "&Other::g_stack_chk_guard"; }
		if (variant == 2) { forged.name.replace(forged.name.find("[Object]"), 8, "[Func]"); }
		if (variant == 3) { forged.name.replace(forged.name.find("_v1]"), 4, "_v2]"); }
		if (!check(linker.ResolveApplicationImportData(&programs[5], Nids[0], forged) == 0 && !programs[5].application_import_data,
		           "wrong address/expression/type/version must not allocate or publish a data store")) { return false; }
	}
	{
		SymbolDatabase conflicting;
		Libs::InitLibKernel_1(&conflicting);
		conflicting.Add({Nids[0], "libkernel", 1, "libkernel", 1, 1, SymbolType::Object},
		    host[0].vaddr + 1, host[0].dbg_name);
		if (!check(!ApplicationImportData::Supports(conflicting, Nids[0], host[0]),
		           "a conflicting exact registration must fail closed even when an older matching address remains indexed")) { return false; }
	}
	void* foreign = reinterpret_cast<void*>(1);
	if (!check(linker.ResolveApplicationDlsym(&programs[1], Nids[0], Nids[0], &foreign) == KERNEL_ERROR_EOPNOTSUPP && !foreign,
	           "an explicit foreign private handle must not publish even an otherwise audited data object")) { return false; }
	const SymbolResolve unknown {"appDataUnknown", "libkernel", 1, "libkernel", 1, 1, SymbolType::Object};
	linker.Symbols()->Add(unknown, reinterpret_cast<uint64_t>(&unknown_host_object), "&Fixture::unknown_host_object");
	programs[4].export_symbols->Add(unknown, reinterpret_cast<uint64_t>(&unknown_host_object), "&Fixture::unknown_host_object");
	programs[6].dynamic_info->rela_table_total_size = sizeof(Elf64_Rela);
	symbols[6][0].st_name = offsets[4]; symbols[6][0].st_size = sizeof(unknown_host_object);
	linker.m_programs.push_back(&programs[6]);
	const bool unknown_unresolved = !RelocateRecord(0, &relocations[6][0], &programs[6], false);
	void* refused = reinterpret_cast<void*>(1);
	if (!check(unknown_unresolved && !linker.GetApplicationHardImportBlockers(&programs[0]).empty() &&
	               dlsym(programs[4].unique_id, "appDataUnknown", &refused) == KERNEL_ERROR_EOPNOTSUPP && !refused &&
	               !programs[0].application_execution->Failure(),
	           "unknown host object must remain a hard relocation blocker and a non-poisoning optional dlsym refusal")) { return false; }
	const auto old_lifetime = programs[0].application_execution->Owner();
	programs[0].application_execution->RequestStop();
	auto stopped_relocation = relocations[0][0]; stopped_relocation.r_offset = 8 * sizeof(uint64_t);
	uint64_t sentinel = 0x123456789abcdef0ULL;
	std::memcpy(reinterpret_cast<void*>(programs[0].base_vaddr + stopped_relocation.r_offset), &sentinel, sizeof(sentinel));
	refused = reinterpret_cast<void*>(1);
	if (!check(!RelocateRecord(0, &stopped_relocation, &programs[0], false) &&
	               Read<uint64_t>(programs[0].base_vaddr + stopped_relocation.r_offset) == sentinel &&
	               linker.ResolveApplicationImportData(&programs[0], Nids[0], host[0]) == 0 &&
	               dlsym(programs[4].unique_id, Nids[0], &refused) == KERNEL_ERROR_ECANCELED && !refused &&
	               programs[0].application_import_data && Read<uint32_t>(pointers[0][2]) == 0x12345678,
	           "stop must deny relocation/dlsym publication without releasing or resetting previously copied data pointers")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[5]);
	for (unsigned j = 0; j != 4; ++j) {
		if (!check(RelocateRecord(j, &relocations[5][j], &programs[5], false), "relaunch object relocation")) { return false; }
		pointers[5][j] = Read<uint64_t>(programs[5].base_vaddr + j * sizeof(uint64_t));
		if (!check(pointers[5][j] != pointers[0][j], "a new launch must not alias still-pinned stopped-owner cells")) { return false; }
	}
	if (!check(programs[5].application_execution->Owner().token != old_lifetime.token &&
	               !programs[0].application_import_data->ContainsObjectAddress(programs[5].application_execution->Owner(), pointers[0][0]) &&
	               Read<uint64_t>(pointers[5][0]) == 0xDeadBeef00000007ULL && Read<uint32_t>(pointers[5][2]) == 1 &&
	               Read<uint32_t>(pointers[5][3]) == 1 && std::strcmp(reinterpret_cast<const char*>(Read<uint64_t>(pointers[5][1])), filenames[5]) == 0 &&
	               linker.GetApplicationHardImportBlockers(&programs[5]).empty(),
	           "same executable relaunch must get a fresh lifetime and freshly initialized isolated data")) { return false; }
	{
		Program long_name;
		long_name.application_owner = &long_name;
		long_name.file_name = std::string(600, 'n'); // No filesystem lookup: tests the store's bounded filename copy.
		if (!check(InitializeApplicationExecution(&long_name), "initialize bounded-name storage owner")) { return false; }
		auto data = ApplicationImportData::Create(&long_name, long_name.application_execution->Owner());
		if (!check(data != nullptr, "create bounded-name data store")) { return false; }
		const auto cell = data->Address(long_name.application_execution->Owner(), *linker.Symbols(), Nids[1], host[1]);
		if (!check(cell != 0, "resolve exact program-name pointer cell")) { return false; }
		const auto buffer = Read<uint64_t>(cell);
		if (!check(std::strlen(reinterpret_cast<const char*>(buffer)) == ApplicationImportData::ProgramNameCapacity - 1 &&
		               Read<char>(buffer + ApplicationImportData::ProgramNameCapacity - 1) == 0 &&
		               !data->ContainsObjectAddress(long_name.application_execution->Owner(), buffer),
		           "progname must terminate at 511 bytes and the buffer must not become an importable pointer-cell addend")) { return false; }
		long_name.application_execution->RequestStop();
	}
	return check(Read<uint64_t>(host[0].vaddr) == session_guard && Read<uint32_t>(host[2].vaddr) == session_libc &&
	                 Read<uint32_t>(host[3].vaddr) == session_internal && Read<uint64_t>(host[1].vaddr) == session_name_pointer &&
	                 std::memcmp(reinterpret_cast<const void*>(session_name_pointer), session_name.data(), session_name.size()) == 0,
	             "all shared launches, writes and stops must leave session guard/progname/both flags unchanged");
}
