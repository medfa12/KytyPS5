# Regular 13.60 local profile/service boundary

## Decision after independent firmware review

Target **native UserService and LoginMgr clients over one authoritative local
HLE service**. Keep the native/managed UI. Full native ShellCore server execution
is a distinct configuration requiring demonstrated manager initialization,
storage, transport and source-bound admission. Remote PFAuth/PSN provisioning is
separate from local profiles.

The current export HLE remains the validation frontend while its backend is
completed. A native-client switch must happen per owner as a coherent unit;
mixing native cached queries with unrelated HLE transitions is not acceptable.

The previous statement that unavailable native LoginMgr admission prevents
native UserService *client* execution was too broad. UserService depends on
Ipmi/kernel/LibcInternal/RegMgr/Sysmodule/SystemService, not LoginMgr. A local
endpoint can service it without admitting native ShellCore server handlers.

## Authoritative model and publication contracts

- Persistent profile records: identity, name, account linkage, passcode,
  auto-login, flags/status, last-login order. Creation is not login or PSN.
- Profile storage: three roots and native-required directories/defaults, bound
  into the matching service namespace. Publish a new record and creation event
  only after provisioning succeeds; retain enough transaction state to recover
  a crash between directory publication and durable profile publication.
- Active login slots: first free slot1..4, stable until logout; not profile-ID
  bits, vector index or persistent last-login order.
- Foreground identity and per-owner initial launch identity are distinct from
  login membership. Do not silently rewrite application launch snapshots.
- Events: callbacks receive creation/destruction and session events. Public
  polling exposes login/logout only. Native login/logout waits for callback
  acknowledgment; creation does not have that callback gate.
- Guest storage is session-owned. Logout/deletion must retire it without
  deleting unrelated or unmanaged files; blocked cleanup remains explicit and
  recoverable. A failed profile publication must leave no visible profile.

## Disassembly evidence

All RVAs are for original regular 13.60 images.

### libSceUserService.sprx

- `+0x1930` initializes client state and optionally copies the 29-function,
  0xe8-byte backend table. This table alone does not replace event/login IPC.
- `+0x1b99..0x1bce` creates `SceUserServiceGetEvent_%08x` for the actual PID.
- `+0x1c18..0x1ca2` creates/connects IPMI `SceUserService`, one queue.
- `+0x1cc0` drains initial events; `+0x1d17` starts native event worker `+0x2380`.
- `+0x1e38..0x1e57` receives eight-byte `{type,user}` from vtable slot7, queue0.
  Empty transport queue `0x80020023` becomes UserService no-event.
- Dispatch `+0x213a..0x232c` invokes callbacks outside its registry lock,
  rescans registrations, and deduplicates callback/userdata pairs per event.
- Public GetEvent `+0x3200` filters types above1, unlike callback dispatch.
- Login `+0x28c0` and logout `+0x2a80` use job submission and `0x3001f` status
  polling (`+0x10c0`), then client-local callback acknowledgment flags.
- No-argument termination `bwFjS+bX9mA +0x24d0` joins the event worker and
  tears down the client; it is not a GetUserNumber alias.
- Number `qbwy0Ub8b3M +0x3860` uses backend slot+0x78, default `+0x40f0`,
  IPC0x30012. Color `+0x37e0` maps numbers1..4 to colors0..3.
- AutoLogin `nqDEnj7M0QE +0x4aa0` uses property3 and a four-byte output
  (`+0x520`, IPC0x30008). LastLoginOrder `4nUbGGBcGco +0x4e60` uses property5,
  also four bytes. Success without writing these outputs is not a valid getter.

### ShellCore

- Server worker `+0xa11610`; handler vtable `+0x20358d0`.
- Sync adapter `+0x9ef3a0 -> +0x268690`; dispatch table `+0x1edab60`,
  methods0x30000..0x3002d; session acceptance `+0x9ef3e0`.
- Login job `+0xa11900`; outstanding job returns0x80960106. Status
  `+0xa11830` returns state1/pending or state2/completed and actual result.
  Never synchronously wait for a native client callback inside job submission:
  the login caller has not yet opened its callback gate.
