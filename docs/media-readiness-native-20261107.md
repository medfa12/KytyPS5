# Native ESVM readiness audit — regular 13.60

## Checkpoint: event translation recovered

Read-only firmware inspection establishes the previously missing readiness
translation. Engine event `7` is **not** directly the MediaFramework source event.
Citron's `0x2000` and the application's source `0x2000` belong to different
interfaces. The media server translates the former to source `0x2001`.

All addresses below are module-relative virtual addresses. The helper is
`tools/audit_esvm_readiness.py`; its external ELF inspector and firmware paths
can be supplied with `--inspector` and `--firmware`. `dis` takes a byte length,
not an end address. `near` finds only a candidate prologue, not a proven boundary.
No guest execution, emulator input, debugger attachment, or firmware writes were
used.

### Image identities (SHA256)

| Module | SHA256 |
| --- | --- |
| libSceEsvmEngine.sprx | `95df64b49dedd751913b423106ff547ed6b56d81b356d3d27025e3cfa90f2b9b` |
| citroncore.elf | `7ca3d010867a839f59e212e5e9e7cbde4ec92fb3499190821dabd94ad46292f6` |
| libSceVideoCoreInterface.sprx | `51903e06f307e39729d00ddd9f1b51a935767c69a9a7dc86475eb70e0c2aff51` |
| system_b/vsh/app/NPXS40039/eboot.bin | `9781fec935ce6a53087ea6919a6a9db6fc78c5facf2e8416731498ca0fe1b14f` |
| libSceMediaFrameworkInterface.sprx | `fc380ac0e792b13b19191154ed9de00f1690b2b168434664c71c09f692a7c270` |
| libSceEsvm.sprx | `31593167e10813bdc195fb099da1da723c36818c063c35de6f6a086962d2e3f2` |

### Callback chain

1. Engine `0x1e400` calls its registered callback with handle at parent `+0x60`,
   event `7`, null payload, and user at `+0xa8`.
2. Citron registers `PlayerEventHandler=0x204e0` at `0x203ac`. The handler
   preserves the event; message constructor `0x330a0` writes it at `+0x10`.
   Consumer `0x3422c` forwards that field to `0x18680`.
3. Citron jump table `0x145b00`, index `7`, targets `0x186fe`, which selects
   VideoCore event `0x2000`. `0x18741` calls the callback stored at `+0x2940`
   with the source handle at `+0x2950` and null payload.
4. Citron constructor `0x15280` copies this callback from its supplied descriptor.
   VideoCore `0x8f00` supplies callback `0x8f60` to the registered source creation
   function. `0x8f60` forwards the event unchanged to `0xfc00`.
   `0xfc4e` writes it into the outgoing record, and `0xfe4b` sends the 48-byte
   record through IPC. There is no `0x2001` conversion here.
5. **Media server** `0x14ee98` compares the incoming event with `0x2000`.
   `0x14eecc` calls descriptor slot `+0x3d0` with the source handle.
   Registration `0x8f6d7` stores callback `0x97320` at table offset `0x2a8`;
   `0x143dab` copies the table to object `+0x128`, yielding `+0x3d0`.
6. Server callback `0x97320` constructs internal event `0x1012` at `0x9735d`
   and submits it through the owner's `+0x48` callback. Server dispatcher
   `0x1168a0` reads the internal event. Table `0x1c6fc0`, index `0x12`,
   targets `0x117e4a`. That branch writes source event **`0x2001`** at
   `0x117e6f` and sends it to registered recipients through `0x11d9c0`.
7. MediaFrameworkInterface `0x2723b` recognizes `0x2001`; `0x27160` dispatches
   it unchanged through `0x39960`.
8. Esvm source handler `0x28940` maps source `0x2001` to public event `7`
   (`0x28a73`). By contrast, source `0x2000` takes the separate `0x28b86` path:
   query `aOqAkEUTuAw` for 16 bytes and emit public event `6`.

