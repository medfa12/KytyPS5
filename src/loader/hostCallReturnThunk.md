# Owner-bound guest → HLE → guest gateway

## Parent validation

Windows build and fixture execution passed, without a capability skip.
Parent added an actual unresolved-import thunk inside a nested boundary: its
resolver finishes RAII, inner abort returns to HLE, HLE cleanup finishes, and
the outer return gateway aborts without either guest continuation resuming.
The 256-frame capacity boundary also executes: excess entry is refused and all
256 admitted HLE frames complete cleanup before the outermost tagged abort.
Evidence: `_TempData/vsh-host-return-combined-build.log` and
`_TempData/vsh-host-return-combined-tests.log` (0.13s). This remains primitive
validation, not proof of complete production callback coverage/owner stopping.

`HostCallReturnThunk` supplies a generic **baseline AMD64 SysV** gateway around
an already known, correctly typed HLE address. It is not a resolver, unknown-API
error implementation, owner-stop service, stack switch, or admission-gate change.
It implements the guest-facing return barrier described in
`docs/vsh-deferred-failure-boundary-20261104.md` §§1–3.

## Integration API

```cpp
HostCallDisposition SYSV Enter(void* context, uint64_t guest_return,
                               HostCallEnterReason reason) noexcept;
HostCallDisposition SYSV Leave(void* context, uint64_t guest_return) noexcept;

auto gateway = HostCallReturnThunk::Create(
    exact_owner_boundary, known_typed_hle_address, owner_export_context,
    Enter, Leave, HostCallAbi::BaselineSysV, &status);
```

Here `SYSV` means `KYTY_GUEST_CALL_BOUNDARY_SYSV`. Publish `GetAddress()` only
after successful creation. A deferred-import resolver must return **this gateway
address**, not bypass it with the raw HLE target. It still returns normally first.
Do not put a gateway around native guest code: that would misclassify guest
frames as a normally returning host operation.

`Enter` runs with ordinary host C++ semantics and may use RAII. On `Call`, it
validates owner/export/provider/generation, atomically implements owner admission,
and establishes any execution lease. `Proceed` selects the configured typed
target for this **first invocation**. `Abort` must latch/observe sticky owner
failure and clean up its attempted admission itself: no `Leave` follows a refused
entry. The primitive does not manufacture an API result or choose an alternative
owner. Null context is permitted if callbacks do not require one.

`Leave` runs **after the typed HLE target has returned normally**, including all
of its destructors and internal host callers. It normally releases its admitted
execution lease and checks the owner's sticky failure with the runtime's required
memory ordering. Return `Proceed` only if guest resumption is allowed; `Abort`
requests the exact bound boundary's failure gateway. A real typed negative/zero
API result is otherwise passed through unchanged, never interpreted as failure.

Both callbacks must return normally, finish all callback RAII, and restore any
nested host-to-guest boundary stack before returning. Neither callback may itself
invoke a nonlocal failure/exit gateway. A callback may synchronously reenter via
another actual guest boundary, subject to the capacity rule below. Any runtime
lease stack must likewise be actual-thread LIFO, not borrowed emulated pthread
state. `guest_return` is diagnostic, not a callback-supplied recovery address.

## Transfer mechanics and owner safety

1. Entry saves full RAX (including variadic AL), RDI/RSI/RDX/RCX/R8/R9/R10 and an
   aligned FXSAVE64 image before calling any host helper. Helpers run with DF
   clear, empty x87 state/default masked FP controls and default MXCSR.
2. A normal C++ helper validates `boundary->FailureFunction()` against the actual
   thread's exact active innermost boundary. It records the original guest return
   PC, implementation identity and actual Windows thread ID in **host TLS**.
   Nested owners/objects do not search or reuse an older owner's boundary.
3. After `Enter` and all nested callback frames have returned, successful entry
   replaces just the original return slot with a generated epilogue address,
   restores arguments/FP state and tail-jumps to the configured HLE. Target entry
   RSP and every stack argument retain their original addresses and alignment.
   On refused entry, the TLS record is removed and the original argument state
   is restored before tail-jumping to the matching failure entry, without HLE.
4. HLE RET consumes the substituted slot and reaches the epilogue. It saves
   RAX/RDX and a complete FXSAVE64 image before calling the normal leave helper.
   The helper validates the top TLS record, invokes `Leave`, revalidates the exact
   active boundary, and pops the record. All its ordinary C++ frames return.
5. On success the epilogue restores exact result state and jumps to the **TLS
   continuation**, never a return PC recovered from guest stack storage. On
   failure it reestablishes normal SysV function-entry stack alignment and
   tail-transfers to the real `GuestCallBoundary::FailureFunction()`. Only guest
   and trivial generated frames are discarded, not HLE/helper/callback frames.

The transient substituted stack slot is necessary for an actual HLE RET; it is
not authoritative continuation storage. This is a cooperative correctness
boundary, not protection against guest arbitrary-memory writes or deliberate
return-address corruption. The authoritative TLS record is not a guest-stack
linked record and cannot be selected using an emulated/borrowed thread identity.

Every HLE→guest callback **must establish its own actual boundary immediately
around guest instructions**, even for the same owner and boundary object. An
inner abort returns a tagged outcome to its host caller; that host caller rolls
back and destroys its objects normally. The older HLE then returns into its own
generated epilogue, whose leave callback sees sticky failure and aborts the older
guest segment. Never surround the entire host HLE/loader operation with a
discardable boundary or jump from inside that operation.

## Supported and unsupported ABI contracts

