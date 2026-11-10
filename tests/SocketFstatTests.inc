// Include inside KernelFileSystemTests.cpp's test namespace. Uses its Check,
// existing global InitLibKernel_1 / InitNet_1_Net declarations and standard
// array/cstddef/cstdint/cstdlib/cstring/limits/string/thread includes.
// Run TestSocketFstat() after thread/config/log setup and before normal FS
// startup: it owns FileSystem lifetime. No InitAll/Network instance or external
// network is needed. Requires KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS.
namespace SocketFstatTests {
namespace Net = Libs::Network::Net;
namespace FS = Libs::LibKernel::FileSystem;
namespace Posix = Libs::Posix;

struct Environment {
  bool present;
  std::string old;
  Environment() {
    const char* value = std::getenv("KYTY_NETWORK_HOST");
    present = value != nullptr;
    if (value) { old = value; }
    Set("1"); // Registered sceNetSocket must use the real backend, never FakeSock.
  }
  static void Set(const char* value) {
#ifdef _WIN32
    _putenv_s("KYTY_NETWORK_HOST", value ? value : "");
#else
    if (value) { setenv("KYTY_NETWORK_HOST", value, 1); } else { unsetenv("KYTY_NETWORK_HOST"); }
#endif
  }
  ~Environment() { Set(present ? old.c_str() : nullptr); }
};

struct Api {
  Loader::SymbolDatabase symbols;
  int (KYTY_SYSV_ABI *socket)(const char*, int, int, int) = nullptr;
  decltype(&Net::SocketClose) close = nullptr;
  decltype(&Net::Setsockopt) set = nullptr;
  decltype(&FS::KernelFstat) kernel_stat = nullptr;
  int64_t (KYTY_SYSV_ABI *stat)(int, FS::FileStat*) = nullptr;
  template<class F> F Function(const char* nid, const char* name) {
    const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr && symbol->vaddr != 0 && symbol->dbg_name == name,
          "socket fstat fixture resolves the production export");
    return reinterpret_cast<F>(symbol->vaddr);
  }
  Api() {
    Libs::InitLibKernel_1(&symbols);
    Libs::LibNet::InitNet_1_Net(&symbols);
    socket = Function<decltype(socket)>("Q4qBuN-c0ZM", "LibNet::NetSocket");
    close = Function<decltype(close)>("45ggEzakPJQ", "LibNet::NetSocketClose");
    set = Function<decltype(set)>("2mKX2Spso7I", "LibNet::NetSetsockopt");
    stat = Function<decltype(stat)>("mqQMh1zPPT8", "Posix::fstat");
    kernel_stat = Function<decltype(kernel_stat)>("kBwCPsYX-m4", "FileSystem::KernelFstat");
  }
  int Create(int type = 1) {
    const int fd = socket("socket-fstat", 2, type, 0);
    Check(fd >= 128 && fd < 1024 && Net::IsSocket(fd), "registered socket owns a real transport");
    return fd;
  }
};

struct Guarded {
  std::array<uint8_t, 16> before;
  FS::FileStat value;
  std::array<uint8_t, 16> after;
  Guarded() { before.fill(0x5a); std::memset(&value, 0xa5, sizeof(value)); after.fill(0xc3); }
  void Canaries() const {
    Check(std::all_of(before.begin(), before.end(), [](auto b) { return b == 0x5a; }) &&
              std::all_of(after.begin(), after.end(), [](auto b) { return b == 0xc3; }),
          "fstat writes exactly FileStat without touching adjacent storage");
  }
  void Socket() const {
    Canaries();
    std::array<uint8_t, sizeof(FS::FileStat)> expected {};
    const uint16_t type = 0140000u;
    std::memcpy(expected.data() + offsetof(FS::FileStat, st_mode), &type, sizeof(type));
    Check(std::memcmp(&value, expected.data(), expected.size()) == 0,
          "minimal retained socket stat has S_IFSOCK and no invented native fields");
  }
  void Untouched() const {
    Canaries();
    std::array<uint8_t, sizeof(FS::FileStat)> expected;
    expected.fill(0xa5);
    Check(std::memcmp(&value, expected.data(), expected.size()) == 0,
          "failed socket stat must preserve all output bytes");
  }
};

