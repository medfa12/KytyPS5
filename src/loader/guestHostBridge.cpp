// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/guestHostBridge.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <malloc.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if !defined(_MSC_VER)
#include <cpuid.h>
#endif
#define KYTY_GUEST_HOST_BRIDGE_WIN64 1
#else
#define KYTY_GUEST_HOST_BRIDGE_WIN64 0
#endif

namespace Loader {

#if KYTY_GUEST_HOST_BRIDGE_WIN64
namespace {
constexpr size_t kXsaveCapacity = 4096;
constexpr size_t kCodeCapacity = 4096;
constexpr size_t kMinimumGuestStack = 64 * 1024;
constexpr size_t kHostHeadroom = 64 * 1024;

void Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t (&out)[4]) noexcept {
#if defined(_MSC_VER)
	int v[4];
	__cpuidex(v, static_cast<int>(leaf), static_cast<int>(subleaf));
	for (unsigned i = 0; i != 4; ++i) out[i] = static_cast<uint32_t>(v[i]);
#else
	__cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}

uint64_t Xgetbv0() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
	return _xgetbv(0);
#else
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

bool XstateSupported(uint32_t* mask) noexcept {
	uint32_t r[4];
	Cpuid(0, 0, r);
	if (r[0] < 0x0d) return false;
	Cpuid(1, 0, r);
	constexpr uint32_t kXsave = 1u << 26, kOsxsave = 1u << 27, kAvx = 1u << 28;
	if ((r[2] & (kXsave | kOsxsave)) != (kXsave | kOsxsave)) return false;
	const bool avx = (r[2] & kAvx) != 0;
	const uint64_t xcr0 = Xgetbv0();
	if (xcr0 != 3 && xcr0 != 7) return false;
	if (xcr0 == 7 && !avx) return false;
	Cpuid(0x0d, 0, r);
	const uint64_t supported = (static_cast<uint64_t>(r[3]) << 32) | r[0];
	if ((supported & xcr0) != xcr0 || r[1] < 576 || r[1] > kXsaveCapacity) return false;
	const uint32_t area_size = r[1];
	if (xcr0 == 7) {
		Cpuid(0x0d, 2, r);
		// Standard-format YMM_Hi128, user state, wholly inside the enabled area.
		if ((r[2] & 1) != 0 || r[0] != 256 || r[1] < 576 ||
		    r[1] > area_size || r[0] > area_size - r[1]) return false;
	}
	*mask = static_cast<uint32_t>(xcr0);
	return true;
}

bool WritableCommitted(uintptr_t start, size_t size) noexcept {
	if (!size || start > std::numeric_limits<uintptr_t>::max() - size) return false;
	const uintptr_t end = start + size;
	for (uintptr_t p = start; p < end;) {
		MEMORY_BASIC_INFORMATION m {};
		if (!VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) || m.State != MEM_COMMIT ||
		    (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
		const DWORD prot = m.Protect & 0xff;
		if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY && prot != PAGE_EXECUTE_READWRITE &&
		    prot != PAGE_EXECUTE_WRITECOPY) return false;
		const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
		if (m.RegionSize > std::numeric_limits<uintptr_t>::max() - base) return false;
		const uintptr_t next = base + m.RegionSize;
		if (next <= p) return false;
		p = next < end ? next : end;
	}
	return true;
}

bool Intersects(uintptr_t low, uintptr_t high, const void* object, size_t size) noexcept {
	const uintptr_t other = reinterpret_cast<uintptr_t>(object);
	return other <= std::numeric_limits<uintptr_t>::max() - size && low < other + size && other < high;
}

// Grow the original Windows stack using compiler stack probes while still in
// ordinary host code. After returning, those pages remain committed below the
// live frame, where the generated callback transition can safely use them.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void TouchHostWindow(volatile unsigned char* p, size_t size) noexcept {
	for (size_t offset = 0; offset < size; offset += 4096) p[offset] = 0;
	p[size - 1] = 0;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void CommitHostWindow() noexcept {
	constexpr size_t size = kHostHeadroom + 16 * 1024;
	// The pointer must escape to the separate non-inlined probe. Otherwise
	// scalar replacement can shrink scattered volatile writes to a tiny frame.
	auto* p = static_cast<volatile unsigned char*>(_alloca(size));
	TouchHostWindow(p, size);
}

// A bounded encoder for the deliberately small, fixed instruction vocabulary.
// It emits no guest-derived instructions, relocations, or writable code aliases.
struct Emitter {
	uint8_t* p;
	size_t size = 0;
	bool ok = true;
	void B(uint8_t b) noexcept { if (size < kCodeCapacity) p[size++] = b; else ok = false; }
	void U32(uint32_t v) noexcept { for (int i = 0; i != 4; ++i) B(static_cast<uint8_t>(v >> (i * 8))); }
	void U64(uint64_t v) noexcept { for (int i = 0; i != 8; ++i) B(static_cast<uint8_t>(v >> (i * 8))); }
	void Rex(bool w, unsigned r, unsigned b) noexcept { B(static_cast<uint8_t>(0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | (b >> 3))); }
	void MovImm(unsigned reg, uint64_t value) noexcept { Rex(true, 0, reg); B(static_cast<uint8_t>(0xb8 + (reg & 7))); U64(value); }
	void Mov32(unsigned reg, uint32_t value) noexcept { if (reg >= 8) Rex(false, 0, reg); B(static_cast<uint8_t>(0xb8 + (reg & 7))); U32(value); }
	void RegMov(unsigned dst, unsigned src) noexcept { Rex(true, src, dst); B(0x89); B(static_cast<uint8_t>(0xc0 | ((src & 7) << 3) | (dst & 7))); }
	void MemOp(uint8_t op, unsigned reg, unsigned base, uint32_t disp) noexcept {
		Rex(true, reg, base); B(op); B(static_cast<uint8_t>(0x80 | ((reg & 7) << 3) | (base & 7)));
		if ((base & 7) == 4) B(0x24);
		U32(disp);
	}
	void Load(unsigned reg, unsigned base, size_t disp) noexcept { MemOp(0x8b, reg, base, static_cast<uint32_t>(disp)); }
	void Store(unsigned base, size_t disp, unsigned reg) noexcept { MemOp(0x89, reg, base, static_cast<uint32_t>(disp)); }
	void Lea(unsigned reg, unsigned base, size_t disp) noexcept { MemOp(0x8d, reg, base, static_cast<uint32_t>(disp)); }
	void Push(unsigned reg) noexcept { if (reg >= 8) B(0x41); B(static_cast<uint8_t>(0x50 + (reg & 7))); }
	void Pop(unsigned reg) noexcept { if (reg >= 8) B(0x41); B(static_cast<uint8_t>(0x58 + (reg & 7))); }
	void Call(unsigned reg) noexcept { if (reg >= 8) B(0x41); B(0xff); B(static_cast<uint8_t>(0xd0 | (reg & 7))); }
	// REX.W selects the 64-bit x87 FIP/FDP layout; REX.B alone truncates it.
	void XsaveR11() noexcept { B(0x49); B(0x0f); B(0xae); B(0x23); }
	void XrstorR11() noexcept { B(0x49); B(0x0f); B(0xae); B(0x2b); }
	void Endbr() noexcept { B(0xf3); B(0x0f); B(0x1e); B(0xfa); }
};
enum Reg : unsigned { A, C, D, B, SP, BP, SI, DI, R8, R9, R10, R11, R12, R13, R14, R15 };
} // namespace

