# psn42: first dispatch established; queued executor boundary next

## Bounded live evidence reviewed

Read exactly the first **350936 bytes**, once, of parent-owned
`${USERPROFILE}/AppData/Local/Temp/opencode/home-delivery-psn42-login.jsonl`.
3575 complete records: 3323 forwarded exceptions (count only), 100 exclusions,
79 thread-arm records, 35 dispatches, 30 Resolves, two Rejects, two acquisitions,
two decode unknowns, provenance and armed. No later read, poll or process access.
No join to the earlier event-budget-limited startup capture.

Generation `28eee2c539d1c646b8a23fed34d6dd8a`, user268500997:

| ctxId | callback pair | count | acquisition t | concrete Resolve t | first dispatch t |
|---|---|---|---|---|---|
|4|180/181|0|103.987434|104.009524|104.018590|
|5|186/187|0|104.099320|104.114002|104.120259|

Both chains positively correlate; no invalidations in the prefix. Parent identifies
these as Games/Media. Delivery mode itself acquires serialized GetItems2 identity,
not its original condition/columns. The interface targets remain thunks:
Resolve `af09c88fc`, first dispatch `af09c8929`; Vsh base `e10000000`, PUI `dc0000000`.
Neither target is relabeled as the concrete destination by subtracting PUI base.
The earlier two unknowns (t88.482096 Resolve, t88.485559 dispatch, thread604)
are `generation_backlink` at `identity.app_backlinks`, after valid native type
decoding. They have no acquired request identity and do not erase later positives.
Parent's animated-background observation is separate presentation evidence.

## Exact JS distinction: mounted-but-hidden versus missing mount

Re-ran the hash-pinned Home HBC probe: all25 existing offline cases pass, input
unchanged, SHA256 `565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f`.
Also decoded functions8766/2198/383/388/389/2202/2204/8769/8770/9019/5715 directly
from that bundle. These are static/subset-test results, not live JS observations.

* #546 awaits separate Games/Media via Promise.all. #2198+1b1 calls
  fetchTitleByUserId and resumes at+1bb; #2202+2d selects the matching space response,
  +31 reads `items`, +42 slices, +52 maps into experiences. An empty items array
  runs no per-item mapping; it is a valid result, not an await.
* #8786 readThrough catches revision/cache-read failure at+104 and calls its read
  function at+124. The pinned controls verify rejection -> `isCached:false` ->
  empty result completes; a **pending** revision instead stops before the read.
  Therefore the unacquired psn42 Rejects are not proof of the revision path, and
  a hypothetical revision rejection alone does not explain the stall.
* #8766+61 marks called.current before awaiting. After iterator resume, +215 checks
  data truthiness (arrays, including empty arrays, are truthy); +221..231 can skip
  duplicate-hash publication; +24f publishes data. Actual Error reaches catch+441,
  instanceof check+44e and error-atom setter+45e. This is distinct from pending.
* Module42 is initialized by #383; the **writer is #388**, not #383's initializer:
  #388+26 groups by `spaceId`; #389+19 supplies `[]` for a missing group and +4b
  writes the per-space atom. Module46/#404 initially supplies empty arrays;
  module61/#464 selects current space; module22/#268 initializes `game`.
* Main #5715+99 reads query `called`; +163 obtains readiness; +1a9..1af requires
  both before constructing the main-menu child at+1d5. Readiness #5730 checks
  four callback-holder atoms against null. Empty results do **not** directly test
  false here: this gate is different from visible startup animation.
* Startup #9008+1d9..1e4 derives `!!experiences.length`. #9019+7 returns at+2c
  before any phase action when it is false; #9016 independently returns for empty
  data. Animated values start at1 and #9040 maps that to opacity0 for strand,
  title, system and hub. Thus a mounted menu can remain invisible for empty
  selected-space data. Nonempty data instead awaits the local interactive signal.

**Established condition, not yet established live cause:** if these two empty
results are consumed/published without replacement data, the empty selected-space
gate explains invisible startup. It does not prove absent mount, callback loss,
or a host ABI violation. No production fix is justified from the prefix alone.
Do not force opacity, fabricate items, or reinterpret revision rejection as success.

