#include "loader/runtimeNpManagerClient.h"
#include "loader/runtimeNpTransport.h"

#include "loader/runtimeLinker.h"
#include "common/singleton.h"
#include "loader/runtimeProcessIdentity.h"
#include "libs/npManagerClient.h"
#include "libs/npNativeManagerEndpoint.h"
#include "kernel/eventFlag.h"
#include "kernel/pthread.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "libs/runtimeUserService.h"
#include "libs/errno.h"
#include "kernel/fileSystem.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace Loader::RuntimeNpManagerClient {
namespace {
namespace K = Libs::LibKernel;
namespace I = K::Ipmi;
namespace E = Libs::NpNativeManagerEndpoint;
namespace N = Libs::NpManagerClient;
namespace F = K::EventFlag;
int Bsd(int result) { return result < 0 ? int(uint32_t(result) & 0xffffu) : result; }

struct Owner {
	Program* program = nullptr;
	int pid = -1;
	std::shared_ptr<BootCredentialLifetime> lifetime;
	std::shared_ptr<BootCredentialAdmission> admission;
	bool Live() const {
		// The registry/parent mapping gate protects program. The token must still
		// be the admitted incarnation, not merely some live saved credentials.
		return lifetime && program->boot_credential_lifetime == lifetime &&
		       program->boot_credential_admission == admission && admission &&
		       admission->IsLiveFor(program) && IsGuestProcessIdentityActive(pid);
	}
	bool Current() const {
		auto self = K::PthreadSelfOrNull(); int current = -1;
		return self && K::PthreadGetOwnerProgram(self) == program &&
		       RuntimeLinker::GetCurrentProgram() == program &&
		       TryGetCurrentRetainedGuestProcessId(&current) && current == pid && Live();
	}
};

struct Run {
	Token token = 0;
	uint64_t transport_epoch = 0;
	std::shared_ptr<Owner> owner;
	std::atomic_bool starting {true}, stop {false}, owner_lost {false}, attached {false};
	std::atomic_bool transport_returning {false}, delivery_returning {false}, reaping {false};
	std::atomic_bool coordinator_done {false};
	std::atomic_bool delivery_detached {false};
	std::atomic_int transport_tid {-1}, delivery_tid {-1}, error {0}, attach_result {N::NotInitialized};
	std::atomic<std::shared_ptr<E::Coordinator>> coordinator;
	std::atomic<std::shared_ptr<N::Endpoint>> endpoint;
	// Attach/Detach run on delivery; Client serializes queries against teardown.
	// Export leases retain this Run and the loader-clear gate through each query.
	N::Client client;
	K::Pthread transport = nullptr, delivery = nullptr;
	bool transport_joined = false, delivery_joined = false; // exclusive reaper
	bool creation_uncertain = false; // published with starting=false
	std::mutex grant_mutex; // stop vs final grant; never held over native work/joins
	void Stop() {
		std::lock_guard admission(grant_mutex);
		stop.store(true); attached.store(false);
		if (auto c = coordinator.load()) { c->RequestRetirement(); }
	}
	void Fail(int e) {
		int expected = 0; error.compare_exchange_strong(expected, e); Stop();
	}
	void ObserveCoordinatorProgress(int result, const std::shared_ptr<E::Coordinator>& c) {
		// Advance's return describes native retirement progress, not attachment
		// viability. A timed-out connecting ticket may still require its original
		// owner to drain Busy/WouldBlock; do not leave consumers awaiting attached
		// forever merely because that cleanup has not finished. Keep the Run and
		// both workers retained; the transport loop still advances until Done.
		const int terminal = c->RetirementError();
		if (terminal && !stop.load()) { Fail(terminal); }
		if (result && result != I::WouldBlock && result != I::Busy) { Fail(result); }
	}
};

// Fixed capacity one: admission includes starting and retiring incarnations.
// No locks are held across thread creation, Attach, Detach, native APIs or joins.
std::atomic<std::shared_ptr<Run>> selected;
std::atomic_uint64_t next_token {1};
std::atomic_uint api_calls {0};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
std::atomic<TestObserver> test_observer {nullptr};
void Observe(TestPoint point, Token token, size_t queued = 0) {
	if (const auto observer = test_observer.load()) { observer(point, token, queued); }
}
#endif
struct ApiCall {
	ApiCall() { api_calls.fetch_add(1); }
	~ApiCall() { api_calls.fetch_sub(1); }
};

void Pause() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
bool Enter(const std::shared_ptr<Run>& run, std::atomic_int& tid) {
	if (!run->owner->Current()) { run->owner_lost.store(true); run->Fail(I::Invalid); return false; }
	const int pin = K::PthreadRetainCurrentUntilJoin(run);
	if (pin) { run->Fail(pin); return false; }
	tid.store(K::PthreadGetthreadid()); return true;
}

E::Hooks MakeHooks(const std::shared_ptr<Run>& run) {
	E::Hooks hooks; hooks.owner = run->owner;
	hooks.identity = [] {
		int pid = -1;
		if (!K::PthreadSelfOrNull() || !TryGetCurrentRetainedGuestProcessId(&pid)) { return I::Identity {-1, -1}; }
		return I::Identity {pid, K::PthreadGetthreadid()};
	};
	hooks.owner_alive = [owner = run->owner] { return owner->Live(); };
	hooks.retain = [weak = std::weak_ptr<Run>(run)](std::shared_ptr<E::Coordinator> c) {
		auto r = weak.lock(); if (!r) { return false; }
		std::shared_ptr<E::Coordinator> empty;
		return r->coordinator.compare_exchange_strong(empty, std::move(c));
	};
	hooks.open_flag = [](const char* name, uintptr_t& alias) {
		F::KernelEventFlag flag = nullptr;
		const int result = F::KernelOpenEventFlag(&flag, name);
		alias = reinterpret_cast<uintptr_t>(flag); return result;
	};
	hooks.poll_flag = [](uintptr_t alias, uint32_t channel) {
		uint64_t observed = 0;
		const int result = F::KernelPollEventFlag(reinterpret_cast<F::KernelEventFlag>(alias),
		    uint64_t {1} << channel, 0x22, &observed);
		return result == static_cast<int>(0x80020010u) ? 1 : result;
	};
	hooks.close_flag = [](uintptr_t alias) { return F::KernelCloseEventFlag(reinterpret_cast<F::KernelEventFlag>(alias)); };
	hooks.create_request_flag = [](const char* name, uintptr_t& creator) {
		F::KernelEventFlag flag = nullptr;
		const int result = F::KernelCreateEventFlag(&flag, name, 0x121, 0, nullptr);
		creator = reinterpret_cast<uintptr_t>(flag); return result;
	};
	hooks.clear_request_flag = [](uintptr_t creator, uint64_t keep) {
		return F::KernelClearEventFlag(reinterpret_cast<F::KernelEventFlag>(creator), keep);
	};
	hooks.poll_request_flag = [](uintptr_t creator, uint64_t mask) {
		uint64_t observed = 0;
		const int result = F::KernelPollEventFlag(reinterpret_cast<F::KernelEventFlag>(creator), mask, 0x21, &observed);
		return result == static_cast<int>(0x80020010u) ? 1 : result;
	};
	hooks.delete_request_flag = [](uintptr_t creator) { return F::KernelDeleteEventFlag(reinterpret_cast<F::KernelEventFlag>(creator)); };
	return hooks;
}

void* KYTY_SYSV_ABI TransportEntry(void* argument) {
	// Registry publication precedes pthread creation; it owns the raw argument
	// even if either creation fails or the other worker has already returned.
	auto run = selected.load();
	if (!run || run.get() != argument) { std::terminate(); }
	try {
		if (Enter(run, run->transport_tid) && !run->stop.load()) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			Observe(TestPoint::TransportCreate, run->token);
#endif
			std::shared_ptr<N::Endpoint> endpoint; std::shared_ptr<E::Coordinator> coordinator;
			const int created = E::Create(MakeHooks(run), endpoint, coordinator, std::chrono::milliseconds(1000));
			if (created) { run->Fail(created); }
			else {
				run->endpoint.store(endpoint);
				for (;;) {
					if (run->stop.load()) { coordinator->RequestRetirement(); }
					if (!run->owner->Current()) {
						run->owner_lost.store(true); run->Fail(I::NotConnected);
						// No impersonation, no dropping a pending native token. Parent
						// retains this unresolved owner, mappings and worker indefinitely.
						Pause(); continue;
					}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
					Observe(TestPoint::TransportAdvance, run->token, coordinator->QueuedJobs());
#endif
					const int result = coordinator->Advance();
					run->ObserveCoordinatorProgress(result, coordinator);
					if (coordinator->Done()) { run->Stop(); break; }
					Pause();
				}
			}
		}
	} catch (...) {
		run->Fail(I::NoMemory);
		// A published coordinator must still be driven on THIS actual owner.
		if (auto c = run->coordinator.load()) {
			while (!c->Done()) {
				if (run->owner->Current()) { c->Advance(); }
				else { run->owner_lost.store(true); }
				Pause();
			}
		}
	}
	if (auto c = run->coordinator.load()) { run->coordinator_done.store(c->Done()); }
	else { run->coordinator_done.store(true); }
	run->transport_returning.store(true); // not proof of native thread termination
	return nullptr;
}

