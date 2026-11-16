#include "libs/standardWrite.h"

#include "libs/errno.h"

#include <algorithm>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace Libs::StandardWrite {

Result Write(int descriptor, const void* bytes, uint64_t length) {
	if (descriptor != 1 && descriptor != 2) { return {-1, Posix::POSIX_EBADF}; }
	if (length > static_cast<uint64_t>(INT64_MAX)) { return {-1, Posix::POSIX_EINVAL}; }
	if (bytes == nullptr && length != 0) { return {-1, Posix::POSIX_EFAULT}; }
#ifdef _WIN32
	const auto handle = GetStdHandle(descriptor == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) { return {-1, Posix::POSIX_EBADF}; }
	if (length == 0) { return {0, 0}; }
	const auto request = static_cast<DWORD>((std::min)(length, uint64_t {MAXDWORD}));
	DWORD written = 0;
	if (WriteFile(handle, bytes, request, &written, nullptr)) {
		// PIPE_NOWAIT byte pipes report a full buffer as successful zero-byte
		// progress. POSIX exposes that condition as would-block, not success.
		if (written == 0 && GetFileType(handle) == FILE_TYPE_PIPE) { return {-1, Posix::POSIX_EAGAIN}; }
		return {written, 0};
	}
	const auto error = GetLastError();
	// A backend may have transferred a prefix before reporting failure. Never
	// discard real progress or claim that the untransferred suffix was written.
	if (written != 0) { return {written, 0}; }
	switch (error) {
		case ERROR_INVALID_HANDLE: case ERROR_ACCESS_DENIED: return {-1, Posix::POSIX_EBADF};
		case ERROR_NOACCESS: case ERROR_INVALID_USER_BUFFER: return {-1, Posix::POSIX_EFAULT};
		case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: case ERROR_PIPE_NOT_CONNECTED: return {-1, Posix::POSIX_EPIPE};
		case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return {-1, Posix::POSIX_ENOSPC};
		case ERROR_OPERATION_ABORTED: return {-1, Posix::POSIX_EINTR};
		case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return {-1, Posix::POSIX_ENOMEM};
		case ERROR_INVALID_PARAMETER: return {-1, Posix::POSIX_EINVAL};
		case ERROR_NO_SYSTEM_RESOURCES: return {-1, Posix::POSIX_EAGAIN};
		default: return {-1, Posix::POSIX_EIO};
	}
#else
	const auto written = ::write(descriptor, bytes, static_cast<size_t>(length));
	if (written >= 0) { return {written, 0}; }
	switch (errno) {
		case EBADF: return {-1, Posix::POSIX_EBADF};
		case EFAULT: return {-1, Posix::POSIX_EFAULT};
		case EINVAL: return {-1, Posix::POSIX_EINVAL};
		case EPIPE: return {-1, Posix::POSIX_EPIPE};
		case ENOSPC: return {-1, Posix::POSIX_ENOSPC};
		case EINTR: return {-1, Posix::POSIX_EINTR};
		case EAGAIN: return {-1, Posix::POSIX_EAGAIN};
		case ENOMEM: return {-1, Posix::POSIX_ENOMEM};
		case EFBIG: return {-1, Posix::POSIX_EFBIG};
		default: return {-1, Posix::POSIX_EIO};
	}
#endif
}

} // namespace Libs::StandardWrite
