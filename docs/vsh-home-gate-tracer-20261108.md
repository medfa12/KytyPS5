# Passive correlated Home gate tracer — parent-run handoff

**Safety correction: the original cleanup is defective. Use the lifecycle-v2
repair documented in `docs/vsh-home-trace-lifecycle-20261108.md`.**
Parent WinEvent1000/dump evidence reports psn38 subsequently crashed at the
Resolve trap+1cba30 with exception80000004. Faulting tid37800 retained all four
tracer addresses and DR7=55 despite its `thread_restored` record at180.187052s.
The304-thread dump has only this thread with enabled DR7 low8; DR6=0 and
EFlags=244 (RF/TF clear). API-success cleanup and `end.cleanup_errors:[]` **did
not prove safe detach**. Dump inspection belongs to the parent; these are its
reported findings, not an independent dump parse.

**psn39 decoder correction:** diagnostic version3 resolves the manifest from
`PUIReactApp+30`, reached through validated AppData/weak-app backlinks.
`PUIReactContext+30` is its nested `Data`, not a manifest. See
`docs/vsh-home-manifest-identity-20261108.md` for pinned metadata/AOT evidence,
real-layout negative controls, parent psn39 results and remount-capture commands.

## Implemented / verified boundary

`tools/capture_home_gates_1360.py` is an opt-in Windows AMD64 external debugger.
Without `--attach` it performs **offline provenance validation only**. It shares
the existing `capture_first_fault.py` debug-event layouts and
`capture_native_hang.py` Windows bindings/context conventions.

The collector uses four **hardware execution breakpoints**, not INT3 patches.
It reads managed objects while the debug event pauses all threads. The only
thread-context changes are DR0–3/DR6/DR7 and the resume flag RF needed to continue
an execution breakpoint. Native AL, other general registers, RIP and guest
memory are preserved. Original debug state is verified before and after detach,
with pending exceptions drained while attached. Existing
debug registers or single-stepping cause refusal. Every existing thread is armed
before the initial debugger breakpoint is continued; subsequent thread-create
events are armed before continuation. Unrelated exceptions are forwarded.
Queued faults at the retired Execute slot are acknowledged with RF and logged as
`retired_probe_event`; they do not extend DB-request coverage past the handoff.

The original checkpoint was offline-only. The parent subsequently completed a
180-second live capture on psn38 (see follow-up below): restoration/detach APIs
reported success, but the later crash invalidated safe cleanup. This agent has performed **no emulator attachment, launch or
process lookup**. Offline validation uses synthetic stopped-memory/context
fixtures, pinned files and the historical psn37 placement log. Neither capture
nor fixtures identify psn37's active gate.

Files:

- `tools/home_gate_trace_1360.py`: bounded typed readers and request correlation.
- `tools/capture_home_gates_1360.py`: provenance preflight and bounded debugger.
- `tools/test_home_gate_trace_1360.py`: offline controls and pinned ABI audit.

## psn38 follow-up: diagnostic ambiguity, not an established ABI failure

Parent integrated the original tracer as `f723e2c7` and supplied the completed
artifact `${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn38.jsonl`.
Read-only artifact SHA256:
`da13f39120fb99c17584e3d32f56e0d27ddc269a0082ce6873168f0fd25aed2d`.
Its historical process identity is PID16000/build3a12861; this is not a current
PID selection or process-state claim.

Verified from the artifact: armed at0.735514s with221 threads;108 additional
thread-armed records;304 restored threads; end180.222074s,84 hits,3615 debug
events, `cleanup_errors:[]`. Hits: Resolve78, Execute1, Dispatch3, Reject2.
There are no correlated requests or outstanding records. All three decoder
errors are `object_type` at100.610863/101.501322/101.547913 on thread1784.
The collector returned exit0 and detach API success, later disproved as safe by
the crash. The parent reports no login-success/Home-mount entry
in its reviewed log interval. This does not prove no Home requests, an empty
Games response, a pending promise, or missing interactive production.

