# Pre-OOBE Store/LoginMgr investigation — 2026-11-05

## Outcome

The ended `b2-live-1105` Store failure is a **local LoginMgr initialization failure before IPC**. The native client rejects the HLE IPMI size estimate, leaving its initialization count zero. Correcting that estimate alone would expose a second problem: the generic HLE client accepts connections and requests without executing the native service. Native LoginMgr acceptance, peer identity, property storage and notifications must be integrated together.

New concrete findings:

* Original IPMI `Rih7HnIM3JQ` **returns `0x1000` unconditionally**, not the HLE's `0x100000`.
* LoginMgr service is **`SceLoginMgrServer`**, distinct from `SceNpMgrIpc`. Existing exact-NP transport routing does not cover it.
* SetLoginFlag is method **`0x30023`**, input `{i32 userId, u32 mask, u32 value}` (12 bytes), one 4-byte flags output, client vtable `+0x58`. Its real ShellCore handler explicitly replies via session `+0x30`.
* Server-side execution checks the **actual peer's auth-info capability bit62** before modifying UserService property **6**. The native expression is **`(old & ~mask) | value`**; it does not additionally mask `value`.
* NP value **1 is `NOT_SIGNED_UP`**, established from managed metadata. The former unnamed “cached-account” path actually calls **GetOfflineAccountId**, **String.IsNullOrWhiteSpace**, and **FindTeenAccountInfo**. The latter checks **File.Exists** on a locally constructed path. A teen UI label is not a remote account verdict.
* Offline-ID HLE writes an eight-byte fake integer into a wrapper-declared **40-byte string-and-padding output**, while the inspected original 13.60 native export returns an unavailable/error result without filling that output. This is both ABI and provenance work, not enum renumbering.
* Apple's rwlock blocker remains a full owner-lifetime synchronization batch: current rwlocks lack owner metadata, typed call pins and terminal wait cancellation.

Only this document and `_TempData/loginmgr-agent-*` analysis files were written. No production/stage/profile edits, executable build/relaunch, UI input, commit/push, or further agents. Source references are observation anchors and may move during parent integration. This report concerns the ended pre-OOBE run; it makes no new OOBE runtime claim.

## 1. Evidence and immediate failure

Parent's recovered ShellUI chain is retained:

```text
Store Sign In -> psaccounts:mf entry=signin user=0x10000001
 -> NPXS40009 / millennium-falcon RNPS Main/Processing
 -> LoginMgrShellUI.setLoginFlag
 -> Legacy _SetLoginFlagByMaskValue(int,uint,uint,ref uint,ref uint)
 -> ShellUI eboot 0x3acf60 -> 0x3ef030
 -> old flags: QNk7qD4dlD4 [UserService]
 -> setter: Jha8yMoMzrg [LoginMgrServer] +0x1b00 -> +0x3a60
 -> initialized count singleton+0x10 == 0 -> 0x813e0001, BEFORE request
```

Original LoginMgrServer: `${USERPROFILE}/Downloads/13.60/system_b/priv/lib/libSceLoginMgrServer.sprx`, SHA256 `80706bd9da3f0239c38a5c0901f055cb25e50e6867186a13250dc150e125b19a`. Parent's native loaded base was `0xa40000000`; static addresses here are image-relative VAs.

Bounded log extraction: `_TempData/loginmgr-agent-live-excerpts.txt`.

* `_Runs/b2-live-1105/_kyty.txt:48073–48075`: Config constructor log-only, required-size stub `0x100000`, `sceLoginMgrServerInitialize():0x813e0003`.
* `:692610`, `:1024829`, `:1154603`: SetLoginFlag fails `-2126643199 / 0x813e0001`.
* Parent's `:37392–37395`: SceLoginMgrServer worker reaches success-only HLE dispatcher and exits. The general early dispatcher no-op is independently visible in the bounded excerpts; this worker-specific line attribution is inherited from parent.
* `_Logs/b2-live-1105.log:3432–3433` records RNPS's local error. No request identifiers, URLs containing salts, credentials or token payloads are included in these artifacts/report.

Detailed registration addresses and prior component validation remain in `vsh-integration-validation-20261105.md`.

