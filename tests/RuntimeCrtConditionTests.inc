// Parent includes applicationImportBindings.h at TOP LEVEL and this fixture
// inside Libs::LibKernel alongside RuntimeGuestWaitTests.
// Actual registered POSIX targets + actual owned workers. Deliberately includes
// a foreign-notify regression: current broadcast/signal bodies lack that gate.
// Do NOT admit those missing pairs just to make the next title import return.
namespace RuntimeCrtConditionFixture {
enum Entry : unsigned { Init, AttrInit, AttrClock, AttrDestroy, Wait, Absolute, Relative, Broadcast, Signal, Lock, Unlock, Destroy, EntryCount };
struct Resource { PthreadMutex mutex = nullptr; PthreadCond condition = nullptr; };
struct Call {
	Loader::Program* owner = nullptr;
	Resource* resource = nullptr;
	uint64_t* entries = nullptr;
	Entry operation = Wait;
	int observed = -999;
	unsigned continued = 0;
	bool restored = false;
	Loader::RuntimeGuestResult result {};
	KernelTimespec deadline {};
	std::atomic_bool done {false};
};
using UnaryCond = int(KYTY_SYSV_ABI*)(PthreadCond*);
static unsigned WaiterCount(Resource& resource) {
	std::lock_guard lock(resource.condition->m); return static_cast<unsigned>(resource.condition->waiters.size());
}
static uint64_t KYTY_SYSV_ABI GuestWait(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	auto& resource = *call.resource;
	using MutexCall = int(KYTY_SYSV_ABI*)(PthreadMutex*);
	if (reinterpret_cast<MutexCall>(call.entries[Lock])(&resource.mutex) ||
	    reinterpret_cast<MutexCall>(call.entries[Lock])(&resource.mutex)) { return 1; }
	if (call.operation == Wait) {
		call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCond*, PthreadMutex*)>(call.entries[Wait])(&resource.condition, &resource.mutex);
	} else if (call.operation == Absolute) {
		call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCond*, PthreadMutex*, const KernelTimespec*)>(
		    call.entries[Absolute])(&resource.condition, &resource.mutex, &call.deadline);
	} else {
		call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCond*, PthreadMutex*, KernelUseconds)>(
		    call.entries[Relative])(&resource.condition, &resource.mutex, 20000);
	}
	{
		std::lock_guard lock(resource.mutex->m);
		call.restored = resource.mutex->owner == PthreadSelfOrNull() && resource.mutex->count == 2;
	}
	reinterpret_cast<MutexCall>(call.entries[Unlock])(&resource.mutex);
	reinterpret_cast<MutexCall>(call.entries[Unlock])(&resource.mutex);
	++call.continued; return static_cast<uint32_t>(call.observed);
}
static uint64_t KYTY_SYSV_ABI GuestNotify(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	call.observed = reinterpret_cast<UnaryCond>(call.entries[call.operation])(&call.resource->condition);
	++call.continued; return static_cast<uint32_t>(call.observed);
}
struct Setup {
	uint64_t* entries; Resource* resource; bool attributes = false;
	Loader::Program* owner = nullptr;
	Loader::RuntimeGuestResult result {};
};
static uint64_t KYTY_SYSV_ABI GuestSetup(void* opaque) {
	auto& setup = *static_cast<Setup*>(opaque);
	PthreadCondattr attribute = nullptr;
	PthreadCond condition = nullptr;
	using AttributeCall = int(KYTY_SYSV_ABI*)(PthreadCondattr*);
	if (reinterpret_cast<AttributeCall>(setup.entries[AttrInit])(&attribute)) { return 1; }
	const auto invalid = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCondattr*, KernelClockid)>(setup.entries[AttrClock])(&attribute, -1);
	const auto valid = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCondattr*, KernelClockid)>(setup.entries[AttrClock])(&attribute, 0);
	setup.attributes = invalid == Libs::Posix::POSIX_EINVAL && valid == 0;
	const auto initialized = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadCond*, const PthreadCondattr*)>(setup.entries[Init])(
	    &condition, &attribute);
	setup.resource->condition = condition; // Transfer the checked, initialized handle to the host fixture.
	const auto destroyed = reinterpret_cast<AttributeCall>(setup.entries[AttrDestroy])(&attribute);
	return initialized || destroyed || !setup.attributes;
}
static void* KYTY_SYSV_ABI SetupCoordinator(void* opaque) {
	auto& setup = *static_cast<Setup*>(opaque);
	setup.result = setup.owner->application_execution->Invoke(GuestSetup, &setup);
	return nullptr;
}
static void* KYTY_SYSV_ABI Coordinator(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	call.result = call.owner->application_execution->Invoke(GuestWait, &call);
	call.done.store(true, std::memory_order_release); return nullptr;
}
template <typename Predicate> static bool Until(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return predicate();
}
}

