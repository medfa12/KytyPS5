# Fresh-profile RNPS routing prerequisite — 2026-11-05

## Established failure

The integrated Home run `_Runs/spv202809/_kyty.txt:245358-245359`
requests `pshomeui:main` and immediately reports that
`_SystemData/priv/cache/urlconfig/urlconfig_cache.json` is absent.
The later OOBE run `_Runs/obfix203624/_kyty.txt:239235-239237`
reports the same failure for `pssettings:play?function=initial_setup&phase=1&onPhaseFinish=false`,
then throws `NullReferenceException` in `MenuScene.CreateRnpsPhase` at
line 240356. Home never logs an application bundle load; its observed Hermes
evaluations are the host/base bundles for the pool.

`tools/run_shellui_1360.ps1` provisioned this prerequisite only from an absolute
external voldemort checkout. That source does not exist on this machine.
The workspace `_SystemData/priv/cache/urlconfig/urlconfig_cache.json` does exist.
The HLE first checks the mounted profile, then a CWD-relative `_SystemData`
fallback (`src/libs/libJscdSmall.cpp:647-654`). Isolated per-run working
directories therefore expose the missing provisioned file instead of finding
the workspace cache. This is a launch/profile prerequisite failure; the
subprocess integration is not established as its cause.

## Change

`tools/prepare_shellui_urlconfig.ps1` seeds only the routing cache, using the
absolute workspace source first and retaining the external checkout fallback.
Existing profile caches are preserved. Missing sources fail preparation with
an actionable error. The launcher invokes this helper in place of its old
silent external-only copy. No user/account state is copied or synthesized.

`tests/shellui_urlconfig_tests.ps1` passed under Windows PowerShell: fresh
isolated profile from an unrelated CWD with no external checkout; byte-exact
copy; preservation of an existing cache; external fallback; missing-source
failure. `git diff --check` passed. No emulator launch, live process inspection,
input operation or executable rebuild was performed by this worker.

## Runtime validation still required

Parent should rerun the launcher with a new isolated profile. No rebuild is
needed. Confirm the provisioning source message, successful Home/settings
resolution and actual application bundle evaluation/presentation. This fixes
the proven missing prerequisite; successful Home/OOBE presentation is not yet
claimed. The parent's preserved-executable comparison failed earlier in Mono
and does not establish an RNPS baseline.
