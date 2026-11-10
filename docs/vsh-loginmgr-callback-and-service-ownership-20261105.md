# LoginMgr callback implementation and VSH service ownership — 2026-11-05

## Callback batch ready for review

Applied the proposed seven-argument transition in owned
`src/loader/runtimeGuestExecution.{h,cpp}` as `InvokeIntegerCallback`.
It accepts a trusted host array of 0–7 integer/pointer values, zero-pads unused
lanes, puts argument seven at target-entry RSP+8, and retains the existing
Callback lease plus `Invoke` boundary. All host-owned arrays/RAII remain above
the naked transition. Invalid count/null arguments/null target fail before
dereferencing the argument array, following the existing sticky
BoundaryUnavailable policy. Stopped admission returns NotInvoked first.

Review differences from the original proposal: argument validation is explicit,
the new assembly excludes ARM64EC, and the unsupported implementation returns
Unsupported. Float/vector/aggregate/variadic/hidden-return ABIs are outside this
bounded entry point. Supported execution still requires the real boundary's
capability check; no architecture fallback calls a raw guest pointer.

### Actual-boundary tests

New `tests/RuntimeIntegerCallbackTests.cpp` uses production
RuntimeGuestExecution, GuestCallBoundary and HostCallReturnThunk, not a copied
assembly probe. It checks:

- seven distinct values, pointer output, negative-return bits, zero padding,
  zero-argument entry and SysV stack alignment;
- nested normal callback on the same actual thread, with both host-return
  gateways and callback/guest/host lease counts;
- unmarked inner guest abort becoming sticky GuestAborted, nested host RAII
  destruction, outer return-gateway propagation and suppressed continuations;
- cooperative owner stop from an actual inner host gateway, separately from
  failure and normal thread exit;
- pending exit through both boundaries and host coordinator RAII, reentry
  refusal, peer-thread independence, root-only single consumption, and failed
  inner consumption retaining the pending exit;
- **real** owner-bound pthread coordinator: callback calls actual PthreadExit
  with argument seven; RunThread consumes only after coordinator destruction;
  PthreadJoin returns the exit pointer rather than the coordinator's return.
  A second worker verifies the owner remains usable and enters with clean TLS;
- count>7 with deliberately unreadable input, null input/target and stopped
  admission, with no leaked work or host-critical/TLS imbalance.

Passed on Windows x64 clang-cl in `_Build/loginmgr-callback-boundary`.
Own sources were copied to `snapshot/`; `snapshot-sha256.txt` records those
inputs. The three execution TUs were rebuilt from snapshots, and remaining
production/common objects were linked read-only from the existing component
overlay. Compiler/linker/test outputs all live in this isolated directory.
The initial minimal link needed the common production dependencies; the real
pthread case also required normal config/log/memory initialization, now part
of the test. Final test exit is zero.

Existing regression overlay results, using the new execution objects:

| Case | Result |
| --- | --- |
| `--runtime-guest-execution-only` | passed, exit 0 |
| `--runtime-guest-threads-only` | passed, exit 0 |
| `--runtime-guest-stop-only` | passed, exit 0 |

Evidence: `_Build/loginmgr-callback-boundary/{test.log,validation-summary.txt}`.
Regression logs are empty under those fixtures' silent logging; exit statuses
are recorded in the summary. Scripts are
`_TempData/loginmgr-agent-{build-callback-boundary,callback-regressions}.py`.
The test's unsupported-platform branch checks actual Create refusal, but that
branch was **not run on another architecture**. No arbitrary-fault recovery or
native firmware E2E claim follows from these synthetic guest callbacks.

### Parent wiring request

The test now includes the actual pthread-root case, so use the existing full
component-test helper (supersedes the earlier minimal-source wiring suggestion):

```cmake
add_kyty_full_emulator_test(runtime_integer_callback_tests
    "${KYTY_TESTS_DIR}/RuntimeIntegerCallbackTests.cpp")
target_link_libraries(runtime_integer_callback_tests ZArchive::zarchive spdlog::spdlog)
# In the existing test-registration scope:
add_test(NAME runtime_integer_callback COMMAND $<TARGET_FILE:runtime_integer_callback_tests>)
set_tests_properties(runtime_integer_callback PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
```

