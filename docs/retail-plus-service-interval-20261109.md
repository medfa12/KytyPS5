# Plus service interval: proven lost attachment error / isolated fix — 2026-11-09

## Outcome

An evidence-based production fix is prepared in
`Temp/opencode/retail-plus-service-fix3/kyty_emulator.exe`, SHA256
`16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd`.

**Proven host bug:** attachment has already timed out, but its terminal error
is hidden behind native-retirement `Busy`/`WouldBlock` progress. The consumer
continues waiting for `attached` indefinitely. The fix propagates the already
latched failure; it does not invent a deadline, cancel an unretired native
ticket, fake an initialized client or choose a Plus membership result.

No usable Home/runtime acceptance of this candidate is claimed. The deeper
reason native NP connect had not completed remains separate/unproven. PSN
authentication, LoginMgr admission, auth/profile data and membership semantics
are unchanged. Parent-owned `docs/retail-localhome6-plus-capture-20261109.md`
and the independent PSN investigation were not edited.

## Frozen evidence and actual IDA

Verified in `Temp/opencode/retail-localhome6-plus1/checkpoint1`:

| File | Bytes | SHA256 |
| --- | ---: | --- |
| plus-native.jsonl | 87700 | `21ccecf3bce421495022abfc2f672bc1b66da4d5f5c47a23682cbf957d629e75` |
| host.log | 189598 | `b6044758f8b4fc3bb14e36ddf2136d8c5fb2de5ba4462642c99bdc7dda3dd4a3` |
| guest.log | 73924177 | `6102dc9059ebfe47b2c623cdb395caabe56b5d17dc020db56f0cca1dc010be46` |

Request139.217669s, generation `2ee07ede9670f846a4328daa5c175f0b`,
user0x10010029, callback IDs26/27; worker139.373956s on OS TID11128;
service-call139.548546s at Plus RVA0x16bf4b. No immediate return, cancellation,
settlement or delivery before181.153148s. This trace alone does **not** prove
a pending native promise, IPC deadlock or exception unwind. There were353
forwarded first-chance AVs,109 after acquisition, without TID/address fields.
The zero-decode-error capture **did cleanly detach** across536 threads, no repairs.

Actual IDA9.3/Hex-Rays against the unchanged theme-session4 private database
(`05d0ef41c5b3e68aaec8d5e45af7aeb6374c69a5af05da441f65e9da13f4be13`)
audited:

- `GetServiceState38`0x757940 →`AcquireQueryClient`0xa52fe0.
- Acquire's attachment-wait sleep return0xa53322; Run selected pointer at
  host RVA0x22df630, validated by the actual RIP-relative load at0xa53089.
- TransportEntry0xa51d10, progress-loop sleep return0xa52395.
- DeliveryEntry0xa527b0, disconnected-endpoint wait return0xa52a50.
- Coordinator::Advance0x92cfc0 and Done0x92cf10. Advance revokes on its existing
  connect deadline, then independently drains retained native resources.
- Exact-options compiler layout fixture (not a target call) plus IDA confirm
  Run control offsets0x20..0x38, coordinator/endpoint0x40/0x50; State's first
  retirement cause0x2e0 and registered/connecting/retiring/done/connected
  flags0x308..0x30c. Non-standard-layout offsetof warnings were retained;
  relevant offsets were independently checked in the pinned executable.

Private reports/specs/layout object and filtered live evidence are in
`Temp/opencode/retail-plus-service-interval1`. No firmware code, comparisons,
inventories or raw live stacks were committed.

## Authorized live diagnosis, not another startup

The user granted exclusive bounded read-only diagnostics of assistant-owned
PID17500, creation FILETIME134387106776455686. Before every sample the tool
verified exact executable path/hash/creation and **no debugger**. There was no
debugger attachment, breakpoint, controller input, process launch/closure,
WriteProcessMemory, store edit or guest invocation.

