// Include inside KernelFileSystemTests.cpp's test namespace. Call
// TestPosixVectorTime(root) with FileSystem initialized and a temporary root.
// No subsystem/build integration here: the parent owns that translation unit.
// Firmware ABI/NIDs: docs/vsh-run19-kernel-imports-20261104.md sections 1 and 6.
namespace PosixVectorTimeTests {
namespace FS = Libs::LibKernel::FileSystem;
namespace Posix = Libs::Posix;
namespace Net = Libs::Network::Net;
using namespace Libs::LibKernel;

struct Api {
  Loader::SymbolDatabase symbols;
  decltype(&FS::KernelReadv) readv = nullptr;
  int (KYTY_SYSV_ABI *futimes)(int, const KernelTimeval*) = nullptr;
  Api() {
    Libs::InitLibKernel_1(&symbols);
    Loader::SymbolResolve read_query {"QqxBetgJH+g", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    Loader::SymbolResolve time_query {"+0EDo7YzcoU", "libScePosix", 1, "libkernel", 1, 1, Loader::SymbolType::Func};
    const auto* read = symbols.FindRegistration(read_query, "FileSystem::KernelReadv");
    const auto* time = symbols.FindRegistration(time_query, "Posix::futimes");
    Check(read != nullptr && time != nullptr && read->vaddr == reinterpret_cast<uint64_t>(&FS::KernelReadv),
          "readv/futimes exact function NIDs and firmware library/module versions resolve");
    readv = reinterpret_cast<decltype(readv)>(read->vaddr);
    futimes = reinterpret_cast<decltype(futimes)>(time->vaddr);
    for (auto query : {read_query, time_query}) {
      const char* expression = query.name == read_query.name ? "FileSystem::KernelReadv" : "Posix::futimes";
      query.library_version = 2;
      Check(symbols.FindRegistration(query, expression) == nullptr, "no readv/futimes library-version alias");
      query.library_version = 1;
      query.module_version_minor = 0;
      Check(symbols.FindRegistration(query, expression) == nullptr, "no readv/futimes module-version alias");
      query.module_version_minor = 1;
      query.type = Loader::SymbolType::Object;
      Check(symbols.FindRegistration(query, expression) == nullptr, "readv/futimes are not object exports");
    }
  }
};

void Seed(const char* path, const char* bytes, size_t size) {
  const int fd = FS::KernelOpen(path, 0x602, 0600);
  Check(fd >= 3 && FS::KernelWrite(fd, bytes, size) == static_cast<int64_t>(size) && FS::KernelClose(fd) == 0,
        "vector/time fixture writes and closes a real temporary backing file");
}

void FileReads(Api& api) {
  Seed("/vector-time/data", "abcdefghij", 10);
  const int fd = FS::KernelOpen("/vector-time/data", 0, 0);
  Check(fd >= 3 && FS::KernelLseek(fd, 1, 0) == 1, "readv starts at a nonzero sequential position");
  char first[3] {}, second[4] {'!', '!', '!', '!'};
  FS::KernelIovec vectors[] {{nullptr, 0}, {first, 3}, {second, 2}};
  Check(api.readv(fd, vectors, 3) == 5 && std::memcmp(first, "bcd", 3) == 0 &&
            std::memcmp(second, "ef!!", 4) == 0 && FS::KernelLseek(fd, 0, 1) == 6,
        "readv scatters in order, skips empty elements, and advances by transferred bytes");
  FS::KernelIovec positioned {first, 3};
  Check(FS::KernelPreadv(fd, &positioned, 1, 0) == 3 && std::memcmp(first, "abc", 3) == 0 &&
            FS::KernelLseek(fd, 0, 1) == 6,
        "preadv shares vector reading but preserves the sequential position");
  vectors[2].iov_len = 4;
  Check(api.readv(fd, vectors, 3) == 4 && std::memcmp(first, "ghi", 3) == 0 &&
            std::memcmp(second, "jf!!", 4) == 0 && FS::KernelLseek(fd, 0, 1) == 10,
        "short vector read returns actual bytes and preserves the untouched output tail");
  Check(api.readv(fd, vectors, 3) == 0 && FS::KernelLseek(fd, 0, 1) == 10,
        "readv returns genuine backing EOF without advancing");
  Check(api.readv(fd, nullptr, 0) == 0 && FS::KernelClose(fd) == 0,
        "zero vectors are valid for an open file");
  Check(api.readv(fd, nullptr, 0) == static_cast<int64_t>(KERNEL_ERROR_EBADF) &&
            api.readv(-1, nullptr, 0) == static_cast<int64_t>(KERNEL_ERROR_EBADF),
        "readv returns sign-extended 64-bit kernel errors, never a POSIX -1");
}

void InvalidAndPartialReads(Api& api) {
  const int fd = FS::KernelOpen("/vector-time/data", 0, 0);
  Check(fd >= 3, "open vector validation fixture");
  char output[4] {};
  FS::KernelIovec vector {output, 1};
  Check(api.readv(fd, &vector, -1) == KERNEL_ERROR_EINVAL &&
            api.readv(fd, &vector, 1025) == KERNEL_ERROR_EINVAL &&
            api.readv(fd, nullptr, 1) == KERNEL_ERROR_EFAULT &&
            api.readv(fd, reinterpret_cast<const FS::KernelIovec*>(1), 1) == KERNEL_ERROR_EFAULT,
        "readv validates count and fault-contained iovec copyin before dereferencing guest storage");
  vector.iov_len = static_cast<size_t>(INT32_MAX) + 1;
  Check(api.readv(fd, &vector, 1) == KERNEL_ERROR_EINVAL, "readv retains the documented emulator INT_MAX cap");
  vector.iov_len = std::numeric_limits<size_t>::max();
  Check(api.readv(fd, &vector, 1) == KERNEL_ERROR_EINVAL, "readv rejects size overflow");
  FS::KernelIovec sum[] {{output, static_cast<size_t>(INT32_MAX)}, {output, 1}};
  Check(api.readv(fd, sum, 2) == KERNEL_ERROR_EINVAL && FS::KernelLseek(fd, 0, 1) == 0,
        "readv rejects aggregate length overflow without consuming bytes");
  vector = {reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 1), 4};
  Check(api.readv(fd, &vector, 1) == KERNEL_ERROR_EFAULT && FS::KernelLseek(fd, 0, 1) == 0,
        "readv rejects wrapped destination ranges without advancing");
  FS::KernelIovec partial[] {{output, 3}, {reinterpret_cast<void*>(1), 2}};
  Check(api.readv(fd, partial, 2) == 3 && std::memcmp(output, "abc", 3) == 0 &&
            FS::KernelLseek(fd, 0, 1) == 3,
        "a later element fault returns already transferred bytes and their actual sequential offset");
  Check(FS::KernelPreadv(fd, partial, 2, 0) == 3 && FS::KernelLseek(fd, 0, 1) == 3,
        "partial preadv fault still restores the original file position");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
  // A mapped but read-only destination must fail, not bypass guest permissions.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Check(page != nullptr, "allocate vector permission fixture");
  DWORD old = 0;
  Check(VirtualProtect(page, 4096, PAGE_READONLY, &old) != 0, "protect vector destination read-only");
#else
  const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED && mprotect(page, page_size, PROT_READ) == 0, "protect vector destination read-only");
#endif
  vector = {page, 1};
  Check(api.readv(fd, &vector, 1) == KERNEL_ERROR_EFAULT && FS::KernelLseek(fd, 0, 1) == 3,
        "readv enforces actual guest destination permissions without consuming file data");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Check(VirtualFree(page, 0, MEM_RELEASE) != 0, "release vector permission fixture");
#else
  Check(munmap(page, page_size) == 0, "release vector permission fixture");
#endif
#endif
  Check(FS::KernelClose(fd) == 0, "close vector validation fixture");
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
thread_local int retained_fd = -1;
thread_local int replacement_fd = -1;
void ReplaceRetainedFile() {
  FS::TestSetReadvRetainedCallback(nullptr);
  Check(FS::KernelClose(retained_fd) == 0, "close retained vector descriptor before its operation lock");
  replacement_fd = FS::KernelOpen("/vector-time/replacement", 0, 0);
  Check(replacement_fd == retained_fd, "force fd-table slot reuse while old File remains retained");
}
void DescriptorReuse(Api& api) {
  Seed("/vector-time/replacement", "replacement", 11);
  retained_fd = FS::KernelOpen("/vector-time/data", 0, 0);
  Check(retained_fd >= 3, "open retained vector descriptor");
  char output = '!';
  FS::KernelIovec vector {&output, 1};
  FS::TestSetReadvRetainedCallback(ReplaceRetainedFile);
  Check(api.readv(retained_fd, &vector, 1) == KERNEL_ERROR_EBADF && output == '!' &&
            FS::KernelLseek(replacement_fd, 0, 1) == 0,
        "readv never dispatches to a new descriptor occupying the retained File's old fd");
  Check(FS::KernelClose(replacement_fd) == 0, "close reused vector descriptor");
}
#endif

void PipeAndDeviceReads(Api& api) {
  int pair[2] {-1, -1};
  Check(FS::KernelCreatePipe(pair) == 0 && FS::KernelFcntl(pair[0], 4, 4) == 0,
        "create a real nonblocking pipe vector backend");
  char first = '!', rest[4] {'!', '!', '!', '!'};
  FS::KernelIovec vectors[] {{&first, 1}, {rest, 4}};
  Check(api.readv(pair[0], vectors, 2) == static_cast<int64_t>(KERNEL_ERROR_EAGAIN),
        "empty nonblocking pipe uses kernel EAGAIN, not fabricated EOF");
  Check(FS::KernelWrite(pair[1], "xyz", 3) == 3 && api.readv(pair[0], vectors, 2) == 3 &&
            first == 'x' && std::memcmp(rest, "yz!!", 4) == 0,
        "pipe readv uses one real read and scatters a short transfer across vectors");
  Check(FS::KernelPreadv(pair[0], vectors, 2, 0) == KERNEL_ERROR_ESPIPE &&
            FS::KernelClose(pair[1]) == 0 && api.readv(pair[0], vectors, 2) == 0 &&
            FS::KernelClose(pair[0]) == 0,
        "pipe positioned reads fail ESPIPE and sequential peer-close reads reach real EOF");
  const int random = FS::KernelOpen("/dev/urandom", 0, 0);
  Check(random >= 3 && api.readv(random, vectors, 2) == 5 && FS::KernelClose(random) == 0,
        "readv routes random-device vectors through the current scalar device backend");
}

void SocketReads(Api& api) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
  int pair[2] {-1, -1};
  Check(Net::Socketpair(1, 1, 0, pair) == 0, "create real AF_LOCAL stream transports for readv");
  Check(Net::Send(pair[1], "socket", 6, 0) == 6 && Net::Shutdown(pair[1], 1) == 0,
        "queue socket vector payload and a bounded EOF");
  char first[2] {}, rest[8] {};
  FS::KernelIovec vectors[] {{first, 2}, {rest, 8}};
  Check(api.readv(pair[0], vectors, 2) == 6 && std::memcmp(first, "so", 2) == 0 &&
            std::memcmp(rest, "cket", 4) == 0 && rest[4] == 0,
        "readv scatters one real socket receive, not ESPIPE or separate vector receives");
  Check(api.readv(pair[0], vectors, 2) == 0 && FS::KernelPreadv(pair[0], vectors, 2, 0) == KERNEL_ERROR_ESPIPE,
        "stream readv observes peer shutdown while preadv remains nonseekable");
  Check(Net::SocketClose(pair[0]) == 0 && Net::SocketClose(pair[1]) == 0, "close socket vector transports");
  Check(api.readv(pair[0], vectors, 2) == KERNEL_ERROR_EBADF, "closed socket vector descriptor returns EBADF");
#endif
}

