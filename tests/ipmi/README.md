# Bounded native-IPMI state core

## Isolated native gateway adapter

`src/kernel/ipmiGateway.{h,cpp}` is a host-testable six-argument adapter around
the Core and wire codec. It has **no production provider, export hook or enable
flag**. Construct it with a Core and callbacks for guest copyin/copyout, guest
PID/TID, guest errno, cooperative scheduling, capability62 and explicit host
client-registration/connect authorization policies. It never calls a native handler. The scheduler may
reenter the adapter as another guest identity; identity is captured once per call
and no Core lock is held across callbacks. Callbacks must not throw; a failed
copy can have written a prefix, which the adapter does not roll back.

`Call(op, kid, opResult, desc, descSize, tag)` uses hexadecimal operation numbers:

| Operations | Implemented isolated path |
|---|---|
| 0 / 2 | Server/client registration, desc0x18: native object, name pointer, wire config pointer |
| 1 / 3 | Explicit server/client destroy, subject to Core liveness checks |
| 4 | Session creation, kid=-1, desc0x10: native object, pointer to `{u64 size=0x10,u64 userArg}` |
| 466 / 467 / 468 | Typed server/client/session argument query, 8-byte output |
| 46a / 46b | Typed server/client name query, 25-byte output |
| 302 | Live session peer PID query, 4-byte output |
| 400 | CONNECT with copied opaque argument, null timeout pointer only |
| 201 | Receive into supplied workspace and ReceiveInfo; null pollstate pointer only |
| 212 | Accept explicit client kid, bound to receiving PID **and** TID |
| 320 / 232 | Bounded sync invoke / reply using DataInfo16 and BufferInfo24 |
| 310 / 5 / 222 | Disconnect, session close, then acknowledgment using retained client binding |
| 202 | Queued wake, descriptor `{u64 1,u64 0,u64 0}` |

The local 0x40-byte descriptor is zero-filled, then exactly the supplied size is
copied. Short descriptors are not universally rejected; omitted fields remain
zero. Sizes above0x40 skip descriptor copy and return success after writing
opResult=`0x800e0001`. Ordinary failures return -1, set callback guest errno and
leave opResult untouched. Success writes the LE int32 opResult last; registration
and creation return a nonnegative allocated kid there. Query kind/owner/liveness
failures return errno2; an existing session without a connected peer returns3.
Output spans overlapping opResult are excluded with Unsupported to preserve that
contract. Other output buffers can have earlier successful writes if a later copy
fails; there is no invented multi-buffer transaction or guest-side rollback.

Verified tags are accepted as opaque correlation values: the native default
`0xdeadbadecafebeaf`, and the zero-extended method ID for invoke320 (GetStatus uses
`0x160008`). Tag0 remains available for host fixtures. Other tags are explicitly
Unsupported pending evidence. Tags never grant identity/capability. The oversized
descriptor branch still precedes tag validation. Normal gateway fixtures now use
the native default and method-ID tags rather than relying exclusively on tag0.

Server wire0x38 is decoded field by field: size00, argument limit08, transfer
limit10, multi18, crossNoCap1c, restricted20, userArg28, handler30. Client wire0x150
decodes size00, syncCount08, limits10/18, asyncCount20, limits28/30, queueCount38,
queueCaps[32]40, eventCount140, flag144 and userArg148. Flag144=0 selects effective
syncCount1; flag1 uses the supplied count. Queue capacities are already rounded
and are preserved. Async configuration is represented even though async operations
are unsupported. Native object, handler, registration userArg, session userArg
and copied CONNECT bytes occupy distinct fields; argument queries never substitute
an object or handler pointer.

CONNECT and SYNC payloads are emitted by the codec. The host constructor explicitly
selects SYNC0x41 or inline0x8041 (default inline); deriving this selection from
native runtime state is still an integration task. Inline workspace is only
reserved storage. Reply232 publishes the producer's actual `written` bytes and
method result, then releases the receiver independently. Invoke320 copies DATA,
consumes the packet, copies methodResult, copies the entire saved BufferInfo array
with actual lengths, releases its caller slot, then copies final opResult.
No phase/status is synthesized. Disconnect delivers type3, the client kid and
handler, with exactly eight payload bytes containing the supplied native session.

Receive has a host reservation across copies. Capacity/payload/ReceiveInfo-copy
failure releases it without consuming or binding the queue entry. Successful
copies bind the explicit dispatch to the receiver, then consume it. This is a
conservative isolated transaction, **not a claim about native receive fault
precedence**. No session is created and no request completed by receiving alone.