`tools/sample_retail_plus_interval_1360.py` used frozen pthread records, not TID
alone: worker11128's recorded5MiB stack and transport25732/attach19972's recorded
8MiB stacks. GetProcessIdOfThread rechecked each opened thread. Reads were clipped
to those proven stack bounds and committed regions. Exactly one suspend increment
was removed per sample; no raw registers/stacks were persisted. Output includes
only allowlisted executable-address candidates (explicitly **not** claimed as
unwound frames), control flags and guest thread IDs. No credential, request,
error-body, membership, arbitrary string or callback payload graph was followed.

Two recorded suites (18 samples) showed:

- Plus stack code candidates at host0xa53322, host0x75796c and Plus0x16bf50,
  repeatedly, with top PC in Windows sleep; source/IDA identify the native
  query-client attachment wait, not an unresolved AOT call target.
- Transport stack at0xa52395 and attach stack at0xa52a50, repeatedly.
- All recorded suspend counts0→resume1; maximum recorded stop4.083ms.
- Three stable native control snapshots, selected Run matching **both** runtime
  workers' frozen launch argument, plus coordinator/endpoint/State backlinks:

| Runtime Run | Value | Coordinator State | Value |
| --- | --- | --- | --- |
| token | 1 | registered / connecting | true / true |
| starting / stop / owner_lost | false / false / false | retiring | **true** |
| attached | **false** | connected / done | false / false |
| worker_error | **0** | first retirement cause | **60 (timeout)** |
| initial attach_result | 0x80550002 | existing timeout | 1000ms |
| transport/delivery returning | false / false | native connection retained | **true** |
| guest worker IDs | 510 / 511 | queued jobs / active job | 0 / false |

The log's unique pthread IDs514/515 are not `guest.thread_id`; an initial overly
strict ID comparison rejected the controls after all its stack suspensions had
already been removed. It was corrected to use the proven Run/backlinks and
positive distinct guest IDs; no IDs or state were modified.

A later optional unwind attempt failed at OpenProcess with error87: PID17500
was no longer openable. No thread was opened/suspended by that attempt. Live
diagnosis stopped; no restart, closure, input or alternate-PID search occurred.
The retained host file did not establish the reason for that disappearance.
The earlier recorded snapshots, not the failed unwind attempt, support the fix.

## Causal mechanism and minimal change

`src/libs/npNativeManagerEndpoint.cpp::Coordinator::Advance` already latches
`TimedOut` in `retirement_request`. It must continue calling Retained::Retire on
the original owner while an in-flight native connect ticket cannot be destroyed.
Its return can consequently be `Busy`/`WouldBlock` even though attachment is
permanently unavailable. `Done=false` correctly retains that connection.

Previously `TransportEntry` only failed the Run on a non-progress Advance return.
Delivery kept waiting for Connected, and AcquireQueryClient kept waiting for
attached. The runtime error/stop fields stayed zero/false despite State's60.
The captured control snapshots directly exhibit this lost-terminal-error case.

Changes:

1. `Coordinator::RetirementError()` exposes only the already-latched first cause
   with an atomic, lock-free load; no state/layout/timeout/cleanup change.
2. `Run::ObserveCoordinatorProgress()` propagates that cause to the existing
   Fail/Stop path, independently of cleanup progress. TransportEntry calls this
   after each real Advance, and still advances on the original worker until Done.
3. A planned parent stop is not promoted to an invented worker error; first
   cause remains sticky. The existing AcquireQueryClient stop checks release
   waiters unavailable, without granting a client from a revoked endpoint.

The native managed Plus worker retains its own service-error handling and promise
decision. No code here reads or creates a membership value. No request object,
native ticket, runtime Run, endpoint, worker or source-bound owner is discarded.

## Regression and candidate provenance

`tests/RetailNpCoordinatorFailureTests.cpp` compiles the **actual production**
Coordinator and runtime Run policy. Its isolated scheduling-only Retained test
backend begins but never accepts a connection; it never returns membership or
service success. The real Coordinator clock expires and retains cleanup in both
Busy16 and WouldBlock35 cases. Assertions cover:

