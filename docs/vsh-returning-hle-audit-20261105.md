# Production returning-HLE classification — 2026-11-05

Parent follow-up: `KernelDlsym` has since gained a shared-owner path that returns
checked immutable function entries and only private native data pointers. Its
exact registration was added as pair 119. The original dynamic-pointer exclusion
below describes the pre-follow-up source. The actual imported `KernelDlsym`
lookup/invocation fixture passed in `_TempData/vsh-dlsym-end-to-end-tests.log`;
the original 118-pair audit and its other limitations remain unchanged.

## Outcome and scope

Added `src/loader/applicationReturningExports.h`: **118 exact
`{nid, registration-expression}` pairs**, including the original eight entries
and the two scoped pthread-exit adapters. These are reviewed production source
identities, not test overrides, an enumeration of every `LIB_FUNC`, guessed
NID aliases, or replacements for missing services. The parent owns integration
into `ApplicationImportBindings`, build and tests. This task edits only the new
header and this report; no build, guest/live run, commit, push or agents.

**The VSH launch gate remains closed.** This table classifies normal returning
baseline-SysV HLE transitions and the existing nested failure boundaries; it
does not prove arbitrary-PC preemption, cancellability of waits, comprehensive
host-fault containment, title startup completion, UI, networking or playback.
An unsupported transition must still fail closed when invoked. Returning an
actual API error is different from substituting an ABI-unknown generic stub.

## Identity and bounded app inventory

Inspected the parent `src/loader/applicationImportBindings.cpp`: classification
first distinguishes private native mappings from HLE, checks the resolved
address/name/debug registration identity, then matches both NID and the
**literal stringized registration expression**. The table must replace that
file's local list, not bypass any of those checks. For example, the POSIX
registrations spell `clock_gettime` / `clock_getres`, not
`Posix::clock_gettime` / `Posix::clock_getres`. Multiple expressions for one NID
are intentional only where actual registrations exist. No change to library,
module, version, owner visibility, HLE/native precedence or resolver fallback
is proposed.

Read-only Python inspection reused the `image()` parser in the prior scratch
`%LOCALAPPDATA%/Temp/opencode/run19_provider_audit.py` with bytecode writes
disabled. It reads ELF program headers, SCE dynamic/string/symbol tables and
undefined global/weak symbols, not broken/out-of-file app section headers.
The following local images were parsed directly under
`${USERPROFILE}/Downloads/storeormediaapps/`:

| Title / relative image | Bytes | Undefined `STT_FUNC` records |
| --- | ---: | ---: |
| `PPSA01651-app0/eboot.bin` | 74332 | 9 |
| `PPSA01651-app0/libstarboard.prx` | 6412284 | 420 |
| `PPSA01651-app0/libcobalt.prx` | 40147816 | 203 |
| `PPSA01651-app0/sce_module/libc.prx` | 1964138 | 117 |
| `PPSA01651-app0/sce_module/libSceNpCppWebApi.prx` | 8378535 | 93 |
| `PPSA01783-app0/eboot.bin` | 45753892 | 850 |
| `PPSA01783-app0/sce_module/libc.prx` | 1964138 | 117 |
| `PPSA01783-app0/lib/libCoreADI.prx` | 1593952 | 24 |
| `PPSA01783-app0/lib/libCoreFP.prx` | 1432844 | 10 |

These are on-disk symbol-record counts, **not** unresolved-function counts,
PLT relocation counts, unique NID counts, executed-call counts or full provider
closure. In particular, Apple's 850 includes records beyond the 845 global PLT
relocations in the previous audit. Firmware providers loaded later, dynamically
looked-up targets, callbacks, data imports and transitive firmware imports have
not been exhaustively enumerated here. No claim of whole-title import coverage
or startup reachability follows. Firmware reference context remains **13.60**;
the header is source-implementation classification, not a firmware-version
compatibility override.

SHA-256 identities (shared app libc has identical bytes in both titles):