- Number getter `+0x9f7c30` requires current login membership (otherwise
  0x80960009), then reads a map. Login allocates first free1..4 through
  `+0x9f7010`; logout removes the mapping at `+0x9f5042..0x9f5050`.
- Creation `+0x9f3b90` provisions through `+0x9efa50` before publishing its
  initialized record. Paths: `/user/home/%08x` (`+0x9dfc40`),
  `/system_data/priv/home/%08x` (`+0x9dfc90`), and
  `/system_data/eap/rodata/home/%08x` (`+0x9dfce0`).
- User-root suffix table `+0x2035918`: `/webkit/webbrowser`,
  `/webkit/webbrowser_dialog`, `/webkit/shell`, `/webkit/secure`.
  Failure cleanup `+0x9eff10` covers all three roots. Final creation event is
  0x100 at `+0x9f4259..0x9f4264`.
- Record initialization `+0xa0c3d0 -> +0xa0c3f0` loads registry defaults,
  including keys0x7800100 (ID),0x7800200 (17-byte name),0x7800300
  (16-byte passcode field), and0x7800400 (AutoLogin at record+0x48).
  The function spans many other properties. Provisioning the four WebKit
  directories does not establish equivalence with all native record defaults;
  only the explicit supported local profile fields are currently modeled.

### ShellUI / AsyncStorage

- ShellUI `+0x738a0` handles creation0x100/destruction0x101 and routes to
  `+0x74f40/+0x74fd0`. PSM callback `+0x6df1c0` delivers event40 for login.
- AsyncStorage PathManager `+0x11ce0 -> +0x11d40` constructs the private
  sandbox home path using `JGfTMBOdUJo` and requires it to exist before opening
  databases. See psn27 evidence in `vsh-loginmgr-create-user-20261107.md`.

## Native-client migration gates

1. Finish and validate authoritative profile/storage/session transactions.
2. Add a selected local `SceUserService` endpoint with actual lifecycle,
   owner-bound queue0, initial snapshot, named wakeup, job completion and typed
   property/list methods. Generic `ClientMethod` success is unsuitable:
   `libC.cpp` currently leaves receive slot7 as a success-without-output stub,
   which can make native startup's drain loop never finish.
3. Run one owner-private native client through init, login, callbacks, query,
   termination and actual join. Provider mutable globals must be isolated per
   process: changing current PID does not isolate its backend table, caches,
   callback list, event queue or worker.
4. Move native LoginMgr client serialization/reference counting to an explicitly
   selected local endpoint over that same model. Keep native server admission
   and remote NP/PFAuth routing distinct.

## Live verification status

psn27 proves callback completion, Theme.OnLogin/InitializeUserEntry/Apply, and
RNPS Home mount/active-scene transition. It does **not** prove visible usable
Home: the user still reports a stall. Native AsyncStorage reports ENOENT for
the new guest's home. The process exited before a stalled-thread capture.
Names/images and actual PSN sign-in remain unresolved.

## Lifecycle integration under verification

`localUserStorage.h/.cpp` implements explicit three-root transactions. A
layout-bound receipt is flushed before staging or promotion; each directory
has an ownership marker. Preparation creates all roots and the four WebKit
directories, then publishes them without replacement. A failed record save
rolls them back. Startup reconciles receipts against the successfully loaded
complete persistent record set; invalid JSON never substitutes an empty set
for destructive recovery. Unmanaged directories are neither adopted nor deleted.

The local backend resolves writable mounted roots and recovers once before
creation/login admission. The journal is `local-user-storage` beside the
selected `users.json`. Creation publishes storage before records and event0x100.
Deletion commits the record change and retains owned storage until quiescent
startup recovery. Existing unmanaged records can be removed from the JSON
without claiming ownership of their directories. The component also supports
quarantine/rollback for callers that can prove quiescence; the live frontend
does not yet have that proof. Unresolved rollback/receipt failures disable
subsequent writable/login admission until startup recovery.

