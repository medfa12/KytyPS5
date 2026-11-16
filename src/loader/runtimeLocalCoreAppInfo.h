#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Loader::LocalDbRecovery {
enum class CoreSourceStatus { Read, Unavailable, InvalidInput, IoError, SqlError, Unsupported, Cancelled };
struct CoreVersionRow {
	std::string category;
	int sqlite_type = 0;
	// Native integer extraction's low DWORD; noninteger SQLite values become0.
	int32_t native_value = 0;
};
struct CoreVersionReceipt {
	CoreSourceStatus status = CoreSourceStatus::Unavailable;
	std::filesystem::path source;
	bool table_present = false;
	std::vector<CoreVersionRow> rows;
	int sqlite_error = 0;
	std::string detail;
};
// Existing copied Core appinfo.db only. No CREATE, migration or version writes.
CoreVersionReceipt ReadCoreVersions(const std::filesystem::path& copied_appinfo, const std::atomic<bool>& cancelled);

// Actual kernel observation, not derived from a firmware directory's name or
// a Library descriptor. Runtime binding must retain the native execution lease.
struct KernelVersionObservation {
	int32_t returned_status = 0;
	uint32_t version = 0;
	std::string provenance;
};
struct MetadataVersionEntry {
	std::string directory_name; // native enumeration label; not an entitlement ID
	std::filesystem::path metadata;
	std::vector<uint8_t> retained_bytes;
	std::optional<uint64_t> required_version;
	bool prospero = false;
	bool metadata_absent = false;
};
struct CoreInventoryRequest {
	// Both explicit copied namespaces are required, including an empty external
	// directory. Never substitute system Library or firmware app directories.
	std::filesystem::path copied_appmeta;
	std::filesystem::path copied_external_appmeta;
	std::optional<KernelVersionObservation> legacy_system;
	std::optional<KernelVersionObservation> prospero_system;
};
struct CoreInventoryReceipt {
	CoreSourceStatus status = CoreSourceStatus::Unavailable;
	std::optional<int32_t> callback_status;
	std::vector<MetadataVersionEntry> entries;
	std::string detail;
};
// Core +366330 mode7. This is a metadata compatibility prerequisite, neither a
// full installed-title catalog nor account/license ownership nor recovery ready.
CoreInventoryReceipt CheckCoreMetadataVersions(const CoreInventoryRequest& request, const std::atomic<bool>& cancelled);

struct CoreKeepState {
	uint32_t daemon_status = 0;
	bool keep = false;
	bool suppress_schema_downgrade = false;
};
// +ffe4d0 and its caller +17a15d0: only exact status0x20/keep1 runs mode7.
// Missing/failed host evidence does not mutate state or invent a native result.
CoreSourceStatus CheckCoreKeepState(CoreKeepState& state, const CoreInventoryRequest& request,
                                   CoreInventoryReceipt& receipt, const std::atomic<bool>& cancelled);
} // namespace Loader::LocalDbRecovery