Re-audit against the pinned metadata and AOT confirms Dispatch+555a0 takes
`this` in RDI and an RCTEventEmitter in RSI. +555b8 saves RDI toR14;
+555bb saves RSI to[rbp-38]; +556ef restores that emitter toRDI before the
virtual receiveEvent call. Metadata declares
`ReactPerformanceTimingEvent.Dispatch(RCTEventEmitter)`. The handler constructor
+87704 stores its IReactInstance argument at+10. No evidence supports swapping
registers, loosening type checks, or choosing a new object layout. The old generic
`object_type` could arise at several hops and contains no observed type; the
failing hop and actual class cannot be recovered retrospectively.

The sole Execute hit can be explained **at the collector boundary**: its caller
return address was outside the four decoded entries. In this version that is the
only silent return from request handling; a supported caller would emit a request
or a decoder error, and the artifact's only errors are Dispatch. This does not
identify which excluded operation ran, nor whether its owner was Home or a
pre-login application. Nonselected Execute operations are intentionally excluded
*before* module/context/promise reads. General Resolve/Reject hits without an
observed matching request are also intentionally unassociated. Root configuration,
login and main mount remain outside coverage.

Diagnostic version2 (same parent command, same trap addresses):

- `decode_unknown.diagnostic` identifies the exact decoding `site`, expected
  type, and observed type where a type check failed. Actual names are emitted
  only from a fixed class/namespace allowlist; others become `other`. Null and
  unreadable pointers are distinct. A bounded `type_path` (at most16 entries)
  preserves already-checked hops. No object dump, arbitrary name, or field value
  is added. Unsupported known types remain unsupported.
- `scope_excluded: not_home` requires a readable typed manifest and a valid
  application-name string different from the pinned `rnps-home`. It emits no
  other app name and does not label the event as Login. This check precedes
  launched-user decoding, so ordinary non-Home pre-user events need not be
  mistaken for a missing-user Home layout. Home with no launched user remains
  `decode_unknown: user_unavailable`.
- `scope_excluded: caller_not_traced` records an exact audited method/RVA for
  GetScalar(+474c6), GetIndexById(+47896), GetItemById(+479b1), Insert(+47a96),
  Update(+47b86), Remove(+47c76), or `unrecognized` with no address for other
  callers. Preflight verifies all six direct E8 calls target Execute+471f0.
  This is an operation-scope exclusion, **not** proof of a non-Home owner.
- Each exclusion/error stream has its own100-record detail cap. Aggregate
  `excluded_codes` and probe/site/code/type `diagnostic_codes` remain in `end`,
  alongside the original decoder/hit counters, within the existing hit budget.

The next parent-controlled capture can distinguish a normal non-Home scope
exclusion from, for example, `dispatch.emitter` receiving a different known class,
`dispatch.instance.weak` having an unsupported weak-reference type, or an
unreadable `identity.manifest`. It cannot retroactively classify the three psn38
events. No live replay was performed for this diagnostic revision.

## Parent command

Run in the integrated parent checkout or this independent worktree. The parent
selects the **actual** PID and current run log. No historical PID is supplied.
Start after the four runtime images have mapped and before the Home requests of
interest. The `armed` JSONL record marks the beginning of coverage. All process
control, login/navigation/remount decisions remain with the parent.

PowerShell (64-bit Python):

```powershell
$original = '${USERPROFILE}\Downloads\13.60\system_ex_b\common_ex\lib'
$stage = '${USERPROFILE}\Desktop\kytyps5\_Runs\psn23\stage\common'
$home = '${USERPROFILE}\Downloads\13.60\system_ex_b\rnps\apps\NPXS40002\application.ps.bundle'
$runLog = Read-Host 'Full path to the current run _kyty.txt'
$targetPid = [int](Read-Host 'Parent-verified current process PID')
$target = Get-Process -Id $targetPid -ErrorAction Stop
$created = $target.StartTime.ToUniversalTime().ToFileTimeUtc()
$exe = $target.Path
$exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()
$out = Join-Path $env:LOCALAPPDATA ('Temp\opencode\home-gates-' + $targetPid + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '.jsonl')
python tools/capture_home_gates_1360.py --original $original --stage $stage --home-bundle $home --run-log $runLog --attach --pid $targetPid --created-filetime $created --exe $exe --exe-sha256 $exeHash --seconds 90 --max-hits 4000 --output $out
```

