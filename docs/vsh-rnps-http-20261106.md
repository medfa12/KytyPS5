# Regular 13.60 RNPS HTTP integration

Branch: `ps5-vsh`, starting from `7137d1f8`. Firmware:
`${USERPROFILE}/Downloads/13.60`.

## Production change

`libJscdNet.cpp` previously returned zero from all 26 curl exports, including
pointer-returning constructors. NativeExtensions therefore configured null
share/multi handles. Those exact NIDs now route to `rnpsCurl.cpp`.

The adapter provides opaque easy/multi/share handles, LP64 getinfo/timeout and
24-byte CURLMsg layout, owned header lists, real HostHttp transfers, binary POST
and read-callback uploads, header/body/progress callbacks, HTTP status and typed
errors, bounded response buffering, multi completion queues, removal/cleanup,
poll and cross-thread wakeup. Guest callbacks execute only from the calling
guest thread. Detached HostHttp workers own request bytes, never guest pointers;
removed/timed-out transfers cannot later dispatch guest callbacks.

This is a buffered HTTP adapter, not a complete libcurl implementation. Workers
already in WinHTTP may finish after removal; the removed guest completion is
discarded. Cookie-engine sharing, guest certificate/key/CA blobs, socket-action
callbacks, push, and arbitrary extra poll descriptors are not implemented.
Unsupported options return errors. No sign-in status, account identity, token,
device certificate or PSN response is synthesized.

HostHttp additionally supports negotiated HTTP/2, gzip/deflate decoding, a
configured redirect limit and effective response URL. Response header conversion
now excludes its terminating wide NUL from guest-visible bytes.

## Native call-site evidence

Regular `libNativeExtensions.sprx` SHA256:
`2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb`.
The retained read-only extraction is `_TempData/rnps_curl_contract.py`, output
`_TempData/rnps-curl-native-contract.txt`.

* Native curl_easy_setopt PLT RVA `0x5f0c0`, request setup `0xced0` region.
* `0xd20e/0xd21e`: read callback / userdata (`20012/10009`).
* `0xd25f`: 64-bit upload length (`30115`).
* `0xd276/0xd28f`: header callback / userdata (`20079/10029`).
* `0xd2a6/0xd2ba`: body callback / userdata (`20011/10001`).
* `0xd2d5/0xd2e5`: progress callback / userdata (`20219/10057`).
* `0xd2f8`: enables progress callbacks; `0xd30e` supplies error buffer.
* `0xcc60` reads response code and optional timing fields through getinfo.
  Native caller explicitly handles unsupported timing fields with -1.
* Native setup also has certificate-related options; these are not evidence of
  valid account/device material and are not replaced by successful stubs.

## Checks

Production `_Build/vsh-integrated/kyty_emulator.exe` built successfully.
`host_http_tests` and `rnps_curl_tests` passed against a real loopback server:
binary POST, SysV callbacks on caller thread, header delivery, LP64 outputs,
redirect/effective URL, gzip, HTTP 404 vs transport failure, read-callback upload,
short write and progress aborts, exactly-once multi completion, cancellation,
stale handles, total timeout, cross-thread wakeup, and disabled-network errors.

Final HTTP check: three suites (`host_http_tests`, `network_http_tests`,
`rnps_curl_tests`) passed in2.57seconds. An explicit external probe through the
same adapter, `rnps_curl_tests.exe --probe https://status.playstation.com/`,
returned HTTP200,1158bytes, curl0/host0 with platform TLS verification. This
establishes a real HTTPS transport response, not an authenticated PSN session.

## Additional private network-state seam

