#include "loader/runtimeLocalAppDb.h"
#include "loader/runtimeLocalSystemCatalog.h"
#include "loader/runtimeLocalHomeQuery.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <map>
#include <limits>
#include <vector>
#include <algorithm>

void RunLocalDbRecoveryTests(const std::filesystem::path& root, const std::filesystem::path& copied_db);
void RunLocalHomeQueryTests(const std::filesystem::path& root, const std::filesystem::path& candidate, const std::filesystem::path& sources);
void RunLocalSystemCatalogCommitTests(const std::filesystem::path& root, const std::filesystem::path& copied_db, const std::filesystem::path& sources);
void RunLocalDbPreparationTests(const std::filesystem::path& root, const std::filesystem::path& copied_db);
void RunLocalCoreAppInfoTests(const std::filesystem::path& root);
void RunLocalNativeCatalogTests(const std::filesystem::path& root, const std::filesystem::path& candidate, const std::filesystem::path& sources);

namespace {
using namespace Loader::LocalAppDb;
constexpr int32_t User = 0x10010004;
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "LocalAppDb: %s\n", why); std::abort(); } }
struct Db {
	sqlite3* p = nullptr;
	explicit Db(const std::filesystem::path& path) {
		const auto text = path.u8string();
		Check(sqlite3_open(reinterpret_cast<const char*>(text.c_str()), &p) == SQLITE_OK, "fixture open");
	}
	~Db() { sqlite3_close(p); }
	void Sql(const char* sql) { Check(sqlite3_exec(p, sql, nullptr, nullptr, nullptr) == SQLITE_OK, sql); }
	int64_t Value(const char* sql) {
		sqlite3_stmt* s = nullptr;
		Check(sqlite3_prepare_v2(p, sql, -1, &s, nullptr) == SQLITE_OK, sql);
		Check(sqlite3_step(s) == SQLITE_ROW, "expected query row");
		const auto n = sqlite3_column_int64(s, 0); sqlite3_finalize(s); return n;
	}
};
std::map<std::string, std::vector<std::string>> Contents(sqlite3* db) {
	std::map<std::string, std::vector<std::string>> tables;
	sqlite3_stmt* names = nullptr;
	Check(sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &names, nullptr) == SQLITE_OK, "enumerate original tables");
	while (sqlite3_step(names) == SQLITE_ROW) {
		const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(names, 0)));
		char* sql = sqlite3_mprintf("SELECT * FROM \"%w\"", name.c_str());
		sqlite3_stmt* rows = nullptr;
		Check(sqlite3_prepare_v2(db, sql, -1, &rows, nullptr) == SQLITE_OK, "read original table"); sqlite3_free(sql);
		auto& values = tables[name];
		int rc;
		while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
			std::string value;
			for (int i = 0; i < sqlite3_column_count(rows); ++i) {
				value += std::to_string(sqlite3_column_type(rows, i)) + ":";
				const auto* bytes = sqlite3_column_blob(rows, i);
				const int size = sqlite3_column_bytes(rows, i);
				value += std::to_string(size) + ":";
				if (size) { value.append(static_cast<const char*>(bytes), size); }
			}
			values.push_back(std::move(value));
		}
		Check(rc == SQLITE_DONE, "scan original table"); sqlite3_finalize(rows);
		std::sort(values.begin(), values.end());
	}
	sqlite3_finalize(names);
	return tables;
}
void Lifecycle(const std::filesystem::path& path) {
	{ Db db(path); db.Sql("CREATE TABLE owned_profile(id INTEGER PRIMARY KEY, payload BLOB); INSERT INTO owned_profile VALUES(268500996,x'001122');"); }
	std::ofstream(path.string() + "-corrupt") << "preserve marker";
	Producer producer(path, {User});
	Check(producer.PrepareUser(User + 1).status == Status::UnknownUser, "unknown user admitted");
	auto result = producer.PrepareUser(User);
	Check(result.status == Status::Committed && !result.table_access_index && result.recovery_marker_present,
	      "schema completion must not fabricate revision or erase recovery state");
	{
		Db db(path);
		Check(db.Value("SELECT count(*) FROM owned_profile WHERE id=268500996 AND payload=x'001122'") == 1, "profile changed");
		Check(db.Value("SELECT count(*) FROM tbl_version") == 0, "invented version/catalog");
		Check(db.Value("SELECT count(*) FROM tbl_info_0268500996") == 0, "invented user metadata");
		// Explicit fixture revision tests preservation; production never inserts it.
		db.Sql("INSERT INTO tbl_version VALUES('table_access_index',729); INSERT INTO tbl_info_0268500996 VALUES('fixture','retain');");
	}
	result = producer.PrepareUser(User);
	Check(result.status == Status::Committed && result.table_access_index == 729, "committed revision not retained");
	producer.Stop();
	Check(producer.PrepareUser(User).status == Status::Closed, "terminal stop restarted");
	Check(std::filesystem::exists(path.string() + "-corrupt"), "marker removed");
}
void FailureAtomicity(const std::filesystem::path& path) {
	{ Db db(path); db.Sql("CREATE TABLE tbl_info_0268500996(wrong INTEGER); INSERT INTO tbl_info_0268500996 VALUES(42);"); }
	Producer producer(path, {User});
	Check(producer.PrepareUser(User).status == Status::IncompatibleSchema, "incompatible table admitted");
	Db db(path);
	Check(db.Value("SELECT count(*) FROM sqlite_master WHERE name='tbl_version'") == 0, "failed schema transaction leaked DDL");
	Check(db.Value("SELECT wrong FROM tbl_info_0268500996") == 42, "failed transaction modified original table");
}
void AppInfoBase(const std::filesystem::path& path, bool incompatible) {
	{
		Db db(path);
		db.Sql("CREATE TABLE retained(data BLOB); INSERT INTO retained VALUES(x'010203');");
		if (incompatible) { db.Sql("CREATE TABLE tbl_conceptinfo(wrong TEXT); INSERT INTO tbl_conceptinfo VALUES('preserve');"); }
	}
	Producer producer(path, {});
	const auto result = producer.PrepareAppInfo();
	Check(result.scope == SchemaScope::AppInfoBase && !result.table_access_index, "AppInfo scope fabricated Home revision");
	Db db(path);
	if (incompatible) {
		Check(result.status == Status::IncompatibleSchema, "incompatible AppInfo concept schema admitted");
		Check(db.Value("SELECT count(*) FROM sqlite_master WHERE name IN ('tbl_version','tbl_appinfo')") == 0,
		      "three-statement AppInfo transaction leaked prior DDL on last-table failure");
		Check(db.Value("SELECT count(*) FROM tbl_conceptinfo WHERE wrong='preserve'") == 1, "incompatible AppInfo rows lost");
	} else {
		Check(result.status == Status::Committed, "AppInfo base schema did not commit");
		Check(db.Value("SELECT count(*) FROM tbl_appinfo") == 0 && db.Value("SELECT count(*) FROM tbl_conceptinfo") == 0 &&
		      db.Value("SELECT count(*) FROM tbl_version") == 0, "AppInfo stage synthesized records/versions");
		// Isolated fixture values: verify untyped val blobs and composite keys
		// survive repeat schema work without synthesizing production metadata.
		db.Sql("INSERT INTO tbl_appinfo VALUES('fixture','metadata',7,x'ff0011'); INSERT INTO tbl_conceptinfo VALUES('concept','metadata',8,'retain');");
		Check(producer.PrepareAppInfo().status == Status::Committed, "AppInfo repeat schema work");
		Check(db.Value("SELECT count(*) FROM tbl_appinfo WHERE val=x'ff0011'") == 1 &&
		      db.Value("SELECT count(*) FROM tbl_conceptinfo WHERE val='retain'") == 1, "AppInfo records changed");
	}
	Check(db.Value("SELECT count(*) FROM retained WHERE data=x'010203'") == 1, "AppInfo stage touched unrelated rows");
}
void BusyStop(const std::filesystem::path& path) {
	Db lock(path); lock.Sql("CREATE TABLE existing(value); BEGIN IMMEDIATE;");
	Producer producer(path, {User});
	Result result;
	std::thread work([&] { result = producer.PrepareUser(User); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!producer.WaitingForDatabase() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	Check(producer.WaitingForDatabase(), "did not observe real SQLite lock contention");
	producer.Stop(); // Must cancel/join while the external lock remains held.
	work.join();
	Check(result.status == Status::Cancelled && !result.table_access_index, "cancelled job reported completion");
	lock.Sql("ROLLBACK;");
	Check(lock.Value("SELECT count(*) FROM sqlite_master WHERE name='tbl_version'") == 0, "cancelled job changed schema");
}
void AppInfoInitialization(const std::filesystem::path& path) {
	Producer producer(path, {User}, AppInfoCreation::CreateIfMissing);
	Check(producer.PrepareAppInfo().status == Status::Committed, "AppInfo base before initialization");
	Db db(path);
	db.Sql("INSERT INTO tbl_version VALUES('table_access_index',729),('unrelated_version',26);");
	std::ofstream(path.string() + "-corrupt") << "retain";
	auto result = producer.InitializeAppInfoSchema();
	Check(result.status == Status::Committed && result.scope == SchemaScope::AppInfoSchemaInitialization &&
	      result.access_index == 1 && result.table_access_index == 729 && result.recovery_marker_present,
	      "AppInfo initialization receipt must follow actual index mutation");
	Check(db.Value("SELECT status FROM tbl_version WHERE category='access_index'") == 1, "initial AppInfo index not durable");
	Check(producer.InitializeAppInfoSchema().access_index == 2, "repeat initialization did not advance actual index");
	Check(!producer.PrepareAppInfo().access_index && !producer.PrepareUser(User).access_index,
	      "partial schema scope claimed index operation");
	Check(db.Value("SELECT status FROM tbl_version WHERE category='access_index'") == 2, "partial schema stage advanced index");
	producer.Stop();
	Check(!producer.InitializeAppInfoSchema().access_index, "closed producer returned revision receipt");
	Producer reopened(path, {});
	Check(reopened.InitializeAppInfoSchema().access_index == 3, "reopen lost durable index");
	db.Sql("UPDATE tbl_version SET status=9223372036854775807 WHERE category='access_index';");
	Check(reopened.InitializeAppInfoSchema().access_index == std::numeric_limits<int64_t>::min(), "native 64-bit index wrap changed");
	Check(db.Value("SELECT typeof(status)='integer' FROM tbl_version WHERE category='access_index'") == 1, "index promoted to REAL");
	Check(db.Value("SELECT status FROM tbl_version WHERE category='table_access_index'") == 729 &&
	      db.Value("SELECT status FROM tbl_version WHERE category='unrelated_version'") == 26 &&
	      std::filesystem::exists(path.string() + "-corrupt"), "initialization changed unrelated revisions/marker");
}
void AppInfoIndexFailure(const std::filesystem::path& path, const char* obstruction, Status expected) {
	Db db(path);
	db.Sql("CREATE TABLE tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category)); INSERT INTO tbl_version VALUES('access_index',41);");
	db.Sql(obstruction);
	const auto original = Contents(db.p);
	Producer producer(path, {});
	const auto result = producer.InitializeAppInfoSchema();
	Check(result.status == expected && !result.access_index && !result.table_access_index, "failed index operation returned completion/revision");
	Check(Contents(db.p) == original, "index failure leaked tentative schema or row changes");
}
void ConcurrentAppInfoInitialization(const std::filesystem::path& path) {
	Producer first(path, {}, AppInfoCreation::CreateIfMissing);
	Check(first.InitializeAppInfoSchema().access_index == 1, "concurrency fixture initialization");
	Producer second(path, {});
	Result a, b;
	std::thread one([&] { a = first.InitializeAppInfoSchema(); });
	std::thread two([&] { b = second.InitializeAppInfoSchema(); });
	one.join(); two.join();
	Check(a.status == Status::Committed && b.status == Status::Committed &&
	      ((a.access_index == 2 && b.access_index == 3) || (a.access_index == 3 && b.access_index == 2)), "concurrent initialization lost index update");
	Db db(path);
	Check(db.Value("SELECT status FROM tbl_version WHERE category='access_index'") == 3, "concurrent index not durable");
}
void MissingAndCorrupt(const std::filesystem::path& root) {
	const auto missing = root / "missing.db";
	Producer absent(missing, {User});
	Check(absent.PrepareUser(User).status == Status::SqlError && !std::filesystem::exists(missing), "missing DB silently created");
	Check(absent.PrepareAppInfo().status == Status::SqlError && !std::filesystem::exists(missing), "default AppInfo open silently created DB");
	Check(absent.InitializeAppInfoSchema().status == Status::SqlError && !std::filesystem::exists(missing), "default AppInfo initialization silently created DB");
	Producer user_no_create(missing, {User}, AppInfoCreation::CreateIfMissing);
	Check(user_no_create.PrepareUser(User).status == Status::SqlError && !std::filesystem::exists(missing), "AppInfo creation policy leaked to user metadata");
	const auto corrupt = root / "corrupt.db";
	std::ofstream(corrupt) << "not SQLite";
	Producer broken(corrupt, {User});
	Check(broken.PrepareUser(User).status == Status::SqlError && std::filesystem::file_size(corrupt) == 10, "corruption reset/admitted");
	Producer explicit_create(root / "new-appinfo.db", {}, AppInfoCreation::CreateIfMissing);
	Check(explicit_create.PrepareAppInfo().status == Status::Committed, "explicit new AppInfo initialization failed");
	Producer do_not_reset(corrupt, {}, AppInfoCreation::CreateIfMissing);
	Check(do_not_reset.PrepareAppInfo().status == Status::SqlError && std::filesystem::file_size(corrupt) == 10,
	      "explicit AppInfo create reset existing corrupt data");
}
void CommitBoundary(const std::filesystem::path& path, bool cancel, bool appinfo = false) {
	Db reader(path);
	reader.Sql("CREATE TABLE existing(value);");
	if (appinfo) { reader.Sql("CREATE TABLE tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category)); INSERT INTO tbl_version VALUES('access_index',23);"); }
	reader.Sql("BEGIN;");
	Check(reader.Value("SELECT count(*) FROM existing") == 0, "hold real SQLite shared lock");
	Producer producer(path, {User});
	Result result;
	std::atomic<bool> returned {false};
	std::thread work([&] { result = appinfo ? producer.InitializeAppInfoSchema() : producer.PrepareUser(User); returned.store(true); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!producer.WaitingForDatabase() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	Check(producer.WaitingForDatabase() && !returned.load(), "receipt preceded COMMIT while reader held lock");
	Check(reader.Value(appinfo ? "SELECT count(*) FROM sqlite_master WHERE name='tbl_appinfo'" :
	                           "SELECT count(*) FROM sqlite_master WHERE name='tbl_version'") == 0, "uncommitted DDL visible");
	if (appinfo) { Check(reader.Value("SELECT status FROM tbl_version WHERE category='access_index'") == 23, "uncommitted index visible"); }
	if (cancel) { producer.Stop(); } // joins/rolls back before external reader release
	reader.Sql("ROLLBACK;");
	work.join();
	Check(result.status == (cancel ? Status::Cancelled : Status::Committed), "commit/cancellation boundary result");
	Check(reader.Value(appinfo ? "SELECT count(*) FROM sqlite_master WHERE name='tbl_appinfo'" :
	                           "SELECT count(*) FROM sqlite_master WHERE name='tbl_version'") == (cancel ? 0 : 1), "commit/rollback durable result");
	if (appinfo) {
		Check(reader.Value("SELECT status FROM tbl_version WHERE category='access_index'") == (cancel ? 23 : 24), "commit/cancel index durability");
		Check(cancel ? !result.access_index : result.access_index == 24, "index receipt crossed failed commit");
	}
}
void SystemCatalog(const std::filesystem::path& root, const std::filesystem::path& copied_db, const std::filesystem::path& sources) {
	std::atomic<bool> cancelled {false};
	CatalogRequest request {copied_db, root / "catalog-candidate", sources, {User, User + 1}, "en-US"};
	const auto receipt = BuildSystemCatalogCandidate(request, cancelled);
	if (receipt.status != CatalogStatus::Committed) { std::fprintf(stderr, "catalog: %s (%d)\n", receipt.detail.c_str(), receipt.sqlite_error); }
	Check(receipt.status == CatalogStatus::Committed && receipt.source_records == 2 && receipt.profile_records == 4,
	      "real Library sources did not commit for actual users");
	Db db(receipt.candidate_app_db);
	Check(db.Value("SELECT count(*) FROM hle_system_catalog_source WHERE length(param_json)>0 AND length(icon_png)>0") == 2, "source provenance missing");
	Check(std::filesystem::exists(request.new_candidate_directory / "app.db-corrupt"), "candidate lost copied corruption marker");
	for (auto user: request.local_users) {
		const std::string suffix = "_0" + std::to_string(user);
		const std::string icon = "tbl_iconinfo" + suffix;
		// Metadata constants: space Game=1<<1, Media=2<<1, EXSW=1<<7.
		// Test separately: aggregate two would hide an empty initial Games list.
		Check(db.Value(("SELECT count(*) FROM " + icon + " WHERE (dispLocation&128)<>0 AND (dispLocation&2)<>0 AND titleId='NPXS40071' AND titleName='Game Library' AND hubAppUri='pslibraryhub:main?space=game'").c_str()) == 1, "actual-user Games projection is empty/wrong");
		Check(db.Value(("SELECT count(*) FROM " + icon + " WHERE (dispLocation&128)<>0 AND (dispLocation&4)<>0 AND titleId='NPXS40139' AND titleName='App Library' AND hubAppUri='pslibraryhub:main?space=media'").c_str()) == 1, "actual-user Media projection is empty/wrong");
		Check(db.Value(("SELECT count(*) FROM " + icon + " WHERE entEntitlement IS NOT NULL OR installStatus IS NOT NULL OR lastAccessIndex IS NOT NULL OR conceptId IS NOT NULL").c_str()) == 0, "catalog fabricated unavailable metadata");
		Check(db.Value(("SELECT count(*) FROM " + icon + " AS icon JOIN tbl_contentinfo AS content USING(titleId) JOIN tbl_concepticoninfo" + suffix + " AS cicon USING(localConceptId) WHERE content.icon0Info=cicon.icon0Info AND cicon.validFlag=1").c_str()) == 2, "catalog joins not coherent");
	}
	Check(db.Value("SELECT count(*) FROM sqlite_master WHERE name='tbl_version'") == 0, "source import stamped a revision/version");
	sqlite3* original = nullptr;
	const auto utf8 = copied_db.u8string();
	Check(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &original, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "read-only catalog source open");
	const auto before = Contents(original), after = Contents(db.p); sqlite3_close(original);
	for (const auto& [name, rows]: before) { Check(after.at(name) == rows, "catalog candidate changed original rows"); }
	RunLocalHomeQueryTests(root, receipt.candidate_app_db, sources);
	RunLocalNativeCatalogTests(root, receipt.candidate_app_db, sources);
	RunLocalSystemCatalogCommitTests(root, copied_db, sources);
	// Explicit incremental mode keeps old user tables/rows untouched and adds
	// only a missing user to a private snapshot. User+2 is a fixture ID here.
	CatalogRequest incremental {receipt.candidate_app_db, root / "catalog-incremental", sources,
	                            {User, User + 1, User + 2}, "en-US", true};
	const auto extended = BuildSystemCatalogCandidate(incremental, cancelled);
	Check(extended.status == CatalogStatus::Committed && extended.source_records == 2 &&
	      extended.profile_records == 2 && extended.existing_profiles == 2 &&
	      extended.preserved_profiles == std::vector<int32_t> {User, User + 1}, "incremental catalog receipt differs");
	{
		Db extended_db(extended.candidate_app_db);
		const auto extended_contents = Contents(extended_db.p);
		for (const auto& [name, rows]: after) { Check(extended_contents.at(name) == rows, "incremental projection modified existing rows"); }
	}
	for (const auto space: {HomeSpace::Game, HomeSpace::Media}) {
		const auto read = ReadHomeRows({extended.candidate_app_db, User + 2, space, 14}, cancelled);
		Check(read.status == HomeReadStatus::Read && read.rows.size() == 1 &&
		      std::get<std::string>(read.rows[0].at("TitleId")) == (space == HomeSpace::Game ? "NPXS40071" : "NPXS40139"),
		      "incremental user cannot read source-backed Library");
	}
	incremental.copied_app_db = extended.candidate_app_db;
	incremental.new_candidate_directory = root / "catalog-incremental-repeat";
	const auto again = BuildSystemCatalogCandidate(incremental, cancelled);
	Check(again.status == CatalogStatus::Committed && again.profile_records == 0 && again.existing_profiles == 3 &&
	      again.preserved_profiles == std::vector<int32_t> {User, User + 1, User + 2},
	      "incremental recheck recreated or rejected existing profiles");
	const auto partial_path = root / "catalog-partial-source.db";
	{
		Db partial(partial_path);
		sqlite3* original_input = nullptr;
		const auto original_utf8 = copied_db.u8string();
		Check(sqlite3_open_v2(reinterpret_cast<const char*>(original_utf8.c_str()), &original_input, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
		      "open partial control input");
		auto* backup = sqlite3_backup_init(partial.p, "main", original_input, "main");
		Check(backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE && sqlite3_backup_finish(backup) == SQLITE_OK, "partial control backup");
		sqlite3_close(original_input);
		partial.Sql("CREATE TABLE tbl_iconinfo_0268500998(titleId TEXT); INSERT INTO tbl_iconinfo_0268500998 VALUES('preserve');");
		incremental.copied_app_db = partial_path;
		incremental.new_candidate_directory = root / "catalog-partial-incremental";
		incremental.local_users = {User + 2};
		const auto rejected = BuildSystemCatalogCandidate(incremental, cancelled);
		Check(rejected.status == CatalogStatus::Conflict && rejected.candidate_app_db.empty(), "partial profile silently adopted");
		Db failed_partial(incremental.new_candidate_directory / "app.db");
		Check(failed_partial.Value("SELECT count(*) FROM tbl_iconinfo_0268500998 WHERE titleId='preserve'") == 1 &&
		      failed_partial.Value("SELECT count(*) FROM sqlite_master WHERE name='hle_system_catalog_source'") == 0,
		      "partial profile failure leaked ledger or changed rows");
	}
	const auto mismatch_path = root / "catalog-ledger-mismatch.db";
	{
		Db mismatch(mismatch_path);
		sqlite3* source_input = nullptr;
		const auto source_utf8 = extended.candidate_app_db.u8string();
		Check(sqlite3_open_v2(reinterpret_cast<const char*>(source_utf8.c_str()), &source_input, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
		      "open ledger control input");
		auto* backup = sqlite3_backup_init(mismatch.p, "main", source_input, "main");
		Check(backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE && sqlite3_backup_finish(backup) == SQLITE_OK, "ledger control backup");
		sqlite3_close(source_input);
		mismatch.Sql("UPDATE hle_system_catalog_source SET icon_png=x'00' WHERE title_id='NPXS40071';");
		incremental.copied_app_db = mismatch_path;
		incremental.new_candidate_directory = root / "catalog-ledger-mismatch";
		incremental.local_users = {User + 3};
		const auto rejected = BuildSystemCatalogCandidate(incremental, cancelled);
		Check(rejected.status == CatalogStatus::Conflict && rejected.candidate_app_db.empty(), "changed retained source adopted");
		Db failed_ledger(incremental.new_candidate_directory / "app.db");
		Check(failed_ledger.Value("SELECT count(*) FROM sqlite_master WHERE name='tbl_iconinfo_0268500999'") == 0,
		      "ledger failure leaked new profile");
	}
	std::puts("SystemCatalog incremental: existing profiles preserved, new user source-backed, repeat idempotent, partial/source mismatch rejected");
	const auto repeated = BuildSystemCatalogCandidate(request, cancelled);
	Check(repeated.status == CatalogStatus::Conflict && repeated.candidate_app_db.empty(), "existing candidate overwritten");
	request.new_candidate_directory = root / "catalog-cancelled"; cancelled.store(true);
	Check(BuildSystemCatalogCandidate(request, cancelled).status == CatalogStatus::Cancelled && !std::filesystem::exists(request.new_candidate_directory), "pre-cancelled catalog wrote candidate"); cancelled.store(false);
	request.new_candidate_directory = root / "catalog-partial"; request.local_users.push_back(User);
	Check(BuildSystemCatalogCandidate(request, cancelled).status == CatalogStatus::InvalidInput && !std::filesystem::exists(request.new_candidate_directory), "duplicate roster admitted"); request.local_users.pop_back();
	// Failure on the second profile must roll back the first profile and source ledger.
	request.new_candidate_directory = root / "catalog-conflict"; request.local_users = {User + 2, 268435457};
	const auto conflict = BuildSystemCatalogCandidate(request, cancelled);
	Check(conflict.status != CatalogStatus::Committed && conflict.candidate_app_db.empty(), "existing profile catalog silently replaced");
	Db failed(request.new_candidate_directory / "app.db");
	Check(Contents(failed.p) == before, "failed catalog transaction leaked first user's records");
	// Alter only a disposable clone, never the copied input used for provenance.
	const auto unsupported = root / "catalog-unsupported.db";
	{
		Db clone(unsupported);
		auto* backup = sqlite3_backup_init(clone.p, "main", failed.p, "main");
		Check(backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE, "catalog negative-control backup");
		Check(sqlite3_backup_finish(backup) == SQLITE_OK, "catalog negative-control backup finish");
		clone.Sql("CREATE TRIGGER fixture_trigger AFTER INSERT ON tbl_contentinfo BEGIN SELECT 1; END;");
		request.copied_app_db = unsupported; request.local_users = {User}; request.new_candidate_directory = root / "catalog-trigger";
		Check(BuildSystemCatalogCandidate(request, cancelled).status == CatalogStatus::Conflict, "catalog admitted unknown trigger behavior");
		clone.Sql("DROP TRIGGER fixture_trigger; CREATE UNIQUE INDEX fixture_nocase ON tbl_contentinfo(titleId COLLATE NOCASE);");
		request.new_candidate_directory = root / "catalog-collation";
		Check(BuildSystemCatalogCandidate(request, cancelled).status == CatalogStatus::Conflict, "catalog admitted incompatible identity collation");
		clone.Sql("DROP INDEX fixture_nocase; UPDATE tbl_contentinfo SET icon0Info='conflicting-path' WHERE titleId='NPXS40071';");
		request.new_candidate_directory = root / "catalog-stale-source";
		Check(BuildSystemCatalogCandidate(request, cancelled).status == CatalogStatus::Conflict, "catalog silently repaired conflicting shared source");
	}
	request.copied_app_db = copied_db; request.copied_library_appdb = root / "missing-catalog-sources";
	request.new_candidate_directory = root / "catalog-missing-source";
	const auto missing = BuildSystemCatalogCandidate(request, cancelled);
	Check(missing.status == CatalogStatus::InvalidInput && missing.candidate_app_db.empty(), "missing descriptor produced receipt");
	Db missing_db(request.new_candidate_directory / "app.db");
	Check(Contents(missing_db.p) == before, "missing descriptor leaked projected metadata");
	std::puts("SystemCatalog: genuine Games/Media sources for both users, private snapshot preservation, atomic rollback, source/trigger/collation rejection passed");
}
}
int main(int argc, char** argv) {
	const auto root = std::filesystem::current_path() / ("local-appdb-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(root);
	RunLocalDbRecoveryTests(root, argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path());
	RunLocalDbPreparationTests(root, argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path());
	RunLocalCoreAppInfoTests(root);
	Lifecycle(root / "lifecycle.db");
	FailureAtomicity(root / "incompatible.db");
	AppInfoBase(root / "appinfo.db", false);
	AppInfoBase(root / "appinfo-incompatible.db", true);
	AppInfoInitialization(root / "appinfo-init.db");
	AppInfoIndexFailure(root / "appinfo-index-text.db", "UPDATE tbl_version SET status='retain';", Status::IncompatibleSchema);
	AppInfoIndexFailure(root / "appinfo-index-abort.db", "CREATE TRIGGER block BEFORE INSERT ON tbl_version BEGIN SELECT RAISE(ABORT,'fixture'); END;", Status::SqlError);
	AppInfoIndexFailure(root / "appinfo-index-ignore.db", "CREATE TRIGGER block BEFORE INSERT ON tbl_version BEGIN SELECT RAISE(IGNORE); END;", Status::SqlError);
	AppInfoIndexFailure(root / "appinfo-index-rewrite.db", "CREATE TRIGGER rewrite AFTER INSERT ON tbl_version BEGIN UPDATE tbl_version SET status=43; END;", Status::SqlError);
	ConcurrentAppInfoInitialization(root / "appinfo-concurrent.db");
	BusyStop(root / "busy.db");
	CommitBoundary(root / "commit.db", false);
	CommitBoundary(root / "commit-stop.db", true);
	CommitBoundary(root / "appinfo-commit.db", false, true);
	CommitBoundary(root / "appinfo-commit-stop.db", true, true);
	MissingAndCorrupt(root);
	if (argc >= 2) {
		// Read-only source backup, never connect the producer to the input file.
		sqlite3* input = nullptr;
		Check(sqlite3_open_v2(argv[1], &input, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "copied real input open");
		const auto original = Contents(input);
		{
			Db target(root / "real-input.db");
			auto* backup = sqlite3_backup_init(target.p, "main", input, "main");
			Check(backup != nullptr, "real input backup");
			Check(sqlite3_backup_step(backup, -1) == SQLITE_DONE, "real input backup complete");
			Check(sqlite3_backup_finish(backup) == SQLITE_OK, "real input backup finish");
		}
		sqlite3_close(input);
		Producer real(root / "real-input.db", {User, User + 1});
		Check(real.PrepareUser(User).status == Status::Committed, "real current-user schema work");
		Check(real.PrepareUser(User + 1).status == Status::Committed, "second real current-user schema work");
		Check(real.PrepareAppInfo().status == Status::Committed, "real copied input AppInfo base schema work");
		Db check(root / "real-input.db");
		const auto after = Contents(check.p);
		for (const auto& [name, rows]: original) { Check(after.at(name) == rows, "real source table rows changed"); }
	}
	if (argc == 3) { SystemCatalog(root, argv[1], argv[2]); }
	std::filesystem::remove_all(root);
	std::puts("LocalAppDb: transaction, preservation, incompatible-schema rollback, stop/contention, missing/corrupt controls passed");
}
