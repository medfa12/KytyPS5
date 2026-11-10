# SP2 input/audio and assembled subprocess checkpoint — 2026-11-05

## Outcome / scope of proof

The approved SP0/SP1/SP2 source path is assembled: child-owned embedded HWND,
independent direct-game process, parent-only physical controller ownership,
normalized input transport, full Home hide/resume, epoch-checked outputs and
focus-gain audio. The temporary `ChildIntegrationReady` assembly gate is removed;
Windows VSH defaults to subprocess. `--vsh-app-backend shared` remains rollback.
Ordinary non-VSH direct launches keep their existing backend.

**This is a tested source checkpoint, not a real-app demonstration.** No emulator
executable was built/launched, no visible ShellUI/app test occurred, no controller
or audio endpoint belonging to the user was opened, and the reserved direct
YouTube process was not touched. `_Build/windows` was only read for existing
dependency archives/compiler flags; every new object, executable, log and
checkpoint is under `_Build/vsh-subprocess`. No staging/commits, subagents, frozen
rwlock, parent OOBE, Astra or root CMake edits by this worker.

## SP2 contracts

### Bounded per-run pad transport

* New `loader/runtimeChildInput.{h,cpp}`: private anonymous file mapping, immutable
  launch key, a 64-report ring and latest-output mailbox. Explicit HOST-only
  normalized fields carry one pad's buttons, axes, connection/count, two touches,
  acceleration/gyro/orientation, epoch, serial and timestamp. No guest PadData,
  SDL pointers, foreign callbacks or physical device handles cross processes.
* Copies use an interprocess `InterlockedCompareExchange` **try-lock**, with no
  spins/waits. Contention drops/retries a later input sample; overflow drops the
  oldest bounded report. A crashed peer holding this lock cannot wedge process
  control, job retirement or the other process. No per-frame writes enter the
  lifecycle pipe. The parent mapping handle is non-inheritable; only a temporary
  inheritable duplicate enters the explicit child handle list.
* Internal child descriptor now has a seventh mapping-handle word. Parent and
  child are the same executable/build. Control messages remain fixed 104 bytes;
  strictly increasing focus epochs are enforced at both endpoints.
* `PadFocusFilter` rejects stale epochs/serials, masks PS from ordinary reports,
  neutralizes disconnect/background input, and suppresses buttons, sticks,
  triggers and touches held across a focus transition until release/center.
  Nonfinite motion values are sanitized. History is reset at focus/OS-root
  eligibility changes rather than replayed to the newly focused owner.

### Single controller hardware owner

* Managed children do **not** initialize SDL gamepad, open SDL pads, assign the
  ordinary keyboard fallback, or open DualSense speaker/haptics/HID endpoints.
  Direct DualSense backend opens/rumble also explicitly reject the child role.
* `controller.cpp` injects normalized broker reports into the existing guest pad
  history; only player 1 receives a connected brokered pad. Child-local ordinary
  keyboard/mouse/pad mutations cannot duplicate the broker. Missing broker input
  for 250 ms becomes a neutral disconnect. The shell's keyboard fallback is not
  advertised as another connected physical pad when the DualSense is unplugged.
* The parent retains raw hardware state while its ordinary readers are gated.
  `scePadUiReadState` now uses a separate system reader: navigation remains gated
  but parent PS observation survives. Physical Guide pulses stay in the parent;
  child keyboard Home/P forwards bounded system intent via a separate mailbox
  counter instead of pulsing a fake child-local ShellCore. No app sees PS as
  ordinary navigation. Parent background SDL pad delivery is enabled only for
  the managed VSH role so embedding does not cut off the hardware owner.
* Input/output eligibility follows the existing parent foreground root, not a
  foreign app. Ordinary reports and held history are neutralized outside that
  root; haptics are stopped on its loss. Direct/manual modes remain unchanged.
* Child vibration, LED and encoded adaptive-trigger requests carry focus epoch
  and serial to the parent. Parent applies them only to its existing first pad
  while the matching child is foreground/controller-focused and acknowledged.
  Background/stale requests are ignored. Focus loss, close and proven retirement
  zero motors/triggers/LEDs; prior child output is not resurrected on resume.

### Focus/audio ordering

* Host transitions are break-before-make: gate parent input/audio, send the next
  authenticated epoch and held snapshot **atomically in the fixed Focus control
  message** (not a droppable pad sample), wait without LNC registry locks for the
  child to apply real controller/audio policy plus window visibility, then commit
  the matching acknowledgement. Home mutes/neutralizes the child before parent
  ordinary input/audio is restored. Close/error/timeout retains the process and
  restores the shell only after actual retirement. Failed partial focus hides
  the child, rolls its services back to muted/neutral and reports a negative ack.
* `audio.cpp` uses **stream gain**, never focus-induced pause. Existing and future
  ordinary/native playback streams inherit focus; guest native volume multiplies
  focus policy and cannot unmute the background child. Restoring focus restores
  the requested native gain. Playback consumption/backpressure clocks continue
  while muted, and guest-requested pause remains independent.
* Parent queued controller audio is closed on focus loss and lazily reopened by
  a later focused write. Children never open controller audio/HID. Child PADSPK
  retains default-output fallback; vibration PCM retains its existing silent
  pacing contract. **Controller-speaker / waveform-haptics PCM is not transported
  between processes in this milestone**; brokered motor/LED/trigger output is.
  OS-background audio policy is not changed; these gain transitions represent
  VSH Home/app focus, while ordinary input/haptics also honor OS-root eligibility.

