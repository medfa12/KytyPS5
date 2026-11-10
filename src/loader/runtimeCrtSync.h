// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_RUNTIME_CRT_SYNC_H_
#define KYTY_LOADER_RUNTIME_CRT_SYNC_H_

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

// Header-only so concurrent emulator builds do not need a half-integrated new
// link unit. Only the pthread-once/libc CRT wrappers use this registry.
#include "loader/runtimeLinker.h"
#include "loader/elf.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Loader::RuntimeCrt {
struct Context {
	Program* importer = RuntimeLinker::GetCurrentProgram();
	Program* owner = importer ? importer->application_owner : nullptr;
	std::shared_ptr<RuntimeGuestExecution> execution = GetApplicationExecution(importer);
	bool Shared() const { return owner != nullptr; }
	GuestCallOwner Lifetime() const { return execution ? execution->Owner() : GuestCallOwner {}; }
	bool Canceled() const { return Shared() && (!execution || execution->StopRequested() || execution->PendingThreadExit()); }
	bool Reject(uint64_t detail = 0) const {
		if (execution && !execution->StopRequested() && !execution->PendingThreadExit()) {
			execution->Fail(GuestFailureReason::InvalidTransition, detail);
		}
		return false;
	}
	bool Ready() const {
		if (!Shared()) { return true; }
		if (Canceled()) { return false; }
		return (importer->rt && owner->rt == importer->rt && owner->application_owner == owner &&
		    execution->Owner().identity == owner && execution->HasHostCallForCurrentBoundary()) || Reject();
	}
};

// No generic readable-host-pointer admission. B0 owns arbitrary dynamic heap
// authority and hostile concurrent unmap/protect; these spans are loader/stack
// pinned, exactly as in the audited bootstrap contract.
inline bool WritableSpan(const Context& context, void* pointer, uint64_t size) {
	const auto address = reinterpret_cast<uint64_t>(pointer);
	if (!address || !size || size > UINT64_MAX - address) { return context.Reject(address); }
	bool owned = !context.Shared();
	if (!owned && context.importer && context.importer->rt) {
		for (const auto* program: context.importer->rt->GetVisiblePrograms(context.importer)) {
			if (program->application_owner != context.owner || !program->elf || !program->elf->IsValid() ||
			    address < program->base_vaddr || address - program->base_vaddr >= program->base_size) { continue; }
			const auto offset = address - program->base_vaddr;
			const auto* phdr = program->elf->GetPhdr();
			for (unsigned i = 0; i != program->elf->GetEhdr()->e_phnum; ++i) {
				const auto& segment = phdr[i];
				if ((segment.p_type == PT_LOAD || segment.p_type == PT_OS_RELRO) &&
				    (segment.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W) &&
				    segment.p_vaddr <= program->base_size && segment.p_memsz <= program->base_size - segment.p_vaddr &&
				    offset >= segment.p_vaddr && offset - segment.p_vaddr < segment.p_memsz &&
				    size <= segment.p_memsz - (offset - segment.p_vaddr)) { owned = true; break; }
			}
			if (owned) { break; }
		}
		uint64_t stack = 0, stack_size = 0;
		const auto self = Libs::LibKernel::PthreadSelfOrNull();
		if (!owned && self && GetApplicationExecution(Libs::LibKernel::PthreadGetOwnerProgram(self)) == context.execution &&
		    Libs::LibKernel::PthreadGetGuestStack(self, &stack, &stack_size) && address >= stack &&
		    address - stack < stack_size && size <= stack_size - (address - stack)) { owned = true; }
	}
	if (!owned || !Libs::Graphics::HostMemoryRangeIsReadable(address, size)) { return context.Reject(address); }
	if (context.Shared()) {
		for (auto cursor = address; cursor < address + size;) {
			void* begin = nullptr; void* end = nullptr; int prot = 0;
			if (Libs::LibKernel::Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(cursor), &begin, &end, &prot) ||
			    (prot & 7) != 3 || reinterpret_cast<uint64_t>(begin) > cursor || reinterpret_cast<uint64_t>(end) <= cursor) {
				return context.Reject(address);
			}
			cursor = std::min(address + size, reinterpret_cast<uint64_t>(end));
		}
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	for (auto cursor = address; cursor < address + size;) {
		MEMORY_BASIC_INFORMATION region {};
		if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) || region.State != MEM_COMMIT ||
		    (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return context.Reject(address); }
		const auto prot = region.Protect & 0xff;
		if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY && prot != PAGE_EXECUTE_READWRITE &&
		    prot != PAGE_EXECUTE_WRITECOPY) { return context.Reject(address); }
		const auto base = reinterpret_cast<uint64_t>(region.BaseAddress);
		if (base > cursor || region.RegionSize > UINT64_MAX - base || base + region.RegionSize <= cursor) { return context.Reject(address); }
		cursor = std::min<uint64_t>(address + size, base + region.RegionSize);
	}
