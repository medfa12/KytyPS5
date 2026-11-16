// Synthetic audited instruction shapes; no firmware dependency or guest state.
namespace GuestFatalProfileTest {
// Independent native-byte fragments transcribed from the hash-matched image.
struct Window { uint64_t start; uint8_t jump; size_t suffix_size; std::array<uint8_t, 11> suffix; };
constexpr std::array<Window, 5> windows {{
    {0x11ab6, 0x0e, 6, {0x8b, 0x0f, 0x48, 0x8d, 0x47, 0x40}},
    {0x120e9, 0x0e, 5, {0xb8, 1, 0, 0, 0}},
    {0xf3dc, 0x11, 10, {0x41, 0x89, 0xc4, 0x41, 0x8b, 0x86, 0xf4, 3, 0, 0}},
    {0x66ab, 0x0e, 6, {0x8b, 0x83, 0x10, 4, 0, 0}},
    {0x674f, 0x0e, 11, {0x48, 0x83, 0xc4, 8, 0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3}},
}};
std::vector<uint8_t> Image(bool adapt) {
	std::vector<uint8_t> image(0x150c5, 0xcc);
	constexpr std::array<uint8_t, 21> helper {
	    0x55, 0x48, 0x89, 0xe5, 0x64, 0xc7, 0x04, 0x25, 0x28, 0, 0, 0,
	    0x8b, 0, 2, 0xa0, 0xcd, 0x45, 0x90, 0x0f, 0x0b,
	};
	constexpr std::array<uint8_t, 7> caller {0xe8, 0x0b, 0x21, 0, 0, 0x0f, 0x0b};
	std::copy(helper.begin(), helper.end(), image.begin() + 0x150b0);
	std::copy(caller.begin(), caller.end(), image.begin() + 0x12fa0);
	for (const auto& w: windows) {
		auto* code = image.data() + w.start;
		code[0] = 0x85; code[1] = 0xc0; code[2] = 0x74; code[3] = w.jump;
		std::copy(helper.begin() + 4, helper.begin() + 18, code + 4);
		std::copy_n(w.suffix.begin(), w.suffix_size, code + 18);
		if (adapt) { Check("GuestFatalProfile", Loader::PatchGuestDiagnosticStore({code + 4, 14}), "inline FS adaptation"); }
	}
	if (adapt) { Check("GuestFatalProfile", Loader::PatchGuestDiagnosticStore({image.data() + 0x150b4, 17}), "helper FS adaptation"); }
	return image;
}
} // namespace GuestFatalProfileTest

