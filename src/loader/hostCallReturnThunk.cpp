// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/hostCallReturnThunk.h"

#include <array>
#include <cstring>
#include <exception>
#include <new>

#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xbyak/xbyak.h>
#define KYTY_HOST_RETURN_WIN64 1
#else
#define KYTY_HOST_RETURN_WIN64 0
#endif

namespace Loader {
#if KYTY_HOST_RETURN_WIN64
namespace {
constexpr size_t CodeSize = 4096;
struct Frame {
	const void* implementation = nullptr;
	uint64_t continuation = 0;
	DWORD thread = 0;
};
// This storage is host TLS, not a record linked through guest stack memory.
thread_local std::array<Frame, HostCallReturnThunk::MaximumDepth> frames {};
thread_local unsigned depth = 0;

[[noreturn]] void Invariant() noexcept { __builtin_trap(); }

bool RecoveryPermitted() noexcept {
#if defined(__CET__) && __CET__ != 0
	// Indirect resume at an arbitrary post-CALL guest PC is not an IBT target.
	return false;
#else
	DWORD policy = 0;
	return GetProcessMitigationPolicy(GetCurrentProcess(), static_cast<PROCESS_MITIGATION_POLICY>(15),
	                                  &policy, sizeof(policy)) && (policy & 1u) == 0;
#endif
}
} // namespace

struct HostCallReturnThunk::Impl {
	GuestCallBoundary* boundary = nullptr;
	uint64_t target = 0;
	void* context = nullptr;
	HostCallEnter enter = nullptr;
	HostCallLeave leave = nullptr;
	void* code = nullptr;
	~Impl() { if (code != nullptr && !VirtualFree(code, 0, MEM_RELEASE)) std::terminate(); }

	uint64_t Failure() const noexcept {
		const auto failure = boundary->FailureFunction();
		if (failure == nullptr) Invariant(); // Missing/wrong innermost owner is never guessed.
		return reinterpret_cast<uint64_t>(failure);
	}
	static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Enter(Impl* self, uint64_t caller, uint64_t* entered) noexcept {
		(void)self->Failure(); // Validate exact top boundary before invoking any host code.
		if (caller == 0) Invariant();
		if (depth == MaximumDepth) {
			const auto disposition = self->enter(self->context, caller, HostCallEnterReason::CapacityExceeded);
			if (depth != MaximumDepth || disposition != HostCallDisposition::Abort) Invariant();
			*entered = 0;
			return self->Failure();
		}
		const auto index = depth++;
		frames[index] = {self, caller, GetCurrentThreadId()};
		const auto disposition = self->enter(self->context, caller, HostCallEnterReason::Call);
		if (depth != index + 1 || frames[index].implementation != self ||
		    frames[index].thread != GetCurrentThreadId()) Invariant();
		const auto failure = self->Failure(); // Callback must have returned from every nested boundary.
		if (disposition == HostCallDisposition::Proceed) {
			*entered = 1;
			return self->target;
		}
		if (disposition != HostCallDisposition::Abort) Invariant();
		frames[index] = {};
		--depth;
		*entered = 0;
		return failure;
	}
	static uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Leave(Impl* self, uint64_t* stack_adjust) noexcept {
		if (depth == 0) Invariant();
		const auto index = depth - 1;
		const auto frame = frames[index];
		if (frame.implementation != self || frame.thread != GetCurrentThreadId()) Invariant();
		(void)self->Failure();
		const auto disposition = self->leave(self->context, frame.continuation);
		if (depth != index + 1 || frames[index].implementation != self ||
		    frames[index].thread != GetCurrentThreadId()) Invariant();
		const auto failure = self->Failure();
		if (disposition != HostCallDisposition::Proceed && disposition != HostCallDisposition::Abort) Invariant();
		frames[index] = {};
		--depth;
		const bool abort = disposition == HostCallDisposition::Abort;
		// HLE RET consumed its return slot. The failure entry, unlike the guest
		// continuation, needs normal SysV function-entry alignment (RSP = 8 mod 16).
		*stack_adjust = abort ? 8 : 0;
		return abort ? failure : frame.continuation;
	}