## 2. Config, initialization, failure and teardown contract

Artifact: `loginmgr-agent-config.txt`. Original IPMI SHA256 `0ec3d75d640712b344ad20558985475bd68b7a7e8dc858bebc60ab9187ab819f`.

`O1lQ2+do5r4`, IPMI `+0xf40`, initializes a `0x158`-byte Client::Config extent:

| Offset | Native initialization |
|---|---|
| `+0` | empty name first byte |
| `+0x19` | zero sync flag |
| `+0x1c`, `+0x20` | event count 0, sync slots 1 (single qword store) |
| `+0x28`, `+0x30` | sync input/output limits `0xf00` |
| `+0x38` | async slots 1 |
| `+0x40`, `+0x48` | async input/output limits `0xf00` |
| `+0x50` | queue count 0 |
| `+0x58..+0x157` | zeroed queue/config tail |

Native does not initialize every padding/name byte; do not claim a whole-blob memset was observed. `Rih7HnIM3JQ`, `+0xfd0`, is exactly `mov eax,0x1000; ret`. Config size and required **client backing** size are different quantities.

Artifact: `loginmgr-agent-client.txt`.

1. Init export `+0x18d0 -> +0x1de0`: locks singleton, calls `+0x1e20`, increments count at `+0x1dfe` **only when result == 0**.
2. `+0x1e20` uses a recursive mutex path, checks client pointer `+8`, constructs config, writes name `SceLoginMgrServer` from constant `0x9de0` plus qword `0x72` at name+16, and changes event count `+0x1c` to 1.
3. `+0x1e96`: required size above `0x4000` -> `0x813e0003`.
4. Factory `+0x1eb6`: `(outClient=&singleton[+8], config, NULL, singleton+0x24 backing)`.
5. Connect `+0x1ed0`: client vtable `+0x10`, `(client,NULL,0,&serviceResult)`. Transport `0x80020003` maps to `0x813e0004`; other negative transport errors become `0x813e0005`; negative service result is preserved. Both result channels matter.
6. Failure cleanup `+0x1f42`: disconnect `+0x18`, destroy `+0`, clear pointer. Terminate `+0x1fe0` decrements count; final reference disconnects/destroys. Count-zero terminate returns `0x813e0001`.

Current effective late registrations are `IpmiLate::ClientConfigCtor` and `EstimateClientMemorySize`, `src/libs/libShellHits.cpp:1156–1196`; they supersede earlier registrations in `libC.cpp:8615–8618`. Repair exact/fallback resolution consistently, not just one earlier stub body. The late estimator's comment documents a prior DbNotify allocation workaround; regression coverage must retain nonzero allocation for other users while correcting the native contract.

`libC.cpp:3596–3620` generic connect writes service result zero without native acceptance. `:3677–3686` unhandled sync request writes zero service result and zero output length. `:4083–4091` dispatcher returns immediately. These form a false-success chain if only the size failure is removed.

## 3. Actual LoginMgr server, request and backing state

Artifacts: `loginmgr-agent-{server,handler,dispatch,methods,flag-setter,flag-commit,flag-backend,properties}.txt`. Original ShellCore hash is recorded in those artifacts and the native app lifecycle report.

### Creation and native worker lifetime

* ShellCore `+0x74a7f0`: Server::Config constructor, handler=`this`, flag `+0x18=1`, service name=`SceLoginMgrServer` from constant `0x1ef8144`; allocates **0x100 bytes** caller-owned server-object backing, separately estimates/allocates dispatcher work, calls server factory at `+0x74a873`.
* `+0x7482e0`: builds service, uses pthread attributes with **0x10000-byte stack**, creates entry **`+0x7483a0`**. This is ordinary native `PthreadCreate`, not the separate NpCommon dispatcher-pair ABI.
* Entry `+0x7483a0 -> +0x74a7c0`: server at service+`0x30`, work pointer+`0x20`, size+`0x28`, virtual dispatcher `+0x10`.
* Shutdown `+0x7483b0`: stop/wake through server vtable `+0x20`, actual pthread join, clear handle, then service teardown `+0x74a8d0`. Preserve that ordering and object backing through callback/drain completion.

