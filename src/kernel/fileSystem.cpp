#include "kernel/fileSystem.h"
#include "kernel/guestDescriptorRange.h"
#include "kernel/fileSystemIdentity.h"
#include "kernel/devctlQueue.h"
#include "kernel/socketpairProvenance.h"

#include "common/archive.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "kernel/npDrmDevice.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "libs/standardWrite.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <system_error>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#undef CreateDirectory
#undef DeleteFile
#undef CopyFile
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel::FileSystem {

LIB_NAME("libkernel", "libkernel");

constexpr int      DESCRIPTOR_MIN = 3;
constexpr uint64_t DIR_BLOCK_SIZE = 512;

static thread_local GuestFsContextPtr g_current_fs_context;

GuestFsSnapshot GuestFsContext::Snapshot() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(m_mutex);
	return {m_root, m_cwd, m_generation, true};
}

bool GuestFsContext::TryChangeDirectory(uint64_t generation, const std::string& cwd) {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(m_mutex);
	if (generation != m_generation) { return false; }
	m_cwd = cwd;
	++m_generation;
	return true;
}

GuestFsContextPtr CreateBootstrapFsContext() { return std::make_shared<GuestFsContext>(); }
GuestFsContextPtr CloneFsContext(const GuestFsContextPtr& parent) {
	EXIT_IF(parent == nullptr);
	return std::make_shared<GuestFsContext>(parent->Snapshot());
}
GuestFsContextPtr GetCurrentFsContext() { return g_current_fs_context; }
void SetCurrentFsContext(GuestFsContextPtr context) { g_current_fs_context = std::move(context); }
ScopedGuestFsContext::ScopedGuestFsContext(GuestFsContextPtr context): m_previous(GetCurrentFsContext()) {
	SetCurrentFsContext(std::move(context));
}
ScopedGuestFsContext::~ScopedGuestFsContext() { SetCurrentFsContext(std::move(m_previous)); }

enum class SpecialFile {
	None,
	Random,
	// ps5-vsh: VSH shell UART console (/dev/console). Writes are shell logs.
	Console,
	// ps5-vsh: HID/bus devices (/dev/hid, /dev/usbctl, /dev/ugen, /dev/bt).
	// ShellUI input init stats/opens/reads these for bus-type detection;
	// empty reads (EOF) keep it on default paths instead of EBADF-abort.
	Hid,
	// ps5-vsh chainSUI180: notification devices (/dev/notificationN).
	// ShellUI opens these during bg init; no backend exists in emulation,
	// so empty reads (EOF) keep it on default paths instead of
	// ENOENT-abort. Mirrors Hid.
	Notification,
	// ps5-vsh: anonymous pipe pair (voldemort parity: PipeState FIFO).
	// Mono's socket wakeup backend + the JSCd/ShellUI self-pipe need real
	// descriptors with transferring bytes, not EBADF.
	Pipe,
	NpDrm,
	Sshot,
	// Idle debugger event channel. No debugger commands/events are synthesized.
	MetaDbg,
	Devctl,
	M2Control,
	AnonymousShm,
};

class MountPoints {
public:
	struct MountPair {
		std::filesystem::path dir;
		std::string           point;
		std::shared_ptr<Common::ArchiveReader> archive;
		// ps5-vsh: per-program mount scope (voldemort parity). Zero is a
		// system-wide mount, visible to every program.
		int32_t program_id = 0;
		// ps5-vsh: firmware mounts are read-only (voldemort parity);
		// guest writes there fail EROFS instead of touching the dump.
		bool read_only = false;
	};

	MountPoints() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~MountPoints() = default;

	KYTY_CLASS_NO_COPY(MountPoints);

	void Mount(const std::filesystem::path& folder, const std::string& point,
	             bool read_only = false);
	void MountForProgram(const std::filesystem::path& folder, const std::string& point,
	                     int32_t program_id, bool read_only = false);
	bool IsReadOnly(const std::string& path);
	void Umount(const std::string& folder_or_point);

	[[nodiscard]] std::filesystem::path ResolvePath(const std::string& mounted_name);
	struct Operation {
		GuestFsSnapshot fs;
		int32_t scope = 0; // Existing executable mount filter; independent of context identity.
		std::vector<MountPair> mounts;
		ResolvedGuestPath Resolve(const std::string& path, bool creating_directory = false) const;
		ResolvedGuestPath MapInput(const std::string& path) const;
		ResolvedGuestPath Map(const std::string& absolute, bool resolve_host_parents = false) const;
		bool IsMountAncestor(const std::string& absolute) const;
		int ParentDirectory(std::string* absolute) const;
		int RequireDirectory(const std::string& absolute) const;
	};
	Operation Capture(const GuestFsSnapshot* snapshot = nullptr);

private:
	std::vector<MountPair> m_mount_pairs;
	Common::Mutex          m_mutex;
};

// ps5-vsh: backing state shared by the two descriptors of an anonymous
// pipe (voldemort parity: PipeState FIFO). Mono's socket wakeup backend
// and the JSCd/ShellUI self-pipe need valid descriptors whose bytes
// actually transfer; without this they spin on failed init.
struct PipeState {
	Common::Mutex        mutex;
	Common::CondVar      ready;
	std::vector<uint8_t> buffer[2];
	bool                 open[2] = {true, true};
};

struct File {
	Common::File                        f;
	std::string                         name;
	std::filesystem::path               real_name;
	std::atomic_bool                    opened;
	std::atomic_bool                    directory;
	std::atomic_bool                    readable;
	std::atomic_bool                    writable;
	std::atomic_bool                    append;
	std::atomic_bool                    sync_writes;
	bool                                read_only_mount = false; // Immutable open-time policy.
	std::atomic_bool                    nonblocking;
	std::atomic_bool                    cloexec;
	std::atomic<int>                    access_mode;
	std::atomic<SpecialFile>            special;
	Common::Mutex                       mutex;
	Common::CondVar                     device_ready;
	std::vector<uint8_t>                dirents;
	uint64_t                            dents_offset;
	// ps5-vsh: pipe endpoint state (voldemort parity). shared_ptr: the
	// peer outlives either descriptor (matches File lifetime discipline).
	std::shared_ptr<PipeState>          pipe;
	unsigned                            pipe_end = 0;
	std::shared_ptr<Memory::AnonymousShm> shm;
	std::shared_ptr<Devctl::Queue> devctl;
	bool devctl_closed = false; // protected by the shared queue mutex
	int64_t shm_position = 0;
	FileStat shm_stat {};
	// Borrowed guest pool registration only. No host allocation, pool ownership,
	// capture metadata or pixels. Protected by mutex through ioctl copyout/close.
	std::optional<std::array<uint8_t, 112>> sshot_configuration;
	// There is currently no guest umask/credential source. These metadata
	// fields MUST NOT become input to an authorization decision.
	bool shm_effective_mode_valid = false;
	bool shm_ownership_valid = false;
	// Temporary provenance only; generation/context are immutable before the
	// File is inserted in the table. No guest authorization derives from these.
	uint64_t probe_generation = 0;
	uint64_t probe_creator_context = 0;
	std::atomic_bool probe_gls {false};
	std::atomic<uint32_t> probe_reads {0};
};

#include "kernel/socketpairProvenance.inc"

class FileDescriptors {
public:
	FileDescriptors() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~FileDescriptors() = default;

	KYTY_CLASS_NO_COPY(FileDescriptors);

	int   CreateDescriptor(uint64_t observer_caller = 0, int minimum = 0);
	void  DeleteDescriptor(int d, uint64_t observer_caller = 0);
	std::shared_ptr<File> GetFile(int d);
	std::shared_ptr<File> GetFile(const std::filesystem::path& real_name);
	void  CloseAll();
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	uint64_t CountAnonymous() {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		uint64_t count = 0;
		for (const auto& file: m_files) {
			if (file != nullptr && file->opened && file->special == SpecialFile::AnonymousShm) { ++count; }
		}
		return count;
	}
#endif

private:
	std::vector<std::shared_ptr<File>> m_files;
	Common::Mutex      m_mutex;
};

static MountPoints*     g_mount_points = nullptr;
static FileDescriptors* g_files        = nullptr;

namespace {
std::mutex g_file_identity_mutex;
Detail::FileIdentityMap g_file_identities;

int IdentityError(const std::error_code& error) {
	if (error == std::errc::no_such_file_or_directory) { return KERNEL_ERROR_ENOENT; }
	if (error == std::errc::not_a_directory) { return KERNEL_ERROR_ENOTDIR; }
	if (error == std::errc::permission_denied) { return KERNEL_ERROR_EACCES; }
	if (error == std::errc::operation_not_permitted) { return KERNEL_ERROR_EPERM; }
	if (error == std::errc::bad_file_descriptor) { return KERNEL_ERROR_EBADF; }
	if (error == std::errc::too_many_files_open) { return KERNEL_ERROR_EMFILE; }
	if (error == std::errc::too_many_files_open_in_system) { return KERNEL_ERROR_ENFILE; }
	if (error == std::errc::not_enough_memory) { return KERNEL_ERROR_ENOMEM; }
	if (error == std::errc::filename_too_long) { return KERNEL_ERROR_ENAMETOOLONG; }
	if (error == std::errc::too_many_symbolic_link_levels) { return KERNEL_ERROR_ELOOP; }
	return KERNEL_ERROR_EIO;
}

int TimestampError(const std::error_code& error) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (error.category() == std::system_category()) {
		switch (error.value()) {
			case ERROR_ACCESS_DENIED:
			case ERROR_PRIVILEGE_NOT_HELD:
			case ERROR_SHARING_VIOLATION:
			case ERROR_LOCK_VIOLATION: return KERNEL_ERROR_EACCES;
			case ERROR_INVALID_HANDLE: return KERNEL_ERROR_EBADF;
			case ERROR_WRITE_PROTECT: return KERNEL_ERROR_EROFS;
			case ERROR_NOT_SUPPORTED:
			case ERROR_INVALID_FUNCTION: return KERNEL_ERROR_EOPNOTSUPP;
			case ERROR_INVALID_PARAMETER: return KERNEL_ERROR_EINVAL;
			case ERROR_NOT_ENOUGH_MEMORY:
			case ERROR_OUTOFMEMORY: return KERNEL_ERROR_ENOMEM;
			case ERROR_DISK_FULL: return KERNEL_ERROR_ENOSPC;
			case ERROR_ARITHMETIC_OVERFLOW: return KERNEL_ERROR_EOVERFLOW;
			default: break;
		}
	}
#endif
	if (error == std::errc::invalid_argument) { return KERNEL_ERROR_EINVAL; }
	if (error == std::errc::value_too_large || error == std::errc::result_out_of_range) { return KERNEL_ERROR_EOVERFLOW; }
	if (error == std::errc::read_only_file_system) { return KERNEL_ERROR_EROFS; }
	if (error == std::errc::operation_not_supported || error == std::errc::function_not_supported) {
		return KERNEL_ERROR_EOPNOTSUPP;
	}
	if (error == std::errc::no_space_on_device) { return KERNEL_ERROR_ENOSPC; }
	return IdentityError(error);
}

int SetFileIdentity(const Common::FileIdentity& host, FileStat* stat) {
	// The suspension exclusion covers only the map mutex, never host file I/O.
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_file_identity_mutex);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	try {
#endif
		const auto guest = g_file_identities.Get(host);
		if (!guest) { return KERNEL_ERROR_EOVERFLOW; }
		stat->st_dev = guest->device;
		stat->st_ino = guest->inode;
		return OK;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	} catch (const std::bad_alloc&) {
		return KERNEL_ERROR_ENOMEM;
	}
#endif
}
} // namespace

static NpDrm::Authority CurrentNpDrmAuthority() {
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (program != nullptr && program->application_owner != nullptr) { program = program->application_owner; }
	if (program == nullptr || !program->authority_metadata) { return std::nullopt; }
	return program->authority_metadata->authority_id;
}

// These non-ioctl device ABIs (and ioctls on a different file kind) have not
// been audited. Stop explicitly rather than inventing an errno or accidentally
// using the uninitialized Common::File. No ordinary read/write/stat is claimed.
[[noreturn]] static void NpDrmUnsupportedOperation(
    const char* operation, uint32_t request = 0, std::span<const uint8_t> payload = {}) {
	// Keep the reason visible with silent guest printf, without enabling an
	// unbounded syscall log. The unsupported operation still terminates here.
	// Only already-copied bytes are inspected; this is not another guest read.
	char preview[129] {};
	constexpr char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < std::min<size_t>(payload.size(), 64); ++i) {
		preview[2 * i] = hex[payload[i] >> 4];
		preview[2 * i + 1] = hex[payload[i] & 15];
	}
	EXIT("NPDRM unsupported operation: %s (request=0x%08x, payload=%s)", operation, request, preview);
	std::abort();
}

class NpDrmPersistence final: public NpDrm::Persistence {
public:
	int WriteActivation(std::span<const uint8_t> bytes) noexcept override {
		// Require actual guest mounts: never take KernelOpen's unmounted sandbox
		// fallback. Experiments must mount an isolated writable /user here.
		constexpr const char* directory = "/user/license";
		constexpr const char* path = "/user/license/act.dat";
		// Creation preflight is mapping-only: license is deliberately allowed
		// to be absent until this operation's mkdir sequence below. Do not call
		// lookup on act.dat first, or first activation can never create its parent.
		const auto operation = g_mount_points->Capture();
		const auto parent_mapping = operation.MapInput(directory);
		const auto file_mapping = operation.MapInput(path);
		if (parent_mapping.error != OK || file_mapping.error != OK ||
		    parent_mapping.host.empty() || file_mapping.host.empty()) { return KERNEL_ERROR_ENOENT; }
		if (parent_mapping.read_only || file_mapping.read_only) { return KERNEL_ERROR_EROFS; }
		FileStat info {};
		int error = KernelStat(directory, &info);
		if (error == KERNEL_ERROR_ENOENT) {
			if (IsReadOnly(directory)) { return KERNEL_ERROR_EROFS; }
			error = KernelMkdir(directory, 0770);
			if (error != OK && error != KERNEL_ERROR_EEXIST) { return error; }
		} else if (error != OK) {
			return error;
		} else if ((info.st_mode & 0170000) != 0040000) {
			return KERNEL_ERROR_ENOTDIR;
		}
		// Avoid KernelOpen's directory-write assertion if an invalid store has a
		// directory named act.dat. This remains an ignored persistence failure.
		if (KernelStat(path, &info) == OK && (info.st_mode & 0170000) == 0040000) { return KERNEL_ERROR_EISDIR; }
		const int fd = KernelOpen(path, 0x601, 0770);
		if (fd < 0) { return fd; }
		error = OK;
		if (!bytes.empty()) {
			const auto written = KernelWrite(fd, bytes.data(), bytes.size());
			if (written < 0) { error = static_cast<int>(written); }
			else if (static_cast<size_t>(written) != bytes.size()) { error = KERNEL_ERROR_EIO; }
		}
		// Native fsync: flush the retained real file, not libKernel's ack stub.
		const auto file = g_files->GetFile(fd);
		if (file != nullptr) {
			Common::LockGuard lock(file->mutex);
			if (!file->opened || file->special != SpecialFile::None || file->f.IsInvalid() || !file->f.Flush()) {
				error = KERNEL_ERROR_EIO;
			}
		} else {
			error = KERNEL_ERROR_EBADF;
		}
		const int closed = KernelClose(fd);
		return error != OK ? error : closed;
	}
};

static NpDrmPersistence g_npdrm_persistence;
// One driver instance per filesystem lifetime, never per descriptor/program.
// Initialize/Shutdown, as with the rest of this subsystem, require quiescence.
static std::unique_ptr<NpDrm::Device> g_npdrm_device;

static bool IsRandomDevice(const std::string& path) {
	return path == "/dev/random" || path == "/dev/urandom";
}

static bool IsConsoleDevice(const std::string& path) {
	return path == "/dev/console";
}

	static bool IsNotificationDevice(const std::string& path) {
		return path == "/dev/notification" ||
		       (path.compare(0, 17, "/dev/notification") == 0 &&
		        path.size() > 17 &&
		        path.find_first_not_of("0123456789", 17) == std::string::npos);
	}

static bool IsHidDevice(const std::string& path) {
	return path == "/dev/hid" || path == "/dev/usbctl" || path == "/dev/ugen" ||
	       path == "/dev/bt";
}

static int64_t WriteConsoleLog(const void* buf, size_t nbytes) {
	// NUL-safe: shell UART output is free-form text; keep the guest's
	// newlines so each line lands separately in the emulator log.
	const auto* bytes = reinterpret_cast<const char*>(buf);
	size_t      start = 0;
	for (size_t i = 0; i < nbytes; i++) {
		if (bytes[i] == '\n') {
			Log::WriteToConsoleAndLog(std::string(bytes + start, i + 1 - start));
			start = i + 1;
		}
	}
	if (start < nbytes) {
		Log::WriteToConsoleAndLog(std::string(bytes + start, nbytes - start));
	}
	return static_cast<int64_t>(nbytes);
}

static void FillRandomBuffer(void* buf, size_t nbytes) {
	auto*              out = reinterpret_cast<uint8_t*>(buf);
	std::random_device random;

	for (size_t i = 0; i < nbytes; i++) {
		out[i] = static_cast<uint8_t>(random());
	}
}

static void SecToTimespec(KernelTimespec* ts, double sec) {
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
	return (alignment != 0 ? value & ~(alignment - 1) : value);
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return (alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value);
}

