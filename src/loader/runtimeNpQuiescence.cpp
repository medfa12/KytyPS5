#include "loader/runtimeNpQuiescence.h"

#include "loader/runtimeNpTransport.h"
#include "loader/runtimeNpManagerClient.h"
#include "loader/runtimeLinker.h"
#include "kernel/pthread.h"
#include "libs/npNativeTransport.h"

#include <atomic>
#include <new>
#include <mutex>

namespace Loader::RuntimeNpQuiescence {
namespace K = Libs::LibKernel;
namespace I = K::Ipmi;
namespace N = Libs::NpNativeTransport;
namespace L = N::Lifecycle;
enum class Stage { Drain, Stop, Exit, NativeExited, Joined, Destroyed, Finished };
class State final {
public:
	std::atomic_bool entered {false};
	uint64_t epoch = 0;
	Program* owner = nullptr; // compared only; server lease retains provider/owner
	K::PthreadNativeReceipt creator;
	K::PthreadNativeSnapshot creator_snapshot;
	L::ServerLease server;
	N::DispatcherReceipt dispatcher;
	Stage stage = Stage::Drain;
};
namespace {
// One installed source-bound transport. Successful generations never reopen,
// even after Finish or loss of every public handle. No callbacks under mutex.
std::mutex registry_mutex;
OwnedState active;
uint64_t last_successful_epoch = 0;
// Logical in-flight reservation, not a held mutex: competing Begin fails Busy.
std::atomic_bool beginning {false};
template<class F> int Safe(F&& f) noexcept {
	try { return f(); } catch (const std::bad_alloc&) { return I::NoMemory; } catch (...) { return I::Invalid; }
}
bool Same(const N::DispatcherReceipt& a, const N::DispatcherReceipt& b) {
	return a.generation == b.generation && a.identity == b.identity && a.token.get() == b.token.get() &&
	       !a.token.owner_before(b.token) && !b.token.owner_before(a.token);
}
int Validate(State& s) {
	auto self = K::PthreadSelfOrNull();
	if (!self || RuntimeLinker::GetCurrentProgram() != s.owner || K::PthreadGetOwnerProgram(self) != s.owner) { return I::Invalid; }
	K::PthreadNativeReceipt actual;
	int error = K::PthreadCaptureCurrentNativeReceipt(actual);
	if (error) { return error; }
	K::PthreadNativeSnapshot now;
	if ((error = K::PthreadInspectNativeReceipt(actual, now))) { return error; }
	if (actual != s.creator || now.generation != s.creator_snapshot.generation || now.pid != s.creator_snapshot.pid ||
	    now.tid != s.creator_snapshot.tid || now.detached_at_creation || now.phase != K::PthreadNativePhase::Pending) { return I::Invalid; }
	SourceBoundNpTransportEpoch epoch;
	if ((error = InspectSourceBoundNpTransportEpoch(epoch))) { return error; }
	if (epoch.generation != s.epoch) { return I::NotFound; }
	if (!epoch.live || epoch.owner_pid != now.pid) { return I::Invalid; }
	// Revalidate actual admitted Core authorization, not a selected Program scope.
	return CloseSourceBoundNpTransportEpoch(s.epoch);
}
template<class F> int Enter(const OwnedState& input, F&& f) {
	const auto pin = input;
	if (!pin) { return I::Invalid; }
	if (pin->entered.exchange(true)) { return I::Busy; }
	struct Release { State& s; ~Release() { s.entered = false; } } release {*pin};
	const int error = Validate(*pin);
	return error ? error : f(*pin);
}
int Dispatcher(State& s, L::DispatcherSnapshot& out) {
	int error = L::InspectDispatcher(s.server, out);
	if (error) { return error; }
	if (out.binding == L::DispatcherBinding::Unbound) { return I::WouldBlock; }
	const auto& r = out.receipt;
	if (!r.token || !r.generation || r.identity.pid != s.creator_snapshot.pid || r.identity.tid <= 0 ||
	    r.identity.tid == s.creator_snapshot.tid) { return I::Invalid; }
	if (s.dispatcher.token && !Same(s.dispatcher, r)) { return I::Invalid; }
	// Source-bound provider rejects detached-at-creation dispatchers at capture.
	if (out.proof.successfully_joined && (out.proof.join_origin != N::DispatcherJoinOrigin::Explicit ||
	    !(out.proof.join_source == I::Identity {s.creator_snapshot.pid, s.creator_snapshot.tid}))) { return I::Invalid; }
	if (!s.dispatcher.token) { s.dispatcher = r; }
	return 0;
}
}
int Begin(uint64_t epoch, uint64_t address, uint64_t handle, OwnedState& output) noexcept {
	return Safe([&] {
		if (output || !epoch || !address || !handle) { return int(I::Invalid); }
		if (beginning.exchange(true)) { return int(I::Busy); }
		struct EndBegin { ~EndBegin() { beginning = false; } } end_begin;
		auto s = std::make_shared<State>();
		s->epoch = epoch; s->owner = RuntimeLinker::GetCurrentProgram();
		auto self = K::PthreadSelfOrNull();
		if (!s->owner || !self || K::PthreadGetOwnerProgram(self) != s->owner) { return int(I::Invalid); }
		int error = K::PthreadCaptureCurrentNativeReceipt(s->creator);
		if (error) { return error; }
		if ((error = K::PthreadInspectNativeReceipt(s->creator, s->creator_snapshot))) { return error; }
		if (!s->creator_snapshot.generation || s->creator_snapshot.pid <= 0 || s->creator_snapshot.tid <= 0 ||
		    s->creator_snapshot.owner_cookie != reinterpret_cast<uintptr_t>(s->owner) || s->creator_snapshot.detached_at_creation ||
		    s->creator_snapshot.phase != K::PthreadNativePhase::Pending) { return int(I::Invalid); }
		SourceBoundNpTransportEpoch installed;
		if ((error = InspectSourceBoundNpTransportEpoch(installed))) { return error; }
		if (installed.generation != epoch) { return int(I::NotFound); }
		if (!installed.live || installed.owner_pid != s->creator_snapshot.pid) { return int(I::Invalid); }
		{
			std::unique_lock lock(registry_mutex, std::try_to_lock);
			if (!lock.owns_lock()) { return int(I::Busy); }
			// Before AcquireServer: a finished generation is Busy even though its
			// original native object has now been destroyed and erased.
			if (active || epoch <= last_successful_epoch) { return int(I::Busy); }
		}
		if ((error = L::AcquireServer(address, handle, s->server))) { return error; }
		if ((error = Validate(*s))) { return error; }
		L::DispatcherSnapshot dispatcher;
		error = Dispatcher(*s, dispatcher);
		if (error && error != I::WouldBlock) { return error; }
		{
			std::unique_lock lock(registry_mutex, std::try_to_lock);
			if (!lock.owns_lock()) { return int(I::Busy); }
			if (active || epoch <= last_successful_epoch) { return int(I::Busy); }
			active = s; // empty destination; no token destruction under registry lock
			last_successful_epoch = epoch;
		}
		output = std::move(s); return 0;
	});
}
int Advance(const OwnedState& state, Progress& output) noexcept {
	return Safe([&] { return Enter(state, [&](State& s) {
		if (s.stage > Stage::NativeExited) { return int(I::Invalid); }
		if (s.stage == Stage::Drain) {
			const int error = RuntimeNpManagerClient::CloseAndReapForCurrentTransportOwner(s.epoch);
			if (error == I::WouldBlock || error == I::Busy) { output = Progress::Pending; return 0; }
			if (error) { return error; }
			s.stage = Stage::Stop;
		}
		if (s.stage == Stage::Stop) {
			L::StopResult stop;
			const int error = L::RequestStop(s.server, stop);
			if (error) { return error; }
			if (stop.stop_errno && !(stop.stop_errno == I::Busy && stop.snapshot.stop_succeeded)) { return stop.stop_errno; }
			if (!stop.snapshot.stop_succeeded) { return int(I::BadMessage); }
			s.stage = Stage::Exit;
		}
		L::DispatcherSnapshot dispatcher;
		const int error = Dispatcher(s, dispatcher);
		if (error == I::WouldBlock) { output = Progress::Pending; return 0; }
		if (error) { return error; }
		if (dispatcher.proof.native_exited) { s.stage = Stage::NativeExited; }
		output = s.stage == Stage::NativeExited ? Progress::NativeExited : Progress::Pending;
		return 0;
	}); });
}
int ObserveJoin(const OwnedState& state, int actual_result) noexcept {
	return Safe([&] { return Enter(state, [&](State& s) {
		if (actual_result) { return actual_result; }
		if (s.stage != Stage::NativeExited) { return int(I::Invalid); }
		L::DispatcherSnapshot dispatcher;
		const int error = Dispatcher(s, dispatcher);
		if (error) { return error; }
		if (!dispatcher.proof.native_exited || !dispatcher.proof.successfully_joined) { return int(I::WouldBlock); }
		s.stage = Stage::Joined; return 0;
	}); });
}
int ObserveDestroy(const OwnedState& state, int actual_result) noexcept {
	return Safe([&] { return Enter(state, [&](State& s) {
		if (actual_result) { return actual_result; }
		if (s.stage != Stage::Joined) { return int(I::Invalid); }
		L::DispatcherSnapshot dispatcher;
		int error = Dispatcher(s, dispatcher);
		if (error) { return error; }
		if (!dispatcher.proof.successfully_joined) { return int(I::WouldBlock); }
		L::ServerSnapshot server;
		if ((error = L::InspectServer(s.server, server))) { return error; }
		if (!server.destroyed || server.published || server.dispatching || !server.stop_succeeded) { return int(I::WouldBlock); }
		s.stage = Stage::Destroyed; return 0;
	}); });
}
int Finish(OwnedState& state) noexcept {
	return Safe([&] {
		OwnedState released; // release registry's strong reference OUTSIDE locks
		const int error = Enter(state, [&](State& s) {
			if (s.stage != Stage::Destroyed) { return int(I::Invalid); }
			std::unique_lock lock(registry_mutex, std::try_to_lock);
			if (!lock.owns_lock()) { return int(I::Busy); }
			if (active.get() != &s || last_successful_epoch != s.epoch) { return int(I::Invalid); }
			released = std::move(active);
			s.stage = Stage::Finished; return 0;
		});
		if (!error) { state.reset(); }
		return error;
	});
}
}
