# Retail AutoMounter boundary: native owners and HTTP prerequisite

## Result

This checkpoint implements real HTTP cookie/session ownership needed by retail
SNS initialization. It is **not a verified AutoMounter or Home boot fix**.
No AutoMounter event, device result, service readiness, or managed success is
synthesized. A strict-retail launch remains the integration gate.

The strict run's guest log first reports `_sceNpSnsKsnHttpInitialize()` failing
`sceHttpSetCookieEnabled` with `0x80431081` (line 57744), then
`sceNpSnsVshInit()`/PsnCore failure (58337–58338) and native NP teardown joins.
Our previous implementation deliberately returned that capability error for
every cookie-enable request. Later EHdd open failure (182629) and the managed
M2 exception are distinct missing native event owners, not evidence that
changing one event's bits would repair the startup sequence.

## Actual IDA 9.3 evidence

IDA Professional 9.3 and Hex-Rays ran on private copies of the **retail** files.
Only the Sony ELF `e_type` header was adapted for IDA, using the existing
hash-verifying preparation tool. Originals were neither changed nor executed
by this task. Imagebase is zero; all addresses below are original-image RVAs.
Reports, IDBs and local build receipts are outside the repository.

| Retail image | Original SHA256 |
|---|---|
| SceShellCore.elf | `f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434` |
| NPXS40087/eboot.bin | `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060` |
| Sce.Vsh.ShellUI.Legacy.dll.sprx | `fb6cc305e69345aa08d2d5e31e8f2c75ee817a2ac357de19d04b3df94b1f6837` |

* Core `+df49a0` creates the SNS network/SSL/HTTP resources and the
  `NpSns-agent/1.0.0` template. At `+df4a37` it sets ESI=1 and at `+df4a42`
  calls `XNUoD2B9a6A` (`sceHttpSetCookieEnabled`). Negative return goes to
  the exact `_sceNpSnsKsnHttpInitialize` failure message seen in the strict log.
  Separately, Core's ordinary NP workers `+ca5d00/+ca5da6` disable cookies.
  Both policies must work; globally enabling cookies is not a repair.
* Core `+3de890/+3de8c4` creates `SceAutoMounterEHddMountBlock` with
  attribute `0x121`, initial bits `0x1f`, retaining its handle at `+20a4eb0`.
  OOBE and boot configuration can subsequently clear bits. `+3de590` clears
  `~(1 << blockId)`; `+3de710` sets `(1 << blockId)`. `+3dee40` polls the
  five bits and also consults `SceShellCoreUtilUIStatus`; it is not teardown.
  The creating path is called by native `AutoMounter::DevctlReader::initialize`
  (`+3b4cc0`). The inspected handle xrefs contain no delete operation.
* UI `+872fa0` opens the EHdd event, polls mask `0x18` in AND mode and may
  set those ShellUI unblock bits; it closes its alias. `+873140/+873270`
  implement block/unblock through open, clear/set, close. These are consumers,
  not permission to create a placeholder when the producer has not run.
* The M2 failure is independent: retail Legacy `+13cde0`, independently named
  `Sce.Vsh.AutoMounter.BootManager.GetM2SystemModalDialogTypeFlags` through
  embedded MethodDef/AOT metadata, opens **`SceAutoMounterUIM2Evf`** at
  `+13ce2c`, throws on a negative open result at `+13ce37`, then waits for
  mask `0xfff`, OR mode2, and closes the alias. IDA verifies the instructions;
  the existing AOT metadata decoder independently resolves GOT `+65ef90` to
  that event name. This matches the strict managed fault boundary, without
  assuming it uses the EHdd event.
* Core `+3cb4c0/+3cb62b` creates the M2 event with attribute `0x110`, zero
  initial bits, handle `+20a4de0`, after registering UFS/USB callbacks. It then
  registers the real M2 mount callback `+3cc7e0` for event `0x310000`.
  The callback examines native mount result data and sets the selected modal
  flags (including its no-dialog bit1 path); the zero initial state is not a
  completed mount result. Native AutoMounter initialization `+3a42e0` calls
  the error-dialog producer at `+3a454e`. Its startup function is registered
  from Core `+a59ef0`. Later native storage/IPC progression remains untested.

