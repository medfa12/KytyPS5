// Included by runtimeLinker.cpp inside namespace Loader, under
// KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS. This exercises production loader APIs;
// only the list of loaded programs and guest heap callbacks are synthetic.
namespace RuntimeApplicationScopeFixture {
struct HeapProbe {
	Common::Mutex* mutex = nullptr;
	uint64_t size = 0;
	uint64_t alignment = 0;
	unsigned malloc_calls = 0;
	unsigned memalign_calls = 0;
	bool callbacks_unlocked = true;
	bool fail_memalign = false;
	char malloc_token = 0;
	char memalign_token = 0;
};
static HeapProbe* heap_probes = nullptr;

static bool CallbackRunsUnlocked(Common::Mutex* mutex) {
	// A same-thread reentrant call would not detect a held recursive mutex.
	// TryLock on a second thread fails immediately rather than hanging the test.
	struct Observation { Common::Mutex* mutex; bool unlocked = false; } observation {mutex};
	Common::Thread observer([](void* data) {
		auto* result = static_cast<Observation*>(data);
		result->unlocked = result->mutex->TryLock();
		if (result->unlocked) { result->mutex->Unlock(); }
	}, &observation);
	observer.Join();
	return observation.unlocked;
}

template <unsigned Index>
static KYTY_SYSV_ABI void* HeapMalloc(uint64_t size) {
	auto& probe = heap_probes[Index];
	probe.size = size;
	++probe.malloc_calls;
	probe.callbacks_unlocked &= CallbackRunsUnlocked(probe.mutex);
	return &probe.malloc_token;
}

template <unsigned Index>
static KYTY_SYSV_ABI void HeapFree(void*) {}

template <unsigned Index>
static KYTY_SYSV_ABI int HeapMemalign(void** out, uint64_t alignment, uint64_t size) {
	auto& probe = heap_probes[Index];
	probe.alignment = alignment;
	probe.size = size;
	++probe.memalign_calls;
	probe.callbacks_unlocked &= CallbackRunsUnlocked(probe.mutex);
	*out = &probe.memalign_token;
	return probe.fail_memalign ? 12 : 0;
}
} // namespace RuntimeApplicationScopeFixture