struct alignas(64) GuestHostBridge::Impl {
	DWORD owner = 0;
	HANDLE owner_handle = nullptr;
	std::atomic<bool> active {false};
	uintptr_t stack_low = 0;
	size_t stack_size = 0;
	uintptr_t stack_top = 0;
	GuestHostCallback callback = nullptr;
	void* callback_context = nullptr;
	uint32_t mask = 0;
	void* code = nullptr;
	using Enter = uint64_t (*)(Impl*, uintptr_t, uint64_t);
	Enter enter = nullptr;
	uintptr_t host_anchor = 0;
	uintptr_t guest_frame = 0;
	uint64_t guest_return = 0;
	GuestHostSnapshot snapshot {};
	GuestHostResult result {};
	alignas(64) uint8_t host_xsave[kXsaveCapacity] {};
	alignas(64) uint8_t guest_xsave[kXsaveCapacity] {};

	~Impl() { if (code) VirtualFree(code, 0, MEM_RELEASE); if (owner_handle) CloseHandle(owner_handle); }
	static void Dispatch(Impl* self) noexcept {
		self->result = {};
		self->callback(self->snapshot, self->result, self->callback_context);
		if (self->result.tag != GuestHostResultTag::Return || self->result.carry > 1) {
			self->result.value = std::numeric_limits<uint64_t>::max();
			self->result.carry = 1;
		}
	}

