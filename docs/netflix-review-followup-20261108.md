# Media review follow-up: frame layout and next observable gates

## Review scope and disposition

Read `Temp/opencode/opus55-vsh-review.md`. Its stated scope was committed main
source/documents, not agent worktrees, private changes, binaries or mnetflix2 logs.
It predates `2b4ba100` (parent integration `e758b676`); it is not an independent
review of that fix. Parent reports production build/test in flight; this audit
does not claim those checks have finished. No runtime, launch or mnetflix3 access
was performed.

Already addressed by `2b4ba100`:

- Correct labels:0x12 is signed16LE;0x18 is IEEE float32LE.
- Real F32 input conversion, shared width for sizing/skips/progress, actual
  stereo samples through SystemRender, and unsupported-type error propagation.
- Two negative controls: original fatal and admission-only wrong-stride failure.
- Bounded numeric setup diagnostics before validation, under KYTY_MEDIA_TRACE.
- Earlier readiness documentation now explicitly records the omitted NGS2
  coverage and actual failed startup. No visible startup recovery is claimed.

Still unverified: the exact numeric type of the failed live call, corrected
visible Netflix UI, live NGS2 output/audio, and authenticated title playback.
Static F32 candidates and offline tests do not manufacture the missing runtime
argument capture. New format support is not broadened by this documentation audit.

## Nonzero margin/offset: not the next failure of the same call

The review correctly identifies an unsupported path, but its **“likely the next
Netflix startup blocker”** attribution is speculative.

In the exact `3a12861` source, SetupSampler first computes `deferred`, then rejects
any nondeferred call with either raw field+16/+20 nonzero or zero sample rate/
channels at lines670-672. Deferred setup returns before the type check. The live
fatal is instead line705, reached only after that earlier assertion passed.
Therefore this failed setup had **both raw fields zero and nonzero rate/channels**.
This follows from the source control flow and retained fatal location, not from
a missing log field. Fixing its PCM type cannot newly expose the earlier assertion
for the same immutable request. A different future setup could still exercise it.

Two recovered explicit Netflix setup producers independently agree:

- `0x1075df1` selects0x18 (alternate0x12 at0x1075e1f). `0x1075e56` zeroes xmm0;
  `0x1075e75` stores16 zero bytes at **command+0x14**, covering configData,
  format+16, format+20 and setup flags. `0x1075e7b` clears reserved; VoiceControl
  call is0x1075e7f.
- `0x1644536..0x1644566` selects0x12/0x18. `0x164454e` zeroes xmm0 and
  `0x1644574` writes the same zero range before call0x1644580.

The actual failed caller was not recorded. These are independently established
zero-padding F32 producers, not proof that every app path has the same format.

## Native ABI audit and limits

Images/identity remain those in `netflix-ngs2-float-startup-20261108.md`: actual
Netflix eboot and regular13.60 **libSceNgs2.native.sprx**, not the sibling module
with the older32-byte waveform-block ABI.

Both fields at offsets 16 and 20 are required zero by the HLE, so their current rejection behavior is
symmetric. The new diagnostic deliberately prints **field16/field20**, avoiding
a false semantic label. This naming discrepancy is not an explanation for the
observed type fatal, nor a reason to implement guessed padding rules.

Fresh regular13.60 native wrapper audit:

1. VoiceControl dispatch at0x254da..0x254f8 indexes table0x60650 for0x40010000.
   Entry0 resolves to0x25510. That branch requires at least40 command bytes,
   reads channels at command+0xc and setup flags at+0x20, and builds internal
   commands. For nonzero channels,0x2554f forwards the **complete format pointer
   at command+8**;0x26175 stores it and0x2618b invokes dispatcher0x224c0.
   The wrapper does not itself impose the HLE's zero-field assertions.
2. For control0x40010003, native table0x60650 entry3 targets
   **0x25712**: it checks size>15, sets result0, and jumps directly to next-command
   handling0x2619c. In this provider's wrapper, this command does not read/apply
   its offset value. That fact does **not** prove nonzero setup-format fields
   can be ignored: setup follows the distinct internal dispatch above.