#endif
	return true;
}
inline bool PrivateCallback(const Context& context, uint64_t target) {
	if (!target) { return context.Reject(); }
	if (!context.Shared()) { return true; }
	void* begin = nullptr; void* end = nullptr; int prot = 0;
	if (!context.importer->rt->IsApplicationExecutableAddress(context.owner, target) ||
	    Libs::LibKernel::Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(target), &begin, &end, &prot) != 0 ||
	    !(prot & 4) || reinterpret_cast<uint64_t>(begin) > target || reinterpret_cast<uint64_t>(end) <= target) { return context.Reject(target); }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION region {};
	if (!VirtualQuery(reinterpret_cast<void*>(target), &region, sizeof(region)) || region.State != MEM_COMMIT ||
	    (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return context.Reject(target); }
	const auto protection = region.Protect & 0xff;
	if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
	    protection != PAGE_EXECUTE_WRITECOPY) { return context.Reject(target); }
#endif
	return true;
}

// A weak identity, not a recycled std::thread::id or emulated Pthread handle.
// A constructor abandoned by actual native thread exit becomes retryable when
// that thread's TLS dies. No guest memory/callback is touched by TLS destruction.
using ThreadIdentity = std::shared_ptr<const unsigned char>;
inline const ThreadIdentity& ActualThread() {
	thread_local const ThreadIdentity identity = std::make_shared<const unsigned char>(0);
	return identity;
}
enum class SyncKind { PthreadOnce, StdOnce, Guard };
struct SyncKey {
	uint64_t address;
	SyncKind kind;
	bool operator<(const SyncKey& other) const { return address < other.address || (address == other.address && kind < other.kind); }
};
struct SyncRecord {
	GuestCallOwner owner {};
	bool running = false;
	std::weak_ptr<const unsigned char> thread;
	unsigned waiters = 0;
};
struct SyncRegistry {
	GuestExecutionMutex mutex;
	std::condition_variable_any changed;
	std::map<SyncKey, SyncRecord> records;
};
inline SyncRegistry& Synchronization() {
	// Deliberately lifetime-pinned. B0 owns final owner/mapping reclamation; no
	// stale address is silently reassigned to another owner or a session caller.
	static auto* registry = new SyncRegistry;
	return *registry;
}
inline bool SameOwner(GuestCallOwner a, GuestCallOwner b) { return a.identity == b.identity && a.token == b.token; }
inline bool BindRecord(const Context& context, SyncRecord& record, bool inserted) {
	if (inserted) { record.owner = context.Lifetime(); }
	return SameOwner(record.owner, context.Lifetime()) || context.Reject();
}
inline int ReadState(void* control) { int state = 0; std::memcpy(&state, control, sizeof(state)); return state; }
inline void WriteState(void* control, int state) { std::memcpy(control, &state, sizeof(state)); }
using StdOnceFunction = int(KYTY_SYSV_ABI*)(void*, void*, void**);
struct StdOnceArguments { uint64_t target; void* argument; void** output; };
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
static_assert(offsetof(StdOnceArguments, output) == 16);
__attribute__((naked)) inline uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV StdOnceTransition(void*) {
	__asm__ volatile("movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "xorq %rdi, %rdi\n\t"
	                 "movq 8(%r10), %rsi\n\t"
	                 "movq 16(%r10), %rdx\n\t"
	                 "jmp *%rax\n\t");
}
#endif
inline int Once(void* control, uint64_t target, SyncKind kind, void* argument = nullptr,
                void (*session_poll)() = nullptr) try {
	using namespace Libs::Posix;
	const bool posix = kind == SyncKind::PthreadOnce;
	const Context context;
	if (!context.Ready()) { return posix ? POSIX_ECANCELED : 0; }
	if (!WritableSpan(context, control, posix ? 16 : 4) || !PrivateCallback(context, target)) { return posix ? POSIX_EINVAL : 0; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::Callback) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return posix ? POSIX_ECANCELED : 0; }
	const auto thread = ActualThread(); // May allocate TLS; never do that inside Publish.
	auto& registry = Synchronization();
	const SyncKey key {reinterpret_cast<uint64_t>(control), kind};
	{
		std::unique_lock lock(registry.mutex);
		auto [it, inserted] = registry.records.try_emplace(key);
		auto& record = it->second;
		if (!BindRecord(context, record, inserted)) { return posix ? POSIX_EINVAL : 0; }
		for (;;) {
			if (context.Canceled()) { return posix ? POSIX_ECANCELED : 0; }
			if (record.running && record.thread.expired()) { record.running = false; record.thread.reset(); WriteState(control, 0); }
			if (!record.running) {
				const int state = ReadState(control);
				if (state == 1) { return posix ? 0 : 1; }
				if (state != 0) { context.Reject(key.address); return posix ? POSIX_EINVAL : 0; }
				const auto claim = [&] { record.running = true; record.thread = thread; WriteState(control, 2); };
				if (context.Shared() ? !lease.Publish(claim) : (claim(), false)) { return posix ? POSIX_ECANCELED : 0; }
				break;
			}
			if (record.thread.lock() == thread) { context.Reject(key.address); return posix ? POSIX_EDEADLK : 0; }
			++record.waiters;
			registry.changed.wait_for(lock, std::chrono::milliseconds(1));
			--record.waiters;
			// Shared waiters NEVER dispatch signals, especially after terminal stop.
			if (!context.Shared() && session_poll) { lock.unlock(); session_poll(); lock.lock(); }
		}
	}
	bool completed = false;
	int value = 0;
	if (!context.Shared()) {
		if (posix) { reinterpret_cast<void(KYTY_SYSV_ABI*)()>(target)(); value = 1; }
		else { void* output = nullptr; value = reinterpret_cast<StdOnceFunction>(target)(nullptr, argument, &output); }
		completed = value != 0;
	} else if (posix) {
		completed = context.execution->InvokeCallback(target).Returned(); value = completed ? 1 : 0;
	} else {
		void* output = nullptr;
		StdOnceArguments arguments {target, argument, &output};
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
		const auto result = context.execution->Invoke(StdOnceTransition, &arguments);
		completed = result.Returned(); value = completed ? static_cast<int32_t>(result.call.value) : 0;
#else
		context.execution->Fail(GuestFailureReason::BoundaryUnavailable);
#endif
	}
	{
		std::lock_guard lock(registry.mutex);
		auto& record = registry.records.at(key);
		const auto done = [&] { WriteState(control, 1); };
		if (!completed || !value || (context.Shared() && !lease.Publish(done))) { WriteState(control, 0); }
		else if (!context.Shared()) { done(); }
		record.running = false; record.thread.reset();
	}
	registry.changed.notify_all();
	return posix ? (completed && !context.Canceled() ? 0 : POSIX_ECANCELED) : (!context.Canceled() ? value : 0);
} catch (...) {
	Context {}.Reject();
	return kind == SyncKind::PthreadOnce ? Libs::Posix::POSIX_ECANCELED : 0;
}

