# Normal Home/VSH checkpoint (2026-11-05)

Scope: normal Home startup, followed by tile-driven app launch, presentation,
Home/resume, close and relaunch. OOBE is outside the current objective.

## Verified baseline

- Local checkpoints: `9850cab6` native AppDb integration, `87ba3319` logging
  include/build correction, `a9f32532` matching font staging/mount preflight.
- Integrated emulator built in `_Build/vsh-integrated`; 13 IPMI, four runtime
  lifecycle and four retained-input admission/loader checks passed.
- `avsh2` admitted alternate Core/UI and executed the native Core AppDb
  connection callback at RVA `0xa92940`, returning zero. Native
  `SceAppBrowseEventFlag` creation preceded the connection callback.
- This is connection acceptance, not functional Home. The captured window was
  black at frame 162; managed startup failed in AutoMounter with `0x80020003`.
  The development process (PID 14212) subsequently exited.
- Evidence: `_Runs/avsh2/_kyty.txt`, `_Logs/avsh2.log`, and
  `%LOCALAPPDATA%/Temp/opencode/avsh2-first-frame.png`.

## Remaining startup evidence

- `SceAutoMounterEHddMountBlock` and `SceAppDbSyncManagement` are unavailable.
  Native service initialization/teardown is under separate investigation.
- The sandbox app.db path resolves correctly to `_Runs/a1360sd/priv/mms`.
  Native AppDb reports `ver_check_result=0x40` and writes a corruption marker.
  The offline provisioning script explicitly supplies managed-reader tables;
  it is not an implementation of the native database schema/version contract.
- Notification database creation writes and flushes its database and journal,
  deletes the journal (line 48272), then opens/closes the parent directory
  (48273–48287). COMMIT reports SQLite IOERR at line 49249.
- Parent-directory fsync explicitly fails with EIO at line 46743. Windows
  directory descriptors currently retain FILE_READ_ATTRIBUTES-only handles;
  FlushFileBuffers requires a write-capable handle.
- Isolated host probes: directory CreateFileW with GENERIC_WRITE and
  FILE_FLAG_BACKUP_SEMANTICS successfully flushes. Attributes/read-only handles
  fail with access denied. ReOpenFile cannot upgrade the retained directory
  metadata handle (also access denied), so pathname-based reopening at fsync
  would not be a safe descriptor implementation after rename/replacement.

## Next filesystem batch

Introduce directory-specific host opens retaining a flush-capable handle where
host permissions permit. Preserve metadata-only fallback when write access is
unavailable; fsync must report a genuine host failure in that case. Flush the
retained object, including after rename/path replacement. On POSIX hosts retain
an actual directory descriptor rather than O_PATH for guest directory opens.
Test a journal create/write/flush/unlink/parent-flush sequence, retained identity
after rename, and invalid/closed descriptor behavior before the next Home run.

No database version overrides, synthetic event flags or fabricated native
schema acceptance are part of this batch. Native schema compatibility and app
lifecycle E2E remain unverified. Historical and direct-YouTube builds remain
preserved; all commits are local.

## Exact native schema rejection

Read-only inspection after process exit found `integrity_check=ok` for app.db,
bgft.db and notification2.db. App.db has only the four managed-reader tables;
bgft.db only `tbl_summary`. Notification2.db already contains the native
`notification` table and limit trigger despite its failed COMMIT result.

Disassembly addresses below are RVAs in the original **alternate** UI image
(`f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`):

- `InitClient +0x54e650` calls `DbAccessor::Init +0x55abf0` at `+0x54e75f`,
  passing a null schema-creation callback in `r8`.
- Version reader `+0x55c260` queries `sqlite_master` for `tbl_version`
  (`+0x55c2b4`), then reads `SELECT category, status FROM tbl_version`
  (`+0x55c3b6`) if present.
- The version tuple starts at -1. `checkDbVersion +0x55ba00` tests its
  appbrowse schema value at `+0x55ba87`; a nonpositive value takes `+0x55bac7`
  and stores flag `0x40`.
- `DbAccessor::Init` observes that flag at `+0x55ad25`, closes the connection
  and skips schema creation because the callback is null (`+0x55ae05`).

Thus a healthy SQLite file is insufficient: native schema initialization must
come from its real creator. Adding a version row to the managed-reader seed
would hide the first check while leaving an incompatible native database.

## Filesystem implementation validation

Directory opens now retain a real flush-capable host directory handle, with
metadata-only fallback on Windows when host write access is denied. No pathname
reopen occurs at fsync; host flush failure remains failure. Archive directory
identity uses its existing retained archive path.

Built `kernel_file_system_tests` in `_Build/vsh-integrated` with parallelism 4.
All four targeted checks passed: `filesystem_sync`, `filesystem_identity`,
`guest_fs_context`, `posix_vector_time`. The sync fixture covers journal write,
file flush, journal deletion, parent flush, rename with regular-file replacement,
retained directory identity, and invalid/closed descriptors. It also confirms
that a metadata-only Windows handle still reports a real flush failure.
The POSIX implementation was updated but not executed on this Windows host.
Native SQLite COMMIT and normal Home remain to be checked in a subsequent run.

## Normal Home diagnostic continuation (2026-11-06)

