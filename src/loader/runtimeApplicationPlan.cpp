#include "loader/runtimeApplicationPlan.h"

#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace Loader {
namespace {
namespace fs = std::filesystem;
using Error = RuntimeApplicationPlanError;
using Result = RuntimeApplicationPlanResult;

bool Fail(Result& result, Error error, const std::string& message, const fs::path& path = {}) {
	result.error = error;
	result.message = message;
	if (!path.empty()) { result.message += ": " + path.generic_string(); }
	return false;
}

bool ValidTitleId(const std::string& title) {
	if (title.size() != 9) { return false; }
	for (size_t i = 0; i < title.size(); ++i) {
		if (i < 4 ? (title[i] < 'A' || title[i] > 'Z') : (title[i] < '0' || title[i] > '9')) {
			return false;
		}
	}
	return true;
}

const fs::path* FindMount(const RuntimeApplicationPlanOptions& options, const std::string& point) {
	for (auto it = options.extra_mounts.rbegin(); it != options.extra_mounts.rend(); ++it) {
		if (it->first == point) { return &it->second; }
	}
	return nullptr;
}

fs::path Absolute(const fs::path& path) {
	return fs::weakly_canonical(fs::absolute(path));
}

// Check canonical path components, including junction/symlink targets. String
// prefixes would incorrectly accept siblings such as app-title/../app-title2.
bool Contained(const fs::path& root, const fs::path& path, Result& result) {
	const auto resolved = Absolute(path);
	auto child = resolved.begin();
	for (const auto& part: root) {
		if (child == resolved.end() || *child != part) {
			return Fail(result, Error::PathEscape, "Application path escapes its configured root", path);
		}
		++child;
	}
	return true;
}

bool RequireFile(const fs::path& root, const fs::path& path, Error error,
                 const char* message, Result& result) {
	return Contained(root, path, result) &&
	       (fs::is_regular_file(path) || Fail(result, error, message, path));
}

bool ReadMetadata(const fs::path& root, const fs::path& path, const std::string& title,
                  nlohmann::ordered_json& json, Result& result) {
	if (!RequireFile(root, path, Error::MissingMetadata, "Missing application metadata", result)) { return false; }
	// Metadata is small; do not accidentally parse a payload as JSON.
	if (fs::file_size(path) > 1024 * 1024) {
		return Fail(result, Error::InvalidMetadata, "Application metadata exceeds 1 MiB", path);
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) { return Fail(result, Error::FileSystem, "Cannot read application metadata", path); }
	json = nlohmann::ordered_json::parse(input, nullptr, false);
	if (input.bad()) { return Fail(result, Error::FileSystem, "Cannot read application metadata", path); }
	if (json.is_discarded() || !json.is_object()) {
		return Fail(result, Error::InvalidMetadata, "Application metadata must be a JSON object", path);
	}
	const auto id = json.find("titleId");
	if (id == json.end() || !id->is_string() || !ValidTitleId(id->get_ref<const std::string&>())) {
		return Fail(result, Error::InvalidMetadata, "Metadata requires a valid titleId", path);
	}
	if (id->get_ref<const std::string&>() != title) {
		return Fail(result, Error::MetadataTitleMismatch, "Metadata titleId differs from requested title " + title, path);
	}
	return true;
}

bool ReadCategory(const nlohmann::ordered_json& json, const fs::path& path,
                  uint32_t& category, Result& result) {
	const auto value = json.find("applicationCategoryType");
	if (value == json.end() || !value->is_number_integer() ||
	    (!value->is_number_unsigned() && value->get<int64_t>() < 0) ||
	    value->get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
		return Fail(result, Error::InvalidMetadata, "Metadata requires a uint32 applicationCategoryType", path);
	}
	category = value->get<uint32_t>();
	return true;
}

Result Resolve(const RuntimeApplicationPlanOptions& options, const std::string& title) {
	Result result;
	if (!ValidTitleId(title)) {
		Fail(result, Error::InvalidTitleId, "Title ID must contain four uppercase letters and five digits");
		return result;
	}

	RuntimeApplicationPlan plan;
	plan.title_id = title;
	bool rnps_directory = false;
	if (const auto* mounted = FindMount(options, "/user/app/" + title)) {
		if (mounted->empty() || !fs::is_directory(*mounted)) {
			Fail(result, Error::TitleNotFound, "Explicit application mount is not a directory", *mounted);
			return result;
		}
		plan.app0_path = Absolute(*mounted);
	} else if (options.allow_firmware_titles && !options.fw_root.empty()) {
		const auto firmware = Absolute(options.fw_root);
		const fs::path roots[] = {firmware / "system_ex_b" / "app", firmware / "system_b" / "vsh" / "app",
		                          firmware / "system_ex_b" / "rnps" / "apps"};
		for (size_t i = 0; i < 3; ++i) {
			const auto candidate = roots[i] / title;
			if (!Contained(firmware, candidate, result)) { return result; }
			if (!fs::exists(candidate)) { continue; }
			if (!fs::is_directory(candidate)) {
				Fail(result, Error::TitleNotFound, "Firmware application path is not a directory", candidate);
				return result;
			}
			plan.app0_path = Absolute(candidate);
			rnps_directory = i == 2;
			break;
		}
	}
	if (plan.app0_path.empty()) {
		Fail(result, Error::TitleNotFound, "No application root found for " + title);
		return result;
	}

	const auto param = plan.app0_path / "sce_sys" / "param.json";
	const auto manifest = plan.app0_path / "manifest.json";
	const auto bundle = plan.app0_path / "application.ps.bundle";
	// The RNPS bundle/layout takes precedence even if param.json or eboot.bin
	// also exists. A generic web manifest alone must not override param.json.
	const bool rnps = rnps_directory || fs::exists(bundle) || (!fs::exists(param) && fs::exists(manifest));
	plan.metadata_path = rnps ? manifest : param;
	nlohmann::ordered_json metadata;
	if (!ReadMetadata(plan.app0_path, plan.metadata_path, title, metadata, result)) { return result; }
	plan.metadata_path = Absolute(plan.metadata_path);

	if (rnps) {
		plan.route = RuntimeApplicationRoute::Rnps;
		plan.availability = RuntimeApplicationAvailability::NativeSceneRequired;
		if (!RequireFile(plan.app0_path, bundle, Error::MissingBundle, "Missing RNPS application bundle", result)) {
			return result;
		}
	} else {
		uint32_t category = 0;
		if (!ReadCategory(metadata, plan.metadata_path, category, result)) { return result; }
		plan.application_category_type = category;
		// Audited SceShellCore convertPathForWebApp RVA 0x6989a0 compares
		// 0x10200 at 0x6989cd, selecting /web_app_launcher/eboot.bin backed by
		// /system_ex/app/NPXS40001/eboot.bin. A webAppUri alone is not this test.
		plan.route = category == 0x10200 ? RuntimeApplicationRoute::Web : RuntimeApplicationRoute::Native;
		fs::path executable_root = plan.app0_path;
		if (plan.route == RuntimeApplicationRoute::Web) {
			if (options.fw_root.empty()) {
				Fail(result, Error::MissingExecutable, "Web application requires firmware NPXS40001/eboot.bin (fw_root is empty)");
				return result;
			}
			executable_root = Absolute(options.fw_root);
			plan.executable_path = executable_root / "system_ex_b" / "app" / "NPXS40001" / "eboot.bin";
		} else {
			plan.executable_path = plan.app0_path / "eboot.bin";
		}
		if (!RequireFile(executable_root, plan.executable_path, Error::MissingExecutable,
		                 "Missing application executable", result)) { return result; }
		plan.executable_path = Absolute(plan.executable_path);
		plan.availability = RuntimeApplicationAvailability::ExecutablePresent;
	}

	if (const auto* host = FindMount(options, "/host_resources")) {
		if (host->empty() || !fs::is_directory(*host)) {
			Fail(result, Error::InvalidHostResources, "Explicit /host_resources mount is not a directory", *host);
			return result;
		}
		plan.host_resources_path = Absolute(*host);
	}
	result.plan = std::move(plan);
	return result;
}
} // namespace

RuntimeApplicationPlanResult ResolveRuntimeApplicationPlan(const RuntimeApplicationPlanOptions& options,
                                                           const std::string& title_id) {
	try {
		return Resolve(options, title_id);
	} catch (const fs::filesystem_error& error) {
		Result result;
		Fail(result, Error::FileSystem, error.what());
		return result;
	} catch (const nlohmann::ordered_json::exception& error) {
		Result result;
		Fail(result, Error::InvalidMetadata, error.what());
		return result;
	}
}

} // namespace Loader