inline int GuardAcquire(uint64_t* guard) try {
	const Context context;
	if (!context.Ready() || !WritableSpan(context, guard, 8)) { return 0; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::HostCall) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return 0; }
	const auto thread = ActualThread();
	auto& registry = Synchronization();
	std::unique_lock lock(registry.mutex);
	const SyncKey key {reinterpret_cast<uint64_t>(guard), SyncKind::Guard};
	auto [it, inserted] = registry.records.try_emplace(key);
	auto& record = it->second;
	if (!BindRecord(context, record, inserted)) { return 0; }
	for (;;) {
		if (context.Canceled()) { return 0; }
		if (record.running && record.thread.expired()) { record.running = false; record.thread.reset(); }
		if (!record.running) {
			if (std::atomic_ref<uint8_t>(*reinterpret_cast<uint8_t*>(guard)).load(std::memory_order_acquire)) { return 0; }
			const auto claim = [&] { record.running = true; record.thread = thread; };
			if (context.Shared() ? !lease.Publish(claim) : (claim(), false)) { return 0; }
			return 1;
		}
		if (record.thread.lock() == thread) { context.Reject(key.address); return 0; }
		++record.waiters;
		registry.changed.wait_for(lock, std::chrono::milliseconds(1));
		--record.waiters;
	}
} catch (...) { Context {}.Reject(); return 0; }