### Retained cooperative wait transactions

Connect400, invoke320 and disconnect310 reserve a bounded adapter entry before
Core submission, keyed by **guest PID/TID/op/kid**. Default capacity is64 and a
constructor budget can lower it; full capacity returns host NoMemory before any
enqueue. At most one wait operation is retained per guest identity. An entry owns
the original ticket, zero-extended descriptor, invoke output BufferInfo snapshot,
and scalar opResult/descriptor addresses, size and tag. Core owns copied input and
CONNECT bytes. No identity is derived from a tag or a host thread ID.

When the cooperative callback returns interruption, the entry remains. Retrying
the same scalar invocation resumes its ticket without another Core Connect/Invoke/
Disconnect call and without rereading guest descriptor/input/output metadata.
Mutating guest descriptor bytes cannot redirect completion. A different scalar
invocation on that suspended identity, or a concurrent/reentrant same-key retry,
returns Busy; other guest identities remain independent. Non-wait operations
continue to perform their normal ownership checks. No pending-table lock spans
Core calls, guest copies, errno callbacks or the cooperative pump.

Completion is checked before and after the pump, so completion wins over a
simultaneous signal. **Invoke320 retains only an interrupted wait.** Once terminal
completion begins, DATA failure unreserves without consumption but releases the
caller; method/whole-descriptor/final-opResult failures occur after consumption and
also release the caller. Every terminal path retires its adapter entry, including
final-opResult copy failure. There is no completed-response replay contract.
CONNECT/disconnect retain their prior isolated retry behavior, which is not widened
or asserted native-exact by this reply fix. No eviction or forced teardown is added.
See `src/kernel/ipmi.md` and `ipmiGateway.md` for packet ownership and known limits.

### Authorization and remaining integration gaps

- The injected capability62 result gates **server registration only**: `Sce` names require it;
  non-`Sce` names with it return errno1. No capability is inferred from PID/name.
- Client registration does not apply this server rule. Its native authorization
  remains unverified: without `authorize_client_registration` it returns
  Unsupported. An explicit host-test allow-policy enables transport fixtures only;
  tests prove client registration never consults the server capability callback.
- The restricted flag permits a native caller class; it is not an unconditional
  rejection flag. Nonzero restricted/multi/crossNoCap branches,
  unknown client flags, and missing connect-policy callbacks are explicitly
  excluded with the Core's `Unsupported` (0x4e). The tests' allow-policy callback
  exercises transport, **not native authorization**. Verified connect authorization
  still needs to replace this host-only policy before any production hookup.
- Nonnull connect timeout/pollstate pointers, unverified gateway tags, unsupported
  modes, async/event/queue gateway operations, rejected acceptance,
  accepted-connect rollback, timeout races, forced teardown/520 and close outside
  normal disconnect are not implemented. Unknown operations return Unsupported.
- Core/codec resource bounds are host policy. Budget/shape exclusions return
  Unsupported; allocation failure and too-small receive storage return NoMemory.
  Those exclusions are **not asserted native-exact error mappings**. Copy callback
  errors and verified query/oversized-descriptor behavior are preserved separately.
- Interrupted wait resumption is implemented as described above. One-output and
  zero-output client copy/consume/release ordering follows the native audit.
  Native handling of an unread packet after terminal DATA failure, malformed
  reply errors, allocation-failure rollback, and abandoned-transaction teardown
  still need evidence. No rejected-connect or teardown recovery is
  fabricated. An adapter/Core must outlive every active call and callback.
- Core limits remain one unfinished dispatch per receiving PID/TID and bounded
  synchronous slots; admission waits, full service authentication, scheduler exit
  handling and production guest-memory integration remain review gates.

Build only isolated tests (after checking for other active builds):

```powershell
cmake -S tests/ipmi -B "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build"
cmake --build "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build" --target ipmi_gateway_tests ipmi_state_tests ipmi_wire_tests ipmi_diagnostic_tests -j2
ctest --test-dir "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build" --output-on-failure
```

Gateway fixtures use disjoint object/handler/argument values, two client processes
and two receiver threads, real wire configs, source-buffer mutation after CONNECT
copyin, wrong-client accept with no mutation, query errno/opResult sentinels,
oversized/short descriptors, copy faults, receive retry, three distinct producer
GetStatus replies in both SYNC layouts, and disconnect/close/ack ordering.
Review fixtures additionally interrupt and retry all three ticketed operations,
assert one receive/reply, mutate descriptors between interrupted retries, check
whole-descriptor restoration and terminal copy-fault non-replay, verify pending admission/reuse,
reject same-identity conflicting/reentrant waits, and exclude unverified client
authorization separately from the server Sce/cap62 checks.

