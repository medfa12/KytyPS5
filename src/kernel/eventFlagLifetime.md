# Event-flag lifetime repair: bounded HLE policy

This replaces the broken delete path with a retained ownership registry. The
eight-byte pointer-shaped carrier is an opaque HLE token, never a host pointer.
Tokens use monotonically increasing low-32-bit values 0x10000..0x7fffffff and
never wrap or recycle; exhaustion returns EAGAIN. Lookups use the low 32 bits,
as observed for native Wait. This range is disjoint from the checked
semaphore implementation's <=0xffff IDs, but **is not a shared native kernel ID
table**, and there is no claim of complete native cross-type/process semantics.

The 13.60 static native audit in `eventflag-native-name-stage/NATIVE-NAME-CONTRACT.md`
establishes Open(out,name), 8-byte success output, unary Close and distinct creator/
alias IDs. Attribute bit 0x100 alone publishes an event by name; private events,
including attr 0x11, can independently share a diagnostic name. Duplicate published
name returns EPERM; missing/private-only Open returns ESRCH. Successful Open creates
a distinct nonreused alias token retaining the same object. Delete accepts creator
only; Close accepts alias only, and wrong-role operations return EPERM. Stale IDs
return ESRCH. Delete unlinks only its matching public creator name; aliases keep
the old terminal object until their own Close and cannot target a replacement.
The HLE table is global rather than the native per-process handle table: no process
isolation or full native shared object-table parity is claimed.

Registry and name publication are one mutex transaction. Delete commits terminal
state under registry -> object lock before unlinking; wake and final destruction
are outside both locks. Node allocations and
object construction precede that lock. The resolved permission-respecting
8-byte output operation occurs inside the transaction; failed copyout extracts
the installed handle and, only for a named Create, the matching name before unlock,
retaining ownership outside. Open preallocates its alias node, inserts an alias
under the table lock, and extracts that exact alias on failed copyout. Tokens consumed by a
failed copy are not reused. Windows preflights page protections then calls
NtWriteVirtualMemory; Linux calls process_vm_writev. Partial user bytes on a
cross-page/protection race cannot be rolled back; no byte-atomic promise is made.
Guest names use a 32-byte bounded safe copy (at most 31 bytes plus NUL) for
private, published and Open requests. The later native retirement audit witnesses
NUL at byte 31 as valid, exhaustion as errno 63 (ENAMETOOLONG), and a copy fault as
errno 14 (EFAULT); it does not establish all competing-invalid-argument precedence. Create
validates mask 0x133, rejects mutually exclusive order/mode bits, and normalizes
zero order/mode to the existing FIFO/single behavior. This HLE checks duplicate
public names before token exhaustion; native ID allocation precedes registration,
so exact combined-error precedence at exhaustion is not claimed. Wait
timeout, result and Cancel count are checked using fault-safe host copies. The
preflight is not a memory-map lease; later concurrent remapping can cause partial
copyout or a post-operation EFAULT and is not byte-atomic. Generic pointer-fault
behavior here is HLE policy, not a claim of complete native copyout parity.

All operation lookups retain a shared object owner before any name or object
access. Delete explicitly marks terminal state and wakes independently of the
last shared owner; object destruction runs only after all retained calls/aliases
are gone. The present Delete marks/unlinks and returns without draining admitted
waiters. In contrast, `eventflag-native-retirement-stage/RETIREMENT.md` witnesses
first native teardown draining admitted waiters before that invocation removes the
creator ID/name; already-tombstoned teardown skips that drain, and serialization of
competing Deletes was not reconstructed. The HLE behavior is an explicit deviation,
not a native-timing claim. Do not add a blocking drain without a coherent queued-
waiter, EINTR and signal-reentry design.
Wait checks deletion before enrollment and every wake. Cancel increments
a cancellation generation and wakes, rather than draining waiters under a lock;
Delete wins if both are observed. A retained operation that sees Deleted returns
EACCES, except Poll maps Deleted/Canceled to its existing EBUSY mapping. These are
existing **HLE deviations**, not native alias semantics: the 13.60 audit finds
tombstoned alias Wait/Poll EACCES before pattern, Set success without update,
Cancel normally success with count zero, and Clear success while ANDing the pattern.
The audit also finds per-waiter completion claiming and checks Cancel before Delete
if both completion flags exist, unlike this HLE generation-based Delete-wins rule.
Removed creator or alias IDs return ESRCH even while other aliases retain the object.

