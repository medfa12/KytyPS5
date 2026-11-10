# Regular 13.60 NP EventQueue and local startup

## Implementation scope

The explicit regular `-NativeNpStartup` route now selects native72: the existing
54 ownership/workqueue/callout/empty-HTTP-pool exports, the two dispatcher
exports, and all 16 EventQueue exports/aliases. `-NativeNpDispatcher` alone
retains its native56 selection, and alternate startup retains native54.

This restores the provider family, not an `IsEmpty` return-value patch.
`SymbolDatabase::ExportPolicy::NpEventQueue` filters every database insertion
and is preferred before HLE resolution. The original NpCommon SHA256 pin and
same-open retained backing remain required. Host
`KYTY_NATIVE_NP_EVENT_QUEUE=1` requires all five existing family flags equal to
1; partial/malformed sets and alternate mixing are rejected. Child application
processes do not inherit this host provider selection.

The preceding database transport checkpoint `5038fa4f` passed the parent's
production `kyty_emulator` and `kernel_file_system_tests` build. Its source-bound
credential prerequisite remains separate from this native utility provider.

## Native queue audit

Audited regular NpCommon SHA256:
`a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3`
(411092 bytes). `tools/audit_np_event_queue.py` reproduces the export inventory,
direct control-flow closure and referenced vtable traversal. The initial audit
decoded 1415 instructions, followed ten vtable slots and found twenty imported
functions. The three indirect sites are allocator-owned calls at `+56ed`,
`+571e` and `+18715`, not arbitrary queue worker callbacks.

| Entry | RVA | Contract |
|---|---:|---|
| `esiO4He2WTU` / `OGAIG7dVmUk` | `1dd50` | Construct with allocator descriptor; initialize ring, mutex, two conditions |
| `8kUkQPQP7bA` / `lQXgvDXBGtA` | `1ddb0` | Non-deleting destructor |
| `N1gnYosdK7Q` | `1ddf0` | Empty native delete hook |
| `E4uoqSdo8ek` | `1de00` | Virtual deleting destructor; return prefixed object to allocator |
| `xu9qWN0YYC4` | `1de50` | Reset initialized-state word; not a safe live-queue teardown substitute |
| `b20e017Ei94` | `1de60` | Initialize `(name,itemSize,capacity)`; rollback allocation/synchronization failure |
| `qvpEuKumIGM` | `1dff0` | Terminate backing and synchronization; clear init state |
| `VI8AHrfLdqY` | `1e030` | Query initialized state |
| `jxPY-0x8e-M` | `1e040` | Read actual ring empty state |
| `AV5jHo8O3+E` | `1e050` | Enqueue exact-size item, bounded/infinite wait as requested |
| `suxln7PooIo` | `1e180` | Blocking/timed dequeue |
| `QlP4t2SGZ4I` | `1e2b0` | Nonblocking dequeue; empty returns `80559e16` |
| `slmKkuIoC28` | `1e350` | Set cancellation flags and broadcast |
| `aoas3bJANfY` | `1e3c0` | Clear selected cancellation bits; does not discard items |

Object extent is `0xa0`. Ring is at `+8`, mutex at `+40`, conditions at `+58`
and `+70`, item size/capacity at `+88/+90`, init/cancel state at `+98/+9a`.
Descriptor ABI is allocate/reallocate/free/context, with allocation error at
descriptor `+20`. Ring storage is a raw callback allocation; native object
new/delete additionally uses the existing `0x10` owner prefix. The queue copies
bytes; payload reference release belongs to the caller's drain operation.

Internal ring-buffer, condition and mutex calls stay in NpCommon. External
dependencies resolve to the production pthread mutex/condition implementations
(including timed wait and broadcast), `memcpy`, `snprintf`, printf/abort and
stack guard/failure. No new synchronization HLE or synthetic completion was
introduced.

## Why this is the demonstrated cleanup fix

psn30 and parent-observed psn33 first fail `SceNpMgrIpc` creation with
`8002004e`, then enter PsnCore cleanup. The push-callout join succeeds. The
following unresolved `slmKkuIoC28`, `jxPY-0x8e-M`, `QlP4t2SGZ4I` sequence
matches Core `+d14670`: cancel both queue directions, then repeatedly construct
a command, try-dequeue it, destroy the command and check actual emptiness.
The prior generic zero-return fallback made `IsEmpty` permanently false, while
constructor/init/enqueue/dequeue were also unavailable. Exposing just IsEmpty
would leave storage and payload ownership invalid.

Core wrappers `+d1be00/+d1be50/+d1be70/+d1be90` establish a 32-byte item ABI
and capacity 64. Core's CtlCmdQueue destructor `+d1bd30` asserts emptiness
before calling the base destructor. The implementation therefore enables the
whole family and leaves Core's command destruction/drain authoritative.

