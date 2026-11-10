# Local AppDb runtime producer — first transactional stage

## Implemented boundary

`src/loader/runtimeLocalAppDb.{h,cpp}` implements a real C++ SQLite-backed producer
stage, compiled into the emulator source set and independently buildable. Its
explicit working-database path and stored local-user roster are supplied by the
caller. There is no signed-native-admission prerequisite.

`PrepareUser` performs an existing-file open, `BEGIN IMMEDIATE`, integrity/schema
validation, native additive `tbl_version`/`tbl_info_%010d` DDL, actual revision
read, then COMMIT. It returns `Committed` only after SQLite commits. This status
describes **current-user metadata schema only**, not database recovery/catalog
readiness. Missing revisions remain absent rather than becoming zero.

Existing tables/rows, corruption markers, and revisions are retained. An unknown
local user, incompatible schema, missing database, SQLite failure, or cancellation
cannot publish completion. Missing files are not created. Native schema/version
rows and catalog rows are not seeded. Stop requests cancellation before joining
the transaction, closes the connection, and is terminal. It owns no loader mutex
and executes no guest callbacks. A COMMIT already admitted when Stop is requested
may finish; successful durable work is not retroactively called a failure.

The producer is a runtime component with a direct tested API, **not yet connected
to the generic/native preparation transports**. The recovered stage is too narrow
to acknowledge0x160002 or set a native sync flag. Activation remains the next
integration boundary; there is no environment switch that reports false readiness.

Host dependency: official SQLite3.50.4 amalgamation, SHA256
`1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032`, pinned in
`3rdparty/sqlite.cmake`; loadable extensions disabled. This is distinct from
guest SQL ABI/emulation. Top-level CMake links the backend and exposes a focused
test target. Full emulator integration build remains a parent gate.

## Validation

Standalone CMake project `tests/local_appdb` builds only this backend, its tests,
and the pinned SQLite amalgamation. All source/object/library inputs are isolated
in this worktree; no parent/donor build objects are consumed. At most two jobs.

The meaningful controls use actual SQLite locking/transactions:

- Existing current-user values, unrelated profile bytes, existing revision729
  (fixture-only), and corruption marker survive repeated calls.
- Existing incompatible current-user table causes failure after tentative
  `tbl_version` creation; ROLLBACK removes that tentative DDL and preserves rows.
- A foreign writer holds `BEGIN IMMEDIATE`; Stop cancels and joins while that
  writer is still holding the database.
- A foreign reader holds a shared lock across the producer's COMMIT. No receipt
  or new table is visible before reader release. Releasing it permits durable
  completion. Stopping instead rolls back without requiring reader release.
- The stronger COMMIT-stop test initially exposed a cancellation-order defect
  in this new component: waiting for the commit gate before setting cancellation
  left the busy COMMIT unable to observe Stop. Request-before-join fixes it; the
  corrected production implementation passes this negative boundary test.
- Missing files and malformed SQLite inputs fail without creation/reset.
- The actual **previously copied** psn23 app.db is opened read-only and backed up
  to a new test database. Both current profile IDs0x10010004/5 complete additive
  schema work. Every original table's typed row contents are compared afterward.

Commands (use an initialized host C/C++ toolchain):

```text
cmake -S tests/local_appdb -B _Build/local-appdb -G Ninja
cmake --build _Build/local-appdb -j2
_Build/local-appdb/local_appdb_tests.exe <copied-app.db>
```

Local build log: `_Build/local-appdb/build.log`. No live process interaction or
access to the now-live original psn23 database was needed.

## Further recovered native contracts

### Preparation: registration is not completion

The previous checkpoint left the virtual call behind Core `+b23370` unresolved.
It is now resolved through **ELF DT_RELA**, not guessed from adjacent code:

- Notifier constructor `+b236fb..+b23716` installs vtable `+203ec20`; its slot+0x10
  relocates to `+b23940`.
