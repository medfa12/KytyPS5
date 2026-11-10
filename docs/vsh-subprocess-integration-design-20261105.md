# VSH standalone-subprocess integration: design/ownership handoff — 2026-11-05

## Decision and scope

**Reuse the working standalone native app path in a separate host process.**
Keeping apps in ShellUI's process was an incorrect assumed requirement, not the
user's requirement. Preserve the shared-owner/rwlock checkpoint for rollback;
do not keep widening its app dependencies to reach the same outcome.

This is a source audit and staged proposal, **not implementation or live proof**.
No graphics/window/controller/audio/loader production edits were made for this
design. No launch, input, process close, emulator write, profile or mount edit.
The user's direct YouTube PID25648 is reserved and untouched. Parent retains
OOBE; Astra retains LoginMgr/NP. Ownership below is proposed and needs parent
agreement before cross-cutting edits. This new document is ignored by `/docs/**`
and needs explicit force-add if the parent wants it committed.

## Concrete existing seams and gaps

| Existing source | Proven behavior / integration consequence |
|---|---|
| `loader/runtimeLaunch.cpp:43–124` | Already constructs a standalone `--game` child, quotes Windows args, uses `CREATE_SUSPENDED`, restricts inherited handles to its log, assigns a job, resumes, then waits on the real process handle in a detached thread. Reuse this launch shape, not a new guest loader. |
| `runtimeLaunch.cpp:128–130` | `options.vsh_session` immediately selects `ConfigureVshRuntimeLaunch` and **bypasses** the subprocess backend. Merely enabling the old subprocess handler does not integrate it with VSH. |
| `runtimeLaunch.cpp:130,148` | Non-VSH route sets `ConfigureVshRuntime(false)` and registers only a launch callback. No close/focus callback, child protocol, first-frame or entry notification. `RuntimeLaunchExited` cannot complete a VSH Running app that was never accepted/started. |
| `runtimeLaunch.cpp:107–121` | Assignment/resume failure calls `TerminateProcess` then closes handles without waiting; waiter-thread construction can fail after resume. Replace both with a retained per-run control record and actual process/job termination gates. |
| `runtimeLaunch.cpp:72–77` | Copies nearly every parent extra mount except a short blacklist. This can expose writable shell/profile paths to a second writer. Do not reuse this mount policy unchanged. |
| `runtimeVshLaunch.cpp:121–169` | Already resolves/checks the native plan, stable guest launch paths, acceptance ordering and one active run. Reuse those host contracts, but do not call its `Prepare`, private relocation or `ApplicationMain` for standalone children. |
| `runtimeVshLaunch.cpp:173–196,300–366` | Same-process owner stop/drain, retained mappings and owner cleanup are specific to that backend. A standalone child instead retires by its actual retained host process/job handles; do not fabricate an in-parent `Program*`/execution token. |
| `libs/libLncUtil.cpp:395–539` | Host acceptance/start/failure/close/exit state machine exists. Start requires Accepted; quiescent exit requires Running; close captures immutable launch sequence and defers KillEnd behind KillStart. Extend host token checks for subprocess generations, rather than weakening this to OS-PID matching. |
| `libLncUtil.cpp:557–575` | App/controller focus changes currently only change local IDs and queue start/end events synchronously. They do not switch windows, child input or audio. Add a real backend transition/ack path outside LNC locks. |
| `libLncUtil.cpp:599–614` | Direct programs without `application_owner` are granted controller policy locally. A standalone child cannot inherit same-process focus behavior automatically; it needs an explicit host-side child focus gate. |
| `graphics/presentation/window/window.cpp:779–855` | One SDL-owned Vulkan window per process; main-thread creation and event loop. Initial flags are Vulkan + resizable, optional fullscreen. No embedded-child mode. |
| `window.cpp:418–426` | Window keyboard focus and close-request events here only log. They are not VSH focus/close transactions. |
| `window.cpp:297,793`; `hostInput.cpp:712–718` | Each process initializes SDL gamepad and opens physical pads on device events. Guide/PS sets the controller bit and calls that process's `ShellFocusHost::PulsePsButton`. Starting an ordinary child unchanged would allow duplicate physical/HID ownership and send PS to the wrong process. |
| `libs/controller.cpp:1386–1443` | Existing pad reads already have same-process focus-neutralization hooks and retained reports; reuse the normalized state machinery, not fake second physical devices. Output/haptics also need a broker focus gate. |
| `libs/agc.cpp:82–86` | The same Config screen width/height currently feed **both** `WindowInit` and `VideoOutInit`. `--screen-width 1280` alone is not an independently specified 1920×1080 internal configuration. Separate host extent from internal/default guest video dimensions deliberately. |
| `window/swapchain.cpp:1378–1447` | Presentation retries, successful `Present`, actual layer frames and a presented-frame counter exist. This is the right seam for a tagged guest-frame-present notification, not process creation/window creation/splash refresh. |
| `libs/audio.cpp:315–356,548–569,742` | Real SDL audio streams, stream gain and active consuming playback clocks exist. Child focus policy must reach both ordinary AudioOut and native media outputs; do not pause a device in a way that wedges guest queue/backpressure waits. |
| `emulator.cpp:114–123,462–500` | Relative temp/download/data roots can be isolated with a per-run working directory, but later explicit mounts override them. Child storage must be assembled from an allowlist, not assumed safe because cwd differs. |
| `emulator.cpp:191–221` | Child window-loop return currently allows 500 ms grace then `quick_exit(0)`. That is not guest thread quiescence, but OS process termination is a usable isolation/retirement boundary **after actual wait**, with the correct failure/exit classification. |

