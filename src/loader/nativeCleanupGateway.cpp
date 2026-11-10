#include "loader/nativeCleanupGateway.h"
#include "common/threads.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <xbyak/xbyak.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define KYTY_CLEANUP_GATEWAY_WIN64 1
#else
#define KYTY_CLEANUP_GATEWAY_WIN64 0
#endif

namespace Loader::NativeCleanupGateway {
namespace {
constexpr std::array<uint8_t, 5> Prologue {0x55, 0x48, 0x89, 0xe5, 0x53};
constexpr std::array<uint8_t, 7> Load {0x48, 0x8b, 0x3d, 0xf9, 0x20, 0x49, 0x01};
// Core SHA256 f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434.
// Complete caaaf0..caab9c: push rbp at caab02 leaves RSP 0 mod16 after
// either preceding call returns. No synthetic pre-call result guard is used.
// caab2b (file caeb2b) -> caab36: mov qword [213cb00],0, after call caab26.
constexpr std::array<uint8_t, 11> PostJoin {0x48,0xc7,0x05,0xca,0x1f,0x49,0x01,0x00,0x00,0x00,0x00};
// caab47 (file caeb47) -> caab4e: mov rdi,[213cae0], after call [rax] at caab45.
constexpr std::array<uint8_t, 7> PostDestroy {0x48,0x8b,0x3d,0x92,0x1f,0x49,0x01};
bool UserAddress(uintptr_t address, size_t size) noexcept {
	constexpr uint64_t limit = UINT64_C(0x0000800000000000);
	return address >= 0x10000 && address < limit && size <= limit - address;
}
std::span<const uint8_t> Bytes(Site kind) noexcept {
	if (kind == Site::PsnCleanupEntry) { return Prologue; }
	if (kind == Site::IpcInitUnwind) { return Load; }
	if (kind == Site::IpcTermPostJoin) { return PostJoin; }
	if (kind == Site::IpcTermPostDestroy) { return PostDestroy; }
	return {};
}
}

bool Plan(Site kind, uintptr_t site, uintptr_t continuation, std::span<const uint8_t> source,
          std::span<const uint8_t> mapped, Instruction& output) noexcept {
	const auto bytes = Bytes(kind);
	if (bytes.empty() || !UserAddress(site, bytes.size() + 1) || continuation != site + bytes.size() ||
	    source.size() != bytes.size() || mapped.size() != bytes.size() ||
	    !std::equal(bytes.begin(), bytes.end(), source.begin()) ||
	    !std::equal(bytes.begin(), bytes.end(), mapped.begin())) { return false; }
	Instruction instruction;
	instruction.kind = kind; instruction.address = site; instruction.continuation = continuation;
	if (kind != Site::PsnCleanupEntry) {
		// Only the three reviewed positive displacements, not arbitrary decoding.
		const uintptr_t displacement = kind == Site::IpcInitUnwind ? 0x014920f9 :
		    kind == Site::IpcTermPostJoin ? 0x01491fca : 0x01491f92;
		if (!UserAddress(continuation, displacement + sizeof(uint64_t))) { return false; }
		const auto target = continuation + displacement;
		if (target & 7) { return false; }
		if (kind == Site::IpcTermPostJoin) { instruction.store_target = target; }
		else { instruction.load_target = target; }
	}
	instruction.valid = true;
	output = instruction; return true;
}

#if KYTY_CLEANUP_GATEWAY_WIN64
namespace {
constexpr size_t CodeSize = 4096;
constexpr size_t MaxXstate = 4096;
thread_local bool in_guard = false; // recursion rejection only; never a save buffer

void Cpu(uint32_t leaf, uint32_t subleaf, uint32_t (&out)[4]) noexcept {
#if defined(_MSC_VER)
	int data[4]; __cpuidex(data, int(leaf), int(subleaf));
	for (unsigned i = 0; i < 4; ++i) { out[i] = uint32_t(data[i]); }
#else
	__cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}
uint64_t Xcr0() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
	return _xgetbv(0);
#else
	uint32_t low, high;
	__asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return (uint64_t(high) << 32) | low;
#endif
}
bool Xstate(uint32_t& mask, size_t& size) noexcept {
	uint32_t data[4]; Cpu(0, 0, data); if (data[0] < 0xd) { return false; }
	Cpu(1, 0, data);
	if ((data[2] & (3u << 26)) != (3u << 26)) { return false; }
	const auto xcr0 = Xcr0();
	if (xcr0 != 3 && xcr0 != 7 && xcr0 != 0xe7) { return false; }
	if ((xcr0 & 4) && !(data[2] & (1u << 28))) { return false; }
	if (xcr0 == 0xe7) {
		Cpu(7, 0, data); if (!(data[1] & (1u << 16))) { return false; } // AVX512F
	}
	Cpu(0xd, 0, data);
	if (((uint64_t(data[3]) << 32 | data[0]) & xcr0) != xcr0 || data[1] < 576 || data[1] > MaxXstate) { return false; }
	size = data[1];
	size_t previous_end = 576;
	for (unsigned bit = 2; bit <= 7; ++bit) {
		if (!(xcr0 & (uint64_t {1} << bit))) { continue; }
		Cpu(0xd, bit, data);
		// Only standard-format, nonoverlapping user vector components. Offsets
		// and extent come from this CPU, not a fixed legacy FXSAVE-size guess.
		const uint32_t expected = bit == 2 ? 256 : bit == 5 ? 64 : bit == 6 ? 512 : 1024;
		if ((data[2] & 1) || data[0] != expected || data[1] < previous_end || data[1] > size || data[0] > size - data[1]) { return false; }
		previous_end = data[1] + data[0];
	}
	mask = uint32_t(xcr0); return true;
}
bool Memory(uintptr_t address, size_t size, bool writable, bool executable = false) noexcept {
	if (!UserAddress(address, size) || !size) { return false; }
	const auto end = address + size;
	for (auto current = address; current < end;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) != sizeof(info) ||
		    info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
		const auto p = info.Protect & 0xff;
		const bool rw = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		const bool rx = p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		if ((writable && !rw) || (executable && !rx) || (!rw && !rx && p != PAGE_READONLY)) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
		if (!info.RegionSize || base > UINTPTR_MAX - info.RegionSize || base + info.RegionSize <= current) { return false; }
		current = std::min(end, base + info.RegionSize);
	}
	return true;
}
bool Debugged() noexcept {
	BOOL remote = TRUE;
	return IsDebuggerPresent() || !CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) || remote;
}
[[noreturn]] void FailStop() noexcept {
	TerminateProcess(GetCurrentProcess(), 0xc0000409u);
	std::abort();
}
}

