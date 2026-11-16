#include "loader/runtimeLocalAppDb.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <span>
#include <thread>
#include <sqlite3.h>

namespace Loader::LocalAppDb {
namespace {
constexpr const char* VersionDdl = "CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));";
// Core +1005950 -> three relocated SQL pointers at +2064b80.
constexpr const char* AppInfoDdl = "CREATE TABLE IF NOT EXISTS tbl_appinfo(titleId TEXT NOT NULL,metaDataId TEXT,key INT NOT NULL,val,PRIMARY KEY(titleId, metaDataId, key));";
constexpr const char* ConceptInfoDdl = "CREATE TABLE IF NOT EXISTS tbl_conceptinfo(local_concept_id TEXT NOT NULL,metadata_id TEXT,key INT NOT NULL,val,PRIMARY KEY(local_concept_id, key));";
struct Column { std::string_view name, type; int not_null, primary_key; };
constexpr Column VersionColumns[] {{"category", "TEXT", 1, 1}, {"status", "INT", 0, 0}};
constexpr Column UserColumns[] {{"key", "TEXT", 1, 1}, {"value", "TEXT", 0, 0}};
constexpr Column AppInfoColumns[] {{"titleId", "TEXT", 1, 1}, {"metaDataId", "TEXT", 0, 2}, {"key", "INT", 1, 3}, {"val", "", 0, 0}};
constexpr Column ConceptInfoColumns[] {{"local_concept_id", "TEXT", 1, 1}, {"metadata_id", "TEXT", 0, 0}, {"key", "INT", 1, 2}, {"val", "", 0, 0}};
struct Statement {
	sqlite3_stmt* value = nullptr;
	~Statement() { sqlite3_finalize(value); }
};
int Exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }
bool Text(sqlite3_stmt* stmt, int column, std::string_view expected) {
	const auto* value = sqlite3_column_text(stmt, column);
	return value && expected == reinterpret_cast<const char*>(value);
}
// IF NOT EXISTS alone silently accepts a pre-existing incompatible object.
// Admit only the recovered table shape; no implicit coercion or row rewrites.
bool Shape(sqlite3* db, const std::string& name, std::span<const Column> expected) {
	Statement object, columns;
	if (sqlite3_prepare_v2(db, "SELECT type FROM sqlite_master WHERE name=?1", -1, &object.value, nullptr) != SQLITE_OK) { return false; }
	sqlite3_bind_text(object.value, 1, name.c_str(), -1, SQLITE_TRANSIENT);
	if (sqlite3_step(object.value) != SQLITE_ROW || !Text(object.value, 0, "table")) { return false; }
	const std::string pragma = "PRAGMA table_xinfo(\"" + name + "\")";
	if (sqlite3_prepare_v2(db, pragma.c_str(), -1, &columns.value, nullptr) != SQLITE_OK) { return false; }
	for (const auto& column: expected) {
		if (sqlite3_step(columns.value) != SQLITE_ROW ||
		    !Text(columns.value, 1, column.name) || !Text(columns.value, 2, column.type) ||
		    sqlite3_column_int(columns.value, 3) != column.not_null ||
		    sqlite3_column_type(columns.value, 4) != SQLITE_NULL ||
		    sqlite3_column_int(columns.value, 5) != column.primary_key ||
		    sqlite3_column_int(columns.value, 6) != 0) { return false; }
	}
	return sqlite3_step(columns.value) == SQLITE_DONE;
}
}
Producer::Producer(std::filesystem::path path, std::vector<int32_t> users, AppInfoCreation creation):
	m_path(std::move(path)), m_users(std::move(users)), m_creation(creation) {}
