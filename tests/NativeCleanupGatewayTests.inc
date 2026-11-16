// Include in the allocation fixture's anonymous namespace. Parent TU supplies
// loader/nativeCleanupGateway.h, xbyak/xbyak.h, Windows.h, immintrin.h,
// common/threads.h, <thread>, <atomic>, <chrono>, <cstring>, <array>, <memory>,
// and intrin.h (MSVC) or cpuid.h (GNU/Clang GNU mode).
// Synthetic transfers only: no firmware patch, lifecycle admission or guest
// callback. Invoke TestNativeCleanupGateway() through the parent's test dispatch.
namespace NativeCleanupGatewayTest {
namespace G = Loader::NativeCleanupGateway;
constexpr const char* Name = "NativeCleanupGateway";
constexpr std::array<uint8_t, 5> Prologue {0x55,0x48,0x89,0xe5,0x53};
constexpr std::array<uint8_t, 7> Load {0x48,0x8b,0x3d,0xf9,0x20,0x49,0x01};
constexpr std::array<uint8_t, 11> PostJoin {0x48,0xc7,0x05,0xca,0x1f,0x49,0x01,0,0,0,0};
constexpr std::array<uint8_t, 7> PostDestroy {0x48,0x8b,0x3d,0x92,0x1f,0x49,0x01};
constexpr uint64_t Canary = 0x6758493a2b1c0dfeull;
constexpr uint64_t Loaded = 0xabcdef9876543210ull;
constexpr uint64_t AfterGuard = 0x76543210abcdef98ull;
constexpr std::array<G::Site, 4> Sites {G::Site::PsnCleanupEntry, G::Site::IpcInitUnwind,
    G::Site::IpcTermPostJoin, G::Site::IpcTermPostDestroy};
bool Term(G::Site kind) { return kind == G::Site::IpcTermPostJoin || kind == G::Site::IpcTermPostDestroy; }
std::span<const uint8_t> Recipe(G::Site kind) {
	switch (kind) {
		case G::Site::PsnCleanupEntry: return Prologue;
		case G::Site::IpcInitUnwind: return Load;
		case G::Site::IpcTermPostJoin: return PostJoin;
		case G::Site::IpcTermPostDestroy: return PostDestroy;
	}
	return {};
}
uintptr_t Rva(G::Site kind) {
	switch (kind) {
		case G::Site::PsnCleanupEntry: return 0xc2e4f0;
		case G::Site::IpcInitUnwind: return 0xcaa9e0;
		case G::Site::IpcTermPostJoin: return 0xcaab2b;
		case G::Site::IpcTermPostDestroy: return 0xcaab47;
	}
	return 0;
}

void Planner() {
	for (const auto kind: Sites) {
		const auto bytes = Recipe(kind);
		const auto site = Rva(kind), continuation = site + bytes.size();
		G::Instruction output;
		Check(Name, G::Plan(kind, site, continuation, bytes, bytes, output), "exact recipe refused");
		const auto operand = kind == G::Site::PsnCleanupEntry ? 0 : kind == G::Site::IpcTermPostJoin ? 0x213cb00 : 0x213cae0;
		Check(Name, (output.LoadTarget() | output.StoreTarget()) == uintptr_t(operand) &&
		            (kind != G::Site::IpcTermPostJoin || (output.StoreTarget() == uintptr_t(operand) && !output.LoadTarget())),
		      "reviewed native RIP operand target or access direction is wrong");
		const auto original = output;
		for (size_t i = 0; i < bytes.size(); ++i) {
			std::array<uint8_t, 11> bad {};
			std::copy(bytes.begin(), bytes.end(), bad.begin()); bad[i] ^= 1;
			const auto changed = std::span<const uint8_t>(bad).first(bytes.size());
			Check(Name, !G::Plan(kind, site + 0x1000000, continuation + 0x1000000, changed, bytes, output) &&
			            !G::Plan(kind, site + 0x1000000, continuation + 0x1000000, bytes, changed, output) &&
			            output.Address() == original.Address() && output.Continuation() == original.Continuation(),
			      "mismatch mutated the verified descriptor");
		}
		Check(Name, !G::Plan(kind, site, continuation - 1, bytes, bytes, output) &&
		            !G::Plan(kind, site, continuation, bytes.first(bytes.size() - 1), bytes, output) &&
		            !G::Plan(kind, UINTPTR_MAX - 3, 0, bytes, bytes, output) &&
		            !G::Plan(static_cast<G::Site>(99), 0x1000000, 0x1000005, bytes, bytes, output),
		      "unsupported/truncated/overflow/continuation input admitted");
		Check(Name, output.Address() == original.Address() && output.LoadTarget() == original.LoadTarget() &&
		            output.StoreTarget() == original.StoreTarget(), "failed planning changed operand binding");
		if (operand) {
			Check(Name, !G::Plan(kind, site + 1, continuation + 1, bytes, bytes, output), "misaligned RIP target admitted");
			constexpr uintptr_t limit = UINT64_C(0x0000800000000000);
			const uintptr_t displacement = uintptr_t(operand) - continuation;
			const auto last_continuation = limit - 8 - displacement;
			Check(Name, G::Plan(kind, last_continuation - bytes.size(), last_continuation, bytes, bytes, output) &&
			            (output.LoadTarget() | output.StoreTarget()) == limit - 8,
			      "last bounded qword RIP target rejected");
			const auto bounded = output;
			Check(Name, !G::Plan(kind, last_continuation + 8 - bytes.size(), last_continuation + 8, bytes, bytes, output) &&
			            output.Address() == bounded.Address() && output.LoadTarget() == bounded.LoadTarget() &&
			            output.StoreTarget() == bounded.StoreTarget(), "out-of-user-range RIP target admitted/mutated descriptor");
		}
	}
}

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
struct Memory {
	void* p = nullptr;
	explicit Memory(size_t size): p(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
		Check(Name, p != nullptr, "synthetic mapping allocation failed");
	}
	~Memory() { if (p) { VirtualFree(p, 0, MEM_RELEASE); } }
	uintptr_t At(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(p) + offset; }
};
struct Shared {
	std::atomic<unsigned> ready {0};
	std::atomic<unsigned> exercised {0}, unsupported {0};
	std::atomic_bool release {false};
};
struct State {
	Shared* shared = nullptr;
	G::Site kind = G::Site::PsnCleanupEntry;
	uint64_t* operand = nullptr;
	uint64_t native_result = 0, observed_operand = 0;
	uintptr_t native_return_pc = 0;
	unsigned native_call_seen = 0;
	uintptr_t gateway = 0, stack_top = 0, host_rsp = 0;
	uintptr_t teb_base = 0, teb_limit = 0;
	uintptr_t entry_rsp = 0, entry_rbp = 0, continuation_rsp = 0;
	uint64_t expected_flags = 0, flags = 0;
	std::array<uint64_t, 15> post {};
	G::Snapshot captured {};
	unsigned guard_calls = 0;
	unsigned returned_calls = 0;
	DWORD thread_id = 0;
	bool redzone_ok = false, canary_ok = false, guard_ok = true;
	uint32_t guest_mxcsr = 0x5f80, post_mxcsr = 0;
	alignas(64) std::array<uint8_t, 4096> host_state {}, before {}, after {};
	alignas(64) std::array<uint8_t, 64> vector {};
	void (*clobber)() = nullptr;
};
bool Guard(const G::Snapshot& snapshot, void* context) noexcept {
	auto& s = *static_cast<State*>(context);
	s.captured = snapshot; // copy values only, never retain the reference
	s.guard_ok &= snapshot.site == s.kind;
	if (Term(s.kind)) {
		// Deliberately permit zero AND nonzero results in this preservation test.
		// This is not a native acceptance/receipt policy. The real machine CALL
		// has returned, but the displaced clear/load must not have run yet.
		s.observed_operand = *s.operand;
		s.guard_ok &= s.native_call_seen == 1 && snapshot.rax == s.native_result && s.observed_operand == Loaded;
		*s.operand = AfterGuard; // proves replay consumes memory only AFTER guard
	}
	uint64_t flags = 0;
#if defined(_MSC_VER) && !defined(__clang__)
	flags = __readeflags();
#else
	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
#endif
	const auto tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
	const auto local = reinterpret_cast<uintptr_t>(&flags);
	s.guard_ok &= !(flags & 0x400) && _mm_getcsr() == 0x1f80 &&
	              Common::GetHostLockCriticalCounter()->load() != 0 &&
	              GetCurrentThreadId() == s.thread_id && local >= s.stack_top - 256 * 1024 && local < snapshot.rsp &&
	              tib->StackBase == nullptr && tib->StackLimit == nullptr;
	if (s.guard_calls++ == 0) {
		s.shared->ready.fetch_add(1);
		const auto deadline = GetTickCount64() + 10000;
		while (!s.shared->release.load() && GetTickCount64() < deadline) { Sleep(1); }
		if (!s.shared->release.load()) { return false; }
	}
	s.clobber(); // real host ABI helper; only volatile SIMD registers are clobbered
	return s.guard_ok;
}
void Rx(uintptr_t address, size_t size) {
	DWORD previous = 0;
	Check(Name, VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READ, &previous) &&
	            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size), "synthetic code publication failed");
}