## Local-account route and remaining startup boundary

`-LocalAccountHle` selects guest `KYTY_NP_LOCAL_ACCOUNT_HLE=1`. The existing
local profile backend supplies user creation/login/status and private NP
sign-in snapshots: no account association means NotSignedUp, an existing
association means SignedOut. This is independent of native NP admission and
does not provide tickets, online sessions or native service callbacks. Parent
psn33 evidence confirms local login succeeded for `0x10010004` and Home was
launched, while the screen remained background-only.

Core's PsnCore initializer `+c4d6c0` registers `SceNpMgrIpc` unconditionally at
`+c4d767`. On failure `+c4e1ac` calls cleanup, then returns the original error
at `+c4e1b1`. ShellCore's initializer loop `+a79600` records nonzero errors at
`+a79680`, queues an UnexpectedErrorJob (`+a84940`), and continues later table
entries. At the end, the accumulated failure takes `+a796d4`, requests safe
mode and calls `sceLncServiceSystemReboot` (`+70a1b0`). Thus successful queue
cleanup can unblock later initializers but is not successful full startup.

The available `NpManagerService::StartUnavailableService` is a distinct HLE
broker with local connections/events and an unavailable account backend. It
has no production startup caller. Starting it is not evidence that native
PsnCore initialized; it cannot stand in for native callback owners or supply
database recovery results. A full local startup route would need an explicit
emulator-owned startup/service boundary in place of the required native NP
initializer, paired teardown and accurate unavailable downstream operations.
Neither relabeling an error as native success nor filling the native initialized
bitmap establishes that boundary. This checkpoint does neither.

The source-bound NP/database transports still require active-build Core/UI auth
records, which remain unverified. Local profile state does not confer those
credentials. Native database request admission, schema creation and Home queries
remain to be observed after the queue correction. No shared databases/profiles
or corruption markers were changed.

## Parent runtime validation

Use the regular firmware/profile and retain the existing psn33 launch settings,
adding/preserving **`-LocalAccountHle -NativeNpStartup`**. Do not supply alternate
auth records or select alternate startup. The launcher supplies all host family
flags and the matching regular NpCommon provider; no manual environment is needed.

Expected evidence is `NP regular72: native EventQueue lifecycle selected`, native
bindings for all Core queue imports, real queue allocation, and cleanup passing
the previous unresolved drain loop. NP server creation may still return
`8002004e`; preserve that result. Capture subsequent initializer timings/errors,
DbRecovery/DbPreparation server/client behavior and request/status outputs.
Visible Home recovery must be verified separately.

No live emulator restart, controller input or debugger attachment was performed.

## Verification

- Built `virtual_memory_allocation_tests` and `symbol_database_tests`, then
  rebuilt the former after adding the dependency-binding assertions.
- Real regular-image `--np-native-event-queue-only` passed with
  `[native] NpNativeEventQueue ok`: all twenty imported function bindings,
  allocation failure rollback/retry, FIFO/wraparound, full-queue timeout,
  producer/consumer wakeup, cancellation/drain, clear cancellation,
  termination and paired virtual deletion.
- Real-image `--np-alternate-startup-only` regression exited successfully with
  the original and alternate image inputs.
- `symbol_database` and `vsh_subprocess_lifecycle` passed; the subprocess test
  and probe targets were rebuilt before verification.
- `np_allocator`, `np_mutex`, `np_atomic`, `runtime_user_service_owners` and
  `runtime_user_service_controllers` passed (5/5).
- Launcher PowerShell syntax and extracted startup/dispatcher/alternate/HTTP
  selection checks passed. The read-only pinned-image audit passed.
- `git diff --check` passed.
- Production `kyty_emulator` build passed on incremental retry with shared-tree
  HEAD `158bd7d6` (including EventQueue checkpoint `bf0f8147`). The first build
  failed at link on `IsFsTcbRead` / `TryEmulateFsTcbRead`; the retry rebuilt
  `x64InstructionEmulator.cpp` and linked successfully using the definitions
  now present in the shared tree. No EventQueue source correction was needed.

Reproduce the real queue fixture in PowerShell:

```powershell
$env:KYTY_TEST_NP_ORIGINAL_IMAGE = '${USERPROFILE}/Downloads/13.60/system_b/common/lib/libSceNpCommon.sprx'
& _Build/vsh-integrated/virtual_memory_allocation_tests.exe --np-native-event-queue-only
```

These are component/build results. Post-fix Core cleanup, native database
requests and visible Home remain pending parent-controlled runtime validation.
