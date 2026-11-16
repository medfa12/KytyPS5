#include "kernel/eventFlag.h"
#include "kernel/socketpairProvenance.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <string>
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

namespace Libs::LibKernel { void KernelDispatchPendingSignalForCurrentThread(); }
namespace Libs::LibKernel::EventFlag {
LIB_NAME("libkernel", "libkernel");
namespace {

// Explicit compatibility experiment matching the observed 4.03 effort run.
// This substitutes for the absent boot producer; it is not native readiness.
bool ReferenceBootSeedEnabled(const char* name) {
	const char* value = std::getenv("KYTY_REFERENCE_BOOT_FLAGS");
	return value && std::strcmp(value, "1") == 0 && std::strcmp(name, "SceBootStatusFlags") == 0;
}
constexpr uint64_t ReferenceBootSeed = 0x2ce;

// Keep the existing opt-in named-event tracing, but emit outside object locks.
bool TraceWanted(const std::string& name) {
	static const std::string spec = [] { const char* s = std::getenv("KYTY_EVENTFLAG_TRACE"); return std::string(s ? s : ""); }();
	if (spec.empty() || spec == "0") { return false; }
	if (spec == "1" || spec == "*") { return true; }
	for (size_t start = 0; start <= spec.size();) {
		const auto comma = spec.find(',', start);
		std::string token = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		const auto first = token.find_first_not_of(" \t"), last = token.find_last_not_of(" \t");
		if (first != std::string::npos && name.find(token.substr(first, last - first + 1)) != std::string::npos) { return true; }
		if (comma == std::string::npos) { break; }
		start = comma + 1;
	}
	return false;
}
std::atomic<uint64_t> g_trace_lines {0};
thread_local uint64_t g_trace_caller = 0;
bool DiagnosticFailure();
template<class F> void Diagnostic(F&& fn) noexcept {
	try { if (DiagnosticFailure()) { throw std::bad_alloc(); } fn(); }
	catch (const std::exception&) { /* logging-only boundary; fatal assertions are not suppressed */ }
}
void Trace(const std::string& name, const char* operation, uint64_t bits, uint64_t now) noexcept {
	if (g_trace_lines.fetch_add(1, std::memory_order_relaxed) < 20000) {
		Diagnostic([&] { LOGF("ps5-vsh: ef-trace %s name=%s bits=%016" PRIx64 " now=%016" PRIx64 " ra=0x%016" PRIx64 " host=%d\n",
		     operation, name.c_str(), bits, now, g_trace_caller, Common::Thread::GetThreadIdUnique()); });
	}
}

// The carrier stays pointer-shaped for compatibility with existing HLE and the
// observer, but never holds or dereferences a host pointer. Native wrappers use
// low 32 bits. Tokens are process-lifetime monotonic and never recycled.
constexpr uint32_t FirstToken = 0x10000, LastToken = 0x7fffffff;
uint32_t Token(KernelEventFlag handle) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle)); }
KernelEventFlag Handle(uint32_t token) { return reinterpret_cast<KernelEventFlag>(static_cast<uintptr_t>(token)); }
static_assert(sizeof(KernelEventFlag) == 8);
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t) && std::atomic<uint32_t>::is_always_lock_free);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
std::atomic<int> g_objects {0};
thread_local EventFlagHookPoint g_hook_point = EventFlagHookPoint::None;
thread_local void (*g_hook)(void*) = nullptr;
thread_local void* g_hook_context = nullptr;
thread_local void (*g_dispatch_hook)(void*) = nullptr;
thread_local void* g_dispatch_context = nullptr;
thread_local EventFlagFailure g_failure = EventFlagFailure::None;
bool Fail(EventFlagFailure p) { if (g_failure != p) { return false; } g_failure = EventFlagFailure::None; return true; }
bool DiagnosticFailure() { return Fail(EventFlagFailure::Diagnostic); }
bool Hook(EventFlagHookPoint p) noexcept {
	if (g_hook && g_hook_point == p) {
		auto fn = std::exchange(g_hook, nullptr);
		g_hook_point = EventFlagHookPoint::None;
		try { fn(std::exchange(g_hook_context, nullptr)); }
		catch (const std::bad_alloc&) { return p != EventFlagHookPoint::Enrolled; }
	}
	return true;
}
void DispatchTestHook() {
	if (g_dispatch_hook) {
		auto fn = std::exchange(g_dispatch_hook, nullptr);
		fn(std::exchange(g_dispatch_context, nullptr));
	}
}
#else
enum class EventFlagHookPoint { Retained, Enrolled, Cancelled, Retired, AfterPark, Dispatch };
enum class EventFlagFailure { Prepare, Diagnostic, Park, Store };
bool Fail(EventFlagFailure) { return false; }
bool DiagnosticFailure() { return false; }
bool Hook(EventFlagHookPoint) { return true; }
void DispatchTestHook() {}
#endif

