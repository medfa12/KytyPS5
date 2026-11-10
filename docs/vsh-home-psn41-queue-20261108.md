# VSH priority resumed: psn41 queue capture

User reports Apple still displays Unable to Play and asks about VSH. VSH is
again the parent live priority. Persistent user survival/login was previously
verified, but usable Home, guest flow and genuine PSN remain unresolved.

No emulator was running at the switch; parent did not close mapple3. Its logs
and original media stores remain retained. The Apple agent continues isolated
Sync investigation without controlling the live session.

Parent launched psn41 PID25108 with production build6b8cc8b, executable SHA256
`048351953452fa10d6bc7d0f7081c5b20b71dfff1159f8e58056b84ce0e72739`.
Launch identity/creation FILETIME and actual command line are retained in
`_Runs/psn41/launch-identity.json`.

Configuration: regular Downloads/13.60, preserved psn23 stage and system/user
store, foreground persistent-user flow, local-account HLE, VSH session, RNPS
replay, native54 HTTP-pool family, native SSL/libc, host networking. Database
preparation and graphics debug dump disabled; Mono log warning. Existing launcher
restaged managed inputs and prepared URL configuration. No candidate recovery
database was activated. Controller navigation remains user-controlled.

New-run queue preflight passed with pinned Vsh/PUI/JSON/Mono hashes and current
module bases; no historical psn40 base was reused. The lifecycle2 four-probe
queue capture was started for180 seconds, user268500997, max4000 hits/10000
events/8MiB output at `Temp/opencode/home-queue-psn41.jsonl`.

Initial window capture `Temp/opencode/psn41-login.png` at frame155 shows a black
startup surface with thin colored lines at the top, not a usable login screen.
Capture completion, teardown verification, login, Home requests and post-detach
survival have not yet been observed at this checkpoint. Starting the tracer is
not evidence that Home or PSN works.

## Completed capture and next ownership

Capture ended at181.174s on deadline. Teardown verified/released344 threads,
zero residual/post-detach repairs, no cleanup errors. Parent checked the same
creation FILETIME after completion and captured the live window at frame3410,
35fps: still only Home's animated background, with thin colored top-edge lines.
Screenshot: `Temp/opencode/psn41-post-trace.png`.

There are8 Home lookups,8 corresponding actions,2 item completions,2 scalar
error entries,13 deliberately excluded non-Home observations and0 decode unknowns.
All fresh callback identities refer to user268500997 in generation
`8ac782c903119f44b99a69278d30de51`.

- Games context4 request3 callbacks176/177 reaches GetItems2 completion with
  item_count0 after1.279s. Its prior condition selects game, user268500997, limit14.
- Media context5 request5 callbacks182/183 likewise completes with item_count0
  after1.290s, following the media condition for the same user/limit.
- Both adapters use queue `0x2172b0df0`; the queue progresses through the actions.
  A persistent head-of-line stall does not explain these observed requests.
- Scalar2 context1 requests1 and8 (70/71 and398/399) reach the error helper after
  action dispatch. Exception class is the tracer's bounded `other`; exact cause
  remains unknown. Reject target is `0xdc0088240`.
- Item dispatch target `0xaf09c8005` is classified other/interface-thunk. Completion
  here proves serialized empty results before Resolve dispatch, not JS consumption.
- Four condition requests remain unmatched by this probe set; that is a coverage
  limitation, not evidence of four blocked promises.

User challenged why VSH remained unfixed. Parent clarified that the Home agent
had been reassigned to firmware DRM; that work produced analysis, not a Home fix.
Agent is now reassigned to the concrete VSH scalar errors/empty catalog path,
using native contracts and read-only stores. Apple fix `32ffbdec` has separately
arrived with isolated passing regressions but remains unintegrated/live-unverified
while the parent retains VSH priority. No usable Home claim follows from this run.

## Recovered pre-DRM assignment

User requested recovery of the exact prior VSH task. Git history and retained
handoffs establish this sequence, rather than a newly invented assignment:

1. `vsh-home-startup`: Core mode7 prerequisite implementation `4c6bb691`
   (parent `8a667fe6`), then queue/continuation and Promise coverage analysis
   `feb5d2af` (parent `ae90a877`). The worktree remains preserved and clean at
   `feb5d2af`. Full Core recovery/Promote was deliberately paused pending live
   causal evidence; component validation was not Home acceptance.
2. `home-queue-capture`: implemented the four-probe lifecycle2 tracer `05dd2bdf`
   (parent `e697761b`), with fresh callback-ID ownership and offline controls.
3. Immediate pre-diversion checkpoint `f108efa6`, authored12:22 on November8
   (parent `32fb4358`), audited the Opus review against psn40 and specified the
   next bounded live capture. First DRM commit `8aac40eb` followed at14:08.

The actual outstanding assignment was to determine where Home DatabaseModule
requests stop: Default lookup, adapter action, native result production,
Promise/callback delivery or JS consumption. It was not to continue adding
recovery prerequisites without evidence. Exact branching handoff is
`vsh-opus55-review-audit-20261108.md`, lines98–136.

psn41 now supplies the previously missing capture: item production reaches
serialization and scalar requests reach the error helper. The restored next
branches are concrete callback/JS delivery and scalar error attribution.
Empty item lists alone do not establish the cause of blank Home. Parent sent
this recovered chronology and exact branch selection to the original agent;
its existing work and commits remain intact.
