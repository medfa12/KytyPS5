#include "kernel/eventFlag.h"
#include "kernel/semaphore.h"
#include "libs/errno.h"
#include "libs/semaphoreProvenance.h"
#include "loader/symbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string_view>
#include <type_traits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
namespace S = Libs::LibKernel::Semaphore;
namespace E = Libs::LibKernel::EventFlag;
namespace P = Libs::LibKernel::SemaphoreProvenance;
namespace K = Libs::LibKernel;

namespace {
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "KernelSemaphoreProvenanceTests: %s\n", message);
    std::abort();
  }
}
template<class T> T Sentinel() {
  if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(uintptr_t{0x1234});
  else return static_cast<T>(0x1234);
}
struct Outcome { int result; int guest; DWORD host; };
template<class F> Outcome Invoke(F f) {
  *Libs::Posix::GetErrorAddr() = 12345;
  SetLastError(0x12345678);
  const int result = f();
  const DWORD host = GetLastError();
  const int guest = *Libs::Posix::GetErrorAddr();
  return {result, guest, host};
}
void Same(Outcome direct, Outcome registered, int expected, const char* message) {
  if (direct.result != expected || registered.result != direct.result ||
      registered.guest != direct.guest || registered.host != direct.host) {
    std::fprintf(stderr, "direct=(%x,%d,%lx) registered=(%x,%d,%lx) expected=%x\n",
                 static_cast<unsigned>(direct.result), direct.guest, direct.host,
                 static_cast<unsigned>(registered.result), registered.guest, registered.host,
                 static_cast<unsigned>(expected));
    Check(false, message);
  }
}
template<class F> F Lookup(Loader::SymbolDatabase& symbols, const char* nid, F expected) {
  const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
  Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(expected),
        "registered target mismatch");
  return reinterpret_cast<F>(symbol->vaddr);
}
void Name(char (&out)[32], const char* prefix, unsigned n) {
  const int length = std::snprintf(out, sizeof(out), "%s_%lu_%u", prefix,
                                   static_cast<unsigned long>(GetCurrentProcessId()), n);
  Check(length > 0 && length < static_cast<int>(sizeof(out)), "name too long");
}
}