Handler vtable at **`0x2028928`**:

| Slot | Target | Role established by call site |
|---|---:|---|
| `+0x10` | `0x74a550` | sync adapter -> `0x6e470` |
| `+0x18` | `0x74a570` | async callback; no full async contract newly claimed here |
| `+0x20` | `0x74a0d0` | connect; creates native session via `0x749ee0` |
| `+0x28` | `0x74a1f0` | ordinary disconnect path |
| `+0x30` | `0x74a480` | forced-disconnect path |

Connect allocates wrapper/session state, calls server vtable `+0x28` with storage sized from SessionConfig+`0x148`, records accepted session in its map, and uses session vtable `+0x40` when event count+`0x38` is nonzero. Thus **event-mask support is part of this LoginMgr acceptance path**, not optional simply because NP uses named-event methods. The current NP adapter's unsupported event/poll slots must be audited before reusing it here.

### Request wire and peer policy

Client `+0x11e0` packs one DataInfo of 12 bytes, one BufferInfo of capacity 4, initializes service-result cell zero, and invokes `+0x58` at `+0x1243` with method `0x30023`. Transport failure wins; otherwise returns service result. Native output-length bookkeeping is part of reply, not proof that a zero return filled flags.

Server sync adapter `+0x74a550` removes handler `this` and forwards `(session,method,inputs,numInputs,outputs,numOutputs)` to `+0x6e470`. Dispatch subtracts `0x30000`; table base **`0x1ed5358`**, index **0x23 -> `0x6e4f0`**, unpacks user/mask/value and output pointer, calls **`0x74b660`**. The success/error service result is explicitly replied through session vtable **`+0x30`** at `0x6e868–0x6e881`; output written=4 is assigned at shared `0x6e71a`.

`+0x74b660` requires nonnull session, gets peer via session vtable `+0x18`, calls `+0xb3ef70`; only result1 proceeds. The helper calls **`igMefp4SAv0` / get_authinfo** and extracts **bit62 of qword record+8**; syscall failure produces negative result. Rejection here is `0x813e0005`. Current `KGetAuthInfo` (`libShellCoreFloor.cpp:322`) queries guest process identity; `runtimeProcessIdentity.cpp` builds capability records from retained guest identity, including boot ShellUI bit62. Correct adapter **peer PID** is necessary; host PID/thread ID substitution or universally granting service names would bypass the actual condition.

### Property 6 and source-of-truth gate

Allowed request -> singleton getter `0x7482d0` -> **`0x748b30`**:

1. Requires output, clears it.
2. Calls `0x9e5900(user,&old)`; native UserService property read **selector6**.
3. Computes **`(old & ~mask) | value`** (`andn` at `0x748b7d`, `or` at `0x748b85`). No implicit `value & mask`.
4. Calls `0x9e5970(user,new)`; property write **selector6**.
5. Publishes new flags only when read and write succeed.

The property helpers require the native manager from `0x9df910`; absence returns `0x80960002`. `0x9e1290` / `0x9e1a30` validate property, search native user records by ID at manager+`0x180+n*0x4000`, and lock manager state. Setter calls `0x9e1cc0`, then `0xa05f50` on successful mutation (full notification/persistence semantics beyond that edge remain unclosed).

Original UserService **`QNk7qD4dlD4 +0x4ee0`** is `(i32 user,u32* out)` and invokes registered backend slot+`0x10` with selector6. Current `UserServiceGetAccessibilityUnk_QNk7qD4dlD4` at `libUserService.cpp:994` always writes zero. Its name is wrong and its storage is disconnected from native setter state. HLE `StoredUser.login_flag` and persisted JSON do not establish that native manager records exist or agree. Close the native callback/shared-state seam and notification semantics rather than add another independent flag cache.

## 4. Truthful NP/account state provenance

Artifacts: `loginmgr-agent-{metadata,state,state-native,teen-file,offline-id}.txt`; metadata decoding script records AOT blob offsets and exact MethodDef rows. Unlike metadata-only method bodies, AOT code and compressed references establish the actual calls.

### Enum and exact conversion

