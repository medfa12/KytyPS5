#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/platform/sysFileIO.h"
#include "common/file.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming)
enum sys_file_type_t {
	SYS_FILE_ERROR,       // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_STAT, // NOLINT(readability-identifier-naming)
	SYS_FILE_FILE,        // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_DYN   // NOLINT(readability-identifier-naming)
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_mem_buf_t {
	uint8_t* base;
	uint8_t* ptr;
	uint32_t size;
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	sys_file_type_t type;
	union {
		HANDLE              handle;
		sys_file_mem_buf_t* buf;
	};
};

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

constexpr DWORD FILE_SHARE_POSIX = static_cast<DWORD>(FILE_SHARE_READ) |
                                   static_cast<DWORD>(FILE_SHARE_WRITE) |
                                   static_cast<DWORD>(FILE_SHARE_DELETE);

static DWORD GetCacheAccessType(sys_file_cache_type_t t) {
	if (t == SYS_FILE_CACHE_RANDOM_ACCESS) {
		return FILE_FLAG_RANDOM_ACCESS;
	}

	if (t == SYS_FILE_CACHE_SEQUENTIAL_SCAN) {
		return FILE_FLAG_SEQUENTIAL_SCAN;
	}

	return FILE_ATTRIBUTE_NORMAL;
}

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	if (f.type == SYS_FILE_FILE) {
		DWORD w = 0;
		ReadFile(f.handle, data, size, &w, nullptr);
		if (bytes_read != nullptr) {
			*bytes_read = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		} else {
			s = 0;
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	if (f.type == SYS_FILE_FILE) {
		DWORD w = 0;
		WriteFile(f.handle, data, size, &w, nullptr);
		if (bytes_written != nullptr) {
			*bytes_written = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(f.buf->ptr, data, s);
		f.buf->ptr += s;
		if (bytes_written != nullptr) {
			*bytes_written = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t pos = f.buf->ptr - f.buf->base;
		if (f.buf->size < pos + size) {
			f.buf->base = static_cast<uint8_t*>(std::realloc(f.buf->base, pos + size));
			f.buf->ptr  = f.buf->base + pos;
			f.buf->size = pos + size;
		}
		std::memcpy(f.buf->ptr, data, size);
		f.buf->ptr += size;
		if (bytes_written != nullptr) {
			*bytes_written = size;
		}
	}
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	auto* ret = new sys_file_t;

	auto   wide   = file_name.wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	ret->handle = h_file;
	ret->type   = SYS_FILE_FILE;

	return ret;
}

sys_file_t* SysFileOpenMetadata(const std::filesystem::path& name, std::error_code& error) {
	const bool trailing = !name.empty() && !name.has_filename() && name != name.root_path();
	const auto path = trailing ? name.parent_path() : name;
	// Allocate before acquiring a handle so allocation failure cannot leak it.
	auto* ret = new sys_file_t;
	ret->handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_POSIX, nullptr,
	                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (ret->handle == INVALID_HANDLE_VALUE) {
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		delete ret;
		return nullptr;
	}
	ret->type = SYS_FILE_FILE;
	if (trailing) {
		BY_HANDLE_FILE_INFORMATION info {};
		const bool ok = GetFileInformationByHandle(ret->handle, &info) != 0;
		if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			error = ok ? std::make_error_code(std::errc::not_a_directory)
			           : std::error_code(static_cast<int>(GetLastError()), std::system_category());
			SysFileClose(ret);
			return nullptr;
		}
	}
	error.clear();
	return ret;
}

sys_file_t* SysFileOpenDirectory(const std::filesystem::path& name, std::error_code& error) {
	const auto path = !name.has_filename() && name != name.root_path() ? name.parent_path() : name;
	auto* ret = new sys_file_t;
	// FlushFileBuffers requires GENERIC_WRITE even for a guest O_RDONLY
	// directory. Acquire it now: reopening the pathname during fsync could
	// flush a replacement object after rename, and ReOpenFile cannot upgrade
	// the metadata-only directory handle on Windows.
	ret->handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | GENERIC_WRITE, FILE_SHARE_POSIX,
	                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (ret->handle == INVALID_HANDLE_VALUE &&
	    (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_WRITE_PROTECT)) {
		// Read-only host directories must still open for traversal/identity.
		// A later flush reports the actual host failure, never synthetic success.
		ret->handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_POSIX,
		                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	}
	if (ret->handle == INVALID_HANDLE_VALUE) {
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		delete ret;
		return nullptr;
	}
	ret->type = SYS_FILE_FILE;
	BY_HANDLE_FILE_INFORMATION info {};
	const bool ok = GetFileInformationByHandle(ret->handle, &info) != 0;
	if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		error = ok ? std::make_error_code(std::errc::not_a_directory)
		           : std::error_code(static_cast<int>(GetLastError()), std::system_category());
		SysFileClose(ret);
		return nullptr;
	}
	error.clear();
	return ret;
}

bool SysFileGetIdentity(sys_file_t& f, Common::FileIdentity& identity, std::error_code& error) {
	if (f.type != SYS_FILE_FILE || f.handle == INVALID_HANDLE_VALUE) {
		error = std::make_error_code(std::errc::bad_file_descriptor);
		return false;
	}
	Common::FileIdentity result;
	FILE_ID_INFO info {};
	if (GetFileInformationByHandleEx(f.handle, FileIdInfo, &info, sizeof(info)) != 0) {
		result.device = info.VolumeSerialNumber;
		static_assert(sizeof(info.FileId.Identifier) == 2 * sizeof(uint64_t));
		std::memcpy(&result.inode_low, info.FileId.Identifier, sizeof(uint64_t));
		std::memcpy(&result.inode_high, info.FileId.Identifier + sizeof(uint64_t), sizeof(uint64_t));
	} else {
		const auto code = GetLastError();
		// Older filesystems may only provide the 64-bit file index. Do not turn
		// access/I/O failures into a successful zero or a path-derived identity.
		if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED &&
		    code != ERROR_INVALID_FUNCTION) {
			error = std::error_code(static_cast<int>(code), std::system_category());
			return false;
		}
		BY_HANDLE_FILE_INFORMATION legacy {};
		if (GetFileInformationByHandle(f.handle, &legacy) == 0) {
			error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			return false;
		}
		result.device = legacy.dwVolumeSerialNumber;
		result.inode_low = (static_cast<uint64_t>(legacy.nFileIndexHigh) << 32u) | legacy.nFileIndexLow;
	}
	identity = std::move(result);
	error.clear();
	return true;
}

namespace {
constexpr int64_t FileTimeEpochSeconds = 11644473600LL;
constexpr uint64_t FileTimeTicksPerSecond = 10000000;

Common::FileTimestamp DecodeTimestamp(const FILETIME& time) {
	const uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
	return {static_cast<int64_t>(ticks / FileTimeTicksPerSecond) - FileTimeEpochSeconds,
	        static_cast<int64_t>(ticks % FileTimeTicksPerSecond) * 100};
}

bool EncodeTimestamp(const Common::FileTimestamp& time, FILETIME* output, std::error_code& error) {
	if (time.nanoseconds < 0 || time.nanoseconds >= 1000000000 || time.nanoseconds % 100 != 0) {
		error = std::make_error_code(std::errc::invalid_argument);
		return false;
	}
	// SetFileTime requires nonnegative signed-64-bit 100ns values. Validate
	// both fields before conversion, without signed addition/multiply overflow.
	constexpr auto max_ticks = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	if (time.seconds < -FileTimeEpochSeconds ||
	    time.seconds > static_cast<int64_t>(max_ticks / FileTimeTicksPerSecond) - FileTimeEpochSeconds) {
		error = std::make_error_code(std::errc::value_too_large);
		return false;
	}
	const auto whole = static_cast<uint64_t>(time.seconds + FileTimeEpochSeconds) * FileTimeTicksPerSecond;
	const auto fraction = static_cast<uint64_t>(time.nanoseconds / 100);
	if (fraction > max_ticks - whole) {
		error = std::make_error_code(std::errc::value_too_large);
		return false;
	}
	const uint64_t ticks = whole + fraction;
	if (ticks == 0) {
		// All-zero FILETIME means "leave unchanged" to SetFileTime, not an
		// expressible 1601-01-01 timestamp. Never acknowledge that as mutation.
		error = std::make_error_code(std::errc::value_too_large);
		return false;
	}
	output->dwLowDateTime = static_cast<DWORD>(ticks);
	output->dwHighDateTime = static_cast<DWORD>(ticks >> 32u);
	return true;
}
} // namespace

bool SysFileGetTimestamps(sys_file_t& f, Common::FileTimestamp (&times)[2], std::error_code& error) {
	if (f.type != SYS_FILE_FILE) {
		error = std::make_error_code(f.type == SYS_FILE_ERROR ? std::errc::bad_file_descriptor :
		                                                        std::errc::operation_not_supported);
		return false;
	}
	FILETIME access {}, write {};
	if (!GetFileTime(f.handle, nullptr, &access, &write)) {
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		return false;
	}
	times[0] = DecodeTimestamp(access);
	times[1] = DecodeTimestamp(write);
	error.clear();
	return true;
}

bool SysFileSetTimestamps(sys_file_t& f, const Common::FileTimestamp* times, std::error_code& error) {
	if (f.type != SYS_FILE_FILE) {
		error = std::make_error_code(f.type == SYS_FILE_ERROR ? std::errc::bad_file_descriptor :
		                                                        std::errc::operation_not_supported);
		return false;
	}
	FILETIME access {}, write {};
	if (times == nullptr) {
		GetSystemTimePreciseAsFileTime(&access);
		write = access;
	} else if (!EncodeTimestamp(times[0], &access, error) || !EncodeTimestamp(times[1], &write, error)) {
		return false;
	}
	// One call on the original handle: validate/convert the entire pair first,
	// never reopen a path or partially apply one timestamp. Do not upgrade access
	// rights at read/metadata open time; a handle lacking FILE_WRITE_ATTRIBUTES
	// gets the actual ERROR_ACCESS_DENIED from SetFileTime.
	if (!SetFileTime(f.handle, nullptr, &access, &write)) {
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		return false;
	}
	error.clear();
	return true;
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = file_name.wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_POSIX, nullptr, OPEN_EXISTING,
	                     GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

sys_file_t* SysFileOpen(uint8_t* buf, uint32_t buf_size) {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_STAT;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = buf;
	ret->buf->ptr  = buf;
	ret->buf->size = buf_size;

	return ret;
}

sys_file_t* SysFileCreate() {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_DYN;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = nullptr;
	ret->buf->ptr  = nullptr;
	ret->buf->size = 0;

	return ret;
}

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = file_name.wstring();
	HANDLE h_file = nullptr;
	h_file        = CreateFileW(
	    wide.c_str(), static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE),
	    FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name,
                          sys_file_cache_type_t        cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = file_name.wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type),
	                     nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

void SysFileClose(sys_file_t* f) {
	if (f->type == SYS_FILE_FILE) {
		CloseHandle(f->handle);
	} else if (f->type == SYS_FILE_MEMORY_STAT) {
		delete f->buf;
	} else if (f->type == SYS_FILE_MEMORY_DYN) {
		std::free(f->buf->base);
		delete f->buf;
	}

	// f.type = SYS_FILE_ERROR;
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		GetFileSizeEx(f.handle, &s);
		return s.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->size;
	}