void CheckTimes(int fd, const KernelTimeval (&expected)[2], const char* message) {
  FS::FileStat info {};
  Check(FS::KernelFstat(fd, &info) == 0 && info.st_atim.tv_sec == expected[0].tv_sec &&
            info.st_atim.tv_nsec == expected[0].tv_usec * 1000 && info.st_mtim.tv_sec == expected[1].tv_sec &&
            info.st_mtim.tv_nsec == expected[1].tv_usec * 1000, message);
}

void TimestampPolicy(Api& api, const std::filesystem::path& root) {
  Seed("/vector-time/timestamps", "time", 4);
  const auto path = root / "timestamps";
  const int fd = FS::KernelOpen("/vector-time/timestamps", 2, 0);
  Check(fd >= 3, "open real read/write descriptor for retained-handle futimes");
  KernelTimeval values[2] {{1700000000, 123456}, {1700000100, 654321}};
  *Posix::GetErrorAddr() = Posix::POSIX_EINTR;
  Check(api.futimes(fd, values) == 0 && *Posix::GetErrorAddr() == Posix::POSIX_EINTR,
        "explicit futimes succeeds through the retained handle without modifying guest errno");
  CheckTimes(fd, values, "real descriptor atime/mtime preserve all guest microseconds, not DateTime milliseconds");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  WIN32_FILE_ATTRIBUTE_DATA attributes {};
  Check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != 0,
        "independent host filesystem query reads the actual written timestamp");
  const uint64_t ticks = (static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32u) |
                        attributes.ftLastWriteTime.dwLowDateTime;
  Check(ticks == static_cast<uint64_t>(values[1].tv_sec + 11644473600LL) * 10000000 + values[1].tv_usec * 10,
        "actual host FILETIME contains the requested Unix timestamp and all fractional microseconds");
