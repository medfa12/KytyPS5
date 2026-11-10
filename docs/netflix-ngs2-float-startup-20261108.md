# Netflix startup abort: real NGS2 float PCM support

## Actual failure, superseding test-readiness assumptions

The user reports no app opened. Parent confirms its user-owned PID2932 exited.
Completed `Temp/opencode/mnetflix2/host.log:44-47` identifies production `3a12861`
and the fatal `format.waveform_type != 0x12` at `src/libs/ngs2.cpp:705`.
`guest.log:940811-940818` records voice0, command **0x40010000**, size40, next0,
then the same fatal; the preceding timestamp is00:00:43.579.

Guest log identity: 28,596,039 bytes, SHA256
`1b77825b37a6b2d2208cc89241d8332e0e5af92d4946f02492418d17cd650237`.
No new title playback, loading percentage, video-decoder admission or visible UI
is established. The earlier resource/memory/ESVM component suites **omitted NGS2**;
they could not establish startup readiness for this path.

The logs do not contain the numeric waveform type. The condition proves a
nondeferred, non-ATRAC9, non-I16 sampler format reached setup. The disassembly
below supplies a concrete F32 setup path and an offline reproduction of exactly
this fatal, not a fabricated live argument capture. The next corrected live run
will record the actual numeric type in a bounded diagnostic.

## App and regular13.60 contract

App PPSA01616 eboot SHA256 reconfirmed:
`be42bd7ccc0b436ef0e4309d78266a9661c8827e1012b564c14d838bd5edf8d9`.

- `uu94irFOGpA` (VoiceControl) imports through PLT `0x1961ac0`.
- App routine `0x1075d5a` selects **0x18** at `0x1075df1`; its alternate branch
  selects0x12 at `0x1075e1f`. `0x1075e49` constructs the eight-byte header
  `0x4001000000000028`; `0x1075e66` writes format at command+8, channels at+12,
  sample rate at+16, zeros remaining format/setup words, and calls VoiceControl
  at `0x1075e7f`.
- Independent app setup at `0x1644536..0x1644580` selects0x12 for sample bit-depth16
  and otherwise0x18, then sends the same size40/custom-sampler command.
- Setup at `0x107506f..0x10750a9` copies a format obtained by ParseWaveformData.
  It does not encode a fixed numeric type. Current HLE's successful parser path
  supplies ATRAC9. A static candidate scan is not a live caller stack.

The **Prospero** regular13.60 provider is `system_b/common/lib/libSceNgs2.native.sprx`,
SHA256 `101d55ffa101f54ede5bf79de844ba08981cc6a8cb0e03b3c150259a15ba4d3f`.
The sibling `libSceNgs2.sprx` has a different block ABI; its32-byte block must not
replace the native provider's40-byte block used here.

- Native CalcWaveformBlock `0x10440` zeroes40 bytes; `0x10477..0x10495` indexes
  table `0x595a0` by waveform-type minus0x10, multiplies sample width by channels,
  then by sample offset/count. Table entry0x12 is2 bytes; **0x18 is4 bytes**.
- Native PCM conversion helper `0x5950` dispatches0x18 to direct float copy:
  vector moves at `0x59c0..0x59d2`, scalar `vmovss` at `0x5ca0..0x5cb1`.
  It does not apply the I16 scaling/clamping path to F32. Unsupported types at
  this boundary return **0x804a8050**. Sampler-related dispatch also uses that
  unsupported-type error at `0x27828..0x2783c`.

Read-only audit used `Image` from the parent's `_TempData/lifecycle-re-inspect.py`
under `python -B`; bounded reproduction slices:

```python
app.dis(0x1075d5a, 0x12a, {})
app.dis(0x1644532, 0x53, {})
native.dis(0x10440, 0x1a9, {})
native.dis(0x5950, 0xc0, {})
native.dis(0x5c95, 0x1f, {})
```

## Implementation

`ngs2.cpp` now retains the selected PCM type and admits actual interleaved
**little-endian IEEE float32** alongside existing signed16 and ATRAC9:

- F32 input copies real sample values into the float mixer without integer
  reinterpretation, normalization or fabricated silence. Unaligned block offsets
  use memcpy. Existing signed16 normalization remains `/32768.0f`.
- The same sample-width helper controls decode strides, skipped frames,
  `waveform_data`, decoded byte counts and CalcWaveformBlock sizes/offsets.
- Existing grain staging, interpolation/resampling, repeated blocks, retirement,
  routing, mastering and output conversion operate on those decoded samples.
