# Implemented opt-in pre-guest readiness barrier

## Placement and behavior

No supported existing early-start stop was found in RuntimeLinker's startup
path. `runtimeStartupBarrier.{h,cpp}` now provides a narrow opt-in Windows event
barrier in `RuntimeLinker::Execute`: after `RelocateAll`, game/startup patches
and native-SSL validation, immediately **before** `RunNativeLibcStartup`.
`StartAllModules`, the non-system libc initializer, cohost threads and main guest
entry are all later in that path. This is not a pause at the login screen or a
firmware patch. The normal absent-variable path returns immediately without
creating events or waiting.

Select it only for a new diagnostic host with environment variable
`KYTY_PRE_GUEST_GATE_TOKEN=<32 lowercase hex characters>`. The host consumes
that variable before waiting, so later child launches do not inherit the gate.
It creates two manual-reset, initially nonsignaled, noninherited named events:

```
Local\KytyPreGuest-<host PID>-<token>-ready
Local\KytyPreGuest-<host PID>-<token>-release
```

Existing events cause startup failure, including an already-signaled release
event. Partial setup is closed. After mapping output and a phase/PID/name line
are flushed, the ready event is signaled. The host waits at most120 seconds.
Only release permits guest startup; timeout/setup/publication/wait errors take
the existing fatal startup path, not an automatic continue. Handles close on
every returned outcome; process termination also closes them. No store is read
or written by this barrier. This does not implement persistent-store ownership.

## Parent procedure for a future normal-audio particle run

Do not apply this to the already running native3 process: it has passed startup.
Build a new host with this commit, preserve the prior artifacts, and use the
existing parent-controlled launch recipe with only this opt-in environment
selection added. Home must use normal audio; do not select a dummy device. The
barrier does not configure audio.

1. Generate `$token = [guid]::NewGuid().ToString('N')`. Supply it as
   `KYTY_PRE_GUEST_GATE_TOKEN` in the **new target host's environment**. Restore
   the parent's original environment after spawning if the launcher uses an
   inherited environment snapshot. Reuse no token from an earlier attempt.
2. Wait for the `pre-guest-barrier: ready ... phase=relocated-before-guest` line
   or matching ready event. Snapshot the startup log prefix; it must contain the
   shell placement and staged `Loading:` path. Record the new host executable
   SHA256, PID and creation FILETIME. This startup-stop is at most120 seconds,
   so have observer arguments prepared before launching.
3. Check readiness/identity (no release):

```powershell
python -B tools/release_home_startup_barrier.py --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --token $token
```

4. Run the offline particle observer check, then launch its existing attach
   command from `vsh-home-transition-observer-20261108.md` in a separate parent
   console/job. Choose `particle0` or `particle1`. Wait for its flushed `armed`
   record; do not wait for the bounded capture to finish while the guest is held.
5. Release with the **currently armed** observer's output path:

```powershell
python -B tools/release_home_startup_barrier.py --pid $targetPid --created-filetime $creationFiletime --exe $emulatorExe --exe-sha256 $emulatorHash --token $token --release --armed-trace $newTrace
```

The helper rechecks process path/hash/creation identity, matching ready event,
version7/lifecycle2 particle provenance, same-generation armed receipt, absence
of drain/end records, and that a debugger is currently attached before signaling
release. Native-mode receipts cannot authorize this particle startup release.
Keep the observer running throughout; a receipt is a snapshot, not an atomic
lease on debugger liveness. If attach fails or is abandoned, let the gate time
out or use the parent's normal host-stop procedure; never fabricate an armed
receipt or signal release to collect misleading startup coverage.

The observer can now acquire initializer entry before the load/fallback sites,
provided it reports `armed` while this gate is held and the parent then releases.
No-hit/unknown results still remain inconclusive. Keep the original verified
detach receipt, and user-controlled navigation only after startup resumes.

## Verification and limitations

An isolated MSVC C++20 build and CTest passed. Policy tests cover disabled and
invalid selection, partial setup, publication failure, all bounded wait results,
and cleanup. Windows tests use only **same-test-process** events/one thread:
stale ready/release rejection, ready-before-release ordering, blocked execution
until release, and absence of surviving named events after handle closure.
No emulator or debugger was launched or attached. Seven Python offline receipt
controls pass for both particle modes, wrong identity, native-mode rejection,
draining/detached receipts, truncated armed records, duplicates and budget.

Reproduce from a compiler-configured shell:

```
cmake -S tests/startup_barrier -B _Build/startup-barrier-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _Build/startup-barrier-cmake -j2
ctest --test-dir _Build/startup-barrier-cmake --output-on-failure
python -B tools/test_home_startup_release.py -v
```

Assertions stay enabled in this test target. The new production cpp is picked
up by the existing `src/loader/*.cpp` source glob. The full emulator target and
actual paused-guest sequence were not built/run here; parent integration must
verify them. The test wait-timeout branch is injected rather than spending120
seconds on a real timeout. This is a diagnostic readiness barrier, not a fix for
particle rendering, Home visibility, or authenticated PSN.

## Parent-provided native3 evidence (not independently replayed here)

Closed trace SHA256
`90486d08b33d065cfcec9de461766a98d606e8b02b7ac9327fe202893dee0c1c`:
56 complete native invocation sets,224 hits, no decode unknowns/exclusions or
outstanding sets. Singleton/renderer present and renderer byte1 before/after
each update. This is **pre-login**, with first observed entry19.198s after attach
timing, not startup-particle or Home-transition coverage. Cleanup verified263
threads, one pre-detach residual repair, zero post-detach repairs, no errors.
The parent's SDL-dummy configuration avoids actual sound but alters device timing.

Actual candidate mount is the complete `13_Firmware 13.60.00_PS5UPDATE`
extraction, not the earlier audit directory. Parent reports both particle GNF
and cold/warm AT9 files present there with hashes matching the audited originals.
Their absence is not a supported explanation. File presence still does not prove
load/upload/composition. Current ownership/lease integration remains a separate
next slice; the barrier must not be mistaken for a store lease.