`JXlI9EZVjf4` in `libJscdSmall.cpp` returned zero without publishing its output;
psn9 calls this repeatedly. Native `libSceNetCtl.sprx` +3430 accepts one pointer,
rejects null with0x80412107, and +4d0 requests a four-byte output on IPC0x20009.
Regular ShellUI +8fb0 calls this export then returns that stack word via the
registered icall `lx::lxNetCtl::GetState`. `Sce.Vsh.Lx.dll` return enum `NetState`
has Disconnected0,Connecting1,IPObtaining2,IPObtained3. It now forwards to the
existing host-backed public NetCtlGetState, including the disabled-network path
and null-output error. It does not report PSN login or remote service availability.
Evidence: `_TempData/netctl_state2_contract.py` / `netctl-state2-contract.txt`;
managed enum read with dnfile from `_Runs/psn9/stage/common/Sce.Vsh.Lx.dll`.

## Live regular VSH: psn9

Profile copied from `_Runs/rvsh2/sd` to `_Runs/psn9/sd` (155 files, 19,449,524
bytes). Stage regenerated from regular firmware with the existing NPXS40141
plaintext overlay. Host network enabled; matching regular assets and original
compatibility app routing retained.

PID12656 reached visible Home. Screenshots:
`_TempData/psn9-navigation.png`, `_TempData/psn9-store-attempt.png`.
The latter did **not** reach Store: targeted keyboard messages did not change
selection, and the foreground-input helper failed to acquire OS focus. No
account interaction success is claimed.

`_Logs/psn9.stderr.log` records:
`RNPS curl: host transport initialized enabled=1`.
There was no completed RNPS request in this run. NP queries still fail locally:
`_Runs/psn9/_kyty.txt` shows `RegisterEvent7`, `RegisterEvent4`,
`GetSigninStateForUser`, and `GetServiceState38` returning `0x80550002` with
`endpoint_connected=0`. The PSN agent is implementing the separate service
wiring. Home rendering does not establish PSN authentication or HTTP delivery.

The bounded launcher stopped this run at422 seconds (exit-1); no spontaneous
process crash is established by that exit code.

## Integrated PSN service build

Applied the reviewed PSN service patch: owner-bound NP query resolution,
source-admitted NP/LoginMgr transport installation and dispatcher retention,
and the native UserService property6 getter binding. Six standalone service
tests passed in `_Build/psn-ui-ipmi` (0.79s).

The integrated build then linked `kyty_emulator`, `network_http_tests` and
`np_account_abi_tests` successfully. Four focused tests (`np_account_abi`,
`host_http_tests`, `network_http_tests`, `rnps_curl_tests`) passed in 2.76s,
including the private NetCtl output/error checks. Build evidence:
`_Build/vsh-integrated/psn-service-integration-build.log`.

Regular credential admission is still blocked. The original SELF containers
exist, but the available slot41 candidate fails regular Core/UI RSA-PSS
verification. Required input is the correct regular slot41 AES-128 candidate
or source-bound service0x30 metadata output: Core SELF `[0x530,0xb60)`
(0x630 bytes), UI SELF `[0x530,0xb80)` (0x650 bytes). Source-admitted service
installation cannot activate without that input. The credential follow-up
verifier and evidence remain in the `psn-login-ui` worktree.

`psn10` uses a copied `psn9` profile and a regenerated regular stage for live
integration validation. Virtual-key messages produced visible selection changes
without modifying SDL. Home and the Store selection are captured in
`_TempData/psn10-home.png` and `psn10-store-selected.png`. The user also navigated
during this run, so later route changes cannot be attributed solely to the
automation. Network settings visibly remained at Checking/Not signed in;
`psn10-store-open.png` captures that screen, not a functioning Store.

User-reported failures: Users and Accounts does not open; network status remains
Checking; PS Plus cannot load; Store is empty and does not respond to confirm.
`_Logs/psn10.log:5078,5087` specifically records Settings failures in
`Account.isSignedIn` and `LoginMgr.getSigninState`, both `0x80550002`.
The run reached its 420-second launcher limit (422.4s elapsed), not an observed
spontaneous crash. App switching and property6 callback success remain unverified.

## Interface-list failure found in psn10

`_Runs/psn10/_kyty.txt:502558` shows `sceNetGetIfList(3,buffer,1)` interpreted
capacity=1 as a string byte count, writing one NUL and returning zero entries.
Settings consequently reports `interface not found` despite the host-backed
NetCtl state. This is distinct from the NP service failure.