#else
  Common::File query;
  Common::FileTimestamp host[2] {};
  std::error_code error;
  Check(query.OpenMetadata(path, error) && query.GetTimestamps(host, error) &&
            host[1].seconds == values[1].tv_sec && host[1].nanoseconds == values[1].tv_usec * 1000,
        "separate retained host descriptor observes the actual futimens seconds and fractional microseconds");
  query.Close();
#endif
  for (const auto valid_usec : {int64_t(0), int64_t(999999)}) {
    values[0].tv_usec = valid_usec;
    Check(api.futimes(fd, values) == 0, "futimes accepts both microsecond endpoints");
    CheckTimes(fd, values, "valid microsecond endpoints round-trip exactly");
  }
  const auto before = std::filesystem::last_write_time(path);
  for (size_t index = 0; index < 2; ++index) {
    for (const auto bad_usec : {int64_t(-1), int64_t(1000000), std::numeric_limits<int64_t>::max()}) {
      KernelTimeval bad[2] {values[0], values[1]};
      bad[index].tv_usec = bad_usec;
      Check(api.futimes(fd, bad) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
            "futimes rejects invalid or overflowing microseconds before applying either timestamp");
      CheckTimes(fd, values, "invalid timeval pair leaves both real timestamps unchanged");
    }
  }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  // Windows FILETIME conversion has a narrower representable range than guest
  // int64 seconds. Unix follows its native time_t/filesystem range instead.
  for (const auto bad_seconds : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
                                  int64_t(-11644473600LL)}) {
    KernelTimeval bad[2] {values[0], {bad_seconds, 0}};
    Check(api.futimes(fd, bad) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EOVERFLOW,
          "unrepresentable FILETIME or zero/no-change sentinel returns guest EOVERFLOW");
    CheckTimes(fd, values, "timestamp conversion overflow never partially mutates the atime/mtime pair");
  }
