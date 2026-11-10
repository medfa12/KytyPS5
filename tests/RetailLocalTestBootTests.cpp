#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace Libs::LibcInternal {
int KYTY_SYSV_ABI RegMgrGetInt(uint32_t, int32_t*);
int KYTY_SYSV_ABI SceRegMgrSetInt(uint32_t, int32_t);
}
namespace Libs::LibKernel::Memory { void InvalidateMemory(uint64_t, uint64_t) {} }
static void Check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
int main(int argc, char** argv) try {
	if (argc != 2 && argc != 3) { return 2; }
	const bool enabled = std::string_view(argv[1]) == "on";
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.user_id = argc == 3 ? static_cast<int32_t>(std::strtoul(argv[2], nullptr, 0)) : 0x10010029;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	using namespace Libs::LibcInternal;
	int32_t value = -123;
	Check(RegMgrGetInt(0x02040000, &value) == 0 && value == (enabled ? 1 : 0), "local/test setup gate selection");
	const int32_t user = Config::GetUserId();
	const int32_t selected = user >= 0x10010000 && user <= 0x1fffffff ? user : -1;
	Check(RegMgrGetInt(0x07010000, &value) == 0 && value == (enabled ? selected : 0), "native auto-login selector must contain configured home ID");
	Check(RegMgrGetInt(0x07010001, &value) == 0 && value == 0, "selector neighbor unchanged");
	Check(SceRegMgrSetInt(0x07010000, -1) == 0 && RegMgrGetInt(0x07010000, &value) == 0 && value == -1, "guest disabling auto-login takes precedence");
	Check(SceRegMgrSetInt(0x07010000, 0x10010030) == 0 && RegMgrGetInt(0x07010000, &value) == 0 && value == 0x10010030, "guest auto-login selection takes precedence");
	Check(RegMgrGetInt(0x02040001, &value) == 0 && value == 0, "neighbor unchanged");
	Check(RegMgrGetInt(0x07800100, &value) == 0 && value == -1, "native slot defaults retained");
	Check(SceRegMgrSetInt(0x02040000, 0) == 0 && RegMgrGetInt(0x02040000, &value) == 0 && value == 0, "guest setup write takes precedence");
	Check(SceRegMgrSetInt(0x02040000, 1) == 0 && RegMgrGetInt(0x02040000, &value) == 0 && value == 1, "guest setup write/read");
	std::cout << "local/test opt-in, native auto-login ID selector, bounded keys, native defaults and guest write precedence passed\n";
	return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
