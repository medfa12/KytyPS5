# B2 synchronization / CRT handoff — 2026-11-05

## Parent integration in progress

The shared entry now receives `PrepareEntryExitCallback`, and the final worker
root abandons incomplete guards before consuming thread exit or running cleanup.
The import classifier now validates the full resolved NID/type/name/address/
expression registration, rejecting conflicting exact duplicates while accepting
independent same-body library identities. Symbol-database regression cases cover
that distinction. The exact seven sync/CRT returning pairs are integrated;
process exit remains absent from the generic returning table.

Parent condition changes add pre-lazy-init owner checks, typed retained condition
handles, stop-serialized notification/publication, checked attribute ownership,
and busy destruction while a wait/notify call still holds the object. Wait calls
hold their condition pin through mutex reacquisition. The condition fixture's
setup now runs on an actual owned guest stack for checked local output slots.
Only the audited POSIX broadcast/signal/relative-wait additions are classified;
the broader condition initialization/attribute/destroy catalogue is not admitted
solely from these changes.

Both fresh-process fixtures are wired. The obsolete host-pointer once fixture
has been migrated into the private-code/control CRT fixture, retaining tagged
thread exit, state rollback, host RAII, root consumption and healthy retry checks.
The old executable-entry fixture now checks the pinned exit gateway identity.

First integration build exposed that old entry fixture's reference to the removed
raw callback; it was corrected. A new VM/symbol build and focused regressions are
underway (`_TempData/b2-integration-{build,tests}.log`); no B2 pass or live result
is claimed yet. The explicit pthread-once migration was strengthened after that
build started and needs its final targeted rebuild/check. A second fixture-only
compile error (missing `InitLibKernel_1` declaration in the pthread translation
unit) was corrected. The condition fixture also checks destroy returns Busy both
before notification and after waiter-list removal while mutex reacquisition is
still blocked; that additional case needs the final targeted rebuild as well.

Parent result: VM/symbol build succeeded and all **11** selected checks passed
in `_TempData/b2-integration-tests.log`: CRT sync, condition wrappers, symbol
database, application bindings, guest execution/threads/waits/mutex stop, native
drain, condition deadlines and allocator bootstrap. A final emulator/VM rebuild
and two-fixture recheck is underway to include the latest migrated once-exit and
busy-after-notification assertions (`_TempData/b2-live-build.log`,
`_TempData/b2-final-fixtures-tests.log`). No emulator process was running when
this executable rebuild started. Native Apple continuation remains unverified.

Final emulator/VM build succeeded and both updated fixtures passed
(`_TempData/b2-final-fixtures-tests.log`), including pthread-once tagged-exit
rollback/host RAII/retry and condition destruction refusal after notification.
Targeted live run `b2-live-1105`, PID15496, started with the existing profile,
1920x1080 internal rendering and normal host window. Native NP remains disabled
for this app-lifecycle checkpoint; live startup evidence is pending.

## Outcome and gates

Implemented the independently valid **checked pthread-once / typed std-once / HLE C++ guard / HLE owner CRT** subcontracts, with new real-worker fixtures. **No builds, tests, live operations, agents, staging or commits were performed by this worker.** Static/manual review is not a pass claim.

**The whole B2/native-guard cluster is not declared complete.** Four integration gates remain:

1. Parent must replace the raw shared executable-entry exit callback with `RuntimeCrt::PrepareEntryExitCallback(execution)` before enabling shared HLE atexit/exit, and add the boundary-free worker-root guard-abandon hook **before guest thread cleanup**. Details below; no entry/worker-root edit was made here.
2. The actual Apple route uses **private native libc guards**, which require the POSIX condition wrapper closure. Read-only audit found a real ownership gap in the existing notification/lazy-init bodies, not merely a missing returning expression. The new condition fixture deliberately gates on that safety contract. The granted `pthread.cpp/.h` scope was **once only**, so condition bodies were not changed. Parent must own/transfer those narrowly identified changes before admitting the missing notify pairs.
3. `exit` stays **nonreturning and unadmitted** until parent builds/runs the nested registered-wrapper test and integrates the ordinary-exit/peer/resource contract. Do not put it in a general returning table based on the fact that its HOST frame now returns normally.
4. Production `ApplicationImportBindings::Classify` currently compares a resolved registration against the **first NID+address match**, so exact duplicate-body library identities can be rejected even with the correct returning pair. The condition fixture's two raw broadcast identities expose this independently; parent must use full resolved registration identity, not widen an address/NID alias exception.

