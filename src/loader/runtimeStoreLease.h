#pragma once
#include <cstdint>
#include <filesystem>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::RuntimeChild {
// Share-zero file object, never a delete/recreate marker. Duplicates retain the
// same exclusion until the last process closes its handle.
class StoreLease {
public:
	StoreLease() = default;
	~StoreLease() { Reset(); }
	StoreLease(const StoreLease&) = delete;
	StoreLease& operator=(const StoreLease&) = delete;
	StoreLease(StoreLease&& other) noexcept: handle_(std::exchange(other.handle_, 0)) {}
	StoreLease& operator=(StoreLease&& other) noexcept {
		if (this != &other) { Reset(); handle_ = std::exchange(other.handle_, 0); } return *this;
	}
	uint64_t Handle() const { return handle_; }
	void Reset() noexcept {
#if defined(_WIN32)
		if (handle_) { CloseHandle(reinterpret_cast<HANDLE>(handle_)); }
#endif
		handle_ = 0;
	}
	bool Acquire(const std::filesystem::path& root) {
#if defined(_WIN32)
		if (handle_) { return false; }
		std::error_code ec;
		const auto canonical = std::filesystem::canonical(root, ec);
		if (ec) { return false; }
		auto h = CreateFileW((canonical / ".runtime-lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
		                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (h == INVALID_HANDLE_VALUE) { return false; }
		handle_ = reinterpret_cast<uint64_t>(h);
		BY_HANDLE_FILE_INFORMATION info {};
		if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
		    info.nNumberOfLinks != 1 || !MatchesRoot(canonical)) { Reset(); return false; }
		return true;
#else
		return false;
#endif
	}
	bool Identity(uint64_t* volume, uint64_t* file) const {
#if defined(_WIN32)
		BY_HANDLE_FILE_INFORMATION info {};
		if (!handle_ || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle_), &info)) { return false; }
		*volume = info.dwVolumeSerialNumber; *file = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow; return true;
#else
		return false;
#endif
	}
	bool Duplicate(StoreLease* out) const {
#if defined(_WIN32)
		HANDLE h = nullptr;
		if (!handle_ || !out || out->handle_ || !DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(handle_),
		    GetCurrentProcess(), &h, 0, TRUE, DUPLICATE_SAME_ACCESS)) { return false; }
		out->handle_ = reinterpret_cast<uint64_t>(h); return true;
#else
		return false;
#endif
	}
	bool Adopt(uint64_t handle, uint64_t volume, uint64_t file) {
#if defined(_WIN32)
		if (handle_ || !handle || GetFileType(reinterpret_cast<HANDLE>(handle)) != FILE_TYPE_DISK) { return false; }
		handle_ = handle;
		uint64_t actual_volume = 0, actual_file = 0;
		if (!Identity(&actual_volume, &actual_file) || volume != actual_volume || file != actual_file ||
		    !SetHandleInformation(reinterpret_cast<HANDLE>(handle_), HANDLE_FLAG_INHERIT, 0)) { Reset(); return false; }
		return true;
#else
		return false;
#endif
	}
	bool MatchesRoot(const std::filesystem::path& root) const {
#if defined(_WIN32)
		wchar_t name[32768] {};
		const auto size = GetFinalPathNameByHandleW(reinterpret_cast<HANDLE>(handle_), name, 32768, FILE_NAME_NORMALIZED);
		if (!size || size >= 32768) { return false; }
		std::error_code ec;
		return std::filesystem::path(name).filename() == L".runtime-lock" &&
		       std::filesystem::equivalent(std::filesystem::path(name).parent_path(), root, ec) && !ec;
#else
		return false;
#endif
	}
private:
	uint64_t handle_ = 0;
};
}