```text
YouTube eboot       43c94d3df590614fbd625f927e6b46b4658f61d58eac5ee479dfcf182f540182
YouTube starboard   09aac5e3aece841823d3275305172f87d522f680a04a771c12fae2606e0f7cda
YouTube cobalt     355638d68a9b01d29bd895a19d7ae0ddae48fb7e73a78d96a005283473e6d84b
YouTube NpCppWebApi d03d64abc6f82a2a6b0b9e992782f98b7f488fd225a8553132b41e23bdad5b10
Apple eboot         6f2a81bd687e4415a41f00c5b26059eaffb4ae5cd684eacc5c7d8ecea6f8f768
Apple CoreADI       4d9719c9e56e9fa1aa22b8692b1aa8436fb3acb372d9e5776e0962827cb20bd9
Apple CoreFP        cc0544079183cdd832ad6701a613bdc0e385a96a85df64a0b8d7ee40558e10de
Both app libc      c0e1690a122b597cce854e9ac9987406fa4a3d7b0d4aee16e4dc7ace368fae83
```

YouTube's tiny eboot is not representative of its service demand: its literal
`DT_NEEDED` includes `libstarboard.prx`, while root `libcobalt.prx` has substantial
additional imports. Starboard actually imports POSIX create/identity/attributes,
mutex and TLS-key operations, clocks, direct/flexible memory, libc and service
APIs. Apple eboot plus CoreADI/CoreFP and app libc import the corresponding
kernel/POSIX families. This is why the additions focus on implemented runtime
services rather than a target unresolved count. Existing
`vsh-native-import-admission-audit-20261104.md` and
`vsh-run19-kernel-imports-20261104.md` remain relevant for lazy-binding and native
ABI evidence; their old missing-service statements are not substitutes for
checking today's implementations.

## Source-backed admission rationale

Locations below identify bodies by symbol as concurrent parent work can move
line numbers. Exact pairs in the header are the export inventory.

