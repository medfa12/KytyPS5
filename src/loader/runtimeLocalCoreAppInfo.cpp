#include "loader/runtimeLocalCoreAppInfo.h"
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace Loader::LocalDbRecovery {
namespace {
struct Failure { CoreSourceStatus status; std::string detail; };
using Db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
void Require(bool ok, const char* why) { if (!ok) { throw Failure {CoreSourceStatus::Unsupported, why}; } }
void Cancel(const std::atomic<bool>& stopped) { if (stopped.load()) { throw Failure {CoreSourceStatus::Cancelled, "cancelled"}; } }
Statement Prepare(sqlite3* db, const char* sql) {
	sqlite3_stmt* raw = nullptr;
	const auto status = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
	Statement stmt(raw, sqlite3_finalize);
	if (status != SQLITE_OK) { throw Failure {CoreSourceStatus::SqlError, sqlite3_errmsg(db)}; }
	return stmt;
}
uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
	Require(at <= b.size() && b.size() - at >= 4, "truncated SFO");
	return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
uint16_t U16(const std::vector<uint8_t>& b, size_t at) {
	Require(at <= b.size() && b.size() - at >= 2, "truncated SFO");
	return uint16_t(b[at]) | (uint16_t(b[at + 1]) << 8);
}
std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
	const auto length = std::filesystem::file_size(path);
	Require(length > 0 && length <= 0x20000, "metadata exceeds supported native input size");
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	std::ifstream file(path, std::ios::binary);
	file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
	if (!file || file.peek() != std::char_traits<char>::eof()) { throw Failure {CoreSourceStatus::IoError, "metadata read changed or failed"}; }
	return bytes;
}
std::optional<uint64_t> SfoVersion(const std::vector<uint8_t>& b) {
	Require(U32(b, 0) == 0x46535000 && U32(b, 4) == 0x101, "unsupported SFO header");
	const size_t keys = U32(b, 8), data = U32(b, 12), count = U32(b, 16);
	Require(count <= 256 && 20 + count * 16 <= keys && keys <= data && data <= b.size(), "invalid SFO tables");
	std::optional<uint64_t> version;
	for (size_t i = 0; i < count; ++i) {
		const size_t at = 20 + i * 16, key = keys + U16(b, at), offset = data + uint64_t(U32(b, at + 12));
		const size_t length = U32(b, at + 4), capacity = U32(b, at + 8);
		Require(key < data && length <= capacity && offset <= b.size() && capacity <= b.size() - offset, "invalid SFO entry");
		const auto end = std::find(b.begin() + key, b.begin() + data, 0);
		Require(end != b.begin() + data, "unterminated SFO key");
		const std::string name(b.begin() + key, end);
		if (name != "SYSTEM_VER") { continue; }
		Require(!version && U16(b, at + 2) == 0x404 && length == 4, "unsupported duplicate/type SYSTEM_VER");
		version = U32(b, offset);
	}
	return version;
}
std::optional<uint64_t> JsonVersion(const std::vector<uint8_t>& b) {
	sqlite3* raw = nullptr;
	if (sqlite3_open(":memory:", &raw) != SQLITE_OK) { if (raw) { sqlite3_close(raw); } throw Failure {CoreSourceStatus::SqlError, "JSON parser allocation"}; }
	Db db(raw, sqlite3_close);
	auto valid = Prepare(db.get(), "SELECT json_valid(?1)");
	sqlite3_bind_text(valid.get(), 1, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), SQLITE_STATIC);
	Require(sqlite3_step(valid.get()) == SQLITE_ROW && sqlite3_column_int(valid.get(), 0) == 1, "malformed JSON metadata");
	auto object = Prepare(db.get(), "SELECT json_type(?1)");
	sqlite3_bind_text(object.get(), 1, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), SQLITE_STATIC);
	Require(sqlite3_step(object.get()) == SQLITE_ROW && std::string(reinterpret_cast<const char*>(sqlite3_column_text(object.get(), 0))) == "object", "metadata JSON root is not an object");
	auto field = Prepare(db.get(), "SELECT type,value FROM json_each(?1) WHERE key='requiredSystemSoftwareVersion'");
	sqlite3_bind_text(field.get(), 1, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), SQLITE_STATIC);
	int step = sqlite3_step(field.get());
	if (step == SQLITE_DONE) { return {}; }
	Require(step == SQLITE_ROW, "JSON extraction failed");
	Require(std::string(reinterpret_cast<const char*>(sqlite3_column_text(field.get(), 0))) == "text", "nonstring requiredSystemSoftwareVersion");
	std::string text(reinterpret_cast<const char*>(sqlite3_column_text(field.get(), 1)), sqlite3_column_bytes(field.get(), 1));
	Require(sqlite3_step(field.get()) == SQLITE_DONE, "duplicate requiredSystemSoftwareVersion");
	if (text.empty() || text.size() > 32) { return {}; } // native omits this field
	// Native uses strtoull(base16). Support canonical descriptors; leave exotic
	// sign/whitespace/trailing-junk conversions explicit rather than guessing.
	std::string_view digits(text);
	if (digits.starts_with("0x") || digits.starts_with("0X")) { digits.remove_prefix(2); }
	uint64_t value = 0;
	const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
	Require(!digits.empty() && parsed.ec == std::errc() && parsed.ptr == digits.data() + digits.size(), "unsupported version spelling/overflow");
	return value;
}
bool RegularOrAbsent(const std::filesystem::path& path) {
	std::error_code ec;
	const auto status = std::filesystem::symlink_status(path, ec);
	if (status.type() == std::filesystem::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) { return false; }
	if (ec) { throw std::filesystem::filesystem_error("metadata status", path, ec); }
	Require(std::filesystem::is_regular_file(status), "metadata is not a regular copied file");
	return true;
}
}