void* KYTY_SYSV_ABI DeliveryEntry(void* argument) {
	auto run = selected.load();
	if (!run || run.get() != argument) { std::terminate(); }
	try {
		if (Enter(run, run->delivery_tid)) {
			while (!run->stop.load()) {
				if (!run->owner->Current()) { run->owner_lost.store(true); run->Fail(I::NotConnected); break; }
				auto endpoint = run->endpoint.load();
				if (!endpoint || !endpoint->Connected()) { Pause(); continue; }
				const int result = run->client.Attach(endpoint);
				run->attach_result.store(result);
				if (result) { run->Fail(Bsd(result)); break; }
				run->attached.store(true);
				while (!run->stop.load()) {
					if (!run->owner->Current()) { run->owner_lost.store(true); run->Fail(I::NotConnected); break; }
					Pause(); // NO callbacks or synthetic subscriptions
				}
				break;
			}
		}
	} catch (...) { run->Fail(I::NoMemory); }
	run->attached.store(false);
	try {
		run->client.Detach(); // transport remains independently available until Done
		run->delivery_detached.store(true);
	} catch (...) { run->Fail(I::NoMemory); } // preserve registry on uncertain detach
	run->delivery_returning.store(true);
	return nullptr;
}

bool NativeExited(K::Pthread thread) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	K::PthreadLegacyNativeTarget target;
	if (K::PthreadPinLegacyNativeTarget(thread, &target) != 0) { return false; }
	target.UnlockRecord();
	return target.Handle() && WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 0) == WAIT_OBJECT_0;
