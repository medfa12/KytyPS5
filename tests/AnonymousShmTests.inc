// Include beside GuestFsContextTests.inc. Main owns runner/registration/CMake.
// Requires Windows, KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS, initialized Memory,
// libKernel/guest main-thread TLS and FileSystem. Pass a database populated with
// InitLibKernel_1, InitLibKernel_1_Posix and InitLibcInternal_1.
namespace ShmFixture {
namespace Memory = Libs::LibKernel::Memory;
using namespace Libs::LibKernel;
constexpr uint64_t Page = 0x4000;

template<class T> T Import(Loader::SymbolDatabase& symbols, const char* nid) {
  const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
  Check(symbol != nullptr, "resolve actual registered anonymous-shm ABI");
  return reinterpret_cast<T>(symbol->vaddr);
}

struct Api {
  int (KYTY_SYSV_ABI *open)(const char*, int, unsigned);
  int (KYTY_SYSV_ABI *truncate)(int, int64_t);
  void* (KYTY_SYSV_ABI *mmap)(void*, size_t, int, int, int, int64_t);
  int (KYTY_SYSV_ABI *kernel_mmap)(void*, size_t, int, int, int, int64_t, void**);
  int (KYTY_SYSV_ABI *unmap)(uint64_t, size_t);
  int (KYTY_SYSV_ABI *close)(int);
  int (KYTY_SYSV_ABI *stat)(int, FileSystem::FileStat*);
  int64_t (KYTY_SYSV_ABI *pread)(int, void*, size_t, int64_t);
  int64_t (KYTY_SYSV_ABI *pwrite)(int, const void*, size_t, int64_t);
  int64_t (KYTY_SYSV_ABI *read)(int, void*, size_t);
  int64_t (KYTY_SYSV_ABI *write)(int, const void*, size_t);
  int64_t (KYTY_SYSV_ABI *seek)(int, int64_t, int);
  explicit Api(Loader::SymbolDatabase& symbols):
    open(Import<decltype(open)>(symbols, "QuJYZ2KVGGQ")),
    truncate(Import<decltype(truncate)>(symbols, "ih4CD9-gghM")),
    mmap(Import<decltype(mmap)>(symbols, "BPE9s9vQQXo")),
    kernel_mmap(Import<decltype(kernel_mmap)>(symbols, "PGhQHd-dzv8")),
    unmap(Import<decltype(unmap)>(symbols, "UqDGjXA5yUM")),
    close(Import<decltype(close)>(symbols, "UK2Tl2DWUns")),
    stat(Import<decltype(stat)>(symbols, "kBwCPsYX-m4")),
    pread(Import<decltype(pread)>(symbols, "+r3rMFwItV4")),
    pwrite(Import<decltype(pwrite)>(symbols, "nKWi-N2HBV4")),
    read(Import<decltype(read)>(symbols, "Cg4srZ6TKbU")),
    write(Import<decltype(write)>(symbols, "4wSze92BhLI")),
    seek(Import<decltype(seek)>(symbols, "oib76F-12fk")) {}
  int New(int flags = 0x202, unsigned mode = 0644) const {
    const int fd = open(reinterpret_cast<const char*>(uintptr_t {1}), flags, mode);
    Check(fd >= 3, "SHM_ANON sentinel opens without a string read");
    return fd;
  }
  uint8_t* Map(int fd, size_t length, int64_t offset = 0, int prot = 3) const {
    auto* pointer = mmap(nullptr, length, prot, 1, fd, offset);
    Check(pointer != reinterpret_cast<void*>(-1) && pointer != nullptr, "registered mmap creates shared view");
    return static_cast<uint8_t*>(pointer);
  }
  void Unmap(void* pointer, size_t length) const {
    Check(unmap(reinterpret_cast<uint64_t>(pointer), length) == OK, "registered munmap removes view");
  }
};

static void CountsEqual(Memory::ShmCounts expected, const char* message) {
  const auto actual = Memory::TestShmCounts();
  Check(actual.objects == expected.objects && actual.extents == expected.extents &&
        actual.views == expected.views && actual.quarantined == expected.quarantined && actual.free_bytes == expected.free_bytes, message);
}

static void CoherenceAndLifetime(const Api& api) {
  const auto baseline = Memory::TestShmCounts();
  const int fd = api.New();
  FileSystem::FileStat initial {}, sized {}, other {};
  Check(api.stat(fd, &initial) == OK && initial.st_size == 0 && initial.st_blocks == 0 &&
        initial.st_blksize == Page && (initial.st_mode & 0777) == 0644 && initial.st_nlink == 0,
        "initial real size and explicitly requested-mode metadata");
  Check(initial.st_uid == 0 && initial.st_gid == 0, "unknown ownership ABI fields remain zero, not credentials");
  Check(FileSystem::KernelFcntl(fd, 1, 0) == 1, "CLOEXEC is enabled by default");
  Check(api.truncate(fd, 0x30000) == 0, "registered ftruncate exact native boot length");
  Check(api.stat(fd, &sized) == OK && sized.st_size == 0x30000 && sized.st_blocks == 12 &&
        sized.st_ino == initial.st_ino && sized.st_dev == initial.st_dev, "stable identity and native page count");
  auto* first = api.Map(fd, 0x30000);
  void* second_result = nullptr;
  Check(api.kernel_mmap(nullptr, 0x30000, 3, 1, fd, 0, &second_result) == OK && second_result != first,
        "KernelMmap and libc mmap return distinct aliases");
  auto* second = static_cast<uint8_t*>(second_result);
  for (size_t i = 0; i < 0x30000; ++i) { Check(first[i] == 0, "new mapped pages are zero"); }
  std::array<uint8_t, 32> pattern {}, readback {};
  for (size_t i = 0; i < pattern.size(); ++i) { pattern[i] = static_cast<uint8_t>(i + 17); }
  std::memcpy(first + Page - 13, pattern.data(), pattern.size());
  Check(std::memcmp(second + Page - 13, pattern.data(), pattern.size()) == 0 &&
        api.pread(fd, readback.data(), readback.size(), Page - 13) == readback.size() && readback == pattern,
        "two aliases and pread share page-crossing bytes");
  pattern.fill(0xa3);
  Check(api.pwrite(fd, pattern.data(), pattern.size(), Page - 13) == pattern.size() &&
        std::memcmp(first + Page - 13, pattern.data(), pattern.size()) == 0 &&
        std::memcmp(second + Page - 13, pattern.data(), pattern.size()) == 0, "pwrite is immediately coherent");
  Check(api.seek(fd, Page - 13, 0) == Page - 13 && api.read(fd, readback.data(), readback.size()) == readback.size() &&
        readback == pattern && api.seek(fd, 0, 1) == Page + 19, "read advances the descriptor position");
  pattern.fill(0x63);
  Check(api.seek(fd, Page - 13, 0) == Page - 13 && api.write(fd, pattern.data(), pattern.size()) == pattern.size() &&
        std::memcmp(second + Page - 13, pattern.data(), pattern.size()) == 0, "write shares the mapping backing");

  auto* offset_view = api.Map(fd, Page + 1, 2 * Page);
  offset_view[3] = 0x81;
  Check(first[2 * Page + 3] == 0x81, "nonzero object offset aliases the correct extent");
  Memory::VirtualQueryInfo query {};
  Check(Memory::KernelVirtualQuery(offset_view, 0, &query, sizeof(query)) == OK && query.offset == 2 * Page &&
        query.end - query.start == 2 * Page && query.protection == 3 && !query.is_direct && !query.is_flexible,
        "SHM query exposes rounded length and object offset without direct/flexible ownership");
  Check(Memory::KernelMprotect(offset_view + Page, Page, 1) == OK &&
        Memory::KernelVirtualQuery(offset_view + Page, 0, &query, sizeof(query)) == OK && query.offset == 3 * Page &&
        query.protection == 1, "split protection retains object offset");
  Check(Memory::KernelSetVirtualRangeName(offset_view + Page, Page, "shm split") == OK &&
        Memory::KernelVirtualQuery(offset_view + Page, 0, &query, sizeof(query)) == OK && query.offset == 3 * Page &&
        Memory::KernelClearVirtualRangeName(offset_view + Page, Page) == OK,
        "rename/clear preserve split object offsets and ownership");
  MEMORY_BASIC_INFORMATION host {};
  Check(VirtualQuery(offset_view + Page, &host, sizeof(host)) != 0 && host.Protect == PAGE_READONLY,
        "read-only protection is enforced on host view");
  Check(api.pread(fd, offset_view + Page, 1, 0) == KERNEL_ERROR_EFAULT &&
        VirtualQuery(offset_view + Page, &host, sizeof(host)) != 0 && host.Protect == PAGE_READONLY,
        "fd copyout cannot silently relax read-only mapping permissions");
  Check(Memory::KernelMprotect(offset_view, 2 * Page, 0) == OK &&
        VirtualQuery(offset_view, &host, sizeof(host)) != 0 && host.Protect == PAGE_NOACCESS,
        "PROT_NONE is real host protection");
  uint8_t backing = 0;
  Check(Memory::TryReadBacking(reinterpret_cast<uint64_t>(offset_view) + 3, &backing, 1) && backing == 0x81,
        "owner-aware diagnostic read uses retained section alias without relaxing guest protection");
  Check(!Memory::ProtectGuestHostMemory(reinterpret_cast<uint64_t>(offset_view), Page, Common::VirtualMemory::Mode::ReadWrite) &&
        VirtualQuery(offset_view, &host, sizeof(host)) != 0 && host.Protect == PAGE_NOACCESS,
        "stale GPU transient protection cannot relax CPU-only SHM");
  Check(Memory::KernelMprotect(offset_view, 2 * Page, 3) == OK, "restore semantic CPU permissions");
  void* fixed = first;
  Check(Memory::KernelReserveVirtualRange(&fixed, Page, 0x10, Page) == KERNEL_ERROR_EOPNOTSUPP && fixed == first &&
        first[2 * Page + 3] == 0x81, "fixed-reserve rejection preserves metadata and bytes");
  Check(Memory::KernelMapFlexibleMemory(&fixed, Page, 3, 0x10) == KERNEL_ERROR_EOPNOTSUPP && fixed == first,
        "fixed flexible replacement uses the same typed SHM preflight");
  int64_t physical = 0;
  Check(Memory::KernelAllocateDirectMemory(0, static_cast<int64_t>(Memory::KernelGetDirectMemorySize()), Page, Page, 0, &physical) == OK,
        "allocate direct extent for overlap preflight fixture");
  Check(Memory::KernelMapDirectMemory(&fixed, Page, 3, 0x10, physical, Page) == KERNEL_ERROR_EOPNOTSUPP && fixed == first &&
        Memory::KernelReleaseDirectMemory(physical, Page) == OK, "fixed direct replacement rejects SHM before mutation");
  Check(api.truncate(fd, 0x40000) == 0 && first[2 * Page + 3] == 0x81, "live growth preserves original view identity");
  auto* extension = api.Map(fd, 0x10000, 0x30000);
  Check(extension[0] == 0 && extension[0xffff] == 0, "new growth extents are zero");
  Check(api.truncate(fd, 5) == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP &&
        api.stat(fd, &sized) == OK && sized.st_size == 0x40000, "affected live-map shrink is unsupported without mutation");
  api.Unmap(first + Page, Page);
  first[0] = 0x91;
  first[2 * Page] = 0x92;
  Check(second[0] == 0x91 && second[2 * Page] == 0x92, "middle-page unmap preserves neighbor aliases");
  std::vector<uint8_t> sparse(3 * Page, 0xff);
  Check(Memory::TryReadSparseBacking(reinterpret_cast<uint64_t>(first), sparse.data(), sparse.size()) &&
        sparse[0] == 0x91 && sparse[Page] == 0 && sparse[2 * Page] == 0x92,
        "sparse backing read combines SHM bytes with an unmapped hole");
  Check(api.close(fd) == OK, "descriptor closes before surviving views");
  const int recycled = api.New();
  Check(recycled == fd && api.stat(recycled, &other) == OK && other.st_ino != initial.st_ino && other.st_size == 0,
        "reused descriptor slot does not reuse object identity");
  second[7] = 0x71;
  Check(first[7] == 0x71 && offset_view[3] == 0x81, "views retain object after close and fd reuse");
  api.Unmap(first, 0x30000);
  api.Unmap(first, 0x30000);
  Check(second[7] == 0x71, "repeated/sparse munmap does not destroy another alias");
  api.Unmap(second, 0x30000);
  api.Unmap(offset_view, Page + 1);
  api.Unmap(extension, 0x10000);
  Check(api.close(recycled) == OK, "close recycled object");
  CountsEqual(baseline, "all views, extents and objects released after last owner");
}

static void EofAndZeroing(const Api& api) {
  const auto baseline = Memory::TestShmCounts();
  const int fd = api.New(2, 01777); // CREAT is optional; mode is uint16 & 0777.
  FileSystem::FileStat stat {};
  Check(api.stat(fd, &stat) == OK && (stat.st_mode & 07777) == 0777, "requested mode strips nonpermission bits");
  std::array<uint8_t, 4> data {1, 2, 3, 4}, out {};
  Check(api.write(fd, data.data(), data.size()) == 0, "write never grows an empty object");
  Check(api.truncate(fd, 5) == 0 && api.pwrite(fd, data.data(), 4, 3) == 2 &&
        api.pwrite(fd, data.data(), 4, 5) == 0 && api.pwrite(fd, data.data(), 4, 6) == 0,
        "writes crossing/at/beyond EOF are short or zero");
  Check(api.pread(fd, out.data(), 4, 3) == 2 && out[0] == 1 && out[1] == 2 &&
        api.pread(fd, out.data(), 4, 5) == 0 && api.seek(fd, 0, 1) == 0,
        "pread/pwrite never change position and reads stop at EOF");
  Check(api.pread(fd, nullptr, 1, -1) == KERNEL_ERROR_EINVAL &&
        api.pwrite(fd, nullptr, 1, -1) == KERNEL_ERROR_EINVAL,
        "nonzero positioned I/O validates offset before touching a null buffer");
  for (int64_t offset: {int64_t {5}, int64_t {6}}) {
    Check(api.pread(fd, nullptr, 1, offset) == 0 && api.pwrite(fd, nullptr, 1, offset) == 0,
          "nonzero requests at/beyond EOF do not access a null buffer");
  }
  Check(api.seek(fd, 5, 0) == 5 && api.read(fd, nullptr, 1) == 0 && api.write(fd, nullptr, 1) == 0 &&
        api.seek(fd, 0, 1) == 5 && api.seek(fd, 0, 0) == 0,
        "ordinary null-buffer EOF requests return zero and preserve position");
  Check(api.read(fd, nullptr, 1) == KERNEL_ERROR_EFAULT && api.write(fd, nullptr, 1) == KERNEL_ERROR_EFAULT &&
        api.seek(fd, 0, 1) == 0, "non-EOF requests still validate the accessed buffer");
  FileSystem::FileStat before_same {}, after_same {};
  Check(api.stat(fd, &before_same) == OK && api.truncate(fd, 5) == 0 && api.stat(fd, &after_same) == OK &&
        before_same.st_mtim.tv_sec == after_same.st_mtim.tv_sec && before_same.st_mtim.tv_nsec == after_same.st_mtim.tv_nsec &&
        before_same.st_ctim.tv_sec == after_same.st_ctim.tv_sec && before_same.st_ctim.tv_nsec == after_same.st_ctim.tv_nsec &&
        stat.st_birthtim.tv_sec == after_same.st_birthtim.tv_sec && stat.st_birthtim.tv_nsec == after_same.st_birthtim.tv_nsec,
        "same-size truncate preserves change/write timestamps and I/O preserves birth time");
  Check(api.read(fd, nullptr, 0) == 0 && api.pread(fd, nullptr, 0, -1) == KERNEL_ERROR_EINVAL,
        "zero count avoids buffer access but positioned offsets still validate");
  auto* mapped = api.Map(fd, 1);
  mapped[100] = 0xa5;
  Check(api.truncate(fd, 101) == 0 && api.pread(fd, out.data(), 1, 100) == 1 && out[0] == 0xa5,
        "growth preserves existing padding written past old EOF");
  Check(api.truncate(fd, 2 * Page + 17) == 0, "growth across sparse extent boundaries");
  Check(api.pread(fd, out.data(), 4, Page) == 4 && out == std::array<uint8_t, 4> {}, "absent pages read as zeros");
  api.Unmap(mapped, 1);
  data.fill(0xc4);
  Check(api.pwrite(fd, data.data(), 4, 100) == 4 && api.pwrite(fd, data.data(), 4, 2 * Page) == 4,
        "materialize retained-tail and discarded-page data");
  Check(api.truncate(fd, 5) == 0 && api.truncate(fd, 2 * Page + 17) == 0 &&
        api.pread(fd, out.data(), 4, 100) == 4 && out == std::array<uint8_t, 4> {} &&
        api.pread(fd, out.data(), 4, 2 * Page) == 4 && out == std::array<uint8_t, 4> {},
        "no-map shrink zeroes retained page tail and discards full pages");
  Check(api.truncate(fd, 0) == 0 && api.truncate(fd, Page) == 0 &&
        api.pread(fd, out.data(), 4, 0) == 4 && out == std::array<uint8_t, 4> {}, "shrink-to-zero and regrow cannot resurrect bytes");
  auto* prefix = api.Map(fd, Page, 0, 1);
  MEMORY_BASIC_INFORMATION host {};
  Check(VirtualQuery(prefix, &host, sizeof(host)) != 0 && host.Protect == PAGE_READONLY,
        "read-only requested protection is installed during map publication");
  Check(api.truncate(fd, 3 * Page) == 0 && api.truncate(fd, 2 * Page) == 0 && prefix[0] == 0,
        "shrink can preserve live views strictly below the affected page");
  api.Unmap(prefix, Page);
  Check(api.truncate(fd, INT64_MAX) == 0 && api.stat(fd, &stat) == OK && stat.st_size == INT64_MAX &&
        stat.st_blocks == static_cast<int64_t>((uint64_t {INT64_MAX} + Page - 1) / Page),
        "logical growth is sparse, checked and has no invented object cap");
  Check(api.close(fd) == OK, "close sparse large object");
  CountsEqual(baseline, "EOF and zeroing fixtures release object extents");
}

static void UnsupportedAndValidation(const Api& api) {
  const int fd = api.New();
  Check(api.truncate(fd, 2 * Page) == 0, "size validation fixture");
  const auto baseline = Memory::TestShmCounts();
  auto reject = [&](size_t len, int prot, int flags, int64_t offset, int expected) {
    void* output = reinterpret_cast<void*>(uintptr_t {0x1234});
    Check(api.kernel_mmap(nullptr, len, prot, flags, fd, offset, &output) == expected &&
          output == reinterpret_cast<void*>(uintptr_t {0x1234}), "mmap failure preserves kernel output");
    Check(api.mmap(nullptr, len, prot, flags, fd, offset) == reinterpret_cast<void*>(-1) &&
          *Libs::Posix::GetErrorAddr() == Libs::LibKernel::KernelToPosix(expected), "libc consumed failure translates errno without copy fallback");
    CountsEqual(baseline, "unsupported/invalid mmap publishes no resource");
  };
  reject(0, 3, 1, 0, KERNEL_ERROR_EINVAL);
  reject(SIZE_MAX, 3, 1, 0, KERNEL_ERROR_EINVAL);
  reject(Page, 3, 1, -1, KERNEL_ERROR_EINVAL);
  reject(Page, 3, 1, 1, KERNEL_ERROR_EOPNOTSUPP);
  reject(Page, 3, 2, 0, KERNEL_ERROR_EOPNOTSUPP);
  reject(Page, 7, 1, 0, KERNEL_ERROR_EOPNOTSUPP);
  reject(Page, 0x13, 1, 0, KERNEL_ERROR_EOPNOTSUPP);
  reject(Page, 3, 1, 2 * Page, KERNEL_ERROR_EOPNOTSUPP);
  reject(Page, 0x80, 1, 0, KERNEL_ERROR_EINVAL);
  reject(Page, 3, 3, 0, KERNEL_ERROR_EINVAL);
  void* output = reinterpret_cast<void*>(uintptr_t {0x1234});
  Check(api.kernel_mmap(reinterpret_cast<void*>(0x400000000ull), Page, 3, 0x11, fd, 0, &output) == KERNEL_ERROR_EOPNOTSUPP &&
        output == reinterpret_cast<void*>(uintptr_t {0x1234}), "valid fixed SHM mapping is explicitly unsupported");
  Check(FileSystem::KernelFtruncate(-99, -1) == KERNEL_ERROR_EINVAL &&
        FileSystem::KernelFtruncate(-99, 0) == KERNEL_ERROR_EBADF, "negative truncate precedes fd validation");
  Check(api.mmap(nullptr, Page, 3, 1, -1, 0) == reinterpret_cast<void*>(-1) &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
        "fd minus one without MAP_ANON cannot fall into an anonymous private allocation");
  Check(api.pread(-99, nullptr, 0, -1) == KERNEL_ERROR_EBADF && api.pwrite(-99, nullptr, 0, -1) == KERNEL_ERROR_EBADF,
        "positioned I/O validates descriptor before negative offset");
  Check(api.open(reinterpret_cast<const char*>(1), 0, 0644) == -1 &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL, "read-only anonymous open rejected");
  Check(api.open(reinterpret_cast<const char*>(1), 0x200002, 0644) == -1 &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP, "O_SYSTEM budget operation rejected");
  Check(api.close(fd) == OK && api.mmap(nullptr, Page, 3, 1, fd, 0) == reinterpret_cast<void*>(-1) &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF, "closed fd cannot fall into private-copy success");
  Check(api.read(fd, nullptr, 1) == KERNEL_ERROR_EBADF && api.write(fd, nullptr, 1) == KERNEL_ERROR_EBADF &&
        api.pread(fd, nullptr, 1, 0) == KERNEL_ERROR_EBADF && api.pwrite(fd, nullptr, 1, 0) == KERNEL_ERROR_EBADF,
        "closed descriptor validation precedes nonzero null-buffer checks");
  Check(api.read(fd, nullptr, SIZE_MAX) == KERNEL_ERROR_EINVAL && api.write(fd, nullptr, SIZE_MAX) == KERNEL_ERROR_EINVAL &&
        api.pread(fd, nullptr, SIZE_MAX, 0) == KERNEL_ERROR_EINVAL && api.pwrite(fd, nullptr, SIZE_MAX, 0) == KERNEL_ERROR_EINVAL,
        "scalar count limit precedes descriptor lookup");
  output = reinterpret_cast<void*>(uintptr_t {0x1234});
  Check(api.kernel_mmap(nullptr, Page, 3, 1, fd, 0, nullptr) == KERNEL_ERROR_EFAULT &&
        api.kernel_mmap(nullptr, 0, 3, 1, fd, 0, &output) == KERNEL_ERROR_EINVAL &&
        output == reinterpret_cast<void*>(uintptr_t {0x1234}), "common malformed mmap arguments precede descriptor dispatch");
}

static void FailureTransactions(const Api& api) {
  const auto initial = Memory::TestShmCounts();
  const auto descriptors = FileSystem::TestAnonymousShmDescriptorCount();
  const int fd = api.New();
  Check(api.truncate(fd, 12 * Page) == 0, "size failure fixture");
  auto* live = api.Map(fd, Page);
  live[0] = 0x78;
  const auto baseline = Memory::TestShmCounts();
  for (const auto kind: {Memory::ShmFailure::Prepare, Memory::ShmFailure::Section,
                         Memory::ShmFailure::Split, Memory::ShmFailure::Map, Memory::ShmFailure::Protect}) {
    // Prepare is three coarse checkpoints, NOT an allocator shim covering
    // every individual STL allocation. Protect exercises map-time protection.
    const uint32_t stages = kind == Memory::ShmFailure::Prepare ? 3 :
                            (kind == Memory::ShmFailure::Section ? 11 : 12);
    for (uint32_t stage = 0; stage < stages; ++stage) {
      Memory::TestFailShmAfter(kind, stage);
      void* output = reinterpret_cast<void*>(uintptr_t {0x5678});
      Check(api.kernel_mmap(nullptr, 12 * Page, 3, 1, fd, 0, &output) == KERNEL_ERROR_ENOMEM &&
            output == reinterpret_cast<void*>(uintptr_t {0x5678}) && live[0] == 0x78,
            "prepare/section/split/map/map-time-protection failure is atomic with an old alias live");
      Memory::TestFailShmAfter(Memory::ShmFailure::None, 0);
      CountsEqual(baseline, "failed mapping leaks no object, extent or view");
      Check(FileSystem::TestAnonymousShmDescriptorCount() == descriptors + 1, "failed map cannot leak or remove descriptors");
    }
  }
  auto* mapped = api.Map(fd, 3 * Page);
  mapped[Page] = 0x67;
  const auto mapped_counts = Memory::TestShmCounts();
  Memory::TestFailShmAfter(Memory::ShmFailure::Unmap, 1);
  Check(api.unmap(reinterpret_cast<uint64_t>(mapped), 3 * Page) == KERNEL_ERROR_EIO &&
        mapped[0] == 0x78 && mapped[Page] == 0x67, "failed exact unmap restores all prior views");
  CountsEqual(mapped_counts, "unmap rollback preserves view ownership counts");
  Memory::TestFailShmAfter(Memory::ShmFailure::Protect, 1);
  Check(Memory::KernelMprotect(mapped, 3 * Page, 1) == KERNEL_ERROR_EIO, "multi-page protection failure returns host error");
  for (size_t page = 0; page < 3; ++page) {
    MEMORY_BASIC_INFORMATION host {};
    Memory::VirtualQueryInfo query {};
    Check(VirtualQuery(mapped + page * Page, &host, sizeof(host)) != 0 && host.Protect == PAGE_READWRITE &&
          Memory::KernelVirtualQuery(mapped + page * Page, 0, &query, sizeof(query)) == OK && query.protection == 3,
          "mprotect rollback restores host and semantic protection for full span");
  }
  api.Unmap(mapped + Page, Page);
  Check(Memory::KernelMprotect(mapped, 3 * Page, 1) == KERNEL_ERROR_ENOMEM && mapped[0] == 0x78,
        "mprotect hole preflight changes no earlier page");
  api.Unmap(mapped, 3 * Page);
  Check(Memory::TestPlaceholderRangeIsFree(reinterpret_cast<uint64_t>(mapped), 3 * Page), "unmapped VA is reusable after every exact view is gone");
  api.Unmap(live, Page);
  Check(api.close(fd) == OK, "close failure fixture");
  Check(FileSystem::TestAnonymousShmDescriptorCount() == descriptors, "failure fixture releases its descriptor");
  CountsEqual(initial, "transaction fixtures release all resources");
}

static void PrtExclusion(const Api& api, Loader::SymbolDatabase& symbols) {
  using Set = int (KYTY_SYSV_ABI *)(int, void*, size_t);
  using Get = int (KYTY_SYSV_ABI *)(int, void**, size_t*);
  const auto set = Import<Set>(symbols, "BohYr-F7-is");
  const auto get = Import<Get>(symbols, "L0v2Go5jOuM");
  const auto baseline = Memory::TestShmCounts();
  void* aperture = nullptr;
  size_t aperture_size = 0;
  Check(get(2, &aperture, &aperture_size) == OK && aperture_size == 0,
        "isolated PRT fixture starts with empty slot two");
  const int fd = api.New();
  Check(api.truncate(fd, 3 * Page) == 0, "size PRT exclusion fixture");
  void* mapped = nullptr;
  Check(api.kernel_mmap(reinterpret_cast<void*>(0x1000000000ull), 3 * Page, 3, 1, fd, 0, &mapped) == OK,
        "create SHM in the legal PRT address window");
  const auto base = reinterpret_cast<uint64_t>(mapped);
  auto* bytes = static_cast<uint8_t*>(mapped);
  bytes[0] = 0x52;
  Check(base >= 0x0f00000000ull && base + 4 * Page <= 0xfc00000000ull &&
        Memory::TestPlaceholderRangeIsFree(base + 3 * Page, Page), "fixture has a neighboring free PRT page");
  Check(set(2, bytes + 3 * Page, Page) == OK, "establish disjoint old aperture before rejected replacement");
  const auto mapped_counts = Memory::TestShmCounts();
  Check(set(2, bytes + Page, Page) == KERNEL_ERROR_EOPNOTSUPP &&
        get(2, &aperture, &aperture_size) == OK && aperture == bytes + 3 * Page && aperture_size == Page && bytes[0] == 0x52,
        "SHM-first aperture rejection preserves old aperture metadata and live SHM");
  Memory::VirtualQueryInfo query {};
  Check(Memory::KernelVirtualQuery(mapped, 0, &query, sizeof(query)) == OK && !query.is_gpu_prt,
        "rejected aperture never marks SHM GPU-PRT visible");
  CountsEqual(mapped_counts, "rejected aperture leaves ownership and free VA unchanged");
  Check(set(2, nullptr, 0) == OK, "clear disjoint aperture");
  api.Unmap(mapped, 3 * Page);

  // The candidate straddles the aperture: containment-only IsInPrtAperture
  // would miss this overlap because the candidate starts before the aperture.
  Check(set(2, bytes + Page, Page) == OK, "install aperture over an otherwise free middle page");
  const auto aperture_counts = Memory::TestShmCounts();
  void* untouched = reinterpret_cast<void*>(uintptr_t {0x1234});
  Check(api.kernel_mmap(mapped, 3 * Page, 3, 1, fd, 0, &untouched) == KERNEL_ERROR_EOPNOTSUPP &&
        untouched == reinterpret_cast<void*>(uintptr_t {0x1234}) &&
        api.mmap(mapped, 3 * Page, 3, 1, fd, 0) == reinterpret_cast<void*>(-1) &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP,
        "aperture-first rejects both registered SHM mmap routes before consuming VA");
  Check(get(2, &aperture, &aperture_size) == OK && aperture == bytes + Page && aperture_size == Page &&
        Memory::TestPlaceholderRangeIsFree(base, 3 * Page), "aperture-first failure preserves registration and free placeholders");
  CountsEqual(aperture_counts, "aperture rejection allocates no object extents or views");
  Check(set(2, nullptr, 0) == OK && api.kernel_mmap(mapped, 3 * Page, 3, 1, fd, 0, &untouched) == OK && untouched == mapped,
        "same candidate becomes mappable once its PRT aperture is removed");
  api.Unmap(untouched, 3 * Page);
  Check(api.close(fd) == OK, "close PRT fixture");
  CountsEqual(baseline, "both operation-order fixtures release SHM ownership");
}

static void MixedOwnersAndProtection(const Api& api) {
  const auto baseline = Memory::TestShmCounts();
  const int fd = api.New();
  Check(api.truncate(fd, 3 * Page) == 0, "size mixed-owner fixture");
  auto* bytes = api.Map(fd, 3 * Page);
  api.Unmap(bytes + Page, Page);
  int64_t physical = 0;
  Check(Memory::KernelAllocateDirectMemory(0, static_cast<int64_t>(Memory::KernelGetDirectMemorySize()), Page, Page, 0, &physical) == OK,
        "allocate mixed-owner direct page");
  void* middle = bytes + Page;
  Check(Memory::KernelMapDirectMemory(&middle, Page, 3, 0x10, physical, Page) == OK && middle == bytes + Page,
        "place direct owner in SHM's unmapped middle hole");
  bytes[Page - 1] = 0x35;
  bytes[Page] = 0x46;
  bytes[2 * Page - 1] = 0x57;
  bytes[2 * Page] = 0x68;
  std::vector<uint8_t> result(Page + 2);
  for (bool sparse: {false, true}) {
    const auto start = reinterpret_cast<uint64_t>(bytes) + Page - 1;
    const bool ok = sparse ? Memory::TryReadSparseBacking(start, result.data(), result.size()) :
                             Memory::TryReadBacking(start, result.data(), result.size());
    Check(ok && result[0] == 0x35 && result[1] == 0x46 && result[Page] == 0x57 && result[Page + 1] == 0x68,
          "owner-aware backing reads cross SHM/direct/SHM without false zeros or wrong offsets");
  }
  const auto counts = Memory::TestShmCounts();
  Check(Memory::KernelMprotect(bytes, Page, 7) == KERNEL_ERROR_EOPNOTSUPP &&
        Memory::KernelMprotect(bytes, Page, 0x13) == KERNEL_ERROR_EOPNOTSUPP,
        "executable and GPU mprotect reject CPU-only SHM");
  Check(Memory::KernelMprotect(bytes, 3 * Page, 1) == KERNEL_ERROR_EOPNOTSUPP &&
        api.unmap(reinterpret_cast<uint64_t>(bytes), 3 * Page) == KERNEL_ERROR_EOPNOTSUPP,
        "mixed-owner mprotect and munmap preflight reject the whole span");
  for (size_t page = 0; page < 3; ++page) {
    MEMORY_BASIC_INFORMATION host {};
    Memory::VirtualQueryInfo query {};
    Check(VirtualQuery(bytes + page * Page, &host, sizeof(host)) != 0 && host.Protect == PAGE_READWRITE &&
          Memory::KernelVirtualQuery(bytes + page * Page, 0, &query, sizeof(query)) == OK && query.protection == 3 &&
          query.is_direct == (page == 1), "rejected mixed operations preserve host permissions and each owner");
  }
  Check(bytes[Page - 1] == 0x35 && bytes[Page] == 0x46 && bytes[2 * Page] == 0x68, "rejected mixed operations preserve bytes");
  CountsEqual(counts, "rejected mixed operations preserve SHM resource and free-VA counts");
  api.Unmap(middle, Page);
  Check(Memory::KernelReleaseDirectMemory(physical, Page) == OK, "release mixed fixture direct extent");
  api.Unmap(bytes, 3 * Page);
  Check(api.close(fd) == OK, "close mixed-owner fixture");
  CountsEqual(baseline, "mixed-owner fixture releases SHM resources");
}

static std::atomic<int> race_phase {0};
static void RetainedBarrier() {
  race_phase.store(1);
  while (race_phase.load() != 2) { std::this_thread::yield(); }
}
static void CloseVersusMap(const Api& api) {
  const auto baseline = Memory::TestShmCounts();
  const int fd = api.New();
  Check(api.truncate(fd, Page) == 0, "size close-vs-map fixture");
  race_phase.store(0);
  int result = OK;
  void* output = reinterpret_cast<void*>(uintptr_t {0x5678});
  std::thread mapper([&] {
    FileSystem::TestSetShmMmapRetainedCallback(RetainedBarrier);
    result = api.kernel_mmap(nullptr, Page, 3, 1, fd, 0, &output);
    FileSystem::TestSetShmMmapRetainedCallback(nullptr);
  });
  while (race_phase.load() != 1) { std::this_thread::yield(); }
  Check(api.close(fd) == OK, "close retained-but-not-locked mapping descriptor");
  const int next = api.New();
  Check(next == fd && api.truncate(next, Page) == 0, "reuse descriptor while old File is retained");
  race_phase.store(2);
  mapper.join();
  Check(result == KERNEL_ERROR_EBADF && output == reinterpret_cast<void*>(uintptr_t {0x5678}),
        "single retained lookup cannot map recycled descriptor's new object");
  Check(api.close(next) == OK, "close replacement descriptor");
  CountsEqual(baseline, "close-vs-map releases both descriptor objects");
}
} // namespace ShmFixture