- Healthy pending is not failure.
- Real latched timeout propagates through production Run policy while Done=false.
- Run/endpoint/coordinator remain retained; no service calls/results are invented.
- Retirement continues, later permitted cleanup finishes, first cause persists.
- A planned stop remains error-free.

The literal former production progress-only decision is replayed as a negative
control: exit3, `terminal timeout was not propagated`. This is an **old-policy
replay**, not a claim of a second old emulator run. Fixed fixture exits0.
Test-only seven Retained symbol overrides are validated explicitly; they are
absent from the production candidate and its link has no duplicate symbols.

The candidate is an exact theme-session4 link-input overlay: only
`npNativeManagerEndpoint.cpp.obj` and `runtimeNpManagerClient.cpp.obj` replaced,
plus isolated output/PDB/map paths. All350 retained input hashes were rechecked.
UserService/local-session, native LoginMgr, normal media/audio, M.2, GNF/stdio,
and the separate animation fix remain the same input objects. Compile jobs≤2;
links serial. No emulator was run to build/test the candidate.

- 14 ancestor regressions **relinked with both new objects** and passed:
  local-session selection/callbacks (5), GNF/stdio (1), M.2 (1), autologin (7).
- New native lifecycle regression passes; old-policy negative fails as expected.
- 11 new offline sampler/privacy/field/balancing controls pass;227 existing
  Home/retail trace controls pass. The generic sampler's default behavior is
  unchanged; optional stack_bounds refuses unproven stacks before reading them.

Candidate receipt `retail-plus-service-fix3/receipt.json` SHA256:
`bcbf55c8b014b9f374cb4c34a561caaefd261e514a54ad86d411792e75c49042`.
Launcher-compatible `retail-plus-service-fix3/launch-build-receipt.json` SHA256:
`44d887774e68c3e4f320b38e59ef1b97fa9e256ee8cb135b1d8cfa927078cec6`.
The expected negative remains in the full receipt, not falsely listed as passing
in the launch receipt. Earlier fix1 link failure/fix2 policy-only artifacts are
retained separately; fix3 is the fully regressed candidate.

Diagnostic/build checkpoint receipt:
`Temp/opencode/retail-plus-service-interval1/checkpoint-receipt.json`, SHA256
`b9fd576a1a5c9cf6a31beecc4d427e4c051e76292c3700d8318039f4af598198`.
It pins the filtered successful snapshots, actual IDA reports, layout fixture,
current source files and verified candidate/input/test receipts.

Reproduce in a **new unused** output directory (does not launch an emulator):

```powershell
python -B tools/build_retail_plus_service_fix.py `
  --base "$env:LOCALAPPDATA/Temp/opencode/retail-theme-session4" `
  --output "$env:LOCALAPPDATA/Temp/opencode/retail-plus-service-fix4"
```

## Parent-only runtime acceptance

Keep the existing ordinary audited launcher/settings/mounts. Change only:

```text
--executable .../retail-plus-service-fix3/kyty_emulator.exe
--sha256 16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd
--candidate-build-receipt .../retail-plus-service-fix3/launch-build-receipt.json
```

Use a new isolated run name/store, preserving existing evidence. This task does
not authorize or perform that launch. Do not use the barrier-only config launcher
without a newly audited observer: old observers intentionally pin theme-session4
and must **reject** this new executable; do not weaken their guards or copy RVAs.

Acceptance is native error handling/config publication plus a target screenshot
showing genuinely usable Home. A render loop alone is not success. If Home stays
blank, distinguish subsequent Plus settlement/JS publication from later catalog
gates rather than repeating the original global-opcode timeout.

Independent PSN prerequisite: the captured native manager connection was never
ready before its deadline and still awaited retirement. Real accepted NP transport
and discovery/authentication must be established independently; this fix only
exposes failure correctly and provides no PSN session or membership entitlement.
