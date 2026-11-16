#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/additionalContent.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions config;
	std::filesystem::path app0_dir;
	std::filesystem::path elf;
	std::filesystem::path game_patch;
	// ps5-vsh: extracted firmware root (13.60 layout: system_b/,
	// system_ex_b/, preinst/). Enables /common, /common_ex and /data.
	std::filesystem::path fw_root;
	// Explicit co-hosted VSH runtime; ordinary direct-game launches keep their backend.
	bool vsh_session = false;
	// Native VSH titles default to independent embedded processes on Windows.
	// --vsh-app-backend shared retains the diagnostic/rollback owner backend.
#if defined(_WIN32)
	bool vsh_subprocess = true;
#else
	bool vsh_subprocess = false;
#endif
	std::string runtime_child; // Internal inherited pipe/key descriptor, never guest input.
	uint64_t runtime_parent_window = 0;
	uint32_t runtime_parent_pid = 0;
	// ps5-vsh: extra guest argv entries. Guest sees
	// argv[0]=KytyEmu, argv[1..]=guest_args (e.g. videoCoreName, buildType).
	std::vector<std::string> guest_args;
	// ps5-vsh: --launch-title <TITLE_ID> re-targets --game fw dir to a
	// system title (single use, empty = absent).
	std::string launch_title;
	// ps5-vsh: --rnps-resident <secs> keeps the Hermes runtime alive pumping
	// live ticks for this many seconds after bundle evaluation (0 = legacy
	// one-shot evaluate-and-exit).
	uint64_t rnps_resident_secs = 0;
	// ps5-vsh: --rnps-param <k=v> repeatable startup params for RNPS bundles
	// (surfaced via CSI.params.appconfig + custom keys).
	std::vector<std::pair<std::string, std::string>> rnps_params;
	// ps5-vsh: --co-elf <path> (+ up to two --co-elf-arg each): extra ELFs
	// co-hosted in-process (e.g. ShellCore + JSCd + AgcCompositor together).
	// Each runs its entry on its own guest thread after loading.
	struct CoElfSpec {
		std::filesystem::path      path;
		std::vector<std::string>   args;
	};
	std::vector<CoElfSpec> co_elfs;
	// ps5-vsh: --mount <guest_point>=<host_path> (voldemort parity):
	// host directory or file exposed at a guest mount point. Repeatable;
	// applied after the firmware/sandbox mounts so explicit staging wins.
	std::vector<std::pair<std::string, std::filesystem::path>> extra_mounts;
	// ps5-vsh: --param-json <file> (voldemort parity): application metadata
	// override; defaults to app0/sce_sys/param.json.
	std::filesystem::path param_json;
	std::filesystem::path                addcont_dir;
	std::vector<Common::AddContentEntry> addcont;
};

void Run(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
