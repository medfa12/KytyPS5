// Parent includes libs/kernelBacktrace.h at file scope; Check is parent-provided.
// Trusted byte-memory fixture: no firmware, guest execution or Windows API.
namespace KernelBacktraceTest {
namespace Bt = Libs::KernelBacktrace;
constexpr const char* Name = "KernelBacktrace";

struct Memory {
	static constexpr uint64_t Base = 0x1000;
	std::array<uint8_t, (Bt::MaxFrames + 1) * Bt::FrameBytes> bytes {};
	size_t readable = bytes.size();
	size_t calls = 0;
	size_t requested = 0;
	bool short_success = false;

	void Word(size_t offset, uint64_t value) {
		for (size_t i = 0; i < 8; ++i) { bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i)); }
	}

	void Frame(size_t offset, uint64_t next, uint64_t pc) {
		Word(offset, next);
		Word(offset + 8, pc);
	}

	static bool Read(void* context, uint64_t address, void* destination, size_t size, size_t& copied) noexcept {
		auto& memory = *static_cast<Memory*>(context);
		++memory.calls;
		memory.requested += size;
		copied = 0;
		if (address < Base || address - Base >= memory.readable) { return false; }
		const auto offset = static_cast<size_t>(address - Base);
		auto* output = static_cast<uint8_t*>(destination);
		while (copied < size && copied < memory.readable - offset) {
			output[copied] = memory.bytes[offset + copied];
			++copied;
		}
		return copied == size || memory.short_success;
	}

	Bt::Reader Reader() { return {this, Read}; }
};

void ByteChains() {
	Memory memory;
	// Cross-stack-style movement in BOTH directions, with an unaligned record.
	memory.Frame(64, Memory::Base + 1, 0x910123456);
	memory.Frame(1, Memory::Base + 32, 0x140123456);
	memory.Frame(32, 0, 0x910abcdef);
	const auto capture = Bt::Collect(Memory::Base + 64, memory.Reader());
	Check(Name, capture.stop == Bt::Stop::Complete && capture.count == 3 &&
	            capture.pcs[0] == 0x910123456 && capture.pcs[1] == 0x140123456 && capture.pcs[2] == 0x910abcdef,
	      "byte-chain PCs lost/reordered across nonmonotonic links");
	Check(Name, memory.calls == 3 && memory.requested == 48 && capture.requested_bytes == 48,
	      "chain reread or wrong byte budget");
	memory.Frame(32, Memory::Base + 64, 0x910abcdef);
	const auto cycle = Bt::Collect(Memory::Base + 64, memory.Reader());
	Check(Name, cycle.stop == Bt::Stop::Cycle && cycle.count == 3 && cycle.stop_frame == Memory::Base + 64,
	      "cycle repeated a captured frame");

	Memory broken;
	broken.Frame(0, Memory::Base + 32, 0x1111);
	broken.Frame(32, 0, 0x2222);
	broken.readable = 44; // Only twelve of the second record's sixteen bytes.
	for (int mode = 0; mode < 2; ++mode) {
		broken.short_success = mode != 0;
		const auto partial = Bt::Collect(Memory::Base, broken.Reader());
		Check(Name, partial.stop == Bt::Stop::ReadFailed && partial.count == 1 && partial.pcs[0] == 0x1111 &&
		            partial.pcs[1] == 0 && partial.stop_frame == Memory::Base + 32,
		      "partial frame fabricated a PC or erased prior evidence");
	}
	broken.readable = 0;
	const auto unreadable = Bt::Collect(Memory::Base, broken.Reader());
	Check(Name, unreadable.stop == Bt::Stop::ReadFailed && unreadable.count == 0, "unreadable start fabricated frames");
}

