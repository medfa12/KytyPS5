# Bounded IPMI lifecycle and recovery observation

Windows host-only diagnostics, **default off**. During `InitIpmiConfig_1`, before
guest execution, `KYTY_IPMI_OBSERVE=1` creates a unique 311552-byte mapped file:

```
%LOCALAPPDATA%/Temp/opencode/ipmi-observe-<host-pid>-<QPC-start>.bin
```

Exact opt-in (host environment, not `--guest-env`):

```powershell
$env:KYTY_IPMI_OBSERVE = '1'
# Use the existing isolated launch recipe with -PrintfDirection Silent.
# Add no setup, recovery, registry, user or event overrides for observation.
```

Disable with `Remove-Item Env:KYTY_IPMI_OBSERVE`. Existing
`-TraceNativeIpmi` is independent LOGF logging and is unnecessary for this sink.
No emulator launch is performed by the decoder or tests.

```powershell
python -B tools/decode_ipmi_observe.py '<capture.bin>' --recovery-only
python -B tools/decode_ipmi_observe.py '<capture.bin>' > '<capture.json>'
```

## What is captured

Separate append-only banks reserve 256 lifecycle, 256 general invocation, **512
recovery**, 64 poll and 128 gateway records. Invocation entry/exit share `call`.
The recovery bank is selected only by exact HLE endpoint name
`SceDbPreparationServer` and method `0x160008`. Slot11/descriptor invocation is
recorded at its vtable entry; raw slot8 is marked separately and may also produce
an inner descriptor invocation with an HLE return address.

Every invocation includes object pointer, raw return address, vtable, endpoint,
method, input/output descriptor pointers/counts, first output buffer/capacity/
written size, method-result pointer/value and exit transport return. Up to two
input descriptors are recorded, **not their payloads**. Only the exact recovery
request gets a 24-byte output preview, before and after the original HLE call.
The decoder labels the little-endian u32 at offset `0x14` as phase. The observer
does not initialize, interpret into success, or change those bytes. A readable,
same-buffer, identical 24-byte before/after pair sets `preview_unchanged`.
Before-return `transport` is not a result; the decoder exposes it only on exit.

All optional reads use bounded Win32 `ReadProcessMemory(GetCurrentProcess(), ...)`.
Partial/faulting reads are marked unavailable; zeroed storage without a readable
flag is **not evidence of a zero value**. No generic guest-payload reads occur.
Names are bounded to 31 bytes; server config is the existing 0x38-byte layout,
with its 25-byte name separately terminated. Handler pointers are never called.

Creation records cover the exported IPMI client/server factories. The unrelated
unnamed `libOfflineLogin` direct `MakeIpmiClient()` helper is not a factory record.
Connect/disconnect/destroy and dispatcher entry/exit preserve current no-op
behavior. Gateway records contain the four arguments of the existing
`IpmimgrCall` signature, not a claimed decoded firmware ABI.

## Binary schema v1

All integers little-endian u64. Header is 256 bytes: magic `IPMIOSB1`, version,
host PID, QPC frequency, start tick, invocation counter, ready, five capacities,
five attempted counters, five dropped counters, reserved. Banks start at slots
0/256/512/1024/1088. Each 256-byte slot contains:

| Offset | Field |
|---|---|
| 0 | commit: accept only `slot_index + 1` |
| 8/16/24 | event / QPC tick / invocation ID |
| 32 | 16 numeric values |
| 160 | 32-byte name |
| 192 | 24-byte preview |
| 216 | host thread ID |
| 224 | reserved |

Invocation values: client, caller, vtable, method, inputs, input count, outputs,
output count, result pointer, first output buffer, capacity, written,
method result (u32), transport (u32, exit only), flags, raw-written pointer.
Raw invocation descriptor addresses refer to observer-local adapter descriptors;
value15 records the actual caller's written-size pointer. Raw `written` is
available only when `raw_written_readable` is set. Null or unreadable pointers
leave that flag unset; synthetic adapter zero is never reported as an output.
Input records: client, caller, vtable, method, index, payload pointer, size,
descriptor address; flags in value14.
Client creation: client, caller, vtable, config, supplied storage, HLE-owned bool.
Server creation: server, caller, vtable, config, arg2, storage, buffer size0,
buffer size8, handler, packed flags18/32/33. Connect: client, caller, arg,
arg size, result pointer, method/transport zero on exit. Dispatcher:
server, caller, work, size on entry; server, caller, return on exit.
Gateway: a0..a3, caller, return on exit. Poll: client, caller, a1..a3,
event-output pointer, a5 on entry; client, caller, return on exit.
Disconnect/destroy: client, caller. Event IDs and readability flags are defined
in `src/libs/ipmiObserve.h` and decoded symbolically by the script.

