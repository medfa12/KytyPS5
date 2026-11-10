#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "kernel/pthread.h"
#include "kernel/guestFsContext.h"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace Libs::LibKernel::FileSystem {

struct FileStat {
	uint32_t       st_dev;
	uint32_t       st_ino;
	uint16_t       st_mode;
	uint16_t       st_nlink;
	uint32_t       st_uid;
	uint32_t       st_gid;
	uint32_t       st_rdev;
	KernelTimespec st_atim;
	KernelTimespec st_mtim;
	KernelTimespec st_ctim;
	int64_t        st_size;
	int64_t        st_blocks;
	uint32_t       st_blksize;
	uint32_t       st_flags;
	uint32_t       st_gen;
	int32_t        st_lspare;
	KernelTimespec st_birthtim;
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
};

struct KernelIovec {
	void*  iov_base;
	size_t iov_len;
};

// Native SQLite builds its inode key from these two 32-bit ABI fields.
static_assert(offsetof(FileStat, st_dev) == 0);
static_assert(offsetof(FileStat, st_ino) == 4);
static_assert(sizeof(KernelIovec) == 16);
static_assert(offsetof(KernelIovec, iov_len) == 8);

void Initialize();
void Shutdown();
void EmergencyShutdown();

struct Lifecycle {
	static constexpr const char* name        = "FileSystem";
	static constexpr auto        initialize  = Libs::LibKernel::FileSystem::Initialize;
	static constexpr auto        shutdown    = Libs::LibKernel::FileSystem::Shutdown;
	static constexpr auto emergency_shutdown = Libs::LibKernel::FileSystem::EmergencyShutdown;
};

void                  Mount(const std::filesystem::path& folder, const std::string& point,
                              bool read_only = false);
void                  MountForProgram(const std::filesystem::path& folder, const std::string& point,
	                                  int32_t program_id, bool read_only = false);
bool                  IsReadOnly(const std::string& path);
void                  Umount(const std::string& folder_or_point);
struct ResolvedGuestPath {
	std::string guest;
	std::filesystem::path host;
	int error = 0; // Kernel-coded error.
	bool read_only = false;
	bool directory_required = false;
	bool terminal_dot = false; // Last nonempty component was . or ..; never discard for mutations.
	bool unmounted_fallback = false; // Syntax-safe, unmapped file name; may retain ENOENT from parent lookup.
	std::filesystem::path fallback_data_root; // /data from this operation's mount/scope snapshot.
	int mount_index = -1; // Comparable only within the same internal operation.
};
// Copyin + one cwd/mount snapshot. Consumers must use this result's guest path
// for any explicit fallback, and its policy for the returned backing path.
ResolvedGuestPath ResolveGuestPath(const char* path);
// Mapping-only compatibility API: does not require intermediate directories to
// exist (mount planning/owned creation rely on this). Guest syscalls use ResolveGuestPath.
std::filesystem::path GetRealFilename(const std::string& mounted_file_name);
std::filesystem::path GetFileRealPath(int d);

// Kernel-coded errors; the POSIX exports translate these through guest errno.
int ChangeDirectory(const char* path);
int GetGuestCurrentDirectory(std::string* path);
bool CopyPathResultToGuest(void* destination, const void* source, size_t size);

// true: NPDRM route consumed; *result is a kernel-coded return value. false:
// unrelated ioctl, leaving *result/arg untouched for the existing handler.
// Call with low-32-bit request and signed-32-bit fd. NPDRM copyin precedes fd
// validation; copyout failure never rolls back driver state or persistence.
// All group-'N' requests and all requests on retained NPDRM files are consumed.
// Unaudited request/device-operation ABIs stop explicitly (EXIT_NOT_IMPLEMENTED).
// Driver -> filesystem is the lock order: callers must hold no filesystem lock.
bool TryNpDrmIoctl(int32_t fd, uint32_t request, void* arg, int* result);
// Lifecycle-only /dev/sshot. Consumes every ioctl on this device and every
// group-0x9d request (including invalid/wrong fds). Kernel-coded result; capture
// is explicitly unsupported. Retains one File and serializes copyout with close.
bool TrySshotIoctl(int32_t fd, uint32_t request, void* arg, int* result);
bool TryMetaDbgIoctl(int32_t fd, int* result);
bool TryDevctlIoctl(int32_t fd, int* result);
bool TryM2Ioctl(int32_t fd, uint32_t request, void* arg, int* result);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestSetMetaDbgWaitingCallback(void (*callback)());
#endif
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestSetSshotIoctlRetainedCallback(void (*callback)());
void TestSetSshotIoctlLockedCallback(void (*callback)());
void TestSetNpDrmIoctlAcceptedCallback(void (*callback)());
void TestSetShmMmapRetainedCallback(void (*callback)());
void TestSetMmapFileReadBeforeLockCallback(void (*callback)());
void TestSetReadvRetainedCallback(void (*callback)());
uint64_t TestAnonymousShmDescriptorCount();
#endif
// Returns whether d is an open file, directory or device descriptor.
bool                  IsOpenDescriptor(int d);

