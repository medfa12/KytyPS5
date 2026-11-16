// Standalone: hostCallReturnThunk.cpp + guestCallBoundary.cpp + xbyak + threads.
// All executable calls below use the ACTUAL generated gateway and boundary.
#include "loader/hostCallReturnThunk.h"
#include "loader/deferredImportThunk.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) && !defined(_M_ARM64EC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <xbyak/xbyak.h>
#define HOST_RETURN_FIXTURE 1
#else
#define HOST_RETURN_FIXTURE 0
#endif

namespace {
using Boundary = Loader::GuestCallBoundary;
using Thunk = Loader::HostCallReturnThunk;
using Disposition = Loader::HostCallDisposition;
using Tag = Loader::GuestCallOutcomeTag;
void Check(bool condition, const char* text) {
	if (!condition) { std::fprintf(stderr, "HostCallReturnThunkTests: %s\n", text); std::abort(); }
}

#if HOST_RETURN_FIXTURE
thread_local DWORD expected_thread = 0;
struct State;
thread_local State* last_target_destroyed = nullptr;
struct State {
	Boundary* boundary = nullptr;
	Thunk* thunk = nullptr;
	State* inner = nullptr;
	State* propagate = nullptr;
	std::atomic<unsigned> enters {0}, leaves {0}, enter_cleanups {0}, leave_cleanups {0};
	std::atomic<unsigned> targets {0}, host_live {0}, host_cleanups {0}, guest_after {0};
	std::atomic<bool> failed {false};
	bool fail_enter = false, fail_target = false;
	unsigned remaining = 0;
	std::atomic<unsigned>* rendezvous = nullptr;
	std::atomic<uint64_t> last_caller {0};
	uint64_t deferred = 0;
	std::atomic<unsigned> resolver_cleanups {0};
};
struct Cleanup {
	std::atomic<unsigned>& count;
	~Cleanup() { count.fetch_add(1); }
};

// Deliberately destroy volatile arguments/results so favorable optimizer register
// allocation cannot conceal missing preservation in either generated path.
void KYTY_GUEST_CALL_BOUNDARY_SYSV Clobber() noexcept {
	asm volatile(
	    "mov $-1, %%rax\n\tmov $-2, %%rdx\n\tmov $-3, %%rdi\n\tmov $-4, %%rsi\n\t"
	    "mov $-5, %%rcx\n\tmov $-6, %%r8\n\tmov $-7, %%r9\n\tmov $-8, %%r10\n\t"
	    "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\tpxor %%xmm2, %%xmm2\n\t"
	    "pxor %%xmm3, %%xmm3\n\tpxor %%xmm4, %%xmm4\n\tpxor %%xmm5, %%xmm5\n\t"
	    "pxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7\n\tfninit"
	    : : : "rax", "rdx", "rdi", "rsi", "rcx", "r8", "r9", "r10",
	            "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory", "cc");
}
Disposition KYTY_GUEST_CALL_BOUNDARY_SYSV Enter(void* opaque, uint64_t caller, Loader::HostCallEnterReason reason) noexcept {
	auto& s = *static_cast<State*>(opaque);
	Cleanup cleanup {s.enter_cleanups};
	Check(caller != 0 && GetCurrentThreadId() == expected_thread, "enter actual thread and original continuation");
	Check(s.boundary->FailureFunction() != nullptr && Thunk::CurrentDepth() != 0, "enter exact boundary/TLS record");
	s.enters.fetch_add(1);
	s.last_caller.store(caller);
	last_target_destroyed = nullptr;
	if (s.fail_enter || reason == Loader::HostCallEnterReason::CapacityExceeded) {
		s.failed.store(true);
		if (s.propagate) s.propagate->failed.store(true);
	}
	const auto result = s.failed.load() ? Disposition::Abort : Disposition::Proceed;
	Clobber();
	return result;
}
Disposition KYTY_GUEST_CALL_BOUNDARY_SYSV Leave(void* opaque, uint64_t caller) noexcept {
	auto& s = *static_cast<State*>(opaque);
	Cleanup cleanup {s.leave_cleanups};
	Check(caller != 0 && GetCurrentThreadId() == expected_thread, "leave actual thread and TLS continuation");
	Check(s.boundary->FailureFunction() != nullptr, "leave exact innermost boundary");
	s.leaves.fetch_add(1);
	if (s.failed.load()) Check(last_target_destroyed == &s, "this thread's HLE RAII returned before failure barrier");
	const auto result = s.failed.load() ? Disposition::Abort : Disposition::Proceed;
	Clobber();
	return result;
}
std::unique_ptr<Thunk> Make(State& s, uint64_t target) {
	Loader::HostCallReturnThunkStatus status;
	auto result = Thunk::Create(s.boundary, target, &s, Enter, Leave, Loader::HostCallAbi::BaselineSysV, &status);
	Check(result != nullptr && status == Loader::HostCallReturnThunkStatus::Ok, "create gateway");
	s.thunk = result.get();
	return result;
}
std::unique_ptr<Boundary> MakeBoundary(State& s, uint64_t token) {
	auto result = Boundary::Create({&s, token});
	Check(bool(result), "create fixture boundary"); s.boundary = result.get(); return result;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Guest(void* opaque) {
	auto* s = static_cast<State*>(opaque); // Deliberately no nontrivial guest locals.
	using Call = uint64_t (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(void*);
	const auto value = reinterpret_cast<Call>(s->deferred != 0 ? s->deferred : s->thunk->GetAddress())(s);
	s->guest_after.fetch_add(1);
	return value;
}
struct HostOwner {
	State& s;
	explicit HostOwner(State& value): s(value) { s.host_live.fetch_add(1); }
	~HostOwner() { s.host_live.fetch_sub(1); s.host_cleanups.fetch_add(1); last_target_destroyed = &s; }
};
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV Target(void* opaque) {
	auto& s = *static_cast<State*>(opaque);
	HostOwner owner(s);
	const auto ordinal = s.targets.fetch_add(1);
	if (s.rendezvous) {
		s.rendezvous->fetch_add(1);
		while (s.rendezvous->load() < 8) std::this_thread::yield();
	}
	if (s.remaining) {
		--s.remaining;
		const auto outcome = s.boundary->Invoke(Guest, &s);
		Check(s.failed.load() ? outcome.tag == Tag::Aborted :
		      outcome.tag == Tag::Returned && outcome.value == 0xfedcba9876543210ull,
		      "same gateway reentrant target or capacity abort");
	}
	if (s.inner) {
		Check(s.inner->boundary->FailureFunction() == nullptr, "different owner is not outer active owner");
		const auto outcome = s.inner->boundary->Invoke(Guest, s.inner);
		Check(outcome.tag == (s.inner->failed.load() ? Tag::Aborted : Tag::Returned), "inner tagged outcome");
		Check(s.boundary->FailureFunction() != nullptr && s.inner->boundary->FailureFunction() == nullptr,
		      "inner pop restores exact outer owner");
		Check(s.host_live.load() == 1, "real outer HLE RAII survives inner abort");
	}
	if (s.fail_target) {
		// Concurrent case: one actual peer latches failure while every target
		// has entered. Other real HLE calls return successfully into the barrier.
		if (!s.rendezvous || ordinal == 0) s.failed.store(true);
		else while (!s.failed.load()) std::this_thread::yield();
		if (s.propagate) s.propagate->failed.store(true);
	}
	return 0xfedcba9876543210ull;
}

void CheckDrained() {
	Check(Thunk::CurrentDepth() == 0 && Boundary::CurrentDepth() == 0, "both actual-thread LIFO stacks drain");
}
void TestRecovery() {
	for (unsigned mode = 0; mode != 3; ++mode) {
		State outer, inner;
		auto outer_boundary = MakeBoundary(outer, 1);
		auto inner_boundary = MakeBoundary(inner, 2);
		outer.inner = &inner;
		inner.propagate = mode == 0 ? nullptr : &outer;
		inner.fail_enter = mode == 1;
		inner.fail_target = mode == 2;
		auto outer_thunk = Make(outer, reinterpret_cast<uint64_t>(&Target));
		auto inner_thunk = Make(inner, reinterpret_cast<uint64_t>(&Target));
		for (unsigned i = 1; i <= 32; ++i) {
			outer.failed.store(false); inner.failed.store(false);
			const auto outcome = outer_boundary->Invoke(Guest, &outer);
			Check(outcome.tag == (mode == 0 ? Tag::Returned : Tag::Aborted), "outer return barrier propagation");
			Check(outer.host_cleanups.load() == i && outer.host_live.load() == 0, "outer HLE destructor exactly once");
			Check(outer.enter_cleanups.load() == i && outer.leave_cleanups.load() == i, "outer callback RAII exactly once");
			Check(inner.enter_cleanups.load() == i, "inner enter RAII before failure transfer");
			Check(inner.targets.load() == (mode == 1 ? 0 : i), "enter failure never invokes target");
			Check(inner.leave_cleanups.load() == (mode == 1 ? 0 : i), "leave only for admitted HLE");
			Check(outer.guest_after.load() == (mode == 0 ? i : 0) && inner.guest_after.load() == (mode == 0 ? i : 0),
			      "no failed inner/outer guest continuation");
			CheckDrained();
		}
	}
	State recursive;
	auto boundary = MakeBoundary(recursive, 3);
	auto thunk = Make(recursive, reinterpret_cast<uint64_t>(&Target));
	recursive.remaining = 12;
	Check(boundary->Invoke(Guest, &recursive).tag == Tag::Returned, "recursive callback into same gateway");
	Check(recursive.targets.load() == 13 && recursive.host_cleanups.load() == 13 && recursive.leaves.load() == 13,
	      "all reentrant records pop LIFO and HLE owners return normally");
	CheckDrained();
}

uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV MissingImport(void* opaque, uint64_t caller) noexcept {
	auto& s = *static_cast<State*>(opaque);
	Cleanup cleanup {s.resolver_cleanups};
	Check(caller != 0, "deferred resolver has real guest caller");
	s.failed.store(true);
	s.propagate->failed.store(true);
	return reinterpret_cast<uint64_t>(s.boundary->FailureFunction());
}

void TestDeferredRecoveryAndCapacity() {
	State outer, inner;
	auto outer_boundary = MakeBoundary(outer, 8);
	auto inner_boundary = MakeBoundary(inner, 9);
	outer.inner = &inner; inner.propagate = &outer;
	auto host_thunk = Make(outer, reinterpret_cast<uint64_t>(&Target));
	auto missing = Loader::DeferredImportThunk::Create(&inner, MissingImport);
	Check(bool(missing), "create actual unresolved import thunk");
	inner.deferred = missing->GetAddress();
	const auto result = outer_boundary->Invoke(Guest, &outer);
	Check(result.tag == Tag::Aborted && inner.resolver_cleanups.load() == 1 &&
	      outer.host_cleanups.load() == 1 && outer.leave_cleanups.load() == 1 &&
	      inner.guest_after.load() == 0 && outer.guest_after.load() == 0,
	      "actual unresolved import aborts inner segment; HLE cleans up then outer return barrier aborts");
	CheckDrained();
	State capacity;
	auto boundary = MakeBoundary(capacity, 10);
	auto thunk = Make(capacity, reinterpret_cast<uint64_t>(&Target));
	capacity.remaining = Thunk::MaximumDepth + 4;
	Check(boundary->Invoke(Guest, &capacity).tag == Tag::Aborted, "capacity exhaustion aborts the owner");
	Check(capacity.enters.load() == Thunk::MaximumDepth + 1 &&
	      capacity.targets.load() == Thunk::MaximumDepth && capacity.leaves.load() == Thunk::MaximumDepth &&
	      capacity.host_cleanups.load() == Thunk::MaximumDepth && capacity.host_live.load() == 0 &&
	      capacity.guest_after.load() == 0,
	      "capacity refusal never invokes excess HLE; all admitted host frames clean up normally");
	CheckDrained();
}

class Executable {
public:
	explicit Executable(const Xbyak::CodeGenerator& code) {
		memory_ = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		Check(memory_ != nullptr && code.getSize() <= 4096, "fixture allocation");
		std::memcpy(memory_, code.getCode(), code.getSize()); DWORD previous = 0;
		Check(VirtualProtect(memory_, 4096, PAGE_EXECUTE_READ, &previous) &&
		      FlushInstructionCache(GetCurrentProcess(), memory_, code.getSize()), "fixture RW to RX");
	}
	~Executable() { Check(VirtualFree(memory_, 0, MEM_RELEASE) != FALSE, "fixture release"); }
	uint64_t Address() const { return reinterpret_cast<uint64_t>(memory_); }
private:
	void* memory_ = nullptr;
};

struct Snapshot {
	std::array<uint64_t, 8> gp {};
	std::array<std::array<uint64_t, 2>, 8> xmm {};
	std::array<uint64_t, 6> nv {};
	std::array<uint64_t, 4> stack {};
	uint64_t rsp = 0, after_rsp = 0;
	std::array<uint64_t, 2> result_gp {};
	std::array<std::array<uint64_t, 2>, 2> result_xmm {};
	std::array<std::array<uint8_t, 10>, 2> result_x87 {};
};
void TestRawAbi() {
	using namespace Xbyak::util;
	State state;
	auto boundary = MakeBoundary(state, 4);
	Snapshot expected, actual;
	for (size_t i = 0; i != 8; ++i) {
		expected.gp[i] = 0x8877665544332200ull + i;
		expected.xmm[i] = {0x1122334455667700ull + i, 0x99aabbccddeeff00ull + i};
	}
	for (size_t i = 0; i != 6; ++i) expected.nv[i] = 0x9876543210123400ull + i;
	for (size_t i = 0; i != 4; ++i) expected.stack[i] = 0xfedcba9876543200ull + i;
	expected.result_gp = {0xfedcba9876543210ull, 0xabcdef1234567890ull};
	expected.result_xmm = {{{0xabcdef0011223344ull, 0x5566778899aabbccull}, {0x123456789abcdef0ull, 0xfedcba9876543210ull}}};
	// Exact x87 80-bit +1 and +2, independent of host long-double typedef.
	expected.result_x87[0] = {0,0,0,0,0,0,0,0x80,0xff,0x3f};
	expected.result_x87[1] = {0,0,0,0,0,0,0,0x80,0,0x40};
	std::array<uint8_t, 2048> target_bytes {}, caller_bytes {};
	Xbyak::CodeGenerator t(target_bytes.size(), target_bytes.data());
	const std::array<Xbyak::Reg64, 8> gp {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
	const std::array<Xbyak::Reg64, 6> nv {rbx, rbp, r12, r13, r14, r15};
	t.mov(r11, reinterpret_cast<uint64_t>(&actual));
	for (size_t i = 0; i != gp.size(); ++i) t.mov(t.ptr[r11 + offsetof(Snapshot, gp) + i * 8], gp[i]);
	for (int i = 0; i != 8; ++i) t.movdqu(t.ptr[r11 + offsetof(Snapshot, xmm) + i * 16], Xbyak::Xmm(i));
	for (size_t i = 0; i != nv.size(); ++i) t.mov(t.ptr[r11 + offsetof(Snapshot, nv) + i * 8], nv[i]);
	t.mov(t.ptr[r11 + offsetof(Snapshot, rsp)], rsp);
	for (int i = 0; i != 4; ++i) {
		t.mov(rax, t.ptr[rsp + 8 + i * 8]); t.mov(t.ptr[r11 + offsetof(Snapshot, stack) + i * 8], rax);
	}
	t.mov(r11, reinterpret_cast<uint64_t>(&expected));
	t.mov(rax, t.ptr[r11 + offsetof(Snapshot, result_gp)]); t.mov(rdx, t.ptr[r11 + offsetof(Snapshot, result_gp) + 8]);
	for (int i = 0; i != 2; ++i) t.movdqu(Xbyak::Xmm(i), t.ptr[r11 + offsetof(Snapshot, result_xmm) + i * 16]);
	// REX.B DB /5 m80real, base R11 + disp32 (no assembler-size alias dependency).
	const auto fld80 = [&](uint32_t offset) { t.db(0x41); t.db(0xdb); t.db(0xab); t.dd(offset); };
	fld80(offsetof(Snapshot, result_x87) + 10); fld80(offsetof(Snapshot, result_x87));
	t.ret(); t.ready(); Executable target(t);
	auto thunk = Make(state, target.Address());
	Xbyak::CodeGenerator c(caller_bytes.size(), caller_bytes.data());
	for (auto reg: nv) c.push(reg);
	c.sub(rsp, 40);
	for (size_t i = 0; i != 4; ++i) { c.mov(rax, expected.stack[i]); c.mov(c.ptr[rsp + i * 8], rax); }
	for (size_t i = 0; i != nv.size(); ++i) c.mov(nv[i], expected.nv[i]);
	c.mov(r11, reinterpret_cast<uint64_t>(&expected.xmm));
	for (int i = 0; i != 8; ++i) c.movdqu(Xbyak::Xmm(i), c.ptr[r11 + i * 16]);
	for (size_t i = 0; i != gp.size(); ++i) c.mov(gp[i], expected.gp[i]);
	c.mov(r11, thunk->GetAddress()); c.call(r11);
	const auto continuation_offset = c.getSize();
	c.mov(r11, reinterpret_cast<uint64_t>(&actual));
	c.mov(c.ptr[r11 + offsetof(Snapshot, after_rsp)], rsp);
	c.mov(c.ptr[r11 + offsetof(Snapshot, result_gp)], rax); c.mov(c.ptr[r11 + offsetof(Snapshot, result_gp) + 8], rdx);
	for (int i = 0; i != 2; ++i) c.movdqu(c.ptr[r11 + offsetof(Snapshot, result_xmm) + i * 16], Xbyak::Xmm(i));
	// REX.B DB /7 m80real, base R11 + disp32.
	const auto fstp80 = [&](uint32_t offset) { c.db(0x41); c.db(0xdb); c.db(0xbb); c.dd(offset); };
	fstp80(offsetof(Snapshot, result_x87)); fstp80(offsetof(Snapshot, result_x87) + 10);
	c.add(rsp, 40); for (auto it = nv.rbegin(); it != nv.rend(); ++it) c.pop(*it);
	c.ret(); c.ready(); Executable caller(c);
	const auto outcome = boundary->Invoke(reinterpret_cast<Loader::GuestBoundaryFunction>(caller.Address()), nullptr);
	Check(outcome.tag == Tag::Returned && outcome.value == expected.result_gp[0], "first raw call returns exact RAX");
	Check(actual.gp == expected.gp && actual.xmm == expected.xmm && actual.nv == expected.nv && actual.stack == expected.stack,
	      "first invocation preserves full RAX/AL, all integer/SSE/stack arguments and nonvolatiles");
	Check(actual.rsp % 16 == 8 && actual.rsp + 8 == actual.after_rsp, "target original stack and return alignment");
	Check(state.last_caller.load() == caller.Address() + continuation_offset, "callback original guest continuation, not HLE epilogue");
	Check(actual.result_gp == expected.result_gp && actual.result_xmm == expected.result_xmm && actual.result_x87 == expected.result_x87,
	      "leave callback preserves RAX/RDX, all XMM0/1 bits and both x87 result slots");
	Check(state.enter_cleanups.load() == 1 && state.leave_cleanups.load() == 1, "first-call callbacks exactly once");
	CheckDrained();
}

struct Mixed { uint64_t integer; double real; };
struct Large { uint64_t a, b, c, d; };
Mixed KYTY_GUEST_CALL_BOUNDARY_SYSV MixedTarget(__m128 lanes,
    uint64_t a, double x, uint64_t b, double y, uint64_t c, double z, uint64_t d, double w,
    uint64_t e, double p, uint64_t f, double q, uint64_t g, double r, uint64_t h, double v) {
	alignas(16) float values[4]; _mm_store_ps(values, lanes);
	Check(a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 &&
	      x == 1.25 && y == 2.25 && z == 3.25 && w == 4.25 && p == 5.25 && q == 6.25 && r == 7.25 && v == 8.25,
	      "compiler typed mixed integer/vector/overflow stack arguments");
	return {a + h, x + v + values[3]};
}
Large KYTY_GUEST_CALL_BOUNDARY_SYSV MemoryTarget(Large input, uint64_t extra) {
	return {input.a + extra, input.b + extra, input.c + extra, input.d + extra};
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV MixedGuest(void* opaque) {
	auto* s = static_cast<State*>(opaque);
	const auto result = reinterpret_cast<decltype(&MixedTarget)>(s->thunk->GetAddress())(_mm_set_ps(8,4,2,1),
	    1,1.25,2,2.25,3,3.25,4,4.25,5,5.25,6,6.25,7,7.25,8,8.25);
	Check(result.integer == 9 && result.real == 17.5, "compiler mixed RAX/XMM aggregate result"); return 9;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV MemoryGuest(void* opaque) {
	auto* s = static_cast<State*>(opaque);
	const auto result = reinterpret_cast<decltype(&MemoryTarget)>(s->thunk->GetAddress())({1,2,3,4}, 8);
	Check(result.a == 9 && result.b == 10 && result.c == 11 && result.d == 12, "hidden sret pointer/stack aggregate arguments"); return result.d;
}
uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV VariadicGuest(void* opaque) {
	auto* state = static_cast<State*>(opaque);
	using Variadic = uint64_t (KYTY_GUEST_CALL_BOUNDARY_SYSV *)(uint64_t, ...);
	return reinterpret_cast<Variadic>(state->thunk->GetAddress())(uint64_t(1), 1.25, 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.25, 9.25);
}

// Variadic entry is generated so AL is observed before any compiler prologue.
void TestTypedAndVarargs() {
	State mixed; auto b = MakeBoundary(mixed, 5);
	auto t = Make(mixed, reinterpret_cast<uint64_t>(&MixedTarget));
	Check(b->Invoke(MixedGuest, &mixed).value == 9, "typed mixed first invocation");
	t.reset(); t = Make(mixed, reinterpret_cast<uint64_t>(&MemoryTarget));
	Check(b->Invoke(MemoryGuest, &mixed).value == 12, "typed MEMORY aggregate first invocation");
	std::array<uint8_t, 128> bytes {};
	std::array<uint64_t, 10> observed {}; // RDI, overflow FP argument, XMM0..7 low halves.
	Xbyak::CodeGenerator code(bytes.size(), bytes.data());
	using namespace Xbyak::util;
	code.movzx(eax, al);
	code.mov(r11, reinterpret_cast<uint64_t>(observed.data())); code.mov(code.ptr[r11], rdi);
	code.mov(rdx, code.ptr[rsp + 8]); code.mov(code.ptr[r11 + 8], rdx);
	for (int i = 0; i != 8; ++i) code.movsd(code.ptr[r11 + 16 + i * 8], Xbyak::Xmm(i));
	code.ret(); code.ready(); Executable target(code);
	t.reset(); t = Make(mixed, target.Address());
	Check(b->Invoke(VariadicGuest, &mixed).value == 8, "compiler variadic AL survives first enter callback");
	Check(observed[0] == 1, "compiler variadic integer argument");
	for (size_t i = 1; i != observed.size(); ++i) {
		double value = 0; std::memcpy(&value, &observed[i], sizeof(value));
		Check(value == (i == 1 ? 9.25 : double(i - 2) + 1.25), "compiler variadic XMM and overflow stack arguments");
	}
	CheckDrained();
}

void TestConcurrent(bool fail) {
	State shared; auto boundary = MakeBoundary(shared, 6);
	shared.fail_target = fail;
	std::atomic<unsigned> rendezvous {0}; shared.rendezvous = &rendezvous;
	auto thunk = Make(shared, reinterpret_cast<uint64_t>(&Target));
	Check(boundary->FailureFunction() == nullptr, "no borrowed main-thread boundary");
	std::vector<std::thread> workers;
	for (unsigned i = 0; i != 8; ++i) workers.emplace_back([&] {
		expected_thread = GetCurrentThreadId();
		for (unsigned j = 0; j != (fail ? 1u : 64u); ++j) {
			const auto outcome = boundary->Invoke(Guest, &shared);
			Check(outcome.tag == (fail ? Tag::Aborted : Tag::Returned) &&
			      (fail || outcome.value == 0xfedcba9876543210ull), "concurrent real result or sticky peer-failure abort");
			CheckDrained();
		}
	});
	for (auto& worker: workers) worker.join();
	const unsigned calls = fail ? 8 : 512;
	Check(shared.targets.load() == calls && shared.enters.load() == calls && shared.leaves.load() == calls &&
	      shared.enter_cleanups.load() == calls && shared.leave_cleanups.load() == calls && shared.host_cleanups.load() == calls,
	      "shared gateway concurrent actual-thread TLS and callback/HLE lifetimes");
	Check(shared.guest_after.load() == (fail ? 0 : calls), "peer failure suppresses every pending guest continuation");
	CheckDrained();
}

void TestLifetimeAndUnsupported() {
	State s; auto boundary = MakeBoundary(s, 7);
	Loader::HostCallReturnThunkStatus status;
	for (auto abi: {Loader::HostCallAbi::MicrosoftX64, Loader::HostCallAbi::Avx, Loader::HostCallAbi::Avx512}) {
		Check(!Thunk::Create(boundary.get(), reinterpret_cast<uint64_t>(&Target), &s, Enter, Leave, abi, &status) &&
		      status == Loader::HostCallReturnThunkStatus::Unsupported, "unsupported ABI rejected, never zero API return");
	}
	Check(!Thunk::Create(nullptr, reinterpret_cast<uint64_t>(&Target), &s, Enter, Leave), "null boundary rejected");
	Check(!Thunk::Create(boundary.get(), 0, &s, Enter, Leave), "null target rejected");
	Check(!Thunk::Create(boundary.get(), reinterpret_cast<uint64_t>(&Target), &s, nullptr, Leave), "null callback rejected");
	for (unsigned i = 0; i != 16; ++i) {
		auto thunk = Make(s, reinterpret_cast<uint64_t>(&Target));
		const auto address = thunk->GetAddress(); MEMORY_BASIC_INFORMATION info {};
		Check(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0 && info.Protect == PAGE_EXECUTE_READ,
		      "published gateway immutable RX, not RWX");
		Check(boundary->Invoke(Guest, &s).tag == Tag::Returned, "lifetime fixture normal call");
		std::array<uint8_t, 1024> before {}; std::memcpy(before.data(), reinterpret_cast<void*>(address), before.size());
		Check(boundary->Invoke(Guest, &s).tag == Tag::Returned &&
		      std::memcmp(before.data(), reinterpret_cast<void*>(address), before.size()) == 0, "calls never patch executable mapping");
		thunk.reset(); // All references revoked; all target/epilogue invocations quiesced.
		Check(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0 && info.State == MEM_FREE,
		      "quiesced destructor releases owned code");
	}
}
#endif
} // namespace

int main() {
#if HOST_RETURN_FIXTURE
	expected_thread = GetCurrentThreadId();
	State capability;
	Loader::GuestCallBoundaryStatus status;
	auto probe = Boundary::Create({&capability, 1}, &status);
	if (status == Loader::GuestCallBoundaryStatus::Unsupported) {
		std::puts("HostCallReturnThunkTests: SKIP unsupported boundary/CET capability"); return 77;
	}
	Check(bool(probe), "capability allocation");
	capability.boundary = probe.get();
	Loader::HostCallReturnThunkStatus gateway_status;
	auto gateway_probe = Thunk::Create(probe.get(), reinterpret_cast<uint64_t>(&Target), &capability, Enter, Leave,
	                                   Loader::HostCallAbi::BaselineSysV, &gateway_status);
	if (gateway_status == Loader::HostCallReturnThunkStatus::Unsupported) {
		std::puts("HostCallReturnThunkTests: SKIP unsupported gateway/CET capability"); return 77;
	}
	Check(bool(gateway_probe), "gateway capability allocation");
	TestLifetimeAndUnsupported(); TestRawAbi(); TestTypedAndVarargs(); TestRecovery();
	TestDeferredRecoveryAndCapacity(); TestConcurrent(false); TestConcurrent(true);
	std::puts("HostCallReturnThunkTests: passed"); return 0;
#else
	Loader::HostCallReturnThunkStatus status;
	Check(!Thunk::Create(nullptr, 0, nullptr, nullptr, nullptr, Loader::HostCallAbi::BaselineSysV, &status) &&
	      status == Loader::HostCallReturnThunkStatus::Unsupported, "unsupported platform fails closed");
	std::puts("HostCallReturnThunkTests: SKIP unsupported platform"); return 77;
#endif
}
