# Bounded serialized Home delivery trace

Implements the restored follow-up in `vsh-home-psn41-continuation-20261108.md`.
Parent owns live validation and user-coordinated navigation. This checkpoint used
offline files only; psn41 was not accessed as a process.

## Fixed four probes

| Event | Image/RVA | Contract |
|---|---|---|
| delivery_acquire | Vsh196e17 | Serialized GetItems2 result before interface Resolve. RDI Promise; RAX its vtable; R10 pinned resolve descriptor; Promise equals frame-30; RSI=R13=frame-38 result. Acquire ctxId and items count only. |
| delivery_resolve | PUI1cba30 | Concrete Promise.Resolve entry; freshly validate both callbacks/owners and result ctxId/count. |
| delivery_reject | PUI1cbaa0 | Concrete Promise.Reject entry; freshly validate both callbacks/owners; never read rejection payload. |
| delivery_dispatch | PUI127f15 | First Callback.Invoke interface dispatch attempt. RDI=R14=frame-18 is **ReactInstance**, not PUIReactContext. RSI equals signed DWORD frame-20; RAX is receiver vtable; R10 pinned invoke descriptor. |

Promise and Callback vtables must equal pinned GOT identities. Each callback's
weak ReactInstance and its weak PUIReactContext are resolved afresh; Home manifest,
AppData/app/context backlinks, generation and launched user must validate. Both
callbacks must have the same current instance and distinct nonnegative signed IDs.
Dispatch independently resolves the fresh ReactInstance owner. Acquisition keys
are `(generation,user,resolve ID,reject ID)`; no historical pointer is dereferenced.
Same-owner GC movement is supported, including replacement of the full graph.

Observed ID reuse poisons both pairs, including collisions across callback sides.
Repeated concrete entries or dispatch attempts invalidate the association, even
when the first dispatch lacked an observed concrete entry. Resolve and Reject
remain distinct sides; Resolve followed by Reject is retained. Observed context
termination at context+92 invalidates that owner's requests. Unknown/collected
owners and events without acquisition are not counted as correlated delivery.
Unobserved ID reuse or retirement cannot be excluded by this four-site coverage.

Limits: 512 acquisitions, 16 generation/user owners, 2048 used/poisoned ID tokens,
Reader's 64 KiB/event and the existing outer time/hit/event/output bounds. Budget
failures reject further association; no eviction reopens old IDs. State holds
numeric identity and bounded sets, never retained object pointers. The subclass
reuses QueueCorrelator's stateless GOT/type/Promise helpers, not queue state.

Lifecycle version2 hardware/context/thread/drain/restoration/shutdown helpers are
unchanged. Delivery uses diagnostic version5, a fixed phase and no readiness rearm.
No code patch, guest call, enqueue observation or queue-latency measurement.
`seconds_since_serialization` measures only the observed post-serialization span.
Missing boundaries are coverage gaps, not proof of callback loss. Unacquired
Reject cannot be attributed to scalar2. Dispatch is not JS consumption or Home fixed.

## Parent commands

Offline validation (saved psn41 log is provenance only):

```powershell
python -B tools/capture_home_gates_1360.py --trace-mode delivery --user 268500997 --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn41/_kyty.txt
python -B tools/test_home_delivery_trace_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib
```

For the next parent-owned run, use that run's actual stage/log and freshly verified
process identity; the exact attach argument template is:

```powershell
python -B tools/capture_home_gates_1360.py --trace-mode delivery --user 268500997 --original $original --stage $stage --home-bundle $homeBundle --run-log $newRunLog --attach --pid $targetPid --created-filetime $createdFiletime --exe $exe --exe-sha256 $exeSha256 --output $newOutput --seconds 90 --max-hits 4000
```

All variables are supplied by the parent for the new capture; output must not
already exist. Wait for armed coverage before user-controlled Home navigation.
Next observable: acquired serialization -> matching concrete side -> matching
first dispatch. If reached, downstream second weak check/JS queue/bridge executor
remain unresolved, as mapped in the continuation document.

## Verification receipt