	return 0;
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size) {
	const bool directory_path = !name.empty() && !name.has_filename() && name != name.root_path();
	WIN32_FILE_ATTRIBUTE_DATA info {};
	if (GetFileAttributesExW(directory_path ? name.parent_path().c_str() : name.c_str(),
	                         GetFileExInfoStandard, &info) == 0) {
		return false;
	}
	const bool file = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (directory_path && file) {
		return false;
	}
	*is_file = file;
	*size = *is_file ? ((static_cast<uint64_t>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow) : 0;
	return true;
}

uint64_t SysFileSize(const std::filesystem::path& file_name) {
	bool is_file;
	uint64_t size = 0;
	SysFileGetInfo(file_name, &is_file, &size);
	return size;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	bool ok = false;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		s.QuadPart = static_cast<LONGLONG>(size);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 &&
		              SetEndOfFile(f.handle) != 0);
		SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	}

	return ok;
}

bool SysFileUnlink(sys_file_t& f, const std::filesystem::path& name) {
	if (f.type == SYS_FILE_FILE) {
		// The SDK field spelling can be macro-expanded before common/file.h
		// undefines DeleteFile. Initialize its single BOOLEAN positionally.
		FILE_DISPOSITION_INFO info {TRUE};
		if (SetFileInformationByHandle(f.handle, FileDispositionInfo, &info, sizeof(info)) != 0) {
			return true;
		}
	}

	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	bool ok = true;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		s.QuadPart = static_cast<LONGLONG>(offset);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0);
		// printf("seek: %u\n", offset);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		f.buf->ptr = f.buf->base + offset;
	}

	return ok;
}

