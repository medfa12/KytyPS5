#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>

namespace Loader::LocalDbRecovery {
enum class ReportEncoding { Missing, Short, Recognized, Unknown };
struct CorruptionReport {
	ReportEncoding encoding = ReportEncoding::Missing;
	bool present = false;
	bool primary = false;
	bool secondary = false;
	std::array<uint8_t, 4> header {};
	size_t header_size = 0;
};
// UI +7b93f0 has THREE outputs: presence, primary and secondary. Magic
// changes the latter two, never presence. Only the first four bytes are used.
CorruptionReport DecodeCorruptionReport(bool present, std::span<const uint8_t> bytes);

enum class CheckStatus { Observed, InputError, IoError, Cancelled };
struct ForceRebuildObservation {
	CheckStatus status = CheckStatus::InputError;
	CorruptionReport report;
	std::error_code host_error;
	// UI +8932c0 callback's bool, present only after a successful observation.
	// This is not AppBrowse's result or the complete native 12-byte result.
	std::optional<bool> is_corrupt;
};
enum class LookupStatus { Found, Unavailable, InvalidKey };
struct Lookup {
	LookupStatus status = LookupStatus::Unavailable;
	std::optional<ForceRebuildObservation> observation;
};

// One private copied-input recovery pass. No public setter accepts schema or
// catalog receipts as recovery results. New passes use new objects; Stop is
// terminal and invalidates observations. No process-global retained state.
class EagerCheckPass {
public:
	explicit EagerCheckPass(std::filesystem::path copied_app_db);
	ForceRebuildObservation CheckForceRebuildAppInfo();
	Lookup Get(uint32_t target, uint32_t operation) const;
	void Stop();
private:
	const std::filesystem::path copied_app_db_;
	std::atomic<bool> stopped_ {false};
	mutable std::mutex mutex_;
	std::optional<ForceRebuildObservation> force_rebuild_;
};
} // namespace Loader::LocalDbRecovery
