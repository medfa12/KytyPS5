#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Libs::Audio {

// Numeric-only summaries. Never sanitize or modify the samples being diagnosed.
struct AudioPcmSummary {
	uint32_t count = 0, nonzero = 0, nonfinite = 0, above_one = 0;
	double peak = 0, squares = 0;
	uint64_t hash = 14695981039346656037ull;

	void Add(float value) {
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		hash = (hash ^ bits) * 1099511628211ull;
		++count;
		nonzero += value != 0;
		if (!std::isfinite(value)) { ++nonfinite; return; }
		const double magnitude = std::fabs(static_cast<double>(value));
		above_one += magnitude > 1;
		peak = std::max(peak, magnitude);
		squares += magnitude * magnitude;
	}
	double Rms() const { return count > nonfinite ? std::sqrt(squares / (count - nonfinite)) : 0; }
};

// Startup silence must not consume the active-audio report window. Even with
// indefinitely silent input, work is bounded to one small scan per 128 calls
// after the initial window, and a finite total number of scans.
struct AudioPcmTraceWindow {
	uint64_t calls = 0;
	uint32_t scans = 0, active = 0, reports = 0;
	bool Scan() {
		++calls;
		if (scans >= 4096 || reports >= 128) return false;
		if (calls <= 16 || (active != 0 && active < 16) || calls % 128 == 0) {
			++scans;
			return true;
		}
		return false;
	}
	bool Report(const AudioPcmSummary& summary) {
		if (summary.nonzero == 0 && active == 0) return false;
		if (summary.nonzero != 0) ++active;
		if (active <= 16 || calls % 128 == 0) { ++reports; return true; }
		return false;
	}
};

} // namespace Libs::Audio