**Quiescence boundary:** logout callbacks returning does not prove every native
database/storage consumer has closed its handles. Even creation callbacks can
start consumers before any login. All published profiles are logically retired
on logout/deletion, but physical cleanup is deliberately deferred to the next
process's startup recovery. Guest IDs are not reused within the process. The
receipt keeps this cleanup explicit and durable. Immediate in-process cleanup
requires a storage-consumer drain contract; it is not implemented here.

Startup upgrades legacy record-only persistent profiles when all three homes
are absent, using the same transactional provisioner before consumers start.
Complete existing unmanaged trees remain unmanaged; a partial layout blocks
admission rather than merging ownership. This preserves the active store's
existing `User1` record discovered during integration.

Legacy upgrades use a distinct durable `U` receipt: the authoritative record
predates storage, so ordinary creation recovery cannot infer that provisioning
finished from record membership alone. Interrupted upgrades retire only their
newly owned storage and can be retried before admission. Successful Commit
durably converts `U` to live `C` before consumers can enter. Truncated pending
receipt writes are archived byte-for-byte under non-reserved names; only the
promoted receipt grants recovery authority. Archival never overwrites a file or
follows a symlink/reparse source.

The private-home sandbox alias now maps to the same mounted authoritative
storage. Stable login slots, four-byte AutoLogin/LastLoginOrder getters,
creation/destruction callbacks, and application terminate/reinitialize snapshots
are integrated in the export frontend. Boot initialization/termination still
does not reproduce the full native client lifecycle. Native endpoint/provider
isolation remains future work, not enabled by these changes.

The storage component's independent Windows filesystem tests passed before
integration, including real third-root ACL denial, unmanaged collisions,
record-save rollback, interrupted create/delete recovery, guest cleanup,
corrupt receipts, rollback conflicts, blocked rename and cleanup retry.
Shared build/tests and visible UI results will be recorded after verification.

### Verified checkpoint

Both `virtual_memory_allocation_tests` and `kyty_emulator` built successfully
after all integration changes (`_TempData/local-user-contract-final-build.log`).
The six targeted CTest suites passed 6/6:

- `local_user_creation`: creation callback sees all three homes; failed record
  replacement rolls back actual provisioning; stable slot reuse; exact typed
  property output; callback versus polling events and worker shutdown.
- `local_user_storage`: transaction, upgrade/crash, ownership, archival,
  filesystem-denial and cleanup/retry scenarios. The optional Windows symlink
  case was skipped for missing symlink-creation privilege; other scenarios ran.
- `local_user_storage_upgrade`: startup retires old guest storage and provisions
  a legacy `User1` without rewriting its record bytes; subsequent login succeeds.
- `local_user_storage_invalid_store`: truncated JSON preserves guest storage and
  original bytes, and blocks creation instead of running destructive recovery.
- `runtime_user_service_owners`: stable owner slot/property snapshots and
  terminate/reinitialize behavior without logging out the shared session.
- `runtime_user_service_controllers`: configured-controller behavior retained.

No new live Home result is claimed by these tests. psn28 is the next regular
13.60 run; native endpoint isolation, full boot-client lifecycle, names/images,
genuine PSN and remaining media integration are still open.

### psn28 startup evidence

Regular 13.60 psn28 launched as PID17880 from the verified build (its embedded
pre-commit label is `b11a2f5-dirty`; lifecycle code is checkpoint `99e759f1`).
Native `_Runs/psn28/_kyty.txt` shows the private-home alias at line22937,
loading the existing `User1` at37617, successful recovery at37658, and successful
legacy provisioning for0x10010004 at37967. All five ShellUI callback registrations
return0. The authoritative `users.json` still contains the same `User1` fields.
Early captures show the boot background, not usable Home. A bounded Home-mount
capture watcher and post-name-conversion observer are armed; controller input
remains with the user. No Quick Play or PSN completion is established yet.

### psn28 persistent login: background-only Home captured

The next user-driven transition was persistent `User1` (0x10010004), not a
Quick Play guest. Native log663298 records login export result0; the persisted
record now has last_login_order1. Managed log10353-10424 records Theme.OnLogin,
entry initialization and completed Apply. Home mounts at10555 and becomes the
active scene at10559; LoginMgr's exit/unload sequence completes.