bool RuntimeLinker::TestApplicationSymbolScope(std::string* failure) {
	struct RestoreCurrentProgram {
		Program* previous = RuntimeLinker::GetCurrentProgram();
		~RestoreCurrentProgram() { RuntimeLinker::SetCurrentProgram(previous); }
	} restore_current; // Restore after the local linker's Clear() has run.
	RuntimeLinker linker;
	Program app_a, module_a, app_b, module_b, firmware, direct_game, shell;
	// Clear() owns/deletes published programs and frees their guest mappings.
	// Detach these stack fixtures on every return (and on exception unwinding).
	struct DetachPrograms {
		std::vector<Program*>& programs;
		~DetachPrograms() {
			RuntimeLinker::SetCurrentProgram(nullptr);
			programs.clear();
		}
	} detach {linker.m_programs};
	linker.m_ssl_smoke_enabled = false;
	linker.m_ssl_production_enabled = false;
	app_a.application_owner = &app_a;
	module_a.application_owner = &app_a;
	app_b.application_owner = &app_b;
	module_b.application_owner = &app_b;
	app_a.file_name = "scope-a/eboot.bin";
	module_a.file_name = "scope-a/private.sprx";
	app_b.file_name = "scope-b/eboot.bin";
	module_b.file_name = "scope-b/private.sprx";
	firmware.file_name = "firmware/shared.sprx";
	direct_game.file_name = "direct/eboot.bin";
	shell.file_name = "shell/eboot.bin";
	// Shared firmware and the other application precede A in global load order.
	linker.m_programs = {&shell, &firmware, &app_b, &module_b, &direct_game, &app_a, &module_a};
	int32_t next_id = 100;
	for (auto* p: linker.m_programs) {
		p->rt = &linker;
		p->unique_id = next_id++;
		// Nonzero and distinct even on modules, to catch using the calling
		// module's proc-param rather than its application owner's.
		p->proc_param_vaddr = 0x70000000 + uint64_t(p->unique_id) * 0x1000;
		p->dynamic_info = std::make_unique<DynamicInfo>();
		p->export_symbols = std::make_unique<SymbolDatabase>();
		p->dynamic_info->import_modules = {{"M", 1, 0, "ScopeModule"}};
		p->dynamic_info->import_libs = {{"E", 1, "ScopeExact"}, {"N", 1, "ScopeNid"},
		                                {"C", 1, "Jsc_v1"}};
		p->dynamic_info->export_modules = p->dynamic_info->import_modules;
		p->dynamic_info->export_libs = p->dynamic_info->import_libs;
		p->dynamic_info->export_modules.push_back({"R", 8, 0, "ScopeRenamedModule"});
		p->dynamic_info->export_libs.push_back({"R", 9, "ScopeRenamedLibrary"});
	}
	const auto check = [&](bool ok, const std::string& message) {
		if (!ok && failure != nullptr) { *failure = message; }
		return ok;
	};
	const std::vector<Program*> visible_a {&app_a, &module_a, &shell, &firmware, &direct_game};
	const std::vector<Program*> visible_b {&app_b, &module_b, &shell, &firmware, &direct_game};
	const std::vector<Program*> visible_shared {&shell, &firmware, &direct_game};
	for (auto* requester: {&app_a, &module_a, &app_b, &module_b, &firmware, &direct_game,
	                       static_cast<Program*>(nullptr)}) {
		const auto& expected = requester != nullptr && requester->application_owner == &app_a
		                           ? visible_a
		                           : requester != nullptr && requester->application_owner == &app_b
		                                 ? visible_b : visible_shared;
		if (!check(linker.GetVisiblePrograms(requester) == expected,
		           "visibility must preserve owner-first/session-second order and exclude other apps")) {
			return false;
		}
	}

	// Parse tiny real ELF headers so the legacy GetProcParam path exercises
	// IsShared(), rather than silently testing only its no-ELF/missing case.
	struct RemoveElfFixtures {
		std::vector<Program*>& programs;
		std::vector<std::filesystem::path> paths;
		~RemoveElfFixtures() {
			for (auto* p: programs) { p->elf.reset(); }
			for (const auto& path: paths) {
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		}
	} elf_fixtures {linker.m_programs, {}};
	const auto fixture_prefix = "kyty_application_scope_" +
	    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
	    std::to_string(reinterpret_cast<uintptr_t>(&linker));
	for (auto* p: linker.m_programs) {
		Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\177ELF", 4);
		header.e_ident[EI_CLASS] = ELFCLASS64;
		header.e_ident[EI_DATA] = ELFDATA2LSB;
		header.e_ident[EI_VERSION] = EV_CURRENT;
		header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
		const bool shared = p == &firmware || p == &module_a || p == &module_b;
		header.e_type = shared ? ET_DYNAMIC : ET_DYNEXEC;
		header.e_machine = EM_X86_64;
		header.e_version = EV_CURRENT;
		header.e_ehsize = sizeof(header);
		header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Elf64_Phdr);
		header.e_phnum = 1;
		Elf64_Phdr segment {};
		segment.p_type = PT_LOAD;
		segment.p_flags = PF_R;
		const auto path = std::filesystem::temp_directory_path() /
		                  (fixture_prefix + "_" + std::to_string(p->unique_id) + ".elf");
		elf_fixtures.paths.push_back(path);
		Common::File file;
		if (!check(file.Create(path), "could not create proc-param ELF fixture")) { return false; }
		uint32_t header_written = 0, segment_written = 0;
		file.Write(&header, sizeof(header), &header_written);
		file.Write(&segment, sizeof(segment), &segment_written);
		file.Close();
		if (!check(header_written == sizeof(header) && segment_written == sizeof(segment),
		           "could not write proc-param ELF fixture")) { return false; }
		p->elf = std::make_unique<Elf64>();
		p->elf->Open(path);
		if (!check(p->elf->IsValid() && p->elf->IsShared() == shared,
		           "invalid proc-param ELF fixture")) { return false; }
	}
	for (auto* requester: {&app_a, &module_a, &app_b, &module_b, &shell, &firmware, &direct_game,
	                       static_cast<Program*>(nullptr)}) {
		SetCurrentProgram(requester);
		auto* expected = requester != nullptr && requester->application_owner != nullptr
		                     ? requester->application_owner : &shell;
		if (!check(linker.FindProgramById(0) == expected &&
		               linker.GetProcParam() == expected->proc_param_vaddr,
		           "main handle/proc-param did not select application owner or shared session")) {
			return false;
		}
		// Explicit IDs keep their existing global handle meaning, even when the
		// implicit main handle is scoped to a different application.
		for (auto* p: linker.m_programs) {
			if (!check(linker.FindProgramById(p->unique_id) == p,
			           "application scope changed an explicit program handle")) { return false; }
		}
	}
	SetCurrentProgram(&module_a);
	const auto saved_proc_param = app_a.proc_param_vaddr;
	app_a.proc_param_vaddr = 0;
	if (!check(linker.GetProcParam() == 0, "missing app proc-param fell back to shell")) { return false; }
	app_a.proc_param_vaddr = saved_proc_param;
	// The direct-game-only launch remains the legacy main when no shell or
	// runtime application is loaded. Restore the full fixture before checking.
	const auto all_programs = linker.m_programs;
	linker.m_programs = {&direct_game, &firmware};
	SetCurrentProgram(&firmware);
	const bool direct_main = linker.FindProgramById(0) == &direct_game &&
	                         linker.GetProcParam() == direct_game.proc_param_vaddr;
	linker.m_programs = all_programs;
	if (!check(direct_main, "legacy direct-game main handle/proc-param changed")) { return false; }

	using namespace RuntimeApplicationScopeFixture;
	if (!check(InitializeApplicationExecution(&app_a) && InitializeApplicationExecution(&app_b),
	           "owner-scoped heap callbacks require live execution admission")) { return false; }
	HeapProbe probes[3]; // Shared shell, application A, application B.
	for (auto& probe: probes) { probe.mutex = &linker.m_mutex; }
	struct RestoreHeapProbes {
		HeapProbe* previous = heap_probes;
		~RestoreHeapProbes() { heap_probes = previous; }
	} restore_probes;
	heap_probes = probes;
	void* apis[3][10] {};
	apis[0][0] = reinterpret_cast<void*>(HeapMalloc<0>);
	apis[0][1] = reinterpret_cast<void*>(HeapFree<0>);
	apis[0][6] = reinterpret_cast<void*>(HeapMemalign<0>);
	apis[1][0] = reinterpret_cast<void*>(HeapMalloc<1>);
	apis[1][1] = reinterpret_cast<void*>(HeapFree<1>);
	apis[1][6] = reinterpret_cast<void*>(HeapMemalign<1>);
	apis[2][0] = reinterpret_cast<void*>(HeapMalloc<2>);
	apis[2][1] = reinterpret_cast<void*>(HeapFree<2>);
	apis[2][6] = reinterpret_cast<void*>(HeapMemalign<2>);
	SetCurrentProgram(&shell);
	linker.SetApplicationHeapApi(apis[0]);
	const auto expect_heap = [&](Program* requester, int index) {
		SetCurrentProgram(requester);
		const auto total_calls = [&] {
			unsigned total = 0;
			for (const auto& probe: probes) { total += probe.malloc_calls + probe.memalign_calls; }
			return total;
		};
		const auto before = total_calls();
		const auto malloc_before = index >= 0 ? probes[index].malloc_calls : 0;
		const auto memalign_before = index >= 0 ? probes[index].memalign_calls : 0;
		void* allocated = linker.ApplicationHeapMalloc(123);
		const bool malloc_ok = index < 0 ? allocated == nullptr :
		    allocated == &probes[index].malloc_token && probes[index].size == 123 &&
		    probes[index].malloc_calls == malloc_before + 1;
		void* aligned = linker.ApplicationHeapMemalign(256, 789);
		const bool memalign_ok = index < 0 ? aligned == nullptr :
		    aligned == &probes[index].memalign_token && probes[index].size == 789 &&
		    probes[index].alignment == 256 && probes[index].memalign_calls == memalign_before + 1;
		return check(malloc_ok && memalign_ok && total_calls() == before + (index < 0 ? 0 : 2),
		             "heap dispatch/arguments leaked across owners or used shell fallback") &&
		       check(index < 0 || probes[index].callbacks_unlocked,
		             "guest heap callback ran while loader mutex was held");
	};
	// An application without its own callbacks must not borrow the shell heap.
	if (!expect_heap(&module_a, -1) || !expect_heap(&app_b, -1)) { return false; }
	SetCurrentProgram(&module_a); // Registration through a module belongs to A.
	linker.SetApplicationHeapApi(apis[1]);
	SetCurrentProgram(&app_b);
	linker.SetApplicationHeapApi(apis[2]);
	if (!check(app_a.application_heap_free == HeapFree<1> &&
	               app_b.application_heap_free == HeapFree<2> &&
	               linker.m_application_heap_free == HeapFree<0> &&
	               module_a.application_heap_malloc == nullptr && module_a.application_heap_free == nullptr &&
	               module_a.application_heap_posix_memalign == nullptr,
	           "heap API registration was stored on module or overwrote shared free callback")) { return false; }
	for (auto* requester: {&shell, &firmware, &direct_game, static_cast<Program*>(nullptr)}) {
		if (!expect_heap(requester, 0)) { return false; }
	}
	if (!expect_heap(&app_a, 1) || !expect_heap(&module_a, 1) ||
	    !expect_heap(&app_b, 2) || !expect_heap(&module_b, 2)) { return false; }
	SetCurrentProgram(&module_b);
	probes[2].fail_memalign = true;
	const auto failed_calls = probes[2].memalign_calls;
	if (!check(linker.ApplicationHeapMemalign(512, 321) == nullptr &&
	               probes[2].memalign_calls == failed_calls + 1 && probes[2].alignment == 512 &&
	               probes[2].size == 321 && probes[2].callbacks_unlocked,
	           "failed owner memalign exposed its output pointer or retried another heap")) { return false; }
	probes[2].fail_memalign = false;
	// Exercise explicit reset and both invalid required callback slots.
	for (int reset_kind = 0; reset_kind != 3; ++reset_kind) {
		SetCurrentProgram(&module_a);
		linker.SetApplicationHeapApi(apis[1]);
		void* invalid[10] {};
		std::copy(std::begin(apis[1]), std::end(apis[1]), std::begin(invalid));
		if (reset_kind != 0) { invalid[reset_kind - 1] = nullptr; }
		linker.SetApplicationHeapApi(reset_kind == 0 ? nullptr : invalid);
		if (!check(app_a.application_heap_malloc == nullptr && app_a.application_heap_free == nullptr &&
		               app_a.application_heap_posix_memalign == nullptr &&
		               linker.m_application_heap_free == HeapFree<0> &&
		               app_b.application_heap_free == HeapFree<2>,
		           "reset did not clear owner heap callbacks or changed another heap's free callback") ||
		    !expect_heap(&app_a, -1) || !expect_heap(&module_a, -1) ||
		    !expect_heap(&shell, 0) || !expect_heap(&module_b, 2)) { return false; }
	}
	// Resetting the legacy/session API likewise leaves registered app heaps intact.
	SetCurrentProgram(&app_a);
	linker.SetApplicationHeapApi(apis[1]);
	SetCurrentProgram(&firmware);
	linker.SetApplicationHeapApi(nullptr);
	if (!check(linker.m_application_heap_malloc == nullptr && linker.m_application_heap_free == nullptr &&
	               linker.m_application_heap_posix_memalign == nullptr &&
	               app_a.application_heap_free == HeapFree<1> && app_b.application_heap_free == HeapFree<2>,
	           "session reset retained callbacks or changed an application free callback") ||
	    !expect_heap(&shell, -1) || !expect_heap(&direct_game, -1) ||
	    !expect_heap(&module_a, 1) || !expect_heap(&module_b, 2)) { return false; }

	struct Route {
		const char* nid;
		const char* library_id;
		const char* library;
		bool nid_only;
		bool cohost;
	};
	const Route routes[] = {
	    {"ScopeExactNid", "E", "ScopeExact", false, false},
	    {"ScopeFallbackNid", "N", "ScopeNid", true, false},
	    {"ScopeCohostExactNid", "C", "Jsc_v1", false, true},
	    {"ScopeCohostFallbackNid", "C", "Jsc_v1", true, true},
	};
	uint64_t route_offset = 0;
	for (const auto& route: routes) {
		for (const auto type: {SymbolType::Func, SymbolType::Object}) {
			const uint64_t offset = route_offset + (type == SymbolType::Object ? 0x100 : 0);
			const SymbolResolve requested {route.nid, route.library, 1, "ScopeModule", 1, 0, type};
			auto exported = requested;
			if (route.nid_only) {
				// Neither library nor module identity matches the import. Only a real
				// NID lookup can find this record (including the cohost fast path).
				exported.library = "ScopeRenamedLibrary";
				exported.library_version = 9;
				exported.module = "ScopeRenamedModule";
				exported.module_version_major = 8;
			}
			const auto add = [&](Program& provider, const SymbolResolve& key, uint64_t address) {
				provider.export_symbols->Add(key, address, provider.file_name.generic_string());
			};
			uint64_t provider_base = 0x10000;
			for (auto* provider: {&module_a, &module_b, &firmware}) {
				if (!route.nid_only) {
					// Insert a same-NID/wrong-version decoy first. A resolver that
					// accidentally skips exact matching must not pass these cases.
					auto decoy = exported;
					decoy.module_version_minor = 99;
					add(*provider, decoy, provider_base + offset + 0x80);
				}
				add(*provider, exported, provider_base + offset);
				provider_base += 0x10000;
			}
			if (route.cohost) {
				// Cohost providers must beat an exact HLE stand-in, including when
				// the native export is found by NID with mismatched metadata.
				linker.Symbols()->Add(requested, 0xf0000 + offset, "HLE stand-in");
			}
			auto private_key = exported;
			private_key.name += "PrivateB";
			add(module_b, private_key, 0x40000 + offset);
			auto shared_key = exported;
			shared_key.name += "FirmwareOnly";
			add(firmware, shared_key, 0x50000 + offset);
			auto legacy_key = exported;
			legacy_key.name += "DirectGameOnly";
			add(direct_game, legacy_key, 0x60000 + offset);

			const auto resolve = [&](Program& requester, const std::string& nid,
			                         const char* library_id, const char* module_id,
			                         Program* provider, const SymbolResolve& expected_key,
			                         uint64_t address, bool verify_bind) {
				SymbolRecord out {"stale-name", "stale-debug", 0xdeadbeef};
				bool bind_self = true;
				const auto import = nid + "#" + library_id + "#" + module_id;
				linker.Resolve(import, type, &requester, &out, &bind_self);
				const auto context = requester.file_name.generic_string() + " resolving " + import +
				                     (type == SymbolType::Object ? " (object)" : " (function)");
				return check(out.vaddr == address && out.name == SymbolDatabase::GenerateName(expected_key) &&
				                 out.dbg_name == (provider ? provider->file_name.generic_string() : ""),
				             context + ": wrong provider/record or cross-application leak") &&
				       check(!verify_bind || bind_self == (provider == &requester),
				             context + ": incorrect bind_self");
			};
			for (auto* requester: {&app_a, &module_a, &app_b, &module_b, &firmware, &direct_game}) {
				auto* provider = requester->application_owner == &app_a ? &module_a :
				                 requester->application_owner == &app_b ? &module_b : &firmware;
				const uint64_t address = (provider == &module_a ? 0x10000 :
				                          provider == &module_b ? 0x20000 : 0x30000) + offset;
				// The generic NID fallback currently does not report self binding;
				// exact and cohost paths do, so exercise their true and false cases.
				const bool verify_bind = !route.nid_only || route.cohost;
				if (!resolve(*requester, exported.name, route.library_id, "M", provider, exported,
				             address, verify_bind) ||
				    !resolve(*requester, shared_key.name, route.library_id, "M", &firmware, shared_key,
				             0x50000 + offset, verify_bind) ||
				    !resolve(*requester, legacy_key.name, route.library_id, "M", &direct_game, legacy_key,
				             0x60000 + offset, verify_bind)) {
					return false;
				}
				const bool owns_private = requester->application_owner == &app_b;
				auto missing_key = requested;
				missing_key.name = private_key.name;
				if (!resolve(*requester, private_key.name, route.library_id, "M",
				             owns_private ? &module_b : nullptr, owns_private ? private_key : missing_key,
				             owns_private ? 0x40000 + offset : 0, verify_bind)) {
					return false;
				}
				if (route.nid_only && !route.cohost) {
					// Missing import metadata takes a separate fallback branch. Keep
					// these positive: its no-provider behavior intentionally aborts.
					if (!resolve(*requester, exported.name, "MissingLibrary", "MissingModule", provider,
					             exported, address, false) ||
					    !resolve(*requester, shared_key.name, "MissingLibrary", "MissingModule", &firmware,
					             shared_key, 0x50000 + offset, false)) {
						return false;
					}
				}
			}
		}
		route_offset += 0x1000;
	}

	// Native56 must beat both early and late HLE registrations in the real
	// resolver, including metadata-free lookup. Native54 keeps its old identity.
	{
		struct RestoreExports {
			Program& program;
			std::unique_ptr<SymbolDatabase> saved;
			~RestoreExports() { program.export_symbols = std::move(saved); }
		} restore {firmware, std::move(firmware.export_symbols)};
		for (const auto policy: {SymbolDatabase::ExportPolicy::NpHttpPool,
		                         SymbolDatabase::ExportPolicy::NpDispatcher}) {
			firmware.export_symbols = std::make_unique<SymbolDatabase>(policy);
			for (const auto* nid: {"fhJ5uKzcn0w", "EjMsfO3GCIA"}) {
				const SymbolResolve key {nid, "ScopeExact", 1, "ScopeModule", 1, 0, SymbolType::Func};
				linker.Symbols()->Add(key, 0xf1000, "early NP stub");
				firmware.export_symbols->Add(key, 0x71000, "native NP dispatcher");
				// A private provider loaded earlier must remain invisible to ShellUI.
				module_b.export_symbols->Add(key, 0x72000, "foreign private dispatcher");
				for (const bool late: {false, true}) {
					if (late) { linker.Symbols()->Add(key, 0xf2000, "late NP stub"); }
					for (const auto* suffix: {"#E#M", "#MissingLibrary#MissingModule", ""}) {
						SymbolRecord result {};
						bool self = true;
						linker.Resolve(std::string(nid) + suffix, SymbolType::Func, &shell, &result, &self);
						// Legacy NID fallback keeps its first-insertion HLE behavior.
						const bool exact = std::string_view(suffix) == "#E#M";
						const auto expected = policy == SymbolDatabase::ExportPolicy::NpDispatcher ? 0x71000u :
						                      (*suffix == '\0' ? 0u : (late && exact ? 0xf2000u : 0xf1000u));
						if (!check(result.vaddr == expected && !self,
						           "native dispatcher policy did not preserve scoped HLE/native precedence")) { return false; }
					}
				}
			}
		}
	}

	// Query real relocation records without mapping or patching any guest memory.
	// Ordinary RELA has Z then a weak import; PLT has A then a duplicate Z,
	// deliberately requiring both table traversal and sorted/deduplicated output.
	struct ImportRelocations {
		DynamicInfo& info;
		DynamicInfo saved;
		std::string names = std::string(1, '\0');
		std::array<Elf64_Sym, 4> symbols {};
		std::array<Elf64_Rela, 2> rela {};
		std::array<Elf64_Rela, 2> plt {};
		ImportRelocations(Program& program, const char* prefix)
		    : info(*program.dynamic_info), saved(info) {
			unsigned index = 1; // Symbol zero remains the ELF undefined sentinel.
			for (const auto* suffix: {"Z#E#M", "Weak#E#M", "A#E#M"}) {
				auto& symbol = symbols[index];
				symbol.st_name = static_cast<Elf64_Word>(names.size());
				symbol.st_info = static_cast<unsigned char>(((index == 2 ? STB_WEAK : STB_GLOBAL) << 4) | STT_FUNC);
				names += std::string(prefix) + suffix;
				names.push_back('\0');
				++index;
			}
			rela[0].r_info = (uint64_t {1} << 32) | R_X86_64_GLOB_DAT;
			rela[1].r_info = (uint64_t {2} << 32) | R_X86_64_GLOB_DAT;
			plt[0].r_info = (uint64_t {3} << 32) | R_X86_64_JUMP_SLOT;
			plt[1].r_info = (uint64_t {1} << 32) | R_X86_64_JUMP_SLOT;
			// r_offset, addend, st_shndx and st_value remain zero. The query
			// must resolve metadata only, never dereference a relocation target.
			info.str_table = names.data();
			info.str_table_size = names.size();
			info.symbol_table = symbols.data();
			info.symbol_table_total_size = sizeof(symbols);
			info.symbol_table_entry_size = sizeof(Elf64_Sym);
			info.rela_table = rela.data();
			info.rela_table_total_size = sizeof(rela);
			info.rela_table_entry_size = sizeof(Elf64_Rela);
			info.jmprela_table = plt.data();
			info.jmprela_table_size = sizeof(plt);
		}
		~ImportRelocations() { info = std::move(saved); } // Detach before arrays/strings die.
	} main_imports {app_a, "ScopeLaunchMain"}, module_imports {module_a, "ScopeLaunchModule"},
	  other_imports {module_b, "ScopeLaunchOther"}, session_imports {firmware, "ScopeLaunchSession"};
	const auto provide_import = [&](Program& provider, const char* nid, uint64_t address) {
		provider.export_symbols->Add({nid, "ScopeExact", 1, "ScopeModule", 1, 0, SymbolType::Func}, address);
	};
	provide_import(module_b, "ScopeLaunchMainZ", 0x90000); // Invisible to application A.
	SetCurrentProgram(&module_b); // The explicit owner, not current TLS context, selects the scan.
	if (!check(linker.GetUnresolvedApplicationImports(&app_a) == std::vector<std::string> {
	               "eboot.bin: ScopeLaunchMainA#E#M", "eboot.bin: ScopeLaunchMainZ#E#M",
	               "private.sprx: ScopeLaunchModuleA#E#M", "private.sprx: ScopeLaunchModuleZ#E#M"},
	           "required import scan lost main/module misses, included weak/foreign imports, or failed sorting/dedup") ||
	    !check(linker.GetUnresolvedApplicationImports(&app_b) == std::vector<std::string> {
	               "private.sprx: ScopeLaunchOtherA#E#M", "private.sprx: ScopeLaunchOtherZ#E#M"},
	           "other application's strong relocation fixture was not scanned independently")) { return false; }
	provide_import(module_a, "ScopeLaunchMainZ", 0xa0000);
	if (!check(linker.GetUnresolvedApplicationImports(&app_a) == std::vector<std::string> {
	               "eboot.bin: ScopeLaunchMainA#E#M", "private.sprx: ScopeLaunchModuleA#E#M",
	               "private.sprx: ScopeLaunchModuleZ#E#M"},
	           "visible owner-module provider did not remove its resolved required import")) { return false; }
	provide_import(firmware, "ScopeLaunchMainA", 0xb0000);
	provide_import(firmware, "ScopeLaunchModuleA", 0xb0010);
	provide_import(firmware, "ScopeLaunchModuleZ", 0xb0020);
	if (!check(linker.GetUnresolvedApplicationImports(&app_a).empty(),
	           "shared providers did not resolve remaining strong imports, or weak/session/other-app misses leaked")) {
		return false;
	}
	return true;
}
