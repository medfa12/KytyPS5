// Integrator supplies a full emulator fixture and enables the test hooks on all
// emulator source compilations. No separate main or CMake changes here.
#include "common/abi.h"
#include "common/threads.h"
#include "kernel/eventFlag.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "EventFlagParkWitness.h"
#endif
#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#error EventFlag lifetime fixture requires emulator source test hooks
#endif
namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); namespace ShellCoreFloor { void InitShellCoreFloor_1(Loader::SymbolDatabase*); } }
namespace {
namespace E = Libs::LibKernel::EventFlag;
using namespace Libs::LibKernel;
using Handle = E::KernelEventFlag;
void Check(bool okay, const char* what) { if (!okay) { std::fprintf(stderr, "EventFlagLifetime: %s\n", what); std::abort(); } }
struct Deadline {
	std::mutex mutex; std::condition_variable cv; bool done = false; std::thread thread;
	Deadline(): thread([this] { std::unique_lock lock(mutex); Check(cv.wait_for(lock, std::chrono::seconds(90), [&] { return done; }), "90 second fixture deadline"); }) {}
	~Deadline() { { std::lock_guard lock(mutex); done = true; } cv.notify_one(); thread.join(); }
};
struct Gate {
	std::mutex mutex; std::condition_variable cv; bool arrived = false, released = false;
	static void Hook(void* context) {
		Check(Common::GetHostLockCriticalCounter()->load() == 0, "callback holds no HLE critical guard");
		auto& g = *static_cast<Gate*>(context);
		std::unique_lock lock(g.mutex); g.arrived = true; g.cv.notify_all();
		Check(g.cv.wait_for(lock, std::chrono::seconds(5), [&] { return g.released; }), "gate release");
	}
	void Await() { std::unique_lock lock(mutex); Check(cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived; }), "gate arrival"); }
	void Release() { { std::lock_guard lock(mutex); released = true; } cv.notify_all(); }
};
struct Api {
	using Create = int (KYTY_SYSV_ABI *)(Handle*, const char*, uint32_t, uint64_t, const void*);
	using Open = int (KYTY_SYSV_ABI *)(Handle*, const char*);
	using Unary = int (KYTY_SYSV_ABI *)(Handle);
	using Wait = int (KYTY_SYSV_ABI *)(Handle, uint64_t, uint32_t, uint64_t*, uint32_t*);
	using Poll = int (KYTY_SYSV_ABI *)(Handle, uint64_t, uint32_t, uint64_t*);
	using Mutate = int (KYTY_SYSV_ABI *)(Handle, uint64_t);
	using Cancel = int (KYTY_SYSV_ABI *)(Handle, uint64_t, int*);
	Create create; Open open; Unary remove, close; Wait wait; Poll poll; Mutate set, clear; Cancel cancel;
	template<class T> static T Get(Loader::SymbolDatabase& s, const char* nid) {
		const auto* e = s.FindByNid(nid, Loader::SymbolType::Func);
		Check(e && e->vaddr, "registered NID exists"); return reinterpret_cast<T>(e->vaddr);
	}
	Api() {
		Loader::SymbolDatabase s; Libs::InitLibKernel_1(&s);
		create = Get<Create>(s, "BpFoboUJoZU"); open = Get<Open>(s, "1vDaenmJtyA");
		remove = Get<Unary>(s, "8mql9OcQnd4"); close = Get<Unary>(s, "s9-RaxukuzQ");
		wait = Get<Wait>(s, "JTvBflhYazQ"); poll = Get<Poll>(s, "9lvj5DjHZiA");
		set = Get<Mutate>(s, "IOnSvHzqu6A"); clear = Get<Mutate>(s, "7uhBFWRAS60");
		cancel = Get<Cancel>(s, "PZku4ZrXJqg");
		Loader::SymbolDatabase floor; Libs::ShellCoreFloor::InitShellCoreFloor_1(&floor);
		Check(Get<Unary>(floor, "s9-RaxukuzQ") == close, "floor closes same real event aliases");
	}
	// Public, name-openable fixtures opt in; generic lifecycle coverage stays private.
	Handle Make(const char* name, uint32_t attr = 0x20) const {
		Handle h = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
		Check(create(&h, name, attr, 0, nullptr) == 0, "real registered create");
		const auto n = reinterpret_cast<uintptr_t>(h);
		Check(n >= 0x10000 && n <= 0x7fffffff, "monotonic low-32 token"); return h;
	}
};
void Basic(const Api& a, int baseline) {
	auto h = a.Make("ef-lifetime-basic");
	Check(E::EventFlagObjectsForTest() == baseline + 1, "object constructed");
	Check(a.remove(h) == 0 && a.remove(h) == KERNEL_ERROR_ESRCH, "delete claims once");
	Check(E::EventFlagObjectsForTest() == baseline, "actual destructor after delete");
	for (int i = 0; i < 128; ++i) {
		Check(a.poll(h, 1, 1, nullptr) == KERNEL_ERROR_ESRCH, "stale poll safe on diagnostic cadence");
		Check(a.poll(reinterpret_cast<Handle>(static_cast<uintptr_t>(-1)), 1, 1, nullptr) == KERNEL_ERROR_ESRCH, "invalid poll safe");
	}
	auto contested = a.Make("ef-lifetime-two-deletes");
	std::atomic_bool go {false}; int r1 = 0, r2 = 0;
	auto run = [&](int& result) { while (!go.load()) { std::this_thread::yield(); } result = a.remove(contested); };
	std::thread one(run, std::ref(r1)), two(run, std::ref(r2)); go = true; one.join(); two.join();
	Check((r1 == 0 && r2 == KERNEL_ERROR_ESRCH) || (r2 == 0 && r1 == KERNEL_ERROR_ESRCH), "concurrent delete single winner");
	Check(E::EventFlagObjectsForTest() == baseline, "contested delete destructor");
}
void CopyoutFailure(const Api& a, int baseline) {
#if defined(_WIN32)
	constexpr size_t page_size = 4096;
	auto* page = static_cast<Handle*>(VirtualAlloc(nullptr, page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	Check(page != nullptr, "allocate copyout test page");
	*page = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	DWORD old = 0;
	Check(VirtualProtect(page, page_size, PAGE_READONLY, &old) != 0, "protect event output");
	Check(a.create(page, "ef-fault-copy", 0x20, 0, nullptr) == KERNEL_ERROR_EFAULT &&
	      reinterpret_cast<uintptr_t>(*page) == UINTPTR_MAX, "failed Create copyout preserves read-only output");
	Check(E::EventFlagObjectsForTest() == baseline, "failed copyout destroyed unpublished object");
	Handle out = nullptr;
	Check(a.open(&out, "ef-fault-copy") == KERNEL_ERROR_ESRCH, "failed copyout removed name");
	Check(a.create(&out, reinterpret_cast<const char*>(static_cast<uintptr_t>(1)), 0x20, 0, nullptr) == KERNEL_ERROR_EFAULT,
	      "invalid guest name is safely rejected");
	auto valid = a.Make("ef-fault-inputs", 0x120);
	Check(a.wait(valid, 1, 1, nullptr, reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(1))) == KERNEL_ERROR_EFAULT,
	      "invalid timeout is safely rejected");
	Check(a.poll(valid, 1, 1, reinterpret_cast<uint64_t*>(page)) == KERNEL_ERROR_EFAULT,
	      "read-only result is safely rejected");
	Check(a.cancel(valid, 1, reinterpret_cast<int*>(page)) == KERNEL_ERROR_EFAULT &&
	      a.poll(valid, 1, 1, nullptr) == KERNEL_ERROR_EBUSY, "bad count does not apply cancellation bits");
	Check(a.open(page, "ef-no-such-object") == KERNEL_ERROR_ESRCH, "missing Open precedes bad output validation");
	Check(a.create(page, "ef-fault-inputs", 0x120, 0, nullptr) == KERNEL_ERROR_EPERM, "duplicate openable Create precedes bad output validation");
	Check(a.remove(valid) == 0 && E::EventFlagObjectsForTest() == baseline, "fault inputs leave resources balanced");
	Check(VirtualFree(page, 0, MEM_RELEASE) != 0, "release copyout test page");
#endif
}
void RetainedOps(const Api& a, int baseline) {
	for (int op = 0; op < 5; ++op) {
		const std::string name = "ef-retained-" + std::to_string(op);
		auto h = a.Make(name.c_str()); Gate gate; int result = 0;
		std::thread thread([&] {
			E::EventFlagHookForTest(E::EventFlagHookPoint::Retained, &Gate::Hook, &gate);
			switch (op) {
				case 0: result = a.wait(h, 1, 1, nullptr, nullptr); break;
				case 1: result = a.poll(h, 1, 1, nullptr); break;
				case 2: result = a.set(h, 1); break;
				case 3: result = a.clear(h, 0); break;
				default: result = a.cancel(h, 0, nullptr); break;
			}
		});
		gate.Await(); Check(a.remove(h) == 0, "delete retained operation");
		Check(E::EventFlagObjectsForTest() == baseline + 1, "in-flight owner keeps actual object alive");
		gate.Release(); thread.join();
		Check(result == (op == 1 ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EACCES), "retained operation sees retirement");
		Check(E::EventFlagObjectsForTest() == baseline, "last retained operation destroys object");
	}
}
void EnrolledWait(const Api& a, int baseline) {
	auto h = a.Make("ef-enrolled-delete"); Gate gate; int result = 0;
	std::thread thread([&] { E::EventFlagHookForTest(E::EventFlagHookPoint::Enrolled, &Gate::Hook, &gate); result = a.wait(h, 1, 1, nullptr, nullptr); });
	gate.Await(); Check(a.remove(h) == 0, "delete after enrollment before park");
	Check(E::EventFlagObjectsForTest() == baseline + 1, "enrolled waiter retains object");
	gate.Release(); thread.join(); Check(result == KERNEL_ERROR_EACCES, "enrolled waiter observes deletion");
	Check(E::EventFlagObjectsForTest() == baseline, "waiter release destroys object");
}
void CancelDelete(const Api& a, int baseline) {
	auto h = a.Make("ef-cancel-delete"); Gate enrolled, canceled; int wait_result = 0, cancel_result = 0, waiting = -1;
	std::thread waiter([&] { E::EventFlagHookForTest(E::EventFlagHookPoint::Enrolled, &Gate::Hook, &enrolled); wait_result = a.wait(h, 1, 1, nullptr, nullptr); });
	enrolled.Await();
	std::thread canceler([&] { E::EventFlagHookForTest(E::EventFlagHookPoint::Cancelled, &Gate::Hook, &canceled); cancel_result = a.cancel(h, 0, &waiting); });
	canceled.Await(); Check(waiting == 1, "cancel counted the enrolled waiter");
	Check(a.remove(h) == 0, "delete while retained cancel paused");
	enrolled.Release(); canceled.Release(); waiter.join(); canceler.join();
	Check(cancel_result == 0 && wait_result == KERNEL_ERROR_EACCES, "delete wins over cancel without resurrection");
	Check(E::EventFlagObjectsForTest() == baseline, "cancel/delete ownership balanced");
}
void Names(const Api& a, int baseline) {
	auto old = a.Make("ef-name-reuse", 0x120); Handle alias = nullptr;
	Check(a.open(&alias, "ef-name-reuse") == 0 && alias != old, "open returns distinct alias ID");
	Handle sentinel = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	Check(a.create(&sentinel, "ef-name-reuse", 0x120, 0, nullptr) == KERNEL_ERROR_EPERM && reinterpret_cast<uintptr_t>(sentinel) == UINTPTR_MAX, "duplicate does not replace live owner");
	Check(a.remove(alias) == KERNEL_ERROR_EPERM && a.close(old) == KERNEL_ERROR_EPERM, "creator and alias roles are enforced");
	Check(a.remove(old) == 0, "unlink old name");
	auto replacement = a.Make("ef-name-reuse", 0x120); Check(replacement != old, "replacement token never reuses stale identity");
	Check(a.set(alias, 1) == KERNEL_ERROR_EACCES && a.poll(replacement, 1, 1, nullptr) == KERNEL_ERROR_EBUSY, "old alias cannot mutate replacement");
	Check(a.remove(alias) == KERNEL_ERROR_EPERM && a.close(alias) == 0 && a.close(alias) == KERNEL_ERROR_ESRCH, "old alias close cannot delete replacement");
	Check(a.set(replacement, 1) == 0 && a.poll(replacement, 1, 1, nullptr) == 0, "replacement remains usable");
	Check(a.remove(replacement) == 0 && E::EventFlagObjectsForTest() == baseline, "name reuse cleanup");
}
void ConcurrentName(const Api& a, int baseline) {
	auto old = a.Make("ef-name-race", 0x120); Gate gate; int set_result = 0;
	std::thread setter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Retained, &Gate::Hook, &gate);
		set_result = E::SetEventFlagByName("ef-name-race", 1);
	});
	gate.Await();
	Handle alias = nullptr;
	Check(a.open(&alias, "ef-name-race") == 0 && alias != old, "open distinct old alias while named operation retained");
	Check(a.remove(old) == 0, "unlink old identity during named operation");
	auto replacement = a.Make("ef-name-race", 0x120);
	Handle fresh = nullptr;
	Check(a.open(&fresh, "ef-name-race") == 0 && fresh != replacement && fresh != alias && fresh != old, "open returns distinct replacement alias");
	gate.Release(); setter.join();
	Check(set_result == KERNEL_ERROR_EACCES && a.poll(replacement, 1, 1, nullptr) == KERNEL_ERROR_EBUSY,
	      "retained old named operation cannot mutate replacement");
	Check(a.close(alias) == 0 && a.close(fresh) == 0 && a.remove(replacement) == 0, "close both generations");
	Check(E::EventFlagObjectsForTest() == baseline, "concurrent name generation destructors");
}
void Transition(const Api& a, int baseline) {
	auto old = a.Make("ef-atomic-retire", 0x120); Handle alias = nullptr;
	Check(a.open(&alias, "ef-atomic-retire") == 0 && alias != old, "hold distinct old alias");
	Gate enrolled, transition; int wait_result = 99, delete_result = 99;
	std::thread waiter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Enrolled, &Gate::Hook, &enrolled);
		wait_result = a.wait(old, 1, 1, nullptr, nullptr);
	});
	enrolled.Await(); enrolled.Release();
	std::thread deleter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Retired, &Gate::Hook, &transition);
		delete_result = a.remove(old);
	});
	transition.Await();
	// The callback is after terminal mark, unlink and wake, with no HLE lock held.
	auto replacement = a.Make("ef-atomic-retire", 0x120);
	Check(a.set(alias, 1) == KERNEL_ERROR_EACCES && a.cancel(alias, 1, nullptr) == KERNEL_ERROR_EACCES,
	      "old alias terminal before replacement publication");
	uint32_t zero = 0;
	Check(a.wait(alias, 1, 1, nullptr, &zero) == KERNEL_ERROR_EACCES && a.poll(replacement, 1, 1, nullptr) == KERNEL_ERROR_EBUSY,
	      "old wait terminal and new object independent");
	waiter.join(); Check(wait_result == KERNEL_ERROR_EACCES, "transition wakes enrolled waiter");
	transition.Release(); deleter.join(); Check(delete_result == 0, "committed delete returns success");
	Check(a.close(alias) == 0 && a.remove(replacement) == 0, "retire aliases and replacement");
	Check(E::EventFlagObjectsForTest() == baseline, "transition destroys actual objects");
}
void ThrowBadAlloc(void* context) { ++*static_cast<std::atomic<int>*>(context); throw std::bad_alloc(); }
void InjectedFailures(const Api& a, int baseline) {
	Handle output = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	E::EventFlagFailForTest(E::EventFlagFailure::Prepare);
	Check(a.create(&output, "ef-prepare-failure", 0x20, 0, nullptr) == KERNEL_ERROR_ENOMEM && reinterpret_cast<uintptr_t>(output) == UINTPTR_MAX,
	      "prepare failure leaves output untouched");
	Check(E::EventFlagObjectsForTest() == baseline, "prepare failure creates no object");
	E::EventFlagFailForTest(E::EventFlagFailure::Diagnostic);
	auto h = a.Make("ef-diagnostic-failure");
	Check(E::EventFlagFailurePendingForTest() == E::EventFlagFailure::None, "diagnostic failure was actually injected");
	Check(E::EventFlagObjectsForTest() == baseline + 1, "diagnostic failure did not roll back success");
	int wait_result = 99; std::atomic<int> hook_calls {0};
	std::thread waiter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Enrolled, &ThrowBadAlloc, &hook_calls);
		wait_result = a.wait(h, 1, 1, nullptr, nullptr);
	});
	waiter.join();
	Check(hook_calls == 1 && wait_result == KERNEL_ERROR_ENOMEM, "throwing hook is contained and returns ENOMEM");
	Check(E::EventFlagWaitersForTest(h) == 0, "RAII retires enrollment after contained throwing hook");
	std::thread park_error([&] { E::EventFlagFailForTest(E::EventFlagFailure::Park); wait_result = a.wait(h, 1, 1, nullptr, nullptr); });
	park_error.join();
	Check(wait_result == KERNEL_ERROR_EIO && E::EventFlagWaitersForTest(h) == 0, "hard park failure retires enrollment and does not spin");
	Check(a.remove(h) == 0 && E::EventFlagObjectsForTest() == baseline, "failed wait resources actually destroyed");
}
void FinalStoreRollback(const Api& a, int baseline) {
	Handle out = nullptr;
	for (uint32_t attr: {0x20u, 0x120u}) {
		out = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
		E::EventFlagFailForTest(E::EventFlagFailure::Store);
		Check(a.create(&out, "ef-final-store-create", attr, 0, nullptr) == KERNEL_ERROR_EFAULT &&
		      E::EventFlagFailurePendingForTest() == E::EventFlagFailure::None && reinterpret_cast<uintptr_t>(out) == UINTPTR_MAX,
		      "injected final private/named Create store fails after insertion without changing output");
		Check(E::EventFlagObjectsForTest() == baseline, "final Create store rollback destroys object");
		Handle missing = nullptr;
		Check(a.open(&missing, "ef-final-store-create") == KERNEL_ERROR_ESRCH, "failed final Create store leaves no published name");
	}
	auto h = a.Make("ef-final-store-open", 0x120);
	out = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	E::EventFlagFailForTest(E::EventFlagFailure::Store);
	Check(a.open(&out, "ef-final-store-open") == KERNEL_ERROR_EFAULT &&
	      E::EventFlagFailurePendingForTest() == E::EventFlagFailure::None && reinterpret_cast<uintptr_t>(out) == UINTPTR_MAX,
	      "injected final Open store rolls back inserted alias");
	Check(a.remove(h) == 0 && E::EventFlagObjectsForTest() == baseline,
	      "failed final Open leaves no retained alias");
	// A private rollback must not erase the other event's public same-name entry.
	auto published = a.Make("ef-rollback-same-name", 0x120);
	out = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	E::EventFlagFailForTest(E::EventFlagFailure::Store);
	Check(a.create(&out, "ef-rollback-same-name", 0x20, 0, nullptr) == KERNEL_ERROR_EFAULT &&
	      E::EventFlagFailurePendingForTest() == E::EventFlagFailure::None && reinterpret_cast<uintptr_t>(out) == UINTPTR_MAX,
	      "private final-store failure after real public publication");
	Handle alias = nullptr;
	Check(a.open(&alias, "ef-rollback-same-name") == 0 && alias != published, "private rollback preserved public name and creator");
	Check(a.close(alias) == 0 && a.remove(published) == 0, "clean public owner after private rollback");
	Handle private_event = nullptr;
	Check(a.create(&private_event, "ef-rollback-private", 0x20, 0, nullptr) == 0, "hold private event across named rollback");
	out = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	E::EventFlagFailForTest(E::EventFlagFailure::Store);
	Check(a.create(&out, "ef-rollback-private", 0x120, 0, nullptr) == KERNEL_ERROR_EFAULT &&
	      E::EventFlagFailurePendingForTest() == E::EventFlagFailure::None && reinterpret_cast<uintptr_t>(out) == UINTPTR_MAX,
	      "named final-store failure after private creation");
	Check(a.open(&out, "ef-rollback-private") == KERNEL_ERROR_ESRCH &&
	      a.set(private_event, 1) == 0 && a.poll(private_event, 1, 1, nullptr) == 0,
	      "named rollback removed publication and preserved private owner");
	Check(a.remove(private_event) == 0 && E::EventFlagObjectsForTest() == baseline, "cross-kind rollback resources destroyed");
}
void NativeInitialPatterns(const Api& a, int baseline) {
	for (const char* name: {"SceBootStatusFlags", "SceShellCoreUtilPowerControl"}) {
		for (uint32_t attr: {0x20u, 0x120u}) {
			for (uint64_t initial: {uint64_t{0}, uint64_t{0x8000000000000001ull}}) {
				Handle creator = nullptr;
				Check(a.create(&creator, name, attr, initial, nullptr) == 0, "create exact-name event with caller pattern");
				uint64_t observed = UINT64_MAX;
				Check(a.poll(creator, UINT64_MAX, 1, &observed) == KERNEL_ERROR_EBUSY && observed == initial,
				      "exact-name creator preserves all caller pattern bits");
				const auto sentinel = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
				Handle alias = sentinel;
				if (attr == 0x20) {
					Check(a.open(&alias, name) == KERNEL_ERROR_ESRCH && alias == sentinel, "exact-name private event is not openable");
				} else {
					Check(a.open(&alias, name) == 0 && alias != creator, "open exact-name public alias");
					observed = UINT64_MAX;
					Check(a.poll(alias, UINT64_MAX, 1, &observed) == KERNEL_ERROR_EBUSY && observed == initial,
					      "public alias observes exact caller pattern");
				}
				Check(a.set(creator, 2) == 0, "set legitimate event bit");
				observed = UINT64_MAX;
				Check(a.poll(attr == 0x120 ? alias : creator, UINT64_MAX, 1, &observed) == KERNEL_ERROR_EBUSY && observed == (initial | 2),
				      "set updates shared event pattern");
				Check(a.clear(creator, ~uint64_t{2}) == 0, "clear applies AND mask");
				observed = UINT64_MAX;
				Check(a.poll(attr == 0x120 ? alias : creator, UINT64_MAX, 1, &observed) == KERNEL_ERROR_EBUSY && observed == initial,
				      "clear retains only bits selected by mask");
				if (attr == 0x120) { Check(a.close(alias) == 0, "close exact-name alias"); }
				Check(a.remove(creator) == 0 && E::EventFlagObjectsForTest() == baseline, "exact-name event resources balanced");
			}
		}
	}
}
void NativeNamesAndRoles(const Api& a, int baseline) {
	const auto sentinel = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	Handle first = nullptr, second = nullptr, alias = sentinel;
	// Exact observed SysCore pattern: same private name and attr=0x11.
	Check(a.create(&first, "ExitSpawnEvf", 0x11, 0, nullptr) == 0 &&
	      a.create(&second, "ExitSpawnEvf", 0x11, 0, nullptr) == 0 && first != second,
	      "same-name private SysCore events are independent");
	Check(a.open(&alias, "ExitSpawnEvf") == KERNEL_ERROR_ESRCH && alias == sentinel,
	      "private name is not openable and failure leaves output untouched");
	Check(E::SetEventFlagByName("ExitSpawnEvf", 1) == KERNEL_ERROR_ESRCH, "internal lookup cannot manufacture private publication");
	Check(a.set(first, 1) == 0 && a.poll(first, 1, 1, nullptr) == 0 &&
	      a.poll(second, 1, 1, nullptr) == KERNEL_ERROR_EBUSY, "private bit patterns are independent");
	Check(a.remove(first) == 0 && a.set(second, 2) == 0 && a.poll(second, 2, 1, nullptr) == 0,
	      "private deletion does not affect private peer");
	Check(a.remove(second) == 0, "delete second private event");
	for (int named_first = 0; named_first != 2; ++named_first) {
		const char* name = named_first ? "ef-mixed-public-first" : "ef-mixed-private-first";
		Handle priv = nullptr, pub = nullptr;
		if (named_first) {
			Check(a.create(&pub, name, 0x120, 0, nullptr) == 0 && a.create(&priv, name, 0x20, 0, nullptr) == 0, "openable/private coexist in public-first order");
		} else {
			Check(a.create(&priv, name, 0x20, 0, nullptr) == 0 && a.create(&pub, name, 0x120, 0, nullptr) == 0, "private/openable coexist in private-first order");
		}
		Handle one = nullptr, two = nullptr;
		Check(a.open(&one, name) == 0 && a.open(&two, name) == 0 && one != two && one != pub && two != pub && one != priv && two != priv,
		      "each Open creates a distinct alias to public object");
		Check(a.set(one, 1) == 0 && a.poll(pub, 1, 1, nullptr) == 0 && a.poll(priv, 1, 1, nullptr) == KERNEL_ERROR_EBUSY,
		      "alias resolves public, never private");
		Check(a.remove(one) == KERNEL_ERROR_EPERM && a.close(pub) == KERNEL_ERROR_EPERM, "alias delete and creator close rejected");
		Check(a.close(one) == 0 && a.close(one) == KERNEL_ERROR_ESRCH && a.poll(pub, 1, 1, nullptr) == 0,
		      "stale alias cannot remove creator");
		Check(a.remove(priv) == 0, "private same-name delete");
		Handle another = nullptr;
		Check(a.open(&another, name) == 0 && another != two && another != pub, "private delete preserves public name");
		Check(a.close(two) == 0 && a.close(another) == 0 && a.remove(pub) == 0, "close public aliases and delete creator");
	}
	Check(E::EventFlagObjectsForTest() == baseline, "private/public lifetime balanced");
}
void NativeValidationAndConcurrentPublication(const Api& a, int baseline) {
	const auto sentinel = reinterpret_cast<Handle>(static_cast<uintptr_t>(-1));
	Handle out = sentinel;
	for (uint32_t attr: {0x400u, 0x3u, 0x30u}) {
		Check(a.create(&out, "ef-invalid-attr", attr, 0, nullptr) == KERNEL_ERROR_EINVAL && out == sentinel,
		      "invalid attr mask/order/mode preserves output");
	}
	const std::string n31(31, 'n'), n32(32, 'n');
	Check(a.create(&out, n31.c_str(), 0x100, 0, nullptr) == 0, "31 bytes plus NUL fits bounded name; zero bits normalize");
	Check(a.remove(out) == 0, "delete 31-byte name event");
	out = sentinel;
	Check(a.create(&out, n32.c_str(), 0x100, 0, nullptr) == KERNEL_ERROR_ENAMETOOLONG && out == sentinel,
	      "32 bytes without NUL exceed bounded HLE copy");
	Check(a.open(&out, n32.c_str()) == KERNEL_ERROR_ENAMETOOLONG && out == sentinel, "Open uses same bounded name copy");
	Handle h1 = sentinel, h2 = sentinel; int r1 = 99, r2 = 99; std::atomic_bool go {false};
	auto create = [&](Handle& h, int& r) { while (!go.load()) { std::this_thread::yield(); } r = a.create(&h, "ef-public-concurrent", 0x120, 0, nullptr); };
	std::thread one(create, std::ref(h1), std::ref(r1)), two(create, std::ref(h2), std::ref(r2));
	go = true; one.join(); two.join();
	Check((r1 == 0 && r2 == KERNEL_ERROR_EPERM && h2 == sentinel) || (r2 == 0 && r1 == KERNEL_ERROR_EPERM && h1 == sentinel),
	      "concurrent duplicate publication selects one real owner");
	Check(a.remove(r1 == 0 ? h1 : h2) == 0 && E::EventFlagObjectsForTest() == baseline, "concurrent publication cleanup");
}
void RetainedAlias(const Api& a, int baseline) {
	auto creator = a.Make("ef-retained-alias", 0x120);
	Handle old = nullptr;
	Check(a.open(&old, "ef-retained-alias") == 0 && old != creator, "allocate distinct retained alias");
	Gate gate; int result = 99;
	std::thread setter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Retained, &Gate::Hook, &gate);
		result = a.set(old, 1);
	});
	gate.Await();
	Check(a.close(old) == 0, "close alias while operation holds owner");
	Handle replacement_alias = nullptr;
	Check(a.open(&replacement_alias, "ef-retained-alias") == 0 && replacement_alias != old && replacement_alias != creator,
	      "new Open receives a fresh token, not closed alias identity");
	Check(a.close(old) == KERNEL_ERROR_ESRCH, "stale alias cannot close replacement");
	gate.Release(); setter.join();
	Check(result == 0 && a.poll(creator, 1, 1, nullptr) == 0 && a.poll(replacement_alias, 1, 1, nullptr) == 0,
	      "retained operation finishes on original shared object");
	Check(a.remove(creator) == 0 && a.set(replacement_alias, 2) == KERNEL_ERROR_EACCES,
	      "creator deletion retires existing alias object");
	Check(a.close(replacement_alias) == 0 && E::EventFlagObjectsForTest() == baseline, "retained alias resources actually destroyed");
}
void DispatchAdmission(const Api& a, int baseline) {
	Handle h = nullptr;
	Check(a.create(&h, "ef-single-dispatch-cancel", 0x10, 0, nullptr) == 0, "create single-waiter flag");
	Gate dispatch; int result = 99;
	std::thread waiter([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Dispatch, &Gate::Hook, &dispatch);
		result = a.wait(h, 1, 1, nullptr, nullptr);
	});
	dispatch.Await(); // real timed host wait returned; before cooperative dispatcher
	Check(E::EventFlagWaitersForTest(h) == 1, "logical admission remains counted during dispatcher window");
	uint32_t zero = 0;
	Check(a.wait(h, 1, 1, nullptr, &zero) == KERNEL_ERROR_EPERM, "single-waiter exclusion holds during dispatch");
	int count = -1;
	Check(a.cancel(h, 0, &count) == 0 && count == 1, "Cancel sees admitted waiter during dispatch");
	dispatch.Release(); waiter.join();
	Check(result == KERNEL_ERROR_ECANCELED && E::EventFlagWaitersForTest(h) == 0, "admitted waiter receives cancellation and retires");
	Check(a.remove(h) == 0 && E::EventFlagObjectsForTest() == baseline, "cancel dispatch case cleanup");
	Check(a.create(&h, "ef-single-dispatch-delete", 0x10, 0, nullptr) == 0, "create delete dispatch case");
	Gate deleting; result = 99;
	std::thread second([&] {
		E::EventFlagHookForTest(E::EventFlagHookPoint::Dispatch, &Gate::Hook, &deleting);
		result = a.wait(h, 1, 1, nullptr, nullptr);
	});
	deleting.Await();
	Check(E::EventFlagWaitersForTest(h) == 1 && a.remove(h) == 0, "non-draining Delete is reentrant while admitted");
	Check(E::EventFlagObjectsForTest() == baseline + 1, "dispatch retains deleted object until wait completes");
	deleting.Release(); second.join();
	Check(result == KERNEL_ERROR_EACCES && E::EventFlagObjectsForTest() == baseline, "delete during dispatcher window retires safely");
}
void ParkErrorOrdering(const Api& a, int baseline) {
	for (int deleting = 0; deleting != 2; ++deleting) {
		auto h = a.Make(deleting ? "ef-park-error-delete" : "ef-park-error-cancel");
		Gate after_park; int outcome = 99; uint64_t observed = UINT64_MAX;
		std::thread worker([&] {
			E::EventFlagHookForTest(E::EventFlagHookPoint::AfterPark, &Gate::Hook, &after_park);
			E::EventFlagFailForTest(E::EventFlagFailure::Park);
			outcome = a.wait(h, 1, 1, &observed, nullptr);
		});
		after_park.Await();
		Check(E::EventFlagWaitersForTest(h) == 1, "failed park remains logically admitted until resolution");
		if (deleting) { Check(a.remove(h) == 0, "delete committed while failed park is gated"); }
		else { int count = -1; Check(a.cancel(h, 0, &count) == 0 && count == 1, "cancel counted failed-park waiter"); }
		after_park.Release(); worker.join();
		Check(outcome == (deleting ? KERNEL_ERROR_EACCES : KERNEL_ERROR_ECANCELED) && observed == 0,
		      "terminal outcome and normal result copy outrank EIO");
		if (!deleting) { Check(E::EventFlagWaitersForTest(h) == 0 && a.remove(h) == 0, "cancel case retires enrollment"); }
		Check(E::EventFlagObjectsForTest() == baseline, "failed-park ordering releases actual object");
	}
}
struct DispatchFailure {
	const Api* api; Handle handle; int action; int calls = 0, status = 99, count = -1;
	static void Hook(void* context) {
		auto& s = *static_cast<DispatchFailure*>(context);
		Check(Common::GetHostLockCriticalCounter()->load() == 0, "dispatcher injection is outside critical scopes");
		++s.calls;
		if (s.action == 1) { s.status = s.api->cancel(s.handle, 0, &s.count); }
		else if (s.action == 2) { s.status = s.api->remove(s.handle); }
		throw std::bad_alloc();
	}
};
void DispatcherErrorOrdering(const Api& a, int baseline) {
	for (int action = 0; action != 3; ++action) {
		const std::string name = "ef-dispatch-error-" + std::to_string(action);
		auto h = a.Make(name.c_str()); DispatchFailure state {&a, h, action};
		int outcome = 99; uint64_t observed = UINT64_MAX;
		std::thread worker([&] {
			E::EventFlagDispatchForTest(&DispatchFailure::Hook, &state);
			outcome = a.wait(h, 1, 1, &observed, nullptr);
		});
		worker.join();
		Check(state.calls == 1, "dispatcher exception hook actually ran");
		if (action == 0) { Check(outcome == KERNEL_ERROR_ENOMEM && observed == UINT64_MAX, "uncontended dispatcher error returns ENOMEM without result fabrication"); }
		if (action == 1) { Check(state.status == 0 && state.count == 1 && outcome == KERNEL_ERROR_ECANCELED && observed == 0, "dispatcher cancel then throw preserves committed cancel and output"); }
		if (action == 2) { Check(state.status == 0 && outcome == KERNEL_ERROR_EACCES && observed == 0, "dispatcher delete then throw preserves committed delete and output"); }
		if (action != 2) { Check(E::EventFlagWaitersForTest(h) == 0 && a.remove(h) == 0, "dispatcher case retires waiter and creator"); }
		Check(E::EventFlagObjectsForTest() == baseline, "dispatcher exception case releases actual object");
	}
}
void UnhookedWait(const Api& a, int baseline) {
	auto h = a.Make("ef-unhooked-wait");
	std::mutex mutex; std::condition_variable cv; bool done = false; int result = 99;
	std::thread waiter([&] { result = a.wait(h, 1, 1, nullptr, nullptr); { std::lock_guard lock(mutex); done = true; } cv.notify_all(); });
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (E::EventFlagWaitersForTest(h) != 1 && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
	Check(E::EventFlagWaitersForTest(h) == 1, "unhooked waiter is enrolled");
	Check(a.remove(h) == 0, "unhooked waiter delete");
	{ std::unique_lock lock(mutex); Check(cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; }), "unhooked wake deadline"); }
	waiter.join(); Check(result == KERNEL_ERROR_EACCES, "unhooked waiter result");
	Check(E::EventFlagObjectsForTest() == baseline, "unhooked wake object destroyed");
}
#if defined(_WIN32)
void WindowsParkWitness(const Api& a, int baseline) {
	auto h = a.Make("ef-os-park-witness");
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	Check(ready && done, "witness events created");
	std::atomic<HANDLE> host {nullptr}; std::atomic<DWORD> tid {0}; int result = 99;
	std::thread waiter([&] {
		HANDLE own = nullptr;
		Check(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &own,
		                      THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0) != 0,
		      "duplicate real waiter thread handle");
		host.store(own); tid.store(GetCurrentThreadId());
		Check(SetEvent(ready) != 0, "publish waiter identity");
		uint32_t timeout = 30000000; // finite 30s, much longer than witness deadline
		result = a.wait(h, 1, 1, nullptr, &timeout);
		Check(SetEvent(done) != 0, "publish waiter completion");
	});
	Check(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "waiter identity deadline");
	const HANDLE target = host.load(); const DWORD target_tid = tid.load();
	Check(target && target_tid, "published actual waiter identity");
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	EventFlagParkWitness::Sample sample {}; bool witnessed = false;
	while (std::chrono::steady_clock::now() < end) {
		Check(WaitForSingleObject(done, 0) == WAIT_TIMEOUT, "waiter exited before witness");
		if (E::EventFlagWaitersForTest(h) == 1 && EventFlagParkWitness::Snapshot(target, target_tid, sample) &&
		    sample.state == 5 && sample.reason == 37) { witnessed = true; break; }
		Sleep(1); // sampling backoff only; never establishes correctness
	}
	Check(witnessed, "no Windows Waiting/WrAlertByThreadId observation within deadline");
	std::fprintf(stderr, "EventFlagLifetime: OS waiter tid=%lu state=%u reason=%u\n",
	             static_cast<unsigned long>(target_tid), sample.state, sample.reason);
	Check(a.remove(h) == 0, "delete after observed host wait");
	Check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0, "parked waiter completion deadline");
	waiter.join();
	Check(result == KERNEL_ERROR_EACCES, "observed parked waiter returns EACCES");
	Check(E::EventFlagObjectsForTest() == baseline, "observed parked waiter object destroyed");
	Check(CloseHandle(target) != 0 && CloseHandle(ready) != 0 && CloseHandle(done) != 0, "witness handles closed");
}
#endif
} // namespace
void TestKernelEventFlagLifetime() {
	Deadline deadline; const Api api; const int baseline = E::EventFlagObjectsForTest();
	Basic(api, baseline); CopyoutFailure(api, baseline); RetainedOps(api, baseline); EnrolledWait(api, baseline); CancelDelete(api, baseline); Names(api, baseline); ConcurrentName(api, baseline);
	Transition(api, baseline); InjectedFailures(api, baseline); UnhookedWait(api, baseline);
#if defined(_WIN32)
	WindowsParkWitness(api, baseline);
#endif
	FinalStoreRollback(api, baseline); DispatchAdmission(api, baseline);
	NativeInitialPatterns(api, baseline); NativeNamesAndRoles(api, baseline); NativeValidationAndConcurrentPublication(api, baseline);
	RetainedAlias(api, baseline);
	ParkErrorOrdering(api, baseline); DispatcherErrorOrdering(api, baseline);
}