| Group | Inspected production bodies / returning-transition rationale |
| --- | --- |
| Original module load/stop | `src/libs/libKernel.cpp` `KernelLoadStartModule` / `KernelStopUnloadModule`; owner-private lifecycle proceeds through `RuntimeLinker::LoadStartModuleReference`, checked start/stop and nested `RuntimeGuestExecution::InvokeModule`. Guest initializer/finalizer failure returns to the host lifecycle frame before its locks/leases unwind. Existing entries retained, not a new blanket sysmodule classification. |
| Counters, clocks, conversions | `src/kernel/pthread.cpp` `KernelClockGetres`, `KernelClockGettime`, `KernelGettimeofday`, timezone/local/UTC conversion and TSC/process counter bodies; `src/libs/libKernel.cpp` clock/time POSIX adapters; `src/libs/processSdk.cpp` compiled SDK query. Real clock reads and typed scalar/pointer outputs, no guest callback or guest wait. Float/long-double internal arithmetic does not change their scalar return ABI. |
| Process parameters, errno, TLS | `src/libs/libKernel.cpp` `KernelGetProcParam`, `getpid`, `get_error_addr`, `tls_get_addr`; `src/libs/errno.cpp` `GetErrorAddr`; `src/loader/runtimeLinker.cpp` `GetProcParam` / `TlsGetAddr`. Process parameter selection respects the current app owner; errno is actual host-thread-local guest errno. TLS allocation holds its host mutex, allocates runtime VM, copies the TLS image and returns; it does not invoke guest application-heap callbacks. |
| Text interval errors | `src/loader/kernelTextLocation.cpp` calls `QueryKernelTextLocationError`, sets guest errno and returns the encoded helper error without writing outputs. The public one-pointer wrapper discards that status and returns zero exactly as its native failure path does; it does not fabricate an address interval. See `vsh-kernel-text-query-policy-20261104.md`. |
| Residency errors | `src/kernel/memory.cpp` `KernelMlock` / `KernelMunlock` return explicit kernel ENOSYS and set guest errno; no allocation, pinning or fake success. Existing entries retained. |
| VM/accounting | `src/kernel/memory.cpp` flexible map/unmap, direct allocation/available-size/map/release and virtual-query bodies. They operate on real VM backing, physical/flexible allocation and virtual-range registries and return typed statuses/outputs. Scoped locks finish before the gateway epilogue. Their indirect alloc/free hooks are host-only: the production caller of `Memory::RegisterCallbacks` is `src/libs/libDbgAsan.cpp::asan_init`, installing host `KernelAlloc` / `KernelFree` / shadow-map routines. No guest callback is hidden in those hooks. |
| Worker creation | `src/kernel/pthread.cpp` `CreatePthread`, `PthreadCreate`, POSIX wrapper, `RunThread`, `RunRuntimeWorker`. Creation acquires/publishes the owner's thread lease before startup; a worker enters guest code under its own actual-thread root boundary. Refused publication joins a worker that never entered guest. A creator return is not mistaken for completion of the worker. |
| Scoped exit | Same file's exact `PthreadExit` and `Posix::pthread_exit` adapters route through `KytyPthreadExitTransition` / `KytyPreparePthreadExit`. The matching shared owner records a typed exit request and returns to its host gateway; it is consumed only after root/nested segments and host calls finish. Raw legacy pthread exit is not classified by alias. |
| Thread identity/detach/yield | Same file's `PthreadSelf`, `PthreadEqual`, `PthreadGetthreadid`, `PthreadDetach`, `PthreadYield` and listed POSIX wrappers. These read/mutate actual thread state. `SchedulerBackoffOnce` is host `SwitchToThread`/`Sleep(0)` or `std::this_thread::yield`, **not** signal dispatch or the signal-polling sleep helper. |
| Selected attributes | Same file's init/destroy/copy/getters, stack-size, affinity, scheduling parameter/policy and POSIX scope adapters. They operate on opaque host/guest attribute state, including preserved guest priority and validated scope. No synchronous guest invocation. This is not approval of all setters, actual priority scheduling parity or asynchronous cancellation. |
| Non-wait mutex operations | Same file's init/destroy/trylock/unlock and listed attr/wrapper bodies. `NativeMutexTrylock` / `NativeMutexUnlock` use internal host locking and actual owner/recursion state without the guest-signal dispatch loop in `NativeMutexLock`. Host critical-section contention is not a claim of guest-wait stoppability or race-free destruction. Existing fresh/consecutive-unlock EPERM masking is not revalidated as native semantics by this table. |
| TLS keys | `PthreadKeyCreate/Delete/Setspecific/Getspecific` and POSIX adapters register actual generation-tagged key/thread-local data. `DestructPthreadSpecific` releases the key mutex before `InvokeCallback` and reacquires it after the nested boundary returns; owned callbacks retain their execution identity. |
| libc environment / cxa | `src/libs/libC.cpp` `init_env` stores actual owner context. `cxa_atexit`, `cxa_finalize`, `LibcInternalExtCxaThreadAtexit` / `RunThreadAtexitDestructors` preserve execution identity; finalization claims/removes callbacks under a lock, then invokes matching owned guest code through `RuntimeGuestExecution::InvokeCallback` outside the lock. Failure/exit returns to these host scopes before the outer return barrier propagates it. |
| printf | `LibC::libc_printf` uses the existing GP/XMM/stack `VA_ARGS` capture in `src/libs/vaContext.h`; `GetGuestPrintfCtxFunc` in `src/libs/guestPrintf.cpp` returns fixed host `kyty_printf_ctx`, not a guest function pointer. No AVX/AVX512 or guest comparator ABI is introduced. Gateway integration must preserve varargs AL, vector arguments and the original overflow area. |
| Strings / scalar math | Listed `src/libs/libC.cpp` comparison/string bodies call direct CRT routines or bounded scalar loops; float/double math calls host math with baseline XMM input/output or pointer outputs. No guest callback, comparator or exception-handling entry is admitted. This is not blanket LibcInternal coverage. |
| malloc/free | `LibcInternal::Malloc` / `Free`, `FreeAlignedBase` and `src/libs/guestHeap.cpp` `Allocate` / `Release` / `Free` use real arena/free-list/block and alignment registries plus runtime VM. They do not call a guest new-handler or registered application heap API. Generic mspace cookies/stats are not treated as allocator implementation evidence. |

`RuntimeGuestExecution::InvokeCallback` itself acquires callback work and uses
`Invoke` / `GuestCallBoundary::Invoke` with a dedicated transition function.
Simply acquiring a work lease or storing a guest callback pointer would not
have supplied that nested return/recovery boundary.

### Important contract limits, not hidden success claims