The leading native startup explanation is a SaveData initialization-failure
cleanup stalled joining `SceSaveDataWatchDog`, not proven global Core teardown.
PsnCore logged `sceNpManagerVshInit()=0x80559b23` after unresolved NpCommon
scalar-allocation import `V75N47uYdQc`. Disassembly places the Syndy shared-service
creator after that failure branch. A null service later produces `0x8055db02`
in Syndy context initialization; that exact runtime return is not yet captured.

Opt-in `KYTY_NATIVE_STARTUP_DIAGNOSTICS=1` adds bounded pre-join frame-chain
logging and fault-contained, read-only Syndy service/member snapshots for a
live admitted alternate Core caller. Expected initialization cleanup return
PC: Core `+0x1d6b45`; normal teardown instead returns at `+0x1d7a7f`.
No join result, native service state or admitted executable bytes are changed.

Next data root `_Runs/a1360native1` is a verified clone of all 18 ordinary files
from `a1360sd`, preserving its profiles/settings. Copied stale IPC sockets were
discarded. The clone's entire old `priv/mms` was moved outside guest mounts to
`_Runs/a1360native1-db-baseline`; a fresh empty `priv/mms/nobackup/sqlite` now
exists for genuine native creation. Original `a1360sd` is preserved. Use
`-NoDatabases` to avoid reintroducing the managed-reader schema.

Native NP flags remain disabled for the diagnostic run: alternate NpCommon
SHA256 `81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35`
does not match the existing startup pin. Matching-provider dependency/relocation
review is ongoing; no alternate admission change has been made.

### AppDb sync-management producer

Alternate Core's singleton at `+0xfc3ce0` constructs the
`SceAppDbSyncManagement` name (SIMD string load `+0xfc3d43`). Its initializer
`+0xfc3e70` creates a named event with attr `0x110`, initial bits zero at
`+0xfc3eac`, then clears it. Caller `+0xfd14e4` invokes this initializer only
after AppInfoDaemon initialization (`+0xf8ef40`) and another prerequisite
(`+0x1004910`) succeed; the database accessor initialization follows later at
`+0xfd1549`. Thus missing sync-management publication is not explained solely
by the managed app.db schema rejection. The `avsh2` log contains no matching
native AppInfo initialization diagnostics; its separate HLE AppInfo client
connection does not establish native producer startup.

## `avsh3` live diagnostic result

Integrated emulator and runtime tests built successfully; both
`runtime_guest_execution` and `runtime_process_identity` passed. Diagnostic
source checkpoint: `248cf936` (build version stamp reflects its parent plus
uncommitted diagnostic source). Normal Home/VSH launched as PID 9672 with
the fresh native database area and all native NP feature switches disabled.

`_Runs/avsh3/_kyty.txt` records:

- Actual native AppDb connection callback returned zero (44919).
- `sceNpManagerVshInit()` again failed with `0x80559b23` (45417).
- Unmatched join target is now confirmed as `SceSaveDataWatchDog`, caller
  Core `+0xf35c24` (59805). The frame-link trace truncated at an unknown stack
  address (59806), so it does not prove the exact cleanup branch.
- The admitted-Core snapshot read succeeded and found the Syndy service
  pointer null (59807). This corroborates the missing-service explanation;
  it is still not a captured return of `0x8055db02`.
- Native notification database journal writes, deletion and parent-directory
  fsync now succeed, including the post-unlink flush at 46829. The prior
  COMMIT disk-I/O error and `syncParentDir` error do not recur. Notification
  initialization proceeds to a later NP sign-in-event error `0x80550002`.
- AppDb reader reports `ver_check_result=0x100` with no app.db seed (45470).
  Native AppInfo/AppBrowse database production is still unavailable.

Home remains black (frame 82) and the managed AutoMounter request still fails
with `0x80020003`. Screenshot:
`%LOCALAPPDATA%/Temp/opencode/avsh3-observation.png`. After evidence capture,
the failed development process closed through its normal window-close request.
Read-only host inspection then confirmed notification2.db integrity is `ok`
and the native notification table/limit trigger are present.

Next prerequisite: separately select the exact audited alternate NpCommon
ownership/queue/callout/HTTP-pool family. Preserve the original pin and exclude
dispatcher56; static equivalence does not establish runtime worker/callback
success or PSN functionality.

### Prepared alternate-NP run

Runner opt-in `-NativeNpAlternateStartup` selects host
`KYTY_NATIVE_NP_ALTERNATE_STARTUP=1` plus the existing ownership/workqueue/
callout/HTTP-pool flags. It requires the alternate launcher profile and the
exact reviewed alternate NpCommon hash. Original startup and dispatcher
combinations are rejected; the original startup hash check remains unchanged.
The runtime loader must independently enforce the selection before execution.

PowerShell parsing and three invalid-selection cases passed. Prepare-only
`avsh4` verifies one matching NpCommon co-ELF, all four family flags, no
dispatcher/first-boot flag and `--vsh-session`. It uses `_Runs/a1360native2`,
another verified 18-file clone preserving profiles/settings, with old mms
archived outside mounts as `_Runs/a1360native2-db-baseline`. No database seed
was introduced and no `avsh4` process was launched during preparation.

### Independent review follow-up