## Minimal coherent architecture

### 1. Parent-owned process ledger, explicit child role

Keep ShellUI and its existing system co-hosts alive in the parent. Launch native
apps with the proven ordinary `--game` route, **without** child `--vsh-session`
or parent `--co-elf` arguments. Add an internal child-control role; do not globally
change ordinary manually launched direct apps. Keep the old shared-owner backend
available only as an explicit diagnostic/rollback choice until parent decides
whether to retire it.

Each launch has copied `{app_id, launch_seq, session_nonce, generation, plan,
requested_user}`, retained actual process handle/PID, per-run job, control/input
channels and all window/input/audio acknowledgements. OS PID, LNC app ID, child
guest PID and parent module IDs remain different namespaces. Do not put an OS
PID into the guest wire merely because it is convenient. Explicitly register a
child guest process identity and authoritative plan metadata; parent has no child
`Program*` to pass to `RegisterProgramLaunch`.

Use two inherited anonymous pipes (or an equivalently restricted local channel)
with parent endpoints non-inheritable and **only** the child's endpoints/log on
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. A versioned bounded protocol binds every
message to the app/sequence/generation and nonce, validates message size/type,
and rejects stale/duplicate/out-of-order events. Do not expose a general HWND,
callback-pointer or unauthenticated named-service command interface. Parent
validates child `HWND` ownership against the retained process before native
window operations. No secrets or native account credentials in command lines.

Prepare the wait/control machinery and retaining record **before ResumeThread**.
On any post-create failure, terminate only this child job as necessary and wait
for its original process handle; keep the record if wait/control fails. Prefer
a per-run job so closing one app cannot kill other managed children or ShellUI.
Track descendants/job empty as well if descendants are allowed; a root process
handle alone does not establish every job member's exit.

### 2. Truthful milestones and close/relaunch

```text
Requested -> plan/path validation -> Accepted
 -> ChildCreated [host diagnostic only]
 -> Hello/WindowReady [host resources only]
 -> GuestEntryAdmitted [existing LNC Started semantic]
 -> FirstGuestPresent [separate real presentation milestone]
 -> foreground <-> Home/background [same live process]
 -> CloseRequested -> child host shutdown / targeted job termination
 -> actual process wait (+ job empty if applicable)
 -> retire input/window/audio channels -> LNC completion -> active-slot release
 -> relaunch: new process, sequence, generation and channels
```

Do not call `RuntimeLaunchStarted` on `CreateProcess`, ResumeThread, a window
handle or log text. Child sends entry-admitted from the ordinary direct entry
seam, immediately before actual guest entry; that is not `main` readiness. A
separate first-present signal comes only after successful presentation of a real
guest frame in this child/generation, with non-null app frame provenance—not a
splash, host overlay, empty refresh, VideoOutOpen or failed/out-of-date present.
This proves present acceptance, not physical monitor scanout or playback.

Keep ShellUI visible while preparing the child; reveal the child surface only
under an explicit presentation-ready policy, not false success. Child death
before entry must terminate the accepted launch with an actual failure. Explicit
close while Accepted/Requested needs a defined terminal cancellation path in LNC
(currently only Running produces normal quiescent exit); preserve event order
and immutable sequence checks rather than force it through a fake Running state.

Home hides/backgrounds the child and restores ShellUI focus without ending the
process. Re-enter restores it without creating a new app generation. Close
requests the child's **host UI shutdown**; do not claim it runs native atexit or
that a native graceful-close callback was recovered. Forced or timed-out close
may terminate only the matching child job, then **must wait**. Never publish
KillEnd/relaunch eligibility merely because close was accepted, pipe disconnected,
window vanished or `TerminateProcess` returned.

