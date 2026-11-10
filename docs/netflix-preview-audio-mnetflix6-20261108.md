# Netflix promotional preview audio: mnetflix6

## Evidence and first observed discontinuity

User reports screeching (earlier: distorted/crackling/repeating) on the promotional
landing preview, with working video. Authenticated title playback is not established.
Read only the immutable `mnetflix6/preview-audio-report` manifest and filtered
numeric audio files: guest 60056414 bytes, SHA256
`d9d2b8a656e386f599506fa3053fb79680f2f834445b6da4794c1372c0d13e80`,
221 records; host 3962 bytes, SHA256
`4246243be929e9e572a814aed0bb6f5bbff91eae779ce4269bd0ff65eba32e4d`, two warnings.
These are manifest identities, not newly computed full-log hashes.
Parent identifies production source 6501a362 and executable SHA256
`7e4aded5a21a402e0c2d30a230ea3b4579653ca1a266b4386cd50b37e7db1b28`.

* AudioOut: internal format enum 6 = Float8Ch (guest selector 5), 48000 Hz,
  256 frames, eight channels, four bytes/sample. Initial steady queues contain
  roughly 33–43 ms. Format enum 6 here is not guest selector 6 (S16 STD).
* NGS2: F32L/6ch/48k, ratio **1.0**, pitch **0 cents**, mask **8**. The ignored
  pitch warning does not establish an audible pitch error in this run.
* First logged output discontinuity: seq17 at **48.285336 s**, block8383,
  waits200041 us then clears67584 queued bytes. Seq18–22 repeat the clear,
  waiting about200–201 ms each, through49.320868 s. First NGS2 add appears
  between seq20 and21. Thus queue non-drain/clearing starts before source PCM
  arrives. No assertion about the exact audible onset follows from log ordering.
* All128 logged adds:40960 bytes,1706 frames, no skip/repeat, flags1;
  1706*6*4=40944, leaving16 bytes outside declared samples. Queue depth is1–2.
  Sampled active renders mostly emit256 frames and decode256/6144 bytes;
  larger deltas at boundaries include staging ahead. Render4224 and4736 show
  queue0/emitted0: actual source starvation. These are not evidence of repeated
  source blocks. Seq23 at80.925573 s reports an underrun after76000 us;
  seq24 at87.524412 s clears again after200567 us.
* Every AudioOut peak in the snapshot is zero, but **all scans after startup
  have count0/scan_complete0**. The old16M-sample scan budget expires after8192
  eight-channel grains, before the preview. `seen_nonzero=0` therefore does not
  say active PCM is silent or valid. Initial silence likewise exhausted the
  first16 short-grain diagnostic slots.

Queue clears discard audio and starvation inserts gaps, explaining possible
interruptions. Neither establishes why the user hears screeching. The trace
contains no active PCM amplitudes, nonfinite counts, or content comparisons.
Changing pacing, scaling floats, or implementing a guessed pitch effect would
not be an evidence-backed correction of that symptom.

## Source and sample checks

NGS2 stages interleaved F32 using memcpy and a24-byte frame stride; no integer
normalization is applied. It consumes the declared1706 frames, not the extra16
bytes. New sample regression queues three40960-byte buffers, each containing1706
distinct six-channel frames and NaN padding, then checks every emitted sample
and decoded-byte progress through256-frame grains. It passes on current
behavior: this geometry alone does not reproduce corruption. It is not a
negative control for a claimed sound fix.

Read-only static Netflix inspection reconfirms10768de..107694b copies producer
byte size and frame count separately into a waveform block and submits it with
flags1. It does not establish which live decoder produced those bytes.
Existing native F32 ABI observations are retained in
`netflix-ngs2-float-startup-20261108.md`; no new IDA/native execution is claimed.
The AJM FFmpeg converter explicitly chooses packed AV_SAMPLE_FMT_FLT, preserves
rate/layout, and uses swresample for planar-to-interleaved conversion. This
snapshot does not establish AJM/Audiodec execution or the preview codec, so that
source inspection cannot certify the actual upstream PCM range/layout.
AudioOut preserves F32 amplitude, applies channel gains and the non-STD8ch
channel permutation, then submits8192 bytes. No concrete wrong-stride or
normalization fix was established.

## Narrowly missing capture and implemented diagnostics

Both existing trace flags remain required. Added numeric-only `event=pcm`:

* NGS2: samples actually staged for consumption, with decode/render ordinal,
  byte offset, block/tail size, scanned count, nonzero/nonfinite/above-unit counts,
  finite peak/RMS, and a bitwise sample hash. This is before routing/effects.
* AudioOut: corresponding statistics and hashes for guest render output and
  post-volume/reordered output, with existing process timestamp, block ordinal,
  queue bytes and pre-submit wait. These are submitted samples, not speaker
  loopback. Counts are explicit; hashes only support comparison within the same
  layout/stage and identical counts. Repeated hashes alone do not prove stale
  ownership (silence and periodic signals can repeat).