// Test-side Xbyak assembly is independent of the gateway's saving implementation.
// The synthetic continuation observes EVERY GPR/flags before using scratch regs,
// then restores the exact original native-style return chain.
struct Continuation : Xbyak::CodeGenerator {
	Continuation(void* code, State& s, G::Site kind, uint32_t mask): CodeGenerator(4096, code) {
		const bool unwind = kind == G::Site::IpcInitUnwind, term = Term(kind), entry = kind == G::Site::PsnCleanupEntry;
		const Xbyak::Reg64 regs[] = {rax,rcx,rdx,rbx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15};
		for (unsigned i = 0; i < 15; ++i) { mov(ptr[rsp + 128 + i * 8], regs[i]); }
		// Do not push into the red zone under test: use the private continuation
		// scratch above RSP for PUSHFQ, then return to the exact observed RSP.
		lea(rsp, ptr[rsp + 384]); pushfq(); pop(rax); lea(rsp, ptr[rsp - 384]);
		mov(rbx, reinterpret_cast<uintptr_t>(&s));
		mov(ptr[rbx + offsetof(State, flags)], rax);
		mov(ptr[rbx + offsetof(State, continuation_rsp)], rsp);
		cld();
		for (unsigned i = 0; i < 15; ++i) {
			mov(rax, ptr[rsp + 128 + i * 8]); mov(ptr[rbx + offsetof(State, post) + i * 8], rax);
		}
		mov(rax, Canary); mov(byte[rbx + offsetof(State, redzone_ok)], 1);
		Xbyak::Label bad, checked;
		// Prologue legitimately overwrites the upper 16 bytes with its pushes.
		// Unwind's MOV must preserve the entire 128-byte incoming red zone.
		const int displacement = entry ? 16 : 0;
		for (int i = 0; i < (entry ? 14 : term ? 15 : 16); ++i) {
			cmp(qword[rsp + displacement - 128 + i * 8], rax); jne(bad, T_NEAR);
		}
		if (term) {
			mov(rdx, ptr[rbx + offsetof(State, native_return_pc)]);
			cmp(qword[rsp - 8], rdx); jne(bad, T_NEAR); // preceding CALL's real return address
		}
		jmp(checked); L(bad); mov(byte[rbx + offsetof(State, redzone_ok)], 0); L(checked);
		if (unwind) {
			mov(rdx, ptr[rbx + offsetof(State, entry_rbp)]);
			cmp(qword[rdx - 0x28], rax); sete(byte[rbx + offsetof(State, canary_ok)]);
		} else if (term) { cmp(qword[rsp + 64], rax); sete(byte[rbx + offsetof(State, canary_ok)]); }
		else { mov(byte[rbx + offsetof(State, canary_ok)], 1); }
		stmxcsr(ptr[rbx + offsetof(State, post_mxcsr)]);
		lea(rcx, ptr[rbx + offsetof(State, after)]); mov(eax, mask); xor_(edx, edx);
		db(0x48); db(0x0f); db(0xae); db(0x21); // xsave64 [rcx]
		if (unwind) { add(rsp, 0x50); pop(r15); pop(r14); pop(r12); pop(rbx); pop(rbp); }
		else if (term) { pop(rbp); }
		else { pop(rbx); pop(rbp); }
		ret(); ready();
	}
};