	bool Emit() noexcept {
		static_assert(std::is_standard_layout_v<GuestHostSnapshot>);
		static_assert(sizeof(GuestHostSnapshot) == 16 * sizeof(uint64_t));
		static_assert(alignof(Impl) >= 64);
		code = VirtualAlloc(nullptr, kCodeCapacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!code) return false;
		Emitter e {static_cast<uint8_t*>(code)};
		const uintptr_t context = reinterpret_cast<uintptr_t>(this);
		const uintptr_t gateway = reinterpret_cast<uintptr_t>(code);
		e.Endbr();
		// LEA, not SUB: the red zone and incoming flags must survive the save.
		e.B(0x48); e.B(0x8d); e.B(0x64); e.B(0x24); e.B(0x80); // lea rsp,[rsp-128]
		e.B(0x9c); // pushfq
		constexpr unsigned guest_regs[] = {A,C,D,B,BP,SI,DI,R8,R9,R10,R11,R12,R13,R14,R15};
		for (auto r : guest_regs) e.Push(r);
		e.MovImm(R11, context);
		e.Store(R11, offsetof(Impl, guest_frame), SP);
		// Copy only ordinary register values and flags, not the trusted frame.
		for (size_t i = 0; i != 16; ++i) {
			e.Load(A, SP, i * 8);
			e.Store(R11, offsetof(Impl, snapshot) + i * 8, A);
		}
		e.Lea(R11, R11, offsetof(Impl, guest_xsave));
		e.Mov32(A, mask); e.B(0x31); e.B(0xd2); e.XsaveR11();
		e.MovImm(R11, context); e.Lea(R11, R11, offsetof(Impl, host_xsave));
		e.Mov32(A, mask); e.B(0x31); e.B(0xd2); e.XrstorR11();
		e.B(0xfc); // cld, before entering host C++
		e.MovImm(R11, context);
		e.Load(SP, R11, offsetof(Impl, host_anchor));
		e.B(0x48); e.B(0x83); e.B(0xec); e.B(0x20); // aligned MS ABI shadow area
		// Host flags were saved above the eight pushed nonvolatile registers.
		e.Load(A, R11, offsetof(Impl, host_anchor));
		e.B(0xff); e.B(0x70); e.B(0x40); e.B(0x9d); // push [rax+64]; popfq
		e.B(0xfc); // host callback ABI always requires DF=0
		e.RegMov(C, R11);
		e.MovImm(A, reinterpret_cast<uintptr_t>(&Impl::Dispatch)); e.Call(A);
		// Result is bounded and validated by Dispatch; never use it for control flow.
		e.MovImm(R11, context); e.Lea(R11, R11, offsetof(Impl, guest_xsave));
		e.Mov32(A, mask); e.B(0x31); e.B(0xd2); e.XrstorR11();
		e.MovImm(R11, context);
		e.Load(R10, R11, offsetof(Impl, guest_frame));
		e.Load(A, R11, offsetof(Impl, result) + offsetof(GuestHostResult, value));
		e.Store(R10, 14 * 8, A); // saved RAX
		e.Load(D, R10, 15 * 8); // saved flags
		e.B(0x48); e.B(0x83); e.B(0xe2); e.B(0xfe); // and rdx,-2
		// Load the bounded 32-bit carry (not the adjacent tag).
		e.Rex(false, C, R11); e.B(0x8b); e.B(0x8b); e.U32(static_cast<uint32_t>(offsetof(Impl, result) + offsetof(GuestHostResult, carry)));
		e.B(0x48); e.B(0x09); e.B(0xca); // or rdx,rcx
		e.Store(R10, 15 * 8, D);
		e.RegMov(SP, R10);
		for (size_t i = sizeof(guest_regs) / sizeof(guest_regs[0]); i != 0; --i) e.Pop(guest_regs[i - 1]);
		e.B(0x9d); // popfq; no flag-changing instructions before RET
		e.B(0x48); e.B(0x8d); e.B(0xa4); e.B(0x24); e.U32(128); // lea rsp,[rsp+128]
		e.B(0xc3);

		const size_t outer = e.size;
		e.Endbr();
		e.B(0x9c); // save host flags before any flag-changing instruction
		constexpr unsigned host_regs[] = {B,BP,SI,DI,R12,R13,R14,R15};
		for (auto r : host_regs) e.Push(r);
		e.RegMov(R12, C); e.RegMov(R13, D); e.RegMov(R14, R8);
		e.Store(R12, offsetof(Impl, host_anchor), SP);
		e.Lea(R11, R12, offsetof(Impl, host_xsave));
		e.Mov32(A, mask); e.B(0x31); e.B(0xd2); e.XsaveR11();
		e.B(0xfc);
		e.Load(SP, R12, offsetof(Impl, stack_top));
		e.MovImm(DI, gateway); e.RegMov(SI, R14); e.Call(R13);
		// Do not trust a guest register as the context after returning.
		e.MovImm(R11, context); e.Store(R11, offsetof(Impl, guest_return), A);
		e.Load(SP, R11, offsetof(Impl, host_anchor));
		e.Lea(R11, R11, offsetof(Impl, host_xsave));
		e.Mov32(A, mask); e.B(0x31); e.B(0xd2); e.XrstorR11();
		e.MovImm(R11, context); e.Load(A, R11, offsetof(Impl, guest_return));
		for (size_t i = sizeof(host_regs) / sizeof(host_regs[0]); i != 0; --i) e.Pop(host_regs[i - 1]);
		e.B(0x9d); e.B(0xc3);
		if (!e.ok) return false;
		DWORD old = 0;
		if (!VirtualProtect(code, kCodeCapacity, PAGE_EXECUTE_READ, &old) ||
		    !FlushInstructionCache(GetCurrentProcess(), code, e.size)) return false;
		enter = reinterpret_cast<Enter>(static_cast<uint8_t*>(code) + outer);
		return true;
	}
};
#else
struct GuestHostBridge::Impl {};
#endif