Astra Medium's independent read-only review found no blocking correctness
defect in the committed directory-flush/join-diagnostic changes and judged the
alternate54 experiment justified, conditional on finished code and checks.
It requested a positive original-provider load/mapping regression alongside
alternate admission and rejection tests; that gate was handed to the runtime
implementation worker. Static import coverage is not native startup success.

The review found that `-NoDatabases` did not suppress a legacy notification DB
copy when a hardcoded historical source exists. The copy now obeys the switch.
PowerShell parsing passed, and executing the actual guard with mocked file
operations verified no copy with the switch and preserved legacy behavior
without it. The historical source was absent during avsh3, so that run's fresh
native database-creation evidence remains valid.

Lifecycle findings (resource-retirement acknowledgment and overlay/background
state) require separate backend-scoped review before app lifecycle sign-off;
they do not establish an additional blocker for this Home-startup diagnostic.

Final backend-scoped review corrections:

- The resource-retirement finding applies to the **shared rollback** backend.
  The active Windows subprocess path waits for actual process/job retirement
  before releasing admission and publishing keyed completion. No equivalent
  premature-completion defect was found there.
- Subprocess focus commands acknowledge visibility/audio/input changes but do
  not update child SystemService background/overlay status. Shared-backend
  overlay/background conflation and subprocess status propagation are distinct
  lifecycle follow-ups.
- A new source-confirmed interaction must be fixed **before tile testing**:
  `runtimeChildProcess.cpp` passes a null environment to CreateProcessW, so
  children inherit the alternate NP startup and four family flags. Their
  manifest does not co-load NpCommon; the new provider/startup consistency check
  would reject them before guest entry. Use an explicit Unicode child environment
  removing the six host-only alternate/ownership/workqueue/callout/HTTP-pool/
  dispatcher variables, preserving unrelated entries and the parent's environment.
  Removing only alternate selection is insufficient because family flags also
  control native-header-aware mutex destruction. Verify with an actual spawned
  probe, not just an environment-filter unit test.

The final NP snapshot review found no additional immediate **Home-host** blocker;
its positive original-provider mapping coverage and documented private-provider
boundary address the initial provisional finding. Home-host acceptance remains
conditional on the parent build/checks and real native startup evidence.

### Completed alternate-NP validation

The frozen implementation built successfully in `_Build/vsh-integrated`
(`kyty_emulator`, `virtual_memory_allocation_tests`, `symbol_database_tests`).
All eight targeted CTest checks passed: `symbol_database`, `np_allocator`,
`np_mutex`, `np_heap`, `np_atomic`, `np_native_worker_http`,
`runtime_guest_execution`, and `runtime_process_identity`.
The separate `--np-alternate-startup-only` run also passed with both reviewed
real images, including positive original/alternate admission and mapped-byte
checks. Its log is `_Build/vsh-integrated/np-alternate-startup-check.log`.
This establishes the reviewed loader/component gate; native Home startup and
per-title lifecycle remain to be exercised. Historical/direct builds were preserved.

### avsh4 — alternate54 live result

Validated source was committed locally as `1afb8ba5` before launch. The executable
was built immediately before that commit, so its embedded label is still
`182a1c2-dirty`. Ran the prepared normal VSH configuration with matching Core/UI
auth records, alternate54, startup diagnostics, and `-NoDatabases` against
`a1360native2`; preflight found no DB/SQLite files. PID was 17164.

- Native scalar allocator `V75N47uYdQc` now binds to `0xa900185d0` (log line6677).
  A real `SceNpPushCallout` worker starts at native `0xa90006150` (49029).
- PsnCore advances to a new failure: `sceNpManagerIpcServerInit()` reports
  `IPMI::Server::create` failure `0x8002004e` (49113–49117), rather than the
  earlier VshInit allocator failure. This is not successful NP initialization.
- AppBrowse still reports `ver_check_result=0x100` (48680); sync-management
  event is missing (169280), AutoMounter EHdd event is missing (197445), and
  the managed fatal exception remains `0x80020003` (217851).
- No SaveDataWatchDog join/Syndy diagnostic appeared in this run's log search;
  their initialization/cleanup state remains unproven.
- Window capture at frame252 is black apart from a thin colored top edge:
  `${USERPROFILE}/AppData/Local/Temp/opencode/avsh4-window.png`.
  The earlier `avsh4-observation.png` screen-region capture was occluded by
  another application and is not emulator presentation evidence.
- Native notification2.db (12288 bytes) and HTTP cache.db (36864 bytes) exist;
  read-only SQLite integrity checks return `ok`. `priv/mms/app.db` is absent.
- Closed the diagnostic through its own window and observed process exit;
  no additional startup attempt was made. Logs remain in `_Runs/avsh4` and
  `_Logs/avsh4.log`. Home and tile-driven lifecycle are still not established.

Astra Medium is tracing the newly exposed IPC server creation failure read-only.
Child environment isolation now supplies an explicit Unicode environment block
to `CreateProcessW`, removing exactly the six host-only NP switches with
case-insensitive name matching while retaining other entries and drive aliases.
The integrated emulator build passed. Actual spawned lifecycle probes verify
removal of all six switches, mixed-case matching, preservation of an unrelated
value and a similarly prefixed Unicode value, and unchanged parent environment.
`vsh_subprocess_lifecycle`, `vsh_subprocess_input`, and
`vsh_subprocess_embedded_vulkan` all passed. This fixes the source-confirmed
inherited-selection blocker; genuine tile startup still awaits navigable Home.

