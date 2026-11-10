#include "loader/runtimeLocalHomeQuery.h"
#include <sqlite3.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace Loader::LocalAppDb {
namespace {
struct Failure { HomeReadStatus status; int sql; std::string why; };
void SqlOk(int rc, const char* why) {
	if (rc != SQLITE_OK) { throw Failure {rc == SQLITE_INTERRUPT ? HomeReadStatus::Cancelled : HomeReadStatus::SqlError, rc, why}; }
}
struct Db {
	sqlite3* p = nullptr;
	~Db() { if (p) { sqlite3_progress_handler(p, 0, nullptr, nullptr); sqlite3_exec(p, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close_v2(p); } }
};
struct Stmt {
	sqlite3_stmt* p = nullptr;
	Stmt(sqlite3* db, const std::string& sql) { SqlOk(sqlite3_prepare_v2(db, sql.c_str(), -1, &p, nullptr), "prepare Home query"); }
	~Stmt() { sqlite3_finalize(p); }
	bool Next() { const int rc = sqlite3_step(p); if (rc == SQLITE_ROW) { return true; } SqlOk(rc == SQLITE_DONE ? SQLITE_OK : rc, "read Home query"); return false; }
	void Text(int index, const std::string& text) { SqlOk(sqlite3_bind_text64(p, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind Home text"); }
};
int Cancel(void* ptr) { return static_cast<const std::atomic<bool>*>(ptr)->load() ? 1 : 0; }
void CheckCancel(const std::atomic<bool>& cancelled) {
	if (cancelled.load()) { throw Failure {HomeReadStatus::Cancelled, SQLITE_INTERRUPT, "cancelled Home read"}; }
}
HomeValue Value(sqlite3_stmt* s, int i) {
	switch (sqlite3_column_type(s, i)) {
	case SQLITE_NULL: return std::monostate {};
	case SQLITE_INTEGER: return int64_t(sqlite3_column_int64(s, i));
	case SQLITE_FLOAT: return sqlite3_column_double(s, i);
	case SQLITE_TEXT: {
		const auto* p = sqlite3_column_text(s, i);
		if (!p) { SqlOk(SQLITE_NOMEM, "read Home text"); }
		return std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, i));
	}
	default: {
		const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(s, i));
		const int n = sqlite3_column_bytes(s, i);
		return n ? HomeBlob(p, p + n) : HomeBlob {};
	}
	}
}
// DB AOT +3f310 dependency/alias dictionaries, +4d020 selection expansion.
// No COALESCE on materialized values: managed getters own default conversion.
constexpr const char* Select =
	"SELECT icon.titleId AS TitleId,icon.titleName AS TitleName,icon.localConceptId AS LocalConceptId,"
	"icon.conceptId AS ConceptId,content.bgMatteType AS BgMatteType,content.icon0Info AS Icon0Info,"
	"content.pic0Info AS Pic0Info,content.snd0Info AS Snd0Info,content.pprDeeplinkUri AS DeeplinkUri,"
	"icon.installStatus AS InstallStatus,content.appinfoContentType AS AppContentType,icon.entEntitlement AS EntEntitlement,"
	"icon.contentId AS ContentId,icon.entStrId AS EntStrId,icon.entStrActiveFlag AS EntStrActiveFlag,"
	"icon.hubAppUri AS HubAppUri,icon.platform AS Platform,icon.psNowFlag AS PSNowFlag,icon.pinnedStatus AS PinnedStatus,"
	"cicon.metadataPath AS ConceptMetadataPath,cicon.icon0Info AS ConceptIcon0Info,cicon.pic0Info AS ConceptPic0Info,"
	"cicon.descriptions AS Description,cicon.ucmIcon0Info AS ConceptIconUrl,cicon.ucmPic0Info AS ConceptBgImageUrl,"
	"cicon.ucmLogoUrl AS LogoUrl,cicon.lastPlayedDate AS ConceptLastPlayedDate,metadata.logoImageInfo AS LogoImageInfo,"
	"content.pprAgeLevel AS AgeLevel,content.ps4parentalLevel AS ParentalLevel,"
	"content.sortPriority AS SortPriority,icon.lastAccessIndex AS LastAccessIndex ";
HomeRow Row(sqlite3_stmt* s) {
	HomeRow row;
	for (int i = 0; i < sqlite3_column_count(s); ++i) { row.emplace(sqlite3_column_name(s, i), Value(s, i)); }
	return row;
}
void Append(std::vector<HomeRow>& rows, HomeRow row, size_t limit) {
	if (rows.size() >= limit) { throw Failure {HomeReadStatus::ResourceLimit, 0, "Home result exceeds caller row budget"}; }
	rows.push_back(std::move(row));
}
void BindAssets(sqlite3* db, HomeReadReceipt& receipt) {
	// Exact identity/path joins prevent redirecting an arbitrary guest file to a
	// descriptor icon. No donor file is reopened after the candidate was built.
	Stmt assets(db, "SELECT metadata_path,icon_png,param_json FROM hle_system_catalog_source WHERE title_id=?1 COLLATE BINARY");
	for (const auto& row: receipt.rows) {
		const auto* title = std::get_if<std::string>(&row.at("TitleId"));
		if (!title || (*title != "NPXS40071" && *title != "NPXS40139")) { continue; }
		assets.Text(1, *title);
		if (!assets.Next()) { throw Failure {HomeReadStatus::SourceConflict, 0, "Library has no retained source"}; }
		const auto path_value = Value(assets.p, 0), bytes_value = Value(assets.p, 1);
		const auto* path = std::get_if<std::string>(&path_value);
		const auto* bytes = std::get_if<HomeBlob>(&bytes_value);
		const auto descriptor_value = Value(assets.p, 2);
		const auto* descriptor = std::get_if<std::string>(&descriptor_value);
		const auto* icon = std::get_if<std::string>(&row.at("Icon0Info"));
		const auto* concept_icon = std::get_if<std::string>(&row.at("ConceptIcon0Info"));
		const std::string expected = std::string("/system_ex/rnps/apps/NPXS40071/appdb/") + (*title == "NPXS40071" ? "default" : "NPXS40139");
		if (!descriptor || descriptor->empty() || !path || *path != expected || !icon || *icon != expected + "/icon0.png" ||
		    (concept_icon && *concept_icon != *icon) || !bytes || bytes->size() < 24 ||
		    !std::equal(bytes->begin(), bytes->begin() + 8, reinterpret_cast<const uint8_t*>("\x89PNG\r\n\x1a\n"))) {
			throw Failure {HomeReadStatus::SourceConflict, 0, "Library asset source/path mismatch"};
		}
		receipt.retained_assets.emplace(*icon, *bytes);
		receipt.retained_descriptors.emplace(expected + "/param.json", *descriptor);
		if (assets.Next()) { throw Failure {HomeReadStatus::SourceConflict, 0, "duplicate Library source"}; }
		SqlOk(sqlite3_reset(assets.p), "reset asset lookup");
	}
}
}
HomeReadReceipt ReadHomeRows(const HomeReadRequest& request, const std::atomic<bool>& cancelled) {
	HomeReadReceipt result;
	try {
		if (request.user_id < 0 || request.content_limit < 0 || !request.max_rows ||
		    (request.space != HomeSpace::Game && request.space != HomeSpace::Media)) {
			throw Failure {HomeReadStatus::InvalidInput, 0, "unsupported Home request"};
		}
		CheckCancel(cancelled);
		Db db;
		const auto path = request.candidate_app_db.u8string();
		SqlOk(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db.p, SQLITE_OPEN_READONLY, nullptr), "open Home candidate read-only");
		sqlite3_progress_handler(db.p, 100, Cancel, const_cast<std::atomic<bool>*>(&cancelled));
		SqlOk(sqlite3_exec(db.p, "BEGIN", nullptr, nullptr, nullptr), "begin Home snapshot");
		char suffix[24]; std::snprintf(suffix, sizeof(suffix), "_%010d", request.user_id);
		const std::string from = std::string("FROM tbl_iconinfo") + suffix + " AS icon "
		    "LEFT OUTER JOIN tbl_contentinfo AS content USING(titleId) LEFT OUTER JOIN tbl_concepticoninfo" + suffix +
		    " AS cicon USING(localConceptId) LEFT OUTER JOIN tbl_conceptmetadata AS metadata USING(localConceptId) ";
		// +4512b visible default, +45528 space, +45dd7 EXSW display.
		// +44210 inner priority/pinned/index; +44b30 drops pinned outside LIMIT.
		Stmt items(db.p, std::string("SELECT * FROM (") + Select + from +
		    "WHERE icon.visible=1 AND (icon.dispLocation & ?1)<>0 AND (icon.dispLocation & 128)<>0 "
		    "ORDER BY content.sortPriority ASC,CASE WHEN icon.pinnedStatus IS NULL THEN 0 ELSE icon.pinnedStatus END DESC,"
		    "icon.lastAccessIndex DESC LIMIT ?2 OFFSET 0) ORDER BY SortPriority ASC,LastAccessIndex DESC");
		SqlOk(sqlite3_bind_int(items.p, 1, request.space == HomeSpace::Game ? 2 : 4), "bind Home space");
		SqlOk(sqlite3_bind_int64(items.p, 2, int64_t(request.content_limit) - 1), "bind Home limit");
		while (items.Next()) { CheckCancel(cancelled); Append(result.rows, Row(items.p), request.max_rows); }
		// +4c23d / +85a0: append actual Library if not already selected, bypassing
		// visibility/space/EXSW/limit; absent Library remains absent.
		const std::string library = request.space == HomeSpace::Game ? "NPXS40071" : "NPXS40139";
		const bool found = std::any_of(result.rows.begin(), result.rows.end(), [&](const HomeRow& row) {
			const auto* id = std::get_if<std::string>(&row.at("TitleId")); return id && *id == library;
		});
		if (!found) {
			Stmt item(db.p, std::string(Select) + from + "WHERE icon.titleId=?1"); item.Text(1, library);
			if (item.Next()) { Append(result.rows, Row(item.p), request.max_rows); }
		}
		BindAssets(db.p, result);
		CheckCancel(cancelled);
		result.status = HomeReadStatus::Read;
	} catch (const Failure& e) {
		result.status = e.status; result.sqlite_error = e.sql; result.detail = e.why;
		result.rows.clear(); result.retained_assets.clear(); result.retained_descriptors.clear();
	} catch (const std::exception& e) {
		result.status = HomeReadStatus::InvalidInput; result.detail = e.what();
		result.rows.clear(); result.retained_assets.clear(); result.retained_descriptors.clear();
	}
	return result;
}
} // namespace Loader::LocalAppDb
