# Owned native guest preemption — integration in progress (2026-11-05)

## Resumed implementation status

### Latest validation and deferred-start integration

The full emulator and CPU targets built in
`_TempData/vsh-native-drain-integrated-build.log`. Seventeen of twenty selected
tests initially passed; three fixture errors (raw host entry instead of private
RX trampoline, a temporary-file read accidentally using the truncating constructor,
and an overlength fixture username) were corrected. All three reruns passed in
`_TempData/vsh-owner-fixture-fixes-tests.log`; the other seventeen passes are in
`_TempData/vsh-native-drain-integrated-tests.log`.

Native stop now includes real key/thread-atexit destructor loops on the proven
guest stack, with release flags zero through join. `runtime_native_drain` parks
a real host epilogue after logical counts reach zero and proves that the new OS
handle gate refuses completion until actual native termination. VSH uses this
gate before service/identity retirement and quiescent publication.

Following these checks, initial preparation now uses hard relocation admission
rather than requiring every unused typed function import to resolve to an audited
target. First-call classification/failure remains unchanged. Ten exact registered
join/mutex/condition wait identities use the tested returning cancellation paths.
The final rebuild and four admission/wait rechecks passed in
`_TempData/vsh-deferred-start-build.log` and
`_TempData/vsh-deferred-start-tests.log`. Visible successful app launch/close is
not yet verified.

### Live preparation results

The visible `vsh-owned-native-live-20261105` session booted with artwork and
navigated Games to Media at the normal host window size. Apple TV (PPSA01783,
app 104) and YouTube (PPSA01651, app 105) both returned a normal launch-error
dialog with `entry=0`, `result=0x80020008`, and `ShellUI retained`.

Apple TV has eight hard relocation blockers for three registered host objects:
`f7uOxY9mM1U` (stack guard), `djxxOmW6-aw` (program-name pointer), and
`P330P3dFF68` (libc flag). YouTube has seven blockers for those objects plus
`ZT4ODD2Ts9o` (a separate libc-internal flag used by libSceNpCppWebApi).
These require audited per-owner storage; admitting shared host addresses would
violate isolation. Owner-local data implementation is now integrated and its
production relocation/dlsym isolation fixture passed alongside seven affected
loader/lifecycle checks; see `vsh-owner-import-data-20261105.md`. A new live
launch is still needed to verify progress past preparation.

Evidence: `_Runs/vsh-owned-native-live-20261105/_kyty.txt` and
`_TempData/vsh-owned-native-live-apple-rejected.png`,
`_TempData/vsh-owned-native-live-recovered.png`, and
`_TempData/vsh-owned-native-live-youtube-launch.png`. These establish safe
pre-entry rejection and shell recovery, not native app frames or running-app close.

### Integration details

Legacy signal/context senders now retain actual native handles; the diagnostic
floor watchdog also owns a duplicate throughout its lifetime instead of reopening
a numeric ID. `runtime_native_target_pins` is wired into CTest. Its handoff has
passed its parent test run.

The bounded-admission review found that thread cleanup callbacks ran after the
original guest stack was restored to the host stack. A looping destructor would
therefore be outside the stopper's trusted stack proof. The pthread integration
corrected this, and both destructor-loop regressions passed before launch-gate relaxation.

The loader now has a separate hard relocation checker, used again before late
module initializers/finalizers. It requires genuinely patched immutable function
entries, rejects unsupported/untyped/addended imports and nonprivate data, and
checks initializer/finalizer/native-call targets against private executable ELF
segments. Explicit native-kernel loads are refused by filename or export metadata.
The initial VSH launch gate now uses these hard checks; unsupported function calls
are refused at their immutable first-call entries.

Shared guest executable allocation/protection requests now fail before memory
mutation; trusted loader allocations remain available. VM operations still assume
normal renderer service progress: synchronous GPU completions are not canceled by
abandoning their stack-backed completion objects. This is not general host-wait or
GPU-failure containment.

Prepared UserService identity subscriptions and SystemService entitlement-event
queues are connected to VSH startup and actual post-drain cleanup. Tests cover
independent shell/two-owner consumption and fresh-lifetime relaunch. These new
tests and the updated loader fixtures passed parent execution. The earlier
seven-test success below predates these changes.