B1 and its files/tests remain parent-owned. Parent reported the full emulator/VM/LNC build, allocator fixture and selected existing owner/import/execution/native-drain checks passing; the corrected combined component set also passed. Those are parent B1 results, **not B2 results**.

## Files and concurrent-build discipline

* New **header-only** `src/loader/runtimeCrtSync.h`: owner/boundary/span/target admission, lifetime-keyed once/guard records, actual-thread identities, typed three-argument std-once transition. No root CMake change or missing link unit is needed.
* New **header-only** `src/loader/runtimeCrtExit.h`: combined CXA/atexit records, DSO finalization, reverse ordinary drain, main-return bookkeeping, pinned entry notification gateway.
* `src/kernel/pthread.cpp/.h`: **only once** implementation/include/contract comments changed. Existing 16-byte native once layout assertion remains; only its first state word is written.
* `src/libs/libC.cpp`: **only** std-once, guard acquire/release/abort, atexit/CXA/finalize/exit/catch-return and related old CRT fields/includes changed. Heap trace, thread-atexit machinery and IPMI were not edited.
* New `tests/RuntimeCrtSyncTests.inc`, `tests/RuntimeCrtConditionTests.inc`: parent integration described below.

Both implementation headers were created **before** adding production includes/call sites. No returning tables, root CMake/dispatcher, `libKernel.cpp`, `runtimeLinker*`, `runtimeGuestExecution*`, B1 files or Astra IPMI files were edited for B2. Inherited/concurrent dirty work was preserved.

## Implemented synchronization contracts

### Checked ownership and admission

Shared wrappers require the current importer's executable execution, matching linker/owner and **the innermost actual-thread host-return boundary**. Missing/stopped controls or pending tagged thread exit never fall through to session behavior. Invalid active requests latch `InvalidTransition` and return through host cleanup; only the generated return gateway performs recovery.

Control spans must fit wholly in private writable, non-executable ELF data/RELRO or the current matching pthread's owned guest stack, with readable/writable backing and tracked RW protections. Private callbacks must be valid same-owner ELF executable text, not native libkernel, a host function or arbitrary JIT allocation; current mapping protection and Windows executable-page protection are checked. Windows writable copyout checks do not change protections.

This intentionally does **not** generalize arbitrary dynamically allocated heap once/guard controls: B0 VM ownership/mapping authority and hostile concurrent unmap/protect/input mutation remain separate. Loader/pthread mappings and callback code must stay pinned. The registry does not claim a security sandbox around malicious native code.

Records bind address + synchronization kind to the immutable owner identity/token. Another owner/session cannot silently adopt a retained record. Owner stop does not reclaim address authority. Session calls retain direct callbacks; the old pthread-once signal polling remains session-only. Shared once/guard waiters never call signal dispatch.

### pthread_once and std_execute_once

* Exactly one actual worker claims the control; contenders park on a host condition variable with bounded **1 ms cancellation polling**, not a success-shaped return while initialization is running.
* The initializer runs with no registry/loader critical lock. pthread once uses the existing void nested callback boundary. Std once uses a new **trivial naked tail transition**, supplying exactly `int(NULL, argument, void** context)`; the local output cell starts null and the callback's signed 32-bit result is preserved.
* DONE is published only for a normal, nonzero std result / normal pthread callback, using `Lease::Publish` against owner stop/failure. False std result resets to INIT and lets a contender retry.
* Failed, stopped or tagged thread-exiting callbacks reset the state to INIT and notify waiters. Reset is host synchronization rollback, **not guest cleanup or successful post-stop publication**. Failure remains sticky; tagged actual-thread exit is not converted into owner failure and is not consumed in a nested helper.
* A terminal contender unlinks even while the initializer remains held. It does not dispatch a pending signal, steal initialization or publish DONE.
* Same-actual-thread recursive initialization is refused (shared owner fails closed; POSIX error is EDEADLK before gateway suppression), rather than deadlocking or running the initializer recursively. Callback reentry on a **different** control remains possible.