static void TestAnonymousShm(Loader::SymbolDatabase& symbols) {
  const ShmFixture::Api api(symbols);
  ShmFixture::CoherenceAndLifetime(api);
  ShmFixture::EofAndZeroing(api);
  ShmFixture::UnsupportedAndValidation(api);
  ShmFixture::FailureTransactions(api);
  ShmFixture::PrtExclusion(api, symbols);
  ShmFixture::MixedOwnersAndProtection(api);
  ShmFixture::CloseVersusMap(api);
}

// Optional alongside the core fixture, using only the runner's disposable root.
static void TestAnonymousShmFileRegressions(Loader::SymbolDatabase& symbols, const std::filesystem::path& root) {
  using namespace ShmFixture;
  const Api api(symbols);
  FileSystem::Mount(root, "/temp0");
  const char* name = "/ScePsmSharedDmem_anonymous_shm_fixture";
  const int named = api.open(name, 0x802, 0600);
  FileSystem::FileStat stat {};
  Check(named >= 3 && api.stat(named, &stat) == OK && stat.st_size == 0x100000,
        "existing PSM named fallback still creates its one-MiB file backing");
  auto* first = api.Map(named, Page);
  first[4] = 0x83;
  const int peer = api.open(name, 2, 0600);
  Check(peer >= 3 && api.Map(peer, Page) == first && first[4] == 0x83,
        "legacy named PSM cache route remains intact");
  Check(api.close(named) == OK && api.close(peer) == OK, "close named regression descriptors");
  api.Unmap(first, Page);
  const int resource = FileSystem::KernelOpen("/temp0/shm-private-resource", 0x602, 0600);
  const std::array<uint8_t, 4> data {0x19, 0x27, 0x53, 0x71};
  Check(resource >= 3 && api.write(resource, data.data(), data.size()) == data.size(), "create disposable private resource");
  void* copy = nullptr;
  Check(api.kernel_mmap(nullptr, Page, 1, 2, resource, 0, &copy) == OK &&
        std::memcmp(copy, data.data(), data.size()) == 0 && static_cast<uint8_t*>(copy)[4] == 0,
        "ordinary private resource mmap preserves its partial-page copy behavior");
  void* preserved = reinterpret_cast<void*>(uintptr_t {0x1234});
  Check(api.kernel_mmap(nullptr, Page, 1, 2, resource, 1, &preserved) == KERNEL_ERROR_EINVAL &&
        preserved == reinterpret_cast<void*>(uintptr_t {0x1234}), "ordinary-file offset alignment retains its EINVAL contract");

  static uint64_t invalidated_address = 0, invalidated_size = 0, invalidation_calls = 0;
  invalidation_calls = 0;
  Memory::TestSetMemoryInvalidationObserver(+[](uint64_t address, uint64_t size) {
    invalidated_address = address;
    invalidated_size = size;
    ++invalidation_calls;
  });
  auto* libc_copy = api.mmap(nullptr, Page, 3, 2, resource, 0);
  Check(libc_copy != reinterpret_cast<void*>(-1) && libc_copy != nullptr && libc_copy != copy &&
        std::memcmp(libc_copy, data.data(), data.size()) == 0 && static_cast<uint8_t*>(libc_copy)[4] == 0 &&
        invalidation_calls == 1 && invalidated_address == reinterpret_cast<uint64_t>(libc_copy) && invalidated_size == data.size(),
        "ordinary registered LIBC mmap performs real EOF-bounded GPU invalidation on its allocated destination");
  api.Unmap(libc_copy, Page);

  std::shared_ptr<FileSystem::File> retained;
  int dispatch_error = OK;
  Check(!FileSystem::TryMmapAnonymous(resource, nullptr, Page, 3, 2, 0, &preserved, &dispatch_error, &retained),
        "ordinary dispatch retains the descriptor for subsequent file reads");
  std::array<uint8_t, 8> tail {};
  invalidation_calls = 0;
  Check(api.seek(resource, 2, 0) == 2 && FileSystem::MmapFileRead(retained, tail.data(), tail.size(), 3) == 1 &&
        tail[0] == data[3] && api.seek(resource, 0, 1) == 2 && invalidation_calls == 1 && invalidated_size == 1,
        "retained ordinary read bounds both invalidation and transfer and restores file position");
  invalidation_calls = 0;
  Check(FileSystem::MmapFileRead(retained, nullptr, 8, 4) == 0 && invalidation_calls == 0,
        "retained ordinary EOF read neither invalidates nor accesses the buffer");

  static int closing_fd = -1, replacement_fd = -1;
  closing_fd = resource;
  replacement_fd = -1;
  const auto before_race = Memory::TestShmCounts();
  FileSystem::TestSetMmapFileReadBeforeLockCallback(+[] {
    FileSystem::TestSetMmapFileReadBeforeLockCallback(nullptr);
    Check(FileSystem::KernelClose(closing_fd) == OK, "close ordinary file after libc captured path/size");
    replacement_fd = FileSystem::KernelShmOpenAnonymous(2, 0600);
    Check(replacement_fd == closing_fd && FileSystem::KernelFtruncate(replacement_fd, Page) == OK,
          "replace closed ordinary descriptor slot with SHM before retained read");
  });
  Check(api.mmap(nullptr, Page, 3, 2, resource, 0) == reinterpret_cast<void*>(-1) &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF && invalidation_calls == 0,
        "registered libc retained read rejects close/reuse instead of copying the replacement SHM");
  Memory::TestSetMemoryInvalidationObserver(nullptr);
  Check(api.close(replacement_fd) == OK, "close replacement SHM from ordinary read race");
  CountsEqual(before_race, "failed ordinary libc mapping frees temporary VA and replacement SHM ownership");
  api.Unmap(copy, Page);
  const int readonly = FileSystem::KernelOpen("/temp0/shm-private-resource", 0, 0);
  Check(readonly >= 3 && FileSystem::KernelFtruncate(readonly, data.size()) == KERNEL_ERROR_EBADF &&
         api.close(readonly) == OK, "ordinary ftruncate retains EBADF for a read-only descriptor even at unchanged length");
  FileSystem::Umount("/temp0");
}