Per window: at most8192 samples per scan,4096 scans,128 reports. Before activity,
scan first16 calls then every128th; silence does not consume reports. After first
observed nonzero input, capture the first16 active scans then every128th call.
NGS2 windows are restricted to the existing maximum8 matching voices;
AudioOut also shares its existing256-report global cap. Startup first-nonzero
scans are now sparse, extending the old scan budget by128x. NGS2 reserves its
short-grain event budget for after decoded progress. No samples/pointers/media
payloads are logged, and no sample values are altered.

Next parent-authorized capture needs only these filtered records from source
arrival through the audible fault, with the user's approximate symptom time.
Bad range already at NGS2 points upstream; clean NGS2 but bad guest AudioOut
points to mixing/effects; clean guest but bad prepared output points to gain or
layout handling. Clean submitted PCM with queue stalls requires SDL/device
consumption evidence. Plausible peaks alone cannot prove correct layout or sound;
if all stages look plausible, a separately authorized short PCM/loopback capture
may be needed. Do not declare a fix from diagnostic success.

## Validation and handoff

Isolated Ninja builds, max2 jobs, no build processes observed immediately before
starting. No shared output/source changes, emulator access, launch or store access.
The overlays rebuild the production-including test translation units and borrow
existing support libraries read-only; they are not clean production builds.
Audio overlay initially failed to link because the windows tree's subprocess
library was absent; it subsequently linked using the existing
`_Build/vsh-integrated/subprocess-tests/vsh_subprocess_component.lib`.

* Full `ngs2_sampler_tests`: pass trace0/1, including padded-block sample regression,
  existing32768-frame reuse coverage, and a diagnostic regression with9000 silent
  calls before activity, NaN/Inf/over-unit classification and hard-budget checks.
  New PCM records:0 off,43 on. Executable SHA256
  `335b4b5d6c2c0de211214a9dfc3385cdbc3b88ee5990880fd013fb51fd16fdbf`.
* Full `audio_out_timing_tests`: pass trace0/1; PCM records0 off,8 on.
  Executable SHA256
  `f3f553c7e637743d0ff28696cc0599ab41cbbcbc9e90e13fdc94b58fcf7163b5`.

Production integration targets: `ngs2_sampler_tests`, `audio_out_timing_tests`,
`kyty_emulator`, parallel<=2. Integrate only the new checkpoint; parent already
has590bb1a5's equivalent6501a362. **This checkpoint fixes diagnostic blind spots,
not the audible defect.** Screeching remains unverified/unfixed.

## Parent integration

Parent reviewed checkpoint922994a3 and integrated it as655e1168. Fresh process
inventory found no emulator; no process closure was performed or exit cause
established. VS18 production rebuild of the three requested targets is underway
with parallel2, log `Temp/opencode/netflix-pcm-parent-build.log`. Production
tests and the next active-PCM preview capture remain pending at this checkpoint.

### Production checks and mnetflix7

Production build completed; both CTest suites `ngs2_sampler` and
`audio_out_timing` passed with tracing disabled and both trace flags enabled
(2/2 each). SHA256 identities:

- Emulator: `57b8480119690b7a23f53b28a6f7798f2d28eb744cac5ea002693c42647cfdb6`.
- Sampler tests: `8d026d0b74c63752e94588370458ca9de60a1db6d8c6aa870c8fff8e36a24b5b`.
- Audio timing tests: `5e8c832588362b98da6ac5d9d511989ec941f6b4706e48f958b17632e63d159f`.

Fresh inventory found no emulator. Parent launched **mnetflix7**, retained logs
and manifest at `${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix7`.
PID25036, creation FILETIME134386313016009958; identity/path verified,
responding Netflix build46cb253 window at frame437/60fps. Original mnetflix1
arguments/store mounts were verified unchanged except the guest log path;
manifest environment matches the original plus both trace flags set to1.
User was asked to leave the landing preview playing about20seconds after
screeching begins. Active-PCM capture and audible correction remain pending.

### mnetflix7 severe screeching; captured PCM available

User reports "blasted my ear with screeching". Parent immediately attempted
identity-checked closure of its test PID25036; it was already absent. No closure
was performed and exit cause is unknown. Parent asked user to reduce/mute their
speakers and stopped requesting continued listening. No system audio setting
was changed. Further routine audible diagnostic retries are deferred.

Fixed complete-line snapshot: `mnetflix7/screeching-report`, including numeric
audio extracts and snapshot.json. Guest38461277bytes SHA256
`17b4293353f1d0a4ed9d6db1e19062a434ae2de97c744b8fbaec73e9c94401ec`,
235audio records including53PCM records. Host4032bytes SHA256
`dc328096be7a9035cadb3e479357d8f765f1f358b2abaafa06a9d6dfd5b85750`,
2audio warnings. Logs/stores remain preserved.

Existing Astra medium media agent resumed with this active-PCM evidence to
locate corruption across source/render/prepared-output stages and implement an
offline-tested causal correction. It has no process/store authorization and was
instructed to avoid further audible retries as routine diagnosis. Screeching is
still unresolved; no amplitude/layout or pacing cause is asserted yet.
