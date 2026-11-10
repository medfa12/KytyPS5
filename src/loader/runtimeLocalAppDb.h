#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

struct sqlite3;

namespace Loader::LocalAppDb {

enum class Status { Committed, UnknownUser, IncompatibleSchema, SqlError, Cancelled, Closed };
enum class SchemaScope { UserMetadata, AppInfoBase, AppInfoSchemaInitialization };
enum class AppInfoCreation { ExistingOnly, CreateIfMissing };
struct Result {
	Status status = Status::SqlError;
	SchemaScope scope = SchemaScope::UserMetadata;
	int sqlite_error = 0;
	// Absence is not revision zero. Values are read from committed native rows.
	std::optional<int64_t> table_access_index;
	// Only AppInfoSchemaInitialization produces this committed revision.
	std::optional<int64_t> access_index;
	bool recovery_marker_present = false;
};

// Local producer schema stages: additive AppInfo base/current-user metadata on an
// explicitly selected working database. It never claims catalog/recovery ready,
// resets a DB, stamps schema versions, removes markers, or signals guests.
// Only InitializeAppInfoSchema advances the native AppInfo access_index.
// Each receipt names its completed operation, not native version compatibility.
// A runtime adapter must supply the actual local-profile roster and its own
// private candidate path until full recovery/catalog publication is supported.
class Producer final {
public:
	Producer(std::filesystem::path working_database, std::vector<int32_t> local_users,
	         AppInfoCreation creation = AppInfoCreation::ExistingOnly);
	~Producer();
	Producer(const Producer&) = delete;
	Producer& operator=(const Producer&) = delete;
	Result PrepareUser(int32_t user);
	Result PrepareAppInfo();
	// Core's DDL + access_index transaction, not catalog/recovery initialization.
	// Existing noninteger indices are rejected and preserved.
	Result InitializeAppInfoSchema();
	// Terminal stop. Requests cancellation before joining the transaction mutex;
	// never holds a loader lock, invokes guest callbacks, or restarts the handle.
	void Stop();
	bool WaitingForDatabase() const { return m_waiting.load(); }
private:
	Result PrepareSchema(SchemaScope scope, int32_t user);
	static int Busy(void* owner, int attempts);
	static int Progress(void* owner);
	std::filesystem::path m_path;
	std::vector<int32_t> m_users;
	AppInfoCreation m_creation;
	std::mutex m_mutex;
	std::mutex m_commit_gate;
	std::atomic<bool> m_cancelled {false};
	std::atomic<bool> m_waiting {false};
	sqlite3* m_db = nullptr;
	bool m_closed = false;
};
} // namespace Loader::LocalAppDb
