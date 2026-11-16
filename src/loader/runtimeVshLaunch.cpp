#include "loader/runtimeLaunch.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "emulator.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/mediaFrameworkEsvm.h"
#include "libs/runtimeUserService.h"
#include "libs/libSystemService.h"
#include "loader/elf.h"
#include "loader/runtimeApplicationPlan.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace Loader {
namespace {
namespace Fs = Libs::LibKernel::FileSystem;
namespace Lnc = Libs::LncUtil::Lnc;

class ProgramScope {
public:
	explicit ProgramScope(Program* program): m_previous(RuntimeLinker::GetCurrentProgram()) {
		RuntimeLinker::SetCurrentProgram(program);
	}
	~ProgramScope() { RuntimeLinker::SetCurrentProgram(m_previous); }
private:
	Program* m_previous;
};

struct ApplicationRun {
	Lnc::PendingLaunch request;
	RuntimeApplicationPlan plan;
	Program* program = nullptr;
	int guest_pid = -1;
	bool user_service_prepared = false;
	bool system_events_prepared = false;
	Fs::GuestFsContextPtr fs = Fs::CreateBootstrapFsContext();
	std::vector<Program*> modules;
	std::vector<std::string> arguments;
	std::vector<uint64_t> entry_words;
	std::atomic_int startup_result {0};
	std::atomic_bool entered {false};
	std::atomic_bool close_requested {false};
	// Published under VshRuntimeHost::m_mutex, before guest module initialization.
	std::shared_ptr<RuntimeGuestExecution> execution;
};

bool CheckImage(const std::filesystem::path& path, bool shared) {
	if (!Common::File::IsFileExisting(path)) { return false; }
	Elf64 image;
	image.Open(path);
	return image.IsValid() && image.IsShared() == shared;
}

// Host coordinator on a guest stack. Each initializer and executable entry
// supplies its own innermost guest boundary; this host frame must finish normally.
void* KYTY_SYSV_ABI ApplicationMain(void* opaque) {
	auto* run = static_cast<ApplicationRun*>(opaque);
	auto* linker = Common::Singleton<RuntimeLinker>::Instance();
	for (auto* module: run->modules) {
		if (run->close_requested.load()) {
			run->startup_result.store(Libs::LibKernel::KERNEL_ERROR_ECANCELED);
			return nullptr;
		}
		if (module->dynamic_info->init_vaddr == 0) { continue; }
		const auto result = linker->StartModuleChecked(module, 0, nullptr, nullptr);
		if (result.ThreadExited()) {
			// Return through coordinator RAII; only RunThread consumes the tagged
			// request. An initializer's ordinary exit is not an owner failure.
			run->startup_result.store(Libs::LibKernel::KERNEL_ERROR_ECANCELED);
			return nullptr;
		}
		// StartModuleChecked commits a module for every nonnegative return;
		// preserve that contract here (some initializers return a positive value).
		if (!result.Returned() || result.value < 0) {
			const bool canceled = result.guest.stopped || run->close_requested.load() ||
			                      run->program->application_exit_requested.load();
			run->startup_result.store(canceled ? Libs::LibKernel::KERNEL_ERROR_ECANCELED :
			    (result.Returned() && result.value < 0 ? result.value : Libs::LibKernel::KERNEL_ERROR_ENOEXEC));
			return nullptr;
		}
	}
	if (run->close_requested.load()) {
		run->startup_result.store(Libs::LibKernel::KERNEL_ERROR_ECANCELED);
		return nullptr;
	}
	if (!Lnc::RuntimeLaunchStarted(run->request.appId, run->guest_pid)) {
		run->startup_result.store(run->close_requested.load() ? Libs::LibKernel::KERNEL_ERROR_ECANCELED : Libs::LibKernel::KERNEL_ERROR_EINVAL);
		return nullptr;
	}
	run->entered.store(true);
	const auto entry = linker->RunApplicationEntry(run->program, reinterpret_cast<EntryParams*>(run->entry_words.data()));
	if (!entry.Returned() && !entry.ThreadExited() && (entry.failure ||
	    (!run->close_requested.load() && !run->program->application_exit_requested.load()))) {
		run->startup_result.store(Libs::LibKernel::KERNEL_ERROR_ECANCELED);
	}
	return nullptr;
}

class VshRuntimeHost: public std::enable_shared_from_this<VshRuntimeHost> {
public:
	explicit VshRuntimeHost(const Emulator::RunOptions& options): m_firmware(options.fw_root) {
		m_inputs.fw_root = options.fw_root;
		m_inputs.extra_mounts = options.extra_mounts;
		m_inputs.allow_firmware_titles = true;
	}

