# Semaphore producer observation

Default off. Set `KYTY_SEMAPHORE_PROVENANCE=1` before startup on Windows x64
Clang to observe CreateEventFlag, CreateSema and PollSema. Disabled registration
keeps the original addresses/debug names. Enabled wrappers forward arguments and
results once, preserving guest errno and Windows LastError around observation.
They implement no semaphore service, initialization or wakeup policy.

## Evidence and bounds

Startup creates a process-lifetime mapping:
`%LOCALAPPDATA%/Temp/opencode/semaphore-provenance-<PID>-<QPC>.bin`.
Setup failure disables capture. Six independent banks hold 24 records each:
pinned Event/Create/Poll and unmatched Event/Create/Poll. The binary is exactly
39,424 bytes. Full banks perform no guest reads; drops remain visible. Producer
work adds no allocation, mutex, logging, loader lookup or blanket critical guard.

Loader placement pins the unique module ID, actual base/size and the SHA256 of
the same open ELF backing (128-MiB maximum):
`6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`.
Mapped E8 call displacements must also match:

| Return RVA | API | PLT RVA | Permitted object derivation |
|---|---|---|---|
| `6e690e` | CreateEvent | `197b1c0` | output − `480` |
| `6e69ca` | CreateSema | `197ba30` | output − `4e8` |
| `6f65ca` | PollSema | `197b3e0` | none |

Unload invalidates the pin before unmapping. Duplicate/failed placement or unload
never rearms. No runtime base is hardcoded. Module IDs and pointers are correlation
data, not credentials. Original native-audit prose contained an extra zero in its
base; the cited run used `0x910000000`, consistent with caller `0x9106f65ca`.

Creation records contain raw arguments/caller, before/after output, result and
errno. Only pinned creation sites may derive object fields: int32 appId at +0,
event at +480, semaphore at +4e8 and lock byte at +4f0. Every read has a validity
bit. Name capture requests 32 bytes, so it can fail even if an earlier NUL is
readable. Failed reads are unknowns, not invalid-argument proof.

Entry and exit commit independently. Entry-only records have no observed result.
Sequences order observation points, not atomic guest operations. Snapshots are
non-atomic and lifetime-unpinned; reused object addresses can alias observations.
Old diagnostics inside the real implementation may see the wrapper as their
caller; this probe captures the guest RA at the registered boundary instead.

**Poll object remains unknown.** No RBX recovery, sentinel-handle join or temporal
join is performed. **Initializer entry/exit remains unknown.** These two creation
milestones cannot observe a negative-appId early exit, skipped initialization or
spawn continuation. Missing records, even without drops, do not establish those
paths. No guest instructions or fields are patched or forced.

## Decode and validation

Collect only after the process stops. There is no explicit flush or crash-durability
guarantee. The offline decoder reads at most 39,425 bytes, caps UTF-8 text at
65,536 bytes, and reports text omissions separately from capture drops.

```text
python -B tools/decode_semaphore_provenance.py <copied-artifact.bin>
python -B -m unittest discover -s tests -p test_semaphore_provenance.py -v
```

Seven synthetic tests validate format, missing fields, partial completion and
bounds; they do not validate compiled ABI or Windows runtime behavior. Before
relying on a boot capture, verify compiled wrapper argument/RA/return preservation,
enabled/disabled registrations, unchanged real API results and errors, failed
observation reads, budget exhaustion forwarding and actual verified placement.
No captured result alone establishes successful initialization, UI or setup.