#endif
  Check(api.futimes(fd, reinterpret_cast<const KernelTimeval*>(1)) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EFAULT && std::filesystem::last_write_time(path) == before,
        "futimes fault-contained copyin leaves real timestamps untouched");
  const int ro = FS::KernelOpen("/vector-time-ro/timestamps", 0, 0);
  Check(ro >= 3 && api.futimes(ro, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EROFS &&
            FS::KernelClose(ro) == 0 && std::filesystem::last_write_time(path) == before,
        "futimes preserves mount read-only policy without touching the backing inode");
  const int read_only_fd = FS::KernelOpen("/vector-time/timestamps", 0, 0);
  Check(read_only_fd >= 3, "ordinary read-only opens remain usable without requesting extra write rights");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Check(api.futimes(read_only_fd, values) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EACCES,
        "Windows existing GENERIC_READ handle reports real SetFileTime access denial, not unsupported");
  CheckTimes(fd, values, "host attribute access denial leaves both timestamps unchanged");
#else
  Check(api.futimes(read_only_fd, values) == 0, "Unix futimens uses retained read fd with owner permission rules");
#endif
  Check(FS::KernelClose(read_only_fd) == 0, "close read-only timestamp descriptor");
  const auto start = std::chrono::system_clock::now();
  Check(api.futimes(fd, nullptr) == 0, "null futimes pair sets both actual host timestamps to now");
  const auto finish = std::chrono::system_clock::now();
  FS::FileStat current {};
  Check(FS::KernelFstat(fd, &current) == 0 && current.st_atim.tv_sec == current.st_mtim.tv_sec &&
            current.st_atim.tv_nsec == current.st_mtim.tv_nsec &&
            current.st_mtim.tv_sec >= std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count() &&
            current.st_mtim.tv_sec <= std::chrono::duration_cast<std::chrono::seconds>(finish.time_since_epoch()).count(),
        "current-time mutation samples the real clock once for access and modification time");
  Check(FS::KernelClose(fd) == 0 && api.futimes(fd, nullptr) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EBADF, "closed futimes descriptor uses POSIX EBADF");
}

