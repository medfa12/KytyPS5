# Local Home SQL read layer and retained assets

Follow-up: `vsh-local-db-preparation-20261108.md` adds native PNG/JSON file staging
and a single-backend mount plan, preserving native managed getter ownership.
The read receipt now also retains the source descriptor bytes for that stage.

## Implemented checkpoint

`runtimeLocalHomeQuery.{h,cpp}` implements `ReadHomeRows` against an existing
copied/candidate AppBrowse database. It returns managed materializer aliases and
typed SQLite values, plus owned source-backed icon bytes, in one read snapshot.
It is a usable **local SQL read layer**, not a serialized RNPS GetItems adapter
or guest activation receipt. `Read` certifies only this read.

Prerequisite chain on this branch:

`8d0defcc -> ccf64776 -> 27dc0a48 -> dbf77516 -> 97f8f6ab -> 501c0712 ->
97ba24f8 -> 98b94224 -> this checkpoint`.

The parent had already integrated the lifecycle/schema/index work and reported
cherry-picking **both** `97ba24f8` and `98b94224`. Parent validation completion
was not observed here. This checkpoint was built/tested only in the startup
worktree, with copied fixtures and at most two build jobs.

## Native-derived Home query

The fixed request shape is actual user ID, space Game/Media, EXSW display,
contentLimit, and Home's twelve requested properties. No arbitrary SQL/order or
filter API is exposed. Profile identifiers use native ten-digit decimal suffixes.

Pinned DB AOT `739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766`:

| Address | Implemented contract |
| --- | --- |
| `+3f220`, `+7810`, `+4512b` | Constructor/Clear default visible filter: `icon.visible=1`. NULL fails it. |
| `+45528`, `+45dd7` | Space masks2/4 and EXSW display mask128; NULL fails these predicates. |
| `+76a0` | LEFT JOIN shared content by titleId, current-user concept and shared conceptmetadata by localConceptId. |
| `+44210` | Inner order: content.sortPriority ASC, CASE NULL pinnedStatus THEN0 ELSE pinnedStatus END DESC, icon.lastAccessIndex DESC. |
| `+4be0c` | contentLimit forces offset0 and SQL limit(contentLimit-1). Zero means SQL LIMIT-1, not an empty list. |
| `+448d1`, `+44b30`, `+4bef1` | Outer SELECT wraps the limited inner query and orders only SortPriority ASC, LastAccessIndex DESC. |
| `+4c23d`, `+85a0`, `+4e190` | Append actual NPXS40071/NPXS40139 if absent, using title lookup without ordinary predicates/limit. No fabricated fallback. |
| `+3f310`, `+4d020` | Expand requested property dependencies, include order fields, and apply native materializer aliases. |

All **32 distinct SQL dependencies** of Home's twelve properties/order are read.
This includes streaming/hub inputs, concept metadata, logo metadata, age/parental
inputs, and sort fields even though Home does not request the latter as output.
The SELECT is in the implementation for inspection. NULL, integer, real, text,
and blob values remain distinct. No NULL catalog field is filled to obtain a
desired sort/filter outcome. Missing LEFT JOIN rows remain nullable.

Native `lastAccessDate` order means **lastAccessIndex**, not lastAccessTime.
Pinned status selects the limited set but does not govern its final display
order. An already-selected Library is not appended again or replaced by another
ordinary row. These behaviors are covered by disagreeing pinned/access fixtures.

## Asset ownership and API boundary

For returned Library rows, the read layer binds their exact guest icon paths to
owned PNG bytes in `hle_system_catalog_source`, read in the same transaction.
Title, expected descriptor directory, shared icon path and any concept icon path
must agree. A missing/mismatched/invalid source fails the entire receipt, with no
partial rows/assets. The reader never reopens a donor icon. Returned vectors own
their bytes after SQLite closes and after the candidate is subsequently changed.

The retained source table is a provenance store produced by the candidate builder,
not a cryptographic authenticity certificate for arbitrarily modified databases.
Only the two Library icons currently have bindings. Other assets, timestamp path
variants, and a guest VFS consumer remain to be implemented. PNG validation is
signature/size validation, not complete image decoding.

`max_rows` defaults to4096 and fails with `ResourceLimit` rather than truncating a
successful result. Negative content limits, invalid spaces and negative users are
rejected; zero/one are supported as above. Missing profile/schema is `SqlError`,
never another user's fallback. Opens are read-only and never create a database.
This API has no byte-budget/authorization/guest lifetime layer and is currently
intended for trusted copied inputs. It does not claim the complete AppBrowse API.

## Computed-property boundary still open

`ReadHomeRows` supplies native aliases to a future/native managed materializer; it
does **not** treat those raw aliases as final JS properties. Pinned AOT evidence:

- icon0PathWithTs (`+a310`) and pic0PathWithTs (`+6f40`) return stored info.
- sndPathWithTs (`+6ec0`) uses ConvertMetaDataPath (`+6f90`), including the runtime
  system-data path token. BgMatteType (`+5ae0`) maps null/empty to `None`.
- concept path getters (`+5ce0`, `+5d00`, `+5d20`) call ConvertPath (`+5fa0`).
  ConvertPath performs the same runtime-token substitution for `/system_data/`;
  other paths, including the retained Library icons, pass through unchanged.
- Vsh AOT `64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee`
  closure `+652a0` emits conceptInfo's iconPathWithTs, bgImagePathWithTs,
  logoImagePathWithTs, iconUrl, bgImageUrl, logoUrl, description, lastPlayedDate.
  Background output passes through age-gating. Date serialization, complete path
  conversion, CLR NULL/default materialization and streaming/hub getters still
  need end-to-end adapter validation before serialized compatibility is claimed.