The mount watcher captured `_TempData/psn28-home-mounted.png` (frame964) and
bounded native thread/stack evidence in `psn28-home-mounted-threads.txt` and
`.raw.json`. A follow-up screenshot `psn28-after-home.png` at frame1657/fps52
still shows only the background, with no Home navigation/cards. This is direct
evidence that active-scene/mount success is insufficient; usable Home remains
broken. The process subsequently exited; its exit cause is not established.

The previous missing-home AsyncStorage error was not found in this run. A real
`priv/home/10010004/nobackup/rnps/NPXS40003/asi_sqlite.db` now exists, establishing
storage progress for Control Center. No NPXS40002 database was found, so this
does not establish Home's own storage/bootstrap completion.

The bounded observer at ShellUI+0x6df4a3 captured two successful username calls:
both native UTF-8 buffers contain `User1`, and both returned managed strings
contain exactly five UTF-16 characters `User1`. See
`_TempData/psn28-managed-username.json`. Any corruption for those observations
is downstream of this conversion; this does not alone identify a renderer bug.

ProfileCache logs `InvalidUriException: wrong userid` before login. Its parser
checks the parsed ID at+0xd38e; ShellUI's registered
`UserServiceWrapper::_IsValidRealUserIdRange` is+0x6e45c0, which accepts
0x10000000 through0x3fffffff. Thus the chosen0x10010004 is in the genuine native
range. The failing URI values still require observation; do not weaken the
validator. PSN pfAuth still reports0x80550002 (managed log4466).

Independent read-only analysis is now following the Home-specific bootstrap
dependencies against the captured evidence, rather than assuming another
UserService export is responsible for the background-only screen.

### psn29 discriminating capture armed

Offline Home bytecode analysis establishes the sequence revision -> cache key
-> Home AsyncStorage read -> per-space item enumeration -> experience atom42.
An unsettled promise before cache access can prevent the NPXS40002 database
from appearing. Startup visibility also awaits nonempty experiences and first
paint; mounted/active Home is not proof those gates completed. No particular
stalled stage has yet been established.

psn29 launched as PID20564 with the same shared store and `-NoDatabases` options
for comparison. `_TempData/trace_home_contract.py` was syntax-checked and its ten
instruction boundaries checked against regular firmware disassembly. Review
corrected its AMD64 context-buffer handling before attachment. The live observer
validated original bytes and recorded all ten probes armed in
`_TempData/psn29-home-contract.jsonl`. It observes database module/adapter calls,
revision output and matching promise settlements; it does not change results.
The Home-mount screenshot/thread watcher is also active. Controller input stays
with the user; the intended comparison is another persistent `User1` login.

Pointer-based promise correlation can miss GC moves, and concurrent trap gaps
can miss calls. Absence alone is therefore not proof of a stalled promise.
An independent read-only review is tracing AppBrowse's schema/producer boundary
and the existing809c0103 initialization errors. Visible Home, Quick Play,
names/images, genuine PSN and media completion remain unresolved.

### psn29 guest result and revision observation

The user-driven flow created guest0x20010000 (type3, incomplete1), then completed
its login export. This was not the intended persistent-user comparison.
`psn29-home-mounted.png` at frame890 and `psn29-after-home.png` at frame15581/fps38
both show background-only Home. Guest creation/login logs are therefore not a
usable-Home success.

The observer captured DatabaseModule.GetScalar2 context1, the Concept adapter's
`revision` key, and GetAppBrowseDbRevision entry on host thread14864. It did not
capture the normal revision output or adapter-result boundary. Later GetItems2
requests for contexts4/5 used the same module. Home attribution is supported by
timing, but its specific module/application identity still needs confirmation.

A subsequent bounded stack sample (`psn29-revision-worker.json`) found14864 back
in the Mono pool's semaphore wait, with no revision frame. This does not support
a synchronous call still blocked inside the accessor; exception unwinding is a
candidate. Live inspection found the accessor's Lazy connection initialized to
Sce.Vsh.Db.Shared.Connection and SQLiteConnection's path pointing to sandbox
`priv/mms/app.db`. The original promise address had already been reused after
GC, so missing pointer-matched settlement is inconclusive. The next observer
version logs all promise settlements and the database rejection/exception
boundary, rather than relying only on the original managed pointer.

