#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Loader::LocalAppDb {
// A private AppBrowse candidate, never a replacement for Core appinfo.db.
// Only the two firmware Library descriptors are supported by this import.
enum class CatalogStatus { Committed, InvalidInput, Conflict, SqlError, Cancelled };
struct CatalogRequest {
	std::filesystem::path copied_app_db;
	std::filesystem::path new_candidate_directory;
	// Contains copied NPXS40071/appdb/{default,NPXS40139}/{param.json,icon0.png}.
	std::filesystem::path copied_library_appdb;
	std::vector<int32_t> local_users;
	std::string language;
	// Explicit incremental mode: keep complete existing profile table pairs
	// untouched; create only absent pairs. A lone table is a conflict. Existing
	// pairs are not certified native-ready, complete, or source-backed.
	bool preserve_existing_profiles = false;
};
struct CatalogReceipt {
	CatalogStatus status = CatalogStatus::InvalidInput;
	int sqlite_error = 0;
	std::string detail;
	// Set only after the entire source/profile projection transaction commits.
	std::filesystem::path candidate_app_db;
	// Claimed private directory, including on failure. Empty means no ownership
	// was acquired. A failed backup may leave an incomplete database here.
	std::filesystem::path retained_candidate_directory;
	bool input_backup_complete = false;
	size_t source_records = 0;
	size_t profile_records = 0;
	size_t existing_profiles = 0;
	// Which existing pairs were deliberately untouched, not validated as usable.
	std::vector<int32_t> preserved_profiles;
};
// Source backup is read-only. Atomically claims a new directory; an existing
// destination is never opened. Failed candidates remain private for diagnosis.
// Cancellation is cooperative through the last check before COMMIT. Once that
// check passes COMMIT is non-cancellable; its actual result owns the receipt.
// The atomic flag is not a linearizable Stop/abort-after-return API.
// No native version, revision, recovery result, marker removal, or event signal.
CatalogReceipt BuildSystemCatalogCandidate(const CatalogRequest&, const std::atomic<bool>& cancelled);
} // namespace Loader::LocalAppDb