void Bounds() {
	Memory memory;
	memory.Frame(0, UINT64_MAX - 7, 0x1234);
	const auto overflow = Bt::Collect(Memory::Base, memory.Reader());
	Check(Name, overflow.stop == Bt::Stop::AddressOverflow && overflow.count == 1 && memory.calls == 1,
	      "overflowing frame reached reader");
	for (size_t i = 0; i <= Bt::MaxFrames; ++i) {
		memory.Frame(i * Bt::FrameBytes, Memory::Base + (i + 1) * Bt::FrameBytes, 0x100000 + i);
	}
	memory.calls = memory.requested = 0;
	const auto capped = Bt::Collect(Memory::Base, memory.Reader());
	Check(Name, capped.stop == Bt::Stop::Limit && capped.count == 512 && capped.pcs[0] == 0x100000 &&
	            capped.pcs[511] == 0x1001ff && memory.calls == 512 && memory.requested == 8192 &&
	            capped.requested_bytes == 8192, "frame/byte cap exceeded or real tail lost");
	memory.Frame(511 * Bt::FrameBytes, 0, 0x1001ff);
	const auto exact = Bt::Collect(Memory::Base, memory.Reader());
	Check(Name, exact.stop == Bt::Stop::Complete && exact.count == 512, "exact-limit terminated chain marked truncated");
	const auto entry_reserved = Bt::Collect(Memory::Base, memory.Reader(), Bt::MaxFrames - 1);
	Check(Name, entry_reserved.stop == Bt::Stop::Limit && entry_reserved.count == 511 &&
	            entry_reserved.requested_bytes == 8176, "entry caller's frame slot not reserved");
}

void Prefixes() {
	Memory memory;
	memory.bytes[0] = '%'; // Prefix is data, never a format string.
	memory.bytes[1] = 's';
	memory.readable = 3; // Terminator immediately before unreadable memory.
	const auto complete = Bt::ReadPrefix(Memory::Base, memory.Reader());
	Check(Name, complete.stop == Bt::PrefixStop::Complete && complete.length == 2 && complete.text[0] == '%' &&
	            complete.text[1] == 's' && complete.text[2] == 0 && memory.calls == 3,
	      "valid prefix rejected at readable boundary");
	memory.readable = 2;
	const auto partial = Bt::ReadPrefix(Memory::Base, memory.Reader());
	Check(Name, partial.stop == Bt::PrefixStop::ReadFailed && partial.length == 2 && partial.text[2] == 0,
	      "prefix read failure lost termination/status");
	memory.bytes.fill('x');
	memory.readable = memory.bytes.size();
	memory.calls = 0;
	const auto capped = Bt::ReadPrefix(Memory::Base, memory.Reader());
	Check(Name, capped.stop == Bt::PrefixStop::Limit && capped.length == 128 && capped.text[128] == 0 && memory.calls == 128,
	      "unterminated prefix exceeded byte cap");
	memory.bytes[127] = 0;
	const auto boundary = Bt::ReadPrefix(Memory::Base, memory.Reader());
	Check(Name, boundary.stop == Bt::PrefixStop::Complete && boundary.length == 127, "last allowed prefix terminator missed");
	const auto calls = memory.calls;
	const auto null_prefix = Bt::ReadPrefix(0, memory.Reader());
	Check(Name, null_prefix.stop == Bt::PrefixStop::NullAddress && memory.calls == calls, "null prefix read or hidden");
}

} // namespace KernelBacktraceTest

void TestKernelBacktrace() {
	KernelBacktraceTest::ByteChains();
	KernelBacktraceTest::Bounds();
	KernelBacktraceTest::Prefixes();
}

