# Home first-render consumer checkpoint (regular 13.60 / psn37)

## Result and evidence boundary

The actual Home bytecode has **two separate gates after configuration**:

1. A nonempty experience list for the **selected space** (`game` initially).
2. A local startup promise released through a tile View's **`interactive`
   performance-timing event** and `loadComponents` callback chain.

An empty selected-space list can leave the strand, title, system icons and hub
at opacity zero even after all database requests finish. A rejected revision
lookup is caught and falls through to the experience reader. Neither the
background-only screenshot nor a revision SQL error proves a pending promise.

The new offline probe executes reviewed instruction subsets from the pinned HBC
and distinguishes these cases. **psn37's actual blocking gate remains unproven**:
existing logs contain no correlated request/settlement/item-count/JS-state record.
This checkpoint is consumer evidence and diagnostic tooling, not a runtime fix or
a claim that Home works. No production source, firmware, profile or DB was changed.

## Reproducible inputs and verification

Home: `system_ex_b/rnps/apps/NPXS40002/application.ps.bundle`, HBC89, 1,795,460 bytes,
SHA256 `565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f`.
Function numbers below refer to this exact file; `+offset` in HBC is function-relative.

```text
python tools/probe_home_first_render_1360.py <Home-bundle> --decoder <hermes-dec/src>
python tools/probe_home_first_render_1360.py <Home-bundle> --decoder <hermes-dec/src> --dump 543 546 547 553 8786 9008 9016 9019 9040
python tools/probe_home_first_render_1360.py <Home-bundle> --decoder <hermes-dec/src> --dump 1958 1959 5744 6721 7182
```

The decoder is an external, existing `hermes-dec` checkout, not bundled firmware.
The probe disables Python bytecode writes before importing it, reads the bundle
only, and verifies the input hash again after experiments. It is a bounded subset
interpreter with explicit native/React/animation test doubles, **not Hermes or a
React integration test**. Unknown instructions/properties fail closed. Numeric
interpolation tests style construction, not animation scheduling or presentation.

Verified controls (25 reported cases, 13 HBC functions executed):

| Input/control | Observed offline outcome |
| --- | --- |
| Revision rejects; experience read returns `[]` | Catch at #8786+104, read called with `isCached:false`, empty value yielded, generator completes |
| Revision rejects; experience read returns an item | Same fallback; item yielded and generator completes |
| Revision stays pending | Stops at that await; experience read not called |
| Item read stays pending | Read entered; stops at item await |
| Item read rejects | Error propagates out of readThrough; distinct from pending |
| Initial animation values | Scale array and five scalar values are all1 |
| Style input1 versus0 | Four menu opacities are respectively0 versus1 |
| Empty selected experiences | Startup completes without prepare/reset/start; startup effect invokes no action |
| Nonempty experiences, local signal pending | Prepare/reset occur; no animation start |
| Nonempty, local signal released, mounted | Prepare/reset/reset/start occur |
| Signal released after unmount | No animation start |
| Media items, selected game empty | Selected-space read still returns `[]` |
| Missing versus installed callback holder | Readiness false versus true |
| `interactive`, capture enabled | Actual timing dispatcher invokes matching callback |
| `firstPaint`, `widgetReady`, or capture disabled | Does not invoke the interactive callback |

Wrong-bundle and wrong-AOT-image controls fail at the full-image hash gate before
decoding. `git diff --check` passes. Outputs are local
`_TempData/home-first-render-probe.json`, `home-native-verified.txt`,
`home-db-verified.txt`, and `home-pui-verified.txt`. No emulator build or live test
was performed for this tooling-only checkpoint.

## Exact AppBrowse consumer contract for the DB owner

Module69/#543 defines the columns, ordering and spaces. #546
`fetchTitleByUserId(userId, maxTiles)` starts separate `game` and `media` requests
and returns **`Promise.all`**. One pending request holds the aggregate; one rejected
request rejects it. #547 creates per-space AppBrowse adapters and applies:

```text
condition = {
  userId: actual logged-in local user,
  contentLimit: maxTiles,
  filterTypeDispLocationExperienceSwitcher: "display",
  filterTypeDispLocationSpace: "game" or "media"
}
order = [
  {fieldName: "sortPriority", ordering: "ascend"},
  {fieldName: "altPinnedStatus", ordering: "descend"},
  {fieldName: "lastAccessDate", ordering: "descend"}
]
columns = [bgMatteType, conceptId, conceptInfo, deeplinkUri, hubAppUri,
           icon0PathWithTs, localConceptId, pic0PathWithTs, pinnedStatus,
           sndPathWithTs, titleId, titleName]
```

