# Netflix mnetflix5 audio: bounded investigation checkpoint

## User-observed and log evidence

User confirms moving Netflix video and describes audio as distorted, crackling,
and repeating. Parent identifies the mnetflix5 production executable SHA256
`498f1d6dc0a9a1a866e9afa4c68062798104f97320bd7f7f7170e1b392f61d5c`.
No launch, process, stores, credentials or live media samples were accessed here.

Retained guest log records an SDL AudioOut source stream at 48000 Hz / 8 channels /
F32, one NGS2 setup type 0x18 / 6 channels / 48000 Hz / zero format offsets,
and warnings for ignored custom control 0x40002000 and module 0x20. The log
does not capture block sizes/progress, actual pitch parameters, SDL queue levels,
or audible output. `KYTY_AUDIO_TRACE` was not enabled. Linker relocations for
Audiodec/AudiodecCpu are not proof of execution or the episode's audio codec.

Static Netflix code 10761b2..107629a sends sampler ratio `r14/r8` via 40010005,
then sends a computed negative cent value via 40002000 with channelMask=8
(channel bit 3/LFE). This is a real candidate call site, not an observed stack;
the live cent, actual module/voice association and active channel mask are not
recorded. Ignoring a required effect is a genuine support gap, but the warning
alone does not prove it caused full-channel crackling/repetition. No guessed
pitch algorithm, fake effect success or silence patch is made.

Static app output loop 1074a6b..1074b54 renders 0x2000 bytes in type 0x18,
8 channels, then submits that same buffer to AudioOut before selecting the next
double-buffer slot: 8192 / (8 * 4) = 256 frames. The loop does not by itself
prove which thread/call site produced the user's observed episode.
Current AudioOut code reorders non-STD for SDL
and keeps STD order. A channel routing mismatch would not on its own explain
repeated grains, and the active guest output format selector is not logged.

## Tests and remaining boundary

Added real six-channel F32 tests at 48000 Hz: three unequal blocks through
a 64-frame grain and six-to-eight-channel SystemRender, checking every
distinct sample and canaries across boundaries; and a 256-frame, 32768-frame
progress-driven four-buffer reuse test checking every channel sample and actual
decoded-byte ownership. Both pass, as does the complete ngs2_sampler suite with
tracing off/on. These synthetic producer conditions do not reproduce or rule out
the live app's block sizes, deadlines, callback sequence or upstream decoding.

Isolated Ninja -j2 build rebuilt the test translation unit (includes production
ngs2.cpp) and timer from this tree, borrowed other objects/libraries read-only
from the prior overlay; not a clean production build. Test executable SHA256
`437ac943a1f30742b8f8672feedfde8a328779cced01b093148060037954fcc4`.
Trace off produced zero new diagnostic records; trace on produced 153 during
the entire suite. No data or samples were printed.

## Instrumentation, not an audio fix

This checkpoint adds bounded, numeric-only `NGS2 six-channel` records under
existing `KYTY_MEDIA_TRACE=1` for up to eight matching F32/6ch/48k voices.
They capture actual block counts, sizes, repeat/skip flags, grain, sampled render
progress, decoded byte/sample deltas, queue depth, PCM staging counts, ratio,
and (when observed on the matched voice) pitch cents/channel mask. They never
print pointers, sample values, packet payloads, URLs or credentials. Add records
are capped at 128 process-wide; each matching voice reports its first 16 render
events, the first 16 short-output grains, and every 128th up to render 6144;
pitch/ratio each cap at 32. A missing
record after a cap does not prove inactivity. This is observability only: no
claim that the reported sound is fixed.

Parent controls all process/store decisions. If a new user-authorized attempt
is made, build `ngs2_sampler_tests` and `kyty_emulator` with parallel <=2, run
the sampler suite and enable **both** `KYTY_MEDIA_TRACE=1` and
`KYTY_AUDIO_TRACE=1`. Correlate NGS2 add/render totals and log ordering with AudioOut
`put` queue/underrun/clear and pace records, including actual output format and
block size; compare first concrete discontinuity to the audible symptom. Only
then select an ownership, pacing, upstream decode or real pitch-effect fix.
Preserve stores and avoid dumping guest logs wholesale.

## Parent integration checkpoint

Parent reviewed and integrated agent checkpoint590bb1a5 as6501a362. The
video-playing baseline remains hash-verified at
`Temp/opencode/mnetflix5/video-baseline/kyty_emulator.exe`. Production
`ngs2_sampler_tests` and `kyty_emulator` build is running with VS18 Build Tools,
parallel2, logged to `Temp/opencode/netflix-audio-parent-build.log`; production
test results and the traced retry are pending at this checkpoint.

The retained manifest replay helper now accepts explicit `--audio-trace`, sets
both trace variables to1 after applying the original environment, and records
those overrides in the new launch manifest. It preserves original arguments
and store mounts, changing the guest log destination for each fresh run.

### Production validation and mnetflix6 launch

Build completed successfully; production CTest `ngs2_sampler` passed with trace
disabled and with both trace flags enabled. Executable SHA256:
`7e4aded5a21a402e0c2d30a230ea3b4579653ca1a266b4386cd50b37e7db1b28`;
sampler test SHA256:
`1a43be64f1cee70fd2ef9559f59ff301585dad0a766a32141bde8461832e93ed`.

Fresh process inventory found no emulator. Parent launched **mnetflix6** using
the original mnetflix1 manifest and `--audio-trace`; arguments/store mounts and
all other original environment entries were programmatically verified unchanged.
Evidence is `${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix6`.
PID33856, creation FILETIME134386303331121745; process identity/path verified,
responding Netflix window build6501a36 at frame423/60fps. Both trace variables
are1 in the retained launch manifest. User was asked to play the same title for
about30seconds after crackling starts. Playback/audio evidence from this retry
is pending; this launch is still diagnostic, not a verified correction.

### Preview reproduces screeching; user selects Astra medium

User reports the preview has the same audio problem, then specifically says
"the audio is creetching". Supplied screenshot shows the promotional landing
preview with Trending Now/Plans, HEART-RACING THRILLS, Sign In/Get Started.
This reproduction is preview audio, not evidence of authenticated title playback.

Parent retained a complete-line fixed snapshot at
`mnetflix6/preview-audio-report`, with numeric audio extracts and snapshot.json.
Guest60056414bytes SHA256
`d9d2b8a656e386f599506fa3053fb79680f2f834445b6da4794c1372c0d13e80`,
221 audio records; host3962bytes SHA256
`4246243be929e9e572a814aed0bb6f5bbff91eae779ce4269bd0ff65eba32e4d`,
2 audio records. This is a fixed prefix, not a claim the process exited.

At user's explicit request, the existing media subagent was resumed with
`openai/gpt-6-astra#medium`, supplied this snapshot and screenshot description,
and assigned causal analysis, a focused fix/regression, or a precisely bounded
next capture if evidence remains insufficient. Audio correction remains pending.
