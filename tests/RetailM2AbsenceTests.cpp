#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/kernelIoctl.h"
#include "loader/symbolDatabase.h"
#include <array>
#include <cstdio>
#include <cstdlib>

static void Check(bool ok, const char* message) {
	if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
namespace Libs::FsSandbox::FsInternalForVsh { void InitFsInternalForVshSandbox_1(Loader::SymbolDatabase*); }
#include "DevctlDeviceTests.inc"
#include "RetailM2DeviceInfoTests.inc"

int main() {
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	Libs::LibKernel::FileSystem::Initialize();
	TestRetailM2DeviceInfo();
	TestDevctlDevice();
	const int meta = Libs::LibKernel::FileSystem::KernelOpen("/dev/metadbg", 6, 0);
	char buffer[8] {};
	Check(meta >= 3 && Libs::LibKernel::FileSystem::KernelRead(meta, buffer, sizeof(buffer)) ==
	      Libs::LibKernel::KERNEL_ERROR_EAGAIN, "metadbg regression");
	Check(Libs::LibKernel::FileSystem::KernelClose(meta) == 0, "metadbg close");
	const int sshot = Libs::LibKernel::FileSystem::KernelOpen("/dev/sshot", 2, 0);
	Check(sshot >= 3 && Libs::LibKernel::FileSystem::KernelClose(sshot) == 0, "sshot regression");
	Libs::LibKernel::FileSystem::Shutdown();
	std::puts("retail absent-M2 production-object tests passed; devctl/metadbg/sshot regressions passed");
}
