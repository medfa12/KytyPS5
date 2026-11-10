# Netflix login/navigation ownership — 2026-11-08

## Result and hard stop

Responsive login -> submit -> visible fade -> persistent black is established by the user. Parent additionally observed browser activation success. Neither establishes that the app received/applied authentication completion.

Static inspection identifies **request completion and native navigation-observer boundaries inside the actual Netflix eboot**, rather than a decoder-based explanation. The exact visible login submit callback, its auth-result consumer, and its fade-release/state transition are **not recovered**. No proven runtime contract violation or correction follows from this audit.

Inspected `PPSA01616-app0/eboot.bin`, SHA256 `be42bd7ccc0b436ef0e4309d78266a9661c8827e1012b564c14d838bd5edf8d9`, using the existing read-only `lifecycle-re-inspect.py` Image helper. All addresses below are image RVAs, not live addresses. Embedded JavaScript addresses identify text, not machine-code entry points. No builds or process operations were performed.

## Actual script/native ownership

The embedded Gibbon compatibility layer defines `gibbon2 = nrdp_platform.gibbon` and `resourcemanager = nrdp_platform.nrdp.resourcemanager` (text around `0x2c54a1d`). Its public `load` and `loadScript` delegate to this resource manager; `loaded` delegates to native `gibbon2.loaded` (`0x2c569c0`, also minified at `0x2beeee5`). Thus an OS socket return is several boundaries upstream of the UI callback.

Embedded Milo has readable and minified variants; the executing variant is not identified from the snapshot. The readable variant gives these exact boundaries:

| Text RVA | Owner / behavior |
| --- | --- |
| `0x29b719b` | `RequestManager.sendInternal(req)`: awaits `req.send(requestTimeouts)`; success assigns nativeErrorCode=0. Catch returns early on shutdown; otherwise maps network/native errors and status (default 500), then calls `handleResponse`. |
| `0x29b9205` | `RequestManager.handleResponse(req)`: missing request index clears buffers and returns; otherwise removes the request, sends inspector result, and returns if aborted. It emits `response`, then continues processing the result. |
| `0x29ba406` | Calls `engine.provideResponse(requestData.ids || requestData.id, response)` unless `handleMiloScript(req)` consumed the result. Clears buffers afterward. Minified counterpart call text: `0x28b7aa9`. |
| `0x29b2f2e` | `handleMiloScript`: for a configured script and 2xx status, evaluates/calls the script; success consumes the response. Exceptions set status=500 and nativeErrorCode=-1 and allow ordinary delivery. |

These are transport/dispatch semantics, **not authentication acceptance**. In particular, request completion can be dropped because it is aborted/missing, consumed by Milo script processing, or delivered to native code without proving that the higher-level login promise/callback finished.

Native binding evidence: string `provideResponse` at `0x1d845f2` is referenced by relocated table entry `0x2d367b0`; its callback slot `0x2d367d8` points to code `0xe33d0a`. That wrapper checks conversion result AL after `0x90da64` (`0xe33d71`); on success it passes thunk `0xe3ab56` to `0xd0f0bb` at `0xe33d86`. The thunk jumps through `0x94ad2e` to `0x94a144`. This is an exact native delivery/conversion boundary, not yet a decoded user-auth callback. The `loadScript` name at `0x1db51c3` similarly has table entry `0x2cdc398`, callback slot `0x2cdc3c0` -> `0xd211f2`.

## Navigation and request observers in native eboot

RIP-relative string references were followed into bounded disassembly, then direct caller instructions were checked:

| Event string RVA | Emitter function | String reference | Verified direct caller |
| --- | --- | --- | --- |
| `Page.navigationStarted` `0x1ddf142` | `0x12ccf4c` | `0x12ccff8` | `0x12a5f31`, `0x12a5f7c` |
| `Page.navigationCommitted` `0x1d2ed5e` | `0x12cd218` | `0x12cd297` | tail jump `0x12a611f` in `0x12a60f2` |
| `Page.navigationAborted` `0x1d25d0c` | `0x12cd0ca` | `0x12cd149` | tail jump `0x12a615b` in `0x12a612e` |
| `Network.loadingFinished` `0x1dfbfba` | `0x12c89da` | `0x12c8a83` | `0x12a4917` in `0x12a48be` |
| `Network.loadingFailed` `0x1d2ed48` | `0x12c87e4` | `0x12c88f9` | `0x12a4856` in `0x12a47c8` |