int KYTY_SYSV_ABI     KernelOpen(const char* path, int flags, uint16_t mode);
int KernelShmOpenAnonymous(int flags, uint16_t mode);
struct File;
// Write-event subset: ordinary host files, including read-only access modes.
// Native generic VFS readiness is always true/data=0, not write authorization.
// Retains File identity; no path reopening or numeric-fd lookup during polling.
// Nonzero NOTE_LOWAT and all special/pipe/archive/directory backends are explicit
// EOPNOTSUPP. Poll returns OK for a live retained file, kernel EBADF after close.
int PinWriteEventFile(int fd, size_t low_water, std::shared_ptr<File>* file);
int PollWriteEventFile(const std::shared_ptr<File>& file);
// Consumes anonymous objects AND invalid/closed descriptors. The single retained
// lookup is also the mapping operation's lifetime reference; consumed failure
// must never fall through to libC's legacy named-file copying/cache route.
bool TryMmapAnonymous(int fd, void* address, size_t length, int protection, int flags,
                      int64_t offset, void** output, int* error, std::shared_ptr<File>* legacy_file = nullptr);
// The legacy named/copy branch must retain the SAME descriptor object too:
// a concurrent close/reuse must not turn a non-SHM dispatch into a copied SHM.
int MmapFileInfo(const std::shared_ptr<File>& file, std::filesystem::path* path, uint64_t* size);
int64_t MmapFileRead(const std::shared_ptr<File>& file, void* buffer, size_t count, int64_t offset);
int KYTY_SYSV_ABI     KernelClose(int d);
int KYTY_SYSV_ABI     KernelMmap(void* address, size_t length, int protection, int flags,
                               int fd, int64_t offset, void** result);
int KYTY_SYSV_ABI     KernelCreatePipe(int fildes[2]);
int KYTY_SYSV_ABI     KernelFcntl(int d, int cmd, int64_t arg);
int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelReadv(int d, const KernelIovec* iov, int iovcnt);
int64_t KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelPreadv(int d, const KernelIovec* iov, int iovcnt, int64_t offset);
int64_t KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelWritev(int d, const KernelIovec* iov, int iovcnt);
int64_t KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelPwritev(int d, const KernelIovec* iov, int iovcnt, int64_t offset);
int64_t KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence);
int KYTY_SYSV_ABI     KernelStat(const char* path, FileStat* sb);
int KYTY_SYSV_ABI     KernelFstat(int d, FileStat* sb);
// Kernel-coded POSIX futimes core. Mutates the retained host handle under its
// descriptor lock; unsupported backends fail explicitly with EOPNOTSUPP.
int KernelFutimes(int d, const KernelTimeval* times);
int KYTY_SYSV_ABI     KernelTruncate(const char* path, int64_t length);
int KYTY_SYSV_ABI     KernelFtruncate(int d, int64_t length);
int KYTY_SYSV_ABI     KernelFsync(int d);
int KYTY_SYSV_ABI     KernelUnlink(const char* path);
int KYTY_SYSV_ABI     KernelRename(const char* from, const char* to);
int KYTY_SYSV_ABI     KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep);
int KYTY_SYSV_ABI     KernelGetdents(int fd, char* buf, int nbytes);
int KYTY_SYSV_ABI     KernelMkdir(const char* path, uint16_t mode);
int KYTY_SYSV_ABI     KernelRmdir(const char* path);
int KYTY_SYSV_ABI     KernelCheckReachability(const char* path);

} // namespace Libs::LibKernel::FileSystem

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_ */