struct Caller : Xbyak::CodeGenerator {
	Caller(void* code, State& s, G::Site kind, uint32_t mask): CodeGenerator(4096, code) {
		const bool unwind = kind == G::Site::IpcInitUnwind, term = Term(kind);
		const Xbyak::Reg64 regs[] = {rax,rcx,rdx,rbx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15};
		const Xbyak::Reg64 nonvolatile[] = {rbx,rbp,rsi,rdi,r12,r13,r14,r15};
		pushfq(); for (const auto& reg: nonvolatile) { push(reg); }
		mov(r11, reinterpret_cast<uintptr_t>(&s)); mov(ptr[r11 + offsetof(State, host_rsp)], rsp);
		lea(rcx, ptr[r11 + offsetof(State, host_state)]); mov(eax, mask); xor_(edx, edx);
		db(0x48); db(0x0f); db(0xae); db(0x21); // xsave64 [rcx]
		// Match the actual legacy pthread transition's zero TEB stack limits.
		db(0x65); db(0x48); db(0x8b); db(0x04); db(0x25); dd(8);
		mov(ptr[r11 + offsetof(State, teb_base)], rax);
		db(0x65); db(0x48); db(0x8b); db(0x04); db(0x25); dd(16);
		mov(ptr[r11 + offsetof(State, teb_limit)], rax);
		xor_(eax, eax);
		db(0x65); db(0x48); db(0x89); db(0x04); db(0x25); dd(8);
		db(0x65); db(0x48); db(0x89); db(0x04); db(0x25); dd(16);
		mov(rsp, s.stack_top - 1024);
		Xbyak::Label shim;
		call(shim); // real call/return, not a fabricated return address
		mov(r11, reinterpret_cast<uintptr_t>(&s));
		inc(dword[r11 + offsetof(State, returned_calls)]);
		mov(rsp, ptr[r11 + offsetof(State, host_rsp)]);
		mov(rax, ptr[r11 + offsetof(State, teb_base)]);
		db(0x65); db(0x48); db(0x89); db(0x04); db(0x25); dd(8);
		mov(rax, ptr[r11 + offsetof(State, teb_limit)]);
		db(0x65); db(0x48); db(0x89); db(0x04); db(0x25); dd(16);
		lea(rcx, ptr[r11 + offsetof(State, host_state)]); mov(eax, mask); xor_(edx, edx);
		db(0x48); db(0x0f); db(0xae); db(0x29); // xrstor64 [rcx]
		for (size_t i = std::size(nonvolatile); i; --i) { pop(nonvolatile[i - 1]); }
		popfq(); ret();
		L(shim);
		if (unwind) {
			push(rbp); mov(rbp, rsp); push(rbx); push(r12); push(r14); push(r15); sub(rsp, 0x50);
			mov(rax, Canary); mov(ptr[rbp - 0x28], rax);
		}
		if (term) {
			// Exact IPC Term frame: its only prologue is push rbp; mov rbp,rsp.
			push(rbp); mov(rbp, rsp); mov(rax, Canary); mov(ptr[rsp + 64], rax);
		}
		mov(r11, reinterpret_cast<uintptr_t>(&s));
		mov(ptr[r11 + offsetof(State, entry_rsp)], rsp);
		mov(ptr[r11 + offsetof(State, entry_rbp)], rbp);
		fninit(); fld1(); fldpi(); ldmxcsr(ptr[r11 + offsetof(State, guest_mxcsr)]);
		if (mask == 0xe7) {
			vmovdqu64(zmm0, ptr[r11 + offsetof(State, vector)]);
			vmovdqu64(zmm31, ptr[r11 + offsetof(State, vector)]);
			mov(eax, 0x5aa5); kmovw(k1, eax);
		} else if (mask & 4) { vmovdqu(ymm0, ptr[r11 + offsetof(State, vector)]); }
		else { movdqu(xmm0, ptr[r11 + offsetof(State, vector)]); }
		lea(rcx, ptr[r11 + offsetof(State, before)]); mov(eax, mask); xor_(edx, edx);
		db(0x48); db(0x0f); db(0xae); db(0x21);
		mov(rax, Canary);
		for (int i = 0; i < 16; ++i) { mov(ptr[rsp - 128 + i * 8], rax); }
		// Use above-RSP scratch: preserve the seeded red zone while installing
		// nontrivial CF/PF/AF/ZF/SF/OF/DF and all fifteen GPR sentinels.
		lea(rsp, ptr[rsp + 384]); pushfq(); pop(rax);
		and_(rax, ~uint64_t(0x240cd5)); or_(rax, 0x240c95); // ID, AC, DF and arithmetic flags
		mov(ptr[r11 + offsetof(State, expected_flags)], rax); push(rax);
		for (unsigned i = 0; i < 15; ++i) {
			if ((unwind || term) && i == 4) { continue; } // retain the real native RBP frame
			mov(regs[i], uint64_t(0x100 + i));
		}
		mov(r14d, 0x80550005u); // original init failure survives host guard
		popfq(); lea(rsp, ptr[rsp - 384]);
		Xbyak::Label result_callee;
		if (term) {
			call(result_callee); // real result-producing call, immediately before the gateway transfer
			s.native_return_pc = reinterpret_cast<uintptr_t>(code) + getSize();
		}
		// No extra CALL: this is the actual-site JMP contract under test.
		const auto padding = (8 - ((getSize() + 6) & 7)) & 7;
		db(0xff); db(0x25); dd(uint32_t(padding));
		for (size_t i = 0; i < padding; ++i) { db(0x90); }
		dq(s.gateway);
		if (term) {
			L(result_callee);
			// Only RAX changes; preserve seeded native flags, other GPRs and xstate.
			mov(rax, reinterpret_cast<uintptr_t>(&s));
			mov(dword[rax + offsetof(State, native_call_seen)], 1);
			mov(rax, ptr[rax + offsetof(State, native_result)]); ret();
		}
		ready();
	}
};