// HOST WORKER ROOT ONLY, once its final guest invocation returned, BEFORE guest
// thread-cleanup callbacks or physical-thread reuse. Native TLS death is also a
// fallback, but is too late for cleanup that reenters an abandoned guard.
inline void AbandonCurrentThreadGuards(const std::shared_ptr<RuntimeGuestExecution>& execution) {
	if (!execution || GuestCallBoundary::CurrentDepth() || HostCallReturnThunk::CurrentDepth()) { return; }
	auto& registry = Synchronization(); std::lock_guard lock(registry.mutex);
	for (auto& [key, record]: registry.records) {
		if (key.kind == SyncKind::Guard && SameOwner(record.owner, execution->Owner()) && record.thread.lock() == ActualThread()) {
			record.running = false; record.thread.reset(); // Never touches guest memory.
		}
	}
	registry.changed.notify_all();
}
inline void GuardFinish(uint64_t* guard, bool completed) try {
	const Context context;
	if (!context.Ready() || !WritableSpan(context, guard, 8)) { return; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::HostCall) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return; }
	auto& registry = Synchronization();
	{
		std::lock_guard lock(registry.mutex);
		const auto it = registry.records.find({reinterpret_cast<uint64_t>(guard), SyncKind::Guard});
		if (it == registry.records.end() || !SameOwner(it->second.owner, context.Lifetime()) ||
		    !it->second.running || it->second.thread.lock() != ActualThread()) { context.Reject(); return; }
		const auto publish = [&] {
			if (completed) { std::atomic_ref<uint8_t>(*reinterpret_cast<uint8_t*>(guard)).store(1, std::memory_order_release); }
		};
		if (context.Shared()) { lease.Publish(publish); } else { publish(); }
		it->second.running = false; it->second.thread.reset();
	}
	registry.changed.notify_all();
} catch (...) { Context {}.Reject(); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
inline unsigned SyncWaiters(void* control, SyncKind kind) {
	auto& registry = Synchronization(); std::lock_guard lock(registry.mutex);
	const auto it = registry.records.find({reinterpret_cast<uint64_t>(control), kind});
	return it == registry.records.end() ? 0 : it->second.waiters;
}
#endif
} // namespace Loader::RuntimeCrt
#endif