void TestGuestFatalTrapBytes() {
	constexpr const char* test = "GuestFatalTrapBytes";
	// LibcInternal+150b0: prologue, FS diagnostic store, INT45/NOP/UD2.
	constexpr std::array<uint8_t, 21> native {
	    0x55, 0x48, 0x89, 0xe5, 0x64, 0xc7, 0x04, 0x25, 0x28, 0, 0, 0,
	    0x8b, 0x00, 0x02, 0xa0, 0xcd, 0x45, 0x90, 0x0f, 0x0b,
	};
	auto adapted = native;
	Check(test, Loader::PatchGuestDiagnosticStore(std::span<uint8_t>(adapted).subspan(4)), "diagnostic store not adapted");
	for (size_t i = 0; i < native.size(); ++i) {
		Check(test, adapted[i] == (i >= 4 && i < 16 ? 0x90 : native[i]), "adaptation changed prologue or native fatal suffix");
	}
	const std::array<uint8_t, 7> caller {0xe8, 0x0b, 0x21, 0, 0, 0x0f, 0x0b};
	Check(test, Loader::NativeAllocatorFatal::MatchCode(adapted, caller), "audited helper/caller pair not recognized");
	auto damaged = adapted;
	damaged[16] = 0x90;
	Check(test, !Loader::NativeAllocatorFatal::MatchCode(damaged, caller) &&
	            !Loader::NativeAllocatorFatal::MatchCode(std::span<const uint8_t>(adapted).first(20), caller),
	      "altered or truncated fatal helper accepted");
	for (size_t size = 0; size < 12; ++size) {
		auto short_code = native;
		Check(test, !Loader::PatchGuestDiagnosticStore(std::span<uint8_t>(short_code).subspan(4, size)) && short_code == native,
		      "short instruction modified");
	}
	auto other = native;
	other[8] = 0x38;
	const auto original = other;
	Check(test, !Loader::PatchGuestDiagnosticStore(std::span<uint8_t>(other).subspan(4)) && other == original,
	      "unrelated FS field changed");
	Check(test, !Loader::X64InstructionEmulator::TryEmulate(nullptr), "null instruction context accepted");
	const auto raw_profile = GuestFatalProfileTest::Image(false);
	auto profile = GuestFatalProfileTest::Image(true);
	Check(test, Loader::NativeAllocatorFatal::MatchProfile(profile) &&
	            !Loader::NativeAllocatorFatal::MatchProfile(raw_profile) &&
	            !Loader::NativeAllocatorFatal::MatchProfile({}) &&
	            !Loader::NativeAllocatorFatal::MatchProfile(std::span<const uint8_t>(profile).first(profile.size() - 1)),
	      "full profile bounds or adaptation recognition");
	for (const auto& range: Loader::NativeAllocatorFatal::ProfileRanges) {
		for (size_t i = 0; i < range.size; ++i) {
			profile[range.rva + i] ^= 1;
			Check(test, !Loader::NativeAllocatorFatal::MatchProfile(profile), "changed branch/store/trap/successor accepted");
			profile[range.rva + i] ^= 1;
		}
	}
	for (const auto& w: GuestFatalProfileTest::windows) {
		for (size_t i = 0; i < 18 + w.suffix_size; ++i) {
			Check(test, profile[w.start + i] == (i >= 4 && i < 16 ? 0x90 : raw_profile[w.start + i]),
			      "inline adaptation changed branch, trap or native successor");
		}
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CONTEXT context {};
	std::array<uint8_t, 32> instruction {0x0f, 0x0b, 0xc3};
	context.Rip = reinterpret_cast<uintptr_t>(instruction.data());
	context.Rax = 0x12345678;
	const CONTEXT before = context;
	Check(test, !Loader::X64InstructionEmulator::TryEmulate(&context) && std::memcmp(&before, &context, sizeof(context)) == 0,
	      "UD2 was swallowed or changed the saved context");
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
namespace GuestFatalTrapTest {
uintptr_t expected_site = 0, recovery_site = 0;
bool observed = false, refused = false;
LONG CALLBACK Handler(EXCEPTION_POINTERS* exception) {
	if (!expected_site || exception->ContextRecord->Rip != expected_site) { return EXCEPTION_CONTINUE_SEARCH; }
	observed = true;
	refused = !Loader::X64InstructionEmulator::TryEmulate(exception->ContextRecord);
	// Test-owned mapping only: escape the expected fatal instruction after proving
	// production emulation refused it. Production does not install this handler.
	exception->ContextRecord->Rip = recovery_site;
	return EXCEPTION_CONTINUE_EXECUTION;
}
LONG CALLBACK FatalWitness(EXCEPTION_POINTERS* exception) {
	// Child only: a later crash must not masquerade as termination at the audited
	// trap. Forward the first exact fault unchanged; never recover or edit context.
	if (observed || exception->ContextRecord->Rip != expected_site) { ExitProcess(75); }
	observed = true;
	return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace GuestFatalTrapTest

void TestGuestFatalTrapExecution() {
	constexpr const char* test = "GuestFatalTrapExecution";
	using namespace GuestFatalTrapTest;
	struct Mapping {
		uint8_t* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		void* handler = nullptr;
		~Mapping() {
			expected_site = recovery_site = 0;
			if (handler) { RemoveVectoredExceptionHandler(handler); }
			if (code) { VirtualFree(code, 0, MEM_RELEASE); }
		}
	} mapping;
	Check(test, mapping.code != nullptr, "test mapping allocation");
	mapping.handler = AddVectoredExceptionHandler(1, Handler);
	Check(test, mapping.handler != nullptr, "test exception handler registration");
	const std::array<uint8_t, 23> native {
	    0x55, 0x48, 0x89, 0xe5, 0x64, 0xc7, 0x04, 0x25, 0x28, 0, 0, 0,
	    0x8b, 0x00, 0x02, 0xa0, 0xcd, 0x45, 0x90, 0x0f, 0x0b,
	    0xc9, 0xc3, // Test-only recovery: leave; ret.
	};
	std::memcpy(mapping.code, native.data(), native.size());
	Check(test, Loader::PatchGuestDiagnosticStore(std::span<uint8_t>(mapping.code + 4, native.size() - 4)), "adapt actual test mapping");
	expected_site = reinterpret_cast<uintptr_t>(mapping.code + 16);
	recovery_site = reinterpret_cast<uintptr_t>(mapping.code + 21);
	observed = refused = false;
	FlushInstructionCache(GetCurrentProcess(), mapping.code, native.size());
	reinterpret_cast<void (*)()>(mapping.code)();
	Check(test, observed && refused, "adapted allocator fatal helper returned without a refused trap");
	// Also execute the caller's fallback UD2 independently of the INT45 helper.
	mapping.code[0] = 0x0f; mapping.code[1] = 0x0b; mapping.code[2] = 0xc3;
	expected_site = reinterpret_cast<uintptr_t>(mapping.code);
	recovery_site = reinterpret_cast<uintptr_t>(mapping.code + 2);
	observed = refused = false;
	FlushInstructionCache(GetCurrentProcess(), mapping.code, 3);
	reinterpret_cast<void (*)()>(mapping.code)();
	Check(test, observed && refused, "caller fallback UD2 was swallowed");
}

void TestGuestFatalTrapPolicy() {
	constexpr const char* test = "GuestFatalTrapPolicy";
	constexpr uint64_t base = 0x12340000;
	const std::array<uint64_t, 3> sites {base + 0x150c0, base + 0x150c3, base + 0x12fa5};
	Check(test, Loader::NativeAllocatorFatal::Register(sites), "register isolated fatal policy");
	struct Cleanup { ~Cleanup() { Loader::NativeAllocatorFatal::Unregister(base, 0x20000); } } cleanup;
	Check(test, !Loader::NativeAllocatorFatal::Register(sites), "duplicate policy accepted");
	Check(test, !Loader::NativeAllocatorFatal::Contains(base + 0x150c1), "neighbor classified as fatal");
	CONTEXT context {};
	context.Rax = 0x1111; context.Rsp = 0x2222; context.Rbp = 0x3333;
	for (auto pc: sites) {
		for (unsigned kind = 0; kind <= 4; ++kind) {
			context.Rip = pc;
			const CONTEXT before = context;
			Check(test, !Loader::TestNativeFatalHandler(pc, kind, &context) &&
			            std::memcmp(&context, &before, sizeof(context)) == 0,
			      "fatal policy reached recovery or modified context under low-address fault classification");
		}
	}
	Loader::NativeAllocatorFatal::Unregister(base, 0x20000);
	for (auto pc: sites) { Check(test, !Loader::NativeAllocatorFatal::Contains(pc), "unload retained stale fatal site"); }
	const auto profile = GuestFatalProfileTest::Image(true);
	auto damaged_profile = profile;
	damaged_profile[0x675f] = 0x90; // Last new site: reject the entire batch, including the old three.
	Check(test, !Loader::NativeAllocatorFatal::RegisterProfile(base, damaged_profile) &&
	            !Loader::NativeAllocatorFatal::RegisterProfile(0, profile) &&
	            !Loader::NativeAllocatorFatal::RegisterProfile(UINT64_MAX - 0x1000, profile), "invalid profile admitted");
	for (auto rva: Loader::NativeAllocatorFatal::ProfileSites) {
		Check(test, !Loader::NativeAllocatorFatal::Contains(base + rva), "failed profile published a prefix");
	}
	Check(test, !Loader::NativeAllocatorFatal::Register({base + 1, 0, base + 2}) &&
	            !Loader::NativeAllocatorFatal::Register({base + 1, base + 2, base + 1}) &&
	            !Loader::NativeAllocatorFatal::Contains(base + 1), "invalid legacy registration published a prefix");
	Check(test, Loader::NativeAllocatorFatal::Register(sites) &&
	            !Loader::NativeAllocatorFatal::ContainsProfile(base) &&
	            !Loader::NativeAllocatorFatal::RegisterProfile(base, profile), "overlapping legacy/profile admitted");
	for (size_t i = 3; i < Loader::NativeAllocatorFatal::ProfileSites.size(); ++i) {
		Check(test, !Loader::NativeAllocatorFatal::Contains(base + Loader::NativeAllocatorFatal::ProfileSites[i]),
		      "overlap rejection published new sites");
	}
	Loader::NativeAllocatorFatal::Unregister(base, 0x20000);
	Check(test, Loader::NativeAllocatorFatal::RegisterProfile(base, profile) &&
	            Loader::NativeAllocatorFatal::ContainsProfile(base), "register complete eight-site profile");
	for (auto rva: Loader::NativeAllocatorFatal::ProfileSites) {
		const auto pc = base + rva;
		Check(test, !Loader::NativeAllocatorFatal::Contains(pc - 1) && !Loader::NativeAllocatorFatal::Contains(pc + 1) &&
		            !Loader::NativeAllocatorFatal::Contains(pc + 2), "non-trap successor/neighbor classified fatal");
		for (unsigned kind = 0; kind <= 4; ++kind) {
			context.Rip = pc;
			const CONTEXT before = context;
			Check(test, !Loader::TestNativeFatalHandler(pc, kind, &context) &&
			            std::memcmp(&context, &before, sizeof(context)) == 0, "extended fatal policy changed context/recovered");
		}
	}
	Loader::NativeAllocatorFatal::Unregister(base + 0x66bb, 1);
	Check(test, !Loader::NativeAllocatorFatal::ContainsProfile(base) &&
	            Loader::NativeAllocatorFatal::Contains(base + 0x675f), "partial retirement lost unrelated sites");
	Loader::NativeAllocatorFatal::Unregister(base, 0x20000);
	for (auto rva: Loader::NativeAllocatorFatal::ProfileSites) {
		Check(test, !Loader::NativeAllocatorFatal::Contains(base + rva), "extended unload retained stale site");
	}
	// Preserve the legacy capacity, then exercise insufficient aggregate space
	// with free batch records available. Capacity rejection must not leak a prefix.
	constexpr uint64_t capacity_base = 0x30000000;
	struct CapacityCleanup { ~CapacityCleanup() { Loader::NativeAllocatorFatal::Unregister(capacity_base, 0x1000000); } } capacity_cleanup;
	for (uint64_t i = 0; i < 32; ++i) {
		const auto b = capacity_base + i * 0x20000;
		Check(test, Loader::NativeAllocatorFatal::Register({b + 1, b + 2, b + 3}), "legacy 96-slot capacity reduced");
	}
	Check(test, !Loader::NativeAllocatorFatal::Register({capacity_base + 0xf00001, capacity_base + 0xf00002, capacity_base + 0xf00003}),
	      "legacy capacity exceeded");
	Loader::NativeAllocatorFatal::Unregister(capacity_base, 0x1000000);
	for (uint64_t i = 0; i < 11; ++i) {
		Check(test, Loader::NativeAllocatorFatal::RegisterProfile(capacity_base + i * 0x20000, profile), "profile batch capacity");
	}
	constexpr uint64_t extra = capacity_base + 0xe00000;
	constexpr uint64_t candidate = capacity_base + 0xf00000;
	Check(test, Loader::NativeAllocatorFatal::Register({extra + 1, extra + 2, extra + 3}) &&
	            !Loader::NativeAllocatorFatal::RegisterProfile(candidate, profile), "five free slots accepted eight-site profile");
	for (auto rva: Loader::NativeAllocatorFatal::ProfileSites) {
		Check(test, !Loader::NativeAllocatorFatal::Contains(candidate + rva), "capacity failure published prefix");
	}
	Loader::NativeAllocatorFatal::Unregister(extra, 4);
	Check(test, Loader::NativeAllocatorFatal::RegisterProfile(candidate, profile) &&
	            Loader::NativeAllocatorFatal::ContainsProfile(candidate), "retired capacity not reusable");
}

int GuestFatalTrapChild(bool ud2) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!code) { return 70; }
	const std::array<uint8_t, 23> helper {
	    0x55, 0x48, 0x89, 0xe5, 0x64, 0xc7, 0x04, 0x25, 0x28, 0, 0, 0,
	    0x8b, 0, 2, 0xa0, 0xcd, 0x45, 0x90, 0x0f, 0x0b, 0xc9, 0xc3,
	};
	std::memcpy(code, helper.data(), helper.size());
	if (!Loader::PatchGuestDiagnosticStore({code + 4, helper.size() - 4})) { return 71; }
	const auto base = reinterpret_cast<uint64_t>(code);
	if (!Loader::NativeAllocatorFatal::Register({base + 16, base + 19, base + 64}) ||
	    !Loader::TestInstallNativeFatalHandler()) { return 72; }
	code[64] = 0x0f; code[65] = 0x0b; code[66] = 0xc3;
	FlushInstructionCache(GetCurrentProcess(), code, 67);
	reinterpret_cast<void (*)()>(ud2 ? code + 64 : code)();
	return 73; // Continuation is always failure; parent requires an actual fault exit.
}

int GuestFatalProfileChild(const char* site) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	char* end = nullptr;
	const uint64_t rva = std::strtoull(site, &end, 16);
	if (!end || *end != 0 || std::none_of(GuestFatalProfileTest::windows.begin(), GuestFatalProfileTest::windows.end(),
	    [rva](const auto& window) { return window.start + 16 == rva; })) { return 70; }
	const auto image = GuestFatalProfileTest::Image(true);
	auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, image.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!code) { return 70; }
	std::memcpy(code, image.data(), image.size());
	// Test-owned trampoline enters the real conditional error block with EAX=1.
	// All validated windows, including native success successors, stay untouched.
	code[0] = 0xb8; code[1] = 1; code[2] = code[3] = code[4] = 0;
	code[5] = 0xe9;
	const int32_t displacement = static_cast<int32_t>(rva - 16 - 10);
	std::memcpy(code + 6, &displacement, sizeof(displacement));
	const auto base = reinterpret_cast<uint64_t>(code);
	if (!Loader::NativeAllocatorFatal::RegisterProfile(base, {code, image.size()}) ||
	    !Loader::TestInstallNativeFatalHandler()) { return 72; }
	GuestFatalTrapTest::expected_site = base + rva;
	GuestFatalTrapTest::observed = false;
	if (!AddVectoredExceptionHandler(1, GuestFatalTrapTest::FatalWitness)) { return 72; }
	FlushInstructionCache(GetCurrentProcess(), code, image.size());
	reinterpret_cast<void (*)()>(code)();
	return 73;
}

