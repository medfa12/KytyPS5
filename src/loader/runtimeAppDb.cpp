#include "loader/runtimeAppDb.h"
#include "common/logging/log.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/runtimeLinker.h"
#include "loader/elf.h"
#include "libs/appDbNativeTransport.h"
#include "libs/dbPreparationNativeTransport.h"
#include "libs/dbRecoveryNativeTransport.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace Loader {
namespace {
struct AppDbOwner {
	Program* core;
	std::weak_ptr<BootCredentialLifetime> lifetime;
	std::shared_ptr<BootCredentialAdmission> admission;
	std::shared_ptr<RuntimeGuestExecution> execution;
	int pid;
	bool Live() const { return !lifetime.expired() && admission && admission->IsLiveFor(core); }
};
bool AuthorizeDatabaseRegistration(int pid, std::string_view service) {
	std::array<uint64_t, 17> record {};
	return (service == Libs::AppDbNativeTransport::ServiceName ||
	        service == Libs::DbPreparationNativeTransport::ServiceName ||
	        service == Libs::DbRecoveryNativeTransport::ServiceName) &&
	       TryGetSourceBoundProcessAuthInfo(pid, &record) && ((record[1] >> 62) & 1) != 0;
}
bool InstallDatabaseTransport(Program* core, const char* service, int (*install)(Libs::NpNativeTransport::Hooks) noexcept) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!core || !core->boot_credential_admission || !core->boot_credential_admission->IsCoreImage() || !core->boot_credential_admission->IsLiveFor(core)) { return false; }
	const auto previous = RuntimeLinker::GetCurrentProgram();
	RuntimeLinker::SetCurrentProgram(core);
	const int pid = GetCurrentGuestProcessId();
	RuntimeLinker::SetCurrentProgram(previous);
	if (!AuthorizeDatabaseRegistration(pid, service)) { return false; }
	auto execution = RuntimeGuestExecution::Create(core);
	if (!execution) { return false; }
	auto owner = std::make_shared<AppDbOwner>(AppDbOwner {core, core->boot_credential_lifetime, core->boot_credential_admission, execution, pid});
	Libs::AppDbNativeTransport::Hooks hooks;
	hooks.owner = owner;
	hooks.identity = [] {
		int pid = -1;
		if (!TryGetCurrentRetainedGuestProcessId(&pid)) { return Libs::LibKernel::Ipmi::Identity {-1, -1}; }
		const auto thread = Libs::LibKernel::PthreadSelfOrNull();
		return Libs::LibKernel::Ipmi::Identity {pid, thread ? Libs::LibKernel::PthreadGetthreadid() : -1};
	};
	hooks.retain_dispatch_thread = [owner](std::shared_ptr<void> lifetime) {
		auto thread = Libs::LibKernel::PthreadSelfOrNull();
		if (!owner->Live() || !thread || RuntimeLinker::GetCurrentProgram() != owner->core ||
		    Libs::LibKernel::PthreadGetOwnerProgram(thread) != owner->core) { return int(Libs::LibKernel::Ipmi::Invalid); }
		return Libs::LibKernel::PthreadRetainCurrentUntilJoin(std::move(lifetime));
	};
	hooks.copy_in = [](uint64_t p, void* output, size_t size) {
		SIZE_T copied = 0;
		return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), output, size, &copied) && copied == size ? 0 : 14;
	};
	hooks.copy_out = [](uint64_t p, const void* input, size_t size) {
		return Libs::LibKernel::FileSystem::CopyPathResultToGuest(reinterpret_cast<void*>(p), input, size) ? 0 : 14;
	};
	hooks.authorize_registration = [owner, service](Libs::LibKernel::Ipmi::Identity id, bool server) {
		if (!owner->Live() || !IsGuestProcessIdentityActive(id.pid)) { return 1; }
		if (!server) { return 0; } // Native client registration has no reserved-name capability gate.
		return id.pid == owner->pid && AuthorizeDatabaseRegistration(id.pid, service) ? 0 : 1;
	};
	hooks.authorize_connect = [owner, service](Libs::LibKernel::Ipmi::Identity id, const Libs::LibKernel::Ipmi::ConnectionInfo& info) {
		if (!owner->Live() || !IsGuestProcessIdentityActive(id.pid) || info.server_pid != owner->pid ||
		    !AuthorizeDatabaseRegistration(owner->pid, service)) { return 1; }
		// AppDb and regular 13.60 DbPreparation +b27cf0 / DbRecovery +17bce20
		// leave all three server config flags zero. Kernel
		// 805a692f checks client capability62 when cross_no_cap is clear.
		std::array<uint64_t, 17> auth {};
		if (info.server_config.multi || info.server_config.restricted || info.server_config.cross_no_cap ||
		    !TryGetSourceBoundProcessAuthInfo(id.pid, &auth)) { return 1; }
		if (id.pid == owner->pid) { return 0; }
		// Kernel 808192d0: restricted authority family; unsigned wrap is intentional.
		const uint64_t restricted_index = auth[0] + UINT64_C(0xb7ffffffeffffffc);
		const bool restricted = (restricted_index <= 15 && ((UINT64_C(0xe001) >> restricted_index) & 1)) ||
		                        auth[0] == UINT64_C(0x4800000000001010);
		return !restricted && ((auth[1] >> 62) & 1) ? 0 : 1;
	};
	hooks.invoke_guest = [owner, service](uint64_t target, const uint64_t* arguments, size_t count, int64_t& value) -> int {
		auto thread = Libs::LibKernel::PthreadSelfOrNull();
		if (!owner->Live() || !thread || Libs::LibKernel::PthreadGetOwnerProgram(thread) != owner->core ||
		    RuntimeLinker::GetCurrentProgram() != owner->core ||
		    GetCurrentGuestProcessId() != owner->pid || !IsGuestProcessIdentityActive(owner->pid)) { return 1; }
		bool executable = false;
		const auto* ph = owner->core->elf->GetPhdr();
		for (size_t i = 0; i < owner->core->elf->GetEhdr()->e_phnum; ++i) {
			const uint64_t begin = owner->core->base_vaddr + ph[i].p_vaddr;
			if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && target >= begin && target - begin < ph[i].p_filesz) { executable = true; }
		}
		if (!executable) { return 14; }
		// The existing actual dispatcher thread stays in RunDispatcher. The
		// boundary pins its callback execution; no callback runs on the client.
		auto lease = owner->execution->TryAcquire(GuestWorkKind::Callback);
		if (!lease) { return Libs::LibKernel::Ipmi::Cancelled; }
		const auto result = owner->execution->InvokeIntegerCallback(target, arguments, count);
		if (!result.Returned()) { return Libs::LibKernel::Ipmi::BadMessage; }
		value = static_cast<int64_t>(result.call.value);
		LOGF("%s: native callback pid=%d rva=0x%llx argc=%zu returned=0x%llx\n", service, owner->pid,
		     static_cast<unsigned long long>(target - owner->core->base_vaddr), count,
		     static_cast<unsigned long long>(result.call.value));
		return 0;
	};
	hooks.cooperate = [owner] {
		if (!owner->Live()) { return int(Libs::LibKernel::Ipmi::Cancelled); }
		Libs::LibKernel::PthreadYield();
		return 0;
	};
	const int installed = install(std::move(hooks));
	LOGF("%s: source-bound Core pid=%d adapter install=%d (server not yet published)\n", service, pid, installed);
	return installed == 0;
