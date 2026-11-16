// Native sceKernel*Sema family only. Included by semaphore.cpp; POSIX sem_t is separate.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
}

namespace Libs::LibKernel::Semaphore {
LIB_NAME("libkernel", "libkernel");
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint16_t SemaType = 0x120;
constexpr uint32_t SlotCount = 8192;
constexpr uint32_t SignalSliceUs = 10000;

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
thread_local SemaFailure g_failure = SemaFailure::None;
thread_local SemaHookPoint g_hook_point = SemaHookPoint::None;
thread_local void (*g_hook)(void*) = nullptr;
thread_local void* g_hook_context = nullptr;
thread_local int g_test_priority = -1;
std::atomic<uint32_t> g_live_objects {0};
bool Fail(SemaFailure point) {
	if (g_failure != point) { return false; }
	g_failure = SemaFailure::None;
	return true;
}
void Hook(SemaHookPoint point) {
	if (g_hook != nullptr && g_hook_point == point) {
		auto fn = std::exchange(g_hook, nullptr);
		g_hook_point = SemaHookPoint::None;
		fn(std::exchange(g_hook_context, nullptr));
	}
}
#else
enum class SemaFailure { ObjectAllocation, IdAllocation, OutputCopy, InterruptWait };
enum class SemaHookPoint { Retained, Enqueued, BeforePark, BeforeRetire, Deleted, DrainPark, Dispatch, RemovePark, AfterPark };
bool Fail(SemaFailure) { return false; }
void Hook(SemaHookPoint) {}
#endif

// These adapters never expose a C++ object pointer to a guest. Resolve only in
// Create, before publication, never on a guest wait/dispatch/drain path.
struct HostAccess {
#if defined(_WIN32)
	using Write = LONG (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
	using Wait = LONG (NTAPI *)(volatile VOID*, PVOID, SIZE_T, PLARGE_INTEGER);
	using Wake = VOID (NTAPI *)(PVOID);
	Write write = nullptr;
	Wait wait = nullptr;
	Wake wake = nullptr;
#endif
	bool Initialize() {
#if defined(_WIN32)
		const auto module = GetModuleHandleW(L"ntdll.dll");
		if (module == nullptr) { return false; }
		write = reinterpret_cast<Write>(GetProcAddress(module, "NtWriteVirtualMemory"));
		wait = reinterpret_cast<Wait>(GetProcAddress(module, "RtlWaitOnAddress"));
		wake = reinterpret_cast<Wake>(GetProcAddress(module, "RtlWakeAddressAll"));
		return write != nullptr && wait != nullptr && wake != nullptr;
#elif defined(__linux__)
		return true;
#else
		return false;
#endif
	}
	bool Store(void* destination, const void* source, size_t size) const {
#if defined(_WIN32)
		SIZE_T copied = 0;
		return write != nullptr && write(GetCurrentProcess(), destination,
		    const_cast<void*>(source), size, &copied) >= 0 && copied == size;
#elif defined(__linux__)
		iovec local {const_cast<void*>(source), size}, remote {destination, size};
		return ::process_vm_writev(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
		return false;
#endif
	}
	bool Park(std::atomic<uint32_t>& epoch, uint32_t expected, uint32_t micros) const {
		// No registry/object mutex or critical guard is owned here. The address
		// comparison closes the unlock-to-park lost-wakeup window.
#if defined(_WIN32)
		LARGE_INTEGER timeout {};
		timeout.QuadPart = -static_cast<LONGLONG>(std::max(micros, 1u)) * 10;
		const LONG status = wait(&epoch, &expected, sizeof(expected), &timeout);
		return status == 0 || status == 0x102; // STATUS_SUCCESS / STATUS_TIMEOUT
#elif defined(__linux__)
		const timespec timeout {static_cast<time_t>(micros / 1000000u),
		                        static_cast<long>(micros % 1000000u) * 1000L};
		const long result = ::syscall(SYS_futex, &epoch, FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
		return result == 0 || errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT;
#else
		return false;
#endif
	}
	void WakeAll(std::atomic<uint32_t>& epoch) const {
#if defined(_WIN32)
		wake(&epoch);
#elif defined(__linux__)
		::syscall(SYS_futex, &epoch, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
#endif
	}
};
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
static_assert(std::atomic<uint32_t>::is_always_lock_free);

bool Range(const void* pointer, size_t size, bool write) {
	const auto begin = reinterpret_cast<uintptr_t>(pointer);
	if (begin == 0 || size > 32 || size > UINTPTR_MAX - begin) { return false; }
	const auto end = begin + size;
#if defined(_WIN32)
	for (auto cursor = begin; cursor < end;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) { return false; }
		const DWORD mode = info.Protect & 0xff;
		const bool writable = mode == PAGE_READWRITE || mode == PAGE_WRITECOPY ||
		                      mode == PAGE_EXECUTE_READWRITE || mode == PAGE_EXECUTE_WRITECOPY;
		if (!writable && (write || (mode != PAGE_READONLY && mode != PAGE_EXECUTE_READ))) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
		if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= cursor) { return false; }
		cursor = base + info.RegionSize;
	}
	return true;
#elif defined(__linux__)
	if (!write) { return true; } // process_vm_readv itself is the fault boundary
	// Preflight all output pages before a possibly partial system copy. This
	// runs BEFORE registry/object locking; it is not an atomic VM-map lease.
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	char line[512];
	auto cursor = begin;
	bool continuation = false;
	for (size_t n = 0; n < 65536 && cursor < end && std::fgets(line, sizeof(line), maps); ++n) {
		const bool skip = continuation;
		continuation = std::strchr(line, '\n') == nullptr;
		if (skip) { continue; }
		unsigned long long lo = 0, hi = 0;
		char perms[5] {};
		if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3 || hi <= cursor) { continue; }
		if (lo > cursor || perms[1] != 'w') { break; }
		cursor = static_cast<uintptr_t>(hi);
	}
	std::fclose(maps);
	return cursor >= end;
#else
	return false;
#endif
}
bool Load(const void* source, void* destination, size_t size) {
	if (!Range(source, size, false)) { return false; }
#if defined(_WIN32)
	SIZE_T copied = 0;
	return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) != 0 && copied == size;
#elif defined(__linux__)
	iovec local {destination, size}, remote {const_cast<void*>(source), size};
	return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
	return false;
#endif
}
int ReadName(const char* guest, std::array<char, 32>& name) {
	const auto address = reinterpret_cast<uintptr_t>(guest);
	for (size_t i = 0; i != name.size(); ++i) {
		if (address > UINTPTR_MAX - i || !Load(reinterpret_cast<const void*>(address + i), &name[i], 1)) {
			return KERNEL_ERROR_EFAULT;
		}
		if (name[i] == '\0') { return OK; }
	}
	return KERNEL_ERROR_ENAMETOOLONG;
}
bool PrepareThread() {
	try { (void)Common::GetHostLockCriticalCounter(); }
	catch (const std::bad_alloc&) { return false; }
	return true;
}

struct Waiter {
	Waiter* previous = nullptr;
	Waiter* next = nullptr;
	int priority = 700;
	int need = 0;
	int result = OK;
	bool ready = false;
	bool queued = false;
};
struct Object {
	Common::Mutex mutex;
	const std::array<char, 32> name;
	const bool fifo;
	const bool named;
	const int initial;
	const int maximum;
	const HostAccess host;
	int count;
	bool deleted = false;
	uint32_t active = 0; // all enrolled waiters, including completed-but-not-retired
	uint32_t handles = 0; // registry mutex only; operation refs are separate
	// Registry mutex only. One creator per Object, never resurrected/rebound.
	bool creator_removing = false;
	bool creator_removed = false;
	alignas(4) std::atomic<uint32_t> removal_epoch {0};
	Waiter* first = nullptr;
	Waiter* last = nullptr;
	alignas(4) std::atomic<uint32_t> epoch {0};
	Object(const std::array<char, 32>& n, uint32_t attr, int init, int max, HostAccess access)
	    : name(n), fifo((attr & 3) != 2), named((attr & 0x100) != 0), initial(init), maximum(max),
	      host(access), count(init) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		++g_live_objects;
#endif
	}
	~Object() {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		--g_live_objects;
#endif
	}
	// All following list/state helpers require mutex + critical guard.
	void Change() { epoch.fetch_add(1, std::memory_order_release); }
	void Remove(Waiter& waiter) {
		if (!waiter.queued) { return; }
		if (waiter.previous != nullptr) { waiter.previous->next = waiter.next; } else { first = waiter.next; }
		if (waiter.next != nullptr) { waiter.next->previous = waiter.previous; } else { last = waiter.previous; }
		waiter.queued = false;
	}
	void Complete(Waiter& waiter, int result) {
		Remove(waiter);
		waiter.result = result;
		waiter.ready = true;
	}
	void Add(Waiter& waiter) {
		Waiter* before = first;
		if (fifo) { before = nullptr; }
		else { while (before != nullptr && before->priority <= waiter.priority) { before = before->next; } }
		waiter.next = before;
		waiter.previous = before != nullptr ? before->previous : last;
		if (waiter.previous != nullptr) { waiter.previous->next = &waiter; } else { first = &waiter; }
		if (before != nullptr) { before->previous = &waiter; } else { last = &waiter; }
		waiter.queued = true;
		++active;
	}
	void Delete() {
		deleted = true;
		while (first != nullptr) { Complete(*first, KERNEL_ERROR_EACCES); }
		Change();
	}
};

