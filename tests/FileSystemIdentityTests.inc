// Included by KernelFileSystemTests.cpp after its fixture helpers.

using SqliteFileKey = std::pair<uint32_t, uint32_t>;

SqliteFileKey IdentityKey(const FileSystem::FileStat& stat) {
  return {stat.st_dev, stat.st_ino};
}

class IdentityDescriptor {
public:
  explicit IdentityDescriptor(const char* path, int flags = 0)
      : m_fd(FileSystem::KernelOpen(path, flags, 0600)) {
    Check(m_fd >= 3, "open identity fixture descriptor");
  }
  ~IdentityDescriptor() { Close(); }
  void Close() {
    if (m_fd >= 3) {
      Check(FileSystem::KernelClose(m_fd) == OK, "close identity fixture descriptor");
      m_fd = -1;
    }
  }
  int Get() const { return m_fd; }
  FileSystem::FileStat Stat() const {
    FileSystem::FileStat result {};
    Check(FileSystem::KernelFstat(m_fd, &result) == OK, "fstat identity fixture");
    return result;
  }
  KYTY_CLASS_NO_COPY(IdentityDescriptor);
private:
  int m_fd;
};

FileSystem::FileStat IdentityPathStat(const char* path) {
  FileSystem::FileStat result {};
  Check(FileSystem::KernelStat(path, &result) == OK, "stat identity fixture");
  return result;
}

void TestIdentityMappingWidth() {
  FileSystem::Detail::FileIdentityMap map;
  // All these would collide if either host field were truncated to 32 bits.
  const std::array<Common::FileIdentity, 5> hosts {{
      {7, 9, 0, std::nullopt},
      {7, (uint64_t {1} << 32) | 9, 0, std::nullopt},
      {(uint64_t {1} << 32) | 7, 9, 0, std::nullopt},
      {7, 9, 1, std::nullopt},
      {7, 9, 0, std::string("archive-entry")}}};
  std::set<SqliteFileKey> keys;
  for (const auto& host : hosts) {
    const auto id = map.Get(host);
    Check(id.has_value() && id->device != 0 && id->inode != 0,
          "full host key has a representable guest identity");
    Check(keys.emplace(id->device, id->inode).second,
          "high host inode/volume bits and archive entries cannot alias");
    Check(map.Get(host) == id, "identity allocation is stable on repeated lookup");
  }
  Check(map.Get(hosts[0])->device == map.Get(hosts[1])->device &&
            map.Get(hosts[0])->device != map.Get(hosts[2])->device,
        "guest devices agree exactly for the same full host device");
}

