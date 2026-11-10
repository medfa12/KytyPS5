# Complete Force Eager wrapper, preparation aggregation and native file staging

## Checkpoint scope

This continues `53c98b7b` in the startup worktree. New production code:

- `runtimeLocalDbPreparation.{h,cpp}`: initialized, pass-owned ForceRebuildAppInfo
  **Eager wrapper**, its separate Estimate result, result-wire encoders, mode1
  operation accounting, and the selected preparation-producer accumulating loop.
- `runtimeLocalNativeCatalog.{h,cpp}`: stages exact retained PNG/JSON files and
  concrete native mount inputs while retaining **one candidate app.db backend**.
- `runtimeLocalHomeQuery` additionally retains original descriptor JSON bytes in
  the same snapshot as its existing SQL rows and PNG bindings.

No callback bool or schema receipt is accepted as a complete operation result.
The full multi-operation recovery/preparation path is **not ready for activation**:
Core AppInfo and other native operation producers remain unavailable. Native
Promote execution, its timed result and the complete composite preparation status
are not fabricated. Existing `EagerCheckPass` remains a read-only observation API;
`PreparationPass` is the separate wrapper/lifecycle API.

Parent reports integration through `47c9d900`, source tests passing, and checkpoint
`b73df526` recording tracer residual DR7=55 in psn38. This work did not inspect or
modify tracer tools, control processes, or launch/attach to an emulator.

## Resolved native promotion extraction

