# Ready offline: retail fix3 native NP-connect diagnostic — 2026-11-09

## Outcome and boundaries

Prepared a **new, independently pinned** diagnostic for the exact executable
`retail-plus-service-fix3/kyty_emulator.exe`, SHA256
`16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd`.
It follows one real retained local NP connection through dispatch, native session
registration and Core::Accept. Offline validation passes. **It has not been
attached/live validated, and no new production defect or PSN login is claimed.**

Task files only:

- `tools/retail_np_connect_trace_1360.py` — pinned native ABI/decoder/model.
- `tools/capture_retail_np_connect_1360.py` — offline-default audit, explicit
  fresh-barrier attach/release and metadata-only analysis.
- `tools/test_retail_np_connect_trace_1360.py` — synthetic controls.
- This document.

No emulator launch, process/thread access, input, account action, store read/edit,
native invocation, launcher edit or firmware modification was performed. The
parent's ordinary localhome8 run is **not** a target: the tool refuses a running
ordinary session without the exact fresh unreleased PID/token barrier. No PID
or creation time defaults exist. No parent/RNPS preflight files were edited.

The previously completed terminal-error propagation fix is not duplicated or
changed. In particular, the diagnostic does not fake a native connection, remove
the existing1000ms deadline, cancel a retained ticket, call Core::Accept, or read
any platform-auth/envelope input. Missing secure device backends remain separate
PSN prerequisites; the user supplies no account/device secrets for this task.

## Actual IDA and pinned ABI

Used installed **IDA Pro9.3** on an exact private copy of fix3, not the old
theme-session executable. Targeted native disassembly is retained in
`Temp/opencode/retail-np-connect-capture1/host-ida.txt` with its IDA log and
script. The copy hashes to the candidate SHA above. The map located functions;
actual IDA, independently checked instruction bytes and direct-call targets
establish the probe contracts.

Re-ran actual IDA/Hex-Rays for selected retail Core sites against the owned
private Core database, output `core-sites-ida.txt` with spec/log in the same
directory. Original Core SHA256 is
`f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`.
The private IDA view SHA256 is
`a4072f89b8c7d416afae8216c21a0bc718dc5aa853c40739bc1819035acdf669`;
only ELF e_type differs, as documented in the preceding PSN audit. Firmware
and code are unchanged. Core is the original retail input, not a counterpart.

| Site | Meaning / decoded facts |
|---|---|
| Core`caaa80` | Original NP server pthread entry; verify exact argument block`213cae8`, original server global and fix3 NP server vtable. Entry coverage ends at retained acquisition; absence later is **not** proof the server never ran. |
| Host`937913` | Retained BeginConnect's **pre-enqueue** call to Core::Connect`3bf540`; Windows RCX=Core, R8=actual packed retained caller identity, R9=client handle. Verify original RetainedState owner/handle/Idle and empty argument-vector metadata. Do not follow any argument bytes. |
| Host`937918` | Optional return of that exact enqueue call: read only stack Result.errno and whether the actual ticket pointer is nonnull. This return can race server dispatch and is not required if the matching dispatch itself proves enqueue. |
| Host`93e054` | NP Run's connect-only Callback invocation. Match exact non-reused client handle at frame+138, Dispatch::Connect at+130, caller identity at+148 and owning Core at frame+4c8+2d0. Validate native handler/server/config arguments and receiver identity. This acquires the **actual matching** server dispatch before following guest code. |
| Core`caae10` | onNewClient entry: exact handler/server/config values from that dispatch, actual dispatcher OS thread, retail handler vtable`202e0a0` and relocated slot+20=`caae10`. |
| Core`cb2020` | Real AddSessionContext entry; verify handler's manager and original caller return`caae83`. Session/storage pointers are compared internally only, never dereferenced or emitted. |
| Core`caae83` | AddSessionContext result in EAX; require the original onNewClient stack frame. |
| Core`caaf86` | Original onNewClient result in R14D before its epilogue. Success requires the observed successful session-registration boundary. Early real failure remains failure. |
| Host`93e085` / `93e08a` | Original NP Run's Core::Accept call and return; match Core/client/receiver/frame and exact native result. Read only Result.errno at the fixed caller stack result slot. |

