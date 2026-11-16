#include "loader/runtimeLocalDbPreparation.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace Loader::LocalDbRecovery {
namespace {
constexpr OperationKey Force {1, 4};
constexpr OperationKey Mode1[] {{1, 4}, {0, 10}, {1, 5}, {1, 0}, {0, 1}, {1, 1}};
bool Valid(OperationKey key) { return (key.target == 0 && key.operation < 15) || (key.target == 1 && key.operation < 6); }
bool Missing(const std::filesystem::file_status& status, const std::error_code& error) {
	return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
}
struct Failure { WrapperStatus status; std::error_code error; std::string detail; };
void Require(bool ok, const char* detail) {
	if (!ok) { throw Failure {WrapperStatus::InputError, std::make_error_code(std::errc::invalid_argument), detail}; }
}
void Write(const std::filesystem::path& path, const std::vector<char>& bytes) {
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); stream.close();
	if (!stream) { throw Failure {WrapperStatus::IoError, std::make_error_code(std::errc::io_error), "write private report snapshot"}; }
}
OperationReceipt Unavailable(OperationKey key) {
	OperationReceipt result; result.key = key;
	if (key == OperationKey {0, 10}) { result.detail = "Core AppInfo inventory/version/recovery producer unavailable"; }
	else if (key == OperationKey {1, 5}) { result.detail = "AppInfoShellUI native AppInfo version service unavailable"; }
	else if (key == OperationKey {1, 0}) { result.detail = "AppBrowse native accessor/error/promotion state unavailable"; }
	else if (key == OperationKey {0, 1}) { result.detail = "AvContentDb operation producer unavailable"; }
	else if (key == OperationKey {1, 1}) { result.detail = "AsyncStorage operation producer unavailable"; }
	else { result.detail = "operation has no completed wrapper result"; }
	if (!Valid(key)) { result.status = WrapperStatus::InputError; result.detail = "invalid operation key"; }
	return result;
}
void Put32(std::span<uint8_t> bytes, size_t at, uint32_t value) {
	for (size_t i = 0; i != 4; ++i) { bytes[at + i] = static_cast<uint8_t>(value >> (8 * i)); }
}
}
PreparationPass::PreparationPass(RecoveryWorkspaceRequest request): request_(std::move(request)), observations_(request_.copied_app_db) {}

