#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/localization.h"
#include "kernel/eventQueue.h"
#include "kernel/fileSystem.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/runtimeUserService.h"
#include "libs/localUserStorage.h"
#include "libs/userServiceProperty.h"
#include "kernel/pthread.h"
#include "loader/elf.h"
#include "loader/runtimeGuestExecution.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeProcessIdentity.h"
#include "libs/loginMgrNativeTransport.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <Windows.h>
#include <io.h>
#undef GetUserName
#else
#include <unistd.h>
#endif

namespace Libs {

LIB_VERSION("UserService", 1, "UserService", 1, 1);

namespace UserService {

struct UserServiceLoginUserIdList {
	int user_id[4];
};

enum UserServiceEventType {
	UserServiceEventTypeLogin, UserServiceEventTypeLogout,
	UserServiceEventTypeCreate = 0x100, UserServiceEventTypeDestroy = 0x101
};

struct SceUserServiceEvent {
	UserServiceEventType event_type;
	int                  user_id;
};

struct UserServiceGamePresets {
	size_t   this_size;
	uint32_t difficulty;
	uint32_t priority;
	uint32_t invert_vertical_view_for_1st_person_view;
	uint32_t invert_horizontal_view_for_1st_person_view;
	uint32_t invert_vertical_view_for_3rd_person_view;
	uint32_t invert_horizontal_view_for_3rd_person_view;
	uint32_t display_sub_titles;
	uint32_t audio_language;
};

static_assert(sizeof(UserServiceGamePresets) == 40);

// ps5-vsh ShellCore boot: LoginMgr wake (threads 12-15 parked, zero HLE calls;
// SceLoginMgrStateFlag/ServerDialog + SceUserServiceCommonEventFlag created but
// never waited-on via HLE; Mbus receive polls no-event; guest never calls
// UserServiceGetEvent). KernelTriggerUserEventForAll exists (kernel/eventQueue)
// so post one login user-event broadcast after first touch (one-shot). The only
// user event registered in boot16.log is DynlibEventEqueue id 0, so use id 1 to
// avoid waking it; queues without id 1 return ENOENT (safe no-op) while any
// future LoginMgr-associated queue with id 1 wakes. Offline identity stays
// Config::GetUserId()/GetUserName() (defaults 1000/Kyty), same as
// libOfflineLogin.cpp — never duplicated here. Null-safe: no guest pointers.
namespace {
constexpr int kLoginUserEventId = 1;
bool          g_login_wake_posted = false;

// ps5-vsh (stage 1, KYTY_USER_STORE): persistent user store. When the guest
// environment carries KYTY_USER_STORE (a host path to a JSON file, passed via
// --guest-env), users are records with per-user fields instead of the
// Config::GetUserId() fabrication: boot is logged-out, login state is
// list-based, and GetEvent serves a real queue. When unset, every function
// below takes the legacy path (byte-for-byte current behavior).
struct StoredUser {
	int         id = -1;
	std::string name;
	std::string passcode;
	bool        auto_login       = false;
	uint64_t    account_id       = 0;
	std::string signin_id;
	int         login_flag       = 0;
	uint32_t    user_status      = 0; // UserService property 0x82b, distinct from login flag (6).
	int         last_login_order = 0;
};

Loader::GuestExecutionMutex      g_store_mutex;
bool                            g_store_ready = false;
bool                            g_store_mode  = false;
bool                            g_store_writable = true;
std::string                     g_store_path;
std::vector<StoredUser>         g_store_users;
int                             g_store_next_user_number = 1;
int                             g_store_next_guest_number = 1;
std::vector<int>                g_store_logged_in; // user ids in login order (max 4)
std::array<int, 4>              g_store_login_slots {-1, -1, -1, -1}; // Stable 1-based numbers, not list indices.
LocalUserStorage::Layout        g_store_storage;
bool                           g_store_storage_ready = false;
int                             g_store_foreground  = -1;
int                             g_store_login_order = 0;
std::deque<SceUserServiceEvent> g_store_events;

struct OwnerIdentity {
	int id;
	std::string name;
	uint64_t account_id = 0;
	int last_login_order = 0;
	bool auto_login = false;
};
struct OwnerSubscription {
	std::shared_ptr<Loader::RuntimeGuestExecution> execution;
	bool initialized = false;
	bool failed = false;
	bool store_mode = false;
	int initial_user = -1; // Launch identity, not a later foreground/user-list query.
	std::vector<OwnerIdentity> identities;
	std::vector<int> logged_in;
	std::array<int, 4> login_slots {-1, -1, -1, -1};
	std::deque<SceUserServiceEvent> events;
};
// Protected by g_store_mutex, including store mutation + fanout. The globally
// non-reused execution token is essential: Program mappings can be retained.
std::map<uint64_t, OwnerSubscription> g_owner_subscriptions;

#include "userServiceCallbacks.inc"

static bool IsPlayerLoggedInForOwner(int player) {
	return !Libs::NoForegroundUser() &&
	       (player == 0 || (player > 0 && Controller::IsPlayerConnected(player)));
}

static void ApplyOwnerEventLocked(OwnerSubscription& owner, const SceUserServiceEvent& event) {
	if (owner.failed || owner.execution->StopRequested()) { return; }
	try {
		if (event.event_type == UserServiceEventTypeLogin) {
			owner.logged_in.push_back(event.user_id);
		} else {
			std::erase(owner.logged_in, event.user_id);
		}
		owner.events.push_back(event);
		if (owner.store_mode) { owner.login_slots = g_store_login_slots; }
	} catch (const std::bad_alloc&) {
		// Never silently lose a transition and report a coherent subscription.
		owner.failed = true;
	}
}

static void FanoutStoreEventLocked(const SceUserServiceEvent& event) {
	QueueLocalCallbacks(event);
	for (auto& [token, owner]: g_owner_subscriptions) {
		if (!owner.store_mode || owner.failed || owner.execution->StopRequested()) { continue; }
		// An application prepared before creation learns the new identity when
		// that user actually logs in. Existing launch snapshots remain stable.
		try {
			if (event.event_type == UserServiceEventTypeLogin &&
			    std::none_of(owner.identities.begin(), owner.identities.end(),
			        [&](const auto& identity) { return identity.id == event.user_id; })) {
				const auto record = std::find_if(g_store_users.begin(), g_store_users.end(),
				    [&](const auto& user) { return user.id == event.user_id; });
				if (record == g_store_users.end()) { owner.failed = true; continue; }
				owner.identities.push_back({record->id, record->name, record->account_id, record->last_login_order, record->auto_login});
			}
			if (event.event_type == UserServiceEventTypeLogin) {
				const auto record = std::find_if(g_store_users.begin(), g_store_users.end(), [&](const auto& user) { return user.id == event.user_id; });
				for (auto& identity: owner.identities) {
					if (identity.id == event.user_id && record != g_store_users.end()) { identity.last_login_order = record->last_login_order; }
				}
			}
			ApplyOwnerEventLocked(owner, event);
		} catch (const std::bad_alloc&) { owner.failed = true; }
	}
}

// Controller changes have no UserService callback. Sampling from ANY relevant
// session/owner query fans changes to ALL subscribers, not just the polling app.
// Like the legacy poll API, a connect/disconnect wholly between polls is unseen.
static void RefreshControllerOwnersLocked() {
	if (g_store_mode) { return; }
	for (int player = 0; player < Config::MAX_PLAYERS; ++player) {
		const bool connected = IsPlayerLoggedInForOwner(player);
		const int id = Config::GetPlayerUserId(player);
		for (auto& [token, owner]: g_owner_subscriptions) {
			if (owner.store_mode) { continue; }
			const bool logged = std::find(owner.logged_in.begin(), owner.logged_in.end(), id) != owner.logged_in.end();
			if (connected != logged) {
				ApplyOwnerEventLocked(owner, {connected ? UserServiceEventTypeLogin : UserServiceEventTypeLogout, id});
			}
		}
	}
}

// An explicitly application-owned caller must NEVER fall through to session
// behavior, even if its execution or preparation is absent/retired.
static Loader::Program* CallingApplicationOwner() {
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	return program ? program->application_owner : nullptr;
}

static OwnerSubscription* FindOwnerLocked(Loader::Program* owner) {
	const auto execution = Loader::GetApplicationExecution(owner);
	if (!execution || execution->StopRequested()) { return nullptr; }
	const auto key = execution->Owner();
	const auto found = g_owner_subscriptions.find(key.token);
	if (found == g_owner_subscriptions.end() || found->second.execution != execution || key.identity != owner) {
		return nullptr;
	}
	return &found->second;
}

static int OwnerStatus(const OwnerSubscription* owner) {
	if (!owner || !owner->initialized) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	return owner->failed ? USER_SERVICE_ERROR_NO_MEMORY : OK;
}

static int InitializeOwner(Loader::Program* application) {
	std::lock_guard lock(g_store_mutex);
	auto* owner = FindOwnerLocked(application);
	if (!owner) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	if (owner->failed) { return USER_SERVICE_ERROR_NO_MEMORY; }
	if (owner->initialized) { return USER_SERVICE_ERROR_ALREADY_INITIALIZED; }
	RefreshControllerOwnersLocked();
	if (owner->failed) { return USER_SERVICE_ERROR_NO_MEMORY; }
	try {
		std::deque<SceUserServiceEvent> events;
		for (int user: owner->logged_in) { events.push_back({UserServiceEventTypeLogin, user}); }
		owner->events.swap(events);
	} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
	owner->initialized = true;
	return OK;
}

static bool IsGuestRecord(int id) {
	return (static_cast<uint32_t>(id) & 0xe0000000u) == 0x20000000u;
}

// Caller holds g_store_mutex. Serialize before opening anything, then flush a
// new sibling and atomically replace the destination. Failed saves preserve
// the previous file and let callers keep the previous in-memory state.
static bool SaveStoreSnapshotLocked(const std::vector<StoredUser>& records, int next_number) {
	if (g_store_path.empty() || !g_store_writable) {
		return false;
	}
	try {
	nlohmann::json users = nlohmann::json::array();
	for (const auto& u : records) {
		if (IsGuestRecord(u.id)) { continue; }
		users.push_back({{"id", u.id},
		                 {"name", u.name},
		                 {"passcode", u.passcode},
		                 {"auto_login", u.auto_login},
		                 {"account_id", u.account_id},
		                 {"signin_id", u.signin_id},
		                 {"login_flag", u.login_flag},
		                 {"user_status", u.user_status},
		                 {"last_login_order", u.last_login_order}});
	}
	const nlohmann::json root = {
	    {"version", 1}, {"next_user_number", next_number}, {"users", users}};
	const auto text = root.dump(2);
	static std::atomic_uint64_t sequence {0};
	const std::filesystem::path destination(g_store_path);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto pid = GetCurrentProcessId();
#else
	const auto pid = getpid();
#endif
	auto temporary = destination;
	temporary += ".pending-" + std::to_string(pid) + "-" + std::to_string(++sequence);
	struct Pending {
		std::filesystem::path path;
		FILE* file = nullptr;
		bool owned = false;
		~Pending() {
			if (file) { std::fclose(file); }
			if (owned) { std::error_code error; std::filesystem::remove(path, error); }
		}
	} pending {temporary};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	pending.file = _wfopen(temporary.c_str(), L"wbx");
#else
	pending.file = std::fopen(temporary.c_str(), "wbx");
#endif
	if (!pending.file) { return false; }
	pending.owned = true;
	if (std::fwrite(text.data(), 1, text.size(), pending.file) != text.size() || std::fflush(pending.file) != 0) { return false; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_commit(_fileno(pending.file)) != 0) { return false; }
#else
	if (fsync(fileno(pending.file)) != 0) { return false; }
#endif
	const int closed = std::fclose(pending.file);
	pending.file = nullptr;
	if (closed != 0) { return false; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { return false; }
#else
	std::error_code error;
	std::filesystem::rename(temporary, destination, error);
	if (error) { return false; }
#endif
	pending.owned = false;
	return true;
	} catch (...) {
		return false;
	}
}

static bool SaveStoreLocked() {
	const bool saved = SaveStoreSnapshotLocked(g_store_users, g_store_next_user_number);
	if (!saved) { LOGF("\t userservice-store: save FAILED path=%s\n", g_store_path.c_str()); }
	return saved;
}

// Caller must hold g_store_mutex. False on missing or unreadable/invalid file.
static bool TryLoadStoreLocked() {
	std::ifstream in(g_store_path, std::ios::binary);
	if (!in) {
		return false;
	}
	try {
		const std::string  text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		nlohmann::json root = nlohmann::json::parse(text);
		if (!root.is_object() || root.value("version", 0) != 1) {
			return false;
		}
		const auto users_it = root.find("users");
		if (users_it == root.end() || !users_it->is_array()) {
			return false;
		}
		std::vector<StoredUser> users;
		for (const auto& j : *users_it) {
			StoredUser u;
			u.id               = j.value("id", -1);
			u.name             = j.value("name", std::string());
			u.passcode         = j.value("passcode", std::string());
			u.auto_login       = j.value("auto_login", false);
			u.account_id       = j.value("account_id", uint64_t(0));
			u.signin_id        = j.value("signin_id", std::string());
			u.login_flag       = j.value("login_flag", 0);
			u.user_status      = j.value("user_status", uint32_t {0});
			u.last_login_order = j.value("last_login_order", 0);
			if (u.id < 0 || u.name.empty() || IsGuestRecord(u.id) || users.size() >= 16 ||
			    std::any_of(users.begin(), users.end(), [&](const auto& existing) { return existing.id == u.id; })) {
				return false;
			}
			users.push_back(std::move(u));
		}
		const int next_user_number = root.value("next_user_number", 1);
		if (next_user_number < 1 || next_user_number > 0x0eff0000) { return false; }
		g_store_users            = std::move(users);
		g_store_next_user_number = next_user_number;
		g_store_logged_in.clear();
		g_store_login_slots.fill(-1);
		g_store_foreground = -1;
		g_store_events.clear();
		g_store_login_order = 0;
		for (const auto& u : g_store_users) {
			g_store_login_order = std::max(g_store_login_order, u.last_login_order);
		}
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

// Same gating pattern as TraceUserContext in libKernel.cpp: the launcher passes
// KYTY_USER_STORE via --guest-env, so read Config::GetGuestEnvironment() once.
static void LogStorageResult(const char* operation, int user, const LocalUserStorage::Result& result) {
	LOGF("userservice-storage: %s user=0x%x code=%d recovery=%d path=%s error=%s\n", operation, user,
	     static_cast<int>(result.code), result.recovery_required, result.path.string().c_str(), result.error.message().c_str());
}

static void RollbackStorageLocked(const char* operation, int user, LocalUserStorage::Transaction& transaction,
                                  const LocalUserStorage::Result& failure) {
	LogStorageResult(operation, user, failure);
	if (transaction.Pending()) {
		const auto rollback = LocalUserStorage::Rollback(transaction);
		LogStorageResult("rollback", user, rollback);
		if (!rollback) { g_store_writable = false; }
	} else if (failure.recovery_required) {
		// A durable write may be ambiguous even before Transaction is active.
		g_store_writable = false;
	}
}

static bool InitializeStorageLocked() try {
	const char* paths[] {"/user/home", "/system_data/priv/home", "/system_data/eap/rodata/home"};
	for (size_t i = 0; i < 3; ++i) {
		const auto resolved = LibKernel::FileSystem::ResolveGuestPath(paths[i]);
		if (resolved.error || resolved.unmounted_fallback || resolved.read_only || resolved.host.empty()) {
			LOGF("userservice-storage: required writable mounted root unavailable: %s\n", paths[i]);
			return false;
		}
		g_store_storage.roots[i] = resolved.host;
	}
	g_store_storage.journal = std::filesystem::path(g_store_path).parent_path() / "local-user-storage";
	std::error_code error;
	std::filesystem::create_directory(g_store_storage.journal, error);
	if (error) { return false; }
	std::vector<uint32_t> committed;
	for (const auto& record: g_store_users) { committed.push_back(static_cast<uint32_t>(record.id)); }
	const auto recovered = LocalUserStorage::Recover(g_store_storage, committed);
	LogStorageResult("startup recovery", -1, recovered);
	if (!recovered) { return false; }
	// Upgrade records made by the earlier record-only backend, but never adopt
	// somebody else's tree. This runs before any local profile consumer starts.
	for (const auto& record: g_store_users) {
		char id[9];
		std::snprintf(id, sizeof(id), "%08x", record.id);
		size_t present = 0;
		for (const auto& root: g_store_storage.roots) {
			const bool exists = std::filesystem::exists(root / id, error);
			if (error) { return false; }
			if (exists) {
				if (!std::filesystem::is_directory(root / id, error) || error) { return false; }
				++present;
			}
		}
		if (present == 3) { continue; } // Existing unmanaged trees stay unmanaged.
		if (present != 0) { return false; } // Partial/unmanaged layout requires explicit repair.
		LocalUserStorage::Transaction upgrade;
		const auto prepared = LocalUserStorage::PrepareUpgrade(g_store_storage, record.id, upgrade);
		if (!prepared) { RollbackStorageLocked("legacy provisioning failed", record.id, upgrade, prepared); return false; }
		const auto committed_storage = LocalUserStorage::Commit(upgrade);
		LogStorageResult("legacy record provisioned", record.id, committed_storage);
		if (!committed_storage) { return false; }
	}
	return true;
} catch (...) { return false; }

static void EnsureStoreInitialized() {
	std::lock_guard lock(g_store_mutex);
	if (g_store_ready) {
		return;
	}
	g_store_ready = true;
	const auto& environment = Config::GetGuestEnvironment();
	const auto  flag        = environment.find("KYTY_USER_STORE");
	if (flag == environment.end() || flag->second.empty()) {
		return;
	}
	g_store_mode = true;
	g_store_path = flag->second;
	if (TryLoadStoreLocked()) {
		LOGF("\t userservice-store: loaded %llu user(s) path=%s\n",
		     static_cast<unsigned long long>(g_store_users.size()), g_store_path.c_str());
	} else {
		std::error_code ec;
		const bool exists = std::filesystem::exists(g_store_path, ec);
		if (ec || exists) {
			g_store_writable = false;
			LOGF("\t userservice-store: unreadable or invalid; preserving path=%s\n", g_store_path.c_str());
		} else {
			LOGF("\t userservice-store: no profiles (store absent) path=%s\n", g_store_path.c_str());
		}
	}
	if (LocalAccountHleEnabled() && g_store_writable) {
		g_store_storage_ready = InitializeStorageLocked();
		if (!g_store_storage_ready) { g_store_writable = false; }
	}
}

static bool InStoreMode() {
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	return g_store_mode;
}

static const StoredUser* FindRecordLocked(int user_id) {
	for (const auto& u : g_store_users) {
		if (u.id == user_id) {
			return &u;
		}
	}
	return nullptr;
}

static StoredUser* FindRecordMutLocked(int user_id) {
	for (auto& u : g_store_users) {
		if (u.id == user_id) {
			return &u;
		}
	}
	return nullptr;
}

// Shared session transitions, used by Mbus and the selected local UserService
// exports. Creation alone never enters this path.
static int StoreLoginUser(int user_id) try {
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) {
		return USER_SERVICE_ERROR_NOT_INITIALIZED;
	}
	StoredUser* record = FindRecordMutLocked(user_id);
	if (record == nullptr) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	for (const int logged : g_store_logged_in) {
		if (logged == user_id) {
			return OK; // idempotent re-login
		}
	}
	if (g_store_logged_in.size() >= 4) {
		return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED;
	}
	const auto free_slot = std::find(g_store_login_slots.begin(), g_store_login_slots.end(), -1);
	if (free_slot == g_store_login_slots.end()) { return USER_SERVICE_ERROR_INTERNAL; }
	if (!g_store_writable || g_store_login_order == INT32_MAX) { return USER_SERVICE_ERROR_INTERNAL; }
	auto logged_in = g_store_logged_in;
	auto events = g_store_events;
	logged_in.push_back(user_id);
	events.push_back({UserServiceEventTypeLogin, user_id});
	const int previous_order = record->last_login_order;
	record->last_login_order = g_store_login_order + 1;
	if (!IsGuestRecord(user_id) && !SaveStoreLocked()) {
		record->last_login_order = previous_order;
		return USER_SERVICE_ERROR_INTERNAL;
	}
	g_store_logged_in.swap(logged_in);
	*free_slot = user_id;
	g_store_events.swap(events);
	if (g_store_foreground == -1) {
		g_store_foreground = user_id;
	}
	++g_store_login_order;
	FanoutStoreEventLocked({UserServiceEventTypeLogin, user_id});
	LOGF("\t userservice-store: login uid=%d foreground=%d order=%d\n", user_id, g_store_foreground,
	     record->last_login_order);
	return OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

static int StoreLogoutUser(int user_id) try {
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) {
		return USER_SERVICE_ERROR_NOT_INITIALIZED;
	}
	if (FindRecordMutLocked(user_id) == nullptr) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	auto logged_in = g_store_logged_in;
	auto events = g_store_events;
	if (!std::erase(logged_in, user_id)) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	events.push_back({UserServiceEventTypeLogout, user_id});
	g_store_logged_in.swap(logged_in);
	std::replace(g_store_login_slots.begin(), g_store_login_slots.end(), user_id, -1);
	g_store_events.swap(events);
	if (g_store_foreground == user_id) {
		g_store_foreground = -1;
	}
	FanoutStoreEventLocked({UserServiceEventTypeLogout, user_id});
	if (IsGuestRecord(user_id)) {
		std::erase_if(g_store_users, [user_id](const auto& user) { return user.id == user_id; });
		LOGF("userservice-storage: guest user=0x%x retired logically; receipt retained for quiescent startup cleanup\n", user_id);
		QueueLocalCallbacks({UserServiceEventTypeDestroy, user_id});
	}
	LOGF("\t userservice-store: logout uid=%d foreground=%d\n", user_id, g_store_foreground);
	return OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

void MaybePostLoginWake() {
	if (InStoreMode()) {
		return; // store mode: nobody is logged in at boot, so no wake
	}
	if (Libs::NoForegroundUser()) {
		return;
	}
	if (g_login_wake_posted) {
		return;
	}
	g_login_wake_posted = true;
	const int rc = LibKernel::EventQueue::KernelTriggerUserEventForAll(kLoginUserEventId, nullptr);
	LOGF("\t userservice-login-wake: broadcast id=%d rc=0x%08x uid=%d (one-shot)\n",
	     kLoginUserEventId, static_cast<uint32_t>(rc), static_cast<int>(Config::GetUserId()));
}

// ps5-vsh: native user-recovery coherence. The guest's native code (in
// SceShellCore.elf) recovers registered users by reading the guest file
// /system_data/priv/home/user_list.dat directly (open, lseek 68, read 4):
// ShellCore reader 0x9f0010 confirms 16 IDs, a maximum-used home ID at +64,
// and a saved group index at +68. The legacy guard conservatively requires
// both the IDs and the high-water word to be -1 before touching it. The normal
// empty file has high-water 0x0fffffff and therefore does not pass this guard.
// When every checked word is empty the
// native side finds zero users while this HLE reports the Config user, so
// seed slot 0 with Config::GetUserId() once, leaving the rest of the file
// byte-identical. Only the legacy configured-user mode can use that identity;
// store-backed or no-foreground launches must not fabricate a native user.
namespace {
constexpr char   kNativeUserListPath[]  = "/system_data/priv/home/user_list.dat";
constexpr size_t kNativeUserListSize    = 72;
constexpr size_t kNativeUserListSlotsEnd = 68; // legacy guard includes high-water metadata
std::once_flag g_userlist_seed_once;
} // namespace

static void MaybeSeedNativeUserList() {
	std::call_once(g_userlist_seed_once, [] {
		if (Libs::NoForegroundUser() || InStoreMode()) {
			return;
		}
		const int fd = LibKernel::FileSystem::KernelOpen(kNativeUserListPath, 0, 0);
		if (fd < 0) {
			LOGF("\t userservice-userlist: not seedable (open rc=%d)\n", fd);
			return;
		}
		uint8_t buf[kNativeUserListSize] = {};
		size_t  total                    = 0;
		while (total < sizeof(buf)) {
			const int64_t n = LibKernel::FileSystem::KernelPread(
			    fd, buf + total, sizeof(buf) - total, static_cast<int64_t>(total));
			if (n <= 0) {
				break;
			}
			total += static_cast<size_t>(n);
		}
		LibKernel::FileSystem::KernelClose(fd);
		if (total < sizeof(buf)) {
			LOGF("\t userservice-userlist: not seedable (short/unreadable %llu/72)\n",
			     static_cast<unsigned long long>(total));
			return;
		}
		for (size_t off = 0; off < kNativeUserListSlotsEnd; off += 4) {
			uint32_t slot = 0;
			std::memcpy(&slot, buf + off, sizeof(slot));
			if (slot != 0xFFFFFFFFu) {
				return; // already populated: leave byte-identical, stay silent
			}
		}
		const auto uid = static_cast<uint32_t>(Config::GetUserId());
		std::memcpy(buf, &uid, sizeof(uid));
		// O_RDWR(2) | O_CREAT(0x200); no O_TRUNC: full-buffer rewrite below
		// keeps every other byte identical. The parent dir may not exist;
		// KernelOpen creates it on O_CREAT (see fileSystem.cpp), else this
		// fails and we give up silently.
		const int wfd = LibKernel::FileSystem::KernelOpen(kNativeUserListPath, 0x202, 0666);
		if (wfd < 0) {
			LOGF("\t userservice-userlist: not seedable (reopen rc=%d)\n", wfd);
			return;
		}
		const int64_t w = LibKernel::FileSystem::KernelPwrite(wfd, buf, sizeof(buf), 0);
		LibKernel::FileSystem::KernelClose(wfd);
		if (w != static_cast<int64_t>(sizeof(buf))) {
			LOGF("\t userservice-userlist: not seedable (short write %lld/72)\n",
			     static_cast<long long>(w));
			return;
		}
		LOGF("\t userservice-userlist: seeded slot0 uid=%d\n", static_cast<int>(uid));
	});
}
} // namespace

int PrepareUserServiceOwner(Loader::Program* application) noexcept {
	try {
		if (!application || application->application_owner != application) {
			return USER_SERVICE_ERROR_INVALID_ARGUMENT;
		}
		const auto execution = Loader::GetApplicationExecution(application);
		if (!execution) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		auto lease = execution->TryAcquire(Loader::GuestWorkKind::HostCall);
		if (!lease) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		std::lock_guard lock(g_store_mutex);
		if (!g_store_ready) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		const auto token = execution->Owner().token;
		if (g_owner_subscriptions.contains(token)) { return USER_SERVICE_ERROR_ALREADY_INITIALIZED; }
		RefreshControllerOwnersLocked();
		OwnerSubscription owner;
		owner.execution = execution;
		owner.store_mode = g_store_mode;
		if (g_store_mode) {
			for (const auto& record: g_store_users) {
				owner.identities.push_back({record.id, record.name, record.account_id, record.last_login_order, record.auto_login});
			}
			owner.logged_in = g_store_logged_in;
			owner.login_slots = g_store_login_slots;
			if (std::find(owner.logged_in.begin(), owner.logged_in.end(), g_store_foreground) != owner.logged_in.end()) {
				owner.initial_user = g_store_foreground;
			}
		} else {
			for (int player = 0; player < Config::MAX_PLAYERS; ++player) {
				const int id = Config::GetPlayerUserId(player);
				owner.identities.push_back({id, player == 0 ? Config::GetUserName() :
				    std::string(Common::Localization::Get(Config::GetConsoleLanguage(), "user.guest")) + " " + std::to_string(player + 1)});
				if (IsPlayerLoggedInForOwner(player)) { owner.logged_in.push_back(id); }
			}
			if (IsPlayerLoggedInForOwner(0)) { owner.initial_user = Config::GetUserId(); }
		}
		for (const int id: owner.logged_in) { owner.events.push_back({UserServiceEventTypeLogin, id}); }
		// Stop/fail cannot race publication into an apparently prepared owner.
		return lease.Publish([&] { g_owner_subscriptions.emplace(token, std::move(owner)); }) ? OK :
		    USER_SERVICE_ERROR_NOT_INITIALIZED;
	} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
	catch (...) { return USER_SERVICE_ERROR_INTERNAL; }
}

int RetireUserServiceOwner(Loader::Program* application) noexcept {
	try {
		if (!application || application->application_owner != application) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
		const auto execution = Loader::GetApplicationExecution(application);
		if (!execution) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		if (!execution->StopRequested() || execution->Outstanding() != 0 || application->application_threads.load() != 0) {
			return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED;
		}
		std::lock_guard lock(g_store_mutex);
		const auto found = g_owner_subscriptions.find(execution->Owner().token);
		if (found == g_owner_subscriptions.end() || found->second.execution != execution) {
			return USER_SERVICE_ERROR_NOT_INITIALIZED;
		}
		g_owner_subscriptions.erase(found);
		return OK;
	} catch (...) { return USER_SERVICE_ERROR_INTERNAL; }
}

// Stage-2 cross-TU entry points (declared in libs/libs.h). Thin forwarders
// over the stage-1 statics above; mutex discipline and persistence live
// there. Callers (Mbus HLEs in libC.cpp) gate on UserServiceStoreEnabled()
// so the legacy path never reaches StoreLoginUser/StoreLogoutUser.
bool UserServiceStoreEnabled() {
	return InStoreMode();
}

bool LocalAccountHleEnabled() {
	const auto& environment = Config::GetGuestEnvironment();
	const auto selection = environment.find("KYTY_NP_LOCAL_ACCOUNT_HLE");
	return selection != environment.end() && selection->second == "1";
}

int CreateLocalUser(std::string_view name, int type, bool incomplete, int* user) try {
	if (!LocalAccountHleEnabled() || CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (g_dispatching_user_callback) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	LocalSessionTransition transition;
	// Original LoginMgr table: 0/1/6 -> home, 2/3/4 -> guest, 5 -> a
	// separate special-user path which this local profile implementation rejects.
	if (!user || name.empty() || name.size() > 16 || name.find('\0') != std::string_view::npos || type < 0 || type > 6) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (type == 5) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	EnsureStoreInitialized();
	std::unique_lock lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	if (!g_store_writable) { return USER_SERVICE_ERROR_INTERNAL; }
	const bool guest = type >= 2 && type <= 4;
	const auto count = std::count_if(g_store_users.begin(), g_store_users.end(),
	    [guest](const auto& record) { return IsGuestRecord(record.id) == guest; });
	if (count >= (guest ? 4 : 16)) { return static_cast<int>(0x80960102u); }
	int number = guest ? g_store_next_guest_number : g_store_next_user_number;
	const int base = guest ? 0x20010000 : 0x10010000;
	while (number <= 0x0eff0000 && FindRecordLocked(base + number - 1)) { ++number; }
	if (number >= 0x0eff0000) { return static_cast<int>(0x80960102u); }
	StoredUser record;
	record.id = base + number - 1;
	record.name = name;
	record.login_flag = incomplete ? 1 : 0;
	auto records = g_store_users;
	records.push_back(record);
	if (!g_store_storage_ready) { return USER_SERVICE_ERROR_INTERNAL; }
	LocalUserStorage::Transaction storage;
	const auto prepared = LocalUserStorage::PrepareCreate(g_store_storage, record.id,
	    guest ? LocalUserStorage::Lifetime::Guest : LocalUserStorage::Lifetime::Persistent, storage);
	if (!prepared) {
		RollbackStorageLocked("create preparation failed", record.id, storage, prepared);
		return USER_SERVICE_ERROR_INTERNAL;
	}
	if (!guest && !SaveStoreSnapshotLocked(records, number + 1)) {
		RollbackStorageLocked("record save failed", record.id, storage, {});
		return USER_SERVICE_ERROR_INTERNAL;
	}
	g_store_users.swap(records);
	(guest ? g_store_next_guest_number : g_store_next_user_number) = number + 1;
	// The authoritative publication is committed; a receipt fault must not be
	// reported as a failed save or undo the record. Recovery retains ownership.
	const auto committed_storage = LocalUserStorage::Commit(storage);
	LogStorageResult("create committed", record.id, committed_storage);
	if (!committed_storage) { g_store_writable = false; }
	*user = record.id;
	QueueLocalCallbacks({UserServiceEventTypeCreate, record.id});
	LOGF("userservice-local: created uid=0x%x type=%d incomplete=%d persistent=%d\n", record.id, type, incomplete, !guest);
	// Only login/logout have native callback completion gates. Creation posts
	// after publication and returns; its event precedes the next login in FIFO.
	return OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

int DestroyLocalUser(int user) try {
	if (!LocalAccountHleEnabled() || CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (g_dispatching_user_callback) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	LocalSessionTransition transition;
	EnsureStoreInitialized();
	std::unique_lock lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	if (!FindRecordLocked(user)) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	if (!g_store_writable) { return USER_SERVICE_ERROR_INTERNAL; }
	auto records = g_store_users;
	std::erase_if(records, [user](const auto& record) { return record.id == user; });
	auto logged_in = g_store_logged_in;
	auto events = g_store_events;
	const bool was_logged = std::erase(logged_in, user) != 0;
	if (was_logged) { events.push_back({UserServiceEventTypeLogout, user}); }
	if (!IsGuestRecord(user) && !SaveStoreSnapshotLocked(records, g_store_next_user_number)) {
		return USER_SERVICE_ERROR_INTERNAL;
	}
	g_store_logged_in.swap(logged_in);
	std::replace(g_store_login_slots.begin(), g_store_login_slots.end(), user, -1);
	g_store_events.swap(events);
	if (was_logged) {
		FanoutStoreEventLocked({UserServiceEventTypeLogout, user});
	}
	if (g_store_foreground == user) { g_store_foreground = -1; }
	g_store_users.swap(records);
	// Even a creation callback can open databases. A complete consumer drain is
	// not implemented, so retain owned storage until next-process recovery.
	LOGF("userservice-storage: deleted user=0x%x cleanup deferred until quiescent startup\n", user);
	QueueLocalCallbacks({UserServiceEventTypeDestroy, user});
	LOGF("userservice-local: destroyed uid=0x%x\n", user);
	lock.unlock();
	return was_logged ? WaitLocalCallbacks(OK) : OK;
} catch (const std::bad_alloc&) { return USER_SERVICE_ERROR_NO_MEMORY; }
catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

int SetLocalLoginFlag(int user, uint32_t mask, uint32_t value, uint32_t* output) try {
	if (!LocalAccountHleEnabled() || CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (!output) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	auto* record = FindRecordMutLocked(user);
	if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	const int previous = record->login_flag;
	const uint32_t next = (static_cast<uint32_t>(previous) & ~mask) | value;
	record->login_flag = static_cast<int>(next);
	if (!IsGuestRecord(user) && !SaveStoreLocked()) {
		record->login_flag = previous;
		return USER_SERVICE_ERROR_INTERNAL;
	}
	*output = next;
	return OK;
} catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

int GetLocalLoginFlag(int user, uint32_t* output) {
	if (!LocalAccountHleEnabled()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (!output) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	if (CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	const auto* record = FindRecordLocked(user);
	if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	*output = static_cast<uint32_t>(record->login_flag);
	return OK;
}

int SetLocalUserStatus(int user, uint32_t mask, uint32_t value, uint32_t* output) try {
	if (!LocalAccountHleEnabled() || CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (!output) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	auto* record = FindRecordMutLocked(user);
	if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	const auto previous = record->user_status;
	const auto next = (previous & ~mask) | value;
	record->user_status = next;
	if (!IsGuestRecord(user) && !SaveStoreLocked()) {
		record->user_status = previous;
		return USER_SERVICE_ERROR_INTERNAL;
	}
	*output = next;
	LOGF("userservice-local: status uid=0x%x mask=0x%x value=0x%x result=0x%x\n", user, mask, value, next);
	return OK;
} catch (...) { return USER_SERVICE_ERROR_INTERNAL; }

int GetLocalUserStatus(int user, uint32_t* output) {
	if (!LocalAccountHleEnabled() || CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	if (!output) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	const auto* record = FindRecordLocked(user);
	if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	*output = record->user_status;
	return OK;
}

static int KYTY_SYSV_ABI UserServiceGetUserStatus(int user, uint32_t* output) {
	uint32_t value = 0;
	const int result = GetLocalUserStatus(user, &value);
	if (result) { return result; }
	return LibKernel::FileSystem::CopyPathResultToGuest(output, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
}

int GetStoredPasscode(int user, char* output, size_t size) {
	if (!output || size == 0) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	if (CallingApplicationOwner()) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (!g_store_mode) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	const auto* record = FindRecordLocked(user);
	if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	if (record->passcode.size() >= size) { return USER_SERVICE_ERROR_BUFFER_TOO_SHORT; }
	return LibKernel::FileSystem::CopyPathResultToGuest(output, record->passcode.c_str(), record->passcode.size() + 1) ?
	    OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
}

int QueryLocalAccountId(int user, uint64_t* account_id) {
	if (!account_id) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	if (auto* application = CallingApplicationOwner()) {
		std::lock_guard lock(g_store_mutex);
		const auto* owner = FindOwnerLocked(application);
		if (const int status = OwnerStatus(owner); status != OK) { return status; }
		for (const auto& identity: owner->identities) {
			if (identity.id == user) { *account_id = identity.account_id; return OK; }
		}
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	if (g_store_mode) {
		if (!g_store_writable) { return USER_SERVICE_ERROR_INTERNAL; }
		const auto* record = FindRecordLocked(user);
		if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
		*account_id = record->account_id;
		return OK;
	}
	// Configured controller profiles are local identities with no NP enrollment.
	// Only identities present in UserService's current login model are exposed.
	const int player = Config::GetPlayerFromUserId(user);
	if (player < 0 || !IsPlayerLoggedInForOwner(player)) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
	*account_id = 0;
	return OK;
}

// Shared with the private UserService exports in libJscdUser.cpp.
int GetStoredScalar(int user, uint32_t property, uint32_t* output) {
	if (!output) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	if (property != 3 && property != 5) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	uint32_t value = 0;
	if (auto* application = CallingApplicationOwner()) {
		const auto* owner = FindOwnerLocked(application);
		if (const int result = OwnerStatus(owner)) { return result; }
		const auto identity = std::find_if(owner->identities.begin(), owner->identities.end(),
		    [&](const auto& identity) { return identity.id == user; });
		if (identity == owner->identities.end()) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
		if (!owner->store_mode) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
		value = property == 3 ? static_cast<uint32_t>(identity->auto_login) : static_cast<uint32_t>(identity->last_login_order);
	} else {
		if (!g_store_mode) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
		const auto* record = FindRecordLocked(user);
		if (!record) { return USER_SERVICE_ERROR_NOT_LOGGED_IN; }
		value = property == 3 ? static_cast<uint32_t>(record->auto_login) : static_cast<uint32_t>(record->last_login_order);
	}
	return LibKernel::FileSystem::CopyPathResultToGuest(output, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
}

int UserServiceStoreGetRegisteredUsers(int* user_ids) {
	if (!user_ids) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	// 13.60 registered-user ABI includes 16 home slots and 4 guest slots.
	std::fill_n(user_ids, 20, -1);
	size_t home = 0, guest = 16;
	for (const auto& record: g_store_users) {
		if (IsGuestRecord(record.id)) {
			if (guest < 20) { user_ids[guest++] = record.id; }
		} else if (home < 16) {
			user_ids[home++] = record.id;
		}
	}
	return OK;
}

int UserServiceStoreGetForegroundUser(int* user_id) {
	EnsureStoreInitialized();
	std::lock_guard lock(g_store_mutex);
	*user_id = g_store_foreground;
	return OK;
}

int UserServiceStoreLogin(int user_id) {
	if (g_dispatching_user_callback) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	LocalSessionTransition transition;
	return WaitLocalCallbacks(StoreLoginUser(user_id));
}

int UserServiceStoreLogout(int user_id) {
	if (g_dispatching_user_callback) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	LocalSessionTransition transition;
	return WaitLocalCallbacks(StoreLogoutUser(user_id));
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceInitialize(const void* /*params*/) {
	PRINT_NAME();
	if (auto* owner = CallingApplicationOwner()) { return InitializeOwner(owner); }

	MaybePostLoginWake();
	MaybeSeedNativeUserList();

	return OK;
}

// bwFjS+bX9mA is no-argument termination (+0x24d0), not GetUserNumber.
static int KYTY_SYSV_ABI UserServiceTerminate() {
	if (auto* application = CallingApplicationOwner()) {
		std::lock_guard lock(g_store_mutex);
		auto* owner = FindOwnerLocked(application);
		if (const int result = OwnerStatus(owner)) { return result; }
		owner->initialized = false;
		owner->events.clear();
		return OK;
	}
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	auto self = LibKernel::PthreadSelfOrNull();
	int pid = -1;
	if (!program || !self || LibKernel::PthreadGetOwnerProgram(self) != program ||
	    !Loader::TryGetCurrentRetainedGuestProcessId(&pid) || !Loader::IsGuestProcessIdentityActive(pid)) {
		return USER_SERVICE_ERROR_NOT_INITIALIZED;
	}
	if (g_dispatching_user_callback) { return USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	return ReleaseLocalCallbacks(program) ? OK : USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED;
}

static bool IsPlayerLoggedIn(int player) {
	return !Libs::NoForegroundUser() &&
	       (player == 0 || (player > 0 && Controller::IsPlayerConnected(player)));
}

static int GetLoggedInPlayer(int user_id) {
	const int player = Config::GetPlayerFromUserId(user_id);
	return IsPlayerLoggedIn(player) ? player : -1;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceInitialize2() {
	PRINT_NAME();
	if (auto* owner = CallingApplicationOwner()) { return InitializeOwner(owner); }

	MaybePostLoginWake();
	MaybeSeedNativeUserList();

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetInitialUser(int* user_id) {
	PRINT_NAME();

	// ps5-vsh: doc-mandated INVALID_ARGUMENT (never halt on guest input).
	if (user_id == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (auto* application = CallingApplicationOwner()) {
		std::lock_guard lock(g_store_mutex);
		const auto* owner = FindOwnerLocked(application);
		if (const int status = OwnerStatus(owner); status != OK) { return status; }
		*user_id = owner->initial_user;
		return *user_id == -1 ? USER_SERVICE_ERROR_NOT_LOGGED_IN : OK;
	}

	MaybePostLoginWake();

	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (g_store_logged_in.empty()) {
			*user_id = -1;
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
		*user_id = g_store_logged_in.front();
		return OK;
	}

	*user_id = Config::GetUserId();

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetEvent(SceUserServiceEvent* event) {
	PRINT_NAME();

	// ps5-vsh: doc-mandated INVALID_ARGUMENT (never halt on guest input).
	if (event == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	{
		std::lock_guard lock(g_store_mutex);
		RefreshControllerOwnersLocked();
		if (auto* application = CallingApplicationOwner()) {
			auto* owner = FindOwnerLocked(application);
			if (const int status = OwnerStatus(owner); status != OK) { return status; }
			if (owner->events.empty()) { return USER_SERVICE_ERROR_NO_EVENT; }
			*event = owner->events.front();
			owner->events.pop_front();
			return OK;
		}
	}

	MaybePostLoginWake();

	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (g_store_events.empty()) {
			return USER_SERVICE_ERROR_NO_EVENT;
		}
		*event = g_store_events.front();
		g_store_events.pop_front();
		return OK;
	}

	static bool logged_in[Config::MAX_PLAYERS] = {};

	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		const bool connected = IsPlayerLoggedIn(player);
		if (connected != logged_in[player]) {
			logged_in[player] = connected;
			event->event_type = connected ? UserServiceEventTypeLogin : UserServiceEventTypeLogout;
			event->user_id    = Config::GetPlayerUserId(player);
			return OK;
		}
	}

	return USER_SERVICE_ERROR_NO_EVENT;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetLoginUserIdList(UserServiceLoginUserIdList* user_id_list) {
	PRINT_NAME();

	// ps5-vsh: doc-mandated INVALID_ARGUMENT (never halt on guest input).
	if (user_id_list == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	{
		std::lock_guard lock(g_store_mutex);
		RefreshControllerOwnersLocked();
		if (auto* application = CallingApplicationOwner()) {
			const auto* owner = FindOwnerLocked(application);
			if (const int status = OwnerStatus(owner); status != OK) { return status; }
			std::fill_n(user_id_list->user_id, 4, -1);
			std::copy_n(owner->logged_in.begin(), std::min<size_t>(owner->logged_in.size(), 4), user_id_list->user_id);
			return OK;
		}
	}

	MaybePostLoginWake();

	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		for (int i = 0; i < 4; i++) {
			user_id_list->user_id[i] =
			    (i < static_cast<int>(g_store_logged_in.size())) ? g_store_logged_in[static_cast<size_t>(i)] : -1;
		}
		return OK;
	}

	// ps5-vsh: -NoForegroundUser (run_shellui-style): no logged-in user.
	// Report an empty list instead of the Config user.
	if (Libs::NoForegroundUser()) {
		user_id_list->user_id[0] = -1;
		user_id_list->user_id[1] = -1;
		user_id_list->user_id[2] = -1;
		user_id_list->user_id[3] = -1;
		return OK;
	}

	int count = 0;
	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		if (IsPlayerLoggedIn(player)) {
			user_id_list->user_id[count++] = Config::GetPlayerUserId(player);
		}
	}
	while (count < Config::MAX_PLAYERS) {
		user_id_list->user_id[count++] = -1;
	}

	return OK;
}

static int KYTY_SYSV_ABI UserServiceIsLoggedIn(int user_id) {
	UserServiceLoginUserIdList users {};
	const int result = UserServiceGetLoginUserIdList(&users);
	if (result) { return result; }
	return user_id != -1 && std::find(std::begin(users.user_id), std::end(users.user_id), user_id) != std::end(users.user_id) ? 1 : 0;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetUserName(int user_id, char* name, size_t size) {
	if (auto* application = CallingApplicationOwner()) {
		std::lock_guard lock(g_store_mutex);
		RefreshControllerOwnersLocked();
		const auto* owner = FindOwnerLocked(application);
		if (const int status = OwnerStatus(owner); status != OK) { return status; }
		const auto identity = std::find_if(owner->identities.begin(), owner->identities.end(),
		    [user_id](const auto& record) { return record.id == user_id; });
		if (identity == owner->identities.end() || (!owner->store_mode &&
		    std::find(owner->logged_in.begin(), owner->logged_in.end(), user_id) == owner->logged_in.end())) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
		if (!name || size == 0 || (!owner->store_mode && size < 5)) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
		const int written = snprintf(name, size, "%s", identity->name.c_str());
		return written < 0 || static_cast<size_t>(written) >= size ? USER_SERVICE_ERROR_BUFFER_TOO_SHORT : OK;
	}
	if (InStoreMode()) {
		std::lock_guard   lock(g_store_mutex);
		const StoredUser* record = FindRecordLocked(user_id);
		if (record == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
		if (name == nullptr || size == 0) {
			return USER_SERVICE_ERROR_INVALID_ARGUMENT;
		}
		int s = snprintf(name, size, "%s", record->name.c_str());
		if (s >= 0 && LocalAccountHleEnabled()) {
			static std::atomic_uint reports {0};
			if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
				LOGF("userservice-local: name uid=0x%x capacity=%zu length=%d value=%s\n", user_id, size, s, name);
			}
		}

		if (s < 0 || static_cast<size_t>(s) >= size) {
			return USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
		}

		return OK;
	}
	const int player = GetLoggedInPlayer(user_id);
	if (player < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	// ps5-vsh: doc-mandated INVALID_ARGUMENT for bad buffers (never halt on
	// guest input); "Kyty" + NUL needs at least 5 bytes.
	if (name == nullptr || size < 5) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}

	const auto user_name = player > 0
	    ? std::string(Common::Localization::Get(Config::GetConsoleLanguage(), "user.guest")) + " " + std::to_string(player + 1)
	    : Config::GetUserName();
	int         s         = snprintf(name, size, "%s", user_name.c_str());

	if (s < 0 || static_cast<size_t>(s) >= size) {
		return USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
	}

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetUserNumber(int user_id, int32_t* number) {
	PRINT_NAME();

	if (number == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (auto* application = CallingApplicationOwner()) {
		std::lock_guard lock(g_store_mutex);
		RefreshControllerOwnersLocked();
		const auto* owner = FindOwnerLocked(application);
		if (const int result = OwnerStatus(owner)) { return result; }
		if (std::find(owner->logged_in.begin(), owner->logged_in.end(), user_id) == owner->logged_in.end()) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
		const auto slot = std::find(owner->login_slots.begin(), owner->login_slots.end(), user_id);
		if (owner->store_mode && slot == owner->login_slots.end()) { return USER_SERVICE_ERROR_INTERNAL; }
		const int32_t value = owner->store_mode ? static_cast<int32_t>(slot - owner->login_slots.begin()) + 1 :
		    Config::GetPlayerFromUserId(user_id) + 1;
		return LibKernel::FileSystem::CopyPathResultToGuest(number, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		const auto slot = std::find(g_store_login_slots.begin(), g_store_login_slots.end(), user_id);
		if (user_id == -1 || slot == g_store_login_slots.end()) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
		const int32_t value = static_cast<int32_t>(slot - g_store_login_slots.begin()) + 1;
		return LibKernel::FileSystem::CopyPathResultToGuest(number, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	const int player = GetLoggedInPlayer(user_id);
	if (player < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	const int32_t value = player + 1;
	return LibKernel::FileSystem::CopyPathResultToGuest(number, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetGamePresets(int user_id, UserServiceGamePresets* presets) {
	PRINT_NAME();

	if (presets == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	auto this_size = presets->this_size;
	if (this_size == 0 || this_size > sizeof(UserServiceGamePresets)) {
		this_size = sizeof(UserServiceGamePresets);
	}

	std::memset(presets, 0, this_size);
	presets->this_size = sizeof(UserServiceGamePresets);

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityVibration(int user_id, int32_t* vibration) {
	PRINT_NAME();

	if (vibration == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*vibration = 1;

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityTriggerEffect(int      user_id,
                                                                  int32_t* trigger_effect) {
	PRINT_NAME();

	if (trigger_effect == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*trigger_effect = 1;

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAgeLevel(int user_id, uint32_t* age_level) {
	PRINT_NAME();

	if (age_level == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*age_level = 9;

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityChatTranscription(int      user_id,
                                                                      int32_t* chat_transcription) {
	PRINT_NAME();

	if (chat_transcription == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*chat_transcription = 0;

	return OK;
}

static KYTY_SYSV_ABI int
UserServiceGetAccessibilityPressAndHoldDelay(int user_id, int32_t* press_and_hold_delay) {
	PRINT_NAME();

	if (press_and_hold_delay == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*press_and_hold_delay = 0;

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomEnabled(int      user_id,
                                                                int32_t* zoom_enabled) {
	PRINT_NAME();

	if (zoom_enabled == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*zoom_enabled = 0;

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomFollowFocus(int      user_id,
                                                                    int32_t* zoom_follow_focus) {
	PRINT_NAME();

	if (zoom_follow_focus == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (InStoreMode()) {
		std::lock_guard lock(g_store_mutex);
		if (FindRecordLocked(user_id) == nullptr) {
			return USER_SERVICE_ERROR_NOT_LOGGED_IN;
		}
	} else if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*zoom_follow_focus = 0;

	return OK;
}

int UserServiceInstallNativeBackend(const void* table) {
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	auto self = LibKernel::PthreadSelfOrNull();
	if (!program || program->application_owner || !self || LibKernel::PthreadGetOwnerProgram(self) != program) {
		return USER_SERVICE_ERROR_NOT_INITIALIZED;
	}
	NativeBackendTable snapshot;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SIZE_T copied = 0;
	if (!table || !ReadProcessMemory(GetCurrentProcess(), table, &snapshot, sizeof(snapshot), &copied) || copied != sizeof(snapshot)) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
#else
	return USER_SERVICE_ERROR_NOT_INITIALIZED;
#endif
	const uint64_t getter = snapshot.functions[2];
	bool executable = false;
	if (program->elf) {
		const auto* ph = program->elf->GetPhdr();
		for (size_t i = 0; i < program->elf->GetEhdr()->e_phnum; ++i) {
			const uint64_t begin = program->base_vaddr + ph[i].p_vaddr;
			if (ph[i].p_type == Loader::PT_LOAD && (ph[i].p_flags & Loader::PF_X) && getter >= begin && getter - begin < ph[i].p_filesz) {
				executable = true;
			}
		}
	}
	if (!executable) { return USER_SERVICE_ERROR_INVALID_ARGUMENT; }
	std::lock_guard lock(g_store_mutex);
	if (program->user_service_execution) { return USER_SERVICE_ERROR_ALREADY_INITIALIZED; }
	auto execution = Loader::RuntimeGuestExecution::Create(program);
	if (!execution) { return USER_SERVICE_ERROR_NO_MEMORY; }
	program->user_service_property_getter = getter;
	program->user_service_execution = std::move(execution);
	return OK;
}

int UserServiceRetireNativeBackend() {
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	auto self = LibKernel::PthreadSelfOrNull();
	if (!program || !self || LibKernel::PthreadGetOwnerProgram(self) != program) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	std::lock_guard lock(g_store_mutex);
	auto& execution = program->user_service_execution;
	if (!execution) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	execution->RequestStop();
	if (!execution->Retire()) { return static_cast<int>(0x80020010u); }
	execution.reset();
	program->user_service_property_getter = 0;
	return OK;
}

bool ReleaseNativeUserServiceBackend(Loader::Program* owner) {
	if (!ReleaseLocalCallbacks(owner)) { return false; }
	std::lock_guard lock(g_store_mutex);
	if (!owner || !owner->user_service_execution) { return true; }
	auto& execution = owner->user_service_execution;
	execution->RequestStop();
	if (!execution->Retire()) { return false; }
	execution.reset();
	owner->user_service_property_getter = 0;
	return true;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServiceGetAccessibilityUnk_QNk7qD4dlD4(int user_id, int* out_val) {
	PRINT_NAME();
	LOGF("\t [QNk7qD4dlD4] sceUserServiceGetAccessibilityUnk user_id = 0x%08x, out_val = %p\n", user_id, (void*)out_val);
	if (out_val == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (LocalAccountHleEnabled() && !LoginMgrNativeTransport::Installed()) {
		uint32_t value = 0;
		const int result = GetLocalLoginFlag(user_id, &value);
		if (result) { return result; }
		return LibKernel::FileSystem::CopyPathResultToGuest(out_val, &value, sizeof(value)) ? OK : USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	auto self = LibKernel::PthreadSelfOrNull();
	if (!program || !self || LibKernel::PthreadGetOwnerProgram(self) != program) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
	std::shared_ptr<Loader::RuntimeGuestExecution> execution;
	Loader::RuntimeGuestExecution::Lease lease;
	uint64_t getter = 0;
	{
		std::lock_guard lock(g_store_mutex);
		execution = program->user_service_execution;
		if (!execution || !(lease = execution->TryAcquire(Loader::GuestWorkKind::Callback))) { return USER_SERVICE_ERROR_NOT_INITIALIZED; }
		getter = program->user_service_property_getter;
	}
	return ReadNativeProperty6(getter, user_id, out_val, [&](uint64_t target, const uint64_t* arguments, size_t count) {
		const auto result = execution->InvokeIntegerCallback(target, arguments, count);
		return result.Returned() ? static_cast<int>(result.call.value) : USER_SERVICE_ERROR_NOT_INITIALIZED;
	});
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int UserServicePlatformPrivacyWs1Stub(uint64_t, uint64_t, uint64_t, uint64_t,
                                                           uint64_t, uint64_t) {
	return OK;
}

} // namespace UserService

namespace LocalLoginMgr {
LIB_VERSION("LoginMgrServer", 1, "LoginMgrServer", 1, 1);
constexpr int NotInitialized = static_cast<int>(0x813e0001u);
constexpr int InvalidArgument = static_cast<int>(0x813e0002u);
constexpr int Unavailable = static_cast<int>(0x813e0005u);

static bool Selected() {
	const char* core = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
	const char* ui = std::getenv("KYTY_APPDB_UI_AUTH_RECORD");
	return UserService::LocalAccountHleEnabled() && !(core && *core) && !(ui && *ui) &&
	       !LoginMgrNativeTransport::Installed();
}

static bool UserServiceSelected() {
	if (Selected()) { return true; }
	// The explicit retail LOCAL/TEST comparison retains native LoginMgr and
	// source-bound admission, but ShellUI's UserService imports are HLE. Give
	// those imports a real local session/callback contract, never stub success.
	const char* test = std::getenv("KYTY_RETAIL_LOCAL_TEST_BOOT");
	const char* core = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
	const char* ui = std::getenv("KYTY_APPDB_UI_AUTH_RECORD");
	return test && test[0] == '1' && test[1] == '\0' && core && *core && ui && *ui &&
	       UserService::LocalAccountHleEnabled();
}

static int CallerStatus(bool user_service = false) {
	if (!(user_service ? UserServiceSelected() : Selected())) { return Unavailable; }
	auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	auto self = LibKernel::PthreadSelfOrNull();
	int pid = -1;
	if (!program || program->application_owner || !self || LibKernel::PthreadGetOwnerProgram(self) != program ||
	    !Loader::TryGetCurrentRetainedGuestProcessId(&pid) || !Loader::IsGuestProcessIdentityActive(pid)) {
		return NotInitialized;
	}
	// This is explicit local emulation using the retained boot-role identity.
	// It never supplies source-bound credentials to NP or the native transport.
	std::array<uint64_t, 17> auth {};
	if (!Loader::TryGetGuestProcessAuthInfo(pid, &auth) || !(auth[1] & (UINT64_C(1) << 62))) { return Unavailable; }
	return UserService::UserServiceStoreEnabled() ? 0 : NotInitialized;
}

static bool Read(const void* pointer, void* output, size_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SIZE_T copied = 0;
	return pointer && ReadProcessMemory(GetCurrentProcess(), pointer, output, size, &copied) && copied == size;
#else
	return false;
#endif
}

static bool ProbeOutput(void* pointer) {
	uint32_t original = 0;
	return Read(pointer, &original, sizeof(original)) &&
	       LibKernel::FileSystem::CopyPathResultToGuest(pointer, &original, sizeof(original));
}

static int KYTY_SYSV_ABI CreateUser(const char* name, int type, bool incomplete, bool /*has_shown_eula*/, int* output) {
	if (const int status = CallerStatus()) { return status; }
	if (!name || !ProbeOutput(output)) { return InvalidArgument; }
	char text[17] {};
	size_t length = 0;
	for (; length < sizeof(text); ++length) {
		if (!Read(name + length, &text[length], 1)) { return InvalidArgument; }
		if (!text[length]) { break; }
	}
	if (length == sizeof(text)) { return InvalidArgument; }
	int user = -1;
	const int result = UserService::CreateLocalUser(std::string_view(text, length), type, incomplete, &user);
	if (result) { return result; }
	return LibKernel::FileSystem::CopyPathResultToGuest(output, &user, sizeof(user)) ? 0 : InvalidArgument;
}

static int KYTY_SYSV_ABI DestroyUser(int user) {
	if (const int status = CallerStatus()) { return status; }
	return UserService::DestroyLocalUser(user);
}

static int KYTY_SYSV_ABI SetLoginFlag(int user, uint32_t mask, uint32_t value, uint32_t* output) {
	if (const int status = CallerStatus()) { return status; }
	if (!ProbeOutput(output)) { return InvalidArgument; }
	uint32_t flags = 0;
	const int result = UserService::SetLocalLoginFlag(user, mask, value, &flags);
	if (result) { return result; }
	return LibKernel::FileSystem::CopyPathResultToGuest(output, &flags, sizeof(flags)) ? 0 : InvalidArgument;
}

static int KYTY_SYSV_ABI SetUserStatus(int user, uint32_t mask, uint32_t value, uint32_t* output) {
	if (const int status = CallerStatus()) { return status; }
	if (!ProbeOutput(output)) { return InvalidArgument; }
	uint32_t flags = 0;
	const int result = UserService::SetLocalUserStatus(user, mask, value, &flags);
	if (result) { return result; }
	return LibKernel::FileSystem::CopyPathResultToGuest(output, &flags, sizeof(flags)) ? 0 : InvalidArgument;
}

static int UserServiceCallerStatus() {
	const int result = CallerStatus(true);
	return result == 0 ? 0 : result == NotInitialized ? UserService::USER_SERVICE_ERROR_NOT_INITIALIZED :
	    UserService::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED;
}

// 13.60 uvVR70ZxFrQ +0x28c0 waits for IPC0x30000 completion. Its input is
// {i32 user, padding, i64 device}. ShellUI LoginProcess passes device=-1.
static int KYTY_SYSV_ABI LoginUser(int user, int64_t device) {
	if (const int result = UserServiceCallerStatus()) { return result; }
	// Device-bound routing requires the corresponding peripheral association.
	if (device != -1) { return UserService::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED; }
	const int result = UserService::UserServiceStoreLogin(user);
	LOGF("userservice-local: login export uid=0x%x device=%lld result=0x%x\n", user, static_cast<long long>(device), result);
	return result;
}

// 13.60 3T9y5xDcfOk +0x2a80: IPC0x30001 {i32 user, i32 reason}.
static int KYTY_SYSV_ABI LogoutUser(int user, int reason) {
	if (const int result = UserServiceCallerStatus()) { return result; }
	const int result = UserService::UserServiceStoreLogout(user);
	LOGF("userservice-local: logout export uid=0x%x reason=%d result=0x%x\n", user, reason, result);
	return result;
}

static void Register(Loader::SymbolDatabase* s) {
	if (!Selected()) { return; }
	// Independent profile operations: no native client, opaque objects or native
	// initialization count is claimed. Unsupported LoginMgr operations retain
	// their native/error paths. The profile store is shared with UserService.
	LIB_FUNC("AT6NySjQ2gY", CreateUser);
	LIB_FUNC("REIRIXzv-H0", DestroyUser);
	LIB_FUNC("Jha8yMoMzrg", SetLoginFlag);
	LIB_FUNC("-+h1C78SdyU", SetUserStatus);
}
} // namespace LocalLoginMgr

int UserService::ChangeEventCallback(uint64_t callback, uint64_t userdata, bool add) {
	// Retain the legacy route outside explicit local-account selection.
	if (!LocalLoginMgr::UserServiceSelected()) { return OK; }
	const int caller = LocalLoginMgr::UserServiceCallerStatus();
	const int result = caller ? caller : ChangeLocalCallback(callback, userdata, add);
	LOGF("userservice-local: callback %s target=0x%llx userdata=0x%llx result=0x%x\n", add ? "register" : "unregister",
	     static_cast<unsigned long long>(callback), static_cast<unsigned long long>(userdata), result);
	return result;
}

LIB_DEFINE(InitUserService_1) {
	LIB_FUNC("j3YMu1MVNNo", UserService::UserServiceInitialize);
	LIB_FUNC("az-0R6eviZ0", UserService::UserServiceInitialize2);
	LIB_FUNC("CdWp0oHWGr0", UserService::UserServiceGetInitialUser);
	LIB_FUNC("yH17Q6NWtVg", UserService::UserServiceGetEvent);
	LIB_FUNC("fPhymKNvK-A", UserService::UserServiceGetLoginUserIdList);
	LIB_FUNC("1xxcMiGu2fo", UserService::UserServiceGetUserName);
	LIB_FUNC("bwFjS+bX9mA", UserService::UserServiceTerminate);
	LIB_FUNC("qbwy0Ub8b3M", UserService::UserServiceGetUserNumber);
	LIB_FUNC("-sD02mFDBh4", UserService::UserServiceGetGamePresets);
	LIB_FUNC("woNpu+45RLk", UserService::UserServiceGetAgeLevel);
	LIB_FUNC("rnEhHqG-4xo", UserService::UserServiceGetAccessibilityChatTranscription);
	LIB_FUNC("ZKJtxdgvzwg", UserService::UserServiceGetAccessibilityPressAndHoldDelay);
	LIB_FUNC("qWYHOFwqCxY", UserService::UserServiceGetAccessibilityVibration);
	LIB_FUNC("-3Y5GO+-i78", UserService::UserServiceGetAccessibilityTriggerEffect);
	LIB_FUNC("hD-H81EN9Vg", UserService::UserServiceGetAccessibilityZoomEnabled);
	LIB_FUNC("O6IW1-Dwm-w", UserService::UserServiceGetAccessibilityZoomFollowFocus);
	LIB_FUNC("D-CzAxQL0XI", UserService::UserServicePlatformPrivacyWs1Stub);
	LIB_FUNC("QNk7qD4dlD4", UserService::UserServiceGetAccessibilityUnk_QNk7qD4dlD4);
	LocalLoginMgr::Register(s);
	if (LocalLoginMgr::UserServiceSelected()) {
		LIB_FUNC("RJX7T4sjNgI", UserService::UserServiceGetUserStatus);
		LIB_FUNC("uvVR70ZxFrQ", LocalLoginMgr::LoginUser);
		LIB_FUNC("3T9y5xDcfOk", LocalLoginMgr::LogoutUser);
		LIB_FUNC("MZxH8029+Wg", UserService::UserServiceIsLoggedIn);
	}
}

} // namespace Libs
