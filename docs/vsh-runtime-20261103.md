# VSH same-session runtime work

Baseline: merge `a048eb73`, followed by 1080p runner checkpoint `c4da128c`.
Rollback branch: `backup/ps5-vsh-before-main-20261103`. Nothing pushed.

Cleanup: at the user's request, historical run/build logs and frame/shader
traces were deleted after the findings below were recorded. Their paths below
are historical references. Cleanup removed 4,934 files (18,065,687,212 bytes),
with no deletion errors; the current `_Build/windows` emulator and saved
application/profile data remain available. The deletion manifest is
`_TempData/cleanup-old-outputs-manifest.json`.

## Mode and routes

`--vsh-session` (development runner: `-VshRuntime`) selects the in-process
application backend for the co-hosted VSH boot. Ordinary direct-game launch
keeps its existing backend. User-facing launcher configuration is deferred.
Internal rendering is configured separately from the host window. The
runner's forced 1920×1080 host-window arguments have been removed; host window
size now uses the normal default and can be maximized by the user. The staging
script now patches the audited 13.60 NPXS40087 startup immediate at RVA
`0x126ab` from 3840×2160 to 1920×1080. The store at `0x126cd` writes PSM
config fields `+0x50/+0x54`; the call at `0x127d4` reaches libScePsm export
`lWlBrUu77Kg` through PLT `0xd25560`. PSM checks these dimensions at `0x2325ec`
and copies them into rendering state at `0x233023`. Staging checks the exact
instruction/store/call bytes and rejects a mismatched launcher. Only four
immediate bytes change; source preservation and repeat staging were verified.
`vshruntime3` confirms the guest setting is `Pixels2K[1920,1080]`
(`_Logs/vshruntime3.log:167`, repeated in later runs). Run7 visibly renders
Home at this setting in the normal host window (capture below).

The launch resolver consumes configured mounts and the supplied firmware root:

- Native applications retain their own executable, title, metadata and `/app0`.
- Category `0x10200` resolves to firmware NPXS40001's executable while retaining
  the requested application's identity and content root. Verified in 13.60
  SceShellCore `convertPathForWebApp`, RVA `0x6989a0`, comparison `0x6989cd`.
- Firmware RNPS bundles are classified as requiring the native scene host.
  The standalone host Hermes diagnostic VM is not this route.

The new execution backend currently admits only the native route. Web and
RNPS plan resolution does not imply successful execution. YouTube PPSA01651
uses native Cobalt/Starboard; Twitch PPSA01667 and Spotify PPSA05688 use the
web host. Spotify also has an independent custom-music companion.

## Ownership and lifecycle

Runtime executable `Program::application_owner` points to itself; its private
modules use that pointer. Null-owner firmware/direct programs retain the
session namespace. Owned modules resolve before shared providers and cannot
provide symbols to another application or the boot session. Implicit main
handle, proc-param, heap callbacks, libc arguments/environment/destructor
registrations and content snapshots follow application ownership. App-local
`/app0`, `/hostapp` and temporary mounts follow the owner rather than a private
module's ID. Application code and identity storage remain session-resident.
PlayGo chunk caches also follow owners, preserving the legacy cache and
initialization-only compatibility fallback. Background app pad reads preserve
shared input history and return neutral controls with connection metadata.
No-target focus does not grant runtime apps input; ShellUI retains PS observation.

Entry runs on a guest pthread with the existing exit trampoline. Intercepted
application exit does not invoke session-wide module shutdown. The backend
waits for owned guest cleanup before publishing completion or releasing its
single-active-launch admission. An initializer that spawns threads and then
fails also requires quiescence. Forced termination of surviving app workers
is not implemented; such a launch remains admitted until those workers stop.
Unresolved strong imports are reported before application initializers run and
produce a failed launch rather than deliberately entering a fatal missing-import
stub. This check cannot detect every incomplete implementation behind an export.

LNC events use its application IDs and immutable guest-visible installed paths.
The verified 13.60 managed event is 5740 bytes, pack-4; native receive capacity
5744 is not permission to enlarge the managed copy. `IsAppLaunched` uses a
title string and one-byte output. Focus exports use the verified 13.60 NIDs.

