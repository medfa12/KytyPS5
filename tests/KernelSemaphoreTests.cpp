// Main-owned integration: call TestKernelSemaphores() after Common thread init,
// before Vulkan/Memory/FS. Compile emulator sources and this TU with the test macro.
#include "kernel/semaphore.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#error KernelSemaphoreTests requires KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS on emulator sources
#endif
namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
namespace {
namespace S = Libs::LibKernel::Semaphore;
using namespace Libs::LibKernel;
using Handle = uint64_t;
static_assert(std::is_same_v<S::KernelSema, Handle> && sizeof(S::KernelSema) == 8);
constexpr Handle Sentinel = UINT64_C(0xfedcba9876543210);
void Check(bool okay, const char* why) {
	if (!okay) { std::fprintf(stderr, "KernelSemaphoreTests: %s\n", why); std::abort(); }
}
struct Deadline {
	std::mutex mutex;
	std::condition_variable cv;
	bool finished = false;
	std::thread watchdog;
	Deadline(): watchdog([this] {
		std::unique_lock lock(mutex);
		Check(cv.wait_for(lock, std::chrono::seconds(90), [&] { return finished; }), "fixture deadline");
	}) {}
	~Deadline() {
		{ std::lock_guard lock(mutex); finished = true; }
		cv.notify_one(); watchdog.join();
	}
};
struct Gate {
	std::mutex mutex;
	std::condition_variable cv;
	bool arrived = false;
	bool released = false;
	Gate* next = nullptr;
	S::SemaHookPoint next_point = S::SemaHookPoint::None;
	static void Hook(void* context) {
		Check(Common::GetHostLockCriticalCounter()->load() == 0, "hook outside every critical scope");
		auto& gate = *static_cast<Gate*>(context);
		{
			std::unique_lock lock(gate.mutex);
			gate.arrived = true; gate.cv.notify_all();
			Check(gate.cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate.released; }), "bounded gate release");
		}
		if (gate.next != nullptr) { S::SemaHookForTest(gate.next_point, &Gate::Hook, gate.next); }
	}
	void Await() {
		std::unique_lock lock(mutex);
		Check(cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived; }), "bounded gate arrival");
	}
	void Release() { { std::lock_guard lock(mutex); released = true; } cv.notify_all(); }
};
struct Api {
	using Create = int (KYTY_SYSV_ABI *)(Handle*, const char*, uint32_t, int, int, void*);
	using Open = int (KYTY_SYSV_ABI *)(Handle*, const char*); // deliberately native output-first ABI
	using Unary = int (KYTY_SYSV_ABI *)(Handle);
	using Count = int (KYTY_SYSV_ABI *)(Handle, int);
	using Wait = int (KYTY_SYSV_ABI *)(Handle, int, uint32_t*);
	using Cancel = int (KYTY_SYSV_ABI *)(Handle, int, int*);
	Create create;
	Open open;
	Unary remove, close;
	Count poll, signal;
	Wait wait;
	Cancel cancel;
	template<class T> static T Get(Loader::SymbolDatabase& symbols, const char* nid) {
		const auto* entry = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(entry != nullptr && entry->vaddr != 0, "real libkernel NID resolves");
		return reinterpret_cast<T>(entry->vaddr);
	}
	Api() {
		Loader::SymbolDatabase symbols;
		Libs::InitLibKernel_1(&symbols);
		create = Get<Create>(symbols, "188x57JYp0g"); open = Get<Open>(symbols, "MwhHNKdBXq8");
		remove = Get<Unary>(symbols, "R1Jvn8bSCW8"); close = Get<Unary>(symbols, "HBkUky2PZPA");
		poll = Get<Count>(symbols, "12wOHk8ywb0"); signal = Get<Count>(symbols, "4czppHBiriw");
		wait = Get<Wait>(symbols, "Zxa0VhQVTsk"); cancel = Get<Cancel>(symbols, "4DM06U2BNEY");
		Check(open == &S::KernelOpenSema, "output-first ABI address is actual Open export");
		// Create/Poll may be registered through main-owned provenance wrappers.
		// Exercise the resolved registrations, without requiring their raw address.
	}
	Handle Make(const char* name, uint32_t attr = 0x101, int initial = 1, int max = 1) const {
		Handle result = Sentinel;
		Check(create(&result, name, attr, initial, max, nullptr) == 0, "create real semaphore");
		Check(result != 0 && result <= 0xffff, "opaque bounded ID, not pointer or sentinel");
		return result;
	}
};
void Resources(uint32_t handles, uint32_t names, uint32_t objects) {
	const auto actual = S::SemaResourcesForTest();
	Check(actual.handles == handles && actual.names == names && actual.objects == objects, "owned resource counts");
}
void Invalid(const Api& a, Handle invalid) {
	int count = 91; uint32_t timeout = 0;
	Check(a.poll(invalid, 1) == KERNEL_ERROR_ESRCH, "invalid poll ESRCH");
	Check(a.wait(invalid, 1, &timeout) == KERNEL_ERROR_ESRCH, "invalid wait ESRCH");
	Check(a.signal(invalid, 1) == KERNEL_ERROR_ESRCH, "invalid signal ESRCH");
	Check(a.cancel(invalid, -1, &count) == KERNEL_ERROR_ESRCH && count == 91, "invalid cancel preserves output");
	Check(a.remove(invalid) == KERNEL_ERROR_ESRCH && a.close(invalid) == KERNEL_ERROR_ESRCH, "invalid delete/close");
	Check(a.poll(invalid, 0) == KERNEL_ERROR_EINVAL && a.signal(invalid, -1) == KERNEL_ERROR_EINVAL &&
	      a.wait(invalid, 0, reinterpret_cast<uint32_t*>(1)) == KERNEL_ERROR_EINVAL, "positive validation before lookup/copyin");
}
void AbiAndRoles(const Api& a) {
	Invalid(a, UINT64_MAX); Invalid(a, 0); Invalid(a, 0x10000); Invalid(a, UINT64_C(0x1234567800000000));
	struct { uint64_t before; Handle id; uint64_t after; } out {Sentinel, Sentinel, Sentinel};
	Check(a.create(&out.id, "sema-test.abi", 0x101, 1, 1, nullptr) == 0, "native producer 101/1/1");
	Check(out.id <= 0xffff && out.id != 0 && out.before == Sentinel && out.after == Sentinel, "exact qword publication/canaries");
	Handle alias = Sentinel;
	Check(a.open(&alias, "sema-test.abi") == 0 && alias != out.id && alias <= 0xffff, "output-first Open and distinct alias");
	Check(a.close(out.id) == KERNEL_ERROR_EPERM && a.remove(alias) == KERNEL_ERROR_EPERM, "creator versus alias roles");
	Check(a.poll(alias | UINT64_C(0xabcdef0100000000), 1) == 0, "low-int32 carrier truncation");
	Check(a.poll(out.id, 1) == KERNEL_ERROR_EBUSY, "alias shares count");
	Check(a.poll(alias, 2) == KERNEL_ERROR_EINVAL && a.signal(out.id, 2) == KERNEL_ERROR_EINVAL, "max count validated");
	Check(a.signal(out.id, 1) == 0, "signal creator");
	Handle untouched = Sentinel;
	Check(a.create(&untouched, "sema-test.abi", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EPERM && untouched == Sentinel,
	      "duplicate named create EPERM, output untouched");
	Check(S::SemaTypeForTest(alias, 0x110), "test corrupt type in this registry only");
	Invalid(a, alias);
	Check(S::SemaTypeForTest(alias, 0x120), "restore type for cleanup");
	Check(a.close(alias) == 0, "close live alias without deleting creator");
	Invalid(a, alias);
	Check(a.poll(out.id, 1) == 0 && a.remove(out.id) == 0, "creator remains usable after close");
	Resources(0, 0, 0);
}
void CreateErrors(const Api& a) {
	Handle out = Sentinel;
	for (uint32_t attr: {3u, 0x103u, 4u, 0x201u, 0xffffffffu}) {
		Check(a.create(&out, "sema-test.bad", attr, 0, 1, nullptr) == KERNEL_ERROR_EINVAL && out == Sentinel, "attr mask and order");
	}
	Check(a.create(&out, "bad", 1, -1, 1, nullptr) == KERNEL_ERROR_EINVAL, "negative initial");
	Check(a.create(&out, "bad", 1, 0, 0, nullptr) == KERNEL_ERROR_EINVAL, "nonpositive maximum");
	Check(a.create(&out, "bad", 1, 2, 1, nullptr) == KERNEL_ERROR_EINVAL, "initial exceeds maximum");
	Check(a.create(&out, "bad", 1, 0, 1, reinterpret_cast<void*>(1)) == KERNEL_ERROR_EINVAL, "nonnull options");
	Check(a.create(&out, nullptr, 1, 0, 1, nullptr) == KERNEL_ERROR_EINVAL, "create null name");
	Check(out == Sentinel, "all scalar errors preserve qword");
	Check(a.open(&out, "sema-test.absent") == KERNEL_ERROR_ESRCH && out == Sentinel, "missing Open never creates");
	Check(a.open(nullptr, "sema-test.absent") == KERNEL_ERROR_ESRCH, "missing Open precedes invalid output");
	const Handle private1 = a.Make("sema-test.private", 1), private2 = a.Make("sema-test.private", 0);
	Check(a.open(&out, "sema-test.private") == KERNEL_ERROR_ESRCH, "nonopenable names not published");
	Check(a.remove(private1) == 0 && a.remove(private2) == 0, "independent private same-name objects");
	const auto counts = a.Make("sema-test.counts", 1, 2, INT32_MAX);
	Check(a.signal(counts, INT32_MAX) == KERNEL_ERROR_EINVAL && a.poll(counts, 2) == 0, "overflow rejected without count mutation");
	Check(a.cancel(counts, INT32_MIN, nullptr) == 0 && a.poll(counts, 2) == 0, "negative Cancel restores nonzero initial count");
	Check(a.signal(counts, INT32_MAX) == 0 && a.signal(counts, 1) == KERNEL_ERROR_EINVAL &&
	      a.poll(counts, INT32_MAX) == 0, "exact maximum accepted, overflow rejected");
	Check(a.remove(counts) == 0, "count fixture cleanup");
	char max_name[32]; std::memset(max_name, 'n', 31); max_name[31] = 0;
	const auto longest = a.Make(max_name);
	Check(a.remove(longest) == 0, "31 character name accepted");
	std::memset(max_name, 'n', sizeof(max_name));
	Check(a.create(&out, max_name, 1, 0, 1, nullptr) == KERNEL_ERROR_ENAMETOOLONG && out == Sentinel, "unterminated 32-byte name");
	Check(a.open(&out, max_name) == KERNEL_ERROR_ENAMETOOLONG && out == Sentinel, "Open bounded name");
	for (auto failure: {S::SemaFailure::ObjectAllocation, S::SemaFailure::IdAllocation, S::SemaFailure::OutputCopy}) {
		S::SemaFailForTest(failure);
		const int expected = failure == S::SemaFailure::ObjectAllocation ? KERNEL_ERROR_ENOMEM :
		                     failure == S::SemaFailure::IdAllocation ? KERNEL_ERROR_EAGAIN : KERNEL_ERROR_EFAULT;
		Check(a.create(&out, "sema-test.rollback", 0x101, 1, 1, nullptr) == expected && out == Sentinel, "Create rollback");
		Resources(0, 0, 0);
		Check(a.open(&out, "sema-test.rollback") == KERNEL_ERROR_ESRCH, "rollback leaves no named publication");
	}
	const auto creator = a.Make("sema-test.openrollback");
	Check(a.create(nullptr, "sema-test.openrollback", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EPERM,
	      "duplicate Create precedes invalid output");
	S::SemaFailForTest(S::SemaFailure::IdAllocation);
	Check(a.open(nullptr, "sema-test.absent") == KERNEL_ERROR_ESRCH, "missing name precedes ID allocation hook");
	Check(a.open(&out, "sema-test.openrollback") == KERNEL_ERROR_EAGAIN && out == Sentinel,
	      "ID failure hook remains pending until actual allocation stage");
	S::SemaFailForTest(S::SemaFailure::IdAllocation);
	Check(a.create(nullptr, "sema-test.openrollback", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EAGAIN,
	      "injected ID allocation failure precedes duplicate at native allocation stage");
	S::SemaFailForTest(S::SemaFailure::OutputCopy);
	Check(a.open(nullptr, "sema-test.absent") == KERNEL_ERROR_ESRCH, "missing name does not consume copy failure hook");
	Check(a.open(&out, "sema-test.openrollback") == KERNEL_ERROR_EFAULT && out == Sentinel,
	      "output hook remains pending until otherwise successful Open");
	S::SemaFailForTest(S::SemaFailure::OutputCopy);
	Check(a.create(nullptr, "sema-test.openrollback", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EPERM,
	      "duplicate does not consume copy failure hook");
	Check(a.create(&out, "sema-test.hook", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EFAULT && out == Sentinel,
	      "output hook remains pending until otherwise successful Create");
	for (auto failure: {S::SemaFailure::IdAllocation, S::SemaFailure::OutputCopy}) {
		S::SemaFailForTest(failure);
		Check(a.open(&out, "sema-test.openrollback") == (failure == S::SemaFailure::IdAllocation ? KERNEL_ERROR_EAGAIN : KERNEL_ERROR_EFAULT)
		      && out == Sentinel, "Open rollback preserves qword and creator");
		Resources(1, 1, 1);
	}
	Check(a.remove(creator) == 0, "cleanup rollback creator");
	Check(S::SemaLimitForTest(3), "quiescent bounded capacity fixture");
	const auto first = a.Make("sema-test.cap1"), second = a.Make("sema-test.cap2");
	Check(a.create(&out, "sema-test.full", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EAGAIN && out == Sentinel, "real ID capacity exhaustion");
	Check(a.open(&out, "sema-test.cap1") == KERNEL_ERROR_EAGAIN && out == Sentinel, "Open real ID exhaustion");
	Check(a.open(nullptr, "sema-test.cap1") == KERNEL_ERROR_EAGAIN, "Open exhaustion precedes invalid output");
	Check(a.create(nullptr, "sema-test.cap1", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EAGAIN,
	      "Create exhaustion precedes duplicate and invalid output");
	Check(!S::SemaLimitForTest(8192), "test reset refuses live ownership");
	Check(a.remove(first) == 0 && a.remove(second) == 0 && S::SemaLimitForTest(8192), "capacity restored only quiescent");
	Resources(0, 0, 0);
}
void Tombstone(const Api& a) {
	const auto original = a.Make("sema-test.tomb");
	Handle alias = 0, alias2 = 0;
	Check(a.open(&alias, "sema-test.tomb") == 0 && a.open(&alias2, "sema-test.tomb") == 0, "two retained aliases");
	Check(a.remove(original) == 0, "delete creator with aliases");
	Invalid(a, original); Resources(2, 0, 1);
	Handle out = Sentinel;
	Check(a.open(&out, "sema-test.tomb") == KERNEL_ERROR_ESRCH && out == Sentinel, "name unlinked at deletion");
	const auto replacement = a.Make("sema-test.tomb");
	Check(a.poll(alias, 1) == 0 && a.poll(alias2, 1) == KERNEL_ERROR_EBUSY, "deleted alias Poll consumes old count");
	Check(a.signal(alias, 1) == 0 && a.poll(alias, 1) == KERNEL_ERROR_EBUSY, "deleted Signal is no-op success");
	uint32_t timeout = 0;
	Check(a.wait(alias, 1, &timeout) == KERNEL_ERROR_EACCES && a.wait(alias, 2, &timeout) == KERNEL_ERROR_EACCES,
	      "deleted Wait checks tombstone before max");
	int waiters = 77;
	Check(a.cancel(alias, INT32_MAX, &waiters) == 0 && waiters == 0, "deleted Cancel success/zero even count above max");
	Check(a.poll(replacement, 1) == 0, "new same-name object's count independent");
	Check(a.close(alias) == 0 && a.close(alias2) == 0 && a.remove(replacement) == 0, "alias final close cleanup");
	Resources(0, 0, 0);
}
void GenerationAndRetention(const Api& a) {
	Handle old = a.Make("sema-test.generation", 1);
	Check(a.remove(old) == 0, "retire original slot");
	Handle next = a.Make("sema-test.generation", 1);
	Check((old & 0x1fff) == (next & 0x1fff) && old != next, "same slot new generation");
	Invalid(a, old);
	Check(a.remove(next) == 0, "generation cleanup");
	// Explicitly demonstrate finite 3-bit wrap, rather than claiming never reuse.
	old = a.Make("sema-test.wrap", 1);
	Check(a.remove(old) == 0, "wrap start");
	for (int i = 0; i != 7; ++i) { next = a.Make("sema-test.wrap", 1); Check(a.remove(next) == 0, "wrap cycle"); }
	next = a.Make("sema-test.wrap", 1);
	Check(next == old && a.remove(next) == 0, "native finite generation cycle");
	const auto creator = a.Make("sema-test.retain"); Handle alias = 0;
	Check(a.open(&alias, "sema-test.retain") == 0, "retained alias setup");
	Gate retained; int result = -1;
	std::thread worker([&] {
		S::SemaHookForTest(S::SemaHookPoint::Retained, &Gate::Hook, &retained);
		result = a.poll(alias, 1);
	});
	retained.Await();
	Check(a.close(alias) == 0, "close slot while Poll owns object");
	Handle reused = 0;
	Check(a.open(&reused, "sema-test.retain") == 0 && (reused & 0x1fff) == (alias & 0x1fff) && reused != alias,
	      "alias numeric slot reuse under retained operation");
	Check(a.remove(creator) == 0 && a.close(reused) == 0, "retained operation is now final old object owner");
	Resources(0, 0, 1);
	const auto replacement = a.Make("sema-test.retain");
	retained.Release(); worker.join();
	Check(result == 0 && a.poll(replacement, 1) == 0, "retained Poll affects old object only");
	Check(a.remove(replacement) == 0, "replacement cleanup"); Resources(0, 0, 0);
}
void WaitCancelDelete(const Api& a) {
	const auto id = a.Make("sema-test.wait", 0x101, 0, 3);
	uint32_t timeout = 1500;
	Check(a.wait(id, 1, &timeout) == KERNEL_ERROR_ETIMEDOUT && timeout == 0, "real bounded timed wait and remaining output");
	Check(S::SemaWaitersForTest(id) == 0, "timeout retires stack node");
	Gate parked;
	int result = -1;
	std::thread signaled([&] {
		S::SemaHookForTest(S::SemaHookPoint::BeforePark, &Gate::Hook, &parked);
		result = a.wait(id, 1, nullptr);
	});
	parked.Await();
	Check(S::SemaWaitersForTest(id) == 1 && a.signal(id, 1) == 0, "signal between predicate and host park");
	parked.Release(); signaled.join(); Check(result == 0, "no lost wakeup across unlock-to-park");
	Gate enrolled;
	std::thread canceled([&] {
		S::SemaHookForTest(S::SemaHookPoint::Enqueued, &Gate::Hook, &enrolled);
		result = a.wait(id, 2, nullptr);
	});
	enrolled.Await(); int waiters = -1;
	Check(a.cancel(id, -1, &waiters) == 0 && waiters == 1, "cancel returns enrolled count, restores initial");
	enrolled.Release(); canceled.join();
	Check(result == KERNEL_ERROR_ECANCELED && a.poll(id, 1) == KERNEL_ERROR_EBUSY, "cancel wake/error and restored zero");
	Check(a.cancel(id, 4, nullptr) == KERNEL_ERROR_EINVAL, "cancel maximum");
	Gate waiting, retiring, draining, competing; waiting.next = &retiring;
	waiting.next_point = S::SemaHookPoint::BeforeRetire;
	std::atomic<bool> deleted {false}, second_done {false};
	int delete_result = -1, second_result = -1;
	std::thread victim([&] {
		S::SemaHookForTest(S::SemaHookPoint::Enqueued, &Gate::Hook, &waiting);
		result = a.wait(id, 1, nullptr);
	});
	waiting.Await();
	std::thread deleter([&] {
		S::SemaHookForTest(S::SemaHookPoint::DrainPark, &Gate::Hook, &draining);
		delete_result = a.remove(id); deleted = true;
	});
	draining.Await();
	Check(!deleted.load() && !S::SemaLimitForTest(8192), "delete waits for enrolled retirement, reset refused");
	waiting.Release(); retiring.Await();
	Resources(1, 1, 1);
	Handle alias = Sentinel, duplicate = Sentinel;
	Check(a.open(&alias, "sema-test.wait") == 0, "Open during drain finds old named object");
	Handle closing_alias = Sentinel;
	Check(a.open(&closing_alias, "sema-test.wait") == 0 && closing_alias != alias,
	      "second alias opened during creator drain");
	Resources(3, 1, 1);
	Check(a.close(closing_alias) == 0 && !deleted.load(), "alias Close completes while creator drain is pending");
	Resources(2, 1, 1);
	Invalid(a, closing_alias);
	Check(S::SemaWaitersForTest(id) == 1, "alias Close preserves creator and enrolled waiter");
	Check(a.create(&duplicate, "sema-test.wait", 0x101, 0, 3, nullptr) == KERNEL_ERROR_EPERM && duplicate == Sentinel,
	      "name remains reserved while delete drains");
	uint32_t zero = 0;
	Check(a.wait(alias, 1, &zero) == KERNEL_ERROR_EACCES && a.signal(alias, 1) == 0 &&
	      a.poll(alias, 1) == KERNEL_ERROR_EBUSY, "opened alias references old tombstone");
	Check(a.wait(id, 1, &zero) == KERNEL_ERROR_EACCES, "creator ID remains reserved during drain");
	std::thread second([&] {
		S::SemaHookForTest(S::SemaHookPoint::RemovePark, &Gate::Hook, &competing);
		second_result = a.remove(id | UINT64_C(0x1234000000000000)); second_done = true;
	});
	competing.Await();
	Check(!second_done.load() && !deleted.load(), "competing Delete serialized outside locks");
	draining.Release(); retiring.Release(); victim.join(); deleter.join();
	Check(result == KERNEL_ERROR_EACCES && delete_result == 0 && deleted.load(), "delete wakes and drains");
	Resources(1, 0, 1);
	Invalid(a, id);
	auto replacement = a.Make("sema-test.wait", 0x101, 1, 3);
	Check((replacement & 0x1fff) == (id & 0x1fff) && replacement != id, "creator slot reused after drain");
	Check(a.remove(replacement) == 0, "retire first replacement");
	for (int i = 0; i != 6; ++i) {
		replacement = a.Make("sema-test.wait", 0x101, 1, 3);
		Check(a.remove(replacement) == 0, "cycle generation while contender is bound to old object");
	}
	replacement = a.Make("sema-test.wait", 0x101, 1, 3);
	Check(replacement == id, "force complete three-bit ABA wrap before contender resumes");
	competing.Release(); second.join();
	Check(second_result == 0 && a.poll(replacement, 1) == 0, "competing Delete cannot remove replacement");
	Check(a.wait(alias, 1, &zero) == KERNEL_ERROR_EACCES && a.close(alias) == 0 && a.remove(replacement) == 0,
	      "old alias remains tombstoned after same-name reuse");
	Resources(0, 0, 0);
}
void DrainFailureRetry(const Api& a) {
	const auto id = a.Make("sema-test.drain-fail", 0x101, 0, 1);
	Gate enrolled, first_drain, contender, retry_drain;
	contender.next = &retry_drain;
	contender.next_point = S::SemaHookPoint::DrainPark;
	int wait_result = -1, first_result = -1, retry_result = -1;
	std::thread waiter([&] {
		S::SemaHookForTest(S::SemaHookPoint::Enqueued, &Gate::Hook, &enrolled);
		wait_result = a.wait(id, 1, nullptr);
	});
	enrolled.Await();
	std::thread first([&] {
		S::SemaFailForTest(S::SemaFailure::DrainPark);
		S::SemaHookForTest(S::SemaHookPoint::DrainPark, &Gate::Hook, &first_drain);
		first_result = a.remove(id);
	});
	first_drain.Await();
	std::thread second([&] {
		S::SemaHookForTest(S::SemaHookPoint::RemovePark, &Gate::Hook, &contender);
		retry_result = a.remove(id);
	});
	contender.Await(); // first still owns removal; second is bound to the same creator
	first_drain.Release(); first.join();
	Check(first_result == KERNEL_ERROR_EIO, "injected drain failure returned EIO");
	Resources(1, 1, 1);
	Check(S::SemaWaitersForTest(id) == 1, "failed drain retains enrolled waiter and creator ID");
	uint32_t zero = 0;
	Check(a.wait(id, 1, &zero) == KERNEL_ERROR_EACCES, "failed delete leaves creator tombstoned");
	Handle alias = Sentinel, duplicate = Sentinel;
	Check(a.open(&alias, "sema-test.drain-fail") == 0 && alias != id,
	      "name still opens tombstoned object after failure");
	Resources(2, 1, 1);
	Check(a.wait(alias, 1, &zero) == KERNEL_ERROR_EACCES && a.close(alias) == 0,
	      "tombstone alias remains closeable");
	Resources(1, 1, 1);
	Check(a.create(&duplicate, "sema-test.drain-fail", 0x101, 0, 1, nullptr) == KERNEL_ERROR_EPERM &&
	      duplicate == Sentinel, "failed delete retains name reservation and output");
	contender.Release(); retry_drain.Await(); // contender reacquires deletion ownership
	enrolled.Release(); waiter.join();
	Check(wait_result == KERNEL_ERROR_EACCES && S::SemaWaitersForTest(id) == 0,
	      "deleted waiter retires before successful retry");
	retry_drain.Release(); second.join();
	Check(retry_result == 0, "competing delete retries and succeeds");
	Invalid(a, id);
	Resources(0, 0, 0);
}
void Ordering(const Api& a, uint32_t order, bool priority_first) {
	const auto id = a.Make("sema-test.order", order, 0, 3);
	Gate first, second;
	int r1 = -1, r2 = -1;
	std::thread t1([&] {
		S::SemaPriorityForTest(700); S::SemaHookForTest(S::SemaHookPoint::Enqueued, &Gate::Hook, &first);
		r1 = a.wait(id, 2, nullptr);
	});
	first.Await();
	std::thread t2([&] {
		S::SemaPriorityForTest(300); S::SemaHookForTest(S::SemaHookPoint::Enqueued, &Gate::Hook, &second);
		r2 = a.wait(id, 1, nullptr);
	});
	second.Await();
	Check(a.signal(id, 1) == 0, "ordering first signal");
	// With FIFO, head needs two, so the one token remains (no skipping).
	// With priority, the second waiter owns it already, despite not retiring.
	Check(a.poll(id, 1) == (priority_first ? KERNEL_ERROR_EBUSY : 0), "FIFO head blocking versus priority selection");
	int count = -1;
	Check(a.cancel(id, 0, &count) == 0 && count == 2, "cancel counts completed-but-not-retired too");
	first.Release(); second.Release(); t1.join(); t2.join();
	Check(r1 == KERNEL_ERROR_ECANCELED && r2 == (priority_first ? 0 : KERNEL_ERROR_ECANCELED), "ordering results");
	Check(a.remove(id) == 0, "ordering cleanup");
}
void DispatchRetirement(const Api& a) {
	struct Context { const Api* api; Handle id; } context {&a, a.Make("sema-test.interrupt", 1, 0, 1)};
	S::SemaFailForTest(S::SemaFailure::InterruptWait);
	S::SemaHookForTest(S::SemaHookPoint::Dispatch, [](void* opaque) {
		auto& c = *static_cast<Context*>(opaque);
		Check(Common::GetHostLockCriticalCounter()->load() == 0 && S::SemaWaitersForTest(c.id) == 0,
		      "waiter retired before guest signal dispatch");
		Check(c.api->remove(c.id) == 0, "signal handler can delete without draining itself");
	}, &context);
	Check(a.wait(context.id, 1, nullptr) == KERNEL_ERROR_ESRCH, "EINTR retry revalidates original ID");
	Resources(0, 0, 0);
}

struct Pages {
	char* data = nullptr;
	size_t size = 0;
	Pages() {
#if defined(_WIN32)
		SYSTEM_INFO info {}; GetSystemInfo(&info); size = info.dwPageSize;
		data = static_cast<char*>(VirtualAlloc(nullptr, size * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#elif defined(__linux__)
		size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
		auto* p = ::mmap(nullptr, size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		data = p == MAP_FAILED ? nullptr : static_cast<char*>(p);
#endif
		Check(data != nullptr, "allocate fault fixture pages");
	}
	void ProtectSecond(bool writable, bool inaccessible = false) {
#if defined(_WIN32)
		DWORD previous = 0;
		Check(VirtualProtect(data + size, size, inaccessible ? PAGE_NOACCESS : writable ? PAGE_READWRITE : PAGE_READONLY,
		                     &previous) != 0, "protect fault fixture");
#elif defined(__linux__)
		Check(::mprotect(data + size, size, inaccessible ? PROT_NONE : writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0,
		      "protect fault fixture");
#endif
	}
	~Pages() {
#if defined(_WIN32)
		Check(VirtualFree(data, 0, MEM_RELEASE) != 0, "free fixture pages");
#elif defined(__linux__)
		Check(::munmap(data, size * 2) == 0, "free fixture pages");
#endif
	}
};
void Faults(const Api& a) {
	Handle out = Sentinel;
	Check(a.create(&out, reinterpret_cast<const char*>(1), 1, 0, 1, nullptr) == KERNEL_ERROR_EFAULT && out == Sentinel,
	      "fault-contained name copy");
	Check(a.open(&out, reinterpret_cast<const char*>(1)) == KERNEL_ERROR_EFAULT && out == Sentinel, "Open name fault");
	Check(a.open(&out, nullptr) == KERNEL_ERROR_EFAULT && out == Sentinel, "Open null name fault");
	Check(a.create(nullptr, "sema-test.fault", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EFAULT, "null output fault");
	Pages pages;
	std::memcpy(pages.data + pages.size - 4, &Sentinel, sizeof(Sentinel));
	pages.ProtectSecond(false, true);
	Check(a.create(reinterpret_cast<Handle*>(pages.data + pages.size - 4), "sema-test.fault", 0x101, 1, 1, nullptr)
	      == KERNEL_ERROR_EFAULT, "cross-page output preflight");
	Check(std::memcmp(pages.data + pages.size - 4, &Sentinel, 4) == 0, "valid output prefix not partially overwritten");
	Check(a.open(&out, "sema-test.fault") == KERNEL_ERROR_ESRCH, "bad output leaves no published name");
	// Copy only to the first terminator; never require readable bytes beyond it.
	pages.data[pages.size - 2] = 'x'; pages.data[pages.size - 1] = 0;
	const auto boundary_name = a.Make(pages.data + pages.size - 2, 1);
	Check(a.remove(boundary_name) == 0, "name ending at guard boundary");
	pages.ProtectSecond(true);
	std::memcpy(pages.data + pages.size, &Sentinel, sizeof(Sentinel));
	pages.ProtectSecond(false);
	const auto id = a.Make("sema-test.copyfault");
	auto* readonly_out = reinterpret_cast<Handle*>(pages.data + pages.size);
	Check(a.open(readonly_out, "sema-test.missing") == KERNEL_ERROR_ESRCH &&
	      a.create(readonly_out, "sema-test.copyfault", 0x101, 1, 1, nullptr) == KERNEL_ERROR_EPERM,
	      "missing and duplicate errors precede read-only output failure");
	Check(a.open(reinterpret_cast<Handle*>(pages.data + pages.size), "sema-test.copyfault") == KERNEL_ERROR_EFAULT,
	      "Open read-only output rejected without alias");
	Check(std::memcmp(pages.data + pages.size, &Sentinel, sizeof(Sentinel)) == 0, "read-only output preserved");
	Check(a.wait(id, 1, reinterpret_cast<uint32_t*>(1)) == KERNEL_ERROR_EFAULT && a.poll(id, 1) == 0,
	      "timeout input fault occurs before acquisition");
	pages.ProtectSecond(true);
	uint32_t zero = 0;
	std::memcpy(pages.data + pages.size, &zero, sizeof(zero));
	pages.ProtectSecond(false);
	auto* read_only_timeout = reinterpret_cast<uint32_t*>(pages.data + pages.size);
	Check(a.wait(id, 2, read_only_timeout) == KERNEL_ERROR_EINVAL, "wait error takes precedence over timeout copyout fault");
	Check(a.wait(id, 1, read_only_timeout) == KERNEL_ERROR_ETIMEDOUT, "timeout error preserved over output fault");
	Check(a.signal(id, 1) == 0 && a.wait(id, 1, read_only_timeout) == KERNEL_ERROR_EFAULT &&
	      a.poll(id, 1) == KERNEL_ERROR_EBUSY, "successful wait consumes before timeout copyout fault");
	Check(a.signal(id, 1) == 0, "restore count");
	Check(a.cancel(id, 0, reinterpret_cast<int*>(pages.data + pages.size)) == KERNEL_ERROR_EFAULT &&
	      a.poll(id, 1) == KERNEL_ERROR_EBUSY, "Cancel mutation precedes failed output copy");
	Resources(1, 1, 1);
	Check(a.remove(id) == 0, "fault fixture cleanup"); Resources(0, 0, 0);
}
} // namespace

void TestKernelSemaphores() {
	Deadline deadline;
	Api api;
	Resources(0, 0, 0); // isolated selector: no audio or guest threads started
	AbiAndRoles(api); CreateErrors(api); Tombstone(api); GenerationAndRetention(api);
	WaitCancelDelete(api); DrainFailureRetry(api); Ordering(api, 0, false); Ordering(api, 1, false); Ordering(api, 2, true);
	DispatchRetirement(api); Faults(api);
	Resources(0, 0, 0);
	Check(S::SemaLimitForTest(8192), "quiescent cleanup; no destructive reset");
	Check(Common::GetHostLockCriticalCounter()->load() == 0, "main critical counter balanced");
	std::fputs("KernelSemaphoreTests: passed\n", stderr);
}