The once implementation no longer uses guest state 3 as its waiter census; the native 0/1/2 completion ABI and the untouched remaining 12 bytes are retained. Do not mix HLE/native implementations on the **same** control without an explicit compatibility contract.

### C++ guards

The old byte test/store was not exclusion. Acquire now claims one actual thread and parks contenders until release/abort or actual native thread abandonment. Release publishes first-byte 1 with acquire/release memory ordering; abort leaves it clear and permits exactly one contender to retry. Upper guard bytes are not repurposed for host pointers or ownership tokens.

Only the acquiring actual thread may release/abort. Recursive acquire fails closed. Stop of a waiter does not pretend the object is initialized. Actual thread identity uses a **weak native-thread TLS incarnation**, not a recyclable pthread handle/thread-id. When an initializer exits its real pthread without guard-abort, native TLS death makes the incomplete guard retryable without a TLS-destructor guest callback or guest-memory write. That fallback is **too late** for a guest thread destructor that reenters the same incomplete guard before native TLS teardown.

Parent must call:

```cpp
Loader::RuntimeCrt::AbandonCurrentThreadGuards(execution);
```

only at the **final worker root**, after **all** guest boundaries/gateways returned and **before consuming the root thread-exit tag / running guest thread destructors** (the existing `RunThread` point after `RunOnGuestStack` returns, before `ConsumeThreadExit`, is the required parent insertion). This applies to normal root completion as well as tagged thread exit; never call it between two still-active guest constructor segments. Also call it before intentionally reusing a HOST coordinator's physical thread after abandoning its guest worker. It clears host guard ownership only; it never consumes tagged thread exit, writes guest memory or invokes guest cleanup. Failure/stop still forbids guest cleanup independently.

That root edit is outside the granted once-only pthread scope and was **not** made here. The new guard-exit fixture registers a real private thread-destructor callback that reenters the guard, so it is an explicit gate for this required hook rather than a weak-TLS-only success claim. This is not automatic exception/unwind-table support: native exception paths must call guard-abort, and guest C++/SEH unwind across host/generated frames remains unsupported.

**Provider boundary:** these HLE guard changes do not replace/interpose the private bundled libc implementation. The Apple checkpoint below is specifically a **native** guard release invoking HLE POSIX conditions.

## Implemented HLE CRT contracts

### Registration / consumption / ordering

CXA and atexit records now share one identity+token keyed owner list, independent of the environment/argc map. Records are published against stop; callback PCs are checked private text except for the **one exact prepared owner entry-notification gateway**. No arbitrary host callback or generated/JIT address is accepted as an exception.

`cxa_finalize(dso)` takes matching CXA records in reverse registration order, claiming each **before** invocation and calling outside locks. Ordinary HLE `exit` drains the combined CXA+atexit registration order, last first: constructors' CXA callbacks, executable fini and supplied entry notification follow their actual registration order. Reentrant same-thread finalize and registration during ordinary drain are supported and cannot replay an already claimed record. Peer finalizers serialize; a peer cannot append indefinitely to an already exiting process.

A destructor's tagged **thread exit** interrupts drain, destroys the surrounding host finalization frames normally, and leaves **unclaimed** records for a later healthy worker; the claimed destructor is not replayed. No nested helper consumes the actual-thread exit tag. A fatal/stopped owner never starts or consumes another guest finalizer. Unconsumed records/code remain pinned; final quiescence/reclamation belongs to B0/B6.

Recursive **process exit** from a finalizer is deliberately refused/fails closed, not disguised as a returning `exit` or allowed to seal the owner while outer finalizers remain live. Native-private CRT queues are not merged speculatively with the HLE queue. The session's direct CXA callbacks and existing host-process exit path remain session behavior; no shared exit falls through to `::exit`.

`catchReturnFromMain` records status and logs only. It no longer dumps unvalidated stack words, consumes no records and does not close admission. Native entry calls exit separately.

### Mandatory parent entry-callback interface

Current `RuntimeLinker::RunApplicationEntry` passes the raw host `ApplicationExitHandler` address to guest entry. Registering that raw pointer is **not admitted** by the new HLE atexit contract. Parent must include `loader/runtimeCrtExit.h` at top level and replace only that shared-entry argument with:

