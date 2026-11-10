# Apple TV streaming and spontaneous abort — 2026-11-07

Worktree: `apple-stream-investigation`, based on `427d9daa`. All analysis uses
the completed `${USERPROFILE}/AppData/Local/Temp/opencode/mapple1` run, production
source `444ad5b`, executable SHA256
`1b9f09cece1acc4ba04b1eba111f56a81b95f6d0fccb8eb33cc6cc37c66861a6`.
No emulator/profile/input interaction or parent build writes occurred.

## Abort: demonstrated guest structure-layout defect

At guest.log:8491010–8491020, VirtualQuery succeeds for `0x217124000`, returning
the direct mapping `[0x217124000,0x21712c000)`, physical offset `0xc104000`,
protection `0x32`, memory type12 and name `mem_217124000-8000`. Immediately
afterward, at22:08:05.032, Apple's Gfx asserts that the first three name bytes
equal `mem`, at Gnmp.cpp:55. This is not an allocation failure or bad name
producer: the host log shows the correct name in its own struct layout.

Fresh disassembly of the actual Apple `eboot.bin` establishes the consumer:

- `0x1eafed6`: query output pointer = `rsp+0x20`; size `0x48` at `0x1eafedb`.
- `0x1eafef1`: call imported VirtualQuery, NID `rVjRvHJ0X6c`.
- `0x1eafef6..0x1eafef8`: test success, then branch to `0x1eaffbc`.
- `0x1eaffc3`, `0x1eaffca`, `0x1eaffd1`: compare bytes at `rsp+0x41`, `+0x42`,
  `+0x43` with `m`, `e`, `m`. The name therefore starts at result **+0x21**.
- `0x1eaffdc..0x1eb0095`: failed-name assertion at source line `0x37` (55),
  then the fatal helper. Successful validation proceeds to genuine direct
  memory release at `0x1eb00a2`, using physical offset result+0x10 and the
  page-rounded requested length. No assertion or release is bypassed.

The Windows host's eight `uint32_t` bitfields consumed a four-byte allocation
unit: name started at **0x24**. The old size-only assertion still passed because
both layouts round to72 bytes. Use a one-byte flag unit and assert name+0x21,
gpuMaskId+0x41 and reserved2+0x42 as well as the overall size. Windows bitfield
allocation rules require checking these byte offsets independently of size.

The regression allocates/maps a genuine named direct page, calls VirtualQuery,
reads flag/name/GPU-mask bytes at guest offsets, then releases the allocation
and checks that the mapping disappeared. It is included in the existing
`--media-memory-only` suite and full memory suite. Production memory.cpp logic
is unchanged; shared-file integration touches memory.h and the test driver.

### Validation

An isolated incremental overlay build passed, `-j2`, compiling memory.cpp,
runtimeLinker.cpp and VirtualMemoryAllocationTests.cpp with worktree headers.
These are every production/test-driver translation unit accessing this struct
in the selected test target; unchanged donor objects/libraries are read-only.
Local helper: `_TempData/build-apple-validation.py`; outputs:
`_Build/apple-validation/{build.log,virtual_memory_allocation_tests.exe}`.
`--media-memory-only` passed exit0, including raw guest-layout/name checks,
direct release and existing reservation/protection/commitment regressions.
This is not a clean full build or a live Apple acceptance result.
The same isolated binary also passed `--media-framework-esvm-source-only`
(exit0), covering real decoder/source/readiness and SDL PCM regressions with
the corrected memory ABI; log `_Build/apple-validation/media-source.log`.

## Streaming is a separate, still reproduced failure

- Initial JSUpdater TLS disconnect at21:58:54.046 (line114197) precedes later
  successful application activity; it does not establish that the later player
  failure is a general network outage.
- PlayReady access still fails at21:59:15.072 (line409083), public0x8a790010.
- Six source instances are admitted. Early previews decode AC3/HEVC; later
  episode attempts decode EAC3/HEVC without the former config rejection.
- All six attempts log `STATE_PREPLAYING TRACK_STATE_WAIT_FOR_READY`:
  lines606581,868150,6669549,6858036,7059873,7391464.
- No `source-ready-delivered`, `flush-complete`, `render-*` or
  `audio-pcm-accepted` trace occurs in this run. In particular, early source
  lifecycle tracing is far below its128-record cap. The retained trace does
  not record Pause controls or detailed queue/arming observations, so this
  cannot identify a hidden-HEVC threshold failure versus missing controls.
