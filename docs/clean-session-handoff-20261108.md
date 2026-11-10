# Clean-session handoff: VSH and media

## User's current instruction

Wait for the VSH agent, try its delivered work once, then hand off to a session
with clean context. Do not start another broad investigation in the old session.
The user is frustrated by repeated diagnostics without usable Home. Keep one
agent on VSH and the other on media; the previous reassignment of the VSH agent
to DRM was a parent mistake, not a prerequisite for Home.

## Repository and preservation

- Main: `${USERPROFILE}/Desktop/kytyps5`, branch `ps5-vsh`; nothing pushed.
- Firmware: `${USERPROFILE}/Downloads/13.60`; retail work stays `ps5-vsh-retail`.
- Preserve `_Runs/psn23/stage`, `_Runs/psn23/sd` and all original databases/users.
- Persistent VSH user268500997; local login is not genuine PSN.
- Preserve all media stores, old dirty worktrees and unrelated untracked
  `_DownloadData/`, `launches/`, `tools/__pycache__/`.
- Parent owns live processes. User controls controller navigation. Assistant may
  close/restart its own launches after checking process identity; retain logs.
- Commit scoped checkpoints with verified results and blockers. Native-backed
  HLE/LLE fixes only; no fabricated readiness, credentials, entitlements or data.

## Final trial result and current relaunch

Agent delivered `3cdd0542`; parent reviewed and integrated as `5f484f61`.
This is executor instrumentation and JS analysis, **not a runtime Home fix**.
Parent passed10 executor +22 delivery +34 gate +21 queue tests (87 total),
including pinned audits. Fresh-run offline preflight passed. Agent also reports
25 pinned Home controls; parent did not rerun those in this final integration.

Parent launched psn43 PID8968 using existing psn23 stores, regular13.60/native54,
native SSL/libc, host network, local-account HLE, foreground user/RNPS/VSH flow.
No database preparation/candidate activation. User had closed psn42 beforehand.

- Identity: `_Runs/psn43/launch-identity.json` (check creation FILETIME, not PID alone).
- Guest/host logs: `_Runs/psn43/_kyty.txt`, `_Logs/psn43.log`.
- Trace: `${USERPROFILE}/AppData/Local/Temp/opencode/home-executor-psn43.jsonl`.
- Completed capture shell: `sh_1bb943f1f001RJUfzF3CUefGIB`.
- Bounds were300s,6000hits,50000events,16MiB.
- Production executable remains build6b8cc8b, SHA256
  `048351953452fa10d6bc7d0f7081c5b20b71dfff1159f8e58056b84ce0e72739`.

User requested relaunch. Parent verified psn43 identity, requested graceful
window close, and it exited within15 seconds; debugger completed automatically.
End record: process_exit at174.522s,1539hits/5976debug events, no cleanup errors,
cleanup_verified false (no live detach/survival validation on process exit).

The trial is **inconclusive due to tracer decoding failures**, not proven missing
JS delivery. It acquired two empty Home results (contexts4/5) but recorded no
executor_queued hits. Executor call/return had34 Home decode failures each:
`read_bounds` at `executor.bridge`, type category null. Those failures occur
before association; both acquired requests remain outstanding. Investigate the
actual closure/context/bridge field and preserved-register assumptions before
another executor capture. Do not interpret the null decode as a proven dead
bridge or lost callback; offline synthetic/pinned tests did not validate this
live path. Full trace remains available for clean-session review.

Parent then launched **psn44 PID36952**, same profile/configuration, per user's
relaunch request. Current identity: `_Runs/psn44/launch-identity.json`; logs
`_Runs/psn44/_kyty.txt`, `_Logs/psn44.log`. **No tracer attached to psn44.**
Verify process identity/survival afresh in the new session. Do not repeat the
known-failing executor capture unchanged. No additional agent assignment was
started after this final trial; proceed with the requested clean-context handoff.

## Established VSH evidence