Reproduction slices:

```text
python -B tools/audit_esvm_readiness.py server dis 0x14ee8d 0x45
python -B tools/audit_esvm_readiness.py server dis 0x8f6c1 0x30
python -B tools/audit_esvm_readiness.py server dis 0x143dab 0x14
python -B tools/audit_esvm_readiness.py server dis 0x97320 0x65
python -B tools/audit_esvm_readiness.py server dis 0x116aa9 0x20
python -B tools/audit_esvm_readiness.py server dis 0x117e4a 0x70
```

### Readiness predicate (field names corrected by producer tracing)

Engine readiness object fields recovered so far:

```text
+0x08 count          +0x10 input_count (NOT a configured capacity)
+0x18 count A        +0x20 count B
+0x28 allocation state +0x30 free_bytes (NOT bytes in use)
+0x38 local state    +0x58 EOS-like flag
+0x60 threshold (default 12)
+0x68 buffering state machine
```

With `sum = count A + count B`, the predicate is:

```text
count != 0 && (
    sum + count >= threshold ||
    (sum >= input_count && allocation_state == 1 && free_bytes < 0x900000) ||
    (sum >= input_count && flag58))
```

`0x19760` stores count, checks local state `3`, and on success changes it to
`1` and invokes the parent readiness callback. `0x19820` updates input count;
`0x19880` updates A/B from notification kinds 1/2. `0x19900` updates free bytes
for pool kind 2 and allocation state for pool kind 4. `0x19b50` sets flag58 and rechecks.

Engine message consumer `0x225f0` supplies the decoded count from message kind 4.
Low-level notification 32 becomes engine message 40 (`0x18b984`), then
`0x1ce5b` / `0x23350` deliver counts to `0x19880`. Producer tracing below
establishes their units; neither first-frame readiness nor an unconditional
twelve-output-frame threshold follows from this predicate.

Initial producer trace (subsequent closure follows below):

- `0x1a94e0` builds low-level notification 32 with kind from observer `+0x30`
  and count **`rsi + 1`**, gated by observer bytes `+0x34` and `+0x35`.
  `0x1a9580` and `0x1a9620` construct the same notification without that
  increment; callers for those variants have not been established.
- `0x1cbb50` obtains `rsi` from the object at `+0x558`, vtable slot `+0x68`,
  then tails the observer. Constructor `0x1ca990` creates that observer with
  kind **2** (`0x1cab36` / `0x1cab3e`). This gives a concrete producer path
  for the readiness object's count B.
- The `+0x558` object comes from factory `0x1ce370`. One factory branch
  constructs `0x1cec60` (`0x1ce8a3`), whose vtable is `0x3732b8`.
  Slot `+0x68` is `0x1d22c0`.
- `0x1d22c0` walks a tree at `+0xf8`, derives an identity from each entry's
  `+0x28` object via `0x1ae1c0` and `0x189070`, and counts **distinct**
  identities using a temporary list. It does not simply return tree size.
  The selected HEVC branch and timestamp meaning are established below.

The host adapter's narrower observations and remaining timing differences are
recorded below.

### Subsequent producer recovery and first implementation

The following supersedes the earlier candidate interpretations above:

- **Selected HEVC backend:** engine initialization at `0x1a7f4` copies the
  vector at `0x355870` (`1,4096,2176,0xffffffff`) to its stack config `-0x130`.
  The config base supplied at `0x1a8bb` is `-0x478`, making this public field
  `+0x348`. `0x18a8fe/0x18a907` copies public `+0x340` to native `+0xa8`, so
  public `+0x348` is native `+0xb0`, with selector **1**. HEVC constructor
  `0x1c63a0` forwards that field through `0x1ca990` to `0x1ce370`; selector 1's
  regular branch constructs `0x1cec60` at `0x1ce8a3`. This is the branch whose
  `+0x68` method is the distinct-timestamp count `0x1d22c0`.