static std::vector<uint8_t> PackDirents(const std::vector<Common::File::DirEntry>& entries) {
	std::vector<uint8_t> dirents;
	uint64_t             offset             = 0;
	uint64_t             last_reclen_offset = 0;
	for (const auto& entry: entries) {
		const auto& name = entry.name;
		EXIT_NOT_IMPLEMENTED(name.size() > 255);
		const auto reclen = AlignUp(8 + name.size() + 1, 4);
		if (offset + reclen > dirents.size()) {
			if (!dirents.empty()) {
				auto* last_reclen =
				    reinterpret_cast<uint16_t*>(dirents.data() + last_reclen_offset);
				*last_reclen += static_cast<uint16_t>(dirents.size() - offset);
			}
			offset = dirents.size();
			dirents.resize(offset + DIR_BLOCK_SIZE);
		}
		*reinterpret_cast<uint32_t*>(dirents.data() + offset) =
		    Common::hash(name.data(), static_cast<uint32_t>(name.size()));
		*reinterpret_cast<uint16_t*>(dirents.data() + offset + 4) = static_cast<uint16_t>(reclen);
		dirents[offset + 6]                                       = entry.is_file ? 8 : 4;
		dirents[offset + 7] = static_cast<uint8_t>(name.size());
		memcpy(dirents.data() + offset + 8, name.c_str(), name.size() + 1);
		last_reclen_offset = offset + 4;
		offset += reclen;
	}
	if (!dirents.empty()) {
		auto* last_reclen = reinterpret_cast<uint16_t*>(dirents.data() + last_reclen_offset);
		*last_reclen += static_cast<uint16_t>(dirents.size() - offset);
	}
	return dirents;
}

static uint64_t ReadDirectory(File* file, void* buf, uint64_t count) {
	const auto remaining =
	    file->dents_offset < file->dirents.size() ? file->dirents.size() - file->dents_offset : 0;
	const auto bytes = std::min(count, remaining);
	if (bytes != 0) {
		Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buf), bytes);
		memcpy(buf, file->dirents.data() + file->dents_offset, bytes);
		file->dents_offset += bytes;
	}
	return bytes;
}

static int PosixToKernel(int posix_errno) {
	return KERNEL_ERROR_UNKNOWN + posix_errno;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_test_mmap_before_read)(uint64_t, size_t) = nullptr;
void TestSetMmapBeforeReadCallback(void (*callback)(uint64_t, size_t)) {
	g_test_mmap_before_read = callback;
}
#endif

static bool TryMmapAnonymousFile(const std::shared_ptr<File>& file, void* address, size_t length,
                                 int protection, int flags, int64_t offset, void** output, int* error);

static int ValidateMmapCommon(size_t length, int protection, int64_t offset, void** output) {
	constexpr size_t page = 0x4000;
	if (output == nullptr) { return KERNEL_ERROR_EFAULT; }
	if (length == 0 || length > SIZE_MAX - (page - 1) || (protection & ~0x37) != 0 || offset < 0) {
		return KERNEL_ERROR_EINVAL;
	}
	const auto rounded = (length + page - 1) & ~(page - 1);
	return rounded > static_cast<uint64_t>(INT64_MAX - offset) ? KERNEL_ERROR_EINVAL : OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelMmap(
    void* address, size_t length, int protection, int flags, int fd, int64_t offset, void** result) {
	PRINT_NAME();
	const int common_error = ValidateMmapCommon(length, protection, offset, result);
	if (common_error != OK) { return common_error; }
	auto file = g_files->GetFile(fd);
	int shm_error = OK;
	if (TryMmapAnonymousFile(file, address, length, protection, flags, offset, result, &shm_error)) { return shm_error; }
	constexpr size_t page = 0x4000;
	if (result == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	if (length == 0 || length > SIZE_MAX - (page - 1) || (protection & ~7) != 0 ||
	    offset < 0 || (static_cast<uint64_t>(offset) & (page - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
	const size_t rounded = (length + page - 1) & ~(page - 1);
	if (rounded > static_cast<uint64_t>(INT64_MAX - offset)) {
		return KERNEL_ERROR_EINVAL;
	}
	// Native PSM uses flags=0 for read-only file resources. Omitted-sharing
	// writes, MAP_SHARED, MAP_FIXED and anonymous/device mappings need different
	// VM semantics; never report success for them using an independent copy.
	if ((flags != 0 && flags != 2) || (flags == 0 && protection != 0 && protection != 1)) {
		return KERNEL_ERROR_EOPNOTSUPP;
	}
	struct Mapping {
		uint64_t address = 0;
		size_t size;
		~Mapping() {
			if (address != 0) {
				EXIT_IF(!Memory::FreeGuestMemory(address, size));
			}
		}
	} mapping {0, rounded};
	{
		Common::LockGuard lock(file->mutex);
		// Keep the descriptor object alive and exclude close throughout loading.
		if (!file->opened) {
			return KERNEL_ERROR_EBADF;
		}
		if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("mmap"); }
		if (file->directory || file->special != SpecialFile::None) {
			return KERNEL_ERROR_ENODEV;
		}
		if (!file->readable) {
			return KERNEL_ERROR_EACCES;
		}
		if (file->f.IsInvalid()) {
			return KERNEL_ERROR_EIO;
		}
		const auto size = file->f.Size();
		const auto start = static_cast<uint64_t>(offset);
		// A copied view cannot emulate faults on whole pages beyond EOF. Only
		// the partial final file page may be zero-filled.
		if (start >= size || (rounded - 1) / page > (size - start - 1) / page) {
			return KERNEL_ERROR_EOPNOTSUPP;
		}
		const auto count = static_cast<size_t>(std::min<uint64_t>(rounded, size - start));
		mapping.address = Memory::AllocateRuntimeMemory(
		    reinterpret_cast<uint64_t>(address), rounded, Common::VirtualMemory::Mode::ReadWrite,
		    "file private mapping");
		if (mapping.address == 0) {
			return KERNEL_ERROR_ENOMEM;
		}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (auto callback = g_test_mmap_before_read; callback != nullptr) {
			g_test_mmap_before_read = nullptr;
			callback(mapping.address, rounded);
		}
#endif
		auto* bytes = reinterpret_cast<uint8_t*>(mapping.address);
		const auto position = file->f.Tell();
		bool loaded = file->f.Seek(start);
		for (size_t done = 0; loaded && done < count;) {
			const auto chunk = static_cast<uint32_t>(std::min<size_t>(count - done, UINT_MAX));
			uint32_t read = 0;
			file->f.Read(bytes + done, chunk, &read);
			loaded = (read == chunk);
			done += read;
		}
		const bool restored = file->f.Seek(position);
		if (!loaded || !restored) {
			return KERNEL_ERROR_EIO;
		}
		std::memset(bytes + count, 0, rounded - count);
	}
	const auto mode = static_cast<Common::VirtualMemory::Mode>(protection);
	if (Common::VirtualMemory::IsExecute(mode) &&
	    !Common::VirtualMemory::FlushInstructionCache(mapping.address, rounded)) {
		return KERNEL_ERROR_EIO;
	}
	if (!Memory::ProtectGuestMemory(mapping.address, rounded, mode)) {
		return KERNEL_ERROR_EACCES;
	}
	// sceKernelMmap's seventh argument is written only on success. The mapping
	// owns its bytes independently of the fd, mount and archive-reader lifetime.
	*result = reinterpret_cast<void*>(mapping.address);
	mapping.address = 0;
	return OK;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_test_shm_mmap_retained)() = nullptr;
static thread_local void (*g_test_mmap_file_read_before_lock)() = nullptr;
void TestSetShmMmapRetainedCallback(void (*callback)()) { g_test_shm_mmap_retained = callback; }
void TestSetMmapFileReadBeforeLockCallback(void (*callback)()) { g_test_mmap_file_read_before_lock = callback; }
uint64_t TestAnonymousShmDescriptorCount() { return g_files != nullptr ? g_files->CountAnonymous() : 0; }
#endif

static bool TryMmapAnonymousFile(const std::shared_ptr<File>& file, void* address, size_t length,
                                 int protection, int flags, int64_t offset, void** output, int* error) {
	if (file == nullptr) { *error = KERNEL_ERROR_EBADF; return true; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	// Deterministic close/reuse race before locking; no guard or table/file
	// lock spans the barrier. The retained File is the dispatch identity.
	if (auto callback = g_test_shm_mmap_retained; callback != nullptr) { callback(); }
#endif
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { *error = KERNEL_ERROR_EBADF; return true; }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { *error = KERNEL_ERROR_ENODEV; return true; }
	if (file->special != SpecialFile::AnonymousShm) { return false; }
	if (!file->readable || ((protection & 2) != 0 && !file->writable)) {
		*error = KERNEL_ERROR_EACCES;
	} else {
		*error = Memory::MapAnonymousShm(file->shm, address, length, protection, flags, offset, output);
	}
	return true;
}

bool TryMmapAnonymous(int fd, void* address, size_t length, int protection, int flags,
                      int64_t offset, void** output, int* error, std::shared_ptr<File>* legacy_file) {
	*error = ValidateMmapCommon(length, protection, offset, output);
	if (*error != OK) { return true; }
	const auto file = g_files->GetFile(fd);
	if (TryMmapAnonymousFile(file, address, length, protection, flags, offset, output, error)) { return true; }
	if (legacy_file != nullptr) { *legacy_file = file; }
	return false;
}

int MmapFileInfo(const std::shared_ptr<File>& file, std::filesystem::path* path, uint64_t* size) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (file->special != SpecialFile::None || file->directory) { return KERNEL_ERROR_ENODEV; }
	if (!file->readable) { return KERNEL_ERROR_EACCES; }
	if (file->f.IsInvalid()) { return KERNEL_ERROR_EIO; }
	try {
		*path = file->real_name;
		*size = file->f.Size();
		return OK;
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int64_t MmapFileRead(const std::shared_ptr<File>& file, void* buffer, size_t count, int64_t offset) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto callback = g_test_mmap_file_read_before_lock; callback != nullptr) { callback(); }
#endif
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (offset < 0 || count > INT64_MAX) { return KERNEL_ERROR_EINVAL; }
	if (file->special != SpecialFile::None || file->directory) { return KERNEL_ERROR_ENODEV; }
	if (!file->readable) { return KERNEL_ERROR_EACCES; }
	if (file->f.IsInvalid()) { return KERNEL_ERROR_EIO; }
	const auto file_size = file->f.Size();
	const auto start = static_cast<uint64_t>(offset);
	const auto bounded = start < file_size ? std::min<uint64_t>(count, file_size - start) : 0;
	if (bounded == 0) { return 0; }
	// Same EOF-bounded cache invalidation as ordinary KernelPread. Only the
	// retained File mutex is held: NEVER hold memory-operation/address-space
	// locks across this GPU callback, and never reacquire by descriptor number.
	Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buffer), bounded);
	const auto position = file->f.Tell();
	bool ok = file->f.Seek(static_cast<uint64_t>(offset));
	uint64_t done = 0;
	while (ok && done < bounded) {
		const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(bounded - done, UINT_MAX));
		uint32_t read = 0;
		file->f.Read(static_cast<uint8_t*>(buffer) + done, chunk, &read);
		done += read;
		if (read != chunk) { break; }
	}
	ok = file->f.Seek(position) && ok;
	return ok ? static_cast<int64_t>(done) : KERNEL_ERROR_EIO;
}

int KernelShmOpenAnonymous(int flags, uint16_t mode) {
	const uint64_t observer_caller = SocketpairProvenance::Enabled() ? KYTY_SPP_CALLER : 0;
	if ((static_cast<uint32_t>(flags) & ~0x00300e02u) != 0 || (flags & 3) != 2) { return KERNEL_ERROR_EINVAL; }
	// O_SYSTEM performs native budget accounting, which is not implemented.
	if ((flags & 0x00200000) != 0) { return KERNEL_ERROR_EOPNOTSUPP; }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return KERNEL_ERROR_EOPNOTSUPP;
#endif
#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
#error Anonymous SHM descriptor publication requires exceptions enabled for fileSystem.cpp
#endif
	Common::HostLockCriticalGuard critical;
	try {
		auto object = Memory::CreateAnonymousShm();
		FileStat metadata {};
		{
			std::lock_guard lock(g_file_identity_mutex);
			const auto identity = g_file_identities.AllocateAnonymous();
			if (!identity) { return KERNEL_ERROR_EOVERFLOW; }
			metadata.st_dev = identity->device;
			metadata.st_ino = identity->inode;
		}
		metadata.st_mode = 0100000u | (mode & 0777u); // requested, NOT umask-adjusted
		metadata.st_blksize = 0x4000;
		SecToTimespec(&metadata.st_birthtim, Common::DateTime::FromSystemUTC().ToUnix());
		metadata.st_atim = metadata.st_mtim = metadata.st_ctim = metadata.st_birthtim;
		// uid/gid zero are unmodeled ABI fields, not root credentials. Native
		// anonymous stat leaves nlink zero and counts 16-KiB pages in st_blocks.
		const auto fd = g_files->CreateDescriptor(observer_caller);
		const auto file = g_files->GetFile(fd);
		Common::LockGuard lock(file->mutex);
		file->shm = std::move(object);
		file->shm_stat = metadata;
		file->special = SpecialFile::AnonymousShm;
		file->readable = file->writable = true;
		file->nonblocking = false;
		file->cloexec = true;
		file->access_mode = 2;
		file->opened = true; // publish last; no fallible operation remains
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, fd, file.get(), observer_caller);
		return fd;
	} catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int FileDescriptors::CreateDescriptor(uint64_t observer_caller, int minimum) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	auto file = std::make_shared<File>();
	file->opened      = false;
	file->directory   = false;
	file->readable    = false;
	file->writable    = false;
	file->append      = false;
	file->sync_writes = false;
	file->special     = SpecialFile::None;
	SocketpairProvenance::Created(file.get());

	int files_num = static_cast<int>(m_files.size());
	for (int index = (std::max)(0, minimum - DESCRIPTOR_MIN); index < files_num; index++) {
		if (GuestDescriptors::IsSocketRange(index + DESCRIPTOR_MIN)) { continue; }
		if (m_files[index] == nullptr) {
			m_files[index] = file;
			SocketpairProvenance::Observe(SocketpairProvenance::Event::Create,
			    index + DESCRIPTOR_MIN, file.get(), observer_caller);
			return index + DESCRIPTOR_MIN;
		}
	}

	if (files_num + DESCRIPTOR_MIN < minimum) {
		m_files.resize(static_cast<size_t>(minimum - DESCRIPTOR_MIN));
		files_num = static_cast<int>(m_files.size());
	}
	if (GuestDescriptors::IsSocketRange(files_num + DESCRIPTOR_MIN)) {
		m_files.resize(GuestDescriptors::SocketEnd - DESCRIPTOR_MIN);
	}
	m_files.push_back(file);
	SocketpairProvenance::Observe(SocketpairProvenance::Event::Create,
	    static_cast<int>(m_files.size()) + DESCRIPTOR_MIN - 1, file.get(), observer_caller);
	return static_cast<int>(m_files.size()) + DESCRIPTOR_MIN - 1;
}

void FileDescriptors::DeleteDescriptor(int d, uint64_t observer_caller) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);

	EXIT_IF(index >= m_files.size());
	EXIT_IF(m_files[index] == nullptr);
	EXIT_IF(m_files[index]->opened);

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	// Close host files opened before a failed descriptor setup.
	m_files[index]->f.Close();
#endif

	// Slot release only: in-flight syscalls hold their own shared_ptr, so the
	// File dies when the last reader drops it (fixes the RtlAllocateHeap
	// null-heap crash: a freed File's null MutexPrivate dereferenced in
	// EnterCriticalSection by a concurrent KernelRead).
	SocketpairProvenance::Observe(SocketpairProvenance::Event::Delete,
	    d, m_files[index].get(), observer_caller);
	m_files[index] = nullptr;
}

std::shared_ptr<File> FileDescriptors::GetFile(int d) {
	// Validate before signed subtraction: INT_MIN - DESCRIPTOR_MIN is UB.
	if (d < DESCRIPTOR_MIN) { return nullptr; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);

	if (index >= m_files.size()) {
		return nullptr;
	}

	return m_files[index];
}

std::shared_ptr<File> FileDescriptors::GetFile(const std::filesystem::path& real_name) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	for (const auto& f: m_files) {
		if (f != nullptr && f->real_name == real_name) {
			return f;
		}
	}

	return nullptr;
}

void FileDescriptors::CloseAll() {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	for (auto& f: m_files) {
		if (f != nullptr) {
			if (f->special == SpecialFile::Devctl && f->devctl) { f->devctl->Close(f->devctl_closed); }
			if (f->special == SpecialFile::MetaDbg) {
				Common::LockGuard device_lock(f->mutex);
				f->opened = false;
				f->device_ready.SignalAll();
			}
			if (f->opened && f->special == SpecialFile::None) {
				f->f.Close();
			}
			f->opened = false;
			f->sshot_configuration.reset();
			f = nullptr;
		}
	}
}

void MountPoints::Mount(const std::filesystem::path& folder, const std::string& point,
                          bool read_only) {
	MountForProgram(folder, point, 0, read_only);
}

void MountPoints::MountForProgram(const std::filesystem::path& folder, const std::string& point,
                                  int32_t program_id, bool read_only) {
	// ps5-vsh: per-program mount (voldemort parity). Same point as the
	// system-wide entry is a distinct mount, shadowed per scope.
	auto point_str = Common::FixDirectorySlash(point);
	MountPair p;
	p.dir        = folder;
	p.point      = point_str;
	p.archive    = Common::OpenArchive(folder);
	p.program_id = program_id;
	p.read_only  = read_only;

	std::vector<MountPair> retired;
	{
		// Open above and destroy retired archive references below the short lock.
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		for (auto it = m_mount_pairs.begin(); it != m_mount_pairs.end();) {
			if (it->point == point_str && it->program_id == program_id) {
				retired.push_back(std::move(*it));
				it = m_mount_pairs.erase(it);
			} else { ++it; }
		}
		m_mount_pairs.push_back(std::move(p));
	}
}

