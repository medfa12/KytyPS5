# Home investigation: native producer identity and completion

## Implemented checkpoint

Boot-session native module loads now join actual initializer completion before
acquiring a successful reference. Concurrent mapping requests for the same host
file also reuse one process-scoped instance under the loader lock. The ShellUI
launcher explicitly routes managed Composite library paths to the firmware file
that PSM initializes, including when the managed stage contains ordinary copies.

This corrects an observed split compositor owner and two independently verified
loader startup defects. **Usable Home has not been demonstrated.** The observed
duplicate instance is used for rendering-time profiling; its error alone does
not establish why rnps-home presents no menu.

## Native and psn36 evidence

Regular `libSceComposite.sprx` SHA256:
`d9abe348692c6e5b9e4de7fa38d6d46c74d675b5a5af63e1761d9a7bfc80bb04`.

`tools/inspect_vsh_compositor_identity_1360.py <image>` reproduces bounded native
disassembly and checks the complete image hash. RVAs:

- `+43e0`, `sceCompsoitorGetProcessRenderingTime`, reads the image-local client
  pointer at `+18030`. A live pointer supplies the actual producer DWORD at
  client `+28`. A null pointer branches at `+43ed` to the diagnostic at `+4421`
  and returns `80d40001`; this branch performs no socket-disconnection check.
- `InitWithProcessOrder +4d90` clears this instance's state (`+4dc5`) and its
  successful mapping path publishes the client pointer at `+51c1`.
- Quit unmaps the client at `+5f8d` and clears that pointer at `+5fa1`.

Read-only parent `_Runs/psn36/_kyty.txt` establishes two instances:

1. Lines24239-24244 load firmware `system_b/priv/lib/libSceComposite.sprx` at
   `ad0000000`. PSM/UI InitWithProcessOrder imports bind to `ad0004d90` at
   lines24331/24375. `_Logs/psn36.log:113-116` records a real client, allocated
   pool, and mapped permanent memory.
2. Lines301648-301651 start the staged `common/libSceComposite.sprx` and resolve
   managed `sceCompsoitorGetProcessRenderingTime` to **`e2200043e0`**, provider147,
   owner-1. The first recurring diagnostic prefix follows at line301667.
3. Read-only host inspection confirms these are distinct host files with the
   same SHA256: firmware file ID9007199255769167 (31 links), staged file
   ID10696049116109826 (one link). Existing hard-link recognition cannot merge
   this ordinary copy. Those IDs/link counts describe inspection time only.

Thus the profiling consumer selects a different image-local singleton than the
one initialized by PSM. The message begins before login; it is not evidence of a
login-triggered disconnect. The real render producer's continuing flip output
and the user's working login/create-user UI are consistent with this distinction.

## Implementation details

- `shellui_native_producer_mounts.ps1` supplies exact file mounts for
  `/common_ex/lib/libSceComposite.sprx`, `/kyty/common_ex/lib/...`, and
  `/system_ex/common_ex/lib/...`. The selected firmware file must exist; an
  existing staged file must have matching bytes by SHA256. Generation reads the
  stage and firmware and emits arguments. Sandbox common aliases use the first
  mount through the existing filesystem resolver.
- `LoadProgram` rechecks process-scoped file identity while holding the mapping
  lock for null-owner boot modules as well as runtime applications. Previously
  boot loaders could both miss their outer lookup and publish separate instances.
- Previously `StartModule` set `started=true` before entering native code, while
  `KernelLoadStartModule` returned a handle to any already-mapped session module.
  Session initialization now has Starting/Started/Failed state, waits outside the
  loader lock, and publishes completion only after the actual initializer returns.
  Self/nested in-flight startup returns Busy. Actual positive and negative native
  initializer outputs are retained; a failed initializer yields no load reference.