MAIN_SYSTEM bus `0x100` has its own retained plane, ordered after application
MAIN and overlay, before host overlays. Independent flip groups can progress
without cross-port head-of-line blocking. CPU tests cover system-only replay,
layer retention/clearing and group selection; this is not proof of an actual
second native Composite client rendering.

## Verification status

- Launch-plan fixture passed: `_TempData/vsh-runtime-plan-tests1.log`.
  Windows symlink checks were skipped for missing creation privilege.
- Six scanout CPU tests passed: `_TempData/vsh-scanout-tests1.log`.
- Build1 reached linking, then failed because process-identity sources were
  added after CMake's source glob was configured. Regeneration/rebuild pending:
  `_TempData/vsh-runtime-build1.log`.
- Build1 LNC and controller tests passed (`_TempData/vsh-lnc_app_status_tests-1.log`
  and `_TempData/vsh-controller_settings_tests-1.log`).
- Build1 libc test exited with `0xc0000005` during the new isolation fixture's
  legacy `setenv` call (`_TempData/vsh-libc_scan_tests-1.log`). The fixture lacked
  Config initialization; correcting its bootstrap made the full libc suite pass
  without a production workaround.
- Build2 succeeded for emulator, memory, filesystem, libc, LNC and controller
  targets (`_TempData/vsh-runtime-build2.log`). All seven focused runs exited 0:
  `_TempData/vsh-build2-{symbol-scope,thread-exit,system-content,input-focus,libc,lnc,controller}.log`.
  These cover required-import admission, scoped symbols/heaps/main handles,
  surviving-thread accounting, metadata/PlayGo, controller focus/history,
  libc state/finalization and LNC ABI/events.
- `vshruntime1` (PID 17984) stopped around 7 seconds during native ShellUI boot.
  The assertion at eboot return RVA `0x5615f9` points to string RVA `0xf5c725`:
  `init_video_out_event.cpp(37): 0 <= sceVideoOutSysAddSetModeEvent2(eq, SCE_VIDEO_OUT_HEAD_MAIN, nullptr)`.
  Independent MAIN_SYSTEM leaves no application MAIN port during system-only
  boot, exposing head-event routing that still expects the old bus alias.
  Correction and visible 1080p verification remain pending. Logs:
  `_Logs/vshruntime1.log`, `_Runs/vshruntime1/_kyty.txt`.
  Fixed by physical-head event state independent of scanout handles, with real
  equeue initial/change delivery and lifetime cleanup. Build3 succeeded for the
  emulator and affected test targets (`_TempData/vsh-runtime-build3.log`).
  Build3 system-event, input-focus, symbol-scope and thread-exit checks passed.
  The new identity fixture first assumed a requested LNC ID would be used without
  a pending launch, then hit an ANSI conversion exception while scanning unrelated
  Unicode temp filenames. It now reads the allocated LNC ID and compares native
  filenames; identity execution is being rechecked.
  The corrected identity fixture passed in `_TempData/vsh-build6-identity.log`,
  including real Unix endpoints, explicit/self queries, getpid, rejected unknown
  peers, endpoint cleanup and retirement after owned-thread quiescence.
- URI messaging passed in `_TempData/vsh-build8-uri.log`: exact initializer and
  envelope sizes, sender ownership, maximum URI length, guarded/read-only output,
  queue backpressure, replacement and concurrent delivery. The initial guarded
  fixture incorrectly attempted a second reservation inside initialized guest
  ranges; using their runtime allocator corrected the fixture.
- Run7 establishes Home rendering and Gallery scene entry with stable PID/HWND;
  complete Gallery UI rendering, PS return and native application entry remain
  pending.
- `vshruntime2` (PID 4968) passed head-event registration but aborted in
  `GraphicsDevice.ReceiveSetModeEvent` with `0x80020009`. Its guest rendering
  setting was still `Pixels4K[3840,2160]`; `VideoOutStatus res=(1920,1080)`
  described the output rather than the internal target. The staging script's
  former single-match heuristic found eight constants and made no patch.
  Physical-head receive correction is being handled alongside the audited
  internal-resolution patch above.
- `vshruntime3` (PID 3208) verified the internal-resolution patch: PSM reports
  `UI RenderingSettings Resolution: Pixels2K[1920,1080]`. Boot still aborts
  at the same `ReceiveSetModeEvent` error with the previous emulator binary.
  `git diff --check` passed after the staging change.