- **Timestamp units:** `0x188ff0` normalizes value/timescale to microseconds;
  `0x189070` returns the normalized value at `+8`. `0x1ae1c0` obtains the first
  metadata-key-0 timestamp and uses that conversion. Missing metadata gives
  zero. The type-2 packet metadata producer `0x1f22c0` copies its timestamp
  tuple from `+0x28` into a list entry at `0x1f246e..0x1f247d` and calls
  `0x1f2b00` with key **0** (`xor edx,edx` at `0x1f2465`). That helper stores
  the supplied key at `0x1f2c90..0x1f2cbe` and inserts through `0x1ab220`.
  Wire reader `0x2a230a` copies AU `+8` to public `+0x18`; admission `0x2adb6`
  copies that PTS to allocation `+0x48`, and `0x2adc8..0x2add2` writes timescale
  **1,000,000** at `+0x50`. The metadata object starts at allocation `+0x18`,
  making these the producer's tuple value `+0x30` and timescale `+0x38`.
  Thus key 0 carries the AU's microsecond PTS, not a decode ordinal. No host
  accepted-minus-output counter is substituted for the native identity set.
- **Buffered input A:** decoder observer `+0x98`, slot `+0x28`, is the thunk
  `0x1a7af0` to notification-32/kind-1 producer `0x1a7a30`. The buffered AU
  implementation calls it at `0x1b072b` with list size `+0x1b8`, after inserting
  the staged AU at `0x1b0697..0x1b06c0`. Removal decrements at `0x1af05b` and
  notifies at `0x1af0b7`. It is a buffered-unit count, not a byte count or a
  decoded-frame count. The regular source's native prefetch setting comes from
  public `+0x430` (`0x1a8a4`, value 50), copied to native `+0x3a0` by `0x18a992`.
- **Input count:** `0x1a7740` sends notification 4/subtype 2 for video. Its
  supplied count is obtained through the AU queue's vtable `+0xb8`, e.g.
  `0x1ac999..0x1ac9cd`. The plain queue method `0x1ae6d0` returns list size
  `+0x48` plus the nonnull active element `+0x28`. The buffered override
  `0x1b0120` additionally includes `+0x1b8` and active prefetch byte `+0x1c8`.
  Thus the previously named **limit is not decoder capacity**.
- **Output:** `0x1cd350` sends notification 4/subtype 4 from output-list count
  `+0x190`. Enqueue `0x1cdc70` increments; dequeue `0x1cdba0` decrements; flush
  `0x1cd2e8` clears it. Engine `0x22741` delivers this count to `0x19760`.
- **Allocator state:** notification 5 is constructed at `0x199272` and
  `0x1995fe`. Allocation request `0x199010` compares requested bytes against
  capacity `+0x60` minus usage `+0x68`. Deferred requests are enqueued and set
  state 1; successful allocation sets state 0. Release `0x199530` subtracts
  usage and retries waiters. The notification's byte value is **free space**.
  The readiness observer uses pool-kind 2's free bytes and pool-kind 4's state.
  Pool kind 2 is constructed at `0x18fd46..0x18fd73`; native initialization
  supplies 18 MiB via public `+0x2d0` (`0x1a81d`). Decoder pool kind 4 is
  registered at `0x1cb08c` and constructed from backend memory requirements
  at `0x1cb627..0x1cb678`. A full host four-frame queue does not establish
  either of these native allocator observations.
- **Rate:** `0x19ae0` compares with 1.0 (`0x355c08`); normal rate always selects
  12. The alternate scaled branch uses 12.0 (`0x355cc8`). The host service
  already rejects non-unit rates, so no alternate-rate threshold is invented.
- **Arming:** native Pause ORs flag 1, synchronization ORs flag 2, successful
  readiness consumes flag 2, Play clears both, and changing the remembered
  normalized timestamp clears them. After flush success, `0x1e088` requests
  first-output synchronization; the zero-to-nonzero video transition at
  `0x226d7..0x22706` supplies it. Constructor and initial Flush closure below
  supersedes checkpoint `2bec4edd`'s implicit admission Pause/first-generation
  synchronization: neither is justified by native construction.
