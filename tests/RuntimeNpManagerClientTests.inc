// Include at file scope AFTER the loader/kernel headers, including
// loader/runtimeNpManagerClient.h, loader/runtimeNpTransport.h,
// loader/runtimeLinker.h, kernel/pthread.h,
// kernel/ipmi.h, Windows.h, <atomic>, <chrono>, <cstdlib>.
// Invoke while the real native NP dispatcher is running, AFTER the fixture's
// direct retained client has retired and BEFORE native stop/term:
//   const auto epoch = TestRuntimeNpManagerClientAttached(core, np, race);
// Normal and race scenarios require separate fresh processes/images. Both use
// the parent's already-running FIRST native IPC lifecycle; parent owns term.
// After actual native term and successful RetireSourceBoundNpTransport:
//   TestRuntimeNpManagerClientEpochRetired(epoch, core);
// Replacement coverage is transport-epoch-only. Never restart native IPC: its
// guarded session-manager singleton retains a sentinel from the destroyed heap.
// No replacement service, fake acceptance, callbacks, Session, or account query.
namespace RuntimeNpManagerClientTest {
namespace M = Loader::RuntimeNpManagerClient;
namespace K = Libs::LibKernel;
namespace I = K::Ipmi;
void Require(bool condition, const std::source_location where = std::source_location::current()) noexcept {
	if (!condition) {
		std::fprintf(stderr, "RuntimeNpManagerClient fixture failed at line %u\n", where.line());
		std::abort();
	}
}
struct Live {
	bool armed = true;
	~Live() { if (armed) { std::abort(); } }
};
struct Context {
	Loader::Program* owner;
	Loader::Program* other;
	uint64_t epoch;
	std::atomic_bool done {false};
};
void JoinExited(K::Pthread thread) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	K::PthreadLegacyNativeTarget target;
	while (K::PthreadPinLegacyNativeTarget(thread, &target) != 0) {
		Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
	target.UnlockRecord(); Require(target.Handle());
#if defined(_WIN32)
	for (;;) {
		const DWORD result = WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 0);
		if (result == WAIT_OBJECT_0) { break; }
		Require(result == WAIT_TIMEOUT && std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
#else
	Require(false);
#endif
	Require(K::PthreadJoin(thread, nullptr) == 0);
}
void* KYTY_SYSV_ABI ForeignCloser(void* argument) {
	Live live;
	auto& context = *static_cast<Context*>(argument);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == context.other);
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.epoch) == I::Invalid);
	// Even a forged current-program scope cannot turn this actual foreign
	// pthread into the Core transport owner. No guest work is run in this scope.
	Loader::RuntimeLinker::SetCurrentProgram(context.owner);
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.epoch) == I::Invalid);
	Require(M::CloseAndReapForCurrentTransportOwner(context.epoch) == I::Invalid);
	Loader::RuntimeLinker::SetCurrentProgram(context.other);
	live.armed = false; return nullptr;
}
void WaitAttached(M::Token token, int controller_tid) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		M::Status status;
		Require(M::InspectForCurrentOwner(token, status) == 0);
		if (status.worker_error || status.stop_requested || status.owner_lost) {
			std::fprintf(stderr, "NP worker status: error=%d attach=0x%x stop=%u lost=%u\n",
			             status.worker_error, unsigned(status.attachment_result), unsigned(status.stop_requested), unsigned(status.owner_lost));
		}
		Require(!status.worker_error && !status.stop_requested && !status.owner_lost);
		if (status.attached) {
			Require(status.attachment_result == 0);
			Require(status.transport_tid > 0 && status.delivery_tid > 0);
			Require(status.transport_tid != status.delivery_tid && status.transport_tid != controller_tid && status.delivery_tid != controller_tid);
			return;
		}
		Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
}
void StopAndReap(M::Token token) {
	Require(M::HasLive());
	Require(M::RequestRetirement(token) == 0);
	M::Status status; Require(M::InspectForCurrentOwner(token, status) == 0 && !status.attached && status.stop_requested);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		const int result = M::Reap(token);
		if (result == 0) { break; }
		Require(result == I::WouldBlock || result == I::Busy);
		Require(M::HasLive() && std::chrono::steady_clock::now() < deadline);
		K::PthreadYield();
	}
	Require(!M::HasLive()); // successful Reap includes both actual native joins
	Require(M::Reap(token) == I::NotFound && M::RequestRetirement(token) == I::NotFound);
}

