# LoginMgr prerequisite implementation handoff — 2026-11-05

## L0 component checkpoint

Implemented exact 13.60 Client::Config writes and constant `0x1000` required
client backing in `src/libs/ipmiClientConfig.h`. Early `libC.cpp` and late
`libShellHits.cpp` registrations use the **same function targets**, including
first-NID fallback. Generic client allocation uses the same backing constant.
Padding/name bytes untouched by the original constructor remain untouched.

Until L1 hooks are explicitly installed, exact `SceLoginMgrServer` factories
return unsupported without writing outputs instead of entering generic
success-only connect/dispatch. This is necessary because removing the size
failure alone would publish fictitious initialization success. No other generic
service route is changed by this checkpoint.

Standalone `_Build/loginmgr-ipmi` configured with clang-cl. All **8/8** tests
passed, including the new real-SysV config/default/unaligned-canary/backing
fixture and seven existing transport/Core regressions. Log:
`_TempData/loginmgr-agent-l0-tests.log`. Initial configuration needed Windows
SDK tools on PATH; the corrected configure/build/test succeeded. No emulator
executable or shared build was built/overwritten. Full production TU/registered
resolver checks remain parent integration work; standalone checks compile the
actual shared implementation header, not all of libC/libShellHits.

## L1 component checkpoint

Implemented `LoginMgrNativeTransport::{Install,CreateServer,CreateClient,
RetryRetirements,Uninstall}` for **exactly** `SceLoginMgrServer`. Its factories
are selected before the generic libC IPMI path. Without installation they return
unsupported and preserve the caller's object output. `Install` requires every
hook and an owner pin; nothing in production installs it automatically.

The existing NP adapter and LoginMgr compile `nativeIpmiTransportImpl.inc` into
separate namespaces, each with independent Core, registry, hooks and vtables.
NP continues to select only `SceNpMgrIpc`; its event slots remain unsupported.
Include the new `.inc` in the parent commit: the small NP `.cpp` now includes it.
Root CMake already globs `src/libs/*.cpp` with `CONFIGURE_DEPENDS`, so the new
LoginMgr TU is discovered on the parent's next permitted reconfigure/build.

LoginMgr adds the audited 13.60 event methods:

| Native slot | Implementation / recovered wrapper |
| --- | --- |
| client `+0x20` | Poll `(self,u32 index,u64 bits,u32 mode,u64* output)`; `bd0 -> 5390`, selector `0x491` |
| client `+0x28` | Wait `(self,u32 index,u64 bits,u32 mode,u64* output,u32* timeout_us)`; `bf0 -> 52c0`, selector `0x490` |
| session `+0x40` | Set `(self,u32 index,u64 bits)`; `39a0 -> 5490`, selector `0x493` |

Negotiated event count is forwarded to the native connect callback at config
`+0x38`. A session may set its initial event during acceptance. Events persist,
AND/OR and clear modes come from Core, bounded waits expire, and a cancelled
host wait unlinks only its own ticket. Core now allocates a wait ticket before
observing/consuming bits and publishes it only after insertion succeeds.

The seven-argument sync handler remains responsible for session `+0x30` explicit
reply; returning from the callback never fabricates success. Peer PID is read
from the actual registered client and remains available during close. The
`0x30023` fixture checks the 12-byte input, four-byte output, rejection and
`(old & ~mask) | value` behavior. **Its flags/capability variable is test-only**;
the production adapter contains no LoginMgr flag/account cache or credentials.

Shutdown now atomically seals Core admission only after attached clients and
receive reservations drain. A Busy stop leaves the dispatcher running. Failed
factory rollback retains its unpublished address record and installed owner;
`RetryRetirements()` requires owning PID (and original receiver PID/TID for a
session). Registry erasure checks object identity, preventing stale rollback
from deleting a replacement. Failed ordinary destruction likewise keeps its
record. Caller-owned guest backing must still be retained by the runtime owner;
retaining a host registry record is not an independent guest allocation pin.

