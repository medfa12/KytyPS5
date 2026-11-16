// Included inside Libs::LibKernel by pthread.cpp, VM-test builds only.
// Real registrations, immutable raw imports, real owned pthreads/native joins.
namespace RuntimeRwlockFixture {
enum Operation : unsigned { Init, Destroy, Read, Write, TryRead, TryWrite, TimedRead, TimedWrite, Unlock,
                           AttrInit, AttrDestroy, AttrSet, AttrGet, PosixInit, PosixDestroy, PosixWrite,
                           EntryCount, Setup, Static, Exercise, ForeignAll, Abandon };
using Unary = int(KYTY_SYSV_ABI*)(PthreadRwlock*);
struct Resource { PthreadRwlock lock = nullptr; PthreadRwlockattr attr = nullptr; };
struct Call {
	Loader::Program* owner = nullptr;
	Resource* resource = nullptr;
	uint64_t* entries = nullptr;
	Operation operation = Setup;
	Loader::RuntimeGuestResult result;
	int observed = -999;
	unsigned continued = 0;
	KernelUseconds timeout = 10000;
	std::atomic_bool done {false};
};
static std::atomic_bool pinned {false}, release_pin {false};
static std::atomic<PthreadRwlock> failed_preparation {nullptr};
static std::atomic<PthreadRwlockattr> failed_attribute {nullptr};
static void OomHook(PthreadRwlock value, Loader::RuntimeGuestExecution*) {
	failed_preparation.store(value); throw std::bad_alloc {};
}
static void AttributeOomHook(PthreadRwlockattr value, Loader::RuntimeGuestExecution*) {
	failed_attribute.store(value); throw std::bad_alloc {};
}
static void PinHook(PthreadRwlock, Loader::RuntimeGuestExecution*) {
	pinned.store(true, std::memory_order_release);
	while (!release_pin.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}
static void StopHook(PthreadRwlock, Loader::RuntimeGuestExecution* execution) { execution->RequestStop(); }
static void ExitHook(PthreadRwlock, Loader::RuntimeGuestExecution* execution) { execution->RequestThreadExit(0x7257); }
static void FailHook(PthreadRwlock, Loader::RuntimeGuestExecution* execution) {
	execution->Fail(Loader::GuestFailureReason::InvalidTransition, 0x7258);
}
template <typename Predicate> static bool Until(Predicate predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	return predicate();
}
static uint64_t KYTY_SYSV_ABI Guest(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	auto* entries = call.entries;
	auto& resource = *call.resource;
	const auto unary = [&](Operation op, PthreadRwlock* slot) { return reinterpret_cast<Unary>(entries[op])(slot); };
	using AttributeUnary = int(KYTY_SYSV_ABI*)(PthreadRwlockattr*);
	if (call.operation == Setup || call.operation == Static) {
		PthreadRwlock local = nullptr;
		PthreadRwlockattr attr = nullptr;
		int type = -1;
		if (reinterpret_cast<AttributeUnary>(entries[AttrInit])(&attr) ||
		    reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlockattr*, int)>(entries[AttrSet])(&attr, 0) != KERNEL_ERROR_EINVAL ||
		    reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlockattr*, int)>(entries[AttrSet])(&attr, 2) ||
		    reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlockattr*, int*)>(entries[AttrGet])(&attr, &type) || type != 2) { return 1; }
		if (call.operation == Setup) {
			call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlock*, const PthreadRwlockattr*, const char*)>(entries[Init])(
			    &local, &attr, nullptr);
		} else {
			call.observed = unary(TryWrite, &local); // Previously fatal on NULL static initializer.
			if (!call.observed && unary(Unlock, &local)) { return 2; }
		}
		resource.lock = local; resource.attr = attr;
	} else if (call.operation == Exercise) {
		auto destroy_slot = resource.lock;
		PthreadRwlock posix_local = nullptr;
		if (reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlock*, const PthreadRwlockattr*)>(entries[PosixInit])(&posix_local, nullptr) ||
		    unary(Read, &posix_local) || unary(PosixDestroy, &posix_local) != Libs::Posix::POSIX_EBUSY ||
		    unary(Unlock, &posix_local) || unary(PosixDestroy, &posix_local)) { return 6; }
		if (unary(Read, &resource.lock) || unary(Read, &resource.lock) ||
		    unary(TryWrite, &resource.lock) != KERNEL_ERROR_EDEADLK ||
		    unary(Destroy, &destroy_slot) != KERNEL_ERROR_EBUSY ||
		    unary(Unlock, &resource.lock) || unary(Unlock, &resource.lock) ||
		    unary(Write, &resource.lock) || unary(TryWrite, &resource.lock) != KERNEL_ERROR_EDEADLK ||
		    unary(PosixWrite, &resource.lock) != Libs::Posix::POSIX_EDEADLK ||
		    unary(TryRead, &resource.lock) || unary(Unlock, &resource.lock) || unary(Unlock, &resource.lock) ||
		    unary(Unlock, &resource.lock) != KERNEL_ERROR_EPERM) { return 3; }
		call.observed = OK;
	} else if (call.operation == Destroy) {
		// Copyout/destruction happens through checked owned stack slots, not the
		// host fixture's Resource storage. All retained aliases are invalidated.
		auto local = resource.lock; auto attr = resource.attr;
		call.observed = unary(Destroy, &local);
		if (!call.observed) { resource.lock = local; }
		if (reinterpret_cast<AttributeUnary>(entries[AttrDestroy])(&attr)) { return 4; }
		resource.attr = attr;
	} else if (call.operation == ForeignAll) {
		auto local = resource.lock; auto attr = resource.attr; int untouched = -73;
		if (reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlock*, const PthreadRwlockattr*, const char*)>(entries[Init])(&local, nullptr, nullptr) != KERNEL_ERROR_EOPNOTSUPP ||
		    unary(Destroy, &local) != KERNEL_ERROR_EOPNOTSUPP || local != resource.lock) { return 7; }
		for (auto op: {Read, Write, TryRead, TryWrite, Unlock}) {
			if (unary(op, &local) != KERNEL_ERROR_EOPNOTSUPP) { return 8; }
		}
		for (auto op: {TimedRead, TimedWrite}) {
			if (reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlock*, KernelUseconds)>(entries[op])(&local, 1) != KERNEL_ERROR_EOPNOTSUPP) { return 9; }
		}
		if (reinterpret_cast<AttributeUnary>(entries[AttrInit])(&attr) != KERNEL_ERROR_EOPNOTSUPP ||
		    reinterpret_cast<AttributeUnary>(entries[AttrDestroy])(&attr) != KERNEL_ERROR_EOPNOTSUPP || attr != resource.attr ||
		    reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlockattr*, int*)>(entries[AttrGet])(&attr, &untouched) != KERNEL_ERROR_EOPNOTSUPP || untouched != -73) { return 10; }
		call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlockattr*, int)>(entries[AttrSet])(&attr, 1);
	} else if (call.operation == Abandon) {
		call.observed = unary(Write, &resource.lock); // No automatic robust/owner-death release.
	} else {
		if (call.operation == TimedRead || call.operation == TimedWrite) {
			call.observed = reinterpret_cast<int(KYTY_SYSV_ABI*)(PthreadRwlock*, KernelUseconds)>(entries[call.operation])(&resource.lock, call.timeout);
		} else { call.observed = unary(call.operation, &resource.lock); }
		if (!call.observed && call.operation != Unlock) {
			if (unary(Unlock, &resource.lock)) { return 5; }
		}
	}
	++call.continued;
	return static_cast<uint32_t>(call.observed);
}
static void* KYTY_SYSV_ABI Coordinator(void* opaque) {
	auto& call = *static_cast<Call*>(opaque);
	call.result = call.owner->application_execution->Invoke(Guest, &call);
	call.done.store(true, std::memory_order_release);
	return nullptr;
}
}

