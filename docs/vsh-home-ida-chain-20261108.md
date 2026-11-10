# IDA Home chain, real bridge ABI, and candidate result

## Executive result

Actual IDA Professional 9.3 and Hex-Rays were run in isolated databases on
private copies of the relevant 13.60 images. The decisive new correction is
that the former executor trace observed the optional **bridge monitor**, not
the actual bridge call. Its `context+0x20` bridge dereference was wrong.
The executor trace has been corrected and tested offline; it has not been
re-run live and no missing JS delivery is asserted.

Separately, parent-owned candidate2 supplied actual native Home serialization:
Game and Media each repeatedly returned a real item for persistent
user268500997, despite the retained corruption marker and context1 scalar
errors. This eliminates zero-item production for those observed candidate
requests, not every reader/recovery problem or later UI gate. The parent
provided a screenshot of **Settings**, not independently confirmed Home tiles.

## IDA provenance and limits

IDA's autonomous ELF loader rejected Sony `e_type=0xfe18` with error4, explicitly
logging unsupported image type; this was not a license diagnosis. For IDA only,
`prepare_ida_home_elf_1360.py` created exclusive copies changing the two-byte
ELF header `0xfe18 -> ET_DYN(3)`. It verified the entire original SHA256 and
that all other bytes are identical. Original firmware was not written or run.
IDA reports, fresh IDBs, specs and logs are retained under this worktree's
`_TempData/ida-home-20261108`; no historical IDB was opened or modified.

| Image | Original SHA256 | IDA-view SHA256 |
|---|---|---|
| ReactNative.PUI | `a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914` | `2c6999871dcf868db710f97b24dbb21720b3f32ca532494ff44449ec203c7898` |
| ReactNative.Modules.Vsh | `64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee` | `e29ed73cd7dc7af09e9b5b761a9bf9a635f79b503173398c7328a582564e7679` |
| Sce.Vsh.Accessor.Db | `739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766` | `c4d2f85f12ff544fc2019315383b6ab8fe8772aec184e99024c9a186127f3080` |
| ShellUI.Legacy | `070de0f8845090812b0490c1cd0ee8fde7bd67cf607fa63b3b0260f1ef6352ae` | `ae17edc068bc26ef1ef9e90f852c4e1d824fb5b04b4d9d70a7c363dbeb878bc4` |
| ShellUI.ReactNativeApps | `5a24f6a4fe34255dd7ab728940ae364880af0bc03062a2e6080175d130199265` | `2e0c4c7f332f658bfca4848d3bc2599b93a3a70276e517de3dc92c9268db56dc` |
| libNativeExtensions | `2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb` | `230ec1aca983a93fbf67abacd1abe0cc6145f5abe699779020c56b0a08e53326` |
| libhermes-0.11 | `e279a9d4272df0347515a481bd0fe4d4bb0f7bb9281c3ece20032688c0f9d10d` | `0b4b7d15e5d47b1c98e72db833dcddfd354d90370ad9094cc5b6308d072ad602` |

IDA imagebase was zero; addresses below are original-image RVAs. Managed method
names and Mono AOT PLT/GOT patches are resolved independently from the pinned
embedded PE/AOT metadata, then checked against IDA instructions. IDA's default
`__fastcall` label is not evidence of Windows x64 guest ABI: the instruction
registers below are the guest x86-64 SysV-style register flow. Stripped managed
exception regions sometimes appear outside IDA's function extent, so the raw
IDA disassembly window, not a shortened decompile, is authoritative there.
Static xrefs do not prove a live indirect target.

## Login to Home request

* Legacy `ForegroundUserManager.OpenForegroundSession +3e250` tests the session
  flags and invokes the installed session delegate at `[delegate+18]` with
  flags/user arguments. Legacy `DoFirstLogin +1ff750` and its async MoveNext
  `+1ffa30` are present in IDA, but IDA associated an overlapping method chunk
  with the former; do not infer the exact live branch from its decompile alone.
  Parent's candidate2 login uid268500997 returned zero; that is local login,
  not PSN authentication.
* ReactNativeApps `HomeUI.EventManager.OnOpenSession +4f380`,
  `StartOpenSessionTask +4fa80`, MoveNext `+51180`, `MenuManager.Open +51640`,
  `UIManager.Start +9100` and `AssemblyEntry.Start +9220` were disassembled in
  IDA. Independently decoded AOT literals in these windows include `autoLogin`,
  `login`, `switchUser`, `intro=login`, and `pshomeui:main`. These are reachable
  static session-to-Home mechanisms, not proof of each branch in candidate2.