struct Gateway::Impl {
	Instruction instruction;
	Config config;
	DWORD owner = 0;
	HANDLE thread = nullptr;
	uint32_t mask = 0;
	size_t xstate_size = 0;
	void* code = nullptr;
	std::atomic_bool active {false};
	~Impl() {
		if (active.load()) { FailStop(); }
		if (code) { VirtualFree(code, 0, MEM_RELEASE); }
		if (thread) { CloseHandle(thread); }
	}
	static void Dispatch(Impl* self, const uint64_t* frame) noexcept {
		Common::HostLockCriticalGuard critical;
		if (in_guard || self->active.exchange(true) || GetCurrentThreadId() != self->owner ||
		    WaitForSingleObject(self->thread, 0) != WAIT_TIMEOUT || Debugged() || Xcr0() != self->mask) { FailStop(); }
		in_guard = true;
		Snapshot snapshot {};
		static_assert(offsetof(Snapshot, rsp) == 16 * sizeof(uint64_t));
		std::memcpy(&snapshot, frame, 16 * sizeof(uint64_t));
		snapshot.rsp = reinterpret_cast<uintptr_t>(frame) + 256;
		snapshot.site = self->instruction.Kind();
		// Stay on the original, bounded, committed guest stack, exactly like
		// ordinary HLE. Do not alter FS/GS or fabricate an application association.
		const auto tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
		const auto base = tib->StackBase, limit = tib->StackLimit;
		if (!self->config.guard(snapshot, self->config.context) || Xcr0() != self->mask ||
		    GetCurrentThreadId() != self->owner || tib != reinterpret_cast<NT_TIB*>(NtCurrentTeb()) ||
		    tib->StackBase != base || tib->StackLimit != limit) { FailStop(); }
		in_guard = false;
		self->active.store(false);
	}

