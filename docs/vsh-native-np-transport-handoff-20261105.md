# Native NP transport batch — parent handoff

## Parent integration update (2026-11-05)

All seven standalone IPMI/transport targets now build and pass with clang-cl:
`_TempData/np-transport-tests.log`. The initial standalone build exposed a
missing generated `cmake_config.h` dependency in the native adapter's production
ABI header. `tests/ipmi/CMakeLists.txt` now generates its own common configuration;
the suite does not borrow an existing emulator build's headers.

`SymbolDatabase::ExportPolicy::NpDispatcher` is integrated as a separate
native56 policy, selected by exact host `KYTY_NATIVE_NP_DISPATCHER=1` for
NpCommon. The loader's existing scoped native-first resolution now includes this
policy before HLE lookup, including fallback paths. Native54 remains unchanged.
Symbol-database visibility tests pass, covering the two added functions and
continued exclusion of object/unrelated exports. A production-resolver regression
also exercises early/late HLE precedence and private-provider isolation. The
VM-target build succeeded and `application_symbol_scope` passed:
`_TempData/np-dispatcher-resolver-tests.log`.

These are component checks. Runtime hook installation, actual native dispatcher
creation/join, native manager-client attachment, incomplete forced teardown and
the first PSN E2E remain outstanding. The current visible shell uses the earlier
executable and has not been restarted for this transport batch.

**Implemented, unbuilt/unrun here.** Parent owns build/test/live/commit. This
batch establishes transport code and fixtures, not observed native acceptance,
remote PSN traffic, authentication, or account provisioning.
Parent has separately integrated the seven IPC bindings and worker HTTP fixture
and reported the IPC failure-unmap retention/retry recheck passing. These new
transport targets have not yet been part of that validation.

## Files and behavior

* `src/libs/npNativeTransport.{h,cpp}`: exact `SceNpMgrIpc` server/client
  factories; caller-owned native-sized server/client/session vtables; Core
  registration, receiver-bound acceptance, explicit sync reply, async start /
  explicit reply / consuming collect, ordinary disconnect, forced notification,
  dispatcher stop/wake and busy destruction. `libC.cpp` routes the exact NP
  server factory here; with hooks installed, its exact NP client factory also
  selects this route. Other libC edits remain concurrent parent/Sol work.
* `src/kernel/ipmi.{h,cpp}`: rejected acceptance rollback (with or without a
  created session), expected-server validation at session creation, transport
  failure completion, finite connection-scoped async tickets, separate async
  read/consume/slot-release phases, and attached-client forced teardown.
  Pending events/queue writers are cancelled; async slots and reply packets are
  retired; live receiver and payload-copy leases defer session cleanup. Repeated
  termination while terminating emits no second notification. Queue reservation
  release processes deferred flush before new admission.
* `src/kernel/ipmiGateway.cpp`: accepts kernel+`1c` only through its existing
  explicit connect-authorization hook. Restricted+`20` stays excluded. Gateway
  async syscall selectors and520 are not wired by this batch; the production
  NP vtable adapter uses Core directly. Do not route a native libSceIpmi syscall
  provider to this gateway and assume those operations are covered.
* `src/loader/npDispatcherExports.h`: **separate native56** predicate, native54
  plus exactly `fhJ5uKzcn0w` / `EjMsfO3GCIA`. Existing native54 identity unchanged.
* Runner: `-NativeNpDispatcher` implies startup and existing dependency chain,
  verifies original NpCommon via that chain, sets host
  `KYTY_NATIVE_NP_DISPATCHER=1`; omission clears it. The option requests the
  parent-owned integration; it does not install the adapter itself.

## Exact parent integration

1. Root CMake already globs `src/libs/*.cpp` and kernel sources with
   `CONFIGURE_DEPENDS`; new adapter files exist. Refresh the parent's build as
   needed. No root CMake/VM dispatcher/runtimeLinker/pthread code edited here.