`Sce.Vsh.Np.Manager.SigninState`: **0 invalid, 1 not signed up, 2 signed out, 3 signed in**. LoginMgr's separate enum is **0 invalid, 1 not signed up, 2 offline, 3 unapproved teen, 4 temporary signed out, 5 signed out, 6 signed in**.

LoginMgr `Utility.ConvertNpSigninState2LoginMgrSigninState`, AOT **`0x461b0`**, calls `0x15f854`, resolved through blob+`0x1ecde` to **NpManager.GetSigninState(user,ref state)**. Value1 takes `0x462ad`:

| AOT stub | Exact target | Consequence |
|---|---|---|
| `0x164f9c`, blob+`0x1fcc4` | `sceNpManagerIntGetOfflineAccountId` | Not `GetCachedAccountInfo`; correction to earlier provisional attribution. |
| `0x164fd4`, blob+`0x1fccc` | `System.String.IsNullOrWhiteSpace` | Success with nonblank offline ID selects LoginMgr2 (offline). |
| `0x164fb8`, blob+`0x1fcc9` | `Utility.FindTeenAccountInfo(user)` | Missing/error/blank offline-ID path: true -> LoginMgr3; false -> LoginMgr1. |

Special offline-ID result `0x80559b0c` also selects LoginMgr2. Other errors flow to the teen-file check. NP value2 calls `sceNpManagerIntIsTemporarySignout` (`0x164ff0`): true -> LoginMgr4, false -> LoginMgr5; error retains an error return while setting signed-out label. NP value3 selects LoginMgr6. Do not obtain desired UI labels by changing the NP enum returned from HLE.

`FindTeenAccountInfo +0xd48a0` takes a managed monitor, calls **GetTeenAccountInfoDirectory(user)**, **System.IO.Path.Combine(directory,static filename)**, then **System.IO.File.Exists**. Stubs `0x16edc4/0x16ede0/0x16ee18` were resolved to those exact methods. It does not validate an authenticated remote account; caught exceptions yield false. This investigation did not inspect or alter the active profile or determine who created any existing teen file. A displayed label alone does not prove that specific branch's live input contents.

### Provider and ABI evidence

* Current `libOfflineLogin.cpp:55–56,155–199` supplies hardcoded NP state1 and fake account integer `0xfeedface`; `ResolveOutPtr` heuristically accepts either of two argument positions. Those are HLE outputs, not console/account observations.
* Original native `6miba-pcQt8 +0xd2e0` and `UAA2-ZTmgJc +0xd340` prove `(i32 user, u32* output)`. Optional hook slots `+0x98/+0xa0`; absent hooks use initialized manager client. Missing manager -> `0x80550002`; null output -> `0x80550003`. The client path uses IPMI methods **0xe/0xf**, one 4-byte user input, one 4-byte state output, vtable+`0x58`.
* `SceNpOfflineAccountId`: `id` is `[MarshalAs(ByValTStr, SizeConst=37)]`, padding `[ByValArray,SizeConst=3]`. Native marshalled extent **40 bytes**, not u64. HLE NID **`70Swvw7h6ck`** writes only fake u64 into this buffer and reports success. Correcting this must respect output/error contracts, not mint a string identity.
* Original 13.60 `70Swvw7h6ck +0xeef0` **does not write output at all**: absent manager -> `0x80550002`; initialized manager plus user -1 -> `0x80550003`; otherwise **`0x80559b1a`**. The inspected provider itself therefore does not establish an available cached offline ID. Keep wrapper ABI and original provider behavior distinct.
* `MgrIntGetCachedAccountInfo` is separately success-noop, but it is **not** the call at AOT `0x462bd`. Do not attribute the teen label to that stub solely from similar naming.

### OOBE evidence limits

`libUserService.cpp:203–225` is a host JSON writer serializing local user name, passcode, login flag, account/signin fields and login order. `StoreLoginUser`/`StoreLogoutUser` call it. A newly created users JSON proves those local HLE writers ran only when writer/call evidence supports that attribution; it does not prove native protected-account storage, valid device identity, enrollment, PSN authentication or remotely accepted credentials. Likewise a guest-written teen-info file can affect a local label without establishing account approval state.

