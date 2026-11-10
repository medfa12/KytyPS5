# Persistent Home versus the historical Home baseline

## Outcome

No runtime fix or usable Home is established by this review. The strongest
specific difference is the per-user AppBrowse catalog, not the Home bundle or
a proven lost callback. The original psn23 store was inspected read-only; no
process was accessed and no runtime store or parent source/build was changed.

## Historical baseline and current profile

* `1660c210:docs/vsh-runtime-20261103.md` records visible Home and actual
  Gallery/Control Center navigation in run12; run13 also navigated to Media.
  `aa05d6ce` retains that report but its new verified playback milestone is a
  **direct YouTube app**, not a new persistent-login Home demonstration. The
  historical run12 launch command and catalog snapshot were not recovered here.
* At `1660c210`, `tools/run_shellui_1360.ps1` normally ran the offline database
  preparer with `--user-id 268435457` unless `-NoDatabases` was selected. It
  passed this same configured user to the guest. Without `-UserStore`, the
  corresponding UserService source explicitly used the configured offline user;
  with `-UserStore` it instead used saved records and started logged out. This
  establishes an available historical path, not the exact flags of run12.
* The old preparer imported the firmware's actual Game Library and App Library
  descriptors into per-user reader tables, alongside diagnostic/default fields.
  It was not a native AppDb schema/recovery implementation. It is unsafe to
  equate its successful reader seed with native database readiness.
* Read-only `mode=ro` / `query_only` inspection of the preserved psn23 app.db
  (SHA256 `76c7fc464066809d525afd72ed96976418268e6467dcfca72289bd5a2f17aa6b`)
  finds `tbl_iconinfo_0268435457` and `tbl_concepticoninfo_0268435457`. The icon
  table contains `NPXS40071` with visible=1, display=146 and `NPXS40139` with
  visible=1, display=148. Shared content identifies them as Game Library and
  App Library with firmware icon paths. There are no icon or concept tables for
  either actual saved user 268500996 or 268500997. `tbl_version` is absent.
  The saved roster's `next_user_number` was 7 at this inspection; no other user
  fields need to be disclosed or copied for this comparison.

## Concrete mismatch in the optional old preparer

The current preparer adds saved user IDs when explicitly invoked with
`--user-store`, but its speculative slot loop still computes
`0x10000000 | number`. `CreateLocalUser` in `src/libs/libUserService.cpp`
instead allocates `0x10010000 + number - 1`, skipping existing IDs and refusing
the upper boundary. The two computations do not address the same IDs: with
next number 7 and this roster, the preparer's first speculative slot is
268435463 while the real next allocation is 268500998. The current saved users
were not covered by those old slots. This explains why those precreated tables
are not a general new-profile bootstrap; it does **not** prove how every
historical table was created or that the optional preparer was run at login.

Do not correct this by blindly running the legacy seed on the active store or
by switching to the configured user. Its speculative records/defaults and
partial schema do not satisfy the native recovery/migration contract, and
the observed corruption report must be preserved. Correct production profile
initialization must follow actual registered user creation and source-backed
catalog/recovery ownership, not invent future identities or readiness.

## What the current Home evidence does and does not show

psn41 recorded same-generation Game and Media item serialization with count 0;
psn42 separately correlated both zero-count responses through concrete Resolve
and first callback dispatch for user 268500997. Static hash-pinned Home bytecode
shows that if these results are consumed and published without replacement, an
empty selected Games list leaves startup's menu animation hidden. With nonempty
data, the separate tile `interactive` signal is still required. Revision
rejection alone falls through to the reader. None of this shows live JS
consumption, the mounted state, or which native exception/path produced zero.

The pinned native `DbItemAccessor.GetItems` disassembly at `+14490` initializes
an empty list before calling its reader at `+1450c`; its exception branch can
return that initialized list after `IsDeletedUser`, or enter distinct registered
user/recovery/error branches. That is a plausible bridge from failed per-user
catalog access to a serialized empty list, **not a live attribution**: the saved
captures do not identify that branch or the exception. The missing current-user
tables therefore make a focused catalog experiment justified but do not by
themselves prove the cause of the live result.

The psn43 executor capture remains inconclusive. Its `executor.bridge`
`read_bounds` failures are not evidence of missing JS delivery; it recorded no
queued-site hits and cannot validate the assumed queued caller/frame for the
call/return hits. Do not repeat it unchanged.

## Next bounded action

Review an **isolated copied-store** comparison, never the original psn23 store:
one control retains the missing current-user catalog; the other uses the existing
source-backed `BuildSystemCatalogCandidate` / `ReadHomeRows` / `StageNativeCatalog`
pipeline for the actual saved roster and original firmware Library descriptors.
Keep the original corruption markers, unrelated MMS databases and sidecars, and
do not advertise the candidate as native-ready or bypass recovery. First validate
offline that the control reports the missing user and the candidate independently
returns real Games and Media Library rows, with original inputs unchanged. The
existing component tests are evidence for this restricted projection, not an
activated native accessor or serialized Home response.

Only if an independently reviewed native-compatible admission/mount path exists,
the parent may compare otherwise equivalent fresh cloned-store runs and observe
same-run user/generation, selected-space serialized items, callback/JS
publication and `interactive`, followed by visible navigation. A candidate that
fails genuine recovery or cannot be safely mounted is a blocker, not permission
to remove its marker, fabricate revision, or claim Home fixed. If the clone still
serializes empty results, attribute the native reader/recovery branch before
adding further bootstrap layers. See `vsh-local-db-preparation-20261108.md` for
the outstanding operation producers, native Promote, composite publication,
and coordinated mount lifetime; none was discharged here.

## Verification

The pinned `DbItemAccessor.GetItems` and user-check disassembly was reproduced
with `tools/inspect_appbrowse_aot_1360.py` against the original 13.60 image.
`python -B tools/test_home_executor_trace_1360.py --original <regular-common-lib>`
passed all 10 offline synthetic controls and the pinned delivery/executor byte
audits. Those tests do not validate the failed live psn43 decoder assumptions.
The psn43 JSONL was read as a completed saved file; no process was accessed.
No compilation, emulator launch, catalog activation, or runtime-store write was
performed by this review.