#else
	return false;
#endif
}
}
bool InstallSourceBoundAppDb(Program* core) {
	// Install before native initializers publish any endpoint. Failure rolls back
	// only the adapters acquired by this attempt; existing installations are Busy.
	if (!InstallDatabaseTransport(core, Libs::AppDbNativeTransport::ServiceName, Libs::AppDbNativeTransport::Install)) { return false; }
	if (!InstallDatabaseTransport(core, Libs::DbPreparationNativeTransport::ServiceName, Libs::DbPreparationNativeTransport::Install)) {
		Libs::AppDbNativeTransport::Uninstall();
		return false;
	}
	if (!InstallDatabaseTransport(core, Libs::DbRecoveryNativeTransport::ServiceName, Libs::DbRecoveryNativeTransport::Install)) {
		Libs::DbPreparationNativeTransport::Uninstall();
		Libs::AppDbNativeTransport::Uninstall();
		return false;
	}
	return true;
}
bool RetireSourceBoundAppDb() {
	// Attempt every retirement. A busy native object or actual dispatcher pin
	// prevents the caller from releasing Core mappings; no synthetic completion.
	const int recovery = Libs::DbRecoveryNativeTransport::Uninstall();
	const int preparation = Libs::DbPreparationNativeTransport::Uninstall();
	const int appdb = Libs::AppDbNativeTransport::Uninstall();
	return recovery == 0 && preparation == 0 && appdb == 0;
}
}