// Run LAST in isolated --anonymous-shm-only fixture; this deliberately shuts
// down both subsystems. Separate invocations exercise either teardown order.
static void TestAnonymousShmShutdown(Loader::SymbolDatabase& symbols, bool descriptors_first) {
  using namespace ShmFixture;
  const Api api(symbols);
  const int open_fd = api.New();
  Check(api.truncate(open_fd, 3 * Page) == 0, "size shutdown fixture");
  auto* live = api.Map(open_fd, 3 * Page);
  live[0] = 0x32;
  Memory::TestFailShmAfter(Memory::ShmFailure::Map, 1);
  Memory::TestFailShmAfter(Memory::ShmFailure::Unmap, 0);
  void* unchanged = reinterpret_cast<void*>(uintptr_t {0x9876});
  Check(api.kernel_mmap(nullptr, 3 * Page, 3, 1, open_fd, 0, &unchanged) == KERNEL_ERROR_EIO &&
        unchanged == reinterpret_cast<void*>(uintptr_t {0x9876}) && Memory::TestShmCounts().quarantined == 1 && live[0] == 0x32,
        "failed rollback quarantines live view and preserves existing alias");
  auto* subsequent = api.Map(open_fd, 3 * Page);
  subsequent[1] = 0x45;
  Check(live[1] == 0x45 && Memory::TestShmCounts().quarantined == 1, "allocator skips quarantined live VA");
  if (descriptors_first) { FileSystem::Shutdown(); Memory::Shutdown(); }
  else { Memory::Shutdown(); FileSystem::Shutdown(); }
  CountsEqual({0, 0, 0, 0, 0}, "shutdown drains ordinary and quarantined views before owned reservations");
  Check(FileSystem::TestAnonymousShmDescriptorCount() == 0, "shutdown drains descriptors in either lifecycle order");
}

