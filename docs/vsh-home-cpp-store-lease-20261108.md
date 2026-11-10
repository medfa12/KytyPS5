# C++ store lease lifecycle checkpoint

Implemented an **unwired** `Spawn::canonical_store_lease` mode in the actual
RuntimeChild process/client code. It acquires the canonical root's existing
share-zero `.runtime-lock`, duplicates that file object into the exact five-handle
STARTUPINFOEX whitelist, and passes its volume/file identity in child arguments.
Legacy launches still use the four-handle whitelist and parent-held lock.

The move-only StoreLease rejects reparse/hardlinked lock files; it never deletes
the marker or closes/reopens for handoff. The child validates inherited identity,
clears inheritance, and compares the final lock pathname's directory to the
actual `/data` mount using filesystem identity. It sends LeaseAck after that
validation; the parent rejects entry without ACK. The parent deliberately keeps
its own lease even after ACK until the original process is signaled and the job
is empty. The child's copy is intentionally OS-process-lifetime: this codebase
does not have a guest-thread/file shutdown join fence, so static destruction
would be an unsafe early release. Startup failures retain parent ownership until
termination observation. Before CreateProcess, RAII closes all acquired handles.

Scope: dedicated title `/data`, not ShellUI canonical profile ownership. Emulator
startup refuses the opt-in mode for VSH or additional/overlapping extra mounts.
`runtimeLaunch.cpp` and `loadExec.cpp` refuse alternate launches from a leased
child. BuildManifest never sets the mode. Standalone launches, original-store
tools, and other filesystem clients do not yet participate. This is **not** an
all-writers invariant and does not authorize promotion/bootstrap activation.

## Verified

Isolated MSVC C++20 build, at most two jobs, and CTest passed (1.04s). It compiles
the production Process, Client, Protocol, Input and Frames sources. Disposable
subprocess controls cover:

* move ownership, duplicate retaining exclusion after original close;
* canonical `.`/`..` aliases colliding, independent roots proceeding;
* before-create, job-assignment and resume failures;
* production client adoption, noninheritability, root validation, ACK then entry;
* wrong inherited identity and wrong mount root;
* missing ACK/startup timeout, normal child exit and actual retirement;
* abrupt disposable parent death before and after ACK, job-killed child copies
  draining before the root becomes available.

Only freshly created test trees/processes were accessed. Sentinel data survived;
successful fixtures remove their own tree. No emulator or original stores used.

```
cmake -S tests/store_lease -B _Build/store-lease-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _Build/store-lease-tests -j2
ctest --test-dir _Build/store-lease-tests --output-on-failure
```

## Remaining integration limits

Full emulator compile of the startup/mount and alternate-launch guards is pending
parent integration. Junction aliases and adversarial directory replacement are
not covered by these controls. Directory rename protection needs a pinned root
handle before making a global mutation invariant. Parent death in the narrow
CreateProcess-before-job-assignment interval can leave a suspended child owning
the inherited handle: exclusion is retained (fail closed), but orphan cleanup is
not solved. The parent-death controls cover the actual assigned-job lifecycle.
The protocol is trusted host IPC, not protection against a malicious handle
substitution by the launcher. Additional mount writers, same-session user
creation, ShellUI root routing and guest shutdown ownership still require work.
SQLite promotion and roster bootstrap remain unwired.