GuestHostBridge::GuestHostBridge() noexcept = default;
GuestHostBridge::~GuestHostBridge() = default;

std::unique_ptr<GuestHostBridge> GuestHostBridge::Create(const GuestHostBridgeConfig& config,
                                                         GuestHostBridgeStatus* status) noexcept {
	auto finish = [status](GuestHostBridgeStatus s) { if (status) *status = s; };
#if KYTY_GUEST_HOST_BRIDGE_WIN64
	if (!config.callback || !config.guest_stack || config.guest_stack_size < kMinimumGuestStack) {
		finish(GuestHostBridgeStatus::InvalidArgument); return nullptr;
	}
	const uintptr_t low = reinterpret_cast<uintptr_t>(config.guest_stack);
	if (low > std::numeric_limits<uintptr_t>::max() - config.guest_stack_size ||
	    !WritableCommitted(low, config.guest_stack_size)) {
		finish(GuestHostBridgeStatus::InvalidStack); return nullptr;
	}
	ULONG_PTR host_low = 0, host_high = 0;
	GetCurrentThreadStackLimits(&host_low, &host_high);
	const uintptr_t high = low + config.guest_stack_size;
	if (!host_low || host_high <= host_low || (low < host_high && high > host_low)) {
		finish(GuestHostBridgeStatus::InvalidStack); return nullptr;
	}
	uint32_t mask = 0;
	if (!XstateSupported(&mask)) { finish(GuestHostBridgeStatus::Unsupported); return nullptr; }
	std::unique_ptr<GuestHostBridge> bridge(new (std::nothrow) GuestHostBridge());
	if (!bridge) { finish(GuestHostBridgeStatus::OutOfMemory); return nullptr; }
	bridge->impl_.reset(new (std::nothrow) Impl());
	if (!bridge->impl_) { finish(GuestHostBridgeStatus::OutOfMemory); return nullptr; }
	Impl& i = *bridge->impl_;
	if (Intersects(low, high, bridge.get(), sizeof(*bridge)) || Intersects(low, high, &i, sizeof(i))) {
		finish(GuestHostBridgeStatus::InvalidStack); return nullptr;
	}
	i.owner = GetCurrentThreadId(); i.stack_low = low; i.stack_size = config.guest_stack_size;
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &i.owner_handle,
	                     SYNCHRONIZE, FALSE, 0)) {
		finish(GuestHostBridgeStatus::Unsupported); return nullptr;
	}
	i.stack_top = high & ~uintptr_t(15); i.callback = config.callback;
	i.callback_context = config.callback_context; i.mask = mask;
	if (!i.Emit()) { finish(GuestHostBridgeStatus::CodeAllocationFailed); return nullptr; }
	if (Intersects(low, high, i.code, kCodeCapacity)) {
		finish(GuestHostBridgeStatus::InvalidStack); return nullptr;
	}
	finish(GuestHostBridgeStatus::Ok); return bridge;