void Success(Api& api, int fd) {
  Guarded output;
  *Posix::GetErrorAddr() = 0x1234;
  Check(api.stat(fd, &output.value) == 0 && *Posix::GetErrorAddr() == 0x1234,
        "POSIX fstat accepts a real socket and preserves errno on success");
  output.Socket();
  Guarded kernel;
  Check(api.kernel_stat(fd, &kernel.value) == 0, "kernel fstat uses the same socket dispatch");
  kernel.Socket();
}

void Bad(Api& api, int fd) {
  Guarded output;
  Check(api.stat(fd, &output.value) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EBADF,
        "closed/invalid reserved socket fd is POSIX EBADF, never a file fallback");
  output.Untouched();
  Check(api.kernel_stat(fd, &output.value) == Libs::LibKernel::KERNEL_ERROR_EBADF,
        "closed/invalid reserved socket fd retains kernel error namespace");
  output.Untouched();
}

struct Reuse { Api* api; int victim; bool replace; int replacement = -1; };
void Retire(void* opaque) {
  auto& reuse = *static_cast<Reuse*>(opaque);
  std::thread worker([&] {
    Check(reuse.api->close(reuse.victim) == 0, "close retained stat socket on another thread");
    if (reuse.replace) {
      reuse.replacement = reuse.api->Create(2);
      Check(reuse.replacement == reuse.victim, "force same numeric fd with a different transport");
    }
  });
  worker.join(); // The stat hook must not retain a table lock/critical guard.
}
} // namespace SocketFstatTests

void TestSocketFstat() {
  using namespace SocketFstatTests;
  Environment environment;
  Api api;
  FS::Initialize();
  for (int type: {1, 2, 6}) {
    const int fd = api.Create(type); // TCP, UDP and real UDP-backed P2P descriptor.
    const int enabled = 1;
    Check(api.set(fd, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0,
          "Apple SO_NBIO followed by fstat sequence uses real socket configuration");
    Success(api, fd);
    Check(api.stat(fd, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
          "socket fstat null destination is EFAULT");
    Success(api, fd); // Failed query must not close or change the descriptor.
    Check(api.close(fd) == 0, "close stat fixture socket");
    Bad(api, fd);
    Check(api.kernel_stat(fd, nullptr) == Libs::LibKernel::KERNEL_ERROR_EFAULT,
          "existing null-pointer precedence is preserved for closed descriptors");
  }
  Bad(api, 1023);
  Bad(api, std::numeric_limits<int>::max());
  Guarded standard;
  Check(api.stat(1, &standard.value) == 0 && (standard.value.st_mode & 0170000u) == 0020000u,
        "socket routing does not capture standard-stream stat");
  standard.Canaries();
  Guarded negative;
  Check(api.kernel_stat(-1, &negative.value) == Libs::LibKernel::KERNEL_ERROR_EPERM,
        "unrelated legacy negative-fd stat behavior is unchanged");
  negative.Untouched();

  for (bool replace: {false, true}) {
    const int fd = api.Create();
    Reuse reuse {&api, fd, replace};
    Net::SocketRetainedStatHookForTest(Retire, &reuse);
    Guarded output;
    Check(api.stat(fd, &output.value) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EBADF,
          "retired generation cannot publish a stat for a replacement socket");
    output.Untouched();
    if (reuse.replacement >= 0) {
      Success(api, reuse.replacement);
      Check(api.close(reuse.replacement) == 0, "close replacement socket");
    }
  }

  const auto baseline = Net::SocketpairResourcesForTest();
  int pair[2] {-1, -1};
  Check(Net::Socketpair(1, 1, 0, pair) == 0, "create real socketpair stat fixture");
  Check(Net::Send(pair[0], "s", 1, 0) == 1, "queue actual socketpair data before stat");
  Success(api, pair[0]); Success(api, pair[1]);
  char byte = 0;
  Check(Net::Recv(pair[1], &byte, 1, 0) == 1 && byte == 's', "fstat does not consume socket data");
  Check(api.close(pair[0]) == 0 && api.close(pair[1]) == 0, "close stat socketpair fixture");
  Bad(api, pair[0]); Bad(api, pair[1]);
  const auto final = Net::SocketpairResourcesForTest();
  Check(final.sockets == baseline.sockets && final.paths == baseline.paths && final.slots == baseline.slots,
        "retained stat snapshots do not leak socketpair metadata/transports");
  FS::Shutdown();
}
