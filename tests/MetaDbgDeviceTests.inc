namespace MetaDbgTests {
std::atomic<bool> waiting {false};
void Waiting() { waiting = true; }
}

void TestMetaDbgDevice() {
  using namespace Libs::LibKernel;
  namespace FS = Libs::LibKernel::FileSystem;
  constexpr auto path = "/dev/metadbg";
  Check(FS::KernelOpen(path, 3, 0) == KERNEL_ERROR_EINVAL &&
        FS::KernelOpen(path, 0x20002, 0) == KERNEL_ERROR_ENOTDIR &&
        FS::KernelOpen(path, 0x602, 0) == KERNEL_ERROR_EOPNOTSUPP &&
        FS::KernelOpen("/dev/metadbg/child", 2, 0) == KERNEL_ERROR_ENOTDIR,
        "metadbg invalid open and namespace contracts");
  FS::FileStat stat {};
  Check(FS::KernelStat(path, &stat) == 0 && (stat.st_mode & 0170000) == 0020000,
        "metadbg is a character device");
  const int fd = FS::KernelOpen(path, 0x100006, 0);
  Check(fd >= 3, "metadbg cloexec nonblocking open");
  std::array<uint8_t, 32> buffer;
  buffer.fill(0x5a);
  const auto original = buffer;
  Check(FS::KernelRead(fd, buffer.data(), buffer.size()) == KERNEL_ERROR_EAGAIN && buffer == original,
        "empty event channel never returns EOF or invents an event");
  Check(FS::KernelRead(fd, nullptr, 0) == 0 &&
        FS::KernelRead(fd, nullptr, 32) == KERNEL_ERROR_EFAULT &&
        FS::KernelWrite(fd, buffer.data(), buffer.size()) == KERNEL_ERROR_EOPNOTSUPP &&
        FS::KernelPread(fd, buffer.data(), buffer.size(), 0) == KERNEL_ERROR_ESPIPE,
        "metadbg unsupported operations return errors");
  int result = 0;
  Check(FS::TryMetaDbgIoctl(fd, &result) && result == KERNEL_ERROR_ENOTTY,
        "metadbg ioctl never falls through to success stubs");
  Check(FS::KernelFcntl(fd, 1, 0) == 1 && FS::KernelFcntl(fd, 3, 0) == 6,
        "metadbg flags retained");
  Check(FS::KernelFcntl(fd, 4, 0) == 0, "switch to blocking mode");
  MetaDbgTests::waiting = false;
  FS::TestSetMetaDbgWaitingCallback(MetaDbgTests::Waiting);
  auto read = std::async(std::launch::async, [&] { return FS::KernelRead(fd, buffer.data(), buffer.size()); });
  WaitForMutexTest([] { return MetaDbgTests::waiting.load(); });
  Check(read.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
        "blocking read remains pending without debugger events");
  Check(FS::KernelClose(fd) == 0, "close wakes pending reader");
  const int replacement = FS::KernelOpen(path, 6, 0);
  Check(replacement == fd, "reuse descriptor while old read retains file");
  Check(read.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
        read.get() == KERNEL_ERROR_EBADF && buffer == original,
        "old pending read observes closure without touching replacement or output");
  FS::TestSetMetaDbgWaitingCallback(nullptr);
  Check(FS::KernelRead(replacement, buffer.data(), buffer.size()) == KERNEL_ERROR_EAGAIN,
        "replacement retains independent event state");
  Check(FS::KernelClose(replacement) == 0, "close replacement");
}