struct Slot {
	std::shared_ptr<Object> object;
	uint16_t type = 0;
	uint8_t generation = 0;
	bool alias = false;
};
// Honest scope: one global HLE semaphore-family namespace. No fabricated guest
// process/capability model; other HLE object families are not in this table.
Common::Mutex g_registry_mutex;
std::array<Slot, SlotCount> g_slots;
uint32_t g_slot_limit = SlotCount;

Slot* Find(KernelSema carrier) { // registry lock required
	const uint32_t id = static_cast<uint32_t>(carrier);
	if (id > 0xffff || (id & 0x1fff) == 0) { return nullptr; }
	auto& slot = g_slots[id & 0x1fff];
	if (!slot.object || slot.type != SemaType || slot.generation != (id >> 13)) { return nullptr; }
	return &slot;
}
uint32_t FreeSlot() { // registry lock required; zero is reserved as HLE invalid
	for (uint32_t i = 1; i < g_slot_limit; ++i) { if (!g_slots[i].object) { return i; } }
	return 0;
}
Slot* Named(const std::array<char, 32>& name) { // registry lock required
	for (auto& slot: g_slots) {
		if (slot.object && slot.type == SemaType && !slot.alias && slot.object->named && slot.object->name == name) {
			return &slot;
		}
	}
	return nullptr;
}
void Retire(Slot& slot) { // an outside owner must retain object through unlock
	slot.object.reset();
	slot.type = 0;
	slot.alias = false;
	slot.generation = (slot.generation + 1) & 7;
}
std::shared_ptr<Object> Retain(KernelSema id) {
	std::shared_ptr<Object> object;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		if (auto* slot = Find(id)) { object = slot->object; }
	}
	if (object) { Hook(SemaHookPoint::Retained); }
	return object;
}

