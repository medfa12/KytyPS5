#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_CHILDLAUNCH_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_CHILDLAUNCH_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Loader {

struct ChildLaunchSpec {
	std::filesystem::path      app0_dir;
	std::vector<std::string>   args;
	uint32_t                   appId = 0;
};

// True iff app0_dir holds a runnable child title: eboot.bin present,
// or an RNPS app (manifest.json + application.ps.bundle, via SystemContentIsRnpsApp).
bool IsLaunchableTitle(const std::filesystem::path& app0_dir);

// Remount /app0 and /hostapp onto the child title dir (Mount over the same
// point; MountPoints::Mount auto-umounts the previous entry).
bool RemountApp0(const std::filesystem::path& app0_dir);

// --guest-arg entries for the child (RuntimeLinker prepends KytyEmu itself).
std::vector<std::string> BuildChildArgv(const std::string& videoCoreName, const std::string& buildType);

// fw_app_dir is the extracted firmware app dir (e.g. .../system_b/vsh/app),
// so the child lives at fw_app_dir/titleId (e.g. .../NPXS40038 with eboot.bin
// + sce_sys/param.json). Fills out->app0_dir; args left for the caller.
bool ProbeChildTitle(const std::filesystem::path& fw_app_dir, const std::string& titleId, ChildLaunchSpec* out);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_CHILDLAUNCH_H_ */
