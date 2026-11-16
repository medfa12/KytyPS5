// Include inside KernelFileSystemTests.cpp's test namespace; call
// TestKernelWriteEvents(root) with FileSystem initialized and a temporary root.
// Parent owns export/build/runner integration. Native evidence and deliberate
// ordinary-host-file-only subset: run19 kernel-imports sections 7-8 follow-up.
// data=0 is the verified native VFS result, NEVER an unknown-capacity sentinel.
namespace KernelWriteEventTests {
namespace EQ = Libs::LibKernel::EventQueue;
namespace FS = Libs::LibKernel::FileSystem;
namespace Net = Libs::Network::Net;
namespace Posix = Libs::Posix;
using namespace Libs::LibKernel;

struct Api {
  Loader::SymbolDatabase symbols;
  int (KYTY_SYSV_ABI *add)(EQ::KernelEqueue, int, size_t, void*) = nullptr;
  int (KYTY_SYSV_ABI *remove)(EQ::KernelEqueue, int) = nullptr;
  Api() {
    Libs::InitLibKernel_1(&symbols);
    Loader::SymbolResolve add_query {"R-tyYMpYaxY", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    Loader::SymbolResolve delete_query {"cBGTk8S92XM", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    const auto* a = symbols.FindRegistration(add_query, "EventQueue::KernelAddWriteEvent");
    const auto* d = symbols.FindRegistration(delete_query, "EventQueue::KernelDeleteWriteEvent");
    Check(a != nullptr && d != nullptr && a->vaddr == reinterpret_cast<uint64_t>(&EQ::KernelAddWriteEvent) &&
              d->vaddr == reinterpret_cast<uint64_t>(&EQ::KernelDeleteWriteEvent),
          "write-event exact typed NIDs/module/library versions resolve to real implementations");
    add = reinterpret_cast<decltype(add)>(a->vaddr);
    remove = reinterpret_cast<decltype(remove)>(d->vaddr);
    add_query.library_version = 2;
    delete_query.module_version_minor = 0;
    Check(symbols.FindRegistration(add_query, "EventQueue::KernelAddWriteEvent") == nullptr &&
              symbols.FindRegistration(delete_query, "EventQueue::KernelDeleteWriteEvent") == nullptr,
          "write events have no invented library/module version aliases");
  }
};

struct Queue {
  EQ::KernelEqueue value = 0;
  Queue() { Check(EQ::KernelCreateEqueue(&value, "write-event-test") == 0, "create write-event queue"); }
  ~Queue() { if (value) { Check(EQ::KernelDeleteEqueue(value) == 0, "close write-event queue"); } }
  EQ::KernelEvent Poll(bool expected, KernelUseconds timeout = 0) {
    EQ::KernelEvent events[4] {};
    int count = -1;
    const int result = EQ::KernelWaitEqueue(value, events, 4, &count, &timeout);
    Check(result == (expected ? 0 : KERNEL_ERROR_ETIMEDOUT) && count == (expected ? 1 : 0),
          "write readiness delivers one event per wait or a real empty-queue timeout");
    return events[0];
  }
};

void Ready(const EQ::KernelEvent& event, int fd, void* data) {
  Check(event.ident == static_cast<uintptr_t>(static_cast<intptr_t>(fd)) && event.filter == -2 &&
            event.flags == 1 && event.fflags == 0 && event.data == 0 && event.udata == data,
        "VFS write event is filter -2, ADD without CLEAR, verified zero data and preserved udata");
}

void Invalid(Api& api) {
  Queue queue;
  Check(api.add(0, -1, 0, nullptr) == KERNEL_ERROR_EBADF && api.remove(0, -1) == KERNEL_ERROR_EBADF,
        "write add/delete reject invalid queue handles");
  for (int fd : {-1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 1023}) {
    Check(api.add(queue.value, fd, 0, nullptr) == KERNEL_ERROR_EBADF,
          "write add rejects negative, closed and out-of-range descriptor identities");
  }
  Check(api.remove(queue.value, -1) == KERNEL_ERROR_ENOENT && api.remove(queue.value, 1023) == KERNEL_ERROR_ENOENT,
        "delete removes a registration rather than validating a data descriptor");
  for (int fd : {0, 1, 2}) {
    Check(api.add(queue.value, fd, 0, nullptr) == KERNEL_ERROR_EOPNOTSUPP,
          "standard streams have no fabricated write-event backend");
  }
  queue.Poll(false);
  const auto start = std::chrono::steady_clock::now();
  queue.Poll(false, 10000);
  Check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(5),
        "empty timed queue wait does not report an immediate fabricated timeout");
}

void Files(Api& api) {
  void* first = reinterpret_cast<void*>(uintptr_t{0x1234});
  void* second = reinterpret_cast<void*>(uintptr_t{0x9876});
  const int seed = FS::KernelOpen("/write-events/file", 0x602, 0600);
  Check(seed >= 3 && FS::KernelWrite(seed, "file", 4) == 4 && FS::KernelClose(seed) == 0,
        "seed real ordinary host file");
  for (int mode : {0, 1, 2}) {
    Queue queue;
    const int fd = FS::KernelOpen("/write-events/file", mode, 0);
    *Posix::GetErrorAddr() = 0x1234;
    Check(fd >= 3 && api.add(queue.value, fd, 0, first) == 0,
          "native generic VFS write readiness accepts read-only, write-only and read/write descriptors");
    Ready(queue.Poll(true), fd, first);
    Ready(queue.Poll(true, 10000), fd, first); // Level survives repeated/timed polls.
    Check(*Posix::GetErrorAddr() == 0x1234, "kernel write readiness preserves guest POSIX errno");
    if (mode == 0) {
      Check(FS::KernelWrite(fd, "x", 1) == KERNEL_ERROR_EBADF,
            "VFS readiness is not authorization to write a read-only descriptor");
      Ready(queue.Poll(true), fd, first);
    }
    Check(api.add(queue.value, fd, 1, second) == KERNEL_ERROR_EOPNOTSUPP &&
              api.add(queue.value, fd, std::numeric_limits<size_t>::max(), second) == KERNEL_ERROR_EOPNOTSUPP,
          "nonzero low-water requests fail explicitly, including nonrepresentable signed sizes");
    Ready(queue.Poll(true), fd, first); // Failed re-add cannot change a live watch.
    Check(api.add(queue.value, fd, 0, second) == 0, "duplicate file add updates udata");
    Ready(queue.Poll(true), fd, second);
    Check(api.remove(queue.value, fd) == 0 && api.remove(queue.value, fd) == KERNEL_ERROR_ENOENT,
          "delete succeeds once, then reports ENOENT");
    queue.Poll(false);
    Check(FS::KernelClose(fd) == 0 && api.add(queue.value, fd, 0, first) == KERNEL_ERROR_EBADF,
          "closed ordinary file registration is EBADF");
  }
}

void CloseReuse(Api& api) {
  for (bool poll_before_readd : {false, true}) {
    Queue queue;
    const int fd = FS::KernelOpen("/write-events/file", 2, 0);
    void* first = reinterpret_cast<void*>(uintptr_t{0xabcd});
    void* second = reinterpret_cast<void*>(uintptr_t{0xdcba});
    Check(fd >= 3 && api.add(queue.value, fd, 0, first) == 0, "register close/reuse victim");
    Ready(queue.Poll(true), fd, first); // An already-delivered level must not stick after close.
    Check(FS::KernelClose(fd) == 0, "close watched descriptor while its File object is retained");
    const int replacement = FS::KernelOpen("/write-events/replacement", 0x602, 0600);
    Check(replacement == fd, "force numeric fd reuse with a distinct live File object");
    if (poll_before_readd) {
      queue.Poll(false);
      Check(api.remove(queue.value, fd) == KERNEL_ERROR_ENOENT,
            "queue pass retires a closed identity instead of following its reused fd");
    }
    Check(api.add(queue.value, replacement, 0, second) == 0,
          "explicit re-add binds the replacement identity, even before lazy old-watch retirement");
    Ready(queue.Poll(true), replacement, second);
    Check(FS::KernelClose(replacement) == 0, "close replacement descriptor");
    queue.Poll(false);
  }
}

void DuplicateMetadata(Api& api) {
  Queue queue;
  const int fd = FS::KernelOpen("/write-events/file", 2, 0);
  Check(fd >= 3, "open duplicate metadata file");
  EQ::KernelEqueueEvent stale {};
  stale.triggered = true;
  stale.event.ident = fd;
  stale.event.filter = -2;
  stale.event.flags = 0x31; // ADD|CLEAR|ONESHOT must not survive typed EV_ADD.
  stale.event.fflags = 0x1234;
  stale.event.data = 12345;
  stale.deadline_ns = 1;
  stale.interval_ns = 1;
  stale.pending_events.push_back(stale.event);
  Check(EQ::KernelAddEvent(queue.value, stale) == 0, "seed stale duplicate registration metadata");
  void* data = reinterpret_cast<void*>(uintptr_t{0x7654});
  Check(api.add(queue.value, fd, 0, data) == 0, "typed duplicate add fully replaces descriptor metadata");
  Ready(queue.Poll(true), fd, data);
  Ready(queue.Poll(true), fd, data); // Neither stale ONESHOT nor queued payloads survive.
  Check(EQ::KernelAddUserEvent(queue.value, fd) == 0 &&
            EQ::KernelTriggerUserEvent(queue.value, fd, data) == 0 && api.remove(queue.value, fd) == 0,
        "write deletion coexists with a different filter on the same identity");
  const auto user = queue.Poll(true);
  Check(user.filter == EQ::KERNEL_EVFILT_USER && user.ident == static_cast<uintptr_t>(fd),
        "typed write deletion cannot remove another filter");
  Check(EQ::KernelDeleteUserEvent(queue.value, fd) == 0 && FS::KernelClose(fd) == 0,
        "close duplicate metadata fixture");
  queue.Poll(false);
}

void Unsupported(Api& api) {
  Queue queue;
  const auto reject = [&](int fd) {
    for (size_t size : {size_t{0}, size_t{1}}) {
      Check(api.add(queue.value, fd, size, reinterpret_cast<void*>(uintptr_t{0x123})) == KERNEL_ERROR_EOPNOTSUPP,
            "unsupported descriptor fails before creating a successful or dormant write watch");
    }
    Check(api.remove(queue.value, fd) == KERNEL_ERROR_ENOENT, "failed write add leaves no registration to delete");
    queue.Poll(false);
  };
  int sockets[2] {-1, -1};
  Check(Net::Socketpair(1, 1, 0, sockets) == 0 && Net::Send(sockets[0], "s", 1, 0) == 1,
        "create actual connected AF_LOCAL socket transport with queued bytes");
  reject(sockets[0]); reject(sockets[1]);
  char socket_byte = '!';
  Check(Net::Recv(sockets[1], &socket_byte, 1, 0) == 1 && socket_byte == 's',
        "unsupported socket event requests do not consume actual transport bytes");
  Check(Net::SocketClose(sockets[0]) == 0 && Net::SocketClose(sockets[1]) == 0 &&
            api.add(queue.value, sockets[0], 0, nullptr) == KERNEL_ERROR_EBADF,
        "closed socket slots remain EBADF, not unsupported live descriptors");
  const int listener = Net::Socket(2, 1, 0);
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  Check(listener >= 0 && Net::Bind(listener, address.data(), address.size()) == 0 && Net::Listen(listener, 1) == 0,
        "create actual loopback listener");
  reject(listener);
  Check(Net::SocketClose(listener) == 0, "close unsupported listener");
  for (int type : {1, 2, 6}) {
    const int fd = Net::Socket(2, type, 0);
    Check(fd >= 0, "create unconnected stream, datagram or P2P descriptor");
    reject(fd);
    Check(Net::SocketClose(fd) == 0, "close unsupported socket");
  }
  int pipe[2] {-1, -1};
  Check(FS::KernelCreatePipe(pipe) == 0, "create actual unbounded emulator pipe");
  reject(pipe[0]); reject(pipe[1]);
  Check(FS::KernelWrite(pipe[1], "pipe", 4) == 4, "unsupported event registration does not break pipe transfers");
  char bytes[4] {};
  Check(FS::KernelRead(pipe[0], bytes, sizeof(bytes)) == 4 && std::memcmp(bytes, "pipe", 4) == 0,
        "unsupported pipe event requests neither consume nor fabricate bytes");
  Check(FS::KernelClose(pipe[0]) == 0, "close pipe peer");
  reject(pipe[1]); // EOF is not permission to invent a capacity backend.
  Check(FS::KernelClose(pipe[1]) == 0 && api.add(queue.value, pipe[1], 0, nullptr) == KERNEL_ERROR_EBADF,
        "closed pipe descriptor is EBADF");
  for (const char* path : {"/dev/random", "/write-events"}) {
    const int fd = FS::KernelOpen(path, 0, 0);
    Check(fd >= 3, "open actual device or directory descriptor");
    reject(fd);
    Check(FS::KernelClose(fd) == 0, "close unsupported device/directory");
  }
  queue.Poll(false, 10000);
}

void WakeOnAdd(Api& api) {
  Queue queue;
  const int fd = FS::KernelOpen("/write-events/file", 0, 0);
  Check(fd >= 3, "open add-wakeup regular file");
  std::atomic_int started {0};
  std::atomic_int finished {0};
  EQ::KernelEvent events[2] {};
  int results[2] {-999, -999};
  int counts[2] {-1, -1};
  const auto handle = queue.value;
  std::thread waiters[2];
  for (int i = 0; i < 2; ++i) {
    waiters[i] = std::thread([&, i] {
      started.fetch_add(1);
      results[i] = EQ::KernelWaitEqueue(handle, &events[i], 1, &counts[i], nullptr);
      finished.fetch_add(1);
    });
  }
  while (started.load() != 2) { std::this_thread::yield(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Check(api.add(queue.value, fd, 0, nullptr) == 0, "add supported VFS write watch to an empty blocked queue");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (finished.load() != 2 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  const bool woke = finished.load() == 2;
  if (!woke) { Check(EQ::KernelDeleteEqueue(queue.value) == 0, "cancel failed add-wakeup waiters"); queue.value = 0; }
  for (auto& waiter : waiters) { waiter.join(); }
  Check(woke && results[0] == 0 && results[1] == 0 && counts[0] == 1 && counts[1] == 1,
        "write EV_ADD wakes all blocked level-event queue waiters");
  Ready(events[0], fd, nullptr); Ready(events[1], fd, nullptr);
  Check(FS::KernelClose(fd) == 0, "close add-wakeup regular file");
}
} // namespace KernelWriteEventTests

void TestKernelWriteEvents(const std::filesystem::path& root) {
  using namespace KernelWriteEventTests;
  const auto folder = root / "write-events";
  std::filesystem::create_directories(folder);
  FS::Mount(folder, "/write-events");
  Api api;
  Invalid(api);
  Files(api);
  CloseReuse(api);
  DuplicateMetadata(api);
  Unsupported(api);
  WakeOnAdd(api);
  FS::Umount("/write-events");
}
