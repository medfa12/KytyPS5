#include "loader/runtimeChildHost.h"
#include "loader/runtimeChildManifest.h"
#include "loader/runtimeApplicationPlan.h"
#include "graphics/presentation/window/runtimeWindowBridge.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/controller.h"
#include "libs/audio.h"
#include "libs/libShellFocus.h"
#include "common/logging/log.h"
#include "emulator.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Loader {
namespace {
namespace Lnc = Libs::LncUtil::Lnc;
namespace Child = RuntimeChild;
constexpr int Invalid = static_cast<int>(0x80020016u), Io = static_cast<int>(0x80020005u), Busy = static_cast<int>(0x80020010u);
constexpr int Unsupported = static_cast<int>(0x8002002du), Canceled = static_cast<int>(0x80020055u);
#if defined(_WIN32)
struct Run {
	Child::Key key;
	std::unique_ptr<Child::Process> process;
	int guest_pid = -1;
	int retire_result = 0;
	uint64_t focus_epoch = 0;
	Child::PadOutput last_output;
	uint64_t ps_pulses = 0;
	bool closing = false, entry_reported = false, foreground = false, controller = false;
};
class Host {
public:
	explicit Host(const Emulator::RunOptions& options): options_(options) {
		inputs_.extra_mounts = options.extra_mounts; inputs_.fw_root = options.fw_root; inputs_.allow_firmware_titles = true;
		wchar_t executable[32768] {}; const auto n = GetModuleFileNameW(nullptr, executable, 32768);
		if (n && n < 32768) { executable_ = std::filesystem::path(std::wstring(executable, n)); }
	}
	void Start() { pump_ = std::jthread([this](std::stop_token token) { Work(token); }); }
	~Host() { pump_.request_stop(); changed_.notify_all(); if (pump_.joinable()) { pump_.join(); } }
	int Launch(const Lnc::PendingLaunch& request) {
		const auto parent_window = Libs::Graphics::RuntimeParentWindow();
		if (!parent_window) { return Unsupported; }
		const auto resolved = ResolveRuntimeApplicationPlan(inputs_, request.titleId);
		if (!resolved.plan || resolved.plan->route != RuntimeApplicationRoute::Native || executable_.empty()) { return Unsupported; }
		auto run = std::make_shared<Run>(); run->key.app = request.appId; run->key.sequence = request.seq;
		if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&run->key.nonce_lo), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !run->key.Valid()) { return Io; }
		std::unique_lock lock(mutex_);
		const auto installed = "/user/app/" + request.titleId, metadata = "/user/appmeta/" + request.titleId;
		// Classify a resolved native title before any admission rejection. ShellUI
		// ignores Unknown-type launch events, leaving its splash waiting forever.
		if (!Lnc::RegisterRuntimeLaunchPath(request.appId, installed + "/eboot.bin", metadata)) { return Invalid; }
		if (active_) {
			const int result = Lnc::RuntimeLaunchConflict(request.appId, active_->key);
			LOGF("VSH subprocess: launch conflict app=%u existing=%u result=0x%08x\n", request.appId, active_->key.app, static_cast<unsigned>(result));
			return result;
		}
		Child::ManifestInput input; input.title = request.titleId; input.app = resolved.plan->app0_path;
		input.metadata = resolved.plan->metadata_path; input.firmware = options_.fw_root;
		input.output = std::filesystem::current_path() / "launches" / "vsh";
		input.guest_arguments = request.args; input.keymap = options_.config.keymap;
		input.user = request.user_id >= 0 ? request.user_id : options_.config.user_id;
		input.user_name = options_.config.user_name; input.language = options_.config.console_language;
		input.network_connected = options_.config.network_connected;
		Child::Spawn spawn; std::string error;
		if (!Child::BuildManifest(input, run->key, &spawn, &error)) { LOGF("VSH subprocess: manifest refused: %s\n", error.c_str()); return Io; }
		spawn.executable = executable_;
		spawn.parent_window = parent_window;
		spawn.composed_frames = true;
		spawn.arguments.insert(spawn.arguments.end(), {L"--vsh-parent-window", std::to_wstring(parent_window),
		    L"--vsh-parent-pid", std::to_wstring(Libs::Graphics::RuntimeParentPid())});
		Libs::LibKernel::FileSystem::Mount(resolved.plan->app0_path, installed, true);
		Libs::LibKernel::FileSystem::Mount(resolved.plan->metadata_path.parent_path(), metadata, true);
		if (!Lnc::RegisterRuntimeProcess(run->key, resolved.plan->application_category_type.value_or(0)) || !Lnc::RuntimeLaunchAccepted(request.appId)) { return Invalid; }
		uint32_t native_error = 0; run->process = Child::Process::Create(spawn, &native_error);
		if (!run->process) {
			// No OS child exists; keyed accepted-start failure is now final. The
			// generic unkeyed callback intentionally cannot mutate this generation.
			lock.unlock(); Lnc::RuntimeProcessRetired(run->key, Io);
			LOGF("VSH subprocess: creation failed app=%u native=%u\n", request.appId, native_error); return Io;
		}
		Child::SetFrameSource(run->process->Frames(), run->key, false);
		active_ = run; changed_.notify_all();
		LOGF("VSH subprocess: created app=%u seq=%llu host_pid=%u (not guest entry/frame readiness)\n", request.appId,
		    static_cast<unsigned long long>(request.seq), run->process->HostPid());
		return 0;
	}
	int Close(const Lnc::RuntimeCloseRequest& request) {
		std::unique_lock lock(mutex_);
		if (!active_ || request.process_token != active_->key || request.app_id != active_->key.app || request.launch_seq != active_->key.sequence ||
		    request.owner_program_id != -1 || request.owner_lifetime) { return Invalid; }
		auto run = active_;
		run->closing = true; Libs::Controller::StopRuntimePadOutput();
		run->process->Close(request.force); changed_.notify_all();
		// Native KillAppSync continues the replacement launch when this call
		// returns. Wait for the pump's actual process/job retirement, releasing
		// the host mutex while waiting. LNC queues KillStart before deferred End.
		if (!changed_.wait_for(lock, std::chrono::seconds(10), [&] { return active_ != run; })) {
			run->process->Close(true);
			return Io;
		}
		return run->retire_result;
	}
	int Focus(const Lnc::RuntimeFocusRequest& request) {
		std::unique_lock lock(mutex_); auto run = active_;
		if (!run && request.app_id == -1) { return 0; }
		if (!run || run->closing || (request.app_id != -1 && request.process_token != run->key)) { return Invalid; }
		constexpr auto required = Child::EmbeddedWindow | Child::BrokeredInput | Child::FocusAudio | Child::ComposedFrames;
		const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
		// Focus is needed to leave background execution and produce a first
		// frame (notably Starboard). Authenticate window/entry/control readiness
		// here; rendered-pixel readiness is a separate presentation observation.
		while (request.app_id != -1 && active_ == run && !run->closing && !run->process->Retired() && !run->process->FocusReady()) {
			if (changed_.wait_until(lock, ready_deadline) == std::cv_status::timeout) { return Busy; }
		}
		if (active_ != run || run->closing || run->process->Retired()) { return Canceled; }
		if ((run->process->Capabilities() & required) != required) { return Unsupported; }
		const bool foreground = request.controller ? run->foreground : request.app_id != -1;
		const bool controller = request.controller ? request.app_id != -1 : run->controller;
		// Break-before-make: neither side owns ordinary input/audio during a
		// transaction. On failure the child is retired before shell restoration.
		Libs::Controller::SetRuntimeControllerFocus(false, request.epoch);
		if (!Libs::Audio::SetRuntimeAudioFocus(false)) { run->closing = true; run->process->Close(true); return Io; }
		run->focus_epoch = request.epoch;
		auto held = Libs::Controller::RuntimePadSnapshot(); held.epoch = request.epoch; run->process->SendPad(held);
		if (!run->process->Focus(foreground, controller, request.epoch, &held)) { run->closing = true; run->process->Close(true); return Busy; }
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
		while (active_ == run && !run->process->Retired() && run->process->FocusAcknowledged() != request.epoch) {
			if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) { run->closing = true; run->process->Close(true); return Canceled; }
		}
		if (active_ != run || run->process->Retired()) { return Canceled; }
		const auto result = run->process->FocusResult();
		if (!result) {
			run->foreground = foreground; run->controller = controller;
			Child::SetFrameVisibility(run->key, foreground);
			Libs::Controller::SetRuntimeControllerFocus(!(foreground && controller), request.epoch);
			// Control Center owns shell interaction sounds while the displayed
			// application keeps its own audio. Display-only arbitration muted the
			// parent throughout the overlay, including every navigation sound.
			if (!Libs::Audio::SetRuntimeAudioFocus(!(foreground && controller))) { run->closing = true; run->process->Close(true); return Io; }
			Libs::Graphics::RequestRuntimeParentKeyboardFocus();
		} else { run->closing = true; run->process->Close(true); }
		return result;
	}
