# 13.60 IPMI async collection contract

Static evidence only; no build, test, emulator or PSN execution in this workstream.
Original kernel SHA256 `7b55225db09dc39ce46ec4123905d22e061eca8a35ca08d6ac4873f15216d379`;
libSceIpmi `0ec3d75d640712b344ad20558985475bd68b7a7e8dc858bebc60ab9187ab819f`.
Kernel addresses below omit `ffffffff`.

## Parent LNC integration

* Start vtable+`60` (`a90`) uses op`241`; collect vtable+`68` (`b50`)
  uses op`243`. The ticket is an output **u32** at start and a scalar u32
  at collect. It is scoped to the client connection, not the calling thread.
* Start allocates the first free configured async slot (`804d0cbd..0d03`).
  No free slot (including zero configured slots) returns errno`10` (EBUSY).
* `804d102f..108c` takes the connection counter at `+448`, increments it,
  resets the next value to zero when it reaches `ffff`, and stores the old
  counter in the slot table at `+3f8`. Ticket zero is valid. Do not use a
  pointer, thread ID, boolean, or permanently increasing host handle as its ABI.
* Collect searches those occupied slots (`804d2b3f..2ba7`). An unknown ticket
  returns errno`16` (EINVAL), hence libSceIpmi transport `80020016`.
* A known ticket whose reply is pending returns errno`23` (EWOULDBLOCK), hence
  transport `80020023`. Collection calls `805b1110`; empty unowned channel
  branches `805b11d1 -> 805b126a`. This is a poll of the reply, **not** successful
  service completion and not an unknown-ticket error. Contention with a live
  channel copy owner can enter the channel wait helper; do not promise that
  every contended collection is wait-free.
* Successful collection validates packet type `44`/`54`, **collect method**,
  ticket and output count (`804d2e21..2e59`), consumes the packet
  (`804d2eee..2f03`), copies service result and output descriptors, and only
  then marks the slot `-1` (`804d31bd..31c3`). A repeated successful collection
  is therefore EINVAL until that finite ticket value is reused by a later start.
* Service errors are data: transport success plus the reply's signed i32
  method result. They consume the ticket just like service success. LNC's
  wrapper returns transport error first, otherwise that method result.
* Payload copy/shape failures release the channel reservation without normal
  successful collection (`804d2f78`, `804d30c7`, `804d30f1`, `804d312d`).
  Service-result/descriptor copyout faults happen **after packet consumption**
  but **before slot reset** (`804d2f09..2f3e`, `804d3087..30c2`). Do not cache
  and replay an already-consumed result or promise retry success after EFAULT.
* Collection on a non-connected connection returns errno3 (`804d2bf1`).
  Termination cancels reply channels separately; exact in-flight cancellation
  error precedence is not established by the empty-channel case.

Artifacts: `_TempData/np-lifecycle-re-kernel-async.txt`,
`np-lifecycle-re-kernel-async-routing.txt`, `np-lifecycle-re-async-channel.txt`,
`np-lifecycle-re-ipmi-async.txt`. No LNC files changed here.

Native async dispatch (`libSceIpmi+2948`) calls handler+18 with
`(handler, session, ticket, method, DataInfo*, count)`, ignores its return, then
calls complete-dispatch op444 (`+2955`). Session+38 reply is separately
`(session, method, ticket, serviceResult, BufferInfo*, count)`. ShellCore's
unsupported async-handler return is not itself a completed ticket response.
