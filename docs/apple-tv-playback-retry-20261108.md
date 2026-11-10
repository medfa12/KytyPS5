# Apple TV playback retry after Netflix success

User requested opening Apple TV for a playback test after confirming Netflix
audio and playback. No emulator process was present before launch.

Launched `mapple6` from the retained `mapple1` manifest with original arguments
and stores, fresh log paths, and media/audio tracing enabled. Normal audio device
selection is in use; no dummy/disk override was present in the manifest or launch
environment. Controller navigation remains user-controlled.

- Executable: `_Build/windows/kyty_emulator.exe`, source banner `58a3de0`.
- SHA256: `b75485633fbb8600a44b870111bcbd60dedb57a35968608f70e54e0e8cfaa65f`.
- PID35160, creation FILETIME134386380110013732.
- Evidence: `${USERPROFILE}/AppData/Local/Temp/opencode/mapple6/`
  (`launch.json`, `guest.log`, `host.log`).
- Verified matching process identity and responding Apple TV window,
  PPSA01783 version01.000.038, frame251/60fps.

Launch is verified. Apple TV audiovisual playback and sustained synchronization
await the user's test; window frame rate alone does not establish playback.

## User result and retained stall evidence

User now confirms login-screen preview video and an Interstellar trailer render,
but previews stop after about two seconds. The requested title (typed "sitich";
earlier investigation identified the Stick pilot) still does not play. This is
real rendering progress, not sustained Apple playback success. Netflix regular
playback remains working. User separately reports late splash-screen audio in
YouTube and Netflix, after the animation; normal video playback is unaffected.
No cause for that splash-only timing issue is established yet.

Retained complete-line snapshot: `mapple6/stall-evidence/guest.log`,
323263170 bytes / 9875343 lines, SHA256
`509b2d3725b25763adc6b9e571bff0dea6992de5fdc1afec37722c3fc7b0b5a5`.
Sibling `host.log` and `receipt.json` preserve launch diagnostics and observation.
Read-only summary records 11 source admissions, 10 startup Flush/readiness/video
mailbox activations, 62 PCM acceptance records and 52 refill records. Trace
budgets cap several categories; these are not total played-frame counts.

Application log establishes a later failure than the prior readiness gate:

- Line615367: STATE_PLAYING / TRACK_STATE_RUNNING, no progress for1588ms,
  position0.000 (20:00:49.373).
- Line823481: same stall after1578ms at position0.100 (20:00:55.692).
- Lines746963/938721: recovery seek reports public Flush error0x8a790001.
- Initial PlayReady access fails0x8a790010; later media-key session creation
  attempts fail0x8a790004. These are additional observed failures, not proof of
  the clear-preview stall's cause.

At the user's request, Home/VSH Astra medium work resumed in parallel with media
Astra medium investigation. Home works offline from completed login/IDA evidence
and its isolated worktree; next live route capture awaits a dedicated launch.
Media works from this fixed Apple snapshot on progress/refill/presentation paths.
Parent retains live-process/controller coordination. No new fix is claimed at
this checkpoint; Apple sustained playback, Home handoff and genuine PSN remain
unresolved.
