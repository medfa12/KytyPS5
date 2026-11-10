# Home tracer lifecycle-v2: psn38 crash correction

## Evidence and lineage

`8825e667` introduced the tracer (parent integration `f723e2c7`). Its one-pass
restore/ContinueDebugEvent/DebugActiveProcessStop ordering was unsafe.
`523d055c` preserved the bounded type diagnostics, corrected the cleanup claim,
and temporarily disabled `--attach`. This follow-up repairs teardown and phase
rearming and restores opt-in attachment after isolated Windows regression.

Parent-reported dump evidence: psn38 historical PID16000, creation
`0x1DD6F1491A84534`, crashed at 2026-11-08 01:07:37 with exception `80000004`,
RIP/address `0xd901cba30` (Resolve trap). Fault tid37800 retained
DR0=`de00471f0`, DR1=`d901cba30`, DR2=`d901cbaa0`, DR3=`d900555a0`,
DR6=0, DR7=`55`, EFlags=`244` (RF/TF clear). Only this thread among 304 dump
threads had enabled DR7 low8. The trace reported it restored at180.187052s and
ended at180.222074s with `cleanup_errors:[]`. It was initially armed, not a
missed new-thread record. API success did not establish safe detach.

Three final sacrificial negative controls reproduced fatal `80000004` after
the old ordering reported API success. Fault contexts included DR7=`455`
(`55` plus architectural reserved bit10) and DR7=0. An intermediate attempted
repair also failed: keeping all threads explicitly suspended while waiting for
an empty debug queue concealed an owned exception until release, producing a
post-detach step even with DR7=0. That attempt is not the shipped protocol.
These observations establish residual state and pending-delivery hazards;
they do not prove a specific internal Windows kernel context-copy mechanism.

## Protocol

Production and the sacrificial harness share `quiesce_detach` in
`tools/capture_home_gates_1360.py`:

1. Close guest-object observation and retain original debug-register state and
   owned thread handles through the entire lifecycle operation.
2. Enumerate threads, acquire one explicit suspension increment each, and use
   GetThreadContext as a suspension rendezvous. Record vacant baselines for
   new unarmed threads; refuse foreign debug state.
3. Restore with DEBUG_REGISTERS-only SetThreadContext and verify readback.
   Track owned thread contexts at trap sites as pending-progress candidates.
4. Release the explicit holds **while still attached**, continue the pending
   event, and drain owned exceptions. At a matching first-chance owned step,
   validate RIP/DR6/TF, set RF through CONTROL-only context, and restore debug
   state again. DR6 may already be zero after the initial disarm. Mixed or
   unexpected causes fail closed; unrelated exceptions are forwarded.
5. Service creates/exits without arming new breakpoints. Retain/defer handles
   when a terminating thread rejects suspension. Liveness uses the signaled
   thread object: GetExitCodeThread is ambiguous when a dead thread returned259.
6. After a running drain interval, freeze and verify again. Candidates must
   advance away from owned trap sites; deferred threads must clear. Repair any
   reinstalled debug state and repeat protected draining. A100ms quiet interval
   alone is insufficient: the final frozen queue and register sweeps must pass.
7. Detach while explicit holds remain. Verify registers again after detach;
   any residual triggers recovery-required failure without automatic resume.
   Release exactly the acquired increments, preserving preexisting suspension
   counts, and close handles last.

Phase handoff performs the same drain, installs the new slot plan while held,
then releases while remaining attached. Pending request associations are closed
before each drain; observed-unsettled durations and the first closure reason
are preserved. Missed settlements cannot later match a recycled promise pointer.
Guest objects are not decoded during these explicitly bounded coverage gaps.

Each drain is bounded to30 seconds/10,000 events. Failure attempts to freeze and
disarm, emits `shutdown_recovery_required` with held/deferred thread identities,
and retains suspension increments for owner recovery. The recovery attempt can
itself fail and is reported separately; this is not successful cleanup. Closing
handles does not release the retained suspension increments.

Output has `lifecycle_version:2`. Normal live-target completion emits
`shutdown_pre_detach_verified`, `shutdown_post_detach_verified`,
`shutdown_released`, and `end.cleanup_verified:true`. Compatibility field
`cleanup_errors:[]` alone remains insufficient. Process exit is reported
separately and is not labeled verified detach. Parent post-capture survival
checks are still needed; sacrificial results do not establish emulator survival.

## Parent commands

Run from the integrated checkout or this worktree. Windows x64 Python and
MinGW-w64 `gcc` are required for the harness. It compiles one tiny C target and
runs children sequentially; it accepts no PID or executable argument and attaches
only to its freshly launched Popen child. Failure cleanup kills only that child.
Artifacts go to fresh `%LOCALAPPDATA%/Temp/opencode/home-trace-lifecycle-*` folders.

```powershell
python tools/test_home_trace_windows.py --rounds 3 --workers 64
python tools/test_home_trace_windows.py --rounds 1 --workers 256 --cases hit phase
python tools/test_home_trace_windows.py --rounds 1 --workers 32 --cases hit deadline create --legacy-negative
```