// Real production workers/consumer; only scheduling is controlled. The timeout
// case withholds an actual queued discovery job, not a fabricated Attach result.
struct ColdGate {
	Loader::Program* owner;
	Loader::Program* other;
	M::TestPoint point;
	std::atomic<M::Token> token {0};
	std::atomic_bool entered {false}, release {false}, waiting {false}, done {false};
	bool granted = true; // read only after real native exit/join
};
ColdGate* cold_gate = nullptr; // published before creating threads; removed after joins
void WaitSignal(const std::atomic_bool& signal) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!signal.load()) { Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield(); }
}
void ColdObserver(M::TestPoint point, M::Token token, size_t queued) {
	auto& gate = *cold_gate;
	M::Token zero = 0; gate.token.compare_exchange_strong(zero, token);
	Require(gate.token.load() == token);
	if (point == M::TestPoint::ConsumerWaiting) { gate.waiting.store(true); }
	if (point != gate.point || (point == M::TestPoint::TransportAdvance && !queued)) { return; }
	if (gate.entered.exchange(true)) { return; }
	WaitSignal(gate.release);
}
void* KYTY_SYSV_ABI ColdConsumer(void* argument) {
	Live live;
	auto& gate = *static_cast<ColdGate*>(argument);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == gate.owner &&
	        K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == gate.owner);
	gate.granted = bool(M::AcquireQueryClient()); // cold start, not an explicit Start
	gate.done.store(true); live.armed = false; return nullptr;
}
void* KYTY_SYSV_ABI ForeignConsumer(void* argument) {
	Live live;
	auto& gate = *static_cast<ColdGate*>(argument);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == gate.other);
	Require(!M::AcquireQueryClient());
	Loader::RuntimeLinker::SetCurrentProgram(gate.owner);
	Require(!M::AcquireQueryClient()); // forged current scope cannot adopt the pending client
	Loader::RuntimeLinker::SetCurrentProgram(gate.other);
	live.armed = false; return nullptr;
}
K::Pthread BeginColdConsumer(ColdGate& gate) {
	Require(!M::HasLive() && !cold_gate);
	cold_gate = &gate; M::SetTestObserver(ColdObserver);
	K::Pthread consumer = nullptr;
	Require(K::PthreadCreate(&consumer, nullptr, ColdConsumer, &gate, "NpColdConsumer") == 0 && consumer);
	WaitSignal(gate.entered);
	if (gate.point != M::TestPoint::ConsumerBeforeGrant) { WaitSignal(gate.waiting); }
	Require(gate.token.load() && !gate.done.load());
	return consumer;
}
void CheckForeignColdConsumer(ColdGate& gate) {
	K::Pthread foreign = nullptr;
	Loader::RuntimeLinker::SetCurrentProgram(gate.other);
	Require(K::PthreadCreate(&foreign, nullptr, ForeignConsumer, &gate, "NpColdForeign") == 0 && foreign);
	Loader::RuntimeLinker::SetCurrentProgram(gate.owner);
	JoinExited(foreign);
	Require(!gate.done.load());
}
void EndColdGate() {
	Require(!M::HasLive());
	M::SetTestObserver(nullptr); cold_gate = nullptr;
}
void ColdFailures(Context& context) {
	for (int scenario = 0; scenario < 3; ++scenario) {
		const bool retirement = scenario != 0, before_grant = scenario == 2;
		ColdGate gate {context.owner, context.other,
		    before_grant ? M::TestPoint::ConsumerBeforeGrant :
		    retirement ? M::TestPoint::TransportCreate : M::TestPoint::TransportAdvance};
		auto consumer = BeginColdConsumer(gate);
		CheckForeignColdConsumer(gate);
		if (retirement) {
			// In the stalled-create case no coordinator exists, so its one-second
			// deadline cannot fire: demonstrate the absence of a global wait timeout.
			if (!before_grant) { std::this_thread::sleep_for(std::chrono::milliseconds(1100)); }
			Require(!gate.done.load());
			Require(M::RequestRetirement(gate.token.load()) == 0);
			if (before_grant) { gate.release.store(true); }
		}
		WaitSignal(gate.done); JoinExited(consumer);
		Require(!gate.granted && M::HasLive());
		M::Status status; Require(M::InspectForCurrentOwner(gate.token.load(), status) == 0);
		Require(status.stop_requested && !status.attached);
		if (!retirement) {
			Require(status.attachment_result == static_cast<int>(0x80020000u | I::TimedOut) &&
			        status.worker_error == I::TimedOut); // actual Client::Attach failure
		}
		if (!before_grant) {
			Require(!status.coordinator_done);
			Require(M::Reap(gate.token.load()) == I::WouldBlock); // stalled owner still retained
		}
		gate.release.store(true);
		StopAndReap(gate.token.load()); EndColdGate();
		std::fprintf(stderr, "NP cold consumer: %s returned unavailable\n",
		             before_grant ? "retirement at final grant" : retirement ? "retirement before worker release" : "discovery timeout before worker release");
	}
}
M::Token CloseColdConsumer(Context& context) {
	const char* scenario = std::getenv("KYTY_TEST_NP_CLIENT_WAIT_SCENARIO");
	const bool before_grant = scenario && std::strcmp(scenario, "grant-close") == 0;
	ColdGate gate {context.owner, context.other,
	    before_grant ? M::TestPoint::ConsumerBeforeGrant : M::TestPoint::TransportCreate};
	auto consumer = BeginColdConsumer(gate);
	CheckForeignColdConsumer(gate);
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.epoch) == 0);
	if (before_grant) { gate.release.store(true); }
	WaitSignal(gate.done); JoinExited(consumer);
	Require(!gate.granted && M::HasLive());
	// Cold wait must return even though the transport has not yet resumed. For
	// the grant case the hook is past the first open-epoch snapshot/attachment.
	gate.release.store(true);
	const auto token = gate.token.load();
	WaitAttached(token, K::PthreadGetthreadid());
	Require(!M::AcquireQueryClient()); // genuine late attachment cannot reopen admission
	// Observer no longer blocks, but its context must survive all worker joins.
	StopAndReap(token); EndColdGate();
	std::fprintf(stderr, "NP cold consumer: epoch close at %s rejected grant and drained original workers\n",
	             before_grant ? "final grant" : "pending initialization");
	return token;
}
void* KYTY_SYSV_ABI Controller(void* argument) {
	Live live;
	auto& context = *static_cast<Context*>(argument);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == context.owner);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == context.owner);
	Loader::SourceBoundNpTransportEpoch epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && epoch.live && !epoch.closing);
	Require(!Loader::InstallSourceBoundNpTransport(context.owner)); // duplicate install cannot replace/open the epoch
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && !epoch.closing);
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.epoch + 1) == I::NotFound);
	K::Pthread foreign = nullptr;
	Loader::RuntimeLinker::SetCurrentProgram(context.other);
	Require(K::PthreadCreate(&foreign, nullptr, ForeignCloser, &context, "NpForeignCloser") == 0 && foreign);
	Loader::RuntimeLinker::SetCurrentProgram(context.owner);
	JoinExited(foreign);
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && !epoch.closing && epoch.live);
	ColdFailures(context);
	M::Token first = 0;
	Require(M::StartForCurrentOwner(context.owner, first) == 0 && first && M::HasLive());
	// The first consumer must wait for the independent workers' real method0
	// discovery/open, not require a second application request to see readiness.
	Require(bool(M::AcquireQueryClient()));
	WaitAttached(first, K::PthreadGetthreadid());
	Require(bool(M::AcquireQueryClient())); // attached endpoint selected on actual owner
	M::Token unchanged = UINT64_MAX;
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	Require(M::Reap(first) == I::WouldBlock); // no implicit detach or join
	{
		// Fixture-only negative scope: changing the current program does NOT
		// change the actual pthread owner, and must never authorize a new client.
		Loader::RuntimeLinker::SetCurrentProgram(context.other);
		Require(!M::AcquireQueryClient());
		Require(Loader::CloseSourceBoundNpTransportEpoch(context.epoch) == I::Invalid);
		Require(M::CloseAndReapForCurrentTransportOwner(context.epoch) == I::Invalid);
		Require(M::StartForCurrentOwner(context.other, unchanged) == I::Invalid && unchanged == UINT64_MAX);
		M::Status untouched; untouched.owner_pid = -123;
		Require(M::InspectForCurrentOwner(first, untouched) == I::Invalid && untouched.owner_pid == -123);
		Loader::RuntimeLinker::SetCurrentProgram(context.owner);
	}
	StopAndReap(first);
	M::Token second = 0;
	Require(M::StartForCurrentOwner(context.owner, second) == 0 && second != first);
	// Startup/stop race: may already be connecting. Never cancel native tickets
	// or bypass the service's actual late acceptance/rejection/disconnect turns.
	StopAndReap(second);
	// Earlier per-client retirements must NOT have closed the source transport.
	// Close during a fresh cold consumer or immediately before its final grant.
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && !epoch.closing);
	const auto third = CloseColdConsumer(context);
	Require(third != second);
	Require(!M::AcquireQueryClient()); // a closed epoch cannot admit more exports
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.closing && epoch.live);
	Require(!Loader::InstallSourceBoundNpTransport(context.owner));
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && epoch.closing);
	Require(!Loader::RetireSourceBoundNpTransport()); // native server preserves epoch
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		const int result = M::CloseAndReapForCurrentTransportOwner(context.epoch);
		if (!result) { break; }
		Require((result == I::WouldBlock || result == I::Busy) && std::chrono::steady_clock::now() < deadline);
		K::PthreadYield();
	}
	Require(!M::HasLive());
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && epoch.live && epoch.closing);
	Require(M::CloseAndReapForCurrentTransportOwner(context.epoch) == 0); // idempotent, authorized empty-registry close
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	Require(!Loader::RetireSourceBoundNpTransport()); // real server still live: Uninstall fails, latch stays closed
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && epoch.closing);
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	context.done.store(true); live.armed = false; return nullptr;
}