Update `$stage` and `$home` if the coordinated run uses different paths. The
stage files must be byte-identical to the pinned originals. `$home` must be the
actual routed Home bundle; its full hash is checked, but the collector does not
hash the in-memory Hermes heap or prove that this file was loaded by that PID.
The expected PID creation time, executable path and executable disk hash are
checked before attach, and the creation time is checked on CREATE_PROCESS.
The process handle remains open throughout. The log alone never identifies a PID.

The output is created exclusively; an existing file is not overwritten. Ctrl+C
requests orderly cleanup. Lifecycle-v2 uses explicit thread suspensions and
debugger-protected exception draining, including idle timeout. It does not use
`DebugBreakProcess`. Failed verification emits recovery-hold information rather
than successful cleanup. Killing the debugger forcibly bypasses this protocol.

Defaults: 90 seconds, 4,000 hardware hits, 10,000 debug events, 8 MiB output
threshold with 1 MiB reserved for cleanup/outstanding records. Hard argument caps:
300 seconds, 20,000 hits, 50,000 events, 16 MiB. At most 1,024 requests and 1,024
armed threads; each decoded event has a 64 KiB read budget, bounded strings,
64 JObject properties and 12 requested columns. Counts are bounded to 1,000,000.
Each lifecycle drain has a separate 30-second/10,000-event bound (one phase
handoff and one final shutdown at most). These are cooperative debugger bounds,
not a hard deadline for a failed Windows
API or filesystem write. All runtime defaults and executable source are unchanged.

## What the next run can establish

Each observed request carries:

- AppData UID from the actual PUIReactApp (`generation`), launched local `user`,
  bridge and PUI context identities, module identity, numeric database context,
  unique trace request number and promise address.
- Operation identified by the **audited caller return address**, not guessed
  Mono delegate method pointers.
- A condition epoch with actual numeric user/content limit and allowlisted
  `game`/`media`/`display` values. Only the exact Home twelve-column request is
  eligible for a per-space count. `revision` requests are identified separately.

Settlements record native Resolve/Reject **entry**, callback presence, elapsed
observation time, and actual `response.items` JArray length when supported.
No item pointers or item contents are traversed or emitted. Revision values are
read only when the actual resolved object contains a typed integer `revision`.
Rejection messages, account/profile data, arbitrary object strings, object dumps
and registers are not logged. Unrecognized strings are not emitted.

Interpretation:

| Record | Defensible conclusion |
| --- | --- |
| Correlated `resolve`, `space:game`, `item_count:0`, callback present | Native Games response was empty; distinguish this from an outstanding native request. It is not proof of JS publication. |
| Revision `reject` | Rejection entry was observed; the pinned Home bytecode supports fallback, but this collector does not prove the catch ran. |
| `end.outstanding` | This request entered during coverage and no correlated settlement entry was observed before the end. This is bounded **observed-unsettled**, not proof of a permanently pending JS promise. |
| Item count plus `decode_status` | Count may have been read, but association/identity validation failed; do not treat as correlated Games/Media. |
| `decode_unknown`, aborted trace, failed cleanup, exhausted budget | Coverage is incomplete. Missing records cannot identify a gate. |
| `phase:interactive` | Same-generation, same-user, same-module Games count >0 and a Media result were both observed under settled conditions. These are separate native requests, not a recovered JS Promise.all identity. |
| `predicate` | Actual AL from the slot188 call, view/manager identities, current virtual target, generation/user and tag. |
| `dispatch` with `name:interactive` | Native timing Dispatch entry for that generation/tag/emitter was reached. Not proof that Hermes or the tile callback consumed it. |