### 3. Display: embedded child HWND first; shared-GPU composition is not minimal

**Recommended first visible integration: a child-owned Vulkan HWND embedded in
the parent's normal top-level client area.** The parent keeps one ordinary
1280×720 resizable window; a child owns its own HWND, SDL event thread, VkSurface,
device/queues and swapchain. The child sets appropriate `WS_CHILD`/window styles
and `SetParent` **before** creating its Vulkan surface, and validates the passed
parent HWND belongs to the expected live parent process. Resize/show/hide
commands are handled on the owning child SDL/main thread. The parent clips its
client rendering around the child (`WS_CLIPCHILDREN` as appropriate).

Feasibility evidence: SDL exposes its own Win32 HWND through
`SDL_PROP_WINDOW_WIN32_HWND_POINTER`; its Windows implementation supports a
native `SetParent` path for `WS_CHILD` windows. However SDL's ordinary
`SDL_SetWindowParent(SDL_Window*)` does **not** accept a pointer to another
process's SDL object. Do not wrap the parent's foreign HWND as the child's SDL
window: SDL's external-window setup subclasses WndProc and that is not a safe
cross-process shortcut. Use the child's own HWND and WSI surface.

This needs an isolated two-process Vulkan spike before promising compatibility:
consistent DPI awareness before window creation, Windows cross-process SetParent
DPI behavior, resize/minimize/restore and surface recreation, parent death,
Alt-Tab, keyboard focus, no deadlocking synchronous cross-process window messages,
and no extra top-level flash/taskbar entry. Main-thread queued work must not wait
while holding Vulkan/renderer/LNC/control locks.

Embedding yields the same top-level window and full Home return without sharing
GPU resources. It **does not** magically allow the parent's swapchain to draw a
translucent system overlay above a native child window. Initial Home behavior
should hide the child and show full ShellUI. A later overlay can use a separate
system HWND or a compositor contract; identify that as a separate stage.

Alternative: parent composition of exported Vulkan images. This requires an
external-memory/external-semaphore-capability audit on the selected device/driver,
Win32 handle duplication, compatible adapter/device identity, bounded image ring,
GPU ownership/layout and semaphore sequencing, color/HDR/resize generations,
and retiring every outstanding GPU reference after child death. A CPU shared-memory
frame ring avoids external GPU sharing but adds readback/copy latency/bandwidth
and still needs color/timing/resize ownership. Both are substantially larger than
embedding and should not be smuggled into the first lifecycle patch.

Separate window extent settings from internal/default VideoOut dimensions:
**1280×720 host client, 1920×1080 internal target**. Child follows parent client
resizes for presentation while guest buffers remain 1080p unless the guest
actually reconfigures them. Respect aspect/DPI; do not force fullscreen,
maximize or desktop resolution. Maximizing/restoring is the user's choice and
must survive Home/re-entry without resizing/recreating their top-level window.

### 4. Exactly one physical DualSense owner and one guest pad

Parent alone owns physical SDL/HID/DualSense hardware and system Guide/PS input,
including when the child has keyboard focus. Use one parent-brokered emulated pad
in the child: disable child physical gamepad enumeration/opening and direct HID,
speaker/haptics initialization for this child role—not for manual direct launches.
Forward normalized connection/capabilities, buttons/axes, touch/motion and report
sequence/time via a bounded ring/latest snapshot. Maintain a real bounded report
history for `PadRead`, not just a success-shaped dummy pad or duplicated devices.

Route Guide/PS to **parent** `ShellFocusHost::PulsePsButton` and system observation;
do not invoke a second child shell focus service. Parent must receive system input
while not keyboard-foreground (explicit background-input policy), not rely only
on the OS focus flag. Focus epochs select ShellUI or one child for ordinary input.
Neutralize/flush the old endpoint and stale reports; suppress held activation
buttons until release across a focus switch to avoid pressing launch/confirm in
both destinations. Disconnect sends neutral state. Child output/haptics commands
carry the epoch; parent rejects stale/background commands and zeros motors on
focus loss/exit. Guest handles remain normal emulated handles, not host SDL IDs.

### 5. Audio and non-destructive storage

Use separate child SDL audio streams initially, not a new cross-process PCM
mixer. Add a focus-generation gate to **all** child ordinary AudioOut and native
media outputs; future streams inherit the current gain. Home/background mutes
child audio while preserving consumption clocks and bounded queues; blindly
pausing a sink can wedge guest backpressure waits. Parent system sounds and shell
media follow an explicit foreground/overlay policy. Controller-speaker/haptics
remain parent-owned and focus-gated. A child process exit retires its OS audio
resources; parent still neutralizes any brokered hardware output.

