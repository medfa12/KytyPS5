// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/applicationImportBindings.h"
#include "loader/applicationReturningExports.h"
#include "loader/applicationServiceReturningExports.h"

#include "common/logging/log.h"
#include "loader/deferredImportThunk.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <optional>

namespace Loader {

struct ApplicationImportBindings::Record {
	ApplicationImportBindings* bindings;
	uint64_t slot;
	std::string raw_name;
	std::optional<SymbolRecord> fixed_symbol; // dlsym snapshots do not rebind with the GOT.
	std::atomic<uint64_t> cached {0};
	std::atomic<uint64_t> revision {0};
	std::unique_ptr<DeferredImportThunk> entry;
	// Keep old targets alive across rebinding: another admitted call may already
	// be in that gateway's epilogue. Owner-wide reclamation owns their lifetime.
	std::vector<std::pair<uint64_t, std::unique_ptr<HostCallReturnThunk>>> gateways;
};

ApplicationImportBindings::ApplicationImportBindings(Program* importer):
    importer_(importer), execution_(GetApplicationExecution(importer)) {}
ApplicationImportBindings::~ApplicationImportBindings() = default;

uint64_t ApplicationImportBindings::Bind(uint64_t slot, const std::string& raw_name) {
	if (!execution_ || !slot || raw_name.empty()) { return 0; }
	std::lock_guard lock(mutex_);
	for (const auto& record: records_) {
		if (record->slot != slot) { continue; }
		if (record->raw_name != raw_name) { return 0; } // Never repurpose a published identity.
		record->cached.store(0, std::memory_order_release);
		record->revision.fetch_add(1, std::memory_order_acq_rel);
		return record->entry->GetAddress();
	}
	auto record = std::make_unique<Record>();
	record->bindings = this;
	record->slot = slot;
	record->raw_name = raw_name;
	record->entry = DeferredImportThunk::Create(record.get(), ResolveCall);
	if (!record->entry) { return 0; }
	const auto address = record->entry->GetAddress();
	records_.push_back(std::move(record));
	return address;
}

uint64_t ApplicationImportBindings::BindResolved(const std::string& nid, const SymbolRecord& symbol) {
	if (!execution_ || nid.empty() || !symbol.vaddr) { return 0; }
	bool host = false;
	if (!Classify(nid, symbol, &host).empty()) { return 0; }
	std::lock_guard lock(mutex_);
	for (const auto& record: records_) {
		if (record->fixed_symbol && record->raw_name == nid && record->fixed_symbol->vaddr == symbol.vaddr &&
		    record->fixed_symbol->name == symbol.name && record->fixed_symbol->dbg_name == symbol.dbg_name) {
			return record->entry->GetAddress();
		}
	}
	auto record = std::make_unique<Record>();
	record->bindings = this;
	record->slot = 0;
	record->raw_name = nid;
	record->fixed_symbol = symbol;
	record->entry = DeferredImportThunk::Create(record.get(), ResolveCall);
	if (!record->entry) { return 0; }
	const auto address = record->entry->GetAddress();
	records_.push_back(std::move(record));
	return address;
}

std::string ApplicationImportBindings::Classify(const std::string& raw_name, const SymbolRecord& symbol, bool* host) const {
	*host = false;
	if (!symbol.vaddr) { return "unresolved function"; }
	if (const auto* provider = importer_->rt->FindProgramByAddr(symbol.vaddr)) {
		return provider->application_owner == importer_->application_owner &&
		    importer_->rt->IsApplicationExecutableAddress(importer_->application_owner, symbol.vaddr) ? std::string {} :
		    "native target is not private executable text";
	}
	const auto nid = raw_name.substr(0, raw_name.find('#'));
	const auto* registered = importer_->rt->Symbols()->FindRegistration(nid, SymbolType::Func, symbol);
	if (!registered) {
		return "target has no matching HLE registration or private native mapping";
	}
	*host = true;
	for (const auto& item: ApplicationReturningExports) {
		if (nid == item.nid && registered->dbg_name == item.expression) { return {}; }
	}
	for (const auto& item: ApplicationServiceReturningExports) {
		if (nid == item.nid && registered->dbg_name == item.expression) { return {}; }
	}
	return "HLE transition has not been audited";
}

uint64_t ApplicationImportBindings::Target(Record& record, const SymbolRecord& symbol) {
	bool host = false;
	const auto reason = Classify(record.raw_name, symbol, &host);
	if (!reason.empty()) {
		LOGF("VSH import: owner=%d importer=%d slot=0x%016" PRIx64 " %s: %s\n",
		     importer_->application_owner->unique_id, importer_->unique_id, record.slot,
		     record.raw_name.c_str(), reason.c_str());
		return 0;
	}
	if (!host) { return symbol.vaddr; }
	std::lock_guard lock(mutex_);
	for (const auto& [target, gateway]: record.gateways) {
		if (target == symbol.vaddr) { return gateway->GetAddress(); }
	}
	auto gateway = HostCallReturnThunk::Create(execution_->Boundary(), symbol.vaddr, execution_.get(),
	    RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
	if (!gateway) { return 0; }
	const auto address = gateway->GetAddress();
	record.gateways.emplace_back(symbol.vaddr, std::move(gateway));
	return address;
}

uint64_t KYTY_SYSV_ABI ApplicationImportBindings::ResolveCall(void* opaque, uint64_t caller) noexcept {
	Common::HostLockCriticalGuard critical;
	auto& record = *static_cast<Record*>(opaque);
	auto& bindings = *record.bindings;
	auto& execution = *bindings.execution_;
	const auto failure = [&] {
		return reinterpret_cast<uint64_t>(execution.FailureFunction());
	};
	try {
		const auto current = GetApplicationExecution(RuntimeLinker::GetCurrentProgram());
		if (current.get() != &execution || !execution.FailureFunction()) {
			execution.Fail(GuestFailureReason::InvalidTransition, caller);
			return 0; // No matching actual-thread segment: invariant trap, not fake API success.
		}
		if (execution.PendingThreadExit()) { return failure(); }
		auto lease = execution.TryAcquire(GuestWorkKind::HostCall);
		if (!lease) { return failure(); }
		const auto revision = record.revision.load(std::memory_order_acquire);
		auto target = record.cached.load(std::memory_order_acquire);
		if (!target) {
			SymbolRecord symbol {};
			bool self = false;
			if (record.fixed_symbol) { symbol = *record.fixed_symbol; }
			else { bindings.importer_->rt->Resolve(record.raw_name, SymbolType::Func, bindings.importer_, &symbol, &self); }
			target = bindings.Target(record, symbol);
			if (!target) {
				execution.Fail(GuestFailureReason::Explicit, record.slot);
				LOGF("VSH import failure: %s caller=0x%016" PRIx64 " lifetime=%" PRIu64 "\n",
				     record.raw_name.c_str(), caller, execution.Owner().token);
				return failure();
			}
		}
		{
			std::lock_guard lock(bindings.mutex_);
			if (!lease.Publish([&] {
				if (record.revision.load(std::memory_order_acquire) == revision) {
					record.cached.store(target, std::memory_order_release);
				}
			})) { return failure(); }
		}
		// All resolver ownership/locks finish before the thunk tail-transfers this
		// very invocation with its original argument registers and stack intact.
		return target;
	} catch (...) {
		execution.Fail(GuestFailureReason::InvalidTransition, record.slot);
		return failure();
	}
}

std::vector<std::string> ApplicationImportBindings::AdmissionBlockers() {
	// Registration runs under loader serialization. Snapshot before resolving,
	// so this mutex is never held while taking the loader lock.
	std::vector<std::pair<std::string, std::optional<SymbolRecord>>> names;
	{
		std::lock_guard lock(mutex_);
		for (const auto& record: records_) { names.emplace_back(record->raw_name, record->fixed_symbol); }
	}
	std::vector<std::string> blockers;
	for (const auto& [name, fixed]: names) {
		SymbolRecord symbol = fixed.value_or(SymbolRecord {});
		bool self = false, host = false;
		if (!fixed) { importer_->rt->Resolve(name, SymbolType::Func, importer_, &symbol, &self); }
		const auto reason = Classify(name, symbol, &host);
		if (!reason.empty()) { blockers.push_back(name + ": " + reason); }
	}
	return blockers;
}

bool ApplicationImportBindings::HasPatchedEntry(uint64_t slot, const std::string& raw_name, uint64_t value) const {
	std::lock_guard lock(mutex_);
	for (const auto& record: records_) {
		if (record->slot == slot && record->raw_name == raw_name && record->entry &&
		    record->entry->GetAddress() == value) { return true; }
	}
	return false;
}

bool ApplicationImportBindings::Published() const {
	std::lock_guard lock(mutex_);
	return !records_.empty();
}
} // namespace Loader
