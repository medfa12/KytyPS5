#include "loader/runtimeNpTransport.h"
#include "loader/runtimeNpManagerClient.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/runtimeLinker.h"
#include "common/singleton.h"
#include "loader/elf.h"
#include "libs/npNativeTransport.h"
#include "libs/loginMgrNativeTransport.h"
#include "libs/npMemoryHeap.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "common/logging/log.h"

#include <algorithm>
#include <memory>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace Loader {
namespace {
namespace I = Libs::LibKernel::Ipmi;
namespace K = Libs::LibKernel;
namespace N = Libs::NpNativeTransport;
struct NpTransportOwner {
	Program* core;
	std::weak_ptr<BootCredentialLifetime> lifetime;
	std::shared_ptr<BootCredentialAdmission> admission;
	std::shared_ptr<RuntimeGuestExecution> execution;
	int pid;
	bool Live() const {
		auto token = lifetime.lock();
		return token && token == core->boot_credential_lifetime && admission &&
		       admission == core->boot_credential_admission &&
		       admission->IsLiveFor(core) && IsGuestProcessIdentityActive(pid);
	}
};
struct InstalledEpoch {
	uint64_t generation;
	std::shared_ptr<NpTransportOwner> owner;
	bool closing = false;
};
// All access uses RuntimeLinker's recursive admission mutex. This state belongs
// to the installed source-bound transport, not the shorter-lived client run.
std::shared_ptr<InstalledEpoch> installed_epoch;
uint64_t next_epoch = 1;
}