Diagnostic op10 has separate fixtures for selector decoding, complete 64-byte
copyin, bounded inline-name padding, owner-independent server-name lookup,
disabled tracing, reentry and irreversible trace effects before final copyout.
See `src/kernel/ipmiGateway.md` for the audited contract and collector limits.

## Standalone wire codec

`src/kernel/ipmiWire.{h,cpp}` encodes the verified 13.60 DataInfo (16 bytes),
BufferInfo (24), invoke320 descriptor (0x30), reply232 descriptor (0x18),
ReceiveInfo (24), SYNC payloads (0x41/0x8041) and CONNECT payload (type1).
It is **not a production gateway**; the isolated adapter above uses it with the
Core. It has no factories, observer or `ipmimgr_call` hookup. Pointer/session/handler/PID values come unchanged
from the caller; descriptor addresses are numeric and never dereferenced.
`ByteView` refers only to already-copied, readable host bytes. The isolated adapter
supplies these through injected copy callbacks; production memory integration
and dispatch scheduling remain separate work.

SYNC input lengths and bytes are packed without alignment padding. Output
capacities are u32. Inline0x8041 appends exactly their summed capacity as zeroed
host workspace, **not a completed reply or recovery status**. A zero-input,
one-output/cap24 GetStatus payload is 28 bytes plus 24 inline bytes = 52 bytes.
CONNECT uses the supplied config fields at their verified offsets, fixed space
for 32 u64 queue sizes, memorySize=0 at0x148, u32 arg length at0x150 and argument
bytes at0x154. Unused/reserved descriptor bytes are zeroed.

`Limits` contains explicit configurable **host resource budgets**, not inferred
firmware validation rules. Wire u32 lengths/capacities and the 32-queue layout
remain mandatory regardless of those budgets. The codec checks counts, aggregate
sizes and overflow before allocating/copying. It publishes by swapping a complete
temporary byte vector; all failures leave the output unchanged. `Error` is a
host codec result, not guest errno or a native method result. BufferInfo encoding
rejects written>capacity as a host consistency check. No process/authentication
or phase semantics are imposed.

Build this independent target without linking the emulator:

```powershell
cmake -S tests/ipmi -B "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build"
cmake --build "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build" --target ipmi_wire_tests -j2
ctest --test-dir "$env:LOCALAPPDATA/Temp/opencode/ipmi-state-build" -R '^ipmi_wire_tests$' --output-on-failure
```

The wire tests cover a literal 52-byte GetStatus frame, unaligned input and
capacity offsets, negative PID/kid/method-result preservation, descriptor and
CONNECT offsets, the 32nd queue boundary, checked overflow, aggregate budgets,
poison-pointer rejection before copying, and unchanged output on errors.

`src/kernel/ipmi.{h,cpp}` is an independent host-side model, not a syscall
adapter. It has no global instance, export registration, guest callback, native
factory change, or enable flag. Production code has no caller of this API.
`libSysProc.cpp` and the existing gateway behavior are untouched.

## Build and test

From the repository root, with a C++17 toolchain available:

```powershell
cmake -S tests/ipmi -B "$env:TEMP\opencode\ipmi-state-build"
cmake --build "$env:TEMP\opencode\ipmi-state-build" --config Debug --target ipmi_state_tests
ctest --test-dir "$env:TEMP\opencode\ipmi-state-build" -C Debug --output-on-failure
```

This target compiles only the core and its tests, links the platform thread
library, and does not load firmware or run the emulator. Tests use explicit
checks even with NDEBUG. After flexarena2 completed, the standalone target built
with GNU 13.2.0 on Windows and passed CTest (1/1 target).
That result is the coordinator-verified baseline committed as `d0673ab`.
The queue additions also built and passed CTest (1/1 target). Review found that
sends during disconnect could strand session close; sends are now rejected in
that state, and the reconnect test verifies rejection and completed cleanup.
CTest passed again after that correction.

## Implemented boundary

- Separate server/client/session registries; process ownership, fixed 25-byte
  name output; explicit bounded configs. All core handles are host-only and
  monotonically assigned. Client name currently identifies its target server.
- Queued connect dispatch, guest PID/TID-bound session creation and successful
  acceptance. Native session/argument/handler addresses are opaque integers.
- Configured synchronous slots, copied inputs and bounded output capacities,
  receiver-TID-bound replies; separate syscall/operation/method results. Reply
  publication retains the caller slot until explicit client terminal completion.