These construct named events using `0x12acb0a` and send via a virtual sink (+0x18). Emitters return without emission when their sink pointer is null. Commit/abort callers additionally call `0x127f458`, invoke virtual +0x40 and test AL before forwarding. The gate's full semantic identity is not recovered; do not assume these events reach normal console logging or bypass the gate. Nor does navigation commit prove a nonblack frame or member authentication. A same-page UI state change may produce no page-navigation event.

## Packaged scripts and existing logging

`data/resources/js/signin.js:4–25` has an explicit `nrdjs.initialize()` -> `nrdjs.profiles.signin()` -> `result.registered` branch, with `Failed to log in` / `Logged in!` messages and exit status 1/0. However, lines 28–55 require credential options, may factory-reset, and load another script. This is a standalone sign-in helper, **not evidence of the rendered login page's callback**; do not run it as an observation mechanism.

`data/resources/js/inject/quit-on-ui-loaded.js` wraps `nrdp.gibbon.loaded`, confirming an existing UI-loaded notification hook. Its exit behavior makes the shipped helper unsuitable for observation. `loaded` indicates UI loading, not necessarily post-login navigation. The directory's packaged helpers/error assets do not supply the recovered member-UI submit/fade handler.

Readable Milo contains existing trace messages `RequestManager Got error`, `handleResponse id`, `handleResponse calling provideResponse`, and missing/aborted branches. Some include URLs/error text; broad trace activation/raw dumps are not an appropriate bounded collection. Release/minified variants omit some of these messages. Their static presence does not establish runtime logging availability.

## Fixed snapshot reuse

One combined case-insensitive anchor pass over the retained guest prefix produced `_TempData/mnetflix3-audit/navigation-anchors.json` (counts/byte offsets only). No Page.navigationStarted/Committed/Aborted, Network.loadingFinished/Failed, authentication, networkerror, loadComplete/loadFailed or requestFinished matches occurred. The two appboot matches at byte offsets 8049827/8049862 are resolver-input records, not HTTP completion; the sole Gibbon match at 4669049 is a filesystem stat. Exception matches are confined to the already indexed early prefix. This absence cannot exclude app callbacks: neither the gated event sink nor verbose Milo messages are established as enabled. No second full-prefix scan or live-log read was performed for this checkpoint.

## Smallest decisive observation for one future capture

Candidate only; no guest assets or runtime source changed. Alongside the decoder trace, collect a bounded, metadata-only sequence at the **resource-manager callback boundary**: monotonic time, local request ordinal/generation, operation (load/loadScript), start/completion, numeric status/native error, exception-present boolean, callback entry/normal return/throw, cancellation/missing-request reason, and native provideResponse entry/conversion success. Preserve callback receiver, arguments, return/throw behavior and request identity; record no URLs, headers, cookies, response bodies or exception strings. Use separate small start/completion/error budgets with saturation markers so startup traffic cannot silently exhaust all error evidence.

In the same sequence, observe `gibbon.loaded` and navigation start/commit/abort, explicitly recording whether the observer gate/sink is enabled. Generic load success is insufficient: identify which request ordinal feeds the actual login consumer before interpreting it as auth completion. The decisive distinction is: **response never completes / completes but is not delivered / callback throws or never returns / callback returns and subsequent navigation or UI-loaded notification occurs**. A returned callback still does not prove the fade-release logic ran.

Hard stop: a verified installation mechanism for these guest-runtime probes, the executing script variant, and the actual downloaded UI/auth consumer are not established here. No guessed offsets for JS fields, no claim that generic navigation events own the fade, and no speculative fade reset. To attribute the final transition, the same future capture must expose the executing UI script identity and submit/auth-result/fade-completion callback locations through an approved bounded diagnostic mechanism. Until then this checkpoint supplies exact native and script boundaries, not a safe ready-to-install patch or an authenticated-app success claim.
