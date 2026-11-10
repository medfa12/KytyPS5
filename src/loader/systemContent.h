#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Loader {

struct Program;

struct RnpsManifest {
	std::string              applicationName;
	std::string              applicationVersion;
	std::string              commitHash;
	std::string              titleId;
	std::string              rnVersion;
	std::string              jsBackend;
	bool                     twinTurbo                = false;
	bool                     enableHttpCache          = false;
	bool                     webSocketTLSWorkaround   = false;
	std::vector<std::string> accessibility;
};

// Legacy boot-session loader; runtime callers must use SystemContentLoadForProgram.
bool SystemContentLoadRnpsManifest(const std::filesystem::path& dir);
bool SystemContentGetRnpsManifest(RnpsManifest* out);
bool SystemContentIsRnpsApp(const std::filesystem::path& dir);

// Legacy boot-session loader; refuses calls from runtime-application threads.
void     SystemContentLoadParamSfo(const std::filesystem::path& file_name);
// Register an immutable runtime-app snapshot without changing boot-session
// metadata or hardware policy. owner must be an application main (self-owned).
// param_json is the app's sce_sys/param.json; companion metadata uses the same
// paths as the legacy loader, with optional manifest.json in the app root.
// Success atomically replaces this owner's snapshot; failure preserves it.
// Owner identities must remain unique while their guest threads can run.
bool     SystemContentLoadForProgram(const Program* owner, const std::filesystem::path& param_json);
// Hardware policy remains boot-session-wide, including for runtime apps.
bool     SystemContentIsTrinityMode();
bool     SystemContentParamSfoGetInt(const char* name, int32_t* value);
bool     SystemContentParamSfoGetString(const char* name, std::string* value);
bool     SystemContentParamSfoGetString(const char* name, char* value, size_t value_size);
// Memory sizes a game's param.json sets, in bytes; 0 where it sets none.
struct MemoryParams {
	uint64_t flexible_size    = 0;
	uint64_t cpu_table_memory = 0;
	uint64_t gpu_table_memory = 0;
	uint64_t amm_table_memory = 0;
};

MemoryParams SystemContentGetMemoryParams();

bool     SystemContentGetIconPath(std::filesystem::path* path);
bool     SystemContentGetChunksNum(uint32_t* num);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_ */