### Next NP dependency batch — admission remains disabled

Read-only investigation identifies avsh4's `0x8002004e` as consistent with
`NpNativeTransport::Current()` finding no installed runtime. The actual factory
binding routes here; no production `Install` call existed. Installing hooks alone
would expose success-only dispatcher create/join bindings, so that is not an
adequate startup fix.

Fresh alternate-image disassembly is recorded in
`_TempData/avsh4-alternate-np-admission-report-20261106.md` and its three evidence
files. Verified alternate Core RVAs: factory `0xcaa896`, dispatcher create
`0xcaa90f`, entry `0xcaaa80`, join `0xcaab26`. NpCommon pair bodies at
`0x6c60/0x6d80` and their direct reference closure match the original; create's
name is its seventh argument. The same Core pair imports also start other
workers: pair-only global admission would activate missing event lifecycles.
Seven event exports are a demonstrated lower bound, not a complete new policy.
The audit also finds post-factory failure cleanup that frees server storage
without a destructor and normal teardown that ignores stop/join/destructor
errors. These native lifetime contracts require explicit integration.

The working tree adds an **unwired** source-bound NP runtime hook seam and
production-hook registration/copy-fault/retirement checks. Independent review
confirmed the guest PID/TID, admitted credential and callback policies, with
mapping retention still conditional on future loader lifecycle integration.
Two test compilation issues found by review (nested header inclusion and the
changed test-function pointer type) were corrected before building. A separate
real-image pair fixture is being prepared to execute only the audited functions
without widening production alternate54 selection or starting Core services.

A bounded manager endpoint requires retained host operations sharing the native
transport's Core and a durable actual guest-thread owner. Caller timeout is not
native cancellation: pending connects must drain after genuine accept/reject;
Busy must retain backing and cleanup execution. A worker is implementing that
private retained-operations layer and component fixtures. Endpoint attachment,
notification buffering, callback pumping and lifecycle integration are subsequent
gates; the unavailable-account HLE broker is not a replacement for native acceptance.
No new Home run or broader NP admission has been enabled for this unfinished batch.

The retained-operations implementation is now source-frozen. The standalone
`_Build/vsh-integrated/ipmi-tests` build succeeded and all **14** CTest checks
passed, including `np_retained_operations_tests`, NP native transport/acceptance,
IPMI lifecycle/retirement/server-wait, and AppDb/LoginMgr transport regressions.
Integrated runtime-hook and real-image pair checks plus independent review are
still pending; these standalone fixtures use host callback handlers.

Expanded static closure is retained in
`_TempData/avsh4-alternate-np-closure-addendum-20261106.md`: all six Core create
sites share the pair, so Core-importer scoping cannot isolate IPC startup. It
enumerates 42 Core-imported synchronization exports plus one registry query;
26 are unresolved in avsh4. Event/mutex/semaphore representation and lifecycle
must be selected coherently before initialization. The recorded fake creates
actually followed unresolved event initialization in avsh4.

The matching Ipmi `destroy()` can return Busy; its C++ destructor instead
asserts on a live handle. Native Core ignores teardown statuses and can also
bypass destroy on post-factory failure. Required backing retention must cover
block reuse and heap unmapping with generation-bound leases, and must drain
callbacks before Core resets handler/session-manager state. Retaining bytes
alone is insufficient.

Final retained-layer review found no concrete supported-path blocker and
confirmed NP-only guards preserve the AppDb/Login specializations. It identified
a native-pair fixture failure-path issue: throwing assertions before unconditional
release/join could outlive stack-local state. That fixture needs correction before
its results can be accepted as dependable failure-path evidence.

### Completed disabled-infrastructure validation (2026-11-06)

The integrated emulator and VM-test target built successfully. Both
`runtime_guest_execution` and `runtime_process_identity` passed. The real-input
`--source-bound-np-transport-only` check exited zero with the reviewed alternate
Core and matching retained auth record.

The native-pair fixture now has an unconditional retirement guard: it releases
the worker and joins through production kernel cleanup before stack state or
provider mappings can be released, including assertion unwinding. Cleanup
failure aborts rather than continuing with live references. After this fix,
`virtual_memory_allocation_tests` rebuilt and both
`--np-native-dispatcher-pair-only` and `--source-bound-core-identity-only`
exited zero with the exact reviewed real inputs. The pair test exercises actual
create/entry/name/argument/join and closed-owner rejection, while asserting
production alternate54 still excludes the pair exports.

Real-input logs are `np-runtime-hooks-check.log`, `np-native-pair-check.log` and
`core-identity-check.log` under `_Build/vsh-integrated`. These checks are silent
on success; the runner's checked process exit codes establish completion.
Together with the 14 standalone IPMI passes, this validates the disabled
infrastructure checkpoint. Production installation, allocator/mapping retention,
synchronization admission, manager endpoint/worker and callback pumping remain
pending. Home and tile lifecycle have not advanced beyond avsh4 evidence.

### Follow-on work after `9475415e`

The disabled NP infrastructure above is committed locally as `9475415e`.
Allocation-lifetime review establishes that allocator-level retention alone
cannot stop Core's subsequent handler/session-manager reset. Generation-bound
allocation leases and whole-heap mapping pins must prevent reuse, unmapping,
fixed replacement and permission removal, but a separate pre-reset drain
boundary is required. That exact native boundary is under static investigation.

