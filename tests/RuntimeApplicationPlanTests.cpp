#include "loader/runtimeApplicationPlan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// Deliberately link only this file and runtimeApplicationPlan.cpp. Calling the
// global SystemContent loaders, mount APIs or a runtime backend fails to link.
namespace {
namespace fs = std::filesystem;
using Error = Loader::RuntimeApplicationPlanError;
using Route = Loader::RuntimeApplicationRoute;
using Availability = Loader::RuntimeApplicationAvailability;
using Options = Loader::RuntimeApplicationPlanOptions;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RuntimeApplicationPlanTests: %s\n", message);
		std::abort();
	}
}

struct Fixture {
	fs::path root;
	Fixture() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto base = fs::temp_directory_path() / ("kyty-application-plan-" + std::to_string(stamp));
		for (unsigned i = 0; i < 100; ++i) {
			root = base.string() + "-" + std::to_string(i);
			if (fs::create_directory(root)) { root = fs::canonical(root); return; }
		}
		Check(false, "create unique fixture directory");
	}
	~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
	fs::path Write(const fs::path& relative, const std::string& text) const {
		const auto path = root / relative;
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary);
		file << text;
		file.close();
		Check(bool(file), "write fixture");
		return path;
	}
	fs::path Param(const fs::path& app, const std::string& title, uint32_t category,
	               const std::string& extra = {}) const {
		return Write(app / "sce_sys" / "param.json", "{\"titleId\":\"" + title +
		    "\",\"applicationCategoryType\":" + std::to_string(category) + extra + "}");
	}
};

Loader::RuntimeApplicationPlan Resolve(const Options& options, const std::string& title) {
	const auto result = Loader::ResolveRuntimeApplicationPlan(options, title);
	if (result.error != Error::None) { std::fprintf(stderr, "%s\n", result.message.c_str()); }
	Check(result.error == Error::None && result.plan.has_value() && result.message.empty(), "resolution succeeds");
	return *result.plan;
}

void Fails(const Options& options, const std::string& title, Error error) {
	const auto result = Loader::ResolveRuntimeApplicationPlan(options, title);
	if (result.error != error) {
		std::fprintf(stderr, "Expected error %d, got %d: %s\n", int(error), int(result.error), result.message.c_str());
	}
	Check(result.error == error && !result.plan && !result.message.empty(), "failure has clear error and no plan");
}

void TestNativeAndMountPrecedence() {
	Fixture f;
	const auto metadata = f.Param("native", "PPSA00001", 0x10000, ",\"webAppUri\":\"https://example.invalid/native\"");
	Options options;
	options.extra_mounts.emplace_back("/user/app/PPSA00001", f.root / "native");
	Fails(options, "PPSA00001", Error::MissingExecutable);
	f.Write("native/eboot.bin", "fixture");
	const auto plan = Resolve(options, "PPSA00001");
	Check(plan.route == Route::Native && plan.availability == Availability::ExecutablePresent, "URI does not select web route");
	Check(plan.executable_path == f.root / "native/eboot.bin", "native executable");
	Check(plan.app0_path == f.root / "native" && plan.metadata_path == metadata, "native paths");
	Check(!plan.host_resources_path && plan.application_category_type == 0x10000, "native category and optional host");

	options.fw_root = f.root / "fw";
	options.allow_firmware_titles = true;
	f.Param("fw/system_ex_b/app/PPSA00001", "PPSA00001", 0x10000);
	f.Write("fw/system_ex_b/app/PPSA00001/eboot.bin", "firmware");
	Check(Resolve(options, "PPSA00001").app0_path == plan.app0_path, "explicit mount precedes firmware");
	options.extra_mounts.emplace_back("/user/app/PPSA00001", f.root / "missing");
	Fails(options, "PPSA00001", Error::TitleNotFound); // Last mount wins; no firmware fallback.
	options.extra_mounts.back().second = f.root / "native";
	f.Write("native/sce_sys/param.json", "{");
	Fails(options, "PPSA00001", Error::InvalidMetadata); // Broken mounted app must not be masked.
}