The fourth hardware slot observes timing Dispatch during active coverage. After both space
results satisfy the native condition above, the Execute slot switches to the
interactive predicate result site; Resolve/Reject remain installed. The `phase`
record explicitly ends DB-request-entry coverage. Each lifecycle drain closes
pending associations and freezes their observed-unsettled duration, marked by
`coverage_closed:phase_drain` or `shutdown_drain` in `end.outstanding`. Guest
objects are not decoded during the drain; missed settlements cannot associate
with later pointer reuse. Phase handoff therefore has an observation gap.
Later requests cannot be
reported as pending. All timing records retain their own generation/user/tag;
`focus_match` says whether they match the generation which triggered the handoff.
A subsequent user switch is not silently merged with that focus.

Promise-address reuse invalidates the older association instead of merging it.
Condition replacement invalidates its epoch. Rejected/pending conditions,
disposed modules, changed user/generation, collected weak handles, unreadable
objects and unsupported layouts never become zero counts. Remount correlation
uses the UID plus context/module/user, not the numeric database context alone.

For a decisive coordinated capture, retain the `provenance`, `armed`, request,
settlement, `phase` and `end` records. An already-mounted Home may have issued its
conditions and requests before attachment: such a trace correctly lacks those
associations. A missing request after attachment cannot prove a pending one.

Root configuration/main-menu mount, current selected-space atom, item-to-
experience conversion, local startup promise, and JS timing callback consumption
are **not covered**. Four hardware slots do not cover those additional boundaries.
If there are no correlated Home requests, this collector cannot distinguish an
earlier root/configuration gate from missed coverage. Native nonempty Games is
not proof that the selected-space atom is nonempty. A false predicate for one tag
is not proof all Home views failed; match generation/tag and the actual dispatch
records. The collector does not identify the tile's JS fiber from a native tag.

## Pinned provenance and ABI

All addresses are **module RVAs**; runtime bases come only from one unambiguous
placement per required runtime image in the supplied log. `Loading:` host paths
must resolve to the supplied stage. Full-file original hashes and exact original
== stage equality are checked before process access. The following hashes are
compiled into the collector:

| Image | SHA256 |
| --- | --- |
| Home HBC89 | `565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f` |
| ReactNative.Modules.Vsh | `64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee` |
| ReactNative.PUI | `a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914` |
| Newtonsoft.Json.PlayStation | `bac49935d17da7be951503c44b5a213d22ac835e67c3ec673375d373a56a03aa` |
| libmonosgen-2.0 | `294af118b930006345855ecc63e452d3fa28a2c138136755c6f2066b15938a40` |
| mscorlib, layout reference only | `c05d3ebb585d8185530793eb259200337fe20b8d656df20cbcd7437437d48ce3` |

psn37 has two corlib instances. Neither is selected as a trap target. Its pinned
original/stage bytes are used only to audit the generic collection layout. Vsh,
PUI, JSON and Mono each require exactly one placement. Before arming and at the
phase handoff the collector compares executable live windows against their
pinned images; each hit rechecks its instruction bytes. Runtime relocations in
GOT/data are not hashed as if they were immutable file bytes.

| Trap | ABI |
| --- | --- |
| Vsh+471f0 Execute | RDI module; ESI context; RDX Action delegate; RCX promise; `[RSP]` caller return |
| PUI+1cba30 Resolve | RDI promise; RSI resolved object; promise+10 callback |
| PUI+1cbaa0 Reject | RDI promise; RSI error (not decoded); promise+18 callback |
| PUI+555a0 timing Dispatch | RDI event; RSI emitter; event+14 tag, +20 name |
| PUI+177cc6 predicate result | AL result, `[RBP-18]` closure; closure+10 view, +20 manager; manager vtable+188 target |

