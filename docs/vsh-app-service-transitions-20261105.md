# Direct-app service returning transitions — 2026-11-05

## Production follow-up: AppContent initialization / SystemService subscriptions

**Integrated, built and tested by the parent.** The full emulator build and
`runtime_application_service_events` passed; see
`_TempData/vsh-native-drain-integrated-build.log` and
`_TempData/vsh-native-drain-integrated-tests.log`. VSH prepares subscriptions
before guest entry and releases them after actual native termination and lease
drain. Separate UserService isolation is also integrated and tested.

This section supersedes the initial audit's AppContent-init/SystemService-event
exclusion and reported parameter-selector fatal paths below. The current
returning-service table contains 24 exact pairs, including initialization and
owner event queries. Pad controller-information remains excluded because a valid
VR handle has no standard-player mapping. Successful native app frames and
close/relaunch remain unverified.

### Integrated lifecycle contract

Include `libs/libSystemService.h` in the parent's lifecycle coordinator:

1. After `InitializeApplicationExecution`, capture/retain the actual
   `std::shared_ptr<Loader::RuntimeGuestExecution>` for this incarnation.
2. Before **any owned module initializer or executable entry**, call
   `Libs::SystemService::RegisterApplicationEvents(execution)` and reject startup
   on false. Registration creates an empty private subscription; it does not copy
   shell/peer backlog or produce an event. Re-registration is idempotent and must
   not clear existing pending events.
3. On both failed preparation and normal/failed app close, request stop, establish
   actual worker/callback/HLE drain, then call
   `Libs::SystemService::ReleaseApplicationEvents(execution)` with the **captured
   snapshot**, before publishing owner retirement. False means cleanup is not yet
   safe; retain/retry. Do not pass whichever execution is currently stored in a
   reused `Program`. The hook refuses Running controls or nonzero outstanding
   work and is otherwise idempotent. It never clears the session queue.

These call sites are wired in `src/loader/runtimeVshLaunch.cpp`.
A runtime owner with no registered subscription receives typed
UNAVAILABLE from SystemService receive/status and NOT_INITIALIZED from
AppContentInitialize, rather than borrowing shell state. These are lifecycle
admission failure policies using existing native error domains, not a claim that
an unimplemented service initialized successfully. Unavailable calls leave guest
output unchanged. `NotifyApplicationEntitlementUpdate(execution)` is also a host
producer API accepting a captured immutable target; a delayed old producer cannot
send to a relaunched application even at the same Program address.

The existing `libs/systemService.h::NotifyEntitlementUpdate()` declaration remains
untouched. Its implementation now routes by current context; null/direct/firmware
contexts retain the legacy session queue, runtime contexts require their own live
subscription. No new resolver aliases or guest-visible lifecycle exports exist.

### Production behavior and lifetime/race invariants

- `AppContentInitialize` captures the current execution, resolves its diagnostic
  title through the existing owner-aware SystemContent snapshot, and emits the
  existing `0x10000003` entitlement-update hint to that subscription only. It
  still clears the 40-byte boot output, accepts the existing nullable init/boot
  arguments, and emits one hint per successful initialization call. It does not
  grant ownership, licenses, DLC, attributes or fabricated entitlement bytes.
- Removed the write-only global `g_title_id` entirely. Initialization has no
  persistent mutable app-content identity to leak or release; the loader already
  owns the actual metadata snapshot. Add-content mount/global filesystem APIs
  remain excluded and unmodified. No AppContent cleanup hook is needed for this
  stateless slice; pending hints belong to SystemService and are released there.
- Session events remain in the existing single queue. Runtime queues are keyed
  by non-reusing execution token, validated against identity **and the exact weak
  execution snapshot**, not raw Program pointer alone. The weak reference cannot
  pin a dead executable; explicit release removes its queue. Registration also
  removes expired abandoned snapshots, preventing permanent raw-pointer-keyed
  accumulation when preparation never reached cleanup. Stopped but still-live
  subscriptions are retained until their explicit quiescent release.
