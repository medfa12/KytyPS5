# psn40: queued DatabaseModule completion and targeted capture

Evidence checkpoint after `4c6bb691`. Offline pinned AOT/metadata and retained
parent artifacts only; no process access or tracer changes. No emulator queue
contract violation has been demonstrated, so no speculative lifecycle fix is
included. Home acceptance is still unproven.

## Resolved queue and scheduler

UILib `TaskQueue::<.ctor>b__10_0 +2d990` starts **MoveNext +2dc70**
(not the other similarly named MoveNext at +2efb0). The state-machine address
passed in RDI is an unboxed value-type address: int32 state+0, builder+8,
owner TaskQueue pointer+20, saved task awaiter+28. State0 resumes the semaphore
await, -1 is running, -2 is terminal. Do not add an object header to this layout.

TaskQueue object layout, proved by constructor +43e0, enqueue +4770 and worker:

| Offset | Field |
|---|---|
| +10 | monitor lock object |
| +18 | SemaphoreSlim, constructed with count0 |
| +20 | List<Task>; backing array+10, count int32+20, version int32+24 |
| +28 | worker Task returned by Task.Run(Func<Task>) |
| +30 | CancellationTokenSource |
| +38 | disposed byte |

Constructor +450f calls mscorlib MethodDef2184 Task.Run(Func<Task>). The pinned
mscorlib path +eb350 -> +3a0f40 starts via the default TaskScheduler and unwraps
the async task. No dedicated per-context native thread is created here.
Run(Action) +48f0 constructs Task(Action), calls Enqueue, returns that Task.

Enqueue +4770 takes the monitor, throws when disposed (+47c0/+48a6), appends
the task, and calls SemaphoreSlim.Release only when list count becomes1
(+4838..4849). The worker calls SemaphoreSlim.WaitAsync at +2dd0e; an incomplete
await stores state0/awaiter and schedules continuation at +2de55, then returns.
On resumption it gets the await result at +2de9e, takes the monitor, snapshots
the list to an array (+2df0b), clears the list, captures disposed, releases the
monitor, and iterates the batch. Thus list count0 does NOT mean no active work.

Each non-cancelled task calls **Task.RunSynchronously at +2e07b**. The next
instruction is +2e080. mscorlib +e7590 selects current task scheduler unless
hidden/unavailable, otherwise default, then calls InternalRunSynchronously
with waitForCompletion=true. A blocked task can serialize later work in this
batch and future batches. This is a contract consequence, not a psn40 finding.
Per-task exception handler +2e085..2e0b0 proceeds to the next task (subject to
runtime pending-exception propagation); outer worker failure +2e0e0..2e107
sets the worker Task exception and terminates. Task completion is separate from
the JavaScript promise captured inside its action.

DisposeAsync +4540 takes the monitor, marks disposed (+45c6), cancels the CTS,
releases the semaphore, then launches a disposal continuation. Its MoveNext
+2e170 awaits worker+28 BEFORE clearing worker/CTS/semaphore/list
(+2e330..2e366). Worker drains its captured batch then exits when disposed.
Enqueue after disposal throws; disposal does not itself settle JS promises.

## Ownership and two-stage dispatch

Vsh Execute +471f0 creates outer closure C:
`C+10 module`, `C+18 promise`, `C+20 action delegate`, `C+28 int32 context`.
It queues C's +484b0 action on UILib Default, discarding the returned Task.
At +484b0, RDI=C. It allocates inner closure D:
`D+10 adapter`, `D+18 C`.

* Module+30 null: +484fa -> +485f9 returns without settlement.
* Dictionary lookup: +48518, context from C+28; bool result AL at +4851d.
* Found: adapter in D+10; queue is adapter+10; +48584 queues D's +48630 action,
  again discarding the returned Task. No local catch surrounds lookup/enqueue
  in +484b0. Failure here can fault the default-queue Task without running the
  action's rejection handler. This is a decisive instrumentation boundary.
* Missing context: builds exception, calls rejection at +485f4 only if
  module+40==0. Otherwise returns silently.

Factory GetTaskQueue +45a30 uses a static array indexed by queue kind, lazily
constructing TaskQueue. **AppBrowseItemAccessorAdapter** factory +46310 calls
GetTaskQueue(1) at +46340; **AppBrowseMixedItemAccessorAdapter** factory +46460
calls GetTaskQueue(1) at +464b1. Both store returned queue in adapter+10.
Context4 versus5 does not establish different queues, nor does a context number
equal a queue kind. Other adapter types may share the same kind.

