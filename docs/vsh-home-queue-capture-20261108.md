# Bounded queued Home capture (diagnostic4 / lifecycle2)

Implementation worktree: `home-queue-capture`, branch based on parent `cba70273`.
That base includes `ebdd1292` (parent integration of manifest-owner fix681aa839).
Cherry-pick only this queue implementation checkpoint onto the current parent;
do not replay an old lifecycle branch. The static analysis note `feb5d2af` is
useful context, not a code prerequisite. Core startup worktree is preserved.

## Fixed probe set and output

Use existing `tools/capture_home_gates_1360.py --trace-mode queue --user USER`.
Exactly four hardware sites, all in pinned Vsh:

| Event | RVA | Observed boundary |
|---|---|---|
| queue_lookup | 484b0 | Default worker delivers Execute closure; lookup has not yet run |
| queue_action | 48630 | Adapter queue delivers its action closure |
| queue_complete | 196e17 | GetItems2 has serialized result and is about to dispatch Resolve |
| queue_error | 197480 | DatabaseModuleError.Reject entered; formatting/delivery still ahead |

No additional debugger loop, code patch, memory write, guest call, forced callback
or runtime queue fix. The original `Threads`, hardware context handling,
`quiesce_detach`, `_quiesce_detach`, process identity and lifecycle failure paths
are unchanged. Queue mode never sets ready or enters interactive phase rearming.
Normal gates mode remains the default. Teardown output/parent survival checks
retain the lifecycle2 meanings documented in the lifecycle note.

Each queue event includes thread ID. Lookups/actions include fresh Home owner,
context number, classified operation, numeric callback pair, module disposed and
dictionary-present observations. Actions for the two supported AppBrowse adapter
types include queue address, disposed flag, bounded queued count and worker flags.
Null list/worker is accepted only with disposed=true, represented as unavailable
fields, not an empty active queue. Other adapter classes are classified `other`
without traversing their fields. Queue count0 does not exclude an executing batch.

GetItems2 completion records actual `[vtable-68]` dispatch target, classified as
the known concrete Resolve or other/interface-thunk, and the reviewed JSON item
count (no item traversal). RAX/vtable, R10/interface descriptor and stack promise
backlink must agree. No assumption that an interface thunk equals concrete Resolve.
Error entry records an allowlisted exception class or `other`, plus the current
IPromise.Reject(string,Exception) interface slot target. That target is a snapshot
of the later dispatch slot, not proof that formatting reached it. No messages,
exception Data, titles, account tokens, URLs or arbitrary class names are emitted.

## Correlation and exact layout checks

The key is `(generation UID, launched user, resolve callback ID, reject callback ID)`.
Context/operation must remain equal across observed lookup/action. No saved object
address is dereferenced for subsequent correlation. Both callbacks independently
resolve their current weak ReactInstance and fresh Home identity; their live owner
instances must match. The existing manifest-owner and AppData/app/context backlinks
are reused. Lookup/action additionally validate that callback owner equals the
current module's Home context. Required `--user` excludes foreign-user Home traffic.

Concrete Promise/Callback, Execute closures, action delegate/argument closures and
AppBrowse/TaskQueue vtables are compared against live GOT references from pinned
allocation sites. This distinguishes nested closures with identical short names
but different owners. Operation is selected by the exact live MonoMethod constant
from the pinned caller, then argument-closure/promise backlink is checked. Zero
uninitialized GOT slots for unrelated operations are ignored, not treated as a
matching method. Required GOT slots fail closed when unavailable.

Partial SetSearchCondition calls remain observable with condition=null, never
inferred to define a selected-space scope. A present condition must have the
same user as the fresh Home owner. Other operations are excluded explicitly.

Each numeric callback ID is reserved for the entire observed generation/user
epoch. Reuse of either ID at another lookup poisons the pair and emits explicit
request invalidation (including previously observed completions). Unobserved reuse
cannot be ruled out by this probe set. Missing lookup means correlated=false;
late observations do not create a fabricated request. New generation with the
same IDs does not match old requests. Weak-target collection is an explicit decode
failure. A Resolve attempt followed by rejection is retained as two observations.

Budgets reuse the existing hit/event/output limits and 64KiB per-event Reader.
At most1024 lookup requests (2048 reserved IDs) are retained. List cardinality is
bounded to4096 for queue diagnostics; result cardinality retains the old bound.
Coverage closure freezes durations, clears live request associations and preserves
the first closure reason. Outstanding means no completion/helper entry observed,
not proof of a permanently pending JS promise.

## Pinned ABI references

`home_queue_trace_1360.LOADS` records the RIP-relative allocation/MonoMethod loads;
offline `abi_slots` derives exact GOT addresses from full-hash-pinned code.
`WINDOWS` adds live byte validation for all four probes, closure accesses, argument
dispatch and callback layout. UILib is a separately full-hash-pinned offline
layout reference; no UILib execution probe is installed. Its constructor/enqueue/
retirement byte windows verify queue fields used here. Existing corlib/Mono
references establish list and weak-handle layouts. All firmware files are read-only.

The new pinned audit checks actual Vsh/PUI disassembled field accesses and nested
owner/type GOT patches, PUI embedded Callback/Promise metadata and UILib field
bytes. This is independent of the synthetic memory fixture's declarations.

## Parent command

After integration, set `$TargetPid`, `$CreatedFiletime`, `$ExeSha256`, `$RunLog`
and `$Output` from the **new parent-owned run**, then execute from the parent
checkout. `$Output` must not exist; executable hash and creation time are mandatory.
No historical psn40 PID is supplied. Use the current executable path if the parent
build location differs. Arm before the user-controlled Home attempt where possible.

```powershell
python -B tools/capture_home_gates_1360.py --trace-mode queue --user 268500997 --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log $RunLog --attach --pid $TargetPid --created-filetime $CreatedFiletime --exe ${USERPROFILE}/Desktop/kytyps5/_Build/windows/kyty_emulator.exe --exe-sha256 $ExeSha256 --output $Output --seconds 180 --max-hits 4000 --max-events 10000 --max-bytes 8388608
```

For offline verification omit all attach/target/output arguments. This was tested
using `--run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn40/_kyty.txt`; it reported
offline_verified=true, process_access=false. Replace stage/user if a new run uses
different genuine inputs. Provenance intentionally rejects inconsistent staging.

## Verified checkpoint

Commands run here, all offline:

```
python -B tools/test_home_queue_trace_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib
python -B tools/test_home_gate_trace_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn40/_kyty.txt
```

21 queue controls passed, including changed-context invalidation, moved promises/closures/entire owner graphs,
wrong owner, foreign user, callback-owner mismatch, ID collision poisoning,
retirement, unsupported receiver, weak collection, dispatch ABI mismatch,
exception-after-Resolve, generation isolation, redaction, missing lookup,
uninitialized unrelated GOTs, budget failure and coverage closure. Pinned queue
ABI/metadata audit passed. Existing30 controls, manifest real-layout audit/four
negative controls and provenance14 negative controls passed.

AST comparison against `cba70273` verified lifecycle/context/thread helpers are
identical. No sacrificial process test or emulator process access was performed
in this task; prior lifecycle2 runtime validation is inherited, not claimed as
new validation. No emulator build is required for this Python instrumentation.

The first live result must still establish which boundary fails. This set does
not observe pre-lookup enqueue failure, every scalar/condition completion, actual
callback invocation, JS consumption, or interactive Home. Completion/helper entry
is not Home acceptance. The original animated-background failure remains the
behavior being diagnosed.
