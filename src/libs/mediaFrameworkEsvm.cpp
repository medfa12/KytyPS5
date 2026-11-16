#include "libs/mediaFrameworkEsvm.h"
#include "libs/mediaFrameworkEsvmKeys.h"
#include "libs/mediaFrameworkEsvmSource.h"

#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/semaphore.h"
#include "loader/runtimeProcessIdentity.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::MediaFramework {
namespace {
constexpr int Invalid = static_cast<int>(0x81790001u);
constexpr int Full = static_cast<int>(0x81790007u);
constexpr int NotInitialized = static_cast<int>(0x8179000du);
constexpr int Failed = static_cast<int>(0x81790fffu);
constexpr size_t SharedBytes = 0x30d0;
constexpr size_t ClientLimit = 10;
// HLE bookkeeping bound, not a claim about a native per-process init limit.
constexpr size_t OwnerLimit = 64;
namespace Fs = LibKernel::FileSystem;
namespace Sem = LibKernel::Semaphore;

bool Read(void* destination, const void* source, size_t size) {
	if (source == nullptr || destination == nullptr) { return false; }
#if defined(_WIN32)
	SIZE_T copied = 0;
	return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) && copied == size;
#elif defined(__linux__)
	iovec local {destination, size}, remote {const_cast<void*>(source), size};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
	return false;
#endif
}

bool Writable(void* destination, size_t size) {
	const auto begin = reinterpret_cast<uintptr_t>(destination);
	if (begin == 0 || size > UINTPTR_MAX - begin) { return false; }
	const auto end = begin + size;
#if defined(_WIN32)
	for (auto cursor = begin; cursor < end;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) { return false; }
		const DWORD mode = info.Protect & 0xff;
		if (mode != PAGE_READWRITE && mode != PAGE_WRITECOPY && mode != PAGE_EXECUTE_READWRITE &&
		    mode != PAGE_EXECUTE_WRITECOPY) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
		if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= cursor) { return false; }
		cursor = base + info.RegionSize;
	}
	return true;
#elif defined(__linux__)
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	char line[512];
	auto cursor = begin;
	while (cursor < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		unsigned long long lo = 0, hi = 0;
		char protection[5] {};
		if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, protection) == 3 && lo <= cursor && cursor < hi) {
			if (protection[1] != 'w') { break; }
			cursor = static_cast<uintptr_t>(hi);
		}
	}
	std::fclose(maps);
	return cursor >= end;
#else
	return false;
#endif
}

bool Store(void* destination, const void* source, size_t size) {
#if defined(_WIN32)
	SIZE_T copied = 0;
	return WriteProcessMemory(GetCurrentProcess(), destination, source, size, &copied) && copied == size;
#elif defined(__linux__)
	iovec local {const_cast<void*>(source), size}, remote {destination, size};
	return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
	return false;
#endif
}

// Byte-wise bounded copy never reads beyond the first NUL (guard-page names).
bool Name(const char* source, std::array<char, 64>* name) {
	for (size_t index = 0; index < name->size(); ++index) {
		const auto address = reinterpret_cast<uintptr_t>(source);
		if (address == 0 || index > UINTPTR_MAX - address ||
		    !Read(&(*name)[index], reinterpret_cast<const void*>(address + index), 1)) { return false; }
		if ((*name)[index] == '\0') { return true; }
	}
	return false;
}

#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
enum class EsvmResourceFailure { None, ShmOpen, ShmResize, ShmMap, FirstSemaphore, SecondSemaphore, ReceiverSetup };
#endif
thread_local EsvmResourceFailure failure = EsvmResourceFailure::None;
bool Fail(EsvmResourceFailure point) {
	if (failure != point) { return false; }
	failure = EsvmResourceFailure::None;
	return true;
}

struct Resources {
	int descriptor = -1;
	void* mapping = nullptr;
	Sem::KernelSema request = Sem::InvalidKernelSema;
	Sem::KernelSema response = Sem::InvalidKernelSema;
	bool complete = false;