All relevant relocation-free instruction windows are checked against full-hash
pinned local images and again in target executable mappings after attach. The
host NP server table at`212abf0` is independently checked: RunDispatcher`93c6e0`
and CreateSession`93c9a0`. Source-bound Core admission is required in the small
pre-guest prefix (`NP: source-bound Core ... adapter install=0`), with exactly one
original Core loading path/mapping and matching unreleased PID/token barrier.
The native server/handler/vtable/dispatch checks then bind observation to the
actual source-backed route. These are firmware/IPC admission checks, **not PSN
account credentials or remote Sony approval**.

Do **not** assume the retained client PID equals the Core server PID. Production
`StartForCurrentOwner` starts workers for the actual source-admitted caller
program, which can be ShellUI. The observer matches the identity in the real
RetainedState with the original pre-enqueue call and then the server dispatch;
the native config's peer PID follows that captured client identity. The original
source-bound authorize-connect hook at host`9377de` must have returned success
to reach the observed enqueue site; that branch is also pinned. A positive PID
by itself is never treated as admission. Synthetic controls cover distinct
client/server PIDs and same-process calls without reading any credential record.

## Four slots and correlation

Only four hardware **execution** breakpoints are active. No opcode traps,
WriteProcessMemory, software INT3 patching, guest calls or result changes.

1. Initial: server entry / retained pre-enqueue / NP dispatch / onNewClient.
2. At pre-enqueue, before the operation runs: replace server-entry coverage with
   enqueue return and retained-submit coverage with session-registration entry.
3. At the correctly acquired native callback entry: replace enqueue-return
   coverage with native-result and dispatch coverage with session-register return.
   If dispatch raced enqueue return, it already proves a queued matching ticket;
   the missing return is not called a transport error.
4. At native result: replace session-return/onNewClient coverage with the
   original host Accept call/return. Stop after that first Accept return, or after
   a real enqueue error, then perform the existing quiescent restore/detach.

Code-only slot migration and queued-retired-trap ownership reuse the existing
`retail_plus_trace_1360.handoff`; its executable profile is **not** reused or
weakened. Six old addresses are retired by the full plan; none is reused or moved
across slots. A partial SetThreadContext failure journals new and old plans for
the existing cleanup path. Tests cover this, queued faults and the full plan.

The retained client handle, packed caller identity, Core pointer, dispatcher
thread, exact native arguments and saved frames are used only for internal
correlation. Unrelated NP clients are excluded before their native arguments
are followed. Metadata reads are small and fixed; the valid synthetic full chain
uses under512 bytes. No credential records, HTTP data, account IDs, tokens,
cookies, strings, raw stacks/registers or arbitrary payload graphs are emitted.
Only fixed event names, stage/error codes, safe thread IDs and booleans leave
the decoder. Frozen prefix content is not echoed; even the diagnostic barrier
token is represented only by its hash in provenance.

## Verification completed

- New observer: **29/29 offline tests pass**. Coverage includes the full accepted
  chain, callback/enqueue-return race, early native rejection, enqueue/Accept
  failures, foreign-client exclusion before heap reads, stale retained state,
  wrong owner/vtable/config/manager/caller/frame/receiver/result, missing real
  session registration, lifecycle gaps, trace injection/reordering, partial
  capture, exception/budget/cleanup uncertainty, prefix/target/source guards and
  four-slot handoff/queued-trap/failure ownership.
- Existing Plus decoder/migration controls: **26/26 pass**.
- Existing interval sampler/privacy/balancing controls: **11/11 pass**.
- Existing pure hardware-context/restore/Home decoder controls: **34/34 pass**;
  fake handles/buffers only, not a Windows target-process test.
- Actual fix3 + original retail Core offline audit passes all pinned code,
  direct-call and server-vtable checks with `process_access:false`.
- Old Plus profile still rejects fix3 in the new regression. Old theme-session
  profile/tool source files are unchanged; old observers must keep rejecting it.
- No C++/runtime/provider change or production rebuild was needed.

