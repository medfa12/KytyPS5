// Offline production filesystem/libc fixture. Uses only a fresh synthetic root.
// The link recipe replaces GPU invalidation below: these are host-only buffers,
// with no renderer or guest execution. Filesystem and directory code are real.
#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/guestDirectory.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace Libs::LibcInternal { void* KYTY_SYSV_ABI GuestOpendir(const char*); }
namespace Libs::LibKernel::Memory {
void InvalidateMemory(uint64_t, uint64_t) {} // Fixture-only renderer dependency.
}

static void Check(bool ok, const char* what) { if (!ok) { throw std::runtime_error(what); } }

int main(int argc, char** argv) try {
	if (argc != 2) { return 2; }
	namespace fs = std::filesystem;
	namespace FS = Libs::LibKernel::FileSystem;
	namespace Dir = Libs::GuestDirectory;
	const fs::path root = argv[1];
	Check(!fs::exists(root), "fixture root must be new");
	fs::create_directories(root / "home/10010000");
	fs::create_directories(root / "home/10010002");
	fs::create_directories(root / "empty");
	std::ofstream(root / "home/user_list.dat") << "synthetic";
	std::set<std::string> expected {"10010000", "10010002", "user_list.dat"};
	// Cross multiple getdents buffers, including a maximal-length name.
	for (int i = 0; i < 350; ++i) {
		const auto name = "entry_" + std::to_string(i);
		std::ofstream(root / "home" / name) << i;
		expected.insert(name);
	}
	const std::string long_name(255, 'a');
	const fs::path long_path = std::wstring(L"\\\\?\\") + fs::absolute(root / "home" / long_name).wstring();
	std::ofstream(long_path) << "x";
	Check(fs::exists(long_path), "host did not create long-name fixture");
	expected.insert(long_name);
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	FS::Initialize();
	FS::Mount(root, "/fixture", true);
	FS::Mount(root / "home", "/system_data/priv/home", true);
	auto open = Libs::LibcInternal::GuestOpendir;
	auto* first = open("/system_data/priv/home");
	auto* second = open("/system_data/priv/home");
	Check(first && second && first != second, "real home directory must open independently");
	std::set<std::string> observed;
	unsigned maximum_id = 0;
	for (void* raw; (raw = Dir::Read(first)) != nullptr;) {
		const auto* entry = static_cast<const unsigned char*>(raw);
		const std::string name(reinterpret_cast<const char*>(entry + 8));
		Check(name.size() == entry[7], "dirent name length ABI");
		if (name == "." || name == "..") { continue; }
		Check(observed.insert(name).second, "duplicate entry");
		if (name == "10010000" || name == "10010002") {
			Check(entry[6] == 4, "home entry must be directory");
			maximum_id = std::max(maximum_id, static_cast<unsigned>(std::stoul(name, nullptr, 16)));
		} else { Check(entry[6] == 8, "file type ABI"); }
	}
	if (observed != expected) {
		for (const auto& name: expected) { if (!observed.contains(name)) { std::cerr << "missing fixture entry: " << name << '\n'; } }
		for (const auto& name: observed) { if (!expected.contains(name)) { std::cerr << "extra fixture entry: " << name << '\n'; } }
	}
	Check(observed == expected && maximum_id == 0x10010002, "real entries and native-style high-water scan");
	*Libs::Posix::GetErrorAddr() = 123;
	Check(Dir::Read(first) == nullptr && *Libs::Posix::GetErrorAddr() == 123, "EOF preserves errno");
	Check(Dir::Read(second) != nullptr, "independent cursor");
	Check(Dir::Close(first) == 0 && Dir::Close(second) == 0, "close streams");
	Check(Dir::Close(first) == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF, "double close");
	Check(Dir::Read(reinterpret_cast<void*>(1)) == nullptr && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF, "invalid stream");
	Check(!open("/fixture/missing") && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENOENT, "missing path");
	Check(!open("/fixture/home/user_list.dat") && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENOTDIR, "regular file");
	Check(!open(nullptr) && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EFAULT, "invalid path");
	auto* empty = open("/fixture/empty");
	Check(empty != nullptr, "empty directory opens");
	for (void* raw; (raw = Dir::Read(empty)) != nullptr;) {
		const std::string name(static_cast<const char*>(raw) + 8);
		Check(name == "." || name == "..", "empty directory contains no fabricated profile");
	}
	Check(Dir::Close(empty) == 0, "empty close");
	Check(FS::KernelOpen("/fixture/home/user_list.dat", 1, 0) == Libs::LibKernel::KERNEL_ERROR_EROFS, "read-only mount retained");
	FS::Shutdown();
	std::cout << "directory ABI, block traversal, independent cursors, EOF, errors and native-style ID scan passed\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