	int Launch(const Lnc::PendingLaunch& request) {
		Common::HostLockCriticalGuard critical;
		auto resolved = ResolveRuntimeApplicationPlan(m_inputs, request.titleId);
		if (!resolved.plan) {
			LOGF("VSH runtime: cannot resolve %s: %s\n", request.titleId.c_str(), resolved.message.c_str());
			return static_cast<int>(0x80940005);
		}
		// Native scenes and the web host require their real service/child-process
		// routes. Never run them through the standalone Hermes diagnostic VM.
		if (resolved.plan->route != RuntimeApplicationRoute::Native) {
			LOGF("VSH runtime: %s requires the %s route, not a native application entry\n",
			     request.titleId.c_str(), resolved.plan->route == RuntimeApplicationRoute::Web ? "firmware web host" : "native RNPS scene");
			return Libs::LibKernel::KERNEL_ERROR_ENOTSUP;
		}
		if (!CheckImage(resolved.plan->executable_path, false)) {
			return Libs::LibKernel::KERNEL_ERROR_ENOEXEC;
		}
		auto run = std::make_shared<ApplicationRun>();
		run->request = request;
		run->plan = std::move(*resolved.plan);
		run->arguments.push_back("/app0/eboot.bin");
		run->arguments.insert(run->arguments.end(), request.args.begin(), request.args.end());
		run->entry_words.resize(run->arguments.size() + 3, 0);
		auto* params = reinterpret_cast<EntryParams*>(run->entry_words.data());
		params->argc = static_cast<int>(run->arguments.size());
		for (size_t i = 0; i < run->arguments.size(); ++i) { params->argv[i] = run->arguments[i].c_str(); }
		{
			std::lock_guard lock(m_mutex);
			if (m_active != nullptr) { return Libs::LibKernel::KERNEL_ERROR_EBUSY; }
			// ShellUI consumes these paths in its own namespace, not the child's
			// private /app0. They remain stable for lifecycle-event snapshots.
			const auto installed = "/user/app/" + request.titleId;
			const auto metadata = "/user/appmeta/" + request.titleId;
			Fs::Mount(run->plan.app0_path, installed, true);
			Fs::Mount(run->plan.metadata_path.parent_path(), metadata, true);
			if (!Lnc::RegisterRuntimeLaunchPath(request.appId, installed + "/eboot.bin", metadata) ||
			    !Lnc::RuntimeLaunchAccepted(request.appId)) {
				return Libs::LibKernel::KERNEL_ERROR_EINVAL;
			}
			m_active = run;
		}
		try {
			std::thread([host = shared_from_this(), run] { host->Run(run); }).detach();
		} catch (...) {
			std::lock_guard lock(m_mutex);
			if (m_active == run) { m_active.reset(); }
			Lnc::RuntimeLaunchFailed(request.appId, Libs::LibKernel::KERNEL_ERROR_EAGAIN);
			return Libs::LibKernel::KERNEL_ERROR_EAGAIN;
		}
		return 0;
	}