Combined final offline run: **100 tests, zero failures/errors**. Private
`retail-np-connect-capture1/offline-receipt2.json` pins the tested observer/helper
sources, before/after read-only input hashes, exact candidate/Core hashes, actual
IDA reports/specs/logs and `offline-tests2.txt`. Its flags explicitly retain
`process_access:false`, `live_observer_validated:false` and
`production_bug_proven:false`. Generated evidence is not committed.

Offline reproduction (does not inspect any process):

```powershell
python -B tools/test_retail_np_connect_trace_1360.py -v
python -B tools/capture_retail_np_connect_1360.py `
  --exe "$env:LOCALAPPDATA/Temp/opencode/retail-plus-service-fix3/kyty_emulator.exe" `
  --core "${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/system_b/vsh/SceShellCore.elf"
```

## Parent-only prospective use — no current-run attach

Parent first decides whether the ordinary Home acceptance run makes another
diagnostic worthwhile. If authorized, use the parent's **existing** audited
pre-guest-barrier launch path for a new isolated run/store with the same fix3,
original retail Core and preserved media baseline. This tool never launches or
restarts it and does not select account actions. Freeze a new, small log prefix
while the barrier is ready and unreleased; obtain that launch's real PID,
creation FILETIME and unique barrier token. Do not borrow current/old run values.

In console1, with those newly verified parent-owned variables:

```powershell
python -B tools/capture_retail_np_connect_1360.py `
  --exe $emu --core $core --run-log $frozenPrefix `
  --pid $targetPid --created-filetime $createdFiletime --token $gateToken `
  --attach --output $newTrace --seconds 180
```

Only after the trace contains its validated `armed` record, console2:

```powershell
python -B tools/capture_retail_np_connect_1360.py `
  --exe $emu --pid $targetPid --created-filetime $createdFiletime `
  --token $gateToken --armed-trace $newTrace --release
```

Release revalidates the exact candidate path/hash, creation time, debugger,
unreleased barrier, source hashes and single initial armed profile. Changed,
ended, already-transitioned, old-profile or wrong-token traces reject. All
outputs are exclusively created; no prior evidence is overwritten. To request
an orderly stop use Ctrl+C; **never forcibly kill the capture process**.

After the final verified detach record:

```powershell
python -B tools/capture_retail_np_connect_1360.py --analyze $newTrace
```

Observation budgets: at most180 seconds,512 trap hits,20,000 debug events and
8MiB output; defaults120 seconds/256 hits/10,000 events/4MiB. Cleanup is not a
forced wall-clock kill and may outlast observation while restoring/draining.

## Interpretation limits

- Last positive stage distinguishes enqueue, delivery, onNewClient,
  AddSessionContext and Accept progress. Missing later events are **not** proof
  of deadlock, nonexecution or Sony rejection outside the covered interval.
- A real Core::Accept return with errno0 follows the pinned production
  `Finish(c->connect, r)` publication path. A negative native acceptance still
  completes a **rejected** connection; it is not connectivity success.
- The tool stops at the first original Accept return. If that returns an error,
  the adapter's subsequent fallback Accept path is not covered. Do not claim
  that no later rejection/cleanup/completion can occur.
- Consumer ticket consumption, ready Client attachment/discovery, Home and PSN
  authentication are **not covered**, even after native acceptance. A successful
  local connect only clears an earlier local prerequisite.
- First-chance exceptions after retained acquisition have unknown thread scope
  in the shared engine and make interval conclusions conservative. Second-chance
  exceptions, decoder/lifecycle gaps, budget exhaustion or incomplete detach
  likewise prohibit a reliable completion/acceptance conclusion.
- Debugger pauses/rearming change real wall-clock timing and can provoke the
  existing1000ms connect timeout. Do not extrapolate an instrumented timeout into
  a normal-run timing diagnosis or increase that deadline just to get a trace.
- Actual Win32 observer integration remains untested here under the explicit
  no-live-access constraint. Shared lifecycle code has prior independent live
  evidence; that is not a live validation of this new observer/ABI combination.

No account credentials are needed to use this local diagnostic. Genuine Sony
sign-in remains separately unverified, with platform-auth/envelope execution and
online-provider routing still documented in
`retail-psn-login-parallel-20261109.md`.
