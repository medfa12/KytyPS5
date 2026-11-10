// Included in VirtualMemoryAllocationTests.cpp's anonymous namespace.
namespace GuestHostBridgeTest {
using Status = Loader::GuestHostBridgeStatus;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
struct State {
	Loader::GuestHostBridge* bridge = nullptr;
	uintptr_t entry = 0;
	Loader::GuestHostSnapshot snapshot {};
	uint64_t post[15] {};
	uint64_t flags = 0, stack_before = 0, stack_after = 0;
	uint32_t host_mxcsr = 0, callback_mxcsr = 0, guest_mxcsr = 0, post_mxcsr = 0;
	alignas(32) uint8_t xmm_input[32] {}, xmm_output[32] {};
	alignas(64) uint8_t guest_x87_seed[512] {}, guest_x87_before[512] {}, guest_x87_after[512] {};
	alignas(64) uint8_t callback_x87[512] {};
	using FxFunction = void (*)(void*);
	FxFunction fxsave = nullptr;
	uint64_t host_gpr_post[8] {};
	alignas(16) uint8_t host_xmm_input[160] {}, host_xmm_post[160] {};
	Status run_status = Status::Unsupported;
	uint64_t run_output = 0;
	bool callback_df_clear = false, callback_on_host_stack = false, callback_no_critical = false;
	bool redzone_ok = false, block = false, timed_out = false;
	Status recursive = Status::Ok;
	uint64_t recursive_output = 0;
	Loader::GuestHostResultTag tag = Loader::GuestHostResultTag::Return;
	uint32_t carry = 0;
	uint64_t value = 0;
	std::atomic<bool> entered {false}, released {false};
};

void RunThunk(State* state) noexcept {
	state->run_status = state->bridge->Run(state->entry, reinterpret_cast<uint64_t>(state), &state->run_output);
}

bool CpuAdmission(uint64_t& xcr0) {
	uint32_t r[4] {};
	const auto cpuid = [&](uint32_t leaf, uint32_t subleaf) {
#if defined(_MSC_VER)
		int v[4]; __cpuidex(v, static_cast<int>(leaf), static_cast<int>(subleaf));
		for (int i = 0; i < 4; ++i) { r[i] = static_cast<uint32_t>(v[i]); }
#else
		__cpuid_count(leaf, subleaf, r[0], r[1], r[2], r[3]);
#endif
	};
	cpuid(0, 0); if (r[0] < 0x0d) { return false; }
	cpuid(1, 0);
	const uint32_t features = r[2];
	if ((features & ((1u << 26) | (1u << 27))) != ((1u << 26) | (1u << 27))) { return false; }
#if defined(_MSC_VER) && !defined(__clang__)
	xcr0 = _xgetbv(0);
#else
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	xcr0 = (uint64_t(hi) << 32) | lo;
#endif
	if (xcr0 != 3 && xcr0 != 7) { return false; }
	if (xcr0 == 7 && !(features & (1u << 28))) { return false; }
	cpuid(0x0d, 0);
	const uint64_t supported = (uint64_t(r[3]) << 32) | r[0];
	if ((supported & xcr0) != xcr0 || r[1] < 576 || r[1] > 4096) { return false; }
	const auto area = r[1];
	if (xcr0 == 7) {
		cpuid(0x0d, 2);
		if ((r[2] & 1) || r[0] != 256 || r[1] < 576 || r[1] > area || r[0] > area - r[1]) { return false; }
	}
	return true;
}

void Callback(const Loader::GuestHostSnapshot& snapshot, Loader::GuestHostResult& result, void* opaque) noexcept {
	auto& s = *static_cast<State*>(opaque);
	// Read floating-point control and DF before any floating-point computation.
	s.callback_mxcsr = _mm_getcsr();
	s.fxsave(s.callback_x87);
	uint64_t flags = 0;
#if defined(_MSC_VER) && !defined(__clang__)
	flags = __readeflags();
#else
	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
#endif
	s.callback_df_clear = (flags & 0x400) == 0;
	ULONG_PTR low = 0, high = 0;
	GetCurrentThreadStackLimits(&low, &high);
	const auto local = reinterpret_cast<uintptr_t>(&flags);
	s.callback_on_host_stack = local >= low && local < high;
	s.callback_no_critical = Common::GetHostLockCriticalCounter()->load() == 0;
	s.snapshot = snapshot;
	s.recursive_output = 0xabcde;
	s.recursive = s.bridge->Run(s.entry, 0, &s.recursive_output);
	if (s.block) {
		s.entered.store(true, std::memory_order_release);
		const auto deadline = GetTickCount64() + 2000;
		while (!s.released.load(std::memory_order_acquire) && GetTickCount64() < deadline) { Sleep(1); }
		s.timed_out = !s.released.load(std::memory_order_acquire);
	}
	result.value = s.value; result.tag = s.tag; result.carry = s.carry;
}

// Builds a standalone synthetic SYSV caller. It is not an encoder for, or a
// copy of, the bridge. Stack slots above RSP are private to this fixture.
struct Code : Xbyak::CodeGenerator {
	Code(void* memory, bool incoming_carry, bool incoming_df, bool avx) : Xbyak::CodeGenerator(4096, memory) {
		using namespace Xbyak;
		const Reg64 regs[] = {rax, rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15};
		push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
		sub(rsp, 0x98); // 16-byte alignment before the gateway CALL
		mov(ptr[rsp], rdi); mov(ptr[rsp + 8], rsi);
		mov(ptr[rsp + 0x10], rsp);
		// FXRSTOR64/FXSAVE64 with a real 64-bit legacy pointer format.
		lea(rcx, ptr[rsi + offsetof(State, guest_x87_seed)]);
		db(0x48); db(0x0f); db(0xae); db(0x09); // fxrstor64 [rcx]
		if (avx) { vmovdqu(ymm0, ptr[rsi + offsetof(State, xmm_input)]); }
		else { movdqu(xmm0, ptr[rsi + offsetof(State, xmm_input)]); }
		ldmxcsr(ptr[rsi + offsetof(State, guest_mxcsr)]);
		lea(rcx, ptr[rsi + offsetof(State, guest_x87_before)]);
		db(0x48); db(0x0f); db(0xae); db(0x01); // fxsave64 [rcx]
		mov(rax, 0x6a5b4c3d2e1f0987ull);
		for (int i = 0; i < 16; ++i) { mov(ptr[rsp - 136 + i * 8], rax); }
		// Modify only arithmetic/DF flags; preserve IF and all reserved bits.
		pushfq(); pop(rax); and_(rax, ~uint64_t(0xcd5));
		or_(rax, uint32_t((incoming_carry ? 1 : 0) | (incoming_df ? 0x400 : 0) |
		                  (incoming_carry ? 0x880 : 0x54)));
		push(rax);
		for (int i = 0; i < 15; ++i) { mov(regs[i], uint64_t(0x100 + i)); }
		popfq();
		call(ptr[rsp]);
		// Save registers without changing flags, then save flags before CLD.
		for (int i = 0; i < 15; ++i) { mov(ptr[rsp + 0x20 + i * 8], regs[i]); }
		pushfq(); pop(rax); mov(rbx, ptr[rsp + 8]); mov(ptr[rbx + offsetof(State, flags)], rax);
		cld();
		mov(rax, ptr[rsp + 0x10]); mov(ptr[rbx + offsetof(State, stack_before)], rax);
		mov(ptr[rbx + offsetof(State, stack_after)], rsp);
		for (int i = 0; i < 15; ++i) {
			mov(rax, ptr[rsp + 0x20 + i * 8]); mov(ptr[rbx + offsetof(State, post) + i * 8], rax);
		}
		stmxcsr(ptr[rbx + offsetof(State, post_mxcsr)]);
		if (avx) { vmovdqu(ptr[rbx + offsetof(State, xmm_output)], ymm0); }
		else { movdqu(ptr[rbx + offsetof(State, xmm_output)], xmm0); }
		lea(rcx, ptr[rbx + offsetof(State, guest_x87_after)]);
		db(0x48); db(0x0f); db(0xae); db(0x01); // fxsave64 [rcx]
		// pushfq above used only [rsp-8], outside the gateway red zone.
		mov(rax, 0x6a5b4c3d2e1f0987ull);
		mov(byte[rbx + offsetof(State, redzone_ok)], 1);
		Label bad, done;
		for (int i = 0; i < 16; ++i) { cmp(qword[rsp - 136 + i * 8], rax); jne(bad); }
		jmp(done); L(bad); mov(byte[rbx + offsetof(State, redzone_ok)], 0); L(done);
		mov(rax, ptr[rbx + offsetof(State, post)]);
		add(rsp, 0x98);
		pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx); ret();
		ready();
	}
};