- Episode requests at22:06:02.407,22:06:15.835,22:06:27.608,22:06:48.186 name
  `umc.cmc.5sc6v0g2nj8tk7nvyc7wejees`, rendition
  `umc.cmr.atvp.ep.1ytgv5prmixubsqhi6c3nuv3b` (Stick pilot, previously identified).
  Native MediaKeys creation fails twice per attempt with0x8a790004 at
  22:06:12.228/.390,22:06:22.623/.750,22:06:35.831/36.435,
  22:06:55.107/.154. These reproduce the native-handle lookup failure after
  unsupported access, before a real license-session backend is reached.
- The user's troubleshooting result, “No problems could be detected. Try again
  later,” is not playback or license success. No PlayReady backend exists.

Remaining gates: trace initial app/native source-control ordering and missing
Flush/readiness observations; retain conservative native arming/queue semantics;
parent-coordinated executable rebuild and Apple retry after the user-owned VSH
slot is released; verify the Gfx release no longer aborts, then actual clear
preview video/audio and protected-content failure separately. Protected playback
still requires a genuine CDM/license/decryption provider.

## Follow-up: internal initial-configuration Flush producer

The app's initial successful preparation tail at `0x4903c..0x49087` sets volume,
calls **Pause** (`+iomwyWJU4o`, `0x4904f`), changes its own state to1 and notifies
its listener. It does not call public Flush there. The public Flush import
`w2N939ABS2Y` is called on the separate seek path at `0x49be3`. Native Esvm
Pause `0x17e30 -> 0x25c20 -> NlSpa23Wdh0` forwards Pause without adding Flush.
This explains why looking only for public Flush misses the startup path.

A newly recovered engine path supplies a real internal Flush request:

1. HEVC wire subtype2 dispatches through table `0x35fd54` to `0x2a1c18`, then
   reader `0x2a2050`; its callback is reader+0x18. The submission callback
   functor table `0x3662b8`, slot+0x10=`0x21860`, forwards to HEVC setup
   `0x2b960` at `0x2189e`. That setup copies the three parameter-set groups at
   `0x2bbbc..0x2bbf7`, admits configuration via `0x283730` at `0x2bc85`, and
   only on success records codec6 via `0x2a1a0`, then calls `0x2b8a0`.
2. Helper `0x2b8a0` rejects when source byte+0xac is already set. Otherwise it
   checks every entry in the registered stream vector+0x68..+0x70 using
   `0x187f10(entry+8)`, i.e. entry's valid-codec byte+0x10. `0x2a1a0` writes
   that valid-byte/type pair from `0x18d230`. This is an **all-stream setup
   barrier**, not an unconditional notification for each codec config.
3. Once all are valid, `0x2b8fd..0x2b923` constructs and dispatches message
   **0x2d** with null payload through the source observer list. `0x1872f0`
   invokes each registered observer's vtable+0x10.
4. Engine construction installs observer table `0x365d08` at `0x1a20a`;
   its+0x10 callback is `0x1c9d0`. That callback recognizes0x2d at
   `0x1ca14..0x1ca2a`, then calls the real queued Flush request `0x1b070` at
   `0x1ca7b`, with null timestamp argument. The message consumer `0x1ff00`
   reaches **`0x1dfd0`** at `0x1ffed`, the previously audited successful Flush
   which arms first-output synchronization at `0x1e088`.
5. After notification, `0x2b933` calls `0x2d690`: it marks each stream's+0x5c,
   sets source+0xac to1 at `0x2d6e1`, and queues further source work. Source
   construction initializes+0xac to0 at `0x2a2aa`. Other codec setup paths
   contain equivalent0x2d producer/gating sequences.

This extends the prior readiness audit: construction itself does not arm
Pause/Sync, and arbitrary later configuration is still not a Flush. However,
**initial all-stream configuration can produce an explicit internal Flush**.
The HLE source has no equivalent barrier/producer; only the externally exported
EsvmFlushContext currently requests first-output sync. That is a concrete
startup-readiness gap independent of hidden HEVC reorder counts.

Implementation is not yet changed for this path. The native AU/input lifecycle
around the barrier must be preserved before doing so: the actual first source
already accepts/decodes one audio AU (line554614) before video configuration
(line554675). Calling the current FlushCurrent over every child at that point
would erase admitted audio. Simply arming Sync after config would bypass the
real Flush contract. The remaining trace must close source+0x5c admission,
queued input handling and Flush completion ordering (including the message
queued by `0x2d690`) before adapting that barrier. Explicit Pause and successful
Flush remain required; no relaxed threshold or fabricated success is added.

Reproduce bounded engine slices with `tools/audit_esvm_readiness.py`:

```text
engine dis 0x2b8a0 0xc0
engine dis 0x2bc70 0x40
engine dis 0x1c9d0 0xb0
engine around 0x1ffed 0x90
engine dis 0x2d690 0x150
```