```cpp
const auto exit_callback = RuntimeCrt::PrepareEntryExitCallback(execution);
// If zero: report startup unavailable/failure; DO NOT run entry with zero/raw fallback.
return execution->InvokeEntry(entry, params, exit_callback);
```

The module retains the generated HostCallReturnThunk and its exact execution for the owner's pinned lifetime. The callback itself is notification/bookkeeping-only, preserving the old handler's **no session StopAllModules** policy. It is also valid when private native libc consumes it from **its own** atexit list: no HLE-drain state is required, no owner stop is inferred from notification alone, and providers must not be switched simply to force use of the HLE registry. Parent's coordinator still owns process completion/resource retirement.

The shared HLE exit body drains while callback admission is open, then calls the existing `ExitCurrentVshApplication`, which publishes exit request/code, closes admission and requests the actual-thread exit. The wrapper requires the matching host-return boundary **before** doing this. Thus its host frame returns normally and gateway epilogue suppresses guest continuation only after outer host RAII. **This is an implementation plus an unrun proof fixture, not permission to classify exit as returning.** Parent must also settle peer quiescence/resource retirement and concurrent close/failure precedence before live ordinary-exit acceptance; no guest callbacks may be added after fatal stop to repair an incomplete drain.

## Native guard / complete POSIX condition closure audit

### Parent live/static evidence (not worker execution)

Parent's bounded Apple checkpoint in PID12548, `_Runs/b1-live-1105/_kyty.txt`, reached two fresh app lifetimes (apps104/105); old heap setter refusal is gone. Both later refused **`mkx2fVhNMsg#A#B` / `Posix::pthread_cond_broadcast`**, native libc return **+0x82ccc**, while ShellUI remained retained (entry0 result `0x80020055`). Parent traced:

* private native guard-acquire **+0x82be0**, export `3GPpjQdAMTw`, condition wait `Op8TBGY5KHg` at **+0x82c30**;
* private native guard-release **+0x82c90**, export `9rAeANT2tyE`: lock `7H0iTOciTLo` **+0x82ca0**, guard qword=1 **+0x82cb0**, unlock `2Z+PpY6CaJg` **+0x82cb7**, broadcast `mkx2fVhNMsg` **+0x82cc7 -> +0x82ccc**;
* native abort **+0x82d00**, export `2emaaluWzUw`.

This is **not execution of HLE CxaGuardRelease**. Preserve private provider binding. The condition closure is an independent dependency, not a reason to force all libc guard exports to HLE.

### Exact registration/error-domain catalogue

All modules below are `libkernel` v1.1; libraries are v1. Expressions must match exactly after normal version/owner/address resolution. Duplicate NIDs are **not** blanket namespace aliases.

| NID | Expression | Registered library | Contract / gate |
|---|---|---|---|
| `mkx2fVhNMsg` | `Posix::pthread_cond_broadcast` | `libkernel`, `Posix` | Positive POSIX errno; **new notify ownership gate required** before admission. |
| `2MOy+rUfuhQ` | `Posix::pthread_cond_signal` | `Posix` | Positive errno; same notify gate. |
| `2MOy+rUfuhQ` | `LibKernel::PthreadCondSignal` | `libkernel` | Encoded kernel errno, **different registered body/expression** from Posix alias. Same ownership gate. |
| `JGgj7Uvrl+A` | `LibKernel::PthreadCondBroadcast` | `libkernel` | Encoded kernel errno; same notify ownership gate, not the observed POSIX expression. |
| `0TyVk4MSLt0` | `Posix::pthread_cond_init` | `Posix` only | Positive errno; explicit initialized object ownership + stop-serialized creation needed. Do not invent a `libkernel` identity. |
| `Op8TBGY5KHg` | `Posix::pthread_cond_wait` | `libkernel`, `Posix` | Positive errno; existing terminal cancellation/recursion policy, but pre-lazy-init authority gate still needed. |
| `27bAgiJmOh0` | `Posix::pthread_cond_timedwait` | `libkernel` | Absolute deadline -> existing `PthreadCondTimedwaitAbs`; positive timeout/cancel errno. |
| `K953PF5u6Pc` | `Posix::pthread_cond_reltimedwait_np` | `libkernel` | Relative microseconds -> existing `PthreadCondTimedwait`; same ownership/cancel gate. |
| `mKoTx03HRWA` | `Posix::pthread_condattr_init` | `libkernel` | Actual attribute creation, positive errno; attribute ownership/span policy required for general shared use. |
| `EjllaAqAPZo` | `Posix::pthread_condattr_setclock` | `libkernel` | Actual clock selection, positive EINVAL for invalid ID; foreign attribute mutation must be gated. |
| `dJcuQVn6-Iw` | `Posix::pthread_condattr_destroy` | `libkernel` | Actual free, positive errno; foreign/in-use attribute authority gate. |

