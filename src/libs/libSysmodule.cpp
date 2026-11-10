#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "kernel/module.h"
#include "libs/libs.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include <atomic>
#include <chrono>
#endif

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

constexpr int      SYSMODULE_ERROR_UNLOADED = static_cast<int>(0x805a1001u);
constexpr int      SYSMODULE_ERROR_FATAL    = static_cast<int>(0x805a10ffu);

static constexpr struct NativeModule {
	uint32_t    id;
	const char* path;
	const char* alternate_path;
} g_native_modules[] = {
    // 13.60 internal ID 0x80000076: record 0x25b80, name relocation
    // 0x25b90 -> 0xf9e5 (libSceRnpsAppMgr).
    {0x80000076, "/system/common/lib/libSceRnpsAppMgr.sprx", nullptr},
    // 13.60 internal ID 0x800000c6: record 0x27440, name relocation
    // 0x27450 -> 0xfc71 (libSceCloudMessaging). PSM requests a real handle.
    {0x800000c6, "/system/common/lib/libSceCloudMessaging.sprx", nullptr},
    {0x0115, "/app0/sce_module/libSceNpCppWebApi.prx", nullptr},
    // 13.60 libSceSysmodule: record 0x26fc0, name relocation 0x26fd0
    // -> 0x10209 (libSceEsvm). Do not substitute an app-local/older provider.
    {0x012e, "/system/common/lib/libSceEsvm.sprx", "/common/lib/libSceEsvm.sprx"},
};

static const NativeModule* FindNativeModule(uint32_t id) {
	for (const auto& module: g_native_modules) {
		if (module.id == id) { return &module; }
	}
	return nullptr;
}

static uint32_t CanonicalModuleId(uint32_t id) {
	// Explicit internal providers retain their full native ID. Preserve legacy
	// low-half aliases only for IDs without an explicit provider mapping.
	return FindNativeModule(id) != nullptr ? id : static_cast<uint16_t>(id);
}

// Per-operation BasicLockable adapter, not shared state on the mutex. Protect
// before acquiring the host lock and drop protection only after releasing it.
// condition_variable_any uses these same unlock/lock operations while parked.
class RegistryLock {
public:
	explicit RegistryLock(std::mutex& mutex): m_mutex(mutex), m_counter(*Common::GetHostLockCriticalCounter()) {}
	void lock() {
		m_counter.fetch_add(1, std::memory_order_seq_cst);
		try {
			m_mutex.lock();
		} catch (...) {
			m_counter.fetch_sub(1, std::memory_order_seq_cst);
			throw;
		}
	}
	void unlock() {
		m_mutex.unlock();
		m_counter.fetch_sub(1, std::memory_order_seq_cst);
	}
	KYTY_CLASS_NO_COPY(RegistryLock);
private:
	std::mutex& m_mutex;
	Common::HostLockCriticalCounter& m_counter;
};

// Only successful loads publish a reference. The per-entry transition is kept
// alive for waiters even after failure/erase; a later independent call can retry.
// In particular, neither guest initializers nor finalizers run under this lock.
class ModuleRegistry {
public:
	struct LoadResult {
		int status;
		LibKernel::KernelModule handle;
		Loader::Program* program = nullptr; // Successful image reference, for host-only late rollback.
	};