A real-image synchronization fixture and exact 42-entry review inventory have
been prepared for event, mutex, condition and semaphore lifecycle checks;
build/execution remain pending. Production admission is still alternate54.

An independent child-status fix is under validation: applied focus acknowledgments
publish the subprocess foreground state; SystemService includes the child's
background state on initial hidden startup, Home, failure and close. Pending
commands do not publish visibility, and controller focus does not imply a visual
overlay. A real pipe/guest-export fixture covers these transitions. Translucent
ShellUI composition still requires an actual overlay producer.

The focused drain-boundary audit is now retained in
`_TempData/avsh4-np-owner-drain-boundaries-20261106.md`. Shared init cleanup begins
at alternate Core `0xcaa9e0`, before frees at `0xcaa9ee/0xcaa9fd` and resets at
`0xcaaa0a/0xcaaa17`, with no intervening destroy/join. More importantly, normal
Stop reaches `0xcaad56` and clears handler+0x10 **before** later IPMI stop/join/
destroy. The latter calls at `0xcaab15/0xcaab26/0xcaab45` all have ignored results.
Internal lifecycle calls are direct, so ordinary imported-export wrappers cannot
guard those resets. Making native nonblocking destroy wait would both change its
contract and still miss the earlier reset.

A vetted owner-control-flow bridge before init unwind and normal Stop remains a
production prerequisite. Normal-termination caller thread and broader teardown
ordering still need proof; queued connections before a failed dispatcher create
also require truthful server-authorized cancellation/drain. No such bridge or
native code transformation has been implemented or enabled by this checkpoint.

### Child status and native synchronization validation

Integrated emulator and fixture builds succeeded. All seven targeted CTest
checks passed: `runtime_child_status` plus its EOF, ACK-write-failure and
window-loss variants; `runtime_application_service_events`;
`vsh_subprocess_lifecycle`; and `vsh_subprocess_embedded_vulkan`.
The child fixture calls the real guest SystemService export through actual
command pipes. It covers initial hidden state, pending versus applied focus,
Home/resume, controller-only focus, failed and superseded acknowledgments,
Close with an unapplied foreground request, EOF, ACK-write failure and terminal
window-loss revocation. Window-bridge loss now explicitly revokes visibility
even before a pipe disconnect is observed.

The exact-pinned alternate-image `--np-native-synchronization-only` fixture
also passed, after review-driven strengthening. It executes native event
constructor/create/open/poll/wait/set/close/destructor, mutex lifecycle and
recursive locking, raw condition timeout/reacquire/signal/broadcast/destroy,
and semaphore timeout/wake/permit/delete. Event and semaphore wakeups require
observed lower-kernel waiter enrollment; condition broadcast wakes two enrolled
waiters. A 20ms requested timeout must actually last at least 10ms. Mutex
contention is established with its real native trylock error before the
blocking-lock observation; that observation remains scheduling-based rather
than a precise mutex-enrollment witness. All workers join before assertions
or mapping release. Final output is `[native] NpNativeSynchronization ok` with
process exit zero in `_Build/vsh-integrated/np-native-sync-check.log`.

The first synchronization runner failed to retrieve `Start-Process.ExitCode`
(blank), not with an observed fixture error. A direct run returned zero;
the strengthened final run used an owned `System.Diagnostics.Process` with a
30-second bound and a captured zero exit code. No timeout or forced termination
occurred. Registry-query execution, deleting destructors, diagnostic/abort paths,
and full service dependencies are outside this fixture's evidence. The manifest
is review inventory only; production alternate54 admission remains unchanged.

### Final cleanup-provenance addendum

Child background propagation and the reviewed synchronization fixture are
committed locally as `6bf3ced3`. Expanded cleanup evidence is retained in
`_TempData/avsh4-np-owner-drain-provenance-addendum-20261106.md`.

The proven caller chain is startup/main -> PsnCore initialization failure ->
`0xc2e4dc -> 0xc2e4f0`. Six unconditional calls and twenty conditional cleanup
stages precede IPC Stop at `0xc2ecfc`; several destroy objects or unregister
callbacks. Consequently, the earlier proposed guard immediately before
`0xcaad56` is too late to protect the broader NP dependencies.

The finite candidate is now a generation-bound quiescence boundary at
`0xc2e4f0` entry, plus the internal IPC-init unwind boundary at `0xcaa9e0`.
The latter must preserve RBX backing, R14D's original failure, stack/canary and
the displaced RIP-relative load. If real stop/join stages are completed earlier,
downstream control flow must explicitly consume those completed stages rather
than simulate successful repeated calls. Failed destruction must not continue
into frees/reset.

Normal termination remains unresolved: table slot `0x20f34c0` stores `0xc2f590`,
but expanded executable/relocation/symbol/literal-reference scans found no
consumer of that field; the decoded table dispatcher invokes initialization
entries only. This is a search limit, not proof that termination cannot occur.

Production admission is still blocked on normal-termination thread provenance,
audited pending-connect cancellation or an independently verified admission
lifecycle avoiding that state, actual callback-unregistration completion,
receiver-bound drain/join ownership, and a continuation mechanism for self-reentry
or incomplete retirement. Existing Busy returns cannot prevent Core's destructive
continuation. No speculative bridge, cancellation, lifetime pin or expanded
startup policy has been enabled. No additional Home attempt was made.