- `+b23940` scans observer lists for an existing pointer, selects the group by
  the supplied integer, and appends the observer pointer. Thus AppInfo's
  `+27d6db -> +b23370` registers object+0x28 in group0 after initialization.
  **It does not publish completion or0x160008 readiness.**
- Notifier `+b237c0` invokes registered callbacks, then marks notifier byte+0x48.
  Getter `+b233b0` overlays that byte into output byte1.

The 24-byte0x160008 path is now narrowed to `+b28ad6 -> +b28150 -> +b233b0`.
The concrete preparation vtable at `+203eee8` maps slot+0x10 to `+b27c00`, which
copies object+0x10..+0x27. `+b28150` additionally overwrites output DWORD+0x14
with `+17e76f0`'s state. This is composite owned state, not24 zero bytes.

Concrete primary-user preparation `+b276a0` checks existing initialization and
real user existence, handles user-ID cache/system-directory work, invokes
`+b24380`/`+b24690`, then writes initialized byte object+0x10 and actual user ID
at+0x14. At `+b24380`, registered producers are selected by input mode/event;
their return values are inspected at `+b2441f`. On the accumulating path, the
first failure is retained at object+0x1c from the producer's record+0x28. Hence
initialized byte0 alone is not an all-producer-success predicate.

### Recovery: querying existing results does not perform recovery

Core recovery instance constructor `+17bd040` installs vtable `+2084008` and
two category maps. `DbRecoveryGetOperationResults +17a1fe0` dispatches slot+0x28
to `+17a24c0`. That function validates keys, looks up the category/operation,
and checks stored status at map-node+0x24. `0x809c1145` means **no EagerCheck
result**; it returns unavailable without copying result fields. Otherwise it
copies node+0x24 status, byte+0x28 corruption, DWORD+0x2c promotion to output
offsets0/4/8 and returns0. This supports a retained-result producer design, not
mapping SQLite `integrity_check=ok` directly to every recovery operation.

The operation-specific EagerCheck writers and promotion decisions remain to be
mapped before the HLE backend can populate these native result slots.

### Version compatibility is policy, not CREATE IF NOT EXISTS

Core `+ffe8b0` separately checks accessor versions and state. In the newer-schema
case, if the compatibility test permits preserving it and stored version>=26,
it records status0x20 with keep-state byte+4=1 (`+ffe9bb`). Other paths record
0x100 or combinations of version/rebuild flags. `+1004a80` obtains a list through
`+1004b20` and tests for a breakpoint greater than the supplied version. It is
not itself the table-creation implementation. Local SQL success cannot erase
this compatibility decision.

UI `+55cc40` constructs version records including `appbrowsever` and
`low_limit_appbrowsever` from its supplied tuple and writes them with
`REPLACE INTO tbl_version VALUES('%s', %d)` only in the enclosing schema
transaction. The backend deliberately does not stamp these after a partial stage.

### Managed table revision is derived from successful row work

UI helper `+472b50` queries `table_access_index` using the native literal at
`+f9c595`. With a row it reads/increments the prior value (`+472cf1..+472cfd`);
without a row it selects1 (`+472d02`), then writes through the real SQL executor
using `REPLACE INTO tbl_version VALUES('%s', %ld)` (`+472d08` onward).

One concrete caller at `+474b0c` first executes its row mutation, checks success
at `+474b14`, checks affected-row count at `+474b18`, and only then calls the
revision helper at `+474b24`. Zero affected rows returns0x80f50004 instead.
The revision therefore belongs to actual table mutation, not to opening the DB,
creating a kernel sync event, or adding the metadata schema stage above.

Core sync-event setter masks remain separate: association of each actual
transaction writer with its UI consumer and completion ordering is still needed.

Reproduce vtable resolution with the enhanced pinned inspector:

```text
python tools/inspect_vsh_db_contract_1360.py core <regular-core> --slots 203ec20:4 --slots 203eee8:14 --slots 2084008:9
```

