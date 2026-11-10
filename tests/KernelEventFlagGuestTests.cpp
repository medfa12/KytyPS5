// Separate Windows fixture: run only after Memory and main-pthread/linker init.
#include "kernel/eventFlag.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if !defined(_WIN32) || !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#error This fixture requires Windows emulator sources with test hooks
#endif
namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
namespace {
namespace K = Libs::LibKernel;
namespace E = K::EventFlag;
using Handle = E::KernelEventFlag;
void Check(bool okay, const char* why) { if (!okay) { std::fprintf(stderr, "EventFlagGuest: %s\n", why); std::abort(); } }
struct Event {
	HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	Event() { Check(handle != nullptr, "host event creation"); }
	~Event() { Check(CloseHandle(handle) != 0, "host event close"); }
	void Set() { Check(SetEvent(handle) != 0, "host event set"); }
	void Wait(DWORD ms = 10000) { Check(WaitForSingleObject(handle, ms) == WAIT_OBJECT_0, "host event deadline"); }
};
struct Deadline {
	Event done; HANDLE thread;
	static DWORD WINAPI Run(void* p) { auto& self = *static_cast<Deadline*>(p); self.done.Wait(90000); return 0; }
	Deadline(): thread(CreateThread(nullptr, 0, Run, this, 0, nullptr)) { Check(thread != nullptr, "watchdog create"); }
	~Deadline() { done.Set(); Check(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0, "watchdog join"); Check(CloseHandle(thread) != 0, "watchdog close"); }
};
struct Gate {
	Event arrived, release;
	static void Hook(void* p) { auto& g = *static_cast<Gate*>(p); Check(Common::GetHostLockCriticalCounter()->load() == 0, "enrolled hook not protected"); g.arrived.Set(); g.release.Wait(); }
};
struct Sigaction { uint64_t handler = 0; int32_t flags = 0, pad = 0; uint64_t mask[2] {}; };
static_assert(sizeof(Sigaction) == 32);
struct Api {
	using Create = int (KYTY_SYSV_ABI *)(Handle*, const char*, uint32_t, uint64_t, const void*);
	using Unary = int (KYTY_SYSV_ABI *)(Handle);
	using Wait = int (KYTY_SYSV_ABI *)(Handle, uint64_t, uint32_t, uint64_t*, uint32_t*);
	using Action = int (KYTY_SYSV_ABI *)(int, const Sigaction*, Sigaction*);
	Create create; Unary remove; Wait wait; Action action;
	template<class T> static T Get(Loader::SymbolDatabase& db, const char* nid) {
		const auto* e = db.FindByNid(nid, Loader::SymbolType::Func); Check(e && e->vaddr, "registered API"); return reinterpret_cast<T>(e->vaddr);
	}
	Api() { Loader::SymbolDatabase db; Libs::InitLibKernel_1(&db); create = Get<Create>(db, "BpFoboUJoZU"); remove = Get<Unary>(db, "8mql9OcQnd4"); wait = Get<Wait>(db, "JTvBflhYazQ"); action = Get<Action>(db, "KiJEPEWRyUY"); }
};
struct State {
	const Api* api; Handle flag; Gate enrolled; Event finished;
	bool cooperative = false; std::atomic_bool dispatch_boundary {false};
	std::atomic<int> result {-1}; std::atomic<DWORD> tid {0};
	std::atomic<K::Pthread> self {nullptr}; std::atomic<unsigned> critical {UINT32_MAX};
	HANDLE dispatch_thread = nullptr; K::Pthread worker = nullptr; int wait_result = -1;
};
State* g_state = nullptr;
void DispatchHook(void* p) {
	auto& s = *static_cast<State*>(p);
	Check(Common::GetHostLockCriticalCounter()->load() == 0, "cooperative dispatch boundary is unguarded");
	s.dispatch_boundary.store(true, std::memory_order_release);
	s.enrolled.arrived.Set(); s.enrolled.release.Wait();
}
void KYTY_SYSV_ABI Handler(int, void*) {
	auto& s = *g_state;
	if (s.cooperative) { Check(s.dispatch_boundary.load(std::memory_order_acquire), "handler follows cooperative boundary"); }
	Check(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s.dispatch_thread,
	                      SYNCHRONIZE, FALSE, 0) != 0, "duplicate dispatch thread");
	s.tid = GetCurrentThreadId(); s.self = K::PthreadSelfOrNull();
	s.critical = Common::GetHostLockCriticalCounter()->load();
	s.result = s.api->remove(s.flag); s.finished.Set();
}
void* KYTY_SYSV_ABI Worker(void* p) {
	auto& s = *static_cast<State*>(p);
	if (s.cooperative) { E::EventFlagHookForTest(E::EventFlagHookPoint::Dispatch, &DispatchHook, &s); }
	else { E::EventFlagHookForTest(E::EventFlagHookPoint::Enrolled, &Gate::Hook, &s.enrolled); }
	s.wait_result = s.api->wait(s.flag, 1, 1, nullptr, nullptr);
	return nullptr;
}
void SignalCase(const Api& api, bool cooperative) {
	Handle h = nullptr;
	Check(api.create(&h, cooperative ? "ef-guest-cooperative" : "ef-guest-apc", 0x20, 0, nullptr) == 0, "real event create");
	State s {&api, h}; s.cooperative = cooperative; g_state = &s;
	Sigaction action {}, old {}; action.handler = reinterpret_cast<uint64_t>(&Handler);
	constexpr int signo = 30;
	Check(api.action(signo, &action, &old) == 0 && old.handler == 0, "install unused signal handler");
	Check(K::PthreadCreate(&s.worker, nullptr, &Worker, &s, "eventflag-guest-test") == 0, "create guest pthread");
	s.enrolled.arrived.Wait();
	const auto target = K::PthreadGetHostThreadId(s.worker);
	if (cooperative) {
		// Queue directly, with no PthreadKill and therefore no host APC/helper.
		// Worker is paused immediately before the event wait's dispatcher call.
		K::PthreadQueuePendingSignal(s.worker, signo);
		Check(K::PthreadHasPendingSignal(s.worker, signo), "pending signal queued before cooperative dispatch");
		s.enrolled.release.Set();
	} else {
		s.enrolled.release.Set();
		Check(K::PthreadKill(s.worker, signo) == 0, "send actual pthread signal");
	}
	s.finished.Wait();
	Check(s.self.load() == s.worker && s.critical.load() == 0 && s.result.load() == 0, "handler logical identity, unguarded delete");
	const bool on_target = s.tid.load() == target;
	if (cooperative) { Check(on_target, "direct queued pending signal ran on target cooperative thread"); }
	Check(WaitForSingleObject(s.dispatch_thread, 10000) == WAIT_OBJECT_0, "dispatch host thread retires before teardown");
	Check(K::PthreadJoin(s.worker, nullptr) == 0, "guest waiter joins");
	Check(s.wait_result == K::KERNEL_ERROR_EACCES || s.wait_result == K::KERNEL_ERROR_ESRCH, "wait follows real deletion");
	Check(api.action(signo, &old, nullptr) == 0, "restore old signal handler");
	g_state = nullptr; Check(CloseHandle(s.dispatch_thread) != 0, "close dispatch thread handle");
	std::fprintf(stderr, "EventFlagGuest: cooperative=%d handler host=%lu target=%llu on_target=%d\n", cooperative, static_cast<unsigned long>(s.tid.load()), static_cast<unsigned long long>(target), on_target);
}
} // namespace
void TestKernelEventFlagGuestThreads() {
	Deadline deadline; Api api;
	SignalCase(api, true);  // deterministic cooperative queue, no APC
	SignalCase(api, false); // actual PthreadKill/APC-compatible integration
}
