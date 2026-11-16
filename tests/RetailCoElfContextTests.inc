// Real host threads and production suspend/get/resume exports. No fake context.
void TestRetailCoElfContexts() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  using namespace Libs::LibKernel;
  Common::Singleton<Loader::RuntimeLinker>::Instance();
  Initialize();
  if (!PthreadSelfOrNull()) { PthreadInitSelfForMainThread(); }
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto* s = symbols.FindByNid("cfjAjVTFG6A", Loader::SymbolType::Func);
  const auto* g = symbols.FindByNid("YkGOXpJEtO8", Loader::SymbolType::Func);
  const auto* r = symbols.FindByNid("QRdE7dBfNks", Loader::SymbolType::Func);
  Check(s && g && r, "context exports registered");
  using Suspend = int(KYTY_SYSV_ABI *)(Pthread);
  using Get = int(KYTY_SYSV_ABI *)(Pthread, void*);
  auto suspend = reinterpret_cast<Suspend>(s->vaddr);
  auto get = reinterpret_cast<Get>(g->vaddr);
  auto resume = reinterpret_cast<Suspend>(r->vaddr);
  std::atomic<Pthread> first {nullptr}, second {nullptr};
  std::atomic<bool> stop {false};
  const auto worker = [&](std::atomic<Pthread>& published) {
    PthreadInitSelfForMainThread();
    published.store(PthreadSelfOrNull(), std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) { Sleep(1); }
  };
  std::thread one(worker, std::ref(first));
  while (!first.load(std::memory_order_acquire)) { std::this_thread::yield(); }
  std::thread two(worker, std::ref(second));
  while (!second.load(std::memory_order_acquire)) { std::this_thread::yield(); }
  auto target = first.load(std::memory_order_acquire);
  PthreadLegacyNativeTarget pinned;
  Check(PthreadPinLegacyNativeTarget(target, &pinned) == OK && pinned.Handle() && pinned.ThreadId(),
        "first co-ELF main remains independently pinnable after second registration");
  const auto stack = pinned.StackAddress();
  const auto stack_size = pinned.StackSize();
  pinned.UnlockRecord();
  Check(suspend(target) == 0, "suspend actual live co-ELF native thread");
  std::array<uint64_t, 160> context {};
  Check(get(target, context.data()) == 0 && context[28] != 0 && stack != 0 && stack_size != 0 &&
        context[31] >= stack && context[31] - stack < stack_size,
        "suspended thread supplies real guest ABI RIP and RSP");
  Check(resume(target) == 0, "resume exact suspended native thread");
  Check(suspend(reinterpret_cast<Pthread>(0x12340)) == Libs::Posix::POSIX_ESRCH,
        "unknown pthread rejected as ESRCH, not successful or shared-owned");
  stop.store(true, std::memory_order_release);
  one.join(); two.join();
  PthreadLegacyNativeTarget dead;
  Check(PthreadPinLegacyNativeTarget(target, &dead) == KERNEL_ERROR_ESRCH && !dead.Handle(),
        "exited external main no longer admits native pin");
  Check(suspend(target) == Libs::Posix::POSIX_ESRCH,
        "exited external main returns ESRCH");
#endif
}
