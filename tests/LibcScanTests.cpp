#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/subsystems.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace Libs::LibcInternal {
void InitLibcInternal_1(Loader::SymbolDatabase*);
}

namespace Libs::LibC {
void InitLibC_1(Loader::SymbolDatabase*);
int GetArgc();
const char** GetArgv();
}

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "libc scan: %s\n", message);
		std::exit(1);
	}
}
}

#include "LibcApplicationStateTests.inc"

int main() {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.guest_environment.emplace("KYTY_LIBC_APP_SEED_TEST", "configured");
	Config::Load(options);

	Loader::SymbolDatabase symbols;
	Libs::LibcInternal::InitLibcInternal_1(&symbols);
	const auto* record = symbols.Find({"1Pk0qZQGeWo", "LibcInternal", 1,
	                                   "LibcInternal", 1, 1, Loader::SymbolType::Func});
	Check(record != nullptr && record->vaddr != 0, "registered sscanf exists");
	using Scan = int (KYTY_SYSV_ABI *)(const char*, const char*, ...);
	const auto scan = reinterpret_cast<Scan>(record->vaddr);
	// Exact native PSM viewBox format: suppressed scansets consume separators,
	// but must not consume an output argument or skip their leading whitespace.
	for (const char* text: {"0 0 64 64", "0,0,64,64", "0 \t,0\t64 64"}) {
		std::array<float, 6> values {123, -1, -1, -1, -1, 456};
		Check(scan(text, "%f%*[%%, \t]%f%*[%%, \t]%f%*[%%, \t]%f",
		           &values[1], &values[2], &values[3], &values[4]) == 4,
		      "all four viewBox coordinates assigned");
		Check(values == std::array<float, 6> {123, 0, 0, 64, 64, 456}, "coordinates and canaries");
	}
	char text[8] = "canary";
	Check(scan("x abc", "%*c%3[a-z]", text) == 0 && std::strcmp(text, "canary") == 0,
	      "scanset does not skip an unmatched space after a conversion");
	Check(scan(" \tabc", "%2[ \t]", text) == 1 && std::strcmp(text, " \t") == 0,
	      "scanset preserves matching whitespace");
	Check(scan(" \tabc", "%3s", text) == 1 && std::strcmp(text, "abc") == 0,
	      "string conversion still skips whitespace");
	TestLibcApplicationState();
	std::puts("libc scan tests passed");
}