	template <typename Load>
	int LoadModule(const Loader::Program* owner, uint32_t id, Load&& load) {
		const auto execution = Loader::GetApplicationExecution(owner);
		Loader::RuntimeGuestExecution::Lease admission;
		if (owner != nullptr) {
			if (!execution) { return SYSMODULE_ERROR_FATAL; }
			admission = execution->TryAcquire(Loader::GuestWorkKind::Module);
			if (!admission) { return SYSMODULE_ERROR_FATAL; }
		}
		const Key key {owner, id};
		RegistryLock mutex(m_mutex);
		std::unique_lock lock(mutex);
		for (;;) {
			if (execution && execution->StopRequested()) { return SYSMODULE_ERROR_FATAL; }
			const auto it = m_modules.find(key);
			if (it == m_modules.end()) { break; }
			const auto entry = it->second;
			if (entry->state == State::Loaded) {
				if (entry->references == std::numeric_limits<uint32_t>::max()) { return SYSMODULE_ERROR_FATAL; }
				if (admission) {
					if (!admission.Publish([&] { ++entry->references; })) { return SYSMODULE_ERROR_FATAL; }
				} else { ++entry->references; }
				return 0;
			}
			if (!Wait(lock, entry, execution)) { return SYSMODULE_ERROR_FATAL; }
			if (entry->state == State::Failed) { return entry->result; }
		}
		auto entry = std::make_shared<Entry>();
		entry->thread = std::this_thread::get_id();
		m_modules.emplace(key, entry);
		lock.unlock();
		LoadResult result {SYSMODULE_ERROR_FATAL, -1};
		try {
			result = load();
		} catch (...) {
			lock.lock();
			entry->state = State::Failed;
			entry->result = SYSMODULE_ERROR_FATAL;
			m_modules.erase(key);
			m_changed.notify_all();
			if (execution) {
				execution->Fail(Loader::GuestFailureReason::Explicit);
				return SYSMODULE_ERROR_FATAL;
			}
			throw;
		}
		lock.lock();
		const auto publish = [&] {
			entry->state = State::Loaded;
			entry->references = 1;
			entry->handle = result.handle;
		};
		bool published = false;
		if (result.status == 0) {
			if (admission) { published = admission.Publish(publish); }
			else { publish(); published = true; }
			if (!published) {
				if (result.program != nullptr) { result.program->rt->RollbackModuleReference(result.program); }
				result = {SYSMODULE_ERROR_FATAL, -1};
			}
		}
		entry->result = result.status;
		if (!published) {
			entry->state = State::Failed;
			m_modules.erase(key);
		}
		m_changed.notify_all();
		return result.status;
	}

	template <typename Stop>
	int UnloadModule(const Loader::Program* owner, uint32_t id, Stop&& stop) {
		const auto execution = Loader::GetApplicationExecution(owner);
		Loader::RuntimeGuestExecution::Lease admission;
		if (owner != nullptr) {
			if (!execution) { return SYSMODULE_ERROR_FATAL; }
			admission = execution->TryAcquire(Loader::GuestWorkKind::Module);
			if (!admission) { return SYSMODULE_ERROR_FATAL; }
		}
		const Key key {owner, id};
		RegistryLock mutex(m_mutex);
		std::unique_lock lock(mutex);
		std::shared_ptr<Entry> entry;
		for (;;) {
			if (execution && execution->StopRequested()) { return SYSMODULE_ERROR_FATAL; }
			const auto it = m_modules.find(key);
			if (it == m_modules.end()) { return SYSMODULE_ERROR_UNLOADED; }
			entry = it->second;
			if (entry->state == State::Loaded) { break; }
			if (!Wait(lock, entry, execution)) { return SYSMODULE_ERROR_FATAL; }
		}
		if (entry->references > 1) {
			if (admission) {
				if (!admission.Publish([&] { --entry->references; })) { return SYSMODULE_ERROR_FATAL; }
			} else { --entry->references; }
			return 0;
		}
		entry->state = State::Unloading;
		entry->thread = std::this_thread::get_id();
		lock.unlock();
		int result = SYSMODULE_ERROR_FATAL;
		try {
			result = entry->handle >= 0 ? stop(entry->handle) : 0;
		} catch (...) {
			lock.lock();
			if (execution) {
				entry->state = State::Failed;
				entry->references = 0;
				entry->result = SYSMODULE_ERROR_FATAL;
				m_modules.erase(key);
				m_changed.notify_all();
				execution->Fail(Loader::GuestFailureReason::Explicit);
				return SYSMODULE_ERROR_FATAL;
			}
			entry->state = State::Loaded;
			m_changed.notify_all();
			throw;
		}
		lock.lock();
		const auto complete = [&] {
			entry->state = result == 0 ? State::Unloaded : State::Loaded;
			if (result == 0) { m_modules.erase(key); }
		};
		bool completed = false;
		if (admission) { completed = admission.Publish(complete); }
		else { complete(); completed = true; }
		if (!completed) {
			entry->state = State::Failed;
			entry->references = 0;
			entry->handle = -1;
			entry->result = SYSMODULE_ERROR_FATAL;
			m_modules.erase(key);
			m_changed.notify_all();
			return SYSMODULE_ERROR_FATAL;
		}
		m_changed.notify_all();
		return result;
	}

