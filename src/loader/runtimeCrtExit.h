// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_RUNTIME_CRT_EXIT_H_
#define KYTY_LOADER_RUNTIME_CRT_EXIT_H_

#include "loader/runtimeCrtSync.h"
#include "common/logging/log.h"

#include <list>

namespace Loader::RuntimeCrt {
enum class ExitKind { Atexit, Cxa };
struct ExitRecord {
	ExitKind kind;
	uint64_t target;
	void* argument;
	void* dso;
};
struct ExitState {
	std::shared_ptr<RuntimeGuestExecution> execution;
	std::list<ExitRecord> callbacks;
	std::weak_ptr<const unsigned char> draining_thread;
	unsigned depth = 0;
	bool exiting = false;
	bool exited = false;
	bool main_returned = false;
	int main_status = 0;
	std::unique_ptr<HostCallReturnThunk> entry_exit;
	unsigned entry_notifications = 0;
};
struct ExitKey {
	const void* identity;
	uint64_t token;
	bool operator<(const ExitKey& other) const {
		return std::less<const void*> {}(identity, other.identity) || (identity == other.identity && token < other.token);
	}
};
inline ExitKey Key(const Context& context) { const auto life = context.Lifetime(); return {life.identity, life.token}; }
struct ExitRegistry {
	GuestExecutionMutex mutex;
	std::condition_variable_any changed;
	std::map<ExitKey, ExitState> states;
};
inline ExitRegistry& Exits() { static auto* registry = new ExitRegistry; return *registry; }

inline void KYTY_SYSV_ABI EntryExitNotification() {
	const Context context;
	if (!context.Ready() || !context.Shared()) { return; }
	auto& registry = Exits();
	bool first = false;
	{
		std::lock_guard lock(registry.mutex);
		const auto it = registry.states.find(Key(context));
		if (it == registry.states.end() || !it->second.entry_exit) { context.Reject(); return; }
		// Native private libc may consume this callback from ITS own atexit list,
		// without entering the HLE drain. Notification never implies owner stop.
		if (!it->second.entry_notifications) { ++it->second.entry_notifications; first = true; }
	}
	if (first) {
		LOGF("CRT: supplied entry exit callback owner=%d lifetime=%llu (notification only)\n", context.owner->unique_id,
		     static_cast<unsigned long long>(context.execution->Owner().token));
	}
}

// HOST ENTRY SETUP ONLY. Parent must pass this exact lifetime-owned generated
// gateway to InvokeEntry INSTEAD OF the old raw ApplicationExitHandler pointer.
// The registry pins both context and code; this is one audited capability, not
// admission of arbitrary executable allocations or host callbacks.
inline uint64_t PrepareEntryExitCallback(const std::shared_ptr<RuntimeGuestExecution>& execution) try {
	if (!execution || execution->StopRequested()) { return 0; }
	auto lease = execution->TryAcquire(GuestWorkKind::Module);
	if (!lease) { return 0; }
	auto& registry = Exits();
	std::lock_guard lock(registry.mutex);
	const auto owner = execution->Owner();
	auto& state = registry.states[{owner.identity, owner.token}];
	if (state.exited) { return 0; }
	if (!state.entry_exit) {
		auto prepared = HostCallReturnThunk::Create(execution->Boundary(), reinterpret_cast<uint64_t>(EntryExitNotification),
		    execution.get(), RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall);
		if (!prepared || !lease.Publish([&] { state.entry_exit = std::move(prepared); state.execution = execution; })) { return 0; }
	} else if (!lease.Publish([] {})) {
		return 0;
	}
	return state.entry_exit->GetAddress();
} catch (...) { return 0; }

inline int RegisterExit(ExitKind kind, uint64_t target, void* argument = nullptr, void* dso = nullptr) try {
	const Context context;
	if (!context.Ready()) { return -1; }
	if (!target) { return 0; } // Retain legacy null-registration behavior.
	bool entry_callback = false;
	if (context.Shared()) {
		auto& registry = Exits();
		std::lock_guard lock(registry.mutex);
		const auto it = registry.states.find(Key(context));
		entry_callback = kind == ExitKind::Atexit && it != registry.states.end() && it->second.entry_exit &&
		    target == it->second.entry_exit->GetAddress();
	}
	if (!entry_callback && !PrivateCallback(context, target)) { return -1; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::Callback) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return -1; }
	std::list<ExitRecord> prepared;
	prepared.push_back({kind, target, argument, dso}); // Allocate before the publication commit.
	auto& registry = Exits();
	std::lock_guard lock(registry.mutex);
	auto& state = registry.states[Key(context)];
	// Reentrant registration by the ordinary finalizer is permitted. A peer
	// cannot extend an already exiting process indefinitely or cross lifetimes.
	if (state.exited || (state.exiting && state.draining_thread.lock() != ActualThread())) { return -1; }
	const auto publish = [&] {
		state.execution = context.execution;
		state.callbacks.splice(state.callbacks.end(), prepared); // No allocation/guest work under Publish.
	};
	if (context.Shared()) { return lease.Publish(publish) ? 0 : -1; }
	publish(); return 0;
} catch (...) { Context {}.Reject(); return -1; }

