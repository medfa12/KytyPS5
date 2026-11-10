# Particle job scheduling contract and observer

## Verified native contract

Actual IDA/Hex-Rays analysis of the pinned native shell establishes:

* Owner constructor `e7880` installs vtable `1169d68`, clears gate byte `+720`
  and the job/control-block pair at `+7a8/+7b0`.
* Mode setter `eb590` receives its audited caller at `c082e` (return `c0833`).
  It ignores unchanged modes, and ignores requests for modes3/4 when the old
  mode is nonzero. Otherwise modes3/4 set the gate to1; other values clear it.
  These are numeric modes; semantic state names have not been established.
* `e7fb0` creates the job only when gate==1 and job==null. It retains the job
  before calling `155fd0` at `e80e6`. The caller does **not check the result**.
* `155fd0` returns `-1` when global scheduler `13acc98` is null; otherwise it
  tail-calls `5b9610`. Under a lock, that function rejects non-idle job state
  (`job+28 != 0`) with `0x80af0901`, or zero registered workers
  (`scheduler+30 == 0`) with `0x80af0902`. Successful insertion sets job state1,
  retains its control block and returns0. Zero proves queue insertion, not work.
* `155f50`, called by native initialization, constructs `SceBGLayerResource`.
  Worker count is incremented only after the worker-start helper returns0.
* Worker `5b9aa0` waits, loads its assigned job from `worker+38`, then invokes
  vtable slot+18 at `5b9b12` (return `5b9b15`). It stores the callback result at
  job+2c and changes state to3. The particle job's slot targets `eb6e0`.
* Owner completion checks `+7a0`; on the completed-resource path it releases
  and clears the retained pair. A submission failure is not automatically
  equivalent to a retry or completion. Worker assignment/wakeup internals and
  actual live failure conditions remain unverified.

No HLE correction or forced gate is justified without live observations.
Particle callback absence must not be treated as Home launch failure; this
observer discriminates the particle/animation path, not Home launch acceptance.
The renderer-ready byte observed by native mode is a different field/object.

## Implemented `job` capture mode

Four hardware slots: mode setter `eb590`, creation gate `e7fde`, submission
helper `155fd0`, and caller return `e80eb`. For a separately established particle
failure, this can follow a route capture with native execution but no particle
callback; absence alone does not justify further investigation.
It does not replace route's same-run native positive control or observe textures.

Mode/gate events are independent snapshots, not joined by reused pointers. Mode
events require the audited caller and owner vtable; `predicted_gate_write:null`
means the setter leaves the gate unchanged. Gate events require RDI==RBX and
the exact owner vtable. Eligibility is a snapshot, not proof of submission.

The shared submission helper excludes other callers **before** dereferencing
their preserved RBX. For the particle caller it requires RDI==owner+7a8, exact
owner/job vtables and job+38==owner. Return correlation requires the same thread,
stack, frame, outer caller, fresh owner/job backlink and scheduler identity.
Only a paired return interprets EAX as a signed result. Skipped creation branches
also reach `e80eb`; an unacquired hit there never fabricates submission success.
Job state may change while the worker runs; state3 at return is allowed.

Nested submissions poison the association; thread boundaries and coverage close
invalidate it. Existing capture budgets and verified detach apply. At most1024
submission invocations are acquired. Instruction windows are verified before
arming and probe bytes rechecked on hits. Dynamic outer caller PCs are checked
for equality across the acquired frame, not labeled as an audited scheduler.

## Parent command

Use a fresh **normal-audio Home** launch held at the existing pre-guest barrier.
Do not use a dummy audio device or run concurrent observers on one host. Reuse
the established original/stage/log provenance inputs and fresh host identity:

```powershell
python -B tools/capture_home_transition_1360.py --mode job --original $originalShell --stage $stagedShell --run-log $startupPrefix --timeline-log $hostLog --attach --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --output $newTrace --seconds 120 --max-hits 4000 --max-events 10000
```

After `armed` in a separate console, release with the existing helper:

```powershell
python -B tools/release_home_startup_barrier.py --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --token $token --release --armed-trace $newTrace
```

Job receipts require diagnostic_version9/lifecycle_version2 and phase `job`.
Existing particle/route receipt versions remain unchanged. Omit `--attach` and
host/output arguments for offline provenance/byte validation. No emulator rebuild
is needed beyond the already integrated barrier. Parent owns launch/navigation.

## Verification and limits

14 job decoder controls,9 release controls,17 route controls,16 transition
controls,34 gate controls and11 directly executed executor controls pass. The
pinned original job ABI and all executable audit windows pass. Tests cover gate
policy, forged/wrong ownership, caller/frame/backlinks, null scheduler, zero
workers, signed returns, skipped returns, lifecycle boundaries and tampered code.
No live job capture has been performed. Static reports remain local analysis
artifacts; no inventories or local firmware comparisons are part of this change.

This adds diagnostic coverage, not Home readiness, PSN authentication or
ShellUI/profile store ownership. Bootstrap activation remains unwired.