Exact additional kernel-style registrations from `libKernel.cpp` (all `libkernel` library v1 / module v1.1):

```cpp
{"2Tb92quprl0", "LibKernel::PthreadCondInit"},
{"g+PZd2hiacg", "LibKernel::PthreadCondDestroy"},
{"RXXqi4CtF8w", "LibKernel::PthreadCondDestroy"},
{"WKAXJ4XBPQ4", "LibKernel::PthreadCondWait"},
{"JGgj7Uvrl+A", "LibKernel::PthreadCondBroadcast"},
{"kDh-NfxgMtE", "LibKernel::PthreadCondSignal"},
{"o69RpYO-Mu0", "LibKernel::PthreadCondSignalto"},
{"BmMjYxmew1w", "LibKernel::PthreadCondTimedwait"},
{"m5-2bsNfv7s", "LibKernel::PthreadCondattrInit"},
{"waPcxYiR3WA", "LibKernel::PthreadCondattrDestroy"},
```

This catalogue is an audit boundary, **not a claim all bodies are now safe to admit**. There is no registered Posix `pthread_cond_destroy` body to invent in this batch; use the actual kernel destroy ABI where appropriate. `POSIX_PTHREAD_CALL` converts the kernel status, so admitting only a differently named kernel expression cannot admit a Posix wrapper.

### Concrete source gaps / required parent condition delta

Existing `PthreadCondWait`/relative/absolute waits have owner comparisons, terminal cancellation, actual waiter removal and recursion restoration only for nonterminal outcomes; existing native-worker fixtures cover those mechanics. `KernelDispatchPendingSignalForCurrentThread` also returns immediately for runtime-owned workers. These are reusable mechanisms, not a rewrite request.

However, notification calls `CreateObject(...Cond)` **without execution admission**, then broadcast/signal/signalto touch waiters **without checking `condition->wait_owner`**. Foreign notification can wake another owner's waiter. Shared condition waits likewise take the legacy lazy-init/shadow-repair route before their later owner checks. `CreateObject`'s existing execution-aware branch only admits Mutex. Init/attr publication is not stop-serialized. Destroy can delete a condition without rejecting live waiters. Merely adding the missing `mkx...` pair would leave these gaps.

Parent condition ownership transfer/fix requirement:

1. Extend the existing execution-aware static-object lookup to **Cond** with a type-correct lifetime predicate, before guest word mutation/lazy init/shadow repair; preserve existing mutex/native field layout and trusted host teardown.
2. Notify/signalto must require current exact boundary/lifetime + same-owner condition; reject foreign/session objects **before waking/removing any waiter**. A matching waiter/thread must also be owned for signalto. Terminal stop must not cause a new notification/guest callback publication.
3. Wait/absolute/relative wrappers must carry that pre-lookup admission through the existing cancellation machinery; do not regress recursive timeout restoration or terminal released-mutex policy.
4. Init/attribute mutation/destruction need a deliberate owned-object/span and stop publication policy; destroy must refuse live waiters, not free their CV/host pointers. Session and trusted boundary-free teardown remain explicit compatibility routes, not shared fallback.

No new runtimeLinker/runtimeGuestExecution interface is necessary for that condition fix; the existing Context/leases/owner metadata are available. It is outside this worker's granted once-only pthread scope. The new registered-wrapper fixture has a **known source-level red gate** for foreign broadcast/signal, with no expected-failure bypass. Keep notify/destroy/init broad admission closed until it passes.

### Separate parent-owned import classifier identity fix

