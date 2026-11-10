# Retail co-ELF GC context lifetime (2026-11-09)

## Live evidence and native contract

In the immutable retail-apps1 mixed comparison, notification RNPS reached
`runApplication` and JSConsole; the user observed particles and shine
background, not confirmed Settings or Home. The process later aborted after
`pthread_suspend_user_context_np failed for 000000FFA2FAE050: 45`.
Guest line 387003 identifies that same record as `CoElf:SceShellCore.elf`,
host thread 32684. Earlier guest lines 23633, 23641 and 23652 show three
separately initialized main records, followed by ShellCore, compositor, and
eboot co-ELF entries. The old implementation kept only one global main record;
subsequent registrations made an earlier live external-main record fail the
pool-or-main validity check. That concrete defect is reproduced by the
failing-old fixture. Error 45 also had a shared-runtime-caller/target branch;
the snapshot does not separately identify which guard returned it at runtime.

Actual IDA Pro 9.3 inspected original retail `libmonosgen-2.0.sprx` SHA256
`294af118b930006345855ecc63e452d3fa28a2c138136755c6f2066b15938a40`,
using a private ELF-type-only adapted copy SHA256
`b97d812f96ddf57426de8445de80ee65f6a2ac7bd2563e9ef78ca73a74531378`.
Mono `+225440` calls suspend NID `cfjAjVTFG6A` at `+225468`; zero return
continues to `YkGOXpJEtO8` get-context at `+22549c`, while nonzero logs the
observed failure and takes the fatal path. The resume NID `QRdE7dBfNks` is
called at `+225564`. Context consumer `+30a880` reads saved register slots,
including RIP at byte offset `0xe0` and RSP at `0xf8`, consistent with the
existing 1280-byte emulator record. The observed abort return `+214252` maps
to Mono, whereas the unrelated final trace addresses in `0xa300...` map to
`libScePubSubClientShared.sprx`; they do not identify the abort caller.

## Implementation and boundaries

Register each externally initialized main thread independently, retaining its
record for safe pointer validation instead of overwriting a single global
pointer. A thread-local lifetime guard marks an exiting record unavailable and
closes its retained OS handle under its stop mutex; the record itself is not
reused. The real legacy target pin still duplicates and validates its native
handle, and existing lock-critical / context capture / resume guards remain.
Unknown or exited targets return ESRCH, and runtime-owned targets retain
EOPNOTSUPP. Bounded logs distinguish a shared caller from a failed target pin;
no error is turned into success and no GC or synchronization is bypassed.

Production-object fixture starts two real external mains, pins and suspends
the first after the second registers, reads a real captured RIP/RSP, resumes
the same OS thread and checks exit, stale and invalid handle rejection. The
exact previous `pthread.cpp` linked against the fixture fails at the first
co-ELF pin, proving the old defect. Retail guest execution and GC progress
remain separate runtime gates; this test does not establish Home or PSN.

## Isolated candidate verification

Built source checkpoint `620c5b4c` against read-only shared Windows build
source `5ee6a153`, with at most two jobs. Retail cookie, SSL, devctl and M2
consumers and `pthread.cpp` were recompiled; every linked object/library was
hashed before and after. No `vsh-integrated` inputs. Private receipt and link
maps remain beside the candidate:

`${USERPROFILE}\AppData\Local\Temp\opencode\retail-coelf-gc\kyty_emulator.exe`

SHA256 `48bfcb0a44d2bdeb2e06d6bee438d0d225190a43bd9648a62478c9c06a24e2a7`.

Production-object fixture passed real two-external-main admission, native
suspend/get/resume, RIP nonzero and RSP within the actual target host stack,
invalid pointer and exited-record rejection; it then passed ten additional
independent process runs. Linking the same final fixture against exact
pre-fix `pthread.cpp` from `6c52ee77` fails on first-main pin after the second
registration, as expected. Devctl and M2 production-object fixtures, metadbg
and sshot smoke checks, retail native SSL policy, HostHTTP, HTTP/HTTP2 and
RNPS curl loopback suites passed. Linked-PE audit checked all 57 retail SSL
exports and preserved RNPS path resolver call targets.

The first candidate built before the final guarded cleanup/diagnostics is
superseded; do not use its SHA `907a7576cc89e305222ecd425038717fc0cfc69e1f9cd1d631911e1ec8e0fb6a`.
The full filesystem CMake test target was not built by this incremental
recipe. No emulator process was launched or accessed. Parent's next run can
use the same explicitly mixed cache/shared-host/app preparation and native
SSL/libc configuration as retail-apps1, changing only the executable. Record
whether the original suspend/abort recurs, whether new bounded shared-caller
or target-pin failure diagnostics appear, and whether actual settings rendering
or a later real blocker emerges. A passing fixture is not proof of guest GC,
Home or PSN authentication.

## Parent live comparison

The parent reviewed the source and launched the hash-verified final candidate
as `retail-coelf1`, retaining retail-apps1's mixed plaintext mounts, native
providers, URL cache and replay-disabled configuration in a new disposable
store. Both notification overlay and Settings now reached `MountApp
runApplication` and emitted JSConsole output. The process remained open at
frame 428 / 1 fps after two observation intervals. The first captured snapshot
contains no preceding suspend failure, GC refusal diagnostic or `Guest abort()`.
This is observed progress, not exhaustive GC validation.

The user reports a PS5-controller prompt, beyond the prior background-only
observation; exact wording and successful navigation remain unverified. The
parent verified the executable base's default keyboard mapping (P/Home for PS,
J for Cross, arrows for D-pad, L for Circle) and provided it to the user.
Controller navigation remains user-controlled. No Home or PSN success claimed.

Local immutable evidence: `retail-coelf1/checkpoint1`. Guest log SHA256
`ebc5971237eb78f32d3ebefc4816f5a7f46c865506c27dd7a1a00a1803880a49`;
host log SHA256 `564b09b7f7dc90dc3c88d8c99d6ea599b9be5ead6359f92536ed7c535865f076`.

The subsequent user screenshot confirms a controller/USB connection onboarding
scene over the shine background. Host output identifies the active scene as
`ps5:settings:onboarding:connect controller`; Settings UI is now visibly beyond
the previous background-only result. This is still initial setup, not Home.
The user suspects a generic-controller identity. Guest logs instead show
`PadDeviceOpen` selecting the keyboard/mouse DualSense at type 4 and an MBUS
owner-change event with device type 24. That does not establish consistency of
all identity, connection, ownership and PS-button paths or setup acceptance.
Those checks are the next investigation; no automatic navigation was sent.
The corresponding immutable logs and observation are in `retail-coelf1/checkpoint2`.

At the user's request, the parent reopened this same hash-verified candidate as
`retail-coelf2` with the same mixed overlays and fresh disposable setup store.
The process check found neither retail nor YouTube still running, so no process
was closed by the parent for this retry. Earlier stores/logs remain retained.
No controller correction is included in this retry; acceptance is still under
investigation. The local `retail-coelf2/launch.json` records its launch identity.
The user confirms this retry still requests a PS5 controller. Reopening the
existing crash-fix candidate has not resolved controller acceptance.