// No allocating container or waiter allocation after enrollment. This guard
// also unlinks a stack node on ordinary C++ unwinding (not forced termination).
struct Enrollment {
	Object& object;
	Waiter& waiter;
	bool enrolled = false;
	~Enrollment() {
		if (!enrolled) { return; }
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(object.mutex);
			object.Remove(waiter);
			--object.active;
			object.Change();
		}
		object.host.WakeAll(object.epoch);
	}
};
uint32_t Remaining(Clock::time_point deadline) {
	const auto delta = std::chrono::ceil<std::chrono::microseconds>(deadline - Clock::now()).count();
	return delta <= 0 ? 0 : static_cast<uint32_t>(std::min<int64_t>(delta, UINT32_MAX));
}
bool PendingSignal() {
	auto* self = PthreadSelfOrNull();
	if (self == nullptr) { return false; }
	for (int signal = 0; signal < 64; ++signal) {
		if (PthreadHasPendingSignal(self, signal)) { return true; }
	}
	return false;
}
int Wait(Object& object, int need, uint32_t* micros) {
	Waiter waiter;
	waiter.need = need;
	waiter.priority = object.fifo ? 0 : PthreadGetCurrentPriorityForKernel();
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_priority >= 0) { waiter.priority = g_test_priority; }
#endif
	Enrollment enrollment {object, waiter};
	const bool timed = micros != nullptr;
	const auto deadline = Clock::now() + std::chrono::microseconds(timed ? *micros : 0);
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(object.mutex);
		if (object.deleted) { return KERNEL_ERROR_EACCES; }
		if (need > object.maximum) { return KERNEL_ERROR_EINVAL; }
		if (object.count >= need) { object.count -= need; return OK; }
		if (timed && *micros == 0) { return KERNEL_ERROR_ETIMEDOUT; }
		object.Add(waiter);
		enrollment.enrolled = true;
	}
	Hook(SemaHookPoint::Enqueued);
	for (;;) {
		uint32_t expected = 0;
		uint32_t slice = timed ? std::min(Remaining(deadline), SignalSliceUs) : SignalSliceUs;
		const bool interrupt = PendingSignal() || Fail(SemaFailure::InterruptWait);
		bool done = false;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(object.mutex);
			if (!waiter.ready && timed && slice == 0) { object.Complete(waiter, KERNEL_ERROR_ETIMEDOUT); }
			if (!waiter.ready && interrupt) { object.Complete(waiter, KERNEL_ERROR_EINTR); }
			done = waiter.ready;
			expected = object.epoch.load(std::memory_order_acquire);
		}
		if (done) { break; }
		Hook(SemaHookPoint::BeforePark);
		if (!object.host.Park(object.epoch, expected, slice)) {
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(object.mutex);
			if (!waiter.ready) { object.Complete(waiter, KERNEL_ERROR_EIO); }
		}
		Hook(SemaHookPoint::AfterPark); // before the next guarded predicate acquisition
	}
	if (timed) { *micros = Remaining(deadline); }
	Hook(SemaHookPoint::BeforeRetire);
	return waiter.result; // Enrollment retires before the caller's timeout copyout
}
int Drain(Object& object) {
	for (;;) {
		uint32_t expected = 0;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(object.mutex);
			if (object.active == 0) { return OK; }
			expected = object.epoch.load(std::memory_order_acquire);
		}
		Hook(SemaHookPoint::DrainPark);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		// Model only the host failure return for this caller's next drain park.
		if (Fail(SemaFailure::DrainPark)) { return KERNEL_ERROR_EIO; }
#endif
		if (!object.host.Park(object.epoch, expected, SignalSliceUs)) { return KERNEL_ERROR_EIO; }
	}
}
int DeleteCreator(KernelSema id) {
	std::shared_ptr<Object> object;
	for (;;) {
		uint32_t expected = 0;
		bool owner = false;
		{
			Common::HostLockCriticalGuard registry_critical;
			Common::LockGuard registry_lock(g_registry_mutex);
			// A contender is bound to the originally retained creator, even if
			// this numeric ID wraps and now names a different object.
			if (object && object->creator_removed) { return OK; }
			auto* slot = Find(id);
			if (slot == nullptr || (object && slot->object != object)) { return KERNEL_ERROR_ESRCH; }
			if (slot->alias) { return KERNEL_ERROR_EPERM; }
			if (!object) { object = slot->object; }
			if (!object->creator_removing) {
				object->creator_removing = true;
				owner = true;
				Common::HostLockCriticalGuard object_critical;
				Common::LockGuard object_lock(object->mutex);
				object->Delete();
			}
			expected = object->removal_epoch.load(std::memory_order_acquire);
		}
		if (owner) { break; }
		Hook(SemaHookPoint::RemovePark);
		if (!object->host.Park(object->removal_epoch, expected, SignalSliceUs)) { return KERNEL_ERROR_EIO; }
	}
	object->host.WakeAll(object->epoch);
	Hook(SemaHookPoint::Deleted);
	int result = Drain(*object); // creator ID/name/handle ownership still reserved
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		auto* slot = Find(id);
		if (slot == nullptr || slot->object != object || slot->alias || !object->creator_removing) {
			result = KERNEL_ERROR_ESRCH;
		} else if (result == OK) {
			--object->handles;
			Retire(*slot); // only after drain: atomically unlinks this creator name
			object->creator_removed = true;
		}
		// On host drain failure leave the tombstoned reservation retryable.
		object->creator_removing = false;
		object->removal_epoch.fetch_add(1, std::memory_order_release);
	}
	object->host.WakeAll(object->removal_epoch);
	return result;
}
int RemoveHandle(KernelSema id, bool close_alias) {
	if (!close_alias) { return DeleteCreator(id); }
	std::shared_ptr<Object> object;
	bool drain = false;
	{
		Common::HostLockCriticalGuard registry_critical;
		Common::LockGuard registry_lock(g_registry_mutex);
		auto* slot = Find(id);
		if (slot == nullptr) { return KERNEL_ERROR_ESRCH; }
		if (slot->alias != close_alias) { return KERNEL_ERROR_EPERM; }
		object = slot->object;
		--object->handles;
		drain = object->handles == 0;
		if (drain) {
			Common::HostLockCriticalGuard object_critical;
			Common::LockGuard object_lock(object->mutex);
			object->Delete();
		}
		// Native Close retires the alias before its final-reference drain.
		Retire(*slot);
	}
	if (!drain) { return OK; }
	object->host.WakeAll(object->epoch);
	Hook(SemaHookPoint::Deleted);
	return Drain(*object);
}
} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCreateSema(
    KernelSema* out, const char* name, uint32_t attr, int init, int max, void* opt) {
	if (opt != nullptr || (attr & ~0x103u) != 0 || (attr & 3) == 3 ||
	    init < 0 || max <= 0 || init > max || name == nullptr) { return KERNEL_ERROR_EINVAL; }
	if ((attr & 3) == 0) { attr |= 1; }
	HostAccess host;
	if (!host.Initialize()) { return KERNEL_ERROR_EOPNOTSUPP; }
	std::array<char, 32> copied_name {};
	if (const int result = ReadName(name, copied_name); result != OK) { return result; }
	const bool output_writable = Range(out, sizeof(*out), true);
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	std::shared_ptr<Object> object;
	try {
		if (Fail(SemaFailure::ObjectAllocation)) { return KERNEL_ERROR_ENOMEM; }
		object = std::make_shared<Object>(copied_name, attr, init, max, host);
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		const uint32_t index = FreeSlot();
		if (index == 0) { return KERNEL_ERROR_EAGAIN; }
		if (Fail(SemaFailure::IdAllocation)) { return KERNEL_ERROR_EAGAIN; }
		if (object->named && Named(copied_name) != nullptr) { return KERNEL_ERROR_EPERM; }
		// Output preflight is outside locks, but its error belongs only to an
		// otherwise-successful operation (native wrappers store after syscall).
		if (!output_writable) { return KERNEL_ERROR_EFAULT; }
		auto& slot = g_slots[index];
		const KernelSema id = index | (uint32_t(slot.generation) << 13);
		slot.object = object;
		slot.type = SemaType;
		slot.alias = false;
		object->handles = 1;
		// Final fallible action, after all initialization; lock excludes observers
		// until this eight-byte publication succeeds or the slot is rolled back.
		if (Fail(SemaFailure::OutputCopy) || !host.Store(out, &id, sizeof(id))) {
			object->handles = 0;
			Retire(slot);
			return KERNEL_ERROR_EFAULT;
		}
	}
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelOpenSema(KernelSema* out, const char* name) {
	std::array<char, 32> copied_name {};
	if (const int result = ReadName(name, copied_name); result != OK) { return result; }
	const bool output_writable = Range(out, sizeof(*out), true);
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	std::shared_ptr<Object> object;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		auto* creator = Named(copied_name);
		if (creator == nullptr) { return KERNEL_ERROR_ESRCH; }
		object = creator->object;
		const uint32_t index = FreeSlot();
		if (index == 0) { return KERNEL_ERROR_EAGAIN; }
		if (Fail(SemaFailure::IdAllocation)) { return KERNEL_ERROR_EAGAIN; }
		if (!output_writable) { return KERNEL_ERROR_EFAULT; }
		auto& slot = g_slots[index];
		const KernelSema id = index | (uint32_t(slot.generation) << 13);
		slot.object = object;
		slot.type = SemaType;
		slot.alias = true;
		++object->handles;
		if (Fail(SemaFailure::OutputCopy) || !object->host.Store(out, &id, sizeof(id))) {
			--object->handles;
			Retire(slot);
			return KERNEL_ERROR_EFAULT;
		}
	}
	return OK;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteSema(KernelSema id) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	return RemoveHandle(id, false);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCloseSema(KernelSema id) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	return RemoveHandle(id, true);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPollSema(KernelSema id, int need) {
	if (need <= 0) { return KERNEL_ERROR_EINVAL; }
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	auto object = Retain(id);
	if (!object) { return KERNEL_ERROR_ESRCH; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(object->mutex);
	// Native Poll deliberately does not test the tombstone.
	if (need > object->maximum) { return KERNEL_ERROR_EINVAL; }
	if (object->count < need) { return KERNEL_ERROR_EBUSY; }
	object->count -= need;
	return OK;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelWaitSema(KernelSema id, int need, KernelUseconds* time) {
	if (need <= 0) { return KERNEL_ERROR_EINVAL; }
	uint32_t micros = 0;
	if (time != nullptr && !Load(time, &micros, sizeof(micros))) { return KERNEL_ERROR_EFAULT; }
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	for (;;) {
		auto object = Retain(id);
		if (!object) { return KERNEL_ERROR_ESRCH; }
		const int result = Wait(*object, need, time != nullptr ? &micros : nullptr);
		if (time != nullptr && (!Range(time, sizeof(micros), true) || !object->host.Store(time, &micros, sizeof(micros)))) {
			// Native 80e264ae..b6 preserves a wait error over a copyout error.
			if (result == OK) { return KERNEL_ERROR_EFAULT; }
		}
		if (result != KERNEL_ERROR_EINTR) { return result; }
		// Native libkernel retries EINTR. Retire the stack waiter before guest
		// dispatch, so a handler may delete this semaphore without self-draining.
		object.reset();
		Hook(SemaHookPoint::Dispatch);
		KernelDispatchPendingSignalForCurrentThread();
		if (time != nullptr && !Load(time, &micros, sizeof(micros))) { return KERNEL_ERROR_EFAULT; }
	}
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSignalSema(KernelSema id, int count) {
	if (count <= 0) { return KERNEL_ERROR_EINVAL; }
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	auto object = Retain(id);
	if (!object) { return KERNEL_ERROR_ESRCH; }
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(object->mutex);
		if (object->deleted) { return OK; }
		if (count > object->maximum - object->count) { return KERNEL_ERROR_EINVAL; }
		object->count += count;
		while (object->first != nullptr && object->first->need <= object->count) {
			object->count -= object->first->need;
			object->Complete(*object->first, OK);
		}
		// Do not skip a head waiter whose need exceeds count (native FIFO/priority).
		object->Change();
	}
	object->host.WakeAll(object->epoch);
	return OK;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCancelSema(KernelSema id, int count, int* threads) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	auto object = Retain(id);
	if (!object) { return KERNEL_ERROR_ESRCH; }
	int waiting = 0;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(object->mutex);
		if (!object->deleted) {
			if (count > object->maximum) { return KERNEL_ERROR_EINVAL; }
			waiting = static_cast<int>(object->active);
			object->count = count < 0 ? object->initial : count;
			while (object->first != nullptr) { object->Complete(*object->first, KERNEL_ERROR_ECANCELED); }
			object->Change();
		}
	}
	object->host.WakeAll(object->epoch);
	// Native mutation precedes optional waiter-count copyout; a copy fault does
	// not undo cancellation. Keep guest pointer access outside object locks.
	if (threads != nullptr && (!Range(threads, sizeof(waiting), true) || !object->host.Store(threads, &waiting, sizeof(waiting)))) {
		return KERNEL_ERROR_EFAULT;
	}
	return OK;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void SemaFailForTest(SemaFailure failure) { g_failure = failure; }
void SemaHookForTest(SemaHookPoint point, void (*hook)(void*), void* context) {
	g_hook_point = point; g_hook = hook; g_hook_context = context;
}
void SemaPriorityForTest(int priority) { g_test_priority = priority; }
SemaResources SemaResourcesForTest() {
	SemaResources result {};
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_registry_mutex);
	for (const auto& slot: g_slots) {
		if (!slot.object) { continue; }
		++result.handles;
		if (!slot.alias && slot.object->named) { ++result.names; }
	}
	result.objects = g_live_objects.load();
	return result;
}
bool SemaLimitForTest(uint32_t limit) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_registry_mutex);
	if (g_live_objects.load() != 0 || limit < 2 || limit > SlotCount) { return false; }
	g_slot_limit = limit;
	return true; // quiescent only; generations are never reset
}
bool SemaTypeForTest(KernelSema id, uint16_t type) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_registry_mutex);
	const uint32_t low = static_cast<uint32_t>(id);
	if (low > 0xffff || (low & 0x1fff) == 0) { return false; }
	auto& slot = g_slots[low & 0x1fff];
	if (!slot.object || slot.generation != (low >> 13)) { return false; }
	slot.type = type;
	return true;
}
uint32_t SemaWaitersForTest(KernelSema id) {
	auto object = Retain(id);
	if (!object) { return 0; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(object->mutex);
	return object->active;
}
#endif
} // namespace Libs::LibKernel::Semaphore
