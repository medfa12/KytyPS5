#include "emulator.h"
#include "libs/mediaFrameworkEsvm.h"
#include "loader/runtimeLaunch.h"
#include "loader/runtimeChildClient.h"
#include "graphics/presentation/window/runtimeWindowBridge.h"
#include "loader/runtimeProcessIdentity.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libHermesVm.h"
#include "libs/libLncUtil.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/childLaunch.h"
#include "loader/additionalContent.h"
#include "loader/runtimeLinker.h"
#include "loader/elf.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <thread>

namespace Emulator {

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

#if defined(__APPLE__)
	static constexpr auto platform_name = "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr auto platform_name = "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static constexpr auto platform_name = "Linux";
#else
	static constexpr auto platform_name = "Unknown";
#endif

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, platform_name, info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static void CloseMediaService() {
	// Admission closes atomically with release of private HLE IPC objects.
	// Snapshot reads already in flight are invalidated before output commit.
	if (!Libs::MediaFramework::ShutdownEsvmService() &&
	    !Libs::MediaFramework::ShutdownEsvmService()) {
		LOGF("MediaFramework: shutdown cleanup retained after retry\n");
		Log::Flush();
	}
}

static void KytyClose() {
	CloseMediaService();
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

// ps5-vsh: firmware is read-only; anything mounted from under fw_root is
// mounted read_only so guest writes fail EROFS instead of touching the
// dump (voldemort parity: read_only mounts). Game dumps outside fw_root
// stay writable.
static bool IsUnderFwRoot(const std::filesystem::path& dir, const std::filesystem::path& fw_root) {
	if (fw_root.empty()) {
		return false;
	}
	const std::string p    = Common::PathToGenericString(dir);
	const std::string base = Common::PathToGenericString(fw_root);
	return p.size() > base.size() && p.compare(0, base.size(), base) == 0 && p[base.size()] == '/';
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static void Init(const Config::ConfigOptions& cfg, const std::filesystem::path& param_json,
                 Common::Subsystems& subsystems, bool cohosted_system) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(cfg);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
	}
	if (cfg.splash_enabled) {
		Libs::Graphics::InitializeSplashScreen(param_json.parent_path());
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	if (cohosted_system && Loader::SystemContentGetMemoryParams().flexible_size == 0) {
		// The shell chain shares one owner across several system processes. Retain
		// its pre-merge 1 GiB fallback; standalone titles use upstream's 448 MiB.
		Libs::LibKernel::Memory::SetFlexibleMemorySize(1ull << 30);
	}
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
	subsystems.Initialize<Libs::Graphics::Lifecycle>();
}

static Loader::Program* LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	// ps5-vsh: CLI-supplied host paths (e.g. --co-elf) bypass the guest
	// mount table; fall back to the raw path when resolution yields nothing.
	auto real =
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf));
	if (real.empty()) {
		real = elf;
	}
	auto* program = rt->LoadProgram(real);

	// ps5-vsh TEMP: map GOT slots to imports.
	program->dbg_print_reloc = true;

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}

	return program;
}

struct ExecuteParams {
	std::filesystem::path            game_patch;
	std::vector<std::string>         guest_args;
	std::vector<Loader::CoElfLaunch> co_elfs;
};

