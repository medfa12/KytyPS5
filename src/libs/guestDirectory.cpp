#include "libs/guestDirectory.h"

#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Libs::GuestDirectory {
namespace {
struct Stream {
	std::mutex mutex;
	int fd = -1;
	// KernelGetdirentries returns FreeBSD records in 512-byte blocks:
	// u32 fileno, u16 reclen, u8 type, u8 namlen, NUL-terminated name.
	alignas(8) std::array<char, 4096> buffer {};
	size_t next = 0;
	size_t available = 0;
};
std::mutex g_mutex;
std::unordered_map<void*, std::shared_ptr<Stream>> g_streams;

std::shared_ptr<Stream> Find(void* key) {
	std::lock_guard lock(g_mutex);
	const auto found = g_streams.find(key);
	if (found != g_streams.end()) { return found->second; }
	*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
	return {};
}

void Error(int result) { *Posix::GetErrorAddr() = LibKernel::KernelToPosix(result); }
} // namespace

void* Open(const char* path) {
	Common::HostLockCriticalGuard critical;
	int fd = -1;
	try {
		auto stream = std::make_shared<Stream>();
		fd = LibKernel::FileSystem::KernelOpen(path, 0x120000, 0); // O_DIRECTORY | O_CLOEXEC
		if (fd < 0) { Error(fd); return nullptr; }
		stream->fd = fd;
		std::lock_guard lock(g_mutex);
		const auto key = stream.get();
		g_streams.emplace(key, std::move(stream));
		return key;
	} catch (const std::bad_alloc&) {
		if (fd >= 0) { LibKernel::FileSystem::KernelClose(fd); }
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return nullptr;
	}
}

void* Read(void* key) {
	Common::HostLockCriticalGuard critical;
	auto stream = Find(key);
	if (!stream) { return nullptr; }
	std::lock_guard lock(stream->mutex);
	if (stream->fd < 0) { *Posix::GetErrorAddr() = Posix::POSIX_EBADF; return nullptr; }
	for (;;) {
		if (stream->next == stream->available) {
			const int size = LibKernel::FileSystem::KernelGetdents(stream->fd, stream->buffer.data(),
			    static_cast<int>(stream->buffer.size()));
			if (size < 0) { Error(size); return nullptr; }
			if (size == 0) { return nullptr; } // EOF preserves errno.
			stream->next = 0;
			stream->available = static_cast<size_t>(size);
		}
		char* entry = stream->buffer.data() + stream->next;
		const size_t remaining = stream->available - stream->next;
		uint16_t length = 0;
		if (remaining >= 8) { std::memcpy(&length, entry + 4, sizeof(length)); }
		if (length < 12 || length > remaining || length % 4 != 0 ||
		    static_cast<unsigned char>(entry[7]) + 9u > length ||
		    entry[8 + static_cast<unsigned char>(entry[7])] != '\0') {
			*Posix::GetErrorAddr() = Posix::POSIX_EIO;
			return nullptr;
		}
		stream->next += length;
		uint32_t inode = 0;
		std::memcpy(&inode, entry, sizeof(inode));
		if (inode != 0) { return entry; }
	}
}

int Close(void* key) {
	Common::HostLockCriticalGuard critical;
	std::shared_ptr<Stream> stream;
	{
		std::lock_guard lock(g_mutex);
		const auto found = g_streams.find(key);
		if (found == g_streams.end()) { *Posix::GetErrorAddr() = Posix::POSIX_EBADF; return -1; }
		stream = std::move(found->second);
		g_streams.erase(found);
	}
	std::lock_guard lock(stream->mutex);
	const int result = LibKernel::FileSystem::KernelClose(stream->fd);
	stream->fd = -1;
	if (result < 0) { Error(result); return -1; }
	return 0;
}
} // namespace Libs::GuestDirectory