The gateway bank also records optional 13.60 PsnCore startup diagnostics:
event18 filters printf callers `0x910c4e192` and `0x910c4db75`, preserves the
variadic context and forwards exactly once. Values contain caller, six original
GP arguments, format offset, stage mask, Syndy root, two NP allocator GOT targets,
root/binding readability, preview readability and termination. Up to 192 format
bytes are split into 24-byte chunks; no guest format is executed by the observer.
Fixed addresses assume the documented comparison launch's ShellCore base
`0x910000000`. Event19 now records the corrected NP manager Create ABI:
values0..5 = caller/requested size/name pointer/supplied buffer/mode/status (uint32),
with value15=1. Mode0 is CreateKernel; mode1 is CreateBuffer. Old captures with
value15=0 retain their historical caller/size/name/a2/out/pointer-result decoding.
Event20 remains caller/descriptor pointer, including null before initialization.
These events share the existing gateway budget and opt-in environment variable.

Event22 (`np_memory_callback`) records values0..6 = caller/operation/requested
size/input pointer/context/result pointer/native diagnostic count. Operations
0/1/2 are alloc/realloc/free. This exposes the actual 0x568 callback request made
by native array-new for ShellCore's 0x558 payload. Free's result field is zero
(void ABI). The counter is not a live-allocation count: realloc leaves it unchanged
and initialized free decrements even for null/foreign pointers. The native U32
counter wraps modulo 2^32 and is zero-extended into the 64-bit record field. All records are
emitted after the allocator lock is released, with no guest dereferences or
changes to forwarded results. Default-off and the shared 128-record Gateway cap
apply; schema version/record size remain unchanged.

Event23 (`np_heap`) observes per-heap HLE exports. Values0..8 are caller,
operation, subject, opaque handle, size, input pointer, result pointer, uint32
status/error, and auxiliary value. Operations0/1/2/3 are Init/Destroy/basic
descriptor/extended descriptor; subject is H. Init uses requested size, name as
input, resulting mapping as result, and rounded H size as auxiliary. Descriptors
record output address and size32/40; extended records its supplied error.
Operations4/5/6 are alloc/realloc/free: subject is the handle-slot address, input
is the original allocation, result is the returned pointer (zero for free), and
auxiliary is callback context H (zero for direct exports).

Only the first **16 per-heap allocation/reallocation/free records per process**
are emitted, shared across heaps and direct/callback entry points. Ordinary and
extended descriptor events each have an independent eight-record budget. Native
Ex-new raw `0x40` allocations at the comparison run's return address
`0xa80018662` have a separate eight-record budget, preserving the late global
heap handoff after ordinary allocation traffic. This fixed address is diagnostic
only and does not affect allocation or binding. Default-off
saturating atomic budgets require no allocation or locks. Later records are
intentionally omitted before reaching the Gateway bank, so they do not increase
its attempted/dropped counters. Init/Destroy bypass these small budgets. This bounds
descriptor storms as well as callback traffic; it does not reserve space against
all other Gateway producers. The existing shared
128-record Gateway limit still applies; file version and layout are unchanged.
Producers never inspect descriptor context H. All observations run after host
registry/state locks are released; they never flush or resolve symbols.

Event24 (`stack_failure`) records the first four stack-check entries in the
independent Recovery bank. Each entry has a header `{caller, HLE_frame, 0, guard}`
and eight chunks `{caller, address, chunk1..8, readable_mask, qword[12]}`.
The 768-byte snapshot starts at the compiler's HLE frame, not an assumed native
canary location. Fault-contained reads preserve inaccessible words as unreadable.
This probe neither changes stack-check termination nor writes guest memory.

