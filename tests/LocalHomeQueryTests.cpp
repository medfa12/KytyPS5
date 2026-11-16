#include "loader/runtimeLocalHomeQuery.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {
using namespace Loader::LocalAppDb;
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "HomeQuery: %s\n", why); std::abort(); } }
struct Db {
	sqlite3* p = nullptr;
	Db(const std::filesystem::path& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
		const auto text = path.u8string();
		Check(sqlite3_open_v2(reinterpret_cast<const char*>(text.c_str()), &p, flags, nullptr) == SQLITE_OK, "open fixture");
	}
	~Db() { sqlite3_close(p); }
	void Sql(const std::string& sql) { Check(sqlite3_exec(p, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, sql.c_str()); }
};
std::vector<std::string> Titles(const HomeReadReceipt& result) {
	if (result.status != HomeReadStatus::Read) { std::fprintf(stderr, "Home read error %d: %s\n", result.sqlite_error, result.detail.c_str()); }
	Check(result.status == HomeReadStatus::Read, "query failed");
	std::vector<std::string> ids;
	for (const auto& row: result.rows) { ids.push_back(std::get<std::string>(row.at("TitleId"))); }
	return ids;
}
}
void RunLocalHomeQueryTests(const std::filesystem::path& root, const std::filesystem::path& candidate,
                            const std::filesystem::path& sources) {
	using namespace Loader::LocalAppDb;
	std::atomic<bool> cancelled {false};
	HomeReadRequest request {candidate, 268500996, HomeSpace::Game, 10};
	for (int user = 268500996; user <= 268500997; ++user) {
		request.user_id = user;
		for (auto space: {HomeSpace::Game, HomeSpace::Media}) {
			request.space = space;
			const std::string title = space == HomeSpace::Game ? "NPXS40071" : "NPXS40139";
			const auto result = ReadHomeRows(request, cancelled);
			Check(Titles(result) == std::vector<std::string> {title}, "actual-user selected-space Library missing");
			const auto& row = result.rows.front();
			Check(row.size() == 32 && std::holds_alternative<std::monostate>(row.at("PinnedStatus")) &&
			      std::holds_alternative<std::monostate>(row.at("ConceptId")) &&
			      std::holds_alternative<std::monostate>(row.at("LastAccessIndex")), "projection lost dependencies or replaced NULL");
			Check(std::get<std::string>(row.at("HubAppUri")) == std::string("pslibraryhub:main?space=") +
			      (space == HomeSpace::Game ? "game" : "media"), "genuine hub dependency differs");
			const auto icon = std::get<std::string>(row.at("Icon0Info"));
			std::ifstream input(sources / (space == HomeSpace::Game ? "default" : "NPXS40139") / "icon0.png", std::ios::binary);
			Check(input.good(), "read source icon control");
			HomeBlob bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			Check(result.retained_assets.size() == 1 && result.retained_assets.at(icon) == bytes, "guest path is not bound to exact retained PNG bytes");
			request.content_limit = 1;
			Check(Titles(ReadHomeRows(request, cancelled)) == std::vector<std::string> {title}, "limit one must append actual Library");
			request.content_limit = 10;
		}
	}
	const auto fixture = root / "home-controls.db";
	Db db(fixture), source(candidate, SQLITE_OPEN_READONLY);
	auto* backup = sqlite3_backup_init(db.p, "main", source.p, "main");
	Check(backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE, "clone candidate");
	Check(sqlite3_backup_finish(backup) == SQLITE_OK, "finish clone");
	request = {fixture, 268500996, HomeSpace::Game, 3};
	const std::string icon = "tbl_iconinfo_0268500996";
	// Synthetic rows exist ONLY in this disposable clone. Deliberately disagree
	// pinned/access order so a single ORDER BY implementation cannot pass.
	db.Sql("UPDATE " + icon + " SET visible=0,dispLocation=0;");
	for (const char* title: {"fixture-A", "fixture-B", "fixture-C", "fixture-null-visible", "fixture-null-space"}) {
		db.Sql(std::string("INSERT INTO tbl_contentinfo(titleId,sortPriority) VALUES('") + title + "',100);");
		db.Sql("INSERT INTO " + icon + "(titleId,visible,dispLocation,pinnedStatus,lastAccessIndex) VALUES('" + title + "',1,130,0,20);");
	}
	db.Sql("UPDATE " + icon + " SET pinnedStatus=1,lastAccessIndex=1 WHERE titleId='fixture-A';"
	       "UPDATE " + icon + " SET pinnedStatus=NULL,lastAccessIndex=30 WHERE titleId='fixture-B';"
	       "UPDATE " + icon + " SET visible=NULL WHERE titleId='fixture-null-visible';"
	       "UPDATE " + icon + " SET dispLocation=NULL WHERE titleId='fixture-null-space';");
	Check(Titles(ReadHomeRows(request, cancelled)) == std::vector<std::string> {"fixture-B", "fixture-A", "NPXS40071"},
	      "inner pinned selection / outer access ordering / unfiltered append differ");
	// Missing content/concept remains a LEFT JOIN result; NULL priority sorts
	// first in ascending SQLite order. Do not repair it to a guessed priority.
	db.Sql("INSERT INTO " + icon + "(titleId,visible,dispLocation,lastAccessIndex) VALUES('fixture-no-content',1,130,5);");
	request.content_limit = 4;
	auto result = ReadHomeRows(request, cancelled);
	Check(Titles(result) == std::vector<std::string> {"fixture-no-content", "fixture-B", "fixture-A", "NPXS40071"}, "NULL priority or left joins changed");
	Check(std::holds_alternative<std::monostate>(result.rows[0].at("BgMatteType")) &&
	      std::holds_alternative<std::monostate>(result.rows[0].at("ConceptIcon0Info")), "missing joins fabricated output dependencies");
	request.content_limit = 0;
	Check(Titles(ReadHomeRows(request, cancelled)) == std::vector<std::string> {"fixture-no-content", "fixture-B", "fixture-C", "fixture-A", "NPXS40071"}, "native zero limit is unlimited inner query");
	request.space = HomeSpace::Media;
	Check(Titles(ReadHomeRows(request, cancelled)) == std::vector<std::string> {"NPXS40139"}, "game mask leaked into Media");
	request.space = HomeSpace::Game;
	request.max_rows = 1;
	result = ReadHomeRows(request, cancelled);
	Check(result.status == HomeReadStatus::ResourceLimit && result.rows.empty() && result.retained_assets.empty(), "resource budget returned partial success");
	request.max_rows = 4096;
	request.content_limit = 2;
	db.Sql("UPDATE " + icon + " SET visible=1,dispLocation=130 WHERE titleId='NPXS40071';"
	       "UPDATE tbl_contentinfo SET sortPriority=-1 WHERE titleId='NPXS40071';"
	       "DELETE FROM " + icon + " WHERE titleId='fixture-no-content';");
	result = ReadHomeRows(request, cancelled);
	Check(Titles(result) == std::vector<std::string> {"NPXS40071"}, "Library duplicated or reserve slot filled again");
	const auto owned_assets = result.retained_assets;
	db.Sql("UPDATE hle_system_catalog_source SET icon_png=x'00' WHERE title_id='NPXS40071';");
	Check(result.retained_assets == owned_assets, "returned assets borrowed mutable database storage");
	result = ReadHomeRows(request, cancelled);
	Check(result.status == HomeReadStatus::SourceConflict && result.rows.empty() && result.retained_assets.empty(), "bad retained source admitted partial transport");
	db.Sql("DELETE FROM " + icon + " WHERE titleId='NPXS40071';");
	request.content_limit = 1;
	Check(Titles(ReadHomeRows(request, cancelled)).empty(), "missing Library fabricated fallback");
	request.user_id = 268500998;
	Check(ReadHomeRows(request, cancelled).status == HomeReadStatus::SqlError, "unknown profile redirected to another user");
	request.user_id = 268500996;
	request.content_limit = -1;
	Check(ReadHomeRows(request, cancelled).status == HomeReadStatus::InvalidInput, "unsupported negative limit accepted");
	request.content_limit = 1;
	cancelled.store(true);
	Check(ReadHomeRows(request, cancelled).status == HomeReadStatus::Cancelled, "pre-cancelled read succeeded");
	cancelled.store(false);
	request.candidate_app_db = root / "missing-home.db";
	Check(ReadHomeRows(request, cancelled).status == HomeReadStatus::SqlError && !std::filesystem::exists(request.candidate_app_db), "read created missing catalog");
	std::puts("HomeQuery: both actual users/spaces, full dependency projection, NULL/left joins, two-stage order/limit, Library append/dedup, owned source assets and failure controls passed");
}