bool MountPoints::IsReadOnly(const std::string& path) {
	// Resolve preserves selected policy even on missing-parent errors, and uses
	// the final mount after a successfully validated dot/dotdot traversal.
	return Capture().Resolve(path).read_only;
}

void MountPoints::Umount(const std::string& folder_or_point) {
	auto folder_or_point_str = Common::FixDirectorySlash(folder_or_point);
	MountPair retired;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		// Only system-wide mounts participate; per-program entries are scoped.
		const auto it = std::find_if(m_mount_pairs.begin(), m_mount_pairs.end(),
		    [&folder_or_point_str](const MountPair& p) {
			    return p.program_id == 0 &&
			           (Common::FixDirectorySlash(Common::PathToGenericString(p.dir)) == folder_or_point_str ||
			            p.point == folder_or_point_str);
		    });
		if (it != m_mount_pairs.end()) {
			retired = std::move(*it);
			m_mount_pairs.erase(it);
		}
	}
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// Resolve guest paths case-insensitively on case-sensitive hosts.
static std::filesystem::path ResolvePathIgnoringCase(const std::filesystem::path& path) {
	std::error_code ec;
	if (std::filesystem::exists(path, ec)) {
		return path;
	}

	// Preserve unmatched components for the caller's ENOENT path.
	std::filesystem::path resolved =
	    path.has_root_path() ? path.root_path() : std::filesystem::path(".");
	bool matched = true;

	for (const auto& component: path.relative_path()) {
		if (component.empty()) {
			continue;
		}

		auto candidate = resolved / component;
		if (!matched || std::filesystem::exists(candidate, ec)) {
			resolved = std::move(candidate);
			continue;
		}

		bool found = false;
		for (std::filesystem::directory_iterator entry(resolved, ec), end; entry != end;
		     entry.increment(ec)) {
			if (ec) {
				break;
			}
			if (Common::EqualNoCase(entry->path().filename().string(), component.string())) {
				resolved = entry->path();
				found    = true;
				break;
			}
		}

		if (!found) {
			resolved = std::move(candidate);
			matched  = false;
		}
	}

	return resolved;
}
#else
static bool HasWindowsForbiddenFilenameCharacter(const std::string& relative_path) {
	return relative_path.find_first_of("<>:\"|?*") != std::string::npos;
}
#endif

std::filesystem::path MountPoints::ResolvePath(const std::string& mounted_name) {
	const auto result = Capture().MapInput(mounted_name);
	return result.error == OK ? result.host : std::filesystem::path {};
}

MountPoints::Operation MountPoints::Capture(const GuestFsSnapshot* snapshot) {
	Operation result;
	if (snapshot != nullptr) { result.fs = *snapshot; }
	else if (const auto context = GetCurrentFsContext()) { result.fs = context->Snapshot(); }
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	const auto* owner = program != nullptr && program->application_owner != nullptr ? program->application_owner : program;
	result.scope = owner != nullptr ? owner->unique_id : 0;
	{
		// Copy metadata only. No path traversal, archive call or guest callback
		// occurs under the mount lock or suspension exclusion.
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		result.mounts = m_mount_pairs;
	}
	return result;
}

static std::string MountName(std::string point) {
	while (point.size() > 1 && point.back() == '/') { point.pop_back(); }
	return point;
}

static bool PathPrefix(const std::string& name, const std::string& prefix) {
	return prefix == "/" || name == prefix ||
	       (name.size() > prefix.size() && name.starts_with(prefix) && name[prefix.size()] == '/');
}

// Preserve the existing /<sandbox-hash>/common[_ex] mapping as a mount alias,
// not a chroot. The guest spelling remains in the result/cwd.
static size_t CommonAliasPrefix(const std::string& name) {
	const auto slash = name.find('/', 1);
	if (slash == std::string::npos || slash <= 4 || slash >= 130) { return 0; }
	for (size_t i = 1; i < slash; ++i) {
		const char c = name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { return 0; }
	}
	const auto rest = name.substr(slash);
	return PathPrefix(rest, "/common") || PathPrefix(rest, "/common_ex") ? slash : 0;
}

ResolvedGuestPath MountPoints::Operation::Map(const std::string& absolute, bool resolve_host_parents) const {
	ResolvedGuestPath result;
	result.guest = absolute;
	const auto alias = CommonAliasPrefix(absolute);
	const auto name = alias != 0 ? absolute.substr(alias) : absolute;
	size_t length = 0;
	bool scoped = false;
	for (size_t i = 0; i < mounts.size(); ++i) {
		const auto& candidate = mounts[i];
		if (candidate.program_id != 0 && candidate.program_id != scope) { continue; }
		const auto point = MountName(candidate.point);
		if (!PathPrefix(name, point)) { continue; }
		const bool local = candidate.program_id != 0;
		// Longest visible mount wins. Scope only breaks ties at the SAME point.
		if (result.mount_index < 0 || point.size() > length ||
		    (point.size() == length && local && !scoped)) {
			result.mount_index = static_cast<int>(i);
			length = point.size();
			scoped = local;
		}
	}
	if (result.mount_index < 0) { return result; }
	const auto& mount = mounts[result.mount_index];
	result.read_only = mount.read_only || Common::IsArchivePath(mount.dir);
	auto relative = name.substr(length);
	if (relative.starts_with('/')) { relative.erase(0, 1); }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Never turn guest backslashes/drive prefixes into host path separators.
	if (HasWindowsForbiddenFilenameCharacter(relative) || relative.find('\\') != std::string::npos) {
		result.error = KERNEL_ERROR_EINVAL;
		return result;
	}
#endif
	result.host = relative.empty() ? mount.dir : mount.dir / Common::PathFromUtf8(relative);
	if (resolve_host_parents && relative.find("..") != std::string::npos && !Common::IsArchivePath(mount.dir)) {
		// A retained host-link traversal outside this mount's backing cannot be
		// expressed as a physical guest name. Follow each prefix before '..';
		// in particular, Windows must not lexically erase link/.. before lookup.
		result.host = mount.dir;
		for (const auto& component: Common::PathFromUtf8(relative)) {
			if (component == "..") {
				std::error_code error;
				result.host = std::filesystem::canonical(result.host, error);
				if (error) { result.error = IdentityError(error); return result; }
				result.host = result.host.parent_path();
			} else if (component != "." && !component.empty()) {
				result.host /= component;
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
				result.host = ResolvePathIgnoringCase(result.host);
#endif
			}
		}
	}
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	if (!Common::IsArchivePath(mount.dir)) { result.host = ResolvePathIgnoringCase(result.host); }
#endif
	return result;
}

bool MountPoints::Operation::IsMountAncestor(const std::string& absolute) const {
	// A visible child mount establishes its namespace ancestors even if a
	// broader mounted backing lacks them (e.g. / -> empty, /mnt/disc -> media).
	// This is namespace topology, not creation of missing host directories.
	for (const auto& mount: mounts) {
		const auto point = MountName(mount.point);
		if ((mount.program_id == 0 || mount.program_id == scope) && point != absolute &&
		    PathPrefix(point, absolute)) { return true; }
	}
	return false;
}

int MountPoints::Operation::RequireDirectory(const std::string& absolute) const {
	// / and the device namespace are guest topology, not host cwd/backings.
	if (absolute == "/" || absolute == "/dev") { return OK; }
	if (absolute == "/dev/npdrm" || absolute == "/dev/sshot" || absolute == "/dev/metadbg" || absolute == "/dev/devctl" || absolute == "/dev/m2.ctl" || IsRandomDevice(absolute) || IsConsoleDevice(absolute) ||
	    IsNotificationDevice(absolute) || IsHidDevice(absolute)) { return KERNEL_ERROR_ENOTDIR; }
	if (IsMountAncestor(absolute)) { return OK; }
	const auto mapped = Map(absolute, true);
	if (mapped.error != OK) { return mapped.error; }
	if (mapped.host.empty()) { return KERNEL_ERROR_ENOENT; }
	if (Common::IsArchivePath(mapped.host)) {
		const auto info = Common::File::GetInfo(mapped.host);
		return !info ? KERNEL_ERROR_ENOENT : info->is_file ? KERNEL_ERROR_ENOTDIR : OK;
	}
	std::error_code error;
	const auto info = std::filesystem::status(mapped.host, error);
	if (error) { return IdentityError(error); }
	if (!std::filesystem::exists(info)) { return KERNEL_ERROR_ENOENT; }
	// ParentDirectory follows this same directory object before consuming '..'.
	if (!std::filesystem::is_directory(info)) { return KERNEL_ERROR_ENOTDIR; }
#if defined(__linux__) || defined(__APPLE__)
	if (::access(mapped.host.c_str(), X_OK) != 0) { return IdentityError(std::error_code(errno, std::generic_category())); }
#endif
	return OK;
}

int MountPoints::Operation::ParentDirectory(std::string* absolute) const {
	const auto pop = [&] {
		const auto slash = absolute->rfind('/');
		absolute->resize(slash == 0 ? 1 : slash);
	};
	// Namespace nodes and mount roots have guest parents, not backing parents.
	if (*absolute == "/" || *absolute == "/dev") { pop(); return OK; }
	const auto mapped = Map(*absolute, true);
	if (mapped.error != OK) { return mapped.error; }
	if (mapped.host.empty() || Common::IsArchivePath(mapped.host)) { pop(); return OK; }
	const auto& mount = mounts[mapped.mount_index];
	const auto point = MountName(mount.point);
	const auto alias = CommonAliasPrefix(*absolute);
	const auto name = alias != 0 ? absolute->substr(alias) : *absolute;
	if (name == point) { pop(); return OK; }

	std::error_code error;
	if (IsMountAncestor(*absolute) && !std::filesystem::is_directory(mapped.host, error)) {
		// A namespace-only ancestor has no host directory object to follow.
		pop(); return OK;
	}
	const auto directory = std::filesystem::canonical(mapped.host, error);
	if (error) { return IdentityError(error); }
	const auto backing = std::filesystem::canonical(mount.dir, error);
	if (error) { return IdentityError(error); }
	const auto relative = directory.lexically_relative(backing);
	if (!relative.empty() && *relative.begin() != "..") {
		// Rebase the followed object within the selected backing, then traverse
		// its parent in the captured guest namespace. Subsequent components still
		// select the longest visible mount (including readonly child overrides).
		const auto prefix = alias != 0 ? absolute->substr(0, alias) : std::string {};
		*absolute = prefix + point;
		if (relative != ".") {
			if (absolute->back() != '/') { *absolute += '/'; }
			*absolute += Common::PathToGenericString(relative);
		}
		pop();
	} else {
		// Preserve admitted host-link lookup outside this backing, without
		// inventing a guest mount for its target. Map follows this prefix before
		// applying '..'; path-backed cwd retains the same traversal spelling.
		*absolute += "/..";
	}
	return OK;
}

ResolvedGuestPath MountPoints::Operation::MapInput(const std::string& path) const {
	ResolvedGuestPath result;
	if (path.empty()) { result.error = KERNEL_ERROR_ENOENT; return result; }
	if (path.size() >= 1024) { result.error = KERNEL_ERROR_ENAMETOOLONG; return result; }
	if (path.find('\0') != std::string::npos || fs.root != "/") { result.error = KERNEL_ERROR_EINVAL; return result; }
	if (path.front() != '/' && !fs.bound) { result.error = KERNEL_ERROR_EINVAL; return result; }
	auto input = path.front() == '/' ? path : fs.cwd + "/" + path;
	for (size_t p = input.find("//"); p != std::string::npos; p = input.find("//")) { input.erase(p, 1); }
	bool dot_component = false;
	bool terminal_dot = false;
	for (size_t begin = 1; begin < input.size();) {
		const auto end = input.find('/', begin);
		const auto component = input.substr(begin, end == std::string::npos ? end : end - begin);
		if (component.size() > 255) { result.error = KERNEL_ERROR_ENAMETOOLONG; return result; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (HasWindowsForbiddenFilenameCharacter(component) || component.find('\\') != std::string::npos) {
			result.error = KERNEL_ERROR_EINVAL; return result;
		}
#endif
		terminal_dot = component == "." || component == "..";
		dot_component = dot_component || terminal_dot;
		if (end == std::string::npos) { break; }
		begin = end + 1;
	}
	result = Map(input); // Keep dot components in this mapping-only host path.
	result.terminal_dot = terminal_dot;
	result.directory_required = input.back() == '/' || terminal_dot;
	result.unmounted_fallback = result.host.empty() && !dot_component && !result.directory_required;
	result.fallback_data_root = Map("/data").host;
	return result;
}

ResolvedGuestPath MountPoints::Operation::Resolve(const std::string& path, bool creating_directory) const {
	const auto mapping = MapInput(path);
	if (mapping.error != OK) { return mapping; }
	const auto& input = mapping.guest;
	const auto alias = CommonAliasPrefix(input);
	const auto alias_parent = alias != 0 ? input.substr(0, alias) : std::string {};
	std::string current = "/";
	for (size_t begin = 1; begin < input.size();) {
		const auto end = input.find('/', begin);
		const auto component = input.substr(begin, end == std::string::npos ? end : end - begin);
		// Check the previous component BEFORE consuming '.', '..', or a child.
		// Thus missing/../x and regular-file/../x cannot be normalized to success.
		const int error = !alias_parent.empty() && current == alias_parent ? OK : RequireDirectory(current);
		if (error != OK) { auto failed = mapping; failed.error = error; return failed; }
		if (component == "..") {
			if (!alias_parent.empty() && current == alias_parent) {
				current = "/";
			} else if (const int parent_error = ParentDirectory(&current); parent_error != OK) {
				auto failed = mapping; failed.error = parent_error; return failed;
			}
		} else if (!component.empty() && component != ".") {
			if (current != "/") { current += '/'; }
			current += component;
		}
		if (end == std::string::npos) { break; }
		begin = end + 1;
	}
	// PATH_MAX bounds the supplied syscall name, not the accumulated cwd:
	// repeated short relative chdir calls can legitimately produce a longer cwd.
	auto result = Map(current, true);
	result.directory_required = mapping.directory_required;
	result.terminal_dot = mapping.terminal_dot;
	// All traversed components were validated. A normalized unmounted file can
	// now use fallback even if a valid relative traversal contained dot/dotdot.
	result.unmounted_fallback = result.host.empty() && !result.directory_required;
	result.fallback_data_root = mapping.fallback_data_root;
	if (result.error != OK) { return result; }
	if (result.directory_required) {
		result.error = RequireDirectory(current);
		if (creating_directory && result.error == KERNEL_ERROR_ENOENT && input.back() == '/' &&
		    !input.ends_with("/./") && !input.ends_with("/../")) { result.error = OK; }
	} else if (!result.host.empty() && !Common::IsArchivePath(result.host)) {
		std::error_code error;
		(void)std::filesystem::status(result.host, error);
		if (error && error != std::errc::no_such_file_or_directory) { result.error = IdentityError(error); }
	}
	return result;
}

void Initialize() {
	SocketpairProvenance::Initialize(); // Host startup, before guest execution/locks.
	g_mount_points = new MountPoints;
	g_files        = new FileDescriptors;
	g_npdrm_device = std::make_unique<NpDrm::Device>(g_npdrm_persistence);
}

void EmergencyShutdown() {
	if (g_files != nullptr) {
		g_files->CloseAll();
	}
}

void Shutdown() {
	EmergencyShutdown();
	g_npdrm_device.reset();
	delete g_files;
	delete g_mount_points;
	g_files        = nullptr;
	g_mount_points = nullptr;
}

void Mount(const std::filesystem::path& folder, const std::string& point, bool read_only) {

	g_mount_points->Mount(folder, point, read_only);
}

void MountForProgram(const std::filesystem::path& folder, const std::string& point, int32_t program_id, bool read_only) {

	g_mount_points->MountForProgram(folder, point, program_id, read_only);
}

bool IsReadOnly(const std::string& path) {

	return g_mount_points->IsReadOnly(path);
}

void Umount(const std::string& folder_or_point) {

	g_mount_points->Umount(folder_or_point);
}

std::filesystem::path GetRealFilename(const std::string& mounted_file_name) {

	return g_mount_points->ResolvePath(mounted_file_name);
}

std::filesystem::path GetFileRealPath(int d) {
	if (g_files == nullptr) {
		return {};
	}
	auto file = g_files->GetFile(d);
	if (!file) {
		return {};
	}
	return file->real_name;
}

bool IsOpenDescriptor(int d) {
	if (g_files == nullptr) { return false; }
	const auto file = g_files->GetFile(d);
	return file != nullptr && file->opened;
}

int PinWriteEventFile(int fd, size_t low_water, std::shared_ptr<File>* output) {
	if (fd >= 0 && fd <= 2) { return KERNEL_ERROR_EOPNOTSUPP; }
	const auto file = g_files != nullptr ? g_files->GetFile(fd) : nullptr;
	if (!file) { return KERNEL_ERROR_EBADF; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (file->directory || file->special != SpecialFile::None || Common::IsArchivePath(file->real_name)) {
		return KERNEL_ERROR_EOPNOTSUPP;
	}
	if (low_water != 0) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (file->f.IsInvalid()) { return KERNEL_ERROR_EBADF; }
	*output = file;
	return OK;
}

int PollWriteEventFile(const std::shared_ptr<File>& file) {
	// Queue -> retained File is the only lock order. Close never calls a queue
	// while holding File::mutex. No table lock or pathname lookup is needed.
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened || file->f.IsInvalid()) { return KERNEL_ERROR_EBADF; }
	// Matches native filt_vfswrite's unconditional readiness/data=0; this is
	// not authorization for a write (read-only access, EROFS, ENOSPC, etc.).
	return OK;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__APPLE__)
// Unlike backing-alias copies, this checks the actual guest-facing mapping.
static bool NpDrmCopyRange(const void* pointer, size_t size, bool write) {
	auto address = reinterpret_cast<uintptr_t>(pointer);
	if (address == 0 || size > UINTPTR_MAX - address) { return false; }
	const auto end = address + size;
	while (address < end) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) { return false; }
		const DWORD mode = info.Protect & 0xffu;
		const bool readable = mode == PAGE_READONLY || mode == PAGE_READWRITE || mode == PAGE_WRITECOPY ||
		                      mode == PAGE_EXECUTE_READ || mode == PAGE_EXECUTE_READWRITE || mode == PAGE_EXECUTE_WRITECOPY;
		const bool writable = mode == PAGE_READWRITE || mode == PAGE_WRITECOPY ||
		                      mode == PAGE_EXECUTE_READWRITE || mode == PAGE_EXECUTE_WRITECOPY;
		if (!readable || (write && !writable)) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
		const auto length = info.RegionSize;
#else
		mach_vm_address_t base = address;
		mach_vm_size_t length = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const auto status = mach_vm_region(mach_task_self(), &base, &length, VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
		if (object != MACH_PORT_NULL) { mach_port_deallocate(mach_task_self(), object); }
		if (status != KERN_SUCCESS || base > address || (info.protection & VM_PROT_READ) == 0 ||
		    (write && (info.protection & VM_PROT_WRITE) == 0)) { return false; }
#endif
		if (length > UINTPTR_MAX - base || base + length <= address) { return false; }
		address = base + length;
	}
	return true;
}
#endif