No VM-main dispatch entry or test-only runtime macro is required. Parent owns
root CMake wiring. The test itself and the two runtime files are the commit
contents for this stage; this ignored `/docs/**` handoff needs explicit inclusion.

Parent has added the full-emulator test target and CTest registration above to
root `CMakeLists.txt`. The recorded passing runs remain isolated component-overlay
validation; a reconfigured combined-tree build is pending the subprocess worker's
coherent stage handoff. This callback API is not installed as a production VSH
service hook by the checkpoint.

## Concrete production ownership assessment

Scope is the existing cohosted VSH's LoginMgr/NP services. Standalone app
subprocess/surface/focus/Home/close work is separate and does not need a shared
application owner for this service plan. No service hooks were installed here.

| Requirement | Actual current implementation | Consequence |
| --- | --- | --- |
| Current guest TID | `PthreadGetthreadid()` uses `g_pthread_self->guest.thread_id`; zero if absent (`pthread.cpp`, about 5740). | A usable genuine guest TID exists. Require an actual live pthread record. |
| Current cohost Program | `PthreadInitSelfForMainThread` captures owner_program; PthreadCreate captures/inherits the creator Program; `PthreadGetOwnerProgram` is available. | Use the live record on its actual thread. RuntimeLinker::GetCurrentProgram alone can fall back to the main Program and is not proof of membership. |
| VSH process IDs | `runtimeProcessIdentity.cpp:118–126` maps every null application_owner to PID100. Only application owners can register distinct runtime PIDs. | ShellCore/ShellUI/SysCore peers cannot currently be distinguished by this API. |
| Peer auth-info | `libShellCoreFloor.cpp:322` KGetAuthInfo delegates to TryGetGuestProcessAuthInfo. `runtimeProcessIdentity.cpp:146–152` supplies legacy authority and bit62 for PID100; configured boot ShellUI has another role-derived bit62 path at 94–99. | These compatibility values are not independent provenance for a LoginMgr setter peer. Do not reuse PID100 or its fallback to authorize a genuine service call. |
| Credential metadata | AuthorityMetadata retains an authority ID/digest/raw extended-info record; it does not parse a credential capability bitmap. Runtime snapshots otherwise default capabilities to zero. | Actual per-process bit62 and cross-process credential-class predicates are still missing providers. Missing credentials must reject. |
| Execution control | GetApplicationExecution requires `application_owner->application_owner == owner`; VSH Programs have null application_owner. | The new callback API is usable only after creating a distinct, retained service execution lifetime and binding it explicitly. |
| Pthread exit under callback | `KytyPreparePthreadExit` (`pthread.cpp:5567`) consults GetApplicationExecution; a nonzero boundary with no matching execution selects InvalidGuestExit. | Merely calling the new callback API on an existing VSH worker would still mishandle PthreadExit. A service-aware exit/root association is required. |
| Host-return gateways | ApplicationImportBindings captures GetApplicationExecution and validates application provider ownership. Existing VSH imports are not owner-bound service gateways. | Reuse the tested thunk engine with a service-specific exact provider/import contract, not the application ownership checks. |
| Provider lifetime | Program is raw loader-owned storage; Clear/DeleteProgram can delete mappings. A shared execution control alone does not pin Program text/data. | Explicit retained service provider/backing pins must gate deletion and outlive final callbacks, API calls and join. |
| Cohost entry lifetime | Execute launches co-ELFs through detached Common::Thread; RunCoElfEntry creates a main-style pthread record and calls raw RunEntry. | No general joinable cohost execution owner exists. Limit new ownership to admitted service dispatchers; do not claim whole-session teardown. |
| Initializers | StartAllModules holds the loader mutex while calling StartModule; null-owner StartModule directly invokes native init. | Do not add blocking callback/service startup under that outer lock. Existing initializer caller attribution must be audited explicitly. |
| Fault-translating copies | libC has private Windows guarded-copy helpers/VEH cooperation; range checks plus memcpy are not a general lifetime/fault-safe transport copy contract. | Supply an explicit bounded guest/scoped-scratch copy helper with faults translated to BSD errno. |
| UserService property6 | Recovered ShellCore manager/record helpers own selector6; public HLE UserService store does not supply that native manager seam. | Native user-record preparation and a coordinated authoritative property seam remain prerequisites after transport. |