bool TestRuntimeRwlockLifecycle(std::string* failure) {
	using namespace RuntimeRwlockFixture;
	const auto check = [&](bool ok, const char* message) { if (!ok && failure) { *failure = message; } return ok; };
	if (!g_pthread_context) { Initialize(); }
	if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
	auto& linker = *Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitLibKernel_1(linker.Symbols());
	constexpr unsigned OwnerCount = 8;
	Loader::Program owners[OwnerCount]; Resource resources[OwnerCount];
	std::vector<Pthread> workers;
	std::vector<std::unique_ptr<Loader::HostCallReturnThunk>> gateways;
	std::vector<std::unique_ptr<Loader::ApplicationImportBindings>> bindings;
	uint64_t entries[OwnerCount][EntryCount] {}, slots[OwnerCount][EntryCount] {};
	struct Restore {
		Loader::Program* previous; Loader::Program* owners; Resource* resources; std::vector<Pthread>& workers;
		~Restore() {
			release_pin.store(true);
			g_runtime_rwlock_pinned_for_test.store(nullptr); g_runtime_rwlock_publish_for_test.store(nullptr);
			g_runtime_rwlock_init_publish_for_test.store(nullptr);
			g_runtime_rwlock_attr_destroy_error_for_test.store(0);
			g_runtime_rwlock_attr_init_publish_for_test.store(nullptr);
			for (unsigned i = 0; i != OwnerCount; ++i) { if (owners[i].application_execution) { owners[i].application_execution->RequestStop(); } }
			for (auto worker: workers) { if (worker && !worker->free) { PthreadJoin(worker, nullptr); } }
			for (unsigned i = 0; i != OwnerCount; ++i) {
				Loader::RuntimeLinker::SetCurrentProgram(&owners[i]);
				if (resources[i].lock) { PthreadRwlockDestroy(&resources[i].lock); }
				if (resources[i].attr) { PthreadRwlockattrDestroy(&resources[i].attr); }
			}
			Loader::RuntimeLinker::SetCurrentProgram(previous);
		}
	} restore {Loader::RuntimeLinker::GetCurrentProgram(), owners, resources, workers};
	const std::array<const char*, EntryCount> nids {"6ULAa0fq4jA", "BB+kb08Tl9A", "Ox9i0c7L5w0", "mqdNorrB+gI", "XD3mDeybCnk",
	    "bIHoZCTomsI", "iPtZRWICjrM", "adh--6nIqTk", "+L98PIbGttk", "xFebsA4YsFI", "qsdmgXjqSgk", "h-OifiouBd8", nullptr,
	    "ytQULN-nhL4", "1471ajPzxh0", "sIlRvQqsN2Y"};
	const std::array<const char*, EntryCount> expressions {"LibKernel::PthreadRwlockInit", "LibKernel::PthreadRwlockDestroy",
	    "LibKernel::PthreadRwlockRdlock", "LibKernel::PthreadRwlockWrlock", "LibKernel::PthreadRwlockTryrdlock", "LibKernel::PthreadRwlockTrywrlock",
	    "LibKernel::PthreadRwlockTimedrdlock", "LibKernel::PthreadRwlockTimedwrlock", "LibKernel::PthreadRwlockUnlock",
	    "LibKernel::PthreadRwlockattrInit", "LibKernel::PthreadRwlockattrDestroy", "LibKernel::PthreadRwlockattrSettype", nullptr,
	    "Posix::pthread_rwlock_init", "Posix::pthread_rwlock_destroy", "Posix::pthread_rwlock_wrlock"};
	for (unsigned i = 0; i != OwnerCount; ++i) {
		auto& owner = owners[i]; owner.rt = &linker; owner.application_owner = &owner; owner.unique_id = 995000 + i;
		if (!check(Loader::InitializeApplicationExecution(&owner), "initialize rwlock owner")) { return false; }
		owner.dynamic_info = std::make_unique<Loader::DynamicInfo>();
		owner.dynamic_info->import_libs = {{"A", 1, "libkernel"}};
		owner.dynamic_info->import_modules = {{"B", 1, 1, "libkernel"}};
		bindings.push_back(std::make_unique<Loader::ApplicationImportBindings>(&owner));
		for (unsigned op = 0; op != EntryCount; ++op) {
			uint64_t target = reinterpret_cast<uint64_t>(PthreadRwlockattrGettype);
			if (nids[op]) {
				const auto* record = linker.Symbols()->FindRegistration({nids[op], op >= PosixInit ? "Posix" : "libkernel", 1, "libkernel", 1, 1,
				    Loader::SymbolType::Func}, expressions[op]);
				if (!check(record != nullptr, "exact kernel-domain rwlock registration exists")) { return false; }
				target = record->vaddr;
				if (op <= Unlock) {
					if (!check(bindings.back()->BindResolved(nids[op], *record) != 0, "only decoded kernel-domain rwlock family is admitted")) { return false; }
					entries[i][op] = bindings.back()->Bind(reinterpret_cast<uint64_t>(&slots[i][op]), std::string(nids[op]) + "#A#B");
					if (!check(entries[i][op] != 0, "create immutable raw rwlock import")) { return false; }
					continue;
				}
			}
			// Attributes are implemented and tested, NOT blanket admitted. Gettype
			// has no actual source registration, so do not invent an export for it.
			auto execution = owner.application_execution;
			auto gateway = Loader::HostCallReturnThunk::Create(execution->Boundary(), target, execution.get(),
			    Loader::RuntimeGuestExecution::EnterHostCall, Loader::RuntimeGuestExecution::LeaveHostCall);
			if (!check(bool(gateway), "create typed attribute return gateway")) { return false; }
			entries[i][op] = gateway->GetAddress(); gateways.push_back(std::move(gateway));
		}
	}
	const auto run = [&](Call& call, unsigned owner, unsigned resource, Operation operation, bool join = true) {
		call.owner = &owners[owner]; call.resource = &resources[resource]; call.entries = entries[owner]; call.operation = operation;
		Loader::RuntimeLinker::SetCurrentProgram(call.owner); Pthread worker = nullptr;
		if (PthreadCreateRuntimeCoordinator(&worker, nullptr, Coordinator, &call, "rwlock-lifecycle")) { return false; }
		workers.push_back(worker);
		return !join || (PthreadJoin(worker, nullptr) == 0 && call.done.load());
	};
	const auto returned = [](const Call& call, int status = OK) {
		return call.result.Returned() && call.result.call.value == static_cast<uint32_t>(status) && call.observed == status && call.continued == 1;
	};
	Call setup[OwnerCount];
	for (unsigned i = 0; i != OwnerCount - 1; ++i) {
		if (!check(run(setup[i], i, i, i == 2 ? Static : Setup) && returned(setup[i]), "checked init/attributes and NULL-static trywrite on real guest stacks")) {
			if (failure) { *failure += fmt::format(" owner={} observed={:x} continued={} returned={} value={:x} failure={:x}", i,
			    static_cast<uint32_t>(setup[i].observed), setup[i].continued, setup[i].result.Returned(), setup[i].result.call.value,
			    owners[i].application_execution->Failure().detail); }
			return false;
		}
	}
	Call exercise;
	if (!check(run(exercise, 2, 2, Exercise) && returned(exercise), "recursive reads, write/read ownership, self deadlock, busy destroy and EPERM through raw imports")) { return false; }
	Call foreign, foreign_attr;
	if (!check(run(foreign, 2, 1, TryRead) && returned(foreign, KERNEL_ERROR_EOPNOTSUPP) &&
	    run(foreign_attr, 2, 1, ForeignAll) && returned(foreign_attr, KERNEL_ERROR_EOPNOTSUPP), "foreign owner rejected by entire rwlock/attribute family without mutation or output clobber")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	const auto old = resources[1].lock;
	if (!check(PthreadRwlockTryrdlock(&resources[1].lock) == KERNEL_ERROR_EOPNOTSUPP &&
	    PthreadRwlockInit(&resources[1].lock, nullptr, nullptr) == KERNEL_ERROR_EOPNOTSUPP && resources[1].lock == old,
	    "session cannot acquire/reinitialize shared rwlock")) { return false; }
	// Only genuinely session-owned slots follow the legacy static/shadow route.
	PthreadRwlock session = nullptr;
	PthreadRwlockattr session_attr = nullptr;
	if (!check(PthreadRwlockTrywrlock(&session) == OK && PthreadRwlockattrInit(&session_attr) == OK,
	    "session NULL-static compatibility")) { return false; }
	resources[7].lock = session; resources[7].attr = session_attr;
	Call foreign_session;
	if (!check(run(foreign_session, 2, 7, ForeignAll) && returned(foreign_session, KERNEL_ERROR_EOPNOTSUPP),
	    "shared owner cannot adopt session lock/attrs through any wrapper")) { return false; }
	resources[7] = {};
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	if (!check(PthreadRwlockUnlock(&session) == OK && PthreadRwlockDestroy(&session) == OK && PthreadRwlockattrDestroy(&session_attr) == OK,
	    "session holder survives rejected shared access and releases normally")) { return false; }
	// Exception rollback after typed/live/slot/shadow preparation: preserve an
	// existing tombstone, remove a newly prepared slot, never publish a handle.
	PthreadRwlock transaction_slots[2] {};
	const auto tombstone = reinterpret_cast<uint64_t>(&transaction_slots[0]);
	const auto fresh_slot = reinterpret_cast<uint64_t>(&transaction_slots[1]);
	if (!check(PthreadRwlockInit(&transaction_slots[0], nullptr, nullptr) == OK &&
	    PthreadRwlockDestroy(&transaction_slots[0]) == OK && !g_rwlock_slots.contains(fresh_slot), "prepare session rollback slot/tombstone")) { return false; }
	const auto before_rollback = g_rwlocks.size();
	g_runtime_rwlock_init_publish_for_test.store(OomHook);
	const auto fresh_error = PthreadRwlockInit(&transaction_slots[1], nullptr, nullptr);
	const bool fresh_live_removed = !PthreadHostObjectAlive(failed_preparation.load());
	const auto tombstone_error = PthreadRwlockInit(&transaction_slots[0], nullptr, nullptr);
	g_runtime_rwlock_init_publish_for_test.store(nullptr);
	const void* shadow = nullptr;
	if (!check(fresh_error == KERNEL_ERROR_ENOMEM && tombstone_error == KERNEL_ERROR_ENOMEM && !transaction_slots[0] && !transaction_slots[1] &&
	    fresh_live_removed && !PthreadHostObjectAlive(failed_preparation.load()) && g_rwlocks.size() == before_rollback &&
	    !g_rwlock_slots.contains(fresh_slot) && g_rwlock_slots.contains(tombstone) && !g_rwlock_slots.at(tombstone).value &&
	    !PthreadShadowGet(fresh_slot, PthreadStaticObject::Type::Rwlock, &shadow) && !PthreadShadowGet(tombstone, PthreadStaticObject::Type::Rwlock, &shadow),
	    "bad_alloc rolls back new typed/live/slot/shadow entries without erasing existing authority")) { return false; }
	// Native HOST stack only: Windows guest-stack C++ EH remains unsupported.
	Loader::RuntimeLinker::SetCurrentProgram(&owners[7]);
	g_runtime_rwlock_init_publish_for_test.store(OomHook);
	const auto owned_host_oom = PthreadRwlockInit(&resources[7].lock, nullptr, nullptr);
	g_runtime_rwlock_init_publish_for_test.store(nullptr);
	if (!check(owned_host_oom == KERNEL_ERROR_ENOMEM && !resources[7].lock &&
	    g_rwlocks.size() == before_rollback && !PthreadHostObjectAlive(failed_preparation.load()),
	    "matching owned HOST init catches preparation exception on native host stack")) { return false; }
	const auto before_attribute_oom = g_rwlock_attributes.size();
	Loader::RuntimeLinker::SetCurrentProgram(&owners[2]);
	PthreadRwlockattr attribute_oom = nullptr;
	g_runtime_rwlock_attr_init_publish_for_test.store(AttributeOomHook);
	const auto attr_oom = PthreadRwlockattrInit(&attribute_oom);
	if (!check(attr_oom == KERNEL_ERROR_ENOMEM && !attribute_oom &&
	    g_rwlock_attributes.size() == before_attribute_oom, "attribute exception releases unpublished native state without allocating rollback storage")) { return false; }
	g_runtime_rwlock_attr_destroy_error_for_test.store(EINVAL);
	const auto attr_retained = PthreadRwlockattrInit(&attribute_oom);
	g_runtime_rwlock_attr_init_publish_for_test.store(nullptr); g_runtime_rwlock_attr_destroy_error_for_test.store(0);
	if (!check(attr_retained == KERNEL_ERROR_ENOMEM && !attribute_oom &&
	    g_rwlock_attributes.size() == before_attribute_oom + 1 && g_rwlock_attributes.contains(failed_attribute.load()),
	    "failed rollback release retains preallocated native attribute node even during bad_alloc")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[2]);
	auto retained_unpublished = failed_attribute.load();
	if (!check(PthreadRwlockattrDestroy(&retained_unpublished) == OK && !retained_unpublished && g_rwlock_attributes.size() == before_attribute_oom,
	    "retained unpublished attribute can be released only by later healthy matching HOST cleanup")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(PthreadRwlockWrlock(&resources[0].lock) == OK, "hold rwlock for real contention")) { return false; }
	Call busy_read, busy_write, timed_read, timed_write;
	if (!check(run(busy_read, 0, 0, TryRead) && returned(busy_read, KERNEL_ERROR_EBUSY) &&
	    run(busy_write, 0, 0, TryWrite) && returned(busy_write, KERNEL_ERROR_EBUSY) &&
	    run(timed_read, 0, 0, TimedRead) && returned(timed_read, KERNEL_ERROR_ETIMEDOUT) &&
	    run(timed_write, 0, 0, TimedWrite) && returned(timed_write, KERNEL_ERROR_ETIMEDOUT),
	    "raw try/timed read+write preserve encoded kernel errno, not positive POSIX errno")) { return false; }
	Call reader, writer;
	if (!check(run(reader, 0, 0, Read, false) && run(writer, 0, 0, Write, false) && Until([&] {
		std::lock_guard registry(g_rwlock_registry_mutex); std::lock_guard object(resources[0].lock->m);
		return g_rwlocks.at(resources[0].lock).use_count() == 3 && resources[0].lock->waiting_writers == 1;
	}), "park actual reader and writer contenders")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(PthreadRwlockDestroy(&resources[0].lock) == KERNEL_ERROR_EBUSY && PthreadRwlockUnlock(&resources[0].lock) == OK &&
	    Until([&] { return reader.done.load() && writer.done.load(); }) && returned(reader) && returned(writer),
	    "queued acquisition/destroy exclusion and normal real contention progress")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	g_runtime_rwlock_publish_for_test.store(OomHook);
	const auto acquisition_oom = PthreadRwlockTryrdlock(&resources[0].lock);
	g_runtime_rwlock_publish_for_test.store(nullptr);
	if (!check(acquisition_oom == KERNEL_ERROR_ENOMEM && !resources[0].lock->reader_count &&
	    !resources[0].lock->writer && !resources[0].lock->waiting_writers && g_rwlocks.at(resources[0].lock).use_count() == 1,
	    "HOST bad_alloc unwinds mutex/object pin without publishing acquisition")) { return false; }
	// Stop both wait kinds without releasing the holder first. Terminal waiters
	// return through host RAII; they neither signal-dispatch nor resume guest code.
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(PthreadRwlockWrlock(&resources[0].lock) == OK, "hold before terminal wait cancellation")) { return false; }
	static_assert(sizeof(KernelUseconds) == 4 && KernelUseconds(-1) == UINT32_MAX);
	Call stopped_reader, stopped_writer, stopped_maximum_timeout;
	stopped_maximum_timeout.timeout = UINT32_MAX; // 4294.967295 seconds, NOT overflowing chrono microseconds.
	if (!check(run(stopped_reader, 0, 0, Read, false) && run(stopped_writer, 0, 0, Write, false) &&
	    run(stopped_maximum_timeout, 0, 0, TimedRead, false) && Until([&] {
		std::lock_guard registry(g_rwlock_registry_mutex); std::lock_guard object(resources[0].lock->m);
		return g_rwlocks.at(resources[0].lock).use_count() == 4 && resources[0].lock->waiting_writers == 1;
	}), "both terminal waiters are really queued")) { return false; }
	owners[0].application_execution->RequestStop();
	if (!check(Until([&] { return stopped_reader.done.load() && stopped_writer.done.load() && stopped_maximum_timeout.done.load(); }) &&
	    stopped_reader.result.stopped && stopped_writer.result.stopped && stopped_maximum_timeout.result.stopped &&
	    !stopped_reader.continued && !stopped_writer.continued && !stopped_maximum_timeout.continued &&
	    resources[0].lock->waiting_writers == 0 && resources[0].lock->reader_count == 0,
	    "stop removes writer accounting and no post-stop acquisition/guest continuation")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[0]);
	if (!check(PthreadRwlockUnlock(&resources[0].lock) == OK, "trusted boundary-free HOST holder release after canceled workers")) { return false; }
	// Race stop with an AVAILABLE acquisition immediately before lease publication.
	Call publication_stop;
	g_runtime_rwlock_publish_for_test.store(StopHook);
	const auto stop_ok = run(publication_stop, 1, 1, Write);
	g_runtime_rwlock_publish_for_test.store(nullptr);
	if (!check(stop_ok && publication_stop.result.stopped && !publication_stop.continued && !resources[1].lock->writer,
	    "stop wins available-acquisition publication; no held lock leaks")) { return false; }
	Call publication_exit;
	g_runtime_rwlock_publish_for_test.store(ExitHook);
	const auto exit_ok = run(publication_exit, 2, 2, Read);
	g_runtime_rwlock_publish_for_test.store(nullptr);
	Call retry;
	if (!check(exit_ok && publication_exit.result.ThreadExited() && !publication_exit.continued && !resources[2].lock->reader_count &&
	    !owners[2].application_execution->StopRequested() && run(retry, 2, 2, TryWrite) && returned(retry),
	    "actual-thread exit rolls back acquisition, does not fail owner, root permits healthy retry")) { return false; }
	Call publication_fail;
	g_runtime_rwlock_publish_for_test.store(FailHook);
	const auto fail_ok = run(publication_fail, 3, 3, Read);
	g_runtime_rwlock_publish_for_test.store(nullptr);
	if (!check(fail_ok && !publication_fail.result.Returned() && !publication_fail.continued && !resources[3].lock->reader_count &&
	    owners[3].application_execution->Failure().reason == Loader::GuestFailureReason::InvalidTransition,
	    "owner failure remains sticky and is not an actual-thread exit")) { return false; }
	// A call pinned before taking m is still busy even with NO holder/waiter.
	Call pinned_call;
	g_runtime_rwlock_pinned_for_test.store(PinHook);
	if (!check(run(pinned_call, 4, 4, Write, false) && Until([] { return pinned.load(); }), "pause idle typed call pin")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[4]);
	const bool busy_pin = PthreadRwlockDestroy(&resources[4].lock) == KERNEL_ERROR_EBUSY;
	owners[4].application_execution->RequestStop(); release_pin.store(true);
	g_runtime_rwlock_pinned_for_test.store(nullptr);
	if (!check(busy_pin && Until([&] { return pinned_call.done.load(); }) && pinned_call.result.stopped && !pinned_call.continued && !resources[4].lock->writer,
	    "destroy refuses full retained call lifetime; stop prevents late acquisition")) { return false; }
	const auto before_init = g_rwlocks.size();
	Call initialization_stop;
	g_runtime_rwlock_init_publish_for_test.store(StopHook);
	const auto init_ok = run(initialization_stop, 7, 7, Setup);
	g_runtime_rwlock_init_publish_for_test.store(nullptr);
	if (!check(init_ok && initialization_stop.result.stopped && !initialization_stop.continued && !resources[7].lock &&
	    g_rwlocks.size() == before_init, "stop racing explicit init rolls back unpublished typed object and guest output")) { return false; }
	for (const auto* alias: {"sIlRvQqsN2Y", "iGjsr1WAtI0", "EgmLo6EWgso", "SFxTMOfuCkE", "XhWHn6P5R7U"}) {
		const auto* record = linker.Symbols()->FindByNid(alias, Loader::SymbolType::Func);
		if (!check(record && !bindings[5]->BindResolved(alias, *record), "unverified same-body/error-domain aliases remain unadmitted")) { return false; }
	}
	const auto* kernel_alias = linker.Symbols()->FindRegistration({"sIlRvQqsN2Y", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func},
	    "LibKernel::PthreadRwlockWrlock");
	if (!check(kernel_alias && !bindings[5]->BindResolved("sIlRvQqsN2Y", *kernel_alias),
	    "same NID's kernel-status alias is distinct from Posix registration and remains closed")) { return false; }
	Call destroyed;
	Loader::RuntimeLinker::SetCurrentProgram(&owners[5]);
	const auto retained_attr = resources[5].attr;
	g_runtime_rwlock_attr_destroy_error_for_test.store(EINVAL);
	const auto failed_attr_release = PthreadRwlockattrDestroy(&resources[5].attr);
	g_runtime_rwlock_attr_destroy_error_for_test.store(0);
	if (!check(failed_attr_release == KERNEL_ERROR_EINVAL && resources[5].attr == retained_attr && g_rwlock_attributes.contains(retained_attr),
	    "injected native attribute release failure preserves handle and retained storage")) { return false; }
	if (!check(run(destroyed, 5, 5, Destroy) && returned(destroyed) && !resources[5].lock && !resources[5].attr,
	    "raw successful destroy + attribute destruction clear only checked slots")) { return false; }
	Call abandoned, recycled, wrong_unlock;
	if (!check(run(abandoned, 6, 6, Abandon) && returned(abandoned) &&
	    run(recycled, 6, 6, TryWrite) && returned(recycled, KERNEL_ERROR_EBUSY) &&
	    run(wrong_unlock, 6, 6, Unlock) && returned(wrong_unlock, KERNEL_ERROR_EPERM),
	    "native-thread death/Pthread reuse cannot acquire or release an abandoned incarnation")) { return false; }
	Loader::RuntimeLinker::SetCurrentProgram(&owners[6]);
	const auto retained = resources[6].lock;
	if (!check(PthreadRwlockDestroy(&resources[6].lock) == KERNEL_ERROR_EBUSY && resources[6].lock == retained,
	    "failed abandoned-holder release/destroy retains typed storage, not fake robust recovery")) { return false; }
	// Join ALL native workers before declaring drain or allowing fixture storage
	// to disappear. Logical done/results alone are never native termination proof.
	for (auto worker: workers) { if (!worker->free && PthreadJoin(worker, nullptr)) { return check(false, "native worker join"); } }
	return true;
}
