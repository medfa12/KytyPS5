# Local database producer feasibility: independent of signed admission

## Result

A local HLE SQLite implementation is feasible for the recovered native schema
fragment: exact regular-13.60 DDL executes on a memory clone of the actual psn23
database, preserves every existing table's columns/rows, and rolls back cleanly.
This is executable evidence independent of signed native admission. It is **not
proof of a complete supported schema, recovery, or usable Home**. No runtime HLE
producer was installed: the remaining migration/reconciliation and completion
contracts below are necessary before that would be faithful.

The current input is structurally valid SQLite but not a supported native-ready
database. Creating the recovered base tables alone changes the Home revision
query from `no such table: tbl_version` to **no matching row**, not a real revision.
Filling that gap with a made-up revision or setting a sync bit would be wrong.

## Important correction: sync management is a kernel event

`SceAppDbSyncManagement` is **not an IPMI endpoint**. Core `+fe41b0` constructs a
singleton with that name (`+fe4213` copies the literal at `+1df0cce`). Its
`Initialize +fe4340` creates a named kernel event flag at `+fe437c` with attributes
`0x110`, initial bits zero, then clears it at `+fe439e`. The handle is stored at
object `+8`; nonzero means already initialized. `+fe43f0` deletes it; `+fe4420`
sets the caller-supplied bits through the kernel, reporting actual errors.

Core `+ff199b` first initializes the AppInfo client, then `+ff19ac..+ff19b4`
gets/initializes this event. This occurs **before** corruption admission and
database open. Its presence must not be interpreted as successful database work.
The event initializer's return is not tested at this call site. This is separate
from `SceAppInfoIpc` (client connection code `+1024e10` onward).

Several later AppInfo operations set distinct bits: calls at `+fbf368` pass8,
`+fc5ced` pass4, `+fc5cff` pass0x20, `+fd93c4` pass0x10. These locations prove
operation-specific signals, **not their names or full success conditions**;
their transaction/consumer associations still require tracing. In particular,
creating an always-signaled event to cure client initialization is unjustified.

## Preparation and recovery state are not one success boolean

The preparation callback `+27d670` interprets the 16-byte notification:

- First DWORD other than1 enters initialization.
- First DWORD1 enters it only when byte8 is1 and DWORD12 is4; that path first
  calls `+f99c00` with DWORD4. Other combinations return0 without initializing.
- It obtains the daemon at `+ff1df0`, calls `+ff1780`, retains that return, installs
  callbacks, and only on a nonnegative result calls `+b23370` with object+0x28 and
  zero. That helper dispatches a preparation-owner virtual method at slot+0x10;
  the full status aggregation/0x160008 publication is not yet reconstructed.

Within `+ff1780`, AppInfo allocates/binds an accessor, initializes its client and
sync event, checks corruption at `+ff19d5 -> +ffe830`, and normally opens with
flags2 at `+ff1a19 -> +10227c0`. Corruption result `0x80f50061` sets daemon status
`0x100` and joins the later initialization tail. An accessor-open failure also
sets0x100. Successful open runs `+ffe8b0` version checking. The final result comes
from recovery notification `+ff1bb0`, so callback return/event existence cannot
stand in for a successfully opened accessor or clean recovery status.

The previous lifecycle checkpoint supplies the bounded recovery protocol:
0x150011 input `{category,operation}`, output `{status,isCorrupt,promoteType}`;
UI selects `(1,5)`, `(0,10)`, conditionally `(0,14)`. Only selected reset reaches
UI `+55e640`; normal opening has no CREATE callback. The existing corruption
marker remains authoritative until a supported recovery operation resolves it.

## Recovered UI schema/revision sequence

All offsets here refer to the original regular UI, not the dimension-patched
stage. Reset `+55e640` supplies flags6, callback `+55f7e0`, and version tuple
including constant `0x00ccc5e0` and -1. It is not a generic always-reset policy.

`runCreateSchemaTransaction +55c5e0`:

1. Begins the real accessor transaction (`+55c61a`).
2. Checks schema/version at `+55c637 -> +55c910`.
3. On the version-dependent path invokes the supplied schema callback (`+55c694`).
4. Only after its zero return writes version metadata (`+55c6a1 -> +55cc40`),
   then calls revision handling (`+55c6bc -> +55de70`).
5. Ends the transaction through `+55ca90`, carrying the operation result. A
   failed transaction begin also communicates through the separate status bits;
   treating only the function's integer return as readiness is insufficient.

The schema callback `+55f7e0` invalidates caches via `+5516f0`, then tail-calls
`+549ed0 -> +54a900`. This executes the exact literal at `+f6b2ac`:

```sql
CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));
```

It next calls `+54a960` with a null user-list pointer. That function obtains the
native local-user list (20 entries, skipping -1), then calls `+54d250` for each
user. The latter assembles native literals at `+fae119` and `+f64c96`:

