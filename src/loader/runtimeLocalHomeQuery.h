#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace Loader::LocalAppDb {
enum class HomeSpace { Game, Media };
enum class HomeReadStatus { Read, InvalidInput, SqlError, Cancelled, ResourceLimit, SourceConflict };
using HomeBlob = std::vector<uint8_t>;
using HomeValue = std::variant<std::monostate, int64_t, double, std::string, HomeBlob>;
using HomeRow = std::map<std::string, HomeValue>;
struct HomeReadRequest {
	std::filesystem::path candidate_app_db;
	int32_t user_id = -1;
	HomeSpace space = HomeSpace::Game;
	// Native SetContentLimit reserves one slot for Library. Zero yields SQL
	// LIMIT -1 (unlimited); one reads only Library. Negative values unsupported.
	int32_t content_limit = 0;
	size_t max_rows = 4096;
};
struct HomeReadReceipt {
	HomeReadStatus status = HomeReadStatus::InvalidInput;
	int sqlite_error = 0;
	std::string detail;
	// Managed materializer aliases, including every dependency of Home's twelve
	// requested properties and order fields. SQLite NULL/type is preserved.
	// These are inputs to managed getters, NOT serialized JS GetItems objects.
	std::vector<HomeRow> rows;
	// Exact guest path -> owned source bytes from the same database snapshot.
	// Only source-ledger-backed Library icons are currently bindable.
	std::map<std::string, HomeBlob> retained_assets;
	// Exact source JSON bytes for native directory staging alongside each icon.
	std::map<std::string, std::string> retained_descriptors;
};
// Read-only, one snapshot for ordered selection, Library append and assets.
// This receipt neither admits recovery nor activates a guest/VFS transport.
HomeReadReceipt ReadHomeRows(const HomeReadRequest&, const std::atomic<bool>& cancelled);
} // namespace Loader::LocalAppDb