- The receive failure was traced to PSM registering MAIN head 0 and VR head 1
  on one equeue. Rejecting VR made initialization delete the queue, leaving
  ReceiveSetModeEvent to return EBADF. Both physical heads now register using
  native identities `(0x73 << 48) | (head << 40) | (1 << 32)`, independently
  of application ports. The dual-head regression failed before the fix and
  passed afterward. Parent build9 succeeded; `video_out_system_events` and
  `scanout_layers` both passed (`_TempData/vsh-build9-events.log`).
- `vshruntime4` (PID 30880) confirmed host `WindowCreate width=1280 height=720`
  and guest `Pixels2K[1920,1080]`. Both PSM heads registered on equeue 18;
  the prior ReceiveSetModeEvent exception is gone and boot advanced through
  background login/RNPS loading. The early frame capture is black; no Home
  success claimed. Later commits failed for a 2 MiB allocation and an 8 MiB
  guest stack, ending at `_Throw_Cpp_error(6)`. Post-exit C: free space was
  about 728 MiB, with a 7966 MiB pagefile. Host memory pressure versus an
  allocator regression remains under investigation; no user files removed.
- Build10 added allocation-failure diagnostics without changing allocation
  policy. `vshruntime5` (PID 36548) confirmed Windows `0x5af`
  (`ERROR_COMMITMENT_LIMIT`) at `_Runs/vshruntime5/_kyty.txt:226605`.
  Available commit was 5,287,936 bytes on the first failure, later below
  1 MiB; total commit limit was 33,866,498,048 bytes. Even 16 KiB guest
  allocations then failed. This confirms host commit exhaustion as the
  immediate blocker, not an invalid-address/placeholder error. More host
  commit headroom is required before the next meaningful visible boot.
- After cleanup, run6 survives the allocation failure and renders background.
  It later exits with Windows event 1000: `igvk64.dll+0x1688bd`, integer
  divide-by-zero `0xc0000094`. The apparent compositor disconnect is separately
  traced to duplicate hard-linked Composite instances (see
  `vsh-run6-composite-instance-20261103.md`).
- Run7 (PID 17004) visibly renders Home (`_TempData/vshruntime7-scene.png`).
  Pressing Cross on Media Gallery starts genuine guest NPXS40075 Hermes
  execution and `MountApp runApplication`, with PSM focus transferring to its
  render scene. Gallery background is captured; complete Gallery controls are
  not yet verified. Before/after JSON snapshots record the same PID 17004 and
  HWND 1115024, with frames advancing from 3632 to 4409.
- The run7 external first-fault capture reproduces the Intel crash and locates
  its caller at `Swapchain::DrawLayer` -> `vkCmdSetViewport`. The captured
  viewport is `{2147483648,0,1,0,0,1}`. Logs show a suboptimal swapchain,
  minimize, recreation, then restore. Win32 supplied zero `currentExtent` while
  the cached SDL size remained nonzero; aspect fitting underflowed its offset.
  `Swapchain::Create` now defers zero-sized surfaces through the existing retry
  path, retaining the last valid prepared-frame format until restoration.
  Build12 and run8's three live minimize/restore cycles passed. Capture:
  `%LOCALAPPDATA%/Temp/opencode/first-fault-17004-1793745238929054600.json`
  (normal minidump under 1 MiB alongside it).

## Build12 validation

- Emulator and `virtual_memory_allocation_tests` rebuilt successfully with
  scoped filesystem-equivalent module reuse, zero-extent deferral and idle
  sequence-handler fixes. All five integrated tests passed: `scanout_layers`,
  `video_out_system_events`, `application_symbol_scope`,
  `runtime_program_file_reuse`, and `sequence_event_idle`.
- CoredumpSeq/ReportSeq now register receive/get exports rather than falling
  through successful unresolved stubs. 13.60 receive reaches IPMI vtable `+0x38`
  and syscall `0x252`; kernel `0x80d53270` supplies a zero timeout and the empty
  path at `0x805b126a` returns errno `0x23` (`0x80020023`). Both managed monitors
  dispatch only on success (13.60 `0x3480`/`0x2450`, local 4.03
  `0x3380`/`0x2350`). Exact cache sizes are `0x6ac` and `0x6c0`. These HLE inboxes
  currently model only idle state: real report producer transport is still
  absent. The integrated fixture checks independent initialization, error
  status, copy canaries and 1,024 polls without fabricated dispatch.
