#include "loader/appLocalLibraries.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
struct Fixture {
	std::filesystem::path root = std::filesystem::temp_directory_path() /
	    ("kyty_app_local_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
	void File(const char* name) {
		const auto path = root / name;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path) << "not an ELF; path discovery never admits images";
	}
};
}

int main(int argc, char** argv) {
	try {
		if (argc >= 4 && std::string_view(argv[1]) == "--resolve") {
			Loader::AppLocalLibraries discovery(argv[2]);
			for (int i = 3; i < argc; ++i) {
				std::filesystem::path path;
				std::string error;
				Check(discovery.Resolve(argv[i], &path, &error) && !path.empty(), "requested local dependency not found/confined");
				std::printf("%s -> %s\n", argv[i], path.generic_string().c_str());
			}
			return 0;
		}
		Fixture f;
		for (const auto* name: {"eboot.bin", "libRoot.prx", "sce_module/libc.prx", "sce_modules/libExtra.sprx",
		                       "lib/libCoreADI.prx", "lib/libCoreFP.prx", "lib/unrequested.prx",
		                       "assets/libAsset.prx", "lib/nested/libNested.prx", "libkernel.prx", "lib/libkernel_sys.sprx"}) {
			f.File(name);
		}
		Loader::AppLocalLibraries discovery(f.root);
		std::vector<std::filesystem::path> paths;
		std::filesystem::path path;
		std::string error;
		Check(discovery.Adjacent(&paths, &error) && paths.size() == 3, "adjacent semantics changed or lib/assets scanned eagerly");
		for (const auto* name: {"libRoot.prx", "libc.prx", "libExtra.sprx", "libCoreADI.prx", "libCoreFP.prx"}) {
			Check(discovery.Resolve(name, &path, &error) && !path.empty(), "allowlisted directory dependency not found");
			Check(path.filename() == name, "dependency basename changed");
		}
		for (const auto* name: {"libAsset.prx", "libNested.prx", "libMissing.sprx", "libkernel.prx", "libkernel_sys.sprx"}) {
			Check(discovery.Resolve(name, &path, &error) && path.empty(), "asset/global/kernel module selected");
		}
		for (const auto* name: {"", "../libCoreADI.prx", "lib/libCoreADI.prx", "..\\libCoreADI.prx", "C:libCoreADI.prx",
		                       "/libCoreADI.prx", "file.prx:stream", ".prx", "libCoreADI.dll", "libCoreADI.prx ", "lib name.prx"}) {
			Check(!discovery.Resolve(name, &path, &error) && path.empty(), "invalid/traversing basename accepted");
		}
		Check(!discovery.Resolve(std::string("evil\0.prx", 9), &path, &error), "embedded NUL accepted");
		f.File("sce_module/libCoreFP.prx");
		Check(discovery.Resolve("libCoreFP.prx", &path, &error) && path.parent_path().filename() == "sce_module", "directory precedence changed");
		std::filesystem::create_directories(f.root / "lib/notfile.prx");
		Check(!discovery.Resolve("notfile.prx", &path, &error), "non-file dependency accepted");
		// Windows may require Developer Mode/admin for symlinks. Exercise escapes
		// when available, explicitly report rather than silently claiming coverage.
		std::error_code ec;
		std::filesystem::create_symlink(f.root / "assets/libAsset.prx", f.root / "lib/escape.prx", ec);
		if (!ec) {
			Check(!discovery.Resolve("escape.prx", &path, &error), "symlink escaped to assets");
			std::filesystem::create_directories(f.root / "aliasedapp");
			std::filesystem::create_directory_symlink(f.root / "assets", f.root / "aliasedapp/lib", ec);
			Check(!ec, "create directory symlink probe");
			Loader::AppLocalLibraries alias(f.root / "aliasedapp");
			Check(!alias.Resolve("libAsset.prx", &path, &error), "directory symlink escaped app root");
			std::filesystem::create_directory_symlink(f.root / "assets", f.root / "aliasedapp/sce_module", ec);
			Check(!ec && !alias.Adjacent(&paths, &error), "redirected adjacent directory was enumerated");
		} else { std::printf("[skip] symlink creation unavailable: %s\n", ec.message().c_str()); }
		std::printf("AppLocalLibraries: bounded directories, dependency-only lib/, precedence, invalid names and confinement passed\n");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "AppLocalLibraries: %s\n", e.what());
		return 1;
	}
}