### Recommended bounded next implementation sequence

**S1 — service identities, provenance and retained registration (no admission yet).**

- Proposed new owned files: `src/loader/vshServiceRuntime.{h,cpp}` and focused
  component tests. Define a retained ServiceLifetime with immutable service
  role, server Program/provider set, execution control, explicit guest PID/TID
  bindings, dispatcher join handle/state, backing pins and drain state.
- Parent-owned `runtimeLinker.{h,cpp}` supplies narrowly scoped provider pin /
  release APIs checked by deletion, plus registration for the actual cohost
  Program records. The module ID is not a guest PID. Don't repurpose
  application_owner or register a shell service as an application.
- Requested coordinated changes to `runtimeProcessIdentity.{h,cpp}`: a distinct
  cohost Program registry, stable non-reused PIDs, explicit credential provenance
  and query-by-peer-PID. Before enabling LoginMgr, its getpid/auth-info view must
  agree with the adapter's identity. `libShellCoreFloor.cpp` must query those
  real admitted records without falling back to boot role credentials.
- Recover and supply the credential bitmap/class provider before admitting
  capability-dependent methods. Authority ID, service name and extended-info
  digest alone must not grant bit62. This is an explicit external dependency,
  not work solvable by another flag cache.
- Audit actual LoginMgr factory caller ownership and native client-global
  storage: null-owner cohosts currently share provider mappings. If one native
  client singleton is shared across distinct process callers, it cannot be
  assigned a new PID at each invoke. Establish exact client-provider state
  ownership/isolation for admitted callers before per-process transport use.

**S2 — retained dispatcher callback boundary and exit/return integration.**

- In the new service runtime, bind an actual dispatcher pthread to the service
  lifetime on that thread; validate guest TID, Program and provider executable
  ranges. The callback uses the one retained RuntimeGuestExecution through
  InvokeIntegerCallback. A service callback never borrows a running app control.
- Coordinate a small `pthread.{h,cpp}` extension with Sol/parent for explicit
  service-worker ownership, Thread lease retention, service-aware exit lookup,
  root consumption and an actual join handle. These files are not owned by this
  callback batch. Do not globally change GetApplicationExecution semantics.
- Add service-specific import gateway ownership for the audited handler import
  closure using HostCallReturnThunk. Pointer-vtable host calls also need the
  same return/stop discipline. Inventory every reachable callback/export ABI;
  do not classify arbitrary VSH imports as safe returning calls.
- Add bounded copies with explicit guest memory and scoped borrowed descriptor
  spans. Test fault/partial-copy behavior and backing retention independently
  from runtime callback recovery.
- Keep raw cohost main threads outside the claimed ownership surface. Required
  service startup occurs at a parent-reviewed safe point without loader locks;
  the dispatcher and admitted callbacks are the lifetime being implemented.

**S3 — install the exact adapters and validate native local state.**

- Reopen the frozen transport files only by parent coordination if binding or
  scratch-range hooks require an API addition. Install LoginMgr hooks only when
  S1/S2 prerequisites are available; NP remains independently installed and
  exact-name scoped. Authorization uses actual process records; callback return
  never becomes an implicit IPMI reply.
- Coordinate native record preparation/property6 with the authoritative
  UserService owner. Then test original LoginMgr initialization, actual peer
  rejection, `0x30023` explicit replies, readback and event notification. Account
  offline-ID ABI/provenance and remote PSN authentication remain subsequent
  service prerequisites.
- For shutdown, close service producers first while cleanup callbacks remain
  admitted; drain clients and callback/copy leases, stop/wake and join the
  dispatcher, release native objects, retry failed retirements, uninstall, then
  retire the execution control and release provider/backing pins. Failure at
  any step retains its state. A timeout is not a successful join or release.

This sequence permits a bounded service-runtime batch without rewriting the
whole boot session. It still requires the listed identity/credential and
pthread/import seams before truthful production installation. AppDb transport
and the parent's AppBrowse/OOBE audit are outside this implementation plan.
