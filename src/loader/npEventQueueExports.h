#pragma once

#include "loader/npDispatcherExports.h"

namespace Loader {
// Regular 13.60 NpCommon, pinned by the loader's original image hash.
// Whole EventQueue lifecycle, including aliases and virtual deleting destructor.
// Internal ring-buffer/allocator/mutex/condition calls stay in the same provider.
inline constexpr std::array<std::string_view, 16> NpEventQueueExports {
    "esiO4He2WTU", "OGAIG7dVmUk", // constructor +1dd50
    "8kUkQPQP7bA", "lQXgvDXBGtA", // non-deleting destructor +1ddb0
    "N1gnYosdK7Q", "E4uoqSdo8ek", // delete hook +1ddf0, deleting destructor +1de00
    "xu9qWN0YYC4", "b20e017Ei94", // state reset +1de50, initialize +1de60
    "qvpEuKumIGM", "VI8AHrfLdqY", // terminate +1dff0, initialized +1e030
    "jxPY-0x8e-M", "AV5jHo8O3+E", // empty +1e040, enqueue +1e050
    "suxln7PooIo", "QlP4t2SGZ4I", // dequeue +1e180, try-dequeue +1e2b0
    "slmKkuIoC28", "aoas3bJANfY", // cancel +1e350, clear cancel bits +1e3c0
};
inline bool IsNpEventQueueExport(std::string_view nid) {
	if (IsNpDispatcherExport(nid)) { return true; }
	for (const auto allowed: NpEventQueueExports) {
		if (nid == allowed) { return true; }
	}
	return false;
}
} // namespace Loader