Regular `Sce.Vsh.SysBridge.Net` metadata defines the three-argument API and
480-byte Sequential/Pack4 record. Retained decompilation:
`_TempData/psn10-net-sysbridge.cs`. ShellUI native callers +71919a/+71921b require
return count1, reading IPv4 at+0x28 and MAC at+0x50. Names are LO0=0, ETH0=1,
ETH1=2, DBG0=3, WLAN0=4, WLAN1=5. The replacement enumerates actual Windows
adapters, includes disconnected adapters, writes only a returned record, and
honors disabled host networking. Link presence is not reported as Internet or
PSN availability. Debug/other absent interfaces remain absent. Focused tests
cover actual host loopback data, ABI bounds, bad pointers and offline behavior.
Both binaries linked successfully; PowerShell reported exit1 from CMake's
stderr GLOB-mismatch notice rather than a compiler/link failure. The focused
`network_http_tests` passed in1.21s. `psn11` was launched with a copy of the
`psn10` profile and a 900-second limit for the user's Network settings retest.
`psn11` aborted after225.11s (exit321), before the900-second limit. The user
confirmed the reproduction: **profile picture -> Add Profile**, not Settings ->
Users and Accounts. `_Logs/psn11.log:6614-6617` reaches WebRunLoop initialization
and reports an unregistered `Sce.PlayStation.PUI.WebRunLoop::InitializeNative(int&)`;
the final `_Runs/psn11/_kyty.txt` trace records `Guest abort()`.
No successful Network settings outcome, account UI or Store is established.

## Add Profile native-provider gap

Regular `libSceNKWeb.sprx` exports raw library `libSceNKWeb`. Its
`BVDkopEoLMk` startup (+a230) performs one-time initialization via+b140, passing
the native registration table (+1c0020) to PSM's `vl8QdnxNxq4` (+23b3e0).
That PSM export iterates name/function pairs and invokes the registration
callback. The table's +1c0080 pair names the exact missing WebRunLoop internal
call and supplies NKWeb+b0a0. Verified from ELF dynamic tags, relocations and
disassembly, with local audit helper `_TempData/audit_nkweb.py`.

NKWeb's dynamic dependencies are Psm, Ipmi, UserService, NpCommon,
SystemService, LibcInternal and libkernel. In `psn11`, NKWeb was staged but
absent from `EnsureImportProviders`; ShellUI's startup import instead called
an unresolved stub (`_Logs/psn11.log:583`). Added the raw import library to
the existing provider lookup so the native startup can register its own
bindings. The emulator build passed (`nkweb-provider-build.log`). `psn12`
uses a copied `psn11` profile and900-second limit. Its loader log confirms
NKWeb loaded at0xaa0000000, ShellUI's startup import resolved to0xaa000a230,
and NKWeb's module initializer ran. The initial screenshot was still in boot;
Home and Add Profile validation remain pending. NKWeb has one remaining
unresolved libkernel import, `iiQjzvfWDq0` (native syscall0x29); this is not
evidence that the import was called.

## psn12 user validation: sign-in and hidden dialog

The user's screenshots establish Home, Switch User, Add User, Profile and the
QR sign-in page. Add User/guest sign-in passed the previous fatal WebRunLoop
point (`_Logs/psn12.log:3574`), but **sign-in is not functional**: the QR is an
unpopulated placeholder, Profile Sign In does not visibly proceed, and Users
and Accounts still does not open. Guest sign-in dismisses the visible panel
and dims the screen. Switch User's existing panel/text does render, so the
failure is not established for all modal UI. The run exited0 after650.02s,
before its900-second limit; this is not the previous guest abort.

Confirmed failure chain:

