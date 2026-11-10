# Regular 13.60 user creation checkpoint

## Current result

The user confirmed that the corrected error dialog is visible, OK dismisses it,
and navigation resumes. Account creation is still failing; no successful PSN
authentication has been demonstrated. The running psn20 session was preserved
through this investigation, with no automated controller input or new debugger
attachment.

`_Runs/psn20/_kyty.txt` records:

- Line 37547: the selected UserService JSON store is absent (zero profiles).
- Line 46483: `sceLoginMgrServerInitialize():0x813e0005`.
- Line 230709: `CreateInitialUser` fails with `0x813e0001`.

The SharePlay event-open warning is nonfatal and does not explain the client
initialization failure. Exact LoginMgr factory routing requires the native
adapter; regular boots currently lack its source-bound admission prerequisites.

## Original firmware path

All addresses below are image-relative RVAs in the original regular 13.60
`system_b/priv/lib/libSceLoginMgrServer.sprx` or
`system_b/vsh/SceShellCore.elf`, not live absolute addresses.

1. CreateUser export `AT6NySjQ2gY`, LoginMgrServer `0x1ad0`, forwards
   `(name, type, isIncomplete, hasShownEula, outUserId)` to facade `0x3880`.
2. Facade `0x38a5` checks singleton initialization count at `+0x10`.
   Zero returns `0x813e0001` at `0x3917`, before request serialization.
3. Serializer `0x1090` sends method `0x30021` through client vtable `+0x58`:
   two inputs (8-byte structure: i32 type, byte incomplete at +4, byte EULA at
   +5; NUL-terminated name including its terminator), one 4-byte user-ID output.
   The remaining two structure bytes are not initialized by this serializer.
   Transport failure takes precedence over the returned service result.
4. ShellCore dispatch table `0x1ed5358`, entry `0x21`, selects `0x6e596`,
   which unpacks those arguments and calls `0x74b590`.
5. The handler obtains the actual peer PID through session vtable `+0x18` and
   checks `0xb3ef70` (auth-info capability bit62). Failure returns `0x813e0005`;
   null session returns `0x813e0002`. Accepted requests reach `0x748830`.
6. The LoginMgr-to-UserService type table at `0x1ef8128` is
   `{0, 0, 1, 1, 1, 2, 0}`. Thus UI persistent type1 maps to UserService type0,
   while UI guest type3 maps to UserService type1.
7. `0x748830` calls UserService implementation `0x9f3b90`, passing the mapped
   type, output ID, a null fifth argument, and isIncomplete. This path does not
   forward hasShownEula to that implementation. Completed successful creation
   also calls `0x7488b0`, whose event payload includes `CreateUser`.

## UserService state effects

`0x9f3b90` is substantially more than ID allocation:

- Type0 scans 16 home records at manager `+0x180`, stride `0x4000`.
- Type1 scans four guest records at manager `+0x40180`, same stride.
- No slot/ID returns `0x80960102`; type2 has separate checks and is not covered
  by the persistent/guest UI mapping above.
- `0x9f37c0` allocates an ID; `0x9efa50` prepares user state with failure cleanup
  through `0x9eff10`.
- Record initialization writes the ID and copies the name with size `0x11`;
  isIncomplete sets record `+0x5c` to 1. Home and guest records follow different
  persistence paths. Other calls update properties, ordering and user lists.

DestroyUser is export `REIRIXzv-H0`, wrapper `0x1af0`, facade `0x3980`, method
`0x30022`. ShellCore dispatch `0x6e540` calls `0x74b610`, repeats peer policy,
then forwards to `0x748b10`.

## Next implementation boundary

Explicit local-account HLE currently provides profile-backed NP sign-in-state
queries, not LoginMgr creation. Its UserService store has no create operation;
its save helper currently returns void. A coherent local creation implementation
must handle persistent versus guest lifetime, persistence failure, registered
lists, login/logout notifications, and application-owned snapshots together.
Returning success from native initialization alone does not implement these
effects. No create-user success shim was added in this checkpoint.

CommonAssets resource mounting succeeds, but the missing profile-image cause
remains unproven. Searches of the current log found no explicit avatar/image
failure identifying a decoder or renderer fault.

## Persistent versus sign-in errors confirmed by user navigation

The user supplied screenshots of `CE-111469-2` at Data Collection and
`NP-102933-8` over Add User. psn20 log lines 90952-90985 show
`PfDataProfiling -> Processing`, followed by `LoginMgr.createUser:0x813e0001`;
the same failure recurs at 93884. Line 96982 separately records
`LoginMgr.pfAuth:0x80550002`. These are different failing operations. The
persistent-creation failure is not confined to the guest flow.

## Explicit local profile implementation (in validation)