- **Input-end path:** `0x1e9db0` emits notification `0x1b`, state 1, following
  `0x24c8b0`; `0x26b2d8` invokes it on reader result `0x81781028`. Engine
  `0x1d432..0x1d470` selects the video stream and state 1, then `0x235d9` sets
  flag58. The output queue's separate state-2 notification (`0x1cdefa`) is
  not this readiness arm. The direct wire path is now closed independently of
  the reader-result path: subtype 0 dispatches to `0x2a2270` (jump table
  `0x35fd54`), then `0x2a22c0`. At `0x2a232c..0x2a2330`, AU byte `+0x18`
  becomes public byte `+0x22`. Callback `0x21570` forwards to `0x2ab90`;
  after successful admission, `0x2b0a5..0x2b0ba` tests that byte, closes input
  admission (`+0x5d = 1`), and calls `0x2839f0`. Its slot `+0x90` targets
  `0x269ee0` in vtable `0x37ad98`, installed by constructor `0x267b00` and
  attached to the public source by `0x267bf4` / `0x283520`. The thunk subtracts
  `0x368` and jumps to `0x1e9db0`. **Wire EOS arms flag58 at input admission,
  without waiting for decoder EOF.** Public byte `+0x23` (wire `+0x19`) instead
  uses `0x283a60`, slot `+0x98`; it must not be confused with the EOS path.

Implementation now includes the native predicate/local-state model in
`mediaFrameworkEsvmReadiness.h` and source delivery of `0x2001` with null payload
and the copied user pointer. Delivery is bounded to one source ticket per
eligible guest entry, shares the refill reentrancy gate, and revalidates owner,
PID, epoch, source selection and decoder generation after refill callbacks.
Play, flush, reconfiguration, source deletion and owner retirement prevent
stale delivery. No scheduler thread calls the guest.

The first host adapter deliberately uses a **lower bound** for the normal-rate
sum: actual decoded output, payload-bearing copied input AUs, and the single
already-received codec frame held on output backpressure. Hidden FFmpeg reorder
state is not estimated. A new `queued_access_units` status excludes EOS and
flush-only markers, including EOS-with-payload's two-stage send. The allocator
fallback is disabled until corresponding pool accounting exists. Short-input
readiness requires accepted wire EOS and decoded video, with the native input
coverage check. Requiring codec EOF instead would deadlock a short stream when
the paused output queue prevents the codec from consuming its EOS marker. This is not a
claim of complete native timing equivalence or verified resolution of Apple4.

Written tests cover predicate boundaries, stale/rearmed tickets, repeated
controls, actual SHM/VP9 twelve-unit admission, completion reentry canceling a
notification, deletion/unregistration from the source callback, and real HEVC
10-bit SDR/EOS plus protected-input rejection. A six-frame VP9/EOS regression
checks the real four-output/one-held-frame backpressure state and verifies
readiness before codec EOF. Decoder rejection/flush tests now check exact
payload-AU accounting too. Parent built checkpoint `2bec4edd` with
`_TempData/build-media-validation.py virtual_memory_allocation_tests.exe media_framework_esvm_decoder_tests.exe`
and reports both the decoder executable and `--media-framework-esvm-source-only`
passed with exit 0, including those readiness tests. These are isolated overlay
results; they do not validate the subsequent corrections below.

### Arming and output-timestamp review correction

- **Construction does not Pause.** Readiness flags initialize to zero at
  `0x19711`, as does first-output state `+0x1d8` at `0x1a3cd`. Constructor tail
  calls configuration at `0x1a48d`, then `0x1a8f0` at `0x1a495`. That function
  queues message vtable `0x365ff8` (`0x1a936`); its `+0x10` consumer `0x1fb20`
  calls underlying source Play (`+0x20`, `0x1fbb9`) and readiness Play
  `0x19b90` at `0x1fbc6`. Explicit Pause uses `0x1afa0`, vtable `0x366038`,
  consumer `0x1fcf0`, source `+0x30`, and readiness `0x19a00`.