	// Every acquired object remains here until its individual release succeeds.
	// A failed cleanup is retained by its session and consumes capacity on retry.
	bool Reset() {
		complete = false;
		if (response != Sem::InvalidKernelSema && Sem::KernelDeleteSema(response) == 0) { response = Sem::InvalidKernelSema; }
		if (request != Sem::InvalidKernelSema && Sem::KernelDeleteSema(request) == 0) { request = Sem::InvalidKernelSema; }
		if (mapping != nullptr && LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(mapping), SharedBytes) == 0) {
			mapping = nullptr;
		}
		if (descriptor >= 0 && Fs::KernelClose(descriptor) == 0) { descriptor = -1; }
		return response == Sem::InvalidKernelSema && request == Sem::InvalidKernelSema && mapping == nullptr && descriptor < 0;
	}
	bool Create(int pid, uint32_t generation) {
		if (Fail(EsvmResourceFailure::ShmOpen)) { return false; }
		// An anonymous shared-memory object replaces the private IPC named object.
		// No /data file, O_SYSTEM budget fiction, host pathname or global alias.
		descriptor = Fs::KernelShmOpenAnonymous(2, 0666);
		if (descriptor < 0) { return false; }
		if (Fail(EsvmResourceFailure::ShmResize) || Fs::KernelFtruncate(descriptor, SharedBytes) != 0) { return false; }
		if (Fail(EsvmResourceFailure::ShmMap) || Fs::KernelMmap(nullptr, SharedBytes, 3, 1, descriptor, 0, &mapping) != 0) {
			return false;
		}
		if (Fs::KernelClose(descriptor) != 0) { return false; }
		descriptor = -1; // Mapping retains the real SHM backing, as on firmware.
		char name[32];
		std::snprintf(name, sizeof(name), "MfEsvmA%08x%08x", static_cast<unsigned>(pid), generation);
		if (Fail(EsvmResourceFailure::FirstSemaphore) || Sem::KernelCreateSema(&request, name, 0x101, 1, 1, nullptr) != 0) {
			return false;
		}
		std::snprintf(name, sizeof(name), "MfEsvmB%08x%08x", static_cast<unsigned>(pid), generation);
		if (Fail(EsvmResourceFailure::SecondSemaphore) || Sem::KernelCreateSema(&response, name, 0x101, 1, 1, nullptr) != 0) {
			return false;
		}
		// HLE receiver admission is synchronous. Exercise the actual shared slot
		// and both synchronization objects before publishing client admission.
		if (Fail(EsvmResourceFailure::ReceiverSetup) || Sem::KernelPollSema(request, 1) != 0) { return false; }
		const int polled = Sem::KernelPollSema(response, 1);
		if (polled == 0) {
			std::memset(mapping, 0, SharedBytes);
			if (Sem::KernelSignalSema(response, 1) != 0) { return false; }
		}
		if (Sem::KernelSignalSema(request, 1) != 0 || polled != 0) { return false; }
		complete = true;
		return true;
	}
};

struct Session {
	std::mutex mutex;
	std::shared_ptr<std::recursive_mutex> dispatch = std::make_shared<std::recursive_mutex>();
	int pid;
	bool initialized = false;
	uint64_t epoch = 0;
	EsvmInit24 config {};
	std::unique_ptr<Resources> resources;
	explicit Session(int value): pid(value) {}
};
struct Registry {
	std::mutex mutex;
	bool stopping = false;
	std::array<std::shared_ptr<Session>, OwnerLimit> owners {};
	uint32_t generation = 0;
	uint32_t next_handle = 0x10000;
	uint64_t reader_epoch = 0;
	EsvmSnapshotReader reader = ReadEsvmGraphicsSnapshot;
};
Registry& Service() {
	// Kernel subsystem lifetime is explicit, not C++ static destructor ordering.
	static auto* registry = new Registry;
	return *registry;
}
bool Clear(Session& session) {
	++session.epoch;
	InvalidateEsvmKeysOwner(session.pid); // Nonblocking; guest callbacks never run here.
	InvalidateEsvmSourcesOwner(session.pid);
	if (session.resources != nullptr) {
		if (!session.resources->Reset()) { return false; }
		session.resources.reset();
	}
	return true;
}
// Registry -> session is the only lock order. Retirement must not hold the
// process-identity lock when calling ReleaseEsvmOwner.
void Reap(Registry& registry) {
	for (auto& owner: registry.owners) {
		if (owner != nullptr && !Loader::IsGuestProcessIdentityActive(owner->pid)) {
			auto retained = owner;
			std::lock_guard lock(retained->mutex);
			retained->initialized = false;
			if (Clear(*retained)) { owner.reset(); }
		}
	}
}
std::shared_ptr<Session> Find(Registry& registry, int pid) {
	for (const auto& owner: registry.owners) { if (owner != nullptr && owner->pid == pid) { return owner; } }
	return nullptr;
}
size_t Clients(const Registry& registry) {
	return std::count_if(registry.owners.begin(), registry.owners.end(), [](const auto& owner) {
		return owner != nullptr && owner->resources != nullptr;
	});
}
struct KeysDrainOnExit {
	~KeysDrainOnExit() { DrainRetiredEsvmServices(); }
};
thread_local size_t dispatch_depth = 0;
} // namespace