- Each runtime producer/consumer obtains a real short HostCall work lease and
  performs its in-memory queue operation through `Lease::Publish` while holding
  the service mutex. This is the existing publication-against-stop pattern used
  by libc callback registration. Stop either follows an already-committed
  enqueue/pop or refuses it; no post-stop registration/enqueue/pop can reopen
  admission. Cleanup requires closed admission and zero outstanding work. No
  mutex is retained across guest calls, sleeps, callback invocation, output copies
  or native thread waits. Host-critical guards protect service locks against
  arbitrary native suspension. No synchronous guest callback is added.
- Receive retains existing successful/NO_EVENT output policy: exact event type
  (or -1), zero 8192-byte payload. It pops only the selected queue. Status retains
  existing dialog/focus flags but counts only that queue; it cannot consume data.
  Output stores occur after registry/publication locks are released. As before,
  arbitrary invalid guest pointers, allocation failure and logging are not made
  fault-contained or hard-latency-bounded by this slice.
- Notifications here are source-scoped initialization/update hints. There is no
  new global entitlement backend or broadcast-to-every-app claim. Real future
  license/content changes must explicitly target each legitimate subscriber;
  they must not infer entitlement grants merely from this existing event type.

### Firmware-backed normal error corrections

Read-only Capstone disassembly used the local 13.60 ELF files; no native code was
executed:

- `system_b/common/lib/libSceSystemService.sprx`, export `SsC-m-S9JTA` at
  **0x4bd0**: **0x4bd7–0x4bf7** checks nonnull output, selector exactly 6 and
  capacity at least **0x41 / 65**. Any rejection returns **0x80a10003** without
  writing output. **0x4c1a** forces byte +0x40 to NUL. The HLE now returns the
  existing `SYSTEM_SERVICE_ERROR_PARAMETER` instead of EXIT for unknown selectors,
  enforces native capacity 65 (not merely enough room for `Kyty`), and terminates
  byte 64. The local system-name policy remains `Kyty`; no real console setting
  read is claimed. `src/libs/errno.h` defines the exact SystemService error domain.
- `system_b/common/lib/libSceAppContent.sprx`, export `99b82IKXpH4` at
  **0x15c0**, tail-calls **0x1d80**. Its initialized-client path at
  **0x1d9b–0x1da3** rejects null output with **0x80d90002**; initial client checks
  at **0x1d8b–0x1d99** use **0x80d90001**. The IPC operation at **0x160–0x1c7**
  serializes selector as four-byte input and a four-byte integer output.
- `system_b/vsh/SceShellCore.elf` app-content handler **0x26fb50** seeds EAX with
  **0x80d90002** at **0x26fb71**, checks output and dispatches selectors 0–4
  (**0x26fb76–0x26fb9a**). Its out-of-range branch
  **0x26fbcc–0x26fbd2** rejects selectors other than **0x44c / 1100** via
  **0x26ffc1** without an output store. The HLE now returns PARAMETER for null
  and genuinely unknown selectors, preserving output rather than EXIT/zeroing it.
- **1100 is valid, not an unknown selector.** The native server reads registry
  key **0x7802c700** at **0x26fbd8–0x26fbe1**; there is no truthful provider for
  that setting in this slice. Instead of inventing a successful boolean or
  returning PARAMETER, the HLE returns **0x80d9000a / INTERNAL** and preserves
  output. This is the native client's unsupported/provider-failure normalization
  at **0x1d56–0x1d6a**: unrecognized errors normalize to **0x80d9000a** at
  **0x1d65** (the separate EBUSY mapping at **0x1d56–0x1d63** is not used).
  Native registry-backed success and its server's incidental failed-query output
  writes are **not implemented**. The preserved-output early provider-failure
  policy is deliberate, not a claim of an emulated registry read.

Existing selector-zero full-install and missing user-param policies remain as
audited; no permission/entitlement service is newly fabricated. No new returning
classification was edited. Parent should account for the now-fixed error paths
and the explicit unsupported 1100 limit when reviewing the existing query entry.

### Test fixture handoff (not run)

At top level of the CPU fixture TU include `libs/libAppContent.h`,
`libs/libSystemService.h`, `loader/runtimeGuestExecution.h` and
`loader/systemContent.h`; include `tests/RuntimeApplicationServiceEventsTests.inc`
inside its existing fixture namespace and dispatch
`TestRuntimeApplicationServiceEvents()` in a **dedicated fresh process after
InitSubsystems**. Parent owns dispatcher/CMake/build/test execution.

