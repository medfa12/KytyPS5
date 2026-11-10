// Windows x64 test-only observation helper; not a production API.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace EventFlagParkWitness {
static_assert(sizeof(void*) == 8, "review SPI layout before using non-x64 builds");
static_assert(sizeof(SYSTEM_PROCESS_INFORMATION) == 0x100 &&
              offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId) == 0x50,
              "unexpected SDK process record layout");
static_assert(sizeof(SYSTEM_THREAD_INFORMATION) == 0x50 &&
              offsetof(SYSTEM_THREAD_INFORMATION, ClientId) == 0x28 &&
              offsetof(SYSTEM_THREAD_INFORMATION, ThreadState) == 0x44 &&
              offsetof(SYSTEM_THREAD_INFORMATION, WaitReason) == 0x48,
              "unexpected SDK thread record layout");
inline void Require(bool value, const char* text) {
 if (!value) { std::fprintf(stderr, "EventFlagParkWitness: %s\n", text); std::abort(); }
}
inline uint64_t FileTimeValue(const FILETIME& t) {
 return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
struct Sample { uint32_t state = 0, reason = 0; };

// thread is a real duplicated self-handle, held until after join. Check both
// process/thread IDs and the kernel creation timestamp, not an ID alone.
inline bool Snapshot(HANDLE thread, DWORD tid, Sample& sample) {
 Require(GetThreadId(thread) == tid && GetProcessIdOfThread(thread) == GetCurrentProcessId(), "thread handle identity");
 FILETIME created {}, exited {}, kernel {}, user {};
 Require(GetThreadTimes(thread, &created, &exited, &kernel, &user) != 0, "GetThreadTimes");
 using Query = NTSTATUS (NTAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
 const auto ntdll = GetModuleHandleW(L"ntdll.dll");
 Require(ntdll != nullptr, "ntdll unavailable");
 const auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
 Require(query != nullptr, "NtQuerySystemInformation unavailable");
 constexpr ULONG cap = 64u * 1024u * 1024u;
 std::vector<unsigned char> bytes(1024u * 1024u);
 ULONG length = 0; NTSTATUS status = 0;
 for (int attempt = 0; ; ++attempt) {
  length = 0;
  status = query(SystemProcessInformation, bytes.data(), static_cast<ULONG>(bytes.size()), &length);
  if (status == 0) break;
  const auto code = static_cast<uint32_t>(status);
  Require((code == 0xc0000004u || code == 0x80000005u || code == 0xc0000023u) && attempt < 8,
          "system query failed or retry limit exceeded");
  const uint64_t next = length > bytes.size() ? static_cast<uint64_t>(length) + 65536u : bytes.size() * 2ull;
  Require(next <= cap, "system query exceeds size cap"); bytes.resize(static_cast<size_t>(next));
 }
 Require(length >= sizeof(SYSTEM_PROCESS_INFORMATION) && length <= bytes.size(), "invalid query length");
 size_t offset = 0; unsigned process_matches = 0, thread_matches = 0;
 for (;;) {
  Require(offset <= length && length - offset >= sizeof(SYSTEM_PROCESS_INFORMATION), "truncated process record");
  SYSTEM_PROCESS_INFORMATION process {};
  std::memcpy(&process, bytes.data() + offset, sizeof(process));
  const size_t record_size = process.NextEntryOffset ? process.NextEntryOffset : length - offset;
  Require(record_size >= sizeof(process) && record_size <= length - offset &&
          (process.NextEntryOffset == 0 || process.NextEntryOffset % alignof(void*) == 0), "bad process next offset");
  Require(process.NumberOfThreads <= (record_size - sizeof(process)) / sizeof(SYSTEM_THREAD_INFORMATION),
          "truncated thread array");
  if (reinterpret_cast<uintptr_t>(process.UniqueProcessId) == GetCurrentProcessId()) {
   ++process_matches;
   for (ULONG i = 0; i < process.NumberOfThreads; ++i) {
    SYSTEM_THREAD_INFORMATION entry {};
    std::memcpy(&entry, bytes.data() + offset + sizeof(process) + static_cast<size_t>(i) * sizeof(entry), sizeof(entry));
    if (reinterpret_cast<uintptr_t>(entry.ClientId.UniqueProcess) == GetCurrentProcessId() &&
        reinterpret_cast<uintptr_t>(entry.ClientId.UniqueThread) == tid) {
     ++thread_matches;
     Require(static_cast<uint64_t>(entry.Reserved1[2].QuadPart) == FileTimeValue(created), "thread creation timestamp mismatch");
     sample.state = entry.ThreadState; sample.reason = entry.WaitReason;
    }
   }
  }
  if (process.NextEntryOffset == 0) break;
  offset += process.NextEntryOffset;
 }
 Require(process_matches == 1 && thread_matches <= 1, "unexpected process/thread match count");
 return thread_matches == 1;
}
} // namespace EventFlagParkWitness