// Fresh process, Common threads/config/log initialized; no Vulkan/Memory/FS.
void TestKernelSemaphoreProvenance(bool enabled) {
  std::printf("KernelSemaphoreProvenanceTests pid=%lu enabled=%d\n",
              static_cast<unsigned long>(GetCurrentProcessId()), enabled);
  std::fflush(stdout);
  const char* env = std::getenv("KYTY_SEMAPHORE_PROVENANCE");
  Check((env != nullptr && std::string_view(env) == "1") == enabled, "environment mismatch");
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  Check(P::Enabled() == enabled, "observer initialization failed or unexpectedly enabled");
  auto create = Lookup(symbols, "188x57JYp0g", enabled ? &P::CreateSema : &S::KernelCreateSema);
  auto poll = Lookup(symbols, "12wOHk8ywb0", enabled ? &P::PollSema : &S::KernelPollSema);
  auto event = Lookup(symbols, "BpFoboUJoZU", enabled ? &P::CreateEventObserved : &E::KernelCreateEventFlag);

  char a[32] {}, b[32] {};
  Name(a, "prov_s", 1); Name(b, "prov_s", 2);
  S::KernelSema direct {}, observed {};
  auto d = Invoke([&] { return S::KernelCreateSema(&direct, a, 0x101, 1, 1, nullptr); });
  auto o = Invoke([&] { return create(&observed, b, 0x101, 1, 1, nullptr); });
  Same(d, o, OK, "semaphore create result/errors");
  Check(direct != S::KernelSema{} && observed != S::KernelSema{}, "semaphore output missing");
  d = Invoke([&] { return S::KernelPollSema(direct, 1); });
  o = Invoke([&] { return poll(observed, 1); });
  Same(d, o, OK, "semaphore initial token");
  d = Invoke([&] { return S::KernelPollSema(direct, 1); });
  o = Invoke([&] { return poll(observed, 1); });
  Same(d, o, K::KERNEL_ERROR_EBUSY, "semaphore empty token");
  Check(S::KernelDeleteSema(direct) == OK && S::KernelDeleteSema(observed) == OK, "delete semaphores");

  d = Invoke([&] { return S::KernelPollSema(UINT64_MAX, 1); });
  o = Invoke([&] { return poll(UINT64_MAX, 1); });
  Same(d, o, K::KERNEL_ERROR_ESRCH, "invalid minus-one carrier forwards native lookup error");

  direct = Sentinel<S::KernelSema>(); observed = Sentinel<S::KernelSema>();
  d = Invoke([&] { return S::KernelCreateSema(&direct, a, 0x101, 2, 1, nullptr); });
  o = Invoke([&] { return create(&observed, b, 0x101, 2, 1, nullptr); });
  Same(d, o, K::KERNEL_ERROR_EINVAL, "invalid semaphore create result/errors");
  Check(direct == Sentinel<S::KernelSema>() && observed == direct, "failed create changed output");

  Name(a, "prov_s", 3);
  S::KernelSema count {};
  Check(create(&count, a, 0x101, 2, 2, nullptr) == OK, "create count test semaphore");
  Check(poll(count, 1) == OK && poll(count, 2) == K::KERNEL_ERROR_EBUSY &&
        poll(count, 1) == OK && poll(count, 1) == K::KERNEL_ERROR_EBUSY,
        "poll must consume exactly one token");
  for (int i = 0; i < 30; ++i) {
    Check(S::KernelSignalSema(count, 1) == OK && poll(count, 1) == OK,
          "forwarding after generic bank capacity");
  }
  Check(S::KernelDeleteSema(count) == OK, "delete count semaphore");

  constexpr uint64_t pattern = UINT64_C(0x8000000100000005);
  Name(a, "prov_e", 1); Name(b, "prov_e", 2);
  E::KernelEventFlag de {}, oe {};
  d = Invoke([&] { return E::KernelCreateEventFlag(&de, a, 0x21, pattern, nullptr); });
  o = Invoke([&] { return event(&oe, b, 0x21, pattern, nullptr); });
  Same(d, o, OK, "event create result/errors");
  Check(de != E::KernelEventFlag{} && oe != E::KernelEventFlag{}, "event output missing");
  uint64_t result1 = 0, result2 = 0;
  Check(E::KernelPollEventFlag(de, UINT64_C(0x8000000000000000), 1, &result1) == OK &&
        E::KernelPollEventFlag(oe, UINT64_C(0x8000000000000000), 1, &result2) == OK &&
        result1 == pattern && result2 == pattern, "event 64-bit pattern");
  const int delete1 = E::KernelDeleteEventFlag(de);
  const int delete2 = E::KernelDeleteEventFlag(oe);

  de = Sentinel<E::KernelEventFlag>(); oe = Sentinel<E::KernelEventFlag>();
  d = Invoke([&] { return E::KernelCreateEventFlag(&de, a, 3, pattern, nullptr); });
  o = Invoke([&] { return event(&oe, b, 3, pattern, nullptr); });
  Same(d, o, K::KERNEL_ERROR_EINVAL, "invalid event create result/errors");
  Check(de == Sentinel<E::KernelEventFlag>() && oe == de, "failed event create changed output");

  SYSTEM_INFO info {}; GetSystemInfo(&info);
  auto* pages = static_cast<char*>(VirtualAlloc(nullptr, 2 * info.dwPageSize,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(pages != nullptr, "allocate name pages");
  DWORD previous = 0;
  Check(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous) != 0,
        "protect name page");
  char* short_name = pages + info.dwPageSize - 2;
  short_name[0] = 'p'; short_name[1] = '\0';
  S::KernelSema boundary {};
  Check(create(&boundary, short_name, 0x101, 1, 1, nullptr) == OK &&
        poll(boundary, 1) == OK && poll(boundary, 1) == K::KERNEL_ERROR_EBUSY,
        "unreadable 32-byte observation name must forward");
  Check(S::KernelDeleteSema(boundary) == OK, "delete boundary semaphore");
  Check(VirtualFree(pages, 0, MEM_RELEASE) != 0, "free name pages");
  Check(delete1 == OK && delete2 == OK, "delete event flags");
}