#else
	return false;
#endif
}
} // namespace

std::optional<int> QueryLocalSigninState(int user, uint32_t* output) {
	const auto& environment = Config::GetGuestEnvironment();
	const auto selection = environment.find("KYTY_NP_LOCAL_ACCOUNT_HLE");
	if (selection == environment.end() || selection->second != "1") { return std::nullopt; }
	int pid = -1;
	if (!K::PthreadSelfOrNull() || !TryGetCurrentRetainedGuestProcessId(&pid) ||
	    !IsGuestProcessIdentityActive(pid)) { return N::NotInitialized; }
	if (!output) { return N::InvalidArgument; }
	uint64_t account = 0;
	const int result = Libs::UserService::QueryLocalAccountId(user, &account);
	if (result == Libs::UserService::USER_SERVICE_ERROR_NOT_LOGGED_IN) { return N::UserNotFound; }
	if (result == Libs::UserService::USER_SERVICE_ERROR_NOT_INITIALIZED) { return N::NotInitialized; }
	if (result) { return N::TransportIoError; }
	const auto state = static_cast<uint32_t>(account ? N::SigninState::SignedOut : N::SigninState::NotSignedUp);
	if (!K::FileSystem::CopyPathResultToGuest(output, &state, sizeof(state))) { return N::InvalidArgument; }
	static std::atomic_uint reports {0};
	if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
		LOGF("NP local-account HLE: pid=%d user=0x%x state=%u (local profile, no online session)\n", pid, user, state);
	}
	return 0;
}

