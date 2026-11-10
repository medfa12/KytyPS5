# Retail controller added-event consistency

Parent's immutable mixed `retail-state1/checkpoint1` confirms the prior
system-state fix: guest current state 1000, native Initial Setup Mode, and
managed `DeviceAdded` for device 2; the controller connection scene still
appears. This is not a strict-retail or authenticated run.

Correction to `retail-controller-state-20261109.md`: the mixed Settings
decompilation also calls `isUnassignedControllerConnected` after installing the
listener. That helper calls `getUnassignedPeripheralInfoList` with
`initialOnboarding: true` and advances when its results are nonempty. It is
therefore incorrect to infer that an earlier event alone must be lost. The
actual reason the existing-device query does not advance remains unverified.

The retail managed AOT consumer `PeripheralModule.OnDeviceBasicEvent` at
`+0x6fae0` dispatches DeviceAdded (enum 0) to `NotifyConnected`; the latter
emits a connected event with deviceId and converted type only after successful
type conversion. The associated retail ReactNativeShellApp AOT source hash is
`8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa`.
Its `PeripheralModule` static initializer identifies the onboarding-allowed
strings as `controller`, `standardController`, and `leonardo`. Its native AOT
unassigned-list worker at `+0x1a6650` requests a device list, filters it,
converts each device and, when `initialOnboarding` is true, retains only those
whose converted type is in that list. The actual runtime query's individual
results and conversion outcome have not been observed.
Actual retail IDA `NPXS40087/eboot.bin` SHA256
`f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`,
private ELF-type-only copy SHA256
`0ea43b4cc6b323a5ffa53e9db8c6c4473ac72abe9da65455c87b049e81a2dd79`,
shows its native event dispatch at `+0x341320`: added-event id is at `+0`,
device ID at `+8`, device type/transport at `+0x10`, and subtype at `+0x14`.
The event payload is passed to `onDeviceAdded` at `+0x341700`.

The snapshot's LoginMgr-generated enumeration event at guest line 347173
reports type 0, subtype 24, while the HLE-queued MBUS added event at 348118
reports type 0, subtype 0 for the same device. `MakeMbusPadEvent` previously
populated the subtype only for owner-change records. Populate transport and
subtype consistently for the already-existing added record as well; do not
generate an extra event or alter the owner. The production-object fixture
decodes actual added and owner event buffers and checks their fields against
the enumerated pad constants.

LoginMgr also logs a bus-transition lookup error `0x80b60031` with unique ID
zero, followed by a call to unresolved `sceMbusDisconnectDevice` returning
stub zero. The retail function at `+0x35fab0` attempts a prior-device lookup;
a missing previous device can be a fresh-store condition, not evidence to
force a successful binding. The stub's success is not proof of an actual
disconnect. No root cause is asserted for either the unassigned-list result or
the remaining controller setup screen.

## Isolated candidate and controls

Source checkpoint `0865711a` was built against read-only stable Windows source
base `5ee6a153`, with no `vsh-integrated` inputs and at most two compile jobs.
The isolated candidate is
`${USERPROFILE}\AppData\Local\Temp\opencode\retail-mbus-event\kyty_emulator.exe`,
SHA256 `4735fba3a5634595171fd1925b3db3d992b69b204010313a93ef951fe1ad0f58`.
The receipt records source and link inputs, with hashes checked before and
after linking. It retains the previously tested system-state fix.

The production-object fixture checked the actual queued added and owner events,
including device ID, transport, subtype and unchanged computed owner; it passed
the build run and ten independent repeats. The identical fixture linked to
the previous state-candidate `libC.cpp.obj` failed on added-event subtype as
expected. System-state, co-ELF context, devctl/M2, retail SSL, HostHTTP,
HTTP/HTTP2 and RNPS curl fixtures also passed. A linked-PE audit checked 57
SSL exports and RNPS resolver targets. The full filesystem CMake target was
not built by this incremental recipe. The candidate has not been guest-run.

Parent review and a fresh isolated mixed run can establish whether LoginMgr's
actual MBUS added record now reports subtype 24 and whether the unassigned
query and onboarding progress. A matching event is necessary consistency, not
proof that the existing-device query returns an accepted controller or that
later setup, navigation, Home or PSN succeeds. No live process was accessed.

## Parent launch

After reviewing the payload correction and recorded controls, the parent
launched the hash-verified candidate as `retail-event1` with the same mixed
cache/shared-host/application overlays, native retail SSL/libc, normal audio
and replay disabled. The process check found no emulator still running.
A fresh disposable store is used; the local launch manifest records exact
inputs and process identity. Subtype consistency and visible controller
acceptance remain pending. The background observation command only waits;
it does not close the emulator.

## First guest result

`retail-event1/checkpoint1` confirms subtype 24 on both the LoginMgr-generated
enumeration record and the queued MBUS DEVICE_ADDED record. System state is
1000, native initial-setup assignment processing runs, and managed LoginMgr
receives DeviceAdded and subsequent user PS-button events for device 2 / user
-1. The subtype inconsistency is corrected in guest execution.

The connect-controller scene still appears in the captured host log, with no
observed language-screen advance. A current user visual result is pending;
controller acceptance, Home and authentication remain unconfirmed. The next
necessary investigation is the actual unassigned-peripheral query and type
conversion/filter results, not another assertion that identity alone fixes it.
Guest log SHA256: `5b0f1076c98a82c483b85a35ccbeacdb4d57a60fca77b9484c5c7ce9202d5bd1`;
host log SHA256: `9b289428a7595e6f12c584cf7a81f1375ca00c1dc3ec6a8e73aef4633a07b16e`.

The user confirms the PS5-controller prompt persists on this candidate and
requests Astra medium for continued investigation. The parent switched the
existing retail agent to `openai/gpt-6-astra#medium`, preserving its task and
worktree. Event subtype consistency has not resolved visible acceptance.