The fixture uses actual registered SysV exports, real execution controls and
real session/per-owner metadata publication. It checks shell/core/null-context
session continuity; empty private registration despite session backlog; repeated
init event policy; two-owner independent counts/consumption; private-module owner
inheritance; concurrent captured producers with opposite TLS contexts; rejection
before registration and after stop; outstanding-work cleanup refusal; pending
event discard at quiescent close; relaunch at the same Program address/ID with a
fresh token; stale producer/cleanup isolation; metadata preservation; null/unknown
selector errors and exact output canaries; native system-name buffer bounds; and
balanced host-critical protection. It does not inspect the service map or use a
test-only implementation/fake execution. No compile/run success is claimed.

---

## Outcome

Added **17 exact production `{nid, expression}` pairs** in
`src/loader/applicationServiceReturningExports.h`, separately from the existing
119-entry `applicationReturningExports.h`. This is a small startup-service slice,
not a launch-enablement change or an unresolved-count target. The parent must
integrate it behind the existing resolved-registration, owner, version, address
and private-native-mapping checks. Native targets remain private-owner checked;
an entry here never authorizes an arbitrary returned native pointer.

Only the two new files were written. No source integration, builds, tests, live
emulator runs, agents, commits or pushes were performed. The shared launch gate
must remain guarded. In particular, the identity/event/port blockers below are
real implementation work, not candidates for widening this list.

## Evidence and bounded startup inventory

Read the existing direct-run reports, including
`direct-app-runtime-20261104.md`, `direct-media-foundation-20261104.md`,
`direct-appletv-output-readiness-20261104.md`,
`direct-youtube-direct5-auth-connect-20261104.md`, and
`direct-youtube-aac-validation-20261104.md`. The last report preserves the user's
positive moving-picture-and-sound observation for the identified sponsored
YouTube result; it is not replaced by later black-screen observations or promoted
to universal/protected-playback support. Direct Apple Home/navigation is likewise
historical normal-game evidence, not evidence of launch/close within shared VSH.

Re-read actual ELF imports using the existing `image()` parser from
`${USERPROFILE}/AppData/Local/Temp/opencode/run19_provider_audit.py` via
`runpy.run_path(..., run_name='audit_readonly')` and `python -B`. This executes
neither the script's file-writing `main()` nor guest code. It reads program/SCE
dynamic/string/symbol tables rather than relying on invalid app section headers.
Inspected title images under `${USERPROFILE}/Downloads/storeormediaapps/`:
YouTube `PPSA01651-app0/{eboot.bin,libstarboard.prx,libcobalt.prx}` and Apple
`PPSA01783-app0/eboot.bin`. Also inspected the actual 13.60 providers
`${USERPROFILE}/Downloads/13.60/system_b/common/lib/{libSceEsvm.sprx,libSceSystemService.sprx}`.
This is deliberately not complete transitive provider closure or proof that every
listed import executed. Title hashes are recorded in the prior returning-HLE audit.

| Consumer | Relevant actual undefined function NIDs |
| --- | --- |
| YouTube Starboard | `99b82IKXpH4`, `fZo48un7LK4`, `SsC-m-S9JTA`, `rPo6tV8D9bM`, `1n37q1Bvc5Y`, `KgiwSrROsIQ`, `Oxh0CSsTZ78`, `PjS5uASwcV8`, `hv1luiJrqQM`, `gjP9-KQzoUk` |
| Apple eboot | `99b82IKXpH4`, `fZo48un7LK4`, `rPo6tV8D9bM`, `PjS5uASwcV8`, `hv1luiJrqQM`, `gjP9-KQzoUk`, `d7eh3psk1J8`, `v9gmbZSAovI`, `ys239q8VgXY` |
| Native 13.60 Esvm | `G-MYv5erXaU`, `+I4K03i3EL0`, `d7eh3psk1J8`, `q9HFykGcuWU`, `v9gmbZSAovI`, `ys239q8VgXY` |
| Native 13.60 SystemService | `G-MYv5erXaU`, `igMefp4SAv0` |