enum class DrainStatus { Completed, Refused, CallbackInterrupted, RecursiveExit };
struct DrainResult {
	DrainStatus status = DrainStatus::Refused;
	RuntimeGuestResult callback {};
	bool Completed() const { return status == DrainStatus::Completed; }
};

// dso filtering applies ONLY to CXA finalization. Ordinary process exit drains
// the combined registration order (CXA + atexit), last registration first.
inline DrainResult DrainExit(bool ordinary_exit, void* dso = nullptr) try {
	const Context context;
	if (!context.Ready()) { return {}; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::Callback) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return {}; }
	auto& registry = Exits();
	const auto key = Key(context);
	{
		std::unique_lock lock(registry.mutex);
		auto& state = registry.states[key];
		while (state.depth && state.draining_thread.lock() != ActualThread()) {
			if (context.Canceled()) { return {}; }
			// No signal dispatch. Stop of a parked peer must not run guest code.
			registry.changed.wait_for(lock, std::chrono::milliseconds(1));
		}
		if (context.Canceled()) { return {}; }
		if (state.exited) { return {DrainStatus::Completed, {}}; }
		if (ordinary_exit && state.depth) {
			context.Reject(); return {DrainStatus::RecursiveExit, {}};
		}
		state.draining_thread = ActualThread(); ++state.depth;
		if (ordinary_exit) { state.exiting = true; }
	}
	// This guard is ABOVE every nested guest boundary. No failure/exit transfer
	// is performed until its destructor and every callback's host frame returns.
	struct Finish {
		ExitRegistry& registry; ExitKey key; bool ordinary; bool completed = false;
		~Finish() {
			std::lock_guard lock(registry.mutex);
			auto& state = registry.states.at(key);
			if (--state.depth == 0) {
				state.draining_thread.reset();
				if (ordinary) { state.exited = completed; state.exiting = false; }
			}
			registry.changed.notify_all();
		}
	} finish {registry, key, ordinary_exit};
	for (;;) {
		if (context.Canceled()) { return {}; }
		ExitRecord next {};
		std::list<ExitRecord> claimed;
		{
			std::lock_guard lock(registry.mutex);
			auto& state = registry.states.at(key);
			for (auto i = state.callbacks.end(); i != state.callbacks.begin();) {
				--i;
				if (ordinary_exit || (i->kind == ExitKind::Cxa && (!dso || i->dso == dso))) {
					// Claim before guest entry: recursive finalize never replays this
					// record. A callback interrupted by thread exit is still consumed.
					const auto claim = [&] { next = *i; claimed.splice(claimed.end(), state.callbacks, i); };
					if (context.Shared()) { if (!lease.Publish(claim)) { return {}; } }
					else { claim(); }
					break;
				}
			}
			if (!next.target) {
				if (context.Shared() && !lease.Publish([&] { finish.completed = true; })) { return {}; }
				finish.completed = true; return {DrainStatus::Completed, {}};
			}
		}
		if (context.Shared()) {
			const auto result = context.execution->InvokeCallback(next.target, next.argument);
			if (!result.Returned()) { return {DrainStatus::CallbackInterrupted, result}; }
		} else if (next.kind == ExitKind::Atexit) { reinterpret_cast<void(KYTY_SYSV_ABI*)()>(next.target)(); }
		else { reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(next.target)(next.argument); }
	}
} catch (...) { Context {}.Reject(); return {}; }
inline void MainReturned(int status) try {
	const Context context;
	if (!context.Ready()) { return; }
	auto lease = context.execution ? context.execution->TryAcquire(GuestWorkKind::HostCall) : RuntimeGuestExecution::Lease {};
	if (context.Shared() && !lease) { return; }
	auto& registry = Exits(); std::lock_guard lock(registry.mutex);
	auto& state = registry.states[Key(context)];
	const auto publish = [&] { state.main_returned = true; state.main_status = status; };
	if (context.Shared()) { lease.Publish(publish); } else { publish(); }
} catch (...) { Context {}.Reject(); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
inline size_t PendingExitRecords(const std::shared_ptr<RuntimeGuestExecution>& execution) {
	auto& registry = Exits(); std::lock_guard lock(registry.mutex);
	const auto life = execution->Owner(); const auto it = registry.states.find({life.identity, life.token});
	return it == registry.states.end() ? 0 : it->second.callbacks.size();
}
inline unsigned EntryNotifications(const std::shared_ptr<RuntimeGuestExecution>& execution) {
	auto& registry = Exits(); std::lock_guard lock(registry.mutex);
	const auto life = execution->Owner(); const auto it = registry.states.find({life.identity, life.token});
	return it == registry.states.end() ? 0 : it->second.entry_notifications;
}
inline bool MainReturnStatus(const std::shared_ptr<RuntimeGuestExecution>& execution, int* status) {
	auto& registry = Exits(); std::lock_guard lock(registry.mutex);
	const auto life = execution->Owner(); const auto it = registry.states.find({life.identity, life.token});
	if (it == registry.states.end() || !it->second.main_returned) { return false; }
	*status = it->second.main_status; return true;
}
#endif
} // namespace Loader::RuntimeCrt
#endif
