// Included in the memory fixture: exercise real libkernel providers without
// firmware or a graphics context, using synthetic regular and archive files.
namespace MmapFs = Libs::LibKernel::FileSystem;
namespace MmapMemory = Libs::LibKernel::Memory;

struct MmapFixture {
	const char* test;
	std::filesystem::path root;
	std::vector<uint8_t> payload;

	explicit MmapFixture(const char* name): test(name), payload(87960) {
		root = std::filesystem::temp_directory_path() /
		       ("kyty_mmap_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(test, std::filesystem::create_directories(root), "create mmap fixture directory");
		for (size_t i = 0; i < payload.size(); ++i) {
			payload[i] = static_cast<uint8_t>(i * 37 + i / 97);
		}
		Common::File file;
		Check(test, file.Create(root / "resource.bin"), "create mmap resource");
		uint32_t written = 0;
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &written);
		Check(test, written == payload.size(), "write mmap resource");
		file.Close();
		MmapFs::Initialize();
		MmapFs::Mount(root, "/mmap-test");
	}
	~MmapFixture() {
		MmapFs::TestSetMmapBeforeReadCallback(nullptr);
		MmapFs::Shutdown();
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
	int Open(int flags = 0) const {
		const int fd = MmapFs::KernelOpen("/mmap-test/resource.bin", flags, 0);
		Check(test, fd >= 3, "open mmap resource");
		return fd;
	}
};

using KernelMmapFunction = int (KYTY_SYSV_ABI *)(void*, size_t, int, int, int, int64_t, void**);

KernelMmapFunction ResolveKernelMmap(const char* test) {
	Loader::SymbolDatabase symbols;
	// Match production order: a leftover floor stub must not shadow NID lookup.
	Libs::ShellCoreFloor::InitShellCoreFloor_1(&symbols);
	Check(test, symbols.FindByNid("PGhQHd-dzv8", Loader::SymbolType::Func) == nullptr,
	      "floor still registers mmap");
	Libs::InitLibKernel_1(&symbols);
	const auto* nid = symbols.FindByNid("PGhQHd-dzv8", Loader::SymbolType::Func);
	const auto* exact = symbols.Find({"PGhQHd-dzv8", "libkernel", 1, "libkernel", 1, 1,
	                                  Loader::SymbolType::Func});
	Check(test, nid != nullptr && exact != nullptr && nid->vaddr == exact->vaddr &&
	                nid->vaddr == reinterpret_cast<uint64_t>(&MmapFs::KernelMmap),
	      "mmap NID/exact-key provider mismatch");
	return reinterpret_cast<KernelMmapFunction>(nid->vaddr);
}

void CheckMmapProtection(const char* test, void* mapped, int protection) {
	Check(test, Query(test, reinterpret_cast<uint64_t>(mapped)).protection == protection,
	      "wrong semantic mmap protection");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION info {};
	Check(test, VirtualQuery(mapped, &info, sizeof(info)) == sizeof(info), "query host protection");
	const DWORD expected = protection == 0 ? PAGE_NOACCESS :
	                       protection == 1 ? PAGE_READONLY : PAGE_READWRITE;
	Check(test, info.State == MEM_COMMIT && info.Protect == expected,
	      "mmap host protection differs from requested protection");
#endif
}

void TestKernelMmapFileResources() {
	const char* test = "KernelMmapFileResources";
	MmapFixture fixture(test);
	const auto mmap = ResolveKernelMmap(test);
	const auto budget = AvailableFlexibleMemory(test);
	const auto rounded = (fixture.payload.size() + SceKernelPageSize - 1) & ~(SceKernelPageSize - 1);
	const auto archive = fixture.root / "resource.zar";
	Check(test, ArchiveTests::CreateArchive(archive, fixture.payload), "create mmap archive");
	MmapFs::Mount(Common::MakeArchivePath(archive), "/mmap-archive", true);

	for (bool archived: {false, true}) {
		const int fd = archived ? MmapFs::KernelOpen("/mmap-archive/assets/subdir/data.bin", 0, 0) :
		                          fixture.Open();
		Check(test, fd >= 3, "open archive mmap resource");
		Check(test, MmapFs::KernelLseek(fd, 123, 0) == 123, "set descriptor cursor");
		void* mapped = reinterpret_cast<void*>(0x1234);
		CheckOk(test, mmap(nullptr, fixture.payload.size(), 1, 0, fd, 0, &mapped),
		        "seven-argument flags-zero mmap");
		Check(test, mapped != nullptr && mapped != reinterpret_cast<void*>(0x1234),
		      "seventh output argument was not written");
		Check(test, MmapFs::KernelLseek(fd, 0, 1) == 123, "mmap changed descriptor cursor");
		CheckMmapProtection(test, mapped, 1);
		const auto* bytes = static_cast<const uint8_t*>(mapped);
		Check(test, std::memcmp(bytes, fixture.payload.data(), fixture.payload.size()) == 0,
		      "mapped resource bytes differ");
		Check(test, std::all_of(bytes + fixture.payload.size(), bytes + rounded,
		                       [](uint8_t byte) { return byte == 0; }), "EOF tail is not zero");

		// The seventh argument is on the SysV stack; also exercise R9 offset and
		// file data beyond the requested byte length within its mapped last page.
		void* partial = nullptr;
		CheckOk(test, mmap(nullptr, 17, 1, 2, fd, SceKernelPageSize, &partial), "offset private mmap");
		Check(test, std::memcmp(partial, fixture.payload.data() + SceKernelPageSize,
		                        SceKernelPageSize) == 0, "short mmap did not load the whole file page");
		Check(test, MmapFs::KernelLseek(fd, 0, 1) == 123, "offset mmap changed descriptor cursor");
		CheckOk(test, MmapMemory::KernelMunmap(reinterpret_cast<uint64_t>(partial), 17),
		        "unmap short original length");
		CheckOk(test, MmapFs::KernelClose(fd), "close mapped descriptor");
		if (archived) {
			MmapFs::Umount("/mmap-archive");
		}
		Check(test, std::memcmp(bytes, fixture.payload.data(), fixture.payload.size()) == 0,
		      "close/unmount invalidated mapped bytes");
		const auto base = reinterpret_cast<uint64_t>(mapped);
		const auto neighbor = MmapMemory::AllocateRuntimeMemory(
		    base + rounded, SceKernelPageSize, Common::VirtualMemory::Mode::ReadWrite, "mmap neighbor", true);
		Check(test, neighbor == base + rounded, "allocate mmap neighbor");
		*reinterpret_cast<uint64_t*>(neighbor) = 0xaabbccdd;
		CheckOk(test, MmapMemory::KernelMunmap(base, fixture.payload.size()), "unmap original file length");
		Check(test, MmapMemory::TestPlaceholderRangeIsFree(base, rounded), "file unmap leaked tail page");
		Check(test, *reinterpret_cast<uint64_t*>(neighbor) == 0xaabbccdd,
		      "file unmap damaged adjacent mapping");
		const auto reused = MmapMemory::AllocateRuntimeMemory(
		    base, rounded, Common::VirtualMemory::Mode::ReadWrite, "mmap reuse", true);
		Check(test, reused == base, "file unmap did not release address ownership");
		Check(test, MmapMemory::FreeGuestMemory(reused, rounded), "free reused mapping");
		Check(test, MmapMemory::FreeGuestMemory(neighbor, SceKernelPageSize), "free mmap neighbor");
	}

	const int fd = fixture.Open(); // Private writes require read access, not a writable fd.
	void* writable = nullptr;
	CheckOk(test, mmap(nullptr, 1, 3, 2, fd, 0, &writable), "private writable mmap");
	CheckMmapProtection(test, writable, 3);
	*static_cast<uint8_t*>(writable) ^= 0xff;
	uint8_t original = 0;
	Common::File original_file(fixture.root / "resource.bin", Common::File::Mode::Read);
	Check(test, !original_file.IsInvalid(), "open private mapping source for verification");
	uint32_t bytes_read = 0;
	original_file.Read(&original, 1, &bytes_read);
	original_file.Close();
	Check(test, bytes_read == 1 && original == fixture.payload[0],
	      "private write changed underlying file");
	void* inaccessible = nullptr;
	CheckOk(test, mmap(nullptr, 1, 0, 0, fd, 0, &inaccessible), "no-access file mmap");
	CheckMmapProtection(test, inaccessible, 0);
	Check(test, MmapMemory::ProtectGuestMemory(reinterpret_cast<uint64_t>(inaccessible),
	                                          SceKernelPageSize, Common::VirtualMemory::Mode::Read),
	      "restore no-access mapping for content verification");
	Check(test, std::memcmp(inaccessible, fixture.payload.data(), SceKernelPageSize) == 0,
	      "no-access mmap discarded file bytes");
	CheckOk(test, MmapMemory::KernelMunmap(reinterpret_cast<uint64_t>(inaccessible), 1), "unmap no-access view");
	CheckOk(test, MmapMemory::KernelMunmap(reinterpret_cast<uint64_t>(writable), 1), "unmap private view");
	CheckOk(test, MmapFs::KernelClose(fd), "close private source");
	Check(test, AvailableFlexibleMemory(test) == budget, "Runtime mmap changed flexible accounting");
	std::printf("[host]    %-48s ok\n", test);
}

uint64_t g_mmap_test_allocation = 0;
size_t g_mmap_test_allocation_size = 0;
std::filesystem::path g_mmap_test_truncate_path;

void ObserveMmapAllocation(uint64_t address, size_t size) {
	g_mmap_test_allocation = address;
	g_mmap_test_allocation_size = size;
	if (!g_mmap_test_truncate_path.empty()) {
		std::error_code error;
		std::filesystem::resize_file(g_mmap_test_truncate_path, 0, error);
		Check("KernelMmapFailures", !error, "truncate host fixture after mmap size check");
	}
}

void TestKernelMmapFailures() {
	using namespace Libs::LibKernel;
	const char* test = "KernelMmapFailures";
	MmapFixture fixture(test);
	const auto mmap = ResolveKernelMmap(test);
	const int fd = fixture.Open();
	const int write_only = fixture.Open(1);
	const int directory = MmapFs::KernelOpen("/mmap-test", 0x20000, 0);
	const int device = MmapFs::KernelOpen("/dev/urandom", 0, 0);
	Check(test, directory >= 3 && device >= 3, "open nonregular fixtures");
	const auto fail = [&](int expected, size_t length, int prot, int flags, int descriptor,
	                      int64_t offset = 0, void* address = nullptr) {
		void* output = reinterpret_cast<void*>(0x12345678);
		Check(test, mmap(address, length, prot, flags, descriptor, offset, &output) == expected,
		      "unexpected mmap error code");
		Check(test, output == reinterpret_cast<void*>(0x12345678), "failed mmap changed output");
	};
	Check(test, mmap(nullptr, 1, 1, 0, fd, 0, nullptr) == KERNEL_ERROR_EFAULT, "null output contract");
	fail(KERNEL_ERROR_EINVAL, 0, 1, 0, fd);
	fail(KERNEL_ERROR_EINVAL, SIZE_MAX, 1, 0, fd);
	fail(KERNEL_ERROR_EINVAL, 1, 8, 0, fd);
	fail(KERNEL_ERROR_EINVAL, 1, 1, 0, fd, -1);
	fail(KERNEL_ERROR_EINVAL, 1, 1, 0, fd, 1);
	fail(KERNEL_ERROR_EINVAL, SceKernelPageSize, 1, 0, fd, INT64_MAX & ~int64_t(SceKernelPageSize - 1));
	fail(KERNEL_ERROR_EBADF, 1, 1, 0, -1);
	fail(KERNEL_ERROR_EBADF, 1, 1, 0, INT32_MIN);
	fail(KERNEL_ERROR_EACCES, 1, 1, 0, write_only);
	fail(KERNEL_ERROR_ENODEV, 1, 1, 0, directory);
	fail(KERNEL_ERROR_ENODEV, 1, 1, 0, device);
	fail(KERNEL_ERROR_EOPNOTSUPP, 1, 3, 0, fd);
	fail(KERNEL_ERROR_EOPNOTSUPP, 1, 1, 1, fd);
	fail(KERNEL_ERROR_EOPNOTSUPP, 1, 1, 0x1002, fd);
	fail(KERNEL_ERROR_EOPNOTSUPP, SceKernelPageSize * 8, 1, 2, fd);
	fail(KERNEL_ERROR_EOPNOTSUPP, 1, 1, 2, fd, SceKernelPageSize * 8);
	const auto occupied = MmapMemory::AllocateRuntimeMemory(
	    0, SceKernelPageSize, Common::VirtualMemory::Mode::ReadWrite, "fixed mmap sentinel");
	Check(test, occupied != 0, "allocate fixed mmap sentinel");
	*reinterpret_cast<uint64_t*>(occupied) = 0xfeedface;
	fail(KERNEL_ERROR_EOPNOTSUPP, 1, 1, 0x12, fd, 0, reinterpret_cast<void*>(occupied));
	Check(test, *reinterpret_cast<uint64_t*>(occupied) == 0xfeedface, "rejected fixed mmap changed owner");
	Check(test, MmapMemory::FreeGuestMemory(occupied, SceKernelPageSize), "free fixed mmap sentinel");
	CheckOk(test, MmapFs::KernelClose(write_only), "close error fixture");
	fail(KERNEL_ERROR_EBADF, 1, 1, 0, write_only);
	Check(test, MmapFs::KernelLseek(fd, 91, 0) == 91, "set rollback cursor");

	for (bool short_read: {false, true}) {
		g_mmap_test_allocation = 0;
		g_mmap_test_truncate_path = short_read ? fixture.root / "resource.bin" : std::filesystem::path {};
		MmapFs::TestSetMmapBeforeReadCallback(ObserveMmapAllocation);
		if (!short_read) {
			MmapMemory::TestFailNextGuestMemoryProtection();
		}
		fail(short_read ? KERNEL_ERROR_EIO : KERNEL_ERROR_EACCES, fixture.payload.size(), 1, 0, fd);
		Check(test, MmapFs::KernelLseek(fd, 0, 1) == 91, "failed mmap changed cursor");
		Check(test, g_mmap_test_allocation != 0 &&
		                MmapMemory::TestPlaceholderRangeIsFree(g_mmap_test_allocation, g_mmap_test_allocation_size),
		      "failed mmap leaked its owner allocation");
		ExpectUnmapped(test, g_mmap_test_allocation);
		const auto reused = MmapMemory::AllocateRuntimeMemory(
		    g_mmap_test_allocation, g_mmap_test_allocation_size, Common::VirtualMemory::Mode::ReadWrite,
		    "mmap rollback reuse", true);
		Check(test, reused == g_mmap_test_allocation, "failed mmap address cannot be reused");
		Check(test, MmapMemory::FreeGuestMemory(reused, g_mmap_test_allocation_size), "free rollback reuse");
	}
	g_mmap_test_truncate_path.clear();
	CheckOk(test, MmapFs::KernelClose(fd), "close rollback source");
	CheckOk(test, MmapFs::KernelClose(directory), "close directory source");
	CheckOk(test, MmapFs::KernelClose(device), "close device source");
	std::printf("[host]    %-48s ok\n", test);
}

void TestMunmapByteLengths() {
	const char* test = "MunmapByteLengths";
	const auto page = SceKernelPageSize;
	const auto budget = AvailableFlexibleMemory(test);
	for (bool flexible: {false, true}) {
		const auto base = flexible ? MapNamedFlexible(test, page * 3, 3, "byte munmap") :
		    MmapMemory::AllocateRuntimeMemory(0, page * 3, Common::VirtualMemory::Mode::ReadWrite, "byte munmap");
		Check(test, base != 0, "allocate byte munmap fixture");
		*reinterpret_cast<uint64_t*>(base) = 0x11223344;
		*reinterpret_cast<uint64_t*>(base + page * 2) = 0x55667788;
		Check(test, MmapMemory::KernelMunmap(base + 1, 1) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
		      "unaligned munmap address accepted");
		Check(test, MmapMemory::KernelMunmap(base, SIZE_MAX) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
		      "munmap rounding overflow accepted");
		Check(test, MmapMemory::KernelMunmap(UINT64_MAX & ~(page - 1), 1) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
		      "rounded munmap address overflow accepted");
		Check(test, MmapMemory::KernelMunmap(base, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
		      "zero munmap length accepted");
		CheckOk(test, MmapMemory::KernelMunmap(base + page, 1), "unmap one byte of middle page");
		ExpectUnmapped(test, base + page);
		Check(test, *reinterpret_cast<uint64_t*>(base) == 0x11223344 &&
		                *reinterpret_cast<uint64_t*>(base + page * 2) == 0x55667788,
		      "rounded partial munmap damaged neighbors");
		CheckOk(test, MmapMemory::KernelMunmap(base, page * 2 + 1), "rounded sparse munmap");
		Check(test, MmapMemory::TestPlaceholderRangeIsFree(base, page * 3), "rounded munmap leaked pages");
	}
	Check(test, AvailableFlexibleMemory(test) == budget, "rounded unmap leaked flexible budget");
	std::printf("[host]    %-48s ok\n", test);
}
