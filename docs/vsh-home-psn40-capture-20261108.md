# psn40 — observed Home requests without matched item completion

User confirms animated background only after existing-user login. Production
`3a12861`, PID38448, creation FILETIME134386074330667953; same regular13.60,
native54 fallback and preserved psn23 stage/storage. No candidate DB activation.

Trace `${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn40.jsonl` used
diagnostic_version3/lifecycle_version2. The corrected manifest owner now validates
Home generation `b2d7d9f520528742b93c550e42d9a27a`, user268500997, module
`0x21de3ffb0`. Managed login success `_Logs/psn40.log`4057, promotion4086,
mount4196 agree. Native AppBrowse still fails sync-event creation at164746
(`80020003`) and getConceptIds at207035/218191 (`80f50003`).

## Observations

- Request2: Games condition, context4, contentLimit14, t108.395; Resolve at
  t110.040 with callback present.
- Request3: Games GetItems/home columns, context4, promise`205aaed10`, t108.427.
- Request4: Media condition, context5, contentLimit14, t108.460.
- Request5: Media GetItems/home columns, context5, promise`205abc6b8`, t108.500.
- Neither item request nor Media condition had a matched settlement before
  coverage closed at shutdown: observed-unsettled durations131.712s,131.648s,
  and131.682s respectively. No item count was decoded.
- Scalar2 request1 remained unmatched138.929s; request6 rejected after0.524s.
  Neither was classified as a revision lookup by this decoder.
- Home Dispatch included widgetReady, firstPaint and interactive for tags23/15
  aroundt102, before the item requests. These do not identify the required tile
  or prove JS consumption/startup release.
- Two Execute observations failed `condition_incomplete`;51 scope exclusions
  were49 non-Home and2 deliberately untraced callers.

Total746 hits: Resolve663, Execute14, Dispatch51, Reject18;8359 debug events.
Six correlated requests, two settlements. This proves bounded unmatched
observations, not permanent pending promises. Alternative completion paths,
managed pointer lifetime and queued-owner behavior still need investigation.

## Lifecycle and retained stack evidence

Trace deadline240s; end241.617492s, `cleanup_verified:true`,345 threads verified
pre/post detach, zero residual repairs and explicit holds released. Parent then
confirmed same process alive/responding, rendering frame6569/fps32.

Parent subsequently captured bounded sequential stacks (32frames/32KiB each):
`${USERPROFILE}/AppData/Local/Temp/opencode/psn40-home-stall-stacks.txt`
and `.raw.json`. All345 threads sampled without errors, prior suspend count0
and matching ResumeThread return1. Guest unwind limitations remain; raw stack
bytes are retained. Home Astra Medium agent owns analysis of these artifacts
and deeper inventory/AppInfo/queued accessor completion.

## Next live slot

Media retest recipe/summary integrated as `826bf844`. Agent verified existing
binary component reruns (not a fresh build); no new live playback result yet.
User selected Netflix retry next. At last inventory Home PID38448 was still
running; await normal user closure before launching Netflix with its original
authenticated mounts and corrected executable. No process was stopped by parent.
