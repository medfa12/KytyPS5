#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace Libs {
void InitPlayGo_1(Loader::SymbolDatabase*);
}

// Invoked in its own virtual_memory_allocation_tests process before the normal
// test subsystem bootstrap. No firmware or guest execution is needed.
namespace {
using namespace Loader;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "system content application: %s\n", message);
		std::exit(1);
	}
}

void Write(const std::filesystem::path& file, const std::string& text) {
	std::filesystem::create_directories(file.parent_path());
	std::ofstream stream(file, std::ios::binary);
	stream << text;
	Check(stream.good(), "write metadata fixture");
}

std::string TitleId() {
	std::string title;
	Check(SystemContentParamSfoGetString("TITLE_ID", &title), "title ID present");
	return title;
}

void CheckMissingMetadata() {
	std::string text = "sentinel";
	int32_t number = -123;
	char buffer[16] = "sentinel";
	std::filesystem::path icon = "sentinel";
	uint32_t chunks = 123;
	RnpsManifest manifest;
	manifest.titleId = "sentinel";
	Check(!SystemContentParamSfoGetString("TITLE_ID", &text) && text == "sentinel", "unknown app cannot see shell strings");
	Check(!SystemContentParamSfoGetString("TITLE", buffer, sizeof(buffer)) && std::strcmp(buffer, "sentinel") == 0,
	      "unknown app cannot see shell string buffers");
	Check(!SystemContentParamSfoGetInt("APPLICATION_CATEGORY_TYPE", &number) && number == -123,
	      "unknown app cannot see shell category");
	Check(!SystemContentGetIconPath(&icon) && icon == "sentinel", "unknown app cannot see shell icon");
	Check(!SystemContentGetChunksNum(&chunks) && chunks == 123, "unknown app cannot see shell PlayGo data");
	Check(!SystemContentGetRnpsManifest(&manifest) && manifest.titleId == "sentinel", "unknown app cannot see shell RNPS data");
	const auto memory = SystemContentGetMemoryParams();
	Check(memory.flexible_size == 0 && memory.cpu_table_memory == 0 && memory.gpu_table_memory == 0 &&
	      memory.amm_table_memory == 0, "unknown app has no shell memory settings");
}

