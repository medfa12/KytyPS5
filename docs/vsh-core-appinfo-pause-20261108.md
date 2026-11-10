# Core AppInfo / MMS namespace pause checkpoint

## State and constraints

User requested pause before implementation. This checkpoint contains **evidence
and resumption notes only**, following `bcc0b34e` on `vsh-home-startup`. No Core
AppInfo producer, version reader, inventory implementation or namespace change
was added during this continuation. Parent reports `bcc0b34e` integrated as
`6af05a9a`, with passing tests recorded at parent checkpoint `ab6f8192`.

Work only in `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/vsh-home-startup`.
Original profiles/firmware/markers remain read-only; use private copied inputs,
isolated builds with at most two jobs, native caller/management/execution leases,
and one authoritative candidate app.db. Do not activate the current whole-MMS
mount plan. User-owned psn39 PID9200 was not accessed or controlled. No child
agents are running from this task.

## Pinned evidence and newly resolved branches

Read-only image: `${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf`.
SHA256: `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`.
All addresses below are relative to this image. Disassembly used the committed
`tools/inspect_vsh_db_contract_1360.py`, which checks the hash before reading.

### Version source and policy

- `+ff1ef0` returns the native expected tuple: appver `0x6eef928f`, appinfover
  `DWORD[+1f60520] + 26`, upper DWORD1. Read-only extraction from this pinned
  image found the delta **10**, hence expected appinfover **36**. This is firmware
  evidence, not permission to stamp candidate versions. The default16-byte tuple
  at `+1ecbb60` is `(-1, 0, -1, 1)`; `checkDbVersion` separately initializes its
  build-target output to0.
- `+10232e0 -> +1022cb0` reads **Core AppInfo** `tbl_version`. It first queries
  `sqlite_master` for that table, then selects `category,status` for `appver`,
  `appver_shellui`, `appinfover`, `build_target`. Missing table returns0 without
  populating outputs. Native row extraction accepts an integer status type;
  other types become0. Do not silently substitute AppBrowse's version/revision
  contract or interpret native default values as genuine stored rows.
- `+ffe8b0` first requires accessor+0x18. Missing accessor returns `0x80f50003`.
  Version-query failure sets daemon status `0x100`. For positive appinfover and
  build_target1: below26 requires equality with expected appinfover; at/above26
  allows stored <= expected. This eligibility also calls `+f7dda0` before the
  compatible branch.
- `+f7dda0` calls import `+197aeb0`; only return exactly1 delegates to `+f7dde0`.
  That path reads `system_app_demoted_on_cronos` through another service. The
  import's meaning and the complete delegated policy are still unresolved;
  neither may be treated as an unconditional false/true gate.
- In the compatible branch (`+ffea2b`), appver mismatch adds0x40, and an older
  appinfover >=26 adds0x80, to base status0x20. Identical appver/appinfover leaves
  status unchanged. The branch retains old appinfover in daemon+0x10.
- Otherwise `+1004a80` checks suppression entries against current platform
  version assembled by `+1024b70/+1024be0/+1024bf0`. Suppression is true when
  an entry exceeds that current version (signed comparison). If not suppressed
  and stored appinfover >=26 and > expected, status becomes0x20 and keep byte+4
  becomes1. Otherwise status becomes0x100; suppression also invokes `+1005860`.
- `+10048f0` constructs the genuine suppression path
  `/system_data/priv/mms/nobackup/appinfo_suppress_keep_state`.
  `+1004b20` enumerates its directory, filters names through a character-class
  check, and parses accepted names using stream extraction with basefield0x800.
  Finish proving accepted naming/error semantics before implementing the reader;
  an absent source location is not proof of an empty suppression set.

No `appinfo.db` was found by `**/appinfo.db` searches in the startup worktree or
`${USERPROFILE}/Desktop/kytyps5`. This is only a scoped search result, not proof
that the user's actual profile lacks AppInfo. The existing copied fixture remains
AppBrowse `app.db`, not a genuine Core AppInfo source.

### Inventory callback and native producer ownership

- Core Eager `+17a15d0` calls shared report check `+17a1170`. Unlike Force's
  callback, the shared checker writes object primary+49c and secondary+4a0 from
  the report and outputs primary OR secondary. Core then calls `+ffe4d0`; a
  nonzero return latches suppress-schema-downgrade byte+4a8 to1.
- Promotion getter `+17a1e60` is secondary==1 AND NOT suppress. Gate+0x80
  (`+17a1f40 -> +17a1500`) is NOT suppress AND secondary==1 AND primary==0.
  Constructor initializes common states to2 and suppress to0. Preserve these
  distinctions and reused-object state; do not import Force's promotion rule.
- `+ffe4d0` requests inventory only for daemon status exactly0x20 and keep==1.
  `+1000650` invokes the registered callback with **mode7, null data**. Missing
  callback returns `0x80f50000`. Any nonzero inventory return clears keep and sets
  status0x100. Library rows cannot stand in for this inventory result.
- Real preparation registration is `+27d670`: call `+ff1780`, then register
  callback `+27cb00` through `+ff1750 -> +faffa0`. Notification registration at
  `+b23370` only follows nonnegative initialization. Callback `+27cb00` forwards
  modes other than2 to `+27dea0` (`recoveryFunctionShellCore`).