Home remains background-only; persistent user login works. Guest workflow and
genuine PSN are unverified. Three distinct boundaries must not be conflated:

1. psn41 queue trace: Games/Media serialized empty item results; queue progressed.
   Two context1 scalar actions reached error helper, exact exception unproven.
2. psn42 login delivery trace: both item requests correlated by fresh owner/IDs
   through concrete Resolve and first Callback.Invoke dispatch. Still blank UI.
3. psn43 executor trial now follows queued bridge call and executor call/return.
   Even returned executor calls do not by themselves prove JS consumption.

psn42 complete login trace SHA256:
`64cb448e4a0cf903eb69066824dd07f8c689bedf5c1f3a06f991a611219cd1f2`.
Lifecycle2 verified/released356 threads, zero residual/post-detach repairs;
same-process survival observed before user closed it. Earlier psn42 startup
trace exhausted its event budget before Home and must not be joined to login.

Static JS audit: empty selected-space arrays can leave a mounted menu invisible;
revision rejection falls back to reading, unlike a pending revision. Missing
current-user icon table and tbl_version in original app.db are verified, but
not yet proven to be the live scalar exception or sole Home cause. Do not force
opacity or invent items. Full Core recovery/Promote stays paused pending causality.

## Primary reading and artifacts

- `docs/vsh-home-psn42-executor-20261108.md`: new four probes, JS function gates,
  exact next-observation branches and stopping rule.
- `docs/vsh-home-psn41-continuation-20261108.md`: native callback chain/scalar fix.
- `docs/vsh-home-delivery-tracer-20261108.md`: complete psn42 evidence/teardown.
- `docs/vsh-home-psn41-queue-20261108.md`: restored original agent task/history.
- `docs/vsh-opus55-review-audit-20261108.md`: original evidence-led handoff.
- Temp/opencode: `home-queue-psn41.jsonl`, `home-delivery-psn42-login.jsonl`,
  `psn42-after-user-click.png`; saved run logs in repository `_Runs`/`_Logs`.
- Tools: `capture_home_gates_1360.py` modes gates/queue/delivery/executor;
  `home_*_trace_1360.py` and corresponding tests; `run_shellui_1360.ps1`.

## Agent/worktree resources

- VSH agent session `ses_e47fe88f9ffeoBjOjGWCxbQNho`, latest work complete at
  `3cdd0542`; tree `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/home-queue-capture`.
- Earlier VSH startup tree `.../vsh-home-startup` preserved at `feb5d2af`;
  render tree `.../vsh-home-render` at `681aa839`.
- Media agent `ses_e454b87f4ffemA3mv5eiBYc7l1`, tree
  `${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/media-resume-20261108`.
- Use `git worktree list` for all preserved trees; do not clean/reset them.

## Pending media work (keep separate from VSH)

- Apple startup Sync fix `32ffbdec`: isolated negative-control/fixed HEVC tests
  pass; **not integrated or live-verified**. Handoff:
  `docs/apple-startup-sync-20261108.md` in media worktree/commit.
- Native DRM ARM/Thumb firmware audit `5a04d48f`: **not integrated**;
  `docs/protected-media-construction-arm-firmware-20261108.md` in VSH tree/commit.
- Integrated Netflix private-buffer correction `b7e20e7d`: build/component
  tests passed, title playback beyond prior24% failure remains unverified.
- Temp/opencode `mapple1/launch.json` and `mnetflix1/launch.json` preserve original
  launch arguments/stores. Later mapple2/3 and mnetflix4 contain failure evidence.
- Existing DRM docs: protected-media-native-topology and
  protected-media-dispatch-tee-contract, both dated20261108.
- Opus High via Antigravity was backend-verified but failed for credits429; no
  completed High report. Saved conversation `c58db73f-2c59-4e3b-9243-b3b606a86261`.
  Low report is separate: `Temp/opencode/opus55-vsh-review.md`.