static bool InstallSourceBoundTransport(Program* core, bool login_mgr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Caller holds the loader admission lock across validation/publication.
	// Install's caller owns the admitted Program through successful retirement;
	// this also permits the isolated retained-input admission fixture. Guest-facing
	// worker Start additionally requires loader membership before dereferencing it.
	if (login_mgr ? Libs::LoginMgrNativeTransport::Installed() : (installed_epoch || next_epoch == UINT64_MAX)) { return false; }
	if (!core || !core->elf || !core->boot_credential_admission ||
	    !core->boot_credential_admission->IsCoreImage() || !core->boot_credential_admission->IsLiveFor(core)) { return false; }
	const auto previous = RuntimeLinker::GetCurrentProgram();
	RuntimeLinker::SetCurrentProgram(core);
	int pid = -1;
	const bool retained = TryGetCurrentRetainedGuestProcessId(&pid);
	RuntimeLinker::SetCurrentProgram(previous);
	std::array<uint64_t, 17> auth {};
	// Reserved Sce service registration uses the actual admitted record's bit62.
	// This is emulator admission, not execution of native mailbox0x16 policy.
	if (!retained || !TryGetSourceBoundProcessAuthInfo(pid, &auth) || !(auth[1] & (UINT64_C(1) << 62))) { return false; }
	auto execution = RuntimeGuestExecution::Create(core);
	if (!execution) { return false; }
	auto owner = std::make_shared<NpTransportOwner>(NpTransportOwner {core, core->boot_credential_lifetime,
	    core->boot_credential_admission, execution, pid});
	// Allocate the epoch BEFORE native installation. No fallible allocation may
	// separate successful Install from publication under the admission mutex.
	auto epoch = std::make_shared<InstalledEpoch>(InstalledEpoch {next_epoch, owner, false});
	Libs::NpNativeTransport::Hooks hooks;
	hooks.owner = owner;
	if (login_mgr) {
		hooks.retain_dispatch_thread = [owner](std::shared_ptr<void> lifetime) {
			auto self = K::PthreadSelfOrNull();
			if (!owner->Live() || !self || K::PthreadGetOwnerProgram(self) != owner->core ||
			    RuntimeLinker::GetCurrentProgram() != owner->core) { return int(I::Invalid); }
			return K::PthreadRetainCurrentUntilJoin(std::move(lifetime));
		};
	}
	hooks.capture_dispatch_thread = [owner](std::shared_ptr<void> lifetime, N::DispatcherReceipt& output) {
		K::PthreadNativeReceipt receipt;
		int result = K::PthreadCaptureCurrentNativeReceipt(receipt);
		if (result) { return result; }
		K::PthreadNativeSnapshot snapshot;
		result = K::PthreadInspectNativeReceipt(receipt, snapshot);
		if (result) { return result; }
		if (!owner->Live() || !snapshot.generation || snapshot.pid != owner->pid || snapshot.tid <= 0 ||
		    snapshot.owner_cookie != reinterpret_cast<uintptr_t>(owner->core) || snapshot.detached_at_creation ||
		    snapshot.phase != K::PthreadNativePhase::Pending) { return int(I::Invalid); }
		result = K::PthreadRetainCurrentUntilJoin(std::move(lifetime));
		if (result) { return result; }
		output = {snapshot.generation, {snapshot.pid, snapshot.tid}, std::move(receipt)};
		return 0;
	};
	hooks.inspect_dispatch_thread = [owner](const N::DispatcherReceipt& receipt, N::DispatcherProof& output) {
		if (!receipt.token || !receipt.generation) { return int(I::Invalid); }
		K::PthreadNativeSnapshot snapshot;
		const int result = K::PthreadInspectNativeReceipt(
		    std::static_pointer_cast<K::PthreadNativeReceiptState>(receipt.token), snapshot);
		if (result) { return result; }
		if (snapshot.generation != receipt.generation || snapshot.pid != receipt.identity.pid ||
		    snapshot.tid != receipt.identity.tid || snapshot.owner_cookie != reinterpret_cast<uintptr_t>(owner->core)) {
			return int(I::Invalid);
		}
		N::DispatcherProof proof;
		proof.native_exited = snapshot.phase != K::PthreadNativePhase::Pending;
		proof.successfully_joined = snapshot.phase == K::PthreadNativePhase::Joined;
		if (proof.successfully_joined) {
			proof.join_source = {snapshot.joiner_pid, snapshot.joiner_tid};
			switch (snapshot.join_source) {
				case K::PthreadNativeJoinSource::Explicit: proof.join_origin = N::DispatcherJoinOrigin::Explicit; break;
				case K::PthreadNativeJoinSource::DetachedCollector: proof.join_origin = N::DispatcherJoinOrigin::DetachedCollector; break;
				case K::PthreadNativeJoinSource::StartupRollback: proof.join_origin = N::DispatcherJoinOrigin::StartupRollback; break;
				default: return int(I::BadMessage);
			}
		}
		output = proof;
		return 0;
	};
	hooks.retain_storage = [](uint64_t address, size_t size, std::shared_ptr<void>& lease) {
		return Libs::NpMemoryHeap::AcquireAllocationLease(address, size, lease);
	};
	hooks.identity = [] {
		int current = -1;
		if (!Libs::LibKernel::PthreadSelfOrNull() || !TryGetCurrentRetainedGuestProcessId(&current)) { return I::Identity {-1, -1}; }
		return I::Identity {current, Libs::LibKernel::PthreadGetthreadid()};
	};
	hooks.copy_in = [](uint64_t address, void* output, size_t size) {
		SIZE_T copied = 0;
		return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &copied) && copied == size ? 0 : 14;
	};
	hooks.copy_out = [](uint64_t address, const void* input, size_t size) {
		return Libs::LibKernel::FileSystem::CopyPathResultToGuest(reinterpret_cast<void*>(address), input, size) ? 0 : 14;
	};
	hooks.authorize_registration = [owner](I::Identity id, bool server) {
		std::array<uint64_t, 17> record {};
		if (!owner->Live() || !TryGetSourceBoundProcessAuthInfo(id.pid, &record)) { return 1; }
		return !server || (id.pid == owner->pid && (record[1] & (UINT64_C(1) << 62))) ? 0 : 1;
	};
	hooks.authorize_connect = [owner](I::Identity id, const I::ConnectionInfo& info) {
		std::array<uint64_t, 17> record {};
		if (!owner->Live() || info.server_pid != owner->pid ||
		    !TryGetSourceBoundProcessAuthInfo(id.pid, &record)) { return 1; }
		// Audited NP/LoginMgr servers: cross_no_cap=1, multi=restricted=0
		// (regular Core LoginMgr +74a83b writes config+18=1). Retain the
		// independent restricted-caller check even though capability62 is waived.
		if (info.server_config.multi || info.server_config.restricted || info.server_config.cross_no_cap != 1) { return 1; }
		if (id.pid == owner->pid) { return 0; }
		const uint64_t index = record[0] + UINT64_C(0xb7ffffffeffffffc);
		const bool restricted = (index <= 15 && ((UINT64_C(0xe001) >> index) & 1)) ||
		                        record[0] == UINT64_C(0x4800000000001010);
		return restricted ? 13 : 0;
	};
	hooks.invoke_guest = [owner, login_mgr](uint64_t target, const uint64_t* arguments, size_t count, int64_t& value) -> int {
		int current = -1;
		auto self = K::PthreadSelfOrNull();
		if (!owner->Live() || RuntimeLinker::GetCurrentProgram() != owner->core ||
		    !self || K::PthreadGetOwnerProgram(self) != owner->core ||
		    !TryGetCurrentRetainedGuestProcessId(&current) || current != owner->pid) { return 1; }
		if (count > 7 || (count && !arguments)) { return I::Invalid; }
		bool executable = false;
		const auto* ph = owner->core->elf->GetPhdr();
		for (size_t i = 0; i < owner->core->elf->GetEhdr()->e_phnum; ++i) {
			const uint64_t begin = owner->core->base_vaddr + ph[i].p_vaddr;
			if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && target >= begin && target - begin < ph[i].p_filesz) { executable = true; }
		}
		if (!executable) { return 14; }
		auto lease = owner->execution->TryAcquire(GuestWorkKind::Callback);
		if (!lease) { return I::Cancelled; }
		const auto result = owner->execution->InvokeIntegerCallback(target, arguments, count);
		if (!result.Returned()) { return I::BadMessage; }
		value = static_cast<int64_t>(result.call.value);
		LOGF("%s: native callback pid=%d tid=%d rva=0x%llx argc=%zu returned=0x%llx\n", login_mgr ? "LoginMgr" : "NP", owner->pid,
		     Libs::LibKernel::PthreadGetthreadid(), static_cast<unsigned long long>(target - owner->core->base_vaddr),
		     count, static_cast<unsigned long long>(result.call.value));
		return 0;
	};
	hooks.cooperate = [owner] {
		if (!owner->Live()) { return int(I::Cancelled); }
		Libs::LibKernel::PthreadYield();
		return 0;
	};
	const int installed = login_mgr ? Libs::LoginMgrNativeTransport::Install(std::move(hooks)) :
	                                 Libs::NpNativeTransport::Install(std::move(hooks));
	if (installed == 0 && !login_mgr) { installed_epoch = std::move(epoch); ++next_epoch; }
	LOGF("%s: source-bound Core pid=%d adapter install=%d (server not yet published)\n", login_mgr ? "LoginMgr" : "NP", pid, installed);
	return installed == 0;