private:
	void Work(std::stop_token token) {
		while (!token.stop_requested()) {
			try {
			std::shared_ptr<Run> run; Child::Tick tick; bool entry = false; int guest_pid = -1, result = 0;
			bool ps = false;
			{
				std::lock_guard lock(mutex_); run = pending_retirement_;
			}
			if (run) {
				// Admission was released only AFTER actual process/job retirement,
				// BEFORE publishing completion. Keep a retry pin if event allocation
				// fails; the next launch does not race an obsolete active slot.
				Lnc::RuntimeProcessRetired(run->key, run->retire_result);
				std::lock_guard lock(mutex_); if (pending_retirement_ == run) { pending_retirement_.reset(); }
				continue;
			}
			{
				std::unique_lock lock(mutex_); changed_.wait_for(lock, std::chrono::milliseconds(10));
				run = active_; if (!run) { continue; }
				tick = run->process->Poll();
				if (!tick.retired) {
					auto report = Libs::Controller::RuntimePadSnapshot(); report.epoch = run->focus_epoch;
					run->process->SendPad(report);
					ps = run->process->ReadPs(&run->ps_pulses) && run->foreground && !run->closing && Libs::Graphics::RuntimeWindowTreeForeground();
					if (run->process->ReadPadOutput(&run->last_output)) {
						Libs::Controller::ApplyRuntimePadOutput(run->last_output, run->focus_epoch,
						    !run->closing && run->foreground && run->controller && run->process->FocusAcknowledged() == run->focus_epoch);
					}
				}
				for (size_t i = 0; i != tick.count; ++i) { if (tick.messages[i].kind == Child::Kind::Entry) { run->guest_pid = static_cast<int>(tick.messages[i].value[0]); } }
				entry = run->process->Entered() && !run->entry_reported && !run->closing; guest_pid = run->guest_pid;
				// A requested process kill's OS exit code is not the LNC teardown
				// result. Successful retirement completes close even after fallback.
				if (tick.retired && !run->closing && (run->process->Error() || run->process->ExitCode())) { result = Io; }
				changed_.notify_all();
			}
			// No host registry/control locks while queuing native shell events.
			if (ps) {
				Libs::Controller::PulseRuntimeSystemPs();
				Libs::ShellFocus::ShellFocusHost::PulsePsButton(0);
			}
			if (entry) { const bool reported = Lnc::RuntimeProcessStarted(run->key, guest_pid); std::lock_guard lock(mutex_); run->entry_reported = reported || run->closing; }
			if (tick.retired) {
				std::lock_guard lock(mutex_);
				Child::RetireFrameSource(run->key);
				Libs::Controller::SetRuntimeControllerFocus(true, run->focus_epoch);
				Libs::Audio::SetRuntimeAudioFocus(true);
				Libs::Graphics::RequestRuntimeParentKeyboardFocus();
				LOGF("VSH subprocess: actual exit app=%u seq=%llu host_pid=%u exit=%u error=%u; ShellUI retained\n", run->key.app,
				    static_cast<unsigned long long>(run->key.sequence), run->process->HostPid(), run->process->ExitCode(), run->process->Error());
				run->retire_result = result; pending_retirement_ = run;
				if (active_ == run) { active_.reset(); } changed_.notify_all();
			}
			} catch (...) {
				// HOST pump failures never discard a running process or fabricate
				// LNC completion. Continue polling its original retained handles.
				std::lock_guard lock(mutex_);
				if (active_) { active_->closing = true; active_->process->Close(true); }
			}
		}
	}
	Emulator::RunOptions options_;
	RuntimeApplicationPlanOptions inputs_;
	std::filesystem::path executable_;
	std::mutex mutex_;
	std::condition_variable changed_;
	std::shared_ptr<Run> active_;
	std::shared_ptr<Run> pending_retirement_;
	std::jthread pump_; // Destroy/join before member storage disappears.
};
#endif
}
void ConfigureVshSubprocessLaunch(const Emulator::RunOptions& options) {
	Lnc::ConfigureVshRuntime(true);
#if defined(_WIN32)
	auto host = std::make_shared<Host>(options); host->Start();
	Lnc::SetRuntimeLaunchHandler([host](const auto& request) { return host->Launch(request); });
	Lnc::SetRuntimeCloseHandler([host](const auto& request) { return host->Close(request); });
	Lnc::SetRuntimeFocusHandler([host](const auto& request) { return host->Focus(request); });
	LOGF("VSH embedded subprocess backend configured (parent pad broker, full Home/resume, focus audio)\n");
#else
	Lnc::SetRuntimeLaunchHandler([](const auto&) { return Unsupported; });
	Lnc::SetRuntimeCloseHandler({}); Lnc::SetRuntimeFocusHandler({});
#endif
}
}
