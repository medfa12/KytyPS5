# Opus5.5 review audit: evidence before another Home experiment

Read the complete116-line `opus55-vsh-review.md`. Its declared scope excluded
05dd2bdf, raw captures and binaries. This is a docs-only response; parent owns
tracer instrumentation. No runtime/tracer edits, builds, process access or launch.
Further SQLite/recovery layering is paused pending causal Home evidence.

## Verified versus hypothesis

| Review point | Audit |
|---|---|
| Shared queue can serialize requests | Verified static contract: both AppBrowse variants use factory kind1; worker batches call RunSynchronously. Context IDs4/5 are not queue kinds. |
| Games GetItems blocked Media behind it (H1) | Plausible, unproven. Execute timestamps are issuance, not adapter enqueue order or action start. No captured Task/queue ownership or reader entry establishes this order in psn40. |
| Native AppBrowse errors identify the blocked wait | Not established. Their temporal proximity supplies candidates, not an owner/event/producer chain. |
| Raw-address correlation can miss GC movement | Correct structural weakness; 05dd2bdf already uses fresh callback-owner IDs. Elapsed132s does not establish a collection, move, pinning state, or even continued object lifetime. “Very likely moved” is not verified psn40 evidence. |
|663 Resolve hits may contain a missed Home settlement | Possible. They are aggregate probe hits, not663 retained decoded callback-ID records. They cannot be retrospectively assigned to Home. |
| Lifecycle2 evidence | Survival is evidence for the parent-observed runs. The review did not audit the implementation; 05dd2bdf preserved its helpers, independently AST-compared against parent cba70273. |
| More local recovery work should wait | Agreed. Whole-MMS staging is unsafe to activate and Core inputs/integration remain incomplete. This does not establish that every production consumer is blocked by the local candidate marker; local stages were inactive in psn40. |

Media recommendations are outside this audit and remain with the media owner.
No additional claim about codecs, protected playback or media readiness is made.

## Bounded retained-stack result: H1 undecidable

Re-read raw JSON metadata and classified all345 symbolized stack blocks once.
Snapshot was sequential, post-detach, at20261108-113603, at most32 frames and32KiB
per thread; raw stack lengths320..32768 bytes, three at the byte limit, no recorded
thread capture errors. 316 blocks contain unknown/guest/JIT frames.

Non-exclusive symbol-name counts:97 PthreadCondWait,92 Posix sem_wait,
20 KernelWaitEventFlag,11 KernelSleep. No symbolized Ipmi/AppBrowse/AppDb names.
Top frames include282 ZwWaitForAlertByThreadId and38 NtWaitForSingleObject.
These are host wait classifications, not345 identified managed task owners.

Examples of the attribution break:
* tid508: PthreadCondWait -> pthread_cond_wait -> guest `0x960216e38`;
  later printed “frames” include1 and `deadbeef00000007`. These are not a reliable
  managed unwind to a TaskQueue action.
* tid856: Posix sem_wait -> guest `0x9305be998`; no queue/accessor identity.
* tid4432: EventFlag::KernelWaitEventFlag -> guest `0x9309a0010`, followed by
  unknown `0x20098c958`. This does not name the AppBrowse synchronization event.

Aligned QWORD scan across every retained stack and CONTEXT record found zero
Vsh/UILib/Accessor.Db mapped-range values and zero exact matches to all six logged
promise addresses. Ranges: Vsh de0000000+5e4000, UILib ce0000000+fc000,
Accessor.Db c10000000+1c0000. This agrees with the parent's negative scan.
Missing caller frames, queue/Task heap objects, wait-object identity and producer
backlinks prevent assigning any sampled wait to Games GetItems. It neither proves
nor refutes H1; no further broad stack scan is justified with these artifacts.

## Scalar/rejection attribution correction

Retained JSONL verifies six Home requests for user268500997/generation
`b2d7d9f520528742b93c550e42d9a27a`:
* req1 scalar2 **context1**,101.189523; no correlated settlement before drain.
* req2 Games condition context4,108.395557; Resolve110.040669, callback_present.
* req3 Games items2 context4,108.427715; req4 Media condition context5,108.460642;
  req5 Media items2 context5,108.500359; no correlated settlements before drain.
* req6 scalar2 **context1**,111.927971; Reject112.449369, elapsed0.524292s,
  callback_present. Both scalar records say revision=false, not a revision read.

The review's “no context attribution” is incorrect: both scalars have context1.
Neither record has adapter type, queue kind, Task, action entry or rejection cause.
Context1 must not be equated to queue kind1. Even a context associated with a kind1
adapter could reject during Default-queue lookup (missing context) without entering
that adapter queue. Only verified later execution on the same worker queue after
the alleged blocking action starts would contradict continuous blocking there.
Req6's rejection alone supplies none of those facts.