For example Starboard imports `99b82IKXpH4#C#D` from
`libSceAppContent` v1 / `libSceAppContentUtil` v1.1; Apple imports
`99b82IKXpH4#N#O` with the same library/module versions. Native Esvm imports
`G-MYv5erXaU#E#F` from `libkernel` v1/v1.1. Symbol-table library indices are
image-local, not additional NID aliases. No extra registration was synthesized
from these names. Tiny YouTube eboot/libcobalt are not substitutes for Starboard's
substantial service requirements.

The direct reports retain concrete older invocation evidence: native Esvm checks
app-info `ATTRIBUTE3 & 0x200`; Apple polls the output interpreters during display
startup; Starboard's signing call is recorded in direct5. Both titles also import
the excluded initialization/event/open/close paths below, so this table cannot
possibly establish complete startup.

## Selected bodies and exact registration identities

All selected bodies explicitly use `KYTY_SYSV_ABI`. Returns are integer status or
`void`; floats below are written to caller buffers, not hidden vector/aggregate
return conventions. None takes a guest callback, dispatches a signal, switches
context, invokes guest code, sleeps or enters a guest synchronization wait.
There is consequently **no new HLE-to-guest callback to account for** in this
slice. Ordinary host critical-section contention and logging are not claimed to
be hard real-time bounded or asynchronously cancellable.

Line ranges describe the inspected source; symbol names are authoritative if
concurrent parent work moves lines. Every exact pair is in the header, with no
shortened/inferred expression spelling.

### PID-scoped native-service prerequisites — 2 pairs

- `G-MYv5erXaU` / **`KernelGetAppInfo`** and `igMefp4SAv0` /
  **`KGetAuthInfo`** are registered literally by
  `src/libs/libShellCoreFloor.cpp:563,571`. Bodies are at 271–300 and 322–337,
  not the adjacent `KStub`, `KFalse`, `KTrue`, SDK-floor or app-state placeholders.
- `KernelGetAppInfo` validates an 88-byte output range, queries a real process
  record and returns ESRCH without writes for absent/inactive/category-less
  records. Success writes declared ATTRIBUTE3 at +0x4c and category at +0x50;
  it does **not** grant the Esvm permission bit. Remaining record fields are zero,
  not a claim of full app-info fidelity. `KGetAuthInfo` validates 136 bytes,
  returns POSIX `-1`/EFAULT or ESRCH on failure, otherwise copies the actual
  authority/capability record. Guest errno is the existing thread-local errno.
- `src/loader/runtimeProcessIdentity.cpp::TryGetGuestProcessIdentity` resolves
  PID zero using `GetCurrentGuestProcessId`, which selects the current program's
  application owner. Runtime records have distinct PIDs, active-state checks and
  snapshotted metadata. Boot authority/capability fallback is scoped to boot PID,
  not copied into a runtime owner. `TryGetGuestProcessAuthInfo` constructs a
  17-word result from that record. Only short host registry locks and in-memory
  metadata reads occur. The legacy boot snapshot's `ProgramScope` restores TLS
  program identity on normal return; it does not change shell metadata.
- The existing Windows `FloorRangeOk` probe is not atomic with the subsequent
  copy and is not an owner-private memory proof. No general guest-pointer/SEH
  containment or protection against concurrent unmapping is claimed.

### App-content read-only query — 1 pair

- `99b82IKXpH4` / **`AppContent::AppContentAppParamGetInt`**:
  `src/libs/libAppContent.cpp:125–150`, registration 342. The body writes a
  typed output: selector zero reports the existing full-install SKU policy (3),
  selectors 1–4 read `USER_DEFINED_PARAM_1` through `_4`; absent values remain
  zero under the existing success policy. This is not a license/entitlement
  backend or permission to manufacture downloadable content.
- `src/loader/systemContent.cpp::CurrentSystemContent` (127–138) selects an
  immutable, shared-owned snapshot by TLS `application_owner`; it does not fall
  back to shell metadata for an unregistered runtime owner. `SystemContentParamSfoGetInt`
  (745–752) and `Psf::GetParamInt` (365–378) only lock/read preloaded memory.
  File parsing occurs when publishing content, not in this query. Neither global
  `g_title_id`, mount tables nor entitlement event queues are touched.
- Null output and unknown selectors still reach host-fatal `EXIT` paths. This
  returning classification covers the implemented normal selector paths, not
  total-function safety or corrected SDK errors for unsupported selectors.