2. In the parent-owned loader policy, select `IsNpDispatcherExport` under the
   new host option and preserve native54 for the old option. Make both **exact
   and fallback** resolution prefer the admitted native dispatcher pair over
   early/late success-only HLE bindings. The create ABI is seven arguments:
   `(threadOut, entry, arg, i32 priority, u64 stack, u64 affinity, name)`;
   native join must join the actual thread. Keep provider hash/owner checks.
3. Before native ShellCore's NP factory runs, explicitly call
   `NpNativeTransport::Install(Hooks)` from the admitted VSH lifetime. All hooks
   are mandatory. `owner` must pin the actual runtime/module lifetime. Derive
   identity from **current guest** process/thread ownership (`PthreadSelfOrNull`,
   `PthreadGetthreadid` are available); never use a host thread ID or unrelated
   application's identity. Install is one coherent transport instance; duplicate
   install is Busy. Do not start the unavailable-account HLE broker.
4. `copy_in`/`copy_out` are bounded fault-translating copies returning BSD errno.
   They must support the bridge's borrowed descriptor/buffer addresses as well
   as guest storage. A failed copy may have written a prefix. `invoke_guest`
   must enter the existing owner-pinned guest execution boundary **on the
   current dispatcher thread**, support up to seven integer/pointer arguments,
   and return the callback's value separately from a boundary failure errno.
   Callback arguments and buffers are borrowed only for that call. All hooks
   execute outside Core and adapter locks. `cooperate` must yield/pump waiting
   work without running service handlers on the blocked client's thread.
5. Registration/connect policy must use actual caller authorization. New
   adapter hooks return BSD errno, not guest error words. The inspected kernel
   same-PID connect branch bypasses the *cross-process* caller-class tests;
   foreign-PID policy needs trusted credential predicates. Missing or unknown
   policy must reject, not authorize every caller because the name starts Sce.
6. Stop native producers, terminate/drain clients, allow forced callbacks to
   close sessions, stop/wake the dispatcher, then **join it**, destroy objects,
   and `Uninstall()`. Busy retains backing and the installed lifetime pin.
   An active request's reply publishes separately from receiver release: the
   latter is delayed until the native callback returns. No native session
   backing may be freed on a logical cancellation alone.
7. The adapter serves native IPMI factory/vtable callers. It does **not** attach
   an `NpManagerClient::Endpoint` to the HLE manager session. If that HLE client
   remains selected, parent needs a host endpoint with genuinely nonblocking
   Poll and the named-event methods0/1/3/4 contract, or the independently audited
   native NpManager caller path. Never invoke a guest handler under that
   endpoint's serialized client mutex. This is still an integration gate.

## Corrected native flag and dispatch evidence

Original hashes and ABI sources remain in
`vsh-native-np-lifecycle-re-20261105.md`. **Correction to its initial report:**
ShellCore sets userland config byte+`18=1`, which libSceIpmi `16df..16f7`
converts to **kernel config+`1c`**. Kernel multi+`18` comes from userland+`32`.
The NP-shaped gateway fixture is corrected; independent multi-slot admission
retains its own case.

`_TempData/np-lifecycle-re-connect-policy.txt`, kernel `805a68c0`:
* `68e7..68f0`: current guest PID equals server PID -> `696c`.
* Otherwise helper `808192d0` nonzero and absent server bit`40000` -> errno`d`.
* `692f..6967`: helper `80818f90` zero and absent server bit`20000`
  (kernel config+`1c`) -> errno1. Credential helper names/ownership must not be
  invented; the branch and required bits are proven, production credentials
  remain parent input.
* `6993..69c0`: absent server bit`10000` rejects >=2 sync slots with errno`16`.

Native callback ABI: handler+`20` connect receives server, session config,
optional argument, size; session config+`148` is memorySize`10`. Handler+`10`
sync receives session, method, DataInfo/count, BufferInfo/count. It explicitly
replies via session+`30`; callback return is not a service result. Handler+`18`
async (`Ipmi+2948`) receives session, **ticket, method**, DataInfo/count; return
is ignored, then native dispatch completion is performed. A reply is a separate
session+`38` call `(session, method, ticket, result, BufferInfo*, count)`.
ShellCore's async callback directly returns `800205c1`; the native dispatcher
does **not** synthesize a reply from that return. NP logical async requests
using sync methods are distinct from IPMI async tickets.