- Run8 visibly renders Home and survives three real Win32 minimize/restore
  cycles with PID 23632 / HWND 3934500 unchanged. Frame counters advance
  1428->1588, 1588->1735, 1735->1891; restored capture is
  `_TempData/vshruntime8-restored.png` (frame 1951). Internal rendering remains
  1920x1080 with normal-sized host window.
- Live run8 Composite reuse/dlsym selects handle28/base `0xab0000000`, already
  started, with SystemMonitor getter `0xab00043e0`. Read-only process memory
  confirms its state pointer at `base+0x18030` is nonzero (`0x21d734000`).
  No compositor-disconnect or all-zero CRSUI dispatch messages were observed
  through the Home/minimize verification.
- Run8's native-media attempt reached Apple TV (`PPSA01783`) ShellUI
  `SCE_BOOT_STAT_BIGAPP_LAUNCH_READY`, SplashScreen and LaunchFlow, but did not
  reach `sceLncUtilLaunchApp` or the runtime backend. The shell continued
  rendering its background; Circle and PS did not restore Home. This is a
  pre-launch guest-flow blocker, separate from later Esvm admission. Run8 was
  closed normally through its window. Run9 was also closed to test the next
  startup-order fix; no app-navigation result is claimed for run9.
- Control Center registration audit finds native eboot calls registration NID
  `9h4kSOsmZAE` before late loading `libNativeExtensions.sprx`, losing genuine
  factories including `VshTmProgressEventShellUIModule`. The runner now preloads
  this real provider after Hermes/RnpsBundle. Run10 resolves registration to
  `0x9a0054480` before entry and NPXS40003 proceeds through its real service
  constructors without the missing-TurboModule exception. Complete Control
  Center rendering and PS/Home return remain unverified. Evidence is in
  `vsh-control-center-registration-20261103.md`.
- Run10 later ends with a guest null execute fault (`pc=0`, access=3) around
  4m37s; this is distinct from the earlier zero-viewport Intel driver fault.
  Full startup/app-return stability is therefore not established. Logs remain
  in `_Runs/vshruntime10`. Audit identifies Hermes `what()` dispatch through
  the zero-filled HLE `std::exception` vtable (`dCzeFfg9WWI`), with exact
  Hermes return RVA `0x1394a`. That one overriding object registration is now
  removed so the real libc export can resolve. Build13 passed; live verification
  is pending. See `vsh-launch-flow-audit-20261104.md`.
- Gallery's USB enumeration error is separately verified: its managed caller
  initializes count=-1, while the unresolved native stub returned success
  without writing count. The actual ABI has two output pointers and fixed
  capacity eight IDs. A guest-owned empty enumeration implementation and
  integrated fixture now compile and `usb_storage_enumeration` passes, including
  protected-buffer/canary cases. Emulator build13 also passed; live verification
  is pending. See
  `vsh-gallery-usb-enumeration-20261104.md`.
- Run11 confirms the real exception-vtable binding and DualSense type4 opens,
  but dies in native libc termination after `DynCastProbe` returns null for
  HermesRuntimeImpl -> HermesRuntime. The RTTI walker interpreted VMI flags and
  base count as a possible pointer; the value `0x300000000` collided with a
  readable Hermes heap allocation. The walker now discriminates actual RTTI
  kinds through their native vtable metadata. A registered-export regression
  reproduces the exact address collision and tests bounded plain/SI/VMI paths.
  Build14b and the integrated `floor_rtti_kind` regression passed; live
  verification is pending in run12. (Build14's initial test-namespace link
  error was corrected before the passing build.) Details:
  `vsh-run11-bad-cast-audit-20261104.md`.

## Run12 visible milestones

- Build14b boots Home with internal1920x1080 and the normal1280x720 client
  window. The keyboard/mouse pad opens as verified DualSense ID2/type4.