At +48630 RDI=D. +4865f invokes C+20 with adapter D+10. Exception handler
+4868c captures exception RAX; +486d5 tests module+40 and +486db rejects only
when undisposed. The rejection helper itself can throw. Module retirement is
also queued on Default (+46cc0); its action +197220 sets module+40=1 at +19724d,
disposes adapters via slot108, clears the dictionary, then nulls module+30
at +197322. These give a concrete explanation for retirement-related silence,
but the retained psn40 capture did not observe these branches.

## Accessor, success and rejection chain

Vsh GetItems2 +196cb0: RDI adapter, RSI JSON request, RDX promise. It calls
protected slot+c0 at +196d28, then returned result's serialization slot+68 at
+196d37. Completion **+196e17** is IPromise.Resolve MethodDef190f, interface
dispatch via `[RAX-68]`, with RDI=current promise, RSI=serialized result,
R10=interface method descriptor. Capture the dispatch target, not just a presumed
concrete Promise address. GetItems has analogous completion +196ca5.

Accessor.Db GetItems +14490: RDI accessor, ESI/EDX reader arguments; actual
reader call **+1450c** is `[vtable+B8]`. At +14512, RAX is its returned managed
list. Capturing those two sites separates reader-entry/nonreturn from downstream
serialization. +14d40/+14da0 are deleted/registered-user exception policy helpers;
their entry alone does not prove settlement or native database success.

DatabaseModuleError.Reject **+197480** takes RDI promise, RSI exception, RDX
adapter (possibly null). It annotates exception data, maps an error code (default
81701100), then calls ReactNative.Vsh.Common ErrorUtility.Reject at +19768a.
Common +19ff0 formats the code and invokes IPromise.Reject(string,Exception)
at +1a025 (`[RAX-10]`, MethodDef1911). For the concrete PUI Promise:

* All five IPromise Reject overloads route through PromiseBase:
  +881f0, +88200, +88240, +88270 -> +88380.
* +88380 constructs the error JSON then virtual slot98 at +8858b invokes
  concrete Promise.Reject **+1cbaa0**. Formatting/exception-property/JSON failures
  can occur before this concrete entry. A helper hit is not successful rejection.
* Concrete Promise.Resolve **+1cba30** and Reject+1cbaa0 call callbacks+10/+18
  respectively; null callbacks silently return. They do not clear the callbacks.
* PUI metadata has IPromise Resolve190f and Reject1910..1914; PromiseBase
  implements IPromise, Promise overrides Resolve and JSON Reject. This establishes
  convergence for this concrete implementation, not every possible external
  implementation or live interface trampoline. Record actual receiver type and
  target at +196e17/+1a025 to close that remaining coverage question.

Metadata scan of PUI, Vsh, Vsh.Common, UILib and Accessor.Db found only
PromiseBase implementing IPromise and Promise extending PromiseBase. No other
Promise/PromiseBase subclasses appeared in those five assemblies. Assemblies
outside this scan and live interface dispatch still require receiver validation.

## Pointer audit and stronger correlation

Parent tracer requests are keyed by raw promise address; settlement pops RDI
before validation and reads the saved raw module address. Reuse is rejected only
when a later observed Execute repeats an address. It cannot detect movement,
unobserved reuse, or an alternate receiver implementation. Host Python integers
are not managed GC handles. psn40 has no retained relocation/pinning evidence;
MONO_GC_PARAMS=null (log40907) does not prove a nonmoving collector. No claim that
GC actually moved one of the six requests is supported.

There is a better source-backed identity: concrete
`ReactDelegateFactoryBase.Callback` has **int32 callback ID+18**, and
WeakReference<ReactInstance>+10 (constructor +900). Promise+10/+18 point to these
callbacks for the factory-created Promise (+870). Read both callback IDs at each
boundary after validating concrete types; correlate `(Home generation, user,
context, resolve ID, reject ID, operation)` rather than addresses alone. Preserve
addresses as observations, validate uniqueness within generation, and explicitly
close coverage on lifecycle changes. Callback IDs alone are not globally unique.