CoreVersionReceipt ReadCoreVersions(const std::filesystem::path& copied_appinfo, const std::atomic<bool>& cancelled) {
	CoreVersionReceipt out; out.source = copied_appinfo;
	try {
		Cancel(cancelled);
		if (copied_appinfo.filename() != "appinfo.db" || !RegularOrAbsent(copied_appinfo)) {
			out.status = CoreSourceStatus::InvalidInput; out.detail = "explicit existing copied Core appinfo.db required"; return out;
		}
		sqlite3* raw = nullptr;
		out.sqlite_error = sqlite3_open_v2(copied_appinfo.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
		Db db(raw, sqlite3_close);
		if (out.sqlite_error != SQLITE_OK) { out.status = CoreSourceStatus::SqlError; return out; }
		sqlite3_progress_handler(db.get(), 100, [](void* p) { return static_cast<const std::atomic<bool>*>(p)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancelled));
		Require(sqlite3_exec(db.get(), "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK, "begin read snapshot");
		auto table = Prepare(db.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='tbl_version'");
		if (sqlite3_step(table.get()) != SQLITE_ROW) { throw Failure {CoreSourceStatus::SqlError, sqlite3_errmsg(db.get())}; }
		out.table_present = sqlite3_column_int(table.get(), 0) != 0;
		if (out.table_present) {
			auto rows = Prepare(db.get(), "SELECT category,status FROM tbl_version WHERE category IN ('appver','appver_shellui','appinfover','build_target')");
			int step;
			while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
				Cancel(cancelled);
				CoreVersionRow row;
				row.category = reinterpret_cast<const char*>(sqlite3_column_text(rows.get(), 0));
				row.sqlite_type = sqlite3_column_type(rows.get(), 1);
				if (row.sqlite_type == SQLITE_INTEGER) { row.native_value = std::bit_cast<int32_t>(static_cast<uint32_t>(sqlite3_column_int64(rows.get(), 1))); }
				out.rows.push_back(std::move(row));
			}
			if (step != SQLITE_DONE) { out.sqlite_error = step; throw Failure {CoreSourceStatus::SqlError, sqlite3_errmsg(db.get())}; }
		}
		Cancel(cancelled); out.status = CoreSourceStatus::Read;
	} catch (const Failure& e) { out.status = e.status; out.detail = e.detail; }
	catch (const std::exception& e) { out.status = CoreSourceStatus::IoError; out.detail = e.what(); }
	if (cancelled.load()) { out.status = CoreSourceStatus::Cancelled; }
	if (out.status != CoreSourceStatus::Read) { out.rows.clear(); }
	return out;
}

CoreInventoryReceipt CheckCoreMetadataVersions(const CoreInventoryRequest& request, const std::atomic<bool>& cancelled) {
	CoreInventoryReceipt out;
	try {
		Cancel(cancelled);
		for (const auto& root: {request.copied_appmeta, request.copied_external_appmeta}) {
			if (root.empty() || !std::filesystem::is_directory(std::filesystem::symlink_status(root))) {
				throw Failure {CoreSourceStatus::Unavailable, "both explicit copied appmeta namespaces required"};
			}
		}
		Require(!std::filesystem::equivalent(request.copied_appmeta, request.copied_external_appmeta), "appmeta namespaces alias");
		for (const auto& root: {request.copied_appmeta, request.copied_external_appmeta}) {
			std::vector<std::filesystem::path> directories;
			for (const auto& entry: std::filesystem::directory_iterator(root)) {
				Cancel(cancelled);
				if (entry.path().filename().string().starts_with('.')) { continue; }
				const auto status = entry.symlink_status();
				Require(!std::filesystem::is_symlink(status), "linked appmeta directory requires native namespace resolution");
				if (std::filesystem::is_directory(status)) { directories.push_back(entry.path()); }
			}
			// Stable diagnostic order; native readdir order is unspecified. Every
			// rejecting version yields the same native0x80a3000d.
			std::sort(directories.begin(), directories.end());
			for (const auto& directory: directories) {
				Cancel(cancelled);
				MetadataVersionEntry entry; entry.directory_name = directory.filename().string();
				Require(entry.directory_name.size() <= 80 && entry.directory_name.find(".sfo") == std::string::npos &&
				        entry.directory_name.find(".json") == std::string::npos, "unsupported native path truncation/substring dispatch");
				entry.metadata = directory / "param.sfo";
				if (!RegularOrAbsent(entry.metadata)) { entry.metadata = directory / "param.json"; entry.prospero = true; }
				if (!RegularOrAbsent(entry.metadata)) {
					entry.metadata_absent = true; out.entries.push_back(std::move(entry)); continue;
				}
				entry.retained_bytes = ReadBytes(entry.metadata);
				entry.required_version = entry.prospero ? JsonVersion(entry.retained_bytes) : SfoVersion(entry.retained_bytes);
				out.entries.push_back(std::move(entry));
				const auto& observed = out.entries.back();
				if (!observed.required_version) { continue; } // native missing key skips title
				const auto& kernel = observed.prospero ? request.prospero_system : request.legacy_system;
				if (!kernel || kernel->provenance.empty()) { throw Failure {CoreSourceStatus::Unavailable, "required kernel system-version observation unavailable"}; }
				if (kernel->returned_status < 0) { out.callback_status = kernel->returned_status; out.status = CoreSourceStatus::Read; break; }
				const auto required = observed.prospero ? uint32_t(*observed.required_version >> 32) & 0xffffff00u : uint32_t(*observed.required_version) & 0xfffff000u;
				const auto current = kernel->version & (observed.prospero ? 0xffffff00u : 0xfffff000u);
				if (required > current) { out.callback_status = std::bit_cast<int32_t>(0x80a3000du); out.status = CoreSourceStatus::Read; break; }
			}
			if (out.callback_status) { break; }
		}
		Cancel(cancelled);
		out.status = CoreSourceStatus::Read;
		if (!out.callback_status) { out.callback_status = 0; }
	} catch (const Failure& e) { out.status = e.status; out.detail = e.detail; }
	catch (const std::exception& e) { out.status = CoreSourceStatus::IoError; out.detail = e.what(); }
	if (cancelled.load()) { out.status = CoreSourceStatus::Cancelled; }
	if (out.status != CoreSourceStatus::Read) { out.callback_status.reset(); }
	return out;
}

CoreSourceStatus CheckCoreKeepState(CoreKeepState& state, const CoreInventoryRequest& request,
                                   CoreInventoryReceipt& receipt, const std::atomic<bool>& cancelled) {
	receipt = {};
	if (cancelled.load()) { receipt.status = CoreSourceStatus::Cancelled; return receipt.status; }
	if (state.daemon_status != 0x20 || !state.keep) {
		receipt.status = CoreSourceStatus::Read; receipt.detail = "native inventory gate not selected"; return receipt.status;
	}
	receipt = CheckCoreMetadataVersions(request, cancelled);
	if (receipt.status == CoreSourceStatus::Read && receipt.callback_status && *receipt.callback_status != 0) {
		state.keep = false; state.daemon_status = 0x100; state.suppress_schema_downgrade = true;
	}
	return receipt.status;
}
} // namespace Loader::LocalDbRecovery