Event25 (`app_id_result`) selects the first eight calls from ShellCore return address `C+cb3bb9`.
Values0..5 are caller, PID32, output address, returned status, and before/after
readability. Values6..8/9..11 contain three dwords before/after: app-ID output
followed by the adjacent saved canary. The 12-byte reads are fault-contained and
default-off; the API's output remains exactly four bytes.

Event26 (`np_global_result`) records the first eight global allocator getter calls.
Values0..7 are caller, operation (0 ordinary / 1 Ex), persistent descriptor,
global Heap address, mapping size, mapping address, handle, and Ex error metadata.
This records the actual returned descriptor rather than the temporary storage used
to prepare its fields. Publication occurs after releasing the descriptor lock.

`--np-heap-only` checks exact and fallback registrations, two heaps plus the
manager concurrently, native H/descriptor writes, duplicate Init resource
retention, partial mapping failure, post-map rollback, copied slots/descriptors,
Destroy/reinit, bounded realloc and native-header destructor callback re-entry.
The shared arena has no mapping ownership or implicit locks. Heap callbacks use
context H's current +0x18 slot; direct helpers receive the slot address itself.
Duplicate Init intentionally retains old resources; copied old slots remain
usable. Retired opaque handles remain tombstones to prevent handle-address reuse;
orphan mappings are reclaimed by the OS at process exit, not implicitly destroyed.
Concurrent allocation is serialized per heap; callers must serialize lifecycle
and guest layout mutations on the same H, as they must keep guest storage valid.
The `GjdHoPpS+W0` statistics stub remains unverified and is not an implemented
statistics contract.

Event21 (`np_mutex_query`) filters the AppCache `Mutex::IsInit` return address
`0x910ca27c6` exactly. Values0..3 are the actual mutex input, return address,
unchanged HLE result, and fault-contained guest byte at mutex+0x10. Value14 bit256
(`mutex_byte_readable`) distinguishes a readable zero from an unavailable byte.
The host map lock is released before observation; no guest state is written.
This event shares the 128-record gateway budget, skips reads after exhaustion,
and retains schema version1. The guest byte is a snapshot, not the HLE result.

No slots are reused. Atomic release publication permits readers to skip a
suspended writer's hole. Separate banks prevent invoke/poll traffic consuming
lifecycle/recovery capacity. Overflow increments counters. Once both invocation
banks fill, unclassified method0x160008 skipped calls count in the invoke bank;
counters then describe dropped observations, not an exact per-endpoint census.
Snapshot the file after stopping the process for a stable record set. Live reads
can omit in-flight records; no record is made durable against power loss by an
explicit flush. The mapping remains process-lifetime and is readable in Silent
mode without stdio, Log::Flush, linker lookup, or producer allocation.

## Verification

`ipmi_observe_tests` covers concurrent publication with a suspended-writer hole,
overflow without overwrite, independent bank budgets, default-off invalid
pointers, before/after unchanged and changed recovery output, unreadable/partial
page snapshots, independent mapped-file reads, and capture persistence after a
test child process exits. This child is the test executable, not the emulator.
Mutex-query checks cover exact caller filtering, both results, guest-byte
preservation, inaccessible/overflow addresses, and bounded overflow. The full-HLE
`virtual_memory_allocation_tests --np-mutex-only` mode checks construction,
initialization, destruction, reinitialization, and reconstruction at the same address.
`--np-allocator-only` exercises versioned manager registrations, copied descriptor
callbacks, real kernel map/unmap notifications, bounded supplied arenas, exhaustion,
reallocation preservation, lifecycle rollback/reinit, and opt-in mutex destructor
dispatch through a recording allocator. Runtime native-export binding tests are
separate from these HLE/backend tests.
Zero-size requests reserve a minimum one-byte request (16-byte aligned).
For an owned pointer, realloc(ptr,0) retains the same pointer and ownership while
shrinking to that minimum, matching native minimum-chunk semantics. Native heap
overhead is not reproduced byte-for-byte. CreateBuffer validates complete host
read/write coverage outside the NP lock before publication; invalid backing returns
HeapFailure as an HLE backend failure, not a claimed native kernel errno. Callers
must keep supplied storage mapped and writable for the arena's lifetime.