static bool NpDrmCopy(void* guest, void* local, size_t size, bool write) {
	if (size == 0) { return true; }
	const auto address = reinterpret_cast<uintptr_t>(guest);
	if (address == 0 || size > UINTPTR_MAX - address) { return false; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!NpDrmCopyRange(guest, size, write)) { return false; }
	SIZE_T copied = 0;
	if (write) {
		// Unlike WriteProcessMemory, this does not retry by temporarily changing
		// RO/RX protection after preflight. Faults stay inside the system call,
		// avoiding the emulator/Mono VEH path of a faulting host memcpy.
		using NtWriteVirtualMemoryFunction = NTSTATUS (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
		const auto module = GetModuleHandleW(L"ntdll.dll");
		const auto write_memory = module != nullptr ? reinterpret_cast<NtWriteVirtualMemoryFunction>(
		    GetProcAddress(module, "NtWriteVirtualMemory")) : nullptr;
		if (write_memory == nullptr) { NpDrmUnsupportedOperation("NtWriteVirtualMemory unavailable"); }
		const NTSTATUS status = write_memory(GetCurrentProcess(), guest, local, size, &copied);
		return status >= 0 && copied == size;
	}
	return ReadProcessMemory(GetCurrentProcess(), guest, local, size, &copied) != 0 && copied == size;
#elif defined(__APPLE__)
	if (!NpDrmCopyRange(guest, size, write)) { return false; }
	if (write) {
		return mach_vm_write(mach_task_self(), address, reinterpret_cast<vm_offset_t>(local),
		                     static_cast<mach_msg_type_number_t>(size)) == KERN_SUCCESS;
	}
	mach_vm_size_t copied = 0;
	return mach_vm_read_overwrite(mach_task_self(), address, size, reinterpret_cast<mach_vm_address_t>(local),
	                              &copied) == KERN_SUCCESS && copied == size;
#elif defined(__linux__)
	iovec local_iov {local, size};
	iovec guest_iov {guest, size};
	// process_vm_* honors the mapping permissions and reports faults without a
	// host signal; never use /proc/self/mem or a writable backing alias here.
	const auto copied = write ? ::process_vm_writev(::getpid(), &local_iov, 1, &guest_iov, 1, 0) :
	                            ::process_vm_readv(::getpid(), &local_iov, 1, &guest_iov, 1, 0);
	return copied == static_cast<ssize_t>(size);
#else
	NpDrmUnsupportedOperation("host copyin/copyout platform");
#endif
}

// Reuse the existing fault-contained, permission-respecting copy primitive.
// Reading page-bounded chunks avoids a host syscall for every pathname byte.
static int CopyGuestPath(const char* path, std::string* output) {
	output->clear();
	const auto address = reinterpret_cast<uintptr_t>(path);
	if (address == 0) { return KERNEL_ERROR_EFAULT; }
	for (size_t offset = 0; offset < 1024;) {
		if (offset > UINTPTR_MAX - address) { return KERNEL_ERROR_EFAULT; }
		const auto cursor = address + offset;
		char bytes[64];
		size_t count = std::min<size_t>({sizeof(bytes), 1024 - offset, 4096 - (cursor & 4095)});
		if (!NpDrmCopy(reinterpret_cast<void*>(cursor), bytes, count, false)) {
			// A short mapped tail can contain the NUL even if the whole chunk is
			// unreadable. Do not turn a valid terminated string into EFAULT.
			count = 1;
			if (!NpDrmCopy(reinterpret_cast<void*>(cursor), bytes, count, false)) { return KERNEL_ERROR_EFAULT; }
		}
		for (size_t i = 0; i < count; ++i) {
			if (bytes[i] == '\0') { return output->empty() ? KERNEL_ERROR_ENOENT : OK; }
			output->push_back(bytes[i]);
		}
		offset += count;
	}
	return KERNEL_ERROR_ENAMETOOLONG;
}

bool CopyPathResultToGuest(void* destination, const void* source, size_t size) {
	return NpDrmCopy(destination, const_cast<void*>(source), size, true);
}

ResolvedGuestPath ResolveGuestPath(const char* path) {
	std::string pathname;
	const int error = CopyGuestPath(path, &pathname);
	if (error != OK) { ResolvedGuestPath result; result.error = error; return result; }
	return g_mount_points->Capture().Resolve(pathname);
}

int ChangeDirectory(const char* path) {
	std::string name;
	if (const int error = CopyGuestPath(path, &name); error != OK) { return error; }
	const auto context = GetCurrentFsContext();
	if (context == nullptr) { return KERNEL_ERROR_EINVAL; }
	for (;;) {
		const auto snapshot = context->Snapshot();
		const auto operation = g_mount_points->Capture(&snapshot);
		const auto resolved = operation.Resolve(name);
		const int error = resolved.error != OK ? resolved.error : operation.RequireDirectory(resolved.guest);
		if (error != OK) {
			LOGF("guest-fs: chdir context=%p cwd=%s path=%s error=%08x\n",
			     static_cast<void*>(context.get()), snapshot.cwd.c_str(), name.c_str(), static_cast<unsigned>(error));
			return error;
		}
		// No mount/context mutex is held across host or archive I/O above.
		if (context->TryChangeDirectory(snapshot.generation, resolved.guest)) {
			LOGF("guest-fs: chdir context=%p cwd=%s -> %s\n", static_cast<void*>(context.get()),
			     snapshot.cwd.c_str(), resolved.guest.c_str());
			return OK;
		}
	}
}

int GetGuestCurrentDirectory(std::string* path) {
	const auto context = GetCurrentFsContext();
	if (context == nullptr) { return KERNEL_ERROR_EINVAL; }
	const auto snapshot = context->Snapshot();
	const auto operation = g_mount_points->Capture(&snapshot);
	const auto resolved = operation.Resolve(".");
	if (resolved.error != OK) { return resolved.error; }
	if (const int error = operation.RequireDirectory(snapshot.cwd); error != OK) { return error; }
	*path = snapshot.cwd;
	return OK;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_test_npdrm_accepted)() = nullptr;
void TestSetNpDrmIoctlAcceptedCallback(void (*callback)()) { g_test_npdrm_accepted = callback; }
static thread_local void (*g_test_sshot_retained)() = nullptr;
static thread_local void (*g_test_sshot_locked)() = nullptr;
void TestSetSshotIoctlRetainedCallback(void (*callback)()) { g_test_sshot_retained = callback; }
void TestSetSshotIoctlLockedCallback(void (*callback)()) { g_test_sshot_locked = callback; }
#endif

bool TryMetaDbgIoctl(int32_t fd, int* result) {
	const auto file = g_files != nullptr ? g_files->GetFile(fd) : nullptr;
	if (!file || file->special != SpecialFile::MetaDbg) { return false; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	*result = file->opened ? KERNEL_ERROR_ENOTTY : KERNEL_ERROR_EBADF;
	return true;
}

bool TryDevctlIoctl(int32_t fd, int* result) {
	if (!g_files || !result) { return false; }
	const auto file = g_files->GetFile(fd);
	if (!file || file->special != SpecialFile::Devctl) { return false; }
	Common::LockGuard lock(file->mutex);
	*result = file->opened ? KERNEL_ERROR_ENOTTY : KERNEL_ERROR_EBADF;
	return true;
}

bool TryM2Ioctl(int32_t fd, uint32_t request, void* arg, int* result) {
	if (!g_files || !result) { return false; }
	const auto file = g_files->GetFile(fd);
	if (!file || file->special != SpecialFile::M2Control) { return false; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { *result = KERNEL_ERROR_EBADF; return true; }
	*result = KERNEL_ERROR_EOPNOTSUPP;
	if (request != 0x80283208u) { return true; }
	// Retail 13.60 kernel m2storage_ctrl_get_summary_info (file +9c00f0):
	// the no-SATA/no-NVMe branch returns ENODEV before any nested copyout.
	// Only the ioctl envelope is copied in; info/hash/controller stay untouched.
	std::array<uint8_t, 40> envelope;
	if (!NpDrmCopy(arg, envelope.data(), envelope.size(), false)) {
		*result = KERNEL_ERROR_EFAULT;
		return true;
	}
	// This controller has no attached storage backend. Attached-device commands
	// remain unsupported; host files never supply device identity or metadata.
	*result = KERNEL_ERROR_ENODEV;
	return true;
}

bool TrySshotIoctl(int32_t fd, uint32_t request, void* arg, int* result) {
	const bool device_request = ((request >> 8) & 0xffu) == 0x9d;
	if (fd >= 0 && fd <= 2) {
		if (!device_request) { return false; }
		*result = KERNEL_ERROR_ENOTTY;
		return true;
	}
	if (::Libs::Network::Net::IsSocket(fd)) {
		if (!device_request) { return false; }
		*result = KERNEL_ERROR_ENOTTY;
		return true;
	}
	// One retained identity, never re-lookup after a close/reuse or guest copy.
	const auto file = g_files != nullptr ? g_files->GetFile(fd) : nullptr;
	if (!device_request && (file == nullptr || file->special != SpecialFile::Sshot)) { return false; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_sshot_retained != nullptr) { g_test_sshot_retained(); }
#endif
	if (file == nullptr) { *result = KERNEL_ERROR_EBADF; return true; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { *result = KERNEL_ERROR_EBADF; return true; }
	if (file->special != SpecialFile::Sshot) { *result = KERNEL_ERROR_ENOTTY; return true; }
	if (!file->readable || !file->writable) { *result = KERNEL_ERROR_EBADF; return true; }
	// Unsupported commands never read or write the argument, status or pool.
	if (request == 0xc4e09d03u) { *result = KERNEL_ERROR_EOPNOTSUPP; return true; }
	if (request != 0xc0709d01u && request != 0xc0709d02u) { *result = KERNEL_ERROR_ENOTTY; return true; }
	std::array<uint8_t, 112> local;
	if (!NpDrmCopy(arg, local.data(), local.size(), false)) { *result = KERNEL_ERROR_EFAULT; return true; }
	if (request == 0xc0709d01u) {
		if (file->sshot_configuration) { *result = KERNEL_ERROR_EBUSY; return true; }
		uint64_t base = 0;
		uint32_t size = 0, auxiliary = 0, option = 0;
		std::memcpy(&base, local.data() + 8, sizeof(base));
		std::memcpy(&size, local.data() + 0x18, sizeof(size));
		std::memcpy(&auxiliary, local.data() + 0x1c, sizeof(auxiliary));
		std::memcpy(&option, local.data() + 0x20, sizeof(option));
		if (size != 0xa200000u || auxiliary != 0x200000u || option != 0x60u ||
		    base == 0 || base > static_cast<uint64_t>(INT64_MAX) - size) {
			*result = KERNEL_ERROR_EINVAL; return true;
		}
		// Audit only establishes zero opaque inputs. Reject rather than silently
		// interpreting or clearing unknown nonzero bytes (including input status).
		for (size_t i = 0; i < local.size(); ++i) {
			if ((i >= 8 && i < 16) || (i >= 0x18 && i < 0x24)) { continue; }
			if (local[i] != 0) { *result = KERNEL_ERROR_EOPNOTSUPP; return true; }
		}
		// Validate the pool base without touching the uncommitted 160-MiB tail.
		// Registration is NOT a claim that the whole reservation is addressable;
		// a future capture producer must validate every offset/span at access time.
		uint8_t probe = 0;
		if (!NpDrmCopy(reinterpret_cast<void*>(base), &probe, 1, false)) {
			*result = KERNEL_ERROR_EFAULT; return true;
		}
	} else {
		if (!file->sshot_configuration ||
		    !std::equal(local.begin() + 4, local.end(), file->sshot_configuration->begin() + 4)) {
			*result = KERNEL_ERROR_EINVAL; return true;
		}
		// Only proven output field: successful release has screenshot status 0.
		std::fill_n(local.begin(), 4, uint8_t {0});
	}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_sshot_locked != nullptr) { g_test_sshot_locked(); }
#endif
	// Transactional HLE policy: failed copyout leaves the registration unchanged.
	// Close cannot return/reuse the slot until all writes and state commits finish.
	if (!NpDrmCopy(arg, local.data(), local.size(), true)) { *result = KERNEL_ERROR_EFAULT; return true; }
	if (request == 0xc0709d01u) { file->sshot_configuration = local; }
	else { file->sshot_configuration.reset(); }
	*result = OK;
	return true;
}

bool TryNpDrmIoctl(int32_t fd, uint32_t request, void* arg, int* result) {
	EXIT_IF(result == nullptr);
	const bool npdrm_request = ((request >> 8) & 0xffu) == 0x4e;
	std::shared_ptr<File> file;
	if (!npdrm_request) {
		// Classification only: unrelated socket and ordinary-file ioctls retain
		// their existing handlers. Do not treat socket-table descriptors as EBADF.
		if (::Libs::Network::Net::IsSocket(fd)) { return false; }
		file = g_files != nullptr ? g_files->GetFile(fd) : nullptr;
		if (file == nullptr || file->special != SpecialFile::NpDrm) { return false; }
	}
	// FreeBSD IOCPARM_LEN is 13 bits, bounded independently of guest pointers.
	std::array<uint8_t, 0x1fff> local {};
	const size_t size = (request >> 16) & 0x1fffu;
	if (!NpDrmCopy(arg, local.data(), size, false)) {
		*result = KERNEL_ERROR_EFAULT;
		return true;
	}
	if (file == nullptr) {
		if (::Libs::Network::Net::IsSocket(fd)) { NpDrmUnsupportedOperation("NPDRM request on socket"); }
		file = g_files != nullptr ? g_files->GetFile(fd) : nullptr;
	}
	if (file == nullptr) {
		*result = KERNEL_ERROR_EBADF;
		return true;
	}
	const auto authority = CurrentNpDrmAuthority();
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened || (!file->readable && !file->writable)) {
			*result = KERNEL_ERROR_EBADF;
			return true;
		}
		if (file->special != SpecialFile::NpDrm) { NpDrmUnsupportedOperation("NPDRM request on ordinary descriptor"); }
		// Acceptance linearizes here. Keep this File reference but release its
		// mutex before entering device -> filesystem persistence. A subsequent
		// close/reuse cannot redirect this accepted operation to the new fd owner.
	}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_npdrm_accepted != nullptr) { g_test_npdrm_accepted(); }
#endif
	const auto outcome = g_npdrm_device->Ioctl(authority, request, std::span<uint8_t>(local).first(size));
	switch (outcome.kind) {
		case NpDrm::ResultKind::Completed:
			*result = NpDrmCopy(arg, local.data(), size, true) ? OK : KERNEL_ERROR_EFAULT;
			return true;
		case NpDrm::ResultKind::NativeError:
			*result = PosixToKernel(outcome.transport_errno.value());
			return true;
		case NpDrm::ResultKind::UnsupportedRequest:
			NpDrmUnsupportedOperation("unaudited ioctl request", request, std::span<const uint8_t>(local).first(size));
		case NpDrm::ResultKind::InvalidBuffer:
			NpDrmUnsupportedOperation("ioctl local buffer contract");
	}
	NpDrmUnsupportedOperation("ioctl result contract");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode) {
	PRINT_NAME();
	const uint64_t observer_caller = SocketpairProvenance::Enabled() ? KYTY_SPP_CALLER : 0;

	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto operation = g_mount_points->Capture();
	const auto resolved = operation.Resolve(pathname);
	if (resolved.error != OK && !(resolved.error == KERNEL_ERROR_ENOENT && resolved.unmounted_fallback)) {
		return resolved.error;
	}
	path = resolved.guest.c_str();
	const bool sshot = std::strcmp(path, "/dev/sshot") == 0;
	const bool metadbg = std::strcmp(path, "/dev/metadbg") == 0;
	const bool devctl = std::strcmp(path, "/dev/devctl") == 0;
	const bool m2ctl = std::strcmp(path, "/dev/m2.ctl") == 0;
	if (m2ctl && flags != 0) { return KERNEL_ERROR_EOPNOTSUPP; }
	// Only the observed retail read-only, nonblocking open is supported.
	if (devctl && flags != 4) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (metadbg) {
		const auto bits = static_cast<uint32_t>(flags);
		if ((bits & 3u) == 3u) { return KERNEL_ERROR_EINVAL; }
		if ((bits & 0x00020000u) != 0) { return KERNEL_ERROR_ENOTDIR; }
		if ((bits & ~(0x00100000u | 4u | 3u)) != 0) { return KERNEL_ERROR_EOPNOTSUPP; }
	}
	if (sshot) {
		// Only the audited O_RDWR open is supported. Do not flow invalid access
		// modes or O_DIRECTORY into the legacy flag assertions below.
		const auto bits = static_cast<uint32_t>(flags);
		if ((bits & 3u) == 3u) { return KERNEL_ERROR_EINVAL; }
		if ((bits & 0x00020000u) != 0) { return KERNEL_ERROR_ENOTDIR; }
		if ((bits & 3u) != 2u) { return KERNEL_ERROR_EACCES; }
		if (bits != 2u) { return KERNEL_ERROR_EOPNOTSUPP; }
	}
	const bool npdrm = std::strcmp(path, "/dev/npdrm") == 0;
	if (npdrm) {
		const int error = g_npdrm_device->Open(CurrentNpDrmAuthority());
		if (error != 0) { return PosixToKernel(error); }
		if ((static_cast<uint32_t>(flags) & 0x00020000u) != 0) { NpDrmUnsupportedOperation("directory open"); }
	}

	auto flags_u = static_cast<uint32_t>(flags);

	LOGF("\t path = %s\n"
	     "\t flags = %08" PRIx32 "\n"
	     "\t mode = %04" PRIx16 "\n",
	     path, flags_u, mode);

	bool append    = (flags_u & 0x0008u) != 0;
	bool fsync     = (flags_u & 0x0080u) != 0;
	bool sync      = (flags_u & 0x0080u) != 0;
	bool creat     = (flags_u & 0x0200u) != 0;
	bool trunc     = (flags_u & 0x0400u) != 0;
	bool excl      = (flags_u & 0x0800u) != 0;
	bool dsync     = (flags_u & 0x1000u) != 0;
	bool direct    = (flags_u & 0x00010000u) != 0;
	bool directory = (flags_u & 0x00020000u) != 0;

	if (direct) {
		LOGF("\t O_DIRECT ignored on host file backend\n");
	}

	flags_u &= 0x3u;

	Common::File::Mode rw_mode = Common::File::Mode::Read;

	switch (flags_u) {
		case 0: rw_mode = Common::File::Mode::Read; break;
		case 1: rw_mode = Common::File::Mode::Write; break;
		case 2: rw_mode = Common::File::Mode::ReadWrite; break;
		default: EXIT("invalid flag_u: %u\n", flags_u);
	}

	EXIT_NOT_IMPLEMENTED(directory && rw_mode != Common::File::Mode::Read);
	EXIT_NOT_IMPLEMENTED(directory && (trunc || creat));

	int  descriptor = g_files->CreateDescriptor(observer_caller);
	auto file       = g_files->GetFile(descriptor);

	EXIT_IF(file == nullptr || file->opened || file->directory);

	file->name        = path;
	if (SocketpairProvenance::Enabled()) {
		file->probe_gls.store(std::strcmp(path, "/temp0/kyty_shm_SceGlsSharedMemory") == 0, std::memory_order_relaxed);
	}
	file->readable    = rw_mode != Common::File::Mode::Write;
	file->writable    = rw_mode != Common::File::Mode::Read;
	file->append      = append;
	file->sync_writes = fsync || sync || dsync;
	file->read_only_mount = resolved.read_only;
	file->access_mode = static_cast<int>(flags_u);
	file->nonblocking = (static_cast<uint32_t>(flags) & 0x0004u) != 0; // FreeBSD O_NONBLOCK
	file->cloexec     = (static_cast<uint32_t>(flags) & 0x00100000u) != 0;
	if (npdrm || sshot || metadbg || devctl || m2ctl) {
		file->real_name = file->name;
		file->special = m2ctl ? SpecialFile::M2Control : devctl ? SpecialFile::Devctl : metadbg ? SpecialFile::MetaDbg : sshot ? SpecialFile::Sshot : SpecialFile::NpDrm;
		if (devctl) { file->devctl = std::make_shared<Devctl::Queue>(); }
		file->opened = true;
		if (metadbg) { LOGF("\tOpen device: /dev/metadbg, [idle event channel]\n"); }
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);
		return descriptor;
	}

	if (!directory && IsRandomDevice(file->name)) {
		file->real_name = file->name;
		file->special   = SpecialFile::Random;
		file->opened    = true;
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);

		LOGF_COLOR(Log::Color::Green, "\tOpen device: %s, [ok]\n",
		           Common::PathToString(file->real_name).c_str());

		return descriptor;
	}

	if (!directory && IsConsoleDevice(file->name)) {
		file->real_name = file->name;
		file->special   = SpecialFile::Console;
		file->opened    = true;
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);

		LOGF_COLOR(Log::Color::Green, "\tOpen device: %s, [ok]\n",
		           Common::PathToString(file->real_name).c_str());

		return descriptor;
	}

	if (!directory && IsHidDevice(file->name)) {
		file->real_name = file->name;
		file->special   = SpecialFile::Hid;
		file->opened    = true;
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);

		LOGF_COLOR(Log::Color::Green, "\tOpen device: %s, [ok]\n",
		           Common::PathToString(file->real_name).c_str());

		return descriptor;
	}

	if (!directory && IsNotificationDevice(file->name)) {
		file->real_name = file->name;
		file->special   = SpecialFile::Notification;
		file->opened    = true;
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);

		LOGF_COLOR(Log::Color::Green, "\tOpen device: %s, [ok]\n",
		           Common::PathToString(file->real_name).c_str());

		return descriptor;
	}

	file->real_name = resolved.host;
	if (file->real_name.empty() && resolved.unmounted_fallback) {
		// ps5-vsh: license DBs are seeded (stat/open) so entitlement paths
		// resolve even when nothing pre-created them.
		Libs::FsSandbox::EnsureLicenseSeed(file->name, resolved.fallback_data_root);
		file->real_name = Libs::FsSandbox::BackingForUnmounted(file->name, false, resolved.fallback_data_root);
		if (file->real_name.empty() && creat) {
			// ps5-vsh chainSUI187: O_CREAT under unmounted roots (per-boot
			// sandboxes like /<hex>/priv/mms/app.db) must create (HW does);
			// probes without O_CREAT above keep exact ENOENT.
			file->real_name = Libs::FsSandbox::BackingForUnmounted(file->name, true, resolved.fallback_data_root);
			if (!file->real_name.empty()) {
				LOGF("\topen O_CREAT backing: %s -> %s\n", file->name.c_str(),
				     Common::PathToString(file->real_name).c_str());
			}
		}
	}
	if (file->real_name.empty()) {
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_ENOENT;
	}
	if (Common::IsArchivePath(file->real_name) &&
	    (rw_mode != Common::File::Mode::Read || trunc || creat)) {
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_EROFS;
	}
	// ps5-vsh: firmware mounts are read-only (voldemort parity); guest
	// writes there fail EROFS instead of touching the dump.
	if ((rw_mode != Common::File::Mode::Read || trunc || creat || append) &&
	    resolved.read_only) {
		LOGF("\topen EROFS (read-only mount): %s\n", file->name.c_str());
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_EROFS;
	}

	// ps5-vsh TEMP: trace all opens.
	LOGF("ps5-vsh: open %s -> %s\n", file->name.c_str(),
	     Common::PathToString(file->real_name).c_str());

	if (trunc && rw_mode == Common::File::Mode::Read) {
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_EACCES;
	}

	const auto info = Common::File::GetInfo(file->real_name);
	const bool dir_exist = info && !info->is_file;
	const bool file_exist = info && info->is_file;

	// A missing path opened without O_CREAT is ENOENT
	if (!creat && !dir_exist && !file_exist) {
		// ps5-vsh: trace missing-path opens; the shell needs /system mounts.
		LOGF("ps5-vsh: open ENOENT: %s (real: %s)\n", file->name.c_str(),
		     Common::PathToString(file->real_name).c_str());
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_ENOENT;
	}

	if (directory || dir_exist) {
		if (!dir_exist) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_ENOTDIR;
		}

		EXIT_NOT_IMPLEMENTED(!directory && rw_mode != Common::File::Mode::Read);
		EXIT_NOT_IMPLEMENTED(!directory && (trunc || creat));

		// Keep the actual directory object alive, not just its pathname. This
		// also makes fstat agree after a host rename or unlink of the directory.
		std::error_code error;
		if (!file->f.OpenDirectory(file->real_name, error)) {
			g_files->DeleteDescriptor(descriptor);
			return IdentityError(error);
		}

		const auto entries = Common::File::GetDirEntries(file->real_name);
		file->dirents      = PackDirents(entries);
		file->dents_offset = 0;
		file->directory    = true;

		LOGF_COLOR(Log::Color::Green, "\tOpen dir: %s, entries = %" PRIu32 ", [ok]\n",
		           Common::PathToString(file->real_name).c_str(),
		           static_cast<uint32_t>(entries.size()));

		for (const auto& f: entries) {
			LOGF("\t\t%s %s\n", f.is_file ? "[file]" : "[dir ]", f.name.c_str());
		}
	} else {
		bool result = false;

		if (excl && creat && file_exist) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EEXIST;
		}

		if (creat && (!file_exist || trunc)) {
			Common::File::CreateDirectories(file->real_name.parent_path());
			result = file->f.Create(file->real_name);

			LOGF_COLOR(result ? Log::Color::Green : Log::Color::Red, "\tCreate: %s, %s\n",
			           Common::PathToString(file->real_name).c_str(), result ? "[ok]" : "[fail]");
		} else {
			result = file->f.Open(file->real_name, rw_mode);

			LOGF_COLOR(result ? Log::Color::Green : Log::Color::Red, "\tOpen: %s, %s\n",
			           Common::PathToString(file->real_name).c_str(), result ? "[ok]" : "[fail]");
		}

		if (result && trunc) {
			result = file->f.Truncate(0);
		}

		if (result && append) {
			file->f.Seek(file->f.Size());
		}

		if (!result || file->f.IsInvalid()) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EACCES;
		}
	}

	file->opened = true;
	SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, descriptor, file.get(), observer_caller);
	return descriptor;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelClose(int d) {
	PRINT_NAME();
	const uint64_t observer_caller = SocketpairProvenance::Enabled() ? KYTY_SPP_CALLER : 0;

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		const auto result = ::Libs::Network::Net::SocketClose(d);
		return (result == OK ? OK : PosixToKernel(::Libs::Network::NetToPosix(result)));
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal (ShellUI
	// input init re-reads after close; EXIT here killed chainSUI runs).
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}

	if (file->special == SpecialFile::NpDrm) {
		const int error = g_npdrm_device->Close(CurrentNpDrmAuthority());
		if (error != 0) { return PosixToKernel(error); }
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		file->opened = false;
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Close, d, file.get(), observer_caller);
		g_files->DeleteDescriptor(d, observer_caller);
		return OK;
	}

	// ps5-vsh: pipe peer shutdown (voldemort parity). Wake a thread
	// blocked in a pipe read so it observes EOF instead of hanging.
	if (file->special == SpecialFile::Pipe && file->pipe) {
		Common::LockGuard lock(file->pipe->mutex);
		file->pipe->open[file->pipe_end] = false;
		file->pipe->ready.SignalAll();
	}

	// ps5-vsh: close under file->mutex: readers hold it across their
	// IsInvalid guard + Read/Write, so an unlocked Close landed mid-op and
	// tripped the IsInvalid EXIT in Common::File (chainSUI26).
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		if (file->special == SpecialFile::None) {
			file->f.Close();
		}
		file->sshot_configuration.reset(); // Never free/unmap the borrowed pool.
		file->opened = false;
		if (file->special == SpecialFile::Devctl && file->devctl) { file->devctl->Close(file->devctl_closed); }
		SocketpairProvenance::Observe(SocketpairProvenance::Event::Close, d, file.get(), observer_caller);
	}
	if (file->special == SpecialFile::MetaDbg) { file->device_ready.SignalAll(); }

	LOGF("\tClose: %s\n", Common::PathToString(file->real_name).c_str());

	g_files->DeleteDescriptor(d, observer_caller);

	return OK;
}