The last command intentionally reproduces crashes in disposable children using
the old ordering. Normal cases cover hit limit, deadline, CREATE_THREAD boundary,
phase replacement, idle timeout and injected restore failure. Successful children
independently scan debug registers, continue hot calls for2 seconds, check counter
progress, preserve a preexisting suspended sentry, and receive forwarded foreign
exceptions. The injected-failure child must retain recovery holds without detach.

Exact read-only offline command:

```powershell
python tools/test_home_gate_trace_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn37/_kyty.txt
```

The parent-controlled emulator command is in
`docs/vsh-home-gate-tracer-20261108.md` under **Parent command**. It requires the
parent's current PID, creation time, executable hash and current placement log;
historical PID16000 must not be reused as a target instruction.

## Verified checkpoint

Final offline run: **28 tests passed**, plus the pinned-image ABI audit and
**14 negative controls**. The three added controls cover retained original
state/holds on restore failure, signal-based thread liveness, and association
closure across lifecycle gaps (including repeated closure).

Local artifact directories under `${USERPROFILE}/AppData/Local/Temp/opencode/`:

| Directory | Result |
| --- | --- |
| `home-trace-lifecycle-ge0pq9ma` | 12/12 survival cases,64 workers, three rounds hit/deadline/create/phase |
| `home-trace-lifecycle-lt5_one3` | 2/2 idle survival cases,64 workers |
| `home-trace-lifecycle-8n110kq0` | 2/2 hit/phase survival cases,256 workers,264 verified threads; one pre-detach residual repair, zero post-detach residuals |
| `home-trace-lifecycle-twvqu8s2` | Injected restore failure retained37 holds and did not detach |
| `home-trace-lifecycle-yj77ghmb` | 3/3 old-ordering negative cases reproduced fatal80000004 after API success |
| `home-trace-lifecycle-rn7b_6cd` | Final targeted run: hit/phase/idle survived,64 workers; zero active DR threads, preserved sentry, forwarded exceptions; injected restore failure retained68 holds without detach |

Final targeted command:

```powershell
python tools/test_home_trace_windows.py --rounds 1 --workers 64 --cases hit phase idle restore-error
```

Per-case JSONL and `results.json` remain local. Failed development attempts are
preserved and are not counted as successful validation. This agent has performed
no emulator attachment/launch/process lookup or production-dump parsing. No
parent build/profile/database writes or guest-memory/code patches were made.
Actual Home gate correlation and the original Dispatch type-error hop remain
unproven; the next coordinated parent capture needs the diagnostic checkpoint
followed by this repair and must cover Home request entry.

## Parent integration validation

Integrated diagnostics as `b2cfdf71` and lifecycle repair as `07c1a2e2`.
Parent reran all28 offline controls, the pinned ABI audit and14 negative controls
successfully. The integrated Windows harness passed one round with64 workers
across hit/deadline/create/phase/idle/restore-error. Artifacts are in
`${USERPROFILE}/AppData/Local/Temp/opencode/home-trace-lifecycle-i3voeghu`.
All five ordinary cases exited0 after independent child checks found zero active
debug-register threads, continued progress, preserved sentry suspension and
forwarded foreign exceptions. Protected drains consumed queued owned steps;
all post-detach repair counts were zero. Injected restore failure retained68
holds and did not detach, as required. These are sacrificial-process results,
not emulator survival or Home acceptance.

The user confirms readiness for a coordinated new capture. Parent will launch a
fresh session, verify its identity, arm before user-controlled login, and inspect
both lifecycle verification records and post-detach emulator survival.

## psn39 first lifecycle-v2 emulator capture

Parent launched production `3a12861` with the existing psn23 storage/stage,
regular13.60/native54 fallback and no database seeding. PID9200, creation FILETIME
`134385727020718544`, executable SHA256
`3aa3636099727914e0cb00ceb2ecfec5c79e65ca3f1c9bda6e04f71be0786e82`.
Artifact: `${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn39.jsonl`.

The240-second trace armed236 initial threads and163 later thread-create records.
It finished at241.742479s with `cleanup_verified:true`:347 threads verified
before/after detach, no residual repairs, and acquired holds released. One initial
trap-site context advanced during protected cleanup. Parent then confirmed the
same process/creation time remained alive and responding, with window title
frame6679/fps26. This is live post-detach survival evidence for this capture,
not just API success or a guarantee for every future lifecycle.

Unlike psn38, this run includes persisted user268500997 login success
(`_Logs/psn39.log`2789+) and Home promotion2792/mount2867. However, identity decoding
prevented correlation:790 total hits (Resolve709, Execute14, Dispatch49, Reject18),
with61 `object_type` failures at `identity.manifest` (49Dispatch,12Execute).
The preceding PUIReactContext/ReactInstance/weak-reference hops validated; the
manifest expected `ReactNative.ReactApplicationManifest` but reported bounded
type category `other`. Two Execute exclusions were deliberate GetItemById and
GetScalar callers. No request associations or outstanding records were created.
This does not prove empty results, settled requests, or missing interactive events.

The consumer agent is correcting the manifest decoder against pinned native
layout/type evidence. The active user session remains untouched. Home's actual
first-render gate and visible acceptance are still unproven.
