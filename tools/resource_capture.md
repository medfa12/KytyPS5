# ResourceArbitrator capture (13.60)

`resource_capture.py` collects bounded read-only component/state evidence through
a supplied reader. `capture_ssl_boot.py` supplies local process admission and
ReadProcessMemory; the helper itself does not authenticate a process.

## Native profile and layout

SHA256 identities:

- `SceShellCore.elf`: `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`
- `system_b/priv/resource_arbitrator_ro/ra-config.json`:
  `d9479085076615484a2a443b7b1ee6725a9f7701225696a63581a636b328c71a`

Offsets/RVAs below are hexadecimal. Native stores at ShellCore+86c68f publish
owner at +20f8fb0; +87ae6d selects owner+738; +87ef77/+87ef7a publish rule/control
at monitor+0/+8. Those separate stores do not transfer ownership to the reader.

| Structure | Layout |
|---|---|
| Rule | +20 tree-sentinel pointer, +28 component count; +08/+18 first-win/exclusive counts; +44 version; +30/+3c/+40 VCN/ACP/memory budgets |
| Tree sentinel | +0/+8/+10 minimum/root/maximum; +18/+19 black/isnil bytes 1/1 |
| Component (size60) | +0/+8/+10 left/parent/right; +18/+19 color/isnil; +20 unsigned ID; +28 inline16 name or heap pointer; +38/+40 name length/capacity (capacity>=16 selects heap); +48 needConfirm; +50/+58 state sentinel/count |
| State (size50) | +0/+8 next/previous; +10 name[32]; +30 VCN; +34 scaninType byte; +38/+3c/+40 vcnType/ACP/memory; +44/+48 priority/kickPriority; +4c..4f suspension/kill/8k/forced-confirm bytes |

Missing tree children reference the tree sentinel; state lists are circular.
State records have no numeric ID. Provenance: tree construction +8755b0,
component insertion +878070..8781e5, independent lookup +875899..875959,
state construction/linking +875fd7..8761f8, and list copying +877af0..877cd6.

Expected content comes from the actual hash-checked config, separately from
observed bytes. Base/Cronos0 expectation:32 components/46 states,5 exclusive/1
first-win group,version3,budgets100/6000/399. Cronos0 is an expectation, not a
measured runtime gate. Matching covers explicit fields and proven defaults:
scaninType=0, forced-confirm absent=0/true=1/false=2, and kickPriority=explicit priority.
Other absent defaults remain decoded but listed as unassessed. Group contents
are not traversed; only their counts are compared.

## Admission, bounds and interpretation

Local capture requires the existing PID/launch FILETIME/image-path checks,
unique placements, log timestamp/1MiB-prefix hash, executable/firmware hashes and
runtime code fingerprints. The helper runs only after those checks.

- Maximum 96 bytes/read, 904 attempts and 63,872 requested resource bytes, including
  reverse-order rereads. Failed attempts consume budget; there are no retries.
- Maximum 64 components, 32 states/component, 256 total states, 64-byte component
  names. Address ceiling 2^40 follows this emulator's GuestHeap profile.
- Two-second cooperative deadline; a controller must separately enforce a
  capture-subprocess timeout because a blocking read cannot be interrupted by
  this deadline. The original six other reads bring combined limits to 910
  attempts/63,933 requested target bytes; host-file hashing is separate.
- Range/alignment/overlap, cycle/reuse, parent/key/red-black/extrema, list-link/
  count and string/flag checks precede a complete structural assessment.
- Rereads use only original ranges, never newly observed pointers. Interpret
  `traversal_complete`, `structural_checks`, `stop_reason`, `recheck_stop_reason`,
  `root_rechecks` and `all_rechecked_bytes_equal` together.
- `config_content_matches` describes INITIAL decoded fields. Equal rereads cover
  only successful initial ranges and can coexist with an incomplete traversal.
  `publication_unchanged_during_read` alone does not establish population.
- Output is always **non-atomic and lifetime-unpinned**. Equal rereads cannot
  exclude tearing, intervening mutation, ABA or freed/reused storage. Capture
  exit0 means evidence was written, not service readiness or ShellUI success.

## Portable synthetic tests

No firmware, process handle, emulator, pytest or third-party package is needed:

```sh
python -B -m unittest discover -s tests -p test_resource_capture.py -v
```

The 19 tests exercise trees/lists, inline/heap/shared names, failure/partial reads,
cycles/overlaps/caps, mutated rereads, expected matching, unassessed defaults and
the exact 904-attempt/63,872-byte maximum using synthetic Python byte arrays.
