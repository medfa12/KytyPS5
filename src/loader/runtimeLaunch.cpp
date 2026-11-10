#include "loader/runtimeLaunch.h"
#include "loader/runtimeChildClient.h"

#include "emulator.h"
#include "common/logging/log.h"
#include "libs/libLncUtil.h"
#include "loader/runtimeChildHost.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Loader {
namespace {
// CommandLineToArgvW/CRT quoting, including empty arguments and trailing slashes.
std::wstring Quote(const std::wstring& value) {
	std::wstring result = L"\"";
	size_t slashes = 0;
	for (wchar_t c: value) {
		if (c == L'\\') { ++slashes; continue; }
		result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
		slashes = 0;
		if (c == L'"') { result += L'\\'; }
		result += c;
	}
	result.append(slashes * 2, L'\\');
	return result + L'"';
}

struct RuntimeHost {
	Emulator::RunOptions options;
	std::filesystem::path executable;
	std::filesystem::path output;
	HANDLE job = nullptr;
	std::mutex mutex;
	~RuntimeHost() { if (job) { CloseHandle(job); } }

	int Launch(const Libs::LncUtil::Lnc::PendingLaunch& request) {
		if (Loader::RuntimeChildHasStoreLease()) { return static_cast<int>(0x80940005); }
		std::lock_guard lock(mutex);
		std::filesystem::path app;
		for (const auto& [point, path]: options.extra_mounts) {
			if (point == "/user/app/" + request.titleId) { app = path; break; }
		}
		std::error_code ec;
		if (app.empty() || !std::filesystem::is_regular_file(app / "eboot.bin", ec) || request.args.size() > 15) {
			LOGF("Runtime launch: %s has no mounted native executable (or too many entry arguments)\n", request.titleId.c_str());
			return static_cast<int>(0x80940005);
		}
		const auto directory = output / (request.titleId + "-" + std::to_string(request.appId));
		std::filesystem::create_directories(directory, ec);
		if (ec) { return static_cast<int>(0x80940001); }
		std::wstring command = Quote(executable.wstring());
		const auto arg = [&](const wchar_t* key, const std::wstring& value) {
			command += L" "; command += key; command += L" "; command += Quote(value);
		};
		const auto wide = [](const std::string& value) { return std::filesystem::u8path(value).wstring(); };
		arg(L"--game", app.wstring());
		if (!options.fw_root.empty()) { arg(L"--fw-root", options.fw_root.wstring()); }
		arg(L"--user-id", std::to_wstring(request.user_id >= 0 ? request.user_id : options.config.user_id));
		arg(L"--user-name", wide(options.config.user_name));
		arg(L"--screen-width", std::to_wstring(options.config.screen_width));
		arg(L"--screen-height", std::to_wstring(options.config.screen_height));
		arg(L"--console-language", std::to_wstring(options.config.console_language));
		arg(L"--printf-direction", L"File");
		arg(L"--printf-output-file", (directory / "guest.log").wstring());
		for (const auto& key: options.config.keymap) { arg(L"--keymap", wide(key)); }
		for (const auto& [point, path]: options.extra_mounts) {
			// Each executable owns its /app0 and temporary socket namespace.
			if (point == "/" || point.starts_with("/app0") || point.starts_with("/hostapp") ||
			    point == "/system_tmp" || point == "/tmp") { continue; }
			arg(L"--mount", wide(point) + L"=" + path.wstring());
		}
		for (const auto& value: request.args) { arg(L"--guest-arg", wide(value)); }
		SECURITY_ATTRIBUTES security {sizeof(security), nullptr, TRUE};
		HANDLE log = CreateFileW((directory / "host.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
		                         &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (log == INVALID_HANDLE_VALUE) { return static_cast<int>(0x80940001); }
		STARTUPINFOEXW startup {};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdOutput = log;
		startup.StartupInfo.hStdError = log;
		SIZE_T bytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
		std::vector<unsigned char> attributes(bytes);
		startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
		if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
			CloseHandle(log); return static_cast<int>(0x80940001);
		}
		PROCESS_INFORMATION process {};
		const bool created = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		    &log, sizeof(log), nullptr, nullptr) &&
		    CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
		        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process);
		const DWORD create_error = created ? 0 : GetLastError();
		DeleteProcThreadAttributeList(startup.lpAttributeList);
		CloseHandle(log);
		if (!created) {
			LOGF("Runtime launch: CreateProcess failed title=%s error=%lu\n", request.titleId.c_str(), create_error);
			return static_cast<int>(0x80940001);
		}
		if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == DWORD(-1)) {
			TerminateProcess(process.hProcess, 1);
			CloseHandle(process.hThread); CloseHandle(process.hProcess);
			return static_cast<int>(0x80940001);
		}
		CloseHandle(process.hThread);
		LOGF("Runtime launch: title=%s app=%u host_pid=%lu log=%s\n", request.titleId.c_str(),
		     request.appId, process.dwProcessId, directory.string().c_str());
		std::thread([handle = process.hProcess, id = request.appId, title = request.titleId] {
			WaitForSingleObject(handle, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(handle, &code);
			CloseHandle(handle);
			Libs::LncUtil::Lnc::RuntimeLaunchExited(id);
			LOGF("Runtime launch exited: title=%s app=%u exit=%lu\n", title.c_str(), id, code);
		}).detach();
		return 0;
	}
};
}

void ConfigureRuntimeLaunch(const Emulator::RunOptions& options) {
	if (options.vsh_session) {
		if (options.vsh_subprocess) { ConfigureVshSubprocessLaunch(options); }
		else { ConfigureVshRuntimeLaunch(options); }
		return;
	}
	Libs::LncUtil::Lnc::SetRuntimeFocusHandler({});
	Libs::LncUtil::Lnc::ConfigureVshRuntime(false);
	bool mounted = false;
	for (const auto& [point, path]: options.extra_mounts) { mounted |= point.starts_with("/user/app/"); }
	if (!mounted) { return; }
	auto host = std::make_shared<RuntimeHost>();
	host->options = options;
	for (auto& [point, path]: host->options.extra_mounts) { path = std::filesystem::absolute(path); }
	if (!options.fw_root.empty()) { host->options.fw_root = std::filesystem::absolute(options.fw_root); }
	host->output = std::filesystem::current_path() / "launches";
	std::wstring executable(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
	if (length == 0 || length >= executable.size()) { return; }
	executable.resize(length);
	host->executable = executable;
	host->job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!host->job || !SetInformationJobObject(host->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { return; }
	Libs::LncUtil::Lnc::SetRuntimeLaunchHandler([host](const auto& request) { return host->Launch(request); });
}
}
#else
namespace Loader {
void ConfigureRuntimeLaunch(const Emulator::RunOptions& options) {
	if (options.vsh_session) {
		if (options.vsh_subprocess) { ConfigureVshSubprocessLaunch(options); }
		else { ConfigureVshRuntimeLaunch(options); }
	}
	else { Libs::LncUtil::Lnc::ConfigureVshRuntime(false); }
}
}
#endif
