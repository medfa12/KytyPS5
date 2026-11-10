// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/guestCallBoundary.h"

#include <cstddef>
#include <initializer_list>
#include <new>
#include <type_traits>

#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define KYTY_GUEST_CALL_BOUNDARY_WIN64 1
#else
#define KYTY_GUEST_CALL_BOUNDARY_WIN64 0
#endif

namespace Loader {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
namespace {
constexpr size_t kCodeSize = 4096;
struct alignas(16) BoundaryFrame {
	BoundaryFrame* previous = nullptr;
	const void* implementation = nullptr;
	GuestCallOwner owner {};
	DWORD thread = 0;
	unsigned depth = 0;
	uintptr_t anchor = 0;
	uint64_t value = 0;
	uint32_t tag = 0;
	bool active = false;
	GuestBoundaryStopState* stop_state = nullptr;
	uint64_t generation = 0;
	alignas(16) uint8_t fx[512] {};
};
static_assert(std::is_standard_layout_v<BoundaryFrame> && std::is_trivially_destructible_v<BoundaryFrame>);
static_assert(offsetof(BoundaryFrame, fx) % 16 == 0);
static thread_local BoundaryFrame* g_boundary = nullptr;
static thread_local GuestBoundaryStopState* g_stop_state = nullptr;
static void __attribute__((ms_abi)) PublishInitializedFrame(BoundaryFrame* frame) noexcept {
	if (frame->stop_state) { frame->stop_state->top.store(frame, std::memory_order_release); }
}

// Called by emitted MS-ABI code, AFTER CLD and FP normalization. Integer-only
// source, no explicit ownership/locks/callbacks or nontrivial automatic objects.
// Compiler TLS access (including emulated TLS) completes normally; this host
// frame and any TLS-runtime helper frames are never discarded.
static BoundaryFrame* __attribute__((ms_abi)) ValidateAbort(const void* identity, uint64_t token,
                                                          const void* implementation) noexcept {
	auto* frame = g_boundary;
	if (frame == nullptr || !frame->active || frame->implementation != implementation ||
	    frame->thread != GetCurrentThreadId() || frame->owner.identity != identity || frame->owner.token != token) {
		return nullptr;
	}
	return frame;
}

// A nonlocal RET to an older call cannot maintain CET's shadow stack. Fail
// closed, including when we cannot query it. Numeric policy avoids requiring
// newer SDK declarations: ProcessUserShadowStackPolicy = 15, one DWORD flags.
static bool RecoveryPermitted() noexcept {
	DWORD policy = 0;
	return GetProcessMitigationPolicy(GetCurrentProcess(), static_cast<PROCESS_MITIGATION_POLICY>(15),
	                                  &policy, sizeof(policy)) && (policy & 1u) == 0;
}

enum Reg : unsigned { A, C, D, B, SP, BP, SI, DI, R8, R9, R10, R11, R12, R13, R14, R15 };
// Fixed private instruction vocabulary, not an assembler for guest input.
struct Encoder {
	uint8_t* code;
	size_t size = 0;
	bool ok = true;
	void Byte(uint8_t v) noexcept { if (size < kCodeSize) code[size++] = v; else ok = false; }
	void U32(uint32_t v) noexcept { for (unsigned i = 0; i != 4; ++i) Byte(static_cast<uint8_t>(v >> (i * 8))); }
	void U64(uint64_t v) noexcept { for (unsigned i = 0; i != 8; ++i) Byte(static_cast<uint8_t>(v >> (i * 8))); }
	void Rex(bool wide, unsigned reg, unsigned base) noexcept {
		Byte(static_cast<uint8_t>(0x40 | (wide ? 8 : 0) | ((reg >> 3) << 2) | (base >> 3)));
	}
	void Imm(unsigned reg, uint64_t value) noexcept { Rex(true, 0, reg); Byte(0xb8 + (reg & 7)); U64(value); }
	void Mov(unsigned dest, unsigned src) noexcept {
		Rex(true, src, dest); Byte(0x89); Byte(0xc0 | ((src & 7) << 3) | (dest & 7));
	}
	void Address(unsigned field, unsigned base, size_t offset) noexcept {
		Byte(0x80 | ((field & 7) << 3) | (base & 7)); if ((base & 7) == SP) Byte(0x24);
		U32(static_cast<uint32_t>(offset));
	}
	void Store(unsigned base, size_t offset, unsigned reg) noexcept {
		Rex(true, reg, base); Byte(0x89); Address(reg, base, offset);
	}
	void Load(unsigned reg, unsigned base, size_t offset) noexcept {
		Rex(true, reg, base); Byte(0x8b); Address(reg, base, offset);
	}
	void Store32(unsigned base, size_t offset, uint32_t value) noexcept {
		Rex(false, 0, base); Byte(0xc7); Address(0, base, offset); U32(value);
	}
	void Fx(unsigned base, size_t offset, bool restore) noexcept {
		Rex(true, 0, base); Byte(0x0f); Byte(0xae); Address(restore ? 1 : 0, base, offset);
	}
	void Push(unsigned reg) noexcept { if (reg >= 8) Byte(0x41); Byte(0x50 + (reg & 7)); }
	void Pop(unsigned reg) noexcept { if (reg >= 8) Byte(0x41); Byte(0x58 + (reg & 7)); }
	void Endbr() noexcept { Byte(0xf3); Byte(0x0f); Byte(0x1e); Byte(0xfa); }
	void CallA() noexcept { Byte(0xff); Byte(0xd0); }
	void Stack(uint8_t op, uint32_t amount) noexcept { Byte(0x48); Byte(0x81); Byte(op); U32(amount); }
	void PatchForward(size_t displacement, size_t target) noexcept {
		if (target < displacement + 4 || target - displacement - 4 > 0x7fffffffu || displacement + 4 > size) {
			ok = false; return;
		}
		const auto delta = static_cast<uint32_t>(target - displacement - 4);
		for (unsigned i = 0; i != 4; ++i) code[displacement + i] = static_cast<uint8_t>(delta >> (i * 8));
	}
	// R11 is trusted boundary storage. No guest register is needed after this.
	void RestoreHost() noexcept {
		Fx(R11, offsetof(BoundaryFrame, fx), true);
		Load(SP, R11, offsetof(BoundaryFrame, anchor));
		for (auto reg: {R15, R14, R13, R12, DI, SI, BP, B}) Pop(reg);
		Byte(0x9d); Byte(0xc3); // popfq; ret to the ordinary Invoke call continuation
	}
};
} // namespace

struct GuestCallBoundary::Impl {
	void* code = nullptr;
	GuestCallOwner owner {};
	using Enter = void (__attribute__((ms_abi)) *)(BoundaryFrame*, GuestBoundaryFunction, void*);
	Enter enter = nullptr;
	GuestBoundaryAbortFunction abort = nullptr;
	GuestBoundaryFailureFunction failure = nullptr;
	uint64_t async_stop = 0;
	~Impl() { if (code != nullptr) VirtualFree(code, 0, MEM_RELEASE); }
	bool Emit() noexcept {
		code = VirtualAlloc(nullptr, kCodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (code == nullptr) return false;
		Encoder e {static_cast<uint8_t*>(code)};
		// MS ABI entry: RCX frame, RDX SysV function, R8 guest argument.
		e.Endbr(); e.Byte(0x9c);
		for (auto reg: {B, BP, SI, DI, R12, R13, R14, R15}) e.Push(reg);
		e.Mov(R12, C); // Guest normal return must preserve SysV R12.
		e.Store(R12, offsetof(BoundaryFrame, anchor), SP);
		e.Fx(R12, offsetof(BoundaryFrame, fx), false);
		// Publish only after anchor/FX initialization. MS ABI preserves SI/DI;
		// this helper returns normally before the disposable guest segment starts.
		e.Byte(0xfc); e.Mov(SI, D); e.Mov(DI, R8); e.Mov(C, R12);
		e.Stack(0xec, 32); e.Imm(A, reinterpret_cast<uintptr_t>(&PublishInitializedFrame)); e.CallA(); e.Stack(0xc4, 32);
		e.Byte(0xfc); e.Mov(A, SI); e.CallA();
		e.Mov(R11, R12);
		e.Store(R11, offsetof(BoundaryFrame, value), A);
		e.Store32(R11, offsetof(BoundaryFrame, tag), static_cast<uint32_t>(GuestCallOutcomeTag::Returned));
		e.RestoreHost();

		const auto emit_abort = [&](bool failure_entry) {
			const size_t offset = e.size;
			// SysV callable gateway: RDI identity, RSI token. Save the guest's ABI
			// nonvolatiles/FP/flags so REJECTION behaves like a normal SysV call.
			e.Endbr(); e.Byte(0x9c);
			for (auto reg: {B, BP, R12, R13, R14, R15}) e.Push(reg);
			constexpr uint32_t local_size = 560; // 32 MS shadow + 512 FX + 16 scratch
			e.Stack(0xec, local_size);
			e.Fx(SP, 32, false);
			e.Byte(0xfc); e.Byte(0xdb); e.Byte(0xe3); // cld; fninit (no pending guest x87 trap)
			e.Store32(SP, 544, 0x1f80);
			e.Byte(0x0f); e.Byte(0xae); e.Address(2, SP, 544); // ldmxcsr, masked defaults
			if (failure_entry) {
				e.Imm(DI, reinterpret_cast<uintptr_t>(owner.identity)); e.Imm(SI, owner.token);
			}
			e.Mov(C, DI); e.Mov(D, SI); e.Imm(R8, reinterpret_cast<uintptr_t>(this));
			e.Imm(A, reinterpret_cast<uintptr_t>(&ValidateAbort)); e.CallA();
			e.Byte(0x48); e.Byte(0x85); e.Byte(0xc0); // test rax,rax
			e.Byte(0x0f); e.Byte(0x84); const size_t rejection = e.size; e.U32(0);
			// Validator has RETURNED: only guest/trivial transition frames are now
			// above the innermost boundary. No C++ unwind or guest return occurs.
			e.Mov(R11, A);
			e.Store32(R11, offsetof(BoundaryFrame, tag), static_cast<uint32_t>(GuestCallOutcomeTag::Aborted));
			e.RestoreHost();
			e.PatchForward(rejection, e.size);
			if (failure_entry) {
				e.Byte(0x0f); e.Byte(0x0b); // No matching boundary: never fabricate an API return.
			} else {
				e.Fx(SP, 32, true); e.Stack(0xc4, local_size);
				for (auto reg: {R15, R14, R13, R12, BP, B}) e.Pop(reg);
				e.Byte(0xb8); e.U32(0); e.Byte(0x9d); e.Byte(0xc3); // Rejected; restore guest flags; ret
			}
			return offset;
		};
		const size_t abort_offset = emit_abort(false);
		const size_t failure_offset = emit_abort(true);
		const size_t stop_offset = e.size;
		// Arbitrary interrupted RIP/RSP entry: NO push, call, TLS access or use
		// of the interrupted guest stack. R11 is a suspended, pinned top frame.
		// All unsupported asynchronous/context routes are excluded before arming.
		e.Endbr(); e.Byte(0xfc);
		e.Store32(R11, offsetof(BoundaryFrame, tag), static_cast<uint32_t>(GuestCallOutcomeTag::Stopped));
		e.RestoreHost();
		if (!e.ok) return false;
		DWORD old = 0;
		if (!VirtualProtect(code, kCodeSize, PAGE_EXECUTE_READ, &old) ||
		    !FlushInstructionCache(GetCurrentProcess(), code, e.size)) return false;
		enter = reinterpret_cast<Enter>(code);
		abort = reinterpret_cast<GuestBoundaryAbortFunction>(static_cast<uint8_t*>(code) + abort_offset);
		failure = reinterpret_cast<GuestBoundaryFailureFunction>(static_cast<uint8_t*>(code) + failure_offset);
		async_stop = reinterpret_cast<uint64_t>(static_cast<uint8_t*>(code) + stop_offset);
		return true;
	}
};
#else
struct GuestCallBoundary::Impl {};
#endif

GuestCallBoundary::GuestCallBoundary() noexcept = default;
GuestCallBoundary::~GuestCallBoundary() = default;

std::unique_ptr<GuestCallBoundary> GuestCallBoundary::Create(GuestCallOwner owner, GuestCallBoundaryStatus* status) noexcept {
	const auto finish = [status](GuestCallBoundaryStatus result) { if (status != nullptr) *status = result; };
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	if (owner.identity == nullptr || owner.token == 0) { finish(GuestCallBoundaryStatus::InvalidArgument); return nullptr; }
	if (!RecoveryPermitted()) { finish(GuestCallBoundaryStatus::Unsupported); return nullptr; }
	std::unique_ptr<GuestCallBoundary> boundary(new (std::nothrow) GuestCallBoundary());
	if (!boundary) { finish(GuestCallBoundaryStatus::OutOfMemory); return nullptr; }
	boundary->impl_.reset(new (std::nothrow) Impl());
	if (!boundary->impl_) { finish(GuestCallBoundaryStatus::OutOfMemory); return nullptr; }
	boundary->impl_->owner = owner;
	if (!boundary->impl_->Emit()) { finish(GuestCallBoundaryStatus::CodeAllocationFailed); return nullptr; }
	finish(GuestCallBoundaryStatus::Ok);
	return boundary;
#else
	(void)owner;
	finish(GuestCallBoundaryStatus::Unsupported);
	return nullptr;
#endif
}

GuestCallOutcome GuestCallBoundary::Invoke(GuestBoundaryFunction guest, void* argument) noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	if (impl_ == nullptr || guest == nullptr ||
	    (g_boundary != nullptr && g_boundary->depth == ~0u)) {
		return {GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0};
	}
	BoundaryFrame frame {};
	frame.previous = g_boundary;
	frame.implementation = impl_.get();
	frame.owner = impl_->owner;
	frame.thread = GetCurrentThreadId();
	frame.depth = frame.previous != nullptr ? frame.previous->depth + 1 : 1;
	frame.active = true;
	frame.stop_state = g_stop_state;
	if (g_stop_state) {
		if (g_stop_state->sequence == UINT64_MAX) { return {GuestCallBoundaryStatus::Unsupported, GuestCallOutcomeTag::NotInvoked, 0}; }
		frame.generation = ++g_stop_state->sequence;
	}
	g_boundary = &frame;
	impl_->enter(&frame, guest, argument);
	// Both ordinary return and the explicit abort landing return through here.
	// Host caller frames remain live and their normal C++ lifetimes continue.
	g_boundary = frame.previous;
	if (frame.stop_state) {
		frame.stop_state->top.store(frame.previous, std::memory_order_release);
		frame.stop_state->in_flight.store(false, std::memory_order_release);
	}
	frame.active = false;
	return {GuestCallBoundaryStatus::Ok, static_cast<GuestCallOutcomeTag>(frame.tag), frame.value};
#else
	(void)guest; (void)argument;
	return {};
#endif
}

GuestBoundaryAbortFunction GuestCallBoundary::AbortFunction() const noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	return impl_ != nullptr ? impl_->abort : nullptr;
#else
	return nullptr;
#endif
}

