// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_GUEST_HOST_BRIDGE_H_
#define KYTY_LOADER_GUEST_HOST_BRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Loader {

// A controlled synthetic CALL gateway, NOT a raw syscall ABI or launch API.
// No guest PC, continuation, stack pointer, or XSAVE pointer is exposed here.
struct GuestHostSnapshot {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax, rflags;
};

enum class GuestHostResultTag : uint32_t { Reject = 0, Return = 1 };
struct GuestHostResult {
	uint64_t value = 0;
	GuestHostResultTag tag = GuestHostResultTag::Reject;
	uint32_t carry = 1; // For Return, exactly 0 or 1.
};
using GuestHostCallback = void (*)(const GuestHostSnapshot&, GuestHostResult&, void*) noexcept;

enum class GuestHostBridgeStatus {
	Ok,
	Unsupported,
	InvalidArgument,
	InvalidStack,
	WrongThread,
	Busy,
	HostStackUnavailable,
	OutOfMemory,
	CodeAllocationFailed,
};

struct GuestHostBridgeConfig {
	void* guest_stack = nullptr; // Low address of a caller-owned committed interval.
	size_t guest_stack_size = 0;
	GuestHostCallback callback = nullptr;
	void* callback_context = nullptr;
};

// Windows x64 only. Create registers the calling thread. The caller must keep
// the stack allocation pinned, writable and exclusively owned until destruction.
// A SYSV synthetic entry receives (gateway address in RDI, guest_arg in RSI)
// and returns its uint64_t in RAX. It must obey SYSV callee-saved-register and
// stack rules and invoke the gateway only via CALL on this stack with at least
// 4096 bytes of committed writable headroom, TF clear, and no nested gateway.
// Both host and guest must have TF clear at transition boundaries. Guest code
// must not change XCR0, TEB, host TLS, stack mappings or unwind across
// either transition. No exception/fault recovery is provided. Callback may
// block on the original host stack, but must not throw, destroy this bridge,
// retain the snapshot/result references, or violate the host ABI. A recursive
// Run is rejected before transition state is written. Wrong-thread or nested
// direct gateway entry is outside the contract and is NOT detected.
// This does not admit firmware, arbitrary FS/TLS or unmediated guest syscalls.
// The C++ Run method follows the host ABI and does not promise to preserve
// its caller's arithmetic flags. The emitted outer transition saves/restores
// its own entry flags; callback arithmetic flags are not a C++ ABI guarantee.
class GuestHostBridge final {
public:
	static std::unique_ptr<GuestHostBridge> Create(const GuestHostBridgeConfig& config,
	                                               GuestHostBridgeStatus* status) noexcept;
	~GuestHostBridge();
	GuestHostBridge(const GuestHostBridge&) = delete;
	GuestHostBridge& operator=(const GuestHostBridge&) = delete;
	GuestHostBridge(GuestHostBridge&&) = delete;
	GuestHostBridge& operator=(GuestHostBridge&&) = delete;

	GuestHostBridgeStatus Run(uintptr_t sysv_entry, uint64_t guest_arg,
	                          uint64_t* guest_return) noexcept;

private:
	GuestHostBridge() noexcept;
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace Loader
#endif