## Implementation and checks

Each guest HTTP context now owns a separate WinHTTP session/cookie namespace.
Templates, connections and requests retain that namespace and inherit the
per-object enable switch by value. Transfers retain the session independently
of guest handle reuse. Deletion/termination releases its ownership outside the
network-table lock. WinHTTP performs actual cookie parsing, path/domain rules,
storage and redirect handling. Disabled requests disable both automatic cookie
send and receive; existing jar contents survive a temporary disable. Requests
have independent agents/timeouts instead of mutating shared session options.
Non-Windows enablement still returns the unsupported-capability error.

Verified locally with clang-cl and outputs isolated outside the repository:

* `HostHttpTests` with `tools/test_host_http.py`: passed actual loopback cookie
  persistence, path matching, disable/reenable, disabled receive, redirect
  cookies and separate-context isolation, plus existing transport checks.
* `NetworkHttpTests` with the same fixture: passed actual exported HTTP API
  policy inheritance, request overrides, context isolation, HTTP/HTTP2 sync and
  async transport, partial reads and offline checks.
* Exact pre-change `network.cpp` from shared base `0f63b337`, linked into the
  same new API test: fails `SNS cookie-enable prerequisite` as expected.
* Restoring the old backend's unconditional `WINHTTP_DISABLE_COOKIES`: fails
  cookie persistence as expected. A successful setter alone cannot pass.
* Existing `RnpsCurlTests`: passed loopback HTTP, callbacks, completion,
  cancellation and wakeup. Production HTTP consumers compiled successfully.

### Corrected build provenance

The first candidate (`99fafbde...`) incorrectly used stale `_Build/vsh-integrated`
objects. Its asserted `0f63b337` base was unsupported; **do not launch it**.
The parent caught different RNPS resolver, media decoder and main objects.
The earlier tests did not establish current-base integration.

The replacement uses the completed shared **`_Build/windows`** graph, compiler
options, generated headers and read-only link inputs. All five direct HTTP-header
consumers (`network.cpp`, `hostHttp.cpp`, `libNet.cpp`, `libNetIfaddr.cpp`,
`rnpsCurl.cpp`) and the three test entry points were recompiled locally, with
at most two compile jobs. Host HTTP, exported Network HTTP/HTTP2 and RNPS curl
loopback suites were rerun and passed against these corrected inputs.

All link input hashes matched before/after; no stale build path appears in the
replacement link map/options. Its embedded `0f63b33` base was verified. Binary
disassembly confirms RNPS code calls `ResolveGuestPath` at RVAs `+7a49e1`,
`+7a51eb`, `+7a6086`. The current RNPS, media decoder and main object hashes
were recorded and verified different from the rejected inputs.

Replacement executable SHA256:
`94bb17ab8c22b5430bd33e3a86884c072f03bf11ed8b6e867e66d4b3d72449ac`.
The isolated output/receipt directory is
`%LOCALAPPDATA%/Temp/opencode/retail-cookie-current`.
The embedded base revision does not include this retail checkpoint; the receipt
records retail source identity separately. No parent build file, firmware,
runtime store or emulator process was modified. This candidate was not launched.

No additional launch switch is needed: use the existing strict retail mounts,
retail-source assets and isolated empty store, including host networking already
enabled there. Verify actual SNS success, NP teardown disappearance, native
creation and signaling of both events, and then Home. Subsequent native errors
must remain visible; this checkpoint does not claim those gates passed.

## Strict-retail runtime result

The parent tested the corrected current-base candidate with the same original
retail launcher/bundles, a fresh empty store, normal audio and no replay. The
previous SNS cookie capability failure did not recur; native logging reached
`PsnCore 1789 msec` and continued to `NetService` initialization. This is local
startup progression, not authenticated PSN success.

Home still fails. PlayGoCore, BGFT, Share, ProfileCache and AppDb report SSL
initialization failures including `0x8095f008`. The EHdd event open still returns
`0x80020003`; managed `GetM2SystemModalDialogTypeFlags()` again throws that error.
The missing-event boundary is therefore not resolved by the cookie prerequisite.
Trace the remaining native initialization dependencies and SSL resource lifetime
before claiming that AutoMounter producers execute or publish mount results.