	int Close(const Lnc::RuntimeCloseRequest& request) {
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(m_mutex);
		const auto run = m_active;
		if (!run || run->request.appId != request.app_id || run->request.seq != request.launch_seq) {
			return Libs::LibKernel::KERNEL_ERROR_EINVAL;
		}
		// Pre-owner startup is identified by the immutable launch sequence. Once
		// LNC has an owner, reject stale module/lifetime snapshots before mutation.
		const auto execution = run->execution;
		if (request.owner_program_id != -1 &&
		    (!execution || run->program->unique_id != request.owner_program_id ||
		     execution->Owner().identity != run->program || execution->Owner().token != request.owner_lifetime)) {
			return Libs::LibKernel::KERNEL_ERROR_EINVAL;
		}
		run->close_requested.store(true);
		if (execution) {
			run->program->application_exit_requested.store(true);
			execution->RequestStop();
		}
		LOGF("VSH runtime: close requested app=%u seq=%llu reason=%d force=%u; awaiting owned native stop and actual drain\n",
		     request.app_id, static_cast<unsigned long long>(request.launch_seq), request.reason, request.force);
		// The host stop pump retries eligible native workers outside this lock.
		return 0;
	}

private:
	int Prepare(ApplicationRun& run) {
		Common::HostLockCriticalGuard critical;
		if (run.close_requested.load()) { return Libs::LibKernel::KERNEL_ERROR_ECANCELED; }
		auto* linker = Common::Singleton<RuntimeLinker>::Instance();
		run.program = linker->LoadProgram(run.plan.executable_path, true);
		if (run.program == nullptr) { return Libs::LibKernel::KERNEL_ERROR_ENOEXEC; }
		if (!InitializeApplicationExecution(run.program)) { return Libs::LibKernel::KERNEL_ERROR_ENOSYS; }
		{
			std::lock_guard lock(m_mutex);
			run.execution = GetApplicationExecution(run.program);
			if (run.close_requested.load()) {
				run.program->application_exit_requested.store(true);
				run.execution->RequestStop();
				return Libs::LibKernel::KERNEL_ERROR_ECANCELED;
			}
		}
		ProgramScope program_scope(run.program);
		Fs::ScopedGuestFsContext fs_scope(run.fs);
		Fs::MountForProgram(run.plan.app0_path, "/app0", run.program->unique_id, true);
		Fs::MountForProgram(run.plan.app0_path, "/hostapp", run.program->unique_id, true);
		const auto temporary = std::filesystem::absolute("_TempData/vsh-app-" + std::to_string(run.request.appId));
		std::filesystem::create_directories(temporary);
		Fs::MountForProgram(temporary, "/temp", run.program->unique_id);
		Fs::MountForProgram(temporary, "/temp0", run.program->unique_id);
		if (!SystemContentLoadForProgram(run.program, run.plan.metadata_path)) {
			return Libs::LibKernel::KERNEL_ERROR_ENOEXEC;
		}
		Lnc::RegisterProgramLaunch(run.program, run.plan.metadata_path,
		    run.request.titleId.starts_with("NPXS"), run.request.appId, m_firmware);
		run.guest_pid = RegisterRuntimeProcessIdentity(run.program, run.request.appId);
		if (run.guest_pid < 0) { return Libs::LibKernel::KERNEL_ERROR_EINVAL; }
		const int user_service = Libs::UserService::PrepareUserServiceOwner(run.program);
		if (user_service != 0) {
			LOGF("VSH runtime: user identity preparation failed app=%u result=0x%08x\n", run.request.appId, user_service);
			return user_service;
		}
		run.user_service_prepared = true;
		if (!Libs::SystemService::RegisterApplicationEvents(run.execution)) { return Libs::LibKernel::KERNEL_ERROR_ECANCELED; }
		run.system_events_prepared = true;
		std::string discovery_error;
		if (!linker->PrepareAppLocalPrograms(run.program, run.plan.app0_path, AppLocalProgramPolicy::AdjacentAndNeeded, &run.modules, &discovery_error)) {
			LOGF("VSH runtime: app-local preparation rejected %s: %s\n", run.request.titleId.c_str(), discovery_error.c_str());
			return Libs::LibKernel::KERNEL_ERROR_ENOEXEC;
		}
		// Owned relocation visits every owner importer, never ShellUI/session slots.
		linker->RelocateProgram(run.program);
		// Supported typed function relocations must have immutable checked entries.
		// Their first unsupported invocation fails this owner before executing the
		// target. Unused function imports no longer prevent native startup.
		const auto blockers = linker->GetApplicationHardImportBlockers(run.program);
		if (!blockers.empty()) {
			LOGF("VSH runtime: %s has %zu hard relocation blockers; entry not started\n",
			     run.request.titleId.c_str(), blockers.size());
			for (size_t i = 0; i < std::min<size_t>(blockers.size(), 16); ++i) {
				LOGF("VSH runtime: import blocker %s\n", blockers[i].c_str());
			}
			return Libs::LibKernel::KERNEL_ERROR_ENOEXEC;
		}
		run.modules = linker->GetApplicationStartupModules(run.program);
		return 0;
	}

