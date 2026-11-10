# NGS2 production build and mnetflix3 launch

Parent integrated the float PCM change as `e758b676` (agent `2b4ba100`).
Production build completed successfully using:

```text
cmake --build _Build/windows --target ngs2_sampler_tests kyty_emulator --parallel 2
ctest --test-dir _Build/windows -R '^ngs2_sampler$' --output-on-failure
```

CTest: ngs2_sampler passed,1/1, zero failures. Build emitted compiler warnings;
no build error. This is a fresh parent build, unlike the earlier component-only
readiness checkpoint. It includes integrated local DB code but does not activate
private recovery candidates.

Executable: `${USERPROFILE}/Desktop/kytyps5/_Build/windows/kyty_emulator.exe`

SHA256: `a3a95197873b5db433caada3b25b6c395d75c43322ccc50a46c679cb56fcef91`

This supersedes the old3a12861 executable hash for new attempts. Later Python/docs
integration commits are not evidence of another C++ rebuild.

## Live attempt started

After verifying no emulator was running, parent launched Netflix PID22780 in
`${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix3/` with the original
`mnetflix1/launch.json` arguments, four authenticated mounts and environment.
Only the guest-log destination changed. The executable hash was checked immediately
before launch; original `KYTY_MEDIA_TRACE=1` was asserted. No controller input
was supplied. Fresh launch manifest, guest.log and host.log retain the evidence.

A bounded75-second process-exit observation and window-rectangle screenshot
capture is in progress. Process creation alone does not establish visible UI.
No numeric live NGS2 setup, visible startup recovery or playback acceptance has
yet been recorded at this checkpoint. Inspect the completed observation before
asking the user to play the authenticated title. Stop on the first actual failure.

Relevant follow-up: `netflix-review-followup-20261108.md`. Setup diagnostics occur
before validation; API entry does not prove accepted/valid decoded output.

## Completed bounded startup observation

PID22780 survived the75-second exit observation, responded to Windows and owned
window8718346 with source build e758b67 and a progressing frame counter. Actual
guest setup lines941658/1081336 report `type=0x18 channels=6 rate=48000 field16=0
field20=0`. This confirms the implemented F32 path is exercised in this run;
the previous waveform fatal did not recur in the inspected startup evidence.

`window-startup.png` captured an overlapping browser activation-success page,
not emulator UI. `window-owned.png` (PrintWindow) showed a black client area.
Parent then brought the existing emulator window to the foreground without any
controller input, verified its foreground handle, and saved
`window-foreground.png`; the captured client region was also black (frame49357).
No Netflix menus or playback are visually established by these screenshots.
This is an observation at capture time, not proof that menus never appeared
earlier. User confirmation of the current display/history is pending.

Bounded guest tail continued buffer0/1 submissions and layered-present counters;
host tail includes ignored custom control0x40002000. Neither establishes a cause
for the black client or genuine picture/audio. The media agent is examining a
bounded log snapshot to distinguish actual executed decoder calls, application
errors and missing observability. No process control or rebuild is delegated.

## User-confirmed transition

User reports: **“netflix stuck on black screen after login”**. This establishes
earlier visible login UI and locates the current failure after login; the black
screenshot must not be described as an always-black startup. It does not establish
title selection, decoding or sustained playback. Media investigation is redirected
to the post-login transition and its first supported failing boundary.

Parent compared production source3a12861 to e758b676: the source delta consists of
NGS2 and private runtimeLocal database/preparation components. There are no source
changes in graphics, ESVM or libVideoDec2 across those revisions. This constrains
the rebuild delta but does not exclude pre-existing presentation/decoder defects
newly reached after the audio fix. No additional causal claim follows from it.
