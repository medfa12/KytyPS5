// Lifecycle-only device regressions; no guest execution, renderer, or window.
namespace SshotTests {
using namespace Libs::LibKernel;
namespace FS = Libs::LibKernel::FileSystem;
namespace Posix = Libs::Posix;
constexpr uint32_t Setup = 0xc0709d01u, Teardown = 0xc0709d02u, Capture = 0xc4e09d03u;
using Block = std::array<uint8_t, 112>;

template<class T> void Put(Block& block, size_t offset, T value) {
  std::memcpy(block.data() + offset, &value, sizeof(value));
}
Block Configuration(void* pool) {
  Block block {};
  Put(block, 8, reinterpret_cast<uint64_t>(pool));
  Put(block, 0x18, uint32_t {0xa200000});
  Put(block, 0x1c, uint32_t {0x200000});
  Put(block, 0x20, uint32_t {0x60});
  return block;
}
int Ioctl(int fd, uint32_t command, void* pointer) {
  int result = 0x1234;
  Check(FS::TrySshotIoctl(fd, command, pointer, &result), "sshot route consumes request");
  return result;
}
int Open() {
  const int fd = FS::KernelOpen("/dev/sshot", 2, 0);
  Check(fd >= 3, "audited RDWR creates special descriptor");
  return fd;
}
void Close(int fd) { Check(FS::KernelClose(fd) == 0, "sshot close"); }

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
struct Pool {
  void* base = VirtualAlloc(nullptr, 0xa200000, MEM_RESERVE, PAGE_NOACCESS);
  Pool() {
    Check(base != nullptr && VirtualAlloc(base, 0x200000, MEM_COMMIT, PAGE_READWRITE) == base,
          "reserve 162 MiB but map only initial 2 MiB like compositor startup");
    std::memset(base, 0x6b, 0x200000);
  }
  void Unchanged() {
    const auto* bytes = static_cast<const uint8_t*>(base);
    Check(std::all_of(bytes, bytes + 0x200000, [](auto b) { return b == 0x6b; }),
          "borrowed mapped pool never modified");
    MEMORY_BASIC_INFORMATION info {};
    Check(VirtualQuery(bytes + 0x200000, &info, sizeof(info)) != 0 && info.State == MEM_RESERVE,
          "unmapped pool tail never committed/touched");
  }
  ~Pool() { Check(VirtualFree(base, 0, MEM_RELEASE) != 0, "caller still owns pool release"); }
};

thread_local void* protected_block = nullptr;
void MakeReadOnly() {
  DWORD old = 0;
  Check(VirtualProtect(protected_block, 4096, PAGE_READONLY, &old) != 0,
        "change argument permission between copyin/copyout");
}
thread_local int victim = -1, replacement = -1;
void Reuse() {
  FS::TestSetSshotIoctlRetainedCallback(nullptr);
  Close(victim);
  replacement = Open();
  Check(replacement == victim, "deterministic fd number reuse");
}
std::atomic<bool> ioctl_locked {false}, release_ioctl {false}, close_started {false}, close_finished {false};
void HoldIoctl() {
  ioctl_locked = true;
  WaitForMutexTest([] { return release_ioctl.load(); });
}
#endif
} // namespace SshotTests

