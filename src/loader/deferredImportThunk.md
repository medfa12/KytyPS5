# Deferred import trampoline handoff — 2026-11-04

## Parent validation

The Windows production-toolchain build and `deferred_import_thunk_tests` now
pass. Evidence: `_TempData/vsh-deferred-thunk-build.log` and
`_TempData/vsh-deferred-thunk-tests.log`. Parent corrected the failure contract:
the resolver returns a failure-gateway address normally, completing its RAII
before the thunk transfers control. The isolated failure fixture checks that
cleanup happened. This validates the primitive, not production app termination
or permission to relax shared-app import admission. Earlier source-only status
below describes the original handoff.

## Scope and status

Added only:

- `src/loader/deferredImportThunk.h`
- `src/loader/deferredImportThunk.cpp`
- `tests/DeferredImportThunkTests.cpp`
- `src/loader/deferredImportThunk.md` (this handoff)

No loader integration, CMake changes, `_Build/windows` changes, builds, test
execution, guest/live runs, commits or pushes were performed. The fixtures are
**written but unexecuted**, not evidence of a passing build. Existing dirty
files were not edited. This primitive alone does not authorize admission of
deferred imports. Its policy boundary is audit sections 4–5 in
`docs/vsh-native-import-admission-audit-20261104.md`.

## Integration API

```cpp
// Must be noexcept and explicitly SysV even on a Windows host.
uint64_t KYTY_SYSV_ABI ResolveDeferred(void* record, uint64_t caller) noexcept;

auto thunk = Loader::DeferredImportThunk::Create(stable_record, ResolveDeferred);
if (!thunk) { /* fail the admission/allocation transaction */ }
// Publish thunk->GetAddress() only after the complete owner record is visible.
```

`Create` returns `std::unique_ptr<Loader::DeferredImportThunk>`. Null record or
resolver and VM allocation/protection/cache failures return null. C++ allocation
or Xbyak generation exceptions may propagate during creation, before publication.
Call `Common::VirtualMemory::Init()` through normal startup before creating one.

The resolver receives the opaque record and the **original guest return PC**,
not a return into the trampoline. It must return a validated nonzero address of
the real implementation, never an API result, another unresolved thunk or an
unvalidated/stale address. On failure it must return the address of the owner's
already-audited **nonreturning app failure gateway**. Resolver RAII must finish
normally before the thunk tail-transfers to that gateway; directly abandoning
the resolver's live C++ frames is forbidden. A returned zero executes `UD2` solely as an
internal contract-violation guard; it is not the production unresolved-import
failure mechanism and must not be relied on to terminate just the owner.

The resolver must be safe under concurrent/reentrant invocation and leave
owner/TLS state unchanged on successful return. There is one resolver callback
per thunk invocation, with no primitive-level target cache, GOT patch or retry.
Each successful invocation jumps to its returned target once, including the
very first call. The target returns directly to the original caller.

## ABI / implementation boundary

Supported: baseline AMD64 **System V C/C++ calling convention with SSE/SSE2**,
using the project's GCC/Clang `KYTY_SYSV_ABI`. The generated entry is not a
Microsoft x64 entry; neither is the resolver. The pointer must only be invoked
through a correctly typed SysV call or a normal guest PLT/GOT transfer.

- Saves/restores all 64 bits of `RAX`, including varargs vector count in `AL`.
- Saves/restores `RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9` (including hidden sret
  pointers), plus `R10` as an extra precaution.
- Saves/restores all 128 bits of `XMM0`–`XMM7`, not just scalar low lanes.
- `RBX`, `RBP`, `R12`–`R15` and ABI-preserved FP control state are preserved by
  the conforming resolver. `R11` and flags are scratch, not baseline arguments.
- The 200-byte temporary frame converts incoming `RSP % 16 == 8` into
  pre-resolver-call `RSP % 16 == 0`; it is removed before the tail jump. Incoming
  return address and every stack argument retain their original location.
- The primitive does not change segment bases, owner context or TLS. Successful
  callback behavior must honor that same constraint.
- Return ABI is not interpreted: integer, SSE, memory and x87 returns go straight
  to the caller because the thunk is no longer on the execution stack.

