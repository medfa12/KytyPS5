# Upstream integration verification

Integration checkpoint on `ps5-vsh`: local checkpoint `f3bbe331` and upstream
`dfec14aa`. Rollback branch: `backup/ps5-vsh-before-main-20261103`.

## Integration adaptations

- Updated command processing, shader/pipeline interfaces, queue interrupt identity,
  native dependency startup, scoped filesystem mounts, and module references.
- Kept anonymous SHM transactions and deferred flexible backing alongside upstream
  range trees, page-table budgets, loader memory charging, and backing reader locks.
- Co-hosted firmware without an explicit flexible-memory budget retains the prior
  1 GiB fallback; ordinary titles retain upstream's 448 MiB default.
- Host HTTP accepts the upstream networking configuration and the ShellUI runner's
  `KYTY_NETWORK_HOST=1` setting. Runner boolean options use the new CLI syntax.
- Intel Arc does not expose `shaderResourceMinLod` or `shaderDeviceClock` here.
  MinLod is optional at startup and checked when a shader requires it. The previous
  `UINT64_MAX` realtime-clock compatibility value is retained only when the real
  device clock is unavailable; this is not an accurate timer implementation.

## Evidence so far

- Emulator and principal tests built successfully; latest emulator/filesystem
  build: `_TempData/upstream-merge-build20.log`.
- Host HTTP and HTTP/HTTP2 sync/async/partial-read/EOF/offline checks passed.
- LNC launch/status, ATRAC9, and AGC queue identity checks passed.
- Fragment-quad SPIR-V validation passed all six permutations.
- `upstreamverify3` exposed exhaustion of the upstream 448 MiB budget in the
  co-hosted shell session. `upstreamverify4` passed that point and reached
  compositor shader compilation, then stopped on unsupported device shader clocks.
- Full memory suite passed (`_TempData/upstream-memory-tests5.log`), including
  sparse transfers, rollback metadata, AMM domains, and per-view Windows protection.
- Mutex regression passed (`_TempData/upstream-mutex-tests4.log`) after explicit
  guest cancellation was routed through the guest exit trampoline instead of
  winpthreads longjmp across the guest stack.
- Sparse-backing, retained socketpair, NP/network, and anonymous-SHM checks passed.
- Default filesystem suite exposed double errno conversion in the POSIX fcntl
  wrapper. The wrapper now preserves the shared implementation's POSIX result.
  Build13 passed EBADF checks and exposed unsupported commands incorrectly
  returning success. Subsequent checks passed EINVAL and exposed O_CLOEXEC
  being read after masking flags to access mode, then missing synthetic root
  stat handling. Those repairs passed in the full build20 suite.
- Build13 socketpair-filesystem, guest-FS-context, anonymous-SHM, and both SHM
  shutdown fixtures passed (`_TempData/upstream-*-build13.log`).
- Build13 backing-transfer and all seven selected suspension fixtures passed:
  runtime allocation, logging, munmap, and live/shadow pthread lock acquisition
  before/while held.
- `upstreamverify6` reached connected pad reads and frame 79, but Home did not
  render: UIGamePad's analog conversion threw a managed overflow. The AOT code
  at RVA `0x1018e0` consumes a double angle and converts it to an integer.
  The live `HUbZmOnT-Dg` binding was an integer-return stub. Firmware 13.60's
  LibcInternal export at RVA `0x49910` confirms double arguments in xmm0/xmm1
  and a double result in xmm0; a host atan2 wrapper now supplies that ABI.
  `upstreamverify7` stopped earlier on host memory-commit failures and guest
  allocation abort while regression processes ran concurrently. Visible
  verification remains pending; retry with the tests finished.
- `upstreamverify8` passed analog conversion and logged `OnMenuOpened` and
  `StartHomeUI`, but the frame-88 capture still lacks Home artwork. Hermes then
  faulted in the preexisting `SysServiceZoneAlloc` shim. Firmware identifies
  `cHlIo6CoRTA` as `sceLncUtilGetSpmemAllocatorConfigValue(const char*)`:
  SystemService RVA `0x5910` jumps to `0x69e0`, scanning 16 key/value records
  and returning zero for null/missing keys. The six-argument allocation shim
  was removed, the empty-configuration query registered in LncUtil, and a
  one-argument export regression added to `LncAppStatusTests.cpp`.