* `psn12.log:3587,8958`: `MF_ApplicationError 0x818e0000`, JSON parse unexpected
  end of input. NPXS40009 Hermes function681 calls
  `LoginMgr.createAuthenticationTicket`; function682 immediately parses
  `responseJson`, then reads the actual service-issued ticket/code/expiry.
  Virtual bytecode offset79002 is function682+30 (sum of function bytecode
  lengths, not a file offset). Audit: `_TempData/psn12-signin.js`.
* ShellUI imports `IxqaB0MHl0s`/`xR8S67myUos` were unresolved non-PLT imports
  (`_kyty.txt:10470,10506`). These are the ordinary/login authentication-ticket
  creation APIs, not RNPS curl. Their native 8/7-argument ABIs were verified
  against managed P/Invoke metadata and regular NpManager+125e0/+135e0.
* `psn12.log:3621-3624,4705-4710,9018-9021`: a ReactDialog is loaded and takes
  focus, with `ReactDialogHost width:0 height:0`. This matches the user's dimmed
  screen/missing prompt report. Zero-size warnings also occur during other scene
  initialization; these logs alone do not prove the final dialog bounds or a
  compositor defect. Dialog sizing/rendering needs its own live investigation.
* Settings still throws `Account.isSignedIn` and `LoginMgr.getSigninState`
  `0x80550002` at5721/5731. Network also retains `interface not found` errors.

The ticket exports now explicitly return the firmware's uninitialized error
without touching outputs, replacing the unresolved-success fallback. This
implements only the unavailable transport path: native request creation,
submit/wait/result delivery and source credential admission are still required
for actual tickets. A state-query endpoint does not initialize ticket transport.
Focused ABI checks cover both exports, unchanged guarded output, initialization
error precedence, and rejection despite an attached host query fixture.
Both emulator and ABI test linked successfully (`np-ticket-errors-build.log`);
`np_account_abi` passed in0.08s. Live confirmation of the corrected error path
is pending. `psn13` uses a copy of the `psn12` profile. The dialog remains
unfixed; the native PUI modal manager forwards the layout rectangle to its
modal scene, but the current evidence does not identify why that rectangle is
zero or whether a later layout corrects it. No firmware layout patch was made.

## psn13 and explicit local-account HLE

`psn13` confirmed the corrected failure path: `createAuthenticationTicket`
reports `0x80550002` at `_Logs/psn13.log:7818`, rather than successful empty
JSON. Its dialog still reports zero dimensions at7901; a subsequent sign-in
state error/dialog occurs at11593/11625. The user still sees a blank Settings
background and PS Plus cannot load. The run exited0 after652.12s.

The user requested LLE or HLE for the missing Users and Accounts service.
Added explicit `-LocalAccountHle` / guest `KYTY_NP_LOCAL_ACCOUNT_HLE=1`
selection for private current/active sign-in-state queries. It reads the local
UserService profile association: no account ID -> NotSignedUp(1), existing
account ID -> SignedOut(2). Configured local controller identities have no
enrollment. No online session is represented by this backend. Unknown users
return UserNotFound; unavailable profile state and bad output buffers retain
errors. Selection does not occur automatically on native query failure.

Actual retained guest process identity is required. Application-owned callers
use their prepared UserService identities including the stored account ID;
unprepared/retired applications cannot borrow the session profile. Snapshot
queries do not create users, write the store, or return credentials. Native
source-bound routing remains selected when the option is absent. Ticket
creation, subscriptions and PSN services are not implemented by this local
state backend. Its goal is to allow the real Settings UI to process legitimate
local/not-signed-in state rather than abort on missing NP initialization.

The standalone native-query export test passed in0.11s after the routing hook
change. Integrated emulator, NP ABI and VM/owner-test binaries built successfully
(`local-account-hle-build.log`). `np_account_abi`, `runtime_user_service_owners`
and `runtime_user_service_controllers` all passed (0.31s total). Launcher
PowerShell syntax also passed. `psn14` launched with `-LocalAccountHle`, a copy
of the `psn13` profile and900-second limit. Live Settings entry validation
remains pending.

## psn14 confirmed UI progress