- Cross opens NPXS40075 in the existing guest Hermes/PSM/native AGC session.
  The complete empty Media Gallery screen renders; Right changes All to
  Favorites and renders its empty-state text. The old negative USB count
  exception is absent. Captures: `_TempData/vshruntime12-gallery3.png` and
  `vshruntime12-gallery-favorites.png`.
- PS opens the genuine NPXS40003 Control Center, including its first-run
  tutorial; completing it reveals the Home action and other controls. Cross
  on Home returns to the Home menu. Captures:
  `vshruntime12-control-center-ready.png`, `vshruntime12-home-return.png`.
- JSON snapshots before Gallery, in Favorites and after Home return all record
  PID29336/HWND3147136. Frames advance1760->14870->20465. No replacement host
  process/window or standalone JS host was used.
- Settings opens via the Home gear and renders its full menu (NPXS40008,
  `_TempData/vshruntime12-settings.png`, frame23575). Circle shows Home, but
  frame progression subsequently stops at24787 during Settings teardown.
  This lifecycle stall is not yet resolved; thread capture is being collected.
- `capture_window.ps1` now requests physical window bounds to match PrintWindow.
  Previous DPI-unaware bounds cropped screenshots at Windows125% scaling;
  this was a capture defect. No host-window size or guest resolution was changed.

## Stopping checkpoint — run13

User requested stopping and committing on 2026-11-04. Run13 (build14b)
reached Home and visibly navigated to Media/YouTube (PPSA01651). Cross entered
the guest splash/LaunchFlow, but the captured screen remained the background;
no native YouTube UI or runtime admission was verified. At this observation,
the native trace contained only the two `sceLncUtilLaunchApp` relocations,
not a handler invocation. The pre-launch wait still needs a thread capture.
Evidence: `_TempData/vshruntime13-youtube-selected2.png`,
`vshruntime13-youtube-launch.png`, `_Logs/vshruntime13.log` and
`_Runs/vshruntime13/_kyty.txt`. Run13 PID27212 closed gracefully through
`CloseMainWindow` when work stopped. Settings teardown remains unresolved;
run12's retained thread capture is available for the next session.

Settings audit: snapshot TID37044 is Mono's GC coordinator at
`libmonosgen+0x22546d`, in `PthreadSuspendUserContext`/`GetThreadContext`,
targeting TID33912 (`SceBgftTMTaskQueueThread`, pthread `0xffdbdf3180`).
The target is in Mono detach/TLS cleanup, waiting through `NativeMutexLock`
on Mono mutex `base+0x4fae28`. Many managed threads
were already suspended before capture; TIDs18892/28648 also wait in
`PthreadJoin`. The follow-up audit withdraws the logging hypothesis:
`0x140413266` is TLS-destructor dispatch, misidentified by nearest-public-symbol
lookup. Coordinator retries were 58/64, not evidence of an indefinite loop.
Neither the critical counter nor mutex owner was proven; no suspension-policy
change or completed fix is justified. See
`vsh-run12-settings-cleanup-audit-20261104.md`.

## Resumed priority: normal app/game launch first

At the user's direction, native media now runs through ordinary `--game`
launch first. Fix common loader/runtime/services there, then reuse the working
path for VSH launches of both apps and games. Run14 retained YouTube's splash
wait snapshot (`_TempData/vshruntime14-youtube-wait.txt` and `.raw.json`) and
closed gracefully; further VSH launch investigation is deferred.

`_Runs/youtube-direct1` uses normal `--game` with firmware13.60, networking,
and a1280x720 host window. It reaches native initialization and shader setup,
then aborts at Starboard `socket_receive_from.cc:59`, "Bad TCP source address".
The immediately preceding unresolved POSIX call is `TXFFFiNldU8`; ABI/causal
audit is in progress. Esvm and other media-service imports also remain missing.
No successful app UI or playback is claimed from this baseline.

## Remaining integration boundaries

Source rollback snapshot after build13/controller verification:
`_TempData/checkpoints/vsh-build13-dualsense-source-20261104.zip` (66 checked
entries, HEAD ID, binary-safe tracked diff and changed/new source files).
Existing rollback branch and profile data are retained; nothing pushed.