	void Emit() {
		using namespace Xbyak;
		CodeGenerator e(CodeSize, code);
		Label bad;
		const Reg64 registers[] = {e.rax,e.rcx,e.rdx,e.rbx,e.rbp,e.rsi,e.rdi,e.r8,e.r9,e.r10,e.r11,e.r12,e.r13,e.r14,e.r15};
		e.db(0xf3); e.db(0x0f); e.db(0x1e); e.db(0xfa); // ENDBR64 (no flags)
		e.lea(e.rsp, e.ptr[e.rsp - 128]); // protect the complete incoming SysV red zone
		e.pushfq();
		for (const auto& reg: registers) { e.push(reg); }
		e.mov(e.r12, e.rsp); // private restoration frame, never exposed to guard
		// Normalize host flags before any potentially unaligned C++ access. Saved
		// user arithmetic/DF/AC/ID flags are restored only after all host work.
		e.push(0x202); e.popfq();
		e.test(e.qword[e.r12 + 120], 0x1a4100); // TF, NT, VM, VIF, VIP unsupported
		e.jnz(bad, CodeGenerator::T_NEAR);
		e.mov(e.rax, e.r12); e.and_(e.eax, 15);
		e.cmp(e.eax, instruction.Kind() == Site::PsnCleanupEntry ? 8 : 0);
		e.jne(bad, CodeGenerator::T_NEAR);
		const auto reserve = (xstate_size + 63u) & ~size_t(63);
		const auto low = reinterpret_cast<uintptr_t>(config.stack);
		e.mov(e.rax, low + config.guard_stack_budget + reserve + 256);
		e.cmp(e.r12, e.rax); e.jb(bad, CodeGenerator::T_NEAR);
		e.mov(e.rax, low + config.stack_size - 264);
		e.cmp(e.r12, e.rax); e.ja(bad, CodeGenerator::T_NEAR);
		// Windows x64 TEB ClientId.UniqueThread. No host call before XSAVE.
		e.db(0x65); e.db(0x48); e.db(0x8b); e.db(0x04); e.db(0x25); e.dd(0x48);
		e.mov(e.edx, owner); e.cmp(e.rax, e.rdx); e.jne(bad, CodeGenerator::T_NEAR);
		e.xor_(e.ecx, e.ecx); e.xgetbv(); e.cmp(e.eax, mask); e.jne(bad, CodeGenerator::T_NEAR);
		e.test(e.edx, e.edx); e.jnz(bad, CodeGenerator::T_NEAR);
		e.sub(e.rsp, uint32_t(reserve + 63)); e.and_(e.rsp, -64);
		e.mov(e.r13, e.rsp); // aligned per-invocation standard XSAVE area
		// XSAVE does not initialize reserved/header bytes. Zero the WHOLE area
		// first, using integer operations only; XRSTOR must never see stack junk.
		e.mov(e.rdi, e.r13); e.mov(e.ecx, uint32_t(reserve / 8)); e.xor_(e.eax, e.eax);
		e.rep(); e.stosq();
		e.mov(e.eax, mask); e.xor_(e.edx, e.edx);
		e.db(0x49); e.db(0x0f); e.db(0xae); e.db(0x65); e.db(0x00); // xsave64 [r13]
		e.fninit();
		e.sub(e.rsp, 48); // shadow space + aligned private MXCSR constant
		e.mov(e.dword[e.rsp + 32], 0x1f80); e.ldmxcsr(e.ptr[e.rsp + 32]);
		e.mov(e.rcx, reinterpret_cast<uintptr_t>(this)); e.mov(e.rdx, e.r12);
		e.mov(e.rax, reinterpret_cast<uintptr_t>(&Dispatch)); e.call(e.rax);
		e.mov(e.eax, mask); e.xor_(e.edx, e.edx);
		e.db(0x49); e.db(0x0f); e.db(0xae); e.db(0x6d); e.db(0x00); // xrstor64 [r13]
		if (instruction.Kind() == Site::IpcTermPostJoin) {
			// Replay AFTER guard, while the restoration frame still owns RAX.
			// A scratch absolute address avoids a near-allocation requirement and
			// any push into the incoming red zone. The saved actual return value
			// and every other GPR/flag are then restored unchanged. Exactly one
			// aligned qword zero store, matching the original imm32 sign extension.
			e.mov(e.rax, instruction.StoreTarget()); e.mov(e.qword[e.rax], 0);
		}
		e.mov(e.rsp, e.r12);
		for (size_t i = std::size(registers); i; --i) { e.pop(registers[i - 1]); }
		e.popfq(); e.lea(e.rsp, e.ptr[e.rsp + 128]);
		if (instruction.Kind() == Site::PsnCleanupEntry) {
			e.push(e.rbp); e.mov(e.rbp, e.rsp); e.push(e.rbx);
		} else if (instruction.LoadTarget()) {
			// Only RDI changes, exactly as in the displaced load. No push/scratch
			// register, flag mutation, red-zone write or guessed RIP relocation.
			e.mov(e.rdi, instruction.LoadTarget()); e.mov(e.rdi, e.ptr[e.rdi]);
		}
		// AC may be set again: the RIP-indirect pointer itself must be aligned.
		const auto padding = (8 - ((e.getSize() + 6) & 7)) & 7;
		e.db(0xff); e.db(0x25); e.dd(uint32_t(padding));
		for (size_t i = 0; i < padding; ++i) { e.db(0x90); }
		e.dq(instruction.Continuation());
		e.L(bad); e.mov(e.ecx, 7); e.db(0xcd); e.db(0x29); e.ud2(); // fail-fast, never native continuation
		e.ready();
		DWORD previous = 0;
		if (!VirtualProtect(code, CodeSize, PAGE_EXECUTE_READ, &previous) ||
		    !FlushInstructionCache(GetCurrentProcess(), code, e.getSize())) { throw std::bad_alloc(); }
	}
};
#else
struct Gateway::Impl {};
#endif