#else
	return false;
#endif
}

bool InstallSourceBoundNpTransport(Program* core) {
	Common::LockGuard lock(Common::Singleton<RuntimeLinker>::Instance()->m_mutex);
	return InstallSourceBoundTransport(core, false);
}
bool InstallSourceBoundLoginMgrTransport(Program* core) {
	Common::LockGuard lock(Common::Singleton<RuntimeLinker>::Instance()->m_mutex);
	return InstallSourceBoundTransport(core, true);
}
bool RetireSourceBoundLoginMgrTransport() {
	Common::LockGuard lock(Common::Singleton<RuntimeLinker>::Instance()->m_mutex);
	return Libs::LoginMgrNativeTransport::Uninstall() == 0;
}

int InspectSourceBoundNpTransportEpoch(SourceBoundNpTransportEpoch& output) noexcept {
	auto* linker = Common::Singleton<RuntimeLinker>::Instance();
	Common::LockGuard admission(linker->m_mutex);
	if (!installed_epoch) { return I::NotFound; }
	const auto& epoch = *installed_epoch;
	output = {epoch.generation, epoch.owner->pid, epoch.closing, epoch.owner->Live()};
	return 0;
}

int CloseSourceBoundNpTransportEpoch(uint64_t expected_generation) noexcept {
	auto* linker = Common::Singleton<RuntimeLinker>::Instance();
	Common::LockGuard admission(linker->m_mutex);
	if (!installed_epoch || !expected_generation || installed_epoch->generation != expected_generation) { return I::NotFound; }
	const auto& owner = *installed_epoch->owner;
	auto self = Libs::LibKernel::PthreadSelfOrNull(); int pid = -1;
	std::array<uint64_t, 17> auth {};
	if (!self || Libs::LibKernel::PthreadGetOwnerProgram(self) != owner.core ||
	    RuntimeLinker::GetCurrentProgram() != owner.core || !owner.Live() ||
	    !owner.admission->IsCoreImage() || !TryGetCurrentRetainedGuestProcessId(&pid) || pid != owner.pid ||
	    !TryGetSourceBoundProcessAuthInfo(pid, &auth)) { return I::Invalid; }
	installed_epoch->closing = true;
	return 0;
}

bool RetireSourceBoundNpTransport() {
	auto* linker = Common::Singleton<RuntimeLinker>::Instance();
	Common::LockGuard admission(linker->m_mutex);
	if (RuntimeNpManagerClient::HasLive()) { return false; }
	if (Libs::NpNativeTransport::Uninstall() != 0) { return false; }
	installed_epoch.reset();
	return Libs::NpMemoryHeap::RetryAllocationRetirements() == 0;
}
}