- Real queued wake entries (including wake-before-receive).
- Normal disconnect refuses occupied slots/event waits; repeated disconnect
  from the same guest TID returns the same ticket. Close removes session state
  but retains disconnect binding until acknowledgment. Explicit destroy refuses
  live state. Completed tickets retain owned results after registry retirement.
- Persistent event flags, AND/OR, full-pattern snapshot, clear-all/clear-mask,
  cancellation with old-pattern snapshots and waiter count, timeout expiry.
- Per-connection message queues for host equivalents of 252/253: opaque payloads,
  eight-byte framing, byte capacity accounting, nonblocking receive, FIFO send
  admission and finite/indefinite send waits. Queue capacities in host `Config`
  explicitly include framing and are not a native-wire reinterpretation.
- Staged queue receives bind channel ownership to guest PID/TID. Payload-copy
  abort preserves a frame; successful payload copy consumes it before length
  copy. Short buffers preserve the frame without creating a receive lease.
- Channel primitives support tag-scoped writer cancellation and deferred full
  flush while a receiver owns the channel. They are internal APIs, not guest
  syscalls and not substitutes for full 520/process-exit state transitions.
- Pending operations retain their completion and connection even if callers
  drop their ticket. Completion publication breaks the lifetime cycle. Host Core
  destruction resolves remaining tickets as Unsupported; it does not emulate
  successful process exit. Destruction must not race calls into Core.
- All transitions serialize on one state mutex. Ticket waits have a separate
  mutex; `Core::Wait` releases all locks before invoking the cooperative signal
  pump. Pump interruption leaves the original operation intact and resumable.
  The pump must capture the current guest execution identity, never substitute
  a host thread ID. Core must outlive `Wait`.

## Deliberately not a drop-in kernel ABI

The following are rejected or inaccessible rather than guessed:

- Async methods (configuration counts/limits are retained, operations unsupported). Message queues are
  now supported, but native UserService still cannot be routed here until the
  wire adapter, registration mapping and remaining lifecycle gates are complete.
- Rejected acceptance, connection timeouts/accepted rollback, 520 and forced
  process/server/client teardown, and session close outside normal disconnect.
- Synchronous invocation slot exhaustion returns WouldBlock before allocating a
  request. Queue send admission waits/cancellation are implemented; synchronous
  channel admission waits and general shared-server channel teardown are not.
- Server destruction with an attached pending connect returns Unsupported;
  with a live session it returns Busy.
- One unfinished dispatch per guest PID/TID. Nested Receive returns Busy.

`Receive` is a nonblocking state transition, not firmware receive201. A future
adapter must arrange cooperative waiting/retry without inventing an empty
successful receive. `Wait` is a host wait helper, not libkernel errno handling.
Event time advances through explicit `ExpireEvents(now)` calls (also pumped by
`Wait`); deterministic tests inject a clock. An adapter scheduler must service
expiry even when using only Poll; no background timer thread is created here.
Remaining timeout-us writeback, fault precedence and zero-time edge cases must
be checked in that adapter for event flags. Queue timeouts expose an optional
`remaining_us` result; pointer-copy validation/precedence stays in the adapter.
Host config bounds/names and registration errors
are API policies, not asserted firmware registration wire contracts.

## Integration gates and coordinator factory proposal

Before adding the Hk7iHmGxB18 hook:

1. Decode registration0/2 payloads and handle-allocation/result semantics;
   map native config/name fields to this model, not by reinterpreting structs.
2. Supply guest PID/TID and checked guest-memory copyin/copyout. Preserve syscall
   errno versus operation result versus method result, including partial-copy
   and post-consumption errors. Do not directly cast `Result` to syscall return.
3. Integrate the tested startup diagnostic10 boundary with trusted caller metadata
   and a bounded collector, plus exact server/client name-query wire validation.
4. Adapt the queue252/253 state API to checked guest memory, complete required
   teardown and synchronous channel admission waiting before enabling any route.
5. Integrate cooperative wait/receive and guest-thread exit policy without
   linker lookup, log flushing, or guest callback execution inside core locks.

Coordinator-owned routing changes must select native Config, server, client and
session factories together for the service graph. Never pass this core's host
objects to guest native vtables or mix HLE-created and native-created objects.
Registered/foreground reads must use the same native UserService authority as
creation and persistence; separate JSON-backed reads would diverge.

## Tests