`ApplicationImportBindings::Classify` calls `Symbols()->FindByNid(nid, Func, symbol.vaddr)` and then requires that returned record's full `name` equal the resolved record's `name`. `FindByNid` returns the **first** matching registration. `Posix` registration is inserted before the duplicate `libkernel` broadcast registration; both are the same body/expression, but full library identities differ. Consequently a correctly resolved `mkx... [libkernel...]` record can still be rejected after the exact returning pair is added. The same general issue affects other duplicate-body identities (e.g. `libc` versus `LibcInternal` CXA/std registrations).

Required parent interface/delta: the HLE classification lookup must validate **NID + full resolved versioned registration name + expression + target address**, rejecting conflicting/overwritten exact registrations, rather than selecting the first NID/address alias. An exact-record query in `SymbolDatabase` or an equivalent existing exact lookup is appropriate. Do not merely remove the name check, use an address exception, or treat every registration with the same NID as supported. Normal private native provider resolution stays unchanged.

No edit to those parent-owned loader/database files was made here. `RuntimeCrtSyncTests.inc` uses the actual `LibcInternal` CXA/std identities for its positive primary classifier gate. `RuntimeCrtConditionTests.inc` intentionally exercises **both** raw broadcast library identities after condition semantic gates, so this alias bug cannot be hidden by choosing whichever registration was inserted first.

## Returning pair handoff (parent only)

For the implemented HLE sync subcontracts, after building/running their component fixture and checking source-selected identities:

```cpp
{"14bOACANTBo", "Posix::pthread_once"},
{"DiGVep5yB5w", "LibC::std_execute_once"},
{"3GPpjQdAMTw", "LibcInternal::CxaGuardAcquire"},
{"9rAeANT2tyE", "LibcInternal::CxaGuardRelease"},
{"2emaaluWzUw", "LibcInternal::CxaGuardAbort"},
{"8G2LB+A3rzg", "LibC::atexit"}, // ONLY after parent entry-gateway hook
{"XKRegsFpEpk", "LibC::catchReturnFromMain"},
```

Keep existing exact `tsvEmnenz48 / LibC::cxa_atexit` and `H2e8t5ScQGc / LibC::cxa_finalize` classification; implementation now shares the checked token-scoped CRT registry. Std-once is registered in `libc` and `LibcInternal` identities; guards are `LibcInternal` library/module v1/v1.1. Once is under the actual `libkernel` / `Posix` registration catalogue, not an invented native-provider bypass.

The selected bundled once import is `14bOACANTBo` in `libkernel`. Source also registers `Z4QosVuAsA0 / Posix::pthread_once` under multiple identities, but the accepted report distinguishes its native alias/error-domain route. Do **not** infer native ABI/domain correctness or add that alias solely because the HLE source points to the same function.

**Do not add `uMei1W9uyNo / LibC::exit` to the returning list in this batch.** The fixture constructs the explicit production HostCallReturnThunk around the real registered target to prove HOST return/guest nonreturn mechanics without changing classification. Parent retains the final nonreturning admission decision and coordinator contract.

For native conditions, the exact missing POSIX notify pair is `{"mkx2fVhNMsg", "Posix::pthread_cond_broadcast"}` (both registered libraries), with `{"2MOy+rUfuhQ", "Posix::pthread_cond_signal"}` a separate Posix-library pair. The other signal expression and the entire catalogue above require their own gates; do not classify a family from one wakeup test.

## Fixture integration — unrun

Parent-owned top-level `runtimeLinker.cpp` include:

```cpp
#include "loader/runtimeCrtExit.h"
```