## Implemented next capture: `--trace-mode executor --user 268500997`

Exactly four hardware slots, fixed for the capture:

1. **Vsh196e17**: unchanged same-run serialized acquisition and validated callback pair.
2. **PUI1e28db**: queued closure has executed past the context+20 bridge-null check
   and is about to call the bridge through AOT PLT327790. R15 is the closure;
   its exact vtable comes from the allocation GOT load93024. Closure+10 is fresh
   PUIReactContext, +20 signed ID, +18 arguments. RDI=RAX=context+20 bridge,
   RSI=ID, RDX=arguments. Native ReactBridge type and Home backlinks are required.
3. **PUI1cbfc0**: actual ReactBridge.InvokeCallback executor-interface call. Its
   prologue preserves R15 and uses R13 as bridge. Fresh closure/context/bridge
   relationships must still agree; RBP+8 must be the queued call return1e28e0;
   frame-10 ID and frame-18 arguments must match closure. RDI=bridge+18 executor,
   RAX=executor vtable, R10=descriptor loaded1cbfb9; record target at RAX-20.
4. **PUI1cbfc3**: executor interface returned. Revalidate preserved R15/R13,
   frame and fresh owner again; volatile RDI/RSI/RDX/R10 are not trusted.
   Record only whether RAX is null, never dereference returned queue content.

Same-generation/user/ID correlation survives movement without retaining pointers.
Both sides remain distinct. Missing predecessor, duplicate or reused ID poisons
association; existing budgets/retirement checks apply. A non-tail PLT frame is
reported unknown (`executor_frame_backlink`), not guessed through by stack scan.
All lifecycle2 helpers remain unchanged. This mode replaces the already-observed
concrete/first-dispatch slots; it does not rearm them or join to psn42 IDs.

The actual executor target is classified only if exactly PUI64f50 (JSJavaScriptExecutor)
or80c60 (WebSocket executor); otherwise it remains other/interface thunk. Static
JS executor64f50 creates a closure and invokes helper308984 at65041. Its
closure66950 requires arguments, loads backend receiver at executor+80 and calls
helper308fa4 at66976 with ID/arguments. This does not select the live backend.
Executor return may include caught errors or a null flushed queue; it is **not**
proof of JS callback acceptance, Promise microtask execution, or atom publication.

### Decisive stopping rule

* Acquired but no queued call: unresolved second weak-target/context/queue boundary;
  absence alone remains coverage, retirement or exception evidence to classify.
* Queued call but no executor call: bridge path/frame validation/exception boundary.
* Executor call without return: that actual call/exception path is the next target.
* Both executor calls return: stop claiming a native delivery wait. The remaining
  positive JS observations needed are #2198+1bb result continuation and #8766+24f /
  #389+4b publication, then selected atom length and #5715 mount gate. Those require
  a reviewed backend-specific frame/continuation observer; current four hardware
  slots cannot establish them. No guessed HBC-PC breakpoint is proposed.

This one capture identifies the actual executor call boundary before choosing a
backend-specific JS observer; do not repeat the prior first-dispatch experiment.

## Parent invocation and verification

Use fresh new-run provenance and identity supplied by the parent, after the active
capture has cleanly finished and before coordinated user navigation:

```powershell
python -B tools/capture_home_gates_1360.py --trace-mode executor --user 268500997 --original $original --stage $stage --home-bundle $homeBundle --run-log $newRunLog --attach --pid $targetPid --created-filetime $createdFiletime --exe $exe --exe-sha256 $exeSha256 --output $newOutput --seconds 180 --max-hits 4000 --max-events 30000
```

Omit attach/identity/output arguments for offline CLI validation. No psn42 process
was accessed or polled by this work. New tests:10 executor controls; preserved
34 gate +21 queue +22 delivery tests and pinned audits pass. Pinned Home probe25
cases pass. Tests used at most two concurrent Python jobs; no emulator build.
Offline executor CLI passed with saved psn41 provenance and `process_access:false`.
AST comparison against219d63df changes only capture/main definitions: every
lifecycle2 helper is identical. No runtime fix or JS-consumption claim is included.
