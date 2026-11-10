// Standalone fixture: link only this file and src/loader/guestCallBoundary.cpp.
// No loader, pthread, firmware, graphics or production deferred-import wiring.
#include "loader/guestCallBoundary.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define BOUNDARY_FIXTURE_WIN64 1
#else
#define BOUNDARY_FIXTURE_WIN64 0
#endif

namespace {
using Boundary = Loader::GuestCallBoundary;
using Status = Loader::GuestCallBoundaryStatus;
using Tag = Loader::GuestCallOutcomeTag;
using Owner = Loader::GuestCallOwner;
using Outcome = Loader::GuestCallOutcome;
using Failure = Loader::GuestBoundaryFailureFunction;

void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "GuestCallBoundaryTests: %s\n", message); std::abort(); }
}

#if BOUNDARY_FIXTURE_WIN64
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV ReturnBits(void* bits) { return *static_cast<uint64_t*>(bits); }

struct Nested {
	Boundary* boundary;
	Owner owner;
	int live = 0, destroyed = 0, resolver_returns = 0;
	bool rolled_back = false, inner_after = false, outer_after = false;
};
struct Counted {
	Nested& state;
	explicit Counted(Nested& s): state(s) { ++state.live; }
	~Counted() { --state.live; ++state.destroyed; }
};