### Allocation retention and bounded endpoint batch (2026-11-06)

The source-bound NP hook now requires actual allocation/backing leases before
publishing native objects or entering dispatcher work. Callback-local session
destruction retains the original object through callback return, and dispatch
lookups compare the native session handle as well as its address. Isolated host
fixtures may omit these hooks only while externally guaranteeing storage lifetime.

The new endpoint coordinator retains copied operations until actual response and
retirement on their original guest owner. Public admission fails fast on lock
contention; completion/deadline observation does not reacquire the coordinator
lock. Unsubscribe invalidates notification generation at submission only, allowing
a queued resubscribe to succeed after an in-flight notification fetch.

All 15 standalone IPMI suites passed after these changes, including held-lock
deadline, pending-fetch unsubscribe/resubscribe, and dispatcher-provider pin
checks. These are adapter fixtures, not native service or Home evidence.

Allocator review found and corrected initial retirement unmapping replacement
generations, permanently stuck retries after partial unmap, and an immediate
retirement ownership-publication gap. The integrated emulator and VM test target
built successfully. Twelve targeted checks passed: allocation leases, pthread
join retention, heap, allocator, IPC memory, source-bound NP transport, process
identity, guest execution, guest threads, native dispatcher pair, native
synchronization, and mmap. The existing IPC-memory fixture caught a Destroy-retry
regression; retry now follows the retained generation instead of losing access
to its mapping record, and both IPC-memory and allocation-lease tests pass.
The exact-image native service fixture executed actual IPC initialization,
dispatcher acceptance (`0xcaae10`), method0 reply (`0xcaadd0`), disconnect
(`0xcaafa0`), and successful native dispatcher join. It then failed during
session-manager cleanup: Core calls Lock at `0xcb1f38`, Destroy at `0xcb1ff3`,
and Unlock at `0xcb1ffb`. Exact exports/relocations confirm the native targets
`0x1e5f0`, `0x1e500`, and `0x1e6a0`; Destroy unconditionally clears initialized
state, so the final Unlock asserts even if kernel destroy were to succeed.
This is not resolved by another import binding or by faking kernel success.
The failed evidence is preserved in
`_Build/vsh-integrated/np-native-service-unlock-assert.log`. The fixture is an
explicit component gate and currently fails; no complete native teardown or
Home success is claimed. Production installation and broader NP lifecycle
admission remain disabled; no Home run followed avsh4.

Runtime attachment planning identified two additional constraints: `Client::Attach`
waits for method0 and event opening, so it cannot execute on the same guest worker
responsible for `Coordinator::Advance`. The process-global `Session()` must also
be replaced by retained owner-bound routing before Core/UI cohost attachment.
The retained transport currently admits only one connection per runtime, including
retiring connections; competing owners must receive Busy until explicitly expanded.
Callback pumping requires a consistent cohost execution association: the existing
`GetApplicationExecution()` and pthread-exit transition resolve application owners,
not a private cohost callback boundary. A private execution object alone does not
establish safe arbitrary callback/thread-exit support.

### Native service teardown and owner workers validated

Retention/endpoint infrastructure is checkpointed locally as `87f7d3ba`.
The subsequent fixture-only correction changes Core calls `cb1ff3/cb1ffb` from
Destroy/Unlock to Unlock/Destroy. It verifies the exact retained source SHA256,
live admission, complete native cleanup functions, mapped 37-byte window,
PLT/relocation identities and target functions before changing two rel32 operands.
Mismatch and double-application planners leave their outputs untouched. The
correction is applied before guest entry and is not enabled in production.

The real-image service test now exits zero after native initialization, real
acceptance/reply/disconnect, optional owner-worker attachment, actual native joins,
and complete teardown. It independently verifies the cleared kernel mutex handle
and the physically freed original IPC heap mapping, not just native Term's return.
Passing evidence: `_Build/vsh-integrated/np-native-service-retention.log`; the
earlier assertion remains in `np-native-service-unlock-assert.log`.

`runtimeNpManagerClient` supplies a disabled, attach-only owner registry with
two actual inherited guest pthreads: one advances retained transport and one
performs blocking method0/event-open attachment and detach. Real flag APIs are
used. Duplicate/foreign-owner admission is rejected; immediate startup/stop and
normal retirement retain the coordinator until native completion and both actual
OS-thread exits/joins. Startup reservation and loader Clear/transport retirement
share the loader mutex, closing the mapping-release publication race. The registry
exposes neither callbacks nor account queries, and does not replace global Session.

Integrated emulator/test builds and native-service, source-bound transport,
runtime-thread and join-retention checks passed. Independent reviews found no
remaining blocker in this bounded slice. Broader pre-reset PsnCore quiescence,
cohost callback execution association, owner-bound guest exports and coherent
production dependency admission are still required before normal Home testing.

### Early-quiescence prerequisites in progress

Checkpoint `b27f9bae` preserves the passing native-service and owner-worker slice.
The next uncommitted batch adds monotonic source-bound transport epochs and an
irreversible client-admission close, retaining draining liveness until actual
transport retirement. Independent review found no blocker; integrated execution
is pending. Added coverage includes replacement-epoch stale close rejection and
concurrent actual Core pthread Start/close admission against a real native server.

