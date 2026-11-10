// Parent integration: include runtimeCrtExit.h at runtimeLinker.cpp's TOP LEVEL,
// declare RuntimeLinker::TestRuntimeCrtSync under the VM-test macro, then include
// this file alongside the other Loader fixtures. No production admission override.
namespace RuntimeCrtSyncFixture {
enum Export : unsigned { Ponce, Sonce, Acquire, Release, Abort, AtExit, CxaAt, Finalize, Exit, MainReturn, ThreadExit, ExportCount };
using VoidFunction = void(KYTY_SYSV_ABI*)();
using CxaFunction = void(KYTY_SYSV_ABI*)(void*);
using PonceFunction = int(KYTY_SYSV_ABI*)(void*, VoidFunction);
using SonceFunction = int(KYTY_SYSV_ABI*)(int*, RuntimeCrt::StdOnceFunction, void*);
using GuardFunction = int(KYTY_SYSV_ABI*)(uint64_t*);
using GuardFinishFunction = void(KYTY_SYSV_ABI*)(uint64_t*);
using AtExitFunction = int(KYTY_SYSV_ABI*)(VoidFunction);
using CxaAtFunction = int(KYTY_SYSV_ABI*)(CxaFunction, void*, void*);
using FinalizeFunction = void(KYTY_SYSV_ABI*)(void*);
enum class Operation { PthreadOnce, StdOnce, GuardRelease, GuardAbort, GuardExit, GuardRecursive, ExitProcess };
enum class Action { Return, Fail, ThreadExit, Reenter };
struct Call {
	Program* program = nullptr;
	std::shared_ptr<RuntimeGuestExecution> execution;
	uint64_t* entries = nullptr;
	uint64_t callback = 0;
	void* control = nullptr;
	Operation operation = Operation::PthreadOnce;
	Action action = Action::Return;
	int value = 1;
	std::atomic_bool entered {false};
	std::atomic_bool allow {false};
	std::atomic_bool done {false};
	bool hold = false;
	unsigned calls = 0;
	unsigned continued = 0;
	unsigned destroyed = 0;
	unsigned cleanup_acquired = 0;
	bool cleanup_returned = false;
	bool typed = true;
	int observed = -1;
	RuntimeGuestResult result {};
	std::vector<int> log;
	uint64_t image = 0;
	uint64_t outer = 0;
	uint64_t once_target = 0;
};
static thread_local Call* active = nullptr;
static thread_local Call* guard_cleanup = nullptr;
static void Arrive(Call& call) {
	++call.calls;
	call.typed &= Common::GetHostLockCriticalCounter()->load() == 0;
	call.entered.store(true, std::memory_order_release);
	while (call.hold && !call.allow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}
static void CallbackAction(Call& call) {
	if (call.action == Action::ThreadExit) {
		reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(call.entries[ThreadExit])(reinterpret_cast<void*>(77));
	} else if (call.action == Action::Fail) {
		call.execution->Fail(GuestFailureReason::Explicit, 0xb2);
		call.execution->FailureFunction()(); // No host objects/frames below the callback boundary.
	}
}
static void KYTY_SYSV_ABI OnceCallback() { Arrive(*active); CallbackAction(*active); }
static int KYTY_SYSV_ABI StdCallback(void* first, void* argument, void** output) {
	auto& call = *active;
	call.typed &= first == nullptr && argument == call.control && output && *output == nullptr;
	if (output) { *output = reinterpret_cast<void*>(0xb2); }
	Arrive(call); CallbackAction(call);
	if (call.action == Action::Reenter) {
		reinterpret_cast<SonceFunction>(call.entries[Sonce])(static_cast<int*>(call.control),
		    reinterpret_cast<RuntimeCrt::StdOnceFunction>(call.callback), call.control);
	}
	return call.value;
}
static void KYTY_SYSV_ABI Fini() {
	active->typed &= Common::GetHostLockCriticalCounter()->load() == 0; active->log.push_back(2);
}
static void KYTY_SYSV_ABI AddedDuringExit() { active->log.push_back(5); }
static void KYTY_SYSV_ABI CxaFirst(void* argument) {
	active->typed &= argument == reinterpret_cast<void*>(active->image + 0x5000) && Common::GetHostLockCriticalCounter()->load() == 0;
	active->log.push_back(3);
}
static void KYTY_SYSV_ABI CxaReentrant(void*) {
	auto& call = *active;
	call.log.push_back(4);
	reinterpret_cast<AtExitFunction>(call.entries[AtExit])(reinterpret_cast<VoidFunction>(call.image + 0x1a0));
	reinterpret_cast<FinalizeFunction>(call.entries[Finalize])(nullptr);
}
static void KYTY_SYSV_ABI CxaFiltered(void*) { active->log.push_back(6); }
static void KYTY_SYSV_ABI CxaThreadExit(void*) {
	active->log.push_back(7);
	reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(active->entries[ThreadExit])(reinterpret_cast<void*>(99));
}
static void KYTY_SYSV_ABI GuardCleanup() {
	if (!guard_cleanup) { return; }
	auto& call = *guard_cleanup;
	const auto acquired = reinterpret_cast<GuardFunction>(call.entries[Acquire])(static_cast<uint64_t*>(call.control));
	if (acquired) {
		++call.cleanup_acquired;
		reinterpret_cast<GuardFinishFunction>(call.entries[Release])(static_cast<uint64_t*>(call.control));
	}
	call.cleanup_returned = true;
}
static void KYTY_SYSV_ABI ExitCaller() {
	reinterpret_cast<void(KYTY_SYSV_ABI*)(int)>(active->entries[Exit])(31);
	++active->continued; // Must be unreachable through the nonreturning barrier.
}
static uint64_t KYTY_SYSV_ABI ExitHost(void*) {
	auto& call = *active;
	struct Pin { Call& call; ~Pin() { ++call.destroyed; } } pin {call};
	call.execution->InvokeCallback(call.image + 0x1e0);
	return 0;
}
static uint64_t KYTY_SYSV_ABI OnceHost(void*) {
	auto& call = *active;
	struct Pin { Call& call; ~Pin() { ++call.destroyed; } } pin {call};
	return reinterpret_cast<PonceFunction>(call.once_target)(call.control, reinterpret_cast<VoidFunction>(call.callback));
}
static uint64_t KYTY_SYSV_ABI GuestCall(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	if (call.operation == Operation::PthreadOnce) {
		call.observed = call.outer ? static_cast<int>(reinterpret_cast<GuestBoundaryFunction>(call.outer)(nullptr)) :
		    reinterpret_cast<PonceFunction>(call.entries[Ponce])(call.control, reinterpret_cast<VoidFunction>(call.callback));
	} else if (call.operation == Operation::StdOnce) {
		call.observed = reinterpret_cast<SonceFunction>(call.entries[Sonce])(static_cast<int*>(call.control),
		    reinterpret_cast<RuntimeCrt::StdOnceFunction>(call.callback), call.control);
	} else if (call.operation == Operation::ExitProcess) {
		reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(call.outer)(nullptr);
	} else {
		call.observed = reinterpret_cast<GuardFunction>(call.entries[Acquire])(static_cast<uint64_t*>(call.control));
		if (call.observed) {
			Arrive(call);
			if (call.operation == Operation::GuardExit) {
				reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(call.entries[ThreadExit])(reinterpret_cast<void*>(81));
			} else if (call.operation == Operation::GuardRecursive) {
				reinterpret_cast<GuardFunction>(call.entries[Acquire])(static_cast<uint64_t*>(call.control));
			} else {
				reinterpret_cast<GuardFinishFunction>(call.entries[call.operation == Operation::GuardAbort ? Abort : Release])(static_cast<uint64_t*>(call.control));
			}
		}
	}
	++call.continued;
	return static_cast<uint32_t>(call.observed);
}
static void* KYTY_SYSV_ABI Worker(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	active = &call;
	if (call.operation == Operation::GuardExit) { guard_cleanup = &call; }
	call.result = call.execution->Invoke(GuestCall, &call);
	active = nullptr;
	call.done.store(true, std::memory_order_release);
	return nullptr; // Runtime worker root consumes tagged thread exit, not this coordinator.
}
struct Registration { Export kind; uint64_t target; uint64_t callback; void* argument; void* dso; unsigned continued = 0; };
static uint64_t KYTY_SYSV_ABI GuestRegister(void* opaque) {
	auto& registration = *static_cast<Registration*>(opaque);
	const auto value = registration.kind == AtExit ? reinterpret_cast<AtExitFunction>(registration.target)(
	    reinterpret_cast<VoidFunction>(registration.callback)) : reinterpret_cast<CxaAtFunction>(registration.target)(
	    reinterpret_cast<CxaFunction>(registration.callback), registration.argument, registration.dso);
	++registration.continued; return static_cast<uint32_t>(value);
}
struct Finalization { uint64_t entry; void* dso; };
static uint64_t KYTY_SYSV_ABI GuestFinalize(void* opaque) {
	auto& finalize = *static_cast<Finalization*>(opaque);
	reinterpret_cast<FinalizeFunction>(finalize.entry)(finalize.dso); return 0;
}
static uint64_t KYTY_SYSV_ABI GuestMainReturn(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	reinterpret_cast<void(KYTY_SYSV_ABI*)(int)>(call.entries[MainReturn])(9); return 0;
}
template <typename Predicate> static bool Until(Predicate condition) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!condition() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return condition();
}
struct Workers {
	std::vector<std::pair<Libs::LibKernel::Pthread, Call*>> threads;
	~Workers() { for (auto& item: threads) { item.second->allow.store(true); } Join(); }
	bool Start(Call& call) {
		RuntimeLinker::SetCurrentProgram(call.program);
		Libs::LibKernel::Pthread thread = nullptr;
		if (Libs::LibKernel::PthreadCreateRuntimeCoordinator(&thread, nullptr, Worker, &call, "crt-sync-fixture")) { return false; }
		threads.emplace_back(thread, &call); return true;
	}
	bool Join() {
		bool ok = true;
		for (auto& item: threads) {
			if (item.first) {
				if (Libs::LibKernel::PthreadJoin(item.first, nullptr) == 0) { item.first = nullptr; }
				else { ok = false; }
			}
		}
		return ok;
	}
};
}