- Build16 LNC tests passed, including the corrected allocator-config ABI.
  Filesystem checks progressed through root, path, archive, Unicode, save and
  AIO cases. The local PEEK|WAITALL test was updated to upstream's Windows
  buffered-stream semantics. Build18 confirmed unknown socket options leaked
  Winsock EINVAL into the guest; explicit supported SOL_SOCKET option mapping
  now rejects unknown names with ENOPROTOOPT before calling Winsock.
- `upstreamverify9` rendered the native background (capture at frame 1798),
  but never a usable Home. It exited after the user closed the stuck window.
  `_TempData/upstreamverify9-threads.txt` identifies a retained suspension on
  thread 35416 inside `GuestGpu::SendCommand` during `HandleFault` readback.
  ShellUI thread 33644 was blocked in `GuestGpu::IsStopping`; compositor
  thread 10788 was blocked in `GuestGpu::Enqueue`. Added host-critical guards
  around the fault path and guest-facing command-queue lock operations.
- `upstreamverify10` no longer retained the faulting thread's suspension or
  blocked the two main threads on the GPU mutex. RNPS still stalled: first-fault
  capture identified `ScanGuestCallers` reading `0x75c27c000`, exactly the end
  of its 5 MiB guest stack (`0x75bd7c000`). The speculative diagnostic scan
  entered GPU read-fault handling before its SEH boundary. It now probes each
  slot's readable protection, including guard-page rejection, before reading.
- Build19 passed the socket-option checks and exposed a leftover early P2P
  rejection in Recvfrom that bypassed upstream's bound/nonblocking queue check.
  Removed that unreachable-path regression. Full default filesystem suite
  passed in `_TempData/upstream-filesystem-build20.log`; the symlink fixture
  reported that Windows symlink creation lacked the required privilege.
- `upstreamverify11` exited after 94.68 seconds with `0xc0000409`, after a
  frame-116 capture showed only a dark background/top line. Windows Application
  Error event 1000 identifies Intel `igc-default64.dll` 32.0.101.9033 at RVA
  `0x2169575` as the faulting module. The preceding thread snapshot shows
  thread 13968 deep inside that compiler, while thread 4832 waits for GPU
  unmapping during runtime allocation. The responsible shader and fail-fast
  subcode are not yet known. Run12 enables pipeline/shader diagnostics and
  captures fail-fast exceptions at any PC, including second-chance events.
- Run12 reached frame 272 in the visible capture, still showing only the
  background/top line. It exited with `0xc0000409` in `ucrtbase.dll`; the
  exception capture could not save its JSON due to exhausted disk space.
  All 12 dumped shaders passed Vulkan 1.3 `spirv-val`, including the 871648-byte
  vertex module `VS_a05705cd1fa8a935_3b798196c89cc01a.spv`.
- Build21 adds compute-pipeline begin/end tracing. Run13 reached frame 495
  (8 FPS at the thread capture), progressed through further shader pipelines,
  and was stopped by the runner at its 512 MB guest-log cap, not a captured
  fail-fast. Completed integration-run logs were NTFS-compressed losslessly.
  Build22 adds `KYTY_PIPELINE_TRACE=1` for low-volume pipeline tracing without
  full graphics debug dumps. Neither run establishes usable Home.
- Run14 reached native Home text, Games/Media tabs, search/settings and blank
  cards (frame 1339, 22 FPS), later continuing compositor frames at up to
  58 FPS. The 240-second fail-fast watch timed out without a matching exception.
  R1 input did not change tabs. The home-state thread snapshot places ShellUI
  in native Composite RVA `0x3bd1`, waiting on its post event, while the GPU
  continues draws. Original icon PNG files opened/read successfully, but
  artwork/profile and navigation remain unverified. Close requested through
  WM_CLOSE stalled; the diagnostic process was subsequently terminated.
- Found a retained/upstream mismatch: `IsSocketReadReady` only checked host
  select, omitting upstream's Windows transport-owned bytes after MSG_PEEK.
  It now includes `HasPendingReceive`, matching select/epoll. Added an actual
  EVFILT_READ regression around buffered and drained stream data; build23
  and the full filesystem suite passed. Live causality remains unproven.
- Audited user's reported regressions against `f3bbe331`: the ATRAC9 decoder
  changes remain, `prepare_shellui_databases_1360.py` is unchanged, external
  app mounts remain (runner changes only adapt the graphics-debug CLI flag),
  and the fragment helper-lane implementation remains in the updated emitter.
  This is a merge into the local checkpoint, not incremental cherry-picking.