The read-only object-graph diagnostic scans bounded candidate pointers and may
cross object bounds; only fields independently established by method layout
should be treated as actual references. No guest results/readiness values or
production implementation were changed by this diagnostic checkpoint.

Final psn29 trace resolves one uncertainty: a second context1 revision request
used promise0x205b69c70, entered the accessor on thread19900, and reached the
matching Promise.Reject about150ms later. Revision rejection is observed, not
an indefinitely unsettled request. Its JObject payload was not expanded by
that observer version. Another module's context2 item request resolved; this
does not establish Home experience completion. The observer restored/detached
after the parent deliberately stopped psn29 for the next diagnostic run.

psn30 is running as PID16052 with twelve byte-validated probes armed, adding
DatabaseModuleError.Reject and recording all promise settlements independently
of original pointer matching. The Home screenshot watcher is armed. The store
and launcher options remain the same; persistent User1 login is requested for
comparison with the captured guest failure. No implementation fix is claimed.

### AppBrowse producer review

Independent firmware review resolved the revision query as
`SELECT status FROM tbl_version WHERE category=?`, binding `table_access_index`.
It is global, not keyed by user. Accessor+0x58daa calls shared
`Connection.ExecuteScalar<Int64>`; SQL preparation exceptions bypass its normal
output and reach DatabaseModule's rejection handler. The actual shared app.db
passes SQLite quick_check but lacks tbl_version. Its existing per-user tables
cover old0x10000001..0x10000010 IDs, not User1's0268500996 or the guest's0536936448
suffixes. These omissions are consistent with rejected revision/item queries;
psn30 still needs to capture the actual exception.

Native initialization has a distinct problem: the logged809c0103 originates
from a null connection handle (ShellUI+0x5b2224/+0x5b2263), before SQL preparation.
Its table-manager initialization queries sqlite_master, for which absent user
tables alone would produce an empty result rather than that error. Firmware
already provides version-table creation (+0x54a900), schema transaction
(+0x55c5e0), mutation revision maintenance (+0x472b50), and user browse creation
(+0x55fbb0) called by CreateUsers (+0x560a80) and event0x100 (+0x54fb10).

The fix candidate is therefore the native producer's connection/schema/user
lifecycle against the same database, pending diagnosis of its failed open/init.
Re-enabling prepare_shellui_databases_1360.py alone is insufficient: it omits the
version lifecycle and addon/playlist/group tables and rejects guest IDs. Do not
replace revision with a constant or manufacture Home experience rows. Revision
rejection is caught by Home's read-through path, so downstream item failures
must also be checked before attributing all invisibility to the revision query.

### psn30: confirmed Home exception and selector producer corruption

Persistent User1 (0x10010004) logged in, but Home remained background-only.
The twelve-probe observer captured two actual `Sce.Vsh.SQLite.SQLiteException:
no such table: tbl_version` rejections. Read-only object/weak-handle inspection
attributed the DatabaseModule to `rnps-home`, NPXS40002, instance
917fe8f3-0fe1-42e0-b8c3-e3c54c103b61. Evidence is in ignored diagnostic artifacts
`psn30-home-contract.jsonl` and `psn30-home-appidentity.json` under `_TempData`.

The user showed correct User1 in Control Center, then a corrupted label/missing
image in Switch User. The six-probe text observer captured Control Center's
decimal URI user268500996 and Switch User's
`profilecache:text/name/default?userid=0x40001001&ItalicIfOnlineId=true&statusIcons=all`.
The latter is the reversed hexadecimal spelling of 0x10010004, outside the
native validator's accepted range. The observer restored/detached after34 hits;
psn30 was then closed through its window for the provider-fix restart.

Firmware JS function30701 (`cc1360.hasm` lines1068548 onward) constructs that
selector URI with `userId.toString(16)`. Hermes+0x95a9c selects the digit alphabet;
+0x95ab0..0x95b34 appends successive radix remainders; +0x95b43 calls
`tJtD8WHnb04` with RDI=buffer and RSI=buffer+length to reverse them. psn30 bound
that import to `CStub_tJtD8WHnb04`, a no-op. Other captured callers
Hermes+0xa4877/+0xa4887/+0xa4894 use three reversals in its string-hash path.

