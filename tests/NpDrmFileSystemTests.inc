// Included in the graphics-free memory test fixture; parent includes
// kernel/fileSystem.h and loader/runtimeLinker.h. No production/user data is
// used: persistence is tested only against this fixture's temporary /user mount.
namespace NpDrmFsTest {
namespace Fs = Libs::LibKernel::FileSystem;
namespace Kernel = Libs::LibKernel;
constexpr const char* Name = "NpDrmFileSystem";
constexpr uint32_t Init = 0xc0044e05;
constexpr uint32_t Term = 0xc0044e06;
constexpr uint32_t State = 0xc00c4e0f;
constexpr uint32_t Hash = 0xc0144e10;
constexpr uint32_t Unknown = 0xc0044eff;
constexpr std::array<uint8_t, 8> Sentinel {0x61, 0x63, 0x74, 0x2d, 0x74, 0x65, 0x73, 0x74};

struct Fixture {
	Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::Program owner, shell_ui, unknown, peer;
	std::filesystem::path root;
	Fixture() {
		owner.unique_id = 730001;
		owner.file_name = "synthetic-process.elf";
		owner.authority_metadata.emplace();
		owner.authority_metadata->authority_id = 0x4800000000000010ull;
		shell_ui.unique_id = 730002;
		shell_ui.file_name = "another-process.elf";
		shell_ui.authority_metadata.emplace();
		shell_ui.authority_metadata->authority_id = 0x480000000000000full;
		unknown.unique_id = 730003;
		unknown.file_name = "SceShellCore.elf"; // Filename must confer no authority.
		peer.unique_id = 730004;
		peer.file_name = "independent-process.elf";
		peer.authority_metadata.emplace();
		peer.authority_metadata->authority_id = 0x4800000000000010ull;
		root = std::filesystem::temp_directory_path() /
		       ("kyty_npdrm_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(Name, std::filesystem::create_directories(root), "create isolated guest /user");
		Loader::RuntimeLinker::SetCurrentProgram(&owner);
		Fs::Initialize();
		Fs::Mount(root, "/user");
	}
	~Fixture() {
		Fs::TestSetNpDrmIoctlAcceptedCallback(nullptr);
		Fs::Shutdown();
		Loader::RuntimeLinker::SetCurrentProgram(previous);
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
	int Open() const {
		const int fd = Fs::KernelOpen("/dev/npdrm", 2, 0);
		Check(Name, fd >= 3, "real NPDRM descriptor open");
		return fd;
	}
	void Seed() const {
		std::filesystem::create_directories(root / "license");
		Common::File file;
		Check(Name, file.Create(root / "license" / "act.dat"), "create temporary activation sentinel");
		uint32_t written = 0;
		file.Write(Sentinel.data(), static_cast<uint32_t>(Sentinel.size()), &written);
		Check(Name, written == Sentinel.size(), "write temporary sentinel");
		file.Close();
	}
	void CheckSentinel() const {
		Common::File file;
		Check(Name, file.Open(root / "license" / "act.dat", Common::File::Mode::Read), "open sentinel for verification");
		std::array<uint8_t, 8> bytes {};
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		Check(Name, read == bytes.size() && bytes == Sentinel && file.Size() == Sentinel.size(), "sentinel was modified");
		file.Close();
	}
	void CheckEmpty() const {
		const auto info = Common::File::GetInfo(root / "license" / "act.dat");
		Check(Name, info && info->is_file && info->size == 0, "native persistence did not create/truncate act.dat");
	}
};

int Ioctl(int fd, uint32_t request, void* payload) {
	int result = 12345;
	Check(Name, Fs::TryNpDrmIoctl(fd, request, payload, &result), "NPDRM request escaped into generic routing");
	return result;
}

void CheckState(int fd, bool initialized) {
	std::array<uint32_t, 5> words {0xaabbccdd, 0x11223344, 0x55667788, 0xdeadbeef, 0xabcdef01};
	CheckOk(Name, Ioctl(fd, State, words.data() + 1), "query NPDRM state");
	Check(Name, words[0] == 0xaabbccdd && words[4] == 0xabcdef01, "state query damaged canaries");
	Check(Name, words[1] == (initialized ? 0x10000u : 0x11223344u) &&
	            words[2] == (initialized ? 0x800f0a0eu : 0x55667788u) &&
	            words[3] == (initialized ? 0u : 0x800f0a32u), "incorrect global state query");
}

void CheckStatusWrite(int fd, uint32_t request) {
	std::array<uint32_t, 3> words {0xaabbccdd, 0xdeadbeef, 0x11223344};
	CheckOk(Name, Ioctl(fd, request, words.data() + 1), "four-byte NPDRM acknowledgment");
	Check(Name, words[0] == 0xaabbccdd && words[1] == 0 && words[2] == 0x11223344,
	      "status acknowledgment wrote more than four bytes");
}

static int accepted_fd = -1;
static int replacement_fd = -1;
static int accepted_close_result = -1;
void CloseAndReuseAcceptedDescriptor() {
	Fs::TestSetNpDrmIoctlAcceptedCallback(nullptr);
	accepted_close_result = Fs::KernelClose(accepted_fd);
	replacement_fd = Fs::KernelOpen("/user/license/act.dat", 0, 0);
}

} // namespace NpDrmFsTest

void TestNpDrmFileSystem() {
	using namespace NpDrmFsTest;
	Fixture fixture;
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.unknown);
	Check(Name, Fs::KernelOpen("/dev/npdrm", 2, 0) == Kernel::KERNEL_ERROR_EPERM,
	      "missing authority was inferred from filename");
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.owner);
	int fd = fixture.Open();
	// Public ABI translates the filesystem's kernel-coded error to -1/errno,
	// normalizes request to u32 and fd to signed i32, and retains copy precedence.
	using PublicIoctl = int (KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	const PublicIoctl public_ioctl = &Libs::ShellHits::Kernel::Ioctl;
	uint32_t public_output = 0xdeadbeef;
	*Libs::Posix::GetErrorAddr() = 0;
	Check(Name, public_ioctl(UINT64_C(0x1234567880000000), UINT64_C(0x12345678c0044e05),
	                         reinterpret_cast<uint64_t>(&public_output), 0, 0, 0) == -1 &&
	            *Libs::Posix::GetErrorAddr() == 9 && public_output == 0xdeadbeef,
	      "public ioctl must return -1/EBADF with low-32-bit fd/request semantics");
	Check(Name, public_ioctl(UINT32_MAX, Init, 0, 0, 0, 0) == -1 && *Libs::Posix::GetErrorAddr() == 14,
	      "public ioctl copyin fault must precede fd error");
	Check(Name, public_ioctl(fd, Unknown, reinterpret_cast<uint64_t>(&public_output), 0, 0, 0) == -1 &&
	            *Libs::Posix::GetErrorAddr() == 25 && public_output == 0xdeadbeef,
	      "public ioctl unknown request must preserve payload and return ENOTTY");
	CheckState(fd, false);
	CheckStatusWrite(fd, Term);
	CheckState(fd, false);
	std::array<uint32_t, 7> pre_hash {0x12345678, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0, 0x87654321};
	CheckOk(Name, Ioctl(fd, Hash, pre_hash.data() + 1), "pre-init hash query");
	Check(Name, pre_hash[0] == 0x12345678 && pre_hash[6] == 0x87654321 && pre_hash[5] == 0x800f0a32 &&
	            std::all_of(pre_hash.begin() + 1, pre_hash.begin() + 5, [](uint32_t v) { return v == 0xa5a5a5a5; }),
	      "pre-init hash query changed prefix/canaries");
	Check(Name, !std::filesystem::exists(fixture.root / "license"), "open/query/term created persistence storage");

	uint32_t output = 0xdeadbeef;
	for (int bad_fd: {INT32_MIN, -1, 0, 1, 2, INT32_MAX}) {
		Check(Name, Fs::GetFileRealPath(bad_fd).empty(), "extreme fd lookup did not reject safely");
		Check(Name, Ioctl(bad_fd, Init, nullptr) == Kernel::KERNEL_ERROR_EFAULT, "copyin must precede bad fd");
		Check(Name, Ioctl(bad_fd, Init, reinterpret_cast<void*>(uintptr_t {1})) == Kernel::KERNEL_ERROR_EFAULT,
		      "unreadable copyin must precede bad fd");
		Check(Name, Ioctl(bad_fd, Init, &output) == Kernel::KERNEL_ERROR_EBADF && output == 0xdeadbeef,
		      "readable payload/bad fd precedence or output preservation");
	}
	Check(Name, Ioctl(fd, Init, reinterpret_cast<void*>(UINTPTR_MAX - 1)) == Kernel::KERNEL_ERROR_EFAULT,
	      "overflowing four-byte payload was accepted");
	std::array<uint8_t, 8191> maximum_payload;
	maximum_payload.fill(0xa5);
	Check(Name, Ioctl(INT32_MAX, 0xdfff4e05, maximum_payload.data()) == Kernel::KERNEL_ERROR_EBADF &&
	            std::all_of(maximum_payload.begin(), maximum_payload.end(), [](uint8_t b) { return b == 0xa5; }),
	      "encoded 13-bit maximum payload size or bad-fd no-copyout contract");
	CheckState(fd, false);
	Check(Name, Ioctl(fd, Unknown, &output) == Kernel::KERNEL_ERROR_ENOTTY && output == 0xdeadbeef,
	      "audited unknown ioctl did not return ENOTTY without copyout");
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.shell_ui);
	const int shell_fd = fixture.Open();
	std::array<uint32_t, 15> clock_payload;
	clock_payload.fill(0xa5a5a5a5);
	clock_payload[3] = 3; // +8 inside the 52-byte payload, surrounded by canaries.
	auto expected_clock = clock_payload;
	expected_clock[4] = 0x800f0513;
	*Libs::Posix::GetErrorAddr() = 42;
	Check(Name, public_ioctl(shell_fd, 0xc0344e18, reinterpret_cast<uint64_t>(clock_payload.data() + 1), 0, 0, 0) == 0 &&
	            clock_payload == expected_clock && *Libs::Posix::GetErrorAddr() == 42,
	      "secure-clock operation failure is a payload result, not a transport failure or timestamp overwrite");
	Check(Name, Ioctl(fd, Init, &output) == Kernel::KERNEL_ERROR_EPERM && output == 0xdeadbeef,
	      "ordinary ioctl accepted ShellUI authority");
	CheckOk(Name, Fs::KernelClose(shell_fd), "ShellUI close");
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.unknown);
	Check(Name, Fs::KernelClose(fd) == Kernel::KERNEL_ERROR_EPERM, "unauthorized close released descriptor");
	Check(Name, Ioctl(fd, Init, nullptr) == Kernel::KERNEL_ERROR_EFAULT, "copyin must precede authority rejection");
	Check(Name, Ioctl(fd, Init, &output) == Kernel::KERNEL_ERROR_EPERM && output == 0xdeadbeef,
	      "missing metadata ioctl mutated payload");
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.owner);
	CheckState(fd, false);

	// First authorized init must mkdir and create the zero-length file.
	CheckStatusWrite(fd, Init);
	fixture.CheckEmpty();
	CheckState(fd, true);
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.peer);
	const int peer_fd = fixture.Open();
	CheckState(peer_fd, true);
	CheckOk(Name, Fs::KernelClose(peer_fd), "independent process closes shared-state device");
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.owner);
	fixture.Seed();
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.shell_ui);
	Check(Name, Ioctl(fd, Init, &output) == Kernel::KERNEL_ERROR_EPERM, "denied repeated init acknowledged");
	fixture.CheckSentinel();
	Loader::RuntimeLinker::SetCurrentProgram(&fixture.owner);
	CheckStatusWrite(fd, Term);
	fixture.CheckSentinel();
	CheckStatusWrite(fd, Init);
	fixture.CheckEmpty(); // Every init truncates, including duplicate attempts.
	fixture.Seed();
	std::array<uint8_t, 28> hash;
	hash.fill(0xa5);
	constexpr std::array<uint8_t, 20> expected_hash {
	    0x26, 0x1c, 0x4b, 0xbf, 0x74, 0x82, 0xc9, 0xf5, 0x63, 0xf4, 0x42, 0x72, 0xe5, 0x43, 0xf6, 0x55, 0, 0, 0, 0};
	CheckOk(Name, Ioctl(fd, Hash, hash.data() + 4), "state hash query");
	Check(Name, std::equal(expected_hash.begin(), expected_hash.end(), hash.begin() + 4) &&
	            std::all_of(hash.begin(), hash.begin() + 4, [](uint8_t b) { return b == 0xa5; }) &&
	            std::all_of(hash.begin() + 24, hash.end(), [](uint8_t b) { return b == 0xa5; }), "hash payload/canaries");
	fixture.CheckSentinel();

	const int closed_fd = fd;
	CheckOk(Name, Fs::KernelClose(fd), "close initialized NPDRM fd");
	Check(Name, Ioctl(closed_fd, Init, &output) == Kernel::KERNEL_ERROR_EBADF, "closed fd accepted");
	fd = fixture.Open();
	Check(Name, fd == closed_fd, "descriptor slot was not reused");
	CheckState(fd, true);
	fixture.CheckSentinel();
	// Acceptance wins over a later close. Reuse the same fd for an ordinary
	// file before the core runs; the retained device File must still complete.
	accepted_fd = fd;
	replacement_fd = -1;
	Fs::TestSetNpDrmIoctlAcceptedCallback(CloseAndReuseAcceptedDescriptor);
	CheckStatusWrite(fd, Init);
	Check(Name, accepted_close_result == 0 && replacement_fd == fd, "accepted-operation close/reuse seam");
	fixture.CheckEmpty();
	int unrelated_result = 0x12345678;
	Check(Name, !Fs::TryNpDrmIoctl(replacement_fd, 0x406cae01, reinterpret_cast<void*>(uintptr_t {1}), &unrelated_result) &&
	            unrelated_result == 0x12345678, "ordinary ioctl routing was consumed or probed");
	CheckOk(Name, Fs::KernelClose(replacement_fd), "close replacement regular file");
	fd = fixture.Open();
	CheckState(fd, true);

	// Persistence failures on a read-only guest mount are ignored by the
	// driver, but cannot change existing files or create a missing directory.
	fixture.Seed();
	Fs::Mount(fixture.root, "/user", true);
	CheckStatusWrite(fd, Init);
	fixture.CheckSentinel();
	CheckState(fd, true);
	std::filesystem::remove_all(fixture.root / "license"); // Fixture-only preparation.
	CheckStatusWrite(fd, Init);
	Check(Name, !std::filesystem::exists(fixture.root / "license"), "read-only persistence created license directory");
	CheckState(fd, true);
	Fs::Mount(fixture.root, "/user");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SYSTEM_INFO system_info {};
	GetSystemInfo(&system_info);
	const size_t page_size = system_info.dwPageSize;
	struct Pages {
		uint8_t* address;
		~Pages() { if (address != nullptr) { VirtualFree(address, 0, MEM_RELEASE); } }
	} pages {static_cast<uint8_t*>(VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
	Check(Name, pages.address != nullptr, "allocate ioctl copy fixture");
	DWORD old = 0;
	Check(Name, VirtualProtect(pages.address + page_size, page_size, PAGE_NOACCESS, &old) != 0, "protect second copy page");
	fixture.Seed();
	Check(Name, Ioctl(fd, Init, pages.address + page_size - 2) == Kernel::KERNEL_ERROR_EFAULT,
	      "cross-page copyin accepted unreadable tail");
	fixture.CheckSentinel();
	std::memset(pages.address + page_size - 8, 0xa5, 8);
	CheckOk(Name, Ioctl(fd, Init, pages.address + page_size - 4), "exact four-byte page-end ioctl");
	fixture.CheckEmpty();
	uint32_t before = 0, status = 1;
	std::memcpy(&before, pages.address + page_size - 8, 4);
	std::memcpy(&status, pages.address + page_size - 4, 4);
	Check(Name, before == 0xa5a5a5a5 && status == 0, "page-end write crossed bounds");
	fixture.Seed();
	std::memset(pages.address, 0xa5, 4);
	// A fresh filesystem lifetime makes copyout failure's state transition
	// observable, rather than merely checking an already-initialized device.
	CheckOk(Name, Fs::KernelClose(fd), "close before lifecycle reset");
	Fs::Shutdown();
	Fs::Initialize();
	Fs::Mount(fixture.root, "/user");
	fd = fixture.Open();
	CheckState(fd, false);
	Check(Name, VirtualProtect(pages.address, page_size, PAGE_READONLY, &old) != 0, "make copyin-readable/output-readonly");
	Check(Name, Ioctl(INT32_MAX, Init, pages.address) == Kernel::KERNEL_ERROR_EBADF,
	      "premature write probe hid bad-fd error");
	fixture.CheckSentinel();
	Check(Name, Ioctl(fd, Init, pages.address) == Kernel::KERNEL_ERROR_EFAULT, "readonly copyout succeeded");
	fixture.CheckEmpty(); // Copyout failure occurs AFTER state and real truncation.
	CheckState(fd, true);
	std::memcpy(&status, pages.address, 4);
	Check(Name, status == 0xa5a5a5a5, "copyout bypassed readonly guest mapping");
#endif
	CheckOk(Name, Fs::KernelClose(fd), "final NPDRM close");
	Check(Name, Common::GetHostLockCriticalCounter()->load() == depth, "filesystem integration leaked host-critical state");
}
