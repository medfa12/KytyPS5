# Apple preview progress checkpoint — 2026-11-08

## Finding and scoped correction

The host source implemented readiness and readable presented PTS, but never
emitted native source **progress event 0x0f**. Native libSceEsvm responds to that
event by querying `PLJs4B6fCk0` and forwarding public event **2**, including the
64-bit PTS and 16-bit sequence. Readiness event 0x2001/public 7 is independent.
This is a concrete missing notification and a strong explanation for Apple's
"Waiting for Progress" after successful preview startup. Live resolution is
**not yet verified**; this does not establish the only cause of every stall.

`DispatchRefills` now checks the unique live video child's actual presentation
receipt on its existing guest-entry pump. It sends event 0x0f only for a new,
valid, current-generation acknowledged receipt, claiming decoder/generation/
serial before invoking guest code. It uses the existing owner dispatch gate,
callback-depth guard and copied callback/user ABI. Scheduler/renderer threads
never invoke guest callbacks. There is no wall-clock, decoded-frame, queued-PCM,
or publication-only substitute for presented PTS. No decoder, audio or DRM
behavior was changed.

## Fixed evidence

Only the fixed mapple6 `stall-evidence/guest.log` and `receipt.json` were read;
no live emulator interaction, launch, closure, attachment or store change.
Guest snapshot SHA256 was independently verified:
`509b2d3725b25763adc6b9e571bff0dea6992de5fdc1afec37722c3fc7b0b5a5`,
323263170 bytes. Root:
`${USERPROFILE}/AppData/Local/Temp/opencode/mapple6/stall-evidence`.

* Line 615367: 20:00:49.373, STATE_PLAYING/TRACK_STATE_RUNNING, waiting for
  progress 1588ms, position 0.000, 1588ms after Prepare.
* Line 823481: next preview's analogous 1578ms stall, position 0.100.
* Video/audio refill notifications occur before the first stall at lines
  589241 and 595493; absence of refill delivery alone is not the explanation.
* Lines 746920/746963: host flush completion succeeds, but subsequent native
  `sceEsvmFlush` seek recovery reports 0x8a790001. This remains separate.
* Earlier PlayReady access 0x8a790010 and later MediaKeys-session 0x8a790004
  are not evidence that DRM caused the initial clear-preview progress stall.

User confirms login preview rendering and Interstellar improvement, then roughly
two-second stops. Stick/Stitch still fails. User confirms Netflix regular audio
**and playback** work. YouTube/Netflix splash audio arriving after the animation
is a separate timing investigation; regular playback is reported fine.

## Actual IDA verification

Ran installed **IDA Professional 9.3 idat.exe**, loading read-only native 13.60
`libSceEsvm.sprx` PT_LOAD bytes into a separate temp database. Independently
verified module SHA256:
`31593167e10813bdc195fb099da1da723c36818c063c35de6f6a086962d2e3f2`.
Artifacts under `${USERPROFILE}/AppData/Local/Temp/opencode`:
`apple-progress-ida.py`, `apple-progress-ida.txt`, `apple-progress.i64`.

IDA command:

```powershell
& 'C:/Program Files/IDA Professional 9.3/idat.exe' -A -TBinary `
  '-o${USERPROFILE}/AppData/Local/Temp/opencode/apple-progress.i64' `
  '-S${USERPROFILE}/AppData/Local/Temp/opencode/apple-progress-ida.py' `
  '${USERPROFILE}/Downloads/13.60/system_b/common/lib/libSceEsvm.sprx'
```

Recovered instructions (module RVAs):

* 0x28a1e subtracts 13; 0x28a22 bounds the 10-entry switch.
* Table 0x3b200 maps source 0x0f to 0x28add.
* 0x28add/0x28ae1 pass addresses of u64 PTS and u16 sequence;
  0x28af6 calls 0x2ebf0. ELF import resolver identifies that target as
  `PLJs4B6fCk0#B#C`.
* 0x28afb rejects failed queries; 0x28b0f writes public event 2;
  0x28b16/0x28b1a copy PTS/sequence into the public record.
* 0x28be0..0x28beb invoke the registered receiver's vtable +0x10.

Independent read-only reproduction through the retained ELF inspector:

```text
python -B tools/audit_esvm_readiness.py esvm calls PLJs4B6fCk0
python -B tools/audit_esvm_readiness.py esvm around 0x28af6 0x100
```

This recovers the actual consumer contract. Native producer notification cadence
is not claimed to be reproduced; host notifications coalesce to the latest
acknowledged receipt at the next eligible guest-entry pump.

## Production-component tests and negative control

Clean worktree at start: HEAD fca4429c, with earlier media fixes already present
under their worktree equivalents. Only source, source tests and this note change.
Isolated `_Build/apple-readiness-validation` overlay reuses read-only parent
dependencies; source, wrappers and test TU compile locally with at most two jobs.
No parent Build/runtime output is written.

```powershell
ninja -C _Build/apple-readiness-validation -j2
& ./_Build/apple-readiness-validation/virtual_memory_allocation_tests.exe --media-framework-esvm-source-only
$env:KYTY_MEDIA_TRACE='1'
& ./_Build/apple-readiness-validation/virtual_memory_allocation_tests.exe --media-framework-esvm-source-only
python ${USERPROFILE}/AppData/Local/Temp/opencode/apple-progress-negative.py
```

Corrected source suite passed with tracing off and on (exit 0), exercising real
packet decode and production source/mailbox components, plus existing real SDL
audio sink coverage. New assertions cover no progress before acknowledgement,
one notification per receipt, a second advancing PTS, exact sequence, original
guest TLS/thread, no host-critical callback, reentrant PTS query and Render,
and no stale progress after Flush. Acknowledge is simulated by this component
test; it is not a live GPU/presentation validation.

Negative control compiles HEAD's **unchanged old source** into its own object
and links the **new tests** into `progress-before.exe`. Exit 1 at:
`guest pump delivers exactly one progress event per acknowledged receipt, never the host worker`.
Logs: `progress-source.log`, `progress-source-trace.log`, `progress-before.log`,
`progress-build.log`, `progress-negative-build.log` in the isolated build folder.
An initial command omitted the `--...-only` option and ran the generic memory
suite (exit 0); that run is not counted as source coverage.

## Parent integration and remaining gates

Cherry-pick this checkpoint's scoped source/test/doc commit. Rebuild production
and run the source suite, then parent-owned relaunch with preserved original
mapple1 stores. User should navigate the login preview/Interstellar trailer and
observe whether playback advances beyond two seconds. Capture bounded
`source-progress-delivered` receipts and app stall positions. Confirm Netflix's
known-good regular playback remains good. Keep protected-title/session errors,
seek-recovery failure and splash timing as separate remaining investigations.
