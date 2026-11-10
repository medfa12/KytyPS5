# Retail devctl and AutoMount dependency (2026-11-08)

## Observed failure and evidence boundary

Parent's immutable strictretail3 snapshot shows real native SSL libc/SSL
initializers returning zero and no `8095f008` in the retained snapshot; it
does not prove authentication. AutoMount then opened `/dev/devctl` with flags
`0x4`, received `80020002` because the emulator resolved it under its empty
root, and returned that error (guest lines 78708–78715). MediaEventListener
subsequently reported null eventSorter. EHdd and M2 named-event failures also
remain downstream observations, not proof that devctl is their only blocker.

Retail `SceShellCore.elf` original SHA256 is
`f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`.
Actual IDA Pro 9.3 analyzed private ELF-type-only adapted input SHA256
`a4072f89b8c7d416afae8216c21a0bc718dc5aa853c40739bc1819035acdf669`,
image base zero; the original image was not modified. The following are IDA
disassembly and Hex-Rays findings, not runtime observations:

1. `AutoMount::Init` at `+3a42e0` opens `/dev/devctl` at `+3a4319` with
   `0x4` (O_NONBLOCK); a negative result exits before any listed initialization.
   Even with a valid descriptor, `DiscHandler::Init` and `_initExApi` may
   independently fail and close it. External HDD controller init follows.
2. M2 UI flag creator `+3cb4c0` is called at `+3a454e`, after those gates.
   It creates `SceAutoMounterUIM2Evf` with attributes `0x110`, initial bits
   zero, and registers callback `+3cc7e0` for M2 mount event `0x00310000`.
   Thus opening a device cannot legitimately imply M2 mount or signal its bit.
3. Successful later initialization creates worker `+3a47f0`. The worker calls
   `+3b4cc0`, which calls EHdd flag creator `+3de890`, then calls Update
   `+3a49d0`. EHdd is therefore downstream of worker creation, independently
   of the M2 flag. Its flag is created with `0x121`, initial bits `0x1f`.
4. Update performs actual M2 boot-mount work `+3e2f60` and disc initial-state
   handling *before* entering the device read loop. The loop reads at most
   1023 bytes, null-terminates positive records, logs nonpositive reads and
   delays before retry. It parses real text notifications, including
   `!system=DEVFS subsystem=CDEV type=%16s cdev=%63s`, CAM disc media,
   UMASS external-HDD timeout, and ICC disc notifications. No positive record,
   successful mount, or event bit may be invented to advance it.

## Actionable implementation boundary

`src/kernel/fileSystem.cpp` has explicit virtual device descriptor dispatch
and a close-wakeable blocking idle channel for `/dev/metadbg`, but no devctl
device or DEVFS/CAM/UMASS/ICC notification producers. Existing POSIX kevent
read/write readiness in `src/libs/libC.cpp` only polls sockets. The observed
AutoMount path itself uses nonblocking `read`, not a demonstrated kevent watch.
FreeBSD's public devctl(4) documentation describes a one-reader event stream,
nonempty single-record reads and newline-terminated records; it is useful
background, **not evidence that all PS5-specific kernel semantics match**.

An accurate next device change needs a bounded, synchronized descriptor and
record queue fed by *actual* emulated device transitions, correct nonblocking
empty reads and close/dup lifetime, and tests for ordering, truncation, races,
invalid descriptors and no invented notifications. Establish PS5-specific
single-reader/open/error/record behavior before claiming those details; retain
raw errors for unsupported operations. The M2 mount and disc initial-state
dependencies must be checked independently once the real worker runs. A
generic file or always-successful idle stub would hide missing event producers
and is not a verified contract, so none was added here. This checkpoint makes
no claim that implementing devctl alone fixes AutoMount, Home, PSN or login.

The worktree merged shared docs-only `9a5cb2df` as `4826454d`. No candidate was
built or guest-launched for this analysis; parent-owned process and runtime
stores were not accessed. IDA reports remain private local analysis artifacts.

## Queue implementation in progress

The parent directed implementation of the supported empty-queue subset rather
than waiting for removable-device producers. `src/kernel/devctlQueue.h` now
contains a standalone synchronized, bounded record queue; it is **not yet
wired into guest descriptor dispatch**. Its configured 256-record/1024-byte
limits, FIFO and truncation policy are implementation choices based on public
FreeBSD background, not verified PS5 limits. No production publisher exists.
Standalone `tests/DevctlQueueTests.cpp` passes with clang++ C++20: empty
nonblocking read, invalid records, FIFO and short-record truncation, full-queue
rejection, close wakeup and independently closed shared queue handles. Test
records are synthetic fixture inputs only, never guest events. These are queue
tests, not proof of guest descriptor, duplicate, or kernel readiness behavior.