Counts in that interface are hexadecimal. It validates DT_RELA entry size and
requires unique R_X86_64_RELATIVE slots; no firmware is executed.

## Next implementation boundary / remaining blockers

The additive metadata stage is now real transactional runtime code. Current-user
catalog tables and their native source records are still absent for the actual
profiles. Do not copy the old seeded-user catalog or stamp version/revision rows
to make the stage look complete. Next work is the operation-specific catalog
transaction and EagerCheck result producer, then a preparation adapter which
aggregates those genuine receipts and publishes only the matching event masks.

`StopAllModules`' loader-lock-across-guest-finalizer hazard remains, but this
backend has no loader/guest finalizer dependency, so no broad loader change is
required for this stage. The earlier session Stop receipt does not solve general
raw-Program lookup/unmap pinning. No new `runtimeLinker.h` overlap is introduced.

## AppInfo base-schema follow-up

`PrepareAppInfo` adds a second, explicitly scoped receipt (`AppInfoBase`). It runs
the three exact Core DDL statements in one transaction, checking all columns,
nullability, defaults, hidden columns, and primary-key positions. Core `+1005950`
uses the DT_RELA SQL pointer array at `+2064b80`: `tbl_version` at `+1dcc90d`,
`tbl_appinfo` at `+1dd8e65`, and `tbl_conceptinfo` at `+1e08745`. A comparison
against the pinned Core image verified all three C++ SQL strings byte-for-byte.

The default open remains existing-only. `AppInfoCreation::CreateIfMissing` allows
explicit creation for the AppInfo scope; it never retries a failed/corrupt open
by resetting the file. User-metadata scope cannot use this creation permission.
Native Core `+ffe372` opens with flags6 before beginning its schema transaction.

Path ownership matters: Core selects system `/system_data/priv/mms/appinfo.db`
and user `/roaming/mms/appinfo.db` paths (`+f9a404`, `+f99f8f`). These differ from
UI AppBrowse `app.db`. Running both schemas on a copied input in the compatibility
test verifies preservation only; it is not a runtime routing prescription.
Likewise psn37 uses `SceAppInfoIpc` (observed0x30007/8 and0x30000), whereas the
existing native AppDb transport names `SceAppDbIpcmsg`.

Standalone validation passes for explicit new-file creation, default missing-file
rejection, user-scope creation rejection, corrupt-file preservation, repeat
AppInfo work preserving typed fixture rows, and failure on the third table
rolling back both earlier DDL statements. Copied-input tests preserve every
original table's rows and both actual current-user metadata scopes.

### Native post-schema access index (recovered, not part of this stage)

The complete Core initialization transaction is larger than its DDL callback:
`+ffe3bd -> +1005950 -> +1006610 -> +1022b40`; `+ffe420` repeats it on an open
accessor. The caller initializes the index output to0. `+1005f20` queries
`SELECT category, status FROM tbl_version WHERE category = ?1`; no row leaves
the supplied numeric default unchanged. Numeric status is retained; string
status becomes a string variant, which `+1006610` converts to0 for this operation.
The critical instruction **`+10067de inc rax`** increments that result before
binding parameter2 of `REPLACE INTO tbl_version VALUES(?1, ?2)`. Missing therefore
becomes1, and an existing integer advances by1 on this native initialization
operation. This is `access_index`, not Home's `table_access_index`.

The base-schema receipt deliberately does not certify that post-schema operation
or full AppInfo initialization. The following checkpoint adds a distinct operation
for the post-schema transaction. Recovery/catalog aggregation and commit-to-event
associations still block guest transport activation.

## AppInfo schema-initialization transaction

`InitializeAppInfoSchema` now implements the evidenced DDL + `access_index`
transaction, with scope `AppInfoSchemaInitialization`. A missing index becomes1;
an existing SQLite integer advances by1. It binds the exact integer value rather
than using SQL arithmetic (which would promote INT64_MAX+1 to REAL). Unsigned
bit arithmetic reproduces the native64-bit increment, including wraparound.
Core descriptor `+1ed06c0` has type1; binder jump-table `+1f60730` selects
`+1032e12`, which forwards the entire64-bit payload.