### Non-consuming SystemService queries — 4 pairs

All registrations are in `src/libs/libSystemService.cpp:734–738`:

| NID / exact expression | Body and limits |
| --- | --- |
| `fZo48un7LK4` / `SystemService::SystemServiceParamGetInt` | 114–144: validates output; language selectors 1/400 read configured console language, known date/time/timezone/parental/accessibility/button selectors use explicit local policies, then write an integer. Unknown selectors currently log and succeed with zero: **not** a claim of implemented unknown parameters. No settings mutation or initialization side effect. |
| `SsC-m-S9JTA` / `SystemService::SystemServiceParamGetString` | 146–168: parameter/size errors are typed; selector 6 copies the local system name `Kyty` including NUL after a capacity check. Unknown selector is host-fatal, not newly contained. No filesystem, hostname query, callback or global state write. |
| `rPo6tV8D9bM` / `SystemService::SystemServiceGetStatus` | 190–209: zero-initializes the typed result, snapshots actual dialog visibility and owner-aware LNC overlay/background state, reads event count without popping. `src/libs/dialog.cpp::SystemDialog::GetVisualState` (69–73) only reads under its host mutex; it does **not** call the dialog visibility callback. `src/libs/libLncUtil.cpp::QueryFocus` (599–623) resolves `application_owner`, checks actual running/runtime-managed registration and focused app. It cannot assign focus. Event count is still the process-global count, **not a private application's independent pending count**. |
| `1n37q1Bvc5Y` / `SystemService::SystemServiceGetDisplaySafeAreaInfo` | 211–222: null yields parameter error; writes ratio 1.0 for the existing full-screen safe-area policy. Does not initialize the reserved payload or change display mode. Not a measurement of host overscan/user-adjusted safe area. |

Fixed configuration/policy outputs are distinguished here from no-output
log-success placeholders. Their limited existing policy is disclosed rather than
claimed as full console-settings support. The status reader may observe global
dialog/event state, but cannot consume, clear or reset shell state.

### Real certification scope and truthful no-backend signing — 2 pairs

- `KgiwSrROsIQ` / **`GetCertificationScope`**, body
  `src/libs/libSystemService.cpp:702–709`, literal registration 712. Writes the
  verified constant `1p-cobalt-ps5` plus NUL only when capacity permits; bad
  pointer/capacity returns `0x80a10003`.
- `Oxh0CSsTZ78` / **`SignCertificationMessage`**, body 714–725, literal
  registration 727. Six **typed** SysV arguments, not the generic six-uint
  fallback. Validates algorithm/message/digest spans and the first 11
  `HMAC-SHA256` bytes, then returns native no-backend INTERNAL `0x80a10001`
  without writing any digest bytes. Invalid arguments return `0x80a10003`.
  There is no network, device service, guest callback, credential or successful
  HMAC operation here. The direct5 audit proves the native failure-before-dispatch
  semantics in both 13.60 and 4.03 and Starboard's ordinary false-result handling.
- **Registration expressions are unqualified** in their local registration
  scope. Do not replace them with `SystemServiceYouTubeDeviceAuth::...` strings.

### Pure VideoOut construction/interpretation — 6 pairs

Registrations are `src/libs/libVideoOut.cpp:20,42–45,48`:

| NID / exact expression | Body |
| --- | --- |
| `PjS5uASwcV8` / `VideoOut::VideoOutSetBufferAttribute2` | `src/graphics/presentation/videoOut.cpp:1659–1689`: clears the caller's attribute and fills format, tiling, dimensions, options and DCC fields. Eight scalar/pointer arguments, including stack arguments; `void` return. No driver access. Null attribute remains host-fatal. |
| `+I4K03i3EL0` / `VideoOut::VideoOutInitializeOutputOptions` | Same file 2317–2327: typed null-address error or zeroes caller output options, no driver access. |
| `d7eh3psk1J8` / `VideoOut::VideoOutPortStatusIsUsable` | `src/graphics/presentation/videoOutStatus.cpp:48–55`: `(~word0) & 1`. |
| `q9HFykGcuWU` / `VideoOut::VideoOutPortStatusIsDeviceConnected` | Same file 57–62: first-byte bit 0x20 clear. |
| `v9gmbZSAovI` / `VideoOut::VideoOutOutputStatusIsHdcp14Enabled` | Same file 64–71: unaligned-safe flags read at +0x10, mask `0xb00000000`, compare `0x300000000`. |
| `ys239q8VgXY` / `VideoOut::VideoOutOutputStatusIsHdcp23Enabled` | Same file 73–80: same flags/mask, compare `0xb00000000`. |

