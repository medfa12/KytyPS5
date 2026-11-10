#include "loader/runtimeLocalSystemCatalog.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using namespace Loader::LocalAppDb;
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "CatalogCommit: %s\n", why); std::abort(); } }
enum class Point { Projection, Commit, RejectedCommit, BusyCommit };
struct Observation {
	std::string target;
	std::atomic<bool>* cancelled = nullptr;
	Point point = Point::Projection;
	bool observed = false;
	bool commit_seen = false;
	sqlite3* reader = nullptr;
};
Observation* observation = nullptr;
int Trace(unsigned, void* context, void*, void* statement) {
	auto& o = *static_cast<Observation*>(context);
	const char* sql = static_cast<const char*>(statement);
	const bool commit = std::strcmp(sql, "COMMIT") == 0;
	if ((o.point == Point::Projection && std::strstr(sql, "CREATE TABLE tbl_iconinfo")) ||
	    (o.point != Point::Projection && commit)) {
		o.observed = true; o.commit_seen = commit; o.cancelled->store(true);
		if (o.point == Point::BusyCommit) {
			Check(sqlite3_open_v2(o.target.c_str(), &o.reader, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open competing snapshot");
			Check(sqlite3_exec(o.reader, "BEGIN; SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr) == SQLITE_OK,
			      "hold competing reader through actual COMMIT");
		}
	}
	return 0;
}
int RejectCommit(void* context) {
	const auto& o = *static_cast<Observation*>(context);
	return o.point == Point::RejectedCommit && o.commit_seen ? 1 : 0;
}
int Extension(sqlite3* db, char**, const sqlite3_api_routines*) {
	// Test-only SQLite instrumentation: the flag flips inside the real COMMIT
	// statement, strictly after the producer's final check, without sleeps or
	// production callbacks. Backup commits do not have a SQL COMMIT trace.
	const char* path = sqlite3_db_filename(db, "main");
	if (observation && path && std::filesystem::path(path) == std::filesystem::path(observation->target)) {
		sqlite3_trace_v2(db, SQLITE_TRACE_STMT, Trace, observation);
		sqlite3_commit_hook(db, RejectCommit, observation);
	}
	return SQLITE_OK;
}
int64_t Count(const std::filesystem::path& path, const char* sql) {
	sqlite3* db = nullptr;
	const auto utf8 = path.u8string();
	Check(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open retained candidate");
	sqlite3_stmt* statement = nullptr;
	Check(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW, "read retained candidate");
	const auto count = sqlite3_column_int64(statement, 0);
	sqlite3_finalize(statement); sqlite3_close(db); return count;
}
}
void RunLocalSystemCatalogCommitTests(const std::filesystem::path& root, const std::filesystem::path& copied_db,
                                     const std::filesystem::path& sources) {
	using namespace Loader::LocalAppDb;
	Check(sqlite3_auto_extension(reinterpret_cast<void(*)()>(Extension)) == SQLITE_OK, "install deterministic SQLite observation");
	for (auto point: {Point::Projection, Point::Commit, Point::RejectedCommit, Point::BusyCommit}) {
		std::atomic<bool> cancelled {false};
		CatalogRequest request {copied_db, root / ("catalog-boundary-" + std::to_string(static_cast<int>(point))), sources, {268500996}, "en-US"};
		const auto path = request.new_candidate_directory / "app.db";
		const auto utf8 = std::filesystem::absolute(path).u8string();
		Observation o {std::string(reinterpret_cast<const char*>(utf8.c_str())), &cancelled, point};
		observation = &o;
		const auto result = BuildSystemCatalogCandidate(request, cancelled);
		observation = nullptr;
		if (o.reader) { sqlite3_exec(o.reader, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(o.reader); }
		Check(o.observed && cancelled.load(), "did not observe requested real transaction boundary");
		Check(result.retained_candidate_directory == request.new_candidate_directory && result.input_backup_complete &&
		      std::filesystem::exists(request.new_candidate_directory / "app.db-corrupt"), "retained input ownership/evidence not exposed");
		if (point == Point::Commit) {
			Check(result.status == CatalogStatus::Committed && result.candidate_app_db == path && result.source_records == 2,
			      "late cancellation hid successful COMMIT");
			Check(Count(path, "SELECT count(*) FROM tbl_iconinfo_0268500996") == 2, "committed receipt has no rows");
		} else {
			Check(result.candidate_app_db.empty() && result.source_records == 0 && result.profile_records == 0,
			      "failed candidate advertised projected catalog");
			Check(result.status == (point == Point::Projection ? CatalogStatus::Cancelled : CatalogStatus::SqlError),
			      "unrelated cancellation masked COMMIT error or projection cancellation ignored");
			if (point == Point::RejectedCommit) { Check(result.sqlite_error == SQLITE_CONSTRAINT, "commit-hook error lost"); }
			if (point == Point::BusyCommit) { Check(result.sqlite_error == SQLITE_BUSY, "concurrent reader COMMIT error lost"); }
			Check(Count(path, "SELECT count(*) FROM sqlite_master WHERE name IN ('hle_system_catalog_source','tbl_iconinfo_0268500996','tbl_concepticoninfo_0268500996')") == 0,
			      "failed projection/COMMIT left candidate rows");
		}
	}
	Check(sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(Extension)) == 1, "remove test observation");
	std::puts("CatalogCommit: deterministic projection cancellation, late COMMIT cancellation, rejected/busy COMMIT, retained-candidate receipts passed");
}
