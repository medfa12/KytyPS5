#pragma once
#include "loader/runtimeChildProcess.h"

namespace Loader::RuntimeChild {
struct ManifestInput {
	std::string title;
	std::filesystem::path app, metadata, firmware, output;
	std::vector<std::string> guest_arguments, keymap;
	int32_t user = 1000;
	std::string user_name = "Kyty";
	uint32_t language = 1;
	bool network_connected = false;
};
// Creates a fresh run directory; never copies parent extra mounts/profile paths.
// Firmware and app content become read-only in the explicit child role.
bool BuildManifest(const ManifestInput& input, const Key& key, Spawn* out, std::string* error);
}