Not supported/claimed: AVX/AVX-512 register arguments or upper YMM/ZMM lanes,
Microsoft x64 guest calls, custom guest conventions, misaligned non-SysV calls,
or C++ exception/SEH unwinding through the generated resolver-call frame. No
unwind metadata is registered. In particular, do not assume Windows `longjmp`
or a generic host exception can safely cross this frame; the production owner
must establish an explicit safe app-failure escape before enabling deferral.
`ENDBR64` is emitted at entry, but complete CET/CFG policy integration is not
claimed by this change.

## Allocation, concurrency and retirement

One owned 4 KiB VM allocation per thunk; OS reservation granularity may be
larger. Xbyak generates into a bounded ordinary writable stack buffer. The owned
allocation is RW while populated, then RX and instruction-cache-flushed before
publication. No RWX request, global code pool, mutable executable patching or
per-call allocation is used. Linux's current `UINT64_MAX` allocation failure
sentinel is explicitly rejected as well as zero. Partial protection/cache
failures release the mapping via RAII. Failed `Free` is an infrastructure
ownership invariant violation and terminates rather than silently losing it.

Independent creations and calls can run concurrently; there is no shared
mutable allocator state in this primitive (the project VM layer provides its
own mapping synchronization). Sharing/publishing the object itself and its
record requires normal synchronization; creation/destruction must not race use.
The record address is baked into the code: keep it stable, not a movable vector
element. The unique owner must keep the record, callback code and thunk alive
until published references are revoked and executions are quiescent.

**Quiescence includes execution of the resolved target**, not merely completion
of the resolver. A tail jump cannot carry an RAII provider lease through target
return. Provider lifetime must therefore be pinned by the owner execution epoch
or equivalent external policy. Generation checks without this lifetime pin are
insufficient. Unload/reload, owner namespace/version/image validation, slot
invalidation, startup rollback and guest-thread lock ordering are loader work.
If the loader later patches a GOT directly, that bypass also needs safe
invalidation/lifetime rules. Never hold registry locks while invoking guest code.

## Host fixtures handed off for build owner

`DeferredImportThunkTests.cpp` has a standalone `main`, suitable for a small
host target linking only this source, `deferredImportThunk.cpp`, `common`,
`xbyak::xbyak` and platform thread support. Use the usual generated-config include
paths (`${inc_headers}` in the root CMake). Do not link a full emulator/main.
The root loader glob already includes new loader `.cpp` files on reconfigure.
Suggested independent target shape (not applied):

```cmake
add_executable(deferred_import_thunk_tests EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DeferredImportThunkTests.cpp"
    "${KYTY_SOURCE_DIR}/loader/deferredImportThunk.cpp")
target_include_directories(deferred_import_thunk_tests PRIVATE ${inc_headers})
target_link_libraries(deferred_import_thunk_tests PRIVATE common xbyak::xbyak)
# Add Threads::Threads when required by the configured host toolchain.
```

Fixtures target Windows x86_64 and Linux x86_64 and cover:

1. Raw first-call entry probe: full RAX, every integer argument, R10, all 128
   bits of XMM0–7, callee-saved registers, stack words, exact caller PC, entry
   alignment and restored RSP; a nonzero result and exactly one target entry.
2. Typed mixed integer/vector/stack arguments with mixed RAX/XMM0 struct return.
3. A compiler-generated **SysV variadic call** overflowing both integer and FP
   registers: checks AL == 8, all GP/FP values and overflow words. This uses a
   raw callee probe rather than a host-platform `va_list` implementation; it
   does not claim to test the emulator's separate varargs helper library.
4. Eight concurrent first calls rendezvous inside the same resolver, followed
   by 128 calls per thread; independent allocation/reclamation also overlaps.
   Target and resolver counts detect dropped/doubled invocations.
5. Resolver explicitly clobbers all volatile argument registers and XMM0–7.
6. RX permissions, executable-code immutability and repeated unmapping on
   destruction, plus null-argument creation rejection.
7. Isolated child fixtures for a deliberately null resolver (`UD2` / illegal
   instruction) and a returned nonreturning synthetic failure gateway (distinct
   exit 27, not an API return), checking that resolver RAII already completed.
   The latter deliberately exits only its fixture subprocess; it
   **does not demonstrate a production app-only failure barrier**.

Before enablement: build/run these host fixtures with the production Windows
compiler, inspect any ABI/compiler diagnostics, then establish loader-specific
app-only failure, initializer rollback, owner/generation/provider pinning and
unload/reload tests. Allocation-failure fault injection, guest lifecycle tests,
full unwind/CET/CFG support and AVX guest ABI support are not included.
