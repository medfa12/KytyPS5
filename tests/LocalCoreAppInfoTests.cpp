#include "loader/runtimeLocalCoreAppInfo.h"
#include <sqlite3.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace {
using namespace Loader::LocalDbRecovery;
void Check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "CoreAppInfo: %s\n", message); std::abort(); } }
void Write(const std::filesystem::path& path, const std::string& text) {
	std::ofstream f(path, std::ios::binary); f.write(text.data(), text.size()); f.close(); Check(bool(f), "write fixture");
}
std::string Read(const std::filesystem::path& path) {
	std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
}
void RunLocalCoreAppInfoTests(const std::filesystem::path& root) {
	using namespace Loader::LocalDbRecovery;
	std::atomic<bool> cancelled {false};
	const auto dir = root / "core-appinfo";
	std::filesystem::create_directory(dir);
	const auto dbpath = dir / "appinfo.db";
	Check(ReadCoreVersions(dbpath, cancelled).status == CoreSourceStatus::InvalidInput && !std::filesystem::exists(dbpath), "missing DB created");
	sqlite3* db = nullptr; Check(sqlite3_open(dbpath.string().c_str(), &db) == SQLITE_OK, "open synthetic Core fixture");
	Check(sqlite3_exec(db, "CREATE TABLE unrelated(x);", nullptr, nullptr, nullptr) == SQLITE_OK, "create fixture");
	auto versions = ReadCoreVersions(dbpath, cancelled);
	Check(versions.status == CoreSourceStatus::Read && !versions.table_present && versions.rows.empty(), "missing versions fabricated");
	Check(sqlite3_exec(db, "CREATE TABLE tbl_version(category TEXT,status); INSERT INTO tbl_version VALUES('appinfover',36),('appver',1861194383),('build_target','1'),('appver_shellui',NULL),('access_index',99);", nullptr, nullptr, nullptr) == SQLITE_OK, "version fixture");
	sqlite3_close(db);
	const auto before = Read(dbpath);
	versions = ReadCoreVersions(dbpath, cancelled);
	Check(versions.status == CoreSourceStatus::Read && versions.table_present && versions.rows.size() == 4, "Core categories extraction");
	Check(versions.rows[0].native_value == 36 && versions.rows[2].sqlite_type == SQLITE_TEXT && versions.rows[2].native_value == 0 &&
	      versions.rows[3].sqlite_type == SQLITE_NULL && versions.rows[3].native_value == 0, "native SQLite type distinction lost");
	Check(Read(dbpath) == before, "Core version read changed source");
	Check(ReadCoreVersions(root / "app.db", cancelled).status == CoreSourceStatus::InvalidInput, "AppBrowse admitted as Core");
	CoreInventoryRequest request {dir / "appmeta", dir / "external", {}, {}};
	Check(CheckCoreMetadataVersions(request, cancelled).status == CoreSourceStatus::Unavailable, "unknown namespace assumed empty");
	std::filesystem::create_directory(request.copied_appmeta); std::filesystem::create_directory(request.copied_external_appmeta);
	auto result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.status == CoreSourceStatus::Read && result.callback_status == 0 && result.entries.empty(), "explicit empty namespace");
	const auto title = request.copied_appmeta / "PPSA_TEST";
	std::filesystem::create_directory(title);
	Write(title / "param.json", "{\"requiredSystemSoftwareVersion\":\"0x136001ff12345678\"}");
	const auto original = Read(title / "param.json");
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.status == CoreSourceStatus::Unavailable && !result.callback_status && result.entries.size() == 1, "missing kernel observation certified compatibility");
	request.prospero_system = KernelVersionObservation {0, 0x13600100, "synthetic kernel observation for branch test"};
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.callback_status == 0 && result.entries[0].required_version == UINT64_C(0x136001ff12345678), "Prospero mask/high DWORD comparison");
	request.prospero_system->version = 0x136000ff;
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.status == CoreSourceStatus::Read && result.callback_status && uint32_t(*result.callback_status) == 0x80a3000d, "higher required version not rejected");
	CoreKeepState state {0x20, true, false};
	Check(CheckCoreKeepState(state, request, result, cancelled) == CoreSourceStatus::Read && state.daemon_status == 0x100 && !state.keep && state.suppress_schema_downgrade,
	      "Core keep-state rejection/latch transition");
	state = {0x60, true, false};
	Check(CheckCoreKeepState(state, {}, result, cancelled) == CoreSourceStatus::Read && !result.callback_status && state.keep && !state.suppress_schema_downgrade,
	      "non-exact daemon status called inventory");
	state = {0x20, true, false};
	request.prospero_system.reset();
	Check(CheckCoreKeepState(state, request, result, cancelled) == CoreSourceStatus::Unavailable && state.keep && !state.suppress_schema_downgrade, "host unavailable converted to native mutation");
	request.prospero_system = KernelVersionObservation {-7, 0, "synthetic kernel failure"};
	Check(CheckCoreMetadataVersions(request, cancelled).callback_status == -7, "kernel error dropped");
	// SFO wins over JSON for appmeta, with legacy mask distinct from Prospero.
	std::string sfo(52, '\0');
	auto put = [&](size_t at, uint32_t v) { for (size_t i = 0; i < 4; ++i) { sfo[at + i] = char(v >> (8 * i)); } };
	put(0, 0x46535000); put(4, 0x101); put(8, 36); put(12, 48); put(16, 1);
	put(20, 0x04040000); put(24, 4); put(28, 4); put(32, 0); sfo.replace(36, 10, "SYSTEM_VER"); put(48, 0x09000fff);
	Write(title / "param.sfo", sfo);
	request.legacy_system = KernelVersionObservation {0, 0x09000000, "synthetic legacy observation"};
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.callback_status == 0 && !result.entries[0].prospero && result.entries[0].retained_bytes.size() == 52, "SFO precedence or legacy mask incorrect");
	std::filesystem::remove(title / "param.sfo");
	const auto external = request.copied_external_appmeta / "external-title";
	std::filesystem::create_directory(external);
	Write(external / "param.json", "{\"requiredSystemSoftwareVersion\":\"0x1400000000000000\"}");
	request.prospero_system = KernelVersionObservation {0, 0x13600100, "synthetic observation"};
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.callback_status && uint32_t(*result.callback_status) == 0x80a3000d && result.entries.size() == 2, "external namespace omitted");
	Write(title / "param.json", "{\"requiredSystemSoftwareVersion\":\"20\",\"requiredSystemSoftwareVersion\":\"30\"}");
	result = CheckCoreMetadataVersions(request, cancelled);
	Check(result.status == CoreSourceStatus::Unsupported && !result.callback_status, "ambiguous JSON produced native success");
	Write(title / "param.json", "[]");
	Check(CheckCoreMetadataVersions(request, cancelled).status == CoreSourceStatus::Unsupported, "array root admitted as metadata object");
	Write(title / "param.json", original);
	cancelled.store(true);
	Check(CheckCoreMetadataVersions(request, cancelled).status == CoreSourceStatus::Cancelled && ReadCoreVersions(dbpath, cancelled).status == CoreSourceStatus::Cancelled,
	      "cancelled source reads succeeded");
	Check(Read(title / "param.json") == original && Read(dbpath) == before, "source preservation");
	std::puts("CoreAppInfo: source version types, mode7 namespace/metadata/version masks, kernel failures, keep-state gate/latch and preservation passed");
}