Pinned regular UI SHA256:
`cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.

Force vtable `+11a7100` slots:

| Slot | Address | Meaning |
| --- | --- | --- |
| +0x20 | +88ee60 | Initialize object fields+49c/+4a0/+4a4 to2. |
| +0x30 | +8932c0 | Eager callback reads configured AppBrowse report; presence AND NOT secondary. |
| +0x38 | +88ef50 | Post-check resolves this object's distinct report path and removes its report. |
| +0x40 | +893390 | Estimate returns0 and writes1 to both output DWORDs. |
| +0x70 | +88e810 | Return **object DWORD+4a0 == 1**, not its raw nonzero value. |
| +0x78 | +8933d0 | Resolve configured directory plus `/force_rebuild_appinfo`. |

Constructor `+893250` also initializes all three fields to2. Force's Eager callback
does not write those object fields. Thus initialized/reused Force yields promotion
**0**, even when an AppBrowse report has secondary1. Copying report.secondary into
this result would incorrectly substitute the shared Eager callback for Force's.
AppBrowse's internal `m_promoteType` at+4e8 is also not this wrapper getter field.

Wrapper `+895c60` saves callback status, calls slot+70 even on callback failure,
writes its zero-extended byte as the output promotion DWORD, then runs slot+38.
It returns the saved callback status, not post-hook status. `+895380` subsequently
stores `{status@0, corruption byte@4, promotion DWORD@8}` in the result store.
Local wire encoding explicitly initializes padding and publishes only a completed
local wrapper. Host read errors have no invented native errno.

Estimate uses separate retained state and a separate12-byte wire encoder. UI
`+889691` selects it only after Eager status0/corruption1; Force writes1/1 and
wrapper `+895cc0` clamps each output to at least1. Rechecking Eager invalidates the
local Estimate receipt; absent/failed/clean Eager cannot create an Estimate result.
Stop invalidates both slots and is terminal. No result is reused across passes.

## Distinct force-report lifecycle

Post-check `+88ef50` resolves slot+78 then calls `+7bba30 -> +7b72a0 -> +7b96e0`.
The latter appends `-corrupt`, checks existence, and removes that file. It is
**not** `app.db-corrupt`. The copied AppBrowse marker remains authoritative and
still requests ForceRebuildAppInfo on the actual fixture.

`PreparationPass::Initialize` requires an explicit copied force-report location,
including when it is known absent. Unknown location/absent parent is unavailable
or an input error, not clean evidence. Sources must be regular files; the force
source cannot alias the AppBrowse database/report. It exclusively claims a new
private directory and retains the exact source bytes as `force-report.input`,
plus a mutable `force_rebuild_appinfo-corrupt` copy. Failed private directories
remain identified in receipts. Established directories are never reused.

Eager post-check removes only that owned mutable copy. The archive and both source
markers remain intact. Repeated initialization does not recreate the removed
copy; repeated Eager still reads the AppBrowse report and observes absent private
force report. Post-check runs after host callback failure too, matching native
ordering. The local API is deliberately stricter on post-hook host I/O failure:
it exposes `PostCheckStatus::Failed`, withholds a complete/wire result, and does
not translate the failure to a guessed guest code. Native itself ignores the
post-hook return in the Eager wrapper.

Inputs must be stable private copies; this is not a hostile concurrent filesystem
sandbox. A copied force-report source is capped at1MiB without silently dropping
trailing evidence. Cancellation suppresses late result publication; Stop waits
for active wrapper work. File side effects already completed in the exclusively
owned workspace are retained as diagnostics, not undone on original files.

## Preparation accounting and genuine missing prerequisites

`RunMode1` accounts for mode1/filter0 in exact order:
`(1,4) -> (0,10) -> (1,5) -> (1,0) -> (0,1) -> (1,1)`.
Force has a complete Eager result after its tail. Remaining entries return named
unavailable prerequisites, not Force's result. The copied marker yields
`recovery_requested=true`; the first unavailable operation is Core AppInfo `(0,10)`.
No native registered-producer registry or readiness certificate is asserted.

Pinned Core SHA256:
`6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`.

`RunSelectedPreparationProducers` implements the accumulating branch of `+b24380`:
pass the16 notification bytes unchanged, inspect **any nonzero** producer return,
continue through the selected list, and retain the first nonzero accumulated
**descriptor record+0x28** failure code. That code is not necessarily the callback
return. A descriptor error0 leaves the accumulator available for a later failure,
matching `+b24435..+b2443f`. Invalid notification fields at offsets0/12, missing
bindings/codes, absent returns, host exceptions, empty registry and cancellation
are explicit outcomes. `SelectedListComplete` certifies only the supplied list.
Production bindings must retain the actual native execution owner/lease; none
are installed by this checkpoint. Tests use explicitly synthetic bindings.

The native24-byte `0x160008` value still requires real composite owner state:
`+b27c00` copies owner+10..+27, notifier completion overlays byte1, and `+b28150`
overwrites DWORD+14 with `+17e76f0` state. The selected-list runner cannot generate
this record, observer completion, revision increments or event masks.

## Concrete native connection points

1. **Recovery results:** local `ReadEagerWire` matches the12-byte payload consumed
   by `0x150011`. Invalid/unavailable/failed/cancelled lookups leave output untouched.
   Core's actual getter `+17a24c0` and setter `+17a22c0` remain the native store
   connection points; Estimate has an independent store/encoder. No new IPC
   command number is guessed for it. There is no generic success-result setter.
2. **Identity/dispatch:** `runtimeAppDb.cpp::InstallSourceBoundAppDb` already installs
   the AppDb, DbPreparation and DbRecovery adapters. Its `invoke_guest` validates
   the actual Core-owned dispatcher thread, process identity, executable target
   and retained execution lease. A local pass must not bypass that by invoking a
   Core server operation on a UI/client thread. No such bypass was added.
3. **Managed materialization:** `StageNativeCatalog` prepares a mount input for
   `/system_data/priv/mms` pointing to the **existing candidate's parent**, plus
   read-only inputs for the two exact Library descriptor directories. No second
   app.db, managed object, JS serializer or timestamp is generated. Both PNG and
   param.json files come from retained source bytes. Thus native SQLite and
   `Sce.Vsh.Accessor.Db`/Vsh getters can consume the same backend when admitted.
4. **Guest scope/lifetime:** the concrete eventual filesystem entry point is
   `FileSystem::MountForProgram`, under a retained native owner and coordinated
   Core/UI namespace. `StageNativeCatalog` returns inputs only; it does not mount
   or authorize a caller. Profile selection, multi-process mount lifetime and
   rollback/retirement of those mounts are still activation prerequisites.

Staging requires both Library assets and a real current-user catalog. It retains
Games/Media SQL/source receipts and rejects an existing asset directory. Staging
assumes a stable candidate; it detects conflicting duplicate source bindings but
is not a concurrent multi-space snapshot/admission protocol. The original
`app.db-corrupt` still blocks native accessor admission. Source staging therefore
must not be mistaken for successful native managed materialization or Home render.

## Verification and exact inputs

In `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/vsh-home-startup`:

```powershell
python -B _TempData/build-local-appdb.py
```

This isolated VS18/clang-cl/Ninja recipe builds with `-j2`. From its
`_Build/local-appdb` directory:

```powershell
./local_appdb_tests.exe
./local_appdb_tests.exe ../../_TempData/db-contract-snapshot/app.db ../../_TempData/db-contract-snapshot/system-catalog
```

Both passed. Real copied app.db SHA256 is
`76c7fc464066809d525afd72ed96976418268e6467dcfca72289bd5a2f17aa6b`.
The source catalog is the existing copied `system-catalog/{default,NPXS40139}`.
New wrapper controls use an additional disposable copy of that DB (or a report-only
file in the normal run), exact known magic01/02 marker bytes, **synthetic distinct
force-report bytes**, and an explicitly absent force report. No genuine distinct
force-report source was supplied, so its real runtime presence is not claimed.

Controls cover completed/reused Force wrappers, callback failure with post-tail,
post-hook failure without fabricated wire success, report-source alias rejection,
Estimate selection/layout/invalidation, untouched unavailable output buffers,
ordered missing-operation accounting, producer descriptor versus callback errors,
Stop races/pass isolation, exact staged source files, one backend and retained
original DB/markers. Existing schema/catalog/Home/cancellation tests also passed.

Read-only evidence reproduction:

```powershell
python -B tools/inspect_vsh_db_contract_1360.py ui ${USERPROFILE}/Downloads/13.60/system_ex_b/app/NPXS40087/eboot.bin 893250:180 895c60:9d 88ee50:180 88e810:20 893390:40 7b96e0:140 --slots 11a7100:11
python -B tools/inspect_vsh_db_contract_1360.py core ${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf b24380:160 b27c00:80 17a24c0:1f0
```

Prerequisites remain `97ba24f8 -> 98b94224 -> 53c98b7b` atop the previously
integrated schema/index chain. This checkpoint does not discharge native migration,
AppInfo installed inventory/version policy, other operation wrappers, Promote,
producer enumeration, composite preparation/event publication or Home's observed
interactive-tile gate. Narrower index handling and unsupported trigger/collation
behavior remain as documented in the earlier checkpoints.

## Parent integration validation

Integrated `bcc0b34e` as `6af05a9a`, preserving the earlier parent validation
notes when resolving a documentation-only conflict. Normal `_Build/windows`
target `local_appdb_tests` built with parallel2; CTest passed1/1 (1.01s test,
1.12s total). A separate invocation supplied both startup-worktree copied inputs:
`_TempData/db-contract-snapshot/app.db` and `system-catalog`.

The copied-input run passed Force Eager wrapper/private post-report lifecycle,
native wire layout, unavailable-operation handling, selected producer aggregation,
Stop, exact staged PNG/JSON bytes and single-backend plan controls. Existing Home
query, catalog COMMIT/cancellation, schema and preservation controls also passed.
Distinct force-report cases remain synthetic copied inputs/known absence, not
observations of the real runtime force-report path.

Core AppInfo `(0,10)` prerequisites and remaining operation producers, Promote,
composite preparation publication and guest mount lifetime still block activation.
The current whole-directory MMS mount plan also needs namespace review before
use so unrelated databases and sidecars remain available. The producer agent is
continuing that work. No candidate was mounted and no original marker removed;
active psn39 remains production `3a12861`. These results do not establish usable
Home or complete recovery.