void TimestampIdentity(Api& api, const std::filesystem::path& root) {
  Seed("/vector-time/time-original", "original", 8);
  const int original = FS::KernelOpen("/vector-time/time-original", 2, 0);
  Check(original >= 3, "retain timestamp identity fixture");
  FS::FileStat identity {};
  Check(FS::KernelFstat(original, &identity) == 0, "snapshot original inode identity");
  Check(FS::KernelRename("/vector-time/time-original", "/vector-time/time-renamed") == 0,
        "rename an inode while its timestamp descriptor remains open");
  Seed("/vector-time/time-original", "new path", 8);
  const auto replacement_before = std::filesystem::last_write_time(root / "time-original");
  KernelTimeval first[2] {{1700000200, 246801}, {1700000300, 135792}};
  Check(api.futimes(original, first) == 0, "futimes mutates a retained renamed inode");
  CheckTimes(original, first, "renamed original descriptor has requested timestamps");
  Common::File renamed;
  std::error_code error;
  Common::FileTimestamp host[2] {};
  Check(renamed.OpenMetadata(root / "time-renamed", error) && renamed.GetTimestamps(host, error) &&
            host[0].seconds == first[0].tv_sec && host[0].nanoseconds == first[0].tv_usec * 1000 &&
            host[1].seconds == first[1].tv_sec && host[1].nanoseconds == first[1].tv_usec * 1000 &&
            std::filesystem::last_write_time(root / "time-original") == replacement_before,
        "host renamed inode changed, while the replacement at the original path did not");
  renamed.Close();
  Check(FS::KernelUnlink("/vector-time/time-renamed") == 0, "unlink the original inode with a retained handle");
  KernelTimeval second[2] {{1700000400, 345678}, {1700000500, 987654}};
  Check(api.futimes(original, second) == 0, "retained-handle futimes works after unlink without path lookup");
  CheckTimes(original, second, "unlinked original descriptor still reports the requested real timestamps");
  FS::FileStat after {};
  Check(FS::KernelFstat(original, &after) == 0 && after.st_dev == identity.st_dev && after.st_ino == identity.st_ino &&
            std::filesystem::last_write_time(root / "time-original") == replacement_before && FS::KernelClose(original) == 0,
        "timestamp updates preserve retained inode identity and never target the replacement path");
}

