#include "loader/runtimeChildProcess.h"
#include "loader/runtimeChildClient.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace Loader::RuntimeChild;
namespace fs = std::filesystem;
static void Check(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
template<class F> void Until(F fn) {
	const auto end = GetTickCount64() + 5000;
	while (!fn()) { Check(GetTickCount64() < end, "deadline"); Sleep(1); }
}
static fs::path Exe() { wchar_t name[32768] {}; Check(GetModuleFileNameW(nullptr, name, 32768) != 0, "exe"); return name; }
static Spawn Make(const fs::path& root, const std::wstring& mode, unsigned serial) {
	Spawn s; s.executable = Exe(); s.directory = root; s.log = root / ("log-" + std::to_string(serial));
	s.exclusive_data = root / "data"; s.canonical_store_lease = true;
	s.key = {104, 0, serial, 123, 456}; s.arguments = {L"child", root.wstring(), mode};
	s.startup_timeout = std::chrono::milliseconds(500); s.close_timeout = std::chrono::milliseconds(100);
	return s;
}
static bool Available(const fs::path& root) { StoreLease l; return l.Acquire(root); }

int wmain(int argc, wchar_t** argv) try {
	static_assert(!std::is_copy_constructible_v<StoreLease> && std::is_nothrow_move_constructible_v<StoreLease>);
	if (argc > 1 && std::wstring(argv[1]) == L"child") {
		const fs::path root = argv[2]; const std::wstring mode = argv[3];
		Check(argc == 6 && std::wstring(argv[4]) == L"--vsh-child", "child arguments");
		const std::wstring wide = argv[5]; const std::string wire(wide.begin(), wide.end());
		ChildArguments parsed; Check(DecodeArguments(wire, &parsed), "decode");
		if (mode == L"badidentity") {
			StoreLease bad; Check(!bad.Adopt(parsed.lease, parsed.lease_volume, parsed.lease_file ^ 1), "identity rejected"); return 7;
		}
		Check(Loader::ConfigureRuntimeChildClient(wire), "configure production client");
		DWORD flags = HANDLE_FLAG_INHERIT;
		Check(GetHandleInformation(reinterpret_cast<HANDLE>(parsed.lease), &flags) && !(flags & HANDLE_FLAG_INHERIT), "lease not reinherited");
		if (mode == L"wrongroot") {
			Check(!Loader::ValidateRuntimeChildStoreLease(root / "other"), "root mismatch rejected"); return 8;
		}
		if (mode != L"beforeack") {
			Check(Loader::ValidateRuntimeChildStoreLease(root / "data"), "root validation/ACK");
			Loader::NotifyRuntimeChildEntry(100);
		}
		std::ofstream(root / "child-ready") << GetCurrentProcessId();
		for (;;) {
			Message command;
			if (Loader::PollRuntimeChildCommand(&command) && command.kind == Kind::Close) { return 0; }
			Sleep(1);
		}
	}
	if (argc > 1 && std::wstring(argv[1]) == L"owner") {
		const fs::path root = argv[2]; auto s = Make(root, argv[3], 1);
		s.startup_timeout = std::chrono::seconds(20);
		uint32_t error = 0; auto p = Process::Create(s, &error); Check(p && !error, "owner spawn");
		Until([&] { p->Poll(); return fs::exists(root / "child-ready") && (std::wstring(argv[3]) == L"beforeack" || p->Entered()); });
		std::ofstream(root / "owner-ready") << p->HostPid();
		for (;;) { Sleep(10); }
	}
	wchar_t temp[32768] {}; Check(GetTempPathW(32768, temp) != 0, "temp");
	const auto root = fs::path(temp) / "opencode" / ("cpp-store-lease-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	fs::create_directories(root / "data"); fs::create_directories(root / "other");
	std::ofstream(root / "data" / "sentinel") << "preserve";
	{
		StoreLease first; Check(first.Acquire(root / "data"), "acquire");
		Check(!Available(root / "data" / ".") && !Available(root / "other" / ".." / "data"), "canonical alias collision");
		Check(Available(root / "other"), "different root independent");
		StoreLease moved(std::move(first)), duplicate;
		Check(!first.Handle() && moved.Duplicate(&duplicate), "move and duplicate");
		moved.Reset(); Check(!Available(root / "data"), "duplicate retains exclusion without original");
		duplicate.Reset(); Check(Available(root / "data"), "last copy releases");
	}
	unsigned serial = 0;
	for (auto fault: {Spawn::Fault::BeforeCreate, Spawn::Fault::AssignJob, Spawn::Fault::Resume}) {
		auto s = Make(root, L"normal", ++serial); s.fault = fault;
		uint32_t error = 0; auto p = Process::Create(s, &error);
		if (fault == Spawn::Fault::BeforeCreate) { Check(!p && error, "precreate failure"); }
		else { Check(bool(p), "failed child retained"); Check(!Available(root / "data"), "failed child ownership retained"); Until([&] { return p->Poll().retired; }); }
		Check(Available(root / "data"), "failure release after OS exit");
	}
	for (const auto* mode: {L"normal", L"beforeack", L"badidentity", L"wrongroot"}) {
		auto s = Make(root, mode, ++serial); uint32_t error = 0; auto p = Process::Create(s, &error);
		Check(p && !error && !Available(root / "data"), "spawn owns root");
		if (std::wstring(mode) == L"normal") {
			Until([&] { p->Poll(); return p->Entered() || p->Retired(); }); Check(p->Entered(), "ACK before entry accepted");
			Check(!Available(root / "data"), "exclusion after ACK"); p->Close(false);
		}
		Until([&] { return p->Poll().retired; });
		Check(Available(root / "data"), "child exit releases");
		if (std::wstring(mode) == L"beforeack") { Check(p->Error() == ERROR_TIMEOUT, "missing ACK startup timeout"); }
		if (std::wstring(mode) == L"badidentity") { Check(p->ExitCode() == 7, "identity failure exit"); }
		if (std::wstring(mode) == L"wrongroot") { Check(p->ExitCode() == 8, "root failure exit"); }
	}
	for (const auto* mode: {L"beforeack", L"normal"}) {
		const auto dir = root / (std::wstring(mode) + L"-death"); fs::create_directories(dir / "data");
		std::wstring command = QuoteWindowsArgument(Exe().wstring()) + L" owner " + QuoteWindowsArgument(dir.wstring()) + L" " + mode;
		STARTUPINFOW startup {}; startup.cb = sizeof(startup); PROCESS_INFORMATION owner {};
		Check(CreateProcessW(Exe().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &owner), "disposable owner");
		CloseHandle(owner.hThread);
		Until([&] { return fs::exists(dir / "owner-ready"); });
		Check(!Available(dir / "data"), "owner/child lease before death");
		Check(TerminateProcess(owner.hProcess, 19) && WaitForSingleObject(owner.hProcess, 5000) == WAIT_OBJECT_0, "disposable parent death");
		CloseHandle(owner.hProcess);
		Until([&] { return Available(dir / "data"); }); // Kill-on-close job drains inherited child copy.
	}
	Check(fs::file_size(root / "data" / "sentinel") == 8, "store sentinel preserved");
	std::cout << "PASS: move/duplicate, aliases/distinct roots, startup failures, ACK/identity/root failures, child exit, parent death before/after ACK\n";
	fs::remove_all(root); return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
