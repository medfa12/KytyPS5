# Apple first-output Sync lost on repeated configuration — 2026-11-08

## Confirmed blocked transition

Parent-supplied `mapple3/readiness-prefix.txt` (SHA256
`42ad0bb5483a0c85897c202d5a53ed0e6b6a489c95b6253dac60bde2a1531778`)
contains 42 state records from a 75012228-byte prefix and two source lifecycles.
Its substring `source-admitted=4` counts both state and event records. Parent
production executable identity was
`048351953452fa10d6bc7d0f7081c5b20b71dfff1159f8e58056b84ce0e72739`.
Only this supplied numeric snapshot was inspected; runtime ownership is parent-side.

For source 65536, child 65537: guest Pause caller `0x940017e6e` sets flags=1.
Startup Flush establishes input-started=1, waiting-sync=1, selected generation=3.
The next HEVC configuration+23-unit batch advances decoder generation to 4,
with decoded=4, payload=14, held=1 and **sufficient=1**. ObserveReadiness then
clears waiting-sync while updating selected generation: flags remain 1,
timestamp=0, pending=0, eligible=0. Source 65539/child 65540 repeats the loss
with 27 units and payload=18. AC-3 queues reach four outputs, payload=27,
held=1, 48000 Hz stereo. First source retires about 11.0 seconds after that
video observation; the second behaves similarly. No readiness delivery/PCM
acceptance is established by the snapshot.

This supersedes the earlier possibility that four decoded frames meant
insufficient video input: these actual predicates are sufficient. The precise
missing producer is the pending first-output Sync consumed incorrectly by
host generation selection. Parent's screenshot was the Apple SIGN IN marketing
UI, not authenticated browsing; content/title and protection remain unknown.

## Native contract and host mismatch

Read-only disassembly rechecked regular 13.60 `libSceEsvmEngine.sprx`, SHA256
`95df64b49dedd751913b423106ff547ed6b56d81b356d3d27025e3cfa90f2b9b`:

* Source Flush calls underlying slot `+0x48` at `0x1e072`, checks success at
  `0x1e075/0x1e077`, and stores **source** first-output state `+0x1d8=1`
  at `0x1e088`. This is distinct from codec configuration/generation identity.
* Output-count consumer `0x226d7` tests that state for 1, `0x226e1` requires a
  positive new count, and `0x226e6` requires prior video count `+0x1d0=0`.
  It sets state=2 at `0x226f7`, calls Sync `0x199d0` at `0x22706`, stores the
  count at `0x2272a`, then calls the predicate at `0x22741`.
* Equivalent first-output completion helper `0x1b580` tests state=1 at
  `0x1b5a2`, positive count at `0x1b5af`, old count zero at `0x1b5b3`, then
  stores 2/calls Sync at `0x1b5c5/0x1b5d0`. Neither completion gate compares a
  host codec generation. The timestamp-side reset at `0x1b542..0x1b54b`
  clears only state 2, not an unfinished state 1.
* Sync invalidates remembered timestamp validity (`0x199d6..0x199e8`) and ORs
  flag 2 (`0x199ec`). It does not fabricate a timestamp. Pause independently
  ORs flag 1 at `0x19a36`; predicate threshold remains 12.

Reproduce these bounded slices with `tools/audit_esvm_readiness.py engine dis`
using `(start, bytes)` pairs `(0x1e065,0x33)`, `(0x226c0,0x86)`,
`(0x1b580,0x80)`, `(0x1b542,0x13)`, `(0x199d0,0x27)`.

Host `EsvmPacketDecoder::Consume` deliberately advances generation on every
accepted config, including repeated config with AUs. It rejects config-only
replacement over queued/held output unless the batch explicitly flushes.
That decoder storage identity is not a new source Flush and must not cancel
an existing source first-output request. The old source branch conflated the
two, unconditionally clearing `readiness_waiting_output` for generation change.
Prior regression expectations encoded that mistake and are corrected here.

## Narrow correction

`ObserveReadiness` still invalidates old tickets and completed Sync state on
generation replacement. It retains only an **already-pending** first-output
request for the **same selected child**, with prior observed output count zero.
Real new output then performs the existing Sync and predicate check. No change
to threshold, timestamps, decoder parsing, queues, authentication or DRM is made.
Different/failed selection and lifecycle invalidation retain their cancellation
paths. Reconfiguration after completed first-output Sync supplies no new arm.

## Meaningful validation and negative control

New real HEVC 10-bit fixtures use AC-3 config → Pause → HEVC config/startup
Flush → repeated HEVC config + 23 AUs. The unfixed source failed the new callback
assertion (exit 1), reproducing generation 3→4, waiting-sync 1→0,
decoded=4, held=1, payload=17, sufficient=1, timestamp=0, pending=0.
Fixture payload count differs from Apple due to different compressed content;
the source state transition is the same. Log retained locally as
`_Build/apple-readiness-validation/repeated-hevc-negative.log`.

With the fix, the same batch produces pending=4/eligible=1, followed by one
claim and genuine source callback, still timestamp=0 and EOS=0. Additional
checks cover config-only replacement awaiting later output, six units remaining
below threshold until more real AUs arrive, Play suppressing paused delivery,
polling without duplicates, and replacement after actual output retrieval not
rearming consumed Sync. Existing explicit Flush, failure, timestamp, callback
reentry/deletion, owner retirement and source resource tests pass.

Isolated build used at most two jobs, rebuilding source/wrappers/test TU against
read-only parent objects using the retained overlay link manifest. Test binary:
`dbf5446a8d52839560de33e1e536267f65af89c25ddd3509800a08ad51f49634`.
Focused `--media-framework-esvm-source-only` passed with trace 0 and 1.
An intermediate test attempted retrieval while paused; the fixture was corrected
to invoke real Play before retrieval, and both final runs passed.

```powershell
python -B tools/check_apple_readiness_trace.py _Build/apple-readiness-validation/source-trace-0.log _Build/apple-readiness-validation/source-trace-1.log --repeated-hevc
```

Checker passed: disabled=0, enabled=820; source saturation 64+1; original
blocked/EOS controls; all four repeated-HEVC variants and exact gen3→4 gate.
The extended drain fixture can reach its per-source diagnostic cap; lifecycle
assertions continue independently of trace saturation. No global cap occurred.

Parent is prioritizing VSH runtime diagnosis and owns later integration. This
fix closes a reproduced source-readiness bug; sustained Apple playback, PCM
acceptance and any remaining downstream failure are not yet runtime-verified.