All four interpreters return typed invalid-address error for null input. They do
not invent connectivity/readiness/HDCP and do not require or change a source-port
owner. Esvm's shared physical-endpoint query is separate from app MAIN/OVERLAY
source ownership, as established in the Apple output audit. No actual GPU,
display-service lifecycle, mutable handle or event operation is approved here.

### Non-destructive Pad setup/information — 2 pairs

- `hv1luiJrqQM` / **`Controller::PadInit`**, registration
  `src/libs/libPad.cpp:299`; body `src/libs/controller.cpp:1238–1244` calls
  `GameController::InitPort` (1165–1171). Under a host mutex the only transition
  is Uninitialized to Closed. Already-open shell ports, open counts, input
  reports, orientation and haptic state are untouched. This is genuine idempotent
  port initialization, unlike the AudioOut log-only init.
- `gjP9-KQzoUk` / **`Controller::PadGetControllerInformation`**, registration
  `src/libs/libPad.cpp:309`; body `controller.cpp:1334–1361` validates a port
  handle and output, snapshots actual connection/count through
  `GameController::GetConnectionInfo` (964–972), zeroes the result and writes the
  supported standard-controller geometry/deadzones/device policy. No SDL device
  wait/output or report-consumption path is called. It does not grant a new handle
  or transfer port ownership. These queries are not a capability-confidentiality
  boundary: current standard handles are process-global.
- Host-created `g_players` and a standard non-VR controller are assumptions of
  the known media-app path. Valid VR handles can pass `PadCheckHandle` through
  `padVr.cpp::PadVrCheckHandle` but lack a `PlayerForHandle` standard-player
  mapping in this information routine; VR behavior is **not** established safe.
  Invalid guest pointers and shutdown races are likewise not fixed by the table.

## Exclusions and concrete next production gaps

1. **First actionable startup slice: app-content initialization plus independent
   per-owner service-event subscriptions.** Both apps actually import
   `R9lA82OraNs`, `656LMQSrg6U`, `yH17Q6NWtVg`, `j3YMu1MVNNo`,
   `fPhymKNvK-A`, `CdWp0oHWGr0` and `bwFjS+bX9mA`. Today:
   - `src/libs/libAppContent.cpp::AppContentInitialize` (103–123) writes global
     `g_title_id` and calls `SystemService::NotifyEntitlementUpdate`.
   - `libSystemService.cpp::NotifyEntitlementUpdate` / `SystemServiceReceiveEvent`
     share one `g_events` deque (84–90,170–188); an app pop steals shell events.
   - `libUserService.cpp::UserServiceGetEvent` (472–505) either pops global
     `g_store_events` or mutates process-global `static logged_in[]`. It is
     nonblocking but **not an independent app subscription**. `GetInitialUser`
     and `GetLoginUserIdList` also call `MaybePostLoginWake` and `InStoreMode`.
     The latter can lazily enter `EnsureStoreInitialized` and host JSON/file I/O.
     `UserServiceInitialize{,2}` additionally call `MaybeSeedNativeUserList`, which
     performs guest-filesystem opens/reads/writes. Even nominal name/number reads
     lazily initialize the store. No UserService family entry is added merely
     because the eventual successful return is an integer.
   Preserve the shell's existing login/store state, publish a read-only initialized
   identity snapshot before app entry, and give each application owner/lifetime
   its own event cursor/queue. App initialization must not reload/reset users or
   enqueue/pop another owner's entitlement/login events. Reentrant close must
   detach just that subscriber. Only then classify the actual startup exports;
   don't disable event handling to get past the call.

