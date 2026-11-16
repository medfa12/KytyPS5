#include "libs/rngSysctl.h"
#include "libs/errno.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/uio.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace Libs::RngSysctl {
namespace {
static_assert(sizeof(size_t) == 8);

int Fail(int error) noexcept {
	*Posix::GetErrorAddr() = error;
	return -1;
}

bool BoundedRange(const void* pointer, size_t size) noexcept {
	const auto address = reinterpret_cast<uintptr_t>(pointer);
	return address != 0 && size <= MaxValidatedRange && size <= UINTPTR_MAX - address;
}

bool Validate(void*, const void* pointer, size_t size, bool write) noexcept {
	if (!BoundedRange(pointer, size)) { return false; }
	if (size == 0) { return true; } // Empty host range: no memory access required.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	const auto end = address + size;
#if defined(_WIN32) || defined(__APPLE__)
	for (size_t regions = 0; address < end && regions < MaxValidationRegions; ++regions) {
#ifdef _WIN32
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
		const auto result = mach_vm_region(mach_task_self(), &base, &length, VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
		if (object != MACH_PORT_NULL) { mach_port_deallocate(mach_task_self(), object); }
		if (result != KERN_SUCCESS || (info.protection & VM_PROT_READ) == 0 ||
		    (write && (info.protection & VM_PROT_WRITE) == 0)) { return false; }
#endif
		if (base > address || length > UINTPTR_MAX - base || base + length <= address) { return false; }
		address = base + length;
	}
	return address >= end;
#elif defined(__linux__)
	// Fixed parsing work/storage, no probing by reading or writing guest bytes.
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	char line[512];
	size_t regions = 0;
	bool continuation = false;
	for (size_t lines = 0; address < end && lines < MaxLinuxMapLines && std::fgets(line, sizeof(line), maps); ++lines) {
		const bool skip = continuation;
		continuation = std::strchr(line, '\n') == nullptr;
		if (skip) { continue; } // A long mapping pathname is not another range header.
		unsigned long long base = 0, limit = 0;
		char permissions[5] {};
		if (std::sscanf(line, "%llx-%llx %4s", &base, &limit, permissions) != 3 || limit <= address) { continue; }
		if (base > address || permissions[0] != 'r' || (write && permissions[1] != 'w') ||
		    ++regions > MaxValidationRegions) { break; }
		address = static_cast<uintptr_t>(limit);
	}
	std::fclose(maps);
	return address >= end;
#else
	return false;
#endif
}

bool Copy(void* guest, void* local, size_t size, bool write) noexcept {
	if (!BoundedRange(guest, size)) { return false; }
	if (size == 0) { return true; }
	// Copies are at most 256 bytes. Preflight rejects guard
	// mappings; the system copy still handles a protection race/partial transfer.
	if (size > ArandomMaxSize || !Validate(nullptr, guest, size, write)) { return false; }
#ifdef _WIN32
	using Transfer = NTSTATUS (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
	const auto module = GetModuleHandleW(L"ntdll.dll");
	const auto transfer = module == nullptr ? nullptr : reinterpret_cast<Transfer>(
	    GetProcAddress(module, write ? "NtWriteVirtualMemory" : "NtReadVirtualMemory"));
	if (transfer == nullptr) { return false; }
	SIZE_T copied = 0;
	const NTSTATUS status = transfer(GetCurrentProcess(), guest, local, size, &copied);
	return status >= 0 && copied == size;
#elif defined(__linux__)
	iovec local_iov {local, size}, guest_iov {guest, size};
	const auto copied = write ? ::process_vm_writev(::getpid(), &local_iov, 1, &guest_iov, 1, 0) :
	                            ::process_vm_readv(::getpid(), &local_iov, 1, &guest_iov, 1, 0);
	return copied == static_cast<ssize_t>(size);
#elif defined(__APPLE__)
	if (write) {
		return mach_vm_write(mach_task_self(), reinterpret_cast<mach_vm_address_t>(guest),
		    reinterpret_cast<vm_offset_t>(local), static_cast<mach_msg_type_number_t>(size)) == KERN_SUCCESS;
	}
	mach_vm_size_t copied = 0;
	return mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(guest), size,
	    reinterpret_cast<mach_vm_address_t>(local), &copied) == KERN_SUCCESS && copied == size;
#else
	return false;
#endif
}

bool ReadLength(void*, const size_t* source, size_t& value) noexcept {
	return Copy(const_cast<size_t*>(source), &value, sizeof(value), false);
}
bool CopyOutput(void*, void* target, const void* source, size_t size) noexcept {
	return Copy(target, const_cast<void*>(source), size, true);
}
bool StoreLength(void*, size_t* target, size_t value) noexcept {
	return Copy(target, &value, sizeof(value), true);
}
bool SystemEntropy(uint8_t* bytes, size_t size) noexcept {
	if (size == 0) { return true; }
#ifdef _WIN32
	return BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
	size_t offset = 0;
	while (offset < size) {
		const auto count = ::getrandom(bytes + offset, size - offset, 0);
		if (count < 0 && errno == EINTR) { continue; }
		if (count <= 0) { return false; }
		offset += static_cast<size_t>(count);
	}
	return true;
#elif defined(__APPLE__)
	::arc4random_buf(bytes, size);
	return true;
#else
	return false;
#endif
}

bool Entropy(void*, Bytes& bytes) noexcept {
	return SystemEntropy(bytes.data(), bytes.size());
}

bool CopyMib(void*, const void* guest, void* local, size_t size) noexcept {
	return Copy(const_cast<void*>(guest), local, size, false);
}

bool NumericEntropy(void*, uint8_t* bytes, size_t size) noexcept {
	return SystemEntropy(bytes, size);
}
} // namespace

int Run(void* oldp, size_t* oldlenp, const void* newp, size_t newlen, const Callbacks& callbacks) noexcept {
	size_t capacity = 0;
	if (oldlenp != nullptr && !callbacks.read_length(callbacks.context, oldlenp, capacity)) { return Fail(Posix::POSIX_EFAULT); }
	if (oldp != nullptr && (!BoundedRange(oldp, capacity) ||
	    !callbacks.validate(callbacks.context, oldp, capacity, true))) { return Fail(Posix::POSIX_EFAULT); }
	if (newp != nullptr) {
		if (!BoundedRange(newp, newlen) || !callbacks.validate(callbacks.context, newp, newlen, false)) {
			return Fail(Posix::POSIX_EFAULT);
		}
		return Fail(Posix::POSIX_EPERM);
	}
	Bytes bytes {};
	if (!callbacks.entropy(callbacks.context, bytes)) { return Fail(Posix::POSIX_EIO); }
	const size_t count = oldp == nullptr ? ResultSize : std::min(capacity, ResultSize);
	if (oldp != nullptr && count != 0 && !callbacks.copy_output(callbacks.context, oldp, bytes.data(), count)) {
		return Fail(Posix::POSIX_EFAULT);
	}
	if (oldlenp != nullptr && !callbacks.store_length(callbacks.context, oldlenp, count)) { return Fail(Posix::POSIX_EFAULT); }
	return oldp != nullptr && capacity < ResultSize ? Fail(Posix::POSIX_ENOMEM) : 0;
}

int Sysctl(void* oldp, size_t* oldlenp, const void* newp, size_t newlen) noexcept {
	const Callbacks callbacks {nullptr, ReadLength, Validate, CopyOutput, StoreLength, Entropy};
	return Run(oldp, oldlenp, newp, newlen, callbacks);
}

int NumericRun(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
               const void* newp, size_t newlen, const NumericCallbacks& callbacks) noexcept {
	if (miblen < 2 || miblen > NumericMaxMib) { return Posix::POSIX_EINVAL; }
	std::array<uint8_t, NumericMaxMib * sizeof(uint32_t)> name {};
	const size_t name_size = miblen * sizeof(uint32_t);
	if (!BoundedRange(mib, name_size) || !callbacks.copy_mib(callbacks.context, mib, name.data(), name_size)) {
		return Posix::POSIX_EFAULT;
	}
	const auto element = [&](size_t i) {
		const size_t pos = i * sizeof(uint32_t);
		return uint32_t(name[pos]) | (uint32_t(name[pos + 1]) << 8) |
		       (uint32_t(name[pos + 2]) << 16) | (uint32_t(name[pos + 3]) << 24);
	};

	size_t capacity = 0;
	if (oldlenp != nullptr && !callbacks.read_length(callbacks.context, oldlenp, capacity)) {
		return Posix::POSIX_EFAULT;
	}
	if (oldp != nullptr && (!BoundedRange(oldp, capacity) ||
	    !callbacks.validate(callbacks.context, oldp, capacity, true))) { return Posix::POSIX_EFAULT; }
	if (newp != nullptr && (!BoundedRange(newp, newlen) ||
	    !callbacks.validate(callbacks.context, newp, newlen, false))) { return Posix::POSIX_EFAULT; }

	if (element(0) != 1 || element(1) != 37) { return Posix::POSIX_ENOSYS; }
	if (miblen != 2) { return Posix::POSIX_ENOTDIR; }
	if (newp != nullptr) { return Posix::POSIX_EPERM; }
	const size_t count = std::min(capacity, ArandomMaxSize);
	// Native arandom returns ENXIO if its provider returns zero, including a
	// zero-byte request. Unlike rng_pseudo it does not report a fixed size.
	if (count == 0) { return Posix::POSIX_ENXIO; }
	std::array<uint8_t, ArandomMaxSize> bytes {};
	if (!callbacks.entropy(callbacks.context, bytes.data(), count)) { return Posix::POSIX_ENXIO; }
	if (oldp != nullptr && !callbacks.copy_output(callbacks.context, oldp, bytes.data(), count)) {
		return Posix::POSIX_EFAULT;
	}
	if (oldlenp != nullptr && !callbacks.store_length(callbacks.context, oldlenp, count)) {
		return Posix::POSIX_EFAULT;
	}
	return 0;
}

int NumericSysctlRaw(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
                     const void* newp, size_t newlen) noexcept {
	const NumericCallbacks callbacks {nullptr, CopyMib, ReadLength, Validate, CopyOutput, StoreLength, NumericEntropy};
	return NumericRun(mib, miblen, oldp, oldlenp, newp, newlen, callbacks);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NumericSysctl(const void* mib, uint32_t miblen, void* oldp, size_t* oldlenp,
                  const void* newp, size_t newlen) noexcept {
	const int error = NumericSysctlRaw(mib, miblen, oldp, oldlenp, newp, newlen);
	return error == 0 ? 0 : Fail(error);
}
} // namespace Libs::RngSysctl