void TestDescriptorWebAndOwnedPlans() {
	Fixture f;
	f.Param("descriptor", "PPSA00002", 0x10200);
	// Root manifests can also be web resources; param.json remains authoritative.
	f.Write("descriptor/manifest.json", "{\"name\":\"web resource\"}");
	Options options;
	options.extra_mounts.emplace_back("/user/app/PPSA00002", f.root / "descriptor");
	Fails(options, "PPSA00002", Error::MissingExecutable);
	options.fw_root = f.root / "fw";
	Fails(options, "PPSA00002", Error::MissingExecutable);
	const auto executable = f.Write("fw/system_ex_b/app/NPXS40001/eboot.bin", "fixture");
	// The launcher has different metadata, which the resolver must never load.
	f.Param("fw/system_ex_b/app/NPXS40001", "NPXS40001", 0x10000);
	f.Write("resources/icon.txt", "fixture");
	options.extra_mounts.emplace_back("/host_resources", f.root / "resources");
	const auto plan = Resolve(options, "PPSA00002");
	Check(plan.route == Route::Web && plan.availability == Availability::ExecutablePresent, "descriptor uses native web launcher");
	Check(plan.executable_path == executable, "web firmware executable");
	Check(plan.title_id == "PPSA00002" && plan.application_category_type == 0x10200, "web retains logical identity");
	Check(plan.app0_path == f.root / "descriptor" && plan.metadata_path == f.root / "descriptor/sce_sys/param.json",
	      "web retains descriptor root and metadata");
	Check(plan.host_resources_path == f.root / "resources", "explicit host resources preserved");
	Check(!fs::exists(plan.app0_path / "eboot.bin"), "web descriptor needs no local eboot");

	options.extra_mounts.back().second = f.root / "missing-host";
	Fails(options, "PPSA00002", Error::InvalidHostResources);
	options.extra_mounts.clear();
	options.allow_firmware_titles = true;
	const auto launcher = Resolve(options, "NPXS40001");
	Check(launcher.title_id == "NPXS40001" && launcher.route == Route::Native, "independent second resolution");
	Check(plan.title_id == "PPSA00002" && plan.route == Route::Web && plan.host_resources_path == f.root / "resources",
	      "plan owns values independently of options and subsequent metadata reads");
}

void TestFirmwareAndRnps() {
	Fixture f;
	Options options;
	options.fw_root = f.root / "fw";
	f.Param("fw/system_ex_b/app/NPXS40002", "NPXS40002", 0x10000);
	f.Write("fw/system_ex_b/app/NPXS40002/eboot.bin", "fixture");
	f.Param("fw/system_b/vsh/app/NPXS40003", "NPXS40003", 0x10000);
	f.Write("fw/system_b/vsh/app/NPXS40003/eboot.bin", "fixture");
	Fails(options, "NPXS40002", Error::TitleNotFound);
	options.allow_firmware_titles = true;
	Check(Resolve(options, "NPXS40002").route == Route::Native, "system_ex firmware discovery");
	Check(Resolve(options, "NPXS40003").route == Route::Native, "system_b firmware discovery");
	const auto manifest = f.Write("fw/system_ex_b/rnps/apps/NPXS40004/manifest.json",
	    "{\"titleId\":\"NPXS40004\",\"applicationName\":\"fixture\",\"jsBackend\":\"hermes\"}");
	Fails(options, "NPXS40004", Error::MissingBundle);
	f.Write("fw/system_ex_b/rnps/apps/NPXS40004/application.ps.bundle", "fixture, not executable bytecode");
	const auto rnps = Resolve(options, "NPXS40004");
	Check(rnps.route == Route::Rnps && rnps.availability == Availability::NativeSceneRequired, "RNPS is native-scene only");
	Check(rnps.executable_path.empty() && !rnps.application_category_type && rnps.metadata_path == manifest,
	      "RNPS classification does not invent an executable");
	options.allow_firmware_titles = false;
	options.extra_mounts.emplace_back("/user/app/NPXS40004", manifest.parent_path());
	Check(Resolve(options, "NPXS40004").route == Route::Rnps, "explicit RNPS discovery");
	f.Param("fw/system_ex_b/rnps/apps/NPXS40004", "NPXS40004", 0x10000);
	f.Write("fw/system_ex_b/rnps/apps/NPXS40004/eboot.bin", "fixture");
	Check(Resolve(options, "NPXS40004").route == Route::Rnps, "RNPS bundle is not made standalone by an eboot or param.json");
	f.Write("fw/system_ex_b/rnps/apps/NPXS40004/manifest.json", "[]");
	Fails(options, "NPXS40004", Error::InvalidMetadata);
}

