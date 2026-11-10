# Video prefetch and MediaKeys admission

## Native reference (13.60, independently checked in IDA)

| Image | SHA256 |
|---|---|
|libSceEsvm.sprx|31593167e10813bdc195fb099da1da723c36818c063c35de6f6a086962d2e3f2|
|libSceEsvmEngine.sprx|95df64b49dedd751913b423106ff547ed6b56d81b356d3d27025e3cfa90f2b9b|

All addresses below are RVAs. Host baseline is `fd4ae606` (integrated as
`0f63b337`); the keys, readiness and refill implementations match at these commits.

### Paused video input is independent of full decoded output

Engine `0x2a000` first rejects outstanding credit (`stream+0x5c`) or input EOS
(`+0x5d`). Its video branch `0x2a04e..0x2a060` checks the sum of count fields
`+0x50/+0x54` against 60 and free-input-space field `+0x58` against 9 MiB.
It does not require the host's four decoded slots to be empty. Accepted work
releases outstanding credit through `0x2a160` and reevaluates the predicate.
Regular engine setup `0x1a81d` supplies `0x1200000` (18 MiB) to the input pool
configuration. See the native readiness note for the pool construction/observer.

The host previously combined a four-frame output veto with a 9 MiB encoded
queue and a 9 MiB minimum free admission window. A paused source could therefore
stop requesting input at four decoded frames, below readiness threshold 12.
Removing only the output veto is insufficient: even one queued encoded byte
then leaves less than a complete admission window.

The source now uses a bounded 18 MiB encoded queue and permits video credit
independently of output saturation. The decoder enforces that aggregate bound
transactionally; per-packet, packet-count, output and held-frame limits remain.
Audio retains its independent 1 MiB bound and output-pressure gate. EOS, failure
and pending codec configuration still suppress credit. Polls cannot rearm a
claimed ticket; accepted copied input does.

Readiness still requires real decoded/buffered work and the native threshold.
No allocator notification, timestamp, EOS, or decoded output is fabricated.
The host byte budget does not claim to reproduce native allocator internals.

Tests submit one real VP9/HEVC AU per granted credit while paused, obtain
readiness only after sufficient buffered work, then retrieve all sixteen frames
with exact PTS. Poll and EOS checks reject duplicate callbacks. Byte-bound tests
own two 9 MiB packets, reject further input, and verify Flush releases capacity.
The pre-change production source fails the new small-batch refill assertion.

### Public session error 0x8a790004 is before backend session creation

Esvm wrapper `0xa470` enters `0x1acb0`. At `0x1acf6` it calls registry lookup
`0x1a1a0`; lookup failure returns **0x8a790004** at `0x1ad02..0x1ad09`.
Backend session construction at `0x1ad51` has not yet run on this branch.
The numeric handle passed by an application cannot be inferred from this error.

Production `EsvmRequestKeySystemAccess` currently completes with `0x8179000a`
(unsupported provider) and no Access handle. Recognizing the key-system string
does not create MediaKeys. Public-message fixture admission is test-only.
Consequently a later invalid MediaKeys lookup is not evidence of a session
callback ABI failure or of a license request reaching a server.

A legitimate protected route requires actual provider admission, MediaKeys and
license/decrypt sessions. Engine PlayReady access `0x40560` calls lazy initializer
`0x437e0` at `0x40595`; initialization invokes PlayReady4 at `0x438f6`, before
access creation at `0x40615`. The recovered native route also requires the
VideoCore/citron callback chain and PlayReady4 TEE context/TA session, described in
`protected-media-dispatch-tee-contract-20261108.md`. Merely loading the libraries
or returning a successful session handle does not implement those dependencies.

Prefetch repair addresses an independent preplay stall. It cannot establish
protected episode playback, resolve provider rejection, or attribute a displayed
post-ad error to a particular title/source without further correlation.

## Parent integration verification

Integrated as `5ee6a153` on `ps5-vsh`. The production emulator, decoder tests and
full-runtime test target rebuilt successfully with VS18 and two build jobs.
The decoder suite and production source suite passed, with source tracing both
disabled and enabled. The readiness trace checker also passed its repeated-HEVC
controls: generation preservation, real output synchronization, bounded trace
records, and no timestamp/EOS readiness shortcut.

This is component/integration verification. No live Apple TV retry with this
prefetch change has yet established preplay recovery or protected playback.

The user tested the verified production executable with existing Apple TV stores
and normal audio. Playback still fails. Starting again at zero feels more stable
to the user than the previous consistent refusal, but this is a qualitative
improvement, not confirmation of episode playback or resume recovery. The run's
logs and application stores are retained. At the user's request, active work
now returns to strict-retail startup and genuine PSN integration.