- **Flush creates the first-output request.** `0x1dfd0` resets input end via
  `0x19980`, then calls underlying source `+0x48` at `0x1e072`. Only success
  sets `+0x1d8 = 1` at `0x1e088`. A zero-to-positive output transition changes
  it to 2 and calls Sync (`0x226d7..0x22706`; equivalent helper
  `0x1b580..0x1b5f5`). A later codec configuration is not itself a Flush.
- **Initial Flush does not require an AU sequence.** The constructor creates
  the underlying source with `0x189f50` (`0x1a2fc`, stored at `+0xe0` by
  `0x1a30b`). Its vtable `0x36d5b0` has Flush `0x18ac50` in slot `+0x48`.
  That calls `0x190eb0`, which checks constructed pipeline objects and invokes
  wrapper `+0x58`, rather than checking an AU sequence. Wrapper factory
  `0x287890` installs vtable `0x37cdc0`; slot `+0x58` is `0x288b30`, forwarding
  to current state `+0x50`. Initialized states 2/3 use `0x28a9f0`/`0x28afd0`
  (tables `0x37d8d0`/`0x37dab0`), both forwarding through `0x287700` to the
  regular source's `+0x40`: `0x2691b0` -> `0x1e8870`. This queues the normalized
  Flush timestamp without reading an AU sequence. Pre-initialization states
  0/1 instead reject through `0x28a140`; host source admission has already
  validated/copied the source setup, distinct from child codec configuration.
  After success the engine can return without a video stream (`0x1e0e4`), or
  query timestamp/sequence through `+0x110` (`0x1e13c`). The query is subsequent
  to arming, not a nonzero-sequence prerequisite.
- **Timestamp is output retrieval.** `0x1b7b0` calls underlying video retrieval
  `+0xf8` (mode 0, `0x1b8f2`) or `+0x100` (mode 1, `0x1b8c5`). Valid successful
  output queues message vtable `0x366138` at `0x1baef`. Its consumer `0x205a0`
  calls readiness Timestamp `0x19a70` at `0x20648`. The message's normalized
  timestamp is the same value returned as public output `+8` at `0x1bc62`.
  Exports `SSUYPtShdAM` and `B627rbCsTAs` call the retrieval at `0x27c79` and
  `0x27dfc`; citron wraps them at `0x15db0` and `0x16110`. No input-admission
  or presentation-acknowledgment timestamp is substituted. Pause and Sync
  discard remembered timestamp validity before setting their respective bits;
  differing subsequent retrieved values clear flags (`0x19a70..0x19aac`).

Host changes implement these findings as follows:

- Admission leaves readiness unarmed. The host playback clock still waits for
  guest Play; it does not supply an implicit readiness Pause.
- Only successful source Flush requests first-output synchronization. The
  request survives initial child/configuration binding. **Correction from
  mapple3:** an unfinished request also survives same-child generation
  replacement before its first output; cancelling it here was a host bug.
  Failed/different-child selection and lifecycle invalidation still cancel it.
  An already-completed Sync never becomes a new request on reconfiguration.
  See [the reproduction and native contract](apple-startup-sync-20261108.md).
- `FlushCurrent()` clears actual queues/codec buffers and advances generation
  while preserving configuration and the actual sequence-validity state. It
  permits an unconfigured child or a configured child before its first AU.
  Explicit packet `Flush(new_sequence)` retains its existing strict contract;
  neither operation hides sticky decoder failure.
- The host's renderer-clear completion remains stricter than native CPU Flush.
  First-output requests are armed after successful host completion, protected
  by a readiness revision across the unlocked renderer wait. This deliberately
  does not reproduce native asynchronous timing during that wait or retroactively
  Sync an already-observed nonempty queue. Failed completion does not arm.