NP-only lifecycle leases retain the exact server handle, object and backing.
Stop preserves the actual result (including Busy); successful prior Stop and
successful native destruction are separate observations. All 15 standalone IPMI
suites passed with generation, destruction and same-address replacement checks.

Notification disassembly establishes that foreign-thread unregister drains the
synchronous native traversal through the shared recursive presence mutex.
Reentrant unregister is unsafe, and unregister does not drain deferred work.
An independently reviewed registry fixture uses an explicitly host observer in
the actual native list; its execution remains pending. It does not initialize the
wider manager or establish producer/deferred-worker quiescence. Evidence is in
`_TempData/np-notification-quiescence-20261106.md`.

The integrated service fixture will additionally perform actual early server
Stop, observe native OS exit, leave the sole actual join to native Term, and
verify genuine destruction through the retained server lease. Production cleanup
gateway installation remains gated on producer closure and later join/destroy
continuation guards. No Home run followed avsh4.

Integrated emulator/VM builds and the generated cleanup-gateway machine-code
test passed. The gateway test exercised both sites without an unsupported-case
skip. Source-bound transport, allocation and pthread-join retention checks also
passed. The expanded native-service run reached notification unregister/join/
cleanup and its first complete native Term, then exposed an invalid fixture
assumption: restarting IPC in the same Core image does not reconstruct its
session-list singleton. Native Term leaves `213cb58` pointing into the retired
IPC heap while the CXA guard `213cb70` remains set. A later Term faults at
`cb1f80` through the stale sentinel's null link. Evidence is preserved in
`_Build/vsh-integrated/np-native-service-second-term-fault.log`.

Validation is being split into fresh-process first-life drain and Start/close
race scenarios. Transport-epoch reinstall/stale-close testing requires no native
IPC restart or fabricated singleton reset.

The subsequent deferred-producer audit found that presence enqueue ignores its
stop byte and can be reached recursively from the consumer's state transitions.
Presence `ce3d30` preserves join status but supplies no admission close;
downstream `c69990` discards join status before freeing resources. Downstream
callbacks use unlocked snapshots. Full producer admission/drain remains unproven;
null globals after failed initialization are insufficient absence evidence.
See `_TempData/np-deferred-producer-quiescence-20261106.md`.

### Early-quiescence prerequisite validation completed

Both fresh-process `np-native-service` and `np-native-service-race` tests now
exit zero. The race run observed Start winning reservation; its original workers
genuinely attached and drained after the competing close. The default run also
checks active-client close, foreign-owner rejection and Busy after empty-registry
closure. Both successfully reinstall an empty transport epoch after native Term,
reject the stale epoch close, and close/retire the replacement without restarting
the native session singleton.

Both runs exercise native notification register/broadcast/foreign-thread
unregister with actual guest pthread joins and native list/heap/mutex cleanup.
They perform genuine server Stop, observe pinned native OS exit, retain the sole
actual join in native Term, and verify exact-generation server destruction before
dropping the lifecycle lease. Lease retention blocks provider retirement even
after native object destruction; releasing it permits actual mapping reclamation.

Passing logs are `_Build/vsh-integrated/np-native-service-retention.log` and
`np-native-service-race-retention.log`. The integrated emulator/VM build, generated
gateway machine-code test, 15 standalone IPC suites, and targeted source-bound
transport/allocation/join-retention checks passed. Independent reviews covered
epochs, server leases, notification fixture and gateway. Production startup and
cleanup installation are still disabled: dispatcher-generation/join receipts,
producer admission/drain, and exact continuation guards remain required.

### Follow-on dispatcher receipts and startup closure

Checkpoint `c565a9f3` preserves the completed prerequisite validation above.
The next batch adds immutable dispatcher receipt binding to exact server leases,
kernel thread-incarnation/join receipts, and post-join/post-destroy gateway
recipes. Adapter fixtures pass all 15 standalone IPC suites after correcting a
post-destruction expectation to NotFound; kernel/provider integration is pending.

The genuine manager-initialization audit establishes that avsh4 entered manager
success before IPC-server failure: five SceNpMgrWorkerThread queue workers ran,
whereas the Core user-event and callback-worker creates were acknowledged by a
log-only stub. That run is not an absent-worker configuration. Genuine startup
also requires NetCtl IPv4/IPv6 workers and network-event registration; the current
network callback-create stub does not retain its callback. AppCache cleanup frees
payloads before unregistering, adding another early-drain obligation.
Details: `_TempData/np-manager-initialization-closure-20261106.md`.

### Dispatcher incarnation and join receipts validated

The integrated emulator/VM build and seven targeted checks passed:
`pthread-native-receipt`, `native-cleanup-gateway`, `np-native-service`,
`np-native-service-race`, `pthread-join-retention`, `runtime-guest-threads`,
and `source-bound-np-transport`. All 15 standalone IPC suites also passed.

Kernel receipts retain the original duplicated OS handle and immutable thread
generation/owner metadata. Actual native exit is distinct from successful join;
join completion remains observable after pool-record reuse. Explicit joins,
detached collection and startup rollback have separate origins. Review caught
join attribution using mutable current-program scope; it now uses validated
actual-current receipt identity. Ownerless launchers or mismatched scopes report
unavailable joiner identity rather than assigning a selected program's PID.