bool PrepareThread() noexcept {
	if (Fail(EventFlagFailure::Prepare)) { return false; }
	try { (void)Common::GetHostLockCriticalCounter(); return true; }
	catch (const std::bad_alloc&) { return false; }
}

struct HostAccess {
	enum class ParkResult { Wake, Timeout, Error };
#if defined(_WIN32)
	using Write = LONG (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
	using Wait = LONG (NTAPI *)(volatile VOID*, PVOID, SIZE_T, PLARGE_INTEGER);
	using WakeFn = VOID (NTAPI *)(PVOID);
	Write write = nullptr; Wait wait = nullptr; WakeFn wake = nullptr;
#endif
	bool Init() {
#if defined(_WIN32)
		const auto module = GetModuleHandleW(L"ntdll.dll");
		if (!module) { return false; }
		write = reinterpret_cast<Write>(GetProcAddress(module, "NtWriteVirtualMemory"));
		wait = reinterpret_cast<Wait>(GetProcAddress(module, "RtlWaitOnAddress"));
		wake = reinterpret_cast<WakeFn>(GetProcAddress(module, "RtlWakeAddressAll"));
		return write && wait && wake;
#elif defined(__linux__)
		return true;
#else
		return false;
#endif
	}
	bool Store(void* out, const void* value, size_t size) const {
		const auto begin = reinterpret_cast<uintptr_t>(out);
		if (!begin || size > UINTPTR_MAX - begin) { return false; }
#if defined(_WIN32)
		for (auto cursor = begin; cursor < begin + size;) {
			MEMORY_BASIC_INFORMATION m {};
			if (!VirtualQuery(reinterpret_cast<void*>(cursor), &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD)) { return false; }
			const auto p = m.Protect & 0xff;
			if (p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) { return false; }
			const auto base = reinterpret_cast<uintptr_t>(m.BaseAddress);
			if (m.RegionSize > UINTPTR_MAX - base || base + m.RegionSize <= cursor) { return false; }
			cursor = base + m.RegionSize;
		}
		SIZE_T copied = 0;
		return write(GetCurrentProcess(), out, const_cast<void*>(value), size, &copied) >= 0 && copied == size;
#elif defined(__linux__)
		iovec local {const_cast<void*>(value), size}, remote {out, size};
		return ::process_vm_writev(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
		return false;
#endif
	}
	ParkResult Park(std::atomic<uint32_t>& epoch, uint32_t expected, uint32_t micros) const {
		if (Fail(EventFlagFailure::Park)) { return ParkResult::Error; }
#if defined(_WIN32)
		LARGE_INTEGER timeout {}; timeout.QuadPart = -static_cast<LONGLONG>(std::max(1u, micros)) * 10;
		const LONG status = wait(&epoch, &expected, sizeof(expected), &timeout);
		return (status == 0 || status == 0x101 || status == 0xc0) ? ParkResult::Wake : status == 0x102 ? ParkResult::Timeout : ParkResult::Error;
#elif defined(__linux__)
		timespec timeout {static_cast<time_t>(micros / 1000000), static_cast<long>(micros % 1000000) * 1000};
		const long result = ::syscall(SYS_futex, &epoch, FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
		if (result == 0 || errno == EAGAIN || errno == EINTR) { return ParkResult::Wake; }
		return errno == ETIMEDOUT ? ParkResult::Timeout : ParkResult::Error;
#else
		return ParkResult::Error;
#endif
	}
	void Wake(std::atomic<uint32_t>& epoch) const {
#if defined(_WIN32)
		wake(&epoch);
#elif defined(__linux__)
		::syscall(SYS_futex, &epoch, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
	}
};

// Preflight is not a mapping lease. A later protect/unmap can still make the
// system copy fail or partially copy across pages; never claim byte atomicity.
bool Writable(const void* pointer, size_t size) {
	const auto begin = reinterpret_cast<uintptr_t>(pointer);
	if (!begin || !size || size > UINTPTR_MAX - begin) { return false; }
	const auto end = begin + size;
#if defined(_WIN32)
	for (auto cursor = begin; cursor < end;) {
		MEMORY_BASIC_INFORMATION m {};
		if (!VirtualQuery(reinterpret_cast<void*>(cursor), &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD)) { return false; }
		const auto p = m.Protect & 0xff;
		if (p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(m.BaseAddress);
		if (m.RegionSize > UINTPTR_MAX - base || base + m.RegionSize <= cursor) { return false; }
		cursor = base + m.RegionSize;
	}
	return true;
#elif defined(__linux__)
	FILE* maps = std::fopen("/proc/self/maps", "r"); if (!maps) { return false; }
	char line[512]; auto cursor = begin; bool continuation = false;
	for (size_t n = 0; n < 65536 && cursor < end && std::fgets(line, sizeof(line), maps); ++n) {
		const bool skip = continuation; continuation = std::strchr(line, '\n') == nullptr;
		if (skip) { continue; }
		unsigned long long lo = 0, hi = 0; char perms[5] {};
		if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3 || hi <= cursor) { continue; }
		if (lo > cursor || perms[1] != 'w') { break; }
		cursor = static_cast<uintptr_t>(hi);
	}
	std::fclose(maps); return cursor >= end;
#else
	return false;
#endif
}
bool Load(const void* source, void* dest, size_t size) {
	const auto address = reinterpret_cast<uintptr_t>(source);
	if (!address || size > UINTPTR_MAX - address) { return false; }
#if defined(_WIN32)
	for (auto cursor = address; cursor < address + size;) {
		MEMORY_BASIC_INFORMATION m {};
		if (!VirtualQuery(reinterpret_cast<void*>(cursor), &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD)) { return false; }
		const DWORD p = m.Protect & 0xff;
		if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(m.BaseAddress);
		if (m.RegionSize > UINTPTR_MAX - base || base + m.RegionSize <= cursor) { return false; }
		cursor = base + m.RegionSize;
	}
	SIZE_T copied = 0; return ReadProcessMemory(GetCurrentProcess(), source, dest, size, &copied) && copied == size;
#elif defined(__linux__)
	iovec local {dest, size}, remote {const_cast<void*>(source), size};
	return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
	return false;
#endif
}
int ReadName(const char* name, std::array<char, 32>& out) {
	const auto address = reinterpret_cast<uintptr_t>(name);
	for (size_t i = 0; i < out.size(); ++i) {
		if (i > UINTPTR_MAX - address || !Load(reinterpret_cast<const void*>(address + i), &out[i], 1)) { return KERNEL_ERROR_EFAULT; }
		if (!out[i]) { return OK; }
	}
	return KERNEL_ERROR_ENAMETOOLONG;
}

struct Flag {
	Flag(std::string n, bool one, bool is_named, uint64_t initial, HostAccess a): name(std::move(n)), single(one), named(is_named), bits(initial), host(a), trace(TraceWanted(name)) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		++g_objects;
#endif
	}
	~Flag() {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		--g_objects;
#endif
	}
	const std::string name;
	const bool single;
	const bool named;
	Common::Mutex mutex;
	uint64_t bits;
	bool deleted = false;
	uint64_t cancel_generation = 0;
	int waiters = 0;
	std::atomic<uint32_t> epoch {0};
	const HostAccess host;
	const bool trace;
	uint64_t trace_bits_seen = 0, trace_last_pattern = 0;
	bool trace_has_last = false;
	void Changed() { epoch.fetch_add(1, std::memory_order_release); host.Wake(epoch); }
};
struct Entry { std::shared_ptr<Flag> flag; bool creator = true; };
Common::Mutex g_registry_mutex;
std::map<uint32_t, Entry> g_entries;
std::map<std::string, uint32_t> g_names;
uint32_t g_next = FirstToken;

std::shared_ptr<Flag> Retain(KernelEventFlag handle) {
	std::shared_ptr<Flag> result;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		const auto it = g_entries.find(Token(handle));
		if (it != g_entries.end()) { result = it->second.flag; }
	}
	if (result) { Hook(EventFlagHookPoint::Retained); }
	return result;
}
struct Enrollment {
	const std::shared_ptr<Flag>& flag;
	bool active = false;
	void Release() noexcept {
		if (!active) { return; }
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(flag->mutex);
		--flag->waiters; active = false; flag->Changed();
	}
	~Enrollment() { Release(); }
};
int Mutate(const std::shared_ptr<Flag>& f, uint64_t value, bool set) {
	bool emit = false; uint64_t now = 0;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(f->mutex);
		if (f->deleted) { return KERNEL_ERROR_EACCES; }
		if (set) { f->bits |= value; } else { f->bits &= value; }
		now = f->bits;
		if (f->trace && set && (f->bits & ~f->trace_bits_seen)) { f->trace_bits_seen |= f->bits; emit = true; }
		if (f->trace && !set && value) { f->trace_bits_seen &= ~value; emit = true; }
		f->Changed();
	}
	if (emit) { Trace(f->name, set ? "set" : "clear", value, now); }
	return OK;
}

// Wait uses address-checked host parking instead of a condvar which would
// reacquire an object mutex without a suspension guard. No host critical guard
// or HLE-owned mutex spans parking, test hooks, or the optional signal window.
int Wait(const std::shared_ptr<Flag>& f, uint64_t pattern, uint32_t mode,
         uint64_t* result, KernelUseconds* timeout, bool poll) {
	const auto condition = mode & 0xf, clear = mode & 0xf0;
	if ((poll && !pattern) || (condition != 1 && condition != 2) || (clear != 0 && clear != 0x10 && clear != 0x20)) { return KERNEL_ERROR_EINVAL; }
	uint32_t budget = UINT32_MAX;
	if (poll) { budget = 0; }
	else if (timeout && (!Load(timeout, &budget, sizeof(budget)) || !Writable(timeout, sizeof(budget)))) { return KERNEL_ERROR_EFAULT; }
	if (result && !Writable(result, sizeof(*result))) { return KERNEL_ERROR_EFAULT; }
	const bool infinite = !poll && !timeout;
	const auto start = std::chrono::steady_clock::now();
	Enrollment enrollment {f};
	bool admitted = false;
	bool trace_checked = false;
	uint64_t generation = 0;
	int pending_error = OK;
	for (;;) {
		uint32_t expected = 0;
		int outcome = OK;
		bool done = false, just_enrolled = false;
		bool write_result = false;
		bool emit_trace = false;
		uint64_t result_value = 0;
		uint32_t remaining = 0;
		uint32_t left = 10000;
		const auto elapsed64 = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0, elapsed64));
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(f->mutex);
			if (!trace_checked && f->trace && (!f->trace_has_last || f->trace_last_pattern != pattern)) {
				f->trace_last_pattern = pattern; f->trace_has_last = true; emit_trace = true;
			}
			trace_checked = true;
			const auto trace_now = f->bits;
			if (emit_trace) { result_value = trace_now; }
			if (f->deleted) { outcome = KERNEL_ERROR_EACCES; done = true; if (admitted) { result_value = f->bits; write_result = true; } }
			else if (admitted && generation != f->cancel_generation) { outcome = KERNEL_ERROR_ECANCELED; done = true; result_value = f->bits; write_result = true; }
			else if (pending_error != OK) { outcome = pending_error; done = true; }
			else if (!enrollment.active && f->single && f->waiters) { outcome = KERNEL_ERROR_EPERM; done = true; }
			else if ((condition == 1 && (f->bits & pattern) == pattern) || (condition == 2 && (f->bits & pattern) != 0)) {
				result_value = f->bits; write_result = true;
				if (clear == 0x10) { f->bits = 0; } else if (clear == 0x20) { f->bits &= ~pattern; }
				done = true;
			} else if (!infinite && elapsed >= budget) {
				result_value = f->bits; write_result = true;
				outcome = poll ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ETIMEDOUT; done = true;
			}
			if (done) {
				if (enrollment.active) { --f->waiters; enrollment.active = false; f->Changed(); }
				remaining = elapsed >= budget ? 0 : budget - static_cast<uint32_t>(elapsed);
			} else {
				if (!enrollment.active) {
					++f->waiters; enrollment.active = true; just_enrolled = true;
					if (!admitted) { admitted = true; generation = f->cancel_generation; }
				}
				expected = f->epoch.load(std::memory_order_acquire);
				if (!infinite) { left = static_cast<uint32_t>(std::min<uint64_t>(10000, budget - elapsed)); }
			}
		}
		if (emit_trace) { Trace(f->name, "wait", pattern, result_value); }
		if (done) {
			if (write_result && result && !f->host.Store(result, &result_value, sizeof(result_value))) { return KERNEL_ERROR_EFAULT; }
			if (timeout && !poll && !f->host.Store(timeout, &remaining, sizeof(remaining))) { return KERNEL_ERROR_EFAULT; }
			return outcome;
		}
		if (just_enrolled && !Hook(EventFlagHookPoint::Enrolled)) { pending_error = KERNEL_ERROR_ENOMEM; continue; }
		const auto parked = f->host.Park(f->epoch, expected, left);
		Hook(EventFlagHookPoint::AfterPark);
		if (parked == HostAccess::ParkResult::Error) { pending_error = KERNEL_ERROR_EIO; continue; }
		if (infinite && parked == HostAccess::ParkResult::Timeout) {
			// Retain logical admission/count across the callback. Delete is
			// non-draining and can be called reentrantly by the signal handler;
			// no object/registry lock or critical guard spans dispatch.
			Hook(EventFlagHookPoint::Dispatch);
			try {
				DispatchTestHook();
				Libs::LibKernel::KernelDispatchPendingSignalForCurrentThread();
			}
			catch (const std::bad_alloc&) {
				// Narrow ABI boundary only. Guest handler effects already performed
				// are not undone. Recheck terminal/cancel state before ENOMEM.
				pending_error = KERNEL_ERROR_ENOMEM;
			}
		}
	}
}
} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* out, const char* name, uint32_t attr, uint64_t initial, const void* opt) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	if (!out || !name || opt) { return KERNEL_ERROR_EINVAL; }
	const auto order = attr & 0xf, mode = attr & 0xf0;
	if ((attr & ~0x133u) != 0 || (order != 0 && order != 1 && order != 2) || (mode != 0 && mode != 0x10 && mode != 0x20)) { return KERNEL_ERROR_EINVAL; }
	const bool named = (attr & 0x100u) != 0;
	HostAccess host;
	if (!host.Init()) { return KERNEL_ERROR_EOPNOTSUPP; }
	std::array<char, 32> safe_name {};
	const int name_error = ReadName(name, safe_name);
	if (name_error != OK) { return name_error; }
	if (ReferenceBootSeedEnabled(safe_name.data())) { initial |= ReferenceBootSeed; }
	try {
		std::string key(safe_name.data());
		// Existing named object errors precede copyout validation; the final
		// publication scope rechecks after preflight to handle concurrent Create.
		if (named) {
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_registry_mutex);
			if (g_names.find(key) != g_names.end()) { return KERNEL_ERROR_EPERM; }
		}
		if (!Writable(out, sizeof(*out))) { return KERNEL_ERROR_EFAULT; }
		auto flag = std::make_shared<Flag>(key, mode != 0x20, named, initial, host);
		std::map<uint32_t, Entry> prepared_entry;
		std::map<std::string, uint32_t> prepared_name;
		prepared_entry.emplace(0, Entry {flag, true});
		if (named) { prepared_name.emplace(key, 0); }
		auto entry_node = prepared_entry.extract(prepared_entry.begin());
		std::map<std::string, uint32_t>::node_type name_node;
		if (named) { name_node = prepared_name.extract(prepared_name.begin()); }
		std::map<uint32_t, Entry>::node_type rollback_entry;
		std::map<std::string, uint32_t>::node_type rollback_name;
		int ret = OK;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_registry_mutex);
			if (named && g_names.find(key) != g_names.end()) { ret = KERNEL_ERROR_EPERM; }
			else if (g_next > LastToken) { ret = KERNEL_ERROR_EAGAIN; }
			else {
				const auto token = g_next++;
				entry_node.key() = token;
				g_entries.insert(std::move(entry_node));
				if (named) { name_node.mapped() = token; g_names.insert(std::move(name_node)); }
				const auto value = Handle(token);
				if (Fail(EventFlagFailure::Store) || !host.Store(out, &value, sizeof(value))) {
					rollback_entry = g_entries.extract(token);
					if (named) { rollback_name = g_names.extract(key); }
					ret = KERNEL_ERROR_EFAULT;
				}
			}
		}
		if (ret == OK) {
			// The result is already published. Diagnostic allocation failure must
			// never turn that success into a failed transactional Create.
			Diagnostic([&] {
				LOGF("\tEventFlag create: %s\n", key.c_str());
			});
		}
		return ret;
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag handle) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	std::shared_ptr<Flag> flag;
	std::map<uint32_t, Entry>::node_type removed;
	std::map<std::string, uint32_t>::node_type removed_name;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		const auto it = g_entries.find(Token(handle));
		if (it == g_entries.end()) { return KERNEL_ERROR_ESRCH; }
		if (!it->second.creator) { return KERNEL_ERROR_EPERM; }
		flag = it->second.flag;
		{
			// Global order: registry -> object. Retained operations only hold the
			// object lock; they never acquire the registry while holding it.
			Common::HostLockCriticalGuard object_critical;
			Common::LockGuard object_lock(flag->mutex);
			flag->deleted = true;
			flag->epoch.fetch_add(1, std::memory_order_release);
		}
		if (flag->named) {
			const auto named = g_names.find(flag->name);
			if (named != g_names.end() && named->second == it->first) { removed_name = g_names.extract(named); }
		}
		removed = g_entries.extract(it);
	}
	flag->host.Wake(flag->epoch);
	Hook(EventFlagHookPoint::Retired);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelOpenEventFlag(KernelEventFlag* out, const char* name) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	if (!out || !name) { return KERNEL_ERROR_EINVAL; }
	HostAccess host; if (!host.Init()) { return KERNEL_ERROR_EOPNOTSUPP; }
	std::array<char, 32> safe_name {};
	const int name_error = ReadName(name, safe_name);
	if (name_error != OK) { return name_error; }
	try {
		const std::string key(safe_name.data());
		bool missing = false;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_registry_mutex);
			missing = g_names.find(key) == g_names.end();
		}
		if (missing) {
			if (!ReferenceBootSeedEnabled(safe_name.data())) { return KERNEL_ERROR_ESRCH; }
			if (!Writable(out, sizeof(*out))) { return KERNEL_ERROR_EFAULT; }
			// Keep a process-lifetime creator in the registry; callers still get
			// ordinary distinct aliases. Never hold the registry lock across Create.
			KernelEventFlag creator = nullptr;
			const int created = KernelCreateEventFlag(&creator, safe_name.data(), 0x121, ReferenceBootSeed, nullptr);
			if (created != OK && created != KERNEL_ERROR_EPERM) { return created; }
			if (created == OK) {
				Diagnostic([&] { LOGF("reference boot experiment: SceBootStatusFlags seed=0x2ce (synthetic)\n"); });
			}
		}
		if (!Writable(out, sizeof(*out))) { return KERNEL_ERROR_EFAULT; }
		// Allocate the alias node outside the lock. During publication/rollback,
		// the extracted node holds ownership until after the table is unlocked.
		std::map<uint32_t, Entry> prepared;
		prepared.emplace(0, Entry {{}, false});
		auto node = prepared.extract(prepared.begin());
		std::map<uint32_t, Entry>::node_type rollback;
		int result = OK;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_registry_mutex);
			const auto named = g_names.find(key);
			if (named == g_names.end()) { result = KERNEL_ERROR_ESRCH; }
			else {
				const auto creator = g_entries.find(named->second);
				if (creator == g_entries.end() || !creator->second.creator) { result = KERNEL_ERROR_ESRCH; }
				else if (g_next > LastToken) { result = KERNEL_ERROR_EAGAIN; }
				else {
					const auto token = g_next++;
					node.key() = token; node.mapped().flag = creator->second.flag;
					g_entries.insert(std::move(node));
					const auto value = Handle(token);
					if (Fail(EventFlagFailure::Store) || !host.Store(out, &value, sizeof(value))) {
						rollback = g_entries.extract(token); result = KERNEL_ERROR_EFAULT;
					}
				}
			}
		}
		return result;
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCloseEventFlag(KernelEventFlag handle) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	std::map<uint32_t, Entry>::node_type removed;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_registry_mutex);
		const auto it = g_entries.find(Token(handle));
		if (it == g_entries.end()) { return KERNEL_ERROR_ESRCH; }
		if (it->second.creator) { return KERNEL_ERROR_EPERM; }
		removed = g_entries.extract(it);
	}
	return OK;
}