Any parent OOBE report should distinguish prepared catalog/staging files, HLE user-store persistence, original guest file writes, and authenticated remote results using observed writer/callchain evidence. This investigation did not collect new profile contents, secrets, tokens or request IDs. TLS transport tests and UI navigation remain separate from authenticated PSN exchange.

## 5. Coherent implementation batches and acceptance gates

| Batch | Required unit of work | Meaningful acceptance gate |
|---|---|---|
| L0 — truthful IPMI config/selection | Exact ctor fields, native `0x1000` estimate, deterministic backing contract, consistent early/late/fallback registrations; preserve other clients' nonzero allocation behavior | Canary-tested `0x158` config extent, exact defaults, LoginMgr's 0x4000 cap, constructor/factory failure and rollback. No fake connect or service success. |
| L1 — LoginMgr transport lifecycle | Explicit audited **SceLoginMgrServer** registration, real dispatcher worker/current-thread guest callbacks, same-peer session creation, event-mask negotiation, actual stop/wake/join, disconnect/forced cleanup and backing retention | Native server accepted session, peer-policy success/rejection, one explicit 0x30023 reply, blocked-call shutdown and failure unwind. Host callbacks alone are component evidence. |
| L2 — authoritative flags/user seam | Correct getter identity/ABI, native manager record existence, property6 read/write, mask/value and error outputs, notifications, agreed persistence writer | Set/get agreement through actual registered paths; invalid user and rejected peer preserve state; masked updates, repeated init/term, disconnect/reconnect; no independent fake cache. |
| N0 — account ABI/provenance | Replace heuristic output positions with recovered signatures, 40-byte offline-ID/error semantics, real NP0xe/0xf provider selection, local-user/account distinction, explicit unavailable states | Exact sizes/canaries; missing provider preserves outputs; managed conversion follows actual error/state/file inputs; no fake nonzero account IDs or label-oriented enum changes. |
| N1 — native NP integration | Install retained Hooks, native56 dispatcher create/join, actual SceNpMgrIpc acceptance, manager endpoint/native caller selection, producer/worker drain | Existing seven IPMI component passes plus genuine native callback/dispatcher execution and accepted manager client. Do not auto-start the unavailable-account HLE broker. |
| N2 — remote prerequisites/E2E | Correct environment/registry ABI, legitimate device/account/protected-storage inputs, network/TLS/error paths, actual auth flow | Observe genuine remote protocol result and source-backed account state. A local form, completed OOBE, or TLS socket is insufficient. |

L1 can reuse tested Core mechanics from `src/kernel/ipmi*`, but **cannot** silently change the current `NpNativeTransport` exact-name isolation into a generic Sce service bypass. `CreateClient`/`CreateServer` compare `SceNpMgrIpc`; `libC.cpp` routes only that name to the adapter. LoginMgr has its own ordinary pthread worker and event-mask requirements. Seven passing `tests/ipmi` targets are valuable but not native LoginMgr acceptance evidence.

The retained NP handoff remains authoritative: `vsh-native-np-transport-handoff-20261105.md` and `vsh-native-np-lifecycle-re-20261105.md`. Runtime hooks were still uninstalled at this investigation's source snapshot; ended B2 run had native NP flags disabled. Native56 is already a separate integrated loader policy; do not duplicate native54 changes. Pending environment ABIs remain `kBON3bAtfGs(char* out,size_t cap)` and `DKWSr89zMsI(u64 key,u32 selector,char* out,u64 cap)` per prior native audit. Account/device inputs are unresolved.

## 6. Preserve Apple's independent lifecycle result

B2 CRT/once/condition: parent reports **11/11**, then final strengthened **2/2** passing. The native guard broadcast blocker was passed. Ended live run now refuses **`6ULAa0fq4jA#A#B / LibKernel::PthreadRwlockInit`**, owner152/importer153, app104/lifetime1, native libc return **`+0xcac44`**, pre-entry result `0x80020055`, ShellUI retained. `loginmgr-agent-live-excerpts.txt` captures the exact refusal at lines604295–604296. App entry/frame/close/relaunch remain unverified.

