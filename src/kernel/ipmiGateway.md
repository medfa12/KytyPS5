# Bounded invoke320 completion

The adapter remains isolated from production. CONNECT/disconnect policies and
unsupported timeout, pollstate, authentication and teardown cases are unchanged.

For an available synchronous response, invoke320 now performs:

1. Reserve the actual response and check count/lengths against saved caller buffers.
2. Copy DATA bytes to the saved output pointers.
3. Consume the response packet.
4. Copy the four-byte method result.
5. Copy the **whole saved BufferInfo array**, with updated `written` fields. For
   GetStatus this is24 bytes, restoring the original pointer and capacity too.
6. Release the caller slot.
7. Copy the final four-byte operation result in the gateway epilogue.

Copy callbacks may fail after writing a prefix. No guest bytes are rolled back.

| Failure | Packet | Caller | Earlier side effects |
|---|---|---|---|
| DATA | unreserved, not consumed | released | possible partial DATA |
| method result | consumed | released | DATA committed |
| BufferInfo copy | consumed | released | DATA and method committed |
| final operation result | consumed | already released | DATA, method and descriptors committed |

None of these guest-copy failures retains an adapter entry for completed-response
replay. Within this bounded contract, only the existing interrupted invoke wait
resumes its original request/snapshots. Host-allocation exception rollback remains
excluded as described in `ipmi.md`. Retrying
a terminal invocation is a new operation; an unread old packet is explicitly
excluded pending the native audit described in `ipmi.md`.

Server reply232 copies descriptors and producer data before publication. Source
copy/budget/allocation failures leave its receiver binding intact. Publication
and receiver release happen before final server opResult copyout; failure of that
last copy cannot roll either back. A repeated reply cannot publish the old response
again after the receiver has been released.

The server no longer rejects publication merely because its response count/lengths
exceed a particular caller's BufferInfo. Existing session/host resource limits
remain. Client validation uses the saved full BufferInfo snapshot, never a guessed
length or a concurrently changed pointer. Malformed response shapes return host
`Unsupported` before DATA and preserve the unread packet; native error mapping
and subsequent-packet handling are not claimed.

See `tests/ipmi/gateway_tests.cpp` for independent fault/order fixtures, including
zero-output parity, partial DATA failure, descriptor mutation, source-copy failure
and final server result failure. `ipmi_state_tests.cpp` exercises both legal orders
of caller and server-worker release. All three standalone IPC CTests passed on
Windows after these additions.

## Isolated diagnostic op0x10

The six arguments are `(op32, selector32, opResult*, descriptor*, size64, tag64)`.
This path ignores **all** tags; other operations retain their existing tag policy.
It is observation only, with no IPC ownership, worker registration, cancellation,
Pending admission, restart entry or completed-result cache. Native reference:
kernel multiplexer `ffffffff809ecd40`, diagnostic handler `ffffffff80e122a0`,
common trace writer `ffffffff80e0f9d0`, final operation-zero store `80e12485`.

Ordering and failure contract (sizes/offsets hexadecimal):

- `size > 40`: the existing outer path copies four-byte `800e0001` to opResult;
  success if that copy succeeds. No descriptor read, even with a null descriptor.
- Otherwise null descriptor or size other than `40` returns errno `16`, without
  reading the descriptor or writing opResult.
- Copy **all 40 bytes**, including unused tail, before any diagnostic observation.
  Copyin failure submits nothing and does not write opResult.
- Snapshot trusted caller identity/names; perform the common server-name lookup;
  decode the full32 selector; observe unknown selectors; optionally submit trace.
- Finally copy four-byte **zero** to opResult. No early output-address probe. A
  partial final-copy failure preserves both the written prefix and any submitted
  observation. Repeating the call rereads the descriptor and submits anew.

| Full32 selector | handle16 | A32 | B32 | P64 |
| --- | --- | --- | --- | --- |
| `1f00` | descriptor h low16 | `+10` | 0 | `+08` |
| `1f01`, `1f03`, `1f05` | h low16 | `+08` | 0 | 0 |
| `1f02` | `ffff` | `+24` | 0 | 0 |
| `1f04` | h low16 | `+08` | `+18` | `+10` |
| unknown, including `00011f00` | `ffff` | 0 | 0 | `ffffffffffffffff` |

`h` is the signed32 descriptor field at `+00`. `1f02` overrides the common lookup
name using bounded `strncpy(...,25)` from inline bytes at `+08`: a NUL pads the
remaining destination bytes with zeros; 25 non-NUL bytes are retained. Numeric P
values (including the `1f00` workspace) are never dereferenced. Negative logged
results remain raw A32 bits and never become operation results.

`Core::TraceServerName` performs read-only, owner-independent lookup in the live
server map, corresponding to native kind `8001`. Missing, retired, wrong-kind and
nonmatching full handles return a zero name. `h == -1` skips lookup. The host Core
allocates non-reused monotonic handles shared across object kinds, capped at
INT32_MAX; exact map membership therefore supplies its lifetime/generation check.
It does not implement native packed handle/generation encoding or permit low16
alias lookup. The copied name leaves the Core lock before any collector callback.

### Collector boundary

`DiagnosticEvent` carries full selector input, selector16, handle16, caller PID/TID,
process/thread name snapshots, server-name bytes and A/B/P. Optional
`diagnostic_caller` supplies one trusted identity/name snapshot. Without it, the
existing injected `identity()` supplies PID/TID and names remain unknown/zero.
Process/thread arrays of32 bytes are **host collector bounds**, not a claim about
native packed ring layout; no name is read from a guest-supplied pointer.

Primary submission requires `diagnostic_enabled()` and `diagnostic_submit` and
is skipped when selector16 has its sign bit set. Missing hooks or disabled tracing
still permit syscall success. Optional `diagnostic_unknown` observes unknown
full32 selectors independently of that primary gate. All callbacks must be bounded
and nonthrowing; they execute outside Core/gateway locks and must copy events they
retain. The collector owns ring capacity, clock and timestamp policy (native uses
microsecond metadata). This adapter implements neither the native kernel ring nor
a fabricated timestamp/backend, and collector enable does not alter IPC state.

`tests/ipmi/diagnostic_tests.cpp` is a separate CTest target covering precedence,
tail faults, partial final faults, fresh repeat submission, numeric workspace,
cross-process/stale/wrong-kind lookup, bounded non-NUL names, selector decoding,
ignored tags, disabled tracing, reentrant collection and a suspended transport
transaction surviving diagnostics. All four standalone IPC CTests passed on
Windows after integration, as did the emulator build. Independent contract review
found no blocking defect in this bounded diagnostic path.

### Connect flag clarification

Native server config `+20` permits a restricted caller authority class; it is not
an unconditional rejection flag. Config `+1c` permits cross-process callers lacking
capability62; `+18` permits multiple synchronous callers. The existing gateway
continues to exclude nonzero flags as an isolated-model boundary. No production
identity, credentials or expanded connect authorization is introduced here.
