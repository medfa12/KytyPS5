# Restored Home handoff: psn41 completion/rejection branches

## Task continuity

Before the DRM diversion, the preserved `vsh-home-startup` checkpoint was
feb5d2af, following Core4c6bb691 (parent8a667fe6). Four-probe implementation
05dd2bdf was integrated as e697761b; the immediate pre-DRM review f108efa6 was
integrated as32fb4358. Its stopping rule in
`vsh-opus55-review-audit-20261108.md:98-136` was explicit: run the four probes;
after correlated serialized completion, follow concrete Promise/callback/JS
delivery; after error-helper entry, identify the exception predecessor.

That is the resumed task. DRM5a04d48f is retained but not part of this handoff.
Core `(0,10)`/Promote remains paused. The original startup tree is untouched.
Parent owns psn41 and all live instrumentation. This checkpoint used saved files,
static firmware and read-only SQLite only; no process access or builds.

Parent source/docs were reviewed at56713f1c. The three existing files relevant to
the decoder fix (`home_gate_trace_1360.py`, its test, and the AOT inspector)
byte-match that parent commit before this change; no prerequisite cherry-picks
or unrelated media changes were needed in this isolated tree.

## What the completed saved capture establishes

`Temp/opencode/home-queue-psn41.jsonl`:5883 records, including5684 forwarded
exceptions; only event counts and the20 lookup/action/error/completion records
were inspected, not a dump of forwarded events. Generation:
`8ac782c903119f44b99a69278d30de51`, user268500997.

* Context4/Game: condition174/175, items176/177, request3, count0,1.279s.
* Context5/Media: condition180/181, items182/183, request5, count0,1.290s.
* Both item requests execute sequentially on queue2172b0df0 and reach Vsh196e17.
  Permanent queue head-of-line blocking is contradicted for those invocations.
* Context1 scalar2 requests1 and8, callbacks70/71 and398/399, reach197480 after
  their actions. Adapter and exception both classify `other`. This does **not**
  identify them as the ConceptBrowse revision implementation or a SQLite exception.
* Resolve targetaf09c8005 is not a pinned concrete method. It remains an observed
  interface dispatch target. Reject targetdc0088240 does map to PUI88240, but
  the helper probe precedes the actual call; neither target establishes delivery.
* Four outstanding condition requests are coverage limitations, not timeouts.

No concrete Promise or JS-consumption observation can be recovered retroactively
from these records. No request numbers/addresses may be reused as live identities.

## Concrete diagnostic defect fixed: wrong side of the JS wrapper

The old gate-mode scalar classifier used the caller-facing singular
`{scalarProp:"revision"}` and read a top-level `revision` at native Resolve.
Those are the wrong contracts at its probes. Queue mode did not decode scalar
properties, so this defect does not invalidate its lookup/action/error counts.

Pinned Vsh AOT SHA256
`64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee`:

* GetScalar2 wrapper1967b0 loads **scalarProps** at196801, gets the JArray
  enumerator at196844, and calls the provider's scalar method at19688c (+f0).
* It builds output **ctxId** at1969b0 and **scalarMap** at196a62, then attempts
  IPromise.Resolve at196a8c. It has not yet returned through the JS wrapper.
* Saved `home.hasm` function4292 shows the intervening JS conversion: offsets0d/13
  read/delete scalarProp,18/1c create its one-element array,22/27 build
  `{param,scalarProps}`, and3c call native GetScalar2. At4c it reads scalarMap;
  at52..90 it checks the requested property;92..ab returns `{ctxId,[property]}`.
  The earlier HBC4278 call-site observation omitted this intervening wrapper.
  Native property literals are independently hash-pinned; the saved HBC text is
  supplementary evidence, not a newly authenticated disassembler output.

`tools/home_gate_trace_1360.py` now recognizes the exact singleton native
`scalarProps:["revision"]` request and reads revision from scalarMap only when
returned ctxId matches the correlated context. It does not traverse param or
emit arbitrary properties. Batch requests are not relabeled as singleton revision
requests. No guest callback or value is changed. This is a diagnostic fix,
**not a Home runtime fix**.

Regression controls cover legacy caller shape rejection, batch exclusion,
native wrapped zero revision, flat-result rejection and wrong-context rejection.
An independent audit resolves the three property strings from the pinned AOT
GOT patches, rather than merely mirroring fixture field names.

## Concrete Promise -> callback -> JS queue: static boundaries

Pinned PUI SHA256
`a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914`.
All addresses below are RVAs, not reusable live addresses.

| Boundary | Actual behavior / remaining loss point |
|---|---|
|1cba30 Promise.Resolve|Reads callback at promise+10; null returns at1cba92. Builds a one-element argument array and dispatches callback at1cba8f.|
|1cbaa0 Promise.Reject|Equivalent callback at promise+18; null returns at1cbb02.|
|88240 PromiseBase.Reject|The observed psn41 target adds `[native exception]` and calls88380 for error formatting. It is not concrete Reject1cbaa0.|
|127e90 Callback.Invoke|Callback+10 weak target; result AL at127ec5. False returns without dispatch. Callback ID is signed DWORD+18.|
|127f15 interface call|RDI is recovered ReactInstance, ESI callback ID, RDX converted arguments; receiver vtable in RAX. This is before actual InvokeCallback execution.|
|87030 ReactInstance.InvokeCallback|A **second** weak target at instance+10;87063 resolves it; false/null at87068..87073 returns. Success calls93000.|
|93000 PUIReactContext.InvokeCallback|Builds closure(owner+10,args+18,ID+20). Context byte92 set, null context+30 or null nested+40 all return at9306d. Otherwise930e6 enqueues through MessageQueueThread.RunOnQueue8a810.|
|1e2850 queued closure|Recovers context+20 bridge; null skips bridge invocation. At1e28db invokes the bridge using closure+20 callback ID and+18 args.|
|1cbf90 ReactBridge.InvokeCallback|At1cbfc0 calls executor interface on bridge+18, then processes returned flushed queue at1cbfe7.|
|64f50 JSJavaScriptExecutor.InvokeCallbackAndReturnFlushedQueue|Builds another executor closure and invokes helper308984 at65041. This implementation exists; selecting the live executor is still required.|

