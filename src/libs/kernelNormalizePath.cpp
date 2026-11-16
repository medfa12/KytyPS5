#include "libs/kernelNormalizePath.h"

#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::KernelNormalizePath {
LIB_VERSION("libkernel", 1, "libkernel", 1, 1);
namespace {
constexpr int Invalid = static_cast<int>(0x80020016u);
constexpr int NameTooLong = static_cast<int>(0x8002003fu);
constexpr size_t PathBytes = 1024; // Native libkernel's bounded scratch buffer.
// Host safety budget, not a claimed firmware input-length restriction. Native
// accepts long inputs that collapse (e.g. repeated '/'); retain that behavior
// up to this bounded scan rather than treating the 1024-byte output as input ABI.
constexpr uint64_t ScanLimit = 1024 * 1024;

bool ReadByte(uintptr_t address, char& out) {
#if defined(_WIN32)
 SIZE_T copied = 0;
 return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &out, 1, &copied) && copied == 1;
#elif defined(__linux__)
 iovec local {&out, 1}, remote {reinterpret_cast<void*>(address), 1};
 return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1;
#else
 return false;
#endif
}
bool Write(char* destination, const char* source, size_t bytes) {
#if defined(_WIN32)
 SIZE_T copied = 0;
 return WriteProcessMemory(GetCurrentProcess(), destination, source, bytes, &copied) && copied == bytes;
#elif defined(__linux__)
 iovec local {const_cast<char*>(source), bytes}, remote {destination, bytes};
 return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
#else
 return false;
#endif
}
int Trace(int result) {
 static const bool enabled = [] {
  const auto* value = std::getenv("KYTY_MEDIA_TRACE");
  return value != nullptr && std::strcmp(value, "0") != 0;
 }();
 static std::atomic_uint calls {0};
 if (enabled && calls.fetch_add(1, std::memory_order_relaxed) < 32) {
  // Never log path bytes; this is a general kernel export, not DRM-specific.
  LOGF("Kernel NormalizePath: result=%08x\n", static_cast<unsigned>(result));
 }
 return result;
}
} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Normalize(
 const char* source, uint64_t source_bytes, char* destination, uint64_t destination_bytes) {
 // Firmware 13.60 libkernel 28fd0..29235. It validates declared ranges for
 // overflow/overlap, requires an absolute NUL-terminated input inside its bound,
 // and publishes the scratch result only after all normalization/capacity checks.
 const auto begin = reinterpret_cast<uintptr_t>(source);
 const auto output = reinterpret_cast<uintptr_t>(destination);
 if (begin == 0 || source_bytes == 0 || output == 0 || destination_bytes < 2 ||
     source_bytes > UINTPTR_MAX - begin || destination_bytes > UINTPTR_MAX - output ||
     (begin < output + destination_bytes && output < begin + source_bytes)) { return Trace(Invalid); }

 uint64_t length = 0;
 char byte = 0;
 for (; length < source_bytes && length < ScanLimit; ++length) {
  if (!ReadByte(begin + length, byte)) { return Trace(Invalid); }
  if (byte == 0) { break; }
 }
 if (length == source_bytes || length == ScanLimit || length == 0 ||
     !ReadByte(begin, byte) || byte != '/') { return Trace(Invalid); }

 std::array<char, PathBytes> normalized {};
 size_t used = 0;
 uint64_t start = 1;
 for (uint64_t index = 1; index <= length; ++index) {
  byte = 0;
  if (index != length && !ReadByte(begin + index, byte)) { return Trace(Invalid); }
  if (index != length && byte != '/') { continue; }
  const uint64_t count = index - start;
  // Empty interior components disappear, but the native final empty component
  // preserves a trailing slash. A final '.' does NOT add a slash of its own.
  if (count == 0 && index != length) { start = index + 1; continue; }
  char first = 0, second = 0;
  if (count != 0 && !ReadByte(begin + start, first)) { return Trace(Invalid); }
  if (count == 2 && !ReadByte(begin + start + 1, second)) { return Trace(Invalid); }
  if (count == 1 && first == '.') { start = index + 1; continue; }
  if (count == 2 && first == '.' && second == '.') {
   // Native clamps traversal above root; it does not consult host filesystems.
   while (used != 0 && normalized[used - 1] != '/') { --used; }
   if (used != 0) { --used; }
  } else {
   if (used > PathBytes - 2 || count > PathBytes - 2 - used) { return Trace(NameTooLong); }
   normalized[used++] = '/';
   for (uint64_t part = 0; part < count; ++part) {
    if (!ReadByte(begin + start + part, normalized[used++])) { return Trace(Invalid); }
   }
  }
  start = index + 1;
 }
 normalized[used] = 0;
 // Firmware checks used+1 before its special empty-result => '/' publication.
 if (used + 1 > destination_bytes) { return Trace(Invalid); }
 if (used == 0) { normalized[0] = '/'; normalized[1] = 0; used = 1; }
 return Trace(Write(destination, normalized.data(), used + 1) ? 0 : Invalid);
}

LIB_DEFINE(InitKernelNormalizePath_1) {
 LIB_FUNC("KB7Vc2+5S54", Normalize);
}
} // namespace Libs::KernelNormalizePath