#ifdef _WIN32
int KernelBacktraceServiceChild() {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Console;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	// A real SysV call from executable test memory with a known guest RBP chain.
	// Save/restore the host caller's RBP instead of relying on its frame layout.
	const std::array<uint64_t, 2> frame {0, 0x0123456789abcde0ull};
	constexpr char prefix[] = "backtrace-fixture";
	std::array<uint8_t, 35> bytes {
	    0x55, 0x48, 0xbd, 0, 0, 0, 0, 0, 0, 0, 0,
	    0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0,
	    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
	    0xff, 0xd0, 0x5d, 0xc3,
	};
	const auto frame_address = reinterpret_cast<uint64_t>(frame.data());
	const auto prefix_address = reinterpret_cast<uint64_t>(prefix);
	const auto entry_address = reinterpret_cast<uint64_t>(&Libs::LibKernel::KernelBacktrace);
	std::memcpy(bytes.data() + 3, &frame_address, 8);
	std::memcpy(bytes.data() + 13, &prefix_address, 8);
	std::memcpy(bytes.data() + 23, &entry_address, 8);
	auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!code) { return 71; }
	std::memcpy(code, bytes.data(), bytes.size());
	FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
	std::printf("expected caller 0x%016llx\n", static_cast<unsigned long long>(reinterpret_cast<uint64_t>(code + 33)));
	using Entry = void (KYTY_SYSV_ABI *)();
	SetLastError(0x12345678);
	reinterpret_cast<Entry>(code)();
	const bool preserved_success = GetLastError() == 0x12345678;
	const uint64_t invalid_prefix = 1;
	std::memcpy(code + 13, &invalid_prefix, 8);
	FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
	SetLastError(0x87654321);
	reinterpret_cast<Entry>(code)();
	const bool preserved_failure = GetLastError() == 0x87654321;
	VirtualFree(code, 0, MEM_RELEASE);
	return preserved_success && preserved_failure ? 0 : 72;
}

void TestKernelBacktraceService() {
	constexpr const char* test = "KernelBacktraceService";
	wchar_t exe[32768] {};
	Check(test, GetModuleFileNameW(nullptr, exe, 32768) != 0, "resolve executable");
	SECURITY_ATTRIBUTES attributes {sizeof(attributes), nullptr, TRUE};
	HANDLE reader = nullptr, writer = nullptr;
	struct Handles {
		HANDLE& first; HANDLE& second;
		~Handles() { if (first) { CloseHandle(first); } if (second) { CloseHandle(second); } }
	} pipe_handles {reader, writer};
	Check(test, CreatePipe(&reader, &writer, &attributes, 8192) != 0, "capture pipe");
	Check(test, SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0) != 0, "capture read ownership");
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = writer;
	startup.hStdError = writer;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION child {};
	Handles child_handles {child.hThread, child.hProcess};
	std::wstring command = L"\"" + std::wstring(exe) + L"\" --kernel-backtrace-service-child";
	const bool created = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
	                                  nullptr, nullptr, &startup, &child) != 0;
	CloseHandle(writer);
	writer = nullptr;
	Check(test, created, "launch service child");
	const auto wait = WaitForSingleObject(child.hProcess, 5000);
	if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 74); WaitForSingleObject(child.hProcess, 1000); }
	DWORD exit_code = 0;
	const bool got_exit = GetExitCodeProcess(child.hProcess, &exit_code) != 0;
	std::array<char, 8192> output {};
	DWORD received = 0;
	const bool read = wait == WAIT_OBJECT_0 &&
	    ReadFile(reader, output.data(), static_cast<DWORD>(output.size() - 1), &received, nullptr) != 0;
	CloseHandle(reader);
	reader = nullptr;
	Check(test, wait == WAIT_OBJECT_0 && got_exit && exit_code == 0 && read, "service failed, hung, or changed LastError");
	const std::string text(output.data(), received);
	const auto marker = text.find("expected caller ");
	Check(test, marker != std::string::npos, "missing test caller witness");
	const auto expected = text.substr(marker + 16, 18);
	Check(test, text.find("  caller " + expected) != std::string::npos &&
	            text.find("[backtrace-fixture] backtrace:") != std::string::npos &&
	            text.find("0x0123456789abcde0") != std::string::npos &&
	            text.find("prefix truncated/unavailable: read error") != std::string::npos,
	      "entry shim lost guest return/frame evidence or failed prefix not reported");
}
#endif