void TestFileSystemIdentity(const std::filesystem::path& root) {
  TestIdentityMappingWidth();
  const auto directory = root / "identity";
  Check(std::filesystem::create_directory(directory), "create isolated identity fixture");
  Check(std::filesystem::create_directory(directory / "folder"), "create directory identity fixture");
  FileSystem::Mount(directory, "/identity");
  FileSystem::Mount(directory, "/identity-alias");

  // Reproduce native SQLite's inode-key comparison, using empty files rather
  // than a replacement SQLite engine or fabricated DB contents.
  const std::array<const char*, 5> paths {
      "/identity/cache.db", "/identity/identity-entitlement.db", "/identity/identity-subscription.db",
      "/identity/bgft.db", "/identity/bgft.db-journal"};
  std::vector<std::unique_ptr<IdentityDescriptor>> opened;
  std::set<SqliteFileKey> keys;
  for (const auto* path : paths) {
    auto fd = std::make_unique<IdentityDescriptor>(path, 0xa02); // RDWR|CREAT|EXCL
    const auto stat = fd->Stat();
    Check(stat.st_size == 0 && stat.st_dev != 0 && stat.st_ino != 0,
          "zero-length files have valid nonzero identity");
    Check(keys.insert(IdentityKey(stat)).second,
          "different databases and journal do not share a native SQLite inode key");
    Check(IdentityKey(IdentityPathStat(path)) == IdentityKey(stat),
          "path and handle stat agree before closing newly created files");
    opened.push_back(std::move(fd));
  }
  const auto original = IdentityKey(opened[0]->Stat());
  {
    // Native SQLite checks the C++ file_type, not KernelStat's zero status.
    // Resolve the production import so this exercises its actual guest ABI.
    Loader::SymbolDatabase symbols;
    Libs::LibcInternal::InitLibcInternal_1(&symbols);
    const auto* word_symbol = symbols.Find({"JGfTMBOdUJo", "LibcInternal", 1, "LibcInternal", 1, 1,
                                           Loader::SymbolType::Func});
    Check(word_symbol != nullptr, "resolve sandbox token import");
    using GetWord = const char* (KYTY_SYSV_ABI *)();
    const std::string mms_alias = std::string("/") + reinterpret_cast<GetWord>(word_symbol->vaddr)() + "/priv/mms";
    FileSystem::Mount(directory, "/system_data/priv/mms", true);
    Libs::LibcInternal::BindSysCoreServiceSandbox();
    const auto alias_file = mms_alias + "/cache.db";
    Check(IdentityKey(IdentityPathStat(alias_file.c_str())) == original,
          "SysCore sandbox database alias retains the selected store's file identity");
    Check(FileSystem::KernelOpen(alias_file.c_str(), 2, 0) == Libs::LibKernel::KERNEL_ERROR_EROFS,
          "SysCore database alias preserves readonly backing policy");
    FileSystem::Umount(mms_alias);
    FileSystem::Umount("/system_data/priv/mms");
    const auto* symbol = symbols.Find({"Pr1S2l2SpPE", "LibcInternal", 1, "LibcInternal", 1, 1,
                                      Loader::SymbolType::Func});
    Check(symbol != nullptr, "resolve filesystem _Stat import");
    using Stat = int (KYTY_SYSV_ABI *)(const char*, uint32_t*, int*);
    const auto stat = reinterpret_cast<Stat>(symbol->vaddr);
    uint32_t permissions = 0xdeadbeef;
    int error = -1;
    *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EIO;
    Check(stat(paths[0], &permissions, &error) == 1 && error == 0 &&
              *Libs::Posix::GetErrorAddr() == 0 &&
              permissions == (IdentityPathStat(paths[0]).st_mode & 07777u),
          "SQLite record is a regular file with permission bits only");
    Check(stat("/identity/folder", nullptr, nullptr) == 2,
          "filesystem _Stat distinguishes a directory with optional outputs");
    permissions = 0xdeadbeef;
    Check(stat("/identity/missing", &permissions, &error) == -1 &&
              error == Libs::Posix::POSIX_ENOENT && permissions == 0xdeadbeef,
          "missing record reports not_found and preserves permissions");
    Check(stat(nullptr, &permissions, &error) == 0 &&
              error == Libs::Posix::POSIX_EFAULT && permissions == 0xdeadbeef,
          "invalid address reports none with EFAULT rather than not_found");
  }
  {
    IdentityDescriptor second("/identity-alias/./cache.db");
    Check(IdentityKey(second.Stat()) == original,
          "multiple opens and mount/lexical aliases share the inode key");
  }
  {
    IdentityDescriptor reopened("/identity/cache.db");
    Check(IdentityKey(reopened.Stat()) == original, "close/reopen preserves identity");
  }
  {
    IdentityDescriptor write_only("/identity/cache.db", 1);
    Check(IdentityKey(write_only.Stat()) == original,
          "write-only and read-only handles expose the same host object identity");
  }

  {
    Common::File fixture;
    Check(fixture.Create(directory / "race.db"), "create first-lookup race fixture");
    fixture.Close();
    std::atomic<bool> start {false};
    std::array<SqliteFileKey, 4> results {};
    std::array<std::thread, 4> workers;
    for (size_t i = 0; i < workers.size(); ++i) {
      workers[i] = std::thread([&, i] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        results[i] = IdentityKey(IdentityPathStat("/identity/race.db"));
      });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) { worker.join(); }
    Check(results[0] != original &&
              results[0] == IdentityKey(IdentityPathStat("/identity/race.db")) &&
              std::all_of(results.begin(), results.end(), [&](auto key) { return key == results[0]; }),
          "concurrent first metadata lookups allocate exactly one guest identity");
  }

  std::error_code error;
  std::filesystem::create_hard_link(directory / "cache.db", directory / "cache-link.db", error);
  const bool hardlink = !error;
  if (hardlink) {
    IdentityDescriptor link("/identity/cache-link.db");
    Check(IdentityKey(link.Stat()) == original &&
              IdentityKey(IdentityPathStat("/identity/cache-link.db")) == original,
          "hard links share the underlying object's identity");
  } else {
    Check(error == std::errc::operation_not_supported || error == std::errc::function_not_supported,
          "hard-link fixture only skips a filesystem lacking support");
    std::printf("filesystem_identity: host filesystem does not support hard links\n");
  }
  error.clear();
  std::filesystem::create_symlink("cache.db", directory / "cache-symlink.db", error);
  const bool symlink = !error;
  if (symlink) {
    IdentityDescriptor link("/identity/cache-symlink.db");
    Check(IdentityKey(link.Stat()) == original &&
              IdentityKey(IdentityPathStat("/identity/cache-symlink.db")) == original,
          "stat/open follow symbolic links to the same object");
  } else {
    bool privilege_missing = false;
#if defined(_WIN32)
    // ERROR_PRIVILEGE_NOT_HELD is not translated to errc by every Windows STL.
    privilege_missing = error.category() == std::system_category() && error.value() == 1314;
#endif
    Check(privilege_missing || error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
              error == std::errc::operation_not_supported || error == std::errc::function_not_supported,
          "symlink fixture only skips unsupported or unprivileged host creation");
    std::printf("filesystem_identity: symlink creation unavailable: %s\n", error.message().c_str());
  }

  // Rename outside KernelRename, so the descriptor's saved pathname remains
  // stale. A path-based fstat would now identify the replacement incorrectly.
  std::filesystem::rename(directory / "cache.db", directory / "renamed.db");
  IdentityDescriptor replacement("/identity/cache.db", 0xa02);
  Check(IdentityKey(opened[0]->Stat()) == original &&
            IdentityKey(IdentityPathStat("/identity/renamed.db")) == original &&
            IdentityKey(replacement.Stat()) != original,
        "fstat follows the open object after rename and old-path replacement");
  if (hardlink) {
    Check(std::filesystem::remove(directory / "cache-link.db"), "unlink hard-link fixture");
  }
  if (symlink) {
    Check(std::filesystem::remove(directory / "cache-symlink.db"), "unlink symbolic-link fixture");
  }
  Check(std::filesystem::remove(directory / "renamed.db"), "unlink last name while file is open");
  Check(IdentityKey(opened[0]->Stat()) == original,
        "fstat identity remains available on an unlinked open handle");

  {
    IdentityDescriptor dir("/identity/folder", 0x20000);
    const auto before = IdentityKey(dir.Stat());
    Check(before == IdentityKey(IdentityPathStat("/identity/folder/")) &&
              keys.find(before) == keys.end(),
          "directory stat/fstat agree and do not alias files");
    std::filesystem::rename(directory / "folder", directory / "renamed-folder");
    Check(std::filesystem::create_directory(directory / "folder"), "replace old directory path");
    Check(IdentityKey(dir.Stat()) == before &&
              IdentityKey(IdentityPathStat("/identity/renamed-folder")) == before &&
              IdentityKey(IdentityPathStat("/identity/folder")) != before,
          "directory descriptor retains its object across external rename/replacement");
  }

  FileSystem::FileStat untouched {};
  untouched.st_dev = 0x12345678;
  untouched.st_ino = 0x87654321;
  Check(FileSystem::KernelStat("/identity/missing", &untouched) == Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            untouched.st_dev == 0x12345678 && untouched.st_ino == 0x87654321,
        "failed path stat reports ENOENT without fabricating an identity");
  const int closed = opened.back()->Get();
  opened.back()->Close();
  Check(FileSystem::KernelFstat(closed, &untouched) == Libs::LibKernel::KERNEL_ERROR_EBADF &&
            untouched.st_dev == 0x12345678 && untouched.st_ino == 0x87654321,
        "closed descriptor stat preserves EBADF and the output buffer");

  Common::FileIdentity host {0x1234, 0x5678, 0x9abc, std::nullopt};
  const auto sentinel = host;
  Common::File invalid;
  Check(!invalid.GetIdentity(host, error) && error == std::errc::bad_file_descriptor && host == sentinel,
        "invalid Common::File identity fails without modifying output");
  Check(!Common::File::GetIdentity(directory / "missing", host, error) &&
            error == std::errc::no_such_file_or_directory && host == sentinel,
        "failed temporary metadata open has no successful zero identity");
  Common::File query;
  Check(query.OpenMetadata(directory, error) && query.GetIdentity(host, error),
        "metadata-only directory handle supports identity queries");
  query.Close();
  Check(!query.GetIdentity(host, error) && error == std::errc::bad_file_descriptor,
        "closing metadata handle invalidates identity queries");

  // Archive members remain distinct from each other and their backing file;
  // spelling aliases use the archive's entry ID, not the requested pathname.
  const auto archive = directory / "identity.zar";
  const auto renamed_archive = directory / "renamed-identity.zar";
  const auto replacement_archive = directory / "replacement-identity.zar";
  // Exceed the reader's 4 MiB block cache so later full reads must still use
  // the retained source after replacement/unlink, not only cached bytes.
  const std::vector<uint8_t> payload_a(4 * 1024 * 1024 + 17, 42);
  const std::vector<uint8_t> payload_b(4 * 1024 * 1024 + 31, 91);
  Check(ArchiveTests::CreateArchive(archive, payload_a) &&
            ArchiveTests::CreateArchive(replacement_archive, payload_b),
        "create distinguishable identity archives A and B");
  const auto member_path = Common::MakeArchivePath(archive, "assets/subdir/data.bin");
  const auto dir_path = Common::MakeArchivePath(archive, "assets");
  const auto read_payload = [&](Common::File& file, const std::vector<uint8_t>& expected) {
    std::vector<uint8_t> actual(expected.size());
    uint32_t count = 0;
    Check(file.Size() == expected.size() && file.Seek(0), "archive view has retained member size");
    file.Read(actual.data(), static_cast<uint32_t>(actual.size()), &count);
    Check(count == actual.size() && actual == expected, "archive bytes match retained backing object");
  };
  Common::FileIdentity backing_a, backing_b, member_a, directory_a;
  Check(Common::File::GetIdentity(archive, backing_a, error) &&
            Common::File::GetIdentity(replacement_archive, backing_b, error) && backing_a != backing_b,
        "A and B have distinct live host identities");
  Common::File directory_view;
  SqliteFileKey member_key, dir_key;
  FileSystem::Mount(Common::MakeArchivePath(archive), "/identity-archive");
  {
    IdentityDescriptor member("/identity-archive/ASSETS/subdir/DATA.BIN");
    IdentityDescriptor dir("/identity-archive/assets", 0x20000);
    member_key = IdentityKey(member.Stat());
    dir_key = IdentityKey(dir.Stat());
    const auto backing_key = IdentityKey(IdentityPathStat("/identity/identity.zar"));
    Check(member_key == IdentityKey(IdentityPathStat("/identity-archive/assets/subdir/data.bin")) &&
              dir_key == IdentityKey(IdentityPathStat("/identity-archive/ASSETS")) &&
              member_key != dir_key && member_key != backing_key && dir_key != backing_key &&
              member_key != IdentityKey(IdentityPathStat("/identity-archive/eboot.bin")),
          "archive entry identity preserves aliases without merging members or backing file");
    Common::File retained_member(member_path, Common::File::Mode::Read);
    Check(!retained_member.IsInvalid() && retained_member.GetIdentity(member_a, error) &&
              directory_view.OpenMetadata(dir_path, error) && directory_view.GetIdentity(directory_a, error),
          "member and metadata views retain archive A");
    auto member_backing = member_a;
    member_backing.member.reset();
    Check(member_backing == backing_a, "member identity comes from A's byte source");

    const auto check_cached_a = [&] {
      IdentityDescriptor cached("/identity-archive/assets/subdir/data.bin");
      IdentityDescriptor cached_dir("/identity-archive/assets", 0x20000);
      Check(IdentityKey(member.Stat()) == member_key && IdentityKey(cached.Stat()) == member_key &&
                IdentityKey(dir.Stat()) == dir_key && IdentityKey(cached_dir.Stat()) == dir_key &&
                IdentityKey(IdentityPathStat("/identity-archive/assets/subdir/data.bin")) == member_key &&
                IdentityKey(IdentityPathStat("/identity-archive/assets")) == dir_key,
            "cached opens and path/descriptor metadata continue identifying A");
      Common::File cached_member(member_path, Common::File::Mode::Read);
      Common::FileIdentity cached_identity;
      Check(!cached_member.IsInvalid() && cached_member.GetIdentity(cached_identity, error) &&
                cached_identity == member_a && directory_view.GetIdentity(cached_identity, error) &&
                cached_identity == directory_a,
            "cached member and retained directory identity never reopen the backing pathname");
      read_payload(cached_member, payload_a);
    };

    // Delay all payload reads until the first rename: the read must reach the
    // retained source rather than merely succeeding from a decompression cache.
    std::filesystem::rename(archive, renamed_archive);
    check_cached_a();
    std::filesystem::rename(replacement_archive, archive);
    Check(Common::File::GetIdentity(archive, host, error) && host == backing_b,
          "original backing pathname now identifies B");
    check_cached_a();
    read_payload(retained_member, payload_a);

    // Keep B alive under another name for the later fresh-open assertion.
    std::filesystem::rename(archive, replacement_archive);
    Check(std::filesystem::remove(renamed_archive), "unlink A's last name while reader remains open");
    Check(!std::filesystem::exists(archive), "cached archive pathname is absent");
    check_cached_a();
    read_payload(retained_member, payload_a);
  }
  FileSystem::Umount("/identity-archive");

  // Only the directory metadata view now owns A. It must retain the reader,
  // not just a separate host metadata handle, even with every pathname gone.
  {
    Common::File cached(member_path, Common::File::Mode::Read);
    Check(!cached.IsInvalid() && cached.GetIdentity(host, error) && host == member_a &&
              Common::File::GetIdentity(dir_path, host, error) && host == directory_a,
          "directory-only retention keeps cached member opens and path metadata usable");
    read_payload(cached, payload_a);
  }
  std::filesystem::rename(replacement_archive, archive);
  directory_view.Close(); // Release the last reader of A; the cache is weak.
  FileSystem::Mount(Common::MakeArchivePath(archive), "/identity-archive");
  {
    IdentityDescriptor fresh("/identity-archive/assets/subdir/data.bin");
    IdentityDescriptor fresh_dir("/identity-archive/assets", 0x20000);
    Common::File fresh_member(member_path, Common::File::Mode::Read);
    auto expected_b = backing_b;
    expected_b.member = member_a.member;
    Check(!fresh_member.IsInvalid() && fresh_member.GetIdentity(host, error) && host == expected_b &&
              IdentityKey(fresh.Stat()) != member_key && IdentityKey(fresh_dir.Stat()) != dir_key &&
              IdentityKey(fresh.Stat()) ==
                  IdentityKey(IdentityPathStat("/identity-archive/assets/subdir/data.bin")),
          "after releasing A a fresh reader reports B's distinct identity");
    read_payload(fresh_member, payload_b);
  }
  FileSystem::Umount("/identity-archive");

  replacement.Close();
  opened.clear();
  FileSystem::Umount("/identity-alias");
  FileSystem::Umount("/identity");
  Check(std::filesystem::remove_all(directory) != 0, "remove isolated identity fixture after closing handles");
}