The selected local HLE now binds CreateUser, DestroyUser and SetLoginFlag to
the shared UserService store. Guest exports require a retained current guest
thread/boot-role identity; applications cannot mutate session profiles. Native
credential configurations retain native bindings. These independent operations
do not claim that the native client connected or alter its initialization count.

- Home IDs and guest IDs use their respective firmware classes; capacity is
  16 persistent and four guest records. Type5 special-user creation is rejected.
- Persistent creation/deletion/flags use a flushed sibling-file replacement;
  failed persistence preserves previous records, outputs, and login state.
- Guest records are excluded from disk and removed on logout. IDs are not
  reused within the session. Creation itself does not log a user in.
- Registered-user output uses home slots0..15 and guest slots16..19.
- Existing application subscribers learn newly created identities on login;
  their retained launch identity and previous profile snapshots remain intact.
- Local profile creation leaves account ID zero and creates no PSN session.

The emulator and integrated test target built. `local_user_creation`,
`runtime_user_service_owners` and `runtime_user_service_controllers` passed 3/3.
The new check covers actual SysV exports on a registered retained caller,
invalid pointers and absent/retired caller rejection, both ID classes and
capacity limits, real filesystem failure, property updates, login/logout,
deletion and application notification/snapshot behavior.

User authorized restarting whenever needed and said they are not using the
session. psn20 was stopped; psn21 (PID23908) used copied system data/staging
and the explicitly selected regular 13.60 asset tree. Its relocation log exposed
an export-identity mismatch: including `_v1` in the registration's library name
duplicated the version suffix generated by SymbolDatabase. The native binding
won before NID fallback. Corrected the registration to `LoginMgrServer` and
added an exact-identity regression. Rebuilt both targets; the same three tests
pass after correction. psn21 was stopped and its data copied for psn22. Live
flow validation continues; component tests alone do not establish UI completion.

## psn22 rollback and follow-up status/passcode checkpoint

psn22 (PID3212) confirmed the corrected CreateUser binding, including the late
managed import. It saved initial `User1` as `0x10010000`. Persistent type1
creation produced `0x10010001` and `0x10010002`, and Quick Play type3 produced
`0x20010000`; all three new records were immediately destroyed by managed
rollback. Neither UI creation nor actual login completed. The user also
confirmed the Data Collection, Quick Play and guest PSN sign-in errors remain.

The composite managed CreateUser at `ReactNative.Modules.Vsh+0x1881e0`
calls CreateUser, an optional callback, then SetUserStatusByMaskValue with
mask1/value0. On a negative result its finally block calls DestroyUser at
`+0x188499`. SetUserStatusByMaskValue (`+0x3d3c0`) reads UserService status and
then calls LoginMgr SetUserStatus. Live psn22 GOT reads resolve its setter to
`ReactNative.Modules.Vsh+0x249950`, then
`libReactNative.Modules.Vsh+0x2bc70`, a jump to `-+h1C78SdyU`.
The getter's `RJX7T4sjNgI` import was an unresolved stub in psn22.

Firmware establishes this as **property0x82b**, not login flag property6:
`libSceUserService+0x4f60` reads 0x82b; Core's LoginMgr handler
`+0x74b6d0 -> +0x748bd0` calls read `+0x9e59b0` and write `+0x9e5a20`,
both selecting 0x82b, with `(old & ~mask) | value`. The explicit local HLE
now has separate persisted `user_status`, its getter, and the owner-checked
LoginMgr masked setter. Guest status remains session-only. Failed persistent
saves roll back state and preserve output.

The user saw an unexpected passcode on User1, whose stored passcode is empty.
psn22 logged actual `X5rJZNDZ2Ss(uid, buffer, 16)` calls. That export was a
log-and-success stub which never wrote the output. Firmware `+0x48f0` selects
string property2, capped to 0x400 bytes. It now returns the actual stored string,
including NUL for an unset passcode, with size and guest-copy error handling.

Both build targets succeeded. The three targeted suites passed 3/3, including
exact status export identities, persistent/guest status changes, independent
login flags, real save-failure rollback, and unset passcodes in dirty buffers
with canaries and invalid-pointer checks. psn23 (PID11096) was launched from
copied psn22 data for live validation; these results do not yet establish UI
completion or passcode-prompt removal.

User1 displays five replacement glyphs and a broken profile image despite its
stored ASCII name. The user notes older usernames displayed correctly. Cause
remains unproven: investigate this profile's returned data and conversion
before attributing it to rendering. PFAuth remains separately blocked with
`0x80550002`; local status/passcode work does not establish PSN sign-in.

## psn24 session-loop diagnosis and backend decision

The user verified that selecting any profile, Quick Play, and completion after
Data Collection returned to the PS-button/user-selection screens. Names were
normal during creation but replacement glyphs after selection; profile images
were still missing. Guest PSN sign-in still displayed NP-102933-8.