EsvmDispatchLock::EsvmDispatchLock(std::recursive_mutex& mutex, bool wait): gate(mutex, std::defer_lock) {
	if (wait) { gate.lock(); } else { gate.try_lock(); }
	if (gate.owns_lock()) { ++dispatch_depth; }
}
EsvmDispatchLock::~EsvmDispatchLock() { if (gate.owns_lock()) { unlock(); } }
void EsvmDispatchLock::lock() { gate.lock(); ++dispatch_depth; }
void EsvmDispatchLock::unlock() { gate.unlock(); --dispatch_depth; }
bool EsvmDispatchScopeActive() { return dispatch_depth != 0; }
bool DrainRetiredEsvmServices(bool wait) {
	wait = wait && !EsvmDispatchScopeActive();
	DrainRetiredEsvmKeys(wait);
	return DrainRetiredEsvmSources(wait);
}

bool EsvmReadGuest(void* out, const void* in, size_t bytes) {
	const auto address = reinterpret_cast<uintptr_t>(in);
	return address != 0 && bytes <= UINTPTR_MAX - address && Read(out, in, bytes);
}
bool EsvmWritableGuest(void* out, size_t bytes) { return Writable(out, bytes); }
bool EsvmStoreGuest(void* out, const void* in, size_t bytes) { return Writable(out, bytes) && Store(out, in, bytes); }
int AllocateEsvmServiceHandle() {
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	return registry.next_handle < INT32_MAX ? static_cast<int>(registry.next_handle++) : 0;
}
bool IsEsvmServiceHandle(int handle) {
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	return handle >= 0x10000 && static_cast<uint32_t>(handle) < registry.next_handle;
}