22 delivery controls pass, including full-graph movement, collision/duplicate
poisoning, collected weak owner, foreign generation/user, wrong receiver, signed
ID, no acquisition, wrong result context/count, side matching, termination and
budgets. Pinned ABI audit checks register dataflow, Promise-side offsets and
termination getter; independent AOT GOT-string audit checks ItemsResult ctxId/items.
34 existing gate and 21 queue tests plus their pinned audits pass unchanged.
Offline CLI reports `offline_verified: true`, `process_access: false`.
AST comparison against f466a92e changes only capture/main definitions; all lifecycle
helpers match. At most two concurrent Python test jobs; no emulator build.

Live delivery evidence and any production runtime fix remain outstanding.

## Parent integration and psn42 launch

Integrated as `ef87bd2f`. Parent reran22 delivery,34 gate and21 queue controls;
all passed, including pinned ABI/property/manifest/provenance audits. Offline
delivery preflight passed against both saved psn41 and fresh psn42 logs.

psn41 was already absent at restart inspection; parent did not terminate it.
Parent launched psn42 PID32424 with the same regular13.60/native54 configuration,
preserved psn23 stage/storage, foreground user store and local-account flow.
No database preparation/candidate activation. Production build remains6b8cc8b,
SHA256 `048351953452fa10d6bc7d0f7081c5b20b71dfff1159f8e58056b84ce0e72739`.
Fresh identity/creation FILETIME and command line:
`_Runs/psn42/launch-identity.json`.

The delivery capture is armed:245 initial threads, all four expected current-run
sites, lifecycle2. Output: `Temp/opencode/home-delivery-psn42.jsonl`; bounds300s,
6000 hits,15000 debug events,8MiB. The longer window allows user-controlled login
and Home navigation. No controller input was supplied by parent. At this checkpoint
completion/detach survival and correlated delivery remain unobserved.

### Startup budget outcome and login-window rearm

First psn42 capture ended on the15000 debug-event budget at150.778s, before
Home navigation. It observed173 probe hits, all excluded as non-Home, zero
acquisitions and zero decode unknowns.14382 forwarded-exception records account
for most output. This is no evidence of missing Home delivery.

Lifecycle2 drained36 events, repaired one residual debug context before detach,
verified305 threads before/after detach and released holds; post-detach repairs0,
cleanup_verified true. Parent revalidated launch FILETIME and captured the same
live process at frame1503/fps8. `Temp/opencode/psn42-post-delivery.png` shows the
Press-the-PS-button screen, not Home.

Parent rearmed on this same ready-for-login process using a separate coverage
epoch/output `Temp/opencode/home-delivery-psn42-login.jsonl`,300s/6000hits/
50000events/16MiB. Armed event confirmed309 initial threads and the four expected
sites. No identities are carried across captures. User controls PS/login/Home;
the new capture's results and teardown remain pending at this checkpoint.

### User login: correlated delivery observed

User confirmed clicking. In the login-window capture, generation
`28eee2c539d1c646b8a23fed34d6dd8a`, user268500997:

| Context | Resolve/reject IDs | Serialized items | Concrete Resolve | First callback dispatch |
|---|---|---|---|---|
|4 (prior Games mapping)|180/181|count0 at103.987434s|104.009524s|104.018590s|
|5 (prior Media mapping)|186/187|count0 at104.099320s|104.114002s|104.120259s|

Each chain correlates fresh same-generation IDs and owners within this capture.
No request invalidation occurred. The mode does not itself observe the selected
space condition. Interface targets were `0xaf09c88fc` at serialization and
`0xaf09c8929` at first callback dispatch; they remain observed thunk targets,
not proof of downstream JS execution. The screenshot after the click at frame8898
still shows only Home's background (`Temp/opencode/psn42-after-user-click.png`).

Completed capture:352428 bytes, SHA256
`64cb448e4a0cf903eb69066824dd07f8c689bedf5c1f3a06f991a611219cd1f2`.
Deadline301.195s;1517 hits/5364 debug events;1446 non-Home exclusions. Two earlier
unknowns at88.482/88.486s failed generation-backlink validation and were not
associated with later acquired requests. No outstanding acquired requests remain.

Lifecycle2 verified/released356 threads, zero residual/post-detach repairs, one
initial trap context, no cleanup errors. Same-process creation FILETIME matched
after detach; parent observed responding=true, frame10858/fps43.

The agent now follows the next restored branch: downstream callback queue,
bridge/executor and actual Home JS continuation, including how empty results or
revision rejection affect first render. The concrete Promise/first-dispatch gap
is closed for these two invocations, not the later JS-consumption/render boundary.
No additional user click was requested; the emulator remains open.