```sql
CREATE TABLE IF NOT EXISTS tbl_info_<decimal-user-id-width-10>(key TEXT NOT NULL,value TEXT,PRIMARY KEY(key));
```

Name generation `+54e930` copies `tbl_info`, writes underscore at+8, and formats
`%010d`. The other calls in `+54a960` invalidate/initialize additional managers;
this probe does not claim to implement their catalog/table producers.

Revision helper `+55dc70` prepares
`SELECT category, status FROM tbl_version WHERE category = ?1`. End-of-results1
is normalized to success (`+55de2a..+55de31`), leaving the caller's initialized
value when no row exists. `+55de70` reads **access_index**, then uses the literal
`REPLACE INTO tbl_version VALUES('%s', %ld)` to write that value; the schema
transaction initializes its output to zero. This is not the managed Home
**table_access_index** category. Neither may be copied/invented as a substitute
for the other. The offline probe intentionally writes no version/revision rows.

## Actual copied-input experiment

`tools/probe_vsh_db_schema_1360.py` verifies the complete original UI hash, reads
the exact native DDL literals, opens **copied** inputs read-only, and backs up
SQLite into memory. It exercises rollback and commit of the recovered DDL only.
Assertions compare all old table columns and row-content hashes, verify schema
rollback, SQLite integrity, and unchanged disk input hashes. No guest code runs.

Snapshot under this worktree: `_TempData/db-contract-snapshot`.

- app.db SHA256 `76c7fc464066809d525afd72ed96976418268e6467dcfca72289bd5a2f17aa6b`.
- app.db-corrupt SHA256 `c3aad1721fc8260080213fc6b2a94e82ce480a4ff346e30fd08c4d321788bf85`.
- users.json SHA256 `d852887388225cc6e55c043051846a5068c08581f4d6016737f9ee7509af6e25`.
- Integrity check: `ok`. Existing34 tables preserved. They include contentinfo,
  conceptmetadata, and icon/concepticon tables for decimal IDs268435457 through
  268435472. Actual stored profiles are268500996/268500997 (0x10010004/5): the
  provisioned table suffixes are not these current profiles.
- Only new memory tables: empty `tbl_version`, `tbl_info_0268500996`, and
  `tbl_info_0268500997`. No catalog/profile rows or revisions synthesized.
- Before fragment: Home revision SQL fails `no such table: tbl_version`.
  After fragment: it returns zero rows. `database_ready` remains false.
- Invalid-SQLite negative control exits1; its disk bytes remain unchanged.
- Wrong-image disassembly negative control exits1 at the full-image hash gate.
- Source app.db, marker and profiles still byte-match their copied snapshots.

Reproduce after making copies of the database and any journal/WAL sidecars:

```text
python tools/probe_vsh_db_schema_1360.py <original-ui> <copied-app.db> <copied-users.json>
python tools/inspect_vsh_db_contract_1360.py core <regular-core> 27d670:78 ff1780:430 fe41b0:2b0
python tools/inspect_vsh_db_contract_1360.py ui <original-ui> 55c5e0:f0 55f7e0:24 54a900:140 54d250:400 54e930:100 55dc70:3c0
```

Both tooling inputs are pinned to Core SHA256 `6870113e...9250a` and original UI
`cba9fd38...1a09` (full hashes in the inspector). The probe is diagnostic tooling,
not provisioning: no persistent database output or readiness API exists.

## Smallest remaining contract before a runtime local HLE producer

The missing signed metadata is **not** the prerequisite for executing local HLE
SQL. The demonstrated alternative's prerequisites are now concretely narrower:

1. **Compatible non-destructive migration/reconciliation:** finish Core AppInfo
   schema/version validation (`+ffe8b0 -> +1004a80`, accessor `+10227c0`) and UI
   version metadata `+55cc40`. Establish what source tables/records must exist
   for current-user catalog production. The existing managed-reader seed and
   the recovered three empty tables do not establish that contract.
2. **Recovery operation selection/status aggregation:** map the preparation
   owner virtual call behind `+b23370` and 24-byte0x160008 status to the real
   recovery results consumed by0x150011. A valid SQLite file and a corruption
   marker can coexist, as these inputs demonstrate. Automatic reset/marker
   deletion cannot be justified by SQLite integrity alone.
3. **Commit-to-observer contract:** associate Core event-set callers above and
   UI current-user table/revision writers with their actual committed work and
   consumer masks. Distinguish kernel-event existence, preparation callback
   completion, recovery success, and catalog/revision availability. Preserve
   errors and profile-specific ownership on restart/cancellation.

Until those three boundaries are recovered, a production HLE that announces
success would exceed the evidence. The local SQL route is technically viable
at the demonstrated schema/transaction boundary; end-to-end native-compatible
recovery remains unproven. This checkpoint provides a reproducible experiment
and precise native producer targets rather than another credential dependency.