### Earlier validated native-stop slice

The implementation now differs from the historical blocked-design review below.
It has initialized top-boundary publication, a call-free arbitrary-RIP stop
landing, actual native-handle pins, and shared-runtime signal/context/fiber
exclusions. Positive native-loop fixtures built and passed on Windows in
`_TempData/vsh-native-stop-tests.log` alongside six affected recovery/import/wait
checks. The positive cases keep their fixture-release flags zero through actual
native join, including a host join already blocked when stop is requested.
This does not establish complete title launch capability.

Parent integration publishes exact private ELF `PT_LOAD`/`PF_X` intervals before
application entry and module initialization. Invalid/out-of-mapping intervals are
refused; generated host thunks, data and foreign modules are excluded. Private
module mappings remain pinned. The controller loads the immutable snapshot before
suspending a target and acquires no loader lock while that target is suspended.

VSH has a bounded stop pump alongside its real native main-thread join, followed
by stop retries during peer drain. A separate `stop_mutex` pins incarnation,
native handle and stack without being held throughout a blocking join. Creators
and reapers take join then stop locks; stoppers take only stop. A new fixture
starts a host join before requesting native preemption to cover that integration
deadlock. Stop acceptance/redirect remains distinct from actual quiescence.

Read-only review identified a legacy numeric-OS-ID reuse race in signal and
context senders. The latest integration above fixes and validates actual-target
pinning before bounded shared admission.

## Historical blocked-design review

## Outcome and production handoff

**Owner-wide asynchronous preemption is not implemented or certified by this
slice. A pure guest loop still cannot be made to drain by RequestStop alone.**
The required eligibility proof crosses the signal/APC, context-editing and fiber
paths in `src/libs/libKernel.cpp`, which is outside this slice's write scope.
Private executable RIP, a zero host-critical counter and an existing boundary
are necessary but not sufficient evidence that only guest frames will be
discarded. Adding a redirect under those checks would be unsafe, not a partial
implementation of the requested contract.

Host-only API added in `kernel/pthread.h`:

```cpp
PthreadOwnerStopResult PthreadRequestOwnerStop(
    const std::shared_ptr<Loader::RuntimeGuestExecution>& execution) noexcept;
```

- Null/invalid owner: `PthreadOwnerStopStatus::InvalidOwner`.
- Valid owner: closes admission with `RequestStop()` and returns
  `PthreadOwnerStopStatus::Unsupported`, tagged with that control's lifetime.
- There is no success/redirected/drained outcome, opt-in override, native-thread
  lookup, suspend, context modification, cancellation, forced termination or
  outstanding-work manipulation hidden behind this API.
- Existing stop-aware HLE waits/return gateways can still finish cooperatively.
  A remaining pure loop retains its Thread/Guest work and mappings. The parent
  must keep close pending, not retire/reclaim it or loosen shared admission.

This API makes the blocker explicit to the close/drain caller; it does not solve
the user's launch/close requirement. No edits to runtimeLinker/runtimeVshLaunch,
imports, libc, libKernel, CMake, prior tests or existing boundary/normal-exit
semantics were made. The r8/r9 `RunOnGuestStack` input fix is retained.

## Concrete evidence and unsafe counterexample

### 1. Guest signal text can sit above live host frames

In `src/libs/libKernel.cpp`:

- `SignalApcHandler` creates `SignalDispatchScope` and a `SignalUcontext`, calls
  `CallGuestSignalHandler`, and subsequently applies the signal context back to
  the Windows context. `CallGuestSignalHandler` directly calls the guest handler
  (including its siginfo variant).
- `KernelDispatchPendingSignalForCurrentThread` also places a live
  `SignalDispatchScope` around the guest call.
- Fault-driven signal handling can enter guest handlers without a queued
  pthread signal. Checking `pending_signal_mask` alone cannot prove inactivity.
- `g_dispatching_signal_handler` is private actual-thread TLS. Its current-thread
  getter is not a remote-publication or enqueue/restore interlock.
- A special-APC helper can temporarily borrow a `Pthread` using
  `PthreadSwapSelfForSignal`; the borrowed record is not the helper's actual
  native-thread/boundary identity.

