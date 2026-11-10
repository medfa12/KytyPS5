# SP0 subprocess/control checkpoint — 2026-11-05

## Result

Implemented the Windows standalone child process/control/manifest backend and
keyed LNC lifecycle integration. **SP0 is gated in production until SP1/SP2
install embedded presentation and exclusive input/audio ownership.** This is a
real implementation with actual helper-process tests, not process-created =
Running/first-frame. No emulator executable built/launched; no user app input or
process manipulation. Frozen rwlock files and Astra/OOBE files were not edited.
No staging/commit. Parent owns review/commit and root test wiring.

### Production files in this checkpoint

* New `src/loader/runtimeChildProtocol.{h,cpp}`: fixed 104-byte versioned host-only
  messages, bounded descriptor parsing, owned anonymous pipe endpoints.
* New `runtimeChildProcess.{h,cpp}`: original retained process handle, per-run
  kill-on-close job, suspended creation/restricted handle inheritance, polling
  ledger, startup/close deadlines, focus acknowledgement, actual root/job wait.
* New `runtimeChildClient.{h,cpp}`: explicit inherited child role, Hello/entry/
  window/first-present notices and validated UI-command polling. No guest pointers.
* New `runtimeChildManifest.{h,cpp}`: standalone `--game` argument manifest,
  fresh non-overwriting run/log roots, private temporary namespace, dedicated
  locked per-title data; no parent mount/profile forwarding.
* New `runtimeChildHost.{h,cpp}`: VSH process coordinator/handlers and retained
  retirement-notification retry pin. Its pump exists before child creation.
* `runtimeLaunch.{h,cpp}`, minimal `runtimeVshLaunch.cpp`: backend selector,
  legacy/shared fallback retained, no inherited focus handler across backends.
* `main.cpp`, `emulator.{h,cpp}`: `--vsh-app-backend subprocess|shared`, internal
  inherited `--vsh-child` descriptor, standalone identity bootstrap, read-only
  app content for managed children. Child role refuses VSH/co-ELF startup.
* Narrow `libs/libLncUtil.{h,cpp}`: immutable external-process token, keyed
  start/retirement, accepted-before-entry cancellation, serialized backend focus
  transaction outside LNC locks. No fake in-parent Program/execution identity.

New tests: `tests/subprocess/{CMakeLists.txt,probe.cpp,process_tests.cpp,
lnc_process_tests.cpp}`. Root CMake and main VM dispatcher are untouched by this
worker. Loader `.cpp/.h` glob already picks up the new production units when the
parent later configures its build; no hand-written source-list delta needed.

## Contracts closed

* Commands/events bind app ID + launch sequence + 128 random nonce bits, fixed
  protocol version/size/type and strict serial order. Hello verifies the actual
  retained child OS PID. Raw host PID never substitutes for guest/LNC identity.
  Direct child guest PID remains its actual standalone process-local value 100;
  the child reports it from the real entry notice. Its LNC app ID is seeded from
  the authenticated host role before program registration, not profile state.
* Only child pipe ends and its freshly created log are inherited. Parent ends,
  jobs, title-store locks and unrelated inheritable handles are excluded.
* Every **created** process stays retained through failed job assignment/resume,
  protocol failure, early death, startup timeout and close. Assignment/resume
  errors return a retained failed-start record to the coordinator, not an orphan.
* Actual retirement requires the original process handle signaled, its actual
  exit code collected, and the assigned job's active-process count zero.
  Descendants are terminated if they outlive the root. Query/control failure
  retains the obligation; it does not fabricate completion.
* Pipe EOF is **not** exit. A bounded native-epilogue grace avoids overwriting an
  ordinary process's actual return code by racing `TerminateProcess` with CRT
  teardown. Only an expired grace terminates a still-live disconnected child.
* Close accepts at most one host-close command; force or timeout targets only the
  matching child job. Completion/relaunch is never inferred from request return,
  window disappearance, pipe EOF or successful termination request.
* The title writer lock excludes concurrent processes and is released at proven
  OS/job retirement, **before** a queued completion can trigger immediate
  relaunch. Keeping a retired ledger record does not spuriously keep storage busy.
* Accepted-before-entry close produces failed launch-end with ECANCELED and
  invalid process/app sentinel IDs; it never manufactures Running/KillEnd.
  Running close preserves KillStart before KillEnd even when retirement races
  the handler's return. Stale/unkeyed start/failure/exit cannot mutate an external
  process generation. External records cannot be adopted by in-parent Programs.
* Focus host callback runs outside LNC registry locks, is serialized against
  competing focus transitions, and requires a real matching child ack before
  committing local focus/events. SP0 refuses unsupported window/input/audio
  capability combinations. Existing shared/direct focus behavior is preserved.