Search conditions are set when each per-space context is first allocated;
`getItems(ctxId, columns)` is then called. #546 also updates listener context
conditions with `{userId, filterTypeDispLocationExperienceSwitcher:"display"}`
and last-access-update context with `{userId}`. Capture context IDs and module
generation when checking a user switch; a line containing `getConceptIds` is not
enough to identify any of these requests.

Loader #2198 maps actual `response.items` into experiences; falsy userId returns
`[]`. #8766 sets the query's `called.current=true` before awaiting results, then
publishes fetched data into module42. In JavaScript `[]` is truthy: an empty array
is a publishable result, not an outstanding request. Module42/#383 groups by
`spaceId` and writes module46's per-space atoms; missing groups become empty arrays.
Module46/#404 initializes these atoms to `[]`. Module61/#464 selects the atom for
module22's current space; module22/#268 initializes that selection to `game`.
Consequently **aggregate count >0 is insufficient if all items are media**.

Revision helper #553 calls
`ConceptBrowseItemAccessor.getAppBrowseDbRevision(ctx)` and reads `.revision`.
ReadThrough #8786 catches revision/cache-get failures at+104 and invokes
`read({isCached})` at+124. Cache-write/revision failures have another catch at+1df.
A truly pending revision remains an await; a rejection is a supported fallback.
The probe's item-error propagation is the readThrough boundary; outer query
#8766 separately catches actual `Error` values and publishes its error atom.

This is the **UI AppBrowse `app.db` consumer**, not Core's system/user
`appinfo.db`. Per the DB owner's `vsh-local-appdb-runtime-20261108.md` (parent
commits53923722/0e10e578/0feda2f7): Core `access_index` initialization does not
satisfy Home `table_access_index`; UI+472b50 advances the latter after successful
row work (e.g. +474b24). Additive metadata-schema completion alone supplies
neither real current-user catalog results nor those mutation-derived revisions.
Generic preparation registration and Core+b23370 observer registration are not
readiness publication. The native backend is not guest-activated in psn37.

## First-render lifecycle and local-promise release

- Root module379/#3079 enters module380/#3080. #3081 gates children on the appConfig
  atom. AppConfig #3270 uses Suspense fallback null. Verifier #3254 includes
  Registry `npEnv`, Info software version/class, Account info and Plus membership;
  its account/membership rejection fallbacks belong to the firmware. A genuinely
  pending prerequisite is still distinguishable from its handled rejection.
- Main #5715 requires query `called` and module545 readiness before rendering the
  main menu. #5730 checks atoms144–147 with `value !== null`; these hold telemetry
  callbacks, not DB readiness bits. `called` is a ref, not proof of settled data.