- Unsupported types return0x804a8050 **before** resetting the voice or releasing
  its current buffers. VoiceControl propagates the error; unknown types are not
  relabeled as float. Existing deferred/ATRAC9 behavior remains exercised.
- `KYTY_MEDIA_TRACE=1` adds at most32 setup records per process containing type,
  channels, rate and raw words at format+16/+20. No sample bytes or keys are logged.
  Raw offset labels intentionally avoid assigning new semantics to the existing
  source's frame-offset/margin naming. Only dense PCM (both words zero) is covered;
  this does not implement endian variants, packed PCM, compressed formats beyond
  existing ATRAC9, or padded frame layouts.

This is actual source/sample handling, not merely removing the fatal guard.

## Old successful run / dirty-tree audit

The original dirty `media-app-playback/src/libs/ngs2.cpp` is byte-identical to
`3a12861` after CRLF normalization. There is no unintegrated float PCM fix in that
file to copy. `444ad5b2..3a12861` has no ngs2.cpp diff. All old trees were read-only.

Old `mnetflix1/guest.log` is689,407,379 bytes, SHA256
`71973064d6acc25b1fa175f371c9b4b519902355d42465a92464bd3d75d038be`.
It contains one0x40010000 record and no waveform-type fatal, but also omits the
format value. Therefore earlier progress to20% does not establish that this
unsupported format was previously handled, nor that it was the same setup branch.
The old launch's executable hash remains `8e12a26b...a0c079`, distinct from this
failed run's `3aa36360...0786e82`; no unproven byte-level executable equivalence
is asserted.

## Verified regression and negative controls

Isolated overlay build, Ninja **-j2**, own output `_Build/ngs2-validation`:
recompiled `Ngs2SamplerTests.cpp` (includes production ngs2.cpp directly) and
timer.cpp; borrowed unchanged parent common/fmt/ATRAC9 objects/libraries read-only.
No parent build graph was run or written. This is not a clean full emulator build.
Local runtime helpers: `_TempData/build-ngs2-validation.py` and
`_TempData/build-ngs2-negative.py`.

**Full ngs2_sampler_tests passed, exit0**, including existing PCM16/ATRAC9/control/
queue/callback/mastering coverage and new checks:

- Literal F32LE bytes, unaligned start, distinct signed stereo samples, a skipped
  frame, exact sample values and byte/sample progress, final ownership release.
- Both standard and custom samplers; unknown-type error preserves an active
  pending float stream. Float block sizes use4-byte samples.
- Real24-to48kHz interpolation and repeated float input.
- Exact Netflix-shaped size40/0x40010000 setup, exported block submission,
  custom sampler -> mastering -> SystemRender, with exact interleaved output
  samples for both float32 and signed16 destinations and buffer canaries.

Negative controls compile the **same new test driver** against separate local
copies of original `3a12861` ngs2.cpp; they never alter production or the dirty tree:

1. Unchanged baseline builds, then exits **321** at the original fatal line705
   immediately after size40/0x40010000. Log:
   `_Build/ngs2-negative/sampler-baseline.log`.
2. Baseline with **only**0x18 allowed past the guard builds, then fails
   `float skip used the wrong byte stride`, exit **3221226505**. Log:
   `_Build/ngs2-admit-only-negative/sampler-baseline.log`.
   This verifies that merely admitting the format is insufficient.

Final successful suite log: `_Build/ngs2-validation/sampler-final.log`.
Final test executable SHA256:
`e5caefb0a4a61b3bad9b83be163593f7de82e77868c9891dcb53816a6e091abf`.
`git diff --check` passed.
These tests render authentic generated sample values into output buffers, without
a host audio device. They do not establish audible Netflix playback.

## Parent integration / live acceptance

Parent should rebuild `ngs2_sampler_tests` and `kyty_emulator` with parallel2,
run CTest `ngs2_sampler`, and record the new executable hash. The source change
affects private host voice layout, so normal dependent recompilation is required.
Production build/relaunch was not performed by this agent.

Use the same `mnetflix1` stores and launch settings, a new evidence directory
(for example mnetflix3), and the **new** executable. The earlier recipe's pinned
3aa36360 hash must be replaced with the rebuilt hash; launching that old binary
will reproduce the unfixed gate. First acceptance: visible Netflix UI, setup
type diagnostic and no NGS2 fatal. Only then retry authenticated KAMPEN MOD EN
ORKAN and attribute query/create/output beyond20% separately. Protected playback,
presentation and audible output remain unverified. Apple remains pending.