RecoveryWorkspaceReceipt PreparationPass::Initialize() {
	std::lock_guard lock(mutex_);
	if (stopped_.load()) { auto result = workspace_; result.status = WrapperStatus::Cancelled; return result; }
	if (initialization_attempted_) { return workspace_; }
	initialization_attempted_ = true;
	try {
		Require(!request_.copied_force_report.empty(), "copied force-report location unavailable");
		Require(std::filesystem::is_regular_file(std::filesystem::symlink_status(request_.copied_app_db)), "copied database must be a regular file");
		Require(std::filesystem::is_directory(request_.copied_force_report.parent_path()), "copied force-report parent unavailable");
		const auto app_marker = std::filesystem::path(request_.copied_app_db.native() + std::filesystem::path("-corrupt").native());
		Require(std::filesystem::absolute(request_.copied_force_report).lexically_normal() != std::filesystem::absolute(app_marker).lexically_normal(),
		        "force report source must be distinct from AppBrowse report");
		std::error_code error;
		const auto status = std::filesystem::symlink_status(request_.copied_force_report, error);
		std::vector<char> source;
		if (!Missing(status, error)) {
			Require(!error && std::filesystem::is_regular_file(status), "copied force report must be regular or absent");
			Require(!std::filesystem::equivalent(request_.copied_force_report, request_.copied_app_db), "force report source aliases database");
			if (std::filesystem::exists(app_marker)) {
				Require(!std::filesystem::equivalent(request_.copied_force_report, app_marker), "force report source aliases AppBrowse report");
			}
			// Local snapshot limit; do not silently discard a report's trailing data.
			Require(std::filesystem::file_size(request_.copied_force_report) <= 1024 * 1024, "copied force report exceeds supported size");
			std::ifstream in(request_.copied_force_report, std::ios::binary);
			if (!in) { throw Failure {WrapperStatus::IoError, std::make_error_code(std::errc::io_error), "open copied force report"}; }
			std::array<char, 4096> buffer;
			while (in) {
				in.read(buffer.data(), buffer.size());
				const auto count = static_cast<size_t>(in.gcount());
				Require(source.size() + count <= 1024 * 1024, "copied force report changed beyond supported size");
				source.insert(source.end(), buffer.begin(), buffer.begin() + count);
			}
			if (in.bad() || !in.eof()) { throw Failure {WrapperStatus::IoError, std::make_error_code(std::errc::io_error), "read copied force report"}; }
			workspace_.source_report_present = true;
		}
		if (stopped_.load()) { workspace_.status = WrapperStatus::Cancelled; return workspace_; }
		auto retained = std::filesystem::absolute(request_.new_private_report_directory);
		Require(std::filesystem::create_directory(retained), "private report directory already exists");
		workspace_.retained_directory = std::move(retained);
		if (workspace_.source_report_present) {
			Write(workspace_.retained_directory / "force-report.input", source);
			Write(workspace_.retained_directory / "force_rebuild_appinfo-corrupt", source);
		}
		force_secondary_state_ = 2;
		workspace_.status = stopped_.load() ? WrapperStatus::Cancelled : WrapperStatus::Completed;
	} catch (const Failure& error) {
		workspace_.status = error.status; workspace_.host_error = error.error; workspace_.detail = error.detail;
	} catch (const std::filesystem::filesystem_error& error) {
		workspace_.status = WrapperStatus::IoError; workspace_.host_error = error.code(); workspace_.detail = error.what();
	} catch (const std::exception& error) {
		workspace_.status = WrapperStatus::IoError; workspace_.detail = error.what();
	}
	if (stopped_.load()) { workspace_.status = WrapperStatus::Cancelled; }
	return workspace_;
}