struct AdmissionRace {
	Loader::Program* owner;
	uint64_t epoch;
	std::atomic_uint ready {0};
	std::atomic_bool go {false};
	// Each contender writes only its own results. Controller reads after actual
	// native completion + join, not just after entry-function publication.
	int start_result = -1, close_result = -1;
	int starter_tid = -1, closer_tid = -1;
	M::Token token = UINT64_MAX;
};
void RaceBarrier(AdmissionRace& race) {
	Require(Loader::RuntimeLinker::GetCurrentProgram() == race.owner);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == race.owner);
	race.ready.fetch_add(1);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!race.go.load()) {
		Require(std::chrono::steady_clock::now() < deadline); K::PthreadYield();
	}
}
void* KYTY_SYSV_ABI RaceStart(void* argument) {
	Live live;
	auto& race = *static_cast<AdmissionRace*>(argument);
	race.starter_tid = K::PthreadGetthreadid();
	RaceBarrier(race);
	race.start_result = M::StartForCurrentOwner(race.owner, race.token);
	live.armed = false; return nullptr;
}
void* KYTY_SYSV_ABI RaceClose(void* argument) {
	Live live;
	auto& race = *static_cast<AdmissionRace*>(argument);
	race.closer_tid = K::PthreadGetthreadid();
	RaceBarrier(race);
	race.close_result = Loader::CloseSourceBoundNpTransportEpoch(race.epoch);
	live.armed = false; return nullptr;
}