* Pinned Home HBC89 SHA256
  `565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f`
  was **separately** decoded, not by IDA. #546 requests both spaces; #2198
  invokes `fetchTitleByUserId`; #2202+31 reads the selected response's `items`;
  #8766 can publish to #388/#389 per-space atoms. #5715 gates menu construction
  on `called` and callback holders; #9019/#9016 require a nonempty selected list,
  while #9040 initially maps animation value1 to opacity0. With nonempty items,
  a distinct local signal released through the tile `interactive` chain remains.
  The 25-case HBC subset probe passed with explicit doubles; it is not a live
  Hermes/React run or evidence of published candidate data.
* IDA Vsh `DatabaseModule.Execute +471f0` builds a closure capturing module,
  context, parameters and promise; `+484b0` looks up the adapter and queues the
  next closure; `+48630` runs it; `+48800` invokes GetItems2. `+196cb0` calls
  the protected provider and serializes the result before its interface Resolve
  at `+196e17`. `+1a7ad0` is the AppBrowse adapter. These are static call and
  field boundaries; psn candidate2's same-generation queue trace supplies the
  live action/serialized count, not IDA.
* IDA Db `DbItemAccessor.GetItems +14490` initializes a list then calls the
  reader vtable+`0xb8` at `+1450c`. The separately disassembled catch region
  `+1455b` can return the initialized list on a deleted-user branch or take
  registered-user/recovery/error paths. Db `GetAppBrowseDbRevision +58cc0` and
  Vsh revision getter `+72770` remain distinct from the item reader. Candidate2
  proves that actual serialized nonempty items can coexist with scalar errors;
  no causal attribution to a missing revision was observed.

## Promise -> actual callback and corrected executor decoder

IDA PUI `Promise.Resolve +1cba30` selects callback+`0x10`, packages one
argument, and invokes its interface; `Callback.Invoke +127e90` resolves a weak
ReactInstance and dispatches ID/arguments at `+127f15`. `ReactInstance +87030`
checks a second weak context; `PUIReactContext.InvokeCallback +93000` captures
context at closure+`0x10`, arguments+`0x18`, signed callback ID+`0x20`, checks
termination/context+`0x30`/nested+`0x40`, and queues its delegate.

IDA `+1e2850` and independent pinned Mono patch/MethodDef mapping prove:

1. `+1e28a3..+1e28db` checks **context+0x20**, then calls PLT `+327790`.
   Mono PLT patch resolves that to `PUIReactBridgeMonitor.InvokeCallback`, not
   `ReactBridge.InvokeCallback`. The monitor is optional.
2. At `+1e28e4`, PLT `+3103b0` resolves to `PUIReactContext.get__bridge +92390`.
   IDA shows it reads context+`0x30`, then that Data object's+`0x18`, or null.
3. If nonnull, `+1e2908` converts the captured arguments through vtable+`0x48`.
   At `+1e291f`, the actual `IReactBridge.InvokeCallback` interface call uses
   RDI=R14=returned bridge, RSI=callback ID, RDX=**converted** arguments,
   RAX=bridge vtable, R10=the pinned interface descriptor. Closure+`0x18` is not
   necessarily the converted pointer. The direct-call return PC is `+1e2922`.
4. `ReactBridge.InvokeCallback +1cbf90` saves R13 and takes RDI as bridge;
   `+1cbfc0` calls the executor interface on bridge+`0x18`, with its own frame
   holding ID at `[RBP-10]` and converted arguments at `[RBP-18]`. `+1cbfc3`
   is the return boundary. SysV nonvolatile R14/R15 survive the direct path.

The old psn43 capture saw zero hits at the monitor call and 68 decode failures
because it tried to type context+`0x20` as the bridge before verifying the
caller. A null optional monitor is entirely consistent with a live real-bridge
call. However, the saved trace did not record the needed caller return address,
so it cannot retrospectively prove that all 68 hits were this direct path.

`--trace-mode executor` now uses `+1e291f` instead of `+1e28db`, obtains the
bridge from context+`0x30`/Data+`0x18`, verifies the interface descriptor and
fresh bridge receiver, and checks `[RBP+8] == +1e2922` **before** using R15 in
executor call/return. It compares executor-call RDX to the bridge frame's
converted argument, never to the original closure argument. A non-tail thunk,
alternate bridge type, other caller, retired context or reused identity remains
an explicit unknown/exclusion, not proof of loss. Queue, call and return still
do not prove JS acceptance; do not attach to parent processes without parent
coordination.

## Base RNPS HBC -> callbacks; native backend limit