* No parent extra mounts are copied. Firmware/content are read-only in the child
  role; `/data` is a dedicated per-title child store. Fresh run directories and
  CREATE_NEW logs do not overwrite previous output. Metadata/data confinement is
  checked before mutation where applicable. No host-security sandbox against
  malicious concurrent filesystem junction replacement is claimed.

## Validation actually run

Independent **`_Build/vsh-subprocess`** only, VS18 x64 + clang-cl + lld-link.

1. Standalone `tests/subprocess` configured/built without emulator dependencies.
   Actual hidden-console helper subprocess lifecycle test passed **10 consecutive
   CTest runs**. Helpers create no SDL/window/controller/audio resources.
2. Cases: empty/quoted/trailing-backslash/Unicode argv; restricted handle
   inheritance by **actual kernel object comparison** (not reusable handle
   integers); real normal exit codes; pre-entry failure; lingering job descendant;
   wrong nonce/order/duplicate entry/present-before-window rejection; accepted
   cancellation; graceful/forced/timed-out close; startup timeout; injected
   pre-create, assign-job and resume failures; single title writer; immediate
   relaunch while retired record remains retained; non-destructive manifest and
   profile sentinel; invalid descriptor bounds.
3. Separate `lnc_process_tests.cpp` linked in a component overlay with the new
   `libLncUtil.cpp` and read-only pre-existing dependency objects: **PASS**.
   Exercises actual registered launch/kill/focus/event wrappers, nonce/sequence
   refusal, unkeyed callback refusal, exact wire events, live backend callback
   outside locks, failed/successful focus ack, close/retirement race, accepted
   cancellation and creation-before-entry failure.
4. Existing `LncAppStatusTests.cpp` overlay regression: **PASS**, preserving the
   shared/direct app status, events, paths and close/focus behavior.
5. All ten changed/new production translation units compiled individually in
   production mode to `_Build/vsh-subprocess/overlay`: protocol, process, client,
   manifest, host, runtimeLaunch, runtimeVshLaunch, libLncUtil, emulator, main.
   This is **not** a full concurrently changing tree build or emulator link.

Logs/options: standalone CTest `Testing/Temporary/LastTest.log`, overlay
`*-compile.log`, `lnc_process_tests-final.log`, `lnc_regression_tests-final.log`,
`lnc-link.rsp`, `lnc-regression-link.rsp`. Latest final source snapshot/hashes are
under `_Build/vsh-subprocess/checkpoints/sp0-20261105/`.

Two useful tests found/fixed real races: pipe EOF racing normal process epilogue,
and per-title lock release lagging the actual-retirement notification. The
inheritance fixture also had to stop confusing newly reused child handle values
with inheritance of the original parent object.

## Exact parent wiring

### One required direct-entry hook (outside this worker's granted files)

`src/loader/runtimeLinker.cpp` top-level include:

```cpp
#include "loader/runtimeChildClient.h"
```

In **`RuntimeLinker::Execute`**, immediately before the existing direct-main
`RunEntry(entry, params, ProgramExitHandler, ...)` (around line 4377):

```cpp
NotifyRuntimeChildEntry(); // No-op unless authenticated standalone child role.
```

Do not put this before module initialization, in process creation, or in the
co-ELF entry path. Without the hook there is intentionally no successful child
entry notification. No fake earlier notice was added.

### Root CMake test wiring (parent-owned)

After the full-test helper is defined, inside the Windows testing section:

```cmake
if(WIN32)
  add_subdirectory("${KYTY_TESTS_DIR}/subprocess" "${CMAKE_CURRENT_BINARY_DIR}/subprocess-tests")
  add_kyty_full_emulator_test(vsh_subprocess_lnc_tests "${KYTY_TESTS_DIR}/subprocess/lnc_process_tests.cpp")
  add_test(NAME vsh_subprocess_lnc_tokens COMMAND $<TARGET_FILE:vsh_subprocess_lnc_tests>)
  set_tests_properties(vsh_subprocess_lnc_tokens PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
```

The standalone subdirectory adds `vsh_subprocess_lifecycle` itself. No VM main
dispatcher edit is needed. The per-process injection macro applies consistently
to that standalone test component/probe, never to the emulator backend.

## Next stage, not a claimed live result

`ChildIntegrationReady=false` in `runtimeChildHost.cpp` deliberately prevents a
manually selected half-integrated backend from opening an extra top-level app
window or duplicate physical controller/audio owners. SP1 now installs the
child-owned embedded HWND, 1280×720 host/1920×1080 internal sizing and actual guest
present milestone. SP2 installs exclusive parent input and focus/audio routing;
then remove the gate and make subprocess the VSH default as authorized.

Full Home hides the child and returns to the same ShellUI top-level window;
resume retains the process. True translucent parent-over-child GPU composition
is not claimed by native embedding. No full emulator E2E or user app test is
authorized until coordination with the parent and an isolated executable.