The admitted implementations still contain existing pointer assumptions,
`EXIT`/assert invariants and allocating C++ containers. In particular,
`MapFlexibleMemory` can `EXIT` on unsupported protection, and VM rollback
invariants remain host-fatal. The table does **not** turn those paths, invalid
guest-buffer access, C++ allocation exceptions or host SEH into contained guest
failure. They require their own hardening before a complete untrusted guest
lifecycle claim. This audit establishes normal return ABI and absence of an
uncovered synchronous guest callback for the listed paths, not total-function
fault safety, owner resource reclamation, or complete native API semantics.

## Explicit exclusions / follow-up ownership

- **Condition waits and join:** not in this header. Parent's separate wait agent
  owns `PthreadJoin`/`Posix::pthread_join`, condition wait/timed/reltimed paths,
  their mutex reacquisition and cancellation/stop result propagation. In-progress
  source changes were observed, not declared finished or tested here. Even a
  completed condition/join slice must not imply that all other waits stop.
- **`pthread_once`:** its initializer already uses `InvokeCallback`, but a
  contended call still enters a condition wait and
  `KernelDispatchPendingSignalForCurrentThread`. Do not whitelist NIDs
  `14bOACANTBo` / `Z4QosVuAsA0` solely from the initializer fix. Separately prove
  owner-stop behavior, callback failure state reset and safe signal transitions.
- **Mutex/rwlock acquisition, barriers, semaphores, event queue/flag waits,
  sync-on-address waits and sleep:** no blanket returning/stoppable approval.
  Inspected signal-polling paths can reach `CallGuestSignalHandler`; signal
  handler context changes / siglongjmp remain outside this contract. Internal
  host locks in listed non-wait operations are not equivalent to those guest
  synchronization waits.
- **Signals, fibers, longjmp, exception raise/unwind, native process/thread
  termination and cancellation:** no entries. `LibC::exit`, `LibKernel::exit`,
  abort, stack-check failure, kill, cancellation-control, exception diagnostics,
  setjmp/longjmp and fiber APIs are not licensed by the scoped exit adapters.
- **Dynamic function pointers:** `KernelDlsym` returns raw native/HLE addresses
  (and some diagnostic interposers), so allowing the call does not automatically
  cover subsequent indirect calls. It is excluded until target ownership,
  lifetime and gateway admission are enforced for the returned pointer too.
- **Other callback registries:** no `LibC::atexit`, `catchReturnFromMain`, libc
  execute-once, qsort/bsearch, guest heap-API registration or sysmodule-family
  blanket allowance. The checked module-start/finalizer entries do not prove
  every sysmodule service or indirect callback transition is covered.
- **Real remaining service work:** file/socket I/O and its blocking device/pipe
  branches, sysmodule loads, user/system/app-content services, graphics, media,
  DRM and provider closure were not classified from registration presence.
  Their absence is deliberate, not evidence that applications never need them.
- **Typed-looking placeholders also excluded:**
  `LibKernel::pthread_cxa_finalize` (`kbw4UHHSYy0`) is currently log-only,
  unlike implemented `LibC::cxa_finalize`; libc heap-error reporting ignores the
  error; mspace create/stats fabricate cookies/statistics; six-argument generic
  log/success stubs are not implementations. No allowance was generated from
  their names or NIDs. Guarded copy/truncation routines and further libc wrappers
  need separate semantic/failure review. `strtol`/`strtoul` also need guest LP64
  versus Windows host `long` width review rather than automatic CRT approval.

## Checks performed / parent handoff

Read-only source cross-check found **118 unique pairs, no duplicate pair**, all
with an exact literal `LIB_FUNC` registration in current production sources.
The table is self-contained C++ inline constexpr data with no build flags,
test macros, address overrides or ABI casts. No compile/test pass is claimed.

Parent integration should preserve the original registration/address and
private-native checks and use this array only for the returning-HLE decision.
Focused validation still needs normal/stack/vector/vararg returns; deferred
failure and scoped thread exit through nested module/cxa/key callbacks while
proving outer RAII destruction; stop racing worker publication; rejection of
the excluded expression even for the same NID; and unresolved/provider changes
without cross-owner target reuse. None of these can be replaced with a reduced
unresolved count or a nonzero thunk address. Keep launch disabled until the
parent's complete preemption/waits/callback gate is demonstrably satisfied.