The independently decoded staged `NPXS40141/base_dll.ps.bundle.hbc` SHA256 is
`12c7a876da06d1d01dc9c8dd559c6dd41c961f187863d637d6003e1e72dffd74`, also
matching the psn23 stage copy. Base HBC #2519 creates a Promise; #2520 supplies
its resolve closure #2521 **first**, then reject closure #2522, to
`enqueueNativeCall`. #363 pushes even then odd IDs; it stores that first closure
in the map named `_failureCallbacks` and the second in `_successCallbacks`.
PUI `CreateExtractor` closure `+88fa0` reads those arguments in order;
`ReactDelegateFactoryBase.CreatePromise +870` stores first at Promise+`0x10`,
second at+`0x18`. Thus the map names do not mean the even native Resolve ID is
wrong. Base #352 guards #353 -> #371; #371 selects by ID low bit, removes both
callback entries and applies the selected function if present. Our bounded
actual-HBC #363/#371 subset test confirms even resolves, odd rejects and repeat
does not dispatch; reversed input is a failing-behavior negative control. The
wrapper argument order itself is established by HBC disassembly, not by that
subset interpreter. No actual JS callback or Promise microtask was observed.

IDA PUI `JSJavaScriptExecutor +64f50` creates another closure; `+66950` loads
executor+`0x80` and invokes a Mono P/Invoke named
`JSExecutorInvokeCallbackAndReturnFlushedQueue` in `libNativeExtensions.sprx`.
Within that native image, IDA finds the string
`invokeCallbackAndReturnFlushedQueue` referenced at `+14cd0` and `+2c043`;
`+16af0 -> +14c00` packages two arguments and `+2c8d0` has a separate virtual
runtime-call path. In the latter, `+2c928 -> +2c010` obtains the named function,
and `+2c976` invokes the runtime receiver's vtable slot+`0x198` with two values.
IDA also identifies `__fbBatchedBridge` references. In the separate pinned
Hermes image, IDA `+14f19..+14f27` installs the `HermesRuntimeImpl` vtable
address point at `+26c348`; its typeinfo is referenced at `+26c340`. Pinned
ELF RELA slot `+26c4e0 = +26c348 + 0x198` resolves to Hermes `+14010`, whose
IDA code contains `HermesRuntimeImpl::call` stack-overflow handling. Thus, **if**
the `+2c8d0` receiver at+`0x78` is that actual Hermes object, the virtual call
resolves to `+14010`. The live executor/backend and object vtable were not
observed, and we have not proved the P/Invoke binds to this particular native
path. No fabricated live stack or JS consumption is claimed. An actual
backend-specific owner/target observation is needed only if JS consumption
becomes the blocker.

IDA also inspected PUI `SetOnPerformanceTiming +175210`, timing callback
`+177ba0`, `IsInteractive +52d20`, and event dispatch `+555a0`. Their static
presence does not establish candidate2's tile interactive event.

## Live receipt and next boundary

The completed candidate2 queue trace was independently hashed/read once:
`5bf5d6b379a64a0c07d6a0cc23e141236e3f15db5e8ecffd0b08aea07038e779`.
Generation `1f60a658ee08fa43bec064ff723a2a08`: 52 Home lookups, 48 actions,
15 serialized completions (7 Game count1, 7 Media count1, 1 context6 count2),
16 scalar errors, zero decode unknowns. Deadline301.540319s; teardown verified
375 threads, zero residual/post-detach repairs, no errors. Requests49–52 lacked
an action before shutdown; application state is unknown, so no deadlock claim.
The parent reports successful local login and supplied Settings screenshot;
visible interactive Home tiles and post-detach navigation await user confirmation.

The source-backed candidate succeeds as an item producer without full native
recovery completion. Do not force scalar success, clear markers, or add a false
readiness gate. `vsh-home-incremental-catalog-20261108.md` describes the separate
incremental private-candidate fix and its real integration/locking limits. If
Home is visually interactive, prioritize a safe real-profile bootstrap and
installed inventory rather than another delivery trace. If still hidden, the
smallest remaining distinction is actual JS publication/selected-space length
versus the tile interactive signal; the corrected executor observer is optional
and still needs a same-run direct-call validation.

## Checks

11 corrected executor controls, pinned AOT metadata audit, 22 delivery, 21 queue
and 34 gate controls passed, with pinned audits. The first gate command omitted
its required provenance arguments and failed *after* its 34 unit controls; rerun
with original/stage/Home/log arguments passed all audits. Executor offline CLI
reported `offline_verified:true` and `process_access:false`. Base callback HBC
subset and the existing 25 Home HBC subset cases passed. No emulator process,
parent runtime store, firmware original or existing IDB was modified.
The separate hash-pinned `audit_home_hermes_call_1360.py` verified the native
receiver/slot instructions, callback-name reference, Hermes constructor store
and RELA target; using the modified IDA-only image as an original was rejected
by the hash-gate negative control.
