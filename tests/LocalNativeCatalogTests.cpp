#include "loader/runtimeLocalNativeCatalog.h"
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "NativeCatalog: %s\n", why); std::abort(); } }
std::string Read(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary); Check(input.good(), "read fixture"); return {std::istreambuf_iterator<char>(input), {}};
}
}
void RunLocalNativeCatalogTests(const std::filesystem::path& root, const std::filesystem::path& candidate,
                               const std::filesystem::path& sources) {
	using namespace Loader::LocalAppDb;
	const auto original = Read(candidate), marker = Read(candidate.parent_path() / "app.db-corrupt");
	std::atomic<bool> cancelled {false};
	const auto directory = root / "native-catalog-assets";
	const auto stage = StageNativeCatalog(candidate, 268500996, directory, cancelled);
	Check(stage.status == NativeCatalogStageStatus::Staged && stage.mounts.size() == 3 &&
	      stage.home_reads[0].rows.size() == 1 && stage.home_reads[1].rows.size() == 1, "native asset staging failed");
	Check(stage.mounts[0].guest_directory == "/system_data/priv/mms" &&
	      stage.mounts[0].host_directory == std::filesystem::absolute(candidate).parent_path() && !stage.mounts[0].read_only,
	      "native mount plan created another profile database backend");
	Check(!std::filesystem::exists(stage.retained_directory / "app.db"), "asset stage duplicated database");
	for (size_t i = 1; i != 3; ++i) {
		const auto name = i == 1 ? "default" : "NPXS40139";
		Check(stage.mounts[i].read_only && stage.mounts[i].guest_directory == std::string("/system_ex/rnps/apps/NPXS40071/appdb/") + name,
		      "native icon mapping is not exact/read-only");
		for (const char* file: {"icon0.png", "param.json"}) {
			Check(Read(stage.mounts[i].host_directory / file) == Read(sources / name / file), "staged native files differ from retained genuine source bytes");
		}
	}
	Check(StageNativeCatalog(candidate, 268500996, directory, cancelled).status == NativeCatalogStageStatus::Conflict, "native staging overwrote existing directory");
	Check(StageNativeCatalog(candidate, 268500998, root / "unknown-native-user", cancelled).status == NativeCatalogStageStatus::ReadFailed &&
	      !std::filesystem::exists(root / "unknown-native-user"), "unknown profile staged another user's assets");
	cancelled.store(true);
	Check(StageNativeCatalog(candidate, 268500996, root / "cancelled-native-stage", cancelled).status == NativeCatalogStageStatus::Cancelled &&
	      !std::filesystem::exists(root / "cancelled-native-stage"), "cancelled stage created artifacts");
	Check(Read(candidate) == original && Read(candidate.parent_path() / "app.db-corrupt") == marker, "native stage changed backend or recovery evidence");
	std::puts("NativeCatalog: native getter file inputs, exact PNG/JSON bytes, single backend plan, scope/retention and cancellation controls passed");
}
