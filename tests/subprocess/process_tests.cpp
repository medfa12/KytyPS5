#include "loader/runtimeChildProcess.h"
#include "loader/runtimeChildManifest.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>

using namespace Loader::RuntimeChild;
static void Check(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
template <typename F> static void Until(F predicate) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate()) { Check(std::chrono::steady_clock::now() < end, "bounded process wait"); Sleep(1); }
}
int main(int argc, char** argv) try {
	Check(argc == 2, "probe executable argument");
	const auto root = std::filesystem::current_path() / ("subprocess-fixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	std::filesystem::create_directories(root / "app" / "sce_sys");
	std::ofstream(root / "app" / "eboot.bin") << "test image, never executed";
	std::ofstream(root / "app" / "sce_sys" / "param.json") << "{}";
	std::ofstream(root / "parent-profile-sentinel") << "unchanged";
	uint64_t sequence = 0;
	const auto make = [&](const wchar_t* mode) {
		Spawn spawn; spawn.executable = std::filesystem::absolute(argv[1]);
		spawn.directory = root; spawn.log = root / ("probe-" + std::to_string(++sequence) + ".log");
		spawn.key = {104, 0, sequence, 0x1122334455667788ull, 0xaabbccdd};
		spawn.arguments = {L"--probe", mode}; spawn.close_timeout = std::chrono::milliseconds(200); return spawn;
	};
	const auto start = [&](const Spawn& spawn) { uint32_t error = 0; auto process = Process::Create(spawn, &error); Check(bool(process) && !error, "create suspended managed child"); return process; };
	{
		struct SavedEnvironment {
			const wchar_t* name; std::wstring value; bool present;
			explicit SavedEnvironment(const wchar_t* key): name(key), value(32767, L'\0') {
				SetLastError(ERROR_SUCCESS);
				const auto size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
				present = size != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND; value.resize(size);
			}
			~SavedEnvironment() { SetEnvironmentVariableW(name, present ? value.c_str() : nullptr); }
		};
		SavedEnvironment saved[] {SavedEnvironment(L"KYTY_NATIVE_NP_ALTERNATE_STARTUP"), SavedEnvironment(L"kyty_native_np_ownership"),
		    SavedEnvironment(L"KYTY_NATIVE_NP_WORKQUEUE"), SavedEnvironment(L"KYTY_NATIVE_NP_CALLOUT"),
		    SavedEnvironment(L"KYTY_NATIVE_NP_HTTP_POOL"), SavedEnvironment(L"KYTY_NATIVE_NP_DISPATCHER"),
		    SavedEnvironment(L"KYTY_NATIVE_NP_EVENT_QUEUE"),
		    SavedEnvironment(L"KYTY_NATIVE_NP_OWNERSHIP_EXTRA"), SavedEnvironment(L"KYTY_CHILD_ENV_SENTINEL")};
		for (size_t i = 0; i < 7; ++i) { Check(SetEnvironmentVariableW(saved[i].name, L"1") != FALSE, "set parent NP selection"); }
		Check(SetEnvironmentVariableW(saved[7].name, L"preserved=\u00e9\u65e5\u672c") != FALSE &&
		    SetEnvironmentVariableW(saved[8].name, L"unrelated value") != FALSE, "set unrelated Unicode environment");
		auto spawn = make(L"normal"); spawn.arguments.push_back(L"--environment");
		auto p = start(spawn);
		Until([&] { p->Poll(); return p->Entered() || p->Retired(); });
		Check(p->Entered(), "spawned child removes all seven NP variables and preserves unrelated Unicode values");
		p->Close(false); Until([&] { return p->Poll().retired; }); Check(p->ExitCode() == 0, "environment probe normal exit");
		for (size_t i = 0; i < 9; ++i) {
			wchar_t value[128] {};
			const auto* expected = i < 7 ? L"1" : i == 7 ? L"preserved=\u00e9\u65e5\u672c" : L"unrelated value";
			Check(GetEnvironmentVariableW(saved[i].name, value, 128) && std::wstring(value) == expected, "child spawn leaves parent environment unchanged");
		}
	}
	{
		auto spawn = make(L"client");
		SECURITY_ATTRIBUTES security {sizeof(security), nullptr, TRUE};
		const auto event_name = L"Local\\KytySubprocessProbe-" + root.filename().wstring();
		HANDLE secret = CreateEventW(&security, FALSE, FALSE, event_name.c_str()); Check(secret != nullptr, "sentinel inherited handle");
		spawn.arguments.insert(spawn.arguments.end(), {L"--quote", L"", L"space \\\" tail\\", L"é日本", L"--forbidden", std::to_wstring(reinterpret_cast<uint64_t>(secret)), L"--forbidden-name", event_name});
		auto p = start(spawn); Check(!p->Entered() && !p->PresentationReady(), "creation is not guest entry/first frame");
		Until([&] { p->Poll(); if (p->Retired() && !p->Entered()) { throw std::runtime_error("client early exit=" + std::to_string(p->ExitCode()) + " error=" + std::to_string(p->Error())); } return p->Entered(); });
		Check(!p->PresentationReady(), "entry is not presentation");
		Check(p->Focus(true, true, 1) && !p->Focus(false, false, 2), "single outstanding focus command");
		Until([&] { p->Poll(); return p->FocusAcknowledged() == 1; });
		p->Close(false); Check(!p->Retired(), "close request is not completion");
		Until([&] { return p->Poll().retired; }); Check(p->ExitCode() == 0, "actual normal process exit");
		CloseHandle(secret);
	}
	for (const auto* mode: {L"exit", L"preentry", L"tree"}) {
		auto p = start(make(mode)); Until([&] { return p->Poll().retired; });
		Check(p->ExitCode() == (std::wstring(mode) == L"exit" ? 17u : std::wstring(mode) == L"preentry" ? 7u : 0u), "real exit code preserved");
		Check(p->Entered() == (std::wstring(mode) != L"preentry"), "pre-entry versus entered failure");
	}
	for (const auto* mode: {L"wrongkey", L"entryfirst", L"duplicate", L"presentfirst"}) {
		auto p = start(make(mode)); Until([&] { return p->Poll().retired; }); Check(p->Error() == ERROR_INVALID_DATA, "protocol generation/order rejection then real retirement");
	}
	{
		auto p = start(make(L"accepted")); Until([&] { return p->Poll().count != 0; });
		Check(!p->Entered(), "accepted child not yet entered"); p->Close(false);
		Until([&] { return p->Poll().retired; }); Check(!p->Entered(), "accepted cancellation never fakes running");
	}
	for (bool force: {false, true}) {
		auto p = start(make(L"hold")); Until([&] { p->Poll(); return p->Entered(); });
		p->Close(force); Check(!p->Retired(), "kill acceptance not native termination");
		Until([&] { return p->Poll().retired; }); Check(p->ExitCode() != 259, "forced/timed out child really terminated");
	}
	{
		auto spawn = make(L"nohello"); spawn.startup_timeout = std::chrono::milliseconds(20);
		auto p = start(spawn); Until([&] { return p->Poll().retired; }); Check(p->Error() == ERROR_TIMEOUT, "startup timeout retains until OS wait");
	}
	for (auto fault: {Spawn::Fault::BeforeCreate, Spawn::Fault::AssignJob, Spawn::Fault::Resume}) {
		auto spawn = make(L"normal"); spawn.fault = fault;
		uint32_t error = 0; auto p = Process::Create(spawn, &error);
		if (fault == Spawn::Fault::BeforeCreate) { Check(!p && error, "pre-create failure has no managed process"); }
		else { Check(bool(p) && !error && !p->Retired(), "post-create failure remains retained"); Until([&] { return p->Poll().retired; }); Check(!p->Entered(), "failed suspended start cannot enter"); }
	}
	{
		std::filesystem::create_directory(root / "exclusive-title-data");
		auto first = make(L"hold"); first.exclusive_data = root / "exclusive-title-data";
		auto p = start(first); Until([&] { p->Poll(); return p->Entered(); });
		auto second = make(L"normal"); second.exclusive_data = first.exclusive_data;
		uint32_t error = 0; Check(!Process::Create(second, &error) && error == ERROR_SHARING_VIOLATION, "one actual title-store writer");
		p->Close(true); Until([&] { return p->Poll().retired; });
		auto again = start(second); // Even while the retired ledger record is retained.
		p.reset(); Until([&] { again->Poll(); return again->Entered(); }); again->Close(false);
		Until([&] { return again->Poll().retired; });
	}
	{
		const auto spawn = make(L"normal");
		ManifestInput input; input.title = "PPSA01783"; input.app = root / "app"; input.metadata = input.app / "sce_sys" / "param.json"; input.output = root / "launches";
		input.guest_arguments = {"", "spaces and quotes \" \\"}; Spawn manifest; std::string error;
		Check(BuildManifest(input, spawn.key, &manifest, &error), "isolated explicit child manifest");
		Check(std::find(manifest.arguments.begin(), manifest.arguments.end(), L"--network-connected") == manifest.arguments.end(), "offline parent keeps child offline");
		Check(!BuildManifest(input, spawn.key, &manifest, &error), "duplicate run never overwrites prior files");
		input.network_connected = true;
		auto online_key = spawn.key; ++online_key.sequence;
		Check(BuildManifest(input, online_key, &manifest, &error), "online child manifest");
		Check(std::count(manifest.arguments.begin(), manifest.arguments.end(), L"--network-connected") == 1, "online parent enables child networking");
		input.output = input.app / "unsafe-output";
		Check(!BuildManifest(input, spawn.key, &manifest, &error), "content/firmware cannot become writable runtime output");
		for (const auto& argument: manifest.arguments) { Check(argument.find(L"parent-profile") == std::wstring::npos, "parent profile never forwarded"); }
		std::ifstream sentinel(root / "parent-profile-sentinel"); std::string contents; sentinel >> contents; Check(contents == "unchanged", "parent storage sentinel unchanged");
	}
	ChildArguments decoded; Check(!DecodeArguments("1:2:4294967296:1:1:1:3", &decoded) && !DecodeArguments("1:1:104:1:1:1:3", &decoded), "bounded argument identities/handles");
	std::cout << "subprocess lifecycle/isolation tests passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