Execute direct calls/return addresses:
SetSearchCondition+473d0/+473d5, GetScalar2+475b1/+475b6,
GetItems+476b4/+476b9, GetItems2+477a1/+477a6. Preflight verifies each E8
displacement targets+471f0. Delegate target+20 is the closure. Condition closure:
arguments+10, promise+20. Other three closures: arguments+10, promise+18.
HBC #4276 supplies `{func:"GetItems",itemProps,offset,limit}` to GetItems2;
#4278 supplies caller-facing `{scalarProp:"revision"}`. The intervening HBC4292
wrapper converts it to native `{scalarProps:["revision"],param:...}`; native
Resolve returns `{ctxId,scalarMap:{revision:...}}`. The gate decoder now uses
that native boundary (see `vsh-home-psn41-continuation-20261108.md`).

Identity audit:

- PUI+88810: module.Context at+28 is ReactContextImp, not PUIReactContext.
- +89740: context+10 → ReactInstance; instance+10 → WeakReference<T>.
- Mono+42dae..42ef9: handle kind `(handle&7)-1`, slot `handle>>3`;
  table+4f98e0, stride138, capacity+100, exponentially sized buckets starting32.
  Only weak kinds0/1 are accepted; low raw bits must be3. Decode complement and
  clear low2 bits, read entry twice. No guest calls or GC pinning.
- PUI context+88 must pass the native LaunchedUserId getter's initialized check;
  signed DWORD+8c is the user.
- Context+68 AppData → non-generic weak App at+10 (handle atweak+14) →
  PUIReactApp UID at+98, per AppData.get_UID+8d530. App+38 and+10 backlinks must
  match context and AppData. GUID is emitted as its 16 identity bytes, not a
  guessed instance-suffix/user string.
- App+30 is the strictly typed ReactApplicationManifest; manifest+30 contains
  ApplicationName. Only `rnps-home` is accepted. Context+30 instead holds nested
  PUIReactContext/Data. AppData/weak-app/backlink validation now precedes
  application-name filtering. No configured-name or user fallback is used.
- Emitter+10 invocation handler; handler+10 ReactInstance. Metadata and
  constructor+876f0 identify this field as IReactInstance.
- Predicate view+30 auxiliary data: context+10, nullable tag valid byte+38,
  tag DWORD+3c (GetTag+16e720). ThemedReactContext unwraps+10
  (+16968f); then the same audited identity chain applies.

JSON audit: JArray/JObject+38 children; JObject's JPropertyKeyedCollection+10
is the backing List. JProperty+40 name, `[[property+38]+10]` value; JValue+30
boxed value. List count+20, array+10, SZArray length+18/data+20 are audited at
corlib+27b510/+27cc60. Known contexts, containers, strings and boxes are type
checked before decoding. No arbitrary dictionary walking or result-item
enumeration is used.

## Offline verification commands and results

Historical input validation (no process access):

```powershell
python tools/capture_home_gates_1360.py --original $original --stage $stage --home-bundle $home --run-log '${USERPROFILE}\Desktop\kytyps5\_Runs\psn37\_kyty.txt'
python tools/test_home_gate_trace_1360.py --original $original --stage $stage --home-bundle $home --run-log '${USERPROFILE}\Desktop\kytyps5\_Runs\psn37\_kyty.txt'
```

Verified the original20 synthetic controls: weak identity/user/themed-context unwrap;
collected handle; empty Games with nonempty Media; nonempty Games requires Media;
observed pending/rejected revision; actual revision0; late attach; missing items;
null callback; user switch/disposal; condition epoch reuse; promise reuse;
cross-generation isolation; pending condition; untracked settlements; decoder
bounds/content exclusion; matching predicate/dispatch identity; preservation of
AL/RIP/other registers with RF/debug changes; new-thread/phase/cleanup state;
occupied debug-register refusal.