## Validation and exact remaining gates

Standalone `_Build/loginmgr-ipmi`: **10/10 passed** after the retention changes,
with the final log at `_TempData/loginmgr-agent-l1-tests.log`. Coverage includes:

- exact config stores, untouched holes, unaligned canaries and size;
- NP/LoginMgr name and instance isolation, NP event-slot exclusion;
- failed factory copyout, duplicate address rejection, rejected session reuse;
- real separate host dispatcher thread, negotiated events, actual fixture peer
  PID, connect authorization and explicit service-error/success replies;
- persistent/cleared events, timeout, individual wait cancellation, forced
  cancellation, delayed callback release and Busy stop/destruction;
- Core sealing/reconnect rejection and reservation-blocked sealing;
- injected failed rollback with a real Core reservation, retained backing
  authority/owner, wrong-PID retry and receiver-thread-specific session retry.

These tests use host callbacks, not original ShellCore execution. Full
production libC/libShellHits compilation and resolver invocation remain parent
checks. The shared production adapter/Core are compiled by these fixtures.

`docs/vsh-loginmgr-runtime-callback-proposed.patch` is an **unapplied** exact
parent patch for `RuntimeGuestExecution.{h,cpp}`. `git apply --check` passed.
It adds a bounded, nonvariadic integer/pointer callback path with six register
arguments and the seventh at target-entry RSP+8. The existing Callback lease
and `Invoke` boundary retain all host RAII above the naked transition. The
ABI-only `_TempData/loginmgr-agent-check-callback.py` extracts that exact proposed
assembly and passed a clang-cl probe for seven values, pointer output, signed
return bits, zero-padding and target stack alignment. It does **not** validate
runtime abort/stop/thread-exit recovery. Parent must test normal nested return,
abort, stop, thread exit, unsupported architecture and count>7 rejection with
the actual boundary after applying the patch.

Event operation errors currently preserve output and timeout; successful wait
writes snapshot then remaining microseconds. A copy fault can follow event
consumption, and the second copyout may fail after the first succeeded. Exact
firmware cancellation/copyout precedence is not claimed beyond the audited
surface. There is no adapter event-cancel slot, raw sync/queue family or
non-default async-option implementation. Pending-connect forced teardown,
process exit and disappearing-server recovery remain explicit integration
gates inherited from NP; a failed release must retain backing rather than
pretend cancellation completed.

## Retained VSH hook installation contract for parent

The runtime patch supplies the callback transition, **not** a boot-session
lifecycle. Current `runtimeLinker.cpp` has `g_tls_current_program` around line
624, calls `StartAllModules()` in the system-session path around 4320, launches
co-ELFs as detached host threads around 4329, and calls raw `RunEntry` around
4377. A current Program tag alone is not a retained execution owner. Existing
application execution controls must not be borrowed from the unrelated Apple
application or synthesized by casting a VSH Program to an application owner.

1. Parent establishes a retained VSH boot/session control and stable lifetime
   token, with explicit program mappings, guest PID/TID membership, callback
   target provenance, native dispatcher thread handle and backing ownership.
   Its guest entries/initializers and dispatcher callbacks must participate in
   the same supported boundary/host-call-return discipline. All of this must
   exist before installing hooks; missing prerequisites leave LoginMgr gated.
2. Install once before the first native LoginMgr initializer/factory can run
   (before `StartAllModules()` for that path). `Hooks::owner` retains the real
   control and required mappings/backing. Capture that owner strongly in any
   hook closure using it. NP installation remains an independent explicit
   decision; do not start an unavailable-account HLE broker.
3. `identity()` returns the **current guest** PID and TID from admitted runtime
   membership on each call, including client, dispatcher and teardown calls.
   Never use host thread IDs, a constant ShellCore PID, or the stored object's
   owner to impersonate the caller. Reject unbound threads with invalid identity.