Two additional condition_incomplete Execute hits at108.512660/108.528708 lack
request associations. End record:746 hits, Resolve663/Execute14/Dispatch51/Reject18,
two correlated settlements, four bounded outstanding observations, cleanup_verified
with345 verified threads and zero repairs. Callback presence is not JS delivery.

## Four-slot review proposal: do not adopt unchanged

1. UILib2e07b/2e080 + PUI1cba30/1cbaa0 has **no request acquisition site**.
   psn40 contains no callback IDs and no heap snapshot from which they could be
   reconstructed. Historical promise addresses and a different run's IDs cannot
   provide the missing join. Scanning matched raw addresses would not recover
   pointed-to objects, even if matches had existed.
2. At2e07b RDI is Task; at2e080 RDI is volatile. Use the reviewed worker locals
   `[RBP-68]` Task, `[RBP-30]` queue, `[RBP-58]` batch, int32 `[RBP-60]` index.
   2e080 is also the cancelled-task skip target. Exception handling can advance
   the batch without that normal-return site. Entry/exit pairing must account for
   both branches, thread/frame incarnation and lifecycle coverage.
3. A queue object contains no documented numeric kind field. Establish kind1 from
   the fresh factory array's index1 or a freshly validated AppBrowse adapter+10
   backlink. Do not carry a queue pointer across runs/GC and call it kind1.
   Mapping Task to request requires a newly audited Task/delegate/closure decoder,
   including wrapper/unsupported-task handling; callback-ID decoding alone is
   insufficient. Current05dd2bdf does not claim that Task mapping.
4. Returned Task with no observed concrete Resolve/Reject is not yet “delivery
   loss”: retirement, formatting exceptions, unsupported receivers and coverage
   gaps remain. Weak-target loss occurs downstream of concrete Promise entry, so
   missing Promise entries alone does not specifically implicate weak targets.

## Recommended next experiment and stopping rule

**First run the parent-reviewed05dd2bdf fixed set**, before user-controlled Home:
Vsh484b0/48630/196e17/197480. Acquire fresh IDs/owners in that same capture.
This already separates Default delivery, adapter action, serialized GetItems2
result and rejection-helper entry. It intentionally lacks action return, all
scalar/condition settlements and actual callback delivery. Do not score those
missing observations as waits. Preserve parent lifecycle2 teardown/survival checks.

Choose only the branch supported by that run:
* Correlated item_count at196e17: production reached serialization; a never-returning
  reader for that invocation is contradicted. Next measure concrete callback and
  JS delivery, not recovery layers. A later block in callback dispatch is distinct.
*197480: identify the classified exception and predecessor/retirement observations;
  a rejection-helper entry is not a completed Reject callback.
* Lookup without action: inspect actual adapter/queue association; possibilities
  include default lookup/enqueue failure, retirement and an earlier queue task.
* Games action without result/error: prioritize a same-run worker progress
  experiment; do not yet assert H1 or that Media was enqueued behind it.

For the last two cases, a **future parent-approved** four-slot H1-focused set can
retain Vsh484b0/48630 and use UILib2e07b/**2e0b2** (common per-task loop advance,
including handled task exceptions and cancelled skips). Fresh requests and action
ownership then coexist with worker progress in one run. It requires pinned ABI/
negative tests for Task mapping and pairing before use; it is not implemented by
this docs checkpoint. Pair loop advance only with an observed entry for the same
worker frame/batch index, revalidating fresh ownership where available. A skip
without entry is not a return; an exception-driven advance is not normal success.
Task.RunSynchronously may schedule execution elsewhere: do not require the action
thread to equal the waiting worker thread. No settlements are measured in this set.

**Stop after one bounded capture** when it identifies the last supported boundary
or becomes inconclusive through missing entry, saturation, decoder/owner errors or
lifecycle gaps. Report “no loop advance observed during X seconds”, never permanent
nonreturn. Only a mapped current Task/queue plus bounded non-advance and identified
native wait/producer can justify a causal queue fix. If H1 ownership remains absent,
request the one missing boundary instead of replaying the same experiment or
joining different runs. Actual acceptance remains visible interactive Home with
selected-space items, not a trace or preparation receipt.

## Artifact receipts

All inputs under `${USERPROFILE}/AppData/Local/Temp/opencode/`, read-only:
* review SHA256 `ad6b51b6f3ab76a43691d804549073a577f0f6d20cb09e1c474198f5725cb347`
* raw stacks `04b444bf9b1a3c8250f5da6203f66a30c649d2a276a343dcc7cb2170f12eba2e`
* symbolized stacks `512efa6481708454f4eadb845b6a68cd42eb8e529ac942fbdbd3903158130dbf`
* psn40 JSONL `f6d96c9814af842df2d4ca2acc5fc6d87abf2f60fef09f97ab88110b544878ac`