- Guest PID, Unix peer, category and authority routing now has an owned registry
  and endpoint capture. Backend registration/retirement and getpid integration
  passed the explicit identity fixture; native client rendering is still pending.
  Configured PID 100 identifies boot ShellUI; null-owner PID-zero app-ID queries
  retain their current firmware LNC context. Runtime apps never inherit ShellUI's
  compositor capability. The category-by-app-ID AGC stub still needs ABI evidence.
- The 13.60 Home Settings URI is `pssettings:play?mode=settings`; Gallery uses
  `psmediagallery`. Scene-local OpenLinkHandler dispatch reaches BootHelper.Boot
  in the existing guest RNPS host. The generic ShellCoreUtil fallback instead
  sends AppMessaging type 0, which native eboot converts to PSM event `0x4c`.
  The delivery implementation now includes the exact initializer/envelope ABI,
  bounded FIFO, replacement flag, output probes and explicit missing-destination
  errors. Firmware 13.60 empty receive is `0x80c50005` (not the old reference's
  `0x80550006`). Integrated messaging tests passed; no scene success claimed.
- Firmware NPXS40008 and NPXS40075 `application.ps.bundle` headers inspected
  here are raw HBC v89. They are reachable through the firmware RNPS mount;
  run7 establishes NPXS40075 Hermes promotion and MountApp execution. Settings
  execution and complete Gallery UI remain unverified.
- Web launch additionally requires actual browser child/IPC/shared-memory and
  surface handling. HTTP transport alone is insufficient.
- YouTube/Apple TV import missing Esvm NIDs `h59QfNm3QKU`, `ouGTOwK1yDc`,
  `39VZ-wS-7mI`; further Esvm/Widevine dependencies remain.
- Arbitrary guest faults and unsupported loader/native-service paths can still
  terminate the host. Intercepting explicit application exit is not general
  crash containment.
- Application-private replacement of pinned native SSL/libc providers is
  rejected; shared native library data is not automatically process-isolated.

Successful native launch requires observed app entry, native client identity,
an application frame, continuing AGC frames, focus handoff and cleanup. A
loaded executable, successful stub return or advancing shell frames alone
does not meet that criterion.

## Direct-app/playback checkpoint — 2026-11-04

The user requested a commit preserving the successful direct-app milestone:

- Native YouTube Home, Search and populated results work through ordinary
  `--game` launch with 13.60 firmware and the normal 1280x720 host window.
- Native Apple TV displays landing artwork/sign-in options; the user confirmed
  navigation works. Apple TV authenticated playback has not been established.
- In YouTube direct19 the user confirmed moving picture **and sound** for the
  6:59 sponsored nature-search result from **Rincon de la Vieja Costa Rica**,
  titled “The One Day Adventure Pass includes the main tours in a…” (truncated).
  The user subsequently focused that exact result for read-only identification.
  This is a verified individual playback result, not blanket format/DRM support.
- Shared code includes native Esvm lifecycle/source/refill, clear VP9 and AAC-LC
  ADTS decoding, native video composition, audio output, networking and loader
  fixes. Unsupported provisioning/protected media remains explicit.
- Startup splash-animation/sound synchronization remains unfinished.

This checkpoint also contains subsequent shared-runtime recovery integration.
Open VSH application launching remains gated: run21 reached ordinary native
LNC admission with 139 unresolved imports and displayed the launch error. The
user manually closed it. Same-window app frames/input/PS-return are not yet
verified. The full emulator and checkpoint test targets build successfully.
All 25 selected checkpoint checks passed across the initial run and targeted
thread rerun. The first run passed 21; four thread regressions exposed a latent
Windows stack-switch operand allocation bug (R14 input overwritten by TEB
bounds). Pinning stack inputs to R8/R9 fixed all four, including actual worker
failure/exit recovery, guest waits, condition deadlines and legacy thread exit.
Application-entry nested recovery and the media/network/audio suites passed.
Evidence: `_TempData/apps-playback-checkpoint-build.log`,
`apps-playback-checkpoint-tests.log`, and
`apps-playback-checkpoint-thread-fix-build.log` / `-tests.log`.

Local evidence retained in `_Runs/youtube-direct19-aac-capability`,
`_TempData/youtube-direct19-user-focused-video.png`, and the direct-app/media
test logs. Profiles, firmware, downloaded applications and local run artifacts
are preserved outside the source checkpoint.
