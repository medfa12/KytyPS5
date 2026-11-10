# VSH app integration: separate-process direction — 2026-11-05

## User correction

The user explicitly permits another thread or process for apps. Keeping native
apps in the ShellUI host process was an assistant-imposed constraint, not a user
requirement. Prior same-process lifecycle work remains checkpointed; it must not
continue blocking integration of the already-working standalone app path.

The implementation direction is a shell-controlled app subprocess, reusing
ordinary `--game` startup. Integrate its presentation, controller focus, Home
return, close, actual process exit and relaunch with the running VSH. The visible
experience remains shell-integrated. Preserve 1920x1080 guest rendering, normal
1280x720 host window, user-controlled maximizing, one guest DualSense, profiles,
mounts, media/audio and ShellUI/RNPS navigation/artwork.

`src/loader/runtimeLaunch.cpp` already contains the separate-process launch
backend, command-line quoting, inherited-log handle restriction, job ownership
and process-exit waiter. `ConfigureRuntimeLaunch` currently selects the
same-process backend when `RunOptions::vsh_session` is set. This existing branch
is the reuse point; process creation alone must not become first-frame proof.
The background app worker is auditing concrete window/surface integration and
the lifecycle/focus bridge before editing cross-cutting files.

Genuine LoginMgr/PSN authentication remains a separate service dependency. Its
config/transport prerequisite batch is committed at `d8b5c67f`; five production
translation units compile and all ten standalone IPMI checks pass. Production
hooks, native acceptance and real authentication remain unverified.

Main-thread work remains OOBE. Sol handles app integration; Astra handles
LoginMgr/PSN. Finish/checkpoint existing rwlock and callback work without further
expanding same-process app requirements.

## Current manual app

At the user's request, direct YouTube was launched as PID25648 with retained
`_Runs/youtube-direct19-aac-capability`. Fresh logs are
`_Logs/youtube-manual-20261105-175241*`; guest printf is in that profile. Screenshot
`_TempData/youtube-manual-1105.png` verifies populated signed-out Home at frame1920,
60fps. No input was sent. Preserve this user-test process and its executable.
The earlier direct Apple process was no longer running when YouTube was launched.