struct FxCode : Xbyak::CodeGenerator {
	FxCode(void* memory) : Xbyak::CodeGenerator(4096, memory) {
		// Windows x64: RCX is the aligned FXSAVE area. Three fixed helpers.
		db(0x48); db(0x0f); db(0xae); db(0x01); ret(); // +0: fxsave64
		while (getSize() < 16) { nop(); }
		Xbyak::Label restore;
		jmp(restore); // +16: ABI-conforming x87 image restore
		while (getSize() < 32) { nop(); }
		sub(rsp, 16); fnstcw(word[rsp]); fninit(); fld1(); fldcw(word[rsp]); add(rsp, 16); ret();
		while (getSize() < 64) { nop(); }
		L(restore);
		sub(rsp, 0xb8);
		for (int i = 0; i < 10; ++i) { movdqu(ptr[rsp + i * 16], Xbyak::Xmm(i + 6)); }
		stmxcsr(ptr[rsp + 0xa0]); fnstcw(word[rsp + 0xa4]);
		db(0x48); db(0x0f); db(0xae); db(0x09); // fxrstor64 [rcx]
		ldmxcsr(ptr[rsp + 0xa0]); fldcw(word[rsp + 0xa4]);
		for (int i = 0; i < 10; ++i) { movdqu(Xbyak::Xmm(i + 6), ptr[rsp + i * 16]); }
		add(rsp, 0xb8); ret();
		ready();
	}
};

