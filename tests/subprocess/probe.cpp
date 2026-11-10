#include "loader/runtimeChildProtocol.h"
#include "loader/runtimeChildProcess.h"
#include "loader/runtimeChildClient.h"
#include <windows.h>
#include <string>
#include <cstdio>

using namespace Loader::RuntimeChild;
int wmain(int argc, wchar_t** argv) {
	ChildArguments arguments; std::wstring mode = L"normal", forbidden_name; uint64_t forbidden = 0;
	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		if (arg == L"--orphan-sleep") { Sleep(60000); return 0; }
		if (arg == L"--vsh-child" && i + 1 < argc) {
			const std::wstring value = argv[++i]; if (!DecodeArguments(std::string(value.begin(), value.end()), &arguments)) { return 3; }
		} else if (arg == L"--probe" && i + 1 < argc) { mode = argv[++i]; }
		else if (arg == L"--forbidden" && i + 1 < argc) { forbidden = std::stoull(argv[++i]); }
		else if (arg == L"--forbidden-name" && i + 1 < argc) { forbidden_name = argv[++i]; }
		else if (arg == L"--environment") {
			for (const auto* name: {L"KYTY_NATIVE_NP_ALTERNATE_STARTUP", L"KYTY_NATIVE_NP_OWNERSHIP",
			    L"KYTY_NATIVE_NP_WORKQUEUE", L"KYTY_NATIVE_NP_CALLOUT", L"KYTY_NATIVE_NP_HTTP_POOL", L"KYTY_NATIVE_NP_DISPATCHER",
			    L"KYTY_NATIVE_NP_EVENT_QUEUE"}) {
				SetLastError(ERROR_SUCCESS);
				if (GetEnvironmentVariableW(name, nullptr, 0) || GetLastError() != ERROR_ENVVAR_NOT_FOUND) { return 29; }
			}
			wchar_t value[128] {};
			if (!GetEnvironmentVariableW(L"KYTY_NATIVE_NP_OWNERSHIP_EXTRA", value, 128) || std::wstring(value) != L"preserved=\u00e9\u65e5\u672c") { return 30; }
			if (!GetEnvironmentVariableW(L"KYTY_CHILD_ENV_SENTINEL", value, 128) || std::wstring(value) != L"unrelated value") { return 31; }
		}
		else if (arg == L"--quote") {
			if (i + 3 >= argc || std::wstring(argv[++i]) != L"" || std::wstring(argv[++i]) != L"space \\\" tail\\" || std::wstring(argv[++i]) != L"é日本") { return 23; }
		}
	}
	if (mode == L"nohello") { Sleep(60000); return 0; }
	DWORD flags = 0;
	if (forbidden && GetHandleInformation(reinterpret_cast<HANDLE>(forbidden), &flags)) {
		// Handle VALUES can be reused by unrelated child CRT handles. Compare the
		// actual object, not numerical validity. Pin BEFORE opening the named
		// reference, which could otherwise itself reuse the forbidden value.
		HANDLE observed = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(forbidden), GetCurrentProcess(), &observed, 0, FALSE, DUPLICATE_SAME_ACCESS)) { std::fprintf(stderr, "sentinel duplicate failed: %lu\n", GetLastError()); return 24; }
		HANDLE expected = OpenEventW(SYNCHRONIZE, FALSE, forbidden_name.c_str());
		if (!expected) { std::fprintf(stderr, "sentinel open failed: %lu\n", GetLastError()); return 24; }
		const bool leaked = CompareObjectHandles(observed, expected);
		CloseHandle(observed); CloseHandle(expected); if (leaked) { std::fprintf(stderr, "sentinel object inherited\n"); return 24; }
	}
	if (mode == L"client" || mode == L"input") {
		if (!Loader::ConfigureRuntimeChildClient(EncodeArguments(arguments), mode == L"input" ? BrokeredInput : 0)) { return 26; }
		Loader::NotifyRuntimeChildEntry(100);
		PadFocusFilter filter; PadReport held;
		for (;;) {
			Message message;
			if (Loader::RuntimeChildChannelLost()) { return 27; }
			if (Loader::PollRuntimeChildCommand(&message)) {
				if (message.kind == Kind::Close) { return 0; }
				if (message.kind == Kind::Focus) {
					auto baseline = held; Loader::RuntimeChildFocusHeld(&baseline);
					filter.Focus(message.value[0] && message.value[1], static_cast<uint64_t>(message.value[2]), baseline);
					Loader::AcknowledgeRuntimeChildFocus(static_cast<uint64_t>(message.value[2]));
				}
			}
			PadReport raw, report;
			while (Loader::PollRuntimeChildPad(&raw)) {
				held = raw;
				if (!filter.Apply(raw, &report)) { continue; }
				if (report.time != 12345 || report.buttons != 0x4000 || report.axes[0] != 40 || report.axes[2] != 128 || report.touch[0].x != 1000 ||
				    report.touch[0].y != 500 || report.accel[2] != 3 || report.gyro[2] != 6 || report.orientation[3] != 0.9f || report.connections != 2) { return 28; }
				PadOutput reply; reply.epoch = report.epoch; reply.large_motor = report.axes[4]; reply.small_motor = report.axes[5]; reply.red = report.touch[0].id;
				Loader::NotifyRuntimeChildPs(); Loader::SendRuntimeChildPadOutput(reply);
			}
			Sleep(1);
		}
	}
	Channel channel; if (!channel.Adopt(arguments.receive, arguments.send)) { return 4; }
	uint64_t serial = 0;
	const auto send = [&](Kind kind, int64_t a = 0, int64_t b = 0) {
		Message message; message.key = arguments.key; message.kind = kind; message.serial = ++serial;
		message.value[0] = a; message.value[1] = b;
		if (mode == L"wrongkey") { ++message.key.nonce_hi; }
		return channel.Write(message);
	};
	if (mode == L"entryfirst") { send(Kind::Entry, 100); Sleep(60000); return 0; }
	if (!send(Kind::Hello, GetCurrentProcessId(), 0)) { return 5; }
	if (mode == L"preentry") { return 7; }
	if (mode != L"accepted") { send(Kind::Entry, 100); }
	if (mode == L"duplicate") { send(Kind::Entry, 100); }
	if (mode == L"presentfirst") { send(Kind::Present, 1); }
	if (mode == L"exit") { return 17; }
	if (mode == L"tree") {
		std::wstring exe(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())); exe.resize(count);
		auto command = QuoteWindowsArgument(exe) + L" --orphan-sleep";
		STARTUPINFOW startup {}; startup.cb = sizeof(startup); PROCESS_INFORMATION child {};
		if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) { return 25; }
		CloseHandle(child.hThread); CloseHandle(child.hProcess); return 0;
	}
	if (mode == L"hold" || mode == L"wrongkey" || mode == L"entryfirst" || mode == L"duplicate" || mode == L"presentfirst") { Sleep(60000); return 0; }
	uint64_t received = 0;
	for (;;) {
		Message message; const auto read = channel.Read(&message);
		if (read == ReadResult::Closed) { return 6; }
		if (read == ReadResult::Received) {
			if (!ValidMessage(message, arguments.key) || message.serial != ++received) { return 8; }
			if (message.kind == Kind::Close) { return 0; }
			if (message.kind == Kind::Focus) { send(Kind::FocusAck, message.value[2]); }
		}
		Sleep(1);
	}
}