uint64_t SysFileTell(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		// printf("tell: %u\n", r.QuadPart);
		return r.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool SysFileIsError(sys_file_t& f) {
	return f.type == SYS_FILE_ERROR ||
	       (f.type == SYS_FILE_FILE && f.handle == INVALID_HANDLE_VALUE);
}

bool SysFileIsDirectoryExisting(const std::filesystem::path& path) {
	bool is_file;
	uint64_t size;
	return SysFileGetInfo(path, &is_file, &size) && !is_file;
}

bool SysFileIsFileExisting(const std::filesystem::path& name) {
	bool is_file;
	uint64_t size;
	return SysFileGetInfo(name, &is_file, &size) && is_file;
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = path.wstring();
	return CreateDirectoryW(wide.c_str(), nullptr) != 0;
}

bool SysFileDeleteDirectory(const std::filesystem::path& path) {
	auto wide = path.wstring();
	return RemoveDirectoryW(wide.c_str()) != 0;
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto wide = name.wstring();
	return DeleteFileW(wide.c_str()) != 0;
}

bool SysFileFlush(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE && f.handle != INVALID_HANDLE_VALUE) {
		return (FlushFileBuffers(f.handle) != 0);
	}

	return false;
}

SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &r.time, nullptr) == 0));
	SysFileClose(f);
	return r;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, nullptr, &r.time) == 0));
	SysFileClose(f);
	return r;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	// TODO() open file with dwDesiredAccess = 0
	// TODO() open directory
	sys_file_t* f = SysFileOpenR(name);
	a.is_invalid  = w.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &a.time, &w.time) == 0));
	SysFileClose(f);
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	if (f.type == SYS_FILE_FILE) {
		a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		SysTimeStruct t {};
		SysGetSystemTimeUtc(t);
		SysSystemToFileTimeUtc(t, a);
		SysSystemToFileTimeUtc(t, w);
	} else {
		a.is_invalid = w.is_invalid = true;
	}
}

bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access) {
	if (access.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, nullptr) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write) {
	if (write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, nullptr, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write) {
	if (access.is_invalid || write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	const auto pattern = path / L"*";

	WIN32_FIND_DATAW data {};
	HANDLE h = FindFirstFileW(pattern.c_str(), &data);

	if (h == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		std::filesystem::path file_name(data.cFileName);

		sys_dir_entry_t r {};

		r.is_file = ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
		r.name    = Common::PathToString(file_name);

		out.push_back(std::move(r));

	} while (FindNextFileW(h, &data) != 0);

	FindClose(h);
}

bool SysFileCopyFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = src.wstring();
	auto dst_wide = dst.wstring();
	return CopyFileW(src_wide.c_str(), dst_wide.c_str(), FALSE) != 0;
}

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = src.wstring();
	auto dst_wide = dst.wstring();
	return MoveFileW(src_wide.c_str(), dst_wide.c_str()) != 0;
}

void SysFileRemoveReadonly(const std::filesystem::path& name) {
	auto wide = name.wstring();
	SetFileAttributesW(wide.c_str(), GetFileAttributesW(wide.c_str()) &
	                                     (~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)));
}

#endif