static void Execute(const std::filesystem::path& game_patch,
                    const std::vector<std::string>& guest_args,
                    std::vector<Loader::CoElfLaunch> co_elfs = {}) {
	// Heap-allocated: the guest thread outlives this frame (quick_exit).
	auto* exec_params = new ExecuteParams {game_patch, guest_args, std::move(co_elfs)};
	Common::Thread guest_thread(
	    [](void* param) {
		    auto* p  = static_cast<ExecuteParams*>(param);
		    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		    rt->Execute(p->game_patch, p->guest_args, p->co_elfs);
		    delete p;
	    },
	    exec_params);
	// No join: the guest (ShellCore main) runs until process exit, and
	// joining here would deadlock the window pump. Detach documents the
	// no-join intent (also avoids Thread::~Thread EXIT_IF on any future
	// normal-return path); teardown stays with quick_exit below.
	guest_thread.Detach();
	Libs::Graphics::WindowRun();
	// ps5-vsh: graceful teardown. WindowRun returned (window closed) while
	// guest threads may still be inside HLE. There is no guest join-all
	// primitive (kernel/pthread.h offers only per-thread Join), so give the
	// guest a brief grace to settle before quick_exit tears down host
	// state via at_quick_exit EmergencyShutdownActive. Host lifecycle only;
	// no guest semantics changed.
	LOGF("ps5-vsh: window closed, grace shutdown (500ms) before exit\n");
	Log::Flush();
	std::fflush(stdout);
	std::fflush(stderr);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
	if (!Libs::Graphics::ConfigureRuntimeWindowRole(options.vsh_session && options.vsh_subprocess, options.runtime_parent_window, options.runtime_parent_pid)) {
		EXIT("Invalid runtime window ownership\n");
	}
	if (!options.runtime_child.empty() && (!Loader::ConfigureRuntimeChildClient(options.runtime_child,
	    Loader::RuntimeChild::EmbeddedWindow | Loader::RuntimeChild::BrokeredInput | Loader::RuntimeChild::FocusAudio | Loader::RuntimeChild::ComposedFrames) ||
	    !Libs::LncUtil::Lnc::ConfigureStandaloneRuntimeIdentity(Loader::RuntimeChildAppId()))) {
		EXIT("Invalid inherited runtime child role\n");
	}
	Libs::Controller::ConfigureRuntimeControllerRole(options.vsh_session && options.vsh_subprocess);
	if (Loader::RuntimeChildHasStoreLease()) {
		// The opt-in lease currently covers only the dedicated title /data
		// manifest. ShellUI, additional/overlapping mounts and legacy launches
		// have no established lifetime ownership contract and must fail closed.
		if (options.vsh_session || options.extra_mounts.size() != 1 || options.extra_mounts.front().first != "/data" ||
		    !Loader::ValidateRuntimeChildStoreLease(options.extra_mounts.front().second)) {
			EXIT("Unsupported canonical store lease mount layout\n");
		}
	}
	Libs::Audio::SetRuntimeAudioFocus(!Loader::IsRuntimeChild());
	Libs::Graphics::ConfigureRuntimeFocusServices([](bool foreground, bool controller, uint64_t epoch) {
		Libs::Controller::SetRuntimeControllerFocus(foreground && controller, epoch);
		return Libs::Audio::SetRuntimeAudioFocus(foreground);
	}, Libs::Controller::PumpRuntimeChildController);
	// ps5-vsh: uncaught C++ exceptions otherwise die in WER (0xe06d7363)
	// with zero forensics. Name the type, message and guest thread first.
	std::set_terminate([] {
		char thread_name[64] = "(host thread)";
		try {
			if (auto self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
				if (Libs::LibKernel::PthreadGetname(self, thread_name) != 0) {
					std::snprintf(thread_name, sizeof(thread_name), "(unnamed guest thread)");
				}
			}
		} catch (...) {
		}
		try {
			try {
				throw;
			} catch (const std::exception& e) {
				std::printf("--- Terminate: thread=%s what=%s\n", thread_name, e.what());
			} catch (...) {
				std::printf("--- Terminate: thread=%s (non-std exception)\n", thread_name);
			}
		} catch (...) {
		}
		LOGF("--- Terminate: thread=%s (see stdout)\n", thread_name);
		Log::Flush();
		std::fflush(stdout);
		std::fflush(stderr);
		std::abort();
	});

	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	const auto param_json = options.param_json.empty()
	                            ? options.app0_dir / "sce_sys" / "param.json"
	                            : options.param_json;
	Common::Subsystems subsystems(true);
	Init(options.config, param_json, subsystems, !options.fw_root.empty() && !options.co_elfs.empty());

	// ps5-vsh: firmware mounts for system titles (MONO_PATH references
	// /common_ex/lib, /common/lib; shell state lives under /data).
	// Duplicated early for RNPS Hermes runs (which evaluate + quick_exit
	// before the main mount block below); re-mounting later is harmless.
	if (!options.fw_root.empty()) {
		const auto mount_if_present = [](const std::filesystem::path& dir, const char* point,
		                                   bool read_only = false) {
			if (Common::File::IsDirectoryExisting(dir)) {
				Libs::LibKernel::FileSystem::Mount(dir, point, read_only);
			}
		};
		mount_if_present(options.fw_root / "system_b" / "common", "/common", true);
		mount_if_present(options.fw_root / "system_ex_b" / "common_ex", "/common_ex", true);
		mount_if_present(options.fw_root / "system_b", "/system", true);
		mount_if_present(options.fw_root / "system_ex_b", "/system_ex", true);
		mount_if_present(options.fw_root / "preinst", "/preinst", true);
		// ps5-vsh: "/" fallback so guest root stats succeed (PSM Create
		// Root mount stats "/"; without it the mount fails and Mono can
		// never open /app0/psm/Application/app.exe). Backed by an EMPTY
		// dir, not fw_root: unmapped root children must stay ENOENT as on
		// HW (fw_root backing fabricated successes for fw-layout paths
		// like /system_b which do not exist on the console). Longest-match
		// resolution keeps every specific mount winning over this.
		std::error_code empty_ec;
		std::filesystem::create_directories("_TempData/empty_root", empty_ec);
		mount_if_present("_TempData/empty_root", "/");
	}

	PrintSystemInfo();
	std::string title_id;
	Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id);
	if (title_id.empty()) {
		std::string content_id;
		if (Loader::SystemContentParamSfoGetString("CONTENT_ID", &content_id) &&
		    content_id.size() == 36 && content_id[6] == '-' && content_id[16] == '_') {
			title_id = content_id.substr(7, 9);
		}
	}
	if (!title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}
	// ps5-vsh: RNPS Hermes bundles execute on the host Hermes VM
	// (libhermes, v0.12 line for v89 bytecode). Decrypted bundles only;
	// encrypted ones fail closed inside HermesVmCreate.
	Loader::RnpsManifest rnps_manifest {};
	if (Loader::SystemContentGetRnpsManifest(&rnps_manifest) &&
	    !rnps_manifest.titleId.empty()) {
		const auto bundle =
		    options.app0_dir / "application.ps.bundle";
		std::string bytes;
		{
			Common::File f;
			if (!f.Open(Common::PathToString(bundle),
			            Common::File::Mode::Read)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "RNPS app {}: cannot open {}\n", rnps_manifest.titleId,
				    Common::PathToString(bundle)));
				std::quick_exit(1);
			}
			bytes.resize(static_cast<size_t>(f.Size()));
			uint32_t got = 0;
			f.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &got);
			f.Close();
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "RNPS app {}: executing {} bytes on Hermes VM\n",
		    rnps_manifest.titleId, bytes.size()));
		std::fflush(stdout);
		if (!options.rnps_params.empty()) {
			Libs::HermesVm::HermesVmHost::SetRnpsLaunchParams(options.rnps_params);
		}
		// ps5-vsh: app identity feeds the TTPool promote + MountApp path.
		Libs::HermesVm::HermesVmHost::SetRnpsAppInfo(
		    rnps_manifest.titleId, rnps_manifest.applicationName,
		    rnps_manifest.applicationVersion);
		if (options.rnps_resident_secs > 0) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "RNPS app {}: resident mode, {} live ticks\n", rnps_manifest.titleId,
			    options.rnps_resident_secs));
			std::fflush(stdout);
		}
		const int rc = options.rnps_resident_secs > 0
		    ? Libs::HermesVm::HermesVmHost::RunHbcBufferLive(
		          reinterpret_cast<const uint8_t*>(bytes.data()),
		          static_cast<uint64_t>(bytes.size()), options.rnps_resident_secs)
		    : Libs::HermesVm::HermesVmHost::RunHbcBuffer(
		          reinterpret_cast<const uint8_t*>(bytes.data()),
		          static_cast<uint64_t>(bytes.size()));
		Log::WriteToConsoleAndLog(
		    fmt::format("RNPS app {}: Hermes exit {}\n", rnps_manifest.titleId, rc));
		std::fflush(stdout);
		std::quick_exit(rc == 0 ? 0 : 1);
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);
	// Callbacks run in reverse registration order: release IPC while the
	// filesystem, semaphore and memory services still exist.
	ok = at_quick_exit(CloseMediaService);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(
	    options.app0_dir, "/app0", Loader::IsRuntimeChild() || IsUnderFwRoot(options.app0_dir, options.fw_root));
	Libs::LibKernel::FileSystem::Mount(
	    options.app0_dir, "/hostapp", Loader::IsRuntimeChild() || IsUnderFwRoot(options.app0_dir, options.fw_root));

	// ps5-vsh: guest argv actually executed. A drained Lnc child launch
	// carries ShellCore's argv (forwarded at enqueue); otherwise CLI args.
	std::vector<std::string> effective_guest_args = options.guest_args;
	std::filesystem::path effective_param_json = param_json;
	uint32_t effective_app_id = 0;

	// ps5-vsh: mirror run_shellui-style guest boot flags into HLE so the
	// emulated environment matches what the guest was told to expect
	// (-NoForegroundUser: no logged-in user anywhere).
	for (const auto& a : effective_guest_args) {
		if (a == "-NoForegroundUser") {
			Libs::SetNoForegroundUser(true);
			break;
		}
	}

	// ps5-vsh: title launch marker (logging only; single-shot launch was
	// already remounted by selection, so do NOT RemountApp0 here).
	if (!options.fw_root.empty()) {
		const std::string app0_str = Common::PathToGenericString(options.app0_dir);
		const std::string sys_b    = Common::PathToGenericString(options.fw_root / "system_b");
		const std::string sys_ex   = Common::PathToGenericString(options.fw_root / "system_ex_b");
		const auto is_under        = [](const std::string& p, const std::string& base) {
            if (p.size() <= base.size() || p.compare(0, base.size(), base) != 0) {
                return false;
            }
            return p[base.size()] == '/';
        };
		if (is_under(app0_str, sys_b) || is_under(app0_str, sys_ex)) {
			const std::string launched_title = options.launch_title.empty()
			                                       ? options.app0_dir.filename().string()
			                                       : options.launch_title;
			LOGF("ps5-vsh: title launch %s via Lnc gate (direct)\n",
			     launched_title.c_str());
			// ps5-vsh: direct title boot (no ShellCore launcher): enable
			// title-scoped HLE workarounds. ONLY for --launch-title runs:
			// chain/sys boots under system_b (SysCore.elf) must keep legacy
			// semantics, or ShellCore takes fatal branches (0x9111394F9).
			if (!options.launch_title.empty()) {
				Libs::SetDirectTitleBoot(true);
			}
			// ps5-vsh: exercise the Lnc pending-launch queue end to end for
			// single-shot title launches (ShellCore-as-main will enqueue
			// instead of main.cpp). Enqueue -> take -> resolve -> remount.
			Libs::LncUtil::Lnc::EnqueuePendingLaunch(launched_title, options.app0_dir,
			                                         options.guest_args, 100);
			Libs::LncUtil::Lnc::PendingLaunch pl {};
			if (Libs::LncUtil::Lnc::TakePendingLaunch(&pl)) {
				Loader::ChildLaunchSpec spec {};
				spec.app0_dir = pl.app0_dir;
				if (spec.app0_dir.empty() && !options.fw_root.empty()) {
					Loader::ProbeChildTitle(options.fw_root / "system_b" / "vsh" / "app",
					                         pl.titleId, &spec);
					if (!Loader::IsLaunchableTitle(spec.app0_dir)) {
						Loader::ProbeChildTitle(options.fw_root / "system_ex_b" / "app",
						                         pl.titleId, &spec);
					}
					// ps5-vsh: RNPS system titles live under
					// system_ex_b/rnps/apps/<title> (no eboot.bin).
					if (!Loader::IsLaunchableTitle(spec.app0_dir)) {
						Loader::ProbeChildTitle(
						    options.fw_root / "system_ex_b" / "rnps" / "apps",
						    pl.titleId, &spec);
					}
				}
				if (Loader::IsLaunchableTitle(spec.app0_dir) &&
				    Loader::RemountApp0(spec.app0_dir)) {
					effective_param_json = spec.app0_dir / "sce_sys" / "param.json";
					effective_app_id = pl.appId;
					LOGF("ps5-vsh: Lnc queue drained: %s appId=%u args=%zu\n",
					     pl.titleId.c_str(), pl.appId, pl.args.size());
					// ps5-vsh: ShellCore-driven argv wins over CLI args for
					// the re-entered child (e.g. NPXS40038 videoCoreName).
					if (!pl.args.empty()) {
						effective_guest_args = pl.args;
					}
				} else {
					LOGF("ps5-vsh: Lnc queue entry unresolvable: %s\n",
					     pl.titleId.c_str());
				}
			}
		}
	}

	// ps5-vsh: firmware mounts for system titles (MONO_PATH references
	// /common_ex/lib, /common/lib; shell state lives under /data).
	if (!options.fw_root.empty()) {
		const auto mount_if_present = [](const std::filesystem::path& dir, const char* point,
		                                   bool read_only = false) {
			if (Common::File::IsDirectoryExisting(dir)) {
				Libs::LibKernel::FileSystem::Mount(dir, point, read_only);
			}
		};
		mount_if_present(options.fw_root / "system_b" / "common", "/common", true);
		mount_if_present(options.fw_root / "system_ex_b" / "common_ex", "/common_ex", true);
		mount_if_present(options.fw_root / "system_b", "/system", true);
		mount_if_present(options.fw_root / "system_ex_b", "/system_ex", true);
		mount_if_present(options.fw_root / "preinst", "/preinst", true);
		// ps5-vsh: "/" fallback (see early block above; repeated here for
		// the main mount block which re-mounts after the Lnc drain).
		// Same empty-dir backing: unmapped root children stay ENOENT.
		mount_if_present("_TempData/empty_root", "/");
		std::string fw_title_id;
		if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &fw_title_id) ||
		    fw_title_id.empty()) {
			fw_title_id = "UNKNOWN";
		}
		MountOrCreateDir("_FwData/" + fw_title_id, "/data");
	}

	MountSandboxDirs();

	// ps5-vsh: user-supplied extra mounts (voldemort parity: the staged
	// ShellUI tree). Mounted after the firmware/sandbox mounts; a child
	// mount overrides its parent regardless of insertion order, so e.g.
	// /app0/psm/Application=<stage> wins over /app0=<sys dir>.
	for (const auto& [point, dir]: options.extra_mounts) {
		// ps5-vsh: fw-backed staging (e.g. /app0/psm) mounts read-only;
		// workspace staging and stores stay writable.
		const bool read_only = IsUnderFwRoot(dir, options.fw_root);
		Libs::LibKernel::FileSystem::Mount(dir, point, read_only);
		LOGF("ps5-vsh: extra mount %s -> %s%s\n", point.c_str(),
		     Common::PathToString(dir).c_str(), read_only ? " (ro)" : "");
	}

	Loader::AddContentLoad(options.addcont_dir, options.addcont, title_id);
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	auto* main_program = LoadElf(options.elf);
	// In cohost boots --param-json may describe the staged /app0 ShellUI,
	// while the main ELF is SysCore. System executables use their own metadata.
	if (main_program->file_name.filename() != "eboot.bin") {
		effective_param_json = main_program->file_name.parent_path() / "sce_sys" / "param.json";
		effective_app_id = 0;
	}
	const auto is_system_ex = [&](const std::filesystem::path& executable,
	                              const std::filesystem::path& metadata) {
		return !options.fw_root.empty() &&
		       (IsUnderFwRoot(executable, options.fw_root / "system_ex_b") ||
		        IsUnderFwRoot(metadata, options.fw_root / "system_ex_b"));
	};
	Libs::LncUtil::Lnc::RegisterProgramLaunch(main_program, effective_param_json,
	    is_system_ex(main_program->file_name, effective_param_json), effective_app_id, options.fw_root);
	if (options.vsh_session) { Loader::ConfigureBootProcessIdentity(main_program); }

	// ps5-vsh: load co-hosted ELFs; entries launch inside Execute after
	// relocation + module start (threads spawned there would race init).
	std::vector<Loader::CoElfLaunch> co_elfs;
	for (const auto& spec : options.co_elfs) {
		auto* program = LoadElf(spec.path, /*dbg_print_reloc*/ true);
		if (program == nullptr || program->elf == nullptr) {
			continue;
		}
		// ps5-vsh: shared libraries (sprx) load+relocate for symbols but
		// never spawn entry threads: their ELF entry is a relative offset,
		// and jumping to it unmapped kills the process (0x10580 execute
		// fault in chainSUI runs).
		if (program->elf->IsShared()) {
			continue;
		}
		const auto co_param = program->boot_credential_admission && !program->boot_credential_admission->IsCoreImage() &&
		                         !options.param_json.empty() ? options.param_json :
		                         program->file_name.parent_path() / "sce_sys" / "param.json";
		Libs::LncUtil::Lnc::RegisterProgramLaunch(program, co_param,
		    is_system_ex(program->file_name, co_param), 0, options.fw_root);
		if (options.vsh_session) { Loader::ConfigureBootProcessIdentity(program); }
		const uint64_t entry = program->elf->GetEntry() + program->base_vaddr;
		if (entry != 0) {
			Loader::CoElfLaunch launch {entry, spec.path.filename().string(), spec.args};
			launch.program = program;
			co_elfs.push_back(std::move(launch));
		}
	}

	// ps5-vsh: opt-in thread sampler (host env KYTY_THREAD_SAMPLER=1):
	// poor-man's profiler for stalls with no syscall footprint.
	if (const char* sampler = std::getenv("KYTY_THREAD_SAMPLER");
	    sampler != nullptr && sampler[0] != '\0' && sampler[0] != '0') {
		std::thread([] {
			uint64_t dumped = 0;
			unsigned sweep   = 0;
			long     ms      = 10000;
			if (const char* v = std::getenv("KYTY_SAMPLER_MS"); v != nullptr && v[0] != '\0') {
				ms = std::atol(v);
				if (ms < 100) {
					ms = 100;
				}
			}
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(ms));
				Libs::LibKernel::PthreadLogThreadSample();
				// ps5-vsh: incremental touch-ring dump (Mono stall
				// triage): new guarded-page touches since last sweep.
				if ((++sweep % 6) == 0) {
					const uint64_t n = Libs::LibcInternal::Ps5VshTouchCount();
					if (n > dumped) {
						Libs::LibcInternal::Ps5VshDumpTouchesSince(dumped);
						dumped = n;
					}
				}
			}
		}).detach();
		LOGF("ps5-vsh: thread sampler active (10s)\n");
	}

	Loader::ConfigureRuntimeLaunch(options);
	Execute(options.game_patch, effective_guest_args, std::move(co_elfs));
}

} // namespace Emulator