struct ReplacementContext {
	Loader::Program* owner;
	uint64_t retired_epoch;
	uint64_t replacement_epoch = 0;
	std::atomic_bool done {false};
};
void* KYTY_SYSV_ABI ReplacementController(void* argument) {
	Live live;
	auto& context = *static_cast<ReplacementContext*>(argument);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == context.owner);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == context.owner);
	Require(!M::HasLive());
	Require(Loader::InstallSourceBoundNpTransport(context.owner));
	Loader::SourceBoundNpTransportEpoch replacement;
	Require(Loader::InspectSourceBoundNpTransportEpoch(replacement) == 0 && replacement.live && !replacement.closing);
	Require(replacement.generation > context.retired_epoch);
	context.replacement_epoch = replacement.generation;
	// There is deliberately no server yet. These are epoch-only operations:
	// stale teardown must neither close the replacement nor create a connection.
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.retired_epoch) == I::NotFound);
	Require(M::CloseAndReapForCurrentTransportOwner(context.retired_epoch) == I::NotFound);
	Require(Loader::InspectSourceBoundNpTransportEpoch(replacement) == 0 &&
	        replacement.generation == context.replacement_epoch && replacement.live && !replacement.closing && !M::HasLive());
	Require(!Loader::InstallSourceBoundNpTransport(context.owner));
	Require(Loader::InspectSourceBoundNpTransportEpoch(replacement) == 0 &&
	        replacement.generation == context.replacement_epoch && !replacement.closing);
	// No Start and no native init/stop/term in this replacement epoch. Prove that
	// the actual Core owner can close and uninstall the empty replacement only.
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.replacement_epoch) == 0);
	Require(Loader::InspectSourceBoundNpTransportEpoch(replacement) == 0 &&
	        replacement.generation == context.replacement_epoch && replacement.live && replacement.closing);
	Require(M::CloseAndReapForCurrentTransportOwner(context.replacement_epoch) == 0 && !M::HasLive());
	Require(Loader::RetireSourceBoundNpTransport());
	Loader::SourceBoundNpTransportEpoch untouched; untouched.generation = context.replacement_epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(untouched) == I::NotFound && untouched.generation == context.replacement_epoch);
	Require(Loader::CloseSourceBoundNpTransportEpoch(context.replacement_epoch) == I::NotFound);
	Require(!M::HasLive());
	context.done.store(true); live.armed = false; return nullptr;
}