int StartForCurrentOwner(Program* program, Token& output) noexcept {
	ApiCall api;
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return I::Unsupported;
#else
	try {
		auto* linker = Common::Singleton<RuntimeLinker>::Instance();
		std::shared_ptr<Run> run;
		{
			Common::LockGuard admission(linker->m_mutex);
			if (std::find(linker->m_programs.begin(), linker->m_programs.end(), program) == linker->m_programs.end()) { return I::Invalid; }
			auto self = K::PthreadSelfOrNull(); int pid = -1;
			if (!program || !self || K::PthreadGetOwnerProgram(self) != program ||
			    RuntimeLinker::GetCurrentProgram() != program || program->application_owner ||
			    !TryGetCurrentRetainedGuestProcessId(&pid) || !program->boot_credential_lifetime ||
			    !program->boot_credential_admission || !program->boot_credential_admission->IsLiveFor(program)) { return I::Invalid; }
			std::array<uint64_t, 17> auth {};
			if (!TryGetSourceBoundProcessAuthInfo(pid, &auth)) { return I::Invalid; }
			SourceBoundNpTransportEpoch epoch;
			const int epoch_result = InspectSourceBoundNpTransportEpoch(epoch);
			if (epoch_result) { return epoch_result; }
			if (epoch.closing) { return I::Busy; }
			if (!epoch.live) { return I::NotConnected; }
			run = std::make_shared<Run>();
			run->transport_epoch = epoch.generation;
			run->owner = std::make_shared<Owner>(Owner {program, pid, program->boot_credential_lifetime, program->boot_credential_admission});
			if (!run->owner->Current()) { return I::Invalid; }
			Token token = next_token.load();
			do { if (!token || token == UINT64_MAX) { return I::Busy; } }
			while (!next_token.compare_exchange_weak(token, token + 1));
			run->token = token;
			std::shared_ptr<Run> empty;
			if (!selected.compare_exchange_strong(empty, run)) { return I::Busy; }
			// After reservation, even a creation failure is a retained run with an
			// output token. Reap owns rollback; no live pthread unwinds off this stack.
			output = token;
		} // Registry reservation now prevents Clear; never hold the loader lock over thread creation.
		int result = 0;
		try {
			result = K::PthreadCreate(&run->transport, nullptr, TransportEntry, run.get(), "NpManagerTransport");
			if (!result) { result = K::PthreadCreate(&run->delivery, nullptr, DeliveryEntry, run.get(), "NpManagerAttach"); }
		} catch (...) { run->creation_uncertain = true; result = static_cast<int>(0x8002000cu); }
		if (result) { run->Fail(Bsd(result)); }
		run->starting.store(false);
		// Accepted ownership, not attached or service-ready. Inspect reports any
		// asynchronous/startup failure; caller must request retirement and reap.
		return 0;
	} catch (...) { return I::NoMemory; }
#endif
}

int InspectForCurrentOwner(Token token, Status& output) noexcept {
	ApiCall api;
	auto run = selected.load(); if (!run || run->token != token) { return I::NotFound; }
	if (!run->owner->Current()) { return I::Invalid; }
	Status status;
	status.transport_epoch = run->transport_epoch;
	status.owner_pid = run->owner->pid;
	status.transport_tid = run->transport_tid.load(); status.delivery_tid = run->delivery_tid.load();
	status.stop_requested = run->stop.load(); status.owner_lost = run->owner_lost.load();
	status.attached = run->attached.load() && !status.stop_requested;
	status.transport_returning = run->transport_returning.load(); status.delivery_returning = run->delivery_returning.load();
	status.attachment_result = run->attach_result.load(); status.worker_error = run->error.load();
	status.coordinator_done = run->coordinator_done.load();
	output = status; return 0;
}

