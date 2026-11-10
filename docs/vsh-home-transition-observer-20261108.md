# Implemented bounded native transition observer

`tools/capture_home_transition_1360.py` is offline by default. It supplies an
observer to the existing `capture_home_gates_1360.capture` loop: same process
path/hash/creation-time validation, hardware breakpoint ownership, event drain,
thread restoration, post-detach verification and cleanup receipt. It does not
implement a second debugger or change guest code/data. No live attach, emulator
launch or audio playback was performed during this implementation.

## Modes (four slots each)

| Mode | Acquisition | Genuine asset return | Fallback return | Completion |
|---|---|---|---|---|
| particle0 | shell+ea340 | +ea40b | +ea4fb | +ea6a3 |
| particle1 | shell+ea340 | +ea593 | +ea68e | +ea6a3 |

Entry requires the actual audited caller return address+eb6ed. Each interior
hit rechecks the entry's thread and stack frame, return address, and freshly
read owner stored at `[RBP-d0]`. No captured owner pointer is dereferenced later
as a substitute for that frame check. Frame/owner failures invalidate the
association. Reentrant/unfinished particle entries poison that thread until a
thread lifecycle boundary. A late interior hit is `transition_no_observed_entry`,
never evidence that an asset failed or never initialized. The signed loader
return and fallback return are recorded separately. Successful loader return
cannot be paired with a fallback branch, nor negative fallback with completion.
Completion labels the selected asset/fallback path, not GPU upload or composition.

`native` uses shell+d9460 (entry), +d9480 (singleton dispatch), +d9501 (renderer
call), +d9506 (renderer return). Entry requires size0xa0 and records caller PC;
the outer dynamic P/Invoke caller is **recorded, not named or whitelisted**.
The nested native call must return to+d9485 and link to that same acquired outer
frame. Singleton is reread from+13a8ee0 at every relevant hit; renderer identity
must match singleton+8 and the audited register arguments. Native objects are
not treated as managed objects or inferred from a preserved register alone.
Renderer byte0 is recorded before/after its update, alongside raw flow DWORDs
at offsets0/4. These are raw observations, not Home readiness. A null singleton
is explicitly reported; native zero return is not used at all. Repeated native
entry replaces an unfinished prior invocation with an explicit incomplete event.

Thread create/exit events invalidate associations. Shutdown closes coverage
before draining events. At most1024 entries are admitted per capture; generic
time/hit/event/output budgets also apply. Different modes/runs cannot be paired
as one invocation. Managed timeout/normal-start branches are not among these
native modes; this capture resolves the earlier upstream asset/renderer question.

## Inputs and exact parent commands

Inputs must belong to the **new** parent-owned run:

* Original `system_ex_b/app/NPXS40087/eboot.bin`, hash
  `cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.
* Exact staged native eboot named by that run's `Loading:` log line. Either the
  original or existing active 1080p profile hash
  `7e04d0389edcb0881183e20e2ee49c836d422617aecbef552d00a4e35b8d1451`
  is accepted. The latter is also checked byte-for-byte against exactly four
  startup-dimension changes from `prepare_shellui_managed_1360.py`; no other
  staged patch is accepted. The initial offline check correctly rejected this
  staged image until that existing audited profile was explicitly accounted for.
* Immutable startup log prefix containing one `place eboot.bin base=... mapped=...`
  and matching `Loading:` path. Base/extent are extracted, not guessed.
* For attach: host emulator executable path/hash, PID and observed creation UTC
  FILETIME, and a new exclusive output filename.

PowerShell example (parent substitutes all variables):

```powershell
$common = @('--mode','particle0','--original',$originalShell,'--stage',$stagedShell,'--run-log',$startupPrefix)
python -B tools/capture_home_transition_1360.py @common
python -B tools/capture_home_transition_1360.py @common --attach --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --output $newTrace --seconds 60 --max-hits 2000 --max-events 10000
```

Change only `--mode` to `particle1` or `native` for the chosen separate capture.
Do not run observers concurrently on one process. Native full initialization
and invocation byte windows are verified in executable guest mappings before
arming; probe bytes are rechecked on every hit. No new executable/firmware patch
is needed by this observer.

**Timing:** particle initialization can precede the login screen. Arm while the
parent has the new launch stopped after shell mapping/placement but before
initializer+ea340 executes; then allow normal startup. This tool attaches to an
existing process; it does not create or implement an early-launch pause. If the
parent cannot provide that window, the particle modes may miss initialization;
no-hit/late-entry output must remain inconclusive. Do not relabel a capture
started at the login screen as full startup coverage. Native mode may observe
later frame updates but cannot recover prior particle outcomes. User controls
navigation. Future Home captures use normal audio; do not select a dummy device.

Hardware traps stop threads and can affect startup timing. A timeout observed
under tracing is not automatically causal evidence of the original visual report.
Retain `end.cleanup_verified`, cleanup errors/repairs, coverage events and mode
provenance alongside the output. Incomplete/missing events are not fabricated
completions. A failed detach verification must be handled through the existing
parent debugger recovery procedure, not ignored.

## Verification

16 synthetic-memory controls and original-byte audit pass: both particle modes,
loader/fallback path ordering, early file-failure fallback, late attachment,
wrong caller, fresh-owner mismatch, thread separation/exit, nested entry,
closed coverage, impossible completion, instruction tampering, null singleton,
flow size, missing predecessor, replaced native owner and unrelated nested frame.
The34 existing gate/debug-context controls pass after adding the observer hook.
The real candidate2 staged file/log passes the new CLI offline mode with
`process_access:false`; no process was opened. The established Windows debugger
stress harness was not re-run here because this task prohibits live attaches.
Thus live ABI selection and this new mode's live cleanup remain to be verified
by the parent; reuse of the teardown machinery is not a substitute for its receipt.

## Next C++ ownership integration slice

The next scoped change is a move-only canonical-store lease in RuntimeChild,
using the existing share-zero `.runtime-lock`. Extend the spawn/child protocol
with a dedicated inherited lease handle and canonical root identity; preserve
the parent's handle until child acknowledgment and hold the child's until guest
file shutdown. Do not replace the current four-handle inheritance whitelist with
unrestricted inheritance. Add disposable subprocess controls for pre-create,
assignment/resume failure, parent death before/after acknowledgment, child exit,
canonical path collision and distinct stores. Only then couple the validated
roster/candidate producer to prelaunch SQLite promotion. All alternate mount
routes must either participate or explicitly refuse this ownership mode before
activation. The Python component's nine tests establish useful transaction and
handoff controls, not that C++ all-launch-path invariant. No activation is wired
by the observer commit.
