// Offline production registry ABI fixture. No guest execution or store migration.
#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace Libs::LibcInternal {
int KYTY_SYSV_ABI RegMgrGetInt(uint32_t, int32_t*);
int KYTY_SYSV_ABI SceRegMgrSetInt(uint32_t, int32_t);
int KYTY_SYSV_ABI SceRegMgrGetIntInitVal(uint32_t, int32_t*);
}
namespace Libs::LibKernel::Memory {
void InvalidateMemory(uint64_t, uint64_t) {} // Same host-only link fixture as directory tests.
}
static void Check(bool ok, const char* message) {
	if (!ok) { throw std::runtime_error(message); }
}
int main() try {
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	using namespace Libs::LibcInternal;
	for (uint32_t slot = 0; slot < 16; ++slot) {
		const auto key = 0x07800100 + slot * 0x10000;
		int32_t current = 123, initial = 456;
		Check(RegMgrGetInt(key, &current) == 0 && current == -1, "missing profile slot must be empty");
		Check(SceRegMgrGetIntInitVal(key, &initial) == 0 && initial == -1, "empty schema default");
		const int32_t valid_id = 0x10010000 + slot;
		Check(SceRegMgrSetInt(key, valid_id) == 0, "write valid profile ID");
		Check(SceRegMgrGetIntInitVal(key, &initial) == 0 && initial == -1, "initial value must ignore valid overlay");
		Check(RegMgrGetInt(key, &current) == 0 && current == valid_id, "initial query must preserve valid profile ID");
		Check(SceRegMgrSetInt(key, 0) == 0, "write existing zero record");
		Check(SceRegMgrGetIntInitVal(key, &initial) == 0 && initial == -1, "initial value must ignore zero overlay");
		Check(RegMgrGetInt(key, &current) == 0 && current == 0, "explicit zero must not be silently migrated");
	}
	for (const uint32_t key: {0x077f0100u, 0x07900100u, 0x07800101u, 0x07800200u}) {
		int32_t value = 123;
		Check(RegMgrGetInt(key, &value) == 0 && value == 0, "neighboring registry defaults");
	}
	std::cout << "fresh slot defaults, valid-ID preservation, immutable initial IDs, explicit-zero preservation and key bounds passed\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