Coverage includes concurrent independent acceptance with matching numeric TIDs
in different processes; reverse-order reply completion; wrong PID/TID/session;
server-session output-budget rejection before publication; client count/capacity
validation after publication; busy destroy/disconnect; close-before-ack ownership;
completion lifetime after retirement; both caller/receiver release orders;
partial DATA faults with packet retention; method/descriptor/final-result faults;
source-copy failure versus irreversible server publication; set-before-wait and clear ordering;
set/cancel linearization on two host threads; deterministic timeout/set ordering;
cooperative pump reentrancy; interrupted disconnect resumption; queued wakes;
unsupported operations leaving live objects intact; host abandonment errors.

Queue tests additionally cover framing capacity (including the maximum payload),
empty/full polls, short-buffer retry, payload/length copy-fault ordering, receive
lease PID/TID ownership, FIFO head-of-line admission, timeout versus capacity,
remaining timeout values, set-up validation, scoped cancellation versus flush,
deferred flush during ownership, cancellation versus consumption on two host
threads, dropped sender tickets, acceptance-time sending and client isolation.

## Queue API contract and evidence

Kernel evidence is from the already permitted local 13.60 image. Addresses below
omit `ffffffff`; host Config layouts and handles remain independent of wire ABI.

- 252 wrapper `80d53270` -> `80d52990`: client-kind poll. Mandatory guest output
  buffer and uint64 length pointer are the adapter's responsibility. Host
  `PollQueue` returns `outputs[0]` plus `received_size` on success only. Empty is
  errno23. Capacity >= UINT64_MAX-7 is errno59. Too-small capacity is errno0c
  and does not consume or publish a required-size output (`80d52d2f-83`).
- Frame `{u32 type=0x80,u32 total_size}` followed by payload. `SendQueue` writes
  and `BeginQueueReceive` checks this framing. Capacity includes all eight header
  bytes; tests prove an eight-byte payload occupies sixteen channel bytes.
- `BeginQueueReceive`/`EndQueueReceive` are host transactions, not syscall names.
  Payload copy failure aborts (0c maps to59); successful copy commits consumption
  via the modeled `805b0d70` behavior before length output (`80d52e03-eb8`).
  `EndQueueReceive`'s optional length-error argument simulates that later fault
  for tests. A real adapter calls End after payload copy, then performs the
  guest length copy and reports any fault: it must NOT copy the length first.
  Guest copies execute outside locks. Abandoning a lease is not cancellation;
  the adapter must finish/abort it on every path. Active leases pin connections.
- 253 wrapper `80d532e0` -> `80d533e0`: session-kind send, nonzero payload below
  0x7fff9 bytes, optional uint32 microsecond timeout. Null waits indefinitely;
  zero polls (`805b0ea0`). A frame exceeding channel capacity fails with0c.
  Pending sends own their copied bytes and ticket even when callers drop theirs.
  Host copying is already complete on entry: guest send-copy faults/reservation
  abort require adapter work and are not simulated as successful sends.
- Channel wait `805afc60` propagates sleep timeout errno23, distinct from event
  timeout errno3c, and computes nonnegative remaining microseconds at
  `805b0218-260`. Queue APIs advance deadlines at their supplied monotonic time;
  `ExpireQueues`/`Wait` service otherwise idle queues. Expiry linearizes against
  cancellation/commit under the state lock. Deadline expiry removes the sender,
  never publishes its payload. Results distinguish syscall failure from an
  absent operation result; enqueueing a pending host operation is not guest
  syscall completion (inspect its ticket).
- Send waiters are tagged with the session kid: `80d53790` passes the handle
  stored at `rbp-90` as r9 to `805afa40`; channel wait saves it at waiter+28
  (`805afeb3`). `CancelQueueWaiters` models `805b1670`: remove only matching
  tags; retain frames and other tags/queues. Calling it with a client kid does
  not cancel session-tagged sends. Only the owning server process may invoke
  these host channel primitives. Reader252 is nonblocking: no sleeping receive
  waiters are invented. Kernel raw cancellation55 remains55 for blocked sends.
- `FlushQueue` models `805b1340/1430`: defer while an owner is active; on release,
  purge queued bytes and cancel writers before admitting another sender. A
  receive already holding the frame can finish its copy/commit; subsequent data
  and pending sends are flushed. Cancellation is not a permanent closed flag.

Normal disconnect refuses active queue leases and, conservatively, pending
senders. Handling disconnect/close concurrently with server queue-send waiters
through native teardown remains outside this bounded normal lifecycle. Queued
messages alone do not block disconnect; normal retirement purges them. No code
synthesizes user IDs, user creation/login, persistence or notifications. This
transport merely carries bytes supplied by the native server when routing is
eventually integrated with the shared native UserService authority.