int SetEventFlagByName(const char* name, uint64_t bits) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	if (!name) { return KERNEL_ERROR_EINVAL; }
	std::array<char, 32> safe_name {};
	const int name_error = ReadName(name, safe_name); if (name_error != OK) { return name_error; }
	try {
		const std::string key(safe_name.data());
		std::shared_ptr<Flag> flag;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_registry_mutex);
			const auto named = g_names.find(key);
			if (named != g_names.end()) { const auto it = g_entries.find(named->second); if (it != g_entries.end()) { flag = it->second.flag; } }
		}
		if (!flag) { return KERNEL_ERROR_ESRCH; }
		Hook(EventFlagHookPoint::Retained);
		return Mutate(flag, bits, true);
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag h, uint64_t bits, uint32_t mode, uint64_t* result, KernelUseconds* timeout) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	g_trace_caller = KYTY_SPP_CALLER;
	auto f = Retain(h); if (!f) { return KERNEL_ERROR_ESRCH; }
	static std::atomic<uint64_t> census {0};
	const auto n = census.fetch_add(1, std::memory_order_relaxed);
	if (n % 64 == 0) { Diagnostic([&] { LOGF("ps5-vsh: wait-eventflag #%llu ef=%s pattern=%016" PRIx64 " mode=0x%08x ra=0x%016" PRIx64 "\n", static_cast<unsigned long long>(n), f->name.c_str(), bits, mode, g_trace_caller); }); }
	HLE_SLOW_GUARD("event-flag"); // static lifetime for asynchronous watchdog
	return Wait(f, bits, mode, result, timeout, false);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag h, uint64_t bits, uint32_t mode, uint64_t* result) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	g_trace_caller = KYTY_SPP_CALLER;
	auto f = Retain(h); if (!f) { return KERNEL_ERROR_ESRCH; }
	static std::atomic<uint64_t> census {0};
	const auto n = census.fetch_add(1, std::memory_order_relaxed);
	if (n % 64 == 0) { Diagnostic([&] { LOGF("ps5-vsh: poll-eventflag #%llu ef=%s pattern=%016" PRIx64 " mode=0x%08x ra=0x%016" PRIx64 "\n", static_cast<unsigned long long>(n), f->name.c_str(), bits, mode, g_trace_caller); }); }
	const int ret = Wait(f, bits, mode, result, nullptr, true);
	return ret == KERNEL_ERROR_EACCES || ret == KERNEL_ERROR_ECANCELED ? KERNEL_ERROR_EBUSY : ret;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag h, uint64_t bits) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	g_trace_caller = KYTY_SPP_CALLER;
	auto f = Retain(h); if (!f) { return KERNEL_ERROR_ESRCH; }
	const int result = Mutate(f, bits, true);
	if (result == 0 && f->name == "SceBootStatusFlags") { Diagnostic([&] { LOGF("ps5-vsh: bootflag set %016" PRIx64 " ra=0x%016" PRIx64 "\n", bits, g_trace_caller); }); }
	return result;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag h, uint64_t bits) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	g_trace_caller = KYTY_SPP_CALLER;
	auto f = Retain(h); return f ? Mutate(f, bits, false) : KERNEL_ERROR_ESRCH;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag h, uint64_t bits, int* count) {
	if (!PrepareThread()) { return KERNEL_ERROR_ENOMEM; }
	auto f = Retain(h); if (!f) { return KERNEL_ERROR_ESRCH; }
	if (count && !Writable(count, sizeof(*count))) { return KERNEL_ERROR_EFAULT; }
	int waiting = 0;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(f->mutex);
		if (f->deleted) { return KERNEL_ERROR_EACCES; }
		waiting = f->waiters;
		f->bits = bits; ++f->cancel_generation; f->Changed();
	}
	if (count && !f->host.Store(count, &waiting, sizeof(waiting))) { return KERNEL_ERROR_EFAULT; }
	Hook(EventFlagHookPoint::Cancelled);
	return OK;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void EventFlagHookForTest(EventFlagHookPoint point, void (*hook)(void*), void* context) {
	g_hook_point = point; g_hook = hook; g_hook_context = context;
}
void EventFlagFailForTest(EventFlagFailure point) { g_failure = point; }
EventFlagFailure EventFlagFailurePendingForTest() { return g_failure; }
void EventFlagDispatchForTest(void (*hook)(void*), void* context) { g_dispatch_hook = hook; g_dispatch_context = context; }
int EventFlagObjectsForTest() { return g_objects.load(); }
int EventFlagWaitersForTest(KernelEventFlag handle) {
	if (!PrepareThread()) { return -1; }
	auto f = Retain(handle); if (!f) { return -1; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(f->mutex);
	return f->waiters;
}
#endif
} // namespace Libs::LibKernel::EventFlag