int GetEsvmClientLease(int pid, uint64_t* epoch, std::shared_ptr<std::recursive_mutex>* dispatch) {
	if (pid <= 0 || epoch == nullptr) { return Failed; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	if (registry.stopping || !Loader::IsGuestProcessIdentityActive(pid)) { return Failed; }
	auto owner = Find(registry, pid);
	if (owner == nullptr) { return NotInitialized; }
	std::lock_guard session_lock(owner->mutex);
	if (!owner->initialized) { return NotInitialized; }
	if (owner->resources == nullptr || !owner->resources->complete) { return Failed; }
	*epoch = owner->epoch;
	if (dispatch != nullptr) { *dispatch = owner->dispatch; }
	return 0;
}

int GetEsvmClientEpoch(int pid, uint64_t* epoch) {
	return GetEsvmClientLease(pid, epoch, nullptr);
}

void SetEsvmSnapshotReader(EsvmSnapshotReader reader) {
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	registry.reader = reader;
	++registry.reader_epoch;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EsvmInitialize(const EsvmInit24* config, bool reserved_false) {
	KeysDrainOnExit keys_drain; // Declared before framework locks: drain after unlock.
	EsvmInit24 copied {};
	if (config == nullptr || reserved_false || !Read(&copied, config, sizeof(copied))) { return Invalid; }
	const int pid = Loader::GetCurrentGuestProcessId();
	if (pid <= 0) { return Failed; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	Reap(registry);
	if (registry.stopping) { return Failed; }
	if (!Loader::IsGuestProcessIdentityActive(pid)) { return Failed; }
	auto owner = Find(registry, pid);
	try {
		if (owner == nullptr) {
			const auto slot = std::find(registry.owners.begin(), registry.owners.end(), nullptr);
			if (slot == registry.owners.end()) { return Failed; }
			owner = std::make_shared<Session>(pid);
			*slot = owner;
		}
		std::lock_guard session_lock(owner->mutex);
		if (!owner->initialized && !Clear(*owner)) { return Failed; }
		owner->config = copied; // No size/reserved field validation in this ABI.
		owner->initialized = true;
		return 0;
	} catch (const std::bad_alloc&) { return Failed; }
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EsvmRegisterClient(const char* name) {
	KeysDrainOnExit keys_drain;
	if (name == nullptr) { return Invalid; }
	const int pid = Loader::GetCurrentGuestProcessId();
	if (pid <= 0) { return Failed; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	Reap(registry);
	auto owner = Find(registry, pid);
	if (owner == nullptr) { return NotInitialized; }
	std::lock_guard session_lock(owner->mutex);
	if (!owner->initialized) { return NotInitialized; }
	std::array<char, 64> copied {};
	if (!Name(name, &copied) || std::strcmp(copied.data(), "citroncore") != 0) { return Invalid; }
	// Firmware replacement destroys the prior PID slot BEFORE recreating it.
	if (!Clear(*owner)) { return Failed; }
	if (Clients(registry) >= ClientLimit) { return Full; }
	if (registry.generation == std::numeric_limits<uint32_t>::max()) { return Failed; }
	try {
		owner->resources = std::make_unique<Resources>();
		if (!owner->resources->Create(pid, ++registry.generation)) {
			if (owner->resources->Reset()) { owner->resources.reset(); }
			return Failed;
		}
		return 0;
	} catch (const std::bad_alloc&) {
		if (owner->resources != nullptr && owner->resources->Reset()) { owner->resources.reset(); }
		return Failed;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EsvmUnregisterClient(const char* name) {
	KeysDrainOnExit keys_drain;
	if (name == nullptr) { return Invalid; }
	const int pid = Loader::GetCurrentGuestProcessId();
	if (pid <= 0) { return Failed; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	Reap(registry);
	auto owner = Find(registry, pid);
	if (owner == nullptr) { return NotInitialized; }
	std::lock_guard session_lock(owner->mutex);
	if (!owner->initialized) { return NotInitialized; }
	std::array<char, 64> copied {};
	if (!Name(name, &copied)) {
		// Client stops its receiver before sending. A rejected/too-long server
		// name leaves command ID zero; 0x309c0 -> 0x30abe returns 0x81790001.
		return Invalid;
	}
	// The asynchronous server command ignores the name after the bounded check.
	// Empty/other names and an absent PID slot all complete with zero.
	return Clear(*owner) ? 0 : Failed;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EsvmTerminate() {
	const int pid = Loader::GetCurrentGuestProcessId();
	if (pid <= 0) { return Failed; }
	return ReleaseEsvmOwner(pid) ? 0 : Failed;
}

bool ShutdownEsvmService() {
	bool released = true;
	{
		Common::HostLockCriticalGuard critical;
		auto& registry = Service();
		std::lock_guard lock(registry.mutex);
		registry.stopping = true;
		++registry.reader_epoch;
		for (auto& slot: registry.owners) {
			if (slot == nullptr) { continue; }
			auto owner = slot;
			std::lock_guard session_lock(owner->mutex);
			owner->initialized = false;
			if (Clear(*owner)) { slot.reset(); }
			else { released = false; }
		}
	}
	const bool sources_released = DrainRetiredEsvmServices(true);
	if (!EsvmDispatchScopeActive()) { StopEsvmSourceScheduler(); }
	return sources_released && released;
}

bool ReleaseEsvmOwner(int pid) {
	if (pid <= 0) { return false; }
	bool released = true;
	{
		Common::HostLockCriticalGuard critical;
		auto& registry = Service();
		std::lock_guard lock(registry.mutex);
		for (auto& slot: registry.owners) {
			if (slot != nullptr && slot->pid == pid) {
				// Keep a reference while holding its mutex, including after slot reset.
				auto owner = slot;
				std::lock_guard session_lock(owner->mutex);
				owner->initialized = false;
				released = Clear(*owner);
				if (released) { slot.reset(); }
				break;
			}
		}
	}
	return DrainRetiredEsvmServices(true) && released;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EsvmGetStatus(uint32_t selector, EsvmResult56* out) {
	KeysDrainOnExit keys_drain;
	if (out == nullptr) { return Invalid; }
	const int pid = Loader::GetCurrentGuestProcessId();
	if (pid <= 0) { return Failed; }
	std::shared_ptr<Session> owner;
	EsvmSnapshotReader reader = nullptr;
	uint64_t owner_epoch = 0, reader_epoch = 0;
	{
		Common::HostLockCriticalGuard critical;
		auto& registry = Service();
		std::lock_guard lock(registry.mutex);
		Reap(registry);
		owner = Find(registry, pid);
		if (owner == nullptr) { return NotInitialized; }
		std::lock_guard session_lock(owner->mutex);
		if (!owner->initialized) { return NotInitialized; }
		if (!Writable(out, sizeof(*out))) { return Invalid; }
		owner_epoch = owner->epoch;
		reader_epoch = registry.reader_epoch;
		reader = registry.reader;
	}
	EsvmResult56 result {};
	if (selector <= 1) {
		if (reader == nullptr) { return Failed; } // Unwired presentation is not usable output.
		EsvmPresentationSnapshot snapshot {};
		if (!reader(pid, &snapshot)) { return Failed; }
		result.result = selector == 0 ? snapshot.output_result : snapshot.port_result;
		if (selector == 0) { result.payload = snapshot.output_status; }
		else { std::copy(snapshot.port_status.begin(), snapshot.port_status.end(), result.payload.begin()); }
	}
	// Invalid selectors are not InvalidArgument: the native completion sends
	// embedded result zero, reserved zero, and an untouched zero-filled union.
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	std::lock_guard session_lock(owner->mutex);
	if (!owner->initialized || !Loader::IsGuestProcessIdentityActive(pid)) { return NotInitialized; }
	if (owner->epoch != owner_epoch || registry.reader_epoch != reader_epoch) { return Failed; }
	if (!Writable(out, sizeof(*out))) { return Invalid; }
	if (owner->resources != nullptr && owner->resources->complete) {
		auto& resources = *owner->resources;
		if (Sem::KernelPollSema(resources.request, 1) != 0) { return Failed; }
		const int response = Sem::KernelPollSema(resources.response, 1);
		if (response != 0) { Sem::KernelSignalSema(resources.request, 1); return Failed; }
		std::memcpy(resources.mapping, &result, sizeof(result));
		// Publish a complete HLE IPC response in the actual shared backing.
		const bool signaled = Sem::KernelSignalSema(resources.response, 1) == 0;
		const bool released = Sem::KernelSignalSema(resources.request, 1) == 0;
		if (!signaled || !released) { return Failed; }
	}
	return Store(out, &result, sizeof(result)) ? 0 : Invalid;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void EsvmFailNextResourceForTest(EsvmResourceFailure point) { failure = point; }
EsvmServiceResources EsvmResourcesForTest() {
	Common::HostLockCriticalGuard critical;
	auto& registry = Service();
	std::lock_guard lock(registry.mutex);
	EsvmServiceResources result;
	for (const auto& owner: registry.owners) {
		if (owner == nullptr) { continue; }
		std::lock_guard session_lock(owner->mutex);
		++result.sessions;
		result.initialized += owner->initialized;
		if (const auto* resources = owner->resources.get(); resources != nullptr) {
			result.clients += resources->complete;
			result.mappings += resources->mapping != nullptr;
			result.semaphores += (resources->request != 0) + (resources->response != 0);
			result.descriptors += resources->descriptor >= 0;
		}
	}
	return result;
}
#endif
} // namespace Libs::MediaFramework
