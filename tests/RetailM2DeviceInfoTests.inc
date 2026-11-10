void TestRetailM2DeviceInfo() {
  namespace FS = Libs::LibKernel::FileSystem;
  Loader::SymbolDatabase symbols;
  Libs::FsSandbox::FsInternalForVsh::InitFsInternalForVshSandbox_1(&symbols);
  const auto* binding = symbols.FindByNid("NoXU3Apuy4I", Loader::SymbolType::Func);
  Check(binding != nullptr, "retail M2 device-info import registered");
  using GetInfo = int (KYTY_SYSV_ABI *)(void*, void*, void*, void*, size_t);
  const auto get = reinterpret_cast<GetInfo>(binding->vaddr);
  std::array<unsigned char, 256> info; info.fill(0x5a);
  const auto original = info;
  Check(get(nullptr, nullptr, nullptr, nullptr, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "null mandatory M2 info rejected");
  Check(get(info.data(), nullptr, nullptr, info.data(), 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL &&
        get(info.data(), nullptr, nullptr, nullptr, 1) == Libs::LibKernel::KERNEL_ERROR_EINVAL && info == original,
        "nonzero reserved M2 arguments rejected without output mutation");
  std::array<unsigned char, 64> hash; hash.fill(0xa5);
  std::array<unsigned char, 4096> controller; controller.fill(0x3c);
  const auto hash_original = hash;
  const auto controller_original = controller;
  Check(get(info.data(), hash.data(), controller.data(), nullptr, 0) == Libs::LibKernel::KERNEL_ERROR_ENODEV &&
        info == original && hash == hash_original && controller == controller_original,
        "absent retail NVMe returns ENODEV with all device outputs untouched");
  Check(get(info.data(), nullptr, nullptr, nullptr, 0) == Libs::LibKernel::KERNEL_ERROR_ENODEV && info == original,
        "absent device permits null optional hash and controller outputs");
  const int fd = FS::KernelOpen("/dev/m2.ctl", 0, 0);
  Check(fd >= 3, "optional absent storage still has controller endpoint");
  Check(FS::KernelOpen("/dev/m2.ctl", 0x200, 0) == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP,
        "unsupported controller create does not create host file");
  int result = 0;
  uint64_t envelope[] = {reinterpret_cast<uint64_t>(info.data()), reinterpret_cast<uint64_t>(hash.data()),
                         reinterpret_cast<uint64_t>(controller.data()), 0, 0x7800000001ULL};
  Check(FS::TryM2Ioctl(fd, 0x80283208, envelope, &result) && result == Libs::LibKernel::KERNEL_ERROR_ENODEV &&
        info == original && hash == hash_original && controller == controller_original,
        "real controller descriptor routes summary ioctl without metadata copyout");
  *Libs::Posix::GetErrorAddr() = 0;
  Check(Libs::ShellHits::Kernel::Ioctl(fd, 0x80283208, reinterpret_cast<uint64_t>(envelope), 0, 0, 0) == -1 &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENODEV && info == original,
        "native POSIX ioctl boundary returns minus one and ENODEV rather than fallback success");
  Check(FS::TryM2Ioctl(fd, 0x80283208, nullptr, &result) && result == Libs::LibKernel::KERNEL_ERROR_EFAULT,
        "summary ioctl validates envelope before dispatch");
  Check(FS::TryM2Ioctl(fd, 0xdeadbeef, nullptr, &result) && result == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP,
        "unknown and attached-device commands stay unsupported without touching pointers");
  Check(!FS::TryM2Ioctl(1, 0x80283208, envelope, &result), "M2 request cannot claim unrelated descriptor");
  Check(FS::KernelRead(fd, info.data(), info.size()) == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP &&
        FS::KernelWrite(fd, info.data(), info.size()) == Libs::LibKernel::KERNEL_ERROR_EBADF && info == original,
        "controller cannot read or write a host-backed media file");
  Check(FS::KernelClose(fd) == 0 && !FS::TryM2Ioctl(fd, 0x80283208, envelope, &result),
        "closed controller descriptor no longer dispatches");
  const auto* verify_binding = symbols.FindByNid("V6i6yMSwqo4", Loader::SymbolType::Func);
  Check(verify_binding != nullptr, "retail M2 metadata verification import registered");
  using Verify = int (KYTY_SYSV_ABI *)(uint32_t, const void*);
  const auto verify = reinterpret_cast<Verify>(verify_binding->vaddr);
  Check(verify(2, info.data()) == Libs::LibKernel::KERNEL_ERROR_EINVAL &&
        verify(0, info.data()) == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP &&
        verify(1, info.data()) == Libs::LibKernel::KERNEL_ERROR_EOPNOTSUPP && info == original,
        "M2 verification flags retained; attached-device metadata verification remains unsupported");
}
