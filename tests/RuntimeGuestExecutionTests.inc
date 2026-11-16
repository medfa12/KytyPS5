// Included in runtimeLinker.cpp (Loader) and libSysmodule.cpp (Libs::Sysmodule).
// Real boundaries, module lifecycle/ref publication, and registry transitions;
// only the ELF records and guest initializer instructions are synthetic.
#if defined(KYTY_RUNTIME_GUEST_EXECUTION_REGISTRY_TESTS)

bool TestRuntimeGuestRegistry(Loader::RuntimeLinker* linker, Loader::Program* owner, Loader::Program* child,
    const void* argument, std::atomic<bool>* entered, std::atomic<bool>* release,
    std::atomic<unsigned>* destroyed, std::string* failure) {
	ModuleRegistry registry;
	struct HostPin {
		std::atomic<unsigned>* destroyed;
		~HostPin() { destroyed->fetch_add(1); }
	} registry_pin {destroyed};
	std::atomic<bool> observed_waiter {false}, waiter_failed {false};
	std::thread waiter([&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!entered->load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
		std::thread releaser([&] {
			while (!registry.TestWaiters() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
			observed_waiter.store(registry.TestWaiters() != 0);
			release->store(true);
		});
		const int status = registry.LoadModule(owner, 0x012e, [] { return ModuleRegistry::LoadResult {0, 999}; });
		waiter_failed.store(status != 0);
		releaser.join();
	});
	const int status = registry.LoadModule(owner, 0x012e, [&] {
		HostPin kernel_path_pin {destroyed};
		int output = 123;
		const int handle = linker->LoadStartModuleReference(child, sizeof(void*), argument, &output);
		return ModuleRegistry::LoadResult {handle < 0 ? handle : 0, handle, handle < 0 ? nullptr : child};
	});
	waiter.join();
	bool retry_called = false;
	const int retry = registry.LoadModule(owner, 0x012e, [&] {
		retry_called = true;
		return ModuleRegistry::LoadResult {0, 999};
	});
	const bool ok = status < 0 && retry < 0 && !retry_called && observed_waiter.load() && waiter_failed.load() &&
	    registry.TestWaiters() == 0 && registry.TestEntries() == 0 && !registry.IsLoaded(owner, 0x012e) &&
	    child->load_count == 0 && !child->started && child->module_state == Loader::ModuleExecutionState::Failed &&
	    child->module_start_result.guest.call.tag == Loader::GuestCallOutcomeTag::Aborted;
	if (!ok && failure) { *failure = "real nested registry recovery must finish rollback, wake a real waiter, and refuse retry"; }
	return ok;
}

bool TestRuntimeGuestRegistryLatePublication(Loader::RuntimeLinker* linker, Loader::Program* owner,
    Loader::Program* child, const void* argument, std::string* failure) {
	ModuleRegistry registry;
	bool handle_was_valid = false;
	const auto status = registry.LoadModule(owner, 0x012e, [&] {
		int output = 0;
		const auto handle = linker->LoadStartModuleReference(child, sizeof(void*), argument, &output);
		handle_was_valid = handle == child->unique_id && child->load_count == 1 && output == 7;
		Loader::GetApplicationExecution(owner)->Fail(Loader::GuestFailureReason::Explicit, 0x1234);
		return ModuleRegistry::LoadResult {handle < 0 ? handle : 0, handle, child};
	});
	const bool ok = handle_was_valid && status < 0 && child->load_count == 0 && !child->started &&
	    registry.TestEntries() == 0 && !registry.IsLoaded(owner, 0x012e);
	if (!ok && failure) { *failure = "failure after kernel publication must roll back the actual reference before registry publication"; }
	return ok;
}

#else

namespace RuntimeGuestExecutionFixture {
struct Context {
	RuntimeLinker* linker = nullptr;
	Program* owner = nullptr;
	Program* child = nullptr;
	Program* self = nullptr;
	RuntimeGuestExecution* execution = nullptr;
	std::atomic<bool> entered {false};
	std::atomic<bool> release {false};
	std::atomic<unsigned> destroyed {0};
	unsigned unreachable = 0;
	uint64_t nested_gateway = 0;
	bool registry_ok = false;
	int value = 7;
	bool fail_on_return = false;
	ModuleCallStatus reentry = ModuleCallStatus::Unavailable;
	ModuleCallResult thread_exit;
	std::atomic<unsigned>* cycle_entered = nullptr;
	size_t observed_args = 0;
	module_func_t observed_callback = nullptr;
	std::string failure;
};
struct Pin {
	Context* context;
	~Pin() { context->destroyed.fetch_add(1); }
};
// Resolver returns normally with all host ownership destroyed BEFORE the
// synthetic guest tail calls the no-argument abort gateway.
static GuestBoundaryFailureFunction ResolveFailure(Context* context) {
	Pin resolver_pin {context};
	context->execution->Fail(GuestFailureReason::Explicit, 0xfeed);
	return context->execution->FailureFunction();
}
static KYTY_SYSV_ABI int AbortInitializer(size_t args, const void* argp, module_func_t callback) {
	auto* context = static_cast<Context*>(const_cast<void*>(argp));
	context->observed_args = args;
	context->observed_callback = callback;
	context->entered.store(true);
	while (!context->release.load()) { std::this_thread::yield(); }
	ResolveFailure(context)();
	++context->unreachable;
	return 0;
}
static KYTY_SYSV_ABI int ReturnedInitializer(size_t args, const void* argp, module_func_t callback) {
	auto* context = static_cast<Context*>(const_cast<void*>(argp));
	context->observed_args = args;
	context->observed_callback = callback;
	if (context->fail_on_return) { context->execution->Fail(GuestFailureReason::Explicit, 0xbeef); }
	return context->value;
}
static ModuleCallStatus StartNested(Context* context, Program* program) {
	Pin pin {context};
	return context->linker->StartModuleChecked(program, 0, context, nullptr).status;
}
static KYTY_SYSV_ABI int RecursiveInitializer(size_t, const void* argument, module_func_t) {
	auto* context = static_cast<Context*>(const_cast<void*>(argument));
	context->reentry = StartNested(context, context->self);
	return 7;
}
static KYTY_SYSV_ABI int CycleInitializer(size_t, const void* argument, module_func_t) {
	auto* context = static_cast<Context*>(const_cast<void*>(argument));
	context->cycle_entered->fetch_add(1);
	while (context->cycle_entered->load() != 2) { std::this_thread::yield(); }
	context->reentry = StartNested(context, context->child);
	return 7;
}
static KYTY_SYSV_ABI int LoadNested(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin outer_host_pin {context};
	context->registry_ok = Libs::Sysmodule::TestRuntimeGuestRegistry(context->linker, context->owner, context->child, context,
	    &context->entered, &context->release, &context->destroyed, &context->failure);
	return context->registry_ok ? 0 : -1;
}
static KYTY_SYSV_ABI int ReturnHost(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin host_pin {context};
	return context->value;
}
static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV InvokeGateway(void* argument) {
	auto* context = static_cast<Context*>(argument);
	return static_cast<uint64_t>(static_cast<int64_t>(
	    reinterpret_cast<int(KYTY_SYSV_ABI *)(void*)>(context->nested_gateway)(context)));
}
static KYTY_SYSV_ABI int OuterInitializer(size_t, const void* argp, module_func_t) {
	auto* context = static_cast<Context*>(const_cast<void*>(argp));
	reinterpret_cast<int(KYTY_SYSV_ABI *)(void*)>(context->nested_gateway)(context);
	// The real generic return barrier must abort before this guest continuation.
	++context->unreachable;
	return 0;
}
static KYTY_SYSV_ABI int Callback(size_t, const void*) { return 0; }
static int KYTY_SYSV_ABI ExitingInitializer(size_t, const void* argument, module_func_t) {
	auto* context = static_cast<Context*>(const_cast<void*>(argument));
	Libs::LibKernel::PthreadExit(reinterpret_cast<void*>(0x715));
	++context->unreachable;
	return 0;
}
static void* KYTY_SYSV_ABI ModuleExitCoordinator(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin coordinator_pin {context};
	context->thread_exit = context->linker->StartModuleChecked(context->self, 0, context, nullptr);
	return nullptr; // Only RunThread consumes the pending exit after this RAII.
}
static void KYTY_SYSV_ABI ObserveTlsFree(void* argument) {
	static_cast<Context*>(argument)->destroyed.fetch_add(1);
}
static thread_local Context* heap_context = nullptr;
static void* KYTY_SYSV_ABI HeapMalloc(uint64_t size) {
	heap_context->observed_args = size;
	if (heap_context->fail_on_return) { ResolveFailure(heap_context)(); }
	return heap_context;
}
static int KYTY_SYSV_ABI HeapMemalign(void** output, uint64_t alignment, uint64_t size) {
	heap_context->observed_args = size;
	heap_context->value = static_cast<int>(alignment);
	*output = heap_context;
	return 0;
}
static void KYTY_SYSV_ABI HeapFree(void*) {}
static int KYTY_SYSV_ABI HeapHost(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin pin {context};
	context->registry_ok = context->linker->ApplicationHeapMalloc(317) == context;
	if (!context->execution->Failure()) {
		context->registry_ok = context->registry_ok && context->linker->ApplicationHeapMemalign(256, 619) == context;
	}
	return context->registry_ok ? 7 : -1;
}

struct EntryObservation {
	void* params = nullptr;
	uint64_t atexit = 0;
	uint64_t root_parent = ~uint64_t {0};
	uint64_t root_return = ~uint64_t {0};
	uint64_t alignment = 0;
	uint64_t calls = 0;
	uint64_t gateway = 0;
	Context* context = nullptr;
	uint64_t after = 0;
};
static_assert(offsetof(EntryObservation, gateway) == 48 && offsetof(EntryObservation, after) == 64);
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
static __attribute__((naked)) void KYTY_SYSV_ABI ObserveEntry(EntryParams*, atexit_func_t) {
	__asm__ volatile("movq %rdi, 0(%rdi)\n\t"
	                 "movq %rsi, 8(%rdi)\n\t"
	                 "movq 0(%rbp), %rax\n\t"
	                 "movq %rax, 16(%rdi)\n\t"
	                 "movq 8(%rbp), %rax\n\t"
	                 "movq %rax, 24(%rdi)\n\t"
	                 "movq %rsp, %rax\n\t"
	                 "andq $15, %rax\n\t"
	                 "movq %rax, 32(%rdi)\n\t"
	                 "incq 40(%rdi)\n\t"
	                 "pushq %rbx\n\t"
	                 "movq %rdi, %rbx\n\t"
	                 "movq 48(%rbx), %rax\n\t"
	                 "testq %rax, %rax\n\t"
	                 "jz 1f\n\t"
	                 "movq 56(%rbx), %rdi\n\t"
	                 "call *%rax\n\t"
	                 "1: incq 64(%rbx)\n\t"
	                 "popq %rbx\n\t"
	                 "ret\n\t");
}
#else
static void KYTY_SYSV_ABI ObserveEntry(EntryParams*, atexit_func_t) {}
#endif
static int KYTY_SYSV_ABI EntryNestedHost(void* argument) {
	auto* context = static_cast<Context*>(argument);
	Pin host_pin {context};
	const auto inner = context->execution->InvokeModule(reinterpret_cast<uint64_t>(AbortInitializer), 99, context, 0);
	context->registry_ok = inner.call.tag == GuestCallOutcomeTag::Aborted;
	return 0;
}
}

bool RuntimeLinker::TestRuntimeGuestExecution(std::string* failure) {
	using namespace RuntimeGuestExecutionFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	RuntimeLinker linker;
	Program owners[10], modules[12];
	struct Cleanup {
		std::vector<Program*>& programs;
		std::filesystem::path root;
		std::vector<uint64_t> mappings;
		~Cleanup() {
			for (auto* program: programs) { program->elf.reset(); }
			programs.clear(); // These are stack fixtures, not loader-owned allocations.
			for (auto address: mappings) { Libs::LibKernel::Memory::FreeGuestMemory(address, 4096); }
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {linker.m_programs};
	cleanup.root = std::filesystem::temp_directory_path() /
	    ("kyty_guest_execution_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(cleanup.root);
	const auto path = cleanup.root / "fixture.sprx";
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
	if (!check(file.Create(path), "create synthetic shared ELF")) { return false; }
	uint32_t header_written = 0, segment_written = 0;
	file.Write(&header, sizeof(header), &header_written);
	file.Write(&segment, sizeof(segment), &segment_written);
	file.Close();
	if (!check(header_written == sizeof(header) && segment_written == sizeof(segment), "write complete synthetic shared ELF")) { return false; }
	for (unsigned i = 0; i != 10; ++i) {
		owners[i].application_owner = &owners[i];
		owners[i].unique_id = 980000 + i;
		GuestCallBoundaryStatus capability;
		if (!InitializeApplicationExecution(&owners[i], &capability)) {
			return check(false, "shared guest recovery capability unavailable (fixture NOT exercised)");
		}
		if (!check(owners[i].application_execution->Owner().token != 0 &&
		               (i == 0 || owners[i].application_execution->Owner().token != owners[i - 1].application_execution->Owner().token),
		           "application generations must be stable, distinct and nonzero")) { return false; }
	}
	const auto set_target = [&](Program& program, uint64_t target, bool fini = false) {
		const uint64_t offset = fini ? 64 : 32;
		if (!Libs::LibKernel::Memory::ProtectGuestMemory(program.base_vaddr, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite)) { return false; }
		uint8_t code[] {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
		std::memcpy(code + 2, &target, sizeof(target));
		std::memcpy(reinterpret_cast<void*>(program.base_vaddr + offset), code, sizeof(code));
		(fini ? program.dynamic_info->fini_vaddr : program.dynamic_info->init_vaddr) = offset;
		return Libs::LibKernel::Memory::ProtectGuestMemory(program.base_vaddr, 4096, Common::VirtualMemory::Mode::ExecuteRead) &&
		    Common::VirtualMemory::FlushInstructionCache(program.base_vaddr, 4096);
	};
	for (unsigned i = 0; i != 12; ++i) {
		modules[i].rt = &linker;
		modules[i].unique_id = 981000 + i;
		modules[i].application_owner = &owners[i == 0 ? 0 : std::min(i - 1, 6u)];
		modules[i].file_name = path;
		modules[i].elf = std::make_unique<Elf64>();
		modules[i].elf->Open(path);
		modules[i].dynamic_info = std::make_unique<DynamicInfo>();
		modules[i].base_vaddr = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite, "module_execution_fixture");
		if (!check(modules[i].base_vaddr != 0, "allocate private initializer trampoline")) { return false; }
		cleanup.mappings.push_back(modules[i].base_vaddr);
		modules[i].base_size = 4096;
		if (!check(set_target(modules[i], reinterpret_cast<uint64_t>(ReturnedInitializer)), "seal initializer trampoline")) { return false; }
		if (!check(modules[i].elf->IsValid() && modules[i].elf->IsShared(), "parse synthetic shared ELF")) { return false; }
		linker.m_programs.push_back(&modules[i]);
	}
	modules[9].application_owner = &owners[7];
	modules[10].application_owner = &owners[8];
	modules[11].application_owner = &owners[9];
	if (!check(set_target(modules[0], reinterpret_cast<uint64_t>(OuterInitializer)) &&
	               set_target(modules[1], reinterpret_cast<uint64_t>(AbortInitializer)), "configure nested initializer trampolines")) { return false; }
	Context nested;
	nested.linker = &linker;
	nested.owner = &owners[0];
	nested.child = &modules[1];
	nested.execution = owners[0].application_execution.get();
	HostCallReturnThunkStatus gateway_status;
	auto gateway = HostCallReturnThunk::Create(nested.execution->Boundary(), reinterpret_cast<uint64_t>(LoadNested),
	    nested.execution, RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall,
	    HostCallAbi::BaselineSysV, &gateway_status);
	if (!check(gateway != nullptr, "create actual owner-bound host return barrier")) { return false; }
	nested.nested_gateway = gateway->GetAddress();
	const auto baseline = Common::GetHostLockCriticalCounter()->load();
	const auto outer = linker.StartModuleChecked(&modules[0], sizeof(void*), &nested, nullptr);
	if (!check(outer.status == ModuleCallStatus::Aborted && outer.guest.call.tag == GuestCallOutcomeTag::Aborted &&
	               nested.registry_ok && nested.unreachable == 0 && nested.destroyed.load() == 4 &&
	               modules[0].module_state == ModuleExecutionState::Failed && !modules[0].started &&
	               nested.execution->Outstanding() == 0 && GuestCallBoundary::CurrentDepth() == 0 &&
	               HostCallReturnThunk::CurrentDepth() == 0 &&
	               Common::GetHostLockCriticalCounter()->load() == baseline,
	           nested.failure.empty() ? "nested recovery must retain host RAII and suppress both guest continuations" : nested.failure.c_str())) { return false; }
	const auto first = nested.execution->Failure();
	nested.execution->Fail(GuestFailureReason::BoundaryUnavailable, 123);
	if (!check(first.reason == GuestFailureReason::Explicit && first.detail == 0xfeed &&
	               nested.execution->Failure().detail == 0xfeed && !nested.execution->TryAcquire(GuestWorkKind::Thread) &&
	               !nested.execution->TryAcquire(GuestWorkKind::Callback), "first failure is sticky and closes all work admission")) { return false; }
	Context negative;
	negative.value = -41;
	int output = 0;
	const auto rejected = linker.LoadStartModuleReference(&modules[2], 321, &negative, &output);
	if (!check(rejected < 0 && output == -41 && modules[2].load_count == 0 && !modules[2].started &&
	               modules[2].initializer_returned && modules[2].module_start_result.Returned() &&
	               modules[2].module_start_result.guest.call.tag == GuestCallOutcomeTag::Returned &&
	               negative.observed_args == 321, "negative initializer is an ordinary tagged return, not abort or a published handle")) { return false; }
	Context late;
	if (!Libs::Sysmodule::TestRuntimeGuestRegistryLatePublication(&linker, &owners[2], &modules[3], &late, failure)) { return false; }
	Context finalizer;
	finalizer.execution = owners[3].application_execution.get();
	finalizer.release.store(true);
	const auto started = linker.StartModuleChecked(&modules[4], 712, &finalizer, Callback);
	if (!check(started.Returned() && started.value == 7 && finalizer.observed_args == 712 &&
	               finalizer.observed_callback == Callback, "actual SysV module adapter must preserve all three arguments and positive output")) { return false; }
	if (!check(linker.LoadStartModuleReference(&modules[4], 0, &finalizer, &output) == modules[4].unique_id &&
	               output == 7 && modules[4].load_count == 1, "completed initializer reuse retains its real output and publishes once")) { return false; }
	if (!check(set_target(modules[4], reinterpret_cast<uint64_t>(AbortInitializer), true), "configure finalizer trampoline")) { return false; }
	if (!check(linker.StopModuleReference(&modules[4], 0, &finalizer, &output) < 0 && modules[4].load_count == 0 &&
	               modules[4].module_state == ModuleExecutionState::Failed && finalizer.unreachable == 0 &&
	               finalizer.execution->Outstanding() == 0, "finalizer abort must recover locally and invalidate the actual reference")) { return false; }
	Context peer_failure;
	peer_failure.execution = owners[4].application_execution.get();
	peer_failure.fail_on_return = true;
	const auto raced = linker.StartModuleChecked(&modules[5], 0, &peer_failure, nullptr);
	if (!check(raced.status == ModuleCallStatus::Aborted && raced.guest.call.tag == GuestCallOutcomeTag::Returned &&
	               modules[5].initializer_returned && modules[5].initializer_result == 7 &&
	               !modules[5].started && modules[5].module_state == ModuleExecutionState::Failed,
	           "sticky owner failure must veto publication even when guest initializer actually returned successfully")) { return false; }
	Context recursive;
	recursive.linker = &linker;
	recursive.self = &modules[6];
	if (!check(set_target(modules[6], reinterpret_cast<uint64_t>(RecursiveInitializer)), "configure recursive initializer trampoline")) { return false; }
	const auto recursion = linker.StartModuleChecked(&modules[6], 0, &recursive, nullptr);
	if (!check(recursion.Returned() && recursive.reentry == ModuleCallStatus::Busy && recursive.destroyed.load() == 1,
	           "real loader recursive start must return busy instead of deadlocking or pretending the initializer completed")) { return false; }
	Context ordinary_hle;
	ordinary_hle.value = -47;
	auto hle_control = owners[5].application_execution;
	auto ordinary_gateway = HostCallReturnThunk::Create(hle_control->Boundary(), reinterpret_cast<uint64_t>(ReturnHost),
	    hle_control.get(), RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall,
	    HostCallAbi::BaselineSysV, &gateway_status);
	if (!check(ordinary_gateway != nullptr, "create ordinary typed HLE gateway")) { return false; }
	ordinary_hle.nested_gateway = ordinary_gateway->GetAddress();
	const auto ordinary = hle_control->Invoke(InvokeGateway, &ordinary_hle);
	if (!check(ordinary.Returned() && static_cast<int64_t>(ordinary.call.value) == -47 && !hle_control->Failure() &&
	               ordinary_hle.destroyed.load() == 1 && hle_control->Outstanding() == 0 && HostCallReturnThunk::CurrentDepth() == 0,
	           "production enter/leave callbacks must preserve an ordinary negative typed HLE result and release its actual lease")) { return false; }
	if (!check(RuntimeGuestExecution::EnterHostCall(hle_control.get(), 0x9876, HostCallEnterReason::CapacityExceeded) ==
	               HostCallDisposition::Abort && hle_control->Failure().reason == GuestFailureReason::TransitionCapacity &&
	               hle_control->Outstanding() == 0,
	           "gateway capacity callback must latch failure without admitting target work")) { return false; }
	std::atomic<unsigned> in_cycle {0};
	Context left, right;
	left.linker = right.linker = &linker;
	left.child = &modules[8];
	right.child = &modules[7];
	left.cycle_entered = right.cycle_entered = &in_cycle;
	if (!check(set_target(modules[7], reinterpret_cast<uint64_t>(CycleInitializer)) &&
	               set_target(modules[8], reinterpret_cast<uint64_t>(CycleInitializer)), "configure cyclic initializer trampolines")) { return false; }
	ModuleCallResult left_result, right_result;
	std::thread left_thread([&] { left_result = linker.StartModuleChecked(&modules[7], 0, &left, nullptr); });
	std::thread right_thread([&] { right_result = linker.StartModuleChecked(&modules[8], 0, &right, nullptr); });
	left_thread.join();
	right_thread.join();
	if (!check(left_result.Returned() && right_result.Returned() &&
	               (left.reentry == ModuleCallStatus::Busy || right.reentry == ModuleCallStatus::Busy) &&
	               owners[6].application_execution->Outstanding() == 0,
	           "real cross-thread initializer dependency cycle must return a bounded busy result without a host wait cycle")) { return false; }
	auto stop_control = owners[6].application_execution;
	auto admitted_thread = stop_control->TryAcquire(GuestWorkKind::Thread);
	auto moved_thread = std::move(admitted_thread);
	stop_control->RequestStop();
	bool published_after_stop = false;
	if (!check(!admitted_thread && moved_thread && stop_control->Outstanding() == 1 &&
	               !moved_thread.Publish([&] { published_after_stop = true; }) && !published_after_stop &&
	               !stop_control->TryAcquire(GuestWorkKind::Callback) &&
	               !stop_control->WaitForDrain(std::chrono::milliseconds(0)) && !stop_control->Retire(),
	           "a moved in-flight thread lease must block retirement while stop atomically closes publication and admission")) { return false; }
	moved_thread.Reset();
	if (!check(stop_control->WaitForDrain(std::chrono::milliseconds(1)) && stop_control->Retire() &&
	           !stop_control->Failure(), "ordinary stop/drain must release exactly one moved lease without inventing a failure")) { return false; }
	// Exercise the production executable entry, including the real ELF entry
	// lookup and program-scope guard, not just the generic boundary primitive.
	Program entry_owners[2];
	struct RemoveEntryPrograms {
		std::vector<Program*>& programs;
		Program* owners;
		~RemoveEntryPrograms() { for (unsigned i = 0; i != 2; ++i) { std::erase(programs, &owners[i]); } }
	} remove_entry_programs {linker.m_programs, entry_owners};
	struct RestoreProgram {
		Program* previous = RuntimeLinker::GetCurrentProgram();
		~RestoreProgram() { RuntimeLinker::SetCurrentProgram(previous); }
	} restore_program;
	for (unsigned i = 0; i != 2; ++i) {
		auto& owner = entry_owners[i];
		owner.application_owner = &owner;
		owner.rt = &linker;
		owner.unique_id = 982000 + i;
		if (!check(InitializeApplicationExecution(&owner), "initialize executable entry owner")) { return false; }
		const auto entry_path = cleanup.root / ("entry" + std::to_string(i) + ".elf");
		header.e_type = ET_DYNEXEC;
		header.e_entry = 32;
		if (!check(file.Create(entry_path), "create executable entry ELF")) { return false; }
		file.Write(&header, sizeof(header), &header_written);
		file.Write(&segment, sizeof(segment), &segment_written);
		file.Close();
		owner.elf = std::make_unique<Elf64>();
		owner.elf->Open(entry_path);
		owner.file_name = entry_path;
		owner.dynamic_info = std::make_unique<DynamicInfo>();
		owner.base_vaddr = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite, "executable_entry_fixture");
		if (!check(owner.base_vaddr != 0, "allocate private executable entry trampoline")) { return false; }
		cleanup.mappings.push_back(owner.base_vaddr);
		owner.base_size = 4096;
		if (!check(set_target(owner, reinterpret_cast<uint64_t>(ObserveEntry)), "seal executable entry trampoline")) { return false; }
		linker.m_programs.push_back(&owner);
		if (!check(owner.elf->IsValid() && !owner.elf->IsShared(), "parse executable entry ELF")) { return false; }
		RuntimeLinker::SetCurrentProgram(&owner);
		EntryObservation observation;
		Context entry_context;
		entry_context.execution = owner.application_execution.get();
		entry_context.release.store(true);
		std::unique_ptr<HostCallReturnThunk> entry_gateway;
		if (i == 1) {
			entry_gateway = HostCallReturnThunk::Create(entry_context.execution->Boundary(),
			    reinterpret_cast<uint64_t>(EntryNestedHost), entry_context.execution,
			    RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
			if (!check(entry_gateway != nullptr, "create executable entry nested host gateway")) { return false; }
			observation.gateway = entry_gateway->GetAddress();
			observation.context = &entry_context;
		}
		const auto expected_exit = RuntimeCrt::PrepareEntryExitCallback(owner.application_execution);
		const auto entry_result = linker.RunApplicationEntry(&owner, reinterpret_cast<EntryParams*>(&observation));
		if (!check(expected_exit && observation.params == &observation && observation.atexit == expected_exit &&
		               observation.root_parent == 0 && observation.root_return == 0 && observation.alignment == 8 && observation.calls == 1,
		           "production executable entry must preserve parameters/atexit, SysV alignment, and zero frame root")) { return false; }
		if (!check(i == 0 ? entry_result.Returned() && entry_result.call.value == 0 && observation.after == 1 :
		               entry_result.call.tag == GuestCallOutcomeTag::Aborted && observation.after == 0 &&
		                   entry_context.registry_ok && entry_context.destroyed.load() == 2 && entry_context.unreachable == 0,
		           "executable return or nested failure must finish host cleanup and suppress only failed guest continuation")) { return false; }
		if (!check(owner.application_execution->Outstanding() == 0 && GuestCallBoundary::CurrentDepth() == 0 &&
		               HostCallReturnThunk::CurrentDepth() == 0 && Common::GetHostLockCriticalCounter()->load() == baseline,
		           "executable entry recovery must drain transitions and leave clean TLS/critical accounting")) { return false; }
		owner.application_execution->RequestStop();
		if (!check(linker.RunApplicationEntry(&owner, reinterpret_cast<EntryParams*>(&observation)).call.tag ==
		               GuestCallOutcomeTag::NotInvoked && observation.calls == 1,
		           "stopped executable entry must not reenter guest instructions")) { return false; }
	}
	Context tls_callback;
	Program tls_owner;
	tls_owner.tls.tlss[17] = {reinterpret_cast<uint8_t*>(&tls_callback), ObserveTlsFree, false, 0};
	const auto tls_memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, 4096, Common::VirtualMemory::Mode::ReadWrite, "failed_owner_tls_fixture");
	if (!check(tls_memory != 0, "allocate host-backed cleanup TLS fixture")) { return false; }
	tls_owner.tls.tlss[18] = {reinterpret_cast<uint8_t*>(tls_memory), nullptr, true, 4096};
	DeleteTls(&tls_owner, 17, false);
	DeleteTls(&tls_owner, 17, false);
	DeleteTls(&tls_owner, 18, false);
	if (!check(tls_callback.destroyed.load() == 0 && tls_owner.tls.tlss.contains(17) &&
	               tls_owner.tls.tlss.at(17).ptr == reinterpret_cast<uint8_t*>(&tls_callback) &&
	               tls_owner.tls.tlss.at(17).free_func == ObserveTlsFree && !tls_owner.tls.tlss.contains(18),
	           "callback-suppressed TLS cleanup retains guest allocation identity but frees host-backed storage")) { return false; }
	// This callback is a host-only fixture. Production failed-owner records stay
	// pinned until an owner-safe reclamation policy covers guest allocator calls.
	DeleteTls(&tls_owner, 17, true);
	if (!check(tls_callback.destroyed.load() == 1 && tls_owner.tls.tlss.empty(),
	           "retained TLS ownership must remain available for exactly one later release")) { return false; }
	for (unsigned i = 0; i != 2; ++i) {
		Program heap_owner;
		heap_owner.application_owner = &heap_owner;
		heap_owner.unique_id = 983000 + i;
		if (!check(InitializeApplicationExecution(&heap_owner), "initialize allocator callback owner")) { return false; }
		Context context;
		context.linker = &linker;
		context.execution = heap_owner.application_execution.get();
		context.fail_on_return = i == 1;
		heap_context = &context;
		RuntimeLinker::SetCurrentProgram(&heap_owner);
		void* api[10] {};
		api[0] = reinterpret_cast<void*>(HeapMalloc);
		api[1] = reinterpret_cast<void*>(HeapFree);
		api[6] = reinterpret_cast<void*>(HeapMemalign);
		linker.SetApplicationHeapApi(api);
		auto heap_gateway = HostCallReturnThunk::Create(context.execution->Boundary(), reinterpret_cast<uint64_t>(HeapHost),
		    context.execution, RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
		if (!check(heap_gateway != nullptr, "create allocator host gateway")) { return false; }
		context.nested_gateway = heap_gateway->GetAddress();
		const auto result = context.execution->Invoke(InvokeGateway, &context);
		if (!check(i == 0 ? result.Returned() && result.call.value == 7 && context.observed_args == 619 &&
		                       context.value == 256 && context.destroyed.load() == 1 :
		                   result.call.tag == GuestCallOutcomeTag::Aborted && context.observed_args == 317 &&
		                       context.destroyed.load() == 2 && context.execution->Failure().detail == 0xfeed,
		           "allocator callbacks must preserve typed arguments/results and finish host cleanup before failure propagation")) { return false; }
		context.execution->RequestStop();
		linker.SetApplicationHeapApi(nullptr);
		if (!check(heap_owner.application_heap_malloc == HeapMalloc && linker.ApplicationHeapMalloc(999) == nullptr &&
		               context.observed_args != 999 && context.execution->Outstanding() == 0 &&
		               GuestCallBoundary::CurrentDepth() == 0 && HostCallReturnThunk::CurrentDepth() == 0 &&
		               Common::GetHostLockCriticalCounter()->load() == baseline,
		           "stopped owner must reject allocator publication and callback entry without leaking work")) { return false; }
		heap_context = nullptr;
		RuntimeLinker::SetCurrentProgram(nullptr);
	}
	Context exiting_module;
	exiting_module.linker = &linker;
	exiting_module.self = &modules[9];
	if (!check(set_target(modules[9], reinterpret_cast<uint64_t>(ExitingInitializer)), "configure exiting initializer trampoline")) { return false; }
	RuntimeLinker::SetCurrentProgram(&owners[7]);
	Libs::LibKernel::Pthread module_thread = nullptr;
	if (!check(Libs::LibKernel::PthreadCreateRuntimeCoordinator(&module_thread, nullptr,
	               ModuleExitCoordinator, &exiting_module, "module-exit-fixture") == 0,
	           "create actual coordinator for initializer pthread exit")) { return false; }
	void* exit_value = nullptr;
	if (!check(Libs::LibKernel::PthreadJoin(module_thread, &exit_value) == 0 &&
	               exit_value == reinterpret_cast<void*>(0x715) && exiting_module.thread_exit.ThreadExited() &&
	               exiting_module.destroyed.load() == 1 && exiting_module.unreachable == 0 &&
	               !owners[7].application_execution->Failure() && owners[7].application_execution->Outstanding() == 0 &&
	               modules[9].module_state == ModuleExecutionState::Failed && !modules[9].started,
	           "initializer thread exit must remain typed, clean up coordinator RAII and preserve normal join value")) { return false; }
	const auto peer_result = linker.StartModuleChecked(&modules[9], 0, &exiting_module, nullptr);
	if (!check(peer_result.status == ModuleCallStatus::Failed && !peer_result.guest.thread_exit &&
	               !owners[7].application_execution->PendingThreadExit() && !owners[7].application_execution->Failure(),
	           "an incomplete module must never replay another actual thread's exit outcome to a peer")) { return false; }
	modules[10].dynamic_info->init_vaddr = 4096;
	const auto outside_text = linker.StartModuleChecked(&modules[10], 0, nullptr, nullptr);
	if (!check(outside_text.status == ModuleCallStatus::Failed && owners[8].application_execution->Failure() &&
	               !modules[10].started && owners[8].application_execution->Outstanding() == 0,
	           "late initializer outside private executable text must fail before invoking native code")) { return false; }
	Elf64_Sym untyped {};
	untyped.st_info = (STB_GLOBAL << 4) | STT_NOTYPE;
	Elf64_Rela unsupported {};
	unsupported.r_offset = 128;
	unsupported.r_info = R_X86_64_JUMP_SLOT;
	modules[11].dynamic_info->symbol_table = &untyped;
	modules[11].dynamic_info->symbol_table_total_size = sizeof(untyped);
	modules[11].dynamic_info->jmprela_table = &unsupported;
	modules[11].dynamic_info->jmprela_table_size = sizeof(unsupported);
	const auto unsafe_late = linker.StartModuleChecked(&modules[11], 0, nullptr, nullptr);
	if (!check(unsafe_late.status == ModuleCallStatus::Failed && owners[9].application_execution->Failure() &&
	               !modules[11].started && owners[9].application_execution->Outstanding() == 0,
	           "late module starts must repeat hard relocation checks before invoking their initializer")) { return false; }
	auto control = owners[0].application_execution;
	control->BeginDraining();
	return check(control->WaitForDrain(std::chrono::milliseconds(1)) && control->Retire() &&
	                 control->Failure().detail == 0xfeed && !control->TryAcquire(GuestWorkKind::Guest),
	             "retirement requires closed admission/drained leases and must retain the first failure");
}
#endif
