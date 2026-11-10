# Local recovery observations and UI execution order

## Parent integration validation

Parent integrated the catalog prerequisite `97ba24f8` as `1e8bfcd2` and recovery
`98b94224` as `47c9d900`. Normal `_Build/windows` target `local_appdb_tests`
built with parallel2; CTest passed1/1 (0.96s test, 1.03s total). A separate
invocation supplied both `vsh-home-startup/_TempData/db-contract-snapshot/app.db`
and its `system-catalog` directory. Recovery truth-table/retention/error/Stop,
genuine Games/Media projection for both users, preservation/rollback, and
source/trigger/collation rejection controls passed. The pinned source inspector
also verified all four descriptor/icon hashes and the original regular accessor.

These results validate the scoped candidate and observation APIs. Full native
GetItems compatibility, complete recovery wrapper results, preparation/events,
and guest asset binding still block activation. The producer agent continues
those contracts. Production remains build `3a12861`; these tests did not relink
the emulator or activate the candidate in the persistent profiles.

Follow-up: `vsh-local-db-preparation-20261108.md` resolves initialized Force
promotion extraction and implements its complete Eager wrapper/post-hook on an
exclusively owned report copy, plus separate Estimate and preparation accounting.
The observation-only API described here remains read-only and distinct.

## Implemented scope

`runtimeLocalDbRecovery.{h,cpp}` implements the read-only report decoder and
the **ForceRebuildAppInfo EagerCheck callback observation**, keyed `(1,4)`.
`EagerCheckPass` binds one copied AppBrowse input to one pass. A completed check
retains its evidence and callback bool. Failed rechecks replace earlier results
with a host-domain failure and no bool. Getters never create observations;
dependent operations remain unavailable. Stop is terminal, clears retained
results and prevents late publication. A new pass has an independent empty store.

This is deliberately a callback observation, **not the complete native12-byte
EagerCheck result**: promotion-state extraction and the post-check hook have not
been implemented. There is no guest getter/transport registration, readiness
aggregation, marker removal or event publication. There is no generic result
setter that can turn a catalog/schema receipt into recovery success.

The copied DB must be an existing regular file, and an existing report must also
be regular. Host I/O failures have no fabricated native errno. These admission
rules are narrower than native (which also has a process-global force override
and can log some stat/read failures while returning0). The API is for stable
private copied inputs; it does not snapshot concurrently changing live files.
It does not certify SQLite integrity or installed inventory.

## Three distinct report outputs