	bool Emit() {
		// Generate in ordinary RW storage. Xbyak never owns executable pages.
		std::array<uint8_t, CodeSize> bytes {};
		Xbyak::CodeGenerator e(bytes.size(), bytes.data());
		using namespace Xbyak::util;
		const auto endbr = [&] { e.db(0xf3); e.db(0x0f); e.db(0x1e); e.db(0xfa); };
		// Explicit REX.W FXSAVE/FXRSTOR: preserve all legacy FP/SIMD state,
		// including ST0/ST1 complex-long-double returns, not only XMM0/1.
		const auto fx = [&](bool restore, int offset) {
			e.db(0x48); e.db(0x0f); e.db(0xae); e.db(restore ? 0x8c : 0x84); e.db(0x24); e.dd(offset);
		};
		const auto normalize = [&](int scratch) {
			e.cld(); e.fninit(); e.mov(e.dword[rsp + scratch], 0x1f80); e.ldmxcsr(e.ptr[rsp + scratch]);
		};
		constexpr int EntrySize = 584; // 64 GPR + aligned 512 FX + 8 scratch/pad; 8 mod 16.
		constexpr int ReturnSize = 544; // FX + RAX/RDX + scratch/stack adjustment; 0 mod 16.
		const std::array<Xbyak::Reg64, 8> gp {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
		Xbyak::Label epilogue, transfer;
		endbr(); e.sub(rsp, EntrySize);
		for (size_t i = 0; i != gp.size(); ++i) e.mov(e.ptr[rsp + i * 8], gp[i]);
		fx(false, 64); normalize(576);
		e.mov(rdi, reinterpret_cast<uint64_t>(this));
		e.mov(rsi, e.ptr[rsp + EntrySize]);
		e.lea(rdx, e.ptr[rsp + 576]);
		e.mov(rax, reinterpret_cast<uint64_t>(&Enter)); e.call(rax); e.mov(r11, rax);
		e.cmp(e.qword[rsp + 576], 0); e.je(transfer);
		// Relative LEA is relocated together with the completed code mapping.
		e.lea(rax, e.ptr[rip + epilogue]); e.mov(e.ptr[rsp + EntrySize], rax);
		e.L(transfer);
		fx(true, 64);
		for (size_t i = 0; i != gp.size(); ++i) e.mov(gp[i], e.ptr[rsp + i * 8]);
		e.add(rsp, EntrySize); e.jmp(r11);

		e.L(epilogue); endbr(); e.sub(rsp, ReturnSize);
		fx(false, 0);
		e.mov(e.ptr[rsp + 512], rax); e.mov(e.ptr[rsp + 520], rdx);
		normalize(528);
		e.mov(rdi, reinterpret_cast<uint64_t>(this)); e.lea(rsi, e.ptr[rsp + 528]);
		e.mov(rax, reinterpret_cast<uint64_t>(&Leave)); e.call(rax); e.mov(r11, rax);
		e.mov(r10, e.ptr[rsp + 528]);
		fx(true, 0);
		e.mov(rax, e.ptr[rsp + 512]); e.mov(rdx, e.ptr[rsp + 520]);
		e.add(rsp, ReturnSize); e.sub(rsp, r10); e.jmp(r11);
		e.ready();
		code = VirtualAlloc(nullptr, CodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (code == nullptr) return false;
		std::memset(code, 0xcc, CodeSize); std::memcpy(code, bytes.data(), e.getSize());
		DWORD previous = 0;
		return VirtualProtect(code, CodeSize, PAGE_EXECUTE_READ, &previous) &&
		       FlushInstructionCache(GetCurrentProcess(), code, e.getSize());
	}
};
#else
struct HostCallReturnThunk::Impl {};
#endif

HostCallReturnThunk::HostCallReturnThunk() noexcept = default;
HostCallReturnThunk::~HostCallReturnThunk() = default;

std::unique_ptr<HostCallReturnThunk> HostCallReturnThunk::Create(
    GuestCallBoundary* boundary, uint64_t typed_target, void* context, HostCallEnter enter,
    HostCallLeave leave, HostCallAbi abi, HostCallReturnThunkStatus* status) noexcept {
	const auto finish = [status](HostCallReturnThunkStatus s) { if (status != nullptr) *status = s; };
#if KYTY_HOST_RETURN_WIN64
	if (boundary == nullptr || typed_target == 0 || enter == nullptr || leave == nullptr) {
		finish(HostCallReturnThunkStatus::InvalidArgument); return nullptr;
	}
	if (abi != HostCallAbi::BaselineSysV || !RecoveryPermitted()) {
		finish(HostCallReturnThunkStatus::Unsupported); return nullptr;
	}
	try {
		auto thunk = std::unique_ptr<HostCallReturnThunk>(new HostCallReturnThunk);
		thunk->impl_.reset(new Impl);
		auto& impl = *thunk->impl_;
		impl.boundary = boundary; impl.target = typed_target; impl.context = context;
		impl.enter = enter; impl.leave = leave;
		if (!impl.Emit()) { finish(HostCallReturnThunkStatus::CodeAllocationFailed); return nullptr; }
		finish(HostCallReturnThunkStatus::Ok); return thunk;
	} catch (const std::bad_alloc&) {
		finish(HostCallReturnThunkStatus::OutOfMemory); return nullptr;
	} catch (...) {
		finish(HostCallReturnThunkStatus::CodeAllocationFailed); return nullptr;
	}
#else
	(void)boundary; (void)typed_target; (void)context; (void)enter; (void)leave; (void)abi;
	finish(HostCallReturnThunkStatus::Unsupported); return nullptr;
#endif
}

uint64_t HostCallReturnThunk::GetAddress() const noexcept {
#if KYTY_HOST_RETURN_WIN64
	return impl_ != nullptr ? reinterpret_cast<uint64_t>(impl_->code) : 0;
#else
	return 0;
#endif
}
unsigned HostCallReturnThunk::CurrentDepth() noexcept {
#if KYTY_HOST_RETURN_WIN64
	return depth;
#else
	return 0;
#endif
}
} // namespace Loader