- Run15 was closed normally by the user (710.58 seconds, exit 0). It still
  showed blank cards and did not navigate, despite continuing compositor
  frames (53 FPS at frame 639, 43 FPS at frame 13982). Keyboard PS delivery
  reached `HostInputKey`; a native PSM snapshot during held Right still showed
  zero controller/pushed buttons, with user 268435457, hardware class 9,
  focus 0 and blocking disabled. The later thread snapshot caught ShellUI
  actively writing a log, so a sampled Composite post wait alone does not
  establish a persistent deadlock. Baseline documentation also confirms
  compositor/coredump notices existed during working navigation.
  Auditing the upstream pad-port lifecycle against retained privileged opens;
  build24 adds bounded failed-UI-read and explicit close diagnostics.
- Run16 confirms the pad regression: two type-4 device opens and one type-5
  probe all return handle 1; the probe closes it, then every captured
  `PadUiReadState` returns `0x80920003`. The old close was a no-op, but upstream
  introduced real port lifetime. Added counted ownership for privileged
  system-device opens while retaining duplicate rejection for public PadOpen.
  Controller regression covers probe closes preserving input, final-owner
  closure, and privileged-only ownership. Build25 succeeded; the controller
  suite first hit an older integration mismatch in its invalid-tuple fixture:
  it expected type 2 to be rejected although the retained VSH code accepts
  that alias. The fixture now checks type 3 as invalid and verifies type 2's
  retained alias explicitly. Build26/controller suite passed.
  The diagnostic run closed normally on request (149.36 seconds, exit 0).
- Run17 after counted device ownership: held R1 is visible as `0x2000` in
  both native PSM controller state and pushed-button state, unlike run15's
  zeros. No bounded failed UI reads appeared after the probe close. Visual
  navigation, controls, BGM and navigation sounds were confirmed by the user.
  Ten bounded BGM PCM windows were nonzero, including front-channel peaks
  0.041168/0.035461. Cards remained blank white. The user's Apple TV launch
  opened/crashed a separate subprocess; they confirmed that behavior also
  existed in the best pre-merge build. Same-session app launching remains
  the subsequent task. ShellUI closed normally for restart (310.21 s, exit 0).
- Run18 restarted the original persistent store and app mounts. Screenshot
  `_TempData/upstreamverify18-home.png` confirms rounded white cards at frame
  1140/55 FPS. Closed normally for the artwork rebuild (306.55 s, exit 0).
- Found a lost behavioral requirement from `c56d4e8f`: integration replaced
  explicit storage-image publication at GL2-writeback releases with reliance
  on read faults. The corresponding test had also been changed to explicitly
  invoke a read fault, no longer checking publication before the release.
  Restored storage-write tracking and synchronous GPU/priority-copy completion;
  pass GCR GL2_WB (GCR bit 9, packet bit 21) through upstream's ReleaseMem API.
  Restored the tiled-storage regression's direct backing read without injecting
  a fault. Build27 built both emulator and GPU test executable successfully.
  `--buffer-cache-range-only` could not reach the regression: VulkanHarness
  rejects this GPU for unavailable barycentrics/64-bit LDS/image atomics.
  This is an unexecuted GPU test, not a passing result. Run19 is the visible
  artwork verification using the rebuilt emulator.
- Run19 visibly restores Games artwork (frame 1455) and Media artwork
  (frame 2520/31 FPS): Spotify, Apple TV, Twitch, YouTube and Netflix, with
  rounded cards and full-scale Search/Settings. Native R1 state/pushed bits
  are `0x2000`; ten BGM PCM windows are nonzero. The Games/Media captures are
  retained in the external asset backup, not this source distribution.
- The same texture-cache audit found that host-media images had lost their
  guest-registration exemption. Allocate them directly in the image slots,
  acquire their sampled views without guest refresh, and exclude empty host
  backing from stale-registration rebinding. This preserves upstream's strict
  registration checks for guest-backed textures.
  Build28 and the media framework lifecycle/descriptor tests passed. Run20 uses
  a separate copy of the previously verified logged-out `Utilisateur1` store
  to exercise native login video without altering the normal Home store.
- Run20 confirms native `psbutton_press.mp4` loading at 1000x1000 and repeated
  sampled media uploads. A 75-frame capture verifies expanding/fading rings
  across several cycles and moving particles. The contact sheet is retained
  in the external asset backup; local video:
  `_TempData/upstreamverify20-login.mp4`. Closed normally after verification
  (180.81 s, exit 0). Run19 also closed normally (242.56 s, exit 0).
  Run21 returns to the original Home store/app mounts on build28.
