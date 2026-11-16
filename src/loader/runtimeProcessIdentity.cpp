#include "loader/runtimeProcessIdentity.h"

#include "common/threads.h"
#include "libs/libLncUtil.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/elf.h"
#include "loader/runtimeAppDb.h"
#include "loader/runtimeNpTransport.h"

extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

#include <limits>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace Loader {
namespace {
struct Record {
	GuestProcessIdentity identity;
	bool active = true;
	bool cohost = false;
	std::weak_ptr<BootCredentialLifetime> lifetime;
	std::weak_ptr<BootCredentialAdmission> admission;
	bool IsActive() const { return active && (!cohost || !lifetime.expired()); }
};
struct Registry {
	std::mutex mutex;
	int64_t next_pid = BootGuestProcessId + 1;
	std::unordered_map<const Program*, Record> owners;
	std::optional<GuestProcessIdentity> boot;
	const Program* boot_owner = nullptr;
	std::weak_ptr<BootCredentialLifetime> boot_lifetime;
};
Registry& Processes() {
	// Session-lifetime storage, matching resident Program ownership.
	static auto* registry = new Registry;
	return *registry;
}
class ProgramScope {
public:
	explicit ProgramScope(Program* program): previous(RuntimeLinker::GetCurrentProgram()) {
		RuntimeLinker::SetCurrentProgram(program);
	}
	~ProgramScope() { RuntimeLinker::SetCurrentProgram(previous); }
private:
	Program* previous;
};

GuestProcessIdentity Snapshot(Program* program) {
	ProgramScope scope(program);
	GuestProcessIdentity result;
	int32_t category = 0;
	if (SystemContentParamSfoGetInt("APPLICATION_CATEGORY_TYPE", &category)) {
		result.category = static_cast<uint32_t>(category);
	}
	int32_t attribute3 = 0;
	if (SystemContentParamSfoGetInt("ATTRIBUTE3", &attribute3)) {
		result.attribute3 = static_cast<uint32_t>(attribute3);
	}
	if (program != nullptr) {
		if (program->authority_metadata) { result.authority = program->authority_metadata->authority_id; }
		result.abi = program->process_abi;
		result.compiled_sdk_version = program->compiled_sdk_version;
		result.prospero_compiled_sdk_version = program->prospero_compiled_sdk_version;
	}
	int32_t app_id = 0;
	if (Libs::LncUtil::Lnc::TryGetCurrentAppId(&app_id)) { result.app_id = app_id; }
	return result;
}

bool HashCredentialBytes(std::span<const uint8_t> bytes, BootCredentialDigest& digest) {
	auto* state = av_sha_alloc();
	if (!state) { return false; }
	const bool ok = av_sha_init(state, 256) == 0;
	if (ok) { av_sha_update(state, bytes.data(), bytes.size()); av_sha_final(state, digest.data()); }
	av_free(state);
	return ok;
}
} // namespace

int RegisterCohostProcessIdentity(Program* program, uint32_t app_id) {
	if (!program || program->application_owner || !program->boot_credential_lifetime || !app_id || app_id > INT32_MAX) { return -1; }
	Common::HostLockCriticalGuard critical;
	auto snapshot = Snapshot(program);
	if (snapshot.app_id != static_cast<int32_t>(app_id)) { return -1; }
	snapshot.app_id = static_cast<int32_t>(app_id);
	// An unauthenticated declared authority is not an admitted cohost credential.
	snapshot.authority.reset();
	snapshot.capabilities = 0;
	// Null-owner SystemContent is a legacy singleton, not cohost metadata.
	snapshot.category.reset();
	snapshot.attribute3.reset();
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	if (registry.owners.contains(program)) { return -1; }
	if (registry.boot && registry.boot->app_id == snapshot.app_id) { return -1; }
	for (const auto& [owner, record]: registry.owners) {
		if (record.IsActive() && record.identity.app_id == snapshot.app_id) { return -1; }
	}
	if (registry.next_pid > std::numeric_limits<int>::max()) { return -1; }
	snapshot.pid = static_cast<int>(registry.next_pid++);
	registry.owners.emplace(program, Record {snapshot, true, true, program->boot_credential_lifetime, program->boot_credential_admission});
	return snapshot.pid;
}

bool AdmitOriginalCoreBootCredentials(Program* program, std::span<const uint8_t> original,
                                     std::span<const BootMappedSegment> mapped, std::span<const uint8_t> record) {
	return original.size() == 35337516 && AdmitSourceBoundBootCredentials(program, original, mapped, record);
}