Producer::~Producer() { Stop(); }
int Producer::Busy(void* owner, int attempts) {
	auto* self = static_cast<Producer*>(owner);
	self->m_waiting.store(true);
	if (self->m_cancelled.load() || attempts >= 100) { return 0; }
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return 1;
}
int Producer::Progress(void* owner) { return static_cast<Producer*>(owner)->m_cancelled.load() ? 1 : 0; }
void Producer::Stop() {
	// Request first: COMMIT itself can be waiting on another SQLite reader.
	// Waiting for its gate before requesting cancellation would deadlock stop
	// against a caller that keeps that external read transaction alive.
	m_cancelled.store(true);
	{ std::lock_guard gate(m_commit_gate); }
	std::lock_guard lock(m_mutex);
	m_closed = true;
	if (m_db) { sqlite3_close_v2(m_db); m_db = nullptr; }
}
Result Producer::PrepareUser(int32_t user) {
	return PrepareSchema(SchemaScope::UserMetadata, user);
}
Result Producer::PrepareAppInfo() {
	return PrepareSchema(SchemaScope::AppInfoBase, -1);
}
Result Producer::InitializeAppInfoSchema() {
	return PrepareSchema(SchemaScope::AppInfoSchemaInitialization, -1);
}
Result Producer::PrepareSchema(SchemaScope scope, int32_t user) {
	std::lock_guard lock(m_mutex);
	m_waiting.store(false);
	Result result;
	result.scope = scope;
	if (m_closed) { result.status = Status::Closed; return result; }
	if (m_cancelled.load()) { result.status = Status::Cancelled; return result; }
	if (scope == SchemaScope::UserMetadata && (user < 0 || std::find(m_users.begin(), m_users.end(), user) == m_users.end())) {
		result.status = Status::UnknownUser; return result;
	}
	std::error_code error;
	result.recovery_marker_present = std::filesystem::exists(std::filesystem::path(m_path.native() + std::filesystem::path("-corrupt").native()), error);
	if (error) { result.sqlite_error = SQLITE_IOERR; return result; }
	const auto fail = [&](int rc) {
		result.table_access_index.reset();
		result.access_index.reset();
		result.status = m_cancelled.load() ? Status::Cancelled : Status::SqlError;
		result.sqlite_error = rc;
		return result;
	};
	if (!m_db) {
		const auto utf8 = m_path.u8string();
		// New AppInfo schema creation is explicit, never a fallback after failed
		// open, corruption, or an incompatible schema. No truncation/reset.
		const int create = scope != SchemaScope::UserMetadata && m_creation == AppInfoCreation::CreateIfMissing ? SQLITE_OPEN_CREATE : 0;
		const int rc = sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &m_db,
		                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | create, nullptr);
		if (rc != SQLITE_OK) { sqlite3_close_v2(m_db); m_db = nullptr; return fail(rc); }
		sqlite3_busy_handler(m_db, Busy, this);
		sqlite3_progress_handler(m_db, 100, Progress, this);
	}
	sqlite3_progress_handler(m_db, 100, Progress, this);
	int rc = Exec(m_db, "BEGIN IMMEDIATE");
	if (rc != SQLITE_OK) { return fail(rc); }
	struct Rollback {
		sqlite3* db;
		~Rollback() {
			if (!sqlite3_get_autocommit(db)) {
				// Cancellation may interrupt SQL work, never its cleanup.
				sqlite3_progress_handler(db, 0, nullptr, nullptr);
				Exec(db, "ROLLBACK");
			}
		}
	} rollback {m_db};
	// Validate inside the same transaction which owns schema changes.
	{
		Statement integrity;
		rc = sqlite3_prepare_v2(m_db, "PRAGMA quick_check", -1, &integrity.value, nullptr);
		if (rc != SQLITE_OK) { return fail(rc); }
		rc = sqlite3_step(integrity.value);
		if (rc != SQLITE_ROW || !Text(integrity.value, 0, "ok")) { return fail(rc == SQLITE_ROW ? SQLITE_CORRUPT : rc); }
		if ((rc = sqlite3_step(integrity.value)) != SQLITE_DONE) { return fail(rc); }
	}
	rc = Exec(m_db, VersionDdl); // UI +54a925.
	if (rc != SQLITE_OK) { return fail(rc); }
	bool compatible = Shape(m_db, "tbl_version", VersionColumns);
	if (scope == SchemaScope::UserMetadata) {
		char table[48];
		std::snprintf(table, sizeof(table), "tbl_info_%010d", user); // UI +54e930.
		const std::string ddl = std::string("CREATE TABLE IF NOT EXISTS ") + table + "(key TEXT NOT NULL,value TEXT,PRIMARY KEY(key));";
		if ((rc = Exec(m_db, ddl.c_str())) != SQLITE_OK) { return fail(rc); }
		compatible = compatible && Shape(m_db, table, UserColumns);
	} else {
		if ((rc = Exec(m_db, AppInfoDdl)) != SQLITE_OK || (rc = Exec(m_db, ConceptInfoDdl)) != SQLITE_OK) { return fail(rc); }
		compatible = compatible && Shape(m_db, "tbl_appinfo", AppInfoColumns) && Shape(m_db, "tbl_conceptinfo", ConceptInfoColumns);
	}
	if (!compatible) {
		if (m_cancelled.load()) { return fail(SQLITE_INTERRUPT); }
		result.status = Status::IncompatibleSchema; return result;
	}
	{
		Statement revision;
		rc = sqlite3_prepare_v2(m_db, "SELECT status FROM tbl_version WHERE category='table_access_index'", -1, &revision.value, nullptr);
		if (rc != SQLITE_OK) { return fail(rc); }
		rc = sqlite3_step(revision.value);
		if (rc == SQLITE_ROW) {
			if (sqlite3_column_type(revision.value, 0) != SQLITE_INTEGER) { result.status = Status::IncompatibleSchema; return result; }
			result.table_access_index = sqlite3_column_int64(revision.value, 0);
		} else if (rc != SQLITE_DONE) { return fail(rc); }
	}
	std::optional<int64_t> committed_index;
	if (scope == SchemaScope::AppInfoSchemaInitialization) {
		Statement current, update, verify;
		constexpr const char* ReadIndex = "SELECT status FROM tbl_version WHERE category='access_index'";
		if ((rc = sqlite3_prepare_v2(m_db, ReadIndex, -1, &current.value, nullptr)) != SQLITE_OK) { return fail(rc); }
		int64_t previous = 0; // Native caller +ffe3b1 supplies the no-row default.
		rc = sqlite3_step(current.value);
		if (rc == SQLITE_ROW) {
			if (sqlite3_column_type(current.value, 0) != SQLITE_INTEGER) {
				result.table_access_index.reset();
				result.status = Status::IncompatibleSchema; return result;
			}
			previous = sqlite3_column_int64(current.value, 0);
			if ((rc = sqlite3_step(current.value)) != SQLITE_DONE) { return fail(rc); }
		} else if (rc != SQLITE_DONE) { return fail(rc); }
		// Core +10067de increments the 64-bit value before binding. Use integer
		// bits, not signed C++ overflow or SQLite's REAL promotion at INT64_MAX.
		const auto next = std::bit_cast<int64_t>(std::bit_cast<uint64_t>(previous) + uint64_t {1});
		if ((rc = sqlite3_prepare_v2(m_db, "REPLACE INTO tbl_version VALUES(?1, ?2)", -1, &update.value, nullptr)) != SQLITE_OK ||
		    (rc = sqlite3_bind_text(update.value, 1, "access_index", -1, SQLITE_STATIC)) != SQLITE_OK ||
		    (rc = sqlite3_bind_int64(update.value, 2, next)) != SQLITE_OK) { return fail(rc); }
		if ((rc = sqlite3_step(update.value)) != SQLITE_DONE) { return fail(rc); }
		// An existing trigger can ignore/rewrite a successful statement. Certify
		// the resulting row, not merely SQLITE_DONE, before admitting COMMIT.
		if (sqlite3_changes(m_db) != 1) { return fail(SQLITE_CONSTRAINT); }
		if ((rc = sqlite3_prepare_v2(m_db, ReadIndex, -1, &verify.value, nullptr)) != SQLITE_OK) { return fail(rc); }
		rc = sqlite3_step(verify.value);
		if (rc != SQLITE_ROW) { return fail(rc == SQLITE_DONE ? SQLITE_CONSTRAINT : rc); }
		if (sqlite3_column_type(verify.value, 0) != SQLITE_INTEGER || sqlite3_column_int64(verify.value, 0) != next) {
			return fail(SQLITE_CONSTRAINT);
		}
		if ((rc = sqlite3_step(verify.value)) != SQLITE_DONE) { return fail(rc); }
		committed_index = next;
	}
	std::lock_guard commit_gate(m_commit_gate);
	if (m_cancelled.load()) { return fail(SQLITE_INTERRUPT); }
	// The commit is the completion boundary; the receipt describes ONLY this
	// stage. A concurrent Stop after commit does not undo durable work.
	if ((rc = Exec(m_db, "COMMIT")) != SQLITE_OK) { result.table_access_index.reset(); return fail(rc); }
	result.status = Status::Committed;
	result.access_index = committed_index;
	return result;
}
} // namespace Loader::LocalAppDb