GuestBoundaryFailureFunction GuestCallBoundary::FailureFunction() const noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	if (impl_ == nullptr || ValidateAbort(impl_->owner.identity, impl_->owner.token, impl_.get()) == nullptr) return nullptr;
	return impl_->failure;
#else
	return nullptr;
#endif
}

unsigned GuestCallBoundary::CurrentDepth() noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	return g_boundary != nullptr ? g_boundary->depth : 0;
#else
	return 0;
#endif
}
void GuestCallBoundary::BindStopState(GuestBoundaryStopState* state) noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	g_stop_state = state;
#else
	(void)state;
#endif
}
void GuestCallBoundary::PublishHostDepth(unsigned depth) noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	if (g_stop_state) { g_stop_state->host_boundary_depth.store(depth, std::memory_order_release); }
#else
	(void)depth;
#endif
}
GuestBoundaryStopLanding GuestCallBoundary::PrepareAsyncStop(const GuestBoundaryStopState& state, uint64_t native_thread,
    uint64_t rsp, uint64_t stack_begin, uint64_t stack_end) const noexcept {
#if KYTY_GUEST_CALL_BOUNDARY_WIN64
	if (!impl_ || state.in_flight.load(std::memory_order_acquire) || stack_end <= stack_begin ||
	    rsp < stack_begin || rsp >= stack_end) { return {}; }
	const auto pointer = reinterpret_cast<uintptr_t>(state.top.load(std::memory_order_acquire));
	if (pointer < stack_begin || pointer >= stack_end || sizeof(BoundaryFrame) > stack_end - pointer || (pointer & 15)) { return {}; }
	const auto* frame = reinterpret_cast<const BoundaryFrame*>(pointer);
	if (!frame->active || frame->stop_state != &state || frame->thread != native_thread || !frame->generation ||
	    frame->implementation != impl_.get() || frame->owner.identity != impl_->owner.identity || frame->owner.token != impl_->owner.token ||
	    frame->tag != static_cast<uint32_t>(GuestCallOutcomeTag::NotInvoked) ||
	    state.host_boundary_depth.load(std::memory_order_acquire) >= frame->depth || !frame->depth ||
	    frame->anchor < stack_begin || frame->anchor >= stack_end || stack_end - frame->anchor < 80 ||
	    (frame->anchor & 15) || rsp >= frame->anchor) { return {}; }
	return {impl_->async_stop, pointer};
#else
	(void)state; (void)native_thread; (void)rsp; (void)stack_begin; (void)stack_end;
	return {};
#endif
}
} // namespace Loader