bool AdmitSourceBoundBootCredentials(Program* program, std::span<const uint8_t> original,
                                    std::span<const BootMappedSegment> mapped, std::span<const uint8_t> record) {
	if (!program || program->application_owner || !program->elf || program->started.load() ||
	    program->application_threads.load() || program->boot_credential_admission || !program->base_vaddr ||
	    (original.size() != 35337516 && original.size() != 21878244) || !program->elf->GetEhdr() || !program->elf->GetPhdr()) { return false; }
	if (program->elf->GetEhdr()->e_phnum != 14 || program->elf->GetEhdr()->e_phentsize != 56) { return false; }
	// Reject an unrelated source buffer or mapping claimed to belong to this Program.
	if (std::memcmp(program->elf->GetEhdr(), original.data(), 64) != 0 ||
	    std::memcmp(program->elf->GetPhdr(), original.data() + 64, 14 * 56) != 0) { return false; }
	for (const auto& segment: mapped) {
		if (segment.vaddr > program->base_size || segment.bytes.size() > program->base_size - segment.vaddr ||
		    segment.vaddr > UINT64_MAX - program->base_vaddr ||
		    reinterpret_cast<uint64_t>(segment.bytes.data()) != program->base_vaddr + segment.vaddr) { return false; }
	}
	auto admission = BootCredentialAdmission::Admit(program, program->boot_credential_lifetime,
	                                               original, mapped, record, HashCredentialBytes);
	if (!admission) { return false; }
	// ELF parsing happened before the immutable snapshot was taken. Also bind
	// the separately cached dynamic tables; a same-path replacement must not
	// inject relocation/import metadata into an otherwise original mapping.
	for (size_t i = 0; i < 14; ++i) {
		const auto& ph = program->elf->GetPhdr()[i];
		const void* cached = ph.p_type == PT_DYNAMIC ? static_cast<const void*>(program->elf->GetDynamic()) :
		    ph.p_type == PT_OS_DYNLIBDATA ? program->elf->GetDynamicData<const void*>(0) : nullptr;
		if (ph.p_type != PT_DYNAMIC && ph.p_type != PT_OS_DYNLIBDATA) { continue; }
		if (!cached || ph.p_offset > original.size() || ph.p_filesz > original.size() - ph.p_offset ||
		    std::memcmp(cached, original.data() + ph.p_offset, ph.p_filesz) != 0) { return false; }
	}
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	const auto it = registry.owners.find(program);
	if (it != registry.owners.end() && (!it->second.cohost || !it->second.IsActive())) { return false; }
	program->boot_credential_admission = admission;
	// Loading precedes LNC registration. A retained proof is not queryable until
	// the distinct cohost identity is published after its LNC registration.
	if (it != registry.owners.end()) { it->second.admission = admission; }
	return true;
}

void RevokeBootCredentials(Program* program) {
	if (program && program->boot_credential_admission) { program->boot_credential_admission->Revoke(); }
}