4. Registration/connect hooks enforce actual guest credentials and service
   ownership. The peer-capability bit62 check belongs to the original LoginMgr
   handler's actual peer query. `cross_no_cap` is not authority to set arbitrary
   LoginMgr flags; unknown credential providers reject. Authorization hooks
   return BSD errno, not encoded `0x8002xxxx` words.
5. Copy hooks perform bounded overflow-checked, fault-translating copies without
   guest-handler dispatch. They must support guest mappings and explicitly
   scoped borrowed host descriptors/buffers used for the current callback.
   Do not treat arbitrary host addresses as admitted scratch. A failed copy
   may have written a prefix; no rollback of consumed events/replies is implied.
6. `invoke_guest` validates the target against the retained server provider and
   checks that the **current dispatcher guest thread** belongs to that owner.
   Then call `execution->InvokeIntegerCallback(entry,args,count)` synchronously
   on that thread. If `Returned()` is false, return a truthful BSD transport
   error (cancelled for stop/thread-exit, bad-message for boundary abort/failure)
   and leave `result` untouched. Only `Returned()` permits copying all 64 bits
   of `call.value` into `result`. Never use a raw function-pointer cast, a worker
   thread substitute, or a fresh boundary per callback. Preserve pending thread
   exit for its owning worker root; do not consume it in this transport hook.
7. `cooperate()` yields/pumps bounded host work, checks that same owner's stop,
   and never dispatches LoginMgr handlers on a waiting client's thread. The
   adapter retries EINTR on the same ticket. Non-EINTR cancellation can leave
   pending connect/disconnect state requiring retained cleanup; it is not proof
   that a factory, session or borrowed backing can be freed.
8. Stop new **producers first**, while the dispatcher execution control still
   admits the callbacks needed for draining. Drain clients/active replies and
   forced notifications, retry Busy releases, then stop/wake the dispatcher and
   join the actual guest thread. After callbacks/API calls are gone, destroy
   objects, retry failed retirements on their genuine owners, and Uninstall.
   Only then stop/drain/retire the execution control and release its mappings.
   Do not request execution stop before required cleanup callbacks can run.
   Any failed join/release/uninstall retains control, backing and retry state.

Next parent acceptance needs original LoginMgr startup/record preparation,
UserService property6 authoritative updates, and observed native set/get/event
behavior. Account ABI corrections, offline-ID state, and genuine remote
authentication remain prerequisites for Store sign-in; this component batch
does not establish VSH or Store E2E success.

## Review/commit contents

Parent review validation (2026-11-05): the five production translation units
`ipmi.cpp`, `libC.cpp`, `libShellHits.cpp`, `npNativeTransport.cpp`, and
`loginMgrNativeTransport.cpp` compile with production definitions in isolated
`_Build/loginmgr-production-1105/`. The parent then rebuilt the standalone IPMI
targets and all **10/10** checks passed. Source hashes and individual compiler
logs are retained there. Existing libC format warnings are unrelated to this
batch. An initial validation-script invocation failed on environment quoting
and PowerShell treatment of compiler warnings; the corrected invocation passed.
The emulator executable was not rebuilt, and these results do not establish
production hook installation or native LoginMgr acceptance. Seven-argument
runtime callback integration is proceeding as a separate stage.

Parent owns review and commits. Include the modified IPMI files plus new
`ipmiClientConfig.h`, `loginMgrNativeTransport.{h,cpp}`,
`nativeIpmiTransportImpl.inc`, `tests/ipmi/{client_config_tests,
loginmgr_transport_tests,retirement_tests}.cpp`, and standalone test wiring.
This handoff and the proposed patch are under ignored `/docs/**`; parent must
explicitly force-add those two artifacts if desired. No commit or index changes
were made here. Concurrent pthread/applicationReturningExports/rwlock changes
belong to the other batch.