PUI **Callback.Invoke +127e90** takes RDI callback, RSI object[] args. At
+127ec5 AL is WeakReference.TryGetTarget's result; `[RBP-18]` is the recovered
ReactInstance. Failure silently returns at +127f1b. At **+127f15**, RDI=live
instance, ESI=callback ID, RDX=serialized args, `[RAX-98]` is the actual
InvokeCallback interface target. Thus callback_present in the old trace does not
prove delivery. This site permits movement-independent ID correlation and fresh
instance identity validation without dereferencing an old saved module pointer.
Do not invoke guest weak-reference or GC-handle APIs from a passive capture.

## Parent: precise next capture windows

All offsets are module-relative, regular13.60 only. Resolve current module bases
per run. psn40 used Vsh de0000000, UILib ce0000000, DB c10000000, PUI d90000000.
Each set uses at most four hardware execution sites. Separate bounded captures
have separate coverage epochs; absent hits across different runs cannot be joined
into one causal history. Existing parent lifecycle-v2 cleanup remains required.

1. **Next Home capture: Vsh +484b0, +48630, +196e17, +197480.** Recover C/module/
   current promise/context at the first two entries; record callback IDs and
   fresh generation, disposed byte, adapter type/queue, action target. At +196e17
   record result count using the already reviewed JSON decoder and actual target.
   At +197480 record exception type and current IDs. This distinguishes default
   delivery, adapter action, successful result production and rejection-helper
   entry. It does not measure requests that never reach +484b0, nor prove the
   rejection helper delivered its callback.
2. If first/second stage is absent: **Vsh +471f0; UILib +4770, +2e07b, +2e107**.
   Enqueue has RDI queue/RSI Task; snapshot queue layout above. Worker +2e07b has
   RDI Task, `[RBP-30]` queue, `[RBP-58]` batch array, int32 `[RBP-60]` index.
   +2e107 has RSI exception and state-machine pointer `[RBP-158]`; records worker
   failure. Inspect Task flags+44 and only decode task/delegate links after type
   validation; no arbitrary heap walks. To distinguish blocked execution from
   return, replace worker-failure site with **+2e080** (same stack locals).
3. If action runs without success/rejection: **DB +1450c, +14512; Vsh +196d37,
   +486d5**. Reader target and returned list identify the actual native owner;
   serialization entry and guarded exception path distinguish later failure.
   At +486d5 RAX=module, RDI=promise, RSI=exception, RDX=adapter. Record disposed.
4. If producer succeeds but delivery remains unclear: **Vsh +196e17;
   PUI +1cba30, +1cbaa0, +127f15**. Match numeric callback IDs and fresh instance
   identity. To diagnose weak-target loss specifically, replace one concrete
   Promise entry with **PUI +127ec5**. A +127f15 hit proves native callback
   dispatch intent, not JS consumption or interactive Home.

Retirement-specific alternative: Vsh +19724d (R15 module before disposed store),
+197322 (R15 module before dictionary null), +4851d (R14=C,RBX=D,AL lookup result)
and +486d5. These distinguish lifecycle silence from missing context/accessor
failure; no need to infer retirement from post-detach stack absence.

## Reproduction and verification

Existing `tools/inspect_appbrowse_aot_1360.py` handles Vsh/DB. For the other
assemblies use its Image/main in a one-shot Python process with the literal hash
added to the in-memory HASHES dictionary; no on-disk tool edits are needed.
Reviewed windows: UILib 43e0:990,2d990:170,2dc70:500,2e170:2c4;
Vsh 45a30:1c0,46310:70,46460:80,484b0:260,196cb0:180,197480:230,
46cc0:c0,197220:110; PUI 870:100,127e90:95,881f0:3b0,1cba30:d8;
Common 19ff0:3a; mscorlib e7590:1c0,eb350:30,3a0f40:140.

SHA256:
* UILib `5a3c961361cb50fd20506310a7874865a71f0b38a33afece0f3562bb7c31482a`
* Vsh `64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee`
* DB `739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766`
* PUI `a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914`
* Common `d3a25c65c1685dd2ca5509d9ee87451a2ade874ae163c3c0c2d265e59b2685ae`
* mscorlib `c05d3ebb585d8185530793eb259200337fe20b8d656df20cbcd7437437d48ce3`

This checkpoint changes documentation only. Pinned inspections succeeded;
no runtime test was run. Core build/tests remain those verified in4c6bb691.
Retained traces cannot identify the actual queue/reader failure without these
additional boundaries. Selected-space items and interactive Home remain unproven.