	bool IsLoaded(const Loader::Program* owner, uint32_t id) {
		const auto execution = Loader::GetApplicationExecution(owner);
		if (owner && (!execution || execution->StopRequested())) { return false; }
		RegistryLock mutex(m_mutex);
		std::scoped_lock lock(mutex);
		const auto it = m_modules.find({owner, id});
		return it != m_modules.end() && it->second->state == State::Loaded && (!execution || !execution->StopRequested());
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	unsigned TestWaiters() {
		RegistryLock mutex(m_mutex);
		std::scoped_lock lock(mutex);
		return m_test_waiters;
	}
	unsigned TestEntries() {
		RegistryLock mutex(m_mutex);
		std::scoped_lock lock(mutex);
		return static_cast<unsigned>(m_modules.size());
	}
#endif

private:
	struct Key {
		const Loader::Program* owner;
		int32_t owner_id;
		uint64_t lifetime;
		uint32_t id;
		Key(const Loader::Program* application, uint32_t module_id):
		    owner(application), owner_id(application != nullptr ? application->unique_id : -1),
		    lifetime(Loader::GetApplicationExecution(application) ? Loader::GetApplicationExecution(application)->Owner().token : 0),
		    id(module_id) {}
		bool operator==(const Key& other) const {
			return owner == other.owner && owner_id == other.owner_id && lifetime == other.lifetime && id == other.id;
		}
	};
	struct Hash {
		size_t operator()(const Key& key) const {
			return std::hash<const Loader::Program*>()(key.owner) ^ std::hash<int32_t>()(key.owner_id) ^
			       std::hash<uint64_t>()(key.lifetime) ^ (std::hash<uint32_t>()(key.id) << 1);
		}
	};
	enum class State { Loading, Loaded, Unloading, Failed, Unloaded };
	struct Entry {
		State state = State::Loading;
		uint32_t references = 0;
		LibKernel::KernelModule handle = -1;
		int result = 0;
		std::thread::id thread;
	};
	bool Wait(std::unique_lock<RegistryLock>& lock, const std::shared_ptr<Entry>& entry,
	          const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) {
		// A guest callback may request another module, but must not wait on an
		// in-flight operation while owning one itself (including cross-thread
		// A -> B -> A cycles). Never pretend that a recursive load succeeded.
		for (const auto& item: m_modules) {
			const auto& pending = item.second;
			if ((pending->state == State::Loading || pending->state == State::Unloading) &&
			    pending->thread == std::this_thread::get_id()) { return false; }
		}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		++m_test_waiters;
#endif
		const auto completed = [&] { return entry->state != State::Loading && entry->state != State::Unloading; };
		if (execution) {
			while (!completed() && !execution->StopRequested()) { m_changed.wait_for(lock, std::chrono::milliseconds(10)); }
		} else { m_changed.wait(lock, completed); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		--m_test_waiters;
#endif
		return !execution || !execution->StopRequested();
	}
	std::mutex m_mutex;
	std::condition_variable_any m_changed;
	std::unordered_map<Key, std::shared_ptr<Entry>, Hash> m_modules;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	unsigned m_test_waiters = 0;
#endif
};

static ModuleRegistry g_modules;

static const Loader::Program* CurrentModuleOwner() {
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	return program != nullptr ? program->application_owner : nullptr;
}

static ModuleRegistry::LoadResult LoadNativeModule(
    uint32_t id, decltype(&LibKernel::KernelLoadStartModule) load = &LibKernel::KernelLoadStartModule) {
	const auto* module = FindNativeModule(id);
	if (module == nullptr) { return {0, -1}; } // Preserve existing HLE/unknown-ID semantics.
	const char* path = module->path;
	if (module->alternate_path != nullptr &&
	    !Common::File::IsFileExisting(LibKernel::FileSystem::GetRealFilename(path))) {
		path = module->alternate_path;
	}
	int start_result = 0;
	const auto handle = load(path, 0, nullptr, 0, nullptr, &start_result);
	// The kernel owns initializer admission/rollback. A successful handle can
	// accompany a positive module_start output; that output is not an API error
	// (13.60 libkernel 0x35426..0x35437, 0x35505..0x35532, 0x35582..0x3558b).
	if (start_result != 0) {
		LOGF("sysmodule %04x: load API=%d module_start output=%d\n", id, handle, start_result);
	}
	if (handle < 0) { return {handle, -1}; }
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (program != nullptr && program->application_owner != nullptr) {
		auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		program = rt->FindProgramById(handle);
		if (program != nullptr) { return {0, handle, program}; }
	}
	return {0, handle};
}

static int StopNativeModule(
    LibKernel::KernelModule handle,
    decltype(&LibKernel::KernelStopUnloadModule) stop = &LibKernel::KernelStopUnloadModule) {
	int stop_result = 0;
	const int result = stop(handle, 0, nullptr, 0, nullptr, &stop_result);
	// Firmware unloads even after a nonzero/negative finalizer output; only the
	// API status determines whether the registry reference remains live
	// (13.60 libkernel 0x35941..0x35953, teardown 0x359aa..0x35a6d).
	if (stop_result != 0) {
		LOGF("sysmodule handle %d: stop API=%d module_stop output=%d\n", handle, result, stop_result);
	}
	return result != 0 ? SYSMODULE_ERROR_FATAL : 0;
}

static int LoadModule(uint32_t id) {
	id = CanonicalModuleId(id);
	return g_modules.LoadModule(CurrentModuleOwner(), id, [=] { return LoadNativeModule(id); });
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static int SysmoduleIsLoadedId(uint32_t id) {
	return g_modules.IsLoaded(CurrentModuleOwner(), CanonicalModuleId(id)) ? 1 : 0;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	return LoadModule(id);
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	return g_modules.UnloadModule(CurrentModuleOwner(), id, [](LibKernel::KernelModule handle) {
		return StopNativeModule(handle);
	});
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint32_t id, int arg1, int arg2,
                                                            int arg3, int* ret) {
	PRINT_NAME();

	LOGF("\t id = %d arg1 = %d arg2 = %d arg3 = %d ret = %p\n", static_cast<int>(id), arg1,
	     arg2, arg3, static_cast<void*>(ret));

	// ps5-vsh: ShellCore passes nonzero args (e.g. id 16); ignore them like
	// SysmoduleLoadModuleInternal does.
	const int result = LoadModule(id);
	if (ret != nullptr) {
		*ret = result;
	}

	return result;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	const int loaded = SysmoduleIsLoadedId(id);

	LOGF("\t id = %d -> %d\n", static_cast<int>(id), loaded);

	// ps5-vsh: docs say LOADED = SCE_OK (0), not loaded = 0x805A1001.
	return loaded != 0 ? 0 : static_cast<int>(0x805A1001u);
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleLoadModuleInternal(uint32_t id, uint64_t a1, uint64_t a2,
                                                     uint64_t a3, uint64_t a4, uint64_t a5) {
	// ps5-vsh: 39iV5E1HoCk. Native mappings use the same owner-scoped
	// load/start path as public IDs; other IDs retain the existing HLE behavior.
	// Returns 0 (SCE_OK) on success: JSCd proves it - it logs
	// "Failed to load sysmodule ... err: <ret>" and ABORTS on nonzero
	// (our old handle=1 surfaced verbatim as err 0x1 for 0x80000010).
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	LOGF("ps5-vsh: SysmoduleLoadModuleInternal(%08x)\n", id);
	return LoadModule(id);
}

// ps5-vsh: vXZhrtJxkGc. NPXS40038 unloads its VSE/Agc modules on the error
// path. Explicit internal mappings retain their full ID; legacy aliases share
// the public loader's owner-scoped reference/lifecycle handling.
static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleUnloadModuleInternal(uint32_t id, uint64_t a1, uint64_t a2,
                                                       uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();

	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;

	LOGF("\t id = %08x\n", id);

	return g_modules.UnloadModule(CurrentModuleOwner(), CanonicalModuleId(id), [](LibKernel::KernelModule handle) {
		return StopNativeModule(handle);
	});
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int SysmoduleGetModuleHandleInternal(uint32_t id, int32_t* handle) {
	PRINT_NAME();
	LOGF("\t id = %08x handle = %p\n", id, static_cast<const void*>(handle));
	if (handle == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	// Provider names below are verified against 13.60's native sysmodule table.
	// Sysmodule IDs are not kernel module handles: dlsym requires unique_id
	// from the canonical image already loaded by the system-module runner.
	const char* filename = nullptr;
	if (id == 0x800000c6u || id == 0x80000076u) {
		filename = FindNativeModule(id)->path;
	} else if (id == 0x800000d3u) {
		filename = "/system_ex/common_ex/lib/libScePubSubClientShared.sprx";
	} else if (id == 0x95u) {
		filename = "/system/common/lib/libSceIme.sprx";
	} else if (id == 0x96u) {
		filename = "/system/common/lib/libSceImeDialog.sprx";
	} else if (id == 0x012eu) {
		const auto* module = FindNativeModule(0x012e);
		filename = Common::File::IsFileExisting(LibKernel::FileSystem::GetRealFilename(module->path))
		               ? module->path : module->alternate_path;
	}
	if (filename == nullptr) {
		LOGF("\t no native provider mapping for sysmodule %08x\n", id);
		return LibKernel::KERNEL_ERROR_ENOENT;
	}
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramByFileName(LibKernel::FileSystem::GetRealFilename(filename));
	if (program == nullptr) {
		return LibKernel::KERNEL_ERROR_ENOENT;
	}
	if (program->application_owner != nullptr) {
		const auto execution = Loader::GetApplicationExecution(program);
		if (!execution || execution->StopRequested()) { return LibKernel::KERNEL_ERROR_ECANCELED; }
		std::lock_guard lock(program->module_mutex);
		if (program->module_state != Loader::ModuleExecutionState::Started || program->load_count == 0) {
			return LibKernel::KERNEL_ERROR_ENOENT;
		}
	}
	*handle = program->unique_id;
	LOGF("\t sysmodule %08x -> native handle %d (%s)\n", id, *handle, filename);
	return 0;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include "../../tests/SysmoduleNativeTests.inc"
#define KYTY_RUNTIME_GUEST_EXECUTION_REGISTRY_TESTS
#include "../../tests/RuntimeGuestExecutionTests.inc"
#undef KYTY_RUNTIME_GUEST_EXECUTION_REGISTRY_TESTS
#endif

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1) {
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
	LIB_FUNC("39iV5E1HoCk", Sysmodule::SysmoduleLoadModuleInternal); // ps5-vsh
	LIB_FUNC("vXZhrtJxkGc", Sysmodule::SysmoduleUnloadModuleInternal); // ps5-vsh
	LIB_FUNC("D8cuU4d72xM", Sysmodule::SysmoduleGetModuleHandleInternal);
}

} // namespace Libs