The user confirmed **Settings -> Users and Accounts opens**. PS Plus, Welcome
and Store now display a sign-in requirement instead of Can't load. This is
working local-account state handling, not a successful PSN session.
`_Runs/psn14/_kyty.txt` records the local backend returning private state1 for
the actual configured UserService profile0x10000001.

Sign-in still fails on `createAuthenticationTicket` with0x80550002
(`_Logs/psn14.log:6624`), followed by an invisible focused ReactDialog and
zero-size warning at6693-6696. The user hears the error sound and sees dimming.
Run exit0 after384.14s; no guest abort was reported.

Further read-only dialog trace: regular base_dll Hermes DialogPS function1299
passes the selected layout style to RCTDialogHostView. Fullscreen uses100%
width/height (function1285), and its surrounding dialogContainer explicitly
defines1920x1080. Popup layout uses native-exported widthStandard/widthWide
and min/max body dimensions (function1287). Retained decompilation:
`_TempData/psn14-dialog-js.txt`. A blanket replacement of zero dimensions with
fullscreen bounds would therefore be unjustified: the selected presentation
style and final parent/child layout still need runtime measurement.

## psn15/psn16 measured dialog layout and ticket prerequisites

The psn15 bounded debugger restored its breakpoint and detached normally after
ten observations. Nine ReactModalHost calls received either 1920x1080 or
1456x828. The failing ReactDialogHost received `(960,540,0,0)` at the shared
SetDimensions entry. Its ReactDialog scene still contained the initial native
928x474 dimensions before that call. This establishes an actual zero-size
layout delivery, rather than merely an early warning from scene creation.
Raw observations: `_TempData/psn15-layout-trace.json`.

psn16 relocated ReactNative.PUI to 0xd60000000 (psn15 used 0xd80000000).
A first attachment at the old base failed ReadProcessMemory before attaching;
the corrected probe at base+0x160b2d captured 53 successful conversions of
`100%` to float100 in LayoutShadowNode.ToYogaValue, then detached normally.
This rules out the suspected numeric percentage parsing failure for those
observations. It does not yet establish the error dialog's parent layout.

Further regular ShellCore ticket audit (RVAs, not file offsets):

* +cbc326 selects `/api/authn/v3/sso/ticket/consoleSignin` and the corresponding
  `oauth:create_authn_ticket_for_console_signin` scope.
* +cbc383 prepares `/api/authz/v3/oauth/token`; +cbc436 invokes scoped token
  acquisition at +cb9ad0 before sending the ticket request.
* +cb9bf7 constructs the client-credentials form with `token_format`,
  `client_id`, `client_secret`, `console_token`, and `scope`.
* +cbc552 invokes +cbc630 with the acquired token. +cbc7cc/+cbc80b construct
  `https://ps5.<environment>.playstation.net<ticket-path>`.
* +cbc862..+cbc8c6 populate Content-Type application/json, Authorization Bearer,
  and X-Psn-Console-Token. An npsso cookie is conditional on a nonempty input.
* +cbc994 creates the POST request; +cbca27 submits the supplied JSON body
  through the shared HTTP helper, then +cbca93 handles the response.

Consequently the ticket endpoint is not a standalone anonymous QR-code API.
Host HTTPS alone cannot replace the missing scoped-client/console-token
prerequisites. Ticket creation still returns0x80550002, and no authenticated
PSN result or visible corrected error dialog has yet been validated.

## Deeper kernel/secure-service audit (2026-11-07)

See `vsh-psn-deep-firmware-audit-20261107.md` for the source-pinned call chains,
NP methods5–9 completion-event contract, platform-auth device ABI, and envelope
packet layout. Both embedded OAuth envelopes now pass offline RSA-3072/PSS
signature verification using `tools/verify_psn_oauth_envelopes_1360.py`; this
does not recover their plaintext. Kernel platform authentication reaches
service80021005; envelope key setup reaches service80021001/command0x401.
The matching ARM handlers depend on additional secure context/key operations.
No emulator runtime fix or authenticated exchange was validated in this pass.
