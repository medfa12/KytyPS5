// Included by KernelFileSystemTests.cpp. Run --guest-fs-context-only after
// building; these fixtures use a hidden Vulkan context, without firmware or
// emulator entry execution.
struct FsThreadProbe {
  FileSystem::GuestFsContextPtr owner;
  Libs::LibKernel::PthreadKey key = 0;
  std::atomic<int> phase {0};
  bool inherited = false;
  bool saw_change = false;
  bool destructor_owner = false;
  Libs::LibKernel::Pthread target = nullptr; // Assigned before any test APC is queued.
  std::atomic<int> apc_count {0};
  std::atomic<bool> apc_mismatch {false};
};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

static void FsOwnershipApc(void* opaque, void*, void*, PCONTEXT) {
  auto& probe = *static_cast<FsThreadProbe*>(opaque);
  // On the live target only. No allocation/logging/wait in this callback; the
  // record and probe each pin the owner while its reference count is copied.
  const auto owner = Libs::LibKernel::PthreadGetFsContext(probe.target);
  if (owner != probe.owner) { probe.apc_mismatch.store(true); }
  probe.apc_count.fetch_add(1);
}

static void CheckFsSpecialApcCapture(FsThreadProbe& probe) {
  // Same special-user-APC option as KernelRaiseException, not an alertable APC.
  using Queue = LONG (WINAPI *)(HANDLE, ULONG_PTR, decltype(&FsOwnershipApc), void*, void*, void*);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto queue = module != nullptr ? reinterpret_cast<Queue>(GetProcAddress(module, "NtQueueApcThreadEx")) : nullptr;
  Check(queue != nullptr, "special APC queue API exists on Windows fixture host");
  const auto id = Libs::LibKernel::PthreadGetHostThreadId(probe.target);
  HANDLE target = OpenThread(THREAD_SET_CONTEXT, FALSE, static_cast<DWORD>(id));
  Check(target != nullptr, "open live target for special APC ownership fixture");
  constexpr int count = 32;
  for (int i = 0; i < count; ++i) {
    Check(queue(target, ULONG_PTR {1}, FsOwnershipApc, &probe, nullptr, nullptr) == 0,
          "queue special APC during filesystem TLS rebinding");
  }
  CloseHandle(target);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (probe.apc_count.load() != count && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  Check(probe.apc_count.load() == count && !probe.apc_mismatch.load(),
        "special APC captures immutable target binding without a shared_ptr-object lock");
}
#endif

static void KYTY_SYSV_ABI FsThreadDestructor(void* opaque) {
  auto& probe = *static_cast<FsThreadProbe*>(opaque);
  std::string cwd;
  probe.destructor_owner = FileSystem::GetCurrentFsContext() == probe.owner &&
      FileSystem::GetGuestCurrentDirectory(&cwd) == OK && cwd == "/system";
}

static void* KYTY_SYSV_ABI FsThreadEntry(void* opaque) {
  auto& probe = *static_cast<FsThreadProbe*>(opaque);
  probe.inherited = FileSystem::GetCurrentFsContext() == probe.owner;
  Check(Libs::LibKernel::PthreadSetspecific(probe.key, &probe) == OK, "set filesystem destructor fixture");
  const auto self = Libs::LibKernel::PthreadSelf();
  probe.phase.store(1);
  while (probe.phase.load() != 2) {
    // Exercise interruption during TLS ownership changes. APC capture must read
    // the immutable record, not this concurrently interrupted TLS object.
    FileSystem::SetCurrentFsContext(nullptr);
    FileSystem::SetCurrentFsContext(Libs::LibKernel::PthreadGetFsContext(self));
    std::this_thread::yield();
  }
  std::string cwd;
  probe.saw_change = FileSystem::GetGuestCurrentDirectory(&cwd) == OK && cwd == "/system";
  return nullptr;
}

static void* KYTY_SYSV_ABI FsReuseEntry(void* opaque) {
  auto* expected = static_cast<FileSystem::GuestFsContext*>(opaque);
  return FileSystem::GetCurrentFsContext().get() == expected ? opaque : nullptr;
}

static void TestGuestFsPathRegressions(const std::filesystem::path& root) {
  using namespace Libs::LibKernel;
  FileSystem::ScopedGuestFsContext scope(FileSystem::CreateBootstrapFsContext());
  const auto writable = root / "writable";

  // Keep the pre-existing identity contract: stat and open follow host links.
  std::error_code error;
  std::filesystem::create_symlink("same.txt", writable / "same-link.txt", error);
  if (!error) {
    FileSystem::FileStat target {}, linked {}, opened {};
    const int fd = FileSystem::KernelOpen("/rw/same-link.txt", 0, 0);
    Check(fd >= 3 && FileSystem::KernelStat("/rw/same.txt", &target) == OK &&
              FileSystem::KernelStat("/rw/same-link.txt", &linked) == OK && FileSystem::KernelFstat(fd, &opened) == OK &&
              target.st_dev == linked.st_dev && target.st_ino == linked.st_ino &&
              target.st_dev == opened.st_dev && target.st_ino == opened.st_ino,
          "host symlink stat/open retain target identity");
    Check(FileSystem::KernelClose(fd) == OK, "close host symlink descriptor");
    std::filesystem::create_directory_symlink("Application", writable / "Application-link", error);
    Check(!error && FileSystem::ChangeDirectory("/rw/Application-link") == OK,
          "intermediate host directory symlinks remain traversable");
    Check(FileSystem::ChangeDirectory("/") == OK, "leave linked cwd");

    // Distinct files make lexical link/.. collapse observable in both contents
    // and object identity; following a link for stat alone cannot pass this.
    std::filesystem::create_directories(writable / "real/child/grandchild");
    { std::ofstream stream(writable / "target"); stream << "lexical-root"; }
    { std::ofstream stream(writable / "real/target"); stream << "physical-parent"; }
    std::filesystem::create_directory_symlink("real/child", writable / "link", error);
    Check(!error, "create directory link for parent traversal");
    FileSystem::FileStat lexical {}, physical {};
    Check(FileSystem::KernelStat("/rw/target", &lexical) == OK &&
              FileSystem::KernelStat("/rw/real/target", &physical) == OK &&
              (lexical.st_dev != physical.st_dev || lexical.st_ino != physical.st_ino),
          "link parent fixture has distinct target identities");
    const auto check_target = [](const char* path, const FileSystem::FileStat& expected, const char* payload) {
      FileSystem::FileStat by_path {}, by_fd {};
      const int descriptor = FileSystem::KernelOpen(path, 0, 0);
      char bytes[64] {};
      Check(descriptor >= 3 && FileSystem::KernelStat(path, &by_path) == OK &&
                FileSystem::KernelFstat(descriptor, &by_fd) == OK &&
                by_path.st_dev == expected.st_dev && by_path.st_ino == expected.st_ino &&
                by_fd.st_dev == expected.st_dev && by_fd.st_ino == expected.st_ino &&
                FileSystem::KernelRead(descriptor, bytes, sizeof(bytes)) == static_cast<int64_t>(std::strlen(payload)) &&
                std::strcmp(bytes, payload) == 0,
            "symlink parent stat/open/read select the expected object, not its lexical neighbor");
      Check(FileSystem::KernelClose(descriptor) == OK, "close parent traversal fixture");
    };
    check_target("/rw/link/../target", physical, "physical-parent");
    check_target("/rw/link/grandchild/../../target", physical, "physical-parent");
    Check(FileSystem::ChangeDirectory("/rw/link") == OK, "enter linked cwd for parent lookup");
    check_target("../target", physical, "physical-parent");
    Check(FileSystem::ChangeDirectory("..") == OK, "chdir follows linked object's parent");
    std::string parent_cwd;
    Check(FileSystem::GetGuestCurrentDirectory(&parent_cwd) == OK && parent_cwd == "/rw/real",
          "linked parent cwd rebases to physical guest directory");
    check_target("target", physical, "physical-parent");
    Check(FileSystem::ChangeDirectory("/") == OK, "leave physical parent cwd");

    const auto override_dir = root / "link-override";
    std::filesystem::create_directory(override_dir);
    { std::ofstream stream(override_dir / "target"); stream << "mount-override"; }
    FileSystem::Mount(override_dir, "/rw/link/overlay", true);
    FileSystem::FileStat override_identity {};
    Check(FileSystem::KernelStat("/rw/link/overlay/target", &override_identity) == OK &&
              (override_identity.st_dev != physical.st_dev || override_identity.st_ino != physical.st_ino),
          "descendant mount has a separate target object");
    check_target("/rw/link/overlay/target", override_identity, "mount-override");
    check_target("/rw/link/../target", physical, "physical-parent");
    check_target("/rw/link/overlay/../../target", physical, "physical-parent");
    Check(FileSystem::KernelOpen("/rw/link/overlay/target", 1, 0) == KERNEL_ERROR_EROFS,
          "explicit descendant mount retains readonly policy through a link spelling");
    FileSystem::Umount("/rw/link/overlay");
    FileSystem::Mount(override_dir, "/rw/real", true);
    check_target("/rw/link/../target", override_identity, "mount-override");
    Check(FileSystem::KernelOpen("/rw/link/../target", 1, 0) == KERNEL_ERROR_EROFS,
          "physical parent lookup selects its visible mount and paired policy");
    check_target("/rw/real/../target", lexical, "lexical-root");
    FileSystem::Umount("/rw/real");
    FileSystem::Mount(override_dir, "/rw/link", true);
    check_target("/rw/link/target", override_identity, "mount-override");
    check_target("/rw/link/../target", lexical, "lexical-root");
    FileSystem::Umount("/rw/link");

    // Host links already admitted outside the selected backing retain host
    // parent lookup; they must not be rewritten to the mount's lexical parent.
    const auto external = root / "external";
    std::filesystem::create_directories(external / "child");
    { std::ofstream stream(external / "target"); stream << "external-parent"; }
    std::filesystem::create_directory_symlink(external / "child", writable / "external-link", error);
    Check(!error, "create host-absolute directory link outside /rw backing");
    FileSystem::Mount(external, "/external");
    FileSystem::FileStat external_identity {};
    Check(FileSystem::KernelStat("/external/target", &external_identity) == OK, "stat external link target");
    check_target("/rw/external-link/../target", external_identity, "external-parent");
    Check(FileSystem::ChangeDirectory("/rw/external-link") == OK, "enter external linked cwd");
    check_target("../target", external_identity, "external-parent");
    Check(FileSystem::ChangeDirectory("..") == OK, "enter external linked parent cwd");
    check_target("target", external_identity, "external-parent");
    Check(FileSystem::ChangeDirectory("/") == OK, "leave external linked cwd");
    FileSystem::Umount("/external");
  } else {
    bool no_privilege = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    no_privilege = error.category() == std::system_category() && error.value() == 1314;
#endif
    Check(no_privilege || error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
              error == std::errc::operation_not_supported || error == std::errc::function_not_supported,
          "link fixture skips only unsupported/unprivileged creation");
    std::printf("guest_fs_context: symlink fixture unavailable: %s\n", error.message().c_str());
  }

  // A broad / backing must not conceal the namespace ancestors of a submount.
  std::filesystem::create_directory(root / "empty-root");
  std::filesystem::create_directory(root / "disc");
  { std::ofstream stream(root / "disc/payload"); stream << "disc"; }
  FileSystem::Mount(root / "empty-root", "/");
  FileSystem::Mount(root / "disc", "/mnt/disc", true);
  FileSystem::FileStat info {};
  Check(FileSystem::KernelStat("/mnt/disc/payload", &info) == OK &&
            FileSystem::ChangeDirectory("/mnt") == OK,
        "visible child mount makes missing broader-backing ancestor traversable");
  const int disc = FileSystem::KernelOpen("disc/payload", 0, 0);
  Check(disc >= 3 && FileSystem::KernelClose(disc) == OK &&
            FileSystem::KernelOpen("disc/payload", 1, 0) == KERNEL_ERROR_EROFS &&
            !std::filesystem::exists(root / "empty-root/mnt"),
        "child mount read/policy use namespace traversal without creating host ancestors");
  Check(FileSystem::ChangeDirectory("/") == OK, "leave mount-ancestor cwd");
  FileSystem::Umount("/mnt/disc");
  FileSystem::Umount("/");

  std::filesystem::create_directory(writable / "empty");
  for (const char* path: {"/rw/empty/.", "/rw/empty/.//", "/rw/empty/../"}) {
    Check(FileSystem::KernelRmdir(path) != OK && std::filesystem::is_directory(writable / "empty"),
          "rmdir terminal dot/dotdot cannot remove normalized target");
    Check(FileSystem::KernelUnlink(path) != OK && Libs::LibcInternal::RemoveFile(path) == -1 &&
              std::filesystem::is_directory(writable / "empty"),
          "unlink/libc remove preserve terminal-dot targets");
    Check(FileSystem::KernelRename(path, "/rw/moved") != OK &&
              FileSystem::KernelRename("/rw/same.txt", path) != OK &&
              std::filesystem::is_directory(writable / "empty") && std::filesystem::exists(writable / "same.txt"),
          "rename rejects terminal dot/dotdot in either operand");
  }
  Check(FileSystem::KernelRmdir("/rw/empty/") == OK, "ordinary trailing-slash rmdir remains supported");

  // Existing unmounted-root policy may create for O_CREAT, never for an ordinary
  // stat. A scoped /data is captured once, even if subsequently remounted.
  const auto data = root / "sandbox-data";
  std::filesystem::create_directory(data);
  FileSystem::Mount(data, "/data");
  const bool previous_direct = Libs::IsDirectTitleBoot();
  Libs::SetDirectTitleBoot(false);
  Check(FileSystem::KernelStat("/abcdef/priv/mms/app.db", &info) == KERNEL_ERROR_ENOENT &&
            !std::filesystem::exists(data / "savedata/mirror/abcdef"), "stat does not create generic sandbox backing");
  const int created = FileSystem::KernelOpen("abcdef/priv/mms/app.db", 0x601, 0770);
  Check(created >= 3 && FileSystem::KernelClose(created) == OK &&
            std::filesystem::is_regular_file(data / "savedata/mirror/abcdef/priv/mms/app.db"),
        "O_CREAT retains existing unmounted-root backing despite missing namespace parents");
  Check(FileSystem::KernelOpen("/missing-parent/../escaped.db", 0x601, 0770) == KERNEL_ERROR_ENOENT &&
            FileSystem::KernelOpen("/rw/same.txt/../escaped.db", 0x601, 0770) == KERNEL_ERROR_ENOTDIR &&
            !std::filesystem::exists(data / "savedata/mirror/escaped.db"), "fallback does not bypass dot/component errors");
  const auto captured = FileSystem::ResolveGuestPath("/decade/priv/mms/later.db");
  std::filesystem::create_directory(root / "other-data");
  FileSystem::Mount(root / "other-data", "/data");
  Check(captured.error == KERNEL_ERROR_ENOENT && captured.unmounted_fallback &&
            Libs::FsSandbox::BackingForUnmounted(captured.guest, true, captured.fallback_data_root) ==
                data / "savedata/mirror/decade/priv/mms/later.db" &&
            !std::filesystem::exists(root / "other-data/savedata"), "fallback uses captured /data selection after remount");
  FileSystem::Mount(data, "/data");

  // Isolated template and host cwd: no repository/user license or activation
  // bytes are modified. These shims must still reach existing license seeding.
  const auto previous_host_cwd = std::filesystem::current_path();
  const auto seed_cwd = root / "seed-cwd";
  std::filesystem::create_directories(seed_cwd / "_TempData");
  { std::ofstream seed(seed_cwd / "_TempData/empty.db"); seed << "license-fixture-template"; }
  std::filesystem::current_path(seed_cwd);
  Libs::SetDirectTitleBoot(true);
  const auto mirrored_user = data / "savedata/mirror/user";
  std::filesystem::create_directories(mirrored_user);
  FileSystem::Mount(mirrored_user, "/user");
  Check(!std::filesystem::exists(mirrored_user / "priv") && FileSystem::ChangeDirectory("/user") == OK,
        "first-stat seed fixture starts without license parents");
  FileSystem::FileStat first_seed_stat {}, seed_fd_stat {};
  Check(FileSystem::KernelStat("priv/license/entitlement.db", &first_seed_stat) == OK &&
            first_seed_stat.st_size == sizeof("license-fixture-template") - 1 &&
            std::filesystem::is_regular_file(mirrored_user / "priv/license/entitlement.db"),
        "first stat revalidates newly seeded parents using its captured relative cwd");
  const int first_seed_fd = FileSystem::KernelOpen("priv/license/entitlement.db", 0, 0);
  char seed_bytes[64] {};
  Check(first_seed_fd >= 3 && FileSystem::KernelFstat(first_seed_fd, &seed_fd_stat) == OK &&
            seed_fd_stat.st_dev == first_seed_stat.st_dev && seed_fd_stat.st_ino == first_seed_stat.st_ino &&
            FileSystem::KernelRead(first_seed_fd, seed_bytes, sizeof(seed_bytes)) == sizeof("license-fixture-template") - 1 &&
            std::strcmp(seed_bytes, "license-fixture-template") == 0 && FileSystem::KernelClose(first_seed_fd) == OK,
        "first seeded stat identifies the file subsequently opened and read");
  Check(FileSystem::ChangeDirectory("/") == OK, "leave first-stat seed cwd");
  void* libc_file = Libs::LibcInternal::GuestFopen("/fedcba/priv/license/entitlement4.db", "rb");
  Check(libc_file != nullptr && Libs::LibcInternal::GuestFclose(libc_file) == 0,
        "GuestFopen retains license-seed fallback with missing namespace parents");
  void* cpp_file = Libs::GuestFile::Open("/abc123/priv/license/subscription.db", 0);
  Check(cpp_file != nullptr && Libs::GuestFile::Close(cpp_file) == 0,
        "GuestFile::Open retains license-seed fallback with missing namespace parents");
  const int seeded = FileSystem::KernelOpen("/c0ffee/priv/license/entitlement.db", 0, 0);
  Check(seeded >= 3 && FileSystem::KernelClose(seeded) == OK, "kernel read open retains existing gated license seeding");
  Check(FileSystem::KernelStat("/123def/priv/mms/plain.db", &info) == KERNEL_ERROR_ENOENT &&
            !std::filesystem::exists(data / "savedata/mirror/123def"), "direct-title stat still does not create arbitrary paths");
  std::filesystem::current_path(previous_host_cwd);
  Libs::SetDirectTitleBoot(previous_direct);

  // Exercise the actual first-activation persistence route; do not manufacture
  // act.dat or its directory in the fixture before the authorized ioctl.
  const auto user = root / "activation-user";
  std::filesystem::create_directory(user);
  FileSystem::Mount(user, "/user");
  Loader::Program owner;
  owner.unique_id = 4243; // Existing mount filter only; not a PID.
  owner.authority_metadata.emplace();
  owner.authority_metadata->authority_id = 0x4800000000000010ull; // Existing NPDRM fixture authority.
  auto* previous_program = Loader::RuntimeLinker::GetCurrentProgram();
  Loader::RuntimeLinker::SetCurrentProgram(&owner);
  Check(!std::filesystem::exists(user / "license") &&
            FileSystem::GetRealFilename("/user/license/act.dat") == user / "license/act.dat",
        "owned-creation mapping preflight does not require license directory");
  const int npdrm = FileSystem::KernelOpen("/dev/npdrm", 2, 0);
  uint32_t status = 0xdeadbeef;
  int ioctl_result = -1;
  Check(npdrm >= 3 && FileSystem::TryNpDrmIoctl(npdrm, 0xc0044e05, &status, &ioctl_result) &&
            ioctl_result == OK && status == 0 && std::filesystem::is_regular_file(user / "license/act.dat") &&
            std::filesystem::file_size(user / "license/act.dat") == 0,
        "first authorized NPDRM init performs its real mkdir/create sequence");
  Check(FileSystem::KernelClose(npdrm) == OK && !std::filesystem::exists(mirrored_user / "license/act.dat"),
        "NPDRM persistence remains on selected /user, without fallback activation");
  std::ifstream preserved_seed(mirrored_user / "priv/license/entitlement.db", std::ios::binary);
  const std::string preserved_contents((std::istreambuf_iterator<char>(preserved_seed)), std::istreambuf_iterator<char>());
  Check(preserved_contents == "license-fixture-template", "NPDRM preserves the earlier seeded license");
  Loader::RuntimeLinker::SetCurrentProgram(previous_program);
}

void TestGuestFsContext(const std::filesystem::path& root) {
  using namespace Libs::LibKernel;
  const auto system = root / "system";
  const auto writable = root / "writable";
  std::filesystem::create_directories(system / "priv/resource_arbitrator_ro");
  std::filesystem::create_directories(writable / "Application");
  const auto write = [](const std::filesystem::path& path, const char* text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    Check(stream.good(), "write filesystem-context fixture");
  };
  write(system / "priv/resource_arbitrator_ro/ra-config.json", "{\"version\":3}");
  write(system / "same.txt", "readonly");
  write(writable / "same.txt", "writable");
  FileSystem::Mount(system, "/system", true);
  FileSystem::Mount(writable, "/rw");
  FileSystem::Mount(system, "/common", true);

  auto primary = FileSystem::CreateBootstrapFsContext();
  auto shellcore = FileSystem::CloneFsContext(primary);
  auto shellui = FileSystem::CloneFsContext(primary);
  FileSystem::ScopedGuestFsContext primary_scope(primary);
  Check(primary != shellcore && primary != shellui && shellcore != shellui, "independent executable launch objects");
  Check(FileSystem::ChangeDirectory("/rw") == OK, "primary initializer changes only primary cwd");
  Check(shellcore->Snapshot().cwd == "/" && shellui->Snapshot().cwd == "/", "pre-initializer cohost copies");

  {
    FileSystem::ScopedGuestFsContext scope(shellcore);
    constexpr char relative[] = "system/priv/resource_arbitrator_ro/ra-config.json";
    FileSystem::FileStat a {}, b {};
    Check(FileSystem::KernelStat(relative, &a) == OK &&
              FileSystem::KernelStat((std::string("/") + relative).c_str(), &b) == OK &&
              a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size,
          "native relative config path preserves identity patch results");
    const int fd = FileSystem::KernelOpen(relative, 0, 0);
    char contents[32] {};
    Check(fd >= 3 && FileSystem::KernelRead(fd, contents, sizeof(contents)) == 13 &&
              std::strcmp(contents, "{\"version\":3}") == 0,
          "relative config stat/open/read uses real fixture bytes");
    Check(FileSystem::KernelClose(fd) == OK, "close relative config fixture");
    Check(FileSystem::ChangeDirectory("/rw/Application/..") == OK && shellcore->Snapshot().cwd == "/rw",
          "PSM-style parent traversal validates Application before popping it");
    Check(FileSystem::ChangeDirectory("/") == OK, "virtual namespace root is a cwd");
    Check(FileSystem::KernelStat("", &a) == KERNEL_ERROR_ENOENT &&
              FileSystem::KernelStat("/system/missing/../same.txt", &a) == KERNEL_ERROR_ENOENT &&
              FileSystem::KernelStat("/system/same.txt/../same.txt", &a) == KERNEL_ERROR_ENOTDIR &&
              FileSystem::KernelStat("/system/same.txt/", &a) == KERNEL_ERROR_ENOTDIR,
          "empty path, intermediate components and trailing slash retain errors");
    Check(FileSystem::ChangeDirectory("/system/same.txt") == KERNEL_ERROR_ENOTDIR &&
              shellcore->Snapshot().cwd == "/", "failed chdir preserves cwd");
    Check(FileSystem::ChangeDirectory(nullptr) == KERNEL_ERROR_EFAULT &&
              FileSystem::ChangeDirectory(reinterpret_cast<const char*>(uintptr_t {1})) == KERNEL_ERROR_EFAULT,
          "chdir copyin handles invalid guest pointers");
    Check(FileSystem::KernelStat("//abcdef/common/same.txt", &a) == OK &&
              FileSystem::KernelOpen("//abcdef/common/same.txt", 1, 0) == KERNEL_ERROR_EROFS,
          "common alias uses identical mapping and readonly policy");
    const auto archive = root / "cwd-fixture.zar";
    const std::array<uint8_t, 4> archive_payload {1, 2, 3, 4};
    Check(ArchiveTests::CreateArchive(archive, archive_payload), "create cwd archive fixture");
    FileSystem::Mount(Common::MakeArchivePath(archive), "/archivefs");
    Check(FileSystem::ChangeDirectory("/archivefs/assets/subdir") == OK &&
              FileSystem::KernelStat("data.bin", &a) == OK && a.st_size == archive_payload.size() &&
              FileSystem::KernelOpen("data.bin", 1, 0) == KERNEL_ERROR_EROFS,
          "relative traversal uses existing archive directory and identity APIs");
    Check(FileSystem::ChangeDirectory("/") == OK, "leave archive fixture cwd");
  }
  {
    FileSystem::ScopedGuestFsContext scope(shellui);
    Check(FileSystem::ChangeDirectory("/system") == OK, "read-only directories permit chdir");
    Check(FileSystem::KernelOpen("same.txt", 1, 0) == KERNEL_ERROR_EROFS &&
              FileSystem::KernelUnlink("same.txt") == KERNEL_ERROR_EROFS &&
              FileSystem::KernelMkdir("new", 0777) == KERNEL_ERROR_EROFS &&
              FileSystem::KernelRename("same.txt", "/rw/renamed") == KERNEL_ERROR_EROFS &&
              FileSystem::KernelRmdir("priv") == KERNEL_ERROR_EROFS,
          "relative mutations enforce selected readonly mount");
    const auto frozen = FileSystem::ResolveGuestPath("same.txt");
    Check(FileSystem::ChangeDirectory("/rw") == OK, "switch cwd after operation snapshot");
    const auto next = FileSystem::ResolveGuestPath("same.txt");
    Check(frozen.error == OK && frozen.read_only && frozen.host == system / "same.txt" &&
              next.error == OK && !next.read_only && next.host == writable / "same.txt",
          "returned path and policy stay paired across cwd changes");
    Check(FileSystem::KernelMkdir("new/", 0777) == OK && FileSystem::KernelRmdir("new/") == OK,
          "mkdir allows a missing final directory with trailing slash");
    Check(shellcore->Snapshot().cwd == "/", "ShellUI cwd cannot change ShellCore");
    const auto stale = shellui->Snapshot();
    Check(FileSystem::ChangeDirectory("/system") == OK &&
              !shellui->TryChangeDirectory(stale.generation, "/rw") && shellui->Snapshot().cwd == "/system",
          "stale validated relative chdir cannot overwrite a newer cwd");

    std::thread changer([shellui] {
      FileSystem::ScopedGuestFsContext owner(shellui);
      for (int i = 0; i < 200; ++i) {
        Check(FileSystem::ChangeDirectory(i % 2 == 0 ? "/system" : "/rw") == OK, "race fixture chdir");
      }
    });
    for (int i = 0; i < 200; ++i) {
      const int fd = FileSystem::KernelOpen("same.txt", 1, 0);
      Check(fd == KERNEL_ERROR_EROFS || fd >= 3, "cwd race open selects a valid policy/backing pair");
      if (fd >= 3) {
        Check(FileSystem::KernelWrite(fd, "W", 1) == 1 && FileSystem::KernelClose(fd) == OK,
              "cwd race writes only through writable operation result");
      }
    }
    changer.join();
    std::ifstream readonly_file(system / "same.txt", std::ios::binary);
    std::string readonly_contents;
    readonly_file >> readonly_contents;
    Check(readonly_contents == "readonly", "concurrent cwd never bypasses readonly mount");
    Check(FileSystem::ChangeDirectory("/rw") == OK, "restore pthread fixture cwd");

    // A scoped / must not hide a longer global /system. At equal mount points,
    // the scoped mount still overrides the global one. No PID is assigned.
    Loader::Program program;
    program.unique_id = 4242;
    auto* previous_program = Loader::RuntimeLinker::GetCurrentProgram();
    Loader::RuntimeLinker::SetCurrentProgram(&program);
    FileSystem::MountForProgram(writable, "/", program.unique_id);
    Check(FileSystem::KernelOpen("/system/same.txt", 1, 0) == KERNEL_ERROR_EROFS,
          "longest mount precedes program-scope preference");
    FileSystem::Mount(system, "/overlay", true);
    FileSystem::MountForProgram(writable, "/overlay", program.unique_id);
    const auto overlay = FileSystem::ResolveGuestPath("/overlay/same.txt");
    Check(!overlay.read_only && overlay.host == writable / "same.txt", "scoped tie overrides same-point global mount");
    Loader::RuntimeLinker::SetCurrentProgram(previous_program);

    FsThreadProbe probe;
    probe.owner = shellui;
    Check(PthreadKeyCreate(&probe.key, FsThreadDestructor) == OK, "create filesystem destructor key");
    Pthread thread = nullptr;
    Check(PthreadCreate(&thread, nullptr, FsThreadEntry, &probe, "fs-owner") == OK, "create real guest pthread");
    while (probe.phase.load() != 1) { std::this_thread::yield(); }
    probe.target = thread;
    const auto target_context = PthreadGetFsContext(thread);
    Check(target_context == shellui, "signal capture gets live target owner");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    CheckFsSpecialApcCapture(probe);
#endif
    Check(FileSystem::ChangeDirectory("/system") == OK, "parent updates shared pthread cwd");
    probe.phase.store(2);
    Check(PthreadJoin(thread, nullptr) == OK && probe.inherited && probe.saw_change && probe.destructor_owner,
          "pthread inheritance, shared cwd and cleanup destructor ownership");
    Check(PthreadKeyDelete(probe.key) == OK, "delete filesystem destructor key");

    // Exercise the same captured-target scope used by the signal helper, even
    // after the target has joined. This is not an APC-delivery integration test.
    std::thread helper([target_context, shellcore] {
      FileSystem::ScopedGuestFsContext caller(shellcore);
      { FileSystem::ScopedGuestFsContext target(target_context);
        Check(FileSystem::GetCurrentFsContext() == target_context && target_context->Snapshot().cwd == "/system",
              "captured target survives cleanup and overrides helper caller"); }
      Check(FileSystem::GetCurrentFsContext() == shellcore, "helper scope restores previous binding");
    });
    helper.join();
    {
      FileSystem::ScopedGuestFsContext other(shellcore);
      Pthread reused = nullptr;
      Check(PthreadCreate(&reused, nullptr, FsReuseEntry, shellcore.get(), "fs-reuse") == OK,
            "create second launch pthread on reusable record");
      void* returned = nullptr;
      Check(PthreadJoin(reused, &returned) == OK && returned == shellcore.get(), "pool reuse inherits the new launch");
    }
    std::weak_ptr<FileSystem::GuestFsContext> retired;
    {
      auto ephemeral = FileSystem::CloneFsContext(shellcore);
      retired = ephemeral;
      FileSystem::ScopedGuestFsContext owner(ephemeral);
      Pthread completed = nullptr;
      Check(PthreadCreate(&completed, nullptr, FsReuseEntry, ephemeral.get(), "fs-release") == OK &&
                PthreadJoin(completed, nullptr) == OK, "join ephemeral launch worker");
    }
    // Never inspect a joined record: the pool may reuse it immediately. The
    // weak reference establishes release without violating getter provenance.
    Check(retired.expired(), "join and TLS teardown release record ownership before reuse");
  }

  Loader::SymbolDatabase symbols;
  Libs::ShellCoreFloor::LibcFloor::InitShellCoreFloorLibc_1(&symbols);
  Libs::SysKern::InitSysKern_1(&symbols);
  const auto* cwd_symbol = symbols.FindByNid("DYivN1nO-JQ", Loader::SymbolType::Func);
  const auto* cd_symbol = symbols.FindByNid("6mMQ1MSPW-Q", Loader::SymbolType::Func);
  Check(cwd_symbol != nullptr && cd_symbol != nullptr, "resolve actual cwd ABI exports");
  const auto getcwd = reinterpret_cast<char* (KYTY_SYSV_ABI *)(char*, size_t)>(cwd_symbol->vaddr);
  const auto chdir = reinterpret_cast<int (KYTY_SYSV_ABI *)(const char*)>(cd_symbol->vaddr);
  FileSystem::ScopedGuestFsContext cwd_scope(shellcore);
  std::array<char, 4> buffer {'x', 'y', 'z', '!'};
  Check(getcwd(buffer.data(), 0) == nullptr && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL && buffer[0] == 'x',
        "getcwd zero caller size is EINVAL without a write");
  Check(getcwd(buffer.data(), 1) == nullptr && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ERANGE && buffer[1] == 'y',
        "getcwd one-byte buffer is ERANGE without overflow");
  Check(getcwd(buffer.data(), 2) == buffer.data() && buffer[0] == '/' && buffer[1] == 0 && buffer[2] == 'z',
        "root exact-fit buffer preserves canary");
  Check(chdir("/system") == 0 && getcwd(buffer.data(), buffer.size()) == nullptr &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ERANGE, "non-root cwd needs its full size");
  Check(chdir("") == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENOENT,
        "POSIX chdir translates kernel errno");
  Check(getcwd(reinterpret_cast<char*>(uintptr_t {1}), 1024) == nullptr &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EFAULT, "getcwd invalid writable pointer");
  for (const size_t ignored_size: {size_t {0}, size_t {1}, SIZE_MAX}) {
    char* allocated = getcwd(nullptr, ignored_size);
    Check(allocated != nullptr && std::strcmp(allocated, "/system") == 0 &&
              Libs::GuestHeap::Owns(allocated) && Libs::GuestHeap::BlockSize(allocated) >= 1024,
          "null getcwd buffer ignores size and uses HLE guest allocator");
    Libs::GuestHeap::Free(allocated);
    Check(!Libs::GuestHeap::Owns(allocated), "matching HLE free releases getcwd allocation");
  }
  // Accumulated cwd can exceed PATH_MAX through short relative chdir calls.
  // Host configurations without long-path support report this subcase as skipped.
  auto long_host = writable;
  const std::string component(60, 'l');
  std::error_code long_error;
  for (int i = 0; i < 18; ++i) {
    long_host /= component;
    std::filesystem::create_directory(long_host, long_error);
    if (long_error) { break; }
  }
  if (!long_error) {
    Check(chdir("/rw") == 0, "start long cwd fixture");
    for (int i = 0; i < 18; ++i) { Check(chdir(component.c_str()) == 0, "extend cwd with short relative name"); }
    char* grown = getcwd(nullptr, 1);
    Check(grown != nullptr && std::strlen(grown) > 1024 && Libs::GuestHeap::BlockSize(grown) >= 2048,
          "null-buffer getcwd grows for accumulated long cwd");
    Libs::GuestHeap::Free(grown);
    Check(chdir("/") == 0, "leave long cwd fixture");
  } else {
    std::printf("guest_fs_context: long cwd subcase skipped (host: %s)\n", long_error.message().c_str());
  }
  {
    FileSystem::ScopedGuestFsContext unbound(nullptr);
    Check(FileSystem::ResolveGuestPath("system/same.txt").error == KERNEL_ERROR_EINVAL,
          "unbound worker cannot silently inherit primary cwd");
  }
  TestGuestFsPathRegressions(root);
}