psn24 shows persistent `User2` (`0x10010003`) and guest `0x20010000` surviving
creation without the earlier DestroyUser rollback. Its managed login result
is `success`, `retCode:0`, but `foregroundUserSet=False`. Both stored persistent
profiles still have `last_login_order:0`; no actual shared-store login occurred.
The UI success report is therefore not evidence of a functioning session.

ShellUI `LoginProcess` at `+0x3ee680` queries `MZxH8029+Wg`, then at
`+0x3eea04` calls `uvVR70ZxFrQ(user, -1)`. Both were unresolved success stubs
in psn24. Firmware UserService identifies these as a logged-in predicate
(`+0x3870`, searches the appropriate current login vector) and synchronous
login (`+0x28c0`, IPC0x30000, 16-byte user/device input, waits for completion).
Logout `3T9y5xDcfOk` at `+0x2a80` uses IPC0x30001 with user/reason input.

**Decision:** continue the explicitly selected local UserService/LoginMgr HLE
backend with one shared profile/session store, keeping Sony's native/managed
UI execution. Native LoginMgr initialization/admission remains unavailable.
Local HLE operations must update real local state and return errors for
unsupported operations. In particular, local sessions never imply PSN sessions.

The login/logout exports now use the shared transitions and retained boot
caller checks; the predicate uses the caller's actual login list. Device=-1 is
supported as used by ShellUI. Device-specific association returns unsupported
until implemented. Tests exercise the actual SysV exports, foreground identity,
idempotency, session events, guest removal, invalid device association, missing
caller and retired caller. Both build targets succeeded and the three suites
passed 3/3. psn25 (PID1744) uses the same saved profiles for user-driven live
validation. Reaching Home remains unverified at this checkpoint.

## psn25/26: real guest login, then theme crash; requested profile cleanup

psn25 and psn26 both commit guest `0x20010000` as the actual foreground user
with login order1. The native login export returns0 and ShellUI proceeds to
SetForegroundUser. Home does not complete: the managed UI throws an unhandled
NullReferenceException in `Sce.Vsh.ShellUI.Theme.ThemeManager.Apply` at
RVA0x1742 (`+0x132` from method0x1610). It dereferences the null local theme
entry at `[rbp-0x38]`. The screen remains on Please wait after the crash.
psn26 evidence is `_Runs/psn26/_kyty.txt` lines464614-464615 and473457 onward.
The reason that theme entry was not populated remains to be traced.

psn25 name diagnostics and a bounded debugger observation at ShellUI
`+0x6df48d` confirm the UserService getter returns ASCII `User1`/`User2`
successfully, with capacity17 and length5. Four observations were saved to
`_TempData/psn25-username-boundary.json`. A later post-conversion observation
captured no calls; managed string conversion and the displayed glyphs remain
unverified. This does not establish a renderer fault.

The user requested deletion of the two bad test profiles and inspection or
closure of the stuck run. After confirming the same crash, stopped psn26
PID30880 and removed persistent IDs `0x10010000` and `0x10010003` from the
active `_Runs/psn23/sd/priv/user/users.json`. The active store is empty;
`next_user_number` remains5. The session-only Quick Play guest ends with the
process. The previous JSON is retained locally at
`_TempData/psn26-users-before-requested-removal.json`. No restart was performed.

## Local callback dispatch checkpoint

Further 13.60 disassembly confirms the missing notification mechanism:

- UserService initialization at `+0x1d17` creates `SceUserServiceEvent`, entry
  `+0x2380`. It waits for events and drains dispatch via `+0x1e10`.
- Dispatch `+0x213a..0x232c` deduplicates callback/userdata pairs, releases the
  registry lock before `call r12` at `+0x2216`, then rescans registrations.
  Login completion is acknowledged at `+0x2300`, after delivery. Logout uses
  the equivalent acknowledgment at `+0x231b`.
- Register `wuI7c7UNk0A +0x34d0` treats null as a successful no-op. Unregister
  `spW--yoLQ9o +0x35f0` returns invalid-argument for null; an absent non-null
  pair returns success (`+0x37b0`).
- ShellUI callback `+0x6df1c0` maps login0 to PSM event40, logout1 to41.
  Theme `OnLogin +0x11f0` initializes a missing user entry through `+0x18e0`.
  That method allocates the entry and inserts it in the dictionary at `+0x19ca`.
  `Apply`'s fallback calls OnLogin but does not refetch its previously null
  lookup result. Proper notification before foreground processing is required.
- psn26 registered five ShellUI targets: `+0x54fb10`, `+0x3aff70`,
  `+0x6df1c0`, `+0x738a0`, and `+0xa5d130`. The latter two were also inspected;
  `+0x738a0` handles creation/destruction, and `+0xa5d130` includes login work.