- The kernel's reused and newly mapped boot paths share reference acquisition.
  Racing loads increment the existing count rather than both assigning one.
  Explicit pinned-provider application admission retains its existing separate
  checks. Distinct equal-byte files and different application owners remain
  distinct unless the launch manifest explicitly maps the path to the producer.

## Verified validation

Isolated clang-cl overlay in `_Build/home-validation`, at most two compile jobs;
rebuilt `runtimeLinker.cpp`, `libKernel.cpp`, and the main test TU from this tree,
linking read-only parent build objects. Production full rebuild is a parent gate.

Seven CPU modes passed, each in its own working directory:

```text
--runtime-program-file-reuse-only
--app-local-programs-only
--sysmodule-native-only
--runtime-guest-execution-only
--native-libc-startup-only
--native-ssl-smoke-only
--native-ssl-production-only
```

Coverage includes competing actual ELF mapping, private-owner isolation,
canonical file mounts over an ordinary copied stage, sandbox aliases, actual
guest LoadStartModule/Dlsym exports, held initializer success/failure, once-only
initialization, preserved result37/-123, self-reentry Busy, and exact reference
counts. The foreign initializer-wait observation uses a held callback and a
30ms observation after the second caller starts; it is not a kernel waiter-count
witness. Synthetic fixture callbacks are not native firmware initialization.

Negative control removed only the reused-session completion handoff in
`libKernel.cpp`, rebuilt that TU, and linked a separate executable. The expanded
production-export fixture fails with exit1 at its completion/reference assertion;
the corrected executable passes. Recipe/logs: `_TempData/check-home-negative.py`,
`_Build/home-negative/negative.log`. An initial parallel invocation of two tests
in one CWD collided in their shared temporary param.json setup; separate test
directories resolve the harness collision.

PowerShell mount tests passed (copied stage, spaces, mismatch, missing producer,
absent stage, input preservation). The helper also successfully generated all
three mounts against the actual regular firmware/psn23 copied stage read-only.
Pinned disassembly script and `git diff --check` passed.

## Home blocker and next coordinated live checks

The confirmed database producer/completion failure remains: preparation
`160002` is acknowledged by generic HLE, while the AppInfo-owned sync-management
producer has not published `SceAppDbSyncManagement`; client initialization returns
`80020003`. AppBrowse has a null/unopened accessor after corruption admission,
and real user-create notification returns transaction `809c0103` and
`WAITING_RECOVERY` (psn36 lines867591-867648). Native database transports currently
require the unavailable source-bound regular Core/UI admission. The PSN agent's
`158af371` audit confirms absent adapter `Current()` precedes authorization;
`8002004e` is not an observed capability rejection. See its PSN startup checkpoint
for the exact missing input rather than interpreting local login as admission.

For the parent-coordinated next run, use the updated launcher and a rebuilt
executable. Verify there is one Composite mapping and managed rendering-time
dlsym resolves to that mapping's `+43e0`. Verify the duplicate-instance diagnostic
stops, then independently test existing-user/new-user Home presentation. Record
whether native AppInfo/preparation endpoints start, whether `160002` reaches its
real callback and publishes sync management, and the actual recovery/revision
query outputs. If Home is still empty after ownership/completion corrections,
the profiling message is eliminated as a misleading witness, not a proven menu
fix. A usable menu and tile lifecycle remain the live acceptance criteria.

## Parent integration validation

Integrated as `1b9d2233`. The normal `_Build/windows` runtime test target rebuilt
successfully with parallel2. All seven CTest modes listed above passed serially
(0.78s total); the PowerShell producer-mount tests also passed. These are focused
CPU/fixture checks, not live firmware startup or visible Home acceptance.

The PSN grant/closure follow-up is integrated separately as `eeb3c289`, preserving
the session-module lifecycle declaration. At this checkpoint the old psn36 PID
23476 and all `kyty_emulator.exe` processes were absent. Production rebuild and
coordinated live retry remain pending. Database-producer investigation continues
in the isolated Home worktree; existing profile/database originals are retained.

