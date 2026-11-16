void TestProfileCacheFileSystem(const std::filesystem::path& root) {
  namespace Posix = Libs::Posix;
  const auto data = root / "data";
  const auto firmware = root / "firmware";
  std::filesystem::create_directories(data / "priv/mms");
  std::filesystem::create_directories(data / "priv/home");
  std::filesystem::create_directories(data / "priv/cache/profile/0x10010004");
  std::filesystem::create_directories(firmware / "priv_ex/etc");
  std::ofstream(firmware / "priv_ex/etc/default_avatar_260.dds", std::ios::binary) << "asset-bytes";
  FileSystem::Mount(data, "/system_data");
  FileSystem::Mount(firmware, "/system_ex", true);
  Libs::LibcInternal::BindSysCoreServiceSandbox();
  const std::string sandbox = std::string("/") +
      static_cast<const char*>(Libs::LibcInternal::SceKernelGetFsSandboxRandomWord());
  const auto avatar = sandbox + "/priv_ex/etc/default_avatar_260.dds";
  const auto cache = sandbox + "/priv/cache/profile/0x10010004/avatar260.dds";

  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  Libs::ShellCoreFloor::KernelFloor::InitShellCoreFloorKernel_1(&symbols);
  const auto* symbol = symbols.FindByNid("DRGXpDDh8Ng", Loader::SymbolType::Func);
  Check(symbol != nullptr, "native Mono lstat import resolves");
  const auto lstat = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, FileSystem::FileStat*)>(symbol->vaddr);
  FileSystem::FileStat missing;
  std::memset(&missing, 0xa5, sizeof(missing));
  const auto untouched = missing;
  Check(lstat(cache.c_str(), &missing) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_ENOENT &&
            std::memcmp(&missing, &untouched, sizeof(missing)) == 0,
        "missing user avatar does not publish successful uninitialized metadata");
  Check(lstat((cache + "/absent/file").c_str(), &missing) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_ENOENT,
        "missing intermediate directory also propagates POSIX error");
  FileSystem::FileStat direct {}, alias {};
  Check(FileSystem::KernelStat("/system_ex/priv_ex/etc/default_avatar_260.dds", &direct) == OK &&
            lstat(avatar.c_str(), &alias) == 0 && alias.st_size == 11 &&
            (alias.st_mode & 0170000u) == 0100000u &&
            alias.st_dev == direct.st_dev && alias.st_ino == direct.st_ino,
        "fallback asset alias returns the actual shared file identity and size");
  char bytes[11] {};
  const int fd = FileSystem::KernelOpen(avatar.c_str(), 0, 0);
  Check(fd >= 3 && FileSystem::KernelRead(fd, bytes, sizeof(bytes)) == sizeof(bytes) &&
            std::memcmp(bytes, "asset-bytes", 11) == 0 && FileSystem::KernelClose(fd) == OK,
        "native image path reads the selected firmware asset bytes");
  Check(FileSystem::IsReadOnly(avatar) && FileSystem::KernelOpen(avatar.c_str(), 1, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EROFS,
        "firmware alias retains source mount read-only policy");
  const int cached = FileSystem::KernelOpen(cache.c_str(), 0x602, 0600);
  Check(cached >= 3 && FileSystem::KernelWrite(cached, "cache", 5) == 5 &&
            FileSystem::KernelClose(cached) == OK &&
            std::filesystem::file_size(data / "priv/cache/profile/0x10010004/avatar260.dds") == 5,
        "cache alias shares authoritative writable service storage");
  Check(lstat((sandbox + "/priv/cache/profile").c_str(), &alias) == 0 &&
            (alias.st_mode & 0170000u) == 0040000u,
        "lstat reports directories instead of unspecified attributes");
  Check(lstat(nullptr, &alias) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
        "null lstat path preserves POSIX copyin error");
}
