#include "loader/deferredImportThunk.h"
#include "loader/guestCallBoundary.h"
#include "common/virtualMemory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <xmmintrin.h>
#include <xbyak/xbyak.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Standalone host fixtures: link this file, deferredImportThunk.cpp, common,
// xbyak::xbyak and the platform's thread support. No runtimeLinker or guest run.
namespace {
namespace Vm = Common::VirtualMemory;
using Loader::DeferredImportThunk;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "DeferredImportThunkTests: %s\n", message);
		std::abort();
	}
}

// Fixture-generated callers/targets obey the same RW -> RX discipline.
class Executable final {
public:

	explicit Executable(const Xbyak::CodeGenerator& code) {
		Check(code.getSize() <= 0x1000, "fixture fits mapping");
		m_address = Vm::Alloc(0, 0x1000, Vm::Mode::ReadWrite);
		Check(m_address != 0 && m_address != UINT64_MAX, "fixture allocation");
		std::memcpy(reinterpret_cast<void*>(m_address), code.getCode(), code.getSize());
		Check(Vm::Protect(m_address, 0x1000, Vm::Mode::ExecuteRead), "fixture RX protection");
		Check(Vm::FlushInstructionCache(m_address, 0x1000), "fixture cache flush");
	}
	~Executable() { Check(Vm::Free(m_address), "fixture release"); }
	Executable(const Executable&) = delete;
	Executable& operator=(const Executable&) = delete;
	uint64_t Address() const { return m_address; }

private:
	uint64_t m_address = 0;
};

struct Record {
	uint64_t target = 0;
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> caller {0};
	uint64_t rendezvous = 0;
};

uint64_t KYTY_SYSV_ABI Resolve(void* opaque, uint64_t caller) noexcept {
	auto* record = static_cast<Record*>(opaque);
	record->caller.store(caller, std::memory_order_relaxed);
	record->calls.fetch_add(1, std::memory_order_relaxed);
	// Force the first batch of calls to overlap inside the SAME resolver.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (record->calls.load(std::memory_order_relaxed) < record->rendezvous) {
		Check(std::chrono::steady_clock::now() < deadline, "concurrent resolver rendezvous completes");
		std::this_thread::yield();
	}
	const auto target = record->target;
	// An ABI-conforming resolver is allowed to trash every argument register.
	// Do it explicitly so a helpful optimizer cannot hide an incomplete save.
	asm volatile(
	    "mov $-1, %%rax\n\tmov $-2, %%rdi\n\tmov $-3, %%rsi\n\t"
	    "mov $-4, %%rdx\n\tmov $-5, %%rcx\n\tmov $-6, %%r8\n\t"
	    "mov $-7, %%r9\n\tmov $-8, %%r10\n\tmov $-9, %%r11\n\t"
	    "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\t"
	    "pxor %%xmm2, %%xmm2\n\tpxor %%xmm3, %%xmm3\n\t"
	    "pxor %%xmm4, %%xmm4\n\tpxor %%xmm5, %%xmm5\n\t"
	    "pxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7"
	    : : : "rax", "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11",
	            "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
	return target;
}

enum class Mapping { Absent, ReadExecute, Other };
Mapping QueryMapping(uint64_t address) {
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION info {};
	Check(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0, "query thunk mapping");
	if (info.State == MEM_FREE) { return Mapping::Absent; }
	return info.State == MEM_COMMIT && info.Protect == PAGE_EXECUTE_READ ? Mapping::ReadExecute : Mapping::Other;
#else
	// Linux host fixture, independent of the emulator's allocation registry.
	FILE* maps = std::fopen("/proc/self/maps", "r");
	Check(maps != nullptr, "open host mapping table");
	Mapping found = Mapping::Absent;
	char line[1024];
	while (std::fgets(line, sizeof(line), maps) != nullptr) {
		uint64_t begin = 0, end = 0;
		char mode[5] {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, mode) == 3 &&
		    address >= begin && address < end) {
			found = mode[0] == 'r' && mode[1] == '-' && mode[2] == 'x' ? Mapping::ReadExecute : Mapping::Other;
			break;
		}
	}
	std::fclose(maps);
	return found;
#endif
}

