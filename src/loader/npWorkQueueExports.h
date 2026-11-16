#pragma once

#include "loader/npOwnershipExports.h"

#include <array>
#include <string_view>

namespace Loader {

// Audited 13.60 queue/work-item exports added only by the NpWorkQueue policy.
// Ownership remains authoritative through the unchanged 21-export closure.
inline constexpr std::array<std::string_view, 20> NpWorkQueueExports {
    "X6NVkdpRnog", "U0YoWwgg8aI", "+dGO+GS2ZXQ", "VnQolo6vTr4",
    "CznMfhTIvVY", "eTy3L1azX4E", "zmOmSLnqlBQ", "4DE+nnCVRPA",
    "NeopmYshD0U", "1QFKnDJxk3A", "KQSxXJBepQ4", "laqZEULcfgw",
    "WF2l-GUIlrw", "HldN461O2Dw", "ga4OW9MGahU", "Oq5aepLkEWg",
    "JRtw5pROOiM", "417JucZaE3g", "zurkNUps5o8", "5tYi1l9CXD0",
};

inline bool IsNpWorkQueueExport(std::string_view nid) {
	if (IsNpOwnershipExport(nid)) { return true; }
	for (const auto allowed: NpWorkQueueExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}

} // namespace Loader
