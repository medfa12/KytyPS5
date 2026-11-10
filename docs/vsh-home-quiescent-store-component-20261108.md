# Unwired quiescent store component

`tools/home_quiescent_store.py` implements the first executable ownership and
promotion pieces. It has no activation CLI and is not called by the launcher.
Tests use only newly created synthetic stores and ordinary Python children.

## Existing lifecycle comparison

`src/loader/runtimeChildProcess.cpp:79-82` already opens `.runtime-lock` with
CreateFileW share mode0. Its handle is held by the parent Process object, outside
the four-handle inheritance list at line108. The job is kill-on-close and the
destructor waits for child exit before releasing the lock. This component uses
the same lock filename/share policy, canonicalizes the store root, and adds an
explicit DuplicateHandle/child-adopt path. It does not modify that C++ lifecycle.
The child verifies the inherited handle's final path against its canonical store;
it does not reopen the lock during handoff. The parent keeps ownership until
acknowledgment. The test proves the child retains exclusion after the parent's
handle is closed, with an explicit STARTUPINFOEX inheritance whitelist.

## Promotion strategy change from the initial design

Rather than invent a multi-file rename protocol, promotion uses SQLite's backup
API to replace the existing authoritative DB **inside SQLite's destination write
transaction**. SQLite owns the rollback-journal/WAL crash protocol and target
file identity remains unchanged. The FULL synchronization setting is selected
on the destination. No sidecar is deleted or renamed.

Before replacing contents, the component makes and flushes a separate retained
SQLite backup of committed target state, including any WAL content. It refuses
to overwrite a prior rollback artifact. The candidate must match the caller's
hash, pass integrity_check, be standalone without sidecars, and lie within the
leased store. The caller must separately validate real roster, catalog source
provenance and semantics; integrity_check is not that validation. Cancellation
aborts the backup write transaction. Unexpected SQLite contention fails promptly
rather than retrying against a potentially active writer.

On process interruption, reacquire ownership and open the target through SQLite
to perform its normal recovery; do not raw-copy or delete journals. Reopening
twice after an injected process death produced the original intact generation.
Failure after SQLite has committed can leave the new generation: a Python
exception is not itself proof of rollback. The retained old snapshot must not be
automatically restored merely because no success receipt reached the parent.

## Verification and remaining integration

`python -B tools/test_home_quiescent_store.py -v` passes nine controls: canonical
path collision/distinct store, real child handoff, wrong inherited store,
successful replacement/retained old contents, mid-backup cancellation, actual
child process death/repeated recovery, WAL-preserving old snapshot, unexpected
writer contention, and wrong hash/closed lease/candidate-sidecar rejection.
The first test run exposed test-fixture SQLite handles not being closed; the
fixtures now explicitly close connections and all cleanup succeeds.

These are process-crash tests, not power-loss/storage-controller fault tests.
Publication of a separate bootstrap journal/receipt and its directory durability
is not implemented; no claim of durable cross-file receipt atomicity is made.
The SQLite transaction avoids needing that receipt for target crash consistency.

Before activation, every launch route that mounts the canonical store must hold
the same lease throughout guest lifetime. Old/uncoordinated runtimes remain
outside this protocol, and the guest's fake fcntl locks are still unchanged.
The C++ launch integration, validated roster/source snapshot coupling, and
same-session newly created user behavior remain unwired. This component advances
ownership/handoff and promotion testing; it does not claim a usable Home or
PSN login, and must not be used against the preserved trial stores.