void One(Shared& shared, G::Site kind) {
	const bool unwind = kind == G::Site::IpcInitUnwind, term = Term(kind), entry = kind == G::Site::PsnCleanupEntry;
	State s; s.shared = &shared; s.kind = kind; s.thread_id = GetCurrentThreadId();
	for (size_t i = 0; i < s.vector.size(); ++i) { s.vector[i] = uint8_t(0xa0 + i); }
	Memory stack(256 * 1024), image(0x14a0000);
	s.stack_top = stack.At(256 * 1024);
	const auto bytes = Recipe(kind);
	const auto site = image.At(0x100 + (Rva(kind) & 7)); // preserve exact operand alignment
	std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
	G::Instruction instruction;
	Check(Name, G::Plan(kind, site, site + bytes.size(), bytes, bytes, instruction), "synthetic descriptor failed");
	const auto operand = instruction.LoadTarget() | instruction.StoreTarget();
	if (operand) {
		s.operand = reinterpret_cast<uint64_t*>(operand);
		s.operand[-1] = Canary; s.operand[0] = Loaded; s.operand[1] = Canary;
	}
	// Create validates executable original instructions; it never patches them.
	Rx(image.At(), 4096);
	G::Config config {stack.p, 256 * 1024, 64 * 1024, Guard, &s};
	G::Status status;
	auto gateway = G::Gateway::Create(instruction, config, &status);
	if (!gateway && status == G::Status::Unsupported) {
		shared.unsupported.fetch_add(1);
		shared.ready.fetch_add(1); return; // explicitly unsupported CPU/debug context
	}
	Check(Name, gateway && status == G::Status::Ok, "gateway creation failed");
	s.gateway = gateway->Entry();
	const auto mask = uint32_t(gateway->XstateMask());
	// Unsupported configurations fail without executing any generated code.
	auto invalid = config; invalid.guard = nullptr;
	Check(Name, !G::Gateway::Create(instruction, invalid, &status) && status == G::Status::InvalidArgument,
	      "null guard accepted");
	invalid = config; invalid.stack_size = 256;
	Check(Name, !G::Gateway::Create(instruction, invalid, &status), "unbounded/short stack accepted");
	invalid = config; invalid.stack = reinterpret_cast<void*>(1);
	Check(Name, !G::Gateway::Create(instruction, invalid, &status), "unmapped stack accepted");
	if (operand) {
		const auto page = operand & ~uintptr_t(4095);
		DWORD old = 0, ignored = 0;
		Check(Name, VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_NOACCESS, &old), "operand denial setup");
		Check(Name, !G::Gateway::Create(instruction, config, &status) && status == G::Status::InvalidMemory,
		      "inaccessible exact RIP target admitted");
		Check(Name, VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &ignored), "read-only operand setup");
		auto read_only = G::Gateway::Create(instruction, config, &status);
		Check(Name, instruction.StoreTarget() ? (!read_only && status == G::Status::InvalidMemory) : bool(read_only),
		      "RIP load/store access requirement was not distinguished");
		read_only.reset();
		Check(Name, VirtualProtect(reinterpret_cast<void*>(page), 4096, old, &ignored), "operand protection restoration");
	}
	DWORD previous = 0;
	Check(Name, VirtualProtect(image.p, 4096, PAGE_READWRITE, &previous), "synthetic continuation write admission");
	*reinterpret_cast<uint8_t*>(site) ^= 1;
	Rx(image.At(), 4096);
	Check(Name, !G::Gateway::Create(instruction, config, &status) && status == G::Status::InvalidMemory,
	      "stale verified descriptor admitted changed mapped instructions");
	Check(Name, VirtualProtect(image.p, 4096, PAGE_READWRITE, &previous), "synthetic instruction restoration admission");
	std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
	Continuation continuation(reinterpret_cast<void*>(instruction.Continuation()), s, kind, mask);
	// Guard's host helper intentionally destroys x87, XMM0 and AVX high lanes.
	Xbyak::CodeGenerator clobber(256, reinterpret_cast<void*>(image.At(0x800)));
	clobber.fninit(); clobber.fldz();
	if (mask == 0xe7) {
		clobber.vpxord(clobber.zmm0, clobber.zmm0, clobber.zmm0);
		clobber.vpxord(clobber.zmm31, clobber.zmm31, clobber.zmm31);
		clobber.kxorw(clobber.k1, clobber.k1, clobber.k1);
		clobber.vzeroupper();
	} else if (mask & 4) { clobber.vpxor(clobber.ymm0, clobber.ymm0, clobber.ymm0); clobber.vzeroupper(); }
	else { clobber.pxor(clobber.xmm0, clobber.xmm0); }
	clobber.ret(); clobber.ready(); s.clobber = reinterpret_cast<void (*)()>(image.At(0x800));
	Caller caller(reinterpret_cast<void*>(image.At(0x1000)), s, kind, mask);
	Rx(image.At(), 0x2000);
	for (unsigned turn = 0; turn < 2; ++turn) {
		s.native_result = turn ? 0x80020010u : 0; s.native_call_seen = 0;
		if (s.operand) { *s.operand = Loaded; }
		// Fresh zeroed standard XSAVE buffers avoid reserved-byte false positives.
		s.host_state.fill(0); s.before.fill(0); s.after.fill(0);
		reinterpret_cast<void (*)()>(image.At(0x1000))();
		Check(Name, s.guard_calls == turn + 1 && s.returned_calls == turn + 1 && s.guard_ok && s.redzone_ok && s.canary_ok,
		      "guard, red-zone, canary, or exactly-once invocation failed");
		Check(Name, s.captured.rsp == s.entry_rsp && s.continuation_rsp == s.entry_rsp - (entry ? 16 : 0),
		      "displaced prologue ran zero/two times or continuation stack changed");
		Check(Name, (s.flags & 0x240cd5) == (s.expected_flags & 0x240cd5) && s.captured.flags == s.expected_flags,
		      "incoming arithmetic/control flags were not restored");
		const uint64_t captured[] = {s.captured.rax,s.captured.rcx,s.captured.rdx,s.captured.rbx,s.captured.rbp,
		    s.captured.rsi,s.captured.rdi,s.captured.r8,s.captured.r9,s.captured.r10,s.captured.r11,
		    s.captured.r12,s.captured.r13,s.captured.r14,s.captured.r15};
		for (unsigned i = 0; i < 15; ++i) {
			const uint64_t expected = term && i == 0 ? s.native_result : i == 13 ? 0x80550005u :
			    ((unwind || term) && i == 4 ? s.entry_rbp : 0x100 + i);
			Check(Name, captured[i] == expected, "guard snapshot did not capture the original GPR");
			const uint64_t post = entry && i == 4 ? s.entry_rsp - 8 :
			    (instruction.LoadTarget() && i == 6 ? (term ? AfterGuard : Loaded) : expected);
			Check(Name, s.post[i] == post, "gateway altered a GPR beyond the original instruction");
		}
		if (operand) {
			Check(Name, s.operand[-1] == Canary && s.operand[1] == Canary, "RIP replay corrupted adjacent storage");
			Check(Name, *s.operand == (instruction.StoreTarget() ? 0 : term ? AfterGuard : Loaded),
			      "handle slot not cleared exactly by replay, or free-load source changed");
		}
		Check(Name, s.post_mxcsr == s.guest_mxcsr, "guest MXCSR control not restored");
		// Compare defined legacy state (omit reserved bytes and x87 slot padding).
		Check(Name, std::memcmp(s.before.data(), s.after.data(), 32) == 0 &&
		            std::memcmp(s.before.data() + 160, s.after.data() + 160, 256) == 0,
		      "x87 control/pointers or complete XMM state not restored");
		for (unsigned i = 0; i < 8; ++i) {
			Check(Name, std::memcmp(s.before.data() + 32 + i * 16, s.after.data() + 32 + i * 16, 10) == 0,
			      "x87 register state not restored");
		}
		for (unsigned bit = 2; bit <= 7; ++bit) {
			if (!(mask & (1u << bit))) { continue; }
			uint32_t extent = 0, offset = 0;
#if defined(_MSC_VER)
			int cpu[4]; __cpuidex(cpu, 0xd, int(bit)); extent = uint32_t(cpu[0]); offset = uint32_t(cpu[1]);
#else
			uint32_t c = 0, d = 0; __cpuid_count(0xd, bit, extent, offset, c, d);
#endif
			Check(Name, offset <= gateway->XstateSize() && extent <= gateway->XstateSize() - offset &&
			            std::memcmp(s.before.data() + offset, s.after.data() + offset, extent) == 0,
			      "enabled AVX/AVX-512 vector or opmask state not restored");
		}
	}
	Check(Name, std::memcmp(reinterpret_cast<void*>(site), bytes.data(), bytes.size()) == 0,
	      "gateway creation/invocation modified the native instruction site");
	shared.exercised.fetch_add(1);
}
#endif
} // namespace NativeCleanupGatewayTest