Counterexample: suspend an application while its native guest signal handler is
spinning inside owner-private executable text. An older application boundary
can remain live; the signal scope is not itself a host-critical-counter guard.
Redirecting to that older boundary would discard the host signal scope and
Windows APC/context-restoration continuation. An owner-private PC query cannot
make those host frames disposable.

Even publishing a current `signal_active` bit is insufficient: an already queued
special APC or retained user context can run/restore after the eligibility check.
The enqueue/delivery/context-editing paths need a shared arbitration protocol,
not a caller promise that signal code was audited.

### 2. Fibers/nonlocal context restoration are not remotely interlocked

`FiberRun` publishes a local `FiberThreadContext` through a native TLS slot,
retains the caller's CPU context, and uses `FiberSaveContext` /
`FiberRestoreContext` / `FiberStartOnGuestStack`. `FiberSwitch` can move execution
between saved stacks; the native lookup is explicitly refreshed across actual
host-thread migration. These paths do not participate in an owner-stop claim.

`PthreadSuspendUserContext` and the user-context getter/setter/resume paths also
retain/edit native contexts behind a separate registry/mutex. A stale retained
context must not restore a pre-stop guest stack after its boundary has returned.
Observing a zero prior suspend count at one instant is not an interlock against
another context writer. These paths need to be excluded or integrated before
asynchronous stop may be armed.

### 3. Existing boundary storage is synchronous, not remote stop storage

`GuestCallBoundary::Invoke` uses a local `BoundaryFrame` and actual-thread TLS
`g_boundary`. It publishes the frame before the emitted entry writes its stack
anchor/FX image, and restores the previous frame before the local storage dies.
The synchronous validator checks this actual thread's innermost frame, owner,
token and implementation. There is no externally pinned, generation-tagged
descriptor/claim protecting a remote read of the current frame across pop/reuse.
Calling the validator from the stopper would inspect the stopper's TLS instead.

`FailureFunction` is a **SysV call entry**, not an arbitrary-RIP landing:
the emitted code pushes flags/registers, allocates its fixed local area, and
saves FX state on a stack whose call-entry alignment it assumes. A suspended
instruction need not have that alignment, a return address or sufficient live
headroom. Directly assigning `CONTEXT.Rip = FailureFunction()` is invalid even
when owner/token and PC classification match.

### 4. Existing suspension/ownership mechanisms are useful, not a stop proof

- `Common::HostLockCriticalCounter` tracks explicitly protected host operations;
  zero is not proof of absence of host frames, signal state or fiber transitions.
- `PthreadPool::SampleAllThreads` / `SampleOneThread` snapshot numeric OS IDs and
  reopen them later. That best-effort diagnostic pattern must not be copied for
  destructive context edits: IDs and pooled `Pthread` records can be reused.
- The Thread lease covers startup through cleanup; the separate execution/fs
  pins survive until actual join. Neither permits inspecting a reclaimed/reused
  record through an old borrowed pointer.
- Existing native-join serialization and `pthread_gethandle` can help establish
  a future pinned handle, but all pins/locks must be obtained **before** suspend;
  no loader/pool/logger mutex may be acquired while the target might own it.
- `FindProgramByAddr`/broad guest address ranges are not a lock-free proof of an
  exact executable, owner-private, generation-stable text mapping. Data, shared
  providers, JIT/fiber trampolines and retired/rebound regions must not qualify.

## Required integration contract for a genuine implementation

The following is a design handoff, **not a description of implemented support**.

1. **Immutable text proof before suspension.** Parent loader publishes a pinned
   immutable executable-span snapshot tagged with owner identity/lifetime and
   mapping generations. Query only that snapshot while suspended: no callbacks
   into the loader, module discovery, allocation or logging. Pin the mappings,
   execution control and emitted stop landing through acknowledgement/actual
   return. Reject unclassified/shared/non-executable text.
2. **One descriptor per actual native-thread incarnation.** Allocate stable
   descriptor storage before worker publication; pin a duplicated native handle,
   native identity, owner lifetime, critical-counter storage and stack bounds.
   Enumerate/pin descriptors before suspend, then release registry locks.
   Never reopen a numeric thread ID or infer incarnation from a pooled record.
