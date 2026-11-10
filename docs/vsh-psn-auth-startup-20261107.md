# PSN request startup: implementation and active-build admission

## Implemented corrections

Two independent client-lifecycle defects are corrected. Neither supplies a PSN
session nor makes the currently unadmitted regular firmware service available.

1. `RuntimeNpManagerClient::AcquireQueryClient` now waits for its actual owner's
   independent transport/attachment workers to finish lazy initialization. The
   previous code started those workers, then returned null while they were
   starting. A first `CreatePrivateRequest` could therefore fail without waiting
   for an otherwise valid service connection/discovery. Competing same-owner
   publication is revalidated rather than automatically treated as unavailable.
   Wrong owner, missing credentials, stopped workers and closed/replaced epochs
   still return unavailable. The caller runs no server handler and holds no
   loader lock across the wait; transport jobs retain their existing deadlines.
2. `NpNativeManagerEndpoint::Connected` now reads published lifecycle state,
   instead of reporting false whenever `Advance` owns the coordinator mutex.
   Incidental contention formerly became `NOT_INITIALIZED` in real client
   methods. Submission now waits for mutex admission within its original host
   observation deadline. A timeout before publication submits nothing and leaves
   the connection available; a timeout after publication retains the existing
   retirement/drain behavior. No native operation is retried. A full queue still
   returns Busy, and retirement remains immediately visible without that mutex.

### Native evidence

Re-disassembled the actual regular `libSceNpManager.sprx`, SHA256
`9fb15e49f0967015ce9ae19c39aaf316638a5a5dda89eb5c397b9a86dbbb0121`.

- Private create `+fc90` calls accessor `+c930` at `+fccf` before request-manager
  initialization and request creation. It does not promise initialization to a
  later application retry.
- Accessor `+c930` reads ready byte `+61398` and client pointer `+613a0`.
  Cold path calls `+c9c0` (connect), `+15190` (request tracking), and `+13c10`
  (event discovery/attachment), then publishes ready at `+c987`. Failures return
  null. This is synchronous lazy initialization, with a stable published state.
- Ordinary ticket `+125e0` calls that accessor before validating its arguments;
  its null-client branch returns `80550002` at `+126c6`.

Fresh capture: `_TempData/np-request-startup-disassembly.txt`. Existing deeper
request/secure-service evidence is in `vsh-psn-deep-firmware-audit-20261107.md`.

## Actual psn36 failure producer

Read-only `_Runs/psn36/_kyty.txt` in the parent tree binds private request NID
`xZk+QcivrFE` to `NpManagerClient::CreatePrivateRequest` (lines4893/8953/12201).
Lines207220,807285,949041,2739340 and2819089 report that function returning
`80550002` with `owner_endpoint_unavailable`. The managed log lines13331-13350
reports `LoginMgr.pfAuth`, message `NpRequest`, with the same error. The producer
is local request/client initialization, before an authentication-ticket submit;
these records do not establish a Sony rejection or a bad account password.

Earlier, lines42698-42705 show `SceNpMgrIpcServer` creation failing `8002004e`.
This code is **Unsupported (BSD78)** from the exact native adapter's `Current()`
when no adapter is installed. `CreateServer` calls `Current()` before reading
its config or invoking `authorize_registration`. Thus this is not an observed
native kernel capability rejection. There is no source-bound NP installation
record in this log. `ConfigureBootProcessIdentity` installs that adapter only
after admitted Core registration. Native72 selects utility exports, not an
admission provider; local-account HLE supplies no such provider either.

The corrections above remove two downstream initialization failures once an
owner/service is genuinely admitted. They do not explain away this prior absent
adapter, and no claim of fixed live psn36 sign-in is made.

## Current image identities and missing genuine input

