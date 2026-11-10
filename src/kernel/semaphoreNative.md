# Checked native kernel semaphores

`semaphoreNative.inc` implements all eight `sceKernel*Sema` APIs. The separate
16-byte POSIX semaphore implementation and its pthread wrappers remain separate.
The native audit is `semaphore-native-stage/NATIVE-CONTRACT.md`, SHA256
`035898d70a56ce14081b3202266d68e8cfc42e5ce62844012d2a6859f610559b`.

## ABI and ownership

Handles are eight-byte integer carriers. Lookup consumes low 32 bits and validates
a 13-bit index, three-bit generation and entry type `0x120`. Index zero is reserved
by this HLE, leaving 8191 slots. Generations wrap after eight retirements; this is
not unbounded stale-handle protection. No handle is dereferenced for diagnostics.

Open takes `(out, name)` and opens an existing openable semaphore, allocating a
distinct alias. Create's `0x100` attribute publishes a name; duplicate openable
names return EPERM. Missing/nonopenable names return ESRCH. Delete accepts creator
IDs, Close accepts aliases; the opposite role returns EPERM. Each slot and active
operation retains its object. Reusing a numeric slot cannot redirect an already
retained operation.

This is a **global, semaphore-only HLE registry**, not a native per-process shared
kernel-object table. Filesystem contexts are not used as invented credentials.
Foreign numeric handles colliding with a live semaphore ID cannot be distinguished
by this table. Test-only type mutation verifies local type checking, not complete
cross-family isolation. Process exit/capability semantics remain unmodeled.

## Deletion, waits and suspension

Creator deletion tombstones the object, completes queued waiters with EACCES and
keeps the creator ID/name reserved until all enrolled waiters retire. During drain,
Open may alias the old tombstone; duplicate Create still fails. Final removal
revalidates slot/object/role under the registry lock. Competing removers retain the
original object and wait outside locks; after its removal they return success
without touching a replacement, even across generation wrap. Exact native timing
of competing removers is unaudited. Host drain failure returns EIO and leaves the
tombstoned creator reservation retryable. Final alias Close retires the alias
before drain, matching the observed native ordering.

Deleted aliases retain distinct native behaviors: Poll tests/decrements the old
count; Wait returns EACCES; positive Signal succeeds without changing count;
Cancel succeeds with zero waiters. New same-name objects never rebind old aliases.

Every registry/object lock has a preceding host critical guard that outlives its
unlock. The only nested order is registry → object. Stack-owned intrusive waiters
need no allocation after enrollment. Completion fixes the result and removes the
queued node; retirement decrements the active count before its stack disappears.
Cancel reports active nodes, including completed-but-unretired waiters.

Windows uses `RtlWaitOnAddress`/`RtlWakeAddressAll`; Linux uses private futexes.
Epoch predicates close the unlock-to-park window. All parking and waking happen
outside semaphore locks/critical guards with retained ownership. Ten-millisecond
park slices bound signal polling. Missing host adapters return EOPNOTSUPP. Linux
execution and macOS support are not established.

Cooperative EINTR handling retires the waiter before dispatch, releases its object
reference, then retries ID lookup. A handler may delete the waited semaphore.
Existing asynchronous special-APC behavior needs separate runtime validation.
Forced termination/nonlocal jumps across HLE frames are outside normal C++ RAII
cleanup guarantees. Static teardown requires quiesced guest threads; there is no
destructive reset or invented guest-process cleanup.

## Validation and guest memory

Create validates attr mask `0x103`, order/count/name/options, normalizing order zero
to FIFO. Names are read through the first NUL, at most 32 bytes. Unreadable names
return HLE EFAULT and unterminated names ENAMETOOLONG. Need/count positivity precedes
lookup for Wait/Poll/Signal. Wait copies its four-byte timeout before lookup.
Signal checks overflow before mutation. FIFO/priority queues do not skip an
unsatisfied head. Negative Cancel counts restore the initial count.

Create/Open output preflight runs outside locks but does not override native
operation errors: absent Open → ESRCH, duplicate Create → EPERM when capacity
exists, exhausted allocation → EAGAIN. Eight-byte output is the final fallible
publication action under the registry lock. Failure rolls back the exact slot;
an outside reference keeps destruction out of the lock. Allocation failures
return ENOMEM. Timeout/Cancel outputs are copied after object unlock. Cancellation
is not undone by an output fault; existing wait errors take precedence over timeout
copyout failure.

Windows copies use permission checks and ReadProcessMemory/NtWriteVirtualMemory;
Linux uses bounded map preflight and process_vm_readv/writev. These contain ordinary
bad pointers, but do not pin mappings against concurrent protect/unmap. A failed
copy may partially modify output under such races. HLE EFAULT on otherwise-successful
Create/Open is a containment policy, not an audited native wrapper fault policy.

## Verification

`KernelSemaphoreTests.cpp` exercises registered NIDs, roles, generation/type checks,
error precedence, transactional rollback, retained operations, deletion/name
exclusion, cancellation, alias Close during creator drain, and injected drain
failure followed by competing-remover retry. `KernelSemaphoreGuestTests.cpp` adds real guest
pthread suspend/resume and signal-handler deletion. Test hooks run outside locks
and guards. Actual signal dispatch host-thread retirement is awaited before cleanup.
These fixtures must be built with `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS` on both
tests and emulator sources. Both Windows CTest selectors passed after initializing
the guest memory subsystem before guest pthread creation. The observed signal
handler ran on a helper host thread; its actual retirement was awaited. This run
does not independently establish cooperative-only signal dispatch or Linux behavior.

Safe `PollSema(-1, 1)` → ESRCH restores an error path; it does not establish that
ShellCore initialized its suspend-block object or that setup/home works.