	void Run(const std::shared_ptr<ApplicationRun>& run) {
		int result = 0;
		try {
			result = Prepare(*run);
			if (result == 0 && run->close_requested.load()) { result = Libs::LibKernel::KERNEL_ERROR_ECANCELED; }
			if (result == 0) {
				// Construct before the native main: if host thread creation fails,
				// no unjoined guest worker is left behind by exception rollback.
				std::jthread stop_pump([execution = run->execution](std::stop_token token) {
					bool reported_control_error = false;
					while (!token.stop_requested()) {
						if (execution->StopRequested()) {
							const auto stopped = Libs::LibKernel::PthreadRequestOwnerStop(execution);
							if (stopped.status == Libs::LibKernel::PthreadOwnerStopStatus::ControlError && !reported_control_error) {
								LOGF("VSH runtime: native stop control error for lifetime=%llu; retaining pending owner\n",
								     static_cast<unsigned long long>(execution->Owner().token));
								reported_control_error = true;
							}
						}
						if (execution->WaitForDrain(std::chrono::milliseconds(50))) { break; }
					}
				});
				Libs::LibKernel::Pthread main = nullptr;
				{
					ProgramScope scope(run->program);
					Fs::ScopedGuestFsContext fs_scope(run->fs);
					result = Libs::LibKernel::PthreadCreateRuntimeCoordinator(&main, nullptr, ApplicationMain, run.get(), "VshApplication");
				}
				if (result == 0) {
					// Stop retries run beside this actual native join, never in its place.
					result = Libs::LibKernel::PthreadJoin(main, nullptr);
					if (result == 0) { result = run->startup_result.load(); }
				}
			}
		} catch (const std::exception& error) {
			LOGF("VSH runtime: launch failed for %s: %s\n", run->request.titleId.c_str(), error.what());
			result = Libs::LibKernel::KERNEL_ERROR_EIO;
		}
		// Failure in an initializer may follow creation of additional workers.
		// Both failed startup and normal return must wait for owned cleanup.
		if (run->program != nullptr) {
			const auto execution = GetApplicationExecution(run->program);
			if (execution) { execution->RequestStop(); }
			if (run->program->application_threads.load() != 0) {
				LOGF("VSH runtime: main ended for %s; waiting for %u owned threads\n", run->request.titleId.c_str(), run->program->application_threads.load());
			}
			while (run->program->application_threads.load() != 0) {
				if (execution) { Libs::LibKernel::PthreadRequestOwnerStop(execution); }
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			// Counts alone cannot establish callback/HLE quiescence. Keep the
			// owner resident until every admitted unit has actually released it.
			if (execution) {
				while (!execution->WaitForDrain(std::chrono::milliseconds(50))) {}
				// Cleanup counts can reach zero before native epilogues/TLS return.
				// Only retained actual OS handles prove that every owned thread ended.
				bool reported_native_error = false;
				for (;;) {
					const auto native = Libs::LibKernel::PthreadOwnerNativeThreadsTerminated(execution);
					if (native) { break; }
					if (native.status != Libs::LibKernel::PthreadOwnerNativeDrainStatus::Pending && !reported_native_error) {
						LOGF("VSH runtime: native completion pending app=%u status=%u; retaining owner\n",
						     run->request.appId, unsigned(native.status));
						reported_native_error = true;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				if (execution->Failure() && result == 0) { result = Libs::LibKernel::KERNEL_ERROR_ECANCELED; }
			}
			if (run->guest_pid >= 0) {
				if (run->system_events_prepared) {
					if (!Libs::SystemService::ReleaseApplicationEvents(execution)) {
						LOGF("VSH runtime: service event retirement retained app=%u\n", run->request.appId);
					} else { run->system_events_prepared = false; }
				}
				if (run->user_service_prepared) {
					const auto released = Libs::UserService::RetireUserServiceOwner(run->program);
					if (released != 0) { LOGF("VSH runtime: user identity retirement failed app=%u result=0x%08x\n", run->request.appId, released); }
					else { run->user_service_prepared = false; }
				}
				DeactivateRuntimeProcessIdentity(run->program);
				// Threads have drained and the identity lock is no longer held.
				// Failed releases stay owned by the service for its next retirement sweep.
				if (!Libs::MediaFramework::ReleaseEsvmOwner(run->guest_pid)) {
					LOGF("VSH runtime: media cleanup retained for pid=%d; retry pending\n", run->guest_pid);
				}
			}
		}
		{
			Common::HostLockCriticalGuard critical;
			std::lock_guard lock(m_mutex);
			// Keep identities, TLS and backing resident; release admission before
			// publishing completion so a consumer can immediately launch again.
			m_retired.push_back(run);
			if (m_active == run) { m_active.reset(); }
		}
		LOGF("VSH runtime: %s app=%u entry=%u result=0x%08x guest_exit=%d; ShellUI retained\n",
		     run->request.titleId.c_str(), run->request.appId, run->entered.load(), result,
		     run->program != nullptr ? run->program->application_exit_code.load() : 0);
		if (run->entered.load()) {
			Lnc::RuntimeLaunchQuiescentExit(run->request.appId, result);
		} else {
			Lnc::RuntimeLaunchFailed(run->request.appId,
			    result < 0 ? result : Libs::LibKernel::KERNEL_ERROR_ENOEXEC);
		}
	}

	RuntimeApplicationPlanOptions m_inputs;
	std::filesystem::path m_firmware;
	std::mutex m_mutex;
	std::shared_ptr<ApplicationRun> m_active;
	std::vector<std::shared_ptr<ApplicationRun>> m_retired;
};
} // namespace

void ConfigureVshRuntimeLaunch(const Emulator::RunOptions& options) {
	Lnc::SetRuntimeFocusHandler({});
	Lnc::ConfigureVshRuntime(true);
	auto host = std::make_shared<VshRuntimeHost>(options);
	Lnc::SetRuntimeLaunchHandler([host](const auto& request) { return host->Launch(request); });
	Lnc::SetRuntimeCloseHandler([host](const auto& request) { return host->Close(request); });
	LOGF("VSH runtime: same-session application backend configured\n");
}

bool ExitCurrentVshApplication(int code) {
	const auto* current = RuntimeLinker::GetCurrentProgram();
	auto* owner = current != nullptr ? current->application_owner : nullptr;
	if (owner == nullptr) { return false; }
	owner->application_exit_code.store(code);
	owner->application_exit_requested.store(true);
	// Close admission for this exact owner while the ordinary pthread-exit path
	// returns through its guest boundary. Peer drain is still required by Run.
	if (const auto execution = GetApplicationExecution(owner)) { execution->RequestStop(); }
	LOGF("VSH runtime: guest app module=%d requested exit(%d)\n", owner->unique_id, code);
	Libs::LibKernel::PthreadExit(reinterpret_cast<void*>(static_cast<intptr_t>(code)));
	return true;
}
} // namespace Loader
