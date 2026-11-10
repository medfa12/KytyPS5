# Netflix corrected-run attribution preparation — 2026-11-08

Parent integrated the retest checkpoint as `826bf844` and launched user-owned
PID2932, `Temp/opencode/mnetflix2`. Only its `launch.json` was read here. It records
the corrected `3aa3636099727914e0cb00ceb2ecfec5c79e65ca3f1c9bda6e04f71be0786e82`
executable and all four original `mnetflix1` mounts/environment, with the new guest
log destination. No new guest/host log was inspected while awaiting the user's
same-title retry; no polling, attachment or process/input control was performed.

## Exact evidence available from this binary

`libVideoDec2.cpp` enables `PRINT_NAME`, but its QueryDecoderMemoryInfo,
CreateDecoder and Decode exports do **not** log their returned status/configuration
or output fields. `KYTY_MEDIA_TRACE` supplies ESVM diagnostics, not these missing
Videodec2 values. Counted API entries establish calls, not successful return or
decoded output. A nearby kernel allocation of the expected size is supporting
evidence, not an unambiguous join to a specific query without caller context.

Fresh read-only disassembly of the actual PPSA01616 app reconfirmed:

- Query import `qqMCwlULR+E`, PLT `0x1962940`.
- Create import `CNNRoRYd8XI`, PLT `0x1962950`.
- Decode import `852F5+q6+iM`, PLT `0x1962980`.
- AVC create helper `0x1506be6`: query call `0x1506c97`, test returned EAX at
  `0x1506c9c`; memory setup `0x15061f6`; creation `0x1506cdd`, test at `0x1506ce2`.
- HEVC create helper `0x1505924`: codec `0xee049`, query `0x1505a31`, creation
  `0x1505a7a`, with the same shared memory setup. Thus AVC alone must not be assumed
  for a later title's actual create call.

The AVC error paths pass real returned EAX as the logging argument at
`0x1506cfb`, calling logger `0x1515386` at `0x1506d08`. Static strings:

```text
0x1d2f687  VIDEODECODER
0x1d87540  %s: %s(): sceVideodec2QueryDecoderMemoryInfo() returns error %#x
0x1d398bb  %s: %s(): sceVideodec2CreateDecoder() returns error %#x
0x1d28db4  create
```

These are useful exact searches after the parent forwards the attempted-title
result. They are static logging paths, not proof that logging verbosity will
preserve every failure. No new error is claimed at this checkpoint.

## Boundary table for the first actual failure

| Boundary | Current implementation / discriminating evidence |
| --- | --- |
| Requirements | Query success writes CPU/GPU/shared/max-frame sizes each `0x1000000`, alignment `0x100`. Resource rejection `0x811d0203`; unknown codec `0x811d0204`; malformed config `0x811d0200`, depth `0x811d0206`, DPB `0x811d0209`, dimensions `0x811d020a`. Null/size errors `0x811d0102`/`0x811d0101`. |
| Allocation | Original zero-length type12 request and `vdec dmem` EINVAL are the old signature. Shared allocation in `0x15061f6` uses size at memory-info+0x28, rounds to0x4000, calls AllocateDirectMemory at `0x1506276`, MapDirectMemory at `0x150629c`; their errors enter fatal reporting at `0x15062ee`. Labels are `sceKernelAllocateDirectMemory` / `sceKernelMapDirectMemory`. |
| Creation | Each of the four required sizes must be at least16MiB (`0x811d0104` otherwise); CPU/GPU/shared pointers must be nonnull (`0x811d0105`). Only resource1 needs compute queue (`0x811d0110`). Backend creation failure is `0x811d0100`; nearby `Videodec2: FFmpeg decoder is unavailable`, `avcodec_alloc_context3 failed`, or `avcodec_open2 failed` can narrow it. |
| Input/decode | Decoder handle `0x811d0103`; AU size/pointer `0x811d010d`/`0x811d010e`; output buffer size/pointer `0x811d0106`/`0x811d0107`. Backend AU rejection/receive error maps to `0x811d0301` and may log `Video decoder: submitting an AU failed` / `avcodec_receive_frame failed`. These shared backend strings need surrounding call attribution. |
| Output | Oversize picture is `0x811d0302`; insufficient NV12 output buffer `0x811d0106`. Required bytes are `align_up(width,256) * (height + ceil(height/2))`. API failure `0x811d0100` has several backend causes and is not a unique diagnosis. |