3. **Complete shared transition arbitration.** Signals (queue reservation through
   final APC/context-restore completion), synchronous faults, signal helpers,
   nonlocal/fiber transitions and every native context editor must participate.
   A lock-free exclusive stop claim can succeed only with zero active/reserved
   incompatible transitions. Once claimed, new producers must defer safely;
   an APC must not block waiting for a claim held by its own suspended target.
   Fault dispatch must mark exclusion before calling guest text. A pending-bit
   clear must not release an APC/context reservation prematurely. Unsupported
   routes remain rejected/pending; there is no force-enable fallback.
4. **Generation-tagged innermost-boundary publication.** Publish an externally
   readable top boundary and invocation generation only after anchor/FX state
   is initialized; withdraw it before storage can die. The descriptor/claim and
   confirmed suspension must pin that publication. Nested callback boundaries
   supersede the older host-call boundary; host work in the current segment
   makes it ineligible. A boundary-free host coordinator is never eligible.
   A raw callback from an unreviewed HLE must not look like disposable guest
   execution; incomplete HLE/callback/nonlocal coverage keeps admission closed.
5. **Stop closes admission first.** `RequestStop` precedes enumeration/claim and
   redirect. Then, with a bounded attempt budget, suspend once and synchronize
   via `GetThreadContext`; reject a pre-existing suspend or incompatible context
   owner and release exactly the stopper's own suspend contribution. Evaluate
   PC proof, descriptor generation, top boundary, host-critical count, transition
   claim and mitigation/debug constraints without acquiring target-owned locks.
   Every failure path must resume its own contribution; inability to restore
   progress is a surfaced control error, not permission to terminate a thread.
6. **Separate async landing, never the call-entry failure thunk.** Emit a landing
   with an explicitly verified arbitrary-instruction-entry contract. It must not
   consume an assumed guest return address, assume interrupted RSP alignment,
   call a helper on an unprepared stack, or discard a C++ helper that has not
   returned. Only the pinned innermost disposable guest segment can be restored
   to its saved boundary anchor/FX/GPR/flags state. Stop must produce a distinct
   tagged stopped outcome, not an invented guest failure or normal pthread exit.
   Surviving host HLE/callback/coordinator scopes return normally with RAII.
7. **Acknowledgement before reclamation.** The exclusive claim protects the
   selected invocation until the landing/ordinary return acknowledges it.
   Do not publish a raw stack-frame pointer for later asynchronous use after the
   target can pop it. Hold descriptor/code/mapping pins through that handshake;
   counts reaching zero do not replace native join. Concurrent close attempts,
   signal/context writers, failed `SetThreadContext`/resume, thread exit and
   record reuse need explicit state transitions and bounded retries.

The first missing parent integrations are the immutable owner-text snapshot and,
more importantly, shared signal/context/fiber arbitration. A read-only PC query
alone does not remove this blocker. Those changes require another coordinated
slice owning their entry/restore sites.

## Regression fixture and validation limits

Exported under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS`:

```cpp
bool Libs::LibKernel::TestRuntimeGuestStop(std::string* failure);
```

This is deliberately a **fail-closed regression**, not a positive preemption test.
It allocates/seals actual native executable machine code and runs real native
owner-bound workers in loops that have no HLE or owner-stop poll. It checks:

- Unsupported stop retains outstanding work and does not falsely drain/fail the
  owner or stop a peer owner.
- A protected nested guest callback keeps live host RAII intact while pending;
  retries do not escalate into a redirect.
- Only an explicit **test-only release flag** exits the spin. This is fixture
  teardown, not evidence that owner stop preempted it. Normal nested return then
  destroys host RAII and propagates the existing stop gateway correctly.
- An old owner's request does not act on a replacement pooled/native lifetime.
  Actual joins, leases, boundary depth and critical accounting remain truthful.

Positive pure-loop preemption, critical rejection followed by a successful
eligible retry, and actual nested async recovery are **not implemented/tested**.
Adding a green test that secretly releases the loop and calls it preemption
would conceal the production blocker. Parent dispatcher integration must label
this fixture accordingly and must not derive a launch capability from its pass.

No builds, live runs or test executables were run in this child session. Prior
16-test results were reported by the parent for the earlier integrated slice,
not for this new work. `/docs/**` is ignored; force-add this document if desired.
