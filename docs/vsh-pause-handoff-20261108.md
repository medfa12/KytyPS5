# VSH pause handoff — 2026-11-08

User is leaving and requested available evidence collection, agent wrap-up and
resumption later. Both agents were instructed to stop expanding scope, preserve
work, commit coherent checkpoints and return exact verification/blocker notes.
No further emulator attachment, navigation or restart is planned during this pause.

## Integrated state

- Branch `ps5-vsh`; latest implementation `6af05a9a`, validation `ab6f8192`.
- Home SQL layer `e7f4c96d`: native-derived filtering, two-stage order/limit,
  Library append/deduplication, typed dependencies and owned source assets.
- Private Force Eager/Estimate wrappers, result-wire layouts, selected-producer
  failure aggregation and native catalog staging integrated as `6af05a9a`.
  Normal two-job build, CTest and copied app.db/system-catalog tests passed.
- Tracer diagnostics `b2cfdf71`, teardown repair `07c1a2e2`; parent28 offline
  checks/ABI14 negatives and six64-worker Windows cases passed (`a005700b`).
- psn38 crash was residual tracer breakpoints, dump-proven. psn39 lifecycle-v2
  detached with347 verified threads/zero residual repairs and survived; `68a84f8d`.
- Production executable remains `3a12861`; newer local database work is not
  activated. No usable Home, genuine PSN or sustained media playback is claimed.

## Last live evidence

At pause, psn39 PID9200 (creation FILETIME134385727020718544) remained responding,
window frame20097/fps60. User can close the window normally: no debugger remains
attached. Do not reuse that PID later without checking process creation/identity.

`_Logs/psn39.log`2789+ confirms persistent user268500997 login and2867 Home mount.
Completed trace has61 manifest type-decode failures (49Dispatch,12Execute), so
requests/results could not be correlated. No first-render gate was established.
Artifact: `${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn39.jsonl`.

Later, outside that trace, `_Runs/psn39/_kyty.txt`1554923-1554945 records creation
of nonpersistent type3 user `0x20010000`, AppBrowse BeginTransaction `0x809c0103`,
WAITING_RECOVERY and corruption report `0x80f50074`, then native writing of the
four-byte `app.db-corrupt` report. Managed log6677/6696/6700 confirms login,
foreground switch and successful switch-user result for536936448. This establishes
the local guest switch result, not usable guest Quick Play/Home or online access.
Earlier AppBrowse sync-event failure remains `0x80020003` at163349 and
getConceptIds `0x80f50003` at201588/213780. Parent did not alter the live report.

## Pending agent wrap-ups

- Consumer/tracer session `ses_e47bd9e70ffeWnu9rB0sO3O1eU`, worktree
  `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/vsh-home-render`:
  correct `identity.manifest` decoding using pinned layout/inheritance evidence.
- Database session `ses_e47fe88f9ffeoBjOjGWCxbQNho`, worktree
  `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/vsh-home-startup`:
  Core AppInfo(0,10) prerequisites/recovery and scoped mount lifetime. Whole MMS
  directory staging must not hide unrelated databases/sidecars when activated.

Record returned wrap-up hashes and unrun checks before further integration.
Neither agent is authorized to touch the active emulator or original profiles.

### Consumer wrap-up received

Consumer committed `681aa839`; its worktree is clean and paused. This commit
is not yet integrated on ps5-vsh. Pinned metadata/AOT proves the old reader used
the manifest offset on the wrong owner: `PUIReactContext+0x30` is nested Data;
the manifest belongs to `PUIReactApp+0x30`. Correction follows the typed AppData
weak-app chain and validates backlinks, retaining strict Home/user identity.

Agent reports30 offline tests, four independent real-layout negative controls,
ABI audit,14 provenance negatives, offline preflight and diff checks passing.
Parent has read the handoff but has not rerun those checks. Teardown is unchanged;
no new lifecycle stress or corrected live capture was performed. Full evidence,
prerequisites and commands are in the consumer worktree's
`docs/vsh-home-manifest-identity-20261108.md`. Resume by reviewing/integrating
`681aa839` atop the already integrated diagnostics/lifecycle commits, then run
the documented offline checks before user-coordinated capture. No live work is
needed during this pause.

### Database wrap-up received — all agents paused

Database agent committed evidence-only `8f2cfca2` atop `bcc0b34e`; worktree clean,
ignored evidence retained. Not yet integrated. Handoff is in its worktree at
`docs/vsh-core-appinfo-pause-20261108.md`. No implementation changes or new
build/tests; staged diff check passed. Last implementation validation remains
`bcc0b34e` / parent `6af05a9a`, checkpoint `ab6f8192`.

Agent reports genuine Core version categories/compatibility branches, mode7
inventory-owner dispatch, and a Promote helper that discards callback returns:
its zero return cannot certify recovery. Exact-file routing remains unverified;
whole-MMS staging stays unmounted. Next resolve inventory owner `+2090e98`'s
vtable, obtain genuine copied AppInfo inputs, implement the evidenced policy and
operation wrappers, and establish namespace rollback/lifetime preserving nearby
MMS services. Recovery/preparation/guest publication and Home remain unproven.

Both agents have now completed their wrap-ups. Resume later from the two saved
worktree commits; no background investigation or live capture remains running.

## Resume order

1. Review both final agent handoffs; integrate coherent changes with their
   prerequisites and appropriate focused checks. Preserve unfinished work.
2. Correct/validate manifest identity decoding, then coordinate a fresh Home
   login/remount capture with the user. Require same-generation Games/Media
   settlements and distinguish empty data from missing interactive delivery.
3. Continue real Core AppInfo/other recovery producers, Promote, preparation
   publication and caller/mount lifecycle. Partial receipts are not readiness.
4. Resume guest Quick Play, genuine PSN and media acceptance after Home progress.

Keep `_Runs/psn23` profiles/stage, psn38/39 logs, the psn38 crash dump and trace
artifacts. Unrelated `_DownloadData/`, `launches/`, `tools/__pycache__/` and the
original dirty media worktree remain preserved. Nothing was pushed.
