// Included inside Loader by runtimeLinker.cpp in the CPU fixture build.
// Parent dispatch: RuntimeLinker::TestKernelTextLocation(&failure), after
// InitSubsystems. No native libkernel file, guest execution or actual mapping
// is created: synthetic published Program metadata exercises the locked query.
bool RuntimeLinker::TestKernelTextLocation(std::string* failure) {
	SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	for (const auto& export_info: std::array<std::pair<const char*, uint64_t>, 2> {{
	         {"-YTW+qXc3CQ", reinterpret_cast<uint64_t>(&Libs::LibKernel::KernelInternalMemoryGetModuleSegmentInfo)},
	         {"AmJ0mn2l4lM", reinterpret_cast<uint64_t>(&Libs::LibKernel::KernelGetLibkernelTextLocation)}}}) {
		SymbolResolve query {export_info.first, "libkernel", 1, "libkernel", 1, 1, SymbolType::Func};
		const auto* symbol = symbols.Find(query);
		if (symbol == nullptr || symbol->vaddr != export_info.second) {
			if (failure != nullptr) { *failure = "kernel text query exact typed registrations missing"; }
			return false;
		}
	}
	RuntimeLinker linker;
	// Process-identity records retain owner addresses for the session, as in
	// RuntimeProcessIdentityTests. Retire these identities but keep owners alive.
	auto* first = new Program;
	auto* second = new Program;
	Program caller, foreign_kernel, session_kernel, unrelated, local_kernel, duplicate, unpublished;
	struct Cleanup {
		RuntimeLinker& linker;
		Program* first;
		Program* second;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		int saved_errno = *Libs::Posix::GetErrorAddr();
		~Cleanup() {
			DeactivateRuntimeProcessIdentity(first);
			DeactivateRuntimeProcessIdentity(second);
			RuntimeLinker::SetCurrentProgram(previous);
			*Libs::Posix::GetErrorAddr() = saved_errno;
			linker.m_programs.clear(); // All records here are synthetic, not loader allocations.
			first->rt = second->rt = nullptr;
		}
	} cleanup {linker, first, second};
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure != nullptr) { *failure = message; }
		return ok;
	};
	const auto setup = [&](Program* p, Program* owner, const char* name, uint64_t base) {
		p->rt = &linker;
		p->application_owner = owner;
		p->file_name = name;
		p->base_vaddr = base;
		p->mapped_size = 0x4000;
	};
	setup(first, first, "first/eboot.bin", 0x710000000);
	setup(second, second, "second/eboot.bin", 0x720000000);
	setup(&caller, first, "first/libc.prx", 0x730000000);
	setup(&foreign_kernel, second, "second/libkernel.sprx", 0x740000000);
	setup(&session_kernel, nullptr, "firmware/libkernel_sys.sprx", 0x750000000);
	setup(&unrelated, first, "first/libSceUnrelated.prx", 0x760000000);
	setup(&local_kernel, first, "first/libkernel.prx", 0x770000000);
	setup(&duplicate, first, "first/libkernel_web.sprx", 0x780000000);
	setup(&unpublished, first, "first/unpublished.prx", 0x790000000);
	unrelated.dynamic_info = std::make_unique<DynamicInfo>();
	// Imports of libkernel do not make an unrelated image a kernel provider.
	unrelated.dynamic_info->import_modules = {{"A", 1, 1, "libkernel"}};
	unrelated.dynamic_info->import_libs = {{"A", 1, "libkernel"}};
	linker.m_programs = {first, second, &caller, &foreign_kernel, &session_kernel, &unrelated};
	static uint32_t next_app_id = 0x70010000;
	if (!check(RegisterRuntimeProcessIdentity(first, next_app_id++) > 0 &&
	           RegisterRuntimeProcessIdentity(second, next_app_id++) > 0,
	           "register isolated query owners")) { return false; }

	struct Guarded {
		uint64_t before = UINT64_C(0x0badf00dcafebeef);
		uint64_t start = UINT64_C(0x1122334455667788);
		uint64_t end = UINT64_C(0x8877665544332211);
		uint64_t after = UINT64_C(0xfeedface12345678);
	};
	static_assert(sizeof(Guarded) == 32 && offsetof(Guarded, start) == 8 && offsetof(Guarded, end) == 16);
	const auto exercise = [&](int error) {
		// Independent named expectations, not the provider's arithmetic formula.
		const int32_t encoded = error == Libs::Posix::POSIX_ENOENT ? Libs::LibKernel::KERNEL_ERROR_ENOENT :
		                        error == Libs::Posix::POSIX_ENOSYS ? Libs::LibKernel::KERNEL_ERROR_ENOSYS :
		                                                                  Libs::LibKernel::KERNEL_ERROR_ESRCH;
		Guarded words;
		const auto saved = words;
		*Libs::Posix::GetErrorAddr() = 1234;
		if (!check(Libs::LibKernel::KernelGetLibkernelTextLocation(&words.start, &words.end) == encoded &&
		           *Libs::Posix::GetErrorAddr() == error && std::memcmp(&words, &saved, sizeof(words)) == 0,
		           "helper must return kernel error, set guest errno and preserve nonzero outputs/canaries")) { return false; }
		*Libs::Posix::GetErrorAddr() = 5678;
		if (!check(Libs::LibKernel::KernelInternalMemoryGetModuleSegmentInfo(&words.start) == 0 &&
		           *Libs::Posix::GetErrorAddr() == error && std::memcmp(&words, &saved, sizeof(words)) == 0,
		           "public zero must discard status only, not errno or sentinel output contents")) { return false; }
		const std::array<uintptr_t, 4> bad {0, 1, UINTPTR_MAX - uintptr_t {3}, UINT64_C(0x0000800000000000)};
		for (auto address: bad) {
			auto* pointer = reinterpret_cast<uint64_t*>(address);
			*Libs::Posix::GetErrorAddr() = 9012;
			if (!check(Libs::LibKernel::KernelGetLibkernelTextLocation(pointer, pointer) == encoded &&
			           Libs::LibKernel::KernelGetLibkernelTextLocation(&words.start, pointer) == encoded &&
			           Libs::LibKernel::KernelGetLibkernelTextLocation(pointer, &words.end) == encoded &&
			           Libs::LibKernel::KernelInternalMemoryGetModuleSegmentInfo(pointer) == 0 &&
			           *Libs::Posix::GetErrorAddr() == error && std::memcmp(&words, &saved, sizeof(words)) == 0,
			           "failure must not access either bad/null/aliased output, including wrapped out+8")) { return false; }
		}
		// Output-copy suppression must not accidentally suppress errno when the
		// caller deliberately aliases the errno slot as its invalid-size output.
		auto* errno_slot = Libs::Posix::GetErrorAddr();
		*errno_slot = 3456;
		if (!check(Libs::LibKernel::KernelInternalMemoryGetModuleSegmentInfo(errno_slot) == 0 && *errno_slot == error,
		           "aliased output must still observe the independent native errno side effect")) { return false; }
		static constexpr uint64_t readonly[] {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
		return check(Libs::LibKernel::KernelInternalMemoryGetModuleSegmentInfo(const_cast<uint64_t*>(readonly)) == 0 &&
		             readonly[0] == UINT64_C(0x0123456789abcdef) && readonly[1] == UINT64_C(0xfedcba9876543210),
		             "failed query must not write read-only outputs");
	};

	SetCurrentProgram(&caller);
	if (!check(linker.QueryKernelTextLocationError() == Libs::Posix::POSIX_ENOENT,
	           "wrong-owner kernel, session-not-mapped kernel and unrelated importer must not satisfy lookup") ||
	    !exercise(Libs::Posix::POSIX_ENOENT)) { return false; }
	SetCurrentProgram(first);
	if (!exercise(Libs::Posix::POSIX_ENOENT)) { return false; }
	SetCurrentProgram(second);
	if (!check(linker.QueryKernelTextLocationError() == Libs::Posix::POSIX_ENOSYS,
	           "same kernel must be recognized only in its owning process") ||
	    !exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	SetCurrentProgram(&caller);
	linker.m_programs.push_back(&local_kernel);
	if (!exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	linker.m_programs.push_back(&duplicate);
	if (!check(linker.QueryKernelTextLocationError() == Libs::Posix::POSIX_ENOSYS,
	           "ambiguous local kernel candidates must be unsupported, not fabricated or absent")) { return false; }
	linker.m_programs.pop_back();
	linker.m_programs.pop_back();
	// Renamed native candidates are recognized by own exports, not imports.
	unrelated.dynamic_info->export_modules = {{"A", 1, 1, "LibKernel"}};
	if (!exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	unrelated.dynamic_info->export_modules.clear();
	unrelated.dynamic_info->export_libs = {{"A", 1, "libkernel"}};
	if (!exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	unrelated.dynamic_info->export_libs.clear();
	// An unmapped record does not establish a mapped provider.
	local_kernel.base_vaddr = local_kernel.mapped_size = 0;
	linker.m_programs.push_back(&local_kernel);
	if (!exercise(Libs::Posix::POSIX_ENOENT)) { return false; }
	// Incomplete nonzero mapping metadata is ambiguity, never a success/absence.
	local_kernel.mapped_size = 0x4000;
	if (!exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	linker.m_programs.pop_back();

	SetCurrentProgram(&unpublished);
	if (!exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	SetCurrentProgram(&caller);
	caller.rt = nullptr;
	if (!exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	caller.rt = &linker;
	const auto original_base = caller.base_vaddr;
	caller.base_vaddr = 0;
	if (!exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	caller.base_vaddr = original_base;
	linker.m_programs.erase(linker.m_programs.begin()); // Active identity alone is not a mapped owner.
	if (!exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	linker.m_programs.insert(linker.m_programs.begin(), first);
	first->application_owner = second;
	if (!exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	first->application_owner = first;
	if (!check(DeactivateRuntimeProcessIdentity(first), "retire first identity") ||
	    !exercise(Libs::Posix::POSIX_ESRCH)) { return false; }
	SetCurrentProgram(&session_kernel);
	if (!exercise(Libs::Posix::POSIX_ENOSYS)) { return false; }
	return check(Common::GetHostLockCriticalCounter()->load() == depth,
	             "kernel text query leaked suspension protection");
}
