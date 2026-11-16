#pragma once

#include "loader/npHttpPoolExports.h"

namespace Loader {
// Separate native56 policy. Keep the audited native54 predicate unchanged.
// NpCommon 13.60 +6c60: (threadOut, entry, arg, priority, stack, affinity, name).
// +6d80 joins the real pthread; neither export is a success-only HLE stub.
inline constexpr std::array<std::string_view, 2> NpDispatcherExports {
    "fhJ5uKzcn0w", "EjMsfO3GCIA",
};
inline bool IsNpDispatcherExport(std::string_view nid) {
	if (IsNpHttpPoolExport(nid)) { return true; }
	for (const auto allowed: NpDispatcherExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}
} // namespace Loader
