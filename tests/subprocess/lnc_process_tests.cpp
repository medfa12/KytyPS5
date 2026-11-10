// Linked with the existing full component dependency objects in an independent
// overlay, or by the parent full-test helper. Never runs an emulator/guest image.
#include "libs/libLncUtil.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"
#include "loader/runtimeLinker.h"
#include <iostream>
#include <stdexcept>

namespace Libs::LncUtil { void InitLncUtil_1(Loader::SymbolDatabase*); }
namespace {
void Check(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
}
int main() try {
	namespace Lnc = Libs::LncUtil::Lnc;
	Loader::SymbolDatabase symbols; Libs::LncUtil::InitLncUtil_1(&symbols);
	const auto address = [&](const char* nid) { const auto* r = symbols.FindByNid(nid, Loader::SymbolType::Func); Check(r, "real LNC registration"); return r->vaddr; };
	const auto launch = reinterpret_cast<int(KYTY_SYSV_ABI*)(const char*, const char* const*, const Lnc::LaunchAppParam*)>(address("+nRJUD-7qCk"));
	const auto kill = reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t)>(address("SZ2uH5Abws8"));
	const auto focus = reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t,uint64_t,uint64_t)>(address("v7DYuX0G5TQ"));
	const auto controller = reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t)>(address("X8gYbyLG1wk"));
	const auto receive = reinterpret_cast<int(KYTY_SYSV_ABI*)(void*)>(address("ZucoOmNsb7w"));
	Lnc::ConfigureVshRuntime(true); Lnc::PendingLaunch pending;
	Lnc::SetRuntimeLaunchHandler([&](const auto& request) { pending = request; return 0; });
	Lnc::LaunchAppParam param {}; param.size = 40; param.user_id = 1000;
	uint64_t nonce = 1;
	const auto accepted = [&] {
		const auto id = launch("PPSA01783", nullptr, &param); Check(id > 0, "real registered launch request");
		Loader::RuntimeChild::Key token {static_cast<uint32_t>(id),0,pending.seq,nonce++,0x55};
		Check(Lnc::RegisterRuntimeLaunchPath(id, "/user/app/PPSA01783/eboot.bin", "/user/appmeta/PPSA01783"), "stable native paths");
		Check(Lnc::RegisterRuntimeProcess(token, 0x1234) && !Lnc::RegisterRuntimeProcess(token, 0x5678), "one immutable external generation");
		Check(Lnc::RuntimeLaunchAccepted(id), "accepted before child execution");
		Lnc::EventWire event; Check(receive(&event) == 0 && event.event_type == 1 && event.app_id == -1, "launch-start sentinel, not Running");
		return token;
	};
	const auto event = [&](int type) { Lnc::EventWire value; Check(receive(&value) == 0 && value.event_type == type, "exact lifecycle event order"); return value; };
	const auto empty = [&] { Lnc::EventWire value; Check(receive(&value) == Libs::LibKernel::KERNEL_ERROR_EAGAIN, "no fabricated lifecycle events"); };
	auto token = accepted(); auto stale = token; ++stale.nonce_hi;
	Check(!Lnc::RuntimeProcessStarted(stale,100) && !Lnc::RuntimeLaunchStarted(token.app,100) &&
	    !Lnc::RuntimeLaunchFailed(token.app,Libs::LibKernel::KERNEL_ERROR_EIO), "stale and unkeyed start/failure rejected"); empty();
	Check(Lnc::RuntimeProcessStarted(token,100), "exact child entry notification");
	auto started = event(2); Check(started.app_id == static_cast<int>(token.app) && started.app_local_pid == 100 && started.app_category_type == 0x1234, "guest-local PID/category, not host PID");
	Check(!Lnc::RuntimeProcessRetired(stale,0) && !Lnc::RuntimeLaunchQuiescentExit(token.app), "stale/unkeyed exit rejected");
	bool allow_focus = false; unsigned focus_calls = 0;
	Lnc::SetRuntimeFocusHandler([&](const Lnc::RuntimeFocusRequest& request) {
		++focus_calls; Check(request.app_id == -1 || request.process_token == token, "focus uses immutable external token");
		(void)Lnc::GetFocusedAppId(); // Callback is outside g_app_mutex, no deadlock.
		return allow_focus ? 0 : Libs::LibKernel::KERNEL_ERROR_EBUSY;
	});
	Check(focus(token.app,0,0) == Libs::LibKernel::KERNEL_ERROR_EBUSY && Lnc::GetFocusedAppId() == -1, "failed real focus ack preserves registry"); empty();
	allow_focus = true;
	Check(focus(token.app,0x123456789ull,0x987654321ull) == 0, "accepted display focus acknowledgement");
	Check(event(0x1001).check_flags == 0x123456789ull && event(0x1002).context_id == 0x987654321ull, "full-width focus wire arguments");
	Check(controller(token.app) == 0, "controller acknowledgement"); event(0x1003); event(0x1004);
	Loader::Program shell;
	shell.unique_id = 700101; shell.file_name = "NPXS40087/eboot.bin";
	Lnc::RegisterProgramLaunch(&shell, "missing-shell-param.json", true);
	int32_t shell_id = -1; Check(Lnc::TryGetProgramAppId(&shell, &shell_id), "registered native ShellUI identity");
	Check(controller(shell_id) == 0, "native shell controller focus reaches backend without subprocess token");
	Check(event(0x1003).app_id == shell_id && event(0x1004).app_id == shell_id, "shell events retain native identity");
	Check(Lnc::GetFocusedAppId() == static_cast<int32_t>(token.app) && Lnc::GetControllerFocusedAppId() == shell_id,
	    "Control Center takes input while application remains displayed");
	Check(controller(token.app) == 0, "overlay dismiss returns input"); event(0x1003); event(0x1004);
	// The native shell must receive a typed conflict and the existing app ID;
	// generic EBUSY/Unknown cannot drive its KillAppAsync/continue launch path.
	Lnc::SetRuntimeLaunchHandler([&](const auto& request) {
		Check(Lnc::RegisterRuntimeLaunchPath(request.appId, "/user/app/" + request.titleId + "/eboot.bin",
		    "/user/appmeta/" + request.titleId), "classify resolved launch before arbitration");
		Check(Lnc::RuntimeLaunchConflict(request.appId, stale) == Libs::LibKernel::KERNEL_ERROR_EBUSY,
		    "stale generation cannot authorize closing a live app");
		return Lnc::RuntimeLaunchConflict(request.appId, token);
	});
	for (const auto* title: {"PPSA01651", "PPSA01783"}) {
		const int expected = static_cast<int>(std::string(title) == "PPSA01783" ? 0x8094000cu : 0x80940010u);
		Check(launch(title, nullptr, &param) == expected, "native different/same-title conflict result");
		Check(event(1).app_type == 5, "conflict launch-start is a native BigApp target");
		auto conflict = event(2);
		Check(conflict.app_type == 5 && conflict.app_id == -1 && conflict.app_local_pid == -1 &&
		    conflict.error_code == expected && conflict.existing_app_id == static_cast<int32_t>(token.app) &&
		    std::string(conflict.title_id) == title, "native conflict payload retains requested title and exact live target");
		Check(Lnc::GetFocusedAppId() == static_cast<int32_t>(token.app), "conflict does not close or defocus existing app");
		empty(); // The generic handler-failure callback must not duplicate the event.
	}
	Lnc::SetRuntimeLaunchHandler([&](const auto& request) { pending = request; return 0; });
	Lnc::SetRuntimeCloseHandler([&](const Lnc::RuntimeCloseRequest& request) {
		Check(request.process_token == token && request.launch_seq == token.sequence && request.owner_program_id == -1, "close targets external generation, not fake Program");
		Check(Lnc::RuntimeProcessRetired(token,0), "actual-retirement callback can race dispatch return"); return 0;
	});
	Check(kill(token.app) == 0, "close accepted"); event(3); event(4);
	Check(!Lnc::RuntimeProcessRetired(token,0) && Lnc::GetFocusedAppId() == -1 && Lnc::GetControllerFocusedAppId() == -1, "single completion and focus retirement"); empty();
	token = accepted();
	Check(kill(token.app) == 0, "accepted-before-entry close");
	auto canceled = event(2); Check(canceled.app_id == -1 && canceled.app_local_pid == -1 && canceled.error_code == Libs::LibKernel::KERNEL_ERROR_ECANCELED, "accepted cancel fails launch without fake Running/KillEnd"); empty();
	token = accepted(); stale = token; --stale.sequence;
	Check(!Lnc::RuntimeProcessRetired(stale,Libs::LibKernel::KERNEL_ERROR_EIO), "stale sequence cannot fail new process");
	Check(Lnc::RuntimeProcessRetired(token,Libs::LibKernel::KERNEL_ERROR_EIO), "created-before-entry failure after actual wait");
	Check(event(2).error_code == Libs::LibKernel::KERNEL_ERROR_EIO, "real startup failure preserved"); empty();
	Check(focus_calls == 5, "only actual focus requests called backend");
	Lnc::SetRuntimeLaunchHandler({}); Lnc::SetRuntimeCloseHandler({}); Lnc::SetRuntimeFocusHandler({});
	std::cout << "subprocess LNC token/order/cancel/focus tests passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