- Run21 ended after 432.84 s with `0xcfffffff`. Windows Application events
  1002/1001 at 20:49:27 identify a not-responding closure (`AppHangB1`), not
  a recorded faulting-module exception. Home startup was logged; no runtime
  launch call was found in the final run log. No live stack was captured, so
  whether the hang happened during Home use or window closure is unresolved.
  This run does not establish an artwork regression or implicate Apple TV.
- Run22 reproduces the normal Home configuration with a bounded read-only
  window watch: capture all native stacks if Windows marks the titled ShellUI
  window hung for 10 seconds, or its frame counter stops for 45 seconds after
  frame 300. The initial frame-172 snapshot shows the UI thread normally
  waiting in SDL's event loop and GPU thread waiting for queued work; neither
  is held in storage writeback. Watch evidence uses
  `_TempData/upstreamverify22-hang.{jsonl,txt,raw.json}` when triggered.
- Run22 subsequently reproduced the Windows hang at frame 172. The capture
  contains 460 threads, 72 already suspended. UI thread 23308 is blocked in
  `WindowContext::UpdateTitle` -> `fmt::vformat` -> `RtlAllocateHeap`, while
  GPU thread 6956 is idle. A live read of heap critical section `0x5502c8`
  identifies owner `0x60d0` (thread 24784), recursion count 1. That thread has
  a retained suspension inside `GetAdaptersAddresses`, called by
  `NetCtlGetInfo` from `NetCtlCheckCallback`. This directly ties the blocked
  host allocations to the suspended adapter-query thread.
  Added `HostLockCriticalGuard` around the complete host network query,
  including buffer destruction. The `--network-query-suspend-only` fixture
  holds a nested native wait for 2.5 seconds and checks that the production
  suspension path lets the protected query complete before retaining a
  suspension. Run22 was terminated after evidence collection (366.78 s).
  Build29 exposed a misplaced test CLI block; it was moved into the Windows
  dispatch in `main`, using the existing failure-reporting test runner.
- Build30 successfully linked the emulator and memory/suspension test target.
  `virtual_memory_allocation_tests --network-query-suspend-only` exited 0.
  Build log: `_TempData/upstream-merge-build30.log`. Run23 verifies the fix
  using the original Home profile store, external app mounts and host network.
- Run23 advanced to frame 6698, with no Windows-hung samples, before exiting
  after 261.28 s with `0xc0000409`. Windows events 1000/1001 identify
  `ucrtbase.dll` 10.0.26100.9711, RVA `0xa527e`, fail-fast parameter 7
  (`FAST_FAIL_FATAL_APP_EXIT`). This establishes an abort, but not its caller.
  Ten BGM PCM samples were nonzero. The early screenshot only captured the
  startup background, so this run adds no artwork-verification claim.
  Run24 attaches `capture_first_fault.py` for any-address `0xc0000409`,
  including second chance, with a 64 KiB stack and bounded normal minidump.
- Run24 visibly confirms Games artwork at frame 2594 and Media navigation/artwork
  at frame 4001. Native PSM state and pushed bits both show R1 (`0x2000`).
  Captures: `_TempData/upstreamverify24-games.png` and
  `_TempData/upstreamverify24-media-focused.png`. Exception capture and the
  bounded hang watch remain active for sustained verification.
- Run24's 600-second window/frame watch completed without detecting a hang.
  The subsequent Media capture at frame 20604/44 FPS retains all app icons.
  L1 also reaches native PSM state/pushed bits (`0x400`) and starts the return
  transition to Games. The separate abort monitor is still active; run23's
  abort caller remains unresolved.
- Run24 completed 938.35 seconds and exited normally (`0`) after `WM_CLOSE`.
  Final capture shows Media artwork and Twitch selection at frame 29276/60 FPS.
  The 900-second any-address fail-fast monitor timed out without observing
  `0xc0000409` and detached cleanly. Together with the 600-second hang watch,
  this verifies one sustained post-fix session and normal shutdown; it does
  not resolve the intermittent run23 abort. Final evidence: the stability
  screenshot retained in the external asset backup and
  `first-fault-36184-1793736816851205200.json` in the OpenCode temporary directory.

All 61 textual conflicts are resolved. The staged whitespace check passed.
The merge is checkpointed after the regression checks and sustained run24.

Post-merge Home navigation, audio, Games/Media artwork and native login video are
verified. Profile-selection rendering has not been separately verified; the
normal Home fixture and original profile store remain retained. Same-session
app launch is the subsequent task. The checkpoint remains unpushed. Known open
items include run23's intermittent abort, GPU regression execution blocked by
this device's capabilities, and Linux backing-replacement verification.
