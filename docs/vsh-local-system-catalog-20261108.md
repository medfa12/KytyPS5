# Private current-profile Library catalog candidate

## Implemented producer

`runtimeLocalSystemCatalog.{h,cpp}` adds `BuildSystemCatalogCandidate`. It opens
the caller's **copied UI AppBrowse app.db** read-only, snapshots it with SQLite
backup into an exclusively claimed new directory, preserves the corruption
marker, then executes a single source/profile projection transaction. No original
profile path is selected by this API. Existing candidate directories are rejected.

The supported source inventory is deliberately concrete: the regular firmware's
two `NPXS40071/appdb` Library descriptors (`default` and `NPXS40139`) and their
actual icon files. The importer reads JSON values and locale fallback, preserves
the original source JSON/icon bytes in `hle_system_catalog_source`, validates
identity/category/types/duplicate JSON keys, and checks existing shared
`tbl_contentinfo` fields against those source values. Conflicts fail instead of
rewriting shared records. Existing profile catalog tables also fail instead of
being replaced. New per-profile icon/concept rows are committed together for the
caller-supplied real local-user roster.

This is a **scoped HLE Library projection**, using the pinned managed-reader
column contract. It is not native migration DDL, complete installed inventory,
Core `appinfo.db` promotion, or a clean recovery certificate. Unsupported metadata
(entitlements, install flags, dates, remote concept IDs, access indices, ordering)
stays NULL. Local concept identity uses Core `+f82a60`'s `cid:local:` prefix plus
the actual title ID. `visible` is derived from source root-hide bit0; `validFlag`
describes the successfully validated HLE source projection only. Neither flag is
preparation readiness. No `access_index`, `table_access_index`, native version,
recovery result, or guest event is stamped by this import.

## Initial Games is independently populated

The consumer owner's `vsh-home-first-render-20261108.md` establishes that revision
rejection falls through and an empty **selected Games** list leaves menus hidden.
Both space reads participate in Promise.all; aggregate nonzero count is not enough.

Pinned `Sce.Vsh.Accessor.Db.dll.sprx` metadata constants give:

- `DISPLAYINFO_SPACE_SHIFT=1`, GAME=1, MEDIA=2 (masks2 and4).
- `DISPLAYINFO_EXSW_SHIFT=7`, DISPLAY=1 (mask128).
- Root-hide flag HIDE=1 (bit0).

Real `default/param.json` has title `NPXS40071`, name `Game Library`, display146,
and hub `pslibraryhub:main?space=game`: it matches Games + experience-switcher.
Real `NPXS40139/param.json` has title `NPXS40139`, name `App Library`, display148,
and hub `pslibraryhub:main?space=media`: it matches Media independently.
These are genuine system Library entries, not fabricated installed games.

`tools/inspect_local_system_catalog_1360.py` verifies the accessor SHA256 and
these exact copied source hashes before reporting the masks:

| Copied source | SHA256 |
| --- | --- |
| default/param.json | e54f7557211d7958faebb06c78505b5cdbd3052b94413db5403de5aa92486d81 |
| default/icon0.png | aed2443204cf6740c5bdb7d66d82a3a8ec9d8d7ace76b17ee719e8b0f437b2eb |
| NPXS40139/param.json | 43258bbff64dc1abbfaf776550d8601a7a6e3c846e24e41ec13b5696bc3c8282 |
| NPXS40139/icon0.png | c96e519210175afa3c8034198d468df7b3ab52a5ddc4375d61b84523c4a720da |

The runtime API validates source structure, not these firmware-specific hashes;
the offline inspector supplies the pinned provenance of this validation run.

## Checks and activation limits

Standalone SQLite validation builds at most2 jobs and consumes no donor objects.
The copied-input run exercises both actual profile IDs268500996/268500997:

```text
python tools/inspect_local_system_catalog_1360.py <copied-NPXS40071-appdb> <regular-accessor-AOT>
_Build/local-appdb/local_appdb_tests.exe <copied-app.db> <copied-NPXS40071-appdb>
```

Tests check the Games/Media masks and genuine URI/name separately for each user,
coherent shared-content/concept joins, source-byte retention, absence of invented
metadata and version rows, and every original table's typed row contents.
Existing destination, cancellation, duplicate roster, missing source, shared
metadata conflict, unsupported trigger/collation, and second-profile collision
are failure controls. A collision after the first profile's insertions rolls back
the entire projection and source ledger. The existing transaction/backend tests
continue to run in the same executable.

Follow-up `vsh-local-home-query-20261108.md` implements the native-derived Home
SQL predicates, two-stage ordering/limit and all computed-property dependencies,
plus owned guest-path/source-byte bindings in the local read receipt. Final managed
getter serialization and guest VFS activation remain pending. Native ordering
values are not inferred from enumeration. Runtime icon checks validate presence/
signature, not complete PNG decoding. Future activation must establish native
accessor compatibility and finish operation-specific recovery and preparation/
event publication. Existing arbitrary
profile tables require reconciliation that this API rejects. Schema triggers and
non-BINARY shared identity index collations are rejected for this candidate stage;
the older base-schema producer's column/PK checks do not certify all SQL constraints
or collation/trigger semantics. Its AppInfo index policy remains narrower than
native: noninteger existing values are rejected/preserved.

## Further native recovery evidence and correction

Registration descriptors at Core `+179f31e..+179fad2` distinguish:

- `(0,10)` **AppInfo**; object pointer is DT_RELA slot `+209ee20` -> `+226ba68`,
  whose constructor calls `+17a1550` at `+179f1e9`.
- `(1,0)` **AppBrowse** (`+179f4d7`, literal at `+1ecf8a0`).
- `(1,5)` **AppInfoShellUI** (`+179f49c`, literal at `+1ed2660`).
- `(1,4)` **FroceRebuildAppInfo** (native spelling), descriptor `+179f92a`.

Correction to the earlier interpretation: AppInfo's `+17a169d` query `(1,4)`
does not directly query the AppBrowse operation. Its diagnostic literal says
`AppBrowse is corrupted.`, but the operation identity is **ForceRebuildAppInfo**.
These keys must not be interchanged in a retained-result adapter.

The second descriptor sequence builds the order ForceRebuildAppInfo, AppInfo,
AppInfoShellUI, AppBrowse, AvContentDb, AsyncStorage before `+179fad2`; selection
and execution of this sequence still need to be tied to the actual mode.
Core-target objects occupy `+209edd0` (15 pointers); user-target slots at
`+226d1c0` initially share a placeholder. Native registration/callback execution
must be traced before treating any partial local receipt as their result.

Shared corruption-report reading is now resolved through
`+17e88e0 -> +17e3e60 -> +17e62c0`. With a report present, the native reader
initializes the primary flag1 and reads the first4 bytes. Magic `0xfee10001`
keeps primary1; `0xfee10000` clears primary; `0xfee10002` clears primary and
sets secondary1. Short/unrecognized reports conservatively retain primary1.
`+17a1170` derives its corruption byte from primary OR secondary. The actual
copied app.db-corrupt is **01 00 e1 fe**, so SQLite integrity and successful
Library projection cannot make this genuine corruption evidence disappear.

Follow-up `vsh-local-db-recovery-20261108.md` resolves the separate report
**presence** output, UI operation objects and mode-specific execution/save order,
and implements pass-scoped ForceRebuildAppInfo callback observations. It also
records the remaining native wrapper boundary before guest publication.
