#pragma once

#include "loader/npWorkQueueExports.h"

namespace Loader {
// Complete audited ShellCore-imported context/timer lifecycle. Native context
// initialization starts its scheduler; constructor-only exposure is incoherent.
inline constexpr std::array<std::string_view, 10> NpCalloutExports {
    "dV+zK-Ce-2E", "oLpLfV2Ov9A", "C282U0P6Nwg", "XEXFdmQj5oI",
    "14PDhhMEBKY", "3ErXia+y89M", "TDuC6To9HJ8", "r0PYNWZLZS8",
    "EQpGK69EaU8", "xxOTJpEyoj4",
};

inline bool IsNpCalloutExport(std::string_view nid) {
	if (IsNpWorkQueueExport(nid)) { return true; }
	for (const auto allowed: NpCalloutExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}
} // namespace Loader
