#include "loader/runtimeLocalDbRecovery.h"
#include <algorithm>
#include <fstream>
#include <utility>

namespace Loader::LocalDbRecovery {
CorruptionReport DecodeCorruptionReport(bool present, std::span<const uint8_t> bytes) {
	CorruptionReport report;
	if (!present) { return report; }
	report.present = true;
	report.primary = true;
	report.header_size = std::min(bytes.size(), report.header.size());
	std::copy_n(bytes.begin(), report.header_size, report.header.begin());
	if (report.header_size != 4) { report.encoding = ReportEncoding::Short; return report; }
	const uint32_t magic = uint32_t(report.header[0]) | (uint32_t(report.header[1]) << 8) |
	                       (uint32_t(report.header[2]) << 16) | (uint32_t(report.header[3]) << 24);
	report.encoding = ReportEncoding::Recognized;
	switch (magic) {
		case 0xfee10000: report.primary = false; break;
		case 0xfee10001: break;
		case 0xfee10002: report.primary = false; report.secondary = true; break;
		default: report.encoding = ReportEncoding::Unknown; break;
	}
	return report;
}

namespace {
ForceRebuildObservation ReadReport(const std::filesystem::path& copied_app_db) {
	ForceRebuildObservation result;
	// A missing source DB is not evidence that its corruption report is absent.
	// Reject unsupported file kinds rather than treating stat/read errors as clean.
	std::error_code error;
	const auto db_status = std::filesystem::symlink_status(copied_app_db, error);
	if (error || !std::filesystem::is_regular_file(db_status)) {
		result.host_error = error ? error : std::make_error_code(std::errc::invalid_argument);
		return result;
	}
	const auto marker = std::filesystem::path(copied_app_db.native() + std::filesystem::path("-corrupt").native());
	const auto marker_status = std::filesystem::symlink_status(marker, error);
	if (marker_status.type() == std::filesystem::file_type::not_found &&
	    (!error || error == std::errc::no_such_file_or_directory)) {
		result.status = CheckStatus::Observed;
		result.is_corrupt = false;
		return result;
	}
	if (error || !std::filesystem::is_regular_file(marker_status)) {
		result.status = CheckStatus::IoError;
		result.host_error = error ? error : std::make_error_code(std::errc::invalid_argument);
		return result;
	}
	std::ifstream in(marker, std::ios::binary);
	if (!in) {
		result.status = CheckStatus::IoError;
		result.host_error = std::make_error_code(std::errc::io_error);
		return result;
	}
	std::array<uint8_t, 4> header {};
	in.read(reinterpret_cast<char*>(header.data()), header.size());
	if (in.bad() || (in.fail() && !in.eof())) {
		result.status = CheckStatus::IoError;
		result.host_error = std::make_error_code(std::errc::io_error);
		return result;
	}
	result.report = DecodeCorruptionReport(true, {header.data(), static_cast<size_t>(in.gcount())});
	result.status = CheckStatus::Observed;
	// UI +89332e..+89333c tests presence and the THIRD report output.
	result.is_corrupt = result.report.present && !result.report.secondary;
	return result;
}
}

EagerCheckPass::EagerCheckPass(std::filesystem::path copied_app_db): copied_app_db_(std::move(copied_app_db)) {}

ForceRebuildObservation EagerCheckPass::CheckForceRebuildAppInfo() {
	std::lock_guard lock(mutex_);
	force_rebuild_.reset();
	ForceRebuildObservation result;
	if (!stopped_.load()) { result = ReadReport(copied_app_db_); }
	if (stopped_.load()) {
		result = {};
		result.status = CheckStatus::Cancelled;
	} else {
		// Retain host failures too, without translating them to invented native
		// errno values or returning a successful bool from an earlier attempt.
		force_rebuild_ = result;
	}
	return result;
}

Lookup EagerCheckPass::Get(uint32_t target, uint32_t operation) const {
	if (!((target == 0 && operation < 15) || (target == 1 && operation < 6))) {
		return {LookupStatus::InvalidKey, {}};
	}
	std::lock_guard lock(mutex_);
	if (stopped_.load() || target != 1 || operation != 4 || !force_rebuild_) { return {}; }
	return {LookupStatus::Found, force_rebuild_};
}

void EagerCheckPass::Stop() {
	stopped_.store(true);
	std::lock_guard lock(mutex_);
	force_rebuild_.reset();
}
} // namespace Loader::LocalDbRecovery
