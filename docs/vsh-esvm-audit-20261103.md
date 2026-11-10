# Esvm native-media admission audit — 2026-11-03

Read-only binary/source audit; no application execution or build. All binary
offsets below are module-relative RVAs unless specified otherwise.

Sources: verified `Downloads/13.60/system_b/common/lib/` and local4.03 tree
`Downloads/4.03Effort/voldemort/zecoxao/kek/system/common/lib/`.
Names confirmed by hashing firmware diagnostic strings to Sony NIDs.

| NID | SysV signature | local4.03 libSceEsvm | 13.60 libSceEsvm |
| --- | --- | --- | --- |
| h59QfNm3QKU | int sceEsvmInitialize(const SceEsvmInitParams*) | 0xac0 | 0x1e30 |
| 39VZ-wS-7mI | int sceEsvmTerminate(void) | 0xb20 | 0x2950 |
| ouGTOwK1yDc | int sceEsvmVideoOutIsPortStatusUsable(bool* out) | 0x2620 | 0xc6f0 |

`ouGTOwK1yDc` is not provisioning. `sceEsvmIsProvisioned` is `f66NW1ZHh8U`.

## Initialize

Local4.03 requires nonnull params and uint64 size 8, otherwise 0x8a790004.
13.60 accepts sizes 8, 32, 40 (table 0x3b1c8; validation 0x1e4e–0x1f07).
Extended records require nested size 24 at +8 and zero reserved bytes
+0x14..0x17. Size 40 also requires zero +0x21..0x27. Byte +0x20 controls
video resizing; older records receive its default.

Current YouTube libstarboard.prx calls at 0x9f4ba with constant 0x484ee0;
Apple TV eboot.bin calls at 0x51b71 with constant 0x2388240. Both construct
40-byte records, nested size 24, zero resizing flag. Local4.03 rejects these
current records.

13.60 internal initializer 0x134f0 atomically claims initialized state
(duplicate 0x8a790005), allocates 30 MiB flexible and 104 MiB direct memory
(alignment 0x4000), and queries getpid/sceKernelGetAppInfo at 0x1372b–0x1375f.
It rejects category 0x10300 with 0x8a790010 and requires app-info byte +0x4d
mask 0x02, otherwise 0x8a790002 (0x1377c–0x13788). Local4.03 checks the same
permission at 0x3eb6, with older allocations of 64 and 76 MiB.

Current src/libs/libShellCoreFloor.cpp KernelGetAppInfo zeroes its 88-byte
record and fills only category +0x50. Thus that required permission is absent.
Its authentic metadata source must be audited before populating the field.

2026-11-04 follow-up: `direct-media-foundation-20261104.md` identifies the word
as title ATTRIBUTE3 at +0x4c and records the firmware evidence/provenance limits.
A bounded implementation now snapshots that word and publishes it unchanged
(missing means zero); compilation and targeted identity-test execution are pending.
The preceding description is the 2026-11-03 baseline, not current source behavior.

13.60 then initializes MediaFramework and registers `citroncore` through
0x24950. Required imports include S8+GY2JSXnA (initializer), ilSc3Cc3kek
(registration), CwgReBYHFH4 (unregistration), qHa1HbTmGeE (termination).
Current libMediaFramework.cpp does not register these operations.

## Terminate and output status

13.60 termination implementation 0x138b0 returns 0x8a790003 when uninitialized.
Otherwise it unregisters/terminates MediaFramework, destroys context/key
registries, releases memory and clears initialization flags. An HLE equivalent
must scope this state to the owning application.

Port usability follows 13.60 0x1c2b0 -> 0x28020, local4.03 0xa9a0 -> 0x15b30.
Null output returns 0x8a790004; uninitialized returns 0x8a790003. It requests
MediaFramework output status with selector 1 through che6EGiH2gQ, checks
transport and embedded status, then invokes VideoOut d7eh3psk1J8.
13.60 VideoOut 0x15110 computes (~status.word0) & 1. Exactly one output byte
is written on boolean success; failure leaves output unchanged.
Apple checks return and output independently, e.g. 0x50c90–0x50ca2.

Firmware MediaFramework status operation 0x1a180 uses completion event 0x25.
Its server/socket/worker/shared-memory infrastructure is real: strings
SceMediaCoreServerIpcSocket at 0x594c0, SceMediaFwEsvmSenderThread at 0x506b0,
/SceMediaFwEsvmShm at 0x50b27. che6EGiH2gQ is also absent from current HLE.

## Remaining boundary

These three exports alone cannot establish faithful successful initialization.
Next foundation is authentic app-info permission metadata plus owner-scoped
MediaFramework lifecycle and output-status semantics. Do not return invented
success or synthesize usability independently of actual output state.

YouTube additionally imports Widevine initialization/access, provisioning,
key sessions, callbacks, context/decoder/access-unit/render APIs. Apple TV
imports PlayReady initialization/access and corresponding session/decoder APIs.
Import resolution is not proof of native entry, rendering or playback.
