# Isolated synchronous reply ownership

This host model has no production factory, syscall registration or guest callback.
The one-output lifecycle follows the retained 13.60 audit in
`docs/rnps-1360-log.md` section99: server publication `80c9d68b`, subsequent
receiver release `80c9d7d5..80c9d81c`; client DATA `80c9c70f`, consumption
`80c9c73c`, method result `80c9c759`, whole BufferInfo array `80c9caec`, and
caller release `80c9c94a`. Read unreservation `805b0c70` differs from packet
unlink/consumption `805b0d70`.

## Host API

- `PublishReply` publishes response availability without releasing either side.
  Count is bounded by10 and output bytes by the existing negotiated session
  output budget. Individual caller count/capacities are not publication checks.
- `ReleaseReplyReceiver` frees the receiving worker's dispatch binding separately.
  `Reply` composes publication and receiver release using separate lock scopes.
- `BeginInvokeRead` reserves an available response for the original PID/TID/ticket.
- `EndInvokeRead(..., false)` unreserves without consuming; `true` consumes.
- `ReleaseInvokeCaller` releases the caller slot independently of receiver release.
  It rejects a still-reserved read. Consumed responses leave transport ownership;
  unread responses remain owned by the client, not by an adapter retry cache.
- `InspectInvoke` observes an occupied caller slot. `Completion::InspectReply`
  observes publication/reservation/consumption and both release states.
  `InspectReplyPacket` snapshots retained zero/one-output packet bytes only.

The response packet for a24-byte GetStatus output is exactly44 bytes:
`u32{0x42,0x2c,methodResult,1,0x18}` followed by the24 producer bytes.
A zero-output packet is16 bytes: `u32{0x42,0x10,methodResult,0}`.
Multi-output replies retain typed results; their raw packet encoding is deliberately
not asserted by this bounded change. `Poll` remains host observation of availability
and owned result bytes, never permission to replay a terminal guest invocation.

Core state and ticket locks protect short transitions; no guest copies, callbacks
or waits run while either is held. Client completion may finish before the server
releases its receiver. Outstanding receiver accounting prevents normal disconnect
from retiring that still-owned dispatch.

## Explicit limits

An unread response after terminal DATA/shape failure remains observable and keeps
its actual producer bytes. A subsequent invocation on that client returns the
host model's `Unsupported` rather than consuming, discarding or replaying it.
Native routing/reassignment of this leftover packet needs further audit. Existing
normal disconnect retirement clears retained response state; timeout/forced-exit
semantics are not broadened. Client malformed-response error mapping remains a
host exclusion, not an asserted native errno or synthesized method result.

Host-allocation exception rollback remains outside this milestone. In particular,
`Poll()` copies response bytes; allocation failure before the gateway's terminal
completion guard can retain a pending entry. Production hookup must close that
boundary rather than treating it as a native interrupted wait.

Validation: the state, wire and gateway standalone CTests passed on Windows;
independent native-contract review found no blocking zero/one-output mismatch.