bool RuntimeLinker::TestRuntimeCrtSync(std::string* failure) {
	using namespace RuntimeCrtSyncFixture;
	using namespace Libs::LibKernel;
	const auto check = [&](bool value, const char* message) { if (!value && failure) { *failure = message; } return value; };
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	constexpr unsigned Count = 14;
	constexpr uint64_t ImageSize = 0xc000;
	Program programs[Count];
	Call calls[32]; // All contexts outlive their actual native joins, including early-return cleanup.
	std::vector<std::unique_ptr<HostCallReturnThunk>> gateways;
	struct Cleanup {
		RuntimeLinker& linker; Program* programs; std::vector<Program*> original;
		Program* previous = RuntimeLinker::GetCurrentProgram(); std::filesystem::path root;
		~Cleanup() {
			active = nullptr; RuntimeLinker::SetCurrentProgram(previous);
			for (unsigned i = 0; i != Count; ++i) { if (auto execution = GetApplicationExecution(&programs[i])) { execution->RequestStop(); } }
			linker.m_programs = std::move(original);
			for (unsigned i = 0; i != Count; ++i) {
				programs[i].elf.reset();
				if (programs[i].base_vaddr) { Memory::FreeGuestMemory(programs[i].base_vaddr, ImageSize); }
			}
			std::error_code error; if (!root.empty()) { std::filesystem::remove_all(root, error); }
		}
	} cleanup {linker, programs, linker.m_programs};
	Workers workers; // Destroyed BEFORE Cleanup and all Call/Program storage.
	cleanup.root = std::filesystem::absolute("_TempData") /
	    ("crt-sync-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root);
	const auto path = cleanup.root / "crt.prx";
	Elf64_Ehdr header {}; std::memcpy(header.e_ident, "\177ELF", 4);
	header.e_ident[EI_CLASS] = ELFCLASS64; header.e_ident[EI_DATA] = ELFDATA2LSB;
	header.e_ident[EI_VERSION] = EV_CURRENT; header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
	header.e_type = ET_DYNAMIC; header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
	header.e_ehsize = sizeof(header); header.e_phoff = sizeof(header); header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 2;
	Elf64_Phdr segments[2] {};
	segments[0].p_type = PT_LOAD; segments[0].p_flags = PF_R | PF_X; segments[0].p_memsz = 0x4000;
	segments[1].p_type = PT_LOAD; segments[1].p_flags = PF_R | PF_W; segments[1].p_vaddr = 0x4000; segments[1].p_memsz = 0x8000;
	Common::File file;
	if (!check(file.Create(path), "create bounded CRT fixture metadata")) { return false; }
	uint32_t written = 0; file.Write(&header, sizeof(header), &written); file.Write(segments, sizeof(segments), &written); file.Close();
	const std::array<uint64_t, 10> functions {reinterpret_cast<uint64_t>(OnceCallback), reinterpret_cast<uint64_t>(StdCallback),
	    reinterpret_cast<uint64_t>(Fini), reinterpret_cast<uint64_t>(CxaFirst), reinterpret_cast<uint64_t>(CxaReentrant),
	    reinterpret_cast<uint64_t>(AddedDuringExit), reinterpret_cast<uint64_t>(CxaFiltered), reinterpret_cast<uint64_t>(ExitCaller),
	    reinterpret_cast<uint64_t>(CxaThreadExit), reinterpret_cast<uint64_t>(GuardCleanup)};
	for (unsigned i = 0; i != Count; ++i) {
		auto& program = programs[i]; program.rt = &linker; program.unique_id = 991000 + i; program.file_name = path;
		program.application_owner = i == 13 ? nullptr : i == 12 ? &programs[7] : &program;
		program.elf = std::make_unique<Elf64>(); program.elf->Open(path);
		if (!check(program.elf->IsValid(), "parse private CRT fixture segments")) { return false; }
		program.base_vaddr = Memory::AllocateProgramMemory(0, ImageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "crt_sync_fixture");
		program.base_size = program.mapped_size = ImageSize;
		if (!check(program.base_vaddr != 0, "allocate trusted private fixture code/data")) { return false; }
		for (unsigned j = 0; j != functions.size(); ++j) {
			auto* code = reinterpret_cast<uint8_t*>(program.base_vaddr + 0x100 + j * 0x20);
			code[0] = 0x48; code[1] = 0xb8; std::memcpy(code + 2, &functions[j], 8); code[10] = 0xff; code[11] = 0xe0;
		}
		std::memset(reinterpret_cast<void*>(program.base_vaddr + 0x4000), 0, 0x8000);
		Memory::SetProgramMemoryProtection(program.base_vaddr, 0x4000, Common::VirtualMemory::Mode::ExecuteRead);
		Memory::SetProgramMemoryProtection(program.base_vaddr + 0x4000, 0x8000, Common::VirtualMemory::Mode::ReadWrite);
		Common::VirtualMemory::FlushInstructionCache(program.base_vaddr, 0x4000);
		if (program.application_owner == &program && !check(InitializeApplicationExecution(&program), "initialize real CRT owner")) { return false; }
		linker.m_programs.push_back(&program);
	}
	Libs::InitLibKernel_1(linker.Symbols()); Libs::LibC::InitLibC_1(linker.Symbols());
	const std::array<const char*, ExportCount> nids {"14bOACANTBo", "DiGVep5yB5w", "3GPpjQdAMTw", "9rAeANT2tyE", "2emaaluWzUw",
	    "8G2LB+A3rzg", "tsvEmnenz48", "H2e8t5ScQGc", "uMei1W9uyNo", "XKRegsFpEpk", "3kg7rT0NQIs"};
	const std::array<const char*, ExportCount> expressions {"Posix::pthread_once", "LibC::std_execute_once", "LibcInternal::CxaGuardAcquire",
	    "LibcInternal::CxaGuardRelease", "LibcInternal::CxaGuardAbort", "LibC::atexit", "LibC::cxa_atexit", "LibC::cxa_finalize",
	    "LibC::exit", "LibC::catchReturnFromMain", "LibKernel::PthreadExit"};
	uint64_t entries[Count][ExportCount] {};
	uint64_t targets[ExportCount] {};
	SymbolRecord registrations[ExportCount];
	const auto gateway = [&](unsigned index, uint64_t target) {
		const auto execution = GetApplicationExecution(&programs[index]);
		auto thunk = HostCallReturnThunk::Create(execution->Boundary(), target, execution.get(), RuntimeGuestExecution::EnterHostCall,
		    RuntimeGuestExecution::LeaveHostCall);
		if (!thunk) { return uint64_t {0}; }
		const auto address = thunk->GetAddress(); gateways.push_back(std::move(thunk)); return address;
	};
	for (unsigned j = 0; j != ExportCount; ++j) {
		const char* library = j == Ponce || j == ThreadExit ? "libkernel" :
		    (j >= Sonce && j <= Abort) || j == CxaAt || j == Finalize ? "LibcInternal" : "libc";
		const auto* record = linker.Symbols()->FindRegistration({nids[j], library, 1, library, 1, 1, SymbolType::Func}, expressions[j]);
		if (!check(record != nullptr, "find exact production sync/CRT registration, no fake wrapper")) { return false; }
		targets[j] = record->vaddr;
		registrations[j] = *record;
		for (unsigned i = 0; i != Count - 1; ++i) {
			entries[i][j] = gateway(i, targets[j]);
			if (!check(entries[i][j] != 0, "construct production return barrier around real registered wrapper")) { return false; }
		}
	}
	const auto configure = [&](unsigned call_index, unsigned owner, Operation operation) -> Call& {
		auto& call = calls[call_index]; call.program = &programs[owner]; call.execution = GetApplicationExecution(call.program);
		call.entries = entries[owner]; call.image = programs[owner].base_vaddr;
		call.operation = operation; call.control = reinterpret_cast<void*>(programs[owner].base_vaddr + 0x4100);
		call.callback = call.image + (operation == Operation::StdOnce ? 0x120 : 0x100); return call;
	};
	const auto parked = [&](Call& call, RuntimeCrt::SyncKind kind) { return Until([&] { return RuntimeCrt::SyncWaiters(call.control, kind) != 0; }); };
	// Once: true contended success, no callback under registry critical guard.
	auto& once_a = configure(0, 0, Operation::PthreadOnce); once_a.hold = true;
	auto& once_b = configure(1, 0, Operation::PthreadOnce);
	std::memset(static_cast<uint8_t*>(once_a.control) + 4, 0xa5, 12);
	if (!check(workers.Start(once_a) && Until([&] { return once_a.entered.load(); }) && workers.Start(once_b) &&
	    parked(once_b, RuntimeCrt::SyncKind::PthreadOnce), "park an actual pthread-once contender behind a held initializer")) { return false; }
	once_a.allow.store(true);
	if (!check(workers.Join() && once_a.result.Returned() && once_b.result.Returned() && once_a.calls == 1 && once_b.calls == 0 &&
	    once_a.typed && once_a.observed == 0 && once_b.observed == 0 && RuntimeCrt::ReadState(once_a.control) == 1,
	    "pthread once winner publishes DONE once, contender returns only after completion")) { return false; }
	for (unsigned byte = 4; byte != 16; ++byte) {
		if (!check(static_cast<uint8_t*>(once_a.control)[byte] == 0xa5, "pthread once preserves all non-state ABI bytes")) { return false; }
	}
	// Owner-failing initializer: waiter leaves despite initializer state and never signals.
	auto& failing = configure(2, 1, Operation::PthreadOnce); failing.hold = true; failing.action = Action::Fail;
	auto& failed_waiter = configure(3, 1, Operation::PthreadOnce);
	if (!check(workers.Start(failing) && Until([&] { return failing.entered.load(); }) && workers.Start(failed_waiter) &&
	    parked(failed_waiter, RuntimeCrt::SyncKind::PthreadOnce), "park waiter before initializer failure")) { return false; }
	failing.allow.store(true);
	if (!check(workers.Join() && !failing.result.Returned() && !failed_waiter.result.Returned() && failed_waiter.calls == 0 &&
	    failed_waiter.continued == 0 && failing.result.failure.reason == GuestFailureReason::Explicit && RuntimeCrt::ReadState(failing.control) == 0,
	    "failed once resets NEEDS_INIT, wakes/cancels real waiter, does not manufacture DONE")) { return false; }
	// Typed std-once callback false result leaves the healthy control retryable.
	auto& std_false = configure(4, 2, Operation::StdOnce); std_false.hold = true; std_false.value = 0;
	auto& std_retry = configure(5, 2, Operation::StdOnce);
	if (!check(workers.Start(std_false) && Until([&] { return std_false.entered.load(); }) && workers.Start(std_retry) &&
	    parked(std_retry, RuntimeCrt::SyncKind::StdOnce), "contend on typed std-once")) { return false; }
	std_false.allow.store(true);
	if (!check(workers.Join() && std_false.result.Returned() && std_false.observed == 0 && std_retry.result.Returned() && std_retry.observed == 1 &&
	    std_false.typed && std_retry.typed && std_false.calls == 1 && std_retry.calls == 1 && RuntimeCrt::ReadState(std_false.control) == 1,
	    "typed int(NULL,arg,void**) callback preserves ABI and false-result retry")) { return false; }
	auto& std_exit = configure(6, 3, Operation::StdOnce); std_exit.hold = true; std_exit.action = Action::ThreadExit;
	auto& std_after_exit = configure(7, 3, Operation::StdOnce);
	if (!check(workers.Start(std_exit) && Until([&] { return std_exit.entered.load(); }) && workers.Start(std_after_exit) &&
	    parked(std_after_exit, RuntimeCrt::SyncKind::StdOnce), "contend before actual-thread initializer exit")) { return false; }
	std_exit.allow.store(true);
	if (!check(workers.Join() && std_exit.result.ThreadExited() && std_exit.continued == 0 && !programs[3].application_execution->Failure() &&
	    std_after_exit.result.Returned() && std_after_exit.calls == 1, "thread exit stays tagged, does not fail peers, permits once retry")) { return false; }
	// Guard abort releases exclusion WITHOUT publishing the initialized byte.
	auto& guard_abort = configure(8, 4, Operation::GuardAbort); guard_abort.hold = true;
	auto& guard_retry = configure(9, 4, Operation::GuardRelease);
	if (!check(workers.Start(guard_abort) && Until([&] { return guard_abort.entered.load(); }) && workers.Start(guard_retry) &&
	    parked(guard_retry, RuntimeCrt::SyncKind::Guard), "park a real C++ guard contender")) { return false; }
	guard_abort.allow.store(true);
	if (!check(workers.Join() && guard_abort.result.Returned() && guard_retry.result.Returned() && guard_retry.calls == 1 &&
	    *reinterpret_cast<uint8_t*>(guard_retry.control) == 1, "guard abort permits exactly one waiter to initialize and release")) { return false; }
	auto& guard_exit = configure(10, 5, Operation::GuardExit); guard_exit.hold = true;
	auto& guard_after_exit = configure(11, 5, Operation::GuardRelease);
	if (!check(guard_exit.execution->RegisterThreadDestructor(guard_exit.image + 0x220),
	    "register private thread-cleanup callback that reenters the abandoned guard")) { return false; }
	if (!check(workers.Start(guard_exit) && Until([&] { return guard_exit.entered.load(); }) && workers.Start(guard_after_exit) &&
	    parked(guard_after_exit, RuntimeCrt::SyncKind::Guard), "contend behind guard initializer that will exit its actual worker")) { return false; }
	guard_exit.allow.store(true);
	if (!check(workers.Join() && guard_exit.result.ThreadExited() && guard_after_exit.result.Returned() && guard_exit.cleanup_returned &&
	    guard_after_exit.calls + guard_exit.cleanup_acquired == 1 && *static_cast<uint8_t*>(guard_exit.control) == 1 &&
	    !programs[5].application_execution->Failure(),
	    "GATE: parent root abandons incomplete guards BEFORE guest thread cleanup; cleanup/peer retry once without owner failure")) { return false; }
	auto& guard_stopping = configure(12, 6, Operation::GuardRelease); guard_stopping.hold = true;
	auto& stopped_waiter = configure(13, 6, Operation::GuardRelease);
	if (!check(workers.Start(guard_stopping) && Until([&] { return guard_stopping.entered.load(); }) && workers.Start(stopped_waiter) &&
	    parked(stopped_waiter, RuntimeCrt::SyncKind::Guard), "park guard waiter for terminal stop")) { return false; }
	programs[6].application_execution->RequestStop();
	const bool waiter_left_before_initializer = Until([&] { return stopped_waiter.done.load(); });
	guard_stopping.allow.store(true);
	if (!check(workers.Join() && waiter_left_before_initializer && stopped_waiter.result.stopped && stopped_waiter.continued == 0 &&
	    *reinterpret_cast<uint8_t*>(guard_stopping.control) == 0, "stopped guard waiter leaves while initializer is held; no false initialized byte")) { return false; }
	auto& once_stopping = configure(18, 11, Operation::PthreadOnce); once_stopping.hold = true;
	auto& once_stopped_waiter = configure(19, 11, Operation::PthreadOnce);
	if (!check(workers.Start(once_stopping) && Until([&] { return once_stopping.entered.load(); }) && workers.Start(once_stopped_waiter) &&
	    parked(once_stopped_waiter, RuntimeCrt::SyncKind::PthreadOnce), "park once waiter while actual initializer remains live")) { return false; }
	programs[11].application_execution->RequestStop();
	const bool once_waiter_left = Until([&] { return once_stopped_waiter.done.load(); });
	once_stopping.allow.store(true);
	if (!check(workers.Join() && once_waiter_left && once_stopped_waiter.result.stopped && once_stopped_waiter.calls == 0 &&
	    once_stopped_waiter.continued == 0 && RuntimeCrt::ReadState(once_stopping.control) == 0,
	    "stopped pthread-once contender unlinks before held initializer returns, without guest signal dispatch")) { return false; }
	// Ordinary exit: combine cxa + atexit order, DSO filter and reentrant finalize.
	auto& exiting = configure(14, 7, Operation::ExitProcess);
	const auto entry_exit = RuntimeCrt::PrepareEntryExitCallback(exiting.execution);
	if (!check(entry_exit != 0, "prepare owner-pinned entry callback, never a shared raw host pointer")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[12]); // Private module shares executable CRT state.
	// Registration helpers use an actual guest boundary/registered gateway too.
	const auto register_one = [&](unsigned owner, Export kind, uint64_t callback, void* dso = nullptr, void* argument = nullptr) {
		RuntimeLinker::SetCurrentProgram(&programs[owner]); Registration registration {kind, entries[owner][kind], callback, argument, dso};
		const auto result = GetApplicationExecution(&programs[owner])->Invoke(GuestRegister, &registration);
		return result.Returned() && result.call.value == 0 && registration.continued == 1;
	};
	if (!check(register_one(12, AtExit, entry_exit) && register_one(7, AtExit, exiting.image + 0x140) &&
	    register_one(7, CxaAt, exiting.image + 0x160, nullptr, reinterpret_cast<void*>(exiting.image + 0x5000)) &&
	    register_one(7, CxaAt, exiting.image + 0x180) &&
	    register_one(7, CxaAt, exiting.image + 0x1c0, reinterpret_cast<void*>(0x1234)), "register real owner/module CRT records")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[7]);
	const auto main_returned = exiting.execution->Invoke(GuestMainReturn, &exiting); int main_status = 0;
	if (!check(main_returned.Returned() && RuntimeCrt::MainReturnStatus(exiting.execution, &main_status) && main_status == 9 &&
	    RuntimeCrt::PendingExitRecords(exiting.execution) == 5 && !programs[7].application_exit_requested.load(),
	    "registered catchReturnFromMain records status without prematurely consuming callbacks or closing admission")) { return false; }
	// Filtered cxa_finalize through its registered wrapper on this actual thread.
	Finalization filtered {entries[7][Finalize], reinterpret_cast<void*>(0x1234)};
	RuntimeLinker::SetCurrentProgram(&programs[7]); active = &exiting;
	const auto filtered_result = exiting.execution->Invoke(GuestFinalize, &filtered); active = nullptr;
	if (!check(filtered_result.Returned() && exiting.log == std::vector<int> {6} && RuntimeCrt::PendingExitRecords(exiting.execution) == 4,
	    "DSO-filtered finalize consumes only matching CXA record, not executable fini/entry callback")) { return false; }
	exiting.outer = gateway(7, reinterpret_cast<uint64_t>(ExitHost));
	if (!check(exiting.outer && workers.Start(exiting) && workers.Join() && exiting.result.stopped && !exiting.result.failure &&
	    exiting.destroyed == 1 && exiting.continued == 0 && exiting.typed && exiting.log == std::vector<int> {6, 4, 3, 5, 2} &&
	    RuntimeCrt::EntryNotifications(exiting.execution) == 1 && RuntimeCrt::PendingExitRecords(exiting.execution) == 0 &&
	    programs[7].application_exit_requested.load() && programs[7].application_exit_code.load() == 31,
	    "ordinary exit drains reverse/reentrant CRT once, fini before supplied exit notification, outer host RAII before nonreturn")) { return false; }
	// A destructor's tagged thread exit interrupts ordinary finalization without
	// becoming fatal process failure; a later worker resumes only UNCLAIMED work.
	auto& destructor_exit = configure(21, 4, Operation::ExitProcess);
	if (!check(register_one(4, AtExit, destructor_exit.image + 0x140) && register_one(4, CxaAt, destructor_exit.image + 0x200),
	    "register ordinary finalizer that exits its current thread")) { return false; }
	destructor_exit.outer = gateway(4, reinterpret_cast<uint64_t>(ExitHost));
	if (!check(destructor_exit.outer && workers.Start(destructor_exit) && workers.Join() && destructor_exit.result.ThreadExited() &&
	    destructor_exit.destroyed == 1 && destructor_exit.continued == 0 && destructor_exit.log == std::vector<int> {7} &&
	    RuntimeCrt::PendingExitRecords(destructor_exit.execution) == 1 && !destructor_exit.execution->StopRequested(),
	    "tagged destructor thread exit returns through CRT host RAII, retains remaining work and does not fail owner")) { return false; }
	auto& resumed_exit = configure(22, 4, Operation::ExitProcess); resumed_exit.outer = gateway(4, reinterpret_cast<uint64_t>(ExitHost));
	if (!check(resumed_exit.outer && workers.Start(resumed_exit) && workers.Join() && resumed_exit.result.stopped && !resumed_exit.result.failure &&
	    resumed_exit.log == std::vector<int> {2} && resumed_exit.destroyed == 1 && RuntimeCrt::PendingExitRecords(resumed_exit.execution) == 0,
	    "later ordinary exit resumes remaining callbacks but never replays the thread-exiting destructor")) { return false; }
	// Fatal stop is NOT an ordinary finalization request. Queued cleanup stays pinned.
	auto& fatal = configure(15, 8, Operation::ExitProcess);
	if (!check(register_one(8, AtExit, fatal.image + 0x140), "register fatal-owner cleanup before failure")) { return false; }
	fatal.execution->Fail(GuestFailureReason::Explicit, 0xfab2);
	RuntimeLinker::SetCurrentProgram(&programs[8]); active = &fatal;
	reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(targets[Finalize])(nullptr);
	reinterpret_cast<void(KYTY_SYSV_ABI*)(int)>(targets[Exit])(99); active = nullptr;
	if (!check(fatal.log.empty() && RuntimeCrt::PendingExitRecords(fatal.execution) == 1 && !programs[8].application_exit_requested.load(),
	    "fatal stopped owner never runs or consumes guest cleanup and never borrows another owner's CRT")) { return false; }
	// A second healthy owner and session remain isolated from the exited one.
	auto& isolated = configure(16, 9, Operation::ExitProcess);
	const auto native_entry_exit = RuntimeCrt::PrepareEntryExitCallback(isolated.execution);
	RuntimeLinker::SetCurrentProgram(&programs[9]);
	if (!check(native_entry_exit && isolated.execution->InvokeCallback(native_entry_exit).Returned() &&
	    RuntimeCrt::EntryNotifications(isolated.execution) == 1 && !isolated.execution->StopRequested(),
	    "native private CRT may call the supplied owner gateway without HLE drain or session-wide stop side effects")) { return false; }
	if (!check(register_one(9, CxaAt, isolated.image + 0x160, nullptr, reinterpret_cast<void*>(isolated.image + 0x5000)),
	    "register independent B owner destructor")) { return false; }
	filtered = {entries[9][Finalize], nullptr}; RuntimeLinker::SetCurrentProgram(&programs[9]); active = &isolated;
	const auto isolated_result = isolated.execution->Invoke(GuestFinalize, &filtered); active = nullptr;
	if (!check(isolated_result.Returned() && isolated.typed && isolated.log == std::vector<int> {3} && !isolated.execution->StopRequested(),
	    "other owner remains running and consumes only its own record")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[13]); active = &isolated;
	reinterpret_cast<CxaAtFunction>(targets[CxaAt])(CxaFiltered, nullptr, nullptr);
	reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(targets[Finalize])(nullptr); active = nullptr;
	if (!check(isolated.log == std::vector<int> {3, 6} && exiting.log == std::vector<int> {6, 4, 3, 5, 2},
	    "legacy session callbacks remain direct and isolated from owner CRT state")) { return false; }
	auto& recursive = configure(17, 10, Operation::GuardRecursive);
	if (!check(workers.Start(recursive) && workers.Join() && !recursive.result.Returned() && recursive.continued == 0 &&
	    recursive.result.failure.reason == GuestFailureReason::InvalidTransition,
	    "same-actual-thread recursive guard fails closed rather than deadlocking or claiming successful initialization")) { return false; }
	auto& std_recursive = configure(20, 9, Operation::StdOnce); std_recursive.action = Action::Reenter;
	std_recursive.control = reinterpret_cast<void*>(std_recursive.image + 0x4200);
	if (!check(workers.Start(std_recursive) && workers.Join() && !std_recursive.result.Returned() && std_recursive.continued == 0 &&
	    std_recursive.calls == 1 && RuntimeCrt::ReadState(std_recursive.control) == 0 &&
	    std_recursive.result.failure.reason == GuestFailureReason::InvalidTransition,
	    "same-thread recursive std once unwinds only its nested guest segment, resets state and suppresses outer continuation")) { return false; }
	// After semantic proofs, exercise the parent's ACTUAL production policy/raw
	// resolver too. Exit deliberately remains absent from returning classification.
	programs[0].dynamic_info = std::make_unique<DynamicInfo>();
	programs[0].dynamic_info->import_libs = {{"A", 1, "libkernel"}, {"B", 1, "LibcInternal"}};
	programs[0].dynamic_info->import_modules = {{"C", 1, 1, "libkernel"}, {"D", 1, 1, "LibcInternal"}};
	ApplicationImportBindings deferred(&programs[0]);
	for (Export kind: {Ponce, Sonce, Acquire, Release, Abort, AtExit, CxaAt, Finalize, MainReturn}) {
		if (!check(deferred.BindResolved(nids[kind], registrations[kind]) != 0,
		    "parent must integrate exact B2 returning pairs after semantic review, without a fixture policy override")) { return false; }
	}
	if (!check(deferred.BindResolved(nids[Exit], registrations[Exit]) == 0,
	    "process exit must remain unadmitted by the generic returning table in this handoff")) { return false; }
	const std::array<const char*, 5> raw {"14bOACANTBo#A#C", "DiGVep5yB5w#B#D", "3GPpjQdAMTw#B#D", "9rAeANT2tyE#B#D", "2emaaluWzUw#B#D"};
	for (unsigned kind = Ponce; kind <= Abort; ++kind) {
		entries[0][kind] = deferred.Bind(programs[0].base_vaddr + 0x4800 + kind * 8, raw[kind]);
		if (!check(entries[0][kind] != 0, "bind immutable raw once/std/guard import identity")) { return false; }
	}
	auto& deferred_once = configure(23, 0, Operation::PthreadOnce);
	auto& deferred_std = configure(24, 0, Operation::StdOnce); deferred_std.control = reinterpret_cast<void*>(deferred_std.image + 0x4300);
	auto& deferred_guard = configure(25, 0, Operation::GuardRelease); deferred_guard.control = reinterpret_cast<void*>(deferred_guard.image + 0x4400);
	auto& deferred_abort = configure(26, 0, Operation::GuardAbort); deferred_abort.control = reinterpret_cast<void*>(deferred_abort.image + 0x4500);
	if (!check(workers.Start(deferred_once) && workers.Start(deferred_std) && workers.Start(deferred_guard) && workers.Start(deferred_abort) && workers.Join() &&
	    deferred_once.result.Returned() && deferred_once.calls == 0 && deferred_once.observed == 0 &&
	    deferred_std.result.Returned() && deferred_std.observed == 1 && deferred_std.calls == 1 && deferred_std.typed &&
	    deferred_guard.result.Returned() && *static_cast<uint8_t*>(deferred_guard.control) == 1 &&
	    deferred_abort.result.Returned() && *static_cast<uint8_t*>(deferred_abort.control) == 0,
	    "actual raw production deferred entries resolve checked once, typed std callback and guard release/abort on real owned workers")) { return false; }
	// Migrated HostOnce regression: private checked callback/control, real worker
	// exit, and a HOST RAII frame that must return before the gateway propagates.
	auto& ponce_exit = configure(27, 0, Operation::PthreadOnce);
	ponce_exit.control = reinterpret_cast<void*>(ponce_exit.image + 0x4600);
	ponce_exit.action = Action::ThreadExit; ponce_exit.once_target = targets[Ponce];
	ponce_exit.outer = gateway(0, reinterpret_cast<uint64_t>(OnceHost));
	if (!check(ponce_exit.outer && workers.Start(ponce_exit) && workers.Join() && ponce_exit.result.ThreadExited() &&
	    ponce_exit.result.thread_exit.value == 77 && ponce_exit.destroyed == 1 && ponce_exit.continued == 0 &&
	    RuntimeCrt::ReadState(ponce_exit.control) == 0 && !ponce_exit.execution->Failure() && !ponce_exit.execution->PendingThreadExit(),
	    "checked pthread once resets after tagged initializer exit, completes host RAII and leaves no stale root exit")) { return false; }
	auto& ponce_retry = configure(28, 0, Operation::PthreadOnce); ponce_retry.control = ponce_exit.control;
	if (!check(workers.Start(ponce_retry) && workers.Join() && ponce_retry.result.Returned() && ponce_retry.calls == 1 &&
	    RuntimeCrt::ReadState(ponce_retry.control) == 1, "healthy worker retries pthread once after initializer thread exit")) { return false; }
	for (unsigned i = 0; i != Count - 1; ++i) {
		if (!check(GetApplicationExecution(&programs[i])->Outstanding() == 0, "CRT fixtures join every native worker and release all work leases")) { return false; }
	}
	return check(GuestCallBoundary::CurrentDepth() == 0 && HostCallReturnThunk::CurrentDepth() == 0,
	    "all CRT nested boundaries/gateways return balanced");
}
