#include "libs/libLncUtil.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace Libs::LncUtil { void InitLncUtil_1(Loader::SymbolDatabase*); }

namespace {
namespace Lnc = Libs::LncUtil::Lnc;
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "LncAppStatusTests: %s\n", message);
		std::abort();
	}
}
using GetStatus = int(KYTY_SYSV_ABI *)(Lnc::AppStatus*);
using LaunchApp = int(KYTY_SYSV_ABI *)(const char*, const char* const*, const Lnc::LaunchAppParam*);
using KillApp = int(KYTY_SYSV_ABI *)(int32_t);
using KillAppWithReason = int(KYTY_SYSV_ABI *)(int32_t, int32_t);
}

int main() {
	Loader::SymbolDatabase symbols;
	Libs::LncUtil::InitLncUtil_1(&symbols);
	const auto* config_symbol = symbols.FindByNid("cHlIo6CoRTA", Loader::SymbolType::Func);
	Check(config_symbol != nullptr, "Spmem allocator config export missing");
	const auto get_config = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*)>(config_symbol->vaddr);
	Check(get_config(nullptr) == 0 && get_config("hermes") == 0 && get_config("monoNonGc") == 0,
	      "allocator config is a one-argument query with no configured overrides");
	const auto* symbol = symbols.FindByNid("DxRki7T2E44", Loader::SymbolType::Func);
	Check(symbol != nullptr, "GetAppStatus export missing");
	const auto get_status = reinterpret_cast<GetStatus>(symbol->vaddr);
	Check(get_status(nullptr) == static_cast<int>(0x80940005), "firmware null contract");

	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty-lnc-status-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	const auto installed_shell = root / "system_ex_b" / "app" / "NPXS40087";
	std::filesystem::create_directories(installed_shell);
	std::ofstream(installed_shell / "eboot.bin") << "installation catalog fixture";
	std::ofstream(root / "shell.json") << R"({"titleId":"NPXS40087","applicationCategoryType":16777216,"attribute":4294967295,"masterVersion":"01.00","contentVersion":"01.002.003"})";
	std::ofstream(root / "other.json") << R"({"titleId":"NPXS49999","applicationCategoryType":16777216})";
	Loader::Program shell, other, unknown;
	shell.unique_id = 700001;
	other.unique_id = 700002;
	unknown.unique_id = 700003;
	shell.file_name = root / "sys" / "eboot.bin";
	other.file_name = root / "other" / "eboot.bin";
	unknown.file_name = root / "sys" / "eboot.bin";

	struct Guarded {
		uint64_t before = 0x1122334455667788;
		Lnc::AppStatus status;
		uint64_t after = 0x8877665544332211;
	};
	auto buffer = std::make_unique<Guarded>();
	Check(reinterpret_cast<uintptr_t>(&buffer->status) > UINT32_MAX, "test requires a full-width output pointer");
	std::memset(&buffer->status, 0xa5, sizeof(buffer->status));
	std::array<unsigned char, 64> saved;
	std::memcpy(saved.data(), &buffer->status, saved.size());
	Loader::RuntimeLinker::SetCurrentProgram(&unknown);
	Check(get_status(&buffer->status) == static_cast<int>(0x80940001), "unregistered caller must fail");
	Check(std::memcmp(saved.data(), &buffer->status, saved.size()) == 0, "failure changed output");

	Lnc::RegisterProgramLaunch(&shell, root / "shell.json", false, 0, root);
	Lnc::RegisterProgramLaunch(&other, root / "other.json", false);
	Loader::RuntimeLinker::SetCurrentProgram(&shell);
	Check(get_status(&buffer->status) == 0, "first ShellUI query");
	const auto shell_id = buffer->status.app_id;
	Check(buffer->status.app_type == 1 && std::strcmp(buffer->status.title_id, "NPXS40087") == 0,
	      "selected metadata must win over generic sys staging path");
	Check(buffer->status.app_category_type == 0x01000000 && buffer->status.is_system_ex_app == 1, "metadata/provenance");
	Check(buffer->status.app_attr == 0, "param attribute is not LNC launch flags");
	Check(buffer->status.app_boot_mode == -1 && buffer->status.app_boot_sub_mode == -1, "unknown boot modes");
	Check(std::strcmp(buffer->status.app_ver, "01.00") == 0 &&
	      std::strcmp(buffer->status.content_version, "01.002.003") == 0, "distinct version fields");
	Check(buffer->before == 0x1122334455667788 && buffer->after == 0x8877665544332211, "64-byte output boundary");
	Loader::RuntimeLinker::SetCurrentProgram(&other);
	Check(get_status(&buffer->status) == 0 && buffer->status.app_type == 0 &&
	      buffer->status.app_id != shell_id && buffer->status.is_system_ex_app == 0,
	      "cohost must not inherit ShellUI identity or category classification");
	const auto other_id = buffer->status.app_id;

	// Registration is stable once published, even if a later global title changes.
	Lnc::RegisterProgramLaunch(&shell, root / "other.json", false);
	std::atomic<bool> valid {true};
	const auto query_thread = [&](Loader::Program* owner, int32_t id, int32_t type) {
		// Same owner propagation used by guest pthread entry; no global linker lookup.
		Loader::RuntimeLinker::SetCurrentProgram(owner);
		for (int i = 0; i < 1000; ++i) {
			Lnc::AppStatus result;
			if (get_status(&result) != 0 || result.app_id != id || result.app_type != type) {
				valid = false;
			}
		}
		Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	};
	std::thread a(query_thread, &shell, shell_id, 1);
	std::thread b(query_thread, &other, other_id, 0);
	a.join();
	b.join();
	Check(valid, "concurrent caller ownership/stable registration");

	// A real LNC launch retains both its returned app ID and requesting caller
	// when the host later registers the loaded child executable.
	const auto* launch_symbol = symbols.FindByNid("+nRJUD-7qCk", Loader::SymbolType::Func);
	Check(launch_symbol != nullptr, "LaunchApp export missing");
	const auto launch = reinterpret_cast<LaunchApp>(launch_symbol->vaddr);
	const auto* kill_symbol = symbols.FindByNid("SZ2uH5Abws8", Loader::SymbolType::Func);
	const auto* kill_reason_symbol = symbols.FindByNid("RBlEzB--JeA", Loader::SymbolType::Func);
	const auto* force_kill_symbol = symbols.FindByNid("vquYrvImjPg", Loader::SymbolType::Func);
	Check(kill_symbol && kill_reason_symbol && force_kill_symbol, "13.60 native app-close exports");
	const auto kill = reinterpret_cast<KillApp>(kill_symbol->vaddr);
	const auto kill_reason = reinterpret_cast<KillAppWithReason>(kill_reason_symbol->vaddr);
	const auto force_kill = reinterpret_cast<KillApp>(force_kill_symbol->vaddr);
	Loader::SymbolResolve native_close;
	native_close.name = "RBlEzB--JeA";
	native_close.library = "LncUtil";
	native_close.library_version = 1;
	native_close.module = "SystemService";
	native_close.module_version_major = 1;
	native_close.module_version_minor = 1;
	native_close.type = Loader::SymbolType::Func;
	Check(symbols.Find(native_close) && symbols.Find(native_close)->vaddr == kill_reason_symbol->vaddr,
	      "native close resolves exact LncUtil/SystemService identity, not only NID fallback");
	Check(symbols.FindByNid("cCod+B3EdhI", Loader::SymbolType::Func) == nullptr,
	      "LNC client terminate is not an application-close alias");
	Check(kill(shell_id) == Libs::LibKernel::KERNEL_ERROR_ENOTSUP, "direct/session close must not terminate emulator");
	Loader::RuntimeLinker::SetCurrentProgram(&shell);
	Lnc::LaunchAppParam param {40, 0x10000001, 0x40001, 1,
	                         0xfedcba9876543210, 0x123456789abcdef0, 1, {}};
	const char* argv[] = {"videoCoreName", "", "release", nullptr};
	const int child_id = launch("NPXS49998", argv, &param);
	Check(child_id > 0, "three-argument launch returns app ID");
	Lnc::PendingLaunch pending;
	Check(Lnc::TakePendingLaunch(&pending) && pending.appId == child_id, "launch queue ID");
	Check(pending.args == std::vector<std::string>({"videoCoreName", "", "release"}), "null-terminated argv preserves empty entries");
	Check(std::memcmp(&pending.param, &param, sizeof(param)) == 0 &&
	      pending.user_id == param.user_id && pending.launch_request_app_id == shell_id,
	      "startup queue preserves complete launch parameters/requester");
	Check(launch("NPXS49998", nullptr, nullptr) == static_cast<int>(0x80940005), "null launch parameter");
	Check(launch("SHORT", nullptr, &param) == static_cast<int>(0x80940005), "invalid title length");
	std::string oversized(4096, 'a');
	const char* oversized_argv[] = {oversized.c_str(), nullptr};
	Check(launch("NPXS49998", oversized_argv, &param) == static_cast<int>(0x80940005), "argv 4096-byte limit includes terminators");
	Check(!Lnc::HasPendingLaunch(), "invalid requests must not queue launches");
	// Independent run18 firmware fixture: the initializer advertises 40 even
	// though the Pack=4/I1 marshaller copies only this 36-byte field payload.
	const uint8_t native_payload[36] = {
	    0x28, 0, 0, 0, 1, 0, 0, 0x10, 0, 0, 0, 0, 1, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0x38, 0xc7, 0x4c, 0xe1, 0x9b, 0x3a, 0xdf, 0x48,
	    0, 0, 0, 0};
	Lnc::LaunchAppParam native_param {};
	std::memcpy(&native_param, native_payload, sizeof(native_payload));
	const int native_id = launch("NPXS49997", nullptr, &native_param);
	Lnc::PendingLaunch native_pending;
	Check(native_id > 0 && Lnc::TakePendingLaunch(&native_pending) && native_pending.appId == native_id &&
	      native_pending.param.context_id == 0x48df3a9be14cc738 && native_pending.user_id == 0x10000001,
	      "native advertised-size40 payload reaches launch queue with intact fields");
	native_param.size = 36;
	Check(launch("NPXS49997", nullptr, &native_param) == static_cast<int>(0x80940005) && !Lnc::HasPendingLaunch(),
	      "metadata payload length is not the firmware advertised ABI size");
	const auto* launched_symbol = symbols.FindByNid("IGrJsPNL6n4", Loader::SymbolType::Func);
	Check(launched_symbol != nullptr, "IsAppLaunched export missing");
	const auto is_launched = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, uint8_t*)>(launched_symbol->vaddr);
	struct ByteGuard { uint8_t before = 0x61; uint8_t value = 0xa5; uint8_t after[7] {1, 2, 3, 4, 5, 6, 7}; } byte;
	const auto launched = [&](const char* title) {
		byte.value = 0xa5;
		Check(is_launched(title, &byte.value) == 0, "IsAppLaunched returns status, not boolean");
		const uint8_t expected[] {1, 2, 3, 4, 5, 6, 7};
		Check(byte.before == 0x61 && std::memcmp(byte.after, expected, sizeof(expected)) == 0,
		      "IsAppLaunched writes exactly one byte");
		Check(byte.value <= 1, "IsAppLaunched always initializes successful output");
		return byte.value != 0;
	};
	Check(launched("NPXS49998") && !launched("PPSA99999"), "title-based launched query");
	byte.value = 0xa5;
	Check(is_launched(nullptr, &byte.value) == static_cast<int>(0x80940005) && byte.value == 0xa5,
	      "invalid title leaves byte output untouched");
	Check(is_launched("NPXS49998", nullptr) == static_cast<int>(0x80940005), "null byte output rejected");
	Lnc::SetRuntimeLaunchHandler([&](const Lnc::PendingLaunch& request) {
		Check(request.titleId == "PPSA01783" && request.user_id == param.user_id &&
		      request.args == pending.args, "runtime backend receives owned launch data");
		return static_cast<int>(0x80940001);
	});
	Check(launch("PPSA01783", argv, &param) == static_cast<int>(0x80940001), "backend error reaches guest");
	Check(!Lnc::HasPendingLaunch(), "runtime rejection must not leave a startup queue request");
	Lnc::SetRuntimeLaunchHandler([](const Lnc::PendingLaunch&) { return 0; });
	const int runtime_id = launch("PPSA01783", argv, &param);
	Check(runtime_id > 0 && launched("PPSA01783"), "direct mode retains existing handler semantics");
	Lnc::RuntimeLaunchExited(runtime_id);
	Check(!launched("PPSA01783"), "exited runtime process is not reported running");
	Lnc::SetRuntimeLaunchHandler({});
	Loader::Program child;
	child.unique_id = 700004;
	child.file_name = root / "NPXS49998" / "eboot.bin";
	Lnc::RegisterProgramLaunch(&child, root / "missing.json", false, pending.appId);
	Loader::RuntimeLinker::SetCurrentProgram(&child);
	Check(get_status(&buffer->status) == 0 && buffer->status.app_id == static_cast<int32_t>(child_id) &&
	      buffer->status.launch_request_app_id == shell_id &&
	      buffer->status.app_attr == static_cast<uint32_t>(param.app_attr) &&
	      std::strcmp(buffer->status.title_id, "NPXS49998") == 0 && buffer->status.app_type == 0,
	      "child registration must preserve actual launch ownership");

	const auto* event_symbol = symbols.FindByNid("ZucoOmNsb7w", Loader::SymbolType::Func);
	const auto* focus_symbol = symbols.FindByNid("v7DYuX0G5TQ", Loader::SymbolType::Func);
	const auto* controller_symbol = symbols.FindByNid("X8gYbyLG1wk", Loader::SymbolType::Func);
	Check(event_symbol && focus_symbol && controller_symbol, "verified event/focus exports");
	Check(symbols.FindByNid("P7DYuX0G5TQ", Loader::SymbolType::Func) == nullptr, "unverified focus alias absent");
	const auto get_event = reinterpret_cast<int(KYTY_SYSV_ABI *)(void*)>(event_symbol->vaddr);
	const auto set_focus = reinterpret_cast<int(KYTY_SYSV_ABI *)(int32_t, uint64_t, uint64_t)>(focus_symbol->vaddr);
	const auto set_controller = reinterpret_cast<int(KYTY_SYSV_ABI *)(int32_t)>(controller_symbol->vaddr);
	struct EventGuard { uint32_t before = 0x12345678; Lnc::EventWire event; uint32_t after = 0x87654321; } wire;
	static_assert(offsetof(EventGuard, after) == 4 + 5740);
	const auto empty_event = [&] {
		std::array<uint8_t, sizeof(Lnc::EventWire)> before;
		std::memcpy(before.data(), &wire.event, before.size());
		Check(get_event(&wire.event) == Libs::LibKernel::KERNEL_ERROR_EAGAIN, "empty receive is EAGAIN");
		Check(std::memcmp(before.data(), &wire.event, before.size()) == 0, "empty receive preserves output");
	};
	const auto receive = [&](int32_t type) {
		Check(get_event(&wire.event) == 0 && wire.event.event_type == type, "event FIFO transition order");
		Check(wire.before == 0x12345678 && wire.after == 0x87654321, "13.60 event writes exactly 5740 bytes");
	};
	Check(get_event(nullptr) == static_cast<int>(0x80940005), "null event output");
	Check(set_focus(child_id, param.check_flag, param.context_id) == 0 && set_controller(child_id) == 0,
	      "direct focus compatibility");
	Check(Lnc::GetFocusedAppId() == -1 && Lnc::GetControllerFocusedAppId() == -1, "direct mode has no runtime focus state");
	empty_event();

	Lnc::ConfigureVshRuntime(true);
	Loader::RuntimeLinker::SetCurrentProgram(&shell);
	Check(kill(shell_id) == Libs::LibKernel::KERNEL_ERROR_ENOTSUP &&
	      force_kill(other_id) == Libs::LibKernel::KERNEL_ERROR_ENOTSUP &&
	      kill(-1) == static_cast<int>(0x80940005) && kill(unknown.unique_id) == static_cast<int>(0x80940005),
	      "close only accepts runtime LNC identities, not shell/global/module targets");
	Lnc::PendingLaunch runtime_request;
	Lnc::SetRuntimeLaunchHandler([&](const Lnc::PendingLaunch& request) { runtime_request = request; return 0; });
	char mutable_arg[] = "videoCoreName";
	const char* runtime_argv[] = {mutable_arg, "", "release", nullptr};
	auto mutable_param = param;
	const int vsh_id = launch("PPSA01784", runtime_argv, &mutable_param);
	mutable_arg[0] = 'X';
	mutable_param.context_id = 0;
	Check(runtime_request.args == pending.args && runtime_request.param.context_id == param.context_id,
	      "runtime request owns guest argv and parameter snapshots");
	Check(vsh_id > 0 && !launched("PPSA01784"), "handler acceptance is not successful startup");
	Check(!Lnc::RuntimeLaunchStarted(vsh_id), "startup cannot precede accepted callback");
	empty_event();
	std::string guest_path = "/user/app/PPSA01784/eboot.bin";
	Check(Lnc::RegisterRuntimeLaunchPath(vsh_id, guest_path, "/user/app/PPSA01784/sce_sys"), "resolved native guest paths");
	guest_path[1] = 'X';
	Check(!Lnc::RegisterRuntimeLaunchPath(vsh_id, "/changed/eboot.bin", "/changed/sce_sys"), "published guest path immutable");
	Check(Lnc::RuntimeLaunchAccepted(vsh_id) && !Lnc::RuntimeLaunchAccepted(vsh_id), "accepted callback is idempotent");
	Loader::Program runtime, runtime_module;
	runtime.unique_id = 700007;
	runtime.file_name = root / "PPSA01784" / "eboot.bin";
	runtime.application_owner = &runtime;
	Loader::GuestCallBoundaryStatus execution_status;
	const bool has_execution = Loader::InitializeApplicationExecution(&runtime, &execution_status);
	Check(has_execution || execution_status == Loader::GuestCallBoundaryStatus::Unsupported,
	      "real close-owner control must initialize on supported boundary platforms");
	const auto runtime_execution = Loader::GetApplicationExecution(&runtime);
	runtime_module.unique_id = 700008;
	runtime_module.application_owner = &runtime;
	Check(!Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module),
	      "unregistered runtime owner is neutral/overlaid at boot with no focus target");
	Check(Lnc::HasControllerFocus(nullptr) && !Lnc::IsSystemUiOverlaid(nullptr) &&
	      Lnc::HasControllerFocus(&shell), "no-target state preserves system observers");
	std::ofstream(root / "runtime.json") << R"({"titleId":"PPSA01784","applicationCategoryType":305419896,"attribute":4294967295})";
	Lnc::RegisterProgramLaunch(&runtime, root / "runtime.json", false, vsh_id);
	Loader::Program foreign_runtime;
	foreign_runtime.unique_id = 700009;
	foreign_runtime.application_owner = &foreign_runtime;
	Lnc::RegisterProgramLaunch(&foreign_runtime, root / "runtime.json", false, vsh_id);
	int32_t foreign_id = -1;
	Check(!Lnc::TryGetProgramAppId(&foreign_runtime, &foreign_id) && foreign_id == -1,
	      "a different runtime owner cannot overwrite a published app identity/generation");
	Check(!Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module) &&
	      set_controller(vsh_id) == static_cast<int>(0x80940005) &&
	      set_focus(vsh_id, 0, 0) == static_cast<int>(0x80940005),
	      "accepted but not running app cannot receive either focus");
	Loader::RuntimeLinker::SetCurrentProgram(&runtime_module);
	int32_t owner_id = -1;
	Check(Lnc::TryGetCurrentAppId(&owner_id) && owner_id == vsh_id && owner_id != runtime.unique_id,
	      "runtime modules resolve owner LNC ID, not module ID");
	Check(get_status(&buffer->status) == 0 && buffer->status.app_id == vsh_id && buffer->status.app_type == 5 &&
	      buffer->status.app_attr == static_cast<uint32_t>(param.app_attr) && buffer->status.launch_request_app_id == shell_id,
	      "runtime owner status preserves launch identity/attributes");
	receive(1);
	Check(wire.event.app_id == -1 && wire.event.app_local_pid == -1 && wire.event.existing_app_id == -1 &&
	      wire.event.error_code == 0 && wire.event.app_type == 5 && wire.event.app_category_type == 0,
	      "pending sentinel opens container without starting it; queued snapshot is immutable");
	Check(wire.event.launch_request_app_id == shell_id && wire.event.user_id == param.user_id &&
	      wire.event.app_attribute == static_cast<uint32_t>(param.app_attr) && wire.event.check_flags == param.check_flag &&
	      wire.event.context_id == param.context_id, "launch event owns full request identity");
	Check(std::strcmp(wire.event.path, "/user/app/PPSA01784/eboot.bin") == 0 &&
	      std::strcmp(wire.event.sce_sys_dir, "/user/app/PPSA01784/sce_sys") == 0,
	      "queued event owns immutable guest installed paths");
	const char packed_args[] = "videoCoreName\0\0release";
	Check(wire.event.argument_size == sizeof(packed_args) &&
	      std::memcmp(wire.event.arguments, packed_args, sizeof(packed_args)) == 0, "event argv byte serialization");
	empty_event();
	Check(Lnc::RuntimeLaunchStarted(vsh_id, 12345) && !Lnc::RuntimeLaunchStarted(vsh_id, 12345), "real startup once only");
	Check(launched("PPSA01784"), "real startup marks title launched");
	Check(!Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module),
	      "startup does not implicitly assign the no-target sentinel to a runtime app");
	receive(2);
	Check(wire.event.app_id == vsh_id && wire.event.app_local_pid == 12345 && wire.event.error_code == 0 &&
	      wire.event.app_category_type == 0x12345678,
	      "successful launch end uses real LNC identity/local process ID");
	Check(set_focus(runtime.unique_id, 0, 0) == static_cast<int>(0x80940005), "module ID is not an LNC focus ID");
	Check(set_focus(vsh_id, param.check_flag, param.context_id) == 0 && set_controller(vsh_id) == 0, "runtime focus operations");
	receive(0x1001);
	Check(wire.event.app_id == vsh_id && wire.event.check_flags == param.check_flag && wire.event.context_id == param.context_id,
	      "focus ABI preserves both full-width arguments");
	receive(0x1002);
	receive(0x1003);
	receive(0x1004);
	Check(Lnc::HasControllerFocus(&runtime_module) && !Lnc::IsSystemUiOverlaid(&runtime_module), "module inherits owner focus");
	Check(set_focus(shell_id, 0, 0) == 0 && set_controller(shell_id) == 0, "return focus to ShellUI LNC identity");
	Check(!Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module) &&
	      Lnc::HasControllerFocus(&shell) && !Lnc::IsSystemUiOverlaid(&shell), "firmware input/direct semantics survive overlay");
	receive(0x1001); receive(0x1002); receive(0x1003); receive(0x1004);
	Check(set_focus(-1, 0, 0) == 0 && set_controller(-1) == 0, "invalid-ID sentinel clears focus");
	Check(!Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module) &&
	      Lnc::HasControllerFocus(&shell) && Lnc::HasControllerFocus(nullptr),
	      "cleared focus enables system observation without enabling runtime clients");
	receive(0x1001); receive(0x1002); receive(0x1003); receive(0x1004);
	Check(set_focus(vsh_id, 0, 0) == 0 && set_controller(vsh_id) == 0, "focus runtime before exit");
	receive(0x1001); receive(0x1002); receive(0x1003); receive(0x1004);
	Check(kill(vsh_id) == Libs::LibKernel::KERNEL_ERROR_ENOTSUP && launched("PPSA01784") &&
	      Lnc::GetFocusedAppId() == vsh_id, "missing close backend cannot invent successful teardown");
	empty_event();
	Lnc::RuntimeCloseRequest close_request;
	const auto close_critical_depth = Common::GetHostLockCriticalCounter()->load();
	int close_calls = 0;
	Lnc::SetRuntimeCloseHandler([&](const Lnc::RuntimeCloseRequest& request) {
		++close_calls;
		close_request = request;
		Check(Common::GetHostLockCriticalCounter()->load() == close_critical_depth &&
		      Lnc::GetFocusedAppId() == vsh_id, "close backend runs outside registry lock/suspension guard");
		return Libs::LibKernel::KERNEL_ERROR_EBUSY;
	});
	Check(kill_reason(vsh_id, 2) == Libs::LibKernel::KERNEL_ERROR_EBUSY && close_calls == 1 &&
	      close_request.app_id == vsh_id && close_request.launch_seq == runtime_request.seq &&
	      close_request.owner_program_id == runtime.unique_id &&
	      close_request.owner_lifetime == (runtime_execution ? runtime_execution->Owner().token : 0) &&
	      close_request.reason == 2 && !close_request.force,
	      "native close ABI delegates immutable app/launch/owner identity and reason; backend rejection propagates");
	Check(launched("PPSA01784") && Lnc::GetControllerFocusedAppId() == vsh_id, "rejected close retains running state and focus");
	empty_event();
	Lnc::SetRuntimeCloseHandler([&](const Lnc::RuntimeCloseRequest& request) {
		++close_calls;
		close_request = request;
		Check(kill(vsh_id) == Libs::LibKernel::KERNEL_ERROR_EBUSY, "concurrent/reentrant close does not dispatch twice");
		return 0;
	});
	Check(kill_reason(vsh_id, 2) == 0 && close_calls == 2, "rejected close is retryable");
	receive(3);
	Check(wire.event.app_id == vsh_id && wire.event.app_local_pid == 12345 && wire.event.error_code == 0,
	      "accepted close emits owned AppKillStart, not exit");
	Check(kill(vsh_id) == 0 && force_kill(vsh_id) == 0 && close_calls == 2 &&
	      launched("PPSA01784") && Lnc::GetFocusedAppId() == vsh_id && Lnc::HasControllerFocus(&runtime_module),
	      "repeat/forced close is idempotent; request alone cannot remove focus or report exit");
	empty_event();
	Check(Lnc::RuntimeLaunchQuiescentExit(vsh_id) && !Lnc::RuntimeLaunchQuiescentExit(vsh_id), "quiescent exit once only");
	Check(Lnc::GetFocusedAppId() == -1 && Lnc::GetControllerFocusedAppId() == -1 &&
	      !Lnc::HasControllerFocus(&runtime_module) && Lnc::IsSystemUiOverlaid(&runtime_module) &&
	      set_controller(vsh_id) == static_cast<int>(0x80940005) &&
	      set_focus(vsh_id, 0, 0) == static_cast<int>(0x80940005),
	      "exit clears focus without reactivating the exited app and rejects refocus");
	Check(!launched("PPSA01784"), "quiescent exit ends launched state");
	receive(4);
	Check(wire.event.app_id == vsh_id && wire.event.app_local_pid == 12345, "exit snapshot retains owned identity");
	Check(kill(vsh_id) == 0 && close_calls == 2, "repeat close after quiescence cannot call a retired backend");
	empty_event();

	// Relaunching the same title creates a distinct generation. A late old close
	// or completion must never clear the replacement's focus or launched state.
	const int replacement_id = launch("PPSA01784", argv, &param);
	Check(replacement_id > 0 && replacement_id != vsh_id &&
	      Lnc::RegisterRuntimeLaunchPath(replacement_id, "/user/app/PPSA01784/eboot.bin", "/user/appmeta/PPSA01784") &&
	      Lnc::RuntimeLaunchAccepted(replacement_id) && Lnc::RuntimeLaunchStarted(replacement_id, 23456),
	      "same-title replacement has a new LNC launch generation");
	receive(1); receive(2);
	Check(set_focus(replacement_id, 0, 0) == 0 && set_controller(replacement_id) == 0, "replacement owns focus");
	receive(0x1001); receive(0x1002); receive(0x1003); receive(0x1004);
	Check(kill(vsh_id) == 0 && !Lnc::RuntimeLaunchQuiescentExit(vsh_id) && close_calls == 2 &&
	      Lnc::GetFocusedAppId() == replacement_id && Lnc::GetControllerFocusedAppId() == replacement_id && launched("PPSA01784"),
	      "stale generation cannot stop or defocus replacement");
	empty_event();
	Check(Lnc::RuntimeLaunchQuiescentExit(replacement_id), "normal app exit needs no close request");
	receive(4);
	Check(wire.event.app_id == replacement_id, "normal exit belongs to replacement only");
	empty_event();

	// A real backend rejection gets a failed end, never a fabricated successful process.
	const int launch_error = static_cast<int>(0x80940001);
	Lnc::SetRuntimeLaunchHandler([&](const Lnc::PendingLaunch& request) { runtime_request = request; return launch_error; });
	Check(launch("PPSA01785", argv, &param) == launch_error && !launched("PPSA01785"), "failed launch is not running");
	Check(!Lnc::HasPendingLaunch(), "VSH backend rejection does not fall back to startup queue");
	receive(1);
	Check(wire.event.app_id == -1 && wire.event.app_local_pid == -1, "failure start has no process identity");
	receive(2);
	Check(wire.event.app_id == -1 && wire.event.app_local_pid == -1 && wire.event.error_code == launch_error,
	      "failed end never advertises successful launch");
	Check(!Lnc::RuntimeLaunchFailed(runtime_request.appId, launch_error) &&
	      !Lnc::RuntimeLaunchStarted(runtime_request.appId), "failed launch cannot complete again");
	Check(kill(runtime_request.appId) == launch_error && close_calls == 2, "failed launch never dispatches close against another run");
	const auto* result_symbol = symbols.FindByNid("LZs6hfPMnso", Loader::SymbolType::Func);
	Check(result_symbol != nullptr, "launch result export");
	const auto launch_result = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, uint32_t*)>(result_symbol->vaddr);
	uint32_t result_id = 0xaabbccdd;
	Check(launch_result("PPSA01785", &result_id) == launch_error && result_id == 0xaabbccdd, "failed result does not write success ID");
	empty_event();
	const auto critical_depth = Common::GetHostLockCriticalCounter()->load();
	Lnc::SetRuntimeLaunchHandler([&](const Lnc::PendingLaunch& request) {
		runtime_request = request;
		Check(Common::GetHostLockCriticalCounter()->load() == critical_depth,
		      "launch backend executes outside LNC host-lock suspension guards");
		Check(Lnc::RegisterRuntimeLaunchPath(request.appId, "/user/app/PPSA01786/eboot.bin", "/user/app/PPSA01786/sce_sys"),
		      "backend publishes resolved path before asynchronous preparation");
		Check(Lnc::RuntimeLaunchAccepted(request.appId), "backend callback may reenter LNC without deadlock");
		return 0;
	});
	const int failed_after_accept = launch("PPSA01786", argv, &param);
	Check(failed_after_accept > 0 && !launched("PPSA01786"), "accepted load still pending");
	Check(Lnc::RuntimeLaunchFailed(failed_after_accept, launch_error), "asynchronous preparation failure");
	receive(1);
	Check(wire.event.app_type == 5, "explicit native plan classifies accepted request");
	receive(2);
	Check(wire.event.error_code == launch_error && wire.event.app_id == -1, "failure after acceptance has one failed end");
	Check(!Lnc::RuntimeLaunchQuiescentExit(failed_after_accept), "failed preparation does not generate a fake kill end");
	empty_event();

	const int canceled_startup = launch("PPSA01786", argv, &param);
	Check(canceled_startup > 0, "accepted startup cancellation fixture");
	receive(1);
	Lnc::SetRuntimeCloseHandler([&](const Lnc::RuntimeCloseRequest& request) {
		++close_calls;
		Check(request.app_id == canceled_startup && request.launch_seq == runtime_request.seq &&
		      request.owner_program_id == -1 && request.owner_lifetime == 0 && !request.force,
		      "startup close uses launch sequence before owner publication");
		return 0;
	});
	Check(kill(canceled_startup) == 0 && !Lnc::RuntimeLaunchStarted(canceled_startup) && !launched("PPSA01786"),
	      "accepted close blocks future guest startup without fabricating a running app");
	empty_event();
	Check(Lnc::RuntimeLaunchFailed(canceled_startup, Libs::LibKernel::KERNEL_ERROR_ECANCELED),
	      "canceled startup completes only after backend quiescence");
	receive(2);
	Check(wire.event.app_id == -1 && wire.event.error_code == Libs::LibKernel::KERNEL_ERROR_ECANCELED,
	      "startup cancellation is failed launch, not successful process kill");
	empty_event();

	// Quiescence may race the close handler's return. The registry orders the
	// accepted KillStart before the actual KillEnd even for synchronous backends.
	const int synchronous_id = launch("PPSA01786", argv, &param);
	Check(synchronous_id > 0 && Lnc::RuntimeLaunchStarted(synchronous_id, 34567), "synchronous close fixture");
	receive(1); receive(2);
	Lnc::SetRuntimeCloseHandler([&](const Lnc::RuntimeCloseRequest& request) {
		Check(request.app_id == synchronous_id && request.force && request.reason == 0,
		      "ForceKillApp has one argument and explicitly marks a force request");
		Check(Lnc::RuntimeLaunchQuiescentExit(synchronous_id) && !Lnc::RuntimeLaunchQuiescentExit(synchronous_id),
		      "backend can publish actual quiescence reentrantly exactly once");
		return 0;
	});
	Check(force_kill(synchronous_id) == 0 && !launched("PPSA01786"), "synchronous close finishes actual exit");
	receive(3); receive(4);
	empty_event();
	Lnc::SetRuntimeLaunchHandler({});
	Check(launch("PPSA01787", nullptr, &param) == Libs::LibKernel::KERNEL_ERROR_ENOTSUP && !Lnc::HasPendingLaunch(),
	      "VSH session without backend cannot fall back to a fake successful launch");
	receive(1); receive(2);
	Check(wire.event.error_code == Libs::LibKernel::KERNEL_ERROR_ENOTSUP && wire.event.app_id == -1, "missing backend failure event");
	empty_event();
	Lnc::ConfigureVshRuntime(false);
	Check(Lnc::HasControllerFocus(&runtime_module) && !Lnc::IsSystemUiOverlaid(&runtime_module), "explicit gate restores direct semantics");
	Lnc::SetRuntimeLaunchHandler({});
	Lnc::SetRuntimeCloseHandler({});

	Lnc::RegisterProgramLaunch(&unknown, root / "missing.json", false);
	Loader::RuntimeLinker::SetCurrentProgram(&unknown);
	Check(get_status(&buffer->status) == 0 && buffer->status.title_id[0] == '\0' && buffer->status.app_type == 0,
	      "missing metadata must not manufacture a sys title or ShellUI identity");
	Loader::Program syscore, shellcore;
	syscore.unique_id = 700005;
	syscore.file_name = root / "sys" / "SceSysCore.elf";
	shellcore.unique_id = 700006;
	shellcore.file_name = root / "vsh" / "SceShellCore.elf";
	Lnc::RegisterProgramLaunch(&syscore, root / "missing.json", false);
	Lnc::RegisterProgramLaunch(&shellcore, root / "missing.json", false);
	Loader::RuntimeLinker::SetCurrentProgram(&syscore);
	Check(get_status(&buffer->status) == 0 && buffer->status.app_type == 0 && buffer->status.title_id[0] == '\0',
	      "SysCore must not receive globally selected ShellUI metadata");
	Loader::RuntimeLinker::SetCurrentProgram(&shellcore);
	Check(get_status(&buffer->status) == 0 && buffer->status.app_type == 6 && buffer->status.app_id != shell_id,
	      "ShellCore executable identity must remain distinct");
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	std::filesystem::remove_all(root);
	return 0;
}