`tools/summarize_apple_media_log.py <guest.log>` provides a streaming read-only
counter/hash summary without dumping URLs, access units or credentials.
Retained log identity:292742700 bytes,8547517 lines, SHA256
`57d1622af18bfbf722c4ceaac2f87a36ef3d9c9c299005be468d80f0f0505f4a`.
Apple eboot SHA256:
`6f2a81bd687e4415a41f00c5b26059eaffb4ae5cd684eacc5c7d8ecea6f8f768`.
At that checkpoint the memory-layout regression was the only code correction
in this branch; no live playback or Gfx retry was authorized/performed.

## Parent integration validation

Integrated as `99627b31` / `1d22d8aa` on ps5-vsh alongside the Netflix
Videodec2 resource correction. Normal-build `virtual_memory_allocation_tests`
compiled/linked successfully with parallel2. Direct `--media-memory-only`
passed, including raw guest-layout allocation/query/release coverage. CTest
`videodec2_resources` (0.12s) and `media_framework_esvm_source` (2.51s) passed
2/2, total2.68s. The memory-header change caused the dependent production
translation units in this test target to rebuild normally.

The user-owned VSH executable remains untouched; production emulator relink
and actual Apple Gfx retry remain pending. The agent continues tracing and
implementing the internal startup Flush producer with admitted-input lifetime
preservation. No new streaming implementation or live success is claimed here.
## Follow-up implementation: initial input ownership and Flush

The preceding implementation blocker is resolved for the observed **separate
configuration submissions**. Native does not accept the early audio AU seen in
the HLE log and then preserve it across startup Flush: it withholds permission
to submit that AU until every registered stream has a valid codec.

The retained run confirms both children already existed: source65536 at
line553683, video65537 at553919, audio65538 at554219. Audio's separate24-byte
subtype5 (AC3) configuration was submitted at554252 and the premature HLE
refill at554271, before the1608-byte AU at554612 and separate169-byte subtype2
HEVC configuration at554675. This is not a late registration of another child
after a legitimate single-stream startup.

### Native admission, credit and completion evidence

- Engine `0x2ab90`, at `0x2abb3..0x2abbf`, returns **0x81780002** unless
  source+0xac is1. This precedes allocation/copying. Callback `0x21570` stores
  that error at `0x215b2..0x215b6`; only success sets its accepted-input byte at
  `0x215bc..0x215c0`.
- Submission consumer `0x210ec..0x21101` combines parser/callback results.
  Failure takes `0x2119b..0x211d8`, completing with the error. Only successful
  accepted input takes the deferred-completion path `0x21107..0x21378`.
  This is not a successful ownership transfer or a backend retry on rejection.
- Source `0x2d690` marks outstanding-refill byte stream+0x5c, sets source+0xac,
  and queues table `0x367090`. Its consumer `0x31e30` revalidates the source
  handle and calls each stream callback at `0x31eed` with stream handle, event0,
  zero payload and its stored user pointer. `0x2a000` suppresses subsequent
  refills while stream+0x5c or EOS byte+0x5d is set; `0x2a150`/`0x2a160` set
  and clear the outstanding credit. This byte is not an AU-retention queue.
- Esvm callback `0x253a0` turns event0 into the handle/event1 message at
  `0x253de..0x253f2`. Consumer `0x1e740` validates stream and event, ignores an
  EOS-closed stream, atomically changes client stream+0x58 from0 to1 at
  `0x1e7a8`, and queues the public input notification through `0x11f90`.
- Esvm AU submission `0x15680` claims that client credit at `0x15fa1` (1 to0),
  returning **0x8a79000b** if none exists. It invokes serialization/submission
  only after claiming it at `0x15fae..0x15fce`. Failure restores the credit at
  `0x15fdc`. There is no successful pre-barrier AU ownership to flush away.
- The AU functor `0x21850` serializes through `0x2d820`, calls wrapper
  `0x259d0` at `0x218cc`, and resets the serialization arena via `0x2e2b0`.
  `0x259d0` submits WBBO8yQhDP4 with completion `0x25850`, then consumes its
  result through `0x25500`. Completion forwards to `0x293e0`, which resolves
  the registered completion object and calls `0x295b0`; that function stores
  the result and signals it. `0x25500` retrieves the result at `0x25678..0x2568c`
  and returns it at `0x2579b`. The caller's failure path restores the credit;
  it does not convert rejection into successful payload consumption.

### Queued Flush worker

