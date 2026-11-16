#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Loader {

enum class RuntimeApplicationRoute { Native, Web, Rnps };

// File presence is not runtime admission or evidence that an application started.
enum class RuntimeApplicationAvailability { ExecutablePresent, NativeSceneRequired };

struct RuntimeApplicationPlanOptions {
	// Same representation as Emulator::RunOptions::extra_mounts. Exact title mounts
	// take precedence over firmware discovery; repeated mount points are last-wins.
	std::vector<std::pair<std::string, std::filesystem::path>> extra_mounts;
	std::filesystem::path fw_root;
	bool allow_firmware_titles = false;
};

struct RuntimeApplicationPlan {
	std::string title_id; // Requested logical title, never the web launcher's ID.
	RuntimeApplicationRoute route = RuntimeApplicationRoute::Native;
	RuntimeApplicationAvailability availability = RuntimeApplicationAvailability::NativeSceneRequired;
	std::optional<uint32_t> application_category_type;
	std::filesystem::path executable_path; // Empty for RNPS native-scene requests.
	std::filesystem::path app0_path;
	std::filesystem::path metadata_path; // param.json, or manifest.json for RNPS.
	// Only supplied by an explicit /host_resources mount; no URI/path guessing.
	std::optional<std::filesystem::path> host_resources_path;
};

enum class RuntimeApplicationPlanError {
	None,
	InvalidTitleId,
	TitleNotFound,
	MissingMetadata,
	InvalidMetadata,
	MetadataTitleMismatch,
	MissingExecutable,
	MissingBundle,
	InvalidHostResources,
	PathEscape,
	FileSystem,
};

struct RuntimeApplicationPlanResult {
	RuntimeApplicationPlanError error = RuntimeApplicationPlanError::None;
	std::string message;
	std::optional<RuntimeApplicationPlan> plan; // Populated only on resolution success.
};

// VSH application routing only. Reads local files into owned values; does not
// mount, load global SystemContent metadata, create processes or execute Hermes.
// Native/Web require their executable file. RNPS requires manifest + bundle but
// resolves with NativeSceneRequired, not an executable or a launch guarantee.
[[nodiscard]] RuntimeApplicationPlanResult ResolveRuntimeApplicationPlan(
    const RuntimeApplicationPlanOptions& options, const std::string& title_id);

} // namespace Loader