void TestInvalidMetadataAndTitles() {
	Fixture f;
	f.Write("app/eboot.bin", "fixture");
	Options options;
	options.extra_mounts.emplace_back("/user/app/PPSA00005", f.root / "app");
	Fails(options, "PPSA00005", Error::MissingMetadata);
	for (const auto* text: {"", "{", "[]", "null", "{}", "{\"titleId\":12}",
	     "{\"titleId\":\"../../escape\"}", "{\"titleId\":\"PPSA00005\"}",
	     "{\"titleId\":\"PPSA00005\",\"applicationCategoryType\":\"0x10200\"}",
	     "{\"titleId\":\"PPSA00005\",\"applicationCategoryType\":-1}",
	     "{\"titleId\":\"PPSA00005\",\"applicationCategoryType\":66048.0}",
	     "{\"titleId\":\"PPSA00005\",\"applicationCategoryType\":4294967296}"}) {
		f.Write("app/sce_sys/param.json", text);
		Fails(options, "PPSA00005", Error::InvalidMetadata);
	}
	f.Param("app", "PPSA00006", 0x10000);
	Fails(options, "PPSA00005", Error::MetadataTitleMismatch);
	for (const auto* title: {"", "..", "../PPSA00005", "/PPSA00005", "PPSA00005/..", "PPSA00005\\..",
	                        "PPSA00005:stream", "ppsa00005", "PPSA000050", "C:\\app"}) {
		Fails(options, title, Error::InvalidTitleId);
	}
	Fails(options, std::string("PPSA00005\0", 10), Error::InvalidTitleId);
	Fails(options, "PPSA00007", Error::TitleNotFound);
	// Parent mounts are not an implicit license to discover arbitrary host paths.
	options.extra_mounts = {{"/user/app", f.root}};
	Fails(options, "PPSA00005", Error::TitleNotFound);
}

void TestSymlinkEscape() {
	Fixture f;
	f.Param("outside", "PPSA00008", 0x10000);
	f.Write("outside/eboot.bin", "fixture");
	fs::create_directories(f.root / "app");
	std::error_code ec;
	fs::create_directory_symlink(f.root / "outside/sce_sys", f.root / "app/sce_sys", ec);
	if (ec) {
		std::printf("RuntimeApplicationPlanTests: symlink escape checks skipped (%s)\n", ec.message().c_str());
		return;
	}
	Options options;
	options.extra_mounts = {{"/user/app/PPSA00008", f.root / "app"}};
	Fails(options, "PPSA00008", Error::PathEscape);
	fs::remove(f.root / "app/sce_sys");
	f.Param("app", "PPSA00008", 0x10000);
	fs::create_symlink(f.root / "outside/eboot.bin", f.root / "app/eboot.bin");
	Fails(options, "PPSA00008", Error::PathEscape);
	options.extra_mounts.clear();
	options.fw_root = f.root / "fw";
	options.allow_firmware_titles = true;
	fs::create_directories(f.root / "fw/system_ex_b/app");
	fs::create_directory_symlink(f.root / "outside", f.root / "fw/system_ex_b/app/PPSA00008");
	Fails(options, "PPSA00008", Error::PathEscape);
}
} // namespace

int main() {
	TestNativeAndMountPrecedence();
	TestDescriptorWebAndOwnedPlans();
	TestFirmwareAndRnps();
	TestInvalidMetadataAndTitles();
	TestSymlinkEscape();
	std::puts("RuntimeApplicationPlanTests: passed");
}