Additional native/ABI cross-check: retail IDA's `+3a4319` call uses NID
`1G3lF1Gg1k8`; `+3a4b4b` uses `Cg4srZ6TKbU`. Strictretail3 relocation
lines 2431 and 3666 bind these to `KernelOpen` and `KernelRead`, respectively.
The native reader compares a read failure against `0x80020023` (EAGAIN) and
delays on nonpositive reads; it does not independently prove empty-read kernel
semantics. Existing emulator ABI defines flag `0x4` as O_NONBLOCK. PS5-specific
queue capacity, record truncation, exclusivity, and non-observed operations
remain unverified, and must not be represented as native measurements.

Before a candidate, integrate the queue with descriptor open/read/close,
explicitly reject unimplemented operations, implement or explicitly reject
duplication without damaging unrelated descriptors, connect real registered
device owners when present, and test against production filesystem objects.
No candidate is claimed at this intermediate implementation checkpoint.

## Production nonblocking subset

The retail filesystem now handles the observed `/dev/devctl` read-only
`O_NONBLOCK` open, returning raw `80020023` for an empty queue without
inventing a record. Each open owns a queue; `F_DUPFD` shares that queue while
retaining independent descriptor close state and close-on-exec flag. Closing or
reusing one descriptor does not close another. Only the observed open mode is
admitted; blocking operation, other fcntl commands and unmodeled ioctls return
explicit errors. This limited subset does not assert PS5 exclusivity or
blocking-read behavior. Queue capacity/truncation remain policy rather than
measured PS5 behavior. Ordinary static mounts are not device transitions.

No real removable-device backend exists in the configured USB-storage model:
`src/libs/libJscdSmall.cpp` explicitly owns an empty device set. Therefore no
production event publisher is connected, and the queue remains empty; the
queue's publish operation is not called by production. A future modeled device
owner must connect its real attach/detach transitions, rather than infer them
from host mounts or service initialization. This subset enables native guest
initialization but does not claim a complete devctl event implementation.

Production-object integration, build provenance and parent guest results are
separate gates. Native M2 boot mounting, disc state, event delivery, Home and
PSN authentication remain unverified.

## Isolated candidate verification

Source checkpoint `be3c5b21`, shared Windows source base `5ee6a153`; no
`vsh-integrated` inputs. Read-only object/library hashes were captured before
and after linking, with two jobs maximum. Recompiled retail cookie/SSL
consumers plus `fileSystem.cpp` and `libShellHits.cpp`; the binary contains
the shared base marker. Receipt and full link inputs remain outside the repo.

`${USERPROFILE}\AppData\Local\Temp\opencode\retail-devctl\kyty_emulator.exe`
SHA256 `45bbf9625c415faac86579c26d532389d1f3c8b5fcc5b3d605574bbfce179839`.

Passed: production-object devctl descriptor fixture (empty raw EAGAIN, no
output mutation, fd duplicate/close/reuse, unsupported ioctl/write/pread),
plus production-object metadbg and sshot smoke checks; standalone queue
ordering, bounds and close-wakeup tests; native SSL production policy fixture;
HostHTTP, HTTP/HTTP2 and RNPS curl loopback suites; actual-retail SSL metadata
audit and exact old-profile rejection. PE audit rechecked the 57 linked retail
SSL entries and RNPS resolver call targets. The old strictretail3 binary is a
real failing-old control for device open: it returned ENOENT at guest line
78712. Full filesystem test target was not built in this isolated incremental
recipe; the small fixture links the same production objects as the candidate.

Candidate launch otherwise matches strictretail3: original retail providers
and common-lib mount, fresh isolated stores, `KYTY_NATIVE_SSL=1`,
`KYTY_NATIVE_LIBC_STARTUP=1`, smoke absent. No release/performance-strip option
was added. This candidate was not guest-launched. Empty channel is not evidence
that real hardware notifications are supported; producer ownership, guest
initializer outcome, M2/disc behavior, Home and authentication remain gates.

## Strict-retail live verification

Parent tested the hash-verified candidate in `retail-strict4` with a fresh store
and the same original retail/native SSL setup. Native code now creates both
`SceAutoMounterUIM2Evf` and `SceAutoMounterEHddMountBlock`. ShellUI logs
`EvalAutoMounterFirst Complete`, completes backup/transfer and resolution checks,
then enters `EvalInitialSetup`. The retained snapshot has neither the old devctl
open failure nor the managed M2 getter exception; SSL capacity errors remain absent.

This establishes progression past the previous AutoMounter boot exception, not
correct storage capability handling: native M2 checks report `0x80f40047`, with
suspicious device-info values requiring investigation. Later logs also contain
`SharedStoreUriManager.SaveResponse` / `CreateWithSystemPermission` error
`0x80020002` while base-plugin/health-warning waits continue. Home and genuine
PSN authentication are not established. The immutable manifest/log snapshot is
retained locally under `retail-strict4/checkpoint1`.
