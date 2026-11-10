# VSH subprocess parent integration — 2026-11-05

## Baseline rollback checkpoint

Integrated SP0–SP2 with direct-main entry notification in runtimeLinker and
root CMake wiring. Full isolated Release build linked `kyty_emulator.exe` in
`_Build/vsh-integrated`; log: `integration-build.log`. Existing `_Build/windows`
executable was not rebuilt.

Root CTest run: 9/10 passed. Input, embedded synthetic Vulkan, four focused
controller/audio fixtures and three existing controller/audio regressions
passed. Lifecycle failed with `client early exit=24 error=0`, inside the
probe's forbidden-handle verification; cause requires investigation.

Review also confirmed child manifests omit the parent's `network_connected`
setting. Network initialization uses that setting to enable HostHttp, so it
must propagate before online media testing.

Real-app presentation, startup focus timing, Home/resume and close/relaunch
remain unverified. Keyed LNC and permanent callback/NP/rwlock checks are pending
in the integrated build. This checkpoint preserves the assembled source before
fixes and visible E2E divergence.

## Post-checkpoint review

Baseline committed locally as `66e67158`.

Added explicit `network_connected` propagation from parent RunOptions through
ManifestInput to the child CLI. The default remains offline. Existing manifest
fixture now verifies both policies.

Added distinct forbidden-handle probe diagnostics for duplication failure,
named-event open failure and actual object inheritance. After rebuilding that
probe, lifecycle passed once plus 15 consecutive runs (including the new network
manifest checks). The original exit24 is **not diagnosed or claimed fixed**;
future failures now retain the exact branch/error in the fixture's probe log.

Isolated full rebuild linked the emulator and all four test executables.
`integration-checks-build.log` retains the build output. All four selected
CTests passed: `vsh_subprocess_lnc_tokens`, `runtime_integer_callback`,
`np_account_abi`, and `runtime_rwlock_lifecycle` (0.55 seconds total).

## First visible integrated attempt

Network/checks checkpoint: `e55308f6`. Launched isolated VSH as PID9932,
`_Runs/spv202809/sd`, using `_Build/vsh-integrated/kyty_emulator.exe`, foreground
user, RNPS replay, host network and the installed external app catalog. Guest
1920x1080, normal host 1280x720. No pre-existing emulator process was observed.

The window reached the shell background (frame1012/60fps), but Home tiles did
not appear. A later capture showed the same background at frame3721/0fps.
No Apple TV selection or subprocess launch was attempted. Evidence:

* `_Runs/spv202809/boot-observation.png`, `home-check.png`
* `_Runs/spv202809/_kyty.txt`, `_Logs/spv202809.stderr.log`
* `_Runs/spv202809/home-stall.txt`: bounded sequential native thread capture,
  with this build's PDB; suspend increments balanced by the capture tool.

The log contains OnLaunched start/end and the `pshomeui:main` resolver request.
This is a pre-app Home startup blocker, not evidence of a child presentation
failure. Its cause is not established. The window's embedded source label is
`66e6715-dirty`, reflecting the build before committing the tested network fix.
