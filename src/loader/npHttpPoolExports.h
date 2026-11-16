#pragma once

#include "loader/npCalloutExports.h"

namespace Loader {
// Audited empty HTTP-pool lifecycle only; no acquire/request or destructor aliases.
inline constexpr std::array<std::string_view, 3> NpHttpPoolExports {
    "PznfSvchYJ8", "Vzob5RCgfnY", "jCeUP0CpiNs",
};

inline bool IsNpHttpPoolExport(std::string_view nid) {
	if (IsNpCalloutExport(nid)) { return true; }
	for (const auto allowed: NpHttpPoolExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}
} // namespace Loader