Pinned UI SHA256:
`cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.

`+7bba10 -> +7b6f90 -> +7b93f0` returns **presence, primary, secondary**.
The original primary/secondary description remains correct for shared checking,
but does not describe the separate presence output. With a report present,
presence and primary initialize to1; secondary initializes to0. Header decoding
at `+7b967d..+7b96cc` modifies primary/secondary only:

| Report | Presence | Primary | Secondary | Shared OR | Force callback |
| --- | ---: | ---: | ---: | ---: | ---: |
| Missing | 0 | 0 | 0 | 0 | 0 |
| `0xfee10000` | 1 | 0 | 0 | 0 | **1** |
| `0xfee10001` | 1 | 1 | 0 | 1 | **1** |
| `0xfee10002` | 1 | 0 | 1 | 1 | **0** |
| Short/unknown | 1 | 1 | 0 | 1 | **1** |

Only the first4 bytes are read; trailing bytes do not change the flags. Unknown
encoding is retained explicitly, with the conservative native flags, rather than
being mislabeled recognized/clean. A short readable file is different from an
I/O failure.

Force callback `+8932c0` initializes its bool0, obtains the configured AppBrowse
path via `+54ee90`, invokes the report reader, propagates a negative reader return,
then sets bool1 precisely when **presence != 0 && secondary == 0**
(`+89332e..+89333c`). It does not test primary. The configured path's initializer
`+54ec10`, used by `+54f450`, appends the actual `priv/mms/app.db` suffix at
`+54ecfa..+54ed46`; it is not Core's `appinfo.db`.

The actual copied report `01 00 e1 fe` therefore produces a genuine **force
rebuild requested** observation. The Library candidate cannot discharge it.

## Operation objects and dependencies

UI DT_RELA array `+138f920` selects target1 objects by operation ID. The following
table combines the pointer array, constructors, vtables and callback dispatch:

| Key | Object | Vtable | Eager callback (+0x30) |
| --- | --- | --- | --- |
| `(1,0)` AppBrowse | `+26c7a70` | `+11a6c20` | `+88e5e0` |
| `(1,4)` FroceRebuildAppInfo | `+26c8d78` | `+11a7100` | `+8932c0` |
| `(1,5)` AppInfoShellUI | `+26c75c8` | `+11a6fd0` | `+892340` |

Constructors are called at `+88d889`, `+88d897`, and `+88d9eb`.
`+895310`/`+895af0` select the operation object using descriptor operation ID.

- AppInfoShellUI `+892340` reads retained Core AppInfo `(0,10)`. A retained
  corruption bool1 clears its own bool and returns0. Unavailable results fall
  through to the AppInfo service's version check (`+413c80`) against `0x6eef928f`.
  Service errors `0x80f50003`/`0x80f50005` are accepted by that branch; other errors
  propagate. This is not justification for stamping that version locally.
- AppBrowse `+88e5e0` reads `(1,5)` and `(0,10)`, then conditionally M2Storage
  `(0,14)`, and performs its own shared report check. It derives promotion state
  using these retained fields and its own error state. Its result cannot be
  substituted with ForceRebuildAppInfo's bool or a SQLite integrity result.
- Core AppInfo's dependency on `(1,4)` remains as documented in the earlier
  AppInfo checkpoint.

## Mode-specific order is now linked to dispatch

`+88d670` selects descriptor vector `+26c92a0` for mode0 and `+26c92b8` for mode1;
other values return null. The mode1 vector constructed at `+88e10a..+88e2b2` is:

1. `(1,4)` ForceRebuildAppInfo
2. `(0,10)` AppInfo
3. `(1,5)` AppInfoShellUI
4. `(1,0)` AppBrowse
5. `(0,1)` AvContentDb
6. `(1,1)` AsyncStorage

`+889230` loads mode from its argument at `+889335`, selects the vector, and
iterates descriptors in order (`+889390`). `+895af0` filters by its third argument:
zero admits target0 and target1 wrappers; nonzero admits only target1 descriptors
whose byte+8 is1. In mode1 the latter excludes ForceRebuildAppInfo and AppInfo,
leaving AppInfoShellUI, AppBrowse, AsyncStorage. Therefore the full order above is
specifically **mode1 with filtering argument0**, not every recovery invocation.
Which invocation psn37 reaches remains unproven.

The resulting list is traversed for SetEvent, Initialize, and then EagerCheck.
Eager dispatch at `+88959d..+8895a4` invokes wrapper vtable+0x20 in list order.
UI wrapper `+895380` invokes `+895c60`, then saves the returned status and fields
through result-store vtable+0x10 before returning to the iteration. Thus this is
an execution/save order, not merely descriptor registration order. Estimate is a
later traversal and requires status0 and a nonzero corruption bool
(`+889691..+8896b6`).

### Remaining wrapper boundary

`+895c60` calls operation vtable+0x30, extracts promotion state via+0x70 even after
callback failure, calls+0x38, and returns the saved callback status. Force's
post-check hook is shared `+88ef50`: it resolves its own path through+0x78
(`+8933d0`, suffix `/force_rebuild_appinfo`) and calls the report removal helper
`+7bba30 -> +7b72a0 -> +7b96e0`. That helper appends `-corrupt` and removes that
report. It is a different path from AppBrowse's `app.db-corrupt`.

The read-only HLE stage does not run this hook or pretend that the full wrapper
has completed. Exact fresh/reused-object promotion state, this distinct marker's
lifecycle, Core recovery work, and native result-store transport are prerequisites
for guest publication. Preparation, full native GetItems compatibility, and
guest asset binding remain blockers to Home activation.

## Reproduction and verification

All addresses above were inspected read-only with the hash-pinned existing tool:

```text
python -B tools/inspect_vsh_db_contract_1360.py ui <regular-UI> --slots 138f920:6 --slots 11a7100:11 --slots 11a6fd0:11 --slots 11a6c20:11
python -B tools/inspect_vsh_db_contract_1360.py ui <regular-UI> 8932c0:110 7b93f0:2e0 895c60:60 88d670:20 889230:4a0
```

Standalone build with at most2 jobs passed. The copied-input regression passed
the native truth table (including the differing magic00/magic02 cases), short and
unknown reports, trailing bytes, missing-input versus missing-report distinction,
host-error replacement of an earlier success, unavailable dependent operations,
invalid-key handling, independent passes, and Stop against repeated observations.
The actual copied marker produced force=true and remained byte-for-byte intact.
Existing catalog and AppInfo transaction regressions also passed in the same run.
