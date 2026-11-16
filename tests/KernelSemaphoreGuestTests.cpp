// Windows companion fixture. Run after kernel/main-pthread/linker initialization.
#include "kernel/semaphore.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS) || !defined(_WIN32)
#error This fixture requires the Windows emulator test build
#endif

namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
namespace Libs::LibKernel {
int TestSuspendLoggingThread(Pthread);
int TestResumeLoggingThread(Pthread);
}
namespace {
namespace K = Libs::LibKernel;
namespace S = K::Semaphore;
using Handle = uint64_t;
static_assert(std::is_same_v<S::KernelSema, Handle> && sizeof(Handle) == 8);

void Check(bool ok, const char* message) {
 if (!ok) { std::fprintf(stderr, "KernelSemaphoreGuestTests: %s\n", message); std::abort(); }
}
struct Event {
 HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
 Event() { Check(h != nullptr, "CreateEvent"); }
 ~Event() { Check(CloseHandle(h) != 0, "CloseHandle"); }
 void Set() { Check(SetEvent(h) != 0, "SetEvent"); }
 void Wait(DWORD ms = 10000) { Check(WaitForSingleObject(h, ms) == WAIT_OBJECT_0, "event deadline"); }
};
struct Deadline {
 Event done;
 HANDLE thread;
 static DWORD WINAPI Run(void* p) {
  auto* d = static_cast<Deadline*>(p);
  Check(WaitForSingleObject(d->done.h, 90000) == WAIT_OBJECT_0, "overall 90-second deadline");
  return 0;
 }
 Deadline(): thread(CreateThread(nullptr, 0, Run, this, 0, nullptr)) { Check(thread != nullptr, "watchdog create"); }
 ~Deadline() { done.Set(); Check(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0, "watchdog join"); Check(CloseHandle(thread) != 0, "watchdog close"); }
};
struct Gate {
 Event entered, release;
 Gate* next = nullptr;
 S::SemaHookPoint next_point = S::SemaHookPoint::None;
 static void Hook(void* p) {
  auto& g = *static_cast<Gate*>(p);
  Check(Common::GetHostLockCriticalCounter()->load() == 0, "hook inside host critical region");
  g.entered.Set(); g.release.Wait();
  if (g.next) S::SemaHookForTest(g.next_point, Hook, g.next);
 }
};
struct Sigaction { uint64_t handler = 0; int32_t flags = 0, pad = 0; uint64_t mask[2] = {}; };
static_assert(sizeof(Sigaction) == 32);
struct Api {
 using Create = int (KYTY_SYSV_ABI *)(Handle*, const char*, uint32_t, int, int, void*);
 using Unary = int (KYTY_SYSV_ABI *)(Handle);
 using Count = int (KYTY_SYSV_ABI *)(Handle, int);
 using Wait = int (KYTY_SYSV_ABI *)(Handle, int, uint32_t*);
 using Action = int (KYTY_SYSV_ABI *)(int, const Sigaction*, Sigaction*);
 Create create; Unary remove; Count signal; Wait wait; Action action;
 template<class T> static T Get(Loader::SymbolDatabase& db, const char* nid) {
  auto* e = db.FindByNid(nid, Loader::SymbolType::Func);
  Check(e && e->vaddr, "missing libkernel NID"); return reinterpret_cast<T>(e->vaddr);
 }
 Api() {
  Loader::SymbolDatabase db; Libs::InitLibKernel_1(&db);
  create = Get<Create>(db, "188x57JYp0g"); remove = Get<Unary>(db, "R1Jvn8bSCW8");
  signal = Get<Count>(db, "4czppHBiriw"); wait = Get<Wait>(db, "Zxa0VhQVTsk");
  action = Get<Action>(db, "KiJEPEWRyUY");
 }
 Handle Make(const char* name, int initial = 0) const {
  Handle h = 0; Check(create(&h, name, 1, initial, 1, nullptr) == 0 && h, "create semaphore"); return h;
 }
};
struct Worker {
 const Api* api; Handle id; Gate* gate; S::SemaHookPoint point;
 bool deleting = false; int result = -1; K::Pthread thread = nullptr;
 static void* KYTY_SYSV_ABI Run(void* p) {
  auto& w = *static_cast<Worker*>(p);
  S::SemaHookForTest(w.point, Gate::Hook, w.gate);
  w.result = w.deleting ? w.api->remove(w.id) : w.api->wait(w.id, 1, nullptr);
  return nullptr;
 }
 void Start() { Check(K::PthreadCreate(&thread, nullptr, Run, this, "sema-guest-test") == 0, "PthreadCreate"); }
 void Join() { Check(K::PthreadJoin(thread, nullptr) == 0, "PthreadJoin"); }
};
void SuspendAndProbe(const Api& api, K::Pthread t, Handle independent) {
 Check(K::TestSuspendLoggingThread(t) == 0, "production suspend");
 Check(!K::PthreadIsHostLockCritical(t), "suspended critical counter");
 Check(api.signal(independent, 1) == 0, "independent signal while suspended");
 Check(api.wait(independent, 1, nullptr) == 0, "independent wait while suspended");
 Check(K::TestResumeLoggingThread(t) == 0, "production resume");
}
void ParkAndRetire(const Api& api, Handle probe) {
 {
  Handle id = api.Make("guest-park"); Gate gate;
  Worker w {&api, id, &gate, S::SemaHookPoint::BeforePark}; w.Start(); gate.entered.Wait();
  SuspendAndProbe(api, w.thread, probe);
  Check(api.signal(id, 1) == 0, "signal parked waiter"); gate.release.Set(); w.Join();
  Check(w.result == 0 && api.remove(id) == 0, "park result and cleanup");
 }
 {
  Handle id = api.Make("guest-retire"); Gate enqueued, retire;
  enqueued.next = &retire; enqueued.next_point = S::SemaHookPoint::BeforeRetire;
  Worker w {&api, id, &enqueued, S::SemaHookPoint::Enqueued}; w.Start(); enqueued.entered.Wait();
  Check(api.signal(id, 1) == 0, "signal enrolled waiter"); enqueued.release.Set(); retire.entered.Wait();
  SuspendAndProbe(api, w.thread, probe); retire.release.Set(); w.Join();
  Check(w.result == 0 && api.remove(id) == 0, "retire result and cleanup");
 }
}
void Drain(const Api& api, Handle probe) {
 Handle id = api.Make("guest-drain"); Gate enrolled, retire, drain;
 enrolled.next = &retire; enrolled.next_point = S::SemaHookPoint::BeforeRetire;
 Worker waiter {&api, id, &enrolled, S::SemaHookPoint::Enqueued}; waiter.Start(); enrolled.entered.Wait();
 Check(api.signal(id, 1) == 0, "complete waiter before retirement"); enrolled.release.Set(); retire.entered.Wait();
 Worker deleter {&api, id, &drain, S::SemaHookPoint::DrainPark, true}; deleter.Start(); drain.entered.Wait();
 Check(K::TestSuspendLoggingThread(deleter.thread) == 0, "suspend deleter");
 Check(!K::PthreadIsHostLockCritical(deleter.thread), "drain suspended critical counter");
 Check(api.signal(probe, 1) == 0 && api.wait(probe, 1, nullptr) == 0, "independent operation during drain suspension");
 retire.release.Set(); waiter.Join();
 Check(K::TestResumeLoggingThread(deleter.thread) == 0, "resume deleter"); drain.release.Set(); deleter.Join();
 Check(waiter.result == 0 && deleter.result == 0, "drain and retirement results");
}
struct SignalState {
 const Api* api; Handle id; Event finished; std::atomic<int> result {-1};
 std::atomic<DWORD> host_tid {0}; std::atomic<K::Pthread> self {nullptr};
 std::atomic<unsigned> critical {UINT32_MAX};
 HANDLE dispatch_thread = nullptr;
};
SignalState* g_signal = nullptr;
void KYTY_SYSV_ABI SignalHandler(int, void*) {
 auto& s = *g_signal;
 Check(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s.dispatch_thread,
                       SYNCHRONIZE, FALSE, 0) != 0, "duplicate signal dispatch thread handle");
 s.host_tid.store(GetCurrentThreadId()); s.self.store(K::PthreadSelfOrNull());
 s.critical.store(Common::GetHostLockCriticalCounter()->load());
 s.result.store(s.api->remove(s.id)); s.finished.Set();
}
void SignalDelete(const Api& api) {
 constexpr int signo = 30;
 SignalState state {&api, api.Make("guest-signal")}; g_signal = &state;
 Sigaction action {}, old {}; action.handler = reinterpret_cast<uint64_t>(&SignalHandler);
 Check(api.action(signo, &action, &old) == 0, "install sigaction");
 Check(old.handler == 0, "signal slot must be unused in fresh fixture");
 Gate park; Worker w {&api, state.id, &park, S::SemaHookPoint::BeforePark};
 w.Start(); park.entered.Wait();
 const auto target_tid = K::PthreadGetHostThreadId(w.thread);
 park.release.Set();
 Check(K::PthreadKill(w.thread, signo) == 0, "real pthread signal");
 state.finished.Wait();
 Check(state.self.load() == w.thread, "signal logical pthread identity");
 const bool on_target = state.host_tid.load() == target_tid;
 Check(WaitForSingleObject(state.dispatch_thread, 10000) == WAIT_OBJECT_0, "signal dispatch host thread retirement");
 w.Join();
 Check(state.result.load() == 0 && state.critical.load() == 0, "handler delete and critical counter");
 Check(w.result == K::KERNEL_ERROR_ESRCH || w.result == K::KERNEL_ERROR_EACCES, "signal/delete wait race result");
 std::fprintf(stderr, "KernelSemaphoreGuestTests: signal host=%lu target=%llu on_target=%d wait=%d\n",
              static_cast<unsigned long>(state.host_tid.load()), static_cast<unsigned long long>(target_tid),
              on_target, w.result);
 Check(api.action(signo, &old, nullptr) == 0, "restore sigaction after join");
 g_signal = nullptr;
 Check(CloseHandle(state.dispatch_thread) != 0, "signal dispatch handle close");
}
} // namespace

void TestKernelSemaphoreGuestThreads() {
 Deadline deadline; Api api;
 Handle probe = api.Make("guest-probe");
 ParkAndRetire(api, probe); Drain(api, probe); SignalDelete(api);
 Check(api.remove(probe) == 0, "probe cleanup");
 Check(Common::GetHostLockCriticalCounter()->load() == 0, "main critical counter balanced");
 std::fputs("KernelSemaphoreGuestTests: passed\n", stderr);
}