- Vsh pinnedStatus closure `+673c0` boxes DB TypeDef28 `PinnedStatusType`, calls
  ToString and creates a **string** JValue. Metadata constants are0=`unpinned`,
  1=`pinned`,2=`notPinnable`; a raw SQL integer/NULL is not final JS output.
- DB CheckAgeGated (`+72e0`) calls the native parental-control API for platform0
  as well as platforms1/2. An unavailable platform field is not evidence that
  age gating can be skipped or forced clear. The raw projection preserves the
  required inputs instead of synthesizing a parental-control result.

`tools/inspect_appbrowse_aot_1360.py` now reproduces method-address names and
JIT/LLVM GOT string resolution directly from the pinned ELF/embedded PE. Requires
capstone, dnfile and pyelftools; no managed code is executed.

## Candidate cancellation and failure retention

`CatalogReceipt.retained_candidate_directory` identifies an exclusively claimed
private directory even on failure. Empty means the call acquired no ownership.
`input_backup_complete` reports completion of the SQLite backup only; an earlier
failure can leave an incomplete private database. `candidate_app_db` and projected
record counts remain success-only. Failed directories are retained, not deleted
or automatically reused, and the operation never opens an established destination.

Cancellation is cooperative through the final flag check before COMMIT. Progress
cancellation is disabled after that boundary, so successful COMMIT owns its
receipt despite a later flag change. Real SQLite failures retain their SQL error;
an unrelated cancellation flag no longer masks them. This atomic-flag interface
is not a linearizable Stop or rollback-after-COMMIT API. COMMIT contention returns
SQLITE_BUSY immediately, rolls the projection back, and retains the private
candidate; there is no retry/fairness policy.

Test-only SQLite auto-extension tracing flips cancellation *inside the real SQL
COMMIT*, after the producer check, without sleeps or production test hooks. Other
controls cancel during projection, reject COMMIT with a SQLite commit hook, and
hold a second-connection read snapshot through COMMIT to cause SQLITE_BUSY. Tests
verify committed rows or complete projection rollback and accurate retained paths.

## Verified commands/results

Working directory for build and inspectors:
`${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/vsh-home-startup`.
The local build recipe initializes VS18 x64 build tools and invokes CMake/Ninja
on `tests/local_appdb`, using the worktree's SQLite3.50.4 amalgamation and `-j2`:

```powershell
python -B _TempData/build-local-appdb.py
```

Exact test commands, from that worktree's `_Build/local-appdb`:

```powershell
./local_appdb_tests.exe
./local_appdb_tests.exe ../../_TempData/db-contract-snapshot/app.db ../../_TempData/db-contract-snapshot/system-catalog
```

Both passed. Copied tests exercise both actual users268500996/268500997 and both
spaces, all Home dependencies, NULL/LEFT JOIN behavior, two-stage order/limit,
Library bypass/absence/deduplication, exact retained source PNG bytes, ownership
after database changes, cancellation, missing profiles/files, malformed source,
resource rejection, projection rollback and actual failed/contended COMMIT.
Older schema/AppInfo/recovery tests also pass in the same executable.

Example reproducible read-only evidence command:

```powershell
python -B tools/inspect_appbrowse_aot_1360.py db ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib/Sce.Vsh.Accessor.Db.dll.sprx 3f310:4f00 44210:c20 4bcf0:720 4d020:810
```

## Activation blockers

The genuine copied `app.db-corrupt` still requests ForceRebuildAppInfo `(1,4)`.
That remains distinct from AppBrowse `(1,0)`. The existing recovery class retains
callback observations, not complete native wrapper results. Promotion extraction,
distinct force-report post-hook cleanup, operation-specific preparation aggregation,
commit-to-event publication and guest asset/SQL binding are still pending. No
schema/catalog/read receipt discharges these prerequisites. Native migration,
existing-profile reconciliation and complete installed inventory also remain open.
AppInfo index handling remains narrower than native; candidate triggers and
non-BINARY shared identity index collations remain unsupported/rejected.

The user's animated-background/Home gate remains unproven. These offline reads
establish a nonempty source-backed SQL result, not an observed RNPS request,
serialized GetItems response, selected-space experience list or interactive tile.

## Parent integration validation

Integrated `53c98b7b` as `e7f4c96d` on ps5-vsh after the catalog/recovery
prerequisites. Normal `_Build/windows` target `local_appdb_tests` built with
parallel2. CTest passed1/1 (1.10s test, 1.18s total). The separate invocation with
both startup-worktree `db-contract-snapshot/app.db` and `system-catalog` inputs
passed Home query controls for both actual users/spaces, source-asset ownership,
NULL/LEFT JOIN and two-stage ordering/limit behavior, plus real rejected/busy
COMMIT and cancellation-boundary controls. Existing recovery/schema/preservation
checks also passed.

Parent independently ran the pinned DB AOT inspector over `44210:c20`,
`4bcf0:720`, and `4d020:810`; output is retained at
`${USERPROFILE}/AppData/Local/Temp/opencode/home-query-parent-aot-20261108.txt`.
It resolves the pinned-status CASE expression, SortPriority/LastAccessIndex
aliases and limit/offset SQL literals from the original regular image.

The producer agent continues complete recovery-wrapper/preparation integration.
Managed final serialization and guest activation remain pending. Production was
not relinked or launched by this validation; further Home tracing awaits the
separate debugger teardown correction documented in the tracer report. Passing
these tests does not establish visible Home or resolve its live first-render gate.
