# Core mode7 prerequisite producer

Continues `8f2cfca2` atop `bcc0b34e` (parent integrated the latter as `6af05a9a`,
validated at `ab6f8192`). New `runtimeLocalCoreAppInfo.{h,cpp}` implements existing
copied Core version reads, appmeta required-version checks, and the exact gated
keep-state rejection/latch transition. These are local prerequisite operations;
they are not wired as a completed `(0,10)` wrapper or native readiness.

## Actual owner and identities

Pinned Core SHA256 remains
`6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`.
Object `+2090e98` relocates to vtable `+201d0a0`; slot+18 is `+27d330`, which calls
`+366330` and logs `sceAppPromoter::CheckHasTitleWillNotBePromoted`.

`+366330` enumerates `/system_data/priv/appmeta/`, checks it through `+366670`,
then checks `/system_data/priv/appmeta/external/` unless the first check returned
negative. Enumeration `+36eec0` selects non-dot directory entries (d_type4).
The enumeration helper's own return is ignored by native; the local producer
instead requires explicit readable copies of both namespaces. Unknown/missing
source is not evidence of empty inventory.

For these appmeta paths `+36f0a0` chooses existing `param.sfo`, otherwise
`param.json`. Missing metadata/key is skipped. SFO `SYSTEM_VER` invokes
`+377e20`: compare required/current uint32 after masking both with `fffff000`.
JSON `SYSTEM_VER_PPR` invokes `+377ef0`: compare the required uint64's HIGH DWORD
and current Prospero uint32 after masking with `ffffff00`. Too-new returns native
`80a3000d`; negative kernel version lookup returns its actual error.

`+15b5990` chooses the JSON loader by filename. JSON `+15a7a30 -> +15acff0`,
`+15adc3a..15ade64` maps string `requiredSystemSoftwareVersion` through base16
strtoull into `SYSTEM_VER_PPR` type604/length8. Empty or length>32 omits the key.
Local canonical hex parsing rejects unsupported signs/whitespace/trailing junk,
overflow and duplicate JSON fields rather than guessing native exception paths.
SFO parsing is bounded to version101/type404/length4. Malformed/unsupported input
withholds native result (stricter than native paths that skip negative parse
returns). Stable sorted diagnostic traversal can differ from native readdir's
first offending title; it does not claim native enumeration order. Kernel
observations must come from a real retained owner at activation, not directory
names. No version observations were fabricated for production.

The enumerated directory name is a title label, not account ownership, license,
userId or localConceptId. This particular mode7 contains no entitlement query and
does not synthesize a complete installed-games list. The two system Library
descriptors cannot substitute for these appmeta namespaces. Full Home title /
localConcept / user materialization remains the separate native AppBrowse path.

`CheckCoreKeepState` runs mode7 only for status exactly20/keep1. Native nonzero
clears keep, sets status100 and latches suppress-schema-downgrade as in
`+ffe4d0 -> +17a15d0`. Missing host evidence leaves state unchanged and returns
explicit unavailability. This receipt does not execute report post-hooks or
Promote, and has no native Eager wire publication.

## Core version read

Read-only `appinfo.db` query matches `+1022cb0`: table presence plus categories
appver/appver_shellui/appinfover/build_target. Rows retain SQLite types and native
low-DWORD integer conversion; noninteger status maps to0, missing rows stay
absent. `access_index` is not substituted for a version. A read snapshot is used;
missing DB is never created. Version compatibility policy still depends on
unavailable source-backed suppression/platform gates recorded in the pause note.

## Tests and provenance

Verified isolated build with `python -B _TempData/build-local-appdb.py` (-j2),
then from `_Build/local-appdb`:

```
./local_appdb_tests.exe
./local_appdb_tests.exe ../../_TempData/db-contract-snapshot/app.db ../../_TempData/db-contract-snapshot/system-catalog
```

Core controls use explicitly synthetic disposable appinfo.db, metadata and kernel
observations: missing source, table/row/type distinctions, SFO precedence, both
version masks, external namespace, actual kernel error propagation, gated state
transition, host unavailability, duplicates, JSON array-root rejection,
cancellation and byte preservation. Both suites passed after the array-root test.
They are not genuine installed inventory or kernel observations. Existing copied
AppBrowse suite uses SHA256
`76c7fc464066809d525afd72ed96976418268e6467dcfca72289bd5a2f17aa6b` and existing
`system-catalog`; it still passes independently. No genuine Core appinfo/appmeta
copy was supplied in this continuation. No runtime success is claimed.

Reproduce the new source evidence using the existing pinned inspector:

```
python -B tools/inspect_vsh_db_contract_1360.py core ${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf 27d330:30 366330:530 36eec0:340 377e20:1a0 15adc3a:230 --slots 2090e98:1 --slots 201d0a0:4
```

## Live steering and next boundary

Parent's psn40 build3a12861 does not activate local DB stages. It observed Home
user268500997 contexts4/5 GetItems2 without correlated settlements over the bounded
trace; this does not prove permanent pending. Parent supplied retained trace and
sequential thread snapshots; analysis now prioritizes Vsh DatabaseModule Execute,
adapter queue ownership, native accessor completion and promise-entry coverage.
Tracer files remain parent-owned. No emulator/process access occurred here.

Retained `_Runs/psn40/_kyty.txt` pins Accessor.Db at `0xc10000000` (size
`0x1c0000`, line65524) and UILib at `0xce0000000` (size `0xfc000`, line134934).
Scanning every aligned QWORD in all345 retained raw stacks and Windows CONTEXT
records found zero values in either mapped range. Earlier Vsh stack scans also
found none. These sequential, bounded post-detach samples cannot exclude queued
heap state, yielded async continuations, earlier execution or alternate completion.
They do not establish a permanent wait or identify a blocking accessor owner.

Pinned Vsh Execute `+471f0` queues through UILib Default, then `+484b0` selects
the adapter and queues its action; `+48630` invokes that action. Vsh PLT
`+287be4` and `+287068` resolve via AOT dependency image15 to UILib MethodDef124
(`get_Default +4390`) and128 (`Run +48f0`), respectively. AOT dependency order
differs from AssemblyRef order. UILib SHA256 is
`5a3c961361cb50fd20506310a7874865a71f0b38a33afece0f3562bb7c31482a`.
Its worker entry `+2d990` starts an async state machine; its continuation remains
to be resolved. Useful boundaries remain enqueue `+4770`, Vsh adapter lookup
`+484b0`, action `+48630`, GetItems2 `+196cb0` and completion `+196e17`.
Raw promise-address correlation and Resolve/Reject interface/overload coverage
remain incomplete; no settlement absence is promoted to a liveness conclusion.

Full Core recovery/Promote and preparation aggregation remain blocked. Whole-MMS
mount plan remains unmounted: exact-file routing, neighboring DB/sidecar fallback,
rollback and retained native lifetime must be established before activation.

## Parent integration validation

Integrated as `8a667fe6`. Normal `_Build/windows` target `local_appdb_tests`
built with parallel2; CTest passed1/1 (0.69s test,0.74s total). Separate execution
with the startup worktree's copied `db-contract-snapshot/app.db` and
`system-catalog` passed Core controls and all existing recovery, Home query,
staging, transaction/cancellation and preservation suites. Core metadata/kernel
fixtures remain synthetic; this is not genuine runtime inventory validation.

The Home agent continues the UILib asynchronous continuation and Vsh action /
promise-completion coverage investigation using the retained psn40 artifacts.
No guest activation, production relink or usable Home is claimed by these tests.
