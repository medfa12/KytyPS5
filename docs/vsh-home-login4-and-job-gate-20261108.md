# Offline VSH checkpoint: login4 coverage gap and particle job gate

## Closed login4 evidence independently checked

Trace SHA256 matches the parent receipt:
`f4e2399bb70dd74cc36a7242af1dbe54a38b3126eb0d0b4fcb669caab61d06e5`.
It contains136 hits at each executor queued/call/return site (408 total), all
excluded as `not_home`, no delivery acquisitions, zero decode unknowns and
verified detach. Armed t=0.519115; end t=302.219297. Only100 exclusion detail
records were emitted under the diagnostic cap; the end counters cover408.
`not_home` means the validated application manifest was not rnps-home. It does
not identify which other application ran or prove Home callbacks were swallowed.

Closed host log SHA256
`4593252b3095c2a98246aa4def055eed6d587ed3f42cb8be662ec558ef5c096b`,
1790544 bytes, contains:

| Milestone | Line | Byte interval |
|---|---:|---:|
| Home instance promoted from Hermes pool |17583|1768358..1768486|
| LoginMgr instance-closed success/retCode0 log |17608|1771148..1771455|
| Home MountApp runApplication |17702|1780416..1780484|

These establish logged local-login success and a Home application launch path,
not visible tiles or authenticated PSN. The trace has no log-position anchors;
alignment of these milestones to active observation **cannot be recovered** from
its relative timer and file hashes. No live process or current Apple TV evidence
was accessed. No raw login parameters are included in this report.

## Implemented alignment observability

Both existing observer CLIs now accept optional `--timeline-log <host.log>`.
At provenance, armed, shutdown_begin and end, the observer records a read-only
sample: file identity, byte length, first/tail4096-byte hashes and UTC sampling
interval. At most8192 bytes are read per event. No log text is emitted, no flush
or writer modification requested. Missing/unreadable logs yield `unavailable`
rather than interfering with debugger cleanup. The option is disabled by default;
existing diagnostic versions, probe counts and release contracts are unchanged.

`home_capture_timeline.py` validates the samples against a closed source log or
retained copy, rejects changed sampled bytes, replacement/truncation during the
sampled interval and incomplete capture receipts, and reports only whitelisted
milestone labels/line numbers/byte ranges. It classifies persisted bytes relative
to armed and **shutdown_begin**, not final detach, so drain activity is not
misrepresented as active capture. It reports old login4 alignment as unknown.

Limits: these are bounded byte samples, not full-prefix integrity proofs. A
mutation wholly outside sampled regions is not detected by the samples; the
analyzer separately hashes the entire closed log. More importantly, buffering
can delay persistence: a line appended during capture does not prove its code
executed during capture. No automatic causal or readiness conclusion is made.
Keep the source log unchanged and retain the original trace and closed-log hash.

11 new disposable-file controls pass (including partial-line boundaries,
rotation, truncation, unavailable logs, old traces and post-coverage drain).
Existing gate suite:34 passed. Executor-specific controls:11 passed; the earlier
33-case discovery run included inherited tests. The real closed login4 trace/log analyzer also passed and
truthfully returned `unknown_no_log_anchors`.

## Actual IDA particle construction gate

One actual IDA Pro9.3/Hex-Rays analysis used a separate private database copy.
Report/spec/log: `_TempData/ida-home-20261108/shell-job-enqueue.*`.
Analysis input hash:
`beded661699ad3dac61cc71b1cddb9f26a34de12478cf35998c42c51cf8a3db2`.
Original native shell hash remains
`cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.

IDA identifies function+e7fb0, referenced by vtable slot1169d98. At+e7fde:

1. Compare owner byte+720 to1; if unequal, jump+e80eb.
2. Compare owner job pointer+7a8 to0; if nonzero, jump+e80eb.
3. Otherwise clear owner byte+7a0, allocate a64-byte `LightParticle` job and
   initialize it through+5b8b10.
4. Install vtable1169df0 and owner pointer at job+38; retain job at owner+7a8
   and control block at+7b0.
5. At+e80e6 call+155fd0 with RDI=owner+7a8, then continue at+e80eb.

This proves a concrete **pre-dispatch conditional**, not that either condition
was false live. The owner byte+720 is not the native3 renderer-ready byte0;
native3's ready result cannot settle this gate. A nonnull job pointer might mean
already-created work; it is not itself failure. The internal queue/execution
semantics of+155fd0 have not yet been established and must not be guessed.

`audit_home_particle_job_1360.py` pins both conditional branches, job vtable/owner
writes, retained-job store, the reference-call bytes/target and ELF RELA slots.
The audit passed against the retained original. It is an executable static
audit, not another live observer or an enable-flag workaround:

```powershell
python -B tools/audit_home_particle_job_1360.py _TempData/ida-home-20261108/shell-transition-original.elf
```

## Integration and next live commands (parent only)

Cherry-pick this checkpoint after the integrated route observer6423b427. Changes
are Python diagnostics/docs only; no production rebuild or store migration is
required. Leave the current Apple TV run alone. The next startup discriminator
remains the implemented **same-run route capture**, not particle1 or a Gen2Vr
policy override. On a future normal-audio barrier-held launch with fresh identity:

```powershell
python -B tools/capture_home_transition_1360.py --mode route --original $originalShell --stage $stagedShell --run-log $startupPrefix --timeline-log $hostLog --attach --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --output $newTrace --seconds 120 --max-hits 2000 --max-events 10000
```

After route phase `armed`, from another parent console:

```powershell
python -B tools/release_home_startup_barrier.py --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --token $token --release --armed-trace $newTrace
```

If same-run native controls hit but the resource callback does not, the next
source-backed split is gate+e7fde versus reference-call+e80e6 and then dispatch
inside+155fd0. Do not infer an asset failure from that absence.

For a separate parent-owned login/Home executor capture, arm before user login
and include the same log anchor option (firmware/stage directories, not the
native eboot paths used above):

```powershell
python -B tools/capture_home_gates_1360.py --trace-mode executor --user 268500997 --original $originalFirmware --stage $managedStage --home-bundle $homeBundle --run-log $startupPrefix --timeline-log $hostLog --attach --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --output $newTrace --seconds 300 --max-hits 4000 --max-events 50000
```

User controls navigation. Once capture and log snapshots are closed:

```powershell
python -B tools/home_capture_timeline.py --trace $newTrace --log $closedHostLog
```

Home tile acceptance, actual Home callback delivery and genuine PSN remain
unverified. Title `/data` lease is not ShellUI/profile ownership; multi-mount
ownership, root rename pinning, pre-job orphan cleanup and bootstrap activation
remain unresolved. No active/staged store writes, live emulator access, launches,
audio, media-decoder edits or delegation occurred in this checkpoint.
