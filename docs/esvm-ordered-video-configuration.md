# Ordered AVC/HEVC configuration

Configuration is an input-order boundary, not a presentation Flush. Rejecting
every changed configuration while decoded output remains queued can turn an
otherwise valid native push into a fatal playback error.

## Native contract recovered with IDA (firmware 13.60)

* Engine reader `0x2a1b40` dispatches records in wire order; `0x2a1ca0..0x2a1cab`
  checks the result before advancing. AVC reader `0x2a2170` builds the public
  configuration and calls its installed callback at `0x2a2216`, before the next
  AU record. The reader itself returns zero; the callback separately records
  the setup result. HEVC reader `0x2a2050` has the analogous callback.
* Callback `0x216f0` calls AVC setup `0x2b2b0` at `0x2172e` and stores its result
  at `0x2173a`. Setup prepares parameter-set storage through `0x2b760`, forwards
  configuration through `0x283730` at `0x2b655`, and marks codec **5 (AVC)** on
  success at `0x2b6cc..0x2b6d4`.
* `0x2b6d9` tests the already-started byte `+0xac` and bypasses the initial
  barrier when set. Repeated AVC configuration is not an unconditional Flush.
  The HEVC initial barrier has the same one-shot guard at `0x2b8be`.
* `0x283783` forwards through the current object's virtual slot `+0x60`.
  Its downstream hardware-decoder scheduling is not fully recovered. The
  libavcodec drain below is a host implementation of the established ordering
  requirement, not a claim that firmware uses the same drain mechanism.

## Host implementation

For an already configured AVC/HEVC decoder, own one pending configuration and
its opened codec, plus bounded copied AUs. Allocation, parsing and aggregate
packet limits succeed before mutation or input-copy completion. Subsequent AUs
parse against the latest owned configuration, including its NAL framing.

Record the count of old queued packets. Pump those first, drain delayed old
frames with libavcodec's null-packet/EOF protocol, then activate the new codec.
Keep converted output and presentation generation intact. Old frames use their
codec's sequence, rather than the sequence of newly admitted input. Capacity
pressure may suspend this process with one held codec frame; nothing is dropped.

Input-copy completion means all submitted bytes are owned, not that activation
or presentation finished. Suppress refill while the single configuration slot
is occupied. A second configuration receives normal backpressure until that
slot is free; no unlimited configuration queue or forced success is introduced.

Explicit source/decoder Flush and first-AU discontinuity retain the latest owned
configuration, discard queued work, and advance generation. Reset and decode
failure release pending configuration. Internal drain does not signal input EOS
or reset the source clock, staged frame, mailbox lease, or presentation receipt.

## Verification

Synthetic AVC fixtures change SPS/resolution (64x48 to 96x64) with two B frames.
Component tests check queued-input, decoded-output, held-frame, EOS, repeated
transition, copied-input overwrite, future framing, aggregate limits, refill,
Flush/discontinuity/Reset and decode failure. Exact PTS, sequence, dimensions and
generation are checked for all twelve frames, including delayed old output.

The production source test keeps an old mailbox frame unacknowledged during the
change, overwrites submitted guest memory after completion, then acknowledges
all twelve frames in order through the real source clock/mailbox. Explicit Flush
invalidates receipts. These are host receipts, not GPU scanout claims.

Compiling the pre-change decoder against the current host header fails both the
new component admission assertion and the production source completion check.
Existing VP9/audio/HEVC source regressions remain part of the normal suites.
Live playback beyond the reported stop and seek recovery require integration
validation; independent MediaKeys errors are not addressed by this change.

## Integration checkpoint

Integrated on `ps5-vsh` as `0f63b337`. The production emulator, decoder test
target and full-runtime test target rebuilt successfully with VS18 and two
build jobs. Parent verification passed `media_framework_esvm_decoder` and
`media_framework_esvm_source` with tracing disabled and enabled.

The updated production executable was tested with the existing Apple TV stores
and normal audio. The user reports mid-episode resume still fails. Starting from
zero after marking the episode watched plays the Apple TV logo and an ad, then
shows **Unable to Play**. This does not establish episode playback; the earlier
approximately seven-second observation may describe the logo/preroll.

The completed run logged twelve successful `video-configuration-queued` events.
No configuration-barrier, source-failure, push-failure, seek-flush-failure or
`Fatal error:` messages matched the inspected patterns. Twenty-nine media-key
failures remain: one PlayReady-access failure (`0x8a790010`) and twenty-eight
session-creation failures (`0x8a790004`). Preplay waits at position zero remain.
Brief playing-state stalls near 39 and 44 seconds cannot be attributed to an
episode rather than an ad. Sustained episode playback and resume are unresolved;
the media investigation continues at these remaining boundaries.