The `access_index` receipt appears only after successful COMMIT. Repeated explicit
initialization repeats the native transaction and advances the stored index;
`PrepareAppInfo` and `PrepareUser` do not. Existing noninteger indices are rejected
as incompatible and preserved, a deliberately narrower admission policy than
Core's variant-to-zero handling. No schema version, catalog row, Home
`table_access_index`, recovery result, or guest event is produced by this operation.

Failure checks cover statement errors, ignored writes, rewritten index values,
and blocked COMMIT. An existing trigger returning SQLITE_DONE via RAISE(IGNORE)
does not suffice: affected-row count and the resulting integer are verified in
the transaction. Cancellation rolls back both tentative DDL and index changes.
As with earlier stages, the runtime adapter must select the correct private
AppInfo database; guest transport activation is still pending.

Standalone build (`-j2`) and copied-input regression pass. Added controls verify:

- Initial1, repeat2, reopening to3, and exact signed64-bit wrap with integer type.
- Base/user metadata calls leave the initialized index unchanged.
- Existing Home index729, unrelated version26, and corruption marker survive.
- Noninteger status, ABORT, IGNORE, and a rewriting trigger each preserve the
  original table contents and roll back both newly created AppInfo tables.
- Two independent connections serialize read/increment/write and retain both
  committed updates rather than losing one.
- A held reader observes the old index23 and no new table while COMMIT waits;
  release yields24. Stop before reader release yields cancellation, index23,
  no new AppInfo table, and no index receipt.

### Additional recovery evidence

The recovery map has separate EagerCheck, Estimate, and Promote slots. Core
`+17a22c0` saves EagerCheck's12 bytes to node+0x24/+0x2c; `+17a2350` saves
Estimate to+0x30/+0x38; `+17a23e0` saves Promote to+0x3c/+0x40. A negative
Promote result with EagerCheck promotion field1 calls `+b23410(0x809c114d)`.
Getters `+17a26b0` and `+17a28a0` independently reject absent Estimate/Promote
results. `+17a2120` accepts target0 operations0..14 and target1 operations0..5;
only its save-mode path inserts a missing node initialized with unavailable
statuses. A getter must not create a successful result.

AppInfo EagerCheck `+17a15d0` first invokes shared checking `+17a1170`, then
`+ffe4d0`. The latter can revoke keep-state0x20 after
`CheckHasTitleWillNotBePromoted (+1000650)` reports a problem and switch to
rebuild-state0x100. On a further conditional path, AppInfo reads recovery
**target1, operation4** at `+17a169d`; a retained corruption byte there sets
AppInfo's corruption flag and records the literal `AppBrowse is corrupted.`
The later registration trace identifies `(1,4)` as **FroceRebuildAppInfo**, not
AppBrowse itself (which is `(1,0)`); see `vsh-local-system-catalog-20261108.md`.
The diagnostic literal alone did not establish operation identity. Operation
execution/order and catalog promotion remain to be recovered before populating
these result slots from local producers.

## Parent integration validation

Integrated the prerequisite user-schema stage as `53923722`, AppInfo base schema
as `0e10e578`, and schema/access-index transaction as `0feda2f7` on ps5-vsh.
Normal `_Build/windows` configuration and `local_appdb_tests` build succeeded
with parallel2, including the pinned host SQLite dependency. CTest passed1/1
(0.82s test time, 0.87s total). A separate invocation supplied the agent's
previously copied `db-contract-snapshot/app.db` and passed the compatibility
and preservation checks as well as the transaction/cancellation controls.

The live psn37 emulator remains the earlier build `3a12861`; it has not been
relinked or restarted for these stages. Production transport activation,
catalog/recovery aggregation, and visible Home acceptance remain pending.
The database producer and managed first-render investigations continue in
their respective isolated worktrees.