## Decode success is not the final output/ownership boundary

The app's routine beginning `0x1506638` constructs input size48, frame-buffer
size32 and output-info size56. Its decode call at `0x150681f` stores EAX in r15d.
After incrementing its input-ring index it:

1. Treats `0x811d0300` specially (`0x1506834`), entering its Flush path.
2. Tests other nonzero results at `0x1506841`.
3. On zero, ORs its resource-selection byte `[rbx+0x80]` with the frame-buffer's
   **is_accepted byte** `[rbp-0x40]` (`0x1506846..0x150684c`). With both false it
   places **-2** in the app status path at `0x150684f..0x1506874`.
4. Separately tests output-info **is_valid** `[rbp-0x88]` at `0x15068d7` before
   its picture-processing branch.

The HLE sets `is_accepted` from the backend's `buffer_accepted`; that becomes true
only when CopyFrame actually copies an output picture. DecodeInput deliberately
returns success with invalid output when FFmpeg has accepted an AU but produced no
picture yet. Therefore future evidence must distinguish API status, output-buffer
acceptance and picture validity. The -2 path's effect on higher-level retries is
not established here; this is **an attribution lead, not a demonstrated defect**.
Do not mark an output buffer accepted without real retention/copy ownership.

Neither repeated Decode entries nor disappearance of the old fatal allocation
proves video presentation, audio output, or sustained playback. The user-selected
title/time and visible result remain necessary. Do not substitute Apple ESVM
readiness/DRM theories for a Netflix Videodec2 boundary.

## Reproduction / validation scope

Used the existing read-only `Image` inspector from
`${USERPROFILE}/Desktop/kytyps5/_TempData/lifecycle-re-inspect.py`, loaded with
`runpy.run_path` under `python -B`, on the same Netflix eboot whose SHA256 is
recorded in `netflix-videodec-admission-20261107.md`.

Relevant bounded slices (addresses are app RVAs; second argument is byte length):

```python
image.dis(0x1505924, 0x1bc, {})   # HEVC config/query/create and return paths
image.dis(0x15061f6, 0x100, {})   # shared allocation/mapping failures
image.dis(0x1506638, 0x2d8, {})   # decode, acceptance, output-valid consumers
image.dis(0x1506be6, 0x15e, {})   # AVC query/create and error logging
```

Candidate direct-call scans additionally found Decode calls `0x14fe67c` and
`0x1640c29`; they were not attributed to this title's executed path. Initial
mid-instruction exploratory slices were realigned to prologues `0x1506638` and
`0x1505924` before interpreting their surrounding instructions.

Source audit and static disassembly only. No production modification, build or
new runtime acceptance is claimed. Existing component passes are in the media
retest checkpoint. Next action is a bounded read of the new attempt's retained
logs after the parent forwards the user's result.

## Parent mnetflix2 startup failure

Parent launched PID2932 from the documented original manifest after confirming
Home had closed. User then reported no app had opened. Process inventory found
no emulator, and both `mnetflix2/host.log` and `guest.log` end with:

```text
Source build 3a12861
Not implemented (format.waveform_type != 0x12) in .../src/libs/ngs2.cpp:705
```

The guest tail reaches approximately00:00:43.579, then logs sampler voice0,
parameter id`0x40010000`, size40, next0 before abort. The exact waveform value is
not printed by this binary; it must be recovered from the actual caller/native
contract or a bounded diagnostic before choosing conversion behavior. API-entry
logs and shader compilation before the abort do not establish a usable window.

Parent's earlier launch response incorrectly implied visible app readiness.
No authenticated-title retry,20% progress, picture or audio occurred as an observed
acceptance result. The Astra Medium media agent is now tracing this demonstrated
NGS2 startup blocker and implementing an evidence-backed correction/regression.
The old dirty media worktree remains read-only; no speculative waveform acceptance
or audio output bypass is authorized. No emulator is currently running as of the
failure investigation inventory.