#else
	(void)config; finish(GuestHostBridgeStatus::Unsupported); return nullptr;
#endif
}

GuestHostBridgeStatus GuestHostBridge::Run(uintptr_t entry, uint64_t arg, uint64_t* out) noexcept {
#if KYTY_GUEST_HOST_BRIDGE_WIN64
	if (!impl_ || !entry || !out) return GuestHostBridgeStatus::InvalidArgument;
	Impl& i = *impl_;
	if (GetCurrentThreadId() != i.owner || WaitForSingleObject(i.owner_handle, 0) != WAIT_TIMEOUT)
		return GuestHostBridgeStatus::WrongThread;
	bool expected = false;
	if (!i.active.compare_exchange_strong(expected, true, std::memory_order_acquire)) return GuestHostBridgeStatus::Busy;
	struct Release { std::atomic<bool>& active; ~Release() { active.store(false, std::memory_order_release); } } release {i.active};
	uint32_t mask = 0;
	if (!XstateSupported(&mask) || mask != i.mask) return GuestHostBridgeStatus::Unsupported;
	if (!WritableCommitted(i.stack_low, i.stack_size)) return GuestHostBridgeStatus::InvalidStack;
	// Leave a committed window below the live original host stack. Normal Windows
	// stack growth remains available to the callback; no TEB fields are changed.
	uintptr_t marker = reinterpret_cast<uintptr_t>(&mask);
	MEMORY_BASIC_INFORMATION stack_info {};
	if (!VirtualQuery(reinterpret_cast<void*>(marker), &stack_info, sizeof(stack_info)) ||
	    marker < reinterpret_cast<uintptr_t>(stack_info.AllocationBase) ||
	    marker - reinterpret_cast<uintptr_t>(stack_info.AllocationBase) < kHostHeadroom + 128 * 1024)
		return GuestHostBridgeStatus::HostStackUnavailable;
	CommitHostWindow();
	ULONG_PTR host_low = 0, host_high = 0;
	GetCurrentThreadStackLimits(&host_low, &host_high);
	if (marker < kHostHeadroom || marker <= host_low || marker >= host_high ||
	    marker - kHostHeadroom < host_low || !WritableCommitted(marker - kHostHeadroom, kHostHeadroom))
		return GuestHostBridgeStatus::HostStackUnavailable;
	std::memset(i.host_xsave, 0, sizeof(i.host_xsave));
	std::memset(i.guest_xsave, 0, sizeof(i.guest_xsave));
	*out = i.enter(&i, entry, arg);
	return GuestHostBridgeStatus::Ok;
#else
	(void)entry; (void)arg; (void)out; return GuestHostBridgeStatus::Unsupported;
#endif
}
} // namespace Loader