// BSD anonymous pipe pair (voldemort parity: PipeState FIFO). Hands out two
// valid descriptors whose bytes actually transfer; Mono's wakeup backend and
// the JSCd/ShellUI self-pipe need this instead of EBADF.
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCreatePipe(int fildes[2]) {
	PRINT_NAME();
	const uint64_t observer_caller = SocketpairProvenance::Enabled() ? KYTY_SPP_CALLER : 0;

	if (fildes == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	auto state = std::make_shared<PipeState>();

	const int rd    = g_files->CreateDescriptor(observer_caller);
	auto      rfile = g_files->GetFile(rd);
	rfile->opened   = true;
	rfile->readable = true;
	rfile->writable = true;
	rfile->special  = SpecialFile::Pipe;
	rfile->pipe     = state;
	rfile->pipe_end = 0;
	rfile->name     = "<pipe:read>";

	const int wr    = g_files->CreateDescriptor(observer_caller);
	auto      wfile = g_files->GetFile(wr);
	wfile->opened   = true;
	wfile->readable = true;
	wfile->writable = true;
	wfile->special  = SpecialFile::Pipe;
	wfile->pipe     = state;
	wfile->pipe_end = 1;
	wfile->name     = "<pipe:write>";

	fildes[0] = rd;
	fildes[1] = wr;
	SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, rd, rfile.get(), observer_caller);
	SocketpairProvenance::Observe(SocketpairProvenance::Event::Publish, wr, wfile.get(), observer_caller);

	LOGF("\tKernelCreatePipe: read fd = %d, write fd = %d\n", rd, wr);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelFcntl(int d, int cmd, int64_t arg) {
	PRINT_NAME();

	// FreeBSD fcntl commands and flags (PS4/PS5 guest ABI).
	constexpr int F_GETFD = 1;
	constexpr int F_DUPFD = 0;
	constexpr int F_SETFD = 2;
	constexpr int F_GETFL = 3;
	constexpr int F_SETFL = 4;
	constexpr int F_GETLK  = 11;
	constexpr int F_SETLK  = 12;
	constexpr int F_SETLKW = 13;
	constexpr int FD_CLOEXEC      = 1;
	constexpr int GUEST_O_NONBLOCK = 0x0004;
	constexpr int GUEST_O_APPEND   = 0x0008;
	// FreeBSD record-lock type for GETLK "no blocker" reply.
	constexpr int F_UNLCK = 2;
	// FreeBSD guest struct flock layout: l_start(8) l_len(8) l_pid(4)
	// l_type(2) l_whence(2) l_sysid(4). Single-process emulation has no
	// lock contention, so SETLK always succeeds and GETLK reports F_UNLCK.
	struct GuestFlock {
		int64_t l_start;
		int64_t l_len;
		int32_t l_pid;
		int16_t l_type;
		int16_t l_whence;
		int32_t l_sysid;
	};

	LOGF("\t d = %d cmd = %d arg = 0x%016" PRIx64 "\n", d, cmd,
	     static_cast<uint64_t>(arg));

	int socket_result = 0;
	if (::Libs::Network::Net::SocketpairFcntl(d, cmd, arg, &socket_result)) { return socket_result; }

	if (d >= 0 && d <= 2) {
		if (cmd == F_GETFL) {
			return d == 0 ? 0 : 1; // F_GETFL: stdin O_RDONLY, stdout/stderr O_WRONLY
		}
		return 0;
	}

	if (d < DESCRIPTOR_MIN) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr || !file->opened) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}
	if (file->special == SpecialFile::Devctl) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { *Posix::GetErrorAddr() = Posix::POSIX_EBADF; return -1; }
		switch (cmd) {
			case F_DUPFD: {
				if (arg < 0 || arg > 65535) { *Posix::GetErrorAddr() = Posix::POSIX_EINVAL; return -1; }
				const int copy = g_files->CreateDescriptor(0, static_cast<int>(arg));
				auto other = g_files->GetFile(copy);
				other->name = file->name;
				other->real_name = file->real_name;
				other->special = SpecialFile::Devctl;
				other->devctl = file->devctl;
				other->access_mode = 0;
				other->nonblocking = true;
				other->readable = true;
				other->cloexec = false;
				other->opened = true;
				return copy;
			}
			case F_GETFD: return file->cloexec ? FD_CLOEXEC : 0;
			case F_SETFD: file->cloexec = (arg & FD_CLOEXEC) != 0; return 0;
			case F_GETFL: return GUEST_O_NONBLOCK;
			default: *Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP; return -1;
		}
	}
	if (file->special == SpecialFile::NpDrm && cmd != F_GETFD && cmd != F_SETFD && cmd != F_GETFL && cmd != F_SETFL) {
		NpDrmUnsupportedOperation("fcntl (including unimplemented duplication)");
	}
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::M2Control) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { *Posix::GetErrorAddr() = Posix::POSIX_EBADF; return -1; }
		switch (cmd) {
			case F_GETFD: return file->cloexec ? FD_CLOEXEC : 0;
			case F_SETFD:
				if ((static_cast<uint64_t>(arg) & ~uint64_t {FD_CLOEXEC}) != 0) {
					*Posix::GetErrorAddr() = Posix::POSIX_EINVAL; return -1;
				}
				file->cloexec = (arg & FD_CLOEXEC) != 0;
				return 0;
			case F_GETFL: return file->access_mode;
			default: *Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP; return -1;
		}
	}
	if (file->special == SpecialFile::AnonymousShm || file->special == SpecialFile::MetaDbg) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { *Posix::GetErrorAddr() = Posix::POSIX_EBADF; return -1; }
		switch (cmd) {
			case F_GETFD: return file->cloexec ? FD_CLOEXEC : 0;
			case F_SETFD:
				file->cloexec = (static_cast<uint32_t>(arg) & FD_CLOEXEC) != 0;
				return 0;
			case F_GETFL: return file->access_mode | (file->nonblocking ? GUEST_O_NONBLOCK : 0);
			case F_SETFL:
				if ((static_cast<uint64_t>(arg) & ~static_cast<uint64_t>(GUEST_O_NONBLOCK)) == 0) {
					file->nonblocking = (arg & GUEST_O_NONBLOCK) != 0;
					if (file->special == SpecialFile::MetaDbg) { file->device_ready.SignalAll(); }
					return 0;
				}
				break;
			default: break;
		}
		*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
		return -1;
	}

	switch (cmd) {
		case F_GETFD: return (file->cloexec ? FD_CLOEXEC : 0);
		case F_SETFD:
			file->cloexec = ((static_cast<uint32_t>(arg) & FD_CLOEXEC) != 0);
			return 0;
		case F_GETFL:
			return file->access_mode | (file->append ? GUEST_O_APPEND : 0) |
			       (file->nonblocking ? GUEST_O_NONBLOCK : 0);
		case F_SETFL:
			file->nonblocking = ((static_cast<uint32_t>(arg) & GUEST_O_NONBLOCK) != 0);
			file->append      = ((static_cast<uint32_t>(arg) & GUEST_O_APPEND) != 0);
			return 0;
		case F_GETLK: {
			if (arg == 0) {
				*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
				return -1;
			}
			auto* fl = reinterpret_cast<GuestFlock*>(static_cast<uintptr_t>(arg));
			Memory::InvalidateMemory(static_cast<uint64_t>(arg), sizeof(GuestFlock));
			LOGF("\t fcntl F_GETLK: type = %d whence = %d start = %lld len = %lld, reporting F_UNLCK\n",
			     fl->l_type, fl->l_whence, static_cast<long long>(fl->l_start),
			     static_cast<long long>(fl->l_len));
			fl->l_type = static_cast<int16_t>(F_UNLCK);
			return 0;
		}
		case F_SETLK:
		case F_SETLKW: {
			if (arg == 0) {
				*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
				return -1;
			}
			const auto* fl =
			    reinterpret_cast<const GuestFlock*>(static_cast<uintptr_t>(arg));
			LOGF("\t fcntl %s: type = %d whence = %d start = %lld len = %lld, granted\n",
			     (cmd == F_SETLK ? "F_SETLK" : "F_SETLKW"), fl->l_type, fl->l_whence,
			     static_cast<long long>(fl->l_start), static_cast<long long>(fl->l_len));
			return 0;
		}
		default: break;
	}

	LOGF("\t unsupported fcntl cmd %d\n", cmd);
	*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
	return -1;
}