- `PublishDue` and `TakeEsvmDecodedFrame` share `PopReadinessFrame`, observing
  post-Pump count before dequeue and recording actual microsecond output PTS
  afterward. Owner validity, selected child and generation are checked. Frames
  without known PTS do not provide a fabricated timestamp. Staged retries and
  presentation acknowledgments do not repeat the observation. Worker paths can
  revoke/defer tickets but still never invoke a guest callback.

The test formerly named `NativeReadinessReplay` is now `ReadinessPolicyModel`:
it exercises a host model of disassembled semantics, **not firmware execution**.
New source regressions use real VP9 decoding to cover no implicit admission arm,
Flush before child/configuration/sequence, explicit Pause, reconfiguration,
failed renderer-clear Flush, and equal versus advancing retrieved PTS through
both diagnostic transfer and the mailbox publisher. The publisher case uses
its full mailbox/staged slot without any simulated presentation acknowledgment.
Decoder regressions cover unconfigured/configured source Flush, preserved
sequence, cleared EOS/ownership, subsequent real decoding, and sticky failure.
These new changes/tests await parent-owned build and execution.

Reproduction slices for the additional closure:

```text
python -B tools/audit_esvm_readiness.py engine dis 0x1a330 0x170
python -B tools/audit_esvm_readiness.py engine dis 0x1dfd0 0x250
python -B tools/audit_esvm_readiness.py engine table 0x36d5b0 0x138
python -B tools/audit_esvm_readiness.py engine dis 0x18ac50 0x130
python -B tools/audit_esvm_readiness.py engine dis 0x190eb0 0x100
python -B tools/audit_esvm_readiness.py engine table 0x37d8d0 0x60
python -B tools/audit_esvm_readiness.py engine dis 0x1e8870 0x120
python -B tools/audit_esvm_readiness.py engine around 0x1baef 0xc0
```

### Validation and remaining work

- The helper's bounded inspection commands ran successfully against these images.
- `git diff --check` passed (line-ending normalization notices only).
- Parent validated `2bec4edd` successfully as recorded above. This subsequent
  arming/timestamp correction is implemented and awaits its own overlay build.
- No build/test run by this readiness session; parent owns build coordination.
- Review fixed a completion-order bug: synchronization is recorded before the
  completion callback, so a reentrant Play clears that output's arm instead of
  letting deferred observation reintroduce it afterward.
- Retained Apple4 log review found one 37,165-byte video submission for decoder
  65537 between configuration and the first WAIT_FOR_READY, ending with four
  queued frames. The log does not expose that batch's AU count/EOS flag; it
  cannot establish whether the conservative host predicate reaches 12.
- The conservative HEVC lower bound can still stall a non-EOS batch whose
  visible outputs/copied AUs/held frame sum to less than 12 while the remaining
  native-counted identities are hidden in FFmpeg. The retained log does not
  distinguish that case from sufficient buffered input, and does not prove
  the application's required Pause/Flush ordering reached this adapter.
  No threshold relaxation or inferred reorder count is justified by that log.
- Remaining work: coordinated build/source/decoder suites for this correction
  and live workload acceptance when authorized. No new emulator run was made.
- No PlayReady backend or verified playback success is established by this audit.

### Parent integration validation

The corrected `2cfc9d75` isolated overlay build and decoder/source-readiness
suites passed. The complete audit/implementation series is integrated into
`ps5-vsh` through `384b9791`. The normal-build decoder and virtual-memory test
targets also built successfully; CTest source and decoder suites passed 2/2
(3.16s and0.12s). These results supersede the pending-build notes above.

The emulator executable was not rebuilt/restarted while the user's Netflix
session was running. Live Apple acceptance, conservative hidden-HEVC-reorder
coverage and protected playback remain unverified.
