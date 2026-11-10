# Implemented same-run startup route capture

`capture_home_transition_1360.py --mode route` now implements the four-slot
startup discriminator. It uses the existing process identity checks, bounded
hardware observer, thread lifecycle handling and verified detach machinery.
No emulator replacement is needed for the observer change; the existing host
must include the previously integrated pre-guest barrier. Parent alone owns
the active login4 executor capture and future launches.

| Event | Native shell RVA | Evidence |
|---|---:|---|
| initialize_entry | d9330 | Registered InitializeNative entered, not completed |
| update_entry | d9460 | Same-run UpdateNative code/probe positive control |
| resource_entry | eb6e0 | Acquired resource callback object and owner |
| particle_entry | ea340 | Verified callback-to-initializer edge, not load success |

## Exact contract

Original shell SHA256 remains
`cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.
The existing original/1080p stage allowlist, log path/mapping validation and
live executable byte checks apply. The mode additionally audits ELF RELA
registration for InitializeNative/UpdateNative and the callback vtable slot.
The entire17-byte callback and51-byte InitializeNative body are checked in
live executable memory before arming; probe bytes are rechecked on hits.

An actual IDA Pro9.3/Hex-Rays analysis of a separate copy of the private shell
database verified:

* d9330 has no input-owner argument: it allocates/constructs a56-byte object,
  publishes it at13a8ee0 and invokes its next startup function. The observer
  deliberately does not interpret entry RDI as an owner or infer completion.
* eb6e0 pushes RBP, sets RBP=RSP, loads RDI=[callback+38], calls ea340, then
  returns zero. Callback vtable1169df0 has this function at slot+18.
* IDA finds the vtable reference at e801e in function e7fb0. This gives a
  concrete construction-side investigation target if live callback dispatch
  is absent; enqueue/execution semantics are not yet established.

Reports: `_TempData/ida-home-20261108/shell-route.txt`, matching spec/IDA log.
View SHA256 `beded661699ad3dac61cc71b1cddb9f26a34de12478cf35998c42c51cf8a3db2`.
Original firmware and previous databases were preserved.

At resource entry, acquire thread, RSP, saved RBP, dynamic return PC, callback
object, exact relocated vtable and owner at+38. Require vtable==base+1169df0
and slot+18==base+eb6e0. At particle entry, require:

* same observed thread, RSP=callback-entry RSP-16 and RBP=entry RSP-8;
* return PC exactly base+eb6ed;
* outer return PC and saved RBP still match the acquired callback frame;
* freshly reread callback vtable/slot and owner match; current RDI==fresh owner.

Only then emit `callback_edge_verified:true` with the acquired invocation ID.
Remove the pair immediately; no completion probe exists in this mode. Missing
entry, wrong thread, bad frame/owner or duplicate particle hits cannot pair.
Nested/unfinished callbacks poison that thread until a lifecycle boundary.
Thread boundaries invalidate outstanding pairs and report `route_unpaired`.
Shutdown closes coverage before draining. Callback acquisition budget1024 plus
existing capture time/hit/event/output budgets apply.

Native entry events are independent: they never consume, create or complete a
callback invocation. Update requires RSI==0xa0 and bounded readable flow DWORDs
at offsets0/4. Its entry means neither singleton nor renderer readiness.
Dynamic outer callers are bounded and recorded from the stack, **not statically
whitelisted or labeled as a known managed caller/scheduler**. Fresh backlink
checks prove the narrow observed callback edge, not arbitrary caller identity.

## Exact parent commands

Use a **future new normal-audio Home launch**, after the current executor capture is closed
and the parent chooses to replace it. Launch with the existing
`KYTY_PRE_GUEST_GATE_TOKEN=$token` procedure. Record the new host's PID, creation
FILETIME and executable hash, and snapshot its startup log while the barrier is
ready. Do not reuse login4 identity or an older mapping prefix for a new run.

```powershell
$common = @('--mode','route','--original',$originalShell,'--stage',$stagedShell,'--run-log',$startupPrefix)
python -B tools/capture_home_transition_1360.py @common
python -B tools/capture_home_transition_1360.py @common --attach --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --output $newTrace --seconds 120 --max-hits 2000 --max-events 10000
```

Run attach in a separate parent console/job. Once its complete `armed` event
has phase `route`, release from another console while the observer remains live:

```powershell
python -B tools/release_home_startup_barrier.py --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --token $token --release --armed-trace $newTrace
```

Route provenance is diagnostic_version8/lifecycle_version2. The release helper
requires that exact version and phase for route; particle0/1 retain version7.
Same-generation PID/creation/hash, no drain/end, ready event and currently
attached debugger checks remain. A native-mode or stale version7 route receipt
cannot authorize release. The120-second host gate deadline is independent of
the observer deadline; prepare inputs before launch and release promptly after
arming. A later attach cannot recover startup.

## Interpretation and verification

* Initialization/Update hits with no callback: localize next work upstream of
  the resource callback, including e7fb0 construction/enqueue/dispatch. Do not
  infer a VR branch or an asset failure.
* Callback and paired particle entry: dispatch occurred; this mode supplies no
  texture result or composition evidence.
* Callback with no paired entry: inspect unknowns, outstanding pairs, lifecycle
  and teardown first. The audited callback has an unconditional direct call.
* No Update positive control: absence of particle events alone remains weak
  evidence; no renderer-suppression conclusion.

17 new route tests,8 release-receipt tests,16 existing transition tests and34
existing gate/debug-context tests pass. Pinned original instruction audit and
offline route CLI against the closed candidate2 stage/log pass with
`process_access:false`. New tests cover wrong/forged vtables, changing owner,
RDI/backlinks/caller/frame, cross-thread/late/duplicate entry, lifecycle poison,
budgets and independent controls. No live attach, runtime launch, audio or store
mutation occurred. Live route ABI/cleanup validation remains parent-owned.

This observer does not change Home recovery. The lease component still covers
only title `/data`; ShellUI/profile multi-mount lifetime ownership and bootstrap
activation remain outstanding.