void TestGuestFatalTrapSubprocess() {
	constexpr const char* test = "GuestFatalTrapSubprocess";
	wchar_t exe[32768] {};
	Check(test, GetModuleFileNameW(nullptr, exe, 32768) != 0, "resolve fixture executable");
	struct Case { const wchar_t* arguments; const char* label; bool ud2; };
	const std::array<Case, 7> cases {{
	    {L"--guest-fatal-int45-child", "legacy-int45", false},
	    {L"--guest-fatal-ud2-child", "legacy-ud2", true},
	    {L"--guest-fatal-profile-child 11ac6", "stats-lock", false},
	    {L"--guest-fatal-profile-child 120f9", "stats-unlock", false},
	    {L"--guest-fatal-profile-child f3ec", "destroy", false},
	    {L"--guest-fatal-profile-child 66bb", "diagnostic-lock", false},
	    {L"--guest-fatal-profile-child 675f", "diagnostic-unlock", false},
	}};
	for (const auto& c: cases) {
		std::wstring command = L"\"" + std::wstring(exe) + L"\" " + c.arguments;
		STARTUPINFOW startup {}; startup.cb = sizeof(startup);
		PROCESS_INFORMATION child {};
		Check(test, CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		                          nullptr, nullptr, &startup, &child) != 0, "launch fatal subprocess");
		const auto wait = WaitForSingleObject(child.hProcess, 5000);
		if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 74); WaitForSingleObject(child.hProcess, 1000); }
		DWORD result = 0;
		const bool got_result = GetExitCodeProcess(child.hProcess, &result) != 0;
		CloseHandle(child.hThread); CloseHandle(child.hProcess);
		std::printf("fatal fixture %s exit=0x%08lx\n", c.label, static_cast<unsigned long>(result));
		Check(test, wait == WAIT_OBJECT_0 && got_result &&
		            (c.ud2 ? result == EXCEPTION_ILLEGAL_INSTRUCTION :
		                   result == EXCEPTION_ACCESS_VIOLATION || result == EXCEPTION_PRIV_INSTRUCTION),
		      "fatal subprocess continued, timed out, or exited without the expected fault");
	}
}
#endif