// ps5-vsh: empty directory reads are a hot poll (PSM video_csharp spins on
// an empty savedata mirror). Log the first few per path, then go silent.
static bool LogEmptyDirRead(const std::string& path) {
	static Common::Mutex                        s_mutex;
	static std::unordered_map<std::string, int> s_counts;
	constexpr int kCap = 3;
	Common::LockGuard lock(s_mutex);
	auto&             count = s_counts[path];
	if (count >= kCap) {
		return false;
	}
	++count;
	return true;
}

static int64_t ShmTransfer(const std::shared_ptr<File>& file, void* buffer, size_t count,
                           int64_t offset, bool write, bool positioned) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	if (!file->opened || (write ? !file->writable : !file->readable)) { return KERNEL_ERROR_EBADF; }
	const auto result = Memory::TransferAnonymousShm(file->shm, buffer, count,
	                                                positioned ? offset : file->shm_position, write);
	if (result > 0) {
		if (!positioned) { file->shm_position += result; }
		auto& time = write ? file->shm_stat.st_mtim : file->shm_stat.st_atim;
		SecToTimespec(&time, Common::DateTime::FromSystemUTC().ToUnix());
	}
	return result;
}

static int SshotReject(const std::shared_ptr<File>& file, int error = KERNEL_ERROR_EOPNOTSUPP) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(file->mutex);
	return file->opened ? error : KERNEL_ERROR_EBADF;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static void (*g_test_metadbg_waiting)() = nullptr;
void TestSetMetaDbgWaitingCallback(void (*callback)()) { g_test_metadbg_waiting = callback; }
#endif

// The caller retains this exact object, never looks up a recycled fd between
// vector elements. Also used by scalar read so pipe/device semantics stay shared.
static int64_t ReadFile(const std::shared_ptr<File>& file, int d, void* buf, size_t nbytes) {
	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	// ps5-vsh: reads on closed-never-opened descriptors are EBADF, not fatal
	// (ShellUI input init re-reads after close; EXIT here killed chainSUI2).
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	if (SocketpairProvenance::Enabled()) {
		SocketpairProvenance::Observe(SocketpairProvenance::Event::ReadDispatch, d, file.get(),
		    SocketpairProvenance::ReadCaller(KYTY_SPP_CALLER), nbytes);
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("read"); }
	if (file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_EOPNOTSUPP); }
	if (file->special == SpecialFile::Devctl) {
		if (nbytes == 0) { return file->opened ? 0 : KERNEL_ERROR_EBADF; }
		if (!buf) { return KERNEL_ERROR_EFAULT; }
		if (nbytes > INT_MAX) { return KERNEL_ERROR_EINVAL; }
		size_t bytes = 0;
		const auto result = file->devctl->Read(buf, nbytes, &bytes, true, file->devctl_closed);
		return result == Devctl::Queue::ReadResult::Closed ? KERNEL_ERROR_EBADF :
		       result == Devctl::Queue::ReadResult::Empty ? KERNEL_ERROR_EAGAIN : static_cast<int64_t>(bytes);
	}
	if (file->special == SpecialFile::Sshot) { return SshotReject(file); }
	if (file->special == SpecialFile::AnonymousShm) { return ShmTransfer(file, buf, nbytes, 0, false, false); }
	if (file->special == SpecialFile::MetaDbg) {
		Common::LockGuard lock(file->mutex);
		if (!file->opened || !file->readable) { return KERNEL_ERROR_EBADF; }
		if (nbytes == 0) { return 0; }
		if (!buf) { return KERNEL_ERROR_EFAULT; }
		if (nbytes > INT_MAX) { return KERNEL_ERROR_EINVAL; }
		// SysCore +0x3dadc waits for a 32-byte event header. Returning EOF or
		// dummy bytes would spin or dispatch an invented debugger command.
		if (!file->nonblocking) { LOGF("\tmetadbg read waiting for debugger event\n"); }
		while (file->opened && !file->nonblocking) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			if (g_test_metadbg_waiting) { g_test_metadbg_waiting(); }
#endif
			file->device_ready.Wait(&file->mutex);
		}
		return file->opened ? KERNEL_ERROR_EAGAIN : KERNEL_ERROR_EBADF;
	}
	if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }

	if (nbytes > INT_MAX) {
		return KERNEL_ERROR_EINVAL;
	}
	if (file->directory) {
		Common::LockGuard lock(file->mutex);
		const auto        bytes = ReadDirectory(file.get(), buf, nbytes);
		// ps5-vsh: PSM/ShellCore poll empty savedata mirrors in a hot loop
		// (chainSUI27: 15GB of 0-byte reads). Cap empty-read logs per path.
		if (bytes != 0 || LogEmptyDirRead(Common::PathToString(file->real_name))) {
			LOGF("\tRead %" PRIu64 " directory bytes from: %s\n", bytes,
			     Common::PathToString(file->real_name).c_str());
		}
		return static_cast<int64_t>(bytes);
	}

	if (file->special == SpecialFile::Random) {
		FillRandomBuffer(buf, nbytes);

		LOGF("\tRead %" PRIu64 " random bytes from: %s\n", static_cast<uint64_t>(nbytes),
		     Common::PathToString(file->real_name).c_str());

		return static_cast<int64_t>(nbytes);
	}

	if (file->special == SpecialFile::Console) {
		// UART receive side is empty.
		return 0;
	}

	if (file->special == SpecialFile::Hid) {
		// No HID events queued; EOF keeps bus detection on defaults.
		return 0;
	}

	if (file->special == SpecialFile::Notification) {
		// ps5-vsh: no notifications are queued, and EOF keeps init on default
		// paths (chainSUI180), but neither may we return 0 *immediately*.
		// /dev/notificationN is an event channel, and the SceSysCore guest
		// reader is (13.60 SceSysCore.elf sub_37C60):
		//   while (read(fd, &ev, 10) < 0) ;          // no sleep on error
		//   switch (ev[0]) { 'B'/'E'/'P'/'R'/'e'/'p'/'r' ...  default:
		//       log("[SceSysCore:button] An illegal event rises"); }
		// It never looks at the returned byte count, so an immediate 0 makes
		// it dispatch a *stale stack buffer* and re-read in a tight loop:
		// ~1.2e6 iterations/s of one identical line, i.e. a whole host core
		// burned and ~1.5e8 lines of false evidence in the log. On hardware
		// this read simply parks until an event arrives. We have no producer,
		// so park the caller for a bounded interval and still report EOF: the
		// returned value is unchanged, the "init proceeds on defaults"
		// property is preserved, and the spin rate drops by 5 orders of
		// magnitude. O_NONBLOCK keeps the previous immediate-0 behaviour
		// (an EAGAIN here would only move the guest to its error branch,
		// which is itself a hot loop).
		if (file->nonblocking) {
			return 0;
		}
		static Common::Mutex         s_notification_mutex;
		static Common::CondVar       s_notification_ready;
		static std::atomic<uint64_t> s_notification_reads {0};
		constexpr uint32_t           k_notification_park_us = 100000;
		const auto                   n =
		    s_notification_reads.fetch_add(1, std::memory_order_relaxed);
		// Positive control: bounded first-N plus periodic sampling, so the
		// parked path is provable without putting I/O on it.
		if (n < 8 || (n % 4096) == 0) {
			LOGF("ps5-vsh: notification read #%llu parked %u us (no event queued)\n", n,
			     k_notification_park_us);
		}
		{
			Common::LockGuard lock(s_notification_mutex);
			s_notification_ready.WaitFor(&s_notification_mutex, k_notification_park_us);
		}
		return 0;
	}

	// BSD pipe read (voldemort parity): transfer from this end's FIFO;
	// blocking reads wait for bytes or peer closure.
	if (file->special == SpecialFile::Pipe) {
		if (!file->readable || !file->pipe) {
			return KERNEL_ERROR_EBADF;
		}
		if (nbytes == 0) {
			return 0;
		}
		Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buf), nbytes);
		const auto state       = file->pipe;
		const auto end         = file->pipe_end;
		const bool nonblocking = file->nonblocking;
		Common::LockGuard lock(state->mutex);
		auto&             fifo = state->buffer[end];
		while (fifo.empty()) {
			if (!state->open[1 - end]) {
				return 0;
			}
			if (nonblocking) {
				return KERNEL_ERROR_EAGAIN;
			}
			state->ready.Wait(&state->mutex);
			if (!state->open[end]) {
				return KERNEL_ERROR_EBADF;
			}
		}
		const auto n = std::min<size_t>(nbytes, fifo.size());
		std::memcpy(buf, fifo.data(), n);
		fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(n));
		return static_cast<int64_t>(n);
	}

	file->mutex.Lock();

	// ps5-vsh: bail before Tell/Size/Read (all EXIT on invalid); the guest
	// re-reads descriptors whose host file is gone (e.g. SQLite sidecars).
	if (file->f.IsInvalid()) {
		file->mutex.Unlock();
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}
	const auto pos        = file->f.Tell();
	const auto file_size  = file->f.Size();
	const auto remaining  = pos < file_size ? file_size - pos : 0;
	Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buf),
	                         std::min<uint64_t>(nbytes, remaining));
	uint32_t bytes_read = 0;
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileReadBytes, bytes_read);

	file->mutex.Unlock();

	LOGF("\tRead %u bytes from: %s\n", bytes_read, Common::PathToString(file->real_name).c_str());
	if (bytes_read > 1048576 && buf != nullptr) {
		const auto* head = static_cast<const unsigned char*>(buf);
		LOGF("\tRead-head: %02x %02x %02x %02x\n", head[0], head[1], head[2], head[3]);
	}

	return bytes_read;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes) {
	PRINT_NAME();
	if (nbytes > INT64_MAX) { return KERNEL_ERROR_EINVAL; }
	if (d == 0) { return 0; } // stdin EOF
	if (d < DESCRIPTOR_MIN) { return KERNEL_ERROR_EPERM; }
	if (::Libs::Network::Net::IsSocket(d)) {
		if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }
		const auto result = ::Libs::Network::Net::Recv(d, buf, nbytes, 0);
		return result >= 0 ? result : PosixToKernel(*Posix::GetErrorAddr());
	}
	return ReadFile(g_files->GetFile(d), d, buf, nbytes);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes) {
	if (nbytes > INT64_MAX) { return KERNEL_ERROR_EINVAL; }

	if (d == 1 || d == 2) {
		// A descriptor write must not acquire a host FILE lock: Mono can suspend
		// this guest thread during GC while compositor/PSM logging needs that lock.
		// Share the POSIX byte backend, preserving short writes and kernel errors.
		const auto result = StandardWrite::Write(d, buf, nbytes);
		return result.count >= 0 ? result.count : PosixToKernel(result.error);
	}
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }
		const auto result = ::Libs::Network::Net::Send(d, buf, nbytes, 0);
		return (result >= 0 ? result : PosixToKernel(*Posix::GetErrorAddr()));
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr || !file->opened || !file->writable) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("write"); }
	if (file->special == SpecialFile::Devctl) { return SshotReject(file, KERNEL_ERROR_EBADF); }
	if (file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_EOPNOTSUPP); }
	if (file->special == SpecialFile::MetaDbg) { return SshotReject(file); }
	if (file->special == SpecialFile::Sshot) { return SshotReject(file); }
	if (file->special == SpecialFile::AnonymousShm) { return ShmTransfer(file, const_cast<void*>(buf), nbytes, 0, true, false); }
	if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }

	EXIT_NOT_IMPLEMENTED(file->directory);

	if (file->special == SpecialFile::Console) {
		return WriteConsoleLog(buf, nbytes);
	}
	if (file->special == SpecialFile::Hid) {
		return static_cast<int64_t>(nbytes);
	}
	if (file->special == SpecialFile::Notification) {
		return static_cast<int64_t>(nbytes);
	}
	// BSD pipe write (voldemort parity): append to the peer end's FIFO.
	if (file->special == SpecialFile::Pipe) {
		if (!file->writable || !file->pipe) {
			return KERNEL_ERROR_EBADF;
		}
		Common::LockGuard lock(file->pipe->mutex);
		const unsigned    peer = 1 - file->pipe_end;
		if (!file->pipe->open[peer]) {
			return KERNEL_ERROR_EPIPE;
		}
		const auto* p = static_cast<const uint8_t*>(buf);
		auto&       fifo = file->pipe->buffer[peer];
		fifo.insert(fifo.end(), p, p + nbytes);
		file->pipe->ready.SignalAll();
		return static_cast<int64_t>(nbytes);
	}
	EXIT_NOT_IMPLEMENTED(file->special != SpecialFile::None);

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	file->mutex.Lock();

	if (file->f.IsInvalid()) {
		file->mutex.Unlock();
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}
	uint32_t bytes_written = 0;
	if (file->append) {
		file->f.Seek(file->f.Size());
	}
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileWriteBytes, bytes_written);
	if (file->sync_writes) {
		file->f.Flush();
	}

	file->mutex.Unlock();

	LOGF("\tWrite %u bytes to: %s\n", bytes_written, Common::PathToString(file->real_name).c_str());

	return bytes_written;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();
	if (nbytes > INT64_MAX) { return KERNEL_ERROR_EINVAL; }

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("pread"); }
	if (file->special == SpecialFile::MetaDbg) { return SshotReject(file, KERNEL_ERROR_ESPIPE); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_ESPIPE); }
	if (file->special == SpecialFile::AnonymousShm) { return ShmTransfer(file, buf, nbytes, offset, false, true); }
	if (offset < 0) { return KERNEL_ERROR_EINVAL; }
	if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	if (file->special == SpecialFile::Random) {
		FillRandomBuffer(buf, nbytes);

		LOGF("\tRead %" PRIu64 " random bytes (pos = %" PRId64 ") from: %s\n",
		     static_cast<uint64_t>(nbytes), offset, Common::PathToString(file->real_name).c_str());

		return static_cast<int64_t>(nbytes);
	}

	if (file->special == SpecialFile::Console) {
		return 0;
	}

	// Pipes are not seekable (voldemort parity).
	if (file->special == SpecialFile::Pipe) {
		return KERNEL_ERROR_ESPIPE;
	}

	file->mutex.Lock();

	if (file->f.IsInvalid()) {
		file->mutex.Unlock();
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}
	auto       pos        = file->f.Tell();
	const auto file_size  = file->f.Size();
	const auto remaining =
	    static_cast<uint64_t>(offset) < file_size ? file_size - static_cast<uint64_t>(offset) : 0;
	Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buf),
	                         std::min<uint64_t>(nbytes, remaining));
	uint32_t bytes_read = 0;
	file->f.Seek(offset);
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileReadBytes, bytes_read);
	file->f.Seek(pos);

	file->mutex.Unlock();

	LOGF("\tRead %u bytes (pos = %" PRId64 ") from: %s\n", bytes_read, offset,
	     Common::PathToString(file->real_name).c_str());
	if (bytes_read > 1048576 && buf != nullptr) {
		const auto* head = static_cast<const unsigned char*>(buf);
		LOGF("\tPread-head: %02x %02x %02x %02x\n", head[0], head[1], head[2], head[3]);
		// ps5-vsh: read-integrity audit (Mono metadata hunt). FNV-1a
		// over the delivered bytes; compare with the host file hash.
		uint64_t h = 1469598103934665603ull;
		for (uint32_t k = 0; k < bytes_read; k++) {
			h ^= head[k];
			h *= 1099511628211ull;
		}
		LOGF("\tPread-fnv: %016llx (%u bytes)\n", (unsigned long long)h, bytes_read);
	}

	return bytes_read;
}

