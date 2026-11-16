#include "libs/localUserStorage.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace Storage = Libs::LocalUserStorage;
Storage::Layout Layout(const fs::path& root) {
	return {{root / "user/home", root / "priv/home", root / "eap/rodata/home"}, root / "priv/user/local-user-storage"};
}
int main(int argc, char** argv) {
	if (argc != 3) { std::fprintf(stderr, "usage: probe make|create-intent|destroy-intent|upgrade-intent|recover throwaway-store\n"); return 2; }
	const auto layout = Layout(fs::absolute(argv[2]));
	if (std::string(argv[1]) == "make") {
		for (const auto& root: layout.roots) { fs::create_directories(root); }
		fs::create_directories(layout.journal);
		for (auto [id, lifetime]: {std::pair {0x10010004u, Storage::Lifetime::Persistent},
		                            std::pair {0x10010005u, Storage::Lifetime::Persistent},
		                            std::pair {0x20010000u, Storage::Lifetime::Guest}}) {
			Storage::Transaction transaction;
			auto result = Storage::PrepareCreate(layout, id, lifetime, transaction);
			if (!result) { std::fprintf(stderr, "prepare failed %d\n", int(result.code)); return 1; }
			result = Storage::Commit(transaction);
			if (!result) { std::fprintf(stderr, "commit failed %d\n", int(result.code)); return 1; }
		}
		return 0;
	}
	if (std::string(argv[1]) == "create-intent" || std::string(argv[1]) == "destroy-intent" || std::string(argv[1]) == "upgrade-intent") {
		Storage::Transaction transaction;
		const auto result = std::string(argv[1]) == "destroy-intent" ? Storage::PrepareDestroy(layout, 0x10010004u, transaction) :
		                    std::string(argv[1]) == "upgrade-intent" ? Storage::PrepareUpgrade(layout, 0x10010006u, transaction) :
		                    Storage::PrepareCreate(layout, 0x10010007u, Storage::Lifetime::Persistent, transaction);
		return result ? 0 : int(result.code);
	}
	if (std::string(argv[1]) != "recover") { return 2; }
	const auto result = Storage::Recover(layout, std::array {0x10010004u, 0x10010005u, 0x10010006u});
	std::printf("Recover code=%d recovery_required=%d\n", int(result.code), int(result.recovery_required));
	return result ? 0 : int(result.code);
}
