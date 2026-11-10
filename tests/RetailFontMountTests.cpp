// Standalone offline fixture; link against the candidate production objects.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) try {
  if (argc != 8) { return 2; }
  const size_t declared_count = std::stoull(argv[3]);
  const size_t icon_count = std::stoull(argv[5]);
  const size_t font2_count = std::stoull(argv[7]);
  if (declared_count == 0 || icon_count == 0 || font2_count == 0) { return 2; }
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  namespace FS = Libs::LibKernel::FileSystem;
  namespace fs = std::filesystem;
  const fs::path root = argv[1], fonts = argv[2];
  const auto old = root / "old/preinst";
  fs::create_directories(old / "common/font");
  FS::Initialize();
  FS::Mount(old, "/preinst", true);
  // Match the launcher's common-resource parent; sandbox alias traversal
  // validates /<hash>/common before selecting the font child mount.
  FS::Mount(old / "common", "/common", true);
  FS::Mount(old / "common/font", "/common/font", true);
  FS::Mount(old / "common/font", "/kyty/common/font", true);
  const std::string sandbox = "/3a058c110031659f3d186ba8976aaa804f5ff4e63ece5038e7f55e3383f852e/common/font";
  auto check = [&](const std::string& guest, const fs::path& expected, int expected_error = 0) {
    const auto result = FS::ResolveGuestPath(guest.c_str());
    if (result.error != expected_error || !result.read_only || fs::weakly_canonical(result.host) != fs::weakly_canonical(expected)) {
      throw std::runtime_error("incorrect mount selection: " + guest + " error=" + std::to_string(result.error) + " host=" + result.host.string());
    }
    return result.host;
  };
  // Old mapping control: the missing font resolves to the old backing.
  check(sandbox + "/SST-Bold.otf", old / "common/font/SST-Bold.otf");
  if (fs::exists(old / "common/font/SST-Bold.otf")) { return 3; }
  if (FS::KernelOpen((sandbox + "/SST-Bold.otf").c_str(), 0, 0) != Libs::LibKernel::KERNEL_ERROR_ENOENT) { return 6; }
  // Same-point mounts replace previous global mappings; the preinst child
  // must win over its parent, including when the parent is registered later.
  FS::Mount(fonts, "/common/font", true);
  FS::Mount(fonts, "/kyty/common/font", true);
  FS::Mount(fonts, "/preinst/common/font", true);
  FS::Mount(old, "/preinst", true);
  const fs::path icons = argv[4];
  const fs::path font2 = argv[6];
  fs::create_directories(root / "old/common_ex");
  FS::Mount(root / "old/common_ex", "/common_ex", true);
  FS::Mount(icons, "/common_ex/font2", true);
  const auto icon_sandbox = sandbox.substr(0, sandbox.find("/common/font")) + "/common_ex/font2";
  const std::string font2_sandbox = "/9381b383471c982d252a52143c66c233a732263f35a4dd1aa7a3d3a8f891649/common/font2";
  // Reproduce font2's prior launch topology: /system/common/font2 exists,
  // but it does not provide the native /<sandbox>/common/font2 namespace.
  FS::Mount(font2, "/system/common/font2", true);
  if (FS::KernelOpen((font2_sandbox + "/DFHEI5-SONY2022.ttf").c_str(), 0, 0) != Libs::LibKernel::KERNEL_ERROR_ENOENT) { return 11; }
  if (FS::KernelOpen("/common/font2/DFHEI5-SONY2022.ttf", 0, 0) != Libs::LibKernel::KERNEL_ERROR_ENOENT) { return 12; }
  FS::Mount(font2, "/common/font2", true);
  const std::vector<std::pair<std::string, fs::path>> cases = {
      {"/common/font", fonts}, {"/kyty/common/font", fonts}, {"/preinst/common/font", fonts}, {sandbox, fonts},
      {"/common_ex/font2", icons}, {icon_sandbox, icons},
      {"/common/font2", font2}, {font2_sandbox, font2}, {"/system/common/font2", font2}};
  for (const auto& [guest, directory] : cases) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
      const auto name = entry.path().filename().string();
      if (!entry.is_regular_file()) { return 4; }
      const auto resolved = check(guest + "/" + name, directory / name);
      const int fd = FS::KernelOpen((guest + "/" + name).c_str(), 0, 0);
      if (fd < 0) { throw std::runtime_error("guest open failed: " + guest + "/" + name); }
      std::ifstream source(entry.path(), std::ios::binary);
      // KernelRead invalidates renderer resources. This offline resolver
      // fixture instead reads the selected backing after a real KernelOpen.
      std::ifstream backing(resolved, std::ios::binary);
      if (!source || !backing) { return 7; }
      std::array<char, 65536> expected {}, actual {};
      while (source) {
        source.read(expected.data(), expected.size());
        const auto size = source.gcount();
        backing.read(actual.data(), size);
        if (backing.gcount() != size ||
            !std::equal(expected.begin(), expected.begin() + size, actual.begin())) {
          throw std::runtime_error("guest read differs from source: " + name);
        }
      }
      if (backing.peek() != std::char_traits<char>::eof() || FS::KernelClose(fd) != 0) { return 7; }
      if (FS::KernelOpen((guest + "/" + name).c_str(), 1, 0) != Libs::LibKernel::KERNEL_ERROR_EROFS) { return 8; }
      ++count;
    }
    if (count != (directory == fonts ? declared_count : directory == icons ? icon_count : font2_count)) { return 9; }
    std::cout << "verified readonly mapping: " << guest << '\n';
  }
  check("/preinst/common/unrelated.bin", old / "common/unrelated.bin");
  check("/preinst/common/font/Unknown.otf", fonts / "Unknown.otf");
  if (fs::exists(fonts / "Unknown.otf")) { return 5; }
  if (FS::KernelOpen("/preinst/common/font/Unknown.otf", 0, 0) != Libs::LibKernel::KERNEL_ERROR_ENOENT) { return 10; }
  FS::Shutdown();
  std::cout << "retail mixed font mounts: old-mapping control, replacement, longest child, sandbox alias, retail TTFs and missing-file preservation passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << std::endl;
  return 1;
}