## Follow-up: session finalization review

The new cached initializer completion exposed an existing missing session Stop
transition: a finalized mapping could still satisfy a later LoadStartModule.
Stop now closes admission with Stopping before invoking the real finalizer,
publishes Stopped after pthread-static-object retirement, and rejects repeated
finalization/restart of that mapping. Reload requires fresh mapping rather than
assuming native singleton guards were reset. An absent finalizer is a no-op.
Kernel reference decrements are locked; its final-release call receives a
caller-specific completion receipt so it cannot unmap merely because another
caller reached Stopped. Direct StopAll still ignores reference count.

The expanded `--runtime-program-file-reuse-only` passed in an isolated worktree
CWD with worktree-local TEMP/TMP after the <=2-job overlay rebuild. It checks two
nonfinal production-export releases, held finalization returning -9, competing
stop, nested/across-thread load rejection, and repeat stop/reload rejection.
The first fixture attempt access-violated because it had not initialized the
pthread registry now used by actual StopModule; initializing the real kernel
registry fixes that fixture prerequisite. No production null-registry bypass.

Two integration observations:

- Existing `StopAllModules` holds `m_mutex` across native finalizers. A finalizer
  waiting on a different worker which needs the loader mutex can deadlock. This
  broader shutdown ownership issue is not solved by the session state change.
- Generic session initialization accepts nonnegative native return values and
  retains them. Native SSL's `ProviderStartup::Complete` deliberately requires
  exactly zero (`m_failed = result != 0`); `RunNativeSslSmoke` additionally gates
  publication on `Completed()`. That stricter admission is preserved. A positive
  SSL return can set generic Started while SSL admission fails, but cannot set
  production readiness. The previous seven host CPU modes are policy/fixture
  evidence, not execution of real native SSL initializers; parent owns that gate.

Remaining concurrent session raw-Program lifetime/unmap pinning is a broader
loader ownership issue; the completion receipt is not a general lifetime pin.

### Parent finalization integration and live retry

Integrated finalization as `59d314cf` and the local database-contract evidence as
`3a12861c`. Production emulator and normal runtime test target rebuilt with
parallel2. Focused CTest checks passed4/4 (0.39s): program-file reuse including
Stop admission, application-local programs, guest execution, and SSL production
policy. This does not execute real firmware SSL initialization.

Launched `psn37` (PID6812) after confirming no emulator was running, using regular
13.60, existing psn23 storage/stage, native54 fallback and canonical Composite
mounts. No timeout or database seeding was enabled. User-controlled Home and
persistent-login acceptance are pending; the local HLE database producer is
still under development in the isolated worktree.

### psn37 user-reported acceptance failure (2026-11-08)

User reports a stall and explicitly confirms the background remains animated:
Home menus are still absent. Process6812 remains active. The preserved profile
268500997 logs in successfully after restart (`_Logs/psn37.log`5088-5140), and
Home receives focus5174 then MountApp/Running Main5217-5219. This verifies the
local profile restart/login path, not usable Home or genuine PSN.

The Composite correction is observed live: `_Runs/psn37/_kyty.txt`24351 maps
one instance at `ad0000000`; managed dlsym308223 resolves its `+43e0` at
`ad00043e0` with provider30 and the canonical firmware path. AppBrowse still
fails sync-event initialization at165150 (`80020003`), and getConceptIds reports
`80f50003` at213529/228386. These remain failure evidence, not yet proof of the
specific first-render dependency.

Tail logs also contain repeated swapchain recreation failures, but the user's
animated-background observation does not support treating those as the Home
menu cause. No graphics fix or completed playback is claimed. The database
producer agent continues; a separate isolated `vsh-home-render` agent now owns
the managed/RNPS first-render consumer trace, excluding the native SQL/recovery
and loader files owned by the database agent. Live profiles/session are retained.
