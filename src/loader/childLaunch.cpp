#include "loader/childLaunch.h"

#include "common/file.h"
#include "kernel/fileSystem.h"
#include "loader/systemContent.h"

namespace Loader {

bool IsLaunchableTitle(const std::filesystem::path& app0_dir) {
	if (app0_dir.empty()) {
		return false;
	}
	if (Common::File::IsFileExisting(app0_dir / "eboot.bin")) {
		return true;
	}
	return SystemContentIsRnpsApp(app0_dir);
}

bool RemountApp0(const std::filesystem::path& app0_dir) {
	if (app0_dir.empty() || !Common::File::IsDirectoryExisting(app0_dir)) {
		return false;
	}
	// No explicit Umount: MountPoints::Mount auto-umounts the previous entry
	// at the same point (mirrors Emulator::Run startup mounts of /app0 + /hostapp).
	Libs::LibKernel::FileSystem::Mount(app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(app0_dir, "/hostapp");
	return true;
}

std::vector<std::string> BuildChildArgv(const std::string& videoCoreName, const std::string& buildType) {
	return {videoCoreName, buildType};
}

bool ProbeChildTitle(const std::filesystem::path& fw_app_dir, const std::string& titleId, ChildLaunchSpec* out) {
	if (fw_app_dir.empty() || titleId.empty()) {
		return false;
	}
	const std::filesystem::path candidate = fw_app_dir / titleId;
	if (!Common::File::IsDirectoryExisting(candidate)) {
		return false;
	}
	if (!IsLaunchableTitle(candidate)) {
		return false;
	}
	if (out != nullptr) {
		out->app0_dir = candidate;
		out->args.clear();
		out->appId = 0;
	}
	return true;
}

} // namespace Loader

/*
 * Integration snippet for Emulator::Run (orchestrator pastes these 5 lines):
 * Loader::ChildLaunchSpec child_spec{};
 * if (Loader::ProbeChildTitle(fw_app_dir, title_id, &child_spec) &&
 *     Loader::IsLaunchableTitle(child_spec.app0_dir) && Loader::RemountApp0(child_spec.app0_dir)) {
 *   child_spec.args = Loader::BuildChildArgv(video_core_name, build_type);
 * }
 */
