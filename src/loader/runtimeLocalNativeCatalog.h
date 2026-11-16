#pragma once
#include "loader/runtimeLocalHomeQuery.h"
#include <array>

namespace Loader::LocalAppDb {
struct NativeCatalogMount {
	std::string guest_directory;
	std::filesystem::path host_directory;
	bool read_only = true;
};
enum class NativeCatalogStageStatus { Staged, InvalidInput, ReadFailed, Conflict, IoError, Cancelled };
struct NativeCatalogStageReceipt {
	NativeCatalogStageStatus status = NativeCatalogStageStatus::InvalidInput;
	std::string detail;
	std::filesystem::path retained_directory;
	// Both use the SAME candidate app.db. These are source/SQL receipts, not
	// replacement managed objects or a preparation success certificate.
	std::array<HomeReadReceipt, 2> home_reads;
	std::vector<NativeCatalogMount> mounts;
};
// Stage retained PNG bytes for native file readers and produce concrete mount
// inputs. Does not mount, publish services, switch profiles or launch guests.
// Candidate must be stable/private; runtime admission/lifetime remains required.
NativeCatalogStageReceipt StageNativeCatalog(const std::filesystem::path& candidate_app_db, int32_t user,
                                             const std::filesystem::path& new_asset_directory,
                                             const std::atomic<bool>& cancelled);
} // namespace Loader::LocalAppDb