The source-bound NP provider captures these receipts on actual dispatcher entry
and retains Runtime through successful native join. Exact server leases retain
the immutable dispatcher binding after native destruction. Real-image fixtures
check the original OS thread, entry and argument tuple; observe native exit
without join; then observe native Term's genuine successful explicit join.

The gateway planner now additionally recognizes exact post-join `caab2b`
(eleven-byte handle-clear store) and post-destroy `caab47` (seven-byte work load)
recipes. Parent verified their source bytes against the pinned Core SHA256.
Generated machine-code tests exercise all four recipes without skips, preserving
actual preceding return values and replaying displaced effects after the guard.
These are preservation primitives, not installed production cleanup policy.

Next: generation-bound staged cleanup coordination, verified creation/continuation
association, and genuine producer admission/drain. No new Home run or production
NP startup admission is claimed.

### Staged IPC-only cleanup helper validated

Starting from `bcaef0c7`, added `runtimeNpQuiescence.{h,cpp}` and its integrated
component tests. Begin permanently closes client admission and retains the exact
server generation and actual creator pthread receipt. Advance drains original
manager-client workers, requests genuine Stop, and observes the original
dispatcher’s native exit. Native Term still performs the dispatcher join and
server destruction; separate observation stages require their genuine receipts.

Independent review confirmed durable single admission: an atomic Begin
reservation, strong active-state ownership, and a monotonic epoch tombstone
prevent duplicate consumption, including after public-handle abandonment and
Finish. Failed Begin does not consume the generation reservation; any already
performed admission close remains irreversible. Finish releases registry pins
outside its lock. Client-worker reaping may join exited workers and reclaim
stacks; Advance never joins the native dispatcher or destroys its server.

The native-service fixture now executes init through Term on one actual inherited
Core pthread, `NpNativeLifecycle`, and verifies that thread’s genuine join
attribution. Tests cover failed/duplicate Begin, abandoned handles, retirement
blocking, foreign callers, premature evidence, error-result refusal, and
one-shot completion. The post-Term zero arguments remain explicitly policy-test
inputs checked against real receipts, not captured inner native return values.

Integrated emulator and VM builds passed, including CMake discovery of the new
loader source. All seven targeted checks passed: `np-native-service`,
`np-native-service-race`, `pthread-native-receipt`, `native-cleanup-gateway`,
`pthread-join-retention`, `runtime-guest-threads`, and `source-bound-np-transport`.
Build log: `_Build/vsh-integrated/np-staged-quiescence-build.log`; test logs retain
the corresponding `*-retention.log` names.

This establishes IPC-only component coordination. Production creation association,
actual postcall return capture and pre-free continuation enforcement, partial-init
unwind, and producer/callback admission and drain remain unfinished. Production
startup admission remains disabled; no Home run followed avsh4.

### avsh5 — requested live Home verification after staged-helper checkpoint

After `af8a0bf7`, launched the integrated executable visibly using the matching
alternate firmware/stage/RNPS roots and existing `a1360native2` store, with
`-NativeNpAlternateStartup -NoDatabases -VshRuntime -ForegroundUser -RnpsReplay
-NoGraphicsDebugDump -Visible -MonoLogLevel warning` and startup diagnostics.
PID was 11308. Host window creation was 1280x720; the launcher retained the
1920x1080 PSM render configuration. The executable SHA256 was
`c16cf051da5974628c462e7cfadd0018844234f69393d0975c3bca1ce1ce300b`;
its embedded `bcaef0c-dirty` stamp reflects the successful pre-commit build of
the helper batch, not a rebuild of the older checkpoint.

Home did not appear. The window capture at frame 91 shows black content with a
thin colored line at the top. NP IPC server creation again returned `0x8002004e`
(`_Runs/avsh5/_kyty.txt`, lines 45296–45301). ShellUI continued background init
through DBRecoveryRequestChecked, then Mono reported a native SIGSEGV with
original instruction pointer `0x93055c88a`. The reported thread was
`SceDbpNotificationThread`; the terminal unhandled exception was `0xc000001d`
at `0x96030a4f0` (UD2 in crash handling). The original fault's cause is not yet
established; it must not be conflated with that terminal crash-handler trap.
The process exited without intervention. Tile-driven app lifecycle was not reached.

Evidence: `_Logs/avsh5.log`, `_Runs/avsh5/_kyty.txt`, and
`${USERPROFILE}/AppData/Local/Temp/opencode/avsh5-window.png`.
Read-only post-exit integrity check passed for `priv/mms/notification2.db`;
`priv/mms/app.db` remains absent. Production transport/cleanup installation was
not enabled by this run. The prior component passes do not establish Home startup.

### 2026-11-06 review correction

The statement above that avsh5 used matching Core/UI auth records is contradicted
by its log: no pinned admission or AppDb adapter installation occurred. The runner
depended on ambient host variables that were omitted. Explicit record parameters
and alternate-VSH preflight now prevent that configuration mistake. A corrected
live run restores genuine AppDb acceptance but still stops at NP/AutoMounter
startup failures. See `docs/shellui-boot-review-20261106.md` for the first-chance
fault capture, fixes, checks and corrected-run evidence.