void CheckPlayGoIsolation(Program& legacy, Program& app_a, Program& module_a, Program& app_b, Program& unknown) {
	using namespace Libs::PlayGo;
	Loader::SymbolDatabase symbols;
	Libs::InitPlayGo_1(&symbols);
	const auto resolve = [&](const char* nid) {
		const auto* record = symbols.Find({nid, "PlayGo", 1, "PlayGo", 1, 0, Loader::SymbolType::Func});
		Check(record != nullptr && record->vaddr != 0, "PlayGo guest export registered");
		return record->vaddr;
	};
	// Existing PlayGoInitialize ABI: pointer, buffer size, reserved word.
	struct InitParams {
		const void* buf_addr;
		uint32_t buf_size;
		uint32_t reserved;
	};
	const auto initialize = reinterpret_cast<int (KYTY_SYSV_ABI *)(const InitParams*)>(resolve("ts6GlZOKRrE"));
	const auto terminate = reinterpret_cast<int (KYTY_SYSV_ABI *)()>(resolve("MPe0EeBGM-E"));
	const auto open = reinterpret_cast<int (KYTY_SYSV_ABI *)(int*, const void*)>(resolve("M1Gma1ocrGE"));
	using GetChunks = int (KYTY_SYSV_ABI *)(int, uint16_t*, uint32_t, uint32_t*);
	const auto get_chunks = reinterpret_cast<GetChunks>(resolve("73fF1MFU8hA"));
	const auto get_install_chunks = reinterpret_cast<GetChunks>(resolve("8-e7E989rCU"));
	const auto get_locus = reinterpret_cast<int (KYTY_SYSV_ABI *)(int, const uint16_t*, uint32_t, int8_t*)>(resolve("uWIYLFkkwqk"));
	const auto check_count = [&](uint32_t expected) {
		uint32_t count = 0xdeadbeef;
		Check(get_chunks(1, nullptr, 0, &count) == 0 && count == expected, "PlayGo total chunk count is owner-scoped");
		Check(get_install_chunks(1, nullptr, 0, &count) == 0 && count == expected, "install-chunk export uses same owner cache");
	};
	const auto check_unsupported = [&] {
		int handle = -1;
		Check(open(&handle, nullptr) == PLAYGO_ERROR_NOT_SUPPORT_PLAYGO && handle == -1,
		      "app without chunks cannot inherit another owner's cached support");
		check_count(0);
		const uint16_t chunk = 0;
		int8_t locus = -1;
		Check(get_locus(1, &chunk, 1, &locus) == PLAYGO_ERROR_BAD_CHUNK_ID && locus == -1,
		      "app without chunks cannot validate another owner's chunk IDs");
	};
	std::vector<uint8_t> storage(2 * 1024 * 1024);
	const InitParams init {storage.data(), static_cast<uint32_t>(storage.size()), 0};
	auto* previous = RuntimeLinker::GetCurrentProgram();
	RuntimeLinker::SetCurrentProgram(nullptr);
	int handle = -1;
	Check(open(&handle, nullptr) == 0 && handle == 1, "prime shell PlayGo cache through guest Open");
	check_count(9);
	Check(initialize(&init) == 0, "PlayGoHack initialization accepts existing shell metadata");
	check_count(9);

	RuntimeLinker::SetCurrentProgram(&app_a);
	Check(open(&handle, nullptr) == 0, "app A opens its own PlayGo metadata");
	Check(initialize(&init) == 0, "PlayGoHack initialization preserves app A's actual chunk count");
	check_count(3);
	std::array<uint16_t, 5> ids {99, 99, 99, 99, 99};
	uint32_t count = 0;
	Check(get_chunks(handle, ids.data(), static_cast<uint32_t>(ids.size()), &count) == 0 && count == 3 &&
	      ids == std::array<uint16_t, 5> {0, 1, 2, 99, 99}, "app A enumerates only its three chunks");
	uint16_t chunk = 2;
	int8_t locus = -1;
	Check(get_locus(handle, &chunk, 1, &locus) == 0 && locus == 3, "app A's last chunk is local");
	chunk = 3;
	Check(get_locus(handle, &chunk, 1, &locus) == PLAYGO_ERROR_BAD_CHUNK_ID, "shell-only chunk is invalid for app A");
	RuntimeLinker::SetCurrentProgram(&module_a);
	check_count(3);

	RuntimeLinker::SetCurrentProgram(&app_b);
	check_unsupported();
	Check(initialize(nullptr) == PLAYGO_ERROR_BAD_POINTER, "invalid Initialize cannot enable fallback");
	InitParams too_small = init;
	too_small.buf_size = 1;
	Check(initialize(&too_small) == PLAYGO_ERROR_BAD_SIZE, "undersized Initialize cannot enable fallback");
	check_unsupported();
	Program module_b;
	module_b.application_owner = &app_b;
	RuntimeLinker::SetCurrentProgram(&module_b);
	Check(initialize(&init) == 0, "valid app B initialization enables its PlayGoHack fallback");
	RuntimeLinker::SetCurrentProgram(&app_b);
	Check(open(&handle, nullptr) == 0, "main sees private module's initialized owner state");
	check_count(1000);
	Check(terminate() == 0, "legacy Terminate success behavior preserved");
	check_count(1000);
	RuntimeLinker::SetCurrentProgram(&unknown);
	check_unsupported();
	RuntimeLinker::SetCurrentProgram(&module_a);
	check_count(3);
	RuntimeLinker::SetCurrentProgram(&legacy);
	check_count(9);
	Check(SystemContentIsTrinityMode(), "PlayGo initialization preserves session hardware policy");

	// Concurrent callers select independent owners while module A shares A's
	// cache. Configuration is fixed before the threads start.
	std::array<Program*, 4> owners {&legacy, &app_a, &module_a, &app_b};
	std::array<uint32_t, 4> expected {9, 3, 3, 1000};
	std::barrier start(4);
	std::atomic<unsigned> failures = 0;
	std::array<std::thread, 4> readers;
	for (size_t i = 0; i < readers.size(); ++i) {
		readers[i] = std::thread([&, i] {
			RuntimeLinker::SetCurrentProgram(owners[i]);
			start.arrive_and_wait();
			for (unsigned j = 0; j < 16; ++j) {
				uint32_t actual = 0;
				if (initialize(&init) != 0 || get_chunks(1, nullptr, 0, &actual) != 0 || actual != expected[i]) {
					++failures;
				}
			}
			RuntimeLinker::SetCurrentProgram(nullptr);
		});
	}
	for (auto& reader: readers) { reader.join(); }
	Check(failures == 0, "concurrent PlayGo APIs preserve owner selection and counts");
	RuntimeLinker::SetCurrentProgram(previous);
}
} // namespace