Fresh bounded bundled-libc disassembly (`loginmgr-agent-rwlock-family.txt`) closes the immediate wrapper family:

| Operation | Native call VA | NID |
|---|---:|---|
| init | `0xcac3f`, second wrapper `0xcacdc` | `6ULAa0fq4jA` |
| destroy | `0xcad24` | `BB+kb08Tl9A` |
| write/read | `0xcad54 / 0xcad84` | `mqdNorrB+gI / Ox9i0c7L5w0` |
| try write/read | `0xcadb4 / 0xcade4` | `bIHoZCTomsI / XD3mDeybCnk` |
| timed write/read | `0xcae26 / 0xcae76` | `adh--6nIqTk / iPtZRWICjrM` |
| unlock | `0xcaeb4 / 0xcaee4` | `+L98PIbGttk` |

These decoded wrappers convert kernel-style errors to positive errno. Other bundled-libc call candidates use `sIlRvQqsN2Y/iGjsr1WAtI0/EgmLo6EWgso`; source same-body registrations do not establish identical native error-domain semantics. Preserve exact expression/library classification and test selected raw imports rather than admitting all aliases.

Concrete current `pthread.cpp` gaps:

* `PthreadRwlockPrivate` (`:523`) has readers/writer counts but **no GuestCallOwner**; attrs likewise lack ownership.
* Init (`:3509`) only checks outer pointer, allocates/publishes a raw host object and shadow record, does not admit current owner/span or serialize publication against stop; attr is selected but unused.
* Shared-aware `CreateObject` (`:1995`) accepts only Mutex/Cond. Rwlock callers use the legacy no-execution route, so simply adding a returning import would bypass this lifetime mechanism.
* Blocking loop (`:3577`) tracks waiting writers and timeouts but **never checks terminal owner stop**. It calls signal dispatch between polls; runtime signal suppression is not cancellation. A stopped worker can remain inside HLE indefinitely.
* Destroy (`:3471`) checks writer/reader counts only, not waiting acquisitions or retained API calls; it releases the object mutex before deleting raw storage. A waiting caller has no typed pin preventing deletion in the unlocked handoff interval.
* `Trywrlock` uniquely lacks lazy-object handling and can hit `EXIT_NOT_IMPLEMENTED` on null static initializer. Unlock can enter legacy lazy repair. Foreign object access, canceled waiter accounting and post-stop publication require an explicit policy.

**Next Apple batch:** complete init/acquire/try/timed/release/destroy plus needed attributes as an owner/token-scoped synchronization unit; typed retained object pins, boundary/span admission, terminal cancellation with waiter-count rollback, no post-stop acquisition/publication, foreign-owner rejection before mutation, busy destruction through full call lifetime, and ordinary/fatal teardown policy. Reuse the existing condition/mutex admission machinery without changing native private libc provider selection. Test contended stop, two-owner/session isolation, static initializer, destroy racing a queued acquisition and full raw-wrapper error domains before exact returning-table additions. This is semantic work, not an import-name fix.

App lifecycle resource batches B3–B6 remain open per `vsh-native-app-lifecycle-re-20261105.md`; CPU join is not complete video/audio/GPU/resource retirement. LNC scalar async-ticket ABI remains pending per `vsh-ipmi-async-tickets-1360.md`. Keep both workstreams independent of local Store/OOBE progress.

## Reproduction / uncertainty boundary

`python -B _TempData/loginmgr-agent-inspect.py <alias> <unique-artifact-label> <VA:size>...` performs bounded original-ELF disassembly and writes only its own artifact. `loginmgr-agent-metadata.py` resolves the selected AOT method references and enum/marshal metadata. `loginmgr-agent-log-excerpts.py` streams the large ended log and extracts bounded noncredential diagnostic lines. No executable tests were run for this read-only investigation.

The full ShellCore property persistence/notification continuation, all LoginMgr event methods, forced disconnect races, native user-record preparation, and complete remote auth dependency graph remain unresolved. Proven local edges above are sufficient to define coherent next batches; they do not certify successful sign-in or every downstream service. OOBE-generated files cannot close those gaps without writer, ABI, state and remote-result provenance.