void TestSealingAndLifetime() {
	Record record;
	// This callback is never invoked here; only allocation lifetime is tested.
	for (int i = 0; i < 32; ++i) {
		auto thunk = DeferredImportThunk::Create(&record, Resolve);
		Check(bool(thunk), "create lifetime fixture");
		const auto address = thunk->GetAddress();
		Check(QueryMapping(address) == Mapping::ReadExecute, "published thunk is RX, never RWX");
		thunk.reset(); // No published callers: owner has trivially quiesced.
		Check(QueryMapping(address) == Mapping::Absent, "destructor releases owned executable mapping");
	}
}

struct Snapshot {
	std::array<uint64_t, 8> gp {}; // RAX, RDI, RSI, RDX, RCX, R8, R9, R10.
	std::array<std::array<uint64_t, 2>, 8> xmm {};
	std::array<uint64_t, 6> nonvolatile {}; // RBX, RBP, R12..15.
	std::array<uint64_t, 4> stack {};
	uint64_t rsp = 0;
	uint64_t caller = 0;
	uint64_t after_rsp = 0;
	uint64_t invocations = 0;
};
constexpr uint64_t Result = 0xfedcba9876543210;

// Raw target captures entry state BEFORE a compiler prologue can change it.
class Capture final: public Xbyak::CodeGenerator {
public:
	Capture(void* bytes, size_t size, Snapshot* snapshot): CodeGenerator(size, bytes) {
		mov(r11, reinterpret_cast<uint64_t>(snapshot));
		const std::array<Xbyak::Reg64, 8> gp {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
		for (size_t i = 0; i < gp.size(); ++i) { mov(ptr[r11 + offsetof(Snapshot, gp) + i * 8], gp[i]); }
		for (int i = 0; i < 8; ++i) { movdqu(ptr[r11 + offsetof(Snapshot, xmm) + i * 16], Xbyak::Xmm(i)); }
		const std::array<Xbyak::Reg64, 6> nv {rbx, rbp, r12, r13, r14, r15};
		for (size_t i = 0; i < nv.size(); ++i) { mov(ptr[r11 + offsetof(Snapshot, nonvolatile) + i * 8], nv[i]); }
		mov(ptr[r11 + offsetof(Snapshot, rsp)], rsp);
		mov(rax, ptr[rsp]);
		mov(ptr[r11 + offsetof(Snapshot, caller)], rax);
		for (int i = 0; i < 4; ++i) {
			mov(rax, ptr[rsp + 8 + i * 8]);
			mov(ptr[r11 + offsetof(Snapshot, stack) + i * 8], rax);
		}
		inc(qword[r11 + offsetof(Snapshot, invocations)]);
		mov(rax, Result);
		ret();
		ready();
	}
};

void TestRawFirstCall() {
	Snapshot actual;
	std::array<uint8_t, 1024> target_bytes {};
	Capture capture(target_bytes.data(), target_bytes.size(), &actual);
	Executable target(capture);
	Record record;
	record.target = target.Address();
	auto thunk = DeferredImportThunk::Create(&record, Resolve);
	Check(bool(thunk), "create raw thunk");
	Snapshot expected;
	for (size_t i = 0; i < 8; ++i) {
		expected.gp[i] = 0x8877665544332200 + i;
		expected.xmm[i] = {0x1122334455667700 + i, 0x99aabbccddeeff00 + i};
	}
	for (size_t i = 0; i < 6; ++i) { expected.nonvolatile[i] = 0xaabbccddeeff0010 + i; }
	for (size_t i = 0; i < 4; ++i) { expected.stack[i] = 0x0123456789abcde0 + i; }

	std::array<uint8_t, 1024> caller_bytes {};
	Xbyak::CodeGenerator caller(caller_bytes.size(), caller_bytes.data());
	using namespace Xbyak::util;
	const std::array<Xbyak::Reg64, 6> nv {rbx, rbp, r12, r13, r14, r15};
	for (const auto& reg: nv) { caller.push(reg); }
	caller.sub(rsp, 40); // Four stack args + padding; pre-call RSP = 0 mod 16.
	for (size_t i = 0; i < 4; ++i) {
		caller.mov(rax, expected.stack[i]); caller.mov(caller.ptr[rsp + i * 8], rax);
	}
	for (size_t i = 0; i < nv.size(); ++i) { caller.mov(nv[i], expected.nonvolatile[i]); }
	caller.mov(r11, reinterpret_cast<uint64_t>(&expected.xmm));
	for (int i = 0; i < 8; ++i) { caller.movdqu(Xbyak::Xmm(i), caller.ptr[r11 + i * 16]); }
	const std::array<Xbyak::Reg64, 8> gp {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
	for (size_t i = 0; i < gp.size(); ++i) { caller.mov(gp[i], expected.gp[i]); }
	caller.mov(r11, thunk->GetAddress());
	caller.call(r11);
	const auto continuation = caller.getSize();
	caller.mov(r11, reinterpret_cast<uint64_t>(&actual));
	caller.mov(caller.ptr[r11 + offsetof(Snapshot, after_rsp)], rsp);
	caller.add(rsp, 40);
	for (auto it = nv.rbegin(); it != nv.rend(); ++it) { caller.pop(*it); }
	caller.ret(); caller.ready();
	Executable driver(caller);
	using Call = uint64_t (KYTY_SYSV_ABI *)();
	Check(reinterpret_cast<Call>(driver.Address())() == Result, "first-call nonzero return");
	Check(actual.gp == expected.gp, "all integer args, full RAX and R10 survive");
	Check(actual.xmm == expected.xmm, "all 128 bits of XMM0..7 survive");
	Check(actual.nonvolatile == expected.nonvolatile, "callee-saved registers survive");
	Check(actual.stack == expected.stack, "original stack arguments survive");
	Check(actual.rsp % 16 == 8 && actual.rsp + 8 == actual.after_rsp, "original stack/alignment restored");
	Check(actual.caller == driver.Address() + continuation, "original return continuation");
	Check(record.caller.load() == actual.caller, "resolver receives original guest caller");
	Check(record.calls.load() == 1 && actual.invocations == 1, "first invocation executes exactly once");
}

struct MixedResult { uint64_t integer; double real; };
std::atomic<uint64_t> mixed_calls {0};
MixedResult KYTY_SYSV_ABI Mixed(__m128 lanes,
    uint64_t a0, double b0, uint64_t a1, double b1, uint64_t a2, double b2, uint64_t a3, double b3,
    uint64_t a4, double b4, uint64_t a5, double b5, uint64_t a6, double b6, uint64_t a7, double b7) {
	const std::array<uint64_t, 8> ints {a0, a1, a2, a3, a4, a5, a6, a7};
	const std::array<double, 8> doubles {b0, b1, b2, b3, b4, b5, b6, b7};
	for (size_t i = 0; i < 8; ++i) {
		Check(ints[i] == a0 + i && doubles[i] == double(i) + 1.25, "mixed register/stack arguments");
	}
	alignas(16) float values[4];
	_mm_store_ps(values, lanes);
	Check(values[0] == 1 && values[1] == 2 && values[2] == 4 && values[3] == 8, "typed vector lanes");
	mixed_calls.fetch_add(1, std::memory_order_relaxed);
	return {8 * a0 + 28 + 0x1000, b0 + b7 + values[3]};
}

void CallMixed(uint64_t address, uint64_t first) {
	auto call = reinterpret_cast<decltype(&Mixed)>(address);
	const auto result = call(_mm_set_ps(8, 4, 2, 1),
	    first, 1.25, first + 1, 2.25, first + 2, 3.25, first + 3, 4.25,
	    first + 4, 5.25, first + 5, 6.25, first + 6, 7.25, first + 7, 8.25);
	Check(result.integer == 8 * first + 28 + 0x1000 && result.real == 17.5, "mixed RAX/XMM0 return and first-call output");
}

void TestMixedAndConcurrentCalls() {
	Record record;
	record.target = reinterpret_cast<uint64_t>(&Mixed);
	auto thunk = DeferredImportThunk::Create(&record, Resolve);
	Check(bool(thunk), "create typed thunk");
	std::array<uint8_t, 512> sealed {};
	std::memcpy(sealed.data(), reinterpret_cast<const void*>(thunk->GetAddress()), sealed.size());
	const auto baseline = mixed_calls.load();
	CallMixed(thunk->GetAddress(), 41);
	Check(record.calls.load() == 1 && mixed_calls.load() == baseline + 1, "typed first invocation exactly once");
	Check(std::memcmp(sealed.data(), reinterpret_cast<const void*>(thunk->GetAddress()), sealed.size()) == 0,
	      "resolution does not patch executable code");

	constexpr uint64_t Threads = 8, Calls = 128;
	Record shared;
	shared.target = reinterpret_cast<uint64_t>(&Mixed);
	shared.rendezvous = Threads;
	auto concurrent = DeferredImportThunk::Create(&shared, Resolve);
	Check(bool(concurrent), "create shared thunk");
	std::vector<std::thread> threads;
	for (uint64_t i = 0; i < Threads; ++i) {
		threads.emplace_back([&, i] {
			for (uint64_t j = 0; j < Calls; ++j) { CallMixed(concurrent->GetAddress(), i * Calls + j + 1); }
			// Independent allocation and reclamation also happen concurrently.
			Record local;
			local.target = reinterpret_cast<uint64_t>(&Mixed);
			auto own = DeferredImportThunk::Create(&local, Resolve);
			Check(bool(own), "concurrent allocation");
			CallMixed(own->GetAddress(), i + 1);
			Check(local.calls.load() == 1, "independent record identity");
		});
	}
	for (auto& thread: threads) { thread.join(); }
	Check(shared.calls.load() == Threads * Calls, "one resolution per concurrent invocation");
	Check(mixed_calls.load() == baseline + 1 + Threads * (Calls + 1), "no lost/doubled target invocations");
}

void TestCompilerVarargsCaller() {
	Snapshot actual;
	std::array<uint8_t, 1024> bytes {};
	Capture code(bytes.data(), bytes.size(), &actual);
	Executable target(code);
	Record record;
	record.target = target.Address();
	auto thunk = DeferredImportThunk::Create(&record, Resolve);
	Check(bool(thunk), "create varargs thunk");
	using Variadic = uint64_t (KYTY_SYSV_ABI *)(uint64_t, ...);
	const auto result = reinterpret_cast<Variadic>(thunk->GetAddress())(
	    uint64_t(10), uint64_t(11), uint64_t(12), uint64_t(13), uint64_t(14), uint64_t(15), uint64_t(16), uint64_t(17),
	    1.25, 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.25, 9.25, 10.25);
	Check(result == Result && actual.invocations == 1 && record.calls.load() == 1, "variadic first call executes once");
	Check((actual.gp[0] & 0xff) == 8, "compiler-supplied AL varargs vector count preserved");
	for (size_t i = 0; i < 6; ++i) { Check(actual.gp[i + 1] == 10 + i, "varargs GP registers"); }
	for (size_t i = 0; i < 8; ++i) {
		double value = 0;
		std::memcpy(&value, &actual.xmm[i][0], sizeof(value));
		Check(value == double(i) + 1.25, "varargs FP registers");
	}
	Check(actual.stack[0] == 16 && actual.stack[1] == 17, "overflow integer varargs");
	for (size_t i = 2; i < 4; ++i) {
		double value = 0;
		std::memcpy(&value, &actual.stack[i], sizeof(value));
		Check(value == double(i) + 7.25, "overflow floating varargs");
	}
}

bool failure_resolver_cleaned = false;
[[noreturn]] void KYTY_SYSV_ABI FailureGateway() noexcept {
	Check(failure_resolver_cleaned, "resolver RAII completes before failure transfer");
	// Isolated synthetic host fixture ONLY. Production must fail just the owning
	// app; exiting the host is explicitly not the production failure policy.
	std::_Exit(27);
}

uint64_t KYTY_SYSV_ABI ControlledFailure(void*, uint64_t caller) noexcept {
	Check(caller != 0, "failure callback receives guest caller");
	struct Cleanup {
		~Cleanup() { failure_resolver_cleaned = true; }
	} cleanup;
	return reinterpret_cast<uint64_t>(FailureGateway);
}

struct RecoveryRecord {
	Loader::GuestCallBoundary* boundary = nullptr;
	uint64_t thunk = 0;
	uint64_t caller = 0;
	unsigned cleanups = 0;
	bool guest_continued = false;
};

uint64_t KYTY_SYSV_ABI ResolveToBoundary(void* opaque, uint64_t caller) noexcept {
	auto& record = *static_cast<RecoveryRecord*>(opaque);
	struct Cleanup {
		RecoveryRecord& record;
		~Cleanup() { ++record.cleanups; }
	} cleanup {record};
	record.caller = caller;
	return reinterpret_cast<uint64_t>(record.boundary->FailureFunction());
}

uint64_t KYTY_SYSV_ABI CallUnresolvedFixture(void* opaque) {
	auto* record = static_cast<RecoveryRecord*>(opaque);
	using Call = uint64_t (KYTY_SYSV_ABI *)(uint64_t, double);
	(void)reinterpret_cast<Call>(record->thunk)(0x12345678, 9.25);
	record->guest_continued = true;
	return 0;
}

void TestActualRecoveryHandoff() {
	RecoveryRecord record;
	Loader::GuestCallBoundaryStatus status;
	auto boundary = Loader::GuestCallBoundary::Create({&record, 1}, &status);
	if (status == Loader::GuestCallBoundaryStatus::Unsupported) {
		std::puts("DeferredImportThunkTests: recovery handoff skipped (unsupported boundary capability)");
		return;
	}
	Check(bool(boundary), "create actual recovery boundary");
	record.boundary = boundary.get();
	auto thunk = DeferredImportThunk::Create(&record, ResolveToBoundary);
	Check(bool(thunk), "create actual recovery thunk");
	record.thunk = thunk->GetAddress();
	for (unsigned invocation = 1; invocation <= 8; ++invocation) {
		const auto outcome = boundary->Invoke(CallUnresolvedFixture, &record);
		Check(outcome.status == Loader::GuestCallBoundaryStatus::Ok &&
		      outcome.tag == Loader::GuestCallOutcomeTag::Aborted && record.cleanups == invocation &&
		      record.caller != 0 && !record.guest_continued && Loader::GuestCallBoundary::CurrentDepth() == 0,
		      "actual thunk returns from resolver cleanup then aborts only its innermost fixture segment");
	}
}

int FailureChild() {
	Record record;
	auto thunk = DeferredImportThunk::Create(&record, ControlledFailure);
	Check(bool(thunk), "create controlled-failure thunk");
	using Call = uint64_t (KYTY_SYSV_ABI *)();
	(void)reinterpret_cast<Call>(thunk->GetAddress())();
	return 73; // Nonreturning failure gateway must not become an API result.
}

uint64_t KYTY_SYSV_ABI InvalidNullResolver(void*, uint64_t) noexcept { return 0; }
int NullChild() {
	Record record;
	auto thunk = DeferredImportThunk::Create(&record, InvalidNullResolver);
	Check(bool(thunk), "create invariant-violation thunk");
	using Call = uint64_t (KYTY_SYSV_ABI *)();
	(void)reinterpret_cast<Call>(thunk->GetAddress())();
	return 73; // A fabricated return must fail the death test.
}

void TestFailureChild(bool null_invariant) {
#if defined(_WIN32)
	wchar_t executable[32768] {};
	Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "locate death-test executable");
	std::wstring command = L"\"" + std::wstring(executable) +
	                       (null_invariant ? L"\" --null-child" : L"\" --failure-child");
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                     nullptr, nullptr, &startup, &process) != FALSE, "start isolated null death test");
	const auto waited = WaitForSingleObject(process.hProcess, 30000);
	if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 74); }
	Check(waited == WAIT_OBJECT_0, "failure child exits promptly");
	DWORD status = 0;
	Check(GetExitCodeProcess(process.hProcess, &status) != FALSE, "null trap child status");
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	Check(status == (null_invariant ? 0xc000001d : 27), "failure callback never returns a fabricated API result");
#else
	const auto child = fork();
	Check(child >= 0, "fork null death test");
	if (child == 0) { alarm(10); _exit(null_invariant ? NullChild() : FailureChild()); }
	int status = 0;
	Check(waitpid(child, &status, 0) == child, "wait null death test");
	if (null_invariant) {
		Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL, "null resolver traps with SIGILL");
	} else {
		Check(WIFEXITED(status) && WEXITSTATUS(status) == 27, "failure callback does not return an API result");
	}
#endif
}
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
	Vm::Init();
	if (argc == 2 && std::strcmp(argv[1], "--null-child") == 0) { return NullChild(); }
	if (argc == 2 && std::strcmp(argv[1], "--failure-child") == 0) { return FailureChild(); }
	Check(!DeferredImportThunk::Create(nullptr, Resolve), "null record rejected");
	Record record;
	Check(!DeferredImportThunk::Create(&record, nullptr), "null resolver rejected");
	TestFailureChild(true); // Before any fixture worker threads are started.
	TestFailureChild(false);
	TestSealingAndLifetime();
	TestRawFirstCall();
	TestCompilerVarargsCaller();
	TestMixedAndConcurrentCalls();
	TestActualRecoveryHandoff();
	std::puts("DeferredImportThunkTests: passed");
	return 0;
}
