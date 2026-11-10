#include "loader/runtimeLocalSystemCatalog.h"
#include "loader/runtimeLocalNativeCatalog.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace {
using namespace Loader::LocalAppDb;
void Check(bool value, const char* message) {
	if (!value) { std::fprintf(stderr, "catalog comparison: %s\n", message); std::exit(1); }
}
std::string Read(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	Check(file.good(), "read input");
	return {std::istreambuf_iterator<char>(file), {}};
}
}

// Inputs must already be private copies. Outputs are retained under a new root;
// this diagnostic does not mount, launch, set readiness, or delete anything.
int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr, "usage: local_catalog_comparison copied-app.db copied-library-dir new-output-dir user-id...\n");
		return 2;
	}
	const std::filesystem::path input = argv[1], sources = argv[2], root = argv[3];
	std::vector<int32_t> users;
	try {
		for (int i = 4; i < argc; ++i) {
			size_t used = 0;
			const auto n = std::stoll(argv[i], &used, 10);
			Check(used == std::string(argv[i]).size() && n >= 0 && n <= std::numeric_limits<int32_t>::max(), "invalid roster ID");
			users.push_back(static_cast<int32_t>(n));
		}
	} catch (const std::exception&) { Check(false, "invalid roster ID"); }
	Check(std::filesystem::create_directory(root), "output directory must be new");
	const auto original = Read(input), marker = Read(input.parent_path() / "app.db-corrupt");
	std::atomic<bool> cancelled {false};
	for (const auto user: users) {
		for (const auto space: {HomeSpace::Game, HomeSpace::Media}) {
			const auto control = ReadHomeRows({input, user, space, 14}, cancelled);
			Check(control.status == HomeReadStatus::SqlError && control.rows.empty(), "control did not report missing profile catalog");
			std::printf("control user=%d space=%s status=SqlError sqlite=%d detail=%s\n", user,
			            space == HomeSpace::Game ? "game" : "media", control.sqlite_error, control.detail.c_str());
		}
		const auto control_stage = StageNativeCatalog(input, user, root / ("control-assets-" + std::to_string(user)), cancelled);
		Check(control_stage.status == NativeCatalogStageStatus::ReadFailed && control_stage.mounts.empty() && control_stage.retained_directory.empty(),
		      "missing profile unexpectedly staged native inputs");
	}
	const CatalogRequest request {input, root / "candidate", sources, users, "en-US"};
	const auto candidate = BuildSystemCatalogCandidate(request, cancelled);
	if (candidate.status != CatalogStatus::Committed) { std::fprintf(stderr, "candidate: %s (%d)\n", candidate.detail.c_str(), candidate.sqlite_error); }
	Check(candidate.status == CatalogStatus::Committed && candidate.source_records == 2 && candidate.profile_records == users.size() * 2,
	      "source-backed candidate did not commit");
	Check(Read(candidate.candidate_app_db.parent_path() / "app.db-corrupt") == marker, "candidate changed marker");
	for (const auto user: users) {
		for (const auto space: {HomeSpace::Game, HomeSpace::Media}) {
			const auto result = ReadHomeRows({candidate.candidate_app_db, user, space, 14}, cancelled);
			const char* title = space == HomeSpace::Game ? "NPXS40071" : "NPXS40139";
			Check(result.status == HomeReadStatus::Read && result.rows.size() == 1 &&
			      std::get<std::string>(result.rows.front().at("TitleId")) == title &&
			      result.retained_assets.size() == 1 && result.retained_descriptors.size() == 1, "candidate space row or retained source differs");
			std::printf("candidate user=%d space=%s rows=1 title=%s\n", user, space == HomeSpace::Game ? "game" : "media", title);
		}
		const auto stage = StageNativeCatalog(candidate.candidate_app_db, user, root / ("assets-" + std::to_string(user)), cancelled);
		Check(stage.status == NativeCatalogStageStatus::Staged && stage.mounts.size() == 3, "candidate native staging failed");
		Check(stage.mounts[0].host_directory == std::filesystem::absolute(candidate.candidate_app_db).parent_path(), "wrong database backend");
		for (size_t i = 1; i != 3; ++i) {
			const char* name = i == 1 ? "default" : "NPXS40139";
			Check(stage.mounts[i].read_only, "source mount is writable");
			for (const char* file: {"param.json", "icon0.png"}) {
				Check(Read(stage.mounts[i].host_directory / file) == Read(sources / name / file), "staged source bytes differ");
			}
		}
		std::printf("stage user=%d database=single source_mounts=2\n", user);
	}
	Check(Read(input) == original && Read(input.parent_path() / "app.db-corrupt") == marker, "copied control input changed");
	std::puts("comparison passed; no native recovery/admission or Home claim");
}
