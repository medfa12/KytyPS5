// Include after loader/runtimeNpQuiescence.h, loader/runtimeNpTransport.h,
// loader/runtimeLinker.h, kernel/pthread.h, kernel/ipmi.h, and
// <atomic>, <chrono>, <cstdio>, <cstdlib>. Requires runtimeNpQuiescence.cpp,
// discovered by the integrated target's CONFIGURE_DEPENDS loader source glob.
// All three entry points run on the SAME actual admitted Core creator pthread,
// which must also perform native init and the sole native term. Never use a main
// thread selected-Program scope. Native service must already be initialized.
// Begin may follow the existing manager-client fixture's permanent epoch close.
// AfterTerm arguments are POLICY-TEST inputs, not captured native join/destroy
// return values. Native term's aggregate zero is NOT an actual join return.
// Genuine receipts and exact destroyed-object evidence are independently
// required. This exercises component guards only, NOT postcall gateway capture,
// pre-free interception, or production continuation safety.
namespace RuntimeNpQuiescenceTest {
namespace Q = Loader::RuntimeNpQuiescence;
namespace K = Libs::LibKernel;
namespace I = K::Ipmi;
// One fixture lifecycle per process, matching the singleton native service.
uint64_t begun_epoch = 0, begun_server = 0, begun_handle = 0;
void Require(bool value) {
	if (!value) { std::fprintf(stderr, "NP IPC-only quiescence fixture failed\n"); std::abort(); }
}
struct Foreign {
	Q::OwnedState state;
	std::atomic_bool rejected {false};
};
void* KYTY_SYSV_ABI ForeignCreator(void* input) {
	auto& foreign = *static_cast<Foreign*>(input);
	Q::Progress unchanged = Q::Progress::NativeExited;
	Require(Q::Advance(foreign.state, unchanged) == I::Invalid);
	Require(unchanged == Q::Progress::NativeExited);
	foreign.rejected = true;
	return nullptr;
}
void CheckActualOtherThread(const Q::OwnedState& state) {
	Foreign foreign {state};
	K::Pthread thread = nullptr;
	K::PthreadNativeReceipt receipt;
	Require(K::PthreadCreateWithNativeReceipt(&thread, nullptr, ForeignCreator, &foreign,
	    "NpQuiescenceForeign", receipt) == 0 && thread);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		K::PthreadNativeSnapshot snapshot;
		const int error = K::PthreadInspectNativeReceipt(receipt, snapshot);
		Require(error == 0 || error == I::Busy);
		if (!error && snapshot.phase == K::PthreadNativePhase::NativeExited) { break; }
		Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
	for (;;) {
		const int error = K::PthreadJoinNativeReceipt(receipt, thread, nullptr);
		if (!error) { break; }
		Require(error == I::Busy && std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
	Require(foreign.rejected.load()); // context is released only after genuine join
}
}

Loader::RuntimeNpQuiescence::OwnedState TestRuntimeNpQuiescenceBegin(
    Loader::Program* core, uint64_t native_server, uint64_t expected_handle, uint64_t epoch) {
	using namespace RuntimeNpQuiescenceTest;
	Require(core && Loader::RuntimeLinker::GetCurrentProgram() == core);
	Require(K::PthreadSelfOrNull() && K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == core);
	Q::OwnedState rejected;
	Require(Q::Begin(epoch == UINT64_MAX ? epoch - 1 : epoch + 1, native_server, expected_handle, rejected) == I::NotFound && !rejected);
	Require(Q::Begin(epoch, native_server, expected_handle == UINT64_MAX ? expected_handle - 1 : expected_handle + 1,
	    rejected) == I::NotFound && !rejected); // failed publication does not reserve the epoch
	Q::OwnedState state;
	Require(Q::Begin(epoch, native_server, expected_handle, state) == 0 && state);
	begun_epoch = epoch; begun_server = native_server; begun_handle = expected_handle;
	Require(Q::Begin(epoch, native_server, expected_handle, rejected) == I::Busy && !rejected);
	// Lose the last public strong reference. The durable reservation must retain
	// the SAME object and its provider pins, rather than allow a second consumer.
	std::weak_ptr<Q::State> weak = state;
	state.reset();
	Require(!weak.expired());
	Require(Q::Begin(epoch, native_server, expected_handle, rejected) == I::Busy && !rejected);
	Require(!Loader::RetireSourceBoundNpTransport());
	state = weak.lock(); // fixture-only recovery of the original authorized handle
	Require(bool(state));
	const auto original = state;
	Require(Q::Begin(epoch, native_server, expected_handle, state) == I::Invalid && state == original);
	Loader::SourceBoundNpTransportEpoch observed;
	Require(Loader::InspectSourceBoundNpTransportEpoch(observed) == 0 && observed.generation == epoch && observed.live && observed.closing);
	// Negative zero inputs cannot stand in for genuine join/destroy evidence.
	Require(Q::ObserveJoin(state, 0) == I::Invalid);
	Require(Q::ObserveDestroy(state, 0) == I::Invalid);
	Require(Q::Finish(state) == I::Invalid && state);
	CheckActualOtherThread(state);
	return state;
}

void TestRuntimeNpQuiescenceAdvance(const Loader::RuntimeNpQuiescence::OwnedState& state) {
	using namespace RuntimeNpQuiescenceTest;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		Q::Progress progress = Q::Progress::Pending;
		const int error = Q::Advance(state, progress);
		Require(error == 0 || error == I::Busy);
		if (!error && progress == Q::Progress::NativeExited) { break; }
		Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
	// Original dispatcher OS exit is NOT join. Parent has not called term yet.
	Require(Q::ObserveJoin(state, 0) == I::WouldBlock);
	Require(Q::ObserveDestroy(state, 0) == I::Invalid);
}

void TestRuntimeNpQuiescenceAfterTerm(Loader::RuntimeNpQuiescence::OwnedState& state,
    int join_policy_input, int destroy_policy_input) {
	using namespace RuntimeNpQuiescenceTest;
	Require(join_policy_input == 0 && destroy_policy_input == 0);
	// Unlike the pre-term check, the native server is now actually destroyed:
	// durable helper ownership itself must still prevent transport retirement.
	std::weak_ptr<Q::State> weak = state;
	state.reset();
	Require(!weak.expired() && !Loader::RetireSourceBoundNpTransport());
	Q::OwnedState duplicate;
	Require(Q::Begin(begun_epoch, begun_server, begun_handle, duplicate) == I::Busy && !duplicate);
	state = weak.lock(); Require(bool(state));
	// Injected errors are negative guard tests, never evidence of native success.
	Require(Q::ObserveJoin(state, I::Busy) == I::Busy);
	Require(Q::ObserveJoin(state, join_policy_input) == 0);
	Require(Q::ObserveJoin(state, join_policy_input) == I::Invalid); // no second component consumption
	Require(Q::ObserveDestroy(state, I::Busy) == I::Busy);
	Require(Q::ObserveDestroy(state, destroy_policy_input) == 0);
	Require(Q::ObserveDestroy(state, destroy_policy_input) == I::Invalid);
	auto finished_copy = state;
	Require(Q::Finish(state) == 0 && !state);
	Q::OwnedState rejected;
	// Must be registry policy Busy, not incidental NotFound from erased server.
	Require(Q::Begin(begun_epoch, begun_server, begun_handle, rejected) == I::Busy && !rejected);
	Require(Q::Finish(finished_copy) == I::Invalid && finished_copy);
	finished_copy.reset();
	Require(Q::Begin(begun_epoch, begun_server, begun_handle, rejected) == I::Busy && !rejected);
	// Parent now retires the source-bound epoch; it must not do so before Finish.
}