int RunSystemContentApplicationTests() {
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	options.pro_enabled = true;
	options.playgo_hack_enabled = true;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();

	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty_system_content_apps_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto shell = root / "shell" / "sce_sys" / "param.json";
	const auto a = root / "a" / "sce_sys" / "param.json";
	const auto b = root / "b" / "sce_sys" / "param.json";
	const auto bad = root / "bad" / "sce_sys" / "param.json";
	Write(shell, R"({"titleId":"SHELL","applicationCategoryType":7,"attribute3":4194304,
	    "kernel":{"flexibleMemorySize":8192},"localizedParameters":{"en-US":{"titleName":"Shell UI"}}})");
	Write(root / "shell" / "sce_sys" / "icon0.png", "icon sentinel");
	Write(root / "shell" / "playgo-chunkdefs.xml", R"(<chunks default_chunk="0"><chunk id="8"/></chunks>)");
	Write(root / "shell" / "manifest.json", R"({"titleId":"SHELL","applicationName":"shell"})");
	Write(a, R"({"titleId":"APP_A","contentId":"CONTENT_A","appVersion":"01.23","applicationCategoryType":1,
	    "attribute3":0,"kernel":{"flexibleMemorySize":4096,"cpuPageTableSize":1024,"gpuPageTableSize":2048},
	    "amm":{"pagetableMemorySizeInMib":3},"localizedParameters":{"defaultLanguage":"fr-FR",
	    "en-US":{"titleName":"English A"},"fr-FR":{"titleName":"French A"}}})");
	Write(root / "a" / "sce_sys" / "icon0.png", "icon sentinel");
	Write(root / "a" / "playgo-chunkdefs.xml", R"(<chunks default_chunk="0"><chunk id="2"/></chunks>)");
	Write(root / "a" / "manifest.json", R"({"titleId":"APP_A","applicationName":"app a","twinTurbo":true})");
	Write(b, R"({"titleId":"APP_B","applicationCategoryType":2,"attribute3":0})");
	Write(bad, "{bad json");

	Program legacy, app_a, module_a, app_b, unknown;
	app_a.application_owner = &app_a;
	module_a.application_owner = &app_a;
	app_b.application_owner = &app_b;
	unknown.application_owner = &unknown;
	RuntimeLinker::SetCurrentProgram(nullptr);
	SystemContentLoadParamSfo(shell);
	Check(SystemContentLoadRnpsManifest(root / "shell"), "load legacy RNPS metadata");
	Check(SystemContentIsTrinityMode(), "boot metadata establishes Trinity hardware policy");
	RuntimeLinker::SetCurrentProgram(&legacy);
	Check(TitleId() == "SHELL", "null-owner programs retain singleton metadata");
	Check(!SystemContentLoadForProgram(nullptr, a) && !SystemContentLoadForProgram(&legacy, a) &&
	      !SystemContentLoadForProgram(&module_a, a), "registration requires the runtime main owner");
	Check(SystemContentLoadForProgram(&app_a, a) && SystemContentLoadForProgram(&app_b, b), "register two apps from a legacy thread");
	Check(TitleId() == "SHELL" && SystemContentIsTrinityMode(), "runtime registration preserves shell metadata and hardware policy");
	CheckPlayGoIsolation(legacy, app_a, module_a, app_b, unknown);

	RuntimeLinker::SetCurrentProgram(&unknown);
	CheckMissingMetadata();
	Check(!SystemContentLoadForProgram(&unknown, bad) && !SystemContentLoadForProgram(&unknown, root / "missing.json"),
	      "malformed and missing metadata fail registration");
	CheckMissingMetadata();
	Check(SystemContentIsTrinityMode(), "unknown runtime app still uses session hardware policy");

	RuntimeLinker::SetCurrentProgram(&module_a);
	Check(TitleId() == "APP_A", "private module resolves main owner's snapshot");
	std::string text;
	Check(SystemContentParamSfoGetString("CONTENT_ID", &text) && text == "CONTENT_A", "content ID is app-scoped");
	Check(SystemContentParamSfoGetString("TITLE", &text) && text == "French A", "existing localization selection retained");
	char version[6] {};
	Check(SystemContentParamSfoGetString("APP_VER", version, sizeof(version)) && std::strcmp(version, "01.23") == 0,
	      "bounded string accessor uses app snapshot");
	char small[2] = {'x', 'y'};
	Check(!SystemContentParamSfoGetString("APP_VER", small, sizeof(small)) && small[0] == 'x' && small[1] == 'y',
	      "short buffer failure preserves caller storage");
	int32_t category = 0;
	Check(SystemContentParamSfoGetInt("APPLICATION_CATEGORY_TYPE", &category) && category == 1, "category is app-scoped");
	auto memory = SystemContentGetMemoryParams();
	Check(memory.flexible_size == 4096 && memory.cpu_table_memory == 1024 && memory.gpu_table_memory == 2048 &&
	      memory.amm_table_memory == 3 * 1024 * 1024, "all memory parameters are app-scoped");
	std::filesystem::path icon;
	Check(SystemContentGetIconPath(&icon) && icon == root / "a" / "sce_sys" / "icon0.png", "icon is app-scoped");
	uint32_t chunks = 0;
	Check(SystemContentGetChunksNum(&chunks) && chunks == 3, "PlayGo fallback parser is reused per app");
	RnpsManifest manifest;
	Check(SystemContentGetRnpsManifest(&manifest) && manifest.titleId == "APP_A" && manifest.twinTurbo, "RNPS snapshot is app-scoped");
	Check(SystemContentIsTrinityMode(), "app's absent Trinity attribute cannot reconfigure session hardware");
	SystemContentLoadParamSfo(b);
	Check(!SystemContentLoadRnpsManifest(root / "shell") && TitleId() == "APP_A", "legacy loaders cannot mutate runtime snapshots");
	Check(!SystemContentLoadForProgram(&app_a, bad) && TitleId() == "APP_A", "failed replacement preserves previous snapshot");
	Write(bad, R"({"titleId":"BAD","kernel":{"flexibleMemorySize":"wrong-type"}})");
	Check(!SystemContentLoadForProgram(&app_a, bad) && TitleId() == "APP_A", "typed JSON failure does not escape or publish partial metadata");
	Write(bad, R"({"titleId":"BAD"})");
	Write(root / "bad" / "manifest.json", "{bad json");
	Check(!SystemContentLoadForProgram(&app_a, bad) && TitleId() == "APP_A", "malformed companion manifest cannot partially replace snapshot");

	RuntimeLinker::SetCurrentProgram(&app_b);
	Check(TitleId() == "APP_B", "second app remains independent");
	Check(!SystemContentGetIconPath(&icon) && !SystemContentGetChunksNum(&chunks) && !SystemContentGetRnpsManifest(&manifest),
	      "missing optional app metadata cannot fall back to shell");
	Check(!SystemContentParamSfoGetString("TITLE", &text) && SystemContentGetMemoryParams().flexible_size == 0,
	      "missing app fields cannot fall back to shell");

	// Exercise replacement while accessors copy from retained snapshots. Each
	// accessor is atomic independently; a series of calls is not a transaction.
	std::barrier start(5);
	std::atomic<bool> done = false;
	std::atomic<unsigned> failures = 0;
	std::array<std::thread, 4> readers;
	for (auto& reader: readers) {
		reader = std::thread([&] {
			RuntimeLinker::SetCurrentProgram(&module_a);
			start.arrive_and_wait();
			do {
				std::string title;
				if (!SystemContentParamSfoGetString("TITLE_ID", &title) || (title != "APP_A" && title != "APP_B")) {
					++failures;
				}
				RnpsManifest rnps;
				if (SystemContentGetRnpsManifest(&rnps) && rnps.titleId != "APP_A") { ++failures; }
			} while (!done.load());
			RuntimeLinker::SetCurrentProgram(nullptr);
		});
	}
	start.arrive_and_wait();
	for (unsigned i = 0; i < 32; ++i) {
		Check(SystemContentLoadForProgram(&app_a, i % 2 == 0 ? b : a), "publish replacement snapshot");
	}
	done = true;
	for (auto& reader: readers) { reader.join(); }
	Check(failures == 0, "concurrent readers retain complete snapshots across replacement");
	RuntimeLinker::SetCurrentProgram(&app_a);
	Check(TitleId() == "APP_A", "last replacement is published");
	Write(a, "{metadata changed on disk");
	Check(TitleId() == "APP_A", "published snapshot does not change with backing file");
	RuntimeLinker::SetCurrentProgram(nullptr);
	Check(TitleId() == "SHELL" && SystemContentIsTrinityMode(), "all app operations preserve boot singleton");
	Check(SystemContentGetRnpsManifest(&manifest) && manifest.titleId == "SHELL", "boot RNPS singleton survives runtime loads");
	Check(Common::GetHostLockCriticalCounter()->load() == 0, "metadata calls release suspension guards");
	std::filesystem::remove_all(root);
	std::puts("system content application tests passed");
	return 0;
}