OperationReceipt PreparationPass::CheckForceLocked() {
	force_.reset();
	force_estimate_.reset();
	auto result = Unavailable(Force);
	if (stopped_.load()) { result.status = WrapperStatus::Cancelled; return result; }
	if (workspace_.status != WrapperStatus::Completed) {
		result.status = workspace_.status;
		result.host_error = workspace_.host_error;
		result.detail = workspace_.detail.empty() ? "private report workspace has not initialized successfully" : workspace_.detail;
		force_ = result; return result;
	}
	result.observation = observations_.CheckForceRebuildAppInfo();
	// +895c60 extracts +0x70 AFTER callback, including failed callbacks. Force's
	// +88e810 reads (secondary == 1); its initialized state2 is not boolean true.
	result.extracted_promotion = force_secondary_state_ == 1 ? 1 : 0;
	result.status = result.observation.status == CheckStatus::Observed ? WrapperStatus::Completed :
	                result.observation.status == CheckStatus::InputError ? WrapperStatus::InputError :
	                result.observation.status == CheckStatus::Cancelled ? WrapperStatus::Cancelled : WrapperStatus::IoError;
	result.host_error = result.observation.host_error;
	if (result.status != WrapperStatus::Completed) { result.detail = "Force callback report read unavailable"; }
	// +0x38 runs even after callback failure. It targets the DISTINCT force
	// report, never app.db-corrupt. Only this pass's exclusively owned copy moves.
	try {
		Require(std::filesystem::is_directory(std::filesystem::symlink_status(workspace_.retained_directory)), "private workspace replaced");
		const auto path = workspace_.retained_directory / "force_rebuild_appinfo-corrupt";
		std::error_code error;
		const auto status = std::filesystem::symlink_status(path, error);
		if (Missing(status, error)) { result.post_check = PostCheckStatus::ReportAbsent; }
		else {
			Require(!error && std::filesystem::is_regular_file(status), "private force report is not a regular file");
			const bool removed = std::filesystem::remove(path, error);
			if (error || !removed) { throw Failure {WrapperStatus::IoError, error ? error : std::make_error_code(std::errc::io_error), "remove private force report"}; }
			result.post_check = PostCheckStatus::ReportRemoved;
		}
	} catch (const Failure& error) {
		result.status = WrapperStatus::IoError; result.post_check = PostCheckStatus::Failed;
		result.host_error = error.error; result.detail = error.detail;
	} catch (const std::filesystem::filesystem_error& error) {
		result.status = WrapperStatus::IoError; result.post_check = PostCheckStatus::Failed;
		result.host_error = error.code(); result.detail = error.what();
	}
	if (result.status == WrapperStatus::Completed) {
		// Native Force callback returns0 after a successful report read. Host I/O
		// errors have no invented guest status. Padding is encoded separately.
		result.eager = EagerResult {0, *result.observation.is_corrupt, *result.extracted_promotion};
		result.detail.clear();
	}
	if (stopped_.load()) { result.status = WrapperStatus::Cancelled; result.eager.reset(); }
	else { force_ = result; }
	return result;
}
OperationReceipt PreparationPass::CheckForceRebuildAppInfo() {
	std::lock_guard lock(mutex_); return CheckForceLocked();
}
EstimateReceipt PreparationPass::EstimateForceRebuildAppInfo() {
	std::lock_guard lock(mutex_);
	force_estimate_.reset();
	EstimateReceipt result;
	if (stopped_.load()) { result.status = WrapperStatus::Cancelled; return result; }
	// UI +889691 only schedules Estimate for status0 + corruption1. Force
	// +893390 writes1/1 and returns0; +895cc0's minimum1 clamp preserves both.
	if (!force_ || force_->status != WrapperStatus::Completed || !force_->eager ||
	    force_->eager->callback_status != 0 || !force_->eager->is_corrupt) {
		result.detail = "Force Estimate not selected by a completed corrupt Eager result"; return result;
	}
	result.status = WrapperStatus::Completed;
	result.result = EstimateResult {0, 1, 1};
	if (stopped_.load()) { result.status = WrapperStatus::Cancelled; result.result.reset(); }
	else { force_estimate_ = result; }
	return result;
}
OperationReceipt PreparationPass::Get(OperationKey key) const {
	std::lock_guard lock(mutex_);
	auto result = Unavailable(key);
	if (stopped_.load()) { result.status = WrapperStatus::Cancelled; }
	else if (key == Force && force_) { result = *force_; }
	return result;
}
EagerWireStatus PreparationPass::ReadEagerWire(OperationKey key, std::span<uint8_t> output) const {
	if (!Valid(key) || output.size() != 12) { return EagerWireStatus::InvalidRequest; }
	std::lock_guard lock(mutex_);
	if (stopped_.load()) { return EagerWireStatus::Cancelled; }
	if (key != Force || !force_) { return EagerWireStatus::Unavailable; }
	if (force_->status == WrapperStatus::Unavailable) { return EagerWireStatus::Unavailable; }
	if (force_->status != WrapperStatus::Completed || !force_->eager) { return EagerWireStatus::OperationFailed; }
	std::array<uint8_t, 12> bytes {};
	Put32(bytes, 0, static_cast<uint32_t>(force_->eager->callback_status));
	bytes[4] = force_->eager->is_corrupt ? 1 : 0;
	Put32(bytes, 8, force_->eager->promote_type);
	std::copy(bytes.begin(), bytes.end(), output.begin());
	return EagerWireStatus::Found;
}
EagerWireStatus PreparationPass::ReadEstimateWire(OperationKey key, std::span<uint8_t> output) const {
	if (!Valid(key) || output.size() != 12) { return EagerWireStatus::InvalidRequest; }
	std::lock_guard lock(mutex_);
	if (stopped_.load()) { return EagerWireStatus::Cancelled; }
	if (key != Force || !force_estimate_) { return EagerWireStatus::Unavailable; }
	if (force_estimate_->status != WrapperStatus::Completed || !force_estimate_->result) { return EagerWireStatus::OperationFailed; }
	std::array<uint8_t, 12> bytes {};
	Put32(bytes, 0, static_cast<uint32_t>(force_estimate_->result->callback_status));
	Put32(bytes, 4, force_estimate_->result->value1); Put32(bytes, 8, force_estimate_->result->value2);
	std::copy(bytes.begin(), bytes.end(), output.begin());
	return EagerWireStatus::Found;
}
PreparationReceipt PreparationPass::RunMode1() {
	std::lock_guard lock(mutex_);
	PreparationReceipt result;
	for (auto key: Mode1) {
		auto operation = key == Force ? CheckForceLocked() : Unavailable(key);
		if (stopped_.load()) { operation.status = WrapperStatus::Cancelled; }
		if (operation.eager && operation.eager->is_corrupt) { result.recovery_requested = true; }
		if (!result.first_blocking_operation && operation.status != WrapperStatus::Completed) {
			result.first_blocking_operation = key;
			if (operation.status != WrapperStatus::Unavailable) { result.status = PreparationStatus::OperationFailed; }
		}
		result.operations.push_back(std::move(operation));
	}
	if (stopped_.load()) {
		result.status = PreparationStatus::Cancelled;
		for (auto& operation: result.operations) { operation.status = WrapperStatus::Cancelled; operation.eager.reset(); }
	}
	// Even all Eager checks would not certify Estimate/Promote, registered
	// preparation producers, notification completion or composite 0x160008 state.
	return result;
}
void PreparationPass::Stop() {
	stopped_.store(true);
	observations_.Stop();
	std::lock_guard lock(mutex_); force_.reset(); force_estimate_.reset();
}

