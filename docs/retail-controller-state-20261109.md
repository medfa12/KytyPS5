# Controller onboarding: native system-state contract

In checkpoint2, host LoginMgr received PS-button events for device 2, user -1;
this is not merely a key-mapping failure. The explicitly mixed, unauthenticated
settings plaintext used by the run has SHA256
`f6e0b33e66df1847cf20a1661bfc3c3d7eeb606f3456f31ce88780438aa72718`.
It equals `application.ps.hbc` found in the retail directory, but that copied
plaintext is not thereby authenticated or retail-provenance. Its adjacent
decompilation shows `ControllerSetupScreen` subscribing to
`Peripheral.connected`; `_onConnected` advances to the language screen only
for a type in `initialOnboardingAvailablePeripheralTypes`. No PS-button handler
is present in this screen's connection gate.

Actual retail managed AOT `Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx` SHA256
`8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa` was
inspected in IDA Pro; the private input changed only ELF type. Its embedded PE
metadata identifies `PeripheralModule.OnDeviceBasicEvent` at `+0x6fae0` and
`NotifyConnected` at `+0x1a1930`. IDA shows distinct event cases and, after a
successful type conversion, a `connected` event containing `deviceId` and
`type`. Managed retail LoginMgr PE metadata distinguishes `DeviceAdded` (0)
from `OwnerChanged` (2). Snapshot guest lines 288835 and 289184 report
`[onDeviceAdded] system state not working :0`; host line 717 records only an
owner-change event. This is strong evidence for a missing upstream prerequisite,
not proof that fixing it alone causes the JS scene to advance.

Actual retail `libSceSystemService.sprx` SHA256
`cb58e10eb4f39bc3e1b69735969525ea8dcd0d6808f3cfeb1bd77f3ee513a968`
was inspected in IDA Pro 9.3 using an ELF-type-only private copy SHA256
`8bb19c6da6b87a46fbc694d92663962b4f8ed7bcf2f5971271e221f23b59f102`.
Its initializer `+0x1c180` opens `SceSystemStateMgrInfo`, and exported
`ze0ky5Q1yE8` at `+0x1c610` takes no arguments, polls the flag with mask
`0xffff` and mode 2, sleeps 1 ms while the low 32-bit result is zero, and
returns those bits. The previous HLE instead treated stale RDI as an output
pointer, wrote 1 when writable, and returned 0. Snapshot guest line 41341
shows the actual guest state manager entering WORKING before LoginMgr's
rejections; line 75030 reports `currentState=0` through the broken export.

The correction opens and polls the real named guest event flag, returns its
observed state and waits for initialization rather than inventing a state,
owner, device transition or completed onboarding. Guest-driven state changes
remain visible. No Home, controller acceptance or PSN success is implied.

## Native consumer and candidate verification

Actual IDA on retail `NPXS40087/eboot.bin` SHA256
`f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`
(private ELF-type-only copy SHA256
`0ea43b4cc6b323a5ffa53e9db8c6c4473ac72abe9da65455c87b049e81a2dd79`)
shows `onDeviceAdded` at `+0x341700`: state 1000 continues, 500 enters a
standby wake path, and other states take the logged rejection. This identifies
the failed native prerequisite; it does not establish when the downstream
connection listener receives an event or whether there are further gates.

Source checkpoint `40a2c016` was built in the isolated directory
`${USERPROFILE}\AppData\Local\Temp\opencode\retail-state-flag` using the
read-only shared Windows build source `5ee6a153`, at most two compile jobs and
no `vsh-integrated` inputs. The candidate `kyty_emulator.exe` has SHA256
`406213d56cbd989e8d696d82d62e2bdd81999f2d0938c15258c6993fa210a8ae`.
The receipt hashes link inputs before and after. It recompiles `libC.cpp`
alongside the previously required retail consumers. The previous candidate's
receipt identifies shared `libC.cpp.obj`, `libPad.cpp.obj`, and
`controller.cpp.obj`; their source files and shared MBUS header match commit
`5ee6a153`, although the receipt alone cannot prove historical object/header
provenance beyond its recorded hashes.

The production-object state fixture creates the real named kernel flag,
checks zero-state blocking, publishes 1000, verifies the returned value and
unchanged stale argument, then changes the real flag to 500 and observes 500.
It passed once plus ten separate repeat processes. The same blocking control
linked to the unchanged shared-base `libC.cpp.obj` fails as expected because
the old export returns immediately. Co-ELF context, devctl, M2, retail SSL,
HostHTTP, HTTP/HTTP2 and RNPS curl fixtures passed. Linked-PE audit verified
57 SSL exports and retained RNPS resolver targets. Full filesystem CMake test
target and a guest run were not performed. The parent owns all live processes;
none was accessed by this investigation.

The parent may review and launch this candidate in a fresh isolated store using
the same explicitly mixed inputs. Inspect whether `onDeviceAdded` still logs
state 0, whether the `DeviceAdded`/connected event and language screen occur,
and whether a later real prerequisite blocks progression. Never infer setup,
ownership, navigation or authentication from the fixture alone.

## Parent launch

The parent reviewed the corrected export and fixture, then launched the
hash-verified `406213d5...` candidate as `retail-state1`, using the same mixed
RNPS overlays, native SSL/libc, URL cache and disabled replay as retail-coelf2.
The earlier emulator had already exited; no closure was performed. Its logs
and manifest were preserved in `retail-coelf2/checkpoint1`. The new run uses a
fresh disposable setup store and normal audio; its exact configuration is in
the local `retail-state1/launch.json`. Controller acceptance and visible
progress are pending. Observation waits do not close the emulator.

## First runtime result

In `retail-state1/checkpoint1`, the guest reports `currentState=1000` and
`onDeviceAdded` reaches `Auto assign rule is Initial Setup Mode`, rather than
the previous state-zero rejection. Host output now contains managed
`DeviceAdded` for device 2 / user -1, followed by `OwnerChanged`. Both Settings
and notification apps execute. The connect-controller scene still loads;
no language-screen advance, completed onboarding, Home or PSN authentication
is established. Event delivery/subscription and subsequent prerequisites need
further analysis. SharedStore `0x80020002` persists.

The observation command left the process running; its sample was frame 91 /
0 fps, responding. Immutable guest log SHA256:
`251f69b588f60e9f106af652347e3735cd3a4b816845168d959e30e4a50b6595`;
host log SHA256: `e21b99dba71d64754c4c09a8cecbef26a330baee52b67389afc91a8388741fe3`.

The user confirms that this candidate still asks for a PS5 controller. Thus
the corrected state query and observed DeviceAdded delivery have not resolved
visible controller acceptance; onboarding remains blocked.