2. **Pad lifecycle and consumption remain a real blocker.** Both apps import
   `xk0AcarP3V4` / `Controller::PadOpen`, `q1cHNfGycLI` /
   `Controller::PadRead`, and `6ncge5+l5Qs` / `Controller::PadClose`.
   `GameController::OpenPort` / `ClosePort` (1173–1219) have shared count/global
   handle state, not owner/lifetime leases. A second ordinary open can fail
   ALREADY_OPENED; close can decrement somebody else's count. Focus-aware reads
   correctly return neutral data for background runtime owners, but
   `ReadStates` (986–1015) marks process-global `m_obtained[]`, while firmware
   observers remain live. This is not independent shell/app report delivery.
   Need per-owner port references and read cursors plus owner/focus-aware output
   effects; cleanup must not close/reset shell ports. No open/close/read,
   vibration, lightbar, motion reset or generic Pad stubs are admitted.

3. **VideoOut mutable port/graphics services are not isolated by the pure helpers.**
   `VideoOutDriver::Impl::Open` (883–928) assigns one handle by bus and rejects an
   already-open context. `Close` (930–994) clears that bus's buffers, events,
   retained layer and queues without application-owner authorization. Both titles
   import these real calls, buffer registration and waits. Do not whitelist the
   family: owner/generation-scoped source ports, retirement and stop-aware
   display waits need their own proof. Even read-only `VideoOutGetOutputStatus`
   is withheld here: the legacy `DriverState()/Get` path has no safe-snapshot
   lifetime pin, aliases handle zero to MAIN and is not a private-owner handle
   lookup. The four buffer interpreters do not authorize this service call.

4. **No AudioOut entries.** `AudioOutInit` (`JfEPXVxhFqA`) in
   `src/libs/audio.cpp:1263–1267` is print/success only. Real `AudioOutOpen`,
   close, volume and outputs are implemented but public ports are not keyed to
   `application_owner`; the internal media-lease owner checks must not be
   misapplied to the ordinary public ABI. Output pacing/stream-drain paths sleep
   (`Audio::AudioOutOutputs` and the SDL sink), and device open/close can block.
   `AudioOutGetPortState` also reports fixed volume 127 rather than reading the
   actual volume. No blanket audio/media allowance or new cancellation claim.

5. **LNC audit, not extra table filler.**
   `libLncUtil.cpp::sceLncUtilGetAppStatus` (849–871) is a genuine owner-resolved
   0x40-byte snapshot/error query registered as
   `LIB_FUNC("DxRki7T2E44", Lnc::sceLncUtilGetAppStatus)`. The nearby
   `sceLncUtilGetAppTitleId` is also a real read-only registry query; category
   and suspend-block/unblock bodies below it are log-success placeholders.
   No inspected direct title import required the former two, so neither was
   added just to increase table size. Moreover the status registration remains
   under LncUtil-module v1.1, while the inspected native SystemService exports
   `DxRki7T2E44` under LncUtil-library / **SystemService-module** v1.1; only the
   separate close-export registration scope supplies that module alias today.
   This table must not silently repair or bypass that identity mismatch.
   Actual title import `64nkF7LGk8w` in SystemService has no literal production
   registration; native provider availability alone is not an HLE classification.
   Its service semantics/owner effects still need a separate audit.

6. **Still excluded:** splash-hide/global LoadExec side effects, PowerTick/music
   log-success stubs, add-content mounts and filesystem-space queries, all whole
   filesystem/socket families (including their blocking device/pipe branches),
   raw signal/fiber/nonlocal-context operations, unsupported generic six-uint
   handlers, and wholesale sysmodule/media/graphics admission. The parent owns
   native handle pinning and checked module transitions. No conclusion here
   overrides those production requirements.

## Handoff checks and limits

Read-only source cross-check: 17 unique pairs; every exact literal `LIB_FUNC`
registration exists in its identified production file; no duplicate pair or
overlap with the existing 119-entry table. Source bodies were selected first,
not generated from every `LIB_FUNC`. No build/test result is claimed.

The table classifies normal returning transitions, **not** fault containment,
full native-service fidelity, lock-free bounded latency or app teardown
completeness. Allocations/logging, guest-memory races and the specifically listed
host-fatal argument paths remain existing limitations. Parent integration should
exercise eight-argument/stack and void returns, failure-output preservation for
signing, two-owner metadata/status isolation, idempotent Pad init with an open
shell port, rejection of excluded exact expressions, and continued rejection of
cross-owner/stale native targets. Those future checks must not be replaced with
a thunk count, clean entry return or the old direct-launch UI evidence.