ProducerRunReceipt RunSelectedPreparationProducers(const std::array<uint8_t, 16>& notification,
    const std::vector<PreparationProducerBinding>& selected, const std::atomic<bool>& cancelled) {
	ProducerRunReceipt result;
	if (cancelled.load()) { result.status = ProducerRunStatus::Cancelled; return result; }
	const auto invalid_field = [&](size_t offset) {
		return std::all_of(notification.begin() + offset, notification.begin() + offset + 4, [](uint8_t b) { return b == 0xff; });
	};
	if (invalid_field(0) || invalid_field(12)) { result.status = ProducerRunStatus::InvalidNotification; return result; }
	if (selected.empty()) { return result; }
	bool unavailable = false, failed = false;
	for (const auto& binding: selected) {
		if (cancelled.load()) { result.status = ProducerRunStatus::Cancelled; return result; }
		ProducerCallReceipt call {binding.name, {}, binding.descriptor_failure_code};
		if (binding.invoke && binding.descriptor_failure_code) {
			// Exceptions/absent returns are host unavailability, never a guessed
			// native producer status or descriptor error.
			try { call.returned_status = binding.invoke(notification); } catch (...) { call.returned_status.reset(); }
		}
		if (!call.returned_status) { unavailable = true; }
		else if (*call.returned_status != 0) {
			failed = true;
			if (result.accumulated_descriptor_error == 0) { result.accumulated_descriptor_error = *binding.descriptor_failure_code; }
		}
		result.calls.push_back(std::move(call));
	}
	result.status = cancelled.load() ? ProducerRunStatus::Cancelled : unavailable ? ProducerRunStatus::Unavailable :
	                failed ? ProducerRunStatus::ProducerFailed : ProducerRunStatus::SelectedListComplete;
	return result;
}
} // namespace Loader::LocalDbRecovery
