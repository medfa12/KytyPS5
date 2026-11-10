# Bounded graphics observation (Windows x64)

This is host-only instrumentation. It does not patch guest functions, alter packets,
change render decisions, or force presentation. Disabled by default.

## Usage

Set these **host** environment variables before the existing launcher command:

```powershell
$env:KYTY_GRAPHICS_OBSERVE = '1'
$env:KYTY_GRAPHICS_OBSERVE_DELAY = '120' # integer seconds after graphics init, 0..3600
# Optional event bit mask; 0x8000 captures only Texture events from boot.
# Omit for all events. Filtered events do not consume slots or increment totals.
# $env:KYTY_GRAPHICS_OBSERVE_MASK = '0x8000'
# Run the otherwise unchanged launcher; Silent / NoGraphicsDebugDump are supported.
```

Unset `KYTY_GRAPHICS_OBSERVE` to disable. Invalid delay disables observation.
No additional emulator CLI option is required. No shader/command/frame dumps are
enabled by this facility. Use a fresh run with another delay if the budget is spent
before the interesting activity. There is no live rearming or overwritten ring.

Output is one exclusive-created file:

`%LOCALAPPDATA%\Temp\opencode\graphics-observe-<PID>-<start-QPC>.bin`

The file is **exactly 524544 bytes**, including **4096 immutable 128-byte slots**.
Initialization failure leaves observation disabled (no diagnostic logger fallback).
The mapped view intentionally lives until process exit. OS cache writeback owns
durability; there is no explicit flush, so this is not power-loss-safe storage.

Recording uses lock-free 64-bit atomics, QPC and fixed-size stores only: no allocator,
symbol lookup, stdio, locks, target callbacks, or waits. Initialization creates and
zero-touches the file mapping before guest execution. Ordinary OS paging remains
possible. A producer suspended after claiming a slot leaves a hole; other producers
and readers do not wait for it. The process-lifetime mapping prevents teardown races.

## File schema, little endian, version 1

Header: magic `0x3142534f5550474b` (8 bytes), version/capacity (two uint32),
PID/frequency/start-QPC/delay-QPC (four uint64), then eligible/dropped/skipped
(three uint64), then totals[16] (uint64). At offset 200, ready (uint64) is
release-published as 1 when initialization finishes; readers must check it first
(acquire for a mapped concurrent reader). Remaining bytes through offset 255 reserved.
Totals are indexed by event number and include pre-delay and over-budget events.
Eligible counts post-delay reservations, including holes. Dropped counts events
past capacity. Counters continue updating after the record budget is exhausted.
Live counters are individually atomic, not a consistent multi-counter snapshot.

Slots begin at offset 256. Each contains 16 uint64 words:
`commit, event, QPC, values[13]`. Accept slot i only when commit == i+1.
Commit is release-published last; mapped concurrent readers must acquire-load it.
Slots are never overwritten. An ordinary copied-file snapshot can omit a concurrently
committed slot; do not treat an incomplete slot as evidence of pipeline failure.

All pointers/addresses are raw process/guest addresses. The shader-creation hook
re-reads the same validated register array just patched by the original call;
other hooks copy existing host state or the packet they just produced/consumed.
No native Light object pointer traversal occurs in these hooks. Unused words are zero.

| Event | values, in order |
|---|---|
| 1 Shader | header, code-base, type, code-size, SH-register-array, SH-register-count, effective patched program address (0 for no applicable pair) |
| 2 Emit | AGC-buffer, packet, vertex-count, modifier, cursor-after, bottom, packet-dword0/1/2 |
| 3 Submit | root-command-address, DWORD-count, interrupt-ID (before original submit call) |
| 4 Consume | CP, packet-address, vertex-count, flags (validated DRAW_INDEX_AUTO) |
| 5 Draw | submit-ID, host-command-buffer, 0, PS/ES/GS GPU addresses, RT0 address, target-mask, vertex-count, instances, primitive, RT0 width/height |
| 6 Outcome | same as Draw, with reason in values[2] |
| 7 Resolve | presenter-frame, cache-image-pointer, requested address/size, resolved address/size, requested width/height, backing width/height, GPU-modified |
| 8 Prepare | request-ID, presenter-frame, buffer-index, special/blank, source address/size, width/height |
| 9 Present | main-frame, overlay-frame, swapchain-status, attempt, system-overlay-active (after actual Present call) |
| 10 Blank | presenter-frame, width, height, opaque, producer-command-buffer |
| 11 Process | CP, submit-ID, root-command-address, DWORD-count, queue-ID, submission-type (first slice) |
| 12 Dispatch | CP, submit-ID, host-command-buffer, vertex-count, instances |
| 13 Indirect | CP, parent-packet, child-command-address, DWORD-count, control |
| 14 Target | submit-ID, host-command-buffer, target-slot, resolved guest address/size, width/height, color-target-count (first color target only) |
| 15 Texture | shader hash, ShaderType stage, resource ordinal, resolved guest address/size, eight descriptor DWORDs (each zero-extended to uint64) |