static int ValidateIovecs(const KernelIovec* iov, int iovcnt, int64_t offset,
                          std::vector<KernelIovec>* buffers, size_t* total) {
	constexpr int MaxIov = 1024;
	if (iovcnt < 0 || iovcnt > MaxIov || offset < 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (iov == nullptr && iovcnt != 0) {
		return KERNEL_ERROR_EFAULT;
	}

	if (iovcnt != 0) {
		buffers->resize(static_cast<size_t>(iovcnt));
		if (!NpDrmCopy(const_cast<KernelIovec*>(iov), buffers->data(),
		               buffers->size() * sizeof(KernelIovec), false)) { return KERNEL_ERROR_EFAULT; }
	}
	*total = 0;
	for (const auto& buffer: *buffers) {
		if (buffer.iov_len > INT_MAX - *total) {
			return KERNEL_ERROR_EINVAL;
		}
		if (buffer.iov_base == nullptr && buffer.iov_len != 0) {
			return KERNEL_ERROR_EFAULT;
		}
		*total += buffer.iov_len;
	}
	return OK;
}

// Permission preflight, not a mapping lifetime pin. Copyout below remains
// fault-contained if another thread changes a mapping after this check.
static bool VectorReadRange(void* pointer, size_t size) {
	if (size == 0) { return true; }
	auto address = reinterpret_cast<uintptr_t>(pointer);
	if (address == 0 || size > UINTPTR_MAX - address) { return false; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__APPLE__)
	return NpDrmCopyRange(pointer, size, true);
#elif defined(__linux__)
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	const auto end = address + size;
	char line[1024];
	while (address < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		uint64_t first = 0, last = 0;
		char permissions[5] {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &first, &last, permissions) != 3 ||
		    last <= address) { continue; }
		if (first > address || permissions[0] != 'r' || permissions[1] != 'w') { break; }
		address = static_cast<uintptr_t>(std::min<uint64_t>(last, end));
	}
	std::fclose(maps);
	return address == end;
#else
	return false;
#endif
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_test_readv_retained)() = nullptr;
void TestSetReadvRetainedCallback(void (*callback)()) { g_test_readv_retained = callback; }
#endif

static int64_t Readv(int d, const KernelIovec* iov, int iovcnt, std::optional<int64_t> offset) {
	// Unsupported device I/O has no vector ABI: do not dereference guest iovecs.
	// Retain this same File for the later ordinary-file path too.
	const auto file = g_files->GetFile(d);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_readv_retained != nullptr) { g_test_readv_retained(); }
#endif
	if (file != nullptr && (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control)) {
		return SshotReject(file, offset ? KERNEL_ERROR_ESPIPE : KERNEL_ERROR_EOPNOTSUPP);
	}

	std::vector<KernelIovec> buffers;
	size_t                   total = 0;
	const int                error = ValidateIovecs(iov, iovcnt, offset.value_or(0), &buffers, &total);
	if (error != OK) {
		return error;
	}

	const bool socket = GuestDescriptors::IsSocketRange(d);
	if (!offset && d == 0) { return 0; } // Same stdin backend as KernelRead.
	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}
	if (socket && offset) { return KERNEL_ERROR_ESPIPE; }
	if (!socket && (file == nullptr || !file->opened || !file->readable)) {
		return KERNEL_ERROR_EBADF;
	}
	if (!socket && file->special == SpecialFile::NpDrm) {
		// There is no audited NPDRM read backend. Do not invoke its fatal
		// scalar dispatch or invent EOF for this newly exposed operation.
		return SshotReject(file);
	}
	if (total == 0) {
		if (socket) { return ::Libs::Network::Net::IsSocket(d) ? 0 : KERNEL_ERROR_EBADF; }
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		return file->directory ? KERNEL_ERROR_EISDIR : 0;
	}
	if (!offset && (socket || file->special != SpecialFile::None)) {
		// One backend read, not one per element: otherwise a full first iovec
		// can make the second read block on an empty pipe/stream, and datagram
		// vectors would consume multiple packets. Recv retains its own transport.
		for (const auto& buffer: buffers) {
			if (!VectorReadRange(buffer.iov_base, buffer.iov_len)) { return KERNEL_ERROR_EFAULT; }
		}
		std::vector<uint8_t> staging(std::max<size_t>(total, 1));
		const int64_t bytes = socket ? KernelRead(d, staging.data(), total) :
		                              ReadFile(file, d, staging.data(), total);
		if (bytes <= 0) { return bytes; }
		size_t copied = 0;
		for (const auto& buffer: buffers) {
			const auto count = std::min<size_t>(buffer.iov_len, static_cast<size_t>(bytes) - copied);
			if (count != 0) {
				Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buffer.iov_base), count);
				if (!NpDrmCopy(buffer.iov_base, staging.data() + copied, count, true)) {
					return copied != 0 ? static_cast<int64_t>(copied) : KERNEL_ERROR_EFAULT;
				}
				copied += count;
			}
			if (copied == static_cast<size_t>(bytes)) { break; }
		}
		return static_cast<int64_t>(copied);
	}
	if (file->special == SpecialFile::AnonymousShm) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (file->special == SpecialFile::Pipe) { return KERNEL_ERROR_ESPIPE; }

	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (file->directory) {
		return KERNEL_ERROR_EISDIR;
	}
	if (total == 0) {
		return 0;
	}
	if (file->special == SpecialFile::Random) {
		for (const auto& buffer: buffers) {
			if (buffer.iov_len != 0) {
				if (!VectorReadRange(buffer.iov_base, buffer.iov_len)) { return KERNEL_ERROR_EFAULT; }
				Memory::InvalidateMemory(reinterpret_cast<uint64_t>(buffer.iov_base),
				                         buffer.iov_len);
				FillRandomBuffer(buffer.iov_base, buffer.iov_len);
			}
		}
		return static_cast<int64_t>(total);
	}
	if (file->special != SpecialFile::None) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (file->f.IsInvalid()) {
		return KERNEL_ERROR_EIO;
	}

	const auto position  = file->f.Tell();
	const auto file_size = file->f.Size();
	const auto target = offset ? static_cast<uint64_t>(*offset) : position;
	if (target > static_cast<uint64_t>(INT64_MAX) - total) { return KERNEL_ERROR_EINVAL; }
	if (offset && !file->f.Seek(target)) {
		return KERNEL_ERROR_EIO;
	}
	auto    remaining  = target < file_size ? file_size - target : 0;
	int64_t bytes_read = 0;
	int read_error = OK;
	std::array<uint8_t, 65536> staging;
	for (const auto& buffer: buffers) {
		if (remaining == 0) {
			break;
		}
		if (buffer.iov_len == 0) {
			continue;
		}
		const auto length = static_cast<size_t>(std::min<uint64_t>(buffer.iov_len, remaining));
		if (!VectorReadRange(buffer.iov_base, length)) { read_error = KERNEL_ERROR_EFAULT; break; }
		for (size_t copied = 0; copied < length;) {
			const auto count = static_cast<uint32_t>(std::min(length - copied, staging.size()));
			uint32_t bytes = 0;
			// Common::File currently discards the host read error status. Keep
			// its count/EOF behavior; full host-I/O-error parity needs a new
			// retained-handle result API, not guessed CRT errno/GetLastError.
			file->f.Read(staging.data(), count, &bytes);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileReadBytes, bytes);
			auto* destination = static_cast<uint8_t*>(buffer.iov_base) + copied;
			Memory::InvalidateMemory(reinterpret_cast<uint64_t>(destination), bytes);
			if (!NpDrmCopy(destination, staging.data(), bytes, true)) {
				// A protection/unmap race must not crash the host or leave the
				// sequential offset advanced past bytes never delivered.
				file->f.Seek(target + static_cast<uint64_t>(bytes_read));
				read_error = KERNEL_ERROR_EFAULT;
				break;
			}
			bytes_read += bytes;
			remaining -= bytes;
			copied += bytes;
			if (bytes < count) {
				// Preserve the scalar backend's short-read/EOF behavior, even
				// if another handle truncated the file since the size snapshot.
				remaining = 0;
				break;
			}
		}
		if (read_error != OK) { break; }
	}
	if (offset && !file->f.Seek(position)) {
		read_error = KERNEL_ERROR_EIO;
	}
	return bytes_read != 0 || read_error == OK ? bytes_read : read_error;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelReadv(int d, const KernelIovec* iov, int iovcnt) {
	PRINT_NAME();
	try { return Readv(d, iov, iovcnt, std::nullopt); }
	catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPreadv(int d, const KernelIovec* iov, int iovcnt, int64_t offset) {
	PRINT_NAME();
	try { return Readv(d, iov, iovcnt, offset); }
	catch (const std::bad_alloc&) { return KERNEL_ERROR_ENOMEM; }
}

int KernelFutimes(int d, const KernelTimeval* times) {
	static_assert(sizeof(KernelTimeval) == 16 && offsetof(KernelTimeval, tv_usec) == 8);
	std::array<KernelTimeval, 2> values {};
	if (times != nullptr) {
		if (!NpDrmCopy(const_cast<KernelTimeval*>(times), values.data(), sizeof(values), false)) {
			return KERNEL_ERROR_EFAULT;
		}
		for (const auto& value: values) {
			if (value.tv_usec < 0 || value.tv_usec >= 1000000) { return KERNEL_ERROR_EINVAL; }
		}
	}
	if (GuestDescriptors::IsSocketRange(d)) {
		return ::Libs::Network::Net::IsSocket(d) ? KERNEL_ERROR_EOPNOTSUPP : KERNEL_ERROR_EBADF;
	}
	const auto file = g_files->GetFile(d);
	if (file == nullptr) { return KERNEL_ERROR_EBADF; }
	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (file->read_only_mount) { return KERNEL_ERROR_EROFS; }
	if (file->special != SpecialFile::None) { return KERNEL_ERROR_EOPNOTSUPP; }
	Common::FileTimestamp native[2] {};
	for (size_t i = 0; i < 2; ++i) {
		native[i] = {values[i].tv_sec, values[i].tv_usec * 1000};
	}
	std::error_code error;
	return file->f.SetTimestamps(times != nullptr ? native : nullptr, error) ? OK : TimestampError(error);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();
	if (nbytes > INT64_MAX) { return KERNEL_ERROR_EINVAL; }

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr || !file->opened || !file->writable) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("pwrite"); }
	if (file->special == SpecialFile::MetaDbg) { return SshotReject(file, KERNEL_ERROR_ESPIPE); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_ESPIPE); }
	if (file->special == SpecialFile::AnonymousShm) { return ShmTransfer(file, const_cast<void*>(buf), nbytes, offset, true, true); }
	if (offset < 0) { return KERNEL_ERROR_EINVAL; }
	if (buf == nullptr && nbytes != 0) { return KERNEL_ERROR_EFAULT; }

	EXIT_NOT_IMPLEMENTED(file->directory);

	if (file->special == SpecialFile::Console) {
		return WriteConsoleLog(buf, nbytes);
	}
	if (file->special == SpecialFile::Hid) {
		return static_cast<int64_t>(nbytes);
	}
	// Pipes are not seekable (voldemort parity).
	if (file->special == SpecialFile::Pipe) {
		return KERNEL_ERROR_ESPIPE;
	}
	EXIT_NOT_IMPLEMENTED(file->special != SpecialFile::None);

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	file->mutex.Lock();

	if (file->f.IsInvalid()) {
		file->mutex.Unlock();
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}
	auto     pos           = file->f.Tell();
	uint32_t bytes_written = 0;
	file->f.Seek(file->append ? file->f.Size() : static_cast<uint64_t>(offset));
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileWriteBytes, bytes_written);
	if (file->sync_writes) {
		file->f.Flush();
	}
	file->f.Seek(pos);

	file->mutex.Unlock();

	LOGF("\tWrite %u bytes (pos = %" PRId64 ") to: %s\n", bytes_written, offset,
	     Common::PathToString(file->real_name).c_str());

	return bytes_written;
}