void TimestampCapabilities(Api& api, const std::filesystem::path& root) {
  int pair[2] {-1, -1};
  Check(FS::KernelCreatePipe(pair) == 0 && api.futimes(pair[0], nullptr) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP &&
            FS::KernelClose(pair[0]) == 0 && FS::KernelClose(pair[1]) == 0,
        "unsupported timestamp file kinds report POSIX errno 45, never successful no-ops");
  Common::File memory;
  std::error_code error;
  Common::FileTimestamp values[2] {{1700000000, 123456000}, {1700000100, 654321000}};
  Common::File invalid;
  Check(!invalid.SetTimestamps(values, error) && error == std::errc::bad_file_descriptor,
        "closed Common::File timestamp setter reports a genuine invalid handle");
  Common::File native(root / "timestamps", Common::File::Mode::ReadWrite);
  Common::FileTimestamp saved[2] {}, after[2] {};
  Check(!native.IsInvalid() && native.GetTimestamps(saved, error), "snapshot direct platform setter fixture");
  Common::FileTimestamp bad[2] {values[0], {values[1].seconds, 1000000000}};
  Check(!native.SetTimestamps(bad, error) && error == std::errc::invalid_argument &&
            native.GetTimestamps(after, error) && saved[0].seconds == after[0].seconds &&
            saved[0].nanoseconds == after[0].nanoseconds && saved[1].seconds == after[1].seconds &&
            saved[1].nanoseconds == after[1].nanoseconds,
        "platform timestamp setter validates the full nanosecond pair before mutating either native field");
  native.Close();
  Check(memory.CreateInMem(), "create non-host memory timestamp fixture");
  uint32_t written = 0;
  memory.Write("memory", 6, &written);
  Check(written == 6 && !memory.SetTimestamps(values, error) && error == std::errc::operation_not_supported &&
            !memory.SetTimestamps(nullptr, error) && error == std::errc::operation_not_supported &&
            memory.Size() == 6 && memory.Tell() == 6,
        "in-memory backend explicitly rejects timestamp mutation without changing content/offset");
  const auto archive_path = root / "timestamp-archive.zar";
  const std::array<uint8_t, 4> payload {1, 2, 3, 4};
  Check(ArchiveTests::CreateArchive(archive_path, payload), "create archive timestamp capability fixture");
  const auto before = std::filesystem::last_write_time(archive_path);
  Common::File archive;
  Check(archive.Open(Common::MakeArchivePath(archive_path) / "assets/subdir/data.bin", Common::File::Mode::Read) &&
            !archive.SetTimestamps(values, error) && error == std::errc::operation_not_supported &&
            !archive.SetTimestamps(nullptr, error) && error == std::errc::operation_not_supported &&
            std::filesystem::last_write_time(archive_path) == before,
        "archive backend never substitutes mutation of its container or reports synthetic success");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Common::File metadata;
  Check(metadata.OpenMetadata(root, error) && !metadata.SetTimestamps(values, error) &&
            error.category() == std::system_category() && error.value() == ERROR_ACCESS_DENIED,
        "metadata-only Windows directory handles keep read rights and expose the real attribute-write error");
#endif
}
} // namespace PosixVectorTimeTests

// Optional additional coverage: requires initialized Memory (including its
// Windows copyout entry), unlike TestPosixVectorTime's FS-only fixture.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void TestPosixVectorTimeShm() {
  using namespace PosixVectorTimeTests;
  Api api;
  const int fd = FS::KernelShmOpenAnonymous(0x202, 0600);
  Check(fd >= 3 && FS::KernelFtruncate(fd, 5) == 0 && FS::KernelWrite(fd, "shmem", 5) == 5 &&
            FS::KernelLseek(fd, 0, 0) == 0, "create real anonymous SHM sequential vector backing");
  char first[2] {}, second[5] {};
  FS::KernelIovec vectors[] {{first, 2}, {second, 5}};
  Check(api.readv(fd, vectors, 2) == 5 && std::memcmp(first, "sh", 2) == 0 &&
            std::memcmp(second, "mem", 3) == 0 && second[3] == 0 && FS::KernelLseek(fd, 0, 1) == 5 &&
            api.readv(fd, vectors, 2) == 0 && FS::KernelClose(fd) == 0,
        "readv uses real SHM transfer/position accounting and reaches backing EOF");
}
#endif

void TestPosixVectorTime(const std::filesystem::path& root) {
  using namespace PosixVectorTimeTests;
  static_assert(sizeof(KernelTimeval) == 16 && sizeof(FS::KernelIovec) == 16);
  Api api;
  const auto backing = root / "vector-time";
  std::filesystem::create_directories(backing);
  FS::Mount(backing, "/vector-time");
  FS::Mount(backing, "/vector-time-ro", true);
  FileReads(api);
  InvalidAndPartialReads(api);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
  DescriptorReuse(api);
#endif
  PipeAndDeviceReads(api);
  SocketReads(api);
  TimestampPolicy(api, backing);
  TimestampIdentity(api, backing);
  TimestampCapabilities(api, backing);
  FS::Umount("/vector-time-ro");
  FS::Umount("/vector-time");
}