The diagnostic follow-up adds5 controls (25 total): exact dispatch type-failure
hop across five boundaries; unknown-class redaction and null/unreadable
distinction; non-Home exclusion before user initialization; Home missing-user
remaining unknown; all six excluded Execute callers plus an unrecognized caller
without argument reads or fabricated associations. Four additional pinned AOT
instruction checks cover Dispatch argument saves/reload and handler instance
storage. All25 controls and the full pinned ABI/negative-control suite pass.

Pinned-image ABI disassembly checks and 14 negative controls passed: wrong full
image hashes (5), out-of-image RVAs (5), duplicate/missing placements (2), wrong
stage (1), wrong Home (1). The actual historical original/stage equality and
unique mappings passed for all four runtime images. No compile or firmware,
profile, database or parent source/build write was needed.

## Parent integration / first coordinated capture

Integrated as `f723e2c7`. Parent reran all20 offline controls and the pinned ABI
audit/14 negative controls successfully against the preserved psn37 placement
log and actual regular-original/psn23 staged inputs. No emulator rebuild is
needed for the external tracer.

After confirming no emulator was running, parent launched `psn38` as PID16000
using production build3a12861, regular13.60/native54 fallback, and the existing
psn23 profiles/stage. No timeout or database seeding was enabled. Hardware-trace
attachment awaits the required managed mappings, before user-controlled login.
Offline validation does not establish successful live debugger cleanup or the
Home gate; those results remain to be recorded from this coordinated capture.

### psn38 first live attachment result

**Correction after crash-dump inspection:** the API-level cleanup report below
did not establish safe detach. psn38 subsequently crashed with tracer debug
registers still enabled on one thread; see the follow-up evidence below. Do not
repeat emulator attachment with this teardown implementation.

The 180-second capture completed and detached successfully (exit0). Artifact:
`${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn38.jsonl`. Arming covered
221 initial threads;108 new-thread arm records followed. Cleanup recorded304
live-thread restorations with no cleanup errors. The end record reports84 probe
hits (Resolve78, Execute1, Dispatch3, Reject2),3615 debug events, and three
Dispatch `object_type` decode failures. Unrelated first-chance exceptions were
forwarded; their occurrence is not a demonstrated crash.

No correlated Home requests or outstanding records were captured. At analysis
time, the managed run log contained neither login-success nor Home-mount matches.
This is not evidence for empty Games, a pending Home request, or missing Home
interactive delivery: the login/startup of interest was not observed in coverage.
The consumer agent is reviewing the imprecise dispatch decode diagnostics before
another user-coordinated capture. The emulator was not stopped by the tracer.

### psn38 unexpected exit: residual tracer breakpoints confirmed

The user confirms the window exited on its own. A subsequent process inventory
found no emulator. Windows Application event1000 at 2026-11-08 01:07:37 identifies
PID `0x3e80` (16000), creation `0x1dd6f1491a84534`, exception `0x80000004`
(single-step), fault address `0xd901cba30`: the tracer Resolve probe. WER report
ID is `10288fec-ce46-450d-b37a-1d75f718d5f3`.

The preserved dump
`${USERPROFILE}/AppData/Local/CrashDumps/kyty_emulator.exe.16000.dmp`
(149091218 bytes) confirms exception thread37800 with RIP `d901cba30`,
DR0 `de00471f0`, DR1 `d901cba30`, DR2 `d901cbaa0`, DR3 `d900555a0`,
DR6 `0`, DR7 `55`, EFlags `244`. Parsing all304 thread contexts finds only
thread37800 with enabled debug-register low bits. Yet the trace logged that
same thread restored at t180.187052 and ended at t180.222074 without errors.
This proves residual hardware-breakpoint state despite the restoration report;
the exact teardown race is under investigation. It is not evidence of a Home
database failure or successful debugger cleanup.

The managed log ends during welcome-to-login transition, with no login-success
or Home-mount matches. The consumer agent now prioritizes debugger teardown
and an isolated sacrificial-process/thread-churn regression. Further emulator
attachment is deferred until safe post-detach survival is validated. Parent
has neither restarted the emulator nor changed the preserved profile inputs.