void TestNativeCleanupGateway() {
	using namespace NativeCleanupGatewayTest;
	Planner();
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
	Shared shared;
	std::array<std::thread, Sites.size()> threads;
	for (size_t i = 0; i < Sites.size(); ++i) { threads[i] = std::thread([&, kind = Sites[i]] { One(shared, kind); }); }
	const auto deadline = GetTickCount64() + 10000;
	while (shared.ready.load() != Sites.size() && GetTickCount64() < deadline) { Sleep(1); }
	const bool reached = shared.ready.load() == Sites.size();
	shared.release.store(true);
	for (auto& thread: threads) { thread.join(); }
	Check(Name, reached, "independent thread-bound gateways did not overlap their guards");
	Check(Name, shared.exercised.load() + shared.unsupported.load() == Sites.size(), "missing gateway execution result");
	if (shared.unsupported.load()) {
		std::fprintf(stderr, "NativeCleanupGateway: %u machine-code cases explicitly skipped (unsupported xstate/debug context)\n",
		             shared.unsupported.load());
	}
#else
	G::Instruction instruction; G::Config config; G::Status status;
	Check(Name, !G::Gateway::Create(instruction, config, &status) && status == G::Status::Unsupported,
	      "unsupported platform admitted a native gateway");
#endif
}