Thus 127f15 is not JS acceptance. A concrete Resolve and even the first callback
dispatch can precede weak-target/context retirement or deferred queue failure.
There is no saved psn41 evidence that one of these paths actually dropped a
callback, so no production scheduler/lifetime change is justified yet.

### Exact next live observable for the parent

Use the restored four-site follow-up **Vsh196e17 / PUI1cba30 / PUI1cbaa0 /
PUI127f15**, with the reviewed ownership/callback-ID decoder extended and tested
by the parent. At196e17 acquire fresh same-run Promise callback pair, Home
generation/user, serialized ctxId and item count. Validate actual Promise/type,
argument/backlinks and fresh owner; use generation/user/callback pair as key,
not the old object address or psn41 request number. Concrete entries must recover
the same fresh callback owner/IDs. At127f15 recover fresh ReactInstance ownership
and correlate ESI to that pair. Unknown/reused IDs remain uncorrelated; moved
objects and changed owners invalidate association rather than cross-run joining.

This set specifically follows serialized item requests. It cannot label an
otherwise unobserved rejection as the old context1 scalar operation. Scalar
predecessor attribution still requires same-run acquisition of that operation
and a pinned exception/provider discriminator; generic `other` is insufficient.
Substituting127ec5 can test the first weak-target result but loses observation
of the dispatch call; that tradeoff must be explicit, not reported as delivery.

If matching127f15 is observed, the next unresolved boundary is queued callback
execution at1e28db and its actual executor target. Actual JS consumption requires
that later bridge/executor evidence or the correlated JS continuation; even the
four-site result is not a usable-Home acceptance claim.

## Scalar/query and empty catalog: bounded evidence, not a second recovery task

Read-only SQLite transaction on original
`_Runs/psn23/sd/priv/mms/app.db` (`mode=ro`, query_only, no writes):

*34 tables; shared content has15 rows.
* Native SQL `SELECT status FROM tbl_version WHERE category=?` with
  `table_access_index` fails **no such table: tbl_version**.
* Current-user `tbl_iconinfo_0268500997` is absent. Icon/concept tables exist for
 268435457..268435472, not the current profile268500997.

Pinned DB AOT `739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766`:
**58cc0 GetAppBrowseDbRevision** zeroes the output, gets its connection at58d74,
loads the above exact SQL at58d7d and executes at58daa. Vsh **72770** matches
`revision` at727b4, calls the accessor at72802 and creates/throws an error on a
nonzero result at72874..72897. A thrown managed SQL error can also escape before
that integer-result branch. Context1's observed `other` adapter is not enough
to prove this exact route was executed.

Saved `_Runs/psn41/_kyty.txt` provides an earlier native error independently:
line47551 reports Core corruption admission80f50061/detail100; AppBrowse
initialization reports809c0103 for icon/concept/addon/playlist/group at
47605..47636. Prior pinned Core mapping is ff19d5 -> ffe830 (checkCorruption),
which prevents ordinary accessor opening on that branch. Later saved log lines
600665 onward show actual app.db reads, so the startup failure must not be
generalized to “no later connection ever opened.” No messages naming the scalar
exception or missing table were found in the bounded saved log searches.

The native ordinary catalog filters remain visible=1, space masks2/4 and EXSW128;
the missing current-user table is earlier than those row predicates. This schema
would not produce a valid ordinary current-profile query. However, a successful
empty serialization could reflect an error-normalizing/empty connection path,
not a demonstrated execution of those predicates. The last observed item boundary
is serialization, so callback/JS continuation stays the priority. No seeding,
migration, candidate activation, fake catalog or synthesized revision was done.

## Verification

34 gate tests and21 queue tests passed. Pinned native scalar-property audit,
existing manifest-layout audit/four controls,14 provenance controls and queue
ABI audit passed using psn41's saved log and current stage. Tests used no process
access and no emulator build; at most two Python test jobs ran concurrently.
No claim of fixed Home, JS consumption or a fully identified live scalar exception.

## Parent integration

Integrated independently of DRM as `9f22a563`. Parent reran all34 gate tests,
21 queue tests, native scalar-property/manifest audits, four manifest negative
controls,14 provenance controls and queue ABI audit against the preserved stage
and psn41 log; all passed. No production rebuild was needed for this Python
diagnostic correction.

The original VSH agent is now implementing the specified four-site delivery
capture as an explicit mode with fresh callback-ID/owner correlation and pinned
ABI tests. Parent retains live process ownership. No new capture or Home fix is
claimed at this integration checkpoint.