// Main may expose a death-test child mode. A failed restoration of an existing
// view is unrecoverable; the syscall must never return a recoverable error with
// a missing view and apparently-live metadata.
static void TestAnonymousShmFatalRestore(Loader::SymbolDatabase& symbols) {
  using namespace ShmFixture;
  const Api api(symbols);
  const int fd = api.New();
  Check(api.truncate(fd, 2 * Page) == 0, "size fatal-restore fixture");
  auto* mapped = api.Map(fd, 2 * Page);
  Memory::TestFailShmAfter(Memory::ShmFailure::Unmap, 1);
  Memory::TestFailShmAfter(Memory::ShmFailure::Map, 0);
  (void)api.unmap(reinterpret_cast<uint64_t>(mapped), 2 * Page);
  Check(false, "unrecoverable exact-view restoration must not return");
}

static void TestAnonymousShmFatalProtectionRestore(Loader::SymbolDatabase& symbols) {
  using namespace ShmFixture;
  const Api api(symbols);
  const int fd = api.New();
  Check(api.truncate(fd, 2 * Page) == 0, "size fatal protection-restore fixture");
  auto* mapped = api.Map(fd, 2 * Page);
  Memory::TestFailShmAfter(Memory::ShmFailure::Protect, 1);
  Memory::TestFailShmAfter(Memory::ShmFailure::ProtectRestore, 0);
  (void)Memory::KernelMprotect(mapped, 2 * Page, 1);
  Check(false, "unrecoverable protection restoration must not return");
}
