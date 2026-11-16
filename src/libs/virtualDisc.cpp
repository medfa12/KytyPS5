// PORT (kytyps5, 13.60 FW lineage): ported from voldemort-ref (4.03 FW lineage) virtualDisc.
// Same logic throughout; kytyps5-missing dependencies are handled inline and marked PORT NOTE:
// - Loader SFO helpers + sandbox word promoted as file-local (logic preserved verbatim).
// - RuntimeLinker::LaunchApplication and LibJson2::PostFrameworkEvent stubbed (comment + log;
//   the PostFrameworkEvent stub is log-free: PumpEvents runs on the hot pad-polling path).
// - Common::HostCriticalScope sites noted (advisory scope, absent in kytyps5).
// - FileSystem::Mount 3-arg read-only calls adapted to kytyps5's 2-arg Mount.
#include "libs/virtualDisc.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <nlohmann/json.hpp>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <variant> // PORT NOTE (kytyps5): source received this transitively via Loader SFO types; needed here for the promoted SfoFields helper below.
#include <vector>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#undef GetUserName
#endif

namespace Libs::VirtualDisc {
namespace {
using Json = nlohmann::json;
std::mutex mutex;
std::string title_id;
std::filesystem::path disc_folder;
std::map<std::string, std::string> metadata_snapshots;
std::deque<std::pair<int, std::string>> events;
constexpr const char* concept_prefix = "cid:virtualdisc:";
// HomeUI opens the experience switcher (128) and game space (2).
// Library (8) keeps the disc available in the collection browser too.
constexpr int disc_display_location = 128 | 8 | 2;

// PORT NOTE (kytyps5): the three helpers below are promoted verbatim from the source tree
// (Loader::SfoFields / Loader::ReadParamSfo / Loader::FindApplicationMetadata in
// voldemort-ref loader/systemContent.h/.cpp) because kytyps5's loader/systemContent.h is
// JSON-only and declares none of them. Logic is unchanged; only the Loader:: qualification
// is dropped since they are now file-local.
using SfoFields = std::map<std::string, std::variant<std::string, int32_t>>;

std::filesystem::path FindApplicationMetadata(const std::filesystem::path& folder) {
	const auto json = folder / "sce_sys/param.json";
	return std::filesystem::is_regular_file(json) ? json : folder / "sce_sys/param.sfo";
}

SfoFields ReadParamSfo(const std::filesystem::path& file_name) {
	std::ifstream input(file_name, std::ios::binary | std::ios::ate);
	if (!input) throw std::runtime_error("Cannot open sce_sys/param.sfo");
	const auto length = input.tellg();
	if (length < 20 || length > 16 * 1024 * 1024)
		throw std::runtime_error("Invalid param.sfo size");
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	input.seekg(0);
	if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
		throw std::runtime_error("Cannot read param.sfo");
	auto word = [&](size_t offset, size_t width) {
		if (offset > bytes.size() || width > bytes.size() - offset)
			throw std::runtime_error("Truncated param.sfo");
		uint32_t value = 0;
		for (size_t i = 0; i < width; ++i) value |= uint32_t(bytes[offset+i]) << (8*i);
		return value;
	};
	if (word(0,4) != 0x46535000 || word(4,4) != 0x101)
		throw std::runtime_error("Invalid param.sfo signature or version");
	const size_t keys = word(8,4), data = word(12,4), count = word(16,4);
	if (count > (bytes.size()-20)/16 || keys < 20 + count*16 || data < keys || data > bytes.size())
		throw std::runtime_error("Invalid param.sfo tables");
	SfoFields fields;
	for (size_t i = 0; i < count; ++i) {
		const size_t entry = 20 + i*16;
		const size_t key_offset = word(entry,2), format = word(entry+2,2);
		const size_t used = word(entry+4,4), capacity = word(entry+8,4), value_offset = word(entry+12,4);
		if (key_offset >= data-keys || used > capacity || value_offset > bytes.size()-data ||
		    capacity > bytes.size()-data-value_offset)
			throw std::runtime_error("Invalid param.sfo entry bounds");
		const auto key_begin = bytes.begin() + keys + key_offset;
		const auto key_end = std::find(key_begin, bytes.begin()+data, uint8_t(0));
		if (key_end == bytes.begin()+data || key_end == key_begin)
			throw std::runtime_error("Invalid param.sfo key");
		const std::string key(key_begin,key_end);
		if (fields.contains(key)) throw std::runtime_error("Duplicate param.sfo key");
		const size_t offset = data + value_offset;
		if (format == 0x204) {
			if (!used || bytes[offset+used-1] != 0)
				throw std::runtime_error("Unterminated param.sfo string");
			const auto begin = bytes.begin()+offset;
			fields.emplace(key,std::string(begin,std::find(begin,begin+used,uint8_t(0))));
		} else if (format == 0x404) {
			if (used != 4) throw std::runtime_error("Invalid param.sfo integer size");
			fields.emplace(key,static_cast<int32_t>(word(offset,4)));
		} else if (format != 0x004) {
			throw std::runtime_error("Unsupported param.sfo value format");
		}
	}
	return fields;
}

// PORT NOTE (kytyps5): verbatim logic of the source tree's
// Libs::LibKernel::FileSystem::KernelGetFsSandboxRandomWord (voldemort-ref
// kernel/fileSystem.cpp), which kytyps5 does not provide in the FileSystem namespace.
// kytyps5 only exposes a per-boot *random* word via the guest LIB_FUNC
// SceKernelGetFsSandboxRandomWord (libs/libC.cpp); that has different semantics, so the
// stable source token is kept locally instead of inventing behavior.
const char* FsSandboxRandomWord() {
	// The sandbox word is a fixed-length token the system uses to build per-title
	// sandbox paths (e.g. /mnt/sandbox/<word>). We resolve and mount guest paths
	// ourselves, so any stable non-null string suffices here.
	static const char word[] = "kyty";
	return word;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Windows supplies SQLite with the OS; no external process or extra runtime is needed.
struct Sqlite {
	void* db = nullptr;
	HMODULE library = nullptr;
	int (*open)(const char*, void**, int, const char*) = nullptr;
	int (*close)(void*) = nullptr;
	int (*exec)(void*, const char*, int(*)(void*,int,char**,char**), void*, char**) = nullptr;
	int (*prepare)(void*,const char*,int,void**,const char**) = nullptr;
	int (*bind_text)(void*,int,const char*,int,void(*)(void*)) = nullptr;
	int (*bind_int64)(void*,int,int64_t) = nullptr;
	int (*step)(void*) = nullptr;
	int64_t (*column_int64)(void*,int) = nullptr;
	int (*finalize)(void*) = nullptr;
	const char* (*errmsg)(void*) = nullptr;
	int (*busy_timeout)(void*,int) = nullptr;

	Sqlite(const std::filesystem::path& path) {
		library = LoadLibraryW(L"winsqlite3.dll");
		if (!library) throw std::runtime_error("Windows SQLite is unavailable");
#define LOAD(name, symbol) name = reinterpret_cast<decltype(name)>(GetProcAddress(library, "sqlite3_" symbol)); if (!name) { FreeLibrary(library); throw std::runtime_error("Missing SQLite function: " symbol); }
		LOAD(open, "open_v2"); LOAD(close, "close"); LOAD(exec, "exec");
		LOAD(prepare, "prepare_v2"); LOAD(bind_text, "bind_text"); LOAD(bind_int64, "bind_int64");
		LOAD(step, "step"); LOAD(column_int64, "column_int64"); LOAD(finalize, "finalize"); LOAD(errmsg, "errmsg"); LOAD(busy_timeout, "busy_timeout");
#undef LOAD
		const int result = open(Common::PathToGenericString(path).c_str(), &db, 2 /* READWRITE */, nullptr);
		if (result != 0) {
			const std::string message = db ? errmsg(db) : "Cannot open app.db";
			if (db) close(db); db = nullptr; FreeLibrary(library); library = nullptr;
			throw std::runtime_error(message);
		}
		busy_timeout(db, 2000);
	}
	~Sqlite() { if (db) close(db); if (library) FreeLibrary(library); }
	void Execute(const std::string& sql) {
		if (exec(db, sql.c_str(), nullptr, nullptr, nullptr) != 0) throw std::runtime_error(errmsg(db));
	}
	std::vector<std::string> Tables() {
		std::vector<std::string> tables;
		auto callback = [](void* user, int count, char** values, char**) -> int {
			if (count && values[0]) static_cast<std::vector<std::string>*>(user)->emplace_back(values[0]);
			return 0;
		};
		if (exec(db, "SELECT name FROM sqlite_master WHERE type='table'", callback, &tables, nullptr) != 0) throw std::runtime_error(errmsg(db));
		return tables;
	}
	void Insert(const std::string& table, const std::map<std::string,Json>& values) {
		std::string names, placeholders;
		for (const auto& [name, value] : values) {
			if (!names.empty()) { names += ','; placeholders += ','; }
			names += '"' + name + '"'; placeholders += '?';
		}
		const auto sql = "INSERT INTO \"" + table + "\" (" + names + ") VALUES (" + placeholders + ')';
		void* stmt = nullptr;
		if (prepare(db, sql.c_str(), -1, &stmt, nullptr) != 0) throw std::runtime_error(errmsg(db));
		int index = 1, result = 0;
		for (const auto& [name, value] : values) {
			if (value.is_string()) {
				const auto& text = value.get_ref<const std::string&>();
				result = bind_text(stmt, index++, text.c_str(), static_cast<int>(text.size()), nullptr);
			} else { result = bind_int64(stmt, index++, value.get<int64_t>()); }
			if (result != 0) break;
		}
		if (!result) result = step(stmt);
		const std::string error = result == 101 /* DONE */ ? "" : errmsg(db);
		finalize(stmt);
		if (!error.empty()) throw std::runtime_error(error);
	}
	int64_t NextAccessIndex(const std::string& table) {
		const auto sql = "SELECT COALESCE(MAX(lastAccessIndex),-1) FROM \"" + table + "\"";
		void* stmt = nullptr;
		if (prepare(db,sql.c_str(),-1,&stmt,nullptr) != 0) throw std::runtime_error(errmsg(db));
		const auto result = step(stmt);
		const auto maximum = result == 100 ? column_int64(stmt,0) : 0;
		const std::string error = result == 100 ? "" : errmsg(db);
		finalize(stmt);
		if (!error.empty()) throw std::runtime_error(error);
		if (maximum == (std::numeric_limits<int64_t>::max)()) throw std::runtime_error("App catalog access index overflow");
		return maximum + 1;
	}
};
#endif

std::vector<std::string> Catalog(const Json* param) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto path = LibKernel::FileSystem::GetRealFilename("/system_data/priv/mms/app.db");
	if (!std::filesystem::is_regular_file(path)) {
		if (param) throw std::runtime_error("ShellUI app.db is not available; start with -OfflineDatabases");
		return {};
	}
	Sqlite db(path);
	const auto tables = db.Tables();
	db.Execute("BEGIN IMMEDIATE");
	try {
		for (const auto& table : tables) {
			if (table == "tbl_contentinfo") {
				db.Execute("DELETE FROM tbl_contentinfo WHERE metaDataPath='/mnt/disc/sce_sys'");
			} else if (table == "tbl_conceptmetadata" ||
			           std::regex_match(table, std::regex("tbl_(concept)?iconinfo_[0-9]{10}"))) {
				db.Execute("DELETE FROM \"" + table + "\" WHERE localConceptId LIKE 'cid:virtualdisc:%'");
			}
		}
		if (param) {
			const auto title = param->at("titleId").get<std::string>();
			const int platform = title.starts_with("CUSA") ? 1 : 0;
			const auto local_concept = std::string(concept_prefix) + title;
			// ShellCore's AppDb metadata builder supplies these defaults for games
			// without explicit links (sub_D7ECD0 in the FW7 ShellCore dump).
			const auto deeplink = param->value("deeplinkUri", std::string());
			const auto launch_uri = deeplink.empty() ? "psgm:play?id=" + title : deeplink;
			const auto hub = param->value("hubAppUri", std::string());
			const auto hub_uri = hub.empty() ? "pshome:gamehub?titleId=" + title : hub;
			const auto& locales = param->at("localizedParameters");
			const auto language = locales.value("defaultLanguage", "en-US");
			const auto label = locales.at(language).at("titleName").get<std::string>();
			const std::string metadata = "/mnt/disc/sce_sys", icon = metadata + "/icon0.png";
			// A pre-existing installed copy owns its content row; do not overwrite it.
			void* stmt = nullptr;
			db.prepare(db.db, "SELECT 1 FROM tbl_contentinfo WHERE titleId=?", -1, &stmt, nullptr);
			if (!stmt) throw std::runtime_error(db.errmsg(db.db));
			db.bind_text(stmt, 1, title.c_str(), -1, nullptr);
			const bool installed = db.step(stmt) == 100;
			db.finalize(stmt);
			if (installed) throw std::runtime_error("This title already has a content record; keeping the installed copy intact");
			db.Insert("tbl_contentinfo", {{"titleId",title},{"titleName",label},{"metaDataPath",metadata},
				{"icon0Info",icon},{"pprDeeplinkUri",launch_uri},{"contentStatus",0},{"contentLocation",1},{"installStatus",0},
				{"appinfoContentType",1},{"categoryType",param->value("applicationCategoryType",0)},
				{"hddLocation",0},{"externalHddAppStatus",0},{"uninstallable",0},{"sortPriority",100},
				{"pathInfo",0},{"pathInfo2",0},{"size",0}});
			db.Insert("tbl_conceptmetadata", {{"localConceptId",local_concept},{"pathInfo",0},{"pathInfo2",0},
				{"totalSize",0},{"sortPriority",100},{"hddLocation",0},{"externalHddAppStatus",0},
				{"mergedContentsStatus",0},{"mergedOperationCapability",0}});
			for (const auto& table : tables) {
				if (std::regex_match(table, std::regex("tbl_iconinfo_[0-9]{10}"))) {
					db.Insert(table, {{"localConceptId",local_concept},{"titleId",title},{"titleName",label},
						{"conceptId",local_concept},{"contentId",param->value("contentId", "")},{"visible",1},
						{"dispLocation",disc_display_location},{"discStatus",1},{"platform",platform},{"installStatus",0},{"entEntitlement",0},
						{"entFilTypeEnt",0},{"entHidden",0},{"lastAccessIndex",db.NextAccessIndex(table)},{"primaryTitleSort",0},
						{"hubAppUri",hub_uri},{"psNowFlag",0}});
				} else if (std::regex_match(table, std::regex("tbl_concepticoninfo_[0-9]{10}"))) {
					db.Insert(table, {{"localConceptId",local_concept},{"primaryTitleId",title},{"primaryTitleName",label},
						{"conceptName",label},{"primaryTitlePlatform",platform},{"dispLocation",disc_display_location},{"platform",platform},
						{"validFlag",1},{"installedStatus",0},{"entitlementStatus",0},{"metadataPath",metadata},
						{"icon0Info",icon},{"pic0Info",metadata+"/pic0.png"},{"lastAccessIndex",db.NextAccessIndex(table)},
						{"totalPlayedTime",0},{"totalEstimatedSize",0},{"genreFlags",0},{"deeplinkUri",launch_uri},
						{"hubAppUri",hub_uri},{"psNowFlag",0}});
				}
			}
		}
		db.Execute("COMMIT");
	} catch (...) { db.Execute("ROLLBACK"); throw; }
	std::vector<std::string> changed;
	for (const auto& table : tables) {
		if (table == "tbl_contentinfo" || table == "tbl_conceptmetadata" ||
		    std::regex_match(table, std::regex("tbl_(concept)?iconinfo_[0-9]{10}"))) {
			changed.push_back(table);
		}
	}
	return changed;
#else
	if (param) throw std::runtime_error("Virtual-disc catalog currently requires Windows SQLite");
	return {};
#endif
}

void NotifyCatalog(const std::vector<std::string>& tables) {
	// PORT NOTE (kytyps5): FileSystem::KernelGetFsSandboxRandomWord does not exist here;
	// file-local FsSandboxRandomWord() above carries the source logic verbatim.
	const std::string path = std::string("/") + FsSandboxRandomWord() + "/priv/mms/app.db";
	// DbNotify's event 45 invalidates each accessor's cached query. DiscInserted
	// updates drive state, but does not notify readers of changes to app.db.
	for (const auto& table : tables) {
		events.emplace_back(45, Json{{"DbPath",path},{"TableName",table}}.dump());
	}
}
}

namespace LibJson2 {
// PORT NOTE (kytyps5): the source tree implements PostFrameworkEvent in libs/libJson2.cpp by
// posting into the native PSM (libScePsm.sprx) framework event queue; kytyps5's libJson2 has
// no such helper. Stub preserves the deferred-delivery contract: returning false makes
// PumpEvents() leave the event queued until a real transport is supplied via its `post`
// callback, rather than inventing queue behavior. Intentionally log-free: PumpEvents runs on
// the hot guest pad-polling path, so logging here would flood the log while queued.
bool PostFrameworkEvent(int id, const std::string& json) {
	(void)id; (void)json;
	return false;
}
}

bool Insert(const std::filesystem::path& folder, std::string* error) {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	try {
		const auto path = std::filesystem::canonical(folder);
		if (!std::filesystem::is_regular_file(path/"eboot.bin") ||
		    !std::filesystem::is_regular_file(path/"sce_sys/icon0.png"))
			throw std::runtime_error("Select a game dump containing eboot.bin and sce_sys/icon0.png");
		const auto metadata_path = FindApplicationMetadata(path);
		Json param;
		SfoFields sfo;
		const bool ps4 = metadata_path.extension() == ".sfo";
		if (ps4) {
			sfo = ReadParamSfo(metadata_path);
			auto text = [&](const char* key) { return std::get<std::string>(sfo.at(key)); };
			param = {{"titleId",text("TITLE_ID")},{"contentId",text("CONTENT_ID")},
			         {"contentVersion",text("APP_VER")},
			         {"localizedParameters",{{"defaultLanguage","en-US"},{"en-US",{{"titleName",text("TITLE")}}}}}};
		} else {
			std::ifstream input(metadata_path);
			if (!input) throw std::runtime_error("Cannot open sce_sys/param.json");
			param = Json::parse(input);
		}
		const auto title = param.at("titleId").get<std::string>();
		if (!std::regex_match(title, std::regex(ps4 ? "CUSA[0-9]{5}" : "PPSA[0-9]{5}")))
			throw std::runtime_error("Invalid title ID for the selected metadata format");
		std::lock_guard lock(mutex);
		// MetadataReaderWriter uses a typed field list, unlike param.json. Keep
		// the constructor's unknown parser/mime IDs until those parsers exist.
		Json fields = Json::array();
		auto field = [&](const char* key, const Json& value) {
			fields.push_back({{"key",key},{"type",value.is_string()?2:0},
			                 {"size",value.is_string()?value.get<std::string>().size()+1:8},{"data",value}});
		};
		if (ps4) {
			for (const auto& [key,value] : sfo) {
				if (const auto* text = std::get_if<std::string>(&value)) field(key.c_str(),*text);
				else field(key.c_str(),std::get<int32_t>(value));
			}
		} else {
			for (auto [source,target] : {std::pair{"titleId","TITLE_ID"}, {"contentId","CONTENT_ID"},
			                             {"contentVersion","APP_VER"}, {"attribute3","ATTRIBUTE3"},
			                             {"attribute","ATTRIBUTE"}, {"applicationCategoryType","APPLICATION_CATEGORY_TYPE"}}) {
				if (param.contains(source) && (param[source].is_string() || param[source].is_number_integer()))
					field(target,param[source]);
			}
			const auto& locales = param.at("localizedParameters");
			const auto language = locales.value("defaultLanguage", "en-US");
			field("TITLE",locales.at(language).at("titleName"));
		}
		field("_metadata_path","/mnt/disc/sce_sys");
		const auto metadata = Json{{"parserId",UINT32_MAX},{"mimeType",UINT32_MAX},
		    {"mimeTypeFormatVersion",0},{"promoterFormatVersion",0},{"field_list",fields}}.dump();
		const auto changed = Catalog(&param); // Commit before notifying readers.
		if (metadata_snapshots.size() >= 16 && !metadata_snapshots.contains(title)) metadata_snapshots.erase(metadata_snapshots.begin());
		metadata_snapshots[title] = metadata;
		// PORT NOTE (kytyps5): FileSystem::Mount has no read-only flag here; 2-arg Mount used.
		LibKernel::FileSystem::Mount(path, "/mnt/disc");
		LibKernel::FileSystem::Mount(path, "/mnt/bd");
		if (!title_id.empty()) events.emplace_back(87, "{}");
		title_id = title;
		disc_folder = path;
		events.emplace_back(86, Json{{"titleId",title}}.dump());
		NotifyCatalog(changed);
		LOGF("VirtualDisc: mounted %s as %s at /mnt/disc (read-only)\n", Common::PathToGenericString(path).c_str(), title.c_str());
		return true;
	} catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}

bool Eject(std::string* error) {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	try {
		std::lock_guard lock(mutex);
		const auto changed = Catalog(nullptr);
		LibKernel::FileSystem::Umount("/mnt/disc"); LibKernel::FileSystem::Umount("/mnt/bd");
		if (!title_id.empty()) events.emplace_back(87, "{}");
		title_id.clear();
		disc_folder.clear();
		NotifyCatalog(changed);
		LOGF("VirtualDisc: ejected\n");
		return true;
	} catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}

int Launch(const char* title, int user_id) {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	try {
		if (!title || !std::regex_match(title, std::regex("(PPSA|CUSA)[0-9]{5}"))) return static_cast<int>(0x80940005u);
		std::filesystem::path folder;
		{
			std::lock_guard lock(mutex);
			if (title_id != title || disc_folder.empty()) return LibKernel::KERNEL_ERROR_ENOENT;
			folder = disc_folder;
		}
		// PORT NOTE (kytyps5): Loader::RuntimeLinker has no LaunchApplication in this tree
		// (13.60 FW lineage launches via Emulator/childLaunch remount, not an in-process API).
		// Stub keeps the validation above and reports ENOSYS instead of inventing a launch path.
		LOGF("VirtualDisc: launch stubbed (no RuntimeLinker::LaunchApplication in kytyps5): title=%s user=0x%x folder=%s\n",
		     title, user_id, Common::PathToGenericString(folder).c_str());
		return LibKernel::KERNEL_ERROR_ENOSYS;
	} catch (const std::exception& ex) {
		LOGF("VirtualDisc: launch failed: %s\n", ex.what());
		return LibKernel::KERNEL_ERROR_EIO;
	}
}

std::string MetadataJson(const std::string& title) {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	std::lock_guard lock(mutex);
	const auto found = metadata_snapshots.find(title);
	return found == metadata_snapshots.end() ? std::string() : found->second;
}

std::string InfoJson() {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	std::lock_guard lock(mutex);
	return Json{{"titleId",title_id}}.dump();
}

void ResetCatalog() {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	try { Catalog(nullptr); }
	catch (const std::exception& ex) { LOGF("VirtualDisc: startup cleanup failed: %s\n", ex.what()); }
}

void PumpEvents(bool (*post)(int, const std::string&)) {
	// PORT NOTE (kytyps5): no Common::HostCriticalScope in this tree; advisory scope omitted.
	// Called from guest pad polling, so native queue operations run on a guest thread.
	// The native framework subsequently delivers callbacks on its own update thread.
	static std::mutex dispatch_mutex;
	std::unique_lock dispatch(dispatch_mutex, std::try_to_lock);
	if (!dispatch.owns_lock()) return;
	for (;;) {
		std::pair<int,std::string> event;
		{
			std::lock_guard lock(mutex);
			if (events.empty()) return;
			event = events.front();
		}
		if (!(post ? post : LibJson2::PostFrameworkEvent)(event.first,event.second)) return;
		std::lock_guard lock(mutex); events.pop_front();
	}
}
}