// A resolver is HOST code. Its RAII is destroyed by a NORMAL return before
// the synthetic guest/trivial transition is allowed to enter the failure gate.
Failure Resolver(Nested& state) {
	Counted resolver_local(state);
	++state.resolver_returns;
	return state.boundary->FailureFunction();
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV InnerGuest(void* opaque) {
	auto* s = static_cast<Nested*>(opaque); // Only trivial locals in discarded guest frames.
	Check(Boundary::CurrentDepth() == 2 && s->live == 1, "inner depth/host owner live");
	auto failure = Resolver(*s);
	Check(failure != nullptr && s->live == 1 && s->destroyed == 1, "resolver returned and destroyed RAII");
	failure();
	s->inner_after = true;
	return 0;
}
void HostNestedOperation(Nested& state) {
	Counted registry_owner(state);
	const auto inner = state.boundary->Invoke(InnerGuest, &state);
	Check(inner.status == Status::Ok && inner.tag == Tag::Aborted, "inner tagged abort");
	Check(Boundary::CurrentDepth() == 1 && state.live == 1, "inner restores outer TLS without skipping host owner");
	state.rolled_back = true; // Normal host rollback, then Counted's normal destruction.
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV OuterGuest(void* opaque) {
	auto* s = static_cast<Nested*>(opaque);
	HostNestedOperation(*s);
	Check(s->rolled_back && s->live == 0 && s->destroyed == 2, "host rollback/destructor before outer propagation");
	auto failure = Resolver(*s);
	Check(failure != nullptr && s->live == 0 && s->destroyed == 3, "outer resolver returns normally");
	failure();
	s->outer_after = true;
	return 0;
}

struct RejectState { Boundary* own; Boundary* foreign; Owner owner; };
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV RejectGuest(void* opaque) {
	auto* s = static_cast<RejectState*>(opaque);
	const auto depth = Boundary::CurrentDepth();
	int foreign_identity = 0;
	Check(s->own->AbortFunction()(&foreign_identity, s->owner.token) == 0, "wrong owner rejected");
	Check(s->own->AbortFunction()(s->owner.identity, s->owner.token + 1) == 0, "stale token rejected");
	Check(s->foreign->AbortFunction()(s->owner.identity, s->owner.token) == 0, "foreign boundary engine rejected");
	Check(s->foreign->FailureFunction() == nullptr, "foreign no-arg gateway unavailable");
	Check(Boundary::CurrentDepth() == depth && s->own->FailureFunction() != nullptr, "rejection leaves TLS intact");
	return 0xfedcba9876543210ull;
}

struct ThreadState { Boundary* boundary; Owner owner; bool child_aborted = false; };
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV AbortGuest(void* opaque) {
	auto* s = static_cast<ThreadState*>(opaque);
	Check(Boundary::CurrentDepth() == 1, "thread owns its own root boundary");
	s->boundary->AbortFunction()(s->owner.identity, s->owner.token);
	return 0xdead; // If incorrectly resumed, caller observes Returned instead of Aborted.
}
void HostThreadOperation(ThreadState& state) {
	// This host C++ thread object is never skipped. No cross-thread continuation
	// is exposed, even while the parent has an active boundary for the same owner.
	std::thread child([&] {
		Check(Boundary::CurrentDepth() == 0, "fresh actual-thread TLS");
		Check(state.boundary->AbortFunction()(state.owner.identity, state.owner.token) == 0,
		      "foreign thread cannot use parent's active boundary");
		Check(state.boundary->FailureFunction() == nullptr, "foreign thread cannot acquire parent gateway");
		const auto result = state.boundary->Invoke(AbortGuest, &state);
		state.child_aborted = result.status == Status::Ok && result.tag == Tag::Aborted;
		Check(Boundary::CurrentDepth() == 0, "child TLS popped");
	});
	child.join();
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV ParentGuest(void* opaque) {
	auto* s = static_cast<ThreadState*>(opaque);
	HostThreadOperation(*s);
	Check(Boundary::CurrentDepth() == 1 && s->child_aborted, "child abort preserves parent's live segment");
	return 73;
}

struct CrossState { Boundary* outer; Boundary* inner; Owner outer_owner; };
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV CrossInnerGuest(void* opaque) {
	auto* s = static_cast<CrossState*>(opaque);
	Check(Boundary::CurrentDepth() == 2, "cross-owner nested depth");
	Check(s->outer->FailureFunction() == nullptr, "cannot acquire older matching owner's gateway");
	Check(s->outer->AbortFunction()(s->outer_owner.identity, s->outer_owner.token) == 0,
	      "cannot bypass innermost boundary to older matching owner");
	Check(s->inner->AbortFunction()(s->outer_owner.identity, s->outer_owner.token) == 0,
	      "current engine cannot abort with older owner's identity");
	return uint64_t(-9);
}
void HostCrossOperation(CrossState& state) {
	const auto inner = state.inner->Invoke(CrossInnerGuest, &state);
	Check(inner.tag == Tag::Returned && inner.value == uint64_t(-9), "foreign inner remains ordinarily returnable");
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV CrossOuterGuest(void* opaque) {
	auto* s = static_cast<CrossState*>(opaque);
	HostCrossOperation(*s);
	Check(Boundary::CurrentDepth() == 1 && s->outer->FailureFunction() != nullptr, "outer restored after cross-owner inner returns");
	return 19;
}

// Exactly the resolver ABI used by the separate deferred-import thunk. It
// returns an ADDRESS normally, never performs the nonlocal transfer itself.
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV AddressResolver(void* opaque, uint64_t caller) noexcept {
	auto* s = static_cast<Nested*>(opaque);
	Counted host_resolver_owner(*s);
	Check(caller == 0x1020304050607080ull && Boundary::CurrentDepth() == 1, "resolver caller/active segment");
	++s->resolver_returns;
	return reinterpret_cast<uint64_t>(s->boundary->FailureFunction());
}

// Tiny fixture encoder independent of the production emitter. Its two emitted
// functions seed/check the real Win64 nonvolatiles around an ordinary C++ call,
// and dirty state inside a SysV guest segment. No inline-asm register-allocation
// assumptions and no C++ objects in a machine-code-discarded guest frame.
enum R : unsigned { AX, CX, DX, BX, SP, BP, SI, DI, R8, R9, R10, R11, R12, R13, R14, R15 };
struct Code {
	uint8_t* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	size_t length = 0;
	~Code() { if (memory != nullptr) VirtualFree(memory, 0, MEM_RELEASE); }
	void B(uint8_t value) { Check(memory != nullptr && length < 4096, "fixture code allocation/size"); memory[length++] = value; }
	void Bytes(std::initializer_list<uint8_t> bytes) { for (auto b: bytes) B(b); }
	void U32(uint32_t value) { for (unsigned i = 0; i != 4; ++i) B(static_cast<uint8_t>(value >> (i * 8))); }
	void U64(uint64_t value) { for (unsigned i = 0; i != 8; ++i) B(static_cast<uint8_t>(value >> (i * 8))); }
	void Rex(bool wide, unsigned reg, unsigned base) { B(0x40 | (wide ? 8 : 0) | ((reg >> 3) << 2) | (base >> 3)); }
	void Address(unsigned field, unsigned base, size_t offset) {
		B(0x80 | ((field & 7) << 3) | (base & 7)); if ((base & 7) == SP) B(0x24); U32(static_cast<uint32_t>(offset));
	}
	void Imm(unsigned reg, uint64_t value) { Rex(true, 0, reg); B(0xb8 + (reg & 7)); U64(value); }
	void Store(unsigned base, size_t offset, unsigned reg) { Rex(true, reg, base); B(0x89); Address(reg, base, offset); }
	void Load(unsigned reg, unsigned base, size_t offset) { Rex(true, reg, base); B(0x8b); Address(reg, base, offset); }
	void Xmm(bool store, unsigned reg, unsigned base, size_t offset) {
		B(0xf3); Rex(false, reg, base); Bytes({0x0f, uint8_t(store ? 0x7f : 0x6f)}); Address(reg, base, offset);
	}
	void Control(unsigned base, size_t offset, bool x87, bool restore) {
		Rex(false, 0, base);
		if (x87) { B(0xd9); Address(restore ? 5 : 7, base, offset); }
		else { Bytes({0x0f, 0xae}); Address(restore ? 2 : 3, base, offset); }
	}
	void Push(unsigned reg) { if (reg >= 8) B(0x41); B(0x50 + (reg & 7)); }
	void Pop(unsigned reg) { if (reg >= 8) B(0x41); B(0x58 + (reg & 7)); }
	void Stack(bool allocate, uint32_t size) { Bytes({0x48, 0x81, uint8_t(allocate ? 0xec : 0xc4)}); U32(size); }
	void Endbr() { Bytes({0xf3, 0x0f, 0x1e, 0xfa}); }
	void Seal() {
		DWORD old = 0;
		Check(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old) &&
		      FlushInstructionCache(GetCurrentProcess(), memory, length), "seal fixture code RX");
	}
};

struct MachineState {
	Boundary* boundary = nullptr;
	Loader::GuestBoundaryFunction guest = nullptr;
	Loader::GuestBoundaryAbortFunction abort = nullptr;
	Owner owner {};
	Outcome result {};
	uint64_t before = 0, after = 0, flags = 0;
	uint64_t observed_gpr[8] {};
	uint8_t expected_xmm[160] {}, observed_xmm[160] {};
	uint32_t host_mxcsr = 0x5f80, guest_mxcsr = 0x3f80, observed_mxcsr = 0;
	uint16_t host_cw = 0x0b7f, guest_cw = 0x077f, observed_cw = 0;
	uintptr_t teb_base_before = 0, teb_base_after = 0, teb_limit_before = 0, teb_limit_after = 0;
};
void __attribute__((ms_abi)) ProbeInvoke(MachineState* s) noexcept {
	// Boundary deliberately does not touch TEB stack bounds or switch stacks.
	asm volatile("movq %%gs:0x08, %0\n\tmovq %%gs:0x10, %1"
	             : "=r"(s->teb_base_before), "=r"(s->teb_limit_before));
	s->result = s->boundary->Invoke(s->guest, s);
	asm volatile("movq %%gs:0x08, %0\n\tmovq %%gs:0x10, %1"
	             : "=r"(s->teb_base_after), "=r"(s->teb_limit_after));
}

void MakeProbe(Code& code) {
	code.Endbr();
	for (auto r: {BX, BP, SI, DI, R12, R13, R14, R15}) code.Push(r);
	code.Stack(true, 216); // aligned MS shadow, XMM save, pointer and FP controls
	for (unsigned i = 0; i != 10; ++i) code.Xmm(true, i + 6, SP, 32 + i * 16);
	code.Store(SP, 192, CX);
	code.Control(SP, 200, false, false); code.Control(SP, 204, true, false);
	code.Control(CX, offsetof(MachineState, host_mxcsr), false, true);
	code.Control(CX, offsetof(MachineState, host_cw), true, true);
	code.Store(CX, offsetof(MachineState, before), SP);
	for (unsigned i = 0; i != 10; ++i) code.Xmm(false, i + 6, CX, offsetof(MachineState, expected_xmm) + i * 16);
	unsigned i = 0;
	for (auto r: {BX, BP, SI, DI, R12, R13, R14, R15}) code.Imm(r, 0x98760000 + i++);
	code.Imm(AX, reinterpret_cast<uintptr_t>(&ProbeInvoke)); code.Bytes({0xff, 0xd0});
	code.Load(AX, SP, 192);
	i = 0;
	for (auto r: {BX, BP, SI, DI, R12, R13, R14, R15}) code.Store(AX, offsetof(MachineState, observed_gpr) + i++ * 8, r);
	for (i = 0; i != 10; ++i) code.Xmm(true, i + 6, AX, offsetof(MachineState, observed_xmm) + i * 16);
	code.Store(AX, offsetof(MachineState, after), SP);
	code.Bytes({0x9c, 0x5a}); code.Store(AX, offsetof(MachineState, flags), DX);
	code.Control(AX, offsetof(MachineState, observed_mxcsr), false, false);
	code.Control(AX, offsetof(MachineState, observed_cw), true, false);
	code.Control(SP, 200, false, true); code.Control(SP, 204, true, true);
	for (i = 0; i != 10; ++i) code.Xmm(false, i + 6, SP, 32 + i * 16);
	code.Stack(false, 216);
	for (auto r: {R15, R14, R13, R12, DI, SI, BP, BX}) code.Pop(r);
	code.B(0xc3); code.Seal();
}

void MakeDirtyGuest(Code& code, bool abort) {
	code.Endbr();
	// R10 retains the incoming state pointer until gate arguments are loaded.
	code.Bytes({0x49, 0x89, 0xfa}); // mov r10,rdi
	code.Bytes({0xdb, 0xe3}); // fninit: intentionally dirty legacy FP state
	code.Control(R10, offsetof(MachineState, guest_mxcsr), false, true);
	code.Control(R10, offsetof(MachineState, guest_cw), true, true);
	for (unsigned i = 0; i != 16; ++i) {
		code.B(0x66); code.Rex(false, i, i); code.Bytes({0x0f, 0xef});
		code.B(0xc0 | ((i & 7) << 3) | (i & 7)); // pxor xmmN,xmmN
	}
	if (abort) {
		// A nonreturning guest segment need not restore its callee-saved GPRs.
		for (auto r: {BX, BP, R12, R13, R14, R15}) code.Imm(r, 0xaabbccdd);
		code.Load(AX, R10, offsetof(MachineState, abort));
		code.Load(DI, R10, offsetof(MachineState, owner) + offsetof(Owner, identity));
		code.Load(SI, R10, offsetof(MachineState, owner) + offsetof(Owner, token));
		code.Stack(true, 8); code.B(0xfd); // align before CALL; DF dirty at raw abort gateway
		code.Bytes({0xff, 0xd0, 0x0f, 0x0b}); // call gate; ud2 if incorrectly returned
	} else {
		// RSI/RDI and all XMM registers are volatile to a SysV guest. Deliberately
		// leave FP controls/DF dirty as well to exercise the landing's restoration.
		code.Imm(DI, 0xdeadbeef); code.Imm(SI, 0xcafebabe);
		code.Imm(AX, 0xffffffffffffffa5ull); code.B(0xfd); code.B(0xc3);
	}
	code.Seal();
}

void TestMachineState(Boundary& boundary, Owner owner) {
	Code probe;
	MakeProbe(probe);
	for (bool abort: {false, true}) {
		Code guest;
		MakeDirtyGuest(guest, abort);
		MachineState state;
		state.boundary = &boundary; state.owner = owner; state.abort = boundary.AbortFunction();
		state.guest = reinterpret_cast<Loader::GuestBoundaryFunction>(guest.memory);
		for (unsigned i = 0; i != sizeof(state.expected_xmm); ++i) state.expected_xmm[i] = static_cast<uint8_t>(i * 13 + 1);
		reinterpret_cast<void (*)(MachineState*)>(probe.memory)(&state);
		Check(state.result.status == Status::Ok && state.result.tag == (abort ? Tag::Aborted : Tag::Returned), "machine probe tagged result");
		if (!abort) Check(state.result.value == 0xffffffffffffffa5ull, "negative ordinary machine return");
		for (unsigned i = 0; i != 8; ++i) Check(state.observed_gpr[i] == 0x98760000 + i, "Win64 nonvolatile GPR restoration");
		Check(std::memcmp(state.expected_xmm, state.observed_xmm, sizeof(state.expected_xmm)) == 0, "Win64 XMM6-15 restoration");
		Check(state.before == state.after && (state.before & 15) == 0, "stack restored and call aligned");
		Check((state.flags & 0x400) == 0, "dirty guest DF does not escape");
		Check(state.host_mxcsr == state.observed_mxcsr && state.host_cw == state.observed_cw, "MXCSR/x87 control restored");
		Check(state.teb_base_before == state.teb_base_after && state.teb_limit_before == state.teb_limit_after, "TEB bounds unchanged");
		Check(Boundary::CurrentDepth() == 0, "machine probe pops TLS");
	}
}

void TestResolverTailJump(Boundary& boundary, Owner owner) {
	Code thunk;
	thunk.Endbr(); thunk.Stack(true, 8);
	thunk.Imm(SI, 0x1020304050607080ull);
	thunk.Imm(AX, reinterpret_cast<uintptr_t>(&AddressResolver)); thunk.Bytes({0xff, 0xd0});
	thunk.Stack(false, 8);
	// The no-arg gateway cannot depend on original unknown-API arguments. This
	// fixture deliberately overwrites both registers used by the typed abort API.
	thunk.Imm(DI, 0x11111111); thunk.Imm(SI, 0x22222222);
	thunk.Bytes({0xff, 0xe0}); // tail-jump returned address, same guest return continuation
	thunk.Seal();
	Nested state {&boundary, owner};
	const auto result = boundary.Invoke(reinterpret_cast<Loader::GuestBoundaryFunction>(thunk.memory), &state);
	Check(result.tag == Tag::Aborted && state.resolver_returns == 1 && state.destroyed == 1 && state.live == 0,
	      "resolver returns address after RAII, no-arg tail-jump aborts");
	Check(Boundary::CurrentDepth() == 0, "tail-jump failure pops TLS");
}
#endif
} // namespace

int main() {
	int identity = 0;
	const Owner owner {&identity, 0x123456789abcdef0ull};
	Status status = Status::Ok;
	auto boundary = Boundary::Create(owner, &status);
	if (status == Status::Unsupported) {
		Check(!boundary && Boundary::CurrentDepth() == 0, "unsupported platform has no pretend capability");
		std::puts("GuestCallBoundaryTests: SKIP (unsupported ABI/OS mitigation capability)");
		return 77;
	}
#if BOUNDARY_FIXTURE_WIN64
	Check(boundary && status == Status::Ok, "create supported boundary");
	Check(!Boundary::Create({nullptr, 1}, &status) && status == Status::InvalidArgument, "null owner rejected");
	Check(!Boundary::Create({&identity, 0}, &status) && status == Status::InvalidArgument, "zero lifetime token rejected");
	Check(boundary->Invoke(nullptr, nullptr).tag == Tag::NotInvoked, "invalid call not invoked");
	Check(boundary->FailureFunction() == nullptr && boundary->AbortFunction()(owner.identity, owner.token) == 0,
	      "no active boundary rejects abort and gateway acquisition");
	for (uint64_t bits: {uint64_t(0), uint64_t(7), uint64_t(-1), uint64_t(-91)}) {
		const auto result = boundary->Invoke(ReturnBits, &bits);
		Check(result.status == Status::Ok && result.tag == Tag::Returned && result.value == bits, "all ordinary result bits distinct from abort tag");
	}
	auto foreign = Boundary::Create(owner, &status);
	Check(foreign && status == Status::Ok, "second engine for wrong-engine validation");
	RejectState rejection {boundary.get(), foreign.get(), owner};
	Check(boundary->Invoke(RejectGuest, &rejection).tag == Tag::Returned, "rejected abort resumes only typed transition test");
	int other_identity = 0;
	auto other_owner = Boundary::Create({&other_identity, 42}, &status);
	Check(other_owner && status == Status::Ok, "different owner boundary");
	CrossState cross {boundary.get(), other_owner.get(), owner};
	Check(boundary->Invoke(CrossOuterGuest, &cross).tag == Tag::Returned && Boundary::CurrentDepth() == 0,
	      "different-owner nesting remains LIFO");
	for (unsigned repeat = 0; repeat != 32; ++repeat) {
		Nested state {boundary.get(), owner};
		const auto result = boundary->Invoke(OuterGuest, &state);
		Check(result.status == Status::Ok && result.tag == Tag::Aborted, "outer tagged abort");
		Check(state.live == 0 && state.destroyed == 3 && state.resolver_returns == 2 && state.rolled_back &&
		      !state.inner_after && !state.outer_after, "nested failure skips no host RAII and resumes no failed guest continuation");
		Check(Boundary::CurrentDepth() == 0 && boundary->FailureFunction() == nullptr, "complete LIFO pop and no stale gateway acquisition");
	}
	ThreadState threads {boundary.get(), owner};
	const auto parent = boundary->Invoke(ParentGuest, &threads);
	Check(parent.tag == Tag::Returned && parent.value == 73 && threads.child_aborted, "same owner on different actual threads remains independent");
	TestMachineState(*boundary, owner);
	TestResolverTailJump(*boundary, owner);
	std::puts("GuestCallBoundaryTests: passed (standalone groundwork only)");
	return 0;
#else
	Check(false, "unsupported build unexpectedly advertised capability");
	return 1;
#endif
}