void TestSshotDevice(const std::filesystem::path& root) {
  using namespace SshotTests;
  for (int flags : {3, -1, 0, 1, 0x20002, 0x602, 0xa, 0x6, 0x100002, 0x40000002}) {
    Check(FS::KernelOpen("/dev/sshot", flags, 0) < 0,
          "unsupported/invalid open combinations return ordinary errors, never abort");
  }
  Check(FS::KernelOpen("/dev/sshot", 3, 0) == KERNEL_ERROR_EINVAL &&
            FS::KernelOpen("/dev/sshot", 0x20002, 0) == KERNEL_ERROR_ENOTDIR &&
            FS::KernelOpen("/dev/sshot", 0, 0) == KERNEL_ERROR_EACCES,
        "explicit invalid access/directory/read-only policies");
  Check(FS::KernelOpen("/dev/sshot/", 2, 0) == KERNEL_ERROR_ENOTDIR &&
            FS::KernelOpen("/dev/sshot/child", 2, 0) == KERNEL_ERROR_ENOTDIR,
        "sshot is a namespace character device, not a directory or backing path");
  Check(FS::KernelUnlink("/dev/sshot") == KERNEL_ERROR_EPERM &&
            FS::KernelRename("/dev/sshot", "/dev/sshot-other") == KERNEL_ERROR_EPERM &&
            FS::KernelMkdir("/dev/sshot", 0700) == KERNEL_ERROR_EEXIST &&
            FS::KernelRmdir("/dev/sshot") == KERNEL_ERROR_ENOTDIR,
        "device namespace cannot mutate a coincidentally mounted host file");
  Check(Ioctl(0, Setup, nullptr) == KERNEL_ERROR_ENOTTY &&
            Ioctl(1, Capture, nullptr) == KERNEL_ERROR_ENOTTY &&
            Ioctl(-1, Setup, nullptr) == KERNEL_ERROR_EBADF,
        "standard streams are wrong devices, negative fds are invalid");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Pool pool;
  auto block = Configuration(pool.base);
  const auto initial = block;
  const int a = Open(), b = Open();
  Check(a != b && Ioctl(a, Teardown, block.data()) == KERNEL_ERROR_EINVAL,
        "fresh descriptors are independent and not configured");
  Check(Ioctl(a, Setup, nullptr) == KERNEL_ERROR_EFAULT &&
            Ioctl(a, Setup, reinterpret_cast<void*>(1)) == KERNEL_ERROR_EFAULT &&
            Ioctl(a, Setup, reinterpret_cast<void*>(UINTPTR_MAX - 16)) == KERNEL_ERROR_EFAULT,
        "null/unmapped/wrapping request pointers fail safely");

  // Exercise every opaque input byte, not just the conveniently logged fields.
  for (size_t i = 0; i < block.size(); ++i) {
    if ((i >= 8 && i < 16) || (i >= 0x18 && i < 0x24)) { continue; }
    auto opaque = initial;
    opaque[i] = 0xa5;
    const auto before = opaque;
    Check(Ioctl(a, Setup, opaque.data()) == KERNEL_ERROR_EOPNOTSUPP && opaque == before,
          "unsupported opaque/status nonzero byte rejected without clearing input");
  }
  for (size_t offset : {size_t {0x18}, size_t {0x1c}, size_t {0x20}}) {
    for (uint32_t value : {0u, 1u, UINT32_MAX}) {
      auto bad = initial;
      Put(bad, offset, value);
      const auto before = bad;
      Check(Ioctl(a, Setup, bad.data()) == KERNEL_ERROR_EINVAL && bad == before,
            "only exact audited pool/auxiliary/option combination accepted");
    }
  }
  for (uint64_t base : {uint64_t {0}, UINT64_MAX, uint64_t {INT64_MAX - 16}}) {
    auto bad = initial;
    Put(bad, 8, base);
    Check(Ioctl(a, Setup, bad.data()) == KERNEL_ERROR_EINVAL, "pool bounds use checked arithmetic");
  }
  auto unmapped = initial;
  Put(unmapped, 8, uint64_t {1});
  Check(Ioctl(a, Setup, unmapped.data()) == KERNEL_ERROR_EFAULT, "pool base must be readable");

  auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(pages != nullptr, "allocate argument permission fixture");
  DWORD old = 0;
  Check(VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &old) != 0, "argument guard page");
  std::memcpy(pages + 4096 - 56, initial.data(), 56);
  Check(Ioctl(a, Setup, pages + 4096 - 56) == KERNEL_ERROR_EFAULT,
        "entire 112-byte copyin must be accessible");
  std::memcpy(pages, initial.data(), initial.size());
  protected_block = pages;
  FS::TestSetSshotIoctlLockedCallback(MakeReadOnly);
  Check(Ioctl(a, Setup, pages) == KERNEL_ERROR_EFAULT &&
            std::memcmp(pages, initial.data(), initial.size()) == 0,
        "copyout respects RO permission after copyin and never fabricates success");
  FS::TestSetSshotIoctlLockedCallback(nullptr);
  Check(Ioctl(a, Setup, block.data()) == 0 && block == initial,
        "failed setup copyout did not commit; real setup preserves exact block");
  Check(Ioctl(a, Setup, block.data()) == KERNEL_ERROR_EBUSY && block == initial,
        "double setup explicitly busy, not silent reconfiguration");
  Check(Ioctl(b, Setup, block.data()) == 0, "second fd independently registers same borrowed pool");
  Check(Ioctl(a, Teardown, nullptr) == KERNEL_ERROR_EFAULT &&
            Ioctl(a, Teardown, pages) == KERNEL_ERROR_EFAULT &&
            Ioctl(a, Setup, block.data()) == KERNEL_ERROR_EBUSY,
        "failed teardown copyout retains configuration transactionally");
  auto wrong = block;
  wrong[0x30] = 1;
  const auto wrong_before = wrong;
  Check(Ioctl(a, Teardown, wrong.data()) == KERNEL_ERROR_EINVAL && wrong == wrong_before,
        "teardown must match fd registration and preserves unsupported bytes");
  Put(block, 0, uint32_t {0x80c30001});
  Check(Ioctl(a, Teardown, block.data()) == 0 && block == initial,
        "successful release reports status zero without changing opaque inputs");
  Check(Ioctl(a, Teardown, block.data()) == KERNEL_ERROR_EINVAL &&
            Ioctl(b, Setup, block.data()) == KERNEL_ERROR_EBUSY,
        "double teardown rejected and other descriptor remains configured");
  Check(Ioctl(a, Setup, block.data()) == 0, "released fd can configure again");
  struct GuardedBlock { std::array<uint8_t, 16> before; Block value; std::array<uint8_t, 16> after; } guarded;
  guarded.before.fill(0x5a);
  guarded.after.fill(0xc3);
  guarded.value = initial;
  const int bounded = Open();
  Check(Ioctl(bounded, Setup, guarded.value.data()) == 0 && guarded.value == initial &&
            Ioctl(bounded, Teardown, guarded.value.data()) == 0 && guarded.value == initial &&
            std::all_of(guarded.before.begin(), guarded.before.end(), [](auto v) { return v == 0x5a; }) &&
            std::all_of(guarded.after.begin(), guarded.after.end(), [](auto v) { return v == 0xc3; }),
        "setup and teardown copy out exactly 112 bytes, preserving adjacent canaries");
  Close(bounded);

  std::array<uint8_t, 1248 + 32> capture;
  capture.fill(0xa5);
  const auto capture_before = capture;
  Check(Ioctl(a, Capture, capture.data() + 16) == KERNEL_ERROR_EOPNOTSUPP && capture == capture_before &&
            Ioctl(a, Capture, nullptr) == KERNEL_ERROR_EOPNOTSUPP,
        "capture explicitly unsupported: no status/descriptor/pixel writes or argument reads");
  for (uint32_t request : {0xc0709d04u, 0xc06f9d01u, 0x40709d01u, 0u, 0xc0704e01u}) {
    Check(Ioctl(a, request, reinterpret_cast<void*>(1)) == KERNEL_ERROR_ENOTTY,
          "unknown commands/size/direction/groups cannot reach generic success or NPDRM abort");
  }

  char byte = 'x';
  FS::KernelIovec iov {&byte, 1};
  Check(FS::KernelRead(a, &byte, 1) == KERNEL_ERROR_EOPNOTSUPP &&
            FS::KernelWrite(a, &byte, 1) == KERNEL_ERROR_EOPNOTSUPP &&
            FS::KernelPread(a, &byte, 1, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelPwrite(a, &byte, 1, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelPreadv(a, &iov, 1, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelPwritev(a, &iov, 1, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelWritev(a, &iov, 1) == KERNEL_ERROR_EOPNOTSUPP &&
            FS::KernelWritev(a, reinterpret_cast<const FS::KernelIovec*>(1), 1) == KERNEL_ERROR_EOPNOTSUPP &&
            FS::KernelPreadv(a, reinterpret_cast<const FS::KernelIovec*>(1), 1, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelLseek(a, 0, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelFtruncate(a, 0) == KERNEL_ERROR_EINVAL &&
            FS::KernelTruncate("/dev/sshot", 0) == KERNEL_ERROR_EINVAL &&
            FS::KernelFsync(a) == KERNEL_ERROR_EINVAL &&
            FS::KernelGetdents(a, &byte, 512) == KERNEL_ERROR_ENOTDIR && byte == 'x',
        "stream/positioned/vector/seek/truncate/sync/directory operations fail honestly without abort");
  void* output = reinterpret_cast<void*>(0x1234);
  int mmap_error = 0;
  Check(FS::KernelMmap(nullptr, 0x4000, 1, 2, a, 0, &output) == KERNEL_ERROR_ENODEV &&
            FS::TryMmapAnonymous(a, nullptr, 0x4000, 1, 2, 0, &output, &mmap_error) &&
            mmap_error == KERNEL_ERROR_ENODEV && output == reinterpret_cast<void*>(0x1234),
        "kernel and libc mmap routes reject device without allocation/output");
  FS::FileStat stat {};
  Check(FS::KernelFstat(a, &stat) == 0 && (stat.st_mode & 0170000) == 0020000 && stat.st_size == 0 &&
            FS::KernelFstat(a, reinterpret_cast<FS::FileStat*>(1)) == KERNEL_ERROR_EFAULT &&
            FS::KernelStat("/dev/sshot", &stat) == 0 &&
            FS::KernelCheckReachability("/dev/sshot") == 0,
        "stat/fstat describe character device, with validated copyout");
  Check(FS::KernelFcntl(a, 3, 0) == 2 && FS::KernelFcntl(a, 0, 10) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP && FS::KernelFcntl(a, 11, 1) == -1,
        "fcntl preserves audited access mode, rejects dup/record locks without dereferencing args");

  // Public boundary normalizes signed fd/u32 command, translates only failures.
  const auto public_ioctl = &Libs::ShellHits::Kernel::Ioctl;
  *Posix::GetErrorAddr() = 0x1234;
  Check(public_ioctl(uint64_t {0x123400000000} | static_cast<uint32_t>(a),
                     uint64_t {0x123400000000} | Capture,
                     reinterpret_cast<uint64_t>(capture.data()), 0, 0, 0) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP && capture == capture_before,
        "public ioctl returns POSIX unsupported, no successful empty capture");
  *Posix::GetErrorAddr() = 0x1234;
  Check(public_ioctl(a, Teardown, reinterpret_cast<uint64_t>(block.data()), 0, 0, 0) == 0 &&
            *Posix::GetErrorAddr() == 0x1234, "successful public teardown preserves errno");
  Close(a);
  Check(public_ioctl(a, Setup, 0, 0, 0, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EBADF &&
            public_ioctl(UINT64_MAX, Setup, 0, 0, 0, 0) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EBADF,
        "closed/negative group-9d descriptors never fall through generic success");
  Close(b); // implicit release of configured registration
  pool.Unchanged();

  // Teardown/close release only registration state, even if the borrower has
  // revoked access since setup. They must not probe, clear, free or unmap it.
  const int revoked = Open();
  Check(Ioctl(revoked, Setup, block.data()) == 0 &&
            VirtualProtect(pool.base, 0x200000, PAGE_NOACCESS, &old) != 0,
        "revoke pool access after registration");
  Check(Ioctl(revoked, Teardown, block.data()) == 0, "teardown never accesses borrowed pool");
  Close(revoked);
  Check(VirtualProtect(pool.base, 0x200000, PAGE_READWRITE, &old) != 0, "borrower restores own pool access");
  pool.Unchanged();

  victim = Open();
  FS::TestSetSshotIoctlRetainedCallback(Reuse);
  Check(Ioctl(victim, Setup, block.data()) == KERNEL_ERROR_EBADF && block == initial,
        "stale retained ioctl cannot write after close/reuse");
  Check(Ioctl(replacement, Teardown, block.data()) == KERNEL_ERROR_EINVAL &&
            Ioctl(replacement, Setup, block.data()) == 0,
        "reused fd has fresh configuration state, never stale result redirected");
  Close(replacement);

  const int serialized = Open();
  int setup_result = -1, close_result = -1;
  std::thread setup_thread([&] {
    FS::TestSetSshotIoctlLockedCallback(HoldIoctl);
    setup_result = Ioctl(serialized, Setup, block.data());
    FS::TestSetSshotIoctlLockedCallback(nullptr);
  });
  WaitForMutexTest([] { return ioctl_locked.load(); });
  std::thread close_thread([&] {
    close_started = true;
    close_result = FS::KernelClose(serialized);
    close_finished = true;
  });
  WaitForMutexTest([] { return close_started.load(); });
  // Yield to the closer while copyout is deliberately held under the file lock.
  for (unsigned i = 0; i < 1000; ++i) { std::this_thread::yield(); }
  Check(!close_finished, "close cannot complete before in-flight ioctl copyout/state commit");
  release_ioctl = true;
  setup_thread.join();
  close_thread.join();
  Check(setup_result == 0 && close_result == 0 && block == initial,
        "serialized ioctl finishes before close clears owned registration");
  pool.Unchanged();
  Check(VirtualFree(pages, 0, MEM_RELEASE) != 0, "release request test memory");
#else
  // The permission/close-race fixture above targets the audited Windows host.
  const int fallback_fd = Open();
  Check(Ioctl(fallback_fd, Capture, nullptr) == KERNEL_ERROR_EOPNOTSUPP, "capture unsupported on every host");
  Close(fallback_fd);
#endif

  FS::Mount(root, "/sshot-tests");
  const int ordinary = FS::KernelOpen("/sshot-tests/plain", 0x602, 0600);
  Check(ordinary >= 3 && Ioctl(ordinary, Setup, nullptr) == KERNEL_ERROR_ENOTTY,
        "sshot request on ordinary reused fd is ENOTTY before any guest access");
  Check(Libs::ShellHits::Kernel::Ioctl(ordinary, Capture, 0, 0, 0, 0) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_ENOTTY,
        "public wrong-device command cannot generic-success");
  Close(ordinary);
  int pair[2] {-1, -1};
  Check(Libs::Network::Net::Socketpair(1, 1, 0, pair) == 0,
        "local socketpair provides a wrong-device descriptor");
  Check(Ioctl(pair[0], Setup, nullptr) == KERNEL_ERROR_ENOTTY &&
            Libs::ShellHits::Kernel::Ioctl(pair[0], Capture, 0, 0, 0, 0) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_ENOTTY,
        "group-9d socket requests reject without generic success or argument access");
  Check(Libs::Network::Net::SocketClose(pair[0]) == 0 && Libs::Network::Net::SocketClose(pair[1]) == 0,
        "close local socketpair");
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  using PosixOpen = int(KYTY_SYSV_ABI *)(const char*, int, int);
  const auto* symbol = symbols.Find({"wuCroIGjt2g", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
  Check(symbol != nullptr, "POSIX open export exists");
  const auto open = reinterpret_cast<PosixOpen>(symbol->vaddr);
  Check(open("/dev/sshot", 3, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
        "invalid device flags use established POSIX open boundary");
  const int fd = open("/dev/sshot", 2, 0);
  Check(fd >= 3, "POSIX open creates real special device");
  Close(fd);
}