bool TryGetSourceBoundProcessAuthInfo(int64_t pid, std::array<uint64_t, 17>* out) {
	if (!out) { return false; }
	if (pid == 0) { pid = GetCurrentGuestProcessId(); }
	if (pid < BootGuestProcessId || pid > INT32_MAX) { return false; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	for (const auto& [owner, record]: registry.owners) {
		if (record.identity.pid == pid && record.cohost && record.IsActive()) {
			const auto admission = record.admission.lock();
			return admission && admission->CopyRecord(owner, out);
		}
	}
	return false;
}

bool AuthorizeSourceBoundAppDbRegistration(int64_t pid, std::string_view name) {
	// Narrow emulator admission, followed by the real reserved-name capability
	// predicate. No claim of native mailbox environment/caller acceptance.
	std::array<uint64_t, 17> record {};
	return name == "SceAppDbIpcmsg" && TryGetSourceBoundProcessAuthInfo(pid, &record) && ((record[1] >> 62) & 1) != 0;
}

int RegisterRuntimeProcessIdentity(Program* owner, uint32_t app_id) {
	if (owner == nullptr || owner->application_owner != owner || app_id == 0 || app_id > INT32_MAX) { return -1; }
	Common::HostLockCriticalGuard critical;
	auto snapshot = Snapshot(owner);
	snapshot.app_id = static_cast<int32_t>(app_id);
	// Extended-info authority is not a capability bitmap. In particular a
	// runtime executable never inherits the boot ShellUI compositor capability.
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	if (const auto it = registry.owners.find(owner); it != registry.owners.end()) {
		return it->second.IsActive() && it->second.identity.app_id == snapshot.app_id ? it->second.identity.pid : -1;
	}
	for (const auto& [existing_owner, record]: registry.owners) {
		if (record.IsActive() && record.identity.app_id == snapshot.app_id) { return -1; }
	}
	if (registry.next_pid > std::numeric_limits<int>::max()) { return -1; }
	snapshot.pid = static_cast<int>(registry.next_pid++);
	registry.owners.emplace(owner, Record {snapshot});
	return snapshot.pid;
}

bool ConfigureBootProcessIdentity(Program* program) {
	if (program == nullptr || program->application_owner != nullptr) { return false; }
	if (program->boot_credential_admission && program->boot_credential_admission->IsCoreImage()) {
		int32_t app_id = 0;
		if (!Libs::LncUtil::Lnc::TryGetProgramAppId(program, &app_id) || app_id <= 0 ||
		    RegisterCohostProcessIdentity(program, static_cast<uint32_t>(app_id)) <= 0) { return false; }
		if (!InstallSourceBoundAppDb(program)) { return false; }
		if (!InstallSourceBoundNpTransport(program)) {
			RetireSourceBoundAppDb();
			return false;
		}
		if (!InstallSourceBoundLoginMgrTransport(program)) {
			RetireSourceBoundNpTransport();
			RetireSourceBoundAppDb();
			return false;
		}
		return true;
	}
	Common::HostLockCriticalGuard critical;
	ProgramScope scope(program);
	Libs::LncUtil::Lnc::AppStatus status;
	if (Libs::LncUtil::Lnc::sceLncUtilGetAppStatus(&status) != 0) { return false; }
	const bool admitted_ui = program->boot_credential_admission && !program->boot_credential_admission->IsCoreImage();
	if (!admitted_ui && (status.app_type != 1 || std::strcmp(status.title_id, "NPXS40087") != 0)) {
		const char* source_mode = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
		return source_mode && *source_mode && status.app_id > 0 &&
		       RegisterCohostProcessIdentity(program, static_cast<uint32_t>(status.app_id)) > 0;
	}
	auto snapshot = Snapshot(program);
	snapshot.pid = BootGuestProcessId;
	snapshot.app_id = status.app_id;
	// The staged ShellUI ELF has no SELF authority header. Preserve the existing
	// boot-role contract only for the registered ShellUI, never runtime owners.
	if (!snapshot.authority) { snapshot.authority = UINT64_C(0x480000000000000f); }
	// This is the existing, observed ShellUI compositor contract only. No other
	// declared authority (nor missing metadata) establishes this capability.
	if (snapshot.authority == UINT64_C(0x480000000000000f)) { snapshot.capabilities = uint64_t {1} << 62; }
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	if (registry.boot) { return false; }
	registry.boot = snapshot;
	registry.boot_owner = program;
	registry.boot_lifetime = program->boot_credential_lifetime;
	if (program->boot_credential_admission) {
		std::array<uint64_t, 17> auth {};
		if (!program->boot_credential_admission->CopyRecord(program, &auth)) { return false; }
		snapshot.authority = auth[0]; snapshot.capabilities = auth[1];
		registry.boot = snapshot;
		registry.owners.emplace(program, Record {snapshot, true, true, program->boot_credential_lifetime, program->boot_credential_admission});
	}
	return true;
}

bool TryGetCurrentRetainedGuestProcessId(int* out) {
	if (!out) { return false; }
	const auto* program = RuntimeLinker::GetCurrentProgram();
	if (!program) { return false; }
	const auto* owner = program->application_owner ? program->application_owner : program;
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	const auto it = registry.owners.find(owner);
	if (it != registry.owners.end()) {
		if (!it->second.IsActive()) { return false; }
		*out = it->second.identity.pid; return true;
	}
	if (owner == registry.boot_owner && registry.boot && !registry.boot_lifetime.expired()) {
		*out = BootGuestProcessId; return true;
	}
	return false;
}

bool DeactivateRuntimeProcessIdentity(const Program* owner) {
	if (owner == nullptr || (owner->application_owner && owner->application_owner != owner) || owner->application_threads.load() != 0) { return false; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	const auto it = registry.owners.find(owner);
	if (it == registry.owners.end()) { return false; }
	it->second.active = false;
	if (auto admission = it->second.admission.lock()) { admission->Revoke(); }
	return true;
}

int GetCurrentGuestProcessId() {
	const auto* current = RuntimeLinker::GetCurrentProgram();
	const auto* owner = current != nullptr ? current->application_owner : nullptr;
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	std::lock_guard lock(registry.mutex);
	const auto it = registry.owners.find(owner ? owner : current);
	if (it != registry.owners.end()) { return it->second.IsActive() ? it->second.identity.pid : -1; }
	return owner == nullptr ? BootGuestProcessId : -1;
}

bool TryGetGuestProcessIdentity(int64_t pid, GuestProcessIdentity* out) {
	if (out == nullptr) { return false; }
	if (pid == 0) { pid = GetCurrentGuestProcessId(); }
	if (pid <= 0 || pid > INT32_MAX) { return false; }
	Common::HostLockCriticalGuard critical;
	auto& registry = Processes();
	{
		std::lock_guard lock(registry.mutex);
		if (pid == BootGuestProcessId) {
			if (const auto it = registry.owners.find(registry.boot_owner); it != registry.owners.end()) {
				std::array<uint64_t, 17> auth {};
				const auto admission = it->second.admission.lock();
				if (!it->second.IsActive() || !admission || !admission->CopyRecord(registry.boot_owner, &auth)) { return false; }
				auto snapshot = it->second.identity;
				snapshot.authority = auth[0]; snapshot.capabilities = auth[1];
				*out = snapshot; return true;
			}
			if (registry.boot) { *out = *registry.boot; return true; }
		} else {
			for (const auto& [owner, record]: registry.owners) {
				if (record.identity.pid == pid && record.IsActive()) {
					auto snapshot = record.identity;
					if (record.cohost) {
						std::array<uint64_t, 17> auth {};
						const auto admission = record.admission.lock();
						if (admission && admission->CopyRecord(owner, &auth)) { snapshot.authority = auth[0]; snapshot.capabilities = auth[1]; }
					}
					*out = snapshot; return true;
				}
			}
			return false;
		}
	}
	// Preserve the pre-registration boot session. Scope metadata to the boot
	// singleton, never the requesting runtime application's SystemContent.
	auto legacy = Snapshot(nullptr);
	legacy.pid = BootGuestProcessId;
	legacy.authority = UINT64_C(0x480000000000000f);
	legacy.capabilities = uint64_t {1} << 62;
	*out = legacy;
	return true;
}

bool IsGuestProcessIdentityActive(int pid) {
	if (pid == BootGuestProcessId) { return true; }
	if (pid <= 0) { return false; }
	GuestProcessIdentity identity;
	return TryGetGuestProcessIdentity(pid, &identity);
}

bool TryGetGuestProcessCategory(int64_t pid, uint32_t* out) {
	GuestProcessIdentity identity;
	if (out == nullptr || !TryGetGuestProcessIdentity(pid, &identity) || !identity.category) { return false; }
	*out = *identity.category;
	return true;
}

bool TryGetGuestProcessAppId(int64_t pid, int32_t* out) {
	if (out == nullptr) { return false; }
	if (pid == 0 && GetCurrentGuestProcessId() != BootGuestProcessId) {
		GuestProcessIdentity identity;
		if (!TryGetGuestProcessIdentity(0, &identity) || !identity.app_id) { return false; }
		*out = *identity.app_id;
		return true;
	}
	const auto* current = RuntimeLinker::GetCurrentProgram();
	if (pid == 0 && (current == nullptr || current->application_owner == nullptr)) {
		return Libs::LncUtil::Lnc::TryGetCurrentAppId(out);
	}
	// Existing configured direct-game/null-owner behavior resolves its LNC
	// context; runtime PID 100 must never alias the runtime caller's app ID.
	if ((pid == 0 || pid == BootGuestProcessId) && (current == nullptr || current->application_owner == nullptr)) {
		bool configured = false;
		{
			Common::HostLockCriticalGuard critical;
			auto& registry = Processes();
			std::lock_guard lock(registry.mutex);
			configured = registry.boot.has_value();
		}
		if (!configured) { return Libs::LncUtil::Lnc::TryGetCurrentAppId(out); }
	}
	GuestProcessIdentity identity;
	if (!TryGetGuestProcessIdentity(pid, &identity) || !identity.app_id) { return false; }
	*out = *identity.app_id;
	return true;
}

bool TryGetGuestProcessAuthInfo(int64_t pid, std::array<uint64_t, 17>* out) {
	if (TryGetSourceBoundProcessAuthInfo(pid, out)) { return true; }
	const auto* caller = RuntimeLinker::GetCurrentProgram();
	if (caller && caller->boot_credential_admission) { return false; }
	GuestProcessIdentity identity;
	if (out == nullptr || !TryGetGuestProcessIdentity(pid, &identity) || !identity.authority) { return false; }
	std::array<uint64_t, 17> record {};
	record[0] = *identity.authority;
	record[1] = identity.capabilities;
	*out = record;
	return true;
}
} // namespace Loader
