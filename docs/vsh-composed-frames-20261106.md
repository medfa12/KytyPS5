# Parent-composed subprocess scanout — implementation work

## Repair

The subprocess remains isolated, but its HWND remains hidden. Completed child
scanout pixels are copied through a three-slot host frame mailbox and uploaded
as the parent's application plane. Parent overlay bus1 and native AGC bus256
are drawn above that plane by the existing production compositor.

The child exports its final application scanout, including its own native-video
and overlay paths. GPU submission completion and mapped-buffer invalidation
precede publication. The parent uses its own upload buffer, prepared-frame pool,
and scheduler; no GPU resource or guest pointer crosses process boundaries.
The published format and HDR tag preserve the existing presentation conversions.

There is no external-memory or external-semaphore implementation in the current
graphics backend. This implementation deliberately uses bounded CPU staging,
not an unvalidated cross-VkDevice handle import. It costs readback/upload bandwidth
and a GPU completion wait per child presentation. Shared GPU transport remains a
future optimization, not a prerequisite for correct alpha/layer ordering.

## Ownership

* Each launch owns a uniquely named Win32 mapping keyed by its full app/sequence/
  nonce identity. The parent creates it before starting the child; the child
  validates the header and key before advertising the composed-frame capability.
* Three slots use interlocked free/writing/ready/reading ownership. The producer
  may replace unread frames, but cannot overwrite a reader. No cross-process
  mutex remains held if a process crashes. Memory is bounded to three 4K RGBA
  frames. Oversized host windows proportionally cap the hidden child surface.
* Parent mappings are pinned while read. Retained GPU frames belong to the parent
  and are retired through the prepared-frame pool. A new launch key clears old
  pixels; stale retirement/visibility requests cannot change a new generation.
* Native ShellCoreUtil masked UIStatus writes control application visibility
  independently of its lifetime. Host PS pulses do not manufacture overlay state.
* LNC focus accepts the actual registered, in-parent ShellUI application (type1)
  as a host-system target. Previously the backend incorrectly required an
  external process token for this target, rejecting a controller-only transfer.
  Events and the LNC registry retain the real ShellUI app ID; only the host
  subprocess command maps it to system ownership. Unknown IDs remain rejected.
* Keyboard focus stays on the parent. Existing brokered input, held-input
  suppression, and break-before-make focus acknowledgements remain the input
  path. Audio continues to follow application display focus rather than a
  controller-only overlay handoff.

## Verification

`runtime_child_composed_pixels` uses two instances of the full component-test
executable, production hidden Vulkan presenters and the production mailbox.
It checks blue application pixels, a half-alpha red system layer producing
purple pixels, controller-only handoff, overlay dismissal, native visibility,
green updated child pixels, resize, forced retirement, fresh-key relaunch and
clean close. Test-only hooks supply solid frames and read back final scanout;
they do not substitute a separate blend implementation.

Build: `_TempData/vsh-composed-build3.log` completed successfully. The test run in
`_TempData/vsh-composed-tests.log` passed all six selected tests:

* `runtime_child_composed_pixels` (4.76 seconds)
* `vsh_subprocess_lifecycle`
* `vsh_subprocess_input`
* `vsh_subprocess_embedded_vulkan` (legacy direct-embedding fixture)
* `vsh_subprocess_controller_focus`
* `vsh_subprocess_child_controller_focus`

The existing LNC-token fixture also adds a registered native ShellUI transfer;
its final rebuild/check and an additional mailbox concurrency test are pending.

## Native live acceptance and switch diagnosis (`rvsh5`)

Regular 13.60, retained `rvsh2` profile/stage, executable
`kyty_emulator.composed.exe` copied from build3:

* Parent PID34288 and Apple TV PID35908 (app104/sequence2).
* `_TempData/rvsh5-appletv.png`: Apple Account Sign In is visibly presented by
  the parent. This is application UI evidence, not PSN authentication.
* `_TempData/rvsh5-control-center.png`: native Control Center is visibly above
  Apple TV. Controller focus transfers to registered ShellUI app103 while
  display focus remains app104 (`_Runs/rvsh5/_kyty.txt:853768`).
* The user independently opened Apple TV, Control Center, returned Home, and
  attempted YouTube. Home's app103 display/controller requests appear at
  lines1148162–1148164; UIStatus clears `20436 -> 20416 -> 20410`.
* YouTube reaches LNC (`PPSA01651`, app105, line1171150), but no child is created.
  Failed launch events appear at lines1171214 and1171434, app/pid=-1. Apple TV
  is still alive. Repeated splash/Control Center transitions and AGC output
  continue: this does not establish a stopped parent render loop.

The source path rejects an occupied subprocess slot with kernel EBUSY before
`RegisterRuntimeLaunchPath`, leaving its event type Unknown (0). Native 13.60
`ReactNativeApps.LncEventExtender.IsLaunchFlowTarget +1df10` accepts BigApp (5),
MiniApp (4), or Invalid (-1) launch-end, not Unknown. Thus the existing splash
never receives an actionable typed completion. The trace did not log the
numeric backend result; new diagnostics include result, app type, and existing ID.

Native switch protocol was checked against the actual 13.60 AOT image:

* `LaunchFlow.AutoLaunch +17520`, jump table at `e4154`: `80940010 ->185ca`,
  non-debugger branch `188bf`, reads EventArgs.ExistingAppId at +64, invokes
  `KillAppAsync +19bc0` at `18927`, then continues the requested launch.