Gateway::Gateway() noexcept = default;
Gateway::~Gateway() = default;
std::unique_ptr<Gateway> Gateway::Create(const Instruction& instruction, const Config& config, Status* status) noexcept {
	const auto finish = [&](Status value) { if (status) { *status = value; } };
#if KYTY_CLEANUP_GATEWAY_WIN64
	if (!instruction.Valid() || !config.guard || !config.stack || config.guard_stack_budget < 16384 ||
	    config.guard_stack_budget > 1024 * 1024 || config.stack_size < config.guard_stack_budget + MaxXstate + 1024) {
		finish(Status::InvalidArgument); return {};
	}
	uint32_t mask = 0; size_t size = 0;
	if (Debugged() || !Xstate(mask, size)) { finish(Status::Unsupported); return {}; }
	const auto bytes = Bytes(instruction.Kind());
	std::array<uint8_t, 11> mapped {}; SIZE_T copied = 0;
	const auto low = reinterpret_cast<uintptr_t>(config.stack);
	const auto overlap = [&](uintptr_t address, size_t length) {
		return low < address + length && address < low + config.stack_size;
	};
	if (!Memory(reinterpret_cast<uintptr_t>(config.stack), config.stack_size, true) ||
	    overlap(instruction.Address(), bytes.size() + 1) ||
	    (instruction.LoadTarget() && overlap(instruction.LoadTarget(), 8)) ||
	    (instruction.StoreTarget() && overlap(instruction.StoreTarget(), 8)) ||
	    !Memory(reinterpret_cast<uintptr_t>(config.guard), 1, false, true) ||
	    overlap(reinterpret_cast<uintptr_t>(config.guard), 1) ||
	    !Memory(instruction.Address(), bytes.size() + 1, false, true) ||
	    !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(instruction.Address()), mapped.data(), bytes.size(), &copied) ||
	    copied != bytes.size() || !std::equal(bytes.begin(), bytes.end(), mapped.begin()) ||
	    (instruction.LoadTarget() && !Memory(instruction.LoadTarget(), 8, false)) ||
	    (instruction.StoreTarget() && !Memory(instruction.StoreTarget(), 8, true))) {
		finish(Status::InvalidMemory); return {};
	}
	try {
		// Warm the thread-local critical counter before entering generated code.
		(void)Common::GetHostLockCriticalCounter();
		auto gateway = std::unique_ptr<Gateway>(new Gateway());
		gateway->impl = std::make_unique<Impl>(); auto& impl = *gateway->impl;
		impl.instruction = instruction; impl.config = config; impl.owner = GetCurrentThreadId(); impl.mask = mask; impl.xstate_size = size;
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &impl.thread,
		                     SYNCHRONIZE, FALSE, 0)) { finish(Status::Unsupported); return {}; }
		impl.code = VirtualAlloc(nullptr, CodeSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!impl.code) { finish(Status::NoMemory); return {}; }
		impl.Emit(); finish(Status::Ok); return gateway;
	} catch (...) { finish(Status::NoMemory); return {}; }
#else
	(void)instruction; (void)config; finish(Status::Unsupported); return {};
#endif
}
uintptr_t Gateway::Entry() const noexcept {
#if KYTY_CLEANUP_GATEWAY_WIN64
	return impl ? reinterpret_cast<uintptr_t>(impl->code) : 0;
#else
	return 0;
#endif
}
uint64_t Gateway::XstateMask() const noexcept {
#if KYTY_CLEANUP_GATEWAY_WIN64
	return impl ? impl->mask : 0;
#else
	return 0;
#endif
}
size_t Gateway::XstateSize() const noexcept {
#if KYTY_CLEANUP_GATEWAY_WIN64
	return impl ? impl->xstate_size : 0;
#else
	return 0;
#endif
}
} // namespace Loader::NativeCleanupGateway