Fresh read-only file hashes (not a snapshot of the live process's memory):

| Input | Bytes | SHA256 |
|---|---:|---|
| Regular Core | 35491052 | `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a` |
| Regular original UI | 21863396 | `cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09` |
| psn23 staged native UI used by psn36 | 21863396 | `7e04d0389edcb0881183e20e2ee49c836d422617aecbef552d00a4e35b8d1451` |

Staged UI differs only in four bytes of the eight-byte dimensions immediate at
file `+166ad`: 3840x2160 -> 1920x1080. Reversing that immediate gives byte-exact
original regular UI. This transform evidence is not signed credential evidence.
The present admission policy pins only the different alternate pair and its
reviewed transform; setting alternate record environment variables cannot admit
these regular images.

Re-ran `tools/check_shell_self_metadata_1360.py` and its `alternate` mode entirely
within this worktree. Original SELF containers remain accessible read-only in
the regular firmware segment images, and their embedded headers/PHT/extended
info match their own extracted ELFs. The available dictionary slot41 candidate
still fails native-selected RSA-PSS verification for **both regular targets**.
Alternate Core/UI both verify; all six alternate tamper controls reject.

Precisely unavailable: verified regular-build CCP slot41 AES-128 material, or
source-bound successful service0x30 decrypted metadata bytes for Core
`[530,b60)` and UI `[530,b80)`, verified with the regular auth-manager public
key. Then complete segment/image verification and decoded136-byte record binding
are required before adding regular admission. Raw authority fields/ELF names do
not establish either current capability bitmap. No active signed record was
recovered, and no alternate credentials were transplanted.

Beyond admission, the prior audit separately identifies PFAuth's runtime secure
boot data at secure VA380000 (18c0 bytes), secure execution/state lifecycle and
KMS slot3a-dependent envelope operations. None is supplied by HostNetwork or a
local user profile. Their full provenance/execution remains unresolved.

## Verification

- Isolated clang-cl build, at most two compile jobs, under `_Build/psn-ipmi`.
  Six checks passed: `np_manager_endpoint_client_tests`,
  `np_manager_endpoint_tests`, `np_private_request_tests`,
  `np_query_exports_tests`, `np_retained_operations_tests`,
  `np_native_transport_tests`.
- New combined test links the production Client, endpoint, retained adapter and
  kernel transport. It holds a worker turn during attachment, private creation
  and ticket execution. The ticket receives an explicit fixture SignedOut error;
  both caller buffers remain untouched and method9 release occurs exactly once.
  Endpoint tests also cover pre-publication timeout, cancellation, no late
  request publication, and retained post-publication timeout/drain.
- Isolated `_Build/psn-auth-overlay/psn_auth_tests.exe` passed
  `--np-native-service-only` and `--np-native-service-race-only`, using the
  existing **alternate-only bounded native service fixture**, actual source-bound
  Core record, real dispatcher/owner pthreads, discovery and joined teardown.
  This fixture has its pre-existing explicit fixture-only cleanup transform;
  it is not full PsnCore initialization or regular live acceptance.
- Overlay rebuilt the actual runtime client, endpoint and main test TU from this
  branch, linking read-only donor objects. Input hashes were checked stable
  across linking. Recipe/logs are local `_TempData/build_psn_auth_overlay.py` and
  `_Build/psn-auth-overlay/`; no parent build output was written.
- Negative controls: original endpoint fails the combined client test during
  its first held worker turn. Original runtime client with corrected endpoint
  fails the native fixture's first-consumer assertion at line120. Both exit
  `0xc0000409`, while corrected variants pass. Local negative-control recipes
  and logs are retained under `_TempData/` and `_Build/psn-*-negative*`.
- `git diff --check` passed. Full production rebuild remains parent integration.

No live launch, process control, controller navigation, database/profile mutation
or parent source/build modification was performed.

## Parent integration validation

Integrated as `c8804a68` on ps5-vsh. The six focused NP/IPMI targets rebuilt
successfully under `_Build/psn-ui-ipmi` with parallel2. CTest passed all six:
query exports, private requests, combined client/endpoint, native transport,
retained operations, and endpoint lifecycle (total1.15s).

These standalone tests do not compile or validate the runtime lazy-attachment
change in the full emulator. Parent runtime/production validation remains
pending; the agent is adding cold-attachment failure/cancellation coverage.
Regular metadata admission and genuine live PSN sign-in remain blocked as
documented above. The Home investigation continues in its separate worktree.

## Follow-up: cold-wait failure and shutdown validation

The wait-loop epoch snapshot was insufficient to authorize the final client
grant: closure could occur after the snapshot but before lease publication.
The final decision now rechecks epoch/lifetime/owner/attachment under the loader
admission lock shared with epoch close. A per-run grant mutex serializes that
decision with `Run::Stop`/retirement. Lock order is loader -> run; Stop takes only
the run mutex and performs no join, guest execution or coordinator-lock operation.
An already-granted query may drain across close; a query whose final decision
occurs after close/retirement returns unavailable. This is linearized admission,
not retroactive revocation of earlier leases.

### Deterministic real-runtime cases

The existing alternate native-service fixture now drives **cold**
`AcquireQueryClient` calls on actual owner pthreads, without first calling Start.
Test-only scheduling observers block a known worker/consumer milestone. They
cannot supply credentials, endpoint replies, attachment results or event state.

- **Actual attachment failure:** withhold the queued method0 discovery job.
  The real endpoint observer reaches its existing deadline; `Client::Attach`
  returns `8002003c`, and the runtime records BSD60/TimedOut and stop. The first
  consumer returns unavailable while the transport worker remains gated. Only
  after the original worker resumes may Reap drain and join it.
- **Retirement while initialization is stalled:** hold the original transport
  worker before coordinator creation for longer than its nominal one-second
  deadline. The consumer remains waiting. Request retirement; the consumer
  returns unavailable **before** the worker is released. Reap remains WouldBlock
  until that worker resumes and its actual pthread exits/joins.
- **Retirement at the final grant:** block the consumer after it observed
  successful attachment/open epoch; retire its run, then release it. The final
  decision rejects the stale grant.
- **Epoch close during cold wait:** gate the transport before creation, wait
  until the first consumer is actually waiting, close the epoch and join that
  consumer while the worker is still blocked. It returns unavailable. Resume
  the original worker; even its genuine late successful attachment cannot grant
  another export from the closed epoch. Original workers drain and join.
- **Epoch close at the final grant:** pause after the old open-epoch snapshot,
  close, then resume. The final decision rejects. This is a separate fresh
  fixture process selected with `KYTY_TEST_NP_CLIENT_WAIT_SCENARIO=grant-close`;
  `epoch-wait` (or omission) selects the preceding cold-wait closure scenario.
- Every gated case also runs a genuinely foreign pthread. Both its ordinary
  call and its forged current-program=Core scope return unavailable; neither
  adopts the pending client's ownership or drives its transport.

Both `--np-native-service-only` scenarios passed, as did the separate
`--np-native-service-race-only` regression (Start won in this observation).
Logs are `_Build/psn-auth-overlay/cold-{epoch-wait,grant-close,admission-race}.log`.
The isolated overlay rebuilt the runtime client and main fixture with at most
two compile jobs; source-bound alternate service and fixture limitations above
still apply. These checks do not establish regular live PSN authentication.

A negative-control build removed only the final closed-epoch recheck, retaining
all other validation and scheduling gates. Its `grant-close` native fixture
failed at the explicit no-grant assertion (line209, exit `c0000409`); the earlier
timeout/retirement cases passed first. The production runtime TU also compiled
with `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS` removed, verifying that fixture hooks
are excluded. Local recipe/logs: `_TempData/check_psn_grant_negative.py` and
`_Build/psn-grant-negative/`. Full production executable rebuild remains parent
integration work.

### Exact progress guarantee and limitation

`AcquireQueryClient` has **no unconditional wall-clock completion bound**. The
previous reference to bounded transport jobs must not be read as such a bound:

1. A queued discovery/open observer checks its own deadline independently of a
   stalled transport worker; its actual failure drives `Fail`/Stop and wakes
   the consumer through ordinary state observation.
2. The coordinator connect deadline is enforced by `Advance`. A stall before
   coordinator creation, inside Advance/native lock acquisition, or a delivery
   worker that cannot execute may prevent automatic failure from progressing.
   The stalled-create fixture intentionally demonstrates this beyond one second.
3. The consumer's wait can observe retirement/closed epoch independently of
   that gated worker, provided the consumer is scheduled and its loader/identity
   locks remain responsive. `StartForCurrentOwner` itself performs pthread
   creation before entering the wait; an indefinitely blocked creation or
   loader/identity lock is not interruptible by this loop either.
4. Returning unavailable does **not** authorize freeing a stalled worker's
   resources or substituting another thread/owner to retire them. The original
   registry, thread and mappings remain retained until genuine drain/exit/join.

No new guest timeout, successful completion, watchdog teardown or impersonated
cleanup was introduced.

### Dependency to forward to the Home/startup owner

Source-bound native NP and database admission still lack the **regular build's
verified Core/UI136-byte records**, tied to the exact regular image hashes in
the table above (plus separately verified staging transform). Available regular
SELF containers have not disappeared; their slot41 metadata decryption is the
unresolved input. The retained alternate key candidate fails both regular
signatures. Required next input is verified regular slot41 AES-128 material or
successful source-bound service0x30 outputs for Core `[530,b60)` and UI
`[530,b80)`, then selected-key signature and full image/segment verification.
Alternate records, local user creation and profile/database contents cannot
establish those credentials. Home compatibility-service recovery and genuine
native credential admission therefore need separate evidence.

## Parent follow-up integration validation

Follow-up integrated as `eeb3c289`, retaining Home's session-module lifecycle
changes and the earlier parent endpoint results. The normal runtime test target
rebuilt successfully with parallel2. Three separate fixture processes passed:
`epoch-wait` and `grant-close` with `--np-native-service-only`, followed by
`--np-native-service-race-only`. Logs are retained at
`_Build/psn-integrated-{epoch-wait,grant-close,admission-race}/result.log`.

These runs supplied the explicit pinned alternate Core/NpCommon images and
retained alternate Core record required by the fixture; no absent-input skip
was counted as success. They validate the integrated native runtime cold-wait,
failure/closure/retirement and drain/join paths within the bounded fixture.
Regular service admission and genuine PSN sign-in remain unverified/blocked by
the inputs documented above. Production executable rebuild follows this gate.
