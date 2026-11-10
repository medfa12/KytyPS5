# Stop-aware scoped pthread waits — 2026-11-05

Parent validation: `runtime_guest_waits` built and passed on Windows, including
the added no-return-gateway refusal case. See
`_TempData/vsh-launch-close-verified-tests.log`. This validates the focused
no-signal stop contract below; the signal/other-wait exclusions still apply.

Scope: `src/kernel/pthread.cpp/.h` and the included real-worker fixture
`tests/RuntimeGuestWaitTests.inc`. No loader/import/CMake, libc, execution-control,
or prior-test edits are part of this slice. Integrated validation remains pending;
this child session did not build or run tests.

## Admission and bounded cancellation

A wait is scoped only when the actual current pthread has a shared execution
control **and an active guest boundary**. The active boundary must match that
control. Parent integration also requires a matching returning HLE gateway before
admitting a scoped wait, so a raw/dlsym call cannot return with the terminal
released-mutex contract to ordinary guest code. A runtime coordinator running boundary-free host C++ is not a scoped
guest waiter, even when running on a guest stack.

Scoped condition waits require both the condition and mutex to have been created
for that exact execution identity/lifetime token. Foreign/session-owned objects
are refused with `KERNEL_ERROR_EOPNOTSUPP` before waiter registration or mutex
release. This does not request stop or fail either owner. Existing non-scoped
host/session/direct-game operations keep their original admission semantics.

Infinite, relative and absolute condition waits observe owner stop/failure and a
pending actual-thread exit at the existing 10 ms sleeping poll points. Their
waiter-list entries are removed under the condition lock. Cancellation returns
native `ECANCELED`, translated by the existing kernel/POSIX HLE adapters; no root
jump, forced pthread cancellation, host unwind or guest-success result is added.

The original condition deadline is not restarted by spurious wakes or APC polls.
Normal signaling/timeout restores the mutex's prior recursive ownership. Relock
adds stop checks to the existing sleeping 10 ms mutex poll only for scoped
condition waiters, retaining one FIFO waiter rather than dropping/requeueing it
each slice. Stop also releases a waiter already contending to reacquire its mutex.

### Important terminal cancellation ownership contract

After a scoped condition wait successfully releases its mutex, terminal
`ECANCELED` leaves that mutex **released by this caller**, not falsely relocked.
If stop races with acquisition, the newly acquired single level is released
before returning cancellation. Another actual thread's ownership/count is never
stolen or rewritten. Invalid/cross-owner refusal before release leaves the
caller's original ownership unchanged. Normal success and timeout still restore
the full original recursion count, and a relock error is never hidden by a
timeout result.

This is deliberate terminal shared-owner cancellation, **not POSIX
pthread-cancel cleanup semantics**. Unconditional reacquisition on stop can
strand every remaining waiter behind a mutex retained by a guest segment whose
continuation has already been suppressed. These HLEs must return through the
parent's audited owner-stop gateway: guest code must not resume after this
terminal cancellation and assume the mutex is locked. Host/session callers do
not use this altered cancellation contract.

## Owned guest joins versus actual host joins

Scoped guest `PthreadJoin` only admits the same pinned execution control/lifetime.
It checks stop between sleeping 10 ms native-termination polls, refuses competing
joins with `EBUSY`, and leaves the caller's output untouched on cancellation.
If canceled before native join succeeds, it does not free the target stack,
native handle, execution/fs pins or pool record. It never stops its target.

On Windows, `pthread_gethandle` is pinned by a per-record join mutex, and a
zero-time `WaitForSingleObject` must observe actual thread termination before
calling native `pthread_join`. The bundled `_pthread_tryjoin` is intentionally
not used: an early internal `ended` flag is insufficient evidence of actual OS
thread termination. Linux uses `pthread_tryjoin_np`; other platforms refuse
scoped joins until an equivalent native primitive is audited.

Boundary-free host/coordinator joins still serialize and call blocking native
`pthread_join`, including after owner stop. This preserves actual reaping and
the detached collector's responsibility. Successful native join always reclaims
the target, even if a concurrent stop means the guest HLE must report canceled
instead of publishing its result. Ordinary admitted guest joins preserve the
returned pointer and perform normal reclamation.

## Explicitly unsupported / unchanged waits

This slice does not claim whole-owner preemption or general wait cancellation.
Standalone mutex locks/timed locks, rwlocks, barriers, concurrent `pthread_once`
waiters, sleeps/nanosleeps, and other kernel/I/O/SDK waits retain their previous
behavior. Arbitrary pure-guest loops, guest-owned mutexes retained on termination,
native host coordinators that never return, and asynchronous/APC nonlocal exits
are not solved. These limitations must not be used to relax VSH shared admission.
The existing wait polls still reach `KernelDispatchPendingSignalForCurrentThread`;
its synchronous guest-signal transition is not yet covered. Accordingly these
waits remain excluded from `applicationReturningExports.h` even after the focused
no-signal stop fixtures pass.

## Fixture handoff

Exported under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS`:

```cpp
bool Libs::LibKernel::TestRuntimeGuestWaits(std::string* failure);
```

The parent owns dispatcher/CMake integration and the sole Windows build. The
fixture creates real native workers and returning HLE gateways and checks:

- Simultaneously parked infinite/long relative/long absolute condition waiters
  released by owner stop without a condition signal.
- Native cancellation results, removed waiter entries, released ownership,
  destroyed host RAII, suppressed guest continuation and drained leases.
- Same-owner relock contention canceled without stealing a main-thread mutex.
- Unrelated owner isolation, ordinary condition signaling, fixed timeout budgets
  under spurious wakes, and recursive lock restoration.
- A canceled guest join leaves a still-blocked native target intact, while a
  boundary-free coordinator actually joins it only after native return.
- Ordinary owned guest join pointer/reclamation behavior, foreign condition/join
  refusals without global stop, and an explicit non-runtime session timeout.

No execution/pass claim is made. The local static check is `git diff --check`.
The repository ignores `/docs/**`; the parent must force-add this handoff document
if it should be committed.