`0x1eca00` checks `0x24c510(source+0x1b8)`. When ready it calls `0x1e96e0`
at `0x1eca49`; otherwise it owns a deferred functor in source+0x308/count+0x310.
`0x1e96e0` checks source+0x260 interface slot+0x30, calls virtual+0xd0, resets
state via `0x24c5b0`, invokes virtual+0x90, invokes pipeline slot+0x40 with the
timestamp, queries the sequence and calls `0x1eec40`. It emits notification1,
subtype8 carrying the result at `0x1e97aa..0x1e97ef`, including failure.
The regular source vtable `0x37a870` resolves +0xd0 to the return-zero function
`0x1e90c0` and +0x90 to `0x269240` (calls `0x1ee840`, then `0x24c5b0`).
The engine's successful Flush path arms its first-output request, **not Sync
or readiness itself**. These observations do not justify erasing accepted
host AUs or inventing renderer-clear acknowledgement.

### Host implementation and supported scope

- A source-owned one-shot `input_started` barrier gates refill delivery and
  packet AU admission. The packet parser rejects AU-bearing initial batches
  before any state mutation or payload copy. The source completes them with
  0x81780002. It does not retain guest mapping pointers after completion.
- Successful configuration checks all live children. Before initial Flush it
  verifies that there are no established input sequences, packets, decoded or
  held frames, EOS, staged output or output leases. Only then does it actually
  call `FlushCurrent` on each configured decoder. Thus no accepted audio/video
  can be discarded by this path. All operations remain under the owner gate.
- First-output synchronization is requested only after successful resets and
  binding their generations. An actual video output transition still supplies
  Sync; explicit Pause and the original readiness predicate remain required.
- Initial Flush completes before configuration completion can reenter and
  before startup credits dispatch. Guest callbacks use the existing bounded
  ticket dispatch, with owner/epoch/child revalidation after each callback.
  Output selection is deferred until the initial barrier; an earlier Render
  retains geometry without creating an output lease that would require a
  fictitious renderer-clear acknowledgement.
- Once started, later configuration does not repeat the all-stream Flush.
  Normal generation discontinuities still revoke obsolete readiness requests.
  An already accepted audio AU/PCM remains owned across video reconfiguration.
- The native client routes configuration and AUs through separate serializers
  (`0x20fb7` versus `0x21850`). This implementation covers that observed path.
  A direct combined configuration+AU submission before startup is rejected
  atomically, including its configuration; intra-batch partial native admission
  and queued Flush interleavings are not emulated. Standalone packet-decoder
  callers retain their existing combined-batch behavior by default.

No live emulator launch, controller input, profile mutation or parent build
write was performed. Live clear-preview readiness, rendered video and audible
PCM remain acceptance checks for a parent-coordinated run. Hidden HEVC reorder
counts are still not inferred, and protected playback still requires a genuine
CDM/license/decryption backend.

### Verified implementation checkpoint

- Isolated overlay build passed with **at most two jobs**, recompiling
  `mediaFrameworkEsvmSource.cpp`, `mediaFrameworkEsvmDecoder.cpp`, the full source
  test driver and standalone packet-decoder tests against this worktree's
  headers. Donor graphs/objects/libraries were read-only inputs; output is
  `_Build/apple-validation/`. The local overlay helper is a runtime artifact.
- `virtual_memory_allocation_tests.exe --media-framework-esvm-source-only`:
  **exit0**, final log `media-source-startup.log`. Tests cover AAC/video in both
  configuration orders, failed final configuration, atomic early-input rejection,
  exact startup credits, accepted real AAC retained through completion reentry
  and later video reconfiguration, completion deletion, first-refill deletion
  suppressing the second child, and real AC3-before-HEVC startup with short HEVC
  EOS and reentrant owner unregister. Existing paired retirement, renderer-clear
  failure/success and real SDL PCM backpressure/EOS tests also passed. The source
  suite acquired actual SDL sinks; this is not Apple playback acceptance.
- `media_framework_esvm_decoder_tests.exe`: **exit0**,
  `media-decoder-startup.log` ends `MediaFrameworkEsvmDecoderTests: passed`.
  Standalone packet callers retain their default admission behavior.
- `git diff --check`: passed. An initial source run reached the old suite-wide
  assertion that no readiness event had yet occurred; the new readiness-producing
  fixture was moved after that assertion. Both the existing assertion and the
  new startup checks now pass; no readiness expectation was weakened.

Still unverified: this change in an actual coordinated Apple clear-preview run,
the integrated memory-layout fix's live Gfx acceptance, and any subsequent
hidden HEVC-reorder contribution to the full-length readiness threshold.

### Parent startup integration validation

Integrated as `6a88b336`, retaining the earlier parent memory-ABI validation
notes. Normal-build source test driver and standalone decoder target passed
with parallel2. CTest source (3.15s) and decoder (0.13s) passed2/2, total3.35s.
Production emulator relink and live clear-preview/Gfx acceptance remain pending.