- Read-only jump-table extraction at `+1edb294` resolves modes0..7 to:
  `27e10d,27dfee,27e1ef,27e17d,27e1ef,27e1ef,27dfee,27e19c`.
  **Mode7 is +27e19c**, obtains daemon+0x38 and tail-calls its vtable slot+0x18.
  `+344940` is mode3, not mode7; `_TempData/core-appinfo-inventory.txt` disassembles
  that mode3 target and must not be mislabeled as CheckHasTitleWillNotBePromoted.
- Owner binding `+27cc50` supplies `+20b4508` and **+2090e98** to `+ff1cf0`, which
  stores daemon+0x30/+0x38. Next resolve the object at+2090e98 and its relocated
  vtable slot+0x18 to reach the genuine inventory source/algorithm.

### Promote distinctions that must survive implementation

- `+ffe5d0` receives the Core gate bool: true requests mode0, then mode6; false
  conditionally skips catalog metadata deletion in manufacturing mode, otherwise
  invokes `+febbf0`, then requests mode1. Reset and wrapper pre/post hooks remain
  separate prerequisites; no unconditional private reset is implemented.
- Important correction to any inferred error propagation: **+faffb0 invokes the
  callback, then unconditionally returns0**, including when no callback exists.
  Its callback return is not the same contract as `+1000650`, which returns the
  inventory callback result. A host producer must retain actual invocation and
  side-effect evidence rather than treating this native zero as recovery proof.
- Following that zero, `+ffe7b9` reads dynamic source state using
  `+f7dd70 -> +f7dd90`, constructs the version tuple and calls `+1000730`.
  It then returns saved `r12d`, not `+1000730`'s return. Preserve wrapper/producer
  distinctions; do not stamp tuple constants or certify success without source
  operations. No Promote implementation or timed receipt exists yet.

## Namespace investigation: no safe activation yet

`src/kernel/fileSystem.cpp::MountForProgram` normalizes mount names as directories
and replaces same-point/same-program entries. `Operation::Map` near1026 strips
that normalization through `MountName`, selects longest visible `PathPrefix`,
and maps an empty suffix directly to `mount.dir`. This suggests a file-valued
backing can resolve an exact mount-root lookup, but **does not establish a
supported exact-file mount contract**: suffix descendants, ancestor topology,
directory enumeration, rename/unlink, sidecar creation and retirement remain
unverified. Scope breaks ties only at the same point.

Keep `runtimeLocalNativeCatalog.cpp`'s existing plan unmounted: whole
`/system_data/priv/mms` replacement can hide unrelated DBs and sidecars. Next
design a DB-family route/namespace that preserves fallback to neighboring MMS
services, binds app.db and all admitted sidecars/markers to one private backend,
and shares a retained identity/lifetime across Core/UI. It needs atomic install,
rollback restoring previous mappings, retirement of in-flight handles, and an
enumeration policy. Do not implement this by blindly calling current directory
mount APIs once per file or by copying an incomplete MMS directory.

## Retained evidence and reproduction

Ignored local `_TempData` files are preserved, not committed:
`core-appinfo-policy.txt`, `core-appinfo-sources.txt`,
`core-appinfo-version-query.txt`, `core-appinfo-registration.txt`,
`core-appinfo-callback.txt`, `core-appinfo-dispatch.txt`,
`core-appinfo-eventhandler.txt`, `core-appinfo-inventory.txt`,
`core-appinfo-inventory-owner.txt`, `core-appinfo-initialize.txt`,
`core-appinfo-owner-binding.txt`.

Reproduce the key disassembly from the startup directory:

```powershell
python -B tools/inspect_vsh_db_contract_1360.py core ${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf ffe8b0:1c8 1022cb0:630 ff1ef0:20 10048f0:80 1004a80:a0 1004b20:980
python -B tools/inspect_vsh_db_contract_1360.py core ${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf 17a1170:d0 17a1e60:20 17a1f40:10 17a1500:20 1000650:20 27d670:80 27cb00:f0 27dea0:400 27cc50:30 ff1cf0:10 faffb0:20 ffe5d0:250
```

## Verification and next session

This continuation ran hash-checked read-only disassembly/extraction and clean
`git status` checks. **No build or tests were rerun**, because there are no code
changes. Last locally verified implementation remains `bcc0b34e`: isolated
`python -B _TempData/build-local-appdb.py` (`-j2`), then from `_Build/local-appdb`:

```powershell
./local_appdb_tests.exe
./local_appdb_tests.exe ../../_TempData/db-contract-snapshot/app.db ../../_TempData/db-contract-snapshot/system-catalog
```

Both passed at that prior checkpoint. Copied AppBrowse SHA256:
`76c7fc464066809d525afd72ed96976418268e6467dcfca72289bd5a2f17aa6b`.
Parent's normal/copied validation is reported above, not a new local test run.

Resume at the real mode7 owner/vtable, then obtain explicit genuine copied
AppInfo/version/suppression/inventory inputs. Implement source-backed policy and
operation receipts in `runtimeLocalDbPreparation` (or a focused Core module),
preserving native errors versus host unavailability and full wrapper ordering.
Add meaningful branch/source-preservation controls, run the isolated tests, and
commit code separately. Resolve namespace routing/lifetime before any activation.
Core `(0,10)`, the remaining recovery operations, Promote, full registered
preparation/composite state and guest publication all remain blockers. The
animated-background/interactive Home gate is still unproven; no runtime success
is claimed by this checkpoint.