Replace the current extra-mount blacklist with an explicit child manifest:
read-only firmware/content and selected non-secret launching-user metadata;
private per-launch temp/socket/log working directory; explicit per-title writable
app data under a dedicated child store. Never forward writable shell `/data`,
`/system_data`, `/user` or a complete profile root by default. Parent OOBE/user
store remains the sole writer; child reads a snapshot/read-only view or uses a
narrow service later. `libUserService.cpp:203–225` truncates/writes its entire
JSON file, so sharing that file writable across processes is not safe.

Do not delete/remount/reset existing profiles or dumps. Per-session nonce + launch
sequence makes new logs/temporary roots collision-free across parent restarts;
current `title-appId` plus `CREATE_ALWAYS` can otherwise overwrite a previous run.
Persistence/relaunch uses a chosen per-title store with explicit locking, not a
copy-everything or wipe-everything operation. Do not infer authenticated NP/device
state from a snapshot. Astra's LoginMgr/NP implementation remains independent.

## Proposed staged patch and file ownership

No files below are newly claimed for editing by this audit alone.

| Stage | Proposed owner and exact scope | Acceptance gate |
|---|---|---|
| **SP0 — process/control lifecycle** | App-lifecycle worker **after parent grants scope**: `loader/runtimeLaunch.cpp/.h`, new `runtimeChildProtocol` / process-ledger units and component tests. Narrow host state deltas in `libLncUtil.cpp/.h` coordinated with parent. Parent owns `main.cpp`, `emulator.h/.cpp`, `runtimeVshLaunch.cpp` backend-selection/entry-notification integration and CMake wiring unless explicitly transferred. | Isolated helper subprocesses test quote/args, restricted handle inheritance, handshake/version/generation, real wait on normal/crash/forced exit, create/resume/watcher failure rollback, stale close/focus, accepted-start cancellation, exact LNC event order, clean relaunch. **No first-frame or visible-integration pass from this stage.** |
| **SP1 — window/sizing/real frame milestone** | Parent-assigned graphics/window owner: `graphics/presentation/window/window.cpp`, `windowInternal.h`, `window.h`, `vulkanWindow.cpp`, `swapchain.cpp`, narrow `libs/agc.cpp` + Config separation. New internal window-bridge unit preferred. App worker supplies protocol, not parallel edits in these files. | Coordinated isolated two-process Vulkan test: one normal 1280×720 root, 1080p internal, embedding/DPI/resize, real guest-present tag, user maximize unchanged, Home hides child/re-entry restores, crash/parent death releases windows after OS wait. |
| **SP2 — input/focus/audio** | Parent-assigned controller/audio owner(s): `hostInput.cpp/.h`, window physical-pad startup, `libs/controller.cpp/.h`, `dualSenseHaptics`/Bluetooth hardware entry points, `libs/audio.cpp/.h`; coordinated LNC focus callback. Never Astra `libC`/NP files. | One hardware open + one child guest pad; PS always reaches parent; ordinary input/haptics only current epoch; button-release/disconnect tests; child audio silent on Home without backpressure hang; foreground restores actual audio/input, no duplicate pads. |
| **SP3 — storage + visible acceptance** | Parent owns explicit mount/profile policy and controlled test orchestration; app worker owns only approved child-manifest implementation/tests. OOBE and Astra service state remain untouched. | Read-only content/profile sentinels unchanged; per-run temp separation; persistent per-title relaunch policy; actual ShellUI -> Apple/YouTube first guest present -> PS/Home -> resume -> close -> actual process wait -> relaunch. Record host-created, entry and frame milestones separately. |

SP0 can be committed independently as a disabled/probed backend with helper tests.
Do not flip VSH's default to a half-integrated separate window with duplicate pad
ownership and uncontrolled shared profile writes. Once SP1–SP3 gates are met,
select subprocess as the VSH app backend while preserving the same visible shell
experience and user control of their host window. No broad same-process returning
import additions are a prerequisite for this route.

## Immediate parent decision requested

Confirm the staged ownership and choose **embedded child-owned HWND first** versus
the larger IPC compositor. Recommendation is embedding for full Home return,
with true translucent app-overlaid ShellUI explicitly deferred. Reserve an
isolated executable/build path before any new subprocess/window live tests.
Until that agreement, this worker makes no cross-cutting production edits and
does not launch or interfere with the user's current direct app.