- Startup #9008 reads selected-space experiences, initializes animated values1,
  creates a **local Promise** (#9013), stores its resolver in ref15 (#9014), and
  returns it as `startStartupAnimation`. The promise is slot16; mounted ref17.
- #9008+1d9..1e4 derives slot18 from `!!experiences.length`. Effect #9019 checks
  this before *all* phase actions. Empty data prevents login start, hidden reset,
  and finished-phase forceFinish. #9016+16 independently returns for empty data.
- With data, #9016 prepares/resets, yields slot16 at+3d, then checks mounted ref
  at+47..50 before reset/start at+53..80. This is not a database promise.
- Style builder #9040 maps [0,1] to opacity[1,0] for `strandContainer`,
  `titleContainer`, `system`, `hubViewer`; strand translation becomes1920 at1.
- Main menu #5734 passes `loadComponents` through experience-switcher/space/tile
  components (#5750, #6248/#6254, #6484/#6492, #6553, #6703) to telemetry
  wrapper #6721. That wrapper installs View `onPerformanceTiming` except in the
  hidden startup phase. It registers `interactive` with `useOnInteractive`.
- `useOnPerformanceTiming` #1958/#1959 checks capture enabled and
  `nativeEvent.name`, then invokes the matching timing callback. `useOnInteractive`
  #7181/#7182 records TTI marks and calls `loadComponents`. Space #6487 forwards
  it; main #5744 pipes lazy-load initiation and `startStartupAnimation`.
  Module34's pipe is synchronous reduction, not an await of metadata completion.

Thus a missing **interactive timing delivery** can leave the local promise
pending with nonempty data. First-paint telemetry, animation-independent
background motion, and NativeEventEmitter warnings do not establish delivery.
This is a source-proven dependency, not proof of a psn37 event-dispatch defect.

## Managed/native boundaries (module-relative RVAs)

`tools/inspect_home_consumer_1360.py` pins all three original AOT images:

| Kind / DLL | SHA256 |
| --- | --- |
| vsh / ReactNative.Modules.Vsh | `64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee` |
| db / Sce.Vsh.Accessor.Db | `739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766` |
| pui / ReactNative.PUI | `a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914` |

Metadata identifies names/signatures, but stripped managed IL bodies are `ret`
stubs. Behavioral evidence comes from AOT disassembly:

- Vsh DatabaseModule `Execute +471f0` queues work; +484b0 looks up the adapter and
  queues the adapter action. +48630 invokes it; its exception path checks module
  disposed byte+40 before rejection (+486d5..db). `RejectIfIsNotDisposed +46e50`
  has the same guard. A missing completion after disposal cannot establish a
  pending request in the replacement Home instance.
- Vsh GetItems2 action+48800 dispatches adapter slot+b8. Public
  `DatabaseJsAdapter.GetItems2 +196cb0` calls protected slot+c0, serializes through
  result slot+68, and reaches the promise interface call at+196e17 with its result.
  GetItems has the analogous completion at+196ca5. These are concrete completion
  sites to correlate; static reachability alone does not prove execution.
- AppBrowse adapter GetItems+1a7ad0 sets requested fields and calls accessor slot+80
  (+1a7cc2 on its ordinary path). DbItemAccessor.GetItems+14490 calls the actual
  reader at+1450c. Exception handling checks IsDeletedUser+14d40 and
  IsRegisteredUser+14da0, and can return its initialized list or enter recovery;
  other paths throw. A logged SQL exception alone does not specify the JS result.
- PUI `SetOnPerformanceTiming +175210` installs handlers;
  callbacks+177aa0/+177ba0 construct/dispatch timing events conditionally.
  `BaseViewManager.IsInteractive +52d20` tests native view state;
  `ReactPerformanceTimingEvent.Dispatch +555a0` is the event-to-JS boundary.
  No breakpoint hits or runtime predicate values for psn37 were obtained.

```text
python tools/inspect_home_consumer_1360.py vsh <ReactNative.Modules.Vsh.dll.sprx> 196cb0:17c 484b0:260 46e50:20
python tools/inspect_home_consumer_1360.py db <Sce.Vsh.Accessor.Db.dll.sprx> 14490:2de 14d40:cc
python tools/inspect_home_consumer_1360.py pui <ReactNative.PUI.dll.sprx> 175210:2d8 177ba0:200 52d20:56 555a0:c0
```

## psn37 provenance and next live acceptance gates

Read-only `_Logs/psn37.log`: successful login268500997 at5088, focus Home5174,
MountApp/Running Main5217–5219. Later explicit
`psaccounts:switchuser?userId=268500997` at12140, then logout/successful switch to
268500996 at12613 precede old-Home termination12837+ and new mount12963. This is
user-switch lifecycle, not evidence of an automatic Home crash/restart.

The old psn30 breakpoint trace observed revision rejection and item promises
resolving; it cannot identify psn37's pending requests. The earlier read-only
OpenProcess attempt for PID6812 failed with WinError87 before obtaining memory.
No memory snapshot or process-exit explanation resulted. Production remains the
user-owned psn37 build3a12861; its corrected single Composite instance does not
settle these Home gates.

For the next owner-authorized run, record each boundary for the **same Home
generation and user**, rather than infer state from absent generic logs:

1. Root configuration completion and main-menu mount; called/ref and each of the
   four callback holders if mount is absent.
2. Revision and both space-item requests: adapter/context, promise identity,
   userId, condition/columns, actual resolve/reject, item counts and error code.
   Prove pending only with request identity and a bounded outstanding record.
3. Selected space plus its published experience count. An empty result is a
   completed data gate failure, not an unresolved native promise.
4. For nonempty data, local startup slot16/ref15, mounted state and phase;
   native timing event name/target tag, dispatch, #1959 match, #7182 and #5744
   invocation. This distinguishes event production/delivery from JS consumption.
5. Real animation start/completion and visible menu input/focus. SQL COMMIT,
   `table_access_index`, event registration and first paint alone do not pass.

No evidence presently justifies synthesizing catalog entries, a revision,
`interactive`, readiness, or startup completion. The next fix should target the
first boundary actually shown to violate this consumer contract.

## Parent integration and next capture

Integrated as `6add9495`. Parent reran the pinned Home probe against regular
13.60 using the existing `hermes-dec-oobe/src` decoder: all25 cases passed across
the13 listed bytecode functions, and the input hash remained unchanged. These
are bounded offline controls with test doubles, not live runtime observations.

A subsequent process inventory found no running `kyty_emulator.exe`; no shutdown
or restart was issued by the parent for this checkpoint. The consumer agent is
implementing opt-in correlated tracing for request settlement/item counts and
interactive delivery for the next coordinated capture. The native database agent
received the exact selected-space conditions/order/columns and the finding that
handled revision rejection alone does not block startup. The active live gate
remains unproven; usable Home has not been demonstrated.