bool TestRuntimeCrtConditionWrappers(std::string* failure) {
	using namespace RuntimeCrtConditionFixture;
	const auto check = [&](bool ok, const char* message) { if (!ok && failure) { *failure = message; } return ok; };
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	auto& linker = *Common::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::Program owners[3]; Resource resources[2]; Call calls[9];
	std::vector<std::unique_ptr<Loader::HostCallReturnThunk>> gateways;
	std::vector<Pthread> workers;
	struct Restore {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		Loader::Program* owners; Resource* resources; std::vector<Pthread>& workers;
		~Restore() {
			for (unsigned i = 0; i != 3; ++i) { if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); } }
			for (auto worker: workers) { if (worker && !worker->free) { PthreadJoin(worker, nullptr); } }
			for (unsigned i = 0; i != 2; ++i) {
				Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
				if (resources[i].condition) { PthreadCondDestroy(&resources[i].condition); }
				if (resources[i].mutex) { PthreadMutexDestroy(&resources[i].mutex); }
			}
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {Loader::RuntimeLinker::GetCurrentProgram(), owners, resources, workers};
	Libs::InitLibKernel_1(linker.Symbols());
	const std::array<const char*, EntryCount> nids {"0TyVk4MSLt0", "mKoTx03HRWA", "EjllaAqAPZo", "dJcuQVn6-Iw", "Op8TBGY5KHg",
	    "27bAgiJmOh0", "K953PF5u6Pc", "mkx2fVhNMsg", "2MOy+rUfuhQ", "7H0iTOciTLo", "2Z+PpY6CaJg", "g+PZd2hiacg"};
	const std::array<const char*, EntryCount> expressions {"Posix::pthread_cond_init", "Posix::pthread_condattr_init",
	    "Posix::pthread_condattr_setclock", "Posix::pthread_condattr_destroy", "Posix::pthread_cond_wait", "Posix::pthread_cond_timedwait",
	    "Posix::pthread_cond_reltimedwait_np", "Posix::pthread_cond_broadcast", "Posix::pthread_cond_signal", "Posix::pthread_mutex_lock",
	    "Posix::pthread_mutex_unlock", "LibKernel::PthreadCondDestroy"};
	uint64_t entries[3][EntryCount] {};
	uint64_t targets[EntryCount] {};
	Loader::SymbolRecord registrations[EntryCount];
	for (unsigned owner = 0; owner != 3; ++owner) {
		owners[owner].rt = &linker; owners[owner].application_owner = &owners[owner]; owners[owner].unique_id = 992000 + owner;
		if (!check(Loader::InitializeApplicationExecution(&owners[owner]), "initialize condition-wrapper owner")) { return false; }
	}
	for (unsigned j = 0; j != EntryCount; ++j) {
		const char* library = j == Init || j == Signal || j == Lock || j == Unlock ? "Posix" : "libkernel";
		const auto* record = linker.Symbols()->FindRegistration({nids[j], library, 1, "libkernel", 1, 1, Loader::SymbolType::Func}, expressions[j]);
		if (!check(record != nullptr, "condition closure uses exact registered wrapper/library/error domain")) { return false; }
		targets[j] = record->vaddr;
		registrations[j] = *record;
		for (unsigned owner = 0; owner != 3; ++owner) {
			auto execution = owners[owner].application_execution;
			auto gateway = Loader::HostCallReturnThunk::Create(execution->Boundary(), record->vaddr, execution.get(),
			    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall);
			if (!check(bool(gateway), "create real registered POSIX condition return gateway")) { return false; }
			entries[owner][j] = gateway->GetAddress(); gateways.push_back(std::move(gateway));
		}
	}
	// Both broadcast namespaces must resolve to the SAME audited POSIX body;
	// the 2MO signal alias is intentionally a DIFFERENT expression in libkernel.
	const auto* posix_broadcast = linker.Symbols()->FindRegistration({"mkx2fVhNMsg", "Posix", 1, "libkernel", 1, 1,
	    Loader::SymbolType::Func}, "Posix::pthread_cond_broadcast");
	const auto* kernel_signal = linker.Symbols()->FindRegistration({"2MOy+rUfuhQ", "libkernel", 1, "libkernel", 1, 1,
	    Loader::SymbolType::Func}, "LibKernel::PthreadCondSignal");
	if (!check(posix_broadcast && posix_broadcast->vaddr == targets[Broadcast] && kernel_signal,
	    "verify duplicate broadcast namespaces and noninterchangeable signal registration expressions")) { return false; }
	for (unsigned i = 0; i != 2; ++i) {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[i]); PthreadMutexattr attr = nullptr;
		PthreadMutexattrInit(&attr); PthreadMutexattrSettype(&attr, 2);
		const auto mutex_status = PthreadMutexInit(&resources[i].mutex, &attr, "registered-cond-mutex"); PthreadMutexattrDestroy(&attr);
		Setup setup {entries[i], &resources[i], false, &owners[i]};
		Pthread setup_worker = nullptr;
		if (!check(PthreadCreateRuntimeCoordinator(&setup_worker, nullptr, SetupCoordinator, &setup, "condition-setup") == 0,
		    "create setup on an actually owned guest stack")) { return false; }
		workers.push_back(setup_worker);
		if (!check(PthreadJoin(setup_worker, nullptr) == 0 && mutex_status == 0 && setup.result.Returned() && setup.result.call.value == 0 && setup.attributes,
		    "registered POSIX attr init/invalid-clock/valid-clock/cond-init/destroy with positive errno domain")) { return false; }
	}
	const auto start = [&](unsigned call_index, unsigned owner, Entry operation) {
		auto& call = calls[call_index]; call.owner = &owners[owner]; call.resource = &resources[owner]; call.entries = entries[owner]; call.operation = operation;
		if (operation == Absolute) {
			NativeClockGettime(call.resource->condition->clock_id, &call.deadline);
			call.deadline.tv_nsec += 20000000; call.deadline.tv_sec += call.deadline.tv_nsec / 1000000000; call.deadline.tv_nsec %= 1000000000;
		}
		Loader::RuntimeLinker::SetCurrentProgram(call.owner); Pthread worker = nullptr;
		if (PthreadCreateRuntimeCoordinator(&worker, nullptr, Coordinator, &call, "registered-cond-fixture")) { return false; }
		workers.push_back(worker); return true;
	};
	const auto notify = [&](unsigned owner, unsigned resource_owner, Entry operation) {
		Call call; call.owner = &owners[owner]; call.resource = &resources[resource_owner]; call.entries = entries[owner]; call.operation = operation;
		Loader::RuntimeLinker::SetCurrentProgram(call.owner);
		const auto result = call.owner->application_execution->Invoke(GuestNotify, &call);
		return result.Returned() ? call.observed : Libs::Posix::POSIX_ECANCELED;
	};
	const auto broadcast_with_busy_destroy = [&] {
		Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
		if (PthreadMutexLock(&resources[0].mutex) != OK) { return false; }
		struct Unlock { PthreadMutex* mutex; ~Unlock() { PthreadMutexUnlock(mutex); } } unlock {&resources[0].mutex};
		const auto before = resources[0].condition;
		if (PthreadCondDestroy(&resources[0].condition) != KERNEL_ERROR_EBUSY || resources[0].condition != before) { return false; }
		if (notify(0, 0, Broadcast) != 0 || WaiterCount(resources[0]) != 0) { return false; }
		// Both notifications have been accepted but the waits cannot finish while
		// this mutex is held. Empty waiter lists must not permit early destruction.
		return PthreadCondDestroy(&resources[0].condition) == KERNEL_ERROR_EBUSY && resources[0].condition == before;
	};
	if (!check(start(0, 0, Wait) && start(1, 0, Wait) && Until([&] { return WaiterCount(resources[0]) == 2; }) &&
	    broadcast_with_busy_destroy() && Until([&] { return calls[0].done.load() && calls[1].done.load(); }),
	    "registered POSIX broadcast wakes both native guard-shaped condition waiters")) { return false; }
	if (!check(calls[0].result.Returned() && calls[1].result.Returned() && calls[0].restored && calls[1].restored &&
	    calls[0].observed == 0 && calls[1].observed == 0, "broadcast continuation restores recursive mutex before guest resumes")) { return false; }
	if (!check(start(2, 0, Wait) && start(3, 0, Wait) && Until([&] { return WaiterCount(resources[0]) == 2; }) && notify(0, 0, Signal) == 0 &&
	    Until([&] { return WaiterCount(resources[0]) == 1; }) && notify(0, 0, Broadcast) == 0 &&
	    Until([&] { return calls[2].done.load() && calls[3].done.load(); }), "registered POSIX signal wakes one, broadcast drains remaining contender")) { return false; }
	if (!check(start(4, 0, Absolute) && Until([&] { return calls[4].done.load(); }) && start(5, 0, Relative) && Until([&] { return calls[5].done.load(); }) &&
	    calls[4].result.Returned() && calls[5].result.Returned() && calls[4].observed == Libs::Posix::POSIX_ETIMEDOUT &&
	    calls[5].observed == Libs::Posix::POSIX_ETIMEDOUT && calls[4].restored && calls[5].restored,
	    "registered absolute/relative POSIX wrappers return positive timeout errno and restore recursion")) { return false; }
	if (!check(start(6, 0, Wait) && Until([&] { return WaiterCount(resources[0]) == 1; }), "park registered POSIX waiter before owner stop")) { return false; }
	owners[0].application_execution->RequestStop();
	if (!check(Until([&] { return calls[6].done.load(); }) && calls[6].result.stopped && calls[6].continued == 0 &&
	    WaiterCount(resources[0]) == 0, "POSIX wait cancels without signal, fake success or guest mutex reacquisition")) { return false; }
	// This is an explicit admission gate, NOT an expected-failure bypass. Current
	// source is known to wake B here; parent must close owner/lazy-init checks in
	// the CONDITION scope before classifying the missing notify pairs as supported.
	if (!check(start(7, 1, Wait) && Until([&] { return WaiterCount(resources[1]) == 1; }), "park independent owner B before foreign notification")) { return false; }
	const auto foreign = notify(2, 1, Broadcast);
	if (!check(foreign == Libs::Posix::POSIX_EOPNOTSUPP && WaiterCount(resources[1]) == 1 && !calls[7].done.load(),
	    "GATE: foreign registered broadcast must not wake or mutate B; current condition body requires parent scope fix")) { return false; }
	const auto foreign_signal = notify(2, 1, Signal);
	if (!check(foreign_signal == Libs::Posix::POSIX_EOPNOTSUPP && WaiterCount(resources[1]) == 1 && !calls[7].done.load(),
	    "GATE: foreign registered signal must not consume another owner's waiter")) { return false; }
	// Only AFTER the semantic gates: actual returning classifier + immutable raw
	// deferred identities for BOTH broadcast libraries and the POSIX signal body.
	owners[1].dynamic_info = std::make_unique<Loader::DynamicInfo>();
	owners[1].dynamic_info->import_libs = {{"A", 1, "libkernel"}, {"B", 1, "Posix"}};
	owners[1].dynamic_info->import_modules = {{"C", 1, 1, "libkernel"}};
	Loader::ApplicationImportBindings bindings(&owners[1]); uint64_t slots[3] {};
	if (!check(bindings.BindResolved(nids[Broadcast], registrations[Broadcast]) && bindings.BindResolved(nids[Signal], registrations[Signal]),
	    "parent exact returning entries must follow, not bypass, semantic condition gates")) { return false; }
	entries[1][Broadcast] = bindings.Bind(reinterpret_cast<uint64_t>(&slots[0]), "mkx2fVhNMsg#A#C");
	if (!check(entries[1][Broadcast] && notify(1, 1, Broadcast) == 0 && Until([&] { return calls[7].done.load(); }) &&
	    calls[7].result.Returned() && calls[7].restored, "actual libkernel raw deferred broadcast resolves and wakes only its owned native waiter")) { return false; }
	entries[1][Broadcast] = bindings.Bind(reinterpret_cast<uint64_t>(&slots[1]), "mkx2fVhNMsg#B#C");
	entries[1][Signal] = bindings.Bind(reinterpret_cast<uint64_t>(&slots[2]), "2MOy+rUfuhQ#B#C");
	return check(entries[1][Broadcast] && entries[1][Signal] && notify(1, 1, Broadcast) == 0 && notify(1, 1, Signal) == 0,
	    "actual Posix-library raw notify identities remain distinct from libkernel signal registration/error domain");
}