Outcome reasons: 1 zero count, 2 metadata/depth-copy/resolve handled instead of draw,
3 invalid/missing vertex shader, 4 unsupported topology, 5 preparation failed,
6 rectangle-list export mismatch, 7 Vulkan draw encoding returned, 8 zero mesh
primitives/instances, 9 ExecutePreparedDraw returned. Reason 9 alone is **not**
proof of draw encoding; use reason 7. Existing fatal checks remain fatal; their
path may leave only an entry record. The shared encoding/Target hooks also observe
indexed draws; detailed entry/rejection coverage is for DrawAuto.

Swapchain status is the existing enum in `window/swapchain.cpp`:
0 Success, 1 Recreate, 2 SurfaceLost.
Successful Present is not proof of nonblack pixels or GPU completion. Frame/image
pointers can be reused: correlate by sequence/time, not pointer equality alone.

## Correlation

Follow Emit.packet into a Submit span, Process(root, submit-ID, CP), optionally
Indirect spans, Consume(CP, packet), Dispatch(CP, submit-ID, host-buffer), and
Draw/Outcome/Target. This handles the normal direct and IT_INDIRECT_BUFFER path;
conditional branch packets and all alternate draw opcodes are not traced.
An event outside the delay window or beyond budget can break the join; consult
counters before interpreting absence. The stream is reservation order, not a
global happens-before order between producer threads.

Match the Light shader pair to Draw/Outcome, then join its Target guest range to
Resolve requested/resolved ranges, Prepare frame/request and Present main/overlay.
Intermediate compositor passes may sample Light's offscreen result. Texture events
record each stage's materialized image bindings, including storage images, before
draw preparation; they do not prove that a draw executed or that texels are valid.
Join shader hashes to shader dumps and descriptor address ranges to targets.
Address mismatch alone does not prove a lost image. Texture events contain no
pixel data and do not perform guest-memory reads.

For transient upload allocations, `KYTY_TEXTURE_DUMP=1` with graphics debugging
enabled snapshots CPU backing at first binding of each shader/ordinal/descriptor
key. Files and `textures.jsonl` are written beneath
`<ShaderLogFolder>/texture-backing/`. Limits are 128 keys, 4 MiB per image, and
32 MiB total. These reads bypass GPU readback, remain non-atomic with guest
writes, and can contain stale GPU outputs. Preview supported R8/RGBA8/BC7
textures with `python -B tools/preview_texture_bindings.py <directory>`.

Light identity from the independently established native layout:
`VS=qword[Light+0x70]`, `PS=qword[Light+0x78]`, `header=qword[object+0x60]`.
Validate header magic `0x34333231`, version `0x18`. Header+0x20 is the SH-register
array, +0x5c its byte count; entries are `{uint32 offset,value}`. Decode patched
program pairs with `(uint64(LO)<<8)|((uint64(HI)&255)<<40)`: PS offset 0x08,
ES 0xc8, GS 0x88, LS 0x148, HS 0x108 (HI is adjacent). Header+0x10 is the code
base, which can differ from the effective program address by a binary offset.
These reads are external/optional, not performed by recording hooks.
Shader events before the configured delay are skipped too; when targeting a late
window, retain the external Light/header/program snapshot for attribution.

## Bounded offline decoding example

Run with the capture filename as argv[1], after copying it locally or after exit:

```python
import json, struct, sys
from pathlib import Path
b = Path(sys.argv[1]).read_bytes()
assert len(b) == 524544
assert struct.unpack_from('<Q', b, 200)[0] == 1, 'initialization incomplete'
magic, version, capacity = struct.unpack_from('<QII', b)
assert (magic, version, capacity) == (0x3142534f5550474b, 1, 4096)
pid, freq, start, delay, eligible, dropped, skipped = struct.unpack_from('<7Q', b, 16)
print(json.dumps(dict(pid=pid, eligible=eligible, dropped=dropped, skipped=skipped,
                      totals=struct.unpack_from('<16Q', b, 72))))
for i in range(capacity):
    commit, event, tick, *values = struct.unpack_from('<16Q', b, 256 + i*128)
    if commit == i+1:
        print(json.dumps(dict(sequence=commit, event=event, seconds=(tick-start)/freq,
                              values=[hex(v) for v in values])))
```

Tests: build `graphics_observe_tests`, run that executable. Tests cover delayed
capture, concurrent publication and readers, overflow/non-overwrite, a suspended
producer hole, disabled/invalid opt-in, and Windows file-mapping initialization.