void* KYTY_SYSV_ABI RaceController(void* argument) {
	Live live;
	auto& context = *static_cast<Context*>(argument);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == context.owner);
	Require(K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == context.owner);
	Require(!M::HasLive());
	Loader::SourceBoundNpTransportEpoch epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 &&
	        epoch.generation == context.epoch && epoch.live && !epoch.closing);
	// The parent has initialized its real native service exactly once and keeps
	// its dispatcher alive until this controller and all client workers join.
	AdmissionRace race {context.owner, context.epoch};
	K::Pthread starter = nullptr, closer = nullptr;
	Require(K::PthreadCreate(&starter, nullptr, RaceStart, &race, "NpAdmissionStart") == 0 && starter);
	Require(K::PthreadCreate(&closer, nullptr, RaceClose, &race, "NpAdmissionClose") == 0 && closer);
	const auto barrier_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (race.ready.load() != 2) {
		Require(std::chrono::steady_clock::now() < barrier_deadline); K::PthreadYield();
	}
	race.go.store(true);
	JoinExited(starter); JoinExited(closer);
	Require(race.starter_tid > 0 && race.closer_tid > 0 && race.starter_tid != race.closer_tid);
	Require(race.close_result == 0);
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 &&
	        epoch.generation == context.epoch && epoch.live && epoch.closing);
	if (race.start_result == 0) {
		// Start won reservation. Closing must not cancel it or invalidate its
		// owner: prove real attachment, then drain/join its original workers.
		Require(race.token != 0 && race.token != UINT64_MAX && M::HasLive());
		WaitAttached(race.token, K::PthreadGetthreadid());
		M::Status status;
		Require(M::InspectForCurrentOwner(race.token, status) == 0 && status.transport_epoch == context.epoch);
		Require(status.transport_tid != race.starter_tid && status.transport_tid != race.closer_tid);
		Require(status.delivery_tid != race.starter_tid && status.delivery_tid != race.closer_tid);
	} else {
		// Close won admission. No worker registry or changed output is permitted.
		Require(race.start_result == I::Busy && race.token == UINT64_MAX && !M::HasLive());
	}
	std::fprintf(stderr, "NP admission race: %s, epoch=%llu\n", race.start_result == 0 ? "Start won" : "Close won",
	             static_cast<unsigned long long>(context.epoch));
	const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		const int result = M::CloseAndReapForCurrentTransportOwner(context.epoch);
		if (!result) { break; }
		Require((result == I::WouldBlock || result == I::Busy) && std::chrono::steady_clock::now() < drain_deadline);
		K::PthreadYield();
	}
	Require(!M::HasLive());
	if (race.start_result == 0) { Require(M::Reap(race.token) == I::NotFound); }
	M::Token unchanged = UINT64_MAX;
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.closing && epoch.live);
	Require(!Loader::RetireSourceBoundNpTransport()); // actual server still owns runtime resources
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation == context.epoch && epoch.closing);
	Require(M::StartForCurrentOwner(context.owner, unchanged) == I::Busy && unchanged == UINT64_MAX);
	Require(M::CloseAndReapForCurrentTransportOwner(context.epoch) == 0);
	Require(!M::HasLive());
	context.done.store(true); live.armed = false; return nullptr;
}
} // namespace RuntimeNpManagerClientTest