All object/registry lock ownership has a preceding HostLockCriticalGuard whose
lifetime extends past unlock. Waiting occurs through bounded RtlWaitOnAddress on
Windows or futex on Linux, outside every lock and critical guard; the epoch
comparison closes the unlock/park missed-wakeup window. Ten-millisecond slices
recheck deadlines. After an infinite wait slice times out, the logical enrollment,
waiter count, single-waiter exclusion, retained owner and cancellation generation
remain in place across cooperative pending-signal dispatch; no object/registry lock
or critical guard spans the callback. Event Delete marks/wakes without draining,
so a synchronous signal handler may delete the flag without self-deadlock. A Cancel
during the callback counts the waiter and its generation is observed on return.
Delete wins over cancellation if both are observed afterward, an HLE choice as
noted above. Native libkernel retries EINTR after kernel waiter retirement, but
the signal-handler timing around that retry was not audited; persistent HLE
admission during callback is not claimed as native. Timed waits do not dispatch,
matching the original CondVar WaitFor path. Unsupported hosts reject
Create/Open with EOPNOTSUPP. Timeout/EINTR/spurious wake are not hard failures.
Before returning a saved host-park EIO or dispatcher ENOMEM, the waiter rechecks
under its object mutex: committed Delete -> EACCES, then committed Cancel ->
ECANCELED, then the saved infrastructure error. The saved error takes priority over
newly satisfied bits and timeout. Terminal results use normal result/timeout copyout
and waiter retirement; a late copyout can still fail with EFAULT. Guest-handler
effects before an exception are not rolled back. Test hooks
run only after retention/enrollment/cancel/retire publication, outside guards/locks.

`SetEventFlagByName` retains while looking up, replacing the raw-pointer lookup
used by the LibC internal login code. It searches published events only, returns
ESRCH when absent, and does not create a guest Open reference.
The watchdog label is the static string `event-flag`, avoiding an asynchronously
borrowed object-name pointer; synchronous census/trace output uses retained names.
The opt-in `KYTY_EVENTFLAG_TRACE` matching and normal Create log remain.
Create preserves the supplied initial pattern for every name. The former boot/power
name-specific ORs were not a kernel-owned lifecycle and have been removed; real
service readiness must come from its owning producer. Census log fields are simplified; original per-thread
name/timeout and very-low-frequency counter diagnostics are not all retained.
No readiness bits are synthesized by Create.

`tests/KernelEventFlagLifetimeTests.cpp` exports TestKernelEventFlagLifetime().
It uses actual libkernel NIDs, observer-compatible Create signature, floor Close,
actual destructor counter, bounded 5-second gates and a 90-second fixture watchdog.
It covers concurrent/repeated delete, all five retained operation classes,
enrolled-before-park deletion, cancel/delete, stale/invalid Poll, duplicate name,
private/private repeated ExitSpawnEvf attr=0x11 with independent bits, mixed
private/openable creation in both orders, separate alias IDs/role errors, 32-byte
name boundary, final-Store rollback across private/public names, concurrent named
publication and retained-by-name old/replacement separation. Generic lifecycle
fixtures remain private; name-dependent cases explicitly opt into public creation.
A transition gate checks old
operations and the replacement after terminal commit/unlink; injection exercises
counter preparation, diagnostic allocation, host park error and a throwing test
hook. Additional gated cases commit Cancel/Delete after injected Park failure and
inject a dispatcher-boundary callback that mutates then throws std::bad_alloc.
Those callbacks simulate dispatcher-side effect ordering, not actual guest-handler
delivery; the separate guest fixture tests a real handler. The unhooked waiter test
waits until guest enrollment and checks bounded
completion after deletion, but neither that nor the Enrolled gate proves the OS
thread was already parked at deletion. The Windows witness additionally samples
the identified worker in Waiting/WrAlertByThreadId before Delete and checks EACCES
completion and destruction. Its PID/TID/creation-time-checked snapshot is not atomic
with Delete and does not identify a wait address or prove that Delete caused the wake;
the ten-millisecond timeout can also trigger a recheck. Runner timeout >=120 seconds; run in
the full emulator fixture with Common thread/config/log initialized, before Vulkan,
and define KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS for every relevant source.

`tests/KernelEventFlagGuestTests.cpp` additionally defines the Windows guest-pthread
signal fixture `TestKernelEventFlagGuestThreads()`. Integrate only after **Memory**,
guest main-pthread and linker initialization; PthreadCreate requires Memory. One
case queues a pending signal directly with PthreadQueuePendingSignal while the
worker is gated at the recorded pre-dispatch boundary; it does not call PthreadKill
or request an APC, requires target-host-thread execution, and the real guest handler
deletes the flag at zero critical depth. A separate case uses actual PthreadKill;
APC/helper execution is possible and is reported rather than assumed. Both wait
for the dispatch host handle to retire and restore the prior handler after join.
No test-only production signal routing modification is made.

The preceding lifetime implementation passed independent review, Windows builds,
event/observer tests, cooperative and APC signal fixtures and the Windows wait-state
witness. The name/alias correction also passed independent review, Windows builds,
both event fixtures, both semaphore fixtures and enabled/disabled observer cleanup.
Focused tests alone do not establish Linux behavior, sanitizer coverage, full native
kernel parity or a successful ShellUI boot.
