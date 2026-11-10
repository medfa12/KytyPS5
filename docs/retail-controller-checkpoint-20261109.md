# Retail controller onboarding: immutable snapshot findings

The parent-owned `retail-coelf1/checkpoint2` is a mixed, unauthenticated
comparison run, not a strict retail or PSN authentication result. The user
reported a visible USB-controller connection onboarding screen. Host lines
3844–3845 identify its focused scene as
`ps5:settings:onboarding:connect controller`.

Guest line 43417 identifies the device-open call as device 2, port type 4,
physical host controller -1; the following line identifies the keyboard/mouse
backed virtual DualSense. MBUS guest line 150219 reports device 2, type 24,
owner -1. Existing retail PSM analysis in `libMbusDevice.h` maps type 24 to
Bond; standard controller class 0 alone is not proof of a generic model.
Whether transport and all other fields satisfy onboarding is not established.

PS input was delivered: guest lines 1066671 and 1067878 show connected=1
with buttons 0x00010000 then 0, and further transitions follow. The
implementation defines that bit as `PAD_BUTTON_PS`. More importantly, host
lines 10436 and 10631 show LoginMgr receiving `onPSButtonPressed` with
`DeviceId:2`, `UserId:-1`, `ClickCount:0`. Host line 717 and guest line 289323
also record the unassigned owner event. These observations do not prove that the
onboarding application accepted the input, that owner -1 is invalid during
initial setup, or that repeated input alone will advance the scene. The
snapshot does not establish a causal mapping between individual guest and host
log lines. No pairing or successful ownership transition is established.

The candidate receipt identifies stable shared source base `5ee6a153` and a
retail recompile of `libShellHits.cpp`. `git diff 5ee6a153 HEAD` is empty for
`libMbusDevice.h`, `libPad.cpp`, `controller.cpp`, and `libC.cpp`; this source
comparison does not independently verify each linked object or header's build
provenance. The snapshot contains unresolved Pad NIDs `z9FKeblRpBQ`,
`rZiqk1bQwr4`, `b+SbV3m6n6U`, and MBUS `Lrelj2uF1Lc`; their onboarding roles
are not established merely by relocation or calls.

Next gate: use actual retail IDA and the relevant onboarding consumer to trace
the LoginMgr PS event and the connect-controller scene's native prerequisites,
including whether an unassigned device is expected, and establish the called
unresolved imports' actual contracts. Verify linked Pad/MBUS object provenance
before building any correction. Do not assign an artificial owner, change
transport or fabricate setup completion on this evidence. Home, Settings
navigation, and genuine PSN authentication remain unverified.
