#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Loader::NativeCleanupGateway {

// Isolated prerequisite, NOT an installation/admission policy. No guest memory
// is patched. The caller owns source-hash, image-generation, owner/callback and
// continuation-stage admission, plus the lifetime of all code/stack/guard data.
enum class Site { PsnCleanupEntry, IpcInitUnwind, IpcTermPostJoin, IpcTermPostDestroy };
enum class Status { Ok, InvalidArgument, Unsupported, InvalidMemory, NoMemory };

class Instruction final {
public:
	bool Valid() const noexcept { return valid; }
	uintptr_t Address() const noexcept { return address; }
	uintptr_t Continuation() const noexcept { return continuation; }
	uintptr_t LoadTarget() const noexcept { return load_target; }
	uintptr_t StoreTarget() const noexcept { return store_target; }
	Site Kind() const noexcept { return kind; }
private:
	bool valid = false;
	Site kind = Site::PsnCleanupEntry;
	uintptr_t address = 0, continuation = 0, load_target = 0, store_target = 0;
	friend bool Plan(Site, uintptr_t, uintptr_t, std::span<const uint8_t>, std::span<const uint8_t>, Instruction&) noexcept;
};

// Exact recipes only: c2e4f0's five-byte prologue, caa9e0's seven-byte load,
// caab2b's eleven-byte zero store, or caab47's seven-byte load. RIP operands
// must be bounded, eight-byte-aligned user addresses, as in the pinned image.
// source and mapped must BOTH match. Explicit continuation
// must equal site + displaced length. Failure leaves output unchanged. Source
// hash authentication is the parent's responsibility, including hashing the
// actual retained source vector. Addresses may be synthetic in isolated tests.
bool Plan(Site kind, uintptr_t site, uintptr_t continuation,
          std::span<const uint8_t> source, std::span<const uint8_t> mapped,
          Instruction& output) noexcept;

struct Snapshot {
	// Values before any displaced instruction. No writable restoration/control
	// frame is exposed. Guard may copy values, but must not retain this reference.
	// At the two Term sites RAX is the actual preceding join/destroy return.
	// This layer never interprets success or manufactures a stage receipt: the
	// external host guard must validate it before permitting handle clear/free.
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax, flags;
	uintptr_t rsp;
	Site site; // immutable recipe identity supplied by the verified gateway
};
using Guard = bool (*)(const Snapshot&, void*) noexcept;
struct Config {
	void* stack = nullptr;
	size_t stack_size = 0;
	// Maximum downward consumption of the complete host dispatch/guard/callee
	// path, including native API overhead. Caller must enforce this bound;
	// recursion/alloca is not audited. Generated save/shadow space is additional.
	size_t guard_stack_budget = 64 * 1024;
	Guard guard = nullptr;
	void* context = nullptr;
};

class Gateway final {
public:
	// Bind to the creating ACTUAL Windows thread, not a supplied guest identity.
	// Supported: Windows x64, XCR0 exactly 3, 7, or 0xe7 (x87/SSE, AVX,
	// or AVX-512). All enabled state is saved, never a truncated mask. PKRU,
	// AMX and other xstate configurations fail closed pending a separate audit.
	// Stack must be fully committed RW and remain pinned. No application runtime
	// execution association, stack switch, TLS impersonation or guest call occurs.
	static std::unique_ptr<Gateway> Create(const Instruction&, const Config&, Status* = nullptr) noexcept;
	~Gateway();
	Gateway(const Gateway&) = delete;
	Gateway& operator=(const Gateway&) = delete;
	uintptr_t Entry() const noexcept;
	uint64_t XstateMask() const noexcept;
	size_t XstateSize() const noexcept;

	// No invocation wrapper: a vetted native-site transfer enters Entry without
	// adding a return address. Original caller return chain stays on its stack.
	// Guard is HOST ONLY, noexcept, bounded-stack; may block, but must not run
	// guest code, change TLS/identity/XCR0, nonlocally exit, destroy this gateway,
	// or reenter ANY gateway. false or a failed runtime check terminates process.
	// Entry requires the recipe's stack alignment (8 or 0 mod16). Debuggers, TF,
	// asynchronous context rewriting/cancellation, exception/unwind through this
	// generated frame, and faulting displaced loads are NOT supported. RF cannot
	// be faithfully observed by PUSHFQ; breakpoint/resume-flag contexts are excluded.
	// A bad entry stack may fault on the first saves; it cannot resume cleanup.
	// Destroy only after all transfers have returned and the site is unreachable.
private:
	Gateway() noexcept;
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace Loader::NativeCleanupGateway