Parent-owned class declaration under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS` and include alongside existing Loader fixtures:

```cpp
static bool TestRuntimeCrtSync(std::string* failure); // RuntimeLinker class
#include "../../tests/RuntimeCrtSyncTests.inc"       // inside Loader, like existing fixtures
```

Suggested fresh-process dispatcher/CTest case: **`runtime_crt_sync`**, with the same initialized memory/pthread/loader setup as the allocator/owner fixtures. The fixture loads synthetic segment metadata into trusted program-memory allocations; real private RX stubs represent guest callbacks. No guest executable permission or original firmware execution is enabled.

Coverage includes real owned pthread coordinators and native joins; contended once success/failure/stop; typed std false retry, thread exit and recursion; guard exclusion/abort/native-thread abandonment/stop/recursive failure; non-state ABI bytes; real registrations and production return gateways; reverse combined CRT order, DSO filtering, same-thread finalize/registration reentry, nonnull CXA argument, destructor thread-exit interruption/resume without replay, ordinary exit host RAII/guest nonreturn, main-return-only bookkeeping, native-style entry callback consumption, owner/module/session isolation and fatal-stop cleanup suppression. Export lookup uses exact registrations; no fake registration or returning-policy override. After semantic checks, it requires the parent's exact sync/CRT returning pairs and calls **raw production deferred once/std/guard release/abort entries on real workers**. Unlike B1's fixture, explicit gateways also test **still-unadmitted nonreturning exit**, while the actual classifier is asserted to keep exit closed; passing is not blanket returning admission.

Parent condition fixture integration: include `loader/applicationImportBindings.h` at top level in `pthread.cpp`; declare `Libs::LibKernel::TestRuntimeCrtConditionWrappers(std::string*)` under the VM-test macro in `pthread.h`; include `../../tests/RuntimeCrtConditionTests.inc` beside the existing LibKernel native-worker fixtures. Suggested case **`runtime_crt_condition_wrappers`**. This fixture tests actual registered POSIX attribute/init/deadline/error wrappers, two real guard-shaped waiters and recursive restoration, signal-one/broadcast-all, terminal wait cancellation and foreign notification refusal. After semantic gates pass, it requires the parent's real classification and calls immutable **raw deferred** broadcast identities for both `libkernel` and `Posix`, plus the separate Posix signal. No classifier override or expected-failure shortcut exists.

**Known existing-fixture integration delta:** `RuntimeGuestThreadTests.inc`'s old `HostOnce/OnceExit` case supplies a host fixture control/callback and owners without private linker metadata. The new production checked once path intentionally rejects that. Parent must migrate that test case to the private image/owned-stack callback setup (or use this new real registered once fixture for the checked route), preserving its tagged-thread-exit/outer-RAII assertions. Do not weaken production target/span checks to accept a fixture-only host function. No existing fixture was edited here.

Run existing callback/thread-exit/native-drain/owner-mutex/condition-deadline/import-isolation regressions after integration. Parent owns every build result and test fix after handoff. Static whitespace checks alone do not establish ABI or liveness correctness.

Read-only checks performed here: scoped tracked-file `git diff --check`, trailing-whitespace scans on the new files, and a source-text check of documented NID/expression pairs against actual `LIB_FUNC` registrations. No whitespace errors or unmatched documented pairs were found. This is not compilation, fixture execution or live validation.

## Parent-operated targeted live milestones

After the relevant component gates, one bounded Apple checkpoint should establish **native guard** acquire/release/abort condition closure: own mutex lock/unlock, any real wait at +0x82c30, guard publication +0x82cb0, broadcast **+0x82cc7 -> +0x82ccc** and continuation. Do not claim the HLE guard body ran or change private provider selection. If blocked, collect exact namespace/expression/module-relative PC and owner lifetime, and close the component contract rather than repeat one-NID E2E.

For later CRT/entry acceptance, separately capture native executable entry argument/exit gateway, registration order (supplied callback then executable fini), constructors, `main`, `catchReturnFromMain`, ordinary reverse finalization, exit request and actual native/peer/resource drain. A supplied callback notification is **not** proof that HLE exit ran, peer workers stopped, submitted GPU work completed or shell retirement is safe. For fatal stop/startup cancel, capture **zero new guest finalizer callbacks** after admission closes. Broader B3–B6 lifecycle/resource gates remain intact.

## Remaining limits

No active guest exception/SEH unwinder; no asynchronous host-frame recovery or host-thread killing; no signal-handler execution from shared once/guard waits; no guest cleanup after fatal owner stop; no foreign condition repair; no fake successful services; no arbitrary heap/JIT admission; no premature mapping/GPU-resource reclamation. CRT/synchronization metadata and generated entry capabilities are deliberately retained until a separate final owner reclamation contract exists. Recursive process exit and mixed native/HLE use of the same once/guard control remain unsupported. Conditions and parent entry/nonreturning/coordinator integration are explicit gates, not hidden behind a B2-complete claim.