Forced handler+`30` receives native session, like ordinary handler+`28`.
Both ShellCore paths drain logical requests, read peer PID, close, and free
session backing. Original Ipmi dispatcher type`a0` does not send ordinary222.
Peer query302 checks **session-present bit20000**, not the high connection
state (`809ef269..809ef28a`, `_TempData/np-lifecycle-re-peer.txt`); peer PID
therefore remains available before close during disconnect/termination. The
old gateway fixture's NotConnected expectation was corrected accordingly.

## Parent test targets (all unrun here)

`tests/ipmi/CMakeLists.txt` wires:

* `np_native_transport_tests`: native-sized vtables, real separate dispatcher
  **host fixture** thread, registration/connect authorization, callback-created
  rejected-storage reuse, accepted sync/two-output explicit reply, ordinary
  disconnect/reconnect, async zero-ticket/pending/consume/repeat, consumed-result
  copyout failure, forced wake while callback owns session, cleanup/stop/join.
  Its callbacks are host fixture code, not execution of original ShellCore.
* `ipmi_lifecycle_tests`: rollback without/with session, wrong receiver/server,
  pending/unknown/repeat async tickets, service-error consumption, cross-thread
  async reply, forced receiver/response-copy/queue-copy deferral, event/writer
  cancellation and idempotent termination, async-copy deferral, and callback
  boundary failure as a transport error without a fabricated method result.
* Updated `np_native_acceptance_tests`: corrected NP flags, full`2000` argument,
  independent multi-slot admission, two outputs, interruption, allocation sweeps.
* Existing `ipmi_state_tests`, `ipmi_gateway_tests`, `ipmi_wire_tests`,
  `ipmi_diagnostic_tests` remain required regression targets. Former assertions
  that valid rejection/force must be unsupported were adjusted; unauthorized
  teardown still leaves pending state unchanged.

Parent LNC contract/evidence: **`vsh-ipmi-async-tickets-1360.md`**. Pending known
ticket=`80020023`; unknown/repeated consumed ticket=`80020016`; zero is valid;
service errors consume on transport success. No LNC changes here.

Static review only: repository-configured scoped `git diff --check` and a
line-ending-normalized trailing-whitespace scan of the new adapter/policy/test/
handoff files passed. An initial autocrlf-disabled check treated existing CRLF
as trailing whitespace; no broad line-ending rewrite was made.

## Remaining boundaries

* Forced pending-connect, server-disappearance/process-exit races are not
  implemented. Reserved dispatch copy and an already-running ordinary
  disconnect callback return Busy on force before mutation; retry after release.
* Native IPMI message-queue client vtable methods and non-default async options
  remain unsupported in the NP adapter. Client queue negotiation is rejected.
  Raw sync/timeout/pointer-query vtable slots beyond the implemented family
  remain explicit unsupported; verify actual NpManager use before admission.
  The NP factory admits multi=0/restricted=0; independent kernel multi support
  does not bypass libSceIpmi's separate service-name whitelist.
* `NpManagerClient` endpoint installation, the native dispatcher-pair loader
  selection, genuine thread creation/join and runtime hook installation are
  still parent integration work. Native54 alone cannot exercise this path.
* Environment getter/RegMgr ABI correction, seven IPC heap bindings/fixtures,
  networking and TLS option/provisioning evidence remain parent-owned. No TLS
  verification relaxation, credentials, account state, or PSN result fabricated.

For the parent's next live run, keep `LogName` short (e.g. `np1-1105`). Parent
observed a long-name compositor AF_UNIX path exceed Winsock's108-byte limit
before app startup. This batch only adds dispatcher option plumbing to the
runner; it does not change IPC-directory naming or interact with the live run.
