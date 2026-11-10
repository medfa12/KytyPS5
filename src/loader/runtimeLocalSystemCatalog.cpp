#include "loader/runtimeLocalSystemCatalog.h"
#include <sqlite3.h>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <cstdio>
#include <utility>

namespace Loader::LocalAppDb {
namespace {
struct Failure { CatalogStatus status; int sql; std::string why; };
void Require(bool ok, const char* why, CatalogStatus status = CatalogStatus::InvalidInput) {
	if (!ok) { throw Failure {status, 0, why}; }
}
void SqlOk(int rc, const char* why) { if (rc != SQLITE_OK) { throw Failure {CatalogStatus::SqlError, rc, why}; } }
struct Db {
	sqlite3* p = nullptr;
	~Db() { if (p) { sqlite3_close_v2(p); } }
	void Open(const std::filesystem::path& path, int flags) {
		const auto s = path.u8string();
		SqlOk(sqlite3_open_v2(reinterpret_cast<const char*>(s.c_str()), &p, flags, nullptr), "open database");
	}
	void Exec(const std::string& sql) { SqlOk(sqlite3_exec(p, sql.c_str(), nullptr, nullptr, nullptr), "execute catalog transaction"); }
};
struct Stmt {
	sqlite3_stmt* p = nullptr;
	Stmt(sqlite3* db, const std::string& sql) { SqlOk(sqlite3_prepare_v2(db, sql.c_str(), -1, &p, nullptr), "prepare catalog SQL"); }
	~Stmt() { sqlite3_finalize(p); }
	void Bind(int i, const std::string& s) { SqlOk(sqlite3_bind_text64(p, i, s.data(), s.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind catalog text"); }
	int Step() {
		const int rc = sqlite3_step(p);
		if (rc != SQLITE_ROW && rc != SQLITE_DONE) { throw Failure {CatalogStatus::SqlError, rc, "step catalog SQL"}; }
		return rc;
	}
	std::string Text(int i) const {
		const auto* s = sqlite3_column_text(p, i);
		return s ? std::string(reinterpret_cast<const char*>(s), sqlite3_column_bytes(p, i)) : std::string();
	}
};
std::string Read(const std::filesystem::path& path, size_t limit) {
	std::ifstream in(path, std::ios::binary);
	Require(in.good(), "missing source file");
	std::string s;
	std::array<char, 4096> buffer;
	while (in) {
		in.read(buffer.data(), buffer.size()); s.append(buffer.data(), static_cast<size_t>(in.gcount()));
		Require(s.size() <= limit, "source file exceeds supported size");
	}
	Require(in.eof() && !s.empty(), "unreadable or empty source file");
	return s;
}
// This is the pinned managed-reader column contract, not a claim to reproduce
// native migration DDL. Unavailable metadata remains NULL (no synthetic dates,
// entitlements, install flags, ordering, or access indices).
constexpr const char* IconColumns = "titleId TEXT PRIMARY KEY,localConceptId,titleName,contentId,lastPlayedDate,entEntitlement,installStatus,entHidden,conceptId,entStrId,entStrActiveFlag,hubAppUri,platform,psNowFlag,dispLocation,isVr,discStatus,appDrmType,betaFlag,gltTotalPlayedTime,playedTimeOnConsole,contentBadgeType,pinnedStatus,lastLaunchMethod,utmIconUrl,utmBgImageUrl,entPlayabilityDate,entId,entActiveFlag,entFreePsPlusContent,utmEstimatedSize,entActiveDate,entStatusType,entUniId,entUniActiveFlag,entUniRewardMembershipType,entUniFeatureType,entStrStatusType,entStrRewardMembershipType,entStrFeatureType,gltLastPlayedDate,lastAccessTime,visible,lastAccessIndex,LastPlayedDateOnConsole,primaryTitleSort,recentActivityDatePlayedOrPurchased,recentActivityDatePlayedOrInstalled,entFilTypeEnt,playlistId,entPurchasedDate,installedDate";
constexpr const char* ConceptColumns = "localConceptId TEXT PRIMARY KEY,classicGamePlatform,metadataPath,icon0Info,pic0Info,descriptions,ucmIcon0Info,ucmPic0Info,ucmLogoUrl,lastPlayedDate,genreFlags,conceptName,primaryTitleId,primaryTitleName,primaryTitleBgMatteType,deeplinkUri,psNowFlag,hubAppUri,primaryTitlePlatform,snd0Info,totalEstimatedSize,publisherName,totalPlayedTime,validFlag,lastAccessIndex";
struct Source {
	std::string title, name, content, hub, deeplink, matte, metadata, json, icon;
	int64_t display = 0, category = 0;
};
std::string JsonText(sqlite3* db, const std::string& json, const std::string& path) {
	Stmt s(db, "SELECT json_type(?1,?2),json_extract(?1,?2)"); s.Bind(1, json); s.Bind(2, path);
	Require(s.Step() == SQLITE_ROW && s.Text(0) == "text", "missing or nontext catalog field");
	auto text = s.Text(1); Require(text.find('\0') == std::string::npos, "embedded NUL in catalog field"); return text;
}
Source Load(sqlite3* db, const CatalogRequest& request, const char* directory, const char* title) {
	Source r;
	r.json = Read(request.copied_library_appdb / directory / "param.json", 256 * 1024);
	r.icon = Read(request.copied_library_appdb / directory / "icon0.png", 4 * 1024 * 1024);
	Require(r.icon.size() >= 24 && r.icon.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0, "invalid source icon");
	Stmt valid(db, "SELECT json_valid(?1)"); valid.Bind(1, r.json);
	Require(valid.Step() == SQLITE_ROW && sqlite3_column_int(valid.p, 0) == 1, "invalid source JSON");
	Stmt duplicate(db, "SELECT 1 FROM json_tree(?1) WHERE key IS NOT NULL GROUP BY parent,key HAVING count(*)>1 LIMIT 1"); duplicate.Bind(1, r.json);
	Require(duplicate.Step() == SQLITE_DONE, "duplicate source JSON key");
	r.title = JsonText(db, r.json, "$.titleId"); Require(r.title == title, "unsupported Library descriptor identity");
	r.content = JsonText(db, r.json, "$.contentId");
	r.hub = JsonText(db, r.json, "$.hubAppUri");
	r.deeplink = JsonText(db, r.json, "$.deeplinkUri");
	r.matte = JsonText(db, r.json, "$.backgroundBasematType");
	const auto fallback = JsonText(db, r.json, "$.localizedParameters.defaultLanguage");
	Stmt locale(db, "SELECT json_type(?1,?2)"); locale.Bind(1, r.json);
	locale.Bind(2, "$.localizedParameters.\"" + request.language + "\".titleName"); locale.Step();
	const auto language = locale.Text(0) == "text" ? request.language : fallback;
	Require(language.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos,
	        "invalid source locale");
	r.name = JsonText(db, r.json, "$.localizedParameters.\"" + language + "\".titleName");
	Require(!r.name.empty() && !r.hub.empty() && !r.content.empty(), "empty required source value");
	Stmt values(db, "SELECT json_type(?1,'$.displayLocation'),json_extract(?1,'$.displayLocation'),json_type(?1,'$.applicationCategoryType'),json_extract(?1,'$.applicationCategoryType')");
	values.Bind(1, r.json); values.Step();
	Require(values.Text(0) == "integer" && values.Text(2) == "integer", "noninteger source flags");
	r.display = sqlite3_column_int64(values.p, 1); r.category = sqlite3_column_int64(values.p, 3);
	Require(r.display >= 0 && r.display <= 0xffffffffLL && r.category == 0x4000000, "unsupported source category/flags");
	r.metadata = std::string("/system_ex/rnps/apps/NPXS40071/appdb/") + directory;
	return r;
}
void NoTriggers(sqlite3* db) {
	Stmt s(db, "SELECT 1 FROM sqlite_master WHERE type='trigger' LIMIT 1");
	Require(s.Step() == SQLITE_DONE, "candidate with schema triggers requires native reconciliation", CatalogStatus::Conflict);
	Stmt table(db, "SELECT type FROM sqlite_master WHERE name='tbl_contentinfo'");
	Require(table.Step() == SQLITE_ROW && table.Text(0) == "table", "shared catalog must be a stored table", CatalogStatus::Conflict);
	Stmt collation(db, "SELECT 1 FROM pragma_index_list('tbl_contentinfo') l JOIN pragma_index_xinfo(l.name) x WHERE x.key<>0 AND (x.coll IS NULL OR x.coll<>'BINARY') LIMIT 1");
	Require(collation.Step() == SQLITE_DONE, "shared catalog index collation is unsupported", CatalogStatus::Conflict);
}
void CheckContent(sqlite3* db, const Source& r) {
	// Existing shared rows are retained verbatim, including their unknown fields.
	// This narrow operation cannot reconcile conflicting/stale shared metadata.
	Stmt s(db, "SELECT typeof(titleId),titleId,metaDataPath,icon0Info,categoryType,bgMatteType,pprDeeplinkUri FROM tbl_contentinfo WHERE titleId=?1 COLLATE BINARY");
	s.Bind(1, r.title);
	Require(s.Step() == SQLITE_ROW && s.Text(0) == "text" && s.Text(1) == r.title && s.Text(2) == r.metadata &&
	        s.Text(3) == r.metadata + "/icon0.png" && sqlite3_column_type(s.p, 4) == SQLITE_INTEGER &&
	        sqlite3_column_int64(s.p, 4) == r.category && s.Text(5) == r.matte && s.Text(6) == r.deeplink,
	        "shared catalog source differs or is absent", CatalogStatus::Conflict);
	Require(s.Step() == SQLITE_DONE, "duplicate shared source identity", CatalogStatus::Conflict);
}
bool StoredTable(sqlite3* db, const std::string& name) {
	Stmt s(db, "SELECT type FROM sqlite_master WHERE name=?1 COLLATE BINARY");
	s.Bind(1, name);
	if (s.Step() == SQLITE_DONE) { return false; }
	Require(s.Text(0) == "table" && s.Step() == SQLITE_DONE,
	        "catalog name is not a unique stored table", CatalogStatus::Conflict);
	return true;
}
void CheckRetainedSource(sqlite3* db, const Source& source) {
	Stmt s(db, "SELECT metadata_path,param_json,icon_png FROM hle_system_catalog_source WHERE title_id=?1 COLLATE BINARY");
	s.Bind(1, source.title);
	Require(s.Step() == SQLITE_ROW && sqlite3_column_type(s.p, 0) == SQLITE_TEXT &&
	        sqlite3_column_type(s.p, 1) == SQLITE_TEXT && sqlite3_column_type(s.p, 2) == SQLITE_BLOB &&
	        s.Text(0) == source.metadata && s.Text(1) == source.json, "retained catalog source differs", CatalogStatus::Conflict);
	const auto* icon = static_cast<const char*>(sqlite3_column_blob(s.p, 2));
	const int size = sqlite3_column_bytes(s.p, 2);
	Require(size >= 0 && static_cast<size_t>(size) == source.icon.size() && icon &&
	        std::string(icon, static_cast<size_t>(size)) == source.icon && s.Step() == SQLITE_DONE,
	        "retained catalog icon differs", CatalogStatus::Conflict);
}
int Cancel(void* c) { return static_cast<const std::atomic<bool>*>(c)->load() ? 1 : 0; }
}
CatalogReceipt BuildSystemCatalogCandidate(const CatalogRequest& request, const std::atomic<bool>& cancelled) {
	CatalogReceipt receipt;
	try {
		Require(!request.local_users.empty() && !request.language.empty() && request.language.find_first_not_of(
		    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos, "invalid roster/language");
		std::set<int32_t> unique;
		for (auto user: request.local_users) { Require(user >= 0 && unique.insert(user).second, "invalid or duplicate local user"); }
		if (cancelled.load()) { throw Failure {CatalogStatus::Cancelled, SQLITE_INTERRUPT, "cancelled before backup"}; }
		Db input; input.Open(request.copied_app_db, SQLITE_OPEN_READONLY);
		auto retained_directory = request.new_candidate_directory;
		Require(std::filesystem::create_directory(request.new_candidate_directory), "candidate directory already exists", CatalogStatus::Conflict);
		receipt.retained_candidate_directory = std::move(retained_directory);
		auto path = request.new_candidate_directory / "app.db";
		Db db; db.Open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
		auto* backup = sqlite3_backup_init(db.p, "main", input.p, "main");
		Require(backup != nullptr, "cannot start read-only backup", CatalogStatus::SqlError);
		int rc = SQLITE_OK;
		do { rc = sqlite3_backup_step(backup, 128); } while (rc == SQLITE_OK && !cancelled.load());
		const int finish = sqlite3_backup_finish(backup);
		receipt.input_backup_complete = rc == SQLITE_DONE && finish == SQLITE_OK;
		if (cancelled.load()) { throw Failure {CatalogStatus::Cancelled, SQLITE_INTERRUPT, "cancelled during backup"}; }
		SqlOk(rc == SQLITE_DONE ? finish : rc, "snapshot backup");
		// The marker is copied as evidence; SQL integrity cannot remove it.
		const auto marker = std::filesystem::path(request.copied_app_db.native() + std::filesystem::path("-corrupt").native());
		if (std::filesystem::exists(marker)) { std::filesystem::copy_file(marker, request.new_candidate_directory / "app.db-corrupt"); }
		sqlite3_progress_handler(db.p, 100, Cancel, const_cast<std::atomic<bool>*>(&cancelled));
		db.Exec("BEGIN IMMEDIATE");
		struct Rollback { sqlite3* db; ~Rollback() { sqlite3_progress_handler(db, 0, nullptr, nullptr); sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); } } rollback {db.p};
		Stmt integrity(db.p, "PRAGMA quick_check");
		Require(integrity.Step() == SQLITE_ROW && integrity.Text(0) == "ok" && integrity.Step() == SQLITE_DONE, "copied input integrity failure", CatalogStatus::Conflict);
		NoTriggers(db.p);
		const std::array<Source, 2> records {Load(db.p, request, "default", "NPXS40071"), Load(db.p, request, "NPXS40139", "NPXS40139")};
		for (const auto& r: records) { CheckContent(db.p, r); }
		// Reuse only an independently matching retained ledger. Never rewrite it
		// to conceal changed firmware or an existing candidate's provenance.
		if (request.preserve_existing_profiles && StoredTable(db.p, "hle_system_catalog_source")) {
			for (const auto& r: records) { CheckRetainedSource(db.p, r); }
		} else {
			db.Exec("CREATE TABLE hle_system_catalog_source(title_id TEXT PRIMARY KEY,metadata_path TEXT NOT NULL,param_json TEXT NOT NULL,icon_png BLOB NOT NULL)");
			for (const auto& r: records) {
				Stmt source(db.p, "INSERT INTO hle_system_catalog_source VALUES(?1,?2,?3,?4)");
				source.Bind(1, r.title); source.Bind(2, r.metadata); source.Bind(3, r.json);
				SqlOk(sqlite3_bind_blob64(source.p, 4, r.icon.data(), r.icon.size(), SQLITE_TRANSIENT), "bind source icon"); source.Step();
			}
		}
		size_t created = 0;
		std::vector<int32_t> existing;
		for (auto user: request.local_users) {
			char suffix[24]; std::snprintf(suffix, sizeof(suffix), "_%010d", user);
			const std::string icon = std::string("tbl_iconinfo") + suffix, concept_table = std::string("tbl_concepticoninfo") + suffix;
			if (request.preserve_existing_profiles) {
				const bool icon_exists = StoredTable(db.p, icon), concept_exists = StoredTable(db.p, concept_table);
				Require(icon_exists == concept_exists, "partial profile catalog requires reconciliation", CatalogStatus::Conflict);
				if (icon_exists) { existing.push_back(user); continue; }
			}
			// CREATE without IF NOT EXISTS: no racing/partial table is adopted.
			db.Exec("CREATE TABLE " + icon + "(" + IconColumns + ")");
			db.Exec("CREATE TABLE " + concept_table + "(" + ConceptColumns + ")");
			++created;
			for (const auto& r: records) {
				const std::string local = "cid:local:" + r.title; // Core +f82a60.
				Stmt row(db.p, "INSERT INTO " + icon + "(titleId,localConceptId,titleName,contentId,hubAppUri,dispLocation,visible) VALUES(?1,?2,?3,?4,?5,?6,?7)");
				row.Bind(1, r.title); row.Bind(2, local); row.Bind(3, r.name); row.Bind(4, r.content); row.Bind(5, r.hub);
				SqlOk(sqlite3_bind_int64(row.p, 6, r.display), "bind display bits");
				SqlOk(sqlite3_bind_int(row.p, 7, (r.display & 1) == 0), "bind descriptor visibility"); row.Step();
				Stmt c(db.p, "INSERT INTO " + concept_table + "(localConceptId,metadataPath,icon0Info,conceptName,primaryTitleId,primaryTitleName,primaryTitleBgMatteType,deeplinkUri,hubAppUri,validFlag) VALUES(?1,?2,?3,?4,?5,?4,?6,?7,?8,1)");
				c.Bind(1, local); c.Bind(2, r.metadata); c.Bind(3, r.metadata + "/icon0.png"); c.Bind(4, r.name); c.Bind(5, r.title);
				c.Bind(6, r.matte); c.Bind(7, r.deeplink); c.Bind(8, r.hub); c.Step();
			}
		}
		if (cancelled.load()) { throw Failure {CatalogStatus::Cancelled, SQLITE_INTERRUPT, "cancelled before commit"}; }
		// Commit owns the outcome after the final cooperative cancellation check.
		// Do not allow a later progress callback or flag load to relabel it.
		sqlite3_progress_handler(db.p, 0, nullptr, nullptr);
		db.Exec("COMMIT");
		// path's move assignment is noexcept: no receipt allocation after COMMIT.
		receipt.status = CatalogStatus::Committed; receipt.candidate_app_db = std::move(path);
		receipt.source_records = records.size(); receipt.profile_records = records.size() * created;
		receipt.existing_profiles = existing.size();
		receipt.preserved_profiles = std::move(existing);
	} catch (const Failure& e) {
		receipt.status = e.sql == SQLITE_INTERRUPT && cancelled.load() ? CatalogStatus::Cancelled : e.status;
		receipt.sqlite_error = e.sql; receipt.detail = e.why;
	} catch (const std::exception& e) { receipt.detail = e.what(); }
	return receipt;
}
} // namespace Loader::LocalAppDb