3. Native CalcWaveformBlock0x10440 computes dense PCM positions/sizes from
   type/channels/sample counts; it does not establish the source reader's
   padded-frame stride or lifetime contract.

Thus the byte units, wrapper forwarding, record sizes and separate control are
established. The lower reader's full nonzero setup-field semantics remain open.
No native support for a guessed `stride = sample_bytes + margin` or offset as
sample count is claimed. The host currently also lacks control3; it is not an
observed Netflix requirement. Completed mnetflix2 contains no0x40010003 record;
an executable-byte scan found no literal0x40010003, which is only supporting
evidence and cannot exclude dynamically constructed commands.

Reproduction with the retained read-only Image inspector, under python -B:

```python
native.dis(0x254da, 0x1f, {})
native.dis(0x25510, 0x59, {})
native.dis(0x25712, 0x18, {})
native.dis(0x26175, 0x3d, {})
app.dis(0x1075e42, 0x42, {})
app.dis(0x1644532, 0x53, {})
```

## First-boundary evidence for the parent-owned mnetflix3 attempt

Treat setup diagnostics, API entry, output ownership, presentation and user-visible
success as different observations. Preserve launch hash, title and attempt time.

| Gate | What establishes progress | Current trace limitation / failure stop |
| --- | --- | --- |
| Visible UI | Parent/user sees Netflix UI; timestamped screenshot identifies it | Shader creation, process liveness and NGS2 logs do not prove a visible window/UI. If absent, retain first fatal/exit and the last specific boundary. |
| NGS2 format | First `NGS2 sampler setup: type=... channels=... rate=... field16=... field20=...` confirms the actual request | This line is **before validation**, not a successful-return record. If nonzero fields or another unsupported type appears, stop format claims at that exact tuple; do not repeatedly relaunch or guess another codec. Cap32 applies process-wide. |
| NGS2 startup continuation | No recurrence of the fatal plus observed UI progression; subsequent block/play activity is supporting evidence | VoiceControl command headers do not log return status. SystemRender currently emits no per-render output-success/sample trace. Neither a setup line nor the absence of an error proves real audible output. |
| Videodec2 requirements/create | Actual query/create result or Netflix's `...returns error %#x` message; correlate allocation with caller/config | Import relocation is **not execution**. In mnetflix2 the QueryDecoderMemoryInfo match at guest.log2179 is only relocation. No new decoder acceptance is established by that aborted run. |
| Decoded output | Returned API status **and separately** frame-buffer.is_accepted, output-info.is_valid, picture_count/dimensions | Current Videodec2 exports log entry names, not these fields. Zero return can mean accepted input with no picture yet; buffer acceptance concerns output-buffer ownership, not AU acceptance. Do not equate the three. |
| Presentation/audio | Same authenticated KAMPEN MOD EN ORKAN proceeds beyond20%, then user-observed moving video and audible sound for60s, with timestamps | Percentage alone, copied decoder pixels or PCM output buffers do not establish presentation or audibility. |

The actual app decode consumer remains0x150681f (call),0x1506824 (save result),
0x1506841 (nonzero result),0x1506846..0x150684c (resource selector/output-buffer
acceptance),0x15068d7 (picture validity). Its local -2 branch is documented in
`netflix-retest-attribution-20261108.md`; its higher-level effect is not proven
to be fatal. Do not turn it into a new diagnosis without the corrected attempt.

If a corrected attempt reaches Decode but the existing logs cannot distinguish
these outputs, record that **specific observability gap**. Any next instrumentation
should be bounded at the returned status/output tuple, after the real call, without
packet contents or fabricated acknowledgments. No attach or instrumentation of
the parent's running process is requested or performed by this audit.

## Stop condition / sequence

This scoped audit is complete: nonzero fields are not demonstrated by the current
failure, known F32 producers zero them, and relevant native wrapper limits are
documented. No additional production change or rebuild is justified here.
Wait for the parent's new numeric setup and visible-UI result; analyze the first
actual failing boundary once supplied. Parent owns build completion, processes
and navigation. Clear Apple previews follow Netflix; protected Apple playback is
not scheduled. No new component tests were required for this docs-only checkpoint.