std::shared_ptr<N::Client> AcquireQueryClient() noexcept {
	ApiCall api;
	if (!K::PthreadSelfOrNull() || !RuntimeLinker::GetCurrentProgram()) { return {}; }
	try {
		auto run = selected.load();
		if (run && run->stop.load() && run->error.load() && run->owner->Current()) {
			// Failed startup is not a permanent global latch. A later query may
			// retry only AFTER both original workers really exited and joined.
			// This call still reports unavailable; it never waits for attachment.
			Reap(run->token);
			return {};
		}
		if (!run) {
			Token token = 0;
			// Start validates loader membership, real pthread ownership, source
			// credentials and the OPEN epoch before publishing any worker.
			const int started = StartForCurrentOwner(RuntimeLinker::GetCurrentProgram(), token);
			// Another actual thread of this owner may have won lazy publication.
			// Revalidate that run below; Busy is never a grant to a foreign owner.
			if (started != 0 && started != I::Busy) { return {}; }
			run = selected.load();
		}
		// Native NpManager +c930 -> +c9c0/+15190/+13c10 performs lazy
		// connect/discovery before returning to the FIRST request. Returning null
		// merely because our independently-owned workers are starting leaks an
		// artificial NOT_INITIALIZED into LoginMgr's NpRequest constructor.
		// Wait for their real result, never run the dispatcher on this caller and
		// never hold the loader lock. Transport jobs retain their own bounded host
		// deadlines; stop/owner/epoch loss interrupts attachment here as well.
		for (;;) {
			if (!run || run->stop.load() || !run->owner->Current()) { return {}; }
			SourceBoundNpTransportEpoch epoch;
			if (InspectSourceBoundNpTransportEpoch(epoch) != 0 || !epoch.live || epoch.closing ||
			    epoch.generation != run->transport_epoch) { return {}; }
			if (run->attached.load()) { break; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			Observe(TestPoint::ConsumerWaiting, run->token);
#endif
			Pause();
		}
		struct Lease {
			ApiCall admission;
			std::shared_ptr<Run> run;
			explicit Lease(std::shared_ptr<Run> r): run(std::move(r)) {}
		};
		auto lease = std::make_shared<Lease>(run);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		Observe(TestPoint::ConsumerBeforeGrant, run->token);
#endif
		// Epoch close and the final grant share the loader admission lock. A close
		// between the waiting-loop snapshot and lease allocation must not grant a
		// client from the closed epoch. A grant linearized before close is an
		// already-admitted query; its retained lease remains valid through drain.
		Common::LockGuard admission(Common::Singleton<RuntimeLinker>::Instance()->m_mutex);
		std::lock_guard retirement(run->grant_mutex);
		SourceBoundNpTransportEpoch epoch;
		if (InspectSourceBoundNpTransportEpoch(epoch) != 0 || !epoch.live || epoch.closing ||
		    epoch.generation != run->transport_epoch || run->stop.load() || !run->attached.load() ||
		    !run->owner->Current()) { return {}; }
		return std::shared_ptr<N::Client>(std::move(lease), &run->client);
	} catch (...) { return {}; }
}

int RequestRetirement(Token token) noexcept {
	ApiCall api;
	auto run = selected.load(); if (!run || run->token != token) { return I::NotFound; }
	run->Stop(); return 0;
}

int CloseAndReapForCurrentTransportOwner(uint64_t expected_epoch) noexcept {
	// Keeps RetireSourceBound's HasLive gate raised across close, registry
	// observation and the unlocked reap pass. No loader lock is held over joins.
	ApiCall api;
	const int closed = CloseSourceBoundNpTransportEpoch(expected_epoch);
	if (closed) { return closed; }
	auto run = selected.load();
	if (!run) { return 0; }
	if (run->transport_epoch != expected_epoch) { return I::NotFound; }
	run->Stop();
	const int result = Reap(run->token);
	// Another authorized reaper can finish after our snapshot. Closed admission
	// prevents a new run for this epoch, so absent selection is real quiescence.
	return result == I::NotFound && !selected.load() ? 0 : result;
}

int Reap(Token token) noexcept {
	ApiCall api;
	auto run = selected.load(); if (!run || run->token != token) { return I::NotFound; }
	if (run->starting.load()) { return I::WouldBlock; }
	if (run->creation_uncertain) { return I::WouldBlock; }
	bool expected = false;
	if (!run->reaping.compare_exchange_strong(expected, true)) { return I::Busy; }
	struct Release { Run& r; ~Release() { r.reaping.store(false); } } release {*run};
	if (!run->stop.load()) { return I::WouldBlock; }
	if ((run->transport && !run->transport_returning.load()) ||
	    (run->delivery && !run->delivery_returning.load())) { return I::WouldBlock; }
	if (run->delivery && !run->delivery_detached.load()) { return I::WouldBlock; }
	// Both entries are returning before inspecting coordinator state, so its
	// mutex cannot be held by a still-running Advance/Attach operation.
	if (auto c = run->coordinator.load(); c && !c->Done()) { return I::WouldBlock; }
	if ((run->transport && !run->transport_joined && !NativeExited(run->transport)) ||
	    (run->delivery && !run->delivery_joined && !NativeExited(run->delivery))) { return I::WouldBlock; }
	if (run->transport && !run->transport_joined) {
		int result = K::PthreadJoin(run->transport, nullptr); if (result) { return Bsd(result); }
		run->transport_joined = true;
	}
	if (run->delivery && !run->delivery_joined) {
		int result = K::PthreadJoin(run->delivery, nullptr); if (result) { return Bsd(result); }
		run->delivery_joined = true;
	}
	// Destroy endpoint/coordinator before allowing a replacement run to register.
	run->endpoint.store({}); run->coordinator.store({});
	auto same = run;
	if (!selected.compare_exchange_strong(same, {})) { return I::Busy; }
	return 0;
}

bool HasLive() noexcept { return bool(selected.load()) || api_calls.load() != 0; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void SetTestObserver(TestObserver observer) noexcept { test_observer.store(observer); }
#endif
} // namespace Loader::RuntimeNpManagerClient