## Validation actually run

Independent standalone build: `cmake --build _Build/vsh-subprocess`, followed by
CTest. Three tests passed repeatedly (five consecutive runs plus final recheck):

1. `vsh_subprocess_lifecycle`: actual helper creation, restricted inheritance,
   ordered generation notices, normal/abnormal/accepted-before-entry exit, close,
   startup/close timeout, descendant-job retirement, actual OS wait, writer-lock
   isolation and immediate relaunch while a retired record is still retained.
2. `vsh_subprocess_input`: bounded overflow/order, stale/duplicate rejection,
   held-button/stick/trigger/touch suppression, neutral disconnect/background,
   OS-foreground held suppression, separate PS counter, and an actual child
   receiving/validating a full motion/touch/axes sample and returning output/PS.
3. `vsh_subprocess_embedded_vulkan`: real hidden child-owned Win32 Vulkan surface,
   clear/submit/queue-present, resize/recreate/re-present, Home/resume, parent
   death/close and actual HWND/process retirement. Also injects input/audio
   service failure and checks the negative ack cannot leave the child visible.
   Frames are synthetic; this is **not firmware, SDL app or guest title proof**.

Four focused production-CPP fixtures passed as isolated overlays. They reuse the
existing deterministic hardware/SDL queue fakes, never physical endpoints:

* `controller_focus_tests`: raw parent sampling versus ordinary neutral input,
  privileged PS, held/history suppression on Home, epoch-gated parent output,
  zero outputs on loss/exit.
* `child_controller_focus_tests`: actual child role + production controller
  report/history injection, one connected pad, local input exclusion, full
  touch/motion, Home/resume held suppression, mailbox vibration, 250-ms neutral
  disconnect, zero physical output calls from the child.
* `audio_focus_tests`: existing/future ordinary and native gain inheritance,
  guest volume cannot defeat focus mute, muted playback drains without pausing,
  restoration preserves guest gain, resource retirement.
* `child_device_ownership_tests`: actual production audio/haptics/Bluetooth units
  reject child controller audio/HID/rumble opens with **zero endpoint calls**;
  vibration-audio guest port still opens for its timing contract.

Existing `ControllerSettingsTests`, `AudioOutTimingTests` and `PadHapticsTests`
were recompiled against current source and passed as independent overlays. The
last uses only its dummy SDL driver and fake named USB/HID endpoints; its log
device names do not indicate real hardware access. SP0's keyed LNC and existing
LNC status regressions passed at their checkpoint; SP2 does not change LNC code.

Affected production translation units compile individually with current headers.
No whole-tree build, emulator link or app launch is implied. Commands/options and
outputs are `overlay/*.{rsp,log}`, standalone CTest's `LastTest.log`, and fresh
helper fixture log directories. Overlay links explicitly use the current
`emulatorConfig.obj` rather than a stale ConfigOptions layout in an old archive.

## Required parent integration before any visible demonstration

**Still absent at this checkpoint:** `runtimeLinker.cpp` entry notice and root test
wiring. These files remain parent-owned. Exact direct-entry edit:

```cpp
#include "loader/runtimeChildClient.h"
// In RuntimeLinker::Execute, immediately before direct-main RunEntry(...):
NotifyRuntimeChildEntry();
```

Do not insert before module initialization or on the co-ELF route. Without it a
real app intentionally cannot claim Entry/Present readiness; no earlier fake
notice was substituted. Default subprocess is a **source setting only** until
the parent integrates/builds. Do not launch a partially integrated executable.

Root loader/window source globs include all new production files. Retain SP0
Windows root `add_subdirectory(tests/subprocess ...)` and LNC full-test wiring.
Place that subdirectory after the relevant dependency targets/includes exist.
Its own CMake now automatically adds the four focused fixtures when `common`,
SDL/fmt and opus targets exist, in addition to the three standalone tests.
The existing minimal fixtures that include production `.cpp` files now also need
the child no-op/role helpers linked (after those targets have been defined):

```cmake
foreach(t controller_settings_tests audio_out_timing_tests pad_haptics_tests)
  target_link_libraries(${t} PRIVATE vsh_subprocess_component)
endforeach()
```

Do not add this test-only component to the emulator target; its injection macro
is for the standalone process fixtures. Full emulator targets already compile
the new production loader units via their existing glob.

## Handoff / remaining work

Source/hash snapshot: `_Build/vsh-subprocess/checkpoints/sp2-20261105/`.
Earlier SP0/SP1 snapshots remain immutable. Reports under `docs/` are ignored by
the repository's `/docs/**` rule; parent must force-add reports it wants committed.

The next task is **coordinated isolated emulator E2E**, not another architecture
approval: parent installs the exact linker/test wiring, reviews/builds a separate
executable outside `_Build/windows`, and arranges a safe visible ShellUI → native
app → full Home → resume → close/relaunch run with isolated storage. That run may
expose firmware startup, shell focus-event order or live SDL/Vulkan integration
issues that component tests cannot settle. No speculative success/ETA is claimed.
Do not interact with or replace the reserved direct YouTube session for that test.

True translucent ShellUI-over-app GPU composition remains unsupported. Full Home
means hiding the child and revealing the same retained top-level ShellUI window;
the parent is never forcibly maximized or replaced.