struct HostWrapper : Xbyak::CodeGenerator {
	HostWrapper(void* memory) : Xbyak::CodeGenerator(4096, memory) {
		using namespace Xbyak;
		const Reg64 regs[] = {rbx, rbp, rsi, rdi, r12, r13, r14, r15};
		for (const auto& reg : regs) { push(reg); }
		sub(rsp, 0xd8); // alignment, 32-byte shadow, 160-byte XMM save, state pointer
		for (int i = 0; i < 10; ++i) { movdqu(ptr[rsp + 0x20 + i * 16], Xmm(i + 6)); }
		mov(ptr[rsp + 0xc0], rcx);
		for (int i = 0; i < 10; ++i) { movdqu(Xmm(i + 6), ptr[rcx + offsetof(State, host_xmm_input) + i * 16]); }
		for (int i = 0; i < 8; ++i) { mov(regs[i], uint64_t(0x76540000 + i)); }
		mov(rax, reinterpret_cast<uintptr_t>(&RunThunk)); call(rax);
		mov(rax, ptr[rsp + 0xc0]);
		for (int i = 0; i < 8; ++i) { mov(ptr[rax + offsetof(State, host_gpr_post) + i * 8], regs[i]); }
		for (int i = 0; i < 10; ++i) { movdqu(ptr[rax + offsetof(State, host_xmm_post) + i * 16], Xmm(i + 6)); }
		for (int i = 0; i < 10; ++i) { movdqu(Xmm(i + 6), ptr[rsp + 0x20 + i * 16]); }
		add(rsp, 0xd8);
		for (int i = 7; i >= 0; --i) { pop(regs[i]); }
		ret(); ready();
	}
};
#endif
} // namespace GuestHostBridgeTest