static int64_t Writev(int d, const KernelIovec* iov, int iovcnt, std::optional<int64_t> offset) {
	const auto file = g_files->GetFile(d);
	if (file != nullptr && (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control)) {
		return SshotReject(file, offset ? KERNEL_ERROR_ESPIPE : KERNEL_ERROR_EOPNOTSUPP);
	}
	std::vector<KernelIovec> buffers;
	size_t                   total = 0;
	const int error = ValidateIovecs(iov, iovcnt, offset.value_or(0), &buffers, &total);
	if (error != OK) {
		return error;
	}

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}
	if (::Libs::Network::Net::IsSocket(d)) {
		return KERNEL_ERROR_ESPIPE;
	}
	if (file == nullptr || !file->opened || !file->writable) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("pwritev"); }
	if (file->special == SpecialFile::AnonymousShm) { return KERNEL_ERROR_EOPNOTSUPP; }
	Common::LockGuard lock(file->mutex);
	if (file->directory) {
		return KERNEL_ERROR_EISDIR;
	}
	if (file->special != SpecialFile::None) {
		return KERNEL_ERROR_EINVAL;
	}
	if (total == 0) {
		return 0;
	}
	if (file->f.IsInvalid()) {
		return KERNEL_ERROR_EIO;
	}

	const auto position = file->f.Tell();
	auto       target   = position;
	if (file->append) {
		target = file->f.Size();
	} else if (offset) {
		target = static_cast<uint64_t>(*offset);
	}
	if (target > static_cast<uint64_t>(INT64_MAX) - total) {
		return KERNEL_ERROR_EFBIG;
	}
	if ((offset || file->append) && !file->f.Seek(target)) {
		return KERNEL_ERROR_EIO;
	}

	int64_t bytes_written = 0;
	for (const auto& buffer: buffers) {
		if (buffer.iov_len == 0) {
			continue;
		}
		uint32_t bytes = 0;
		file->f.Write(buffer.iov_base, static_cast<uint32_t>(buffer.iov_len), &bytes);
		Common::DebugCounters::Add(Common::DebugCounters::Counter::GuestFileWriteBytes, bytes);
		bytes_written += bytes;
		if (bytes < buffer.iov_len) {
			break;
		}
	}
	const bool flushed  = !file->sync_writes || file->f.Flush();
	const bool restored = !offset || file->f.Seek(position);
	if (!flushed || !restored) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tWritev %" PRId64 " bytes (pos = %" PRIu64 ", iovcnt = %d) to: %s\n", bytes_written,
	     target, iovcnt, Common::PathToString(file->real_name).c_str());
	return bytes_written;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelWritev(int d, const KernelIovec* iov, int iovcnt) {
	PRINT_NAME();
	return Writev(d, iov, iovcnt, std::nullopt);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelPwritev(int d, const KernelIovec* iov, int iovcnt, int64_t offset) {
	PRINT_NAME();
	return Writev(d, iov, iovcnt, offset);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("lseek"); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_ESPIPE); }
	if (file->special == SpecialFile::AnonymousShm) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		uint64_t base = 0;
		if (whence == 1) { base = static_cast<uint64_t>(file->shm_position); }
		else if (whence == 2) { base = Memory::AnonymousShmSize(file->shm); }
		else if (whence != 0) { return KERNEL_ERROR_EINVAL; }
		uint64_t position = 0;
		if (offset < 0) {
			const auto distance = uint64_t {0} - static_cast<uint64_t>(offset);
			if (distance > base) { return KERNEL_ERROR_EINVAL; }
			position = base - distance;
		} else {
			if (base > static_cast<uint64_t>(INT64_MAX - offset)) { return KERNEL_ERROR_EOVERFLOW; }
			position = base + static_cast<uint64_t>(offset);
		}
		file->shm_position = static_cast<int64_t>(position);
		return file->shm_position;
	}

	if (file->special != SpecialFile::None) {
		return KERNEL_ERROR_ESPIPE;
	}

	Common::LockGuard lock(file->mutex);

	if (!file->directory && file->f.IsInvalid()) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	uint64_t base = 0;
	switch (whence) {
		case 0: break;
		case 1: base = file->directory ? file->dents_offset : file->f.Tell(); break;
		case 2: base = file->directory ? file->dirents.size() : file->f.Size(); break;
		default: return KERNEL_ERROR_EINVAL;
	}

	uint64_t position = 0;
	if (offset >= 0) {
		if (base > static_cast<uint64_t>(INT64_MAX - offset)) {
			return KERNEL_ERROR_EOVERFLOW;
		}
		position = base + static_cast<uint64_t>(offset);
	} else {
		const auto distance = uint64_t {0} - static_cast<uint64_t>(offset);
		if (base < distance) {
			return KERNEL_ERROR_EINVAL;
		}
		position = base - distance;
		if (position > INT64_MAX) {
			return KERNEL_ERROR_EOVERFLOW;
		}
	}

	if (file->directory) {
		file->dents_offset = position;
	} else {
		file->f.Seek(position);
		EXIT_IF(file->f.Tell() != position);
	}

	LOGF("\tLseek (pos = %" PRIu64 ") to: %s\n", position,
	     Common::PathToString(file->real_name).c_str());

	return static_cast<int64_t>(position);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelStat(const char* path, FileStat* sb) {
	PRINT_NAME();

	if (sb == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto operation = g_mount_points->Capture();
	auto resolved = operation.Resolve(pathname);
	// Preserve the existing narrowly gated direct-title license seeding even
	// when parent lookup fails. This is not general stat/O_CREAT fallback.
	if (!resolved.guest.empty()) {
		Libs::FsSandbox::EnsureLicenseSeed(resolved.guest, resolved.fallback_data_root);
		// Seeding can create the missing parent as well as the file. Retry the
		// original spelling against the SAME cwd/mount/scope capture, not TLS.
		if (resolved.error == KERNEL_ERROR_ENOENT) { resolved = operation.Resolve(pathname); }
	}
	if (resolved.error != OK) { return resolved.error; }
	path = resolved.guest.c_str();
	if (std::strcmp(path, "/dev/npdrm") == 0) { NpDrmUnsupportedOperation("stat"); }
	if (resolved.guest == "/dev/sshot" || resolved.guest == "/dev/metadbg" || resolved.guest == "/dev/devctl") {
		FileStat stat {};
		stat.st_mode = 0020000u | 0600u;
		return NpDrmCopy(sb, &stat, sizeof(stat), true) ? OK : KERNEL_ERROR_EFAULT;
	}

	LOGF("\t KernelStat: %s\n", path);

	// The unmounted guest root is a namespace node, not a host directory.
	if (resolved.guest == "/" && resolved.host.empty()) {
		*sb = {.st_mode = 0040777u};
		return OK;
	}

	if (IsHidDevice(path)) {
		FileStat stat {};
		stat.st_mode = 0000777u | 0020000u; // S_IFCHR
		stat.st_blksize = 512;
		*sb              = stat;
		return OK;
	}

	auto real_file_name = resolved.host;

	Common::FileIdentity identity;
	if (real_file_name.empty()) { return KERNEL_ERROR_ENOENT; }
	std::error_code identity_error;
	if (!Common::File::GetIdentity(real_file_name, identity, identity_error)) {
		return IdentityError(identity_error);
	}

	const auto info = Common::File::GetInfo(real_file_name);
	if (!info) {
		LOGF("\t file not found\n");
		return KERNEL_ERROR_ENOENT;
	}

	const bool is_dir = !info->is_file;

	FileStat stat {};
	stat.st_mode = 0000777u | (is_dir ? 0040000u : 0100000u);
	const int identity_result = SetFileIdentity(identity, &stat);
	if (identity_result != OK) { return identity_result; }

	auto at = Common::DateTime::FromSystemUTC();
	auto wt = at;

	if (is_dir) {
		stat.st_size =
		    static_cast<int64_t>(PackDirents(Common::File::GetDirEntries(real_file_name)).size());
		stat.st_blksize = 512;
		stat.st_blocks  = stat.st_size / 512;
	} else {
		stat.st_size    = static_cast<int64_t>(info->size);
		stat.st_blksize = 512;
		stat.st_blocks  = (stat.st_size + 511) / 512;

		Common::File::GetLastAccessAndWriteTimeUTC(real_file_name, &at, &wt);
	}

	SecToTimespec(&stat.st_atim, at.ToUnix());
	SecToTimespec(&stat.st_mtim, wt.ToUnix());
	stat.st_ctim     = stat.st_atim;
	stat.st_birthtim = stat.st_mtim;
	*sb              = stat;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelFstat(int d, FileStat* sb) {
	PRINT_NAME();

	if (sb == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	int socket_result = 0;
	if (::Libs::Network::Net::TrySocketFstat(d, sb, &socket_result)) { return socket_result; }

	if (d >= 0 && d <= 2) {
		FileStat attributes {};
		attributes.st_mode    = 0020000u | (d == 0 ? 0444u : 0222u);
		attributes.st_nlink   = 1;
		attributes.st_blksize = 4096;
		*sb                   = attributes;
		return OK;
	}

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("fstat"); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::MetaDbg || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		FileStat stat {};
		stat.st_mode = 0020000u | 0600u;
		return NpDrmCopy(sb, &stat, sizeof(stat), true) ? OK : KERNEL_ERROR_EFAULT;
	}
	if (file->special == SpecialFile::AnonymousShm) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		auto stat = file->shm_stat;
		const auto size = Memory::AnonymousShmSize(file->shm);
		stat.st_size = static_cast<int64_t>(size);
		stat.st_blocks = static_cast<int64_t>((size + 0x3fff) / 0x4000);
		*sb = stat;
		return OK;
	}

	LOGF("\tKernelFstat: %s\n", Common::PathToString(file->real_name).c_str());

	FileStat stat {};
	stat.st_mode = 0000777u | (file->directory ? 0040000u : 0100000u);

	auto at = Common::DateTime::FromSystemUTC();
	auto wt = at;

	if (file->special != SpecialFile::None) {
		stat.st_mode    = 0020644u;
		stat.st_size    = 0;
		stat.st_blksize = 512;
		stat.st_blocks  = 0;
		SecToTimespec(&stat.st_atim, at.ToUnix());
		SecToTimespec(&stat.st_mtim, wt.ToUnix());
		stat.st_ctim     = stat.st_atim;
		stat.st_birthtim = stat.st_mtim;
		*sb              = stat;

		return OK;
	}

	Common::FileIdentity identity;
	Common::FileTimestamp timestamps[2] {};
	bool precise_timestamps = false;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		if (file->f.IsInvalid()) {
			LOGF("\tfile is invalid\n");
			return KERNEL_ERROR_EIO;
		}
		std::error_code error;
		if (!file->f.GetIdentity(identity, error)) { return IdentityError(error); }
		if (!file->directory) {
			stat.st_size = static_cast<int64_t>(file->f.Size());
			stat.st_blocks = (stat.st_size + 511) / 512;
		} else {
			stat.st_size = static_cast<int64_t>(file->dirents.size());
			stat.st_blocks = stat.st_size / 512;
		}
		precise_timestamps = file->f.GetTimestamps(timestamps, error);
		if (!precise_timestamps) {
			if (error != std::errc::operation_not_supported) { return TimestampError(error); }
			// Preserve the legacy archive timestamp query; archives are still
			// explicitly unsupported for timestamp mutation.
			if (!file->directory) { file->f.GetLastAccessAndWriteTimeUTC(&at, &wt); }
		}
	}
	stat.st_blksize = 512;
	const int identity_result = SetFileIdentity(identity, &stat);
	if (identity_result != OK) { return identity_result; }

	if (precise_timestamps) {
		stat.st_atim = {timestamps[0].seconds, timestamps[0].nanoseconds};
		stat.st_mtim = {timestamps[1].seconds, timestamps[1].nanoseconds};
	} else {
		SecToTimespec(&stat.st_atim, at.ToUnix());
		SecToTimespec(&stat.st_mtim, wt.ToUnix());
	}
	stat.st_ctim     = stat.st_atim;
	stat.st_birthtim = stat.st_mtim;
	*sb              = stat;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelTruncate(const char* path, int64_t length) {
	PRINT_NAME();
	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	if (length < 0) {
		return KERNEL_ERROR_EINVAL;
	}
	const auto resolved = g_mount_points->Capture().Resolve(pathname);
	if (resolved.error != OK) { return resolved.error; }
	if (resolved.guest == "/dev/sshot" || resolved.guest == "/dev/metadbg") { return KERNEL_ERROR_EINVAL; }
	if (resolved.read_only) { return KERNEL_ERROR_EROFS; }
	path = resolved.guest.c_str();
	if (IsRandomDevice(path)) {
		return OK;
	}
	const auto& real_name = resolved.host;
	if (real_name.empty()) {
		return KERNEL_ERROR_ENOENT;
	}
	if (Common::IsArchivePath(real_name)) {
		return KERNEL_ERROR_EROFS;
	}

	std::error_code error;
	const auto      status = std::filesystem::status(real_name, error);
	if (!error) {
		if (!std::filesystem::exists(status)) {
			return KERNEL_ERROR_ENOENT;
		}
		if (std::filesystem::is_directory(status)) {
			return KERNEL_ERROR_EISDIR;
		}
		if (std::filesystem::is_regular_file(status)) {
			std::filesystem::resize_file(real_name, static_cast<uint64_t>(length), error);
		}
	}
	if (error) {
		switch (static_cast<std::errc>(error.default_error_condition().value())) {
			case std::errc::no_such_file_or_directory: return KERNEL_ERROR_ENOENT;
			case std::errc::not_a_directory: return KERNEL_ERROR_ENOTDIR;
			case std::errc::filename_too_long: return KERNEL_ERROR_ENAMETOOLONG;
			case std::errc::permission_denied: return KERNEL_ERROR_EACCES;
			case std::errc::read_only_file_system: return KERNEL_ERROR_EROFS;
			case std::errc::file_too_large: return KERNEL_ERROR_EFBIG;
			case std::errc::invalid_argument: return KERNEL_ERROR_EINVAL;
			case std::errc::no_space_on_device: return KERNEL_ERROR_ENOSPC;
			default: return KERNEL_ERROR_EIO;
		}
	}
	LOGF("\tTruncate (size = %" PRId64 ") file: %s\n", length,
	     Common::PathToString(real_name).c_str());
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelFtruncate(int d, int64_t length) {
	PRINT_NAME();

	if (length < 0) { return KERNEL_ERROR_EINVAL; }
	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		return KERNEL_ERROR_EINVAL;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr || !file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("ftruncate"); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_EINVAL); }
	if (file->special == SpecialFile::AnonymousShm) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(file->mutex);
		if (!file->opened) { return KERNEL_ERROR_EBADF; }
		if (!file->writable) { return KERNEL_ERROR_EINVAL; }
		const auto old_size = Memory::AnonymousShmSize(file->shm);
		const int result = Memory::ResizeAnonymousShm(file->shm, length);
		if (result == OK && old_size != static_cast<uint64_t>(length)) {
			SecToTimespec(&file->shm_stat.st_mtim, Common::DateTime::FromSystemUTC().ToUnix());
			file->shm_stat.st_ctim = file->shm_stat.st_mtim;
		}
		return result;
	}

	if (!file->writable) {
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory || file->special != SpecialFile::None) {
		return KERNEL_ERROR_EINVAL;
	}

	Common::LockGuard lock(file->mutex);

	if (file->f.IsInvalid() || !file->f.Truncate(static_cast<uint64_t>(length))) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tFtruncate (size = %" PRId64 ") file: %s\n", length,
	     Common::PathToString(file->real_name).c_str());

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelFsync(int d) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	auto file = g_files->GetFile(d);
	if (file == nullptr || !file->opened) {
		return KERNEL_ERROR_EBADF;
	}

	Common::LockGuard lock(file->mutex);
	if (!file->opened) { return KERNEL_ERROR_EBADF; }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return KERNEL_ERROR_EINVAL; }
	if (file->f.IsInvalid() || !file->f.Flush()) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tFsync: %s\n", Common::PathToString(file->real_name).c_str());
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelUnlink(const char* path) {
	PRINT_NAME();

	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto resolved = g_mount_points->Capture().Resolve(pathname);
	if (resolved.error != OK) { return resolved.error; }
	// The final . or .. still names a directory entry operation, not permission
	// to unlink the normalized parent. Match the existing directory-unlink error.
	if (resolved.terminal_dot || resolved.guest == "/") { return KERNEL_ERROR_EPERM; }
	if (resolved.guest == "/dev/sshot" || resolved.guest == "/dev/metadbg") { return KERNEL_ERROR_EPERM; }
	if (resolved.read_only) { return KERNEL_ERROR_EROFS; }
	const auto& real_file_name = resolved.host;
	if (real_file_name.empty()) { return KERNEL_ERROR_ENOENT; }
	path = resolved.guest.c_str();

	bool is_dir  = Common::File::IsDirectoryExisting(real_file_name);
	bool is_file = Common::File::IsFileExisting(real_file_name);

	if (is_dir) {
		return KERNEL_ERROR_EPERM;
	}

	if (!is_file) {
		return KERNEL_ERROR_ENOENT;
	}

	auto open_file = g_files->GetFile(real_file_name);
	bool  ok        = (open_file != nullptr && open_file->opened && !open_file->directory
	                       ? open_file->f.Unlink()
	                       : Common::File::DeleteFile(real_file_name));

	if (!ok) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tKernelUnlink: %s\n", path);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRename(const char* from, const char* to) {
	PRINT_NAME();

	std::string from_path, to_path;
	if (const int error = CopyGuestPath(from, &from_path); error != OK) { return error; }
	if (const int error = CopyGuestPath(to, &to_path); error != OK) { return error; }
	// Both operands, mount identity and policy share one cwd/mount snapshot.
	const auto operation = g_mount_points->Capture();
	const auto source = operation.Resolve(from_path);
	const auto destination = operation.Resolve(to_path);
	if (source.error != OK) { return source.error; }
	if (destination.error != OK) { return destination.error; }
	if (source.terminal_dot || destination.terminal_dot) { return KERNEL_ERROR_EINVAL; }
	if (source.guest == "/" || destination.guest == "/") { return KERNEL_ERROR_EBUSY; }
	if (source.guest == "/dev/sshot" || destination.guest == "/dev/sshot" ||
	    source.guest == "/dev/metadbg" || destination.guest == "/dev/metadbg") { return KERNEL_ERROR_EPERM; }
	if (source.read_only || destination.read_only) { return KERNEL_ERROR_EROFS; }
	if (source.mount_index != destination.mount_index) { return KERNEL_ERROR_EXDEV; }
	const auto& real_from = source.host;
	const auto& real_to = destination.host;
	from = source.guest.c_str();
	to = destination.guest.c_str();

	if (real_to.empty() || !Common::File::IsFileExisting(real_from)) {
		return KERNEL_ERROR_ENOENT;
	}

	if (Common::File::IsFileExisting(real_to)) {
		Common::File::DeleteFile(real_to);
	}

	if (!Common::File::RenameFile(real_from, real_to)) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tKernelRename: %s -> %s\n", from, to);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep) {
	PRINT_NAME();

	if (fd < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	auto file = g_files->GetFile(fd);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}
	if (file->special == SpecialFile::NpDrm) { NpDrmUnsupportedOperation("getdirentries"); }
	if (file->special == SpecialFile::Sshot || file->special == SpecialFile::Devctl || file->special == SpecialFile::M2Control) { return SshotReject(file, KERNEL_ERROR_ENOTDIR); }

	if (!file->directory || nbytes < static_cast<int>(DIR_BLOCK_SIZE)) {
		return KERNEL_ERROR_EINVAL;
	}

	// ps5-vsh: I/O on closed descriptors is EBADF, never fatal.
	if (!file->opened) {
		return KERNEL_ERROR_EBADF;
	}
	Common::LockGuard lock(file->mutex);
	if (file->dents_offset > file->dirents.size() || file->dents_offset % DIR_BLOCK_SIZE != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	LOGF("\t dir    = %s\n"
	     "\t nbytes = %d\n"
	     "\t offset = %" PRIu64 "\n",
	     Common::PathToString(file->real_name).c_str(), nbytes, file->dents_offset);

	if (basep != nullptr) {
		*basep = static_cast<int64_t>(file->dents_offset);
	}
	return static_cast<int>(
	    ReadDirectory(file.get(), buf, AlignDown(static_cast<uint64_t>(nbytes), DIR_BLOCK_SIZE)));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetdents(int fd, char* buf, int nbytes) {
	PRINT_NAME();

	return KernelGetdirentries(fd, buf, nbytes, nullptr);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelMkdir(const char* path, uint16_t mode) {
	PRINT_NAME();

	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto resolved = g_mount_points->Capture().Resolve(pathname, true);
	if (resolved.error != OK) { return resolved.error; }
	if (resolved.guest == "/dev/sshot" || resolved.guest == "/dev/metadbg") { return KERNEL_ERROR_EEXIST; }
	if (resolved.read_only) { return KERNEL_ERROR_EROFS; }
	path = resolved.guest.c_str();

	LOGF("\t path = %s\n"
	     "\t mode = %04" PRIx16 "\n",
	     path, mode);

	auto real_name = resolved.host;
	if (real_name.empty()) {
		return KERNEL_ERROR_ENOENT;
	}

	if (Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_EEXIST;
	}

	if (!Common::File::CreateDirectory(real_name)) {
		return KERNEL_ERROR_EIO;
	}

	if (!Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRmdir(const char* path) {
	PRINT_NAME();

	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto resolved = g_mount_points->Capture().Resolve(pathname);
	if (resolved.error != OK) { return resolved.error; }
	if (resolved.terminal_dot) { return KERNEL_ERROR_EINVAL; }
	if (resolved.guest == "/") { return KERNEL_ERROR_EBUSY; }
	if (resolved.guest == "/dev/sshot" || resolved.guest == "/dev/metadbg") { return KERNEL_ERROR_ENOTDIR; }
	if (resolved.read_only) { return KERNEL_ERROR_EROFS; }
	path = resolved.guest.c_str();

	LOGF("\t path = %s\n", path);

	auto real_name = resolved.host;
	if (real_name.empty()) { return KERNEL_ERROR_ENOENT; }

	if (!Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_ENOENT;
	}

	if (!Common::File::DeleteDirectory(real_name)) {
		return KERNEL_ERROR_EIO;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCheckReachability(const char* path) {
	PRINT_NAME();

	std::string pathname;
	if (const int error = CopyGuestPath(path, &pathname); error != OK) { return error; }
	const auto resolved = g_mount_points->Capture().Resolve(pathname);
	if (resolved.error != OK) { return resolved.error; }
	path = resolved.guest.c_str();

	LOGF("\t KernelCheckReachability: %s\n", path);

	std::string mounted_path = std::string(path);

	if (mounted_path == "/dev/sshot" || IsRandomDevice(mounted_path) || IsConsoleDevice(mounted_path)) {
		return OK;
	}

	auto real_name = resolved.host;

	if (Common::File::IsFileExisting(real_name) || Common::File::IsDirectoryExisting(real_name)) {
		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

} // namespace Libs::LibKernel::FileSystem