uint64_t TestRuntimeNpManagerClientAttached(Loader::Program* owner, Loader::Program* other_program, bool race = false) {
	using namespace RuntimeNpManagerClientTest;
	Live live;
#if defined(_WIN32)
	Require(owner && other_program && owner != other_program && !M::HasLive());
	Require(Loader::RuntimeLinker::GetCurrentProgram() == owner);
	Loader::SourceBoundNpTransportEpoch epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 && epoch.generation && epoch.live && !epoch.closing);
	// The native-service fixture's launcher was initialized before the images:
	// merely selecting Core on that launcher cannot manufacture Core ownership.
	auto self = K::PthreadSelfOrNull();
	if (!self || K::PthreadGetOwnerProgram(self) != owner) {
		M::Token untouched = UINT64_MAX;
		Require(M::StartForCurrentOwner(owner, untouched) == I::Invalid && untouched == UINT64_MAX && !M::HasLive());
		Require(Loader::CloseSourceBoundNpTransportEpoch(epoch.generation) == I::Invalid);
		Require(M::CloseAndReapForCurrentTransportOwner(epoch.generation) == I::Invalid);
	}
	Context context {owner, other_program, epoch.generation};
	K::Pthread controller = nullptr;
	Require(K::PthreadCreate(&controller, nullptr, race ? RaceController : Controller, &context, "NpManagerFixture") == 0 && controller);
	// Do not treat a C++ entry-return milestone as native termination. Keep the
	// stack Context and mapped native service alive through the actual OS exit.
	JoinExited(controller);
	Require(context.done.load() && !M::HasLive());
	live.armed = false;
	return epoch.generation;
#else
	Require(false);
	return 0;
#endif
}

void TestRuntimeNpManagerClientEpochRetired(uint64_t retired_epoch, Loader::Program* owner) {
	using namespace RuntimeNpManagerClientTest;
	Live live;
	Require(retired_epoch != 0 && !M::HasLive());
	Loader::SourceBoundNpTransportEpoch untouched; untouched.generation = retired_epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(untouched) == I::NotFound && untouched.generation == retired_epoch);
	Require(Loader::CloseSourceBoundNpTransportEpoch(retired_epoch) == I::NotFound);
	Require(owner);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == owner);
	ReplacementContext context {owner, retired_epoch};
	K::Pthread controller = nullptr;
	Require(K::PthreadCreate(&controller, nullptr, ReplacementController, &context, "NpReplacementEpoch") == 0 && controller);
	JoinExited(controller);
	Require(context.done.load() && context.replacement_epoch > retired_epoch && !M::HasLive());
	live.armed = false;
}