* `8094000c ->17d74` exits the launch flow for an already-running title;
  AppSystem handles focus to the existing application.
* Reference managed code independently shows the existing-ID close/retry
  behavior. Actual AOT bytes, not reference stub bodies, verify these constants.

Repair pending build5/live validation:

* Classify resolved native requests before admission, then emit the native
  conflict with a validated immutable existing-process key. Preserve the live
  app; let native ShellUI issue close/focus/retry.
* Close waits on the host pump's actual process/job retirement before returning
  to native KillAppSync. The existing Dispatching/deferred-completion mechanism
  preserves KillStart/End ordering. Process kill exit codes do not become a
  false failed teardown after actual retirement.
* Registered LNC coverage checks different/same-title conflict payloads, stale
  nonce rejection, no implicit focus/close, and no duplicate failure events.
* Parent remote-revision refresh now permits `PresentLastFrame()` without a
  local guest plane; composed-pixel coverage asserts that path.

Build5 completed successfully (`_TempData/vsh-switch-build5.log`). All seven
selected tests passed in 4.77 seconds (`_TempData/vsh-switch-tests5.log`):
LNC tokens/conflicts, composed pixels (3.11s), subprocess lifecycle, input,
frame mailbox, parent controller focus, and child controller focus.

The user authorized restart and live switch verification. The old parent was
stopped, and `rvsh6` started as PID19664 using the newly built executable and
the same regular-firmware profile.

## Live switching and focus/audio follow-up (`rvsh6`, build6)

Native switching completed in both directions:

* Apple TV app104/PID24448 -> conflict app105 with existing104 (`80940010`)
  -> native close104 -> actual process exit0/error0 -> KillStart/KillEnd
  -> YouTube app106/PID19620.
* YouTube app106 -> conflict app107 with existing106 -> native close106
  -> actual process exit0/error0 -> Apple TV app108/PID15788.
* User confirmed Apple TV boots again after switching back. Some YouTube videos
  play in their testing; codec/network explanations remain unproven.

The retained YouTube run also exposed a separate readiness cycle: its display
focus request waited for a first completed frame while the app waited for focus
before rendering. `Process::FocusReady()` now uses authenticated entry, window,
and control readiness independently of first-frame presentation readiness. The
two-process composed-pixel fixture now requires focus before the child can emit
its first frame.

Parent shell audio policy now mutes only while the displayed child also owns
controller focus (`foreground && controller`). Native controller handoff to
ShellUI permits Control Center sounds while the displayed app retains audio.

Build6 component checks passed: `runtime_child_composed_pixels`,
`vsh_subprocess_lnc_tokens`, `vsh_subprocess_audio_focus` (8.27 seconds total;
`_TempData/vsh-focus-tests6.log`). Production executable linked successfully in
`_TempData/vsh-focus-emulator6.log`. Live focus/audio acceptance is pending.

## Control Center modal investigation

User observed icons initially appearing, then fading to near-black with a clock.
`_Logs/rvsh6.log:2664-2678` shows BGS missing-data reporting, native opening of
`feature-discovery-modal`, focus transfer into that modal, then `indexOf` on
undefined and HostFunction exceptions. This timing identifies a modal path to
investigate; it does not yet prove either error causes the blank contents.

Regular firmware NPXS40003 is plaintext Hermes v89. Targeted disassembly and
decompilation artifacts are `_TempData/cc1360.hasm`, `cc-modal.js`,
`cc-modal-paths.js`, and `cc-welcome-animation.js` (P1sec/hermes-dec).

* Function38803/38805 checks native AsyncStorage completion/skip records;
  absent records schedule the tour after 500ms. Function38807 hides the ordinary
  Control Center through CaptureMenu before opening the feature-discovery UI.
* Function38815 builds welcome text and all three steps from bundle literals;
  the welcome config is not supplied by the failed BGS lookup. No basis yet to
  populate synthetic BGS records or suppress the native tour.
* Function38845 creates the modal; function38858 renders clock, welcome/step
  contents, and keyguide. Function38861 starts with opacity1 and requests a
  native-driver animation to1, so an initially zero JS opacity is not supported
  by the bytecode evidence.
* There are 195 functions referencing `indexOf`; the log lacks a JS stack, so
  the exact receiver is still unresolved. The native guest Hermes is in use,
  not the separate host `libHermesVm` implementation.

User authorized the next driven verification run. `rvsh7` started as PID32276
using build6, regular13.60, retained rvsh2 stage/profile, and regular vsh_asset.
The parent owns the sole emulator RAM slot. Media work continues in the separate
`media-app-playback` worktree with lightweight static-analysis permission only;
heavy builds and live launches require explicit coordination.

### Build6 live confirmation (`rvsh7`)

User confirmed app switching now works and requested commit/push. The parent
capture `_TempData/rvsh7-home.png` (despite its filename) visibly shows YouTube's
Get started screen. Control Center's full transition/audio remains a separate
unresolved acceptance item.

Orphan check after Apple TV -> YouTube: `_Runs/rvsh7/_kyty.txt:760720` records
actual Apple TV app104/sequence2/PID15356 exit0/error0 with ShellUI retained.
The next launch is YouTube app106/sequence4/PID27392. Windows process inventory
finds only shell PID32276 and YouTube PID27392 (parent32276); no Apple TV process
remains. Five RNPS staging regression tests also passed before commit.
