#include "loader/runtimeLocalNativeCatalog.h"
#include <fstream>
#include <utility>

namespace Loader::LocalAppDb {
NativeCatalogStageReceipt StageNativeCatalog(const std::filesystem::path& candidate_app_db, int32_t user,
                                             const std::filesystem::path& new_asset_directory,
                                             const std::atomic<bool>& cancelled) {
	NativeCatalogStageReceipt result;
	try {
		if (cancelled.load()) { result.status = NativeCatalogStageStatus::Cancelled; return result; }
		if (candidate_app_db.filename() != "app.db" || user < 0 ||
		    !std::filesystem::is_regular_file(std::filesystem::symlink_status(candidate_app_db))) {
			result.detail = "native AppBrowse binding requires an existing private app.db and actual user"; return result;
		}
		std::map<std::string, HomeBlob> assets;
		std::map<std::string, std::string> descriptors;
		for (size_t i = 0; i != result.home_reads.size(); ++i) {
			result.home_reads[i] = ReadHomeRows({candidate_app_db, user, i == 0 ? HomeSpace::Game : HomeSpace::Media, 0}, cancelled);
			const auto& read = result.home_reads[i];
			if (read.status != HomeReadStatus::Read) {
				result.status = read.status == HomeReadStatus::Cancelled ? NativeCatalogStageStatus::Cancelled : NativeCatalogStageStatus::ReadFailed;
				result.detail = read.detail; return result;
			}
			for (const auto& [path, bytes]: read.retained_assets) {
				const auto [at, inserted] = assets.emplace(path, bytes);
				if (!inserted && at->second != bytes) { result.status = NativeCatalogStageStatus::Conflict; result.detail = "candidate assets changed between space reads"; return result; }
			}
			for (const auto& [path, bytes]: read.retained_descriptors) {
				const auto [at, inserted] = descriptors.emplace(path, bytes);
				if (!inserted && at->second != bytes) { result.status = NativeCatalogStageStatus::Conflict; result.detail = "candidate descriptors changed between space reads"; return result; }
			}
		}
		// Require both genuine Library bindings; an arbitrary image path never
		// becomes a directory traversal or a replacement firmware mount.
		const std::string prefix = "/system_ex/rnps/apps/NPXS40071/appdb/";
		if (assets.size() != 2 || !assets.contains(prefix + "default/icon0.png") || !assets.contains(prefix + "NPXS40139/icon0.png") ||
		    descriptors.size() != 2 || !descriptors.contains(prefix + "default/param.json") || !descriptors.contains(prefix + "NPXS40139/param.json")) {
			result.status = NativeCatalogStageStatus::ReadFailed; result.detail = "both retained Library assets are required"; return result;
		}
		if (cancelled.load()) { result.status = NativeCatalogStageStatus::Cancelled; return result; }
		auto directory = std::filesystem::absolute(new_asset_directory);
		if (!std::filesystem::create_directory(directory)) { result.status = NativeCatalogStageStatus::Conflict; result.detail = "asset directory already exists"; return result; }
		result.retained_directory = std::move(directory);
		std::vector<NativeCatalogMount> mounts;
		// Preserve native SQLite/materialization/getters and native caller. The DB
		// is not copied to a second backend; recovery marker/sidecars share its dir.
		mounts.push_back({"/system_data/priv/mms", std::filesystem::absolute(candidate_app_db).parent_path(), false});
		for (const char* name: {"default", "NPXS40139"}) {
			const auto host = result.retained_directory / name;
			std::filesystem::create_directory(host);
			const auto& bytes = assets.at(prefix + name + "/icon0.png");
			std::ofstream file(host / "icon0.png", std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); file.close();
			if (!file) { result.status = NativeCatalogStageStatus::IoError; result.detail = "write retained native icon"; return result; }
			const auto& descriptor = descriptors.at(prefix + name + "/param.json");
			std::ofstream json(host / "param.json", std::ios::binary | std::ios::trunc);
			json.write(descriptor.data(), static_cast<std::streamsize>(descriptor.size())); json.close();
			if (!json) { result.status = NativeCatalogStageStatus::IoError; result.detail = "write retained native descriptor"; return result; }
			mounts.push_back({prefix + name, host, true});
		}
		if (cancelled.load()) { result.status = NativeCatalogStageStatus::Cancelled; return result; }
		result.mounts = std::move(mounts);
		result.status = NativeCatalogStageStatus::Staged;
	} catch (const std::exception& error) { result.status = NativeCatalogStageStatus::IoError; result.detail = error.what(); result.mounts.clear(); }
	return result;
}
} // namespace Loader::LocalAppDb