**Decision remains local service HLE + native/managed Sony UI.** Native
UserService dispatch is understood, but LLE would still depend on unavailable
LoginMgr/service admission. Do not patch Theme.Apply to conceal the omission.

The selected local backend now registers callback/userdata pairs against the
retained boot owner and validates targets against its executable ELF segment.
Each registering boot owner has an inherited-owner guest pthread, independent
execution boundary and retained state through actual join. Shared-store
login/logout (including logged-in deletion) queue events and wait for callback
completion outside store/registry locks. Session transitions are serialized
through that acknowledgment. Queries and registration are reentrant; nested
session mutations from the delivery thread return unsupported. Unregistration
removes future admission and waits for an invocation on another thread to finish.
Retirement closes callback admission and joins the worker before owner release.
Application callback registration is still unsupported by this local boot-role
path; application poll subscriptions continue through the existing owner store.
Creation/destruction notification types beyond login/logout remain unfinished.

Both targets build. `local_user_creation`, `runtime_user_service_owners`, and
`runtime_user_service_controllers` pass 3/3. The creation suite now runs the
actual callback exports on a real retained worker, checking payload order,
completion-before-return, duplicate pairs, reentrant queries and removal,
unregistered delivery suppression, invalid executable targets, retired callers,
and actual worker join. Test fixture issues (an open ELF blocking a directory
rename, and worker TLS cleanup needing main-thread loader initialization) were
corrected before this passing result. Live Home completion, names/images and
genuine PSN sign-in remain unverified/unresolved at this checkpoint.

## psn27: theme completion verified; visible Quick Play still stalls

The user reports Quick Play still stalls and explicitly requests an independent
agent architecture review instead of continuing isolated patches. That review
is in progress; no new HLE/LLE decision has been adopted from it yet.

psn27 PID14096 registered all five ShellUI targets successfully. Native log
`_Runs/psn27/_kyty.txt` lines5191532-5191721 shows every login callback returning
normally before the login export returns0. Managed `_Logs/psn27.log` records:

- 126179-126180: Theme.OnLogin and InitializeUserEntry for `0x20010000`.
- 126209-126226: Theme.Apply starts and finishes; the psn26 null-reference
  failure is absent in this run.
- 126303-126341: native/managed LoginMgr completes its exit-handler sequence.
- 126359-126365: focus changes from Processing to Render.NPXS40002 and the
  real rnps-home app mounts. This establishes Home initialization, not visible
  or usable Home completion; the user's visible stall remains authoritative.

The emulator had already exited when inspection was attempted, so no stalled
thread snapshot or post-login screenshot was captured. A bounded post-name-
conversion observer expired before input and recorded zero calls; it detached.
Native compositor disconnection messages already begin at managed log line950,
well before login; they cannot alone explain this new transition failure.

A concrete additional lifecycle gap appears after login: native AsyncStorage
at native log5242286-5242366 fails `sceKernelStat(userPath)` with ENOENT and
returns `0x81860001` for guest `0x20010000`. Disassembly of
`libSceAsyncStorageInternal.sprx` gives:

- `_determineDbPath +0x177c` calls PathManager `+0x11ce0 -> +0x11d40`.
- `_determineHomePath +0x11670` obtains the sandbox word via `JGfTMBOdUJo`.
  At `+0x11a10..+0x11a5d` it constructs `/<word>/priv/home`; `+0x11e81`
  appends `/%08x` for the user. It then requires that directory to exist.
- `BindSysCoreMmsSandbox` currently aliases only `/<word>/priv/mms`.
  The active system-data private/user home roots contain only `00000000`,
  not `20010000`. Local profile creation currently persists a record but
  does not provision native per-user storage or that sandbox alias.

This is a confirmed profile/storage contract omission. Its exact contribution
to the visible Home stall still requires verification; no success stub or
manual directory workaround was added. A complete lifecycle design must cover
storage publication, rollback, guest cleanup, native notifications and restart.

Additional ABI audit findings were sent to the independent reviewer: the HLE
maps `bwFjS+bX9mA` to GetUserNumber, but regular native `+0x24d0` tears down the
UserService event thread/client and is a no-argument termination path. The true
number export `qbwy0Ub8b3M +0x3860` dispatches through table `0x1c2a8`, default
`+0x40f0`, IPC0x30012. Native color wrapper `+0x37e0` maps numbers1..4 to
colors0..3, unlike the HLE's raw user-ID low28 bits. These bindings require
contract-level correction; they were not changed speculatively in this run.

## Checkpoint validation

Previously completed on this working implementation: emulator build, formatter
regression (including actual SysV variadic snprintf), MetaDbg/filesystem checks,
NP IPC fixture suites, and integrated NP/UserService checks. Live dialog
visibility and dismissal are user-confirmed. The additional work in this
checkpoint is static firmware tracing and documentation; it does not establish
successful local creation, authenticated PSN, or post-change media switching.