Removed the no-op definition/registration so normal resolution uses native
libc, as already done for frexp. Genuine libc export+0x39eb0..0x3a0eb is a
self-contained end-exclusive byte reversal: no calls, internal branches, one
RIP-relative shuffle constant at+0x16e850. It uses scalar, SSE and AVX2 paths;
no libc initialization/global state is accessed by this leaf. No URI-specific
correction or validator change is involved.

An isolated Windows ABI bridge executed the actual firmware leaf and constant:
4,384 range/alignment cases (lengths0..129,255,256,257,511,512,1023,4096;
alignments0..31), full-buffer canaries, double-reversal round trips and radix
samples passed. The diagnostic is `_TempData/verify_native_reverse.py`.
SHA256 of function bytes [0x39eb0,0x3a0ec):
`a8b1aebd8417c21196a8edcb1c8b981da7f2c471350d04d9d3ebe110d3b6697b`;
libc image SHA256:
`8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`.
This verifies the primitive, not visible UI completion. Restart binding and
user-driven selector/name/image verification are still required.

Native AppBrowse's unopened handle is now explained: ShellUI+0x55bb00 checks
`app.db-corrupt`, sets version status0x100 at+0x55bc80 and returns0 without
calling openDb+0x55bc05. The persisted marker is present in the shared store.
A genuine VFS open failure would be809c0200, distinct from observed809c0103.
RecoveryAppDbReset+0x55e600/+0x55e640 removes DB files and opens with flags6,
schema callback+0x55f7e0; its caller+0x88e830 selects it only for mode+0x4e8==1.
This destructive recovery path must be selected by the actual lifecycle, not
called unconditionally to create a missing version table.

DbPreparation methods0x160007 (bad-power-down byte output),0x160004 (invalid-user
count, four-byte output),0x160006 (delete invalid users),0x160002 (initialized
notification,16-byte input) currently receive generic successful IPC responses
without server execution. Core initializer+0xb28240/+0xb27cf0 and AppInfo
Init+0xff1780 own missing producer work; the latter creates
SceAppDbSyncManagement through+0xfe437c. ShellUI opens that event, receiving
ESRCH0x80020003. AppDb transport alone does not establish these lifecycles.
Independent investigation continues into recovery selection and Core startup.

Provider-fix checkpoint: `kyty_emulator`, `virtual_memory_allocation_tests` and
`symbol_database_tests` rebuilt successfully; CTest `native_libc_startup` and
`symbol_database` passed (2/2). The first combined build command included the
nonexistent target `kyty` and failed before building; corrected commands above
passed. psn31 PID16592 retains the shared store and launch options. Its
relocations, including Hermes GOT+0x275b90, resolve `tJtD8WHnb04` to
libc0xa80039eb0, and native libc startup completed before initializers.
The text observer's first attempt preceded ReactNative.PUI mapping and exited
before attaching; after the required modules mapped, it was relaunched.
Visible selector/name/image, persistent/guest usable Home, genuine PSN and
media acceptance remain outstanding.

### psn31 visible result

The user's screenshot now shows the selector label **User1** correctly. The
live text trace captures the repaired selector URI
`profilecache:text/name/default?userid=0x10010004&ItalicIfOnlineId=true&statusIcons=all`.
This verifies the name fix at both its producer and the displayed label.
The image is still an empty circular placeholder; image completion is not
claimed. Its observed face URI already has correct decimal user268500996.

After user-driven login, the Home-mount watcher captured
`_TempData/psn31-home-mounted.png` (frame1527/1534 capture interval): still
background-only. Native reversal fixes the demonstrated name corruption but
does not establish usable Home. Image-path diagnosis continues through
ProfileCache.LoadImageRequest/ReadImageFile/FileCommonUtility.ReadBinary.
The firmware fallback assets exist under system_ex_b/priv_ex/etc. Their
presence alone does not prove the sandbox path, loader, or decoding succeeds.
