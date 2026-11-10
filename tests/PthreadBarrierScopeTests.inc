// Firmware 13.60 contracts: docs/vsh-run19-kernel-imports-20261104.md, sections 2-5.
// This include uses the existing kernel_file_system_tests target; no new build graph.
void TestPthreadContractRegistrations() {
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  struct Entry { const char* nid; uint64_t address; const char* expression; };
  const Entry entries[] = {
      {"+Pqub9HZCPo", reinterpret_cast<uint64_t>(Libs::Posix::pthread_barrier_destroy), "Posix::pthread_barrier_destroy"},
      {"CawZgCYqXWk", reinterpret_cast<uint64_t>(Libs::Posix::pthread_barrier_wait), "Posix::pthread_barrier_wait"},
      {"ZsXLFtd2jqQ", reinterpret_cast<uint64_t>(Libs::Posix::pthread_barrier_init), "Posix::pthread_barrier_init"},
      {"xesmlSI-KCI", reinterpret_cast<uint64_t>(Libs::Posix::pthread_attr_setscope), "Posix::pthread_attr_setscope"}};
  for (const auto& entry : entries) {
    Loader::SymbolResolve query {entry.nid, "libScePosix", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    const auto* record = symbols.FindRegistration(query, entry.expression);
    Check(record != nullptr && record->vaddr == entry.address,
          "barrier/scope exact Posix v1 / libkernel 1.1 typed registration");
    query.library_version = 2;
    Check(symbols.FindRegistration(query, entry.expression) == nullptr,
          "no new library-version alias for barrier/scope");
    query.library_version = 1;
    query.module_version_minor = 0;
    Check(symbols.FindRegistration(query, entry.expression) == nullptr,
          "no new module-version alias for barrier/scope");
    query.module_version_minor = 1;
    query.type = Loader::SymbolType::Object;
    Check(symbols.FindRegistration(query, entry.expression) == nullptr,
          "barrier/scope exports are functions, not objects");
  }
}

void TestPthreadBarrierContracts() {
  using namespace Libs::Posix;
  using Libs::Posix::pthread_barrier_init;
  using Libs::Posix::pthread_barrier_wait;
  using Libs::Posix::pthread_barrier_destroy;
  using Libs::LibKernel::PthreadBarrier;
  auto* guest_errno = GetErrorAddr();
  *guest_errno = 1777;
  PthreadBarrier barrier = nullptr;
  Check(pthread_barrier_init(nullptr, nullptr, 1) == POSIX_EINVAL &&
            pthread_barrier_init(&barrier, nullptr, 0) == POSIX_EINVAL && barrier == nullptr &&
            pthread_barrier_wait(nullptr) == POSIX_EINVAL &&
            pthread_barrier_wait(&barrier) == POSIX_EINVAL &&
            pthread_barrier_destroy(nullptr) == POSIX_EINVAL &&
            pthread_barrier_destroy(&barrier) == POSIX_EINVAL,
        "barrier invalid slots/count/handles use direct positive errors");
  Check(pthread_barrier_init(&barrier,
            reinterpret_cast<const Libs::LibKernel::PthreadBarrierattr*>(1), 1) == 0,
        "13.60 init ignores attr instead of dereferencing it");
  for (unsigned i = 0; i < 100; ++i) {
    Check(pthread_barrier_wait(&barrier) == -1, "count-one barrier is serial every generation");
  }
  auto stale = barrier;
  Check(pthread_barrier_destroy(&barrier) == 0 && barrier == nullptr &&
            pthread_barrier_destroy(&barrier) == POSIX_EINVAL,
        "destroy clears slot and double destroy fails");
  Check(pthread_barrier_init(&barrier, nullptr, UINT32_MAX) == 0 &&
            pthread_barrier_destroy(&barrier) == 0,
        "unsigned maximum count is accepted without signed truncation");
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
  barrier = stale;
  TestPthreadBarrierFailNextAllocation();
  Check(pthread_barrier_init(&barrier, nullptr, 2) == POSIX_ENOMEM && barrier == stale,
        "allocation failure preserves caller slot");
  barrier = nullptr;
#endif
  Check(*guest_errno == 1777, "barrier errors and serial result preserve guest errno");

  constexpr unsigned participants = 4, rounds = 128;
  Check(pthread_barrier_init(&barrier, nullptr, participants) == 0, "cyclic barrier init");
  std::array<unsigned, participants> published {};
  std::array<std::atomic<unsigned>, rounds * 2> serial {};
  std::array<std::thread, participants> workers;
  for (unsigned i = 0; i < participants; ++i) {
    workers[i] = std::thread([&, i] {
      *GetErrorAddr() = 1888;
      for (unsigned round = 0; round < rounds; ++round) {
        published[i] = round + 1;
        auto result = pthread_barrier_wait(&barrier);
        Check(result == 0 || result == -1, "cyclic barrier actually completes");
        if (result == -1) ++serial[2 * round];
        for (auto value : published) {
          Check(value == round + 1, "barrier publishes every participant's writes");
        }
        result = pthread_barrier_wait(&barrier);
        Check(result == 0 || result == -1, "second phase prevents next-round write races");
        if (result == -1) ++serial[2 * round + 1];
        Check(*GetErrorAddr() == 1888 && Common::GetHostLockCriticalCounter()->load() == 0,
              "ordinary and serial returns preserve errno and balance critical accounting");
      }
    });
  }
  for (auto& worker : workers) worker.join();
  for (const auto& count : serial) Check(count == 1, "exactly one serial thread per generation");
  Check(pthread_barrier_destroy(&barrier) == 0 && pthread_barrier_wait(&stale) == POSIX_EINVAL,
        "stale opaque token never resolves to a later host allocation");

  // Same-slot concurrent lookup/destroy/re-init is serialized by the registry.
  Check(pthread_barrier_init(&barrier, nullptr, 1) == 0, "destroy race init");
  std::atomic<bool> stop {false};
  std::thread racer([&] {
    while (!stop.load()) {
      const int result = pthread_barrier_wait(&barrier);
      Check(result == -1 || result == POSIX_EINVAL, "wait/destroy race has only valid outcomes");
    }
  });
  for (unsigned i = 0; i < 1000; ++i) {
    Check(pthread_barrier_destroy(&barrier) == 0 && pthread_barrier_init(&barrier, nullptr, 1) == 0,
          "count-one destroy/re-init race preserves registry integrity");
  }
  stop = true;
  racer.join();
  Check(pthread_barrier_destroy(&barrier) == 0 && *guest_errno == 1777,
        "destroy race cleanup and errno preservation");
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
std::atomic<bool> barrier_release_parked {false}, barrier_release_continue {false};
void ParkReleasedBarrierWaiter() {
  Check(Common::GetHostLockCriticalCounter()->load() == 0,
        "released waiter callback is outside host lock critical region");
  barrier_release_parked = true;
  WaitForMutexTest([] { return barrier_release_continue.load(); });
}

void WaitForBarrierArrivals(Libs::LibKernel::PthreadBarrier* barrier, unsigned expected) {
  WaitForMutexTest([&] {
    unsigned arrivals = 0;
    return Libs::Posix::TestPthreadBarrierArrivals(barrier, &arrivals) && arrivals == expected;
  });
}

void TestPthreadBarrierReleasedLifetime() {
  using namespace Libs::Posix;
  Libs::LibKernel::PthreadBarrier barrier = nullptr;
  Check(pthread_barrier_init(&barrier, nullptr, 2) == 0, "released-lifetime init");
  auto stale = barrier;
  barrier_release_parked = false;
  barrier_release_continue = false;
  TestPthreadBarrierSetReleaseCallback(ParkReleasedBarrierWaiter);
  std::thread waiter([&] { Check(pthread_barrier_wait(&barrier) == 0, "released waiter returns normally"); });
  WaitForBarrierArrivals(&barrier, 1);
  Check(pthread_barrier_destroy(&barrier) == POSIX_EBUSY && barrier == stale,
        "destroy with arrivals is busy and does not clear the slot");
  Check(pthread_barrier_wait(&barrier) == -1, "last arrival releases old generation");
  WaitForMutexTest([] { return barrier_release_parked.load(); });
  Check(pthread_barrier_destroy(&barrier) == 0 && barrier == nullptr &&
            pthread_barrier_init(&barrier, nullptr, 1) == 0 &&
            pthread_barrier_wait(&barrier) == -1 &&
            pthread_barrier_wait(&stale) == POSIX_EINVAL,
        "destroy/re-init succeeds while released waiter still pins old object");
  barrier_release_continue = true;
  waiter.join();
  TestPthreadBarrierSetReleaseCallback(nullptr);
  Check(pthread_barrier_destroy(&barrier) == 0, "released-lifetime cleanup");
}

struct BarrierGuestWaiter {
  Libs::LibKernel::PthreadBarrier* barrier;
  bool cancel = false;
  std::atomic<bool> signal_parked {false}, signal_continue {false}, completed {false};
  int result = 99;
};
thread_local BarrierGuestWaiter* current_barrier_waiter = nullptr;

void KYTY_SYSV_ABI ParkGuestBarrierSignal(int, void*) {
  Check(Common::GetHostLockCriticalCounter()->load() == 0,
        "barrier polls guest signals with no host locks marked critical");
  auto& waiter = *current_barrier_waiter;
  waiter.signal_parked = true;
  WaitForMutexTest([&] { return waiter.signal_continue.load(); });
}

void* KYTY_SYSV_ABI RunGuestBarrierWaiter(void* opaque) {
  auto& state = *static_cast<BarrierGuestWaiter*>(opaque);
  current_barrier_waiter = &state;
  *Libs::Posix::GetErrorAddr() = 1999;
  state.result = Libs::Posix::pthread_barrier_wait(state.barrier);
  Check(*Libs::Posix::GetErrorAddr() == 1999 && Common::GetHostLockCriticalCounter()->load() == 0,
        "guest waiter errno and critical counter preserved after host CV wait");
  state.completed = true;
  if (state.cancel) {
    Libs::LibKernel::PthreadTestcancel();
    Check(false, "pending guest cancellation fires at explicit cancellation point");
  }
  return nullptr;
}

void TestPthreadScopeAndGuestBarriers() {
  using namespace Libs::LibKernel;
  namespace Px = Libs::Posix;
  PthreadAttr attr = nullptr, copy = nullptr;
  Check(PthreadAttrInit(&attr) == 0 && PthreadAttrInit(&copy) == 0, "scope attributes init");
  int scope = -1;
  *Px::GetErrorAddr() = 2111;
  Check(Px::pthread_attr_setscope(nullptr, 2) == Px::POSIX_EINVAL &&
            Px::pthread_attr_setscope(&attr, 1) == Px::POSIX_EINVAL &&
            Px::pthread_attr_setscope(&attr, -1) == Px::POSIX_EINVAL &&
            PthreadAttrGetscope(&attr, &scope) == 0 && scope == 2,
        "scope defaults to backend SYSTEM and rejects other guest constants");
  for (int requested : {0, 2}) {
    Check(Px::pthread_attr_setscope(&attr, requested) == 0 &&
              PthreadAttrGetscope(&attr, &scope) == 0 && scope == requested &&
              TestPthreadAttrCopy(&copy, &attr) == 0 &&
              PthreadAttrGetscope(&copy, &scope) == 0 && scope == requested,
          "scope zero/two is retained and copied independently of other scheduling flags");
  }
  Check(Px::pthread_attr_setscope(&attr, 0) == 0, "PROCESS accepted at setter time");
  Pthread rejected = reinterpret_cast<Pthread>(uintptr_t {1});
  Check(Px::pthread_create(&rejected, &attr, RunGuestBarrierWaiter, nullptr) == Px::POSIX_EOPNOTSUPP &&
            rejected == reinterpret_cast<Pthread>(uintptr_t {1}),
        "unsupported PROCESS creation fails honestly without publishing a thread");
  Check(Px::pthread_attr_setscope(&attr, 2) == 0 && *Px::GetErrorAddr() == 2111,
        "scope setter/create direct errors preserve errno");

  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  auto* install_record = symbols.FindByNid("WkwEd3N7w0Y", Loader::SymbolType::Func);
  auto* remove_record = symbols.FindByNid("Qhv5ARAoOEc", Loader::SymbolType::Func);
  Check(install_record != nullptr && remove_record != nullptr, "guest signal test exports");
  auto install = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, void*)>(install_record->vaddr);
  auto remove = reinterpret_cast<int(KYTY_SYSV_ABI *)(int)>(remove_record->vaddr);
  Check(install(30, reinterpret_cast<void*>(ParkGuestBarrierSignal)) == 0, "install barrier signal handler");
  PthreadBarrier barrier = nullptr;
  Check(Px::pthread_barrier_init(&barrier, nullptr, 2) == 0, "guest barrier init");
  BarrierGuestWaiter state {&barrier};
  state.cancel = true;
  Pthread worker = nullptr;
  std::vector<uint8_t> stack(1024 * 1024);
  Check(PthreadAttrSetstack(&attr, stack.data(), stack.size()) == 0 &&
            PthreadCreate(&worker, &attr, RunGuestBarrierWaiter, &state, "BarrierScopeTest") == 0 &&
            PthreadAttrGet(worker, &copy) == 0 && PthreadAttrGetscope(&copy, &scope) == 0 && scope == 2,
        "SYSTEM scope maps to a real host thread and survives thread attribute copy");
  WaitForBarrierArrivals(&barrier, 1);
  PthreadQueuePendingSignal(worker, 30);
  WaitForMutexTest([&] { return state.signal_parked.load(); });
  Check(PthreadCancel(worker) == 0 && !state.completed.load() &&
            Px::pthread_barrier_destroy(&barrier) == Px::POSIX_EBUSY,
        "host cancellation cannot remove a queued barrier arrival");
  Check(Px::pthread_barrier_wait(&barrier) == -1 && Px::pthread_barrier_destroy(&barrier) == 0,
        "destroy is safe while guest waiter is parked in unlocked signal dispatch");
  state.signal_continue = true;
  void* result = nullptr;
  Check(PthreadJoin(worker, &result) == 0 && state.completed && state.result == 0 &&
            result == reinterpret_cast<void*>(~uintptr_t {0}),
        "barrier is not a cancellation point; explicit testcancel uses guest stack trampoline");
  Check(remove(30) == 0 && PthreadAttrDestroy(&attr) == 0 && PthreadAttrDestroy(&copy) == 0,
        "scope/signal fixtures cleanup");
  PthreadAttr missing = nullptr;
  Check(Px::pthread_attr_setscope(&missing, 2) == Px::POSIX_EINVAL,
        "scope rejects null attribute handle");
}
#endif