* Supported platform: Windows AMD64 GNU/Clang ABI (not ARM64EC/MSVC-only ABI).
  Creation independently requires a successful mitigation query proving user
  shadow stacks disabled, just like `GuestCallBoundary`. CET-enabled compiler
  builds (`__CET__ != 0`) are also rejected. Policy must remain unchanged while
  objects/calls exist. Arbitrary guest continuation PCs are not IBT landing pads;
  a deployment enforcing IBT/other incompatible control-flow policy is outside
  the baseline platform contract and must not admit these gateways.
* Supported arguments: baseline SysV INTEGER/SSE/SSEUP register arguments,
  integer/FP/vector/aggregate stack arguments, overflow variadic arguments and
  original AL vector-register count. R10 is also preserved for ABI extensions
  using a static chain. RBX/RBP/R12–R15 rely on normal SysV callee preservation.
* Supported results: exact RAX/RDX and complete XMM0/XMM1 contents; ordinary
  integer, SSE and mixed register aggregates; hidden-sret memory aggregates;
  legacy x87 ST0 and ST0/ST1 (including 80-bit long double and complex-long-double
  register returns). FXSAVE64/FXRSTOR64 also preserve the target's legacy x87
  state/controls and MXCSR around `Leave`. All XMM0–15 are saved, not just the
  argument/result subsets. The guest/HLE compiler's **actual SysV classification**
  must agree; the gateway does not fix differing host typedefs or signatures.
* AVX/AVX-512 upper YMM/ZMM state, mask registers, AMX, Microsoft-x64/vectorcall,
  custom calling conventions, callee-pop stacks, variadic ABI mismatches,
  exception propagation and asynchronous unwind/recovery are **unsupported**.
  `MicrosoftX64`, `Avx` and `Avx512` contracts return `Unsupported` at creation;
  callers must not mislabel an unaudited function `BaselineSysV`. There is no
  function-type inference from a raw address. R11/flags are scratch. Baseline
  SysV requires DF clear; arithmetic flags are not API results.
* Targets and callbacks must return synchronously on the same actual thread and
  stack. No exception, `longjmp`, pthread exit, coroutine/fiber suspension, stack
  switch or arbitrary nonlocal abort may cross generated/HLE frames. A noreturn
  operation that abandons this transition is not an admitted target contract.
  There is no registered unwind metadata or shadow-stack repair.

## Capacity, errors and lifecycle

Host TLS reserves `MaximumDepth == 256` trivial transition records per actual
thread; no explicit allocation occurs during generated entry/return or its
helpers. Compiler TLS runtime initialization may execute ordinary host helper
frames, which always complete before a transfer. Sufficient committed stack and
valid platform stack bounds/headroom remain caller obligations.

If transition capacity is exhausted, `Enter` is called normally with
`CapacityExceeded` **without pushing another record**. It must latch owner failure,
return `Abort`, acquire no new execution lease and not recursively enter another
gateway. After its RAII returns, the exact current boundary is aborted. Existing
older records remain live for normal enclosing host cleanup and return barriers.
Thus ordinary capacity refusal is not a fabricated API return or process exit.

Missing/wrong boundary, invalid callback disposition, corrupted TLS LIFO or
continuation, or a capacity callback that fails the above contract is an internal
invariant violation and traps. It is not an app-local missing-coverage fallback.
Admission/dispatch must prove exact boundary coverage and contract ownership;
context, provider and boundary must describe the same application lifetime.

Creation uses normal writable storage to generate code, allocates owned RW
pages, copies completed code, then seals RX and flushes the instruction cache.
There is no RWX interval, executable patching, per-call code mutation, GOT patch,
or provider discovery. Allocation/protection/generation failures return explicit
creation status and no published address. Publication requires normal release /
acquire synchronization; immutable code may be called concurrently.

The **gateway object, boundary object, context, callback code, typed target,
provider mappings and owner generation must all remain pinned** from publication
through the last completed epilogue/failure transfer. Revoke every published or
copied callable reference and quiesce all in-flight target/helper/epilogue calls
before destruction or provider unload. Returning from the raw HLE target alone
is not sufficient quiescence. Destruction frees only this object's code mapping;
it does not drain threads, invalidate stale pointers, free providers or undo
external leases. Failed mapping release is an infrastructure invariant failure.

A sticky check cannot stop a peer in a guest-only loop, wake an arbitrary host
wait, or close the race after the final health observation. Owner-wide admission,
stop/drain, guest execution safepoints and host-only failed cleanup remain runtime
obligations. This gateway does not justify relaxing shared-app admission alone.

## Fixture handoff

`tests/HostCallReturnThunkTests.cpp` links this implementation,
`guestCallBoundary.cpp`, xbyak and platform thread support (C++17 or later). It
does not require `common::VirtualMemory::Init` or the emulator/runtime linker.
Parent owns CMake integration and exclusive `_Build/windows` execution.

Authored cases use the actual generated gateway plus actual `GuestCallBoundary`:
first-call raw integer/XMM/stack/AL/nonvolatile preservation; exact RAX/RDX,
128-bit XMM0/1 and two 80-bit x87 returns despite clobbering callbacks; compiler
mixed-register and hidden-sret aggregate calls; compiler variadic AL; nested
different owners with inner-enter and inner-leave aborts followed by real outer
HLE RAII destruction and outer return-barrier abort; same-gateway recursive HLE
callbacks; concurrent shared gateway execution with actual-thread TLS; RX
sealing and one-peer sticky failure while all concurrent HLE calls return normally;
immutable code, quiesced release and explicit unsupported ABI rejection.
Unsupported platform/boundary capability exits **77** (skip), not a passing
recovery claim. Capacity exhaustion, production stack switching/TEB policy,
whole-owner preemption and asynchronous events are not fixture evidence here.

No builds, test executions, process operations, agents, commits, production
integration, CMake edits or admission changes were performed for this handoff.
