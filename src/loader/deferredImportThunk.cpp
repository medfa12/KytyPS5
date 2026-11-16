#include "loader/deferredImportThunk.h"

#include "common/virtualMemory.h"

#include <array>
#include <cstring>
#include <exception>

#include <xbyak/xbyak.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if !defined(__x86_64__) && !defined(_M_X64)
#error DeferredImportThunk requires x86_64
#endif

namespace Loader {
namespace {
namespace Vm = Common::VirtualMemory;
constexpr uint64_t AllocationSize = 0x1000;
// Entry RSP = 8 mod 16; subtract 200 to obtain 0 mod 16 before CALL.
// [0,64): eight GPRs; [64,192): XMM0..7; [192,200): padding.
constexpr int FrameSize = 200;
static_assert(FrameSize % 16 == 8);

class Code final: public Xbyak::CodeGenerator {
public:
	Code(void* buffer, size_t size, void* record, DeferredImportResolver resolver):
	    Xbyak::CodeGenerator(size, buffer) {
		// ENDBR64 is harmless on baseline hosts and permits an IBT entry.
		db(0xf3); db(0x0f); db(0x1e); db(0xfa);
		sub(rsp, FrameSize);
		const std::array<Xbyak::Reg64, 8> saved {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
		for (size_t i = 0; i < saved.size(); ++i) { mov(ptr[rsp + i * 8], saved[i]); }
		for (int i = 0; i < 8; ++i) { movdqu(ptr[rsp + 64 + i * 16], Xbyak::Xmm(i)); }

		mov(rdi, reinterpret_cast<uint64_t>(record));
		mov(rsi, ptr[rsp + FrameSize]); // Guest's original return continuation.
		mov(rax, reinterpret_cast<uint64_t>(resolver));
		call(rax);
		Xbyak::Label resolved;
		test(rax, rax);
		jnz(resolved);
		ud2(); // Callback violated its contract; never fabricate an API result.
		L(resolved);
		mov(r11, rax);

		for (int i = 0; i < 8; ++i) { movdqu(Xbyak::Xmm(i), ptr[rsp + 64 + i * 16]); }
		for (size_t i = 0; i < saved.size(); ++i) { mov(saved[i], ptr[rsp + i * 8]); }
		add(rsp, FrameSize);
		jmp(r11); // Exactly this invocation; target RET goes directly to caller.
		ready();
	}
};
} // namespace

std::unique_ptr<DeferredImportThunk> DeferredImportThunk::Create(void* record, DeferredImportResolver resolver) {
	if (record == nullptr || resolver == nullptr) { return nullptr; }
	// Xbyak receives ordinary writable storage: it allocates no executable/RWX
	// pages. Generate first so exceptions cannot abandon a VM allocation.
	std::array<uint8_t, 512> bytes {};
	Code code(bytes.data(), bytes.size(), record, resolver);
	auto thunk = std::unique_ptr<DeferredImportThunk>(new DeferredImportThunk);
	// This is host-generated transition code, not guest address-space backing.
	// The Windows guest allocator searches only reserved guest ranges, which are
	// unavailable once runtime memory has claimed them. Match GuestCallBoundary's
	// unconstrained host allocation rather than consuming those reservations.
#if defined(_WIN32)
	const auto address = reinterpret_cast<uint64_t>(VirtualAlloc(nullptr, AllocationSize,
	    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
	const auto address = Vm::Alloc(0, AllocationSize, Vm::Mode::ReadWrite);
#endif
	// Linux VirtualMemory::Alloc currently exposes MAP_FAILED as UINT64_MAX.
	if (address == 0 || address == UINT64_MAX) { return nullptr; }
	thunk->m_address = address;
	auto* destination = reinterpret_cast<void*>(address);
	std::memset(destination, 0xcc, AllocationSize);
	std::memcpy(destination, bytes.data(), code.getSize());
	if (!Vm::Protect(address, AllocationSize, Vm::Mode::ExecuteRead) ||
	    !Vm::FlushInstructionCache(address, AllocationSize)) {
		return nullptr; // RAII releases even a partially sealed allocation.
	}
	return thunk;
}

DeferredImportThunk::~DeferredImportThunk() {
	// Destruction is only legal after owner-enforced quiescence. Losing an
	// owned mapping on failed rollback is an infrastructure invariant failure.
	if (m_address != 0 && !Vm::Free(m_address)) { std::terminate(); }
}

} // namespace Loader