void TestGuestHostBridge() {
	const char* test = "GuestHostBridge";
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
	using namespace GuestHostBridgeTest;
	using namespace Libs::LibKernel::Memory;
	using Loader::GuestHostBridge;
	using Loader::GuestHostBridgeConfig;
	using Loader::GuestHostResultTag;
	constexpr uint64_t stack_size = 0x40000;
	Check(test, Common::GetHostLockCriticalCounter()->load() == 0, "fixture entered with host critical guard held");
	struct StackOwner {
		uint64_t address = 0;
		~StackOwner() { if (address) { KernelMunmap(address, stack_size); } }
	} stack;
	stack.address = AllocateGuestStackMemory(0x900000000, stack_size, Common::VirtualMemory::Mode::ReadWrite, "bridge_test_stack");
	Check(test, stack.address != 0, "guest stack allocation failed");
	State state;
	state.host_mxcsr = _mm_getcsr();
	state.guest_mxcsr = (state.host_mxcsr & ~0x6000u) | ((state.host_mxcsr ^ 0x2000u) & 0x6000u);
	for (size_t i = 0; i < sizeof(state.xmm_input); ++i) { state.xmm_input[i] = static_cast<uint8_t>(i * 13 + 7); }
	GuestHostBridgeConfig config {reinterpret_cast<void*>(stack.address), stack_size, Callback, &state};
	Status status = Status::Ok;
	const auto reject = [&](GuestHostBridgeConfig c, Status expected, const char* label) {
		auto candidate = GuestHostBridge::Create(c, &status);
		Check(test, !candidate && status == expected, label);
	};
	auto bad = config; bad.guest_stack = nullptr; reject(bad, Status::InvalidArgument, "null stack accepted");
	bad = config; bad.guest_stack_size = 4096; reject(bad, Status::InvalidArgument, "small stack accepted");
	bad = config; bad.callback = nullptr; reject(bad, Status::InvalidArgument, "null callback accepted");
	bad = config; bad.guest_stack = reinterpret_cast<void*>(0x10000); reject(bad, Status::InvalidStack, "uncommitted stack accepted");
	ULONG_PTR low = 0, high = 0; GetCurrentThreadStackLimits(&low, &high);
	bad = config; bad.guest_stack = reinterpret_cast<void*>(low); bad.guest_stack_size = high - low;
	reject(bad, Status::InvalidStack, "host stack overlap accepted");
	uint64_t xcr0 = 0;
	if (!CpuAdmission(xcr0)) {
		std::printf("[host]    %-48s skipped: independent CPUID/XCR0 admission failed (XCR0=0x%llx)\n",
		            test, static_cast<unsigned long long>(xcr0));
		return;
	}
	auto bridge = GuestHostBridge::Create(config, &status);
	Check(test, bridge && status == Status::Ok, "bridge creation failed on independently admitted CPU");
	state.bridge = bridge.get();
	struct CodePages {
		void* memory = nullptr;
		~CodePages() { if (memory) { VirtualFree(memory, 0, MEM_RELEASE); } }
	} code;
	code.memory = VirtualAlloc(nullptr, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	Check(test, code.memory != nullptr, "synthetic code allocation failed");
	const bool avx = xcr0 == 7;
	Code clear(code.memory, false, false, avx);
	Code set(static_cast<uint8_t*>(code.memory) + 4096, true, true, avx);
	FxCode fx(static_cast<uint8_t*>(code.memory) + 8192);
	HostWrapper wrapper(static_cast<uint8_t*>(code.memory) + 12288);
	DWORD old = 0;
	Check(test, VirtualProtect(code.memory, 16384, PAGE_EXECUTE_READ, &old) != 0 &&
	                FlushInstructionCache(GetCurrentProcess(), code.memory, 16384) != 0, "synthetic code RX/flush failed");
	const auto host_wrapper = reinterpret_cast<void (*)(State*)>(static_cast<uint8_t*>(code.memory) + 12288);
	for (size_t i = 0; i < sizeof(state.host_xmm_input); ++i) {
		state.host_xmm_input[i] = static_cast<uint8_t>(0x91 + 17 * i);
	}
	auto fxbase = static_cast<uint8_t*>(code.memory) + 8192;
	state.fxsave = reinterpret_cast<State::FxFunction>(fxbase);
	const auto fxrestore = reinterpret_cast<State::FxFunction>(fxbase + 16);
	const auto fxinit = reinterpret_cast<void (*)()>(fxbase + 32);
	struct RestoreX87 {
		alignas(64) uint8_t original[512] {};
		State::FxFunction restore;
		~RestoreX87() { restore(original); }
	} restore_x87 {{}, fxrestore};
	state.fxsave(restore_x87.original);
	fxinit();
	alignas(64) uint8_t host_seed[512] {}, host_after[512] {};
	state.fxsave(host_seed);
	const uint64_t host_fip = reinterpret_cast<uintptr_t>(code.memory) + 0x100;
	const uint64_t host_fdp = reinterpret_cast<uintptr_t>(&state) + 0x100;
	const uint64_t guest_fip = reinterpret_cast<uintptr_t>(code.memory) + 0x1100;
	const uint64_t guest_fdp = reinterpret_cast<uintptr_t>(&state) + 0x200;
	Check(test, host_fip > UINT32_MAX && host_fdp > UINT32_MAX && guest_fip > UINT32_MAX && guest_fdp > UINT32_MAX,
	      "x87 high-address fixture requires pointers above 4 GiB");
	std::memcpy(host_seed + 8, &host_fip, 8); std::memcpy(host_seed + 16, &host_fdp, 8);
	std::memcpy(state.guest_x87_seed, host_seed, sizeof(host_seed));
	std::memcpy(state.guest_x87_seed + 8, &guest_fip, 8); std::memcpy(state.guest_x87_seed + 16, &guest_fdp, 8);
	fxrestore(host_seed);
	state.entry = reinterpret_cast<uintptr_t>(code.memory);
	uint64_t wrong_output = 0x1234;
	Status wrong_status = Status::Ok;
	std::thread wrong([&] { wrong_status = bridge->Run(state.entry, reinterpret_cast<uint64_t>(&state), &wrong_output); });
	wrong.join();
	Check(test, wrong_status == Status::WrongThread && wrong_output == 0x1234, "wrong-thread Run changed output or was accepted");
	DWORD old_stack = 0;
	Check(test, VirtualProtect(reinterpret_cast<void*>(stack.address), 4096, PAGE_NOACCESS, &old_stack) != 0, "protect guest stack failed");
	uint64_t out = 0x5678;
	const auto invalid_stack = bridge->Run(state.entry, reinterpret_cast<uint64_t>(&state), &out);
	DWORD ignored = 0;
	const bool restored = VirtualProtect(reinterpret_cast<void*>(stack.address), 4096, old_stack, &ignored) != 0;
	Check(test, restored && invalid_stack == Status::InvalidStack && out == 0x5678, "Run did not reject protected stack or restore failed");
	const auto check_run = [&](bool incoming, GuestHostResultTag tag, uint32_t carry, uint64_t expected, bool expected_carry) {
		state.entry = reinterpret_cast<uintptr_t>(code.memory) + (incoming ? 4096 : 0);
		state.tag = tag; state.carry = carry; state.value = 0x1122334455667788ull;
		out = 0;
		fxrestore(host_seed);
		state.fxsave(host_after);
		Check(test, std::memcmp(host_after + 8, host_seed + 8, 16) == 0,
		      "host x87 high-address seed was not retained by hardware");
		state.run_output = 0;
		host_wrapper(&state);
		const auto run = state.run_status;
		out = state.run_output;
		state.fxsave(host_after);
		if (run != Status::Ok || out != expected) {
			std::fprintf(stderr, "bridge run status=%u output=0x%llx expected=0x%llx callback=%u\n",
			             static_cast<unsigned>(run), static_cast<unsigned long long>(out),
			             static_cast<unsigned long long>(expected), state.entered.load() ? 1u : 0u);
		}
		Check(test, run == Status::Ok && out == expected, "synthetic gateway returned wrong result");
		for (size_t i = 0; i < 8; ++i) {
			Check(test, state.host_gpr_post[i] == 0x76540000 + i, "host nonvolatile GPR corrupted");
		}
		Check(test, std::equal(std::begin(state.host_xmm_input), std::end(state.host_xmm_input),
		                       std::begin(state.host_xmm_post)), "host nonvolatile XMM corrupted");
		const uint64_t snapshot[] = {state.snapshot.rax, state.snapshot.rcx, state.snapshot.rdx, state.snapshot.rbx,
		                             state.snapshot.rbp, state.snapshot.rsi, state.snapshot.rdi, state.snapshot.r8,
		                             state.snapshot.r9, state.snapshot.r10, state.snapshot.r11, state.snapshot.r12,
		                             state.snapshot.r13, state.snapshot.r14, state.snapshot.r15};
		for (size_t i = 0; i < 15; ++i) {
			Check(test, snapshot[i] == 0x100 + i, "snapshot register mismatch");
			Check(test, state.post[i] == (i == 0 ? expected : 0x100 + i), "gateway register preservation mismatch");
		}
		Check(test, ((state.snapshot.rflags & 1) != 0) == incoming &&
		                ((state.snapshot.rflags & 0x400) != 0) == incoming &&
		                ((state.flags & 1) != 0) == expected_carry &&
		                ((state.flags & 0x400) != 0) == incoming, "carry or direction flag mismatch");
		const uint64_t arithmetic = incoming ? 0x880 : 0x54;
		Check(test, (state.snapshot.rflags & 0x8d4) == arithmetic &&
		                (state.flags & 0x8d4) == arithmetic, "arithmetic flags not preserved");
		Check(test, state.stack_before == state.stack_after && state.redzone_ok, "guest stack or red zone corrupted");
		Check(test, state.callback_df_clear && state.callback_on_host_stack && state.callback_no_critical,
		      "callback host context incorrect");
		// Windows permits host calls preceding the generated transition to change
		// MXCSR status bits 0..5. Its control bits must match the host baseline;
		// the guest's complete MXCSR must survive the gateway unchanged.
		constexpr uint32_t mxcsr_controls = 0xffffffc0u;
		const bool host_controls_ok = (state.callback_mxcsr & mxcsr_controls) ==
		                              (state.host_mxcsr & mxcsr_controls);
		if (!host_controls_ok || state.post_mxcsr != state.guest_mxcsr ||
		    !std::equal(state.xmm_input, state.xmm_input + (avx ? 32 : 16), state.xmm_output)) {
			std::fprintf(stderr, "bridge FP: host=%08x callback=%08x guest=%08x post=%08x avx=%u\n",
			             state.host_mxcsr, state.callback_mxcsr, state.guest_mxcsr, state.post_mxcsr, avx ? 1u : 0u);
			for (size_t i = 0; i < (avx ? 32u : 16u); ++i) {
				if (state.xmm_input[i] != state.xmm_output[i]) {
					std::fprintf(stderr, "bridge vector byte %zu: %02x != %02x\n", i, state.xmm_input[i], state.xmm_output[i]);
				}
			}
		}
		Check(test, host_controls_ok && state.post_mxcsr == state.guest_mxcsr &&
		                std::equal(state.xmm_input, state.xmm_input + (avx ? 32 : 16), state.xmm_output),
		      "host/guest floating-point state incorrect");
		Check(test, std::memcmp(state.callback_x87 + 8, host_seed + 8, 16) == 0 &&
		                std::memcmp(host_after + 8, host_seed + 8, 16) == 0,
		      "host x87 64-bit instruction/data pointers not preserved");
		Check(test, std::memcmp(state.guest_x87_before + 8, state.guest_x87_seed + 8, 16) == 0 &&
		                std::memcmp(state.guest_x87_after + 8, state.guest_x87_seed + 8, 16) == 0 &&
		                std::memcmp(state.guest_x87_before, state.guest_x87_after, 2) == 0,
		      "guest x87 64-bit instruction/data pointers or control word not preserved");
		Check(test, state.recursive == Status::Busy && state.recursive_output == 0xabcde, "recursive Run accepted or changed output");
	};
	state.block = true;
	std::thread unblock([&] {
		const auto deadline = GetTickCount64() + 2000;
		while (!state.entered.load(std::memory_order_acquire) && GetTickCount64() < deadline) { Sleep(1); }
		state.released.store(true, std::memory_order_release);
	});
	// Join even if a Check fails: no joinable thread may survive stack unwinding.
	try { check_run(false, GuestHostResultTag::Return, 0, 0x1122334455667788ull, false); }
	catch (...) { unblock.join(); throw; }
	unblock.join();
	Check(test, state.entered.load() && !state.timed_out, "blocking callback did not resume in time");
	state.block = false;
	check_run(true, GuestHostResultTag::Return, 1, 0x1122334455667788ull, true);
	check_run(false, GuestHostResultTag::Reject, 0, UINT64_MAX, true);
	check_run(true, GuestHostResultTag::Return, 2, UINT64_MAX, true);
	bridge.reset();
	auto again = GuestHostBridge::Create(config, &status);
	Check(test, again && status == Status::Ok, "bridge recreation failed");
	std::printf("[host]    %-48s ok\n", test);
#else
	std::printf("[host]    %-48s skipped: requires Windows x64\n", test);
#endif
}
