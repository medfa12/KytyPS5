#pragma once
#include "loader/runtimeLocalDbRecovery.h"
#include <string>
#include <vector>
#include <functional>

namespace Loader::LocalDbRecovery {
struct OperationKey {
	uint32_t target = 0, operation = 0;
	bool operator==(const OperationKey&) const = default;
};
// Native EagerCheck wire fields at offsets 0/4/8. This is distinct from
// Estimate's 12 bytes and Promote's 8 bytes, and from an observation bool.
struct EagerResult {
	int32_t callback_status = 0;
	bool is_corrupt = false;
	uint32_t promote_type = 0;
};
enum class WrapperStatus { Completed, Unavailable, InputError, IoError, Cancelled };
enum class PostCheckStatus { NotAttempted, ReportAbsent, ReportRemoved, Failed };
struct OperationReceipt {
	OperationKey key;
	WrapperStatus status = WrapperStatus::Unavailable;
	ForceRebuildObservation observation;
	std::optional<uint32_t> extracted_promotion;
	PostCheckStatus post_check = PostCheckStatus::NotAttempted;
	std::optional<EagerResult> eager;
	std::error_code host_error;
	std::string detail;
};
struct EstimateResult {
	int32_t callback_status = 0;
	uint32_t value1 = 0, value2 = 0; // native fields at offsets4/8
};
struct EstimateReceipt {
	OperationKey key {1, 4};
	WrapperStatus status = WrapperStatus::Unavailable;
	std::optional<EstimateResult> result;
	std::string detail;
};
struct RecoveryWorkspaceRequest {
	std::filesystem::path copied_app_db;
	// Explicit copied force-report path, even when absent. Empty is unavailable,
	// not evidence of absence. Its containing directory must exist.
	std::filesystem::path copied_force_report;
	std::filesystem::path new_private_report_directory;
};
struct RecoveryWorkspaceReceipt {
	WrapperStatus status = WrapperStatus::Unavailable;
	std::filesystem::path retained_directory;
	bool source_report_present = false;
	std::error_code host_error;
	std::string detail;
};
enum class EagerWireStatus { Found, Unavailable, InvalidRequest, OperationFailed, Cancelled };
enum class PreparationStatus { PrerequisitesUnavailable, OperationFailed, RecoveryRequired, Cancelled };
struct PreparationReceipt {
	PreparationStatus status = PreparationStatus::PrerequisitesUnavailable;
	// Actual mode1/filter0 order. No result is borrowed from another key.
	std::vector<OperationReceipt> operations;
	std::optional<OperationKey> first_blocking_operation;
	bool recovery_requested = false;
	// Full registered producer list, native composite state and notification
	// publication are not available from local SQL/catalog receipts.
	bool native_producer_registry_available = false;
};

// The accumulating branch of Core +b24380. A binding must retain its actual
// native producer/caller execution lease; this runner never changes identity or
// dispatches a server callback on a client. Missing bindings are unavailable.
struct PreparationProducerBinding {
	std::string name;
	std::optional<int32_t> descriptor_failure_code; // native record+0x28
	std::function<std::optional<int32_t>(const std::array<uint8_t, 16>&)> invoke;
};
enum class ProducerRunStatus { SelectedListComplete, ProducerFailed, Unavailable, InvalidNotification, Cancelled };
struct ProducerCallReceipt {
	std::string name;
	std::optional<int32_t> returned_status;
	std::optional<int32_t> descriptor_failure_code;
};
struct ProducerRunReceipt {
	ProducerRunStatus status = ProducerRunStatus::Unavailable;
	std::vector<ProducerCallReceipt> calls;
	// Native retains descriptor+0x28, NOT the callback's arbitrary return code.
	int32_t accumulated_descriptor_error = 0;
};
// Certifies ONLY the supplied selected list. Does not construct the composite
// native24-byte preparation status or mark observer notification complete.
ProducerRunReceipt RunSelectedPreparationProducers(const std::array<uint8_t, 16>& notification,
    const std::vector<PreparationProducerBinding>& selected, const std::atomic<bool>& cancelled);

// Owns a NEW private force-report workspace, never the source report. The
// AppBrowse database remains the caller's one copied backend, read-only here.
// Models initialized Force's complete Eager wrapper; unsupported operations
// remain explicit. No guest service or preparation completion is installed.
class PreparationPass {
public:
	explicit PreparationPass(RecoveryWorkspaceRequest request);
	RecoveryWorkspaceReceipt Initialize();
	OperationReceipt CheckForceRebuildAppInfo();
	EstimateReceipt EstimateForceRebuildAppInfo();
	OperationReceipt Get(OperationKey key) const;
	PreparationReceipt RunMode1();
	// 0x150011-shaped local connection point. Output is untouched unless Found.
	// Caller identity/copyin/copyout remain owned by the native transport adapter.
	EagerWireStatus ReadEagerWire(OperationKey key, std::span<uint8_t> output) const;
	EagerWireStatus ReadEstimateWire(OperationKey key, std::span<uint8_t> output) const;
	void Stop();
private:
	OperationReceipt CheckForceLocked();
	const RecoveryWorkspaceRequest request_;
	EagerCheckPass observations_;
	std::atomic<bool> stopped_ {false};
	mutable std::mutex mutex_;
	bool initialization_attempted_ = false;
	RecoveryWorkspaceReceipt workspace_;
	// UI ctor +893250 and Initialize +88ee60 set primary/secondary/third to2.
	// Force callback +8932c0 does not update these object fields.
	uint32_t force_secondary_state_ = 2;
	std::optional<OperationReceipt> force_;
	std::optional<EstimateReceipt> force_estimate_;
};
} // namespace Loader::LocalDbRecovery
