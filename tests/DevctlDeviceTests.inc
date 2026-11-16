void TestDevctlDevice() {
  using namespace Libs::LibKernel;
  namespace FS = Libs::LibKernel::FileSystem;
  const int fd = FS::KernelOpen("/dev/devctl", 4, 0);
  Check(fd >= 3, "retail devctl nonblocking open");
  std::array<char, 64> data; data.fill('x');
  const auto original = data;
  Check(FS::KernelRead(fd, data.data(), data.size()) == KERNEL_ERROR_EAGAIN && data == original,
        "empty devctl read returns raw EAGAIN without fake records");
  Check(FS::KernelRead(fd, nullptr, 0) == 0 && FS::KernelRead(fd, nullptr, 1) == KERNEL_ERROR_EFAULT,
        "devctl read buffer validation");
  Check(FS::KernelFcntl(fd, 3, 0) == 4, "devctl nonblocking flags");
  const int dup = FS::KernelFcntl(fd, 0, fd + 10);
  Check(dup >= fd + 10 && FS::KernelFcntl(dup, 3, 0) == 4, "duplicate descriptor and shared channel");
  Check(FS::KernelClose(fd) == 0 && FS::KernelRead(dup, data.data(), data.size()) == KERNEL_ERROR_EAGAIN,
        "closing original retains duplicate");
  const int replacement = FS::KernelOpen("/dev/devctl", 4, 0);
  Check(replacement == fd, "closed descriptor slot reused");
  Check(FS::KernelClose(dup) == 0 && FS::KernelRead(replacement, data.data(), data.size()) == KERNEL_ERROR_EAGAIN,
        "closing duplicate leaves replacement independent");
  int ioctl = 0;
  Check(FS::TryDevctlIoctl(replacement, &ioctl) && ioctl == KERNEL_ERROR_ENOTTY,
        "unmodeled devctl ioctl rejected");
  Check(FS::KernelWrite(replacement, data.data(), 1) == KERNEL_ERROR_EBADF &&
        FS::KernelPread(replacement, data.data(), 1, 0) == KERNEL_ERROR_ESPIPE,
        "unsupported descriptor operations rejected");
  Check(FS::KernelClose(replacement) == 0 && FS::KernelRead(replacement, data.data(), 1) == KERNEL_ERROR_EBADF,
        "closed descriptor rejected");
}
