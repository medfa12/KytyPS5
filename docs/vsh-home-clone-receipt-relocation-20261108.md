# Home trial: relocate cloned LocalUserStorage receipts

## Actual blocker and correction

The parent reports that the first copied-store control stopped at local login
with `80960001` / CE-106456-2, before any Home event. Guest log
`_Runs/home-catalog-control-20261108/_kyty.txt:37859` reports startup recovery
`code4 recovery1` at `user-10010004.receipt`. The completed Home queue trace had
no Home events; it cannot be scored as a catalog test.

`LocalUserStorage::ReadReceipt` deliberately requires the four canonical paths
in the receipt to equal its current roots/journal. A raw directory copy preserves
the old absolute psn23 paths, so all three saved receipts are invalid in the
clone. **Do not weaken Recover and do not remove receipts or owner markers.**

The clone preparer now plans relocation only for matching `user-XXXXXXXX.receipt`
files, validating the original exact canonical roots and journal, receipt magic,
ID, token, lifetime and create/destroy/upgrade intent. It verifies every present
final/staging/retired directory against the exact source **and clone** ownership
marker, rejecting foreign paths, malformed input, missing ownership, unexpected
links and mismatched tokens. It computes plans for both clones before applying
either. Only the four path lines are changed in each private clone; the original
receipt bytes are retained outside the guest tree under each clone's
`original-local-user-receipts/`. The trial manifest records old/new SHA256s for
each clone receipt and the normal inventory comparison accounts for those exact
changes. Any failure leaves the exclusively owned partial output without a
success manifest; it is not a launchable trial.

Pending/unpromoted receipt files are not interpreted or rewritten. Normal
`Recover` retains its own pending archival and transaction reconciliation.
Relocation assumes caller-verified source quiescence and exclusively owned new
clone directories; it is not an in-place repair tool or a general migration API.

## Actual-input and integration controls

Read-only copied the three actual psn23 profile root trees and journal into
`_TempData/real-user-receipt-validation-20261108/sd`. The standalone probe links
the **real** `src/libs/localUserStorage.cpp` and calls `Recover` with persistent
users 268500996 and 268500997:

1. Before relocation, Recover returned `InvalidReceipt` (code4, recovery1).
2. All three source receipts and matching owner files validated; the clone-only
   paths were rewritten and original receipt bytes archived outside its store.
3. After relocation, Recover returned code0. Its normal guest cleanup removed
   the guest receipt/directory only inside that disposable copy. The original
   guest receipt and directories remained present.
4. Full source ordinary-file inventory/hash comparison before and after passed.

`tools/test_clone_user_receipts.py` also constructs real storage using
`PrepareCreate` and executes real `Recover`. It covers pre-relocation failure,
post-relocation persistent survival/guest cleanup, unchanged source hashes,
interrupted native create, destroy and upgrade intent/recovery, malformed receipt,
foreign root, wrong token, and mismatched copied owner. A full preparer fixture
checks both clones, preserved markers, receipt archives and manifest entries.
Existing clone-preparer controls continue to cover other DB sidecars, source
change, no overwrite and quiescence assertion. These are storage/fixture tests,
not proof of logged-in Home in an emulator.

## Fresh parent-owned trial recipe

Integrate this checkpoint first. **Do not modify the already launched control or
its companion clone.** Parent must verify no source psn23 store/stage handles or
writers before asserting quiescence. Confirm the prior comparison inputs still
match; the preparer refuses changed inputs and existing output. Use a new trial
directory and new launcher log names. Example from the parent PowerShell session:

```powershell
$Parent = '${USERPROFILE}/Desktop/kytyps5'
$Review = '${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/home-review-20261108'
$Trial = '${USERPROFILE}/AppData/Local/Temp/opencode/home-catalog-trial-relocated-20261108'
python -B "$Parent/tools/prepare_home_catalog_trial.py" `
  --source-store "$Parent/_Runs/psn23/sd" --source-stage "$Parent/_Runs/psn23/stage" `
  --comparison "$Review/_TempData/home-catalog-comparison-v2-20261108" `
  --output $Trial --source-quiescent
if ($LASTEXITCODE -ne 0) { throw 'Preparation failed; do not launch' }

# Inspect trial-manifest.json, especially both receipt_relocation maps.
# Parent runs one at a time; the user controls login and navigation.
& "$Parent/tools/run_shellui_1360.ps1" -LogName 'home-catalog-control-relocated-20261108' `
  -EmulatorPath "$Parent/_Build/windows/kyty_emulator.exe" `
  -SystemDataPath "$Trial/control/sd" -ManagedStage "$Trial/control/stage" -WorkDir "$Trial/control" `
  -RnpsSource "$Parent/_TempData/rnps_staged" -UserStore -ForegroundUser `
  -LocalAccountHle -VshRuntime -RnpsReplay -NativeNpHttpPool `
  -NativeLibcStartup -NativeSsl -HostNetwork -NoDatabases -Visible `
  -MonoLogLevel warning -NoGraphicsDebugDump

& "$Parent/tools/run_shellui_1360.ps1" -LogName 'home-catalog-candidate-relocated-20261108' `
  -EmulatorPath "$Parent/_Build/windows/kyty_emulator.exe" `
  -SystemDataPath "$Trial/candidate/sd" -ManagedStage "$Trial/candidate/stage" -WorkDir "$Trial/candidate" `
  -RnpsSource "$Parent/_TempData/rnps_staged" -UserStore -ForegroundUser `
  -LocalAccountHle -VshRuntime -RnpsReplay -NativeNpHttpPool `
  -NativeLibcStartup -NativeSsl -HostNetwork -NoDatabases -Visible `
  -MonoLogLevel warning -NoGraphicsDebugDump
```

The launch sequence and native recovery limitations in
`vsh-home-copied-catalog-trial-20261108.md` still apply. Firmware/native startup
may change the isolated stores through normal guest operations; the original
psn23 store and original receipt archives remain outside those stores.

## Verification

Built the worktree-local `local_storage_recover_probe` with `-j2` and real
LocalUserStorage source. Run `KYTY_STORAGE_RECOVER_PROBE=<probe>` with
`python -B tools/test_clone_user_receipts.py`; all controls passed. The existing
`python -B tools/test_prepare_home_catalog_trial.py` controls passed. No process
was accessed or controlled, no parent build/output or original psn23 file was
written, and neither existing trial clone was changed.

## Parent validation and fresh copies

Integrated as `7898304c`. Parent compiled `LocalStorageRecoverProbe.cpp` and the
actual `localUserStorage.cpp` with installed VS18/clang-cl into isolated
`_Build/home-receipt-validation/local_storage_recover_probe.exe`. All six real
recovery/relocation controls passed with that executable supplied explicitly;
all three existing preparer controls also passed. No skipped test counted as pass.

Parent repeated exclusive read-open probes for all496 ordinary original psn23
store/stage files, then prepared fresh copies at
`${USERPROFILE}/AppData/Local/Temp/opencode/home-catalog-trial-relocated-20261108`.
The preparer's before/after inventories passed;117 IPC reparse endpoints were
recorded/omitted symmetrically. Both relocation maps contain three receipts.
Independent checks verified relocated/archive SHA256s, unchanged first two
receipt lines (including ownership token/intent), clone-specific path lines,
and both preserved corruption markers. Existing invalid clones were untouched.

Neither corrected clone has been launched. Apple mapple5 remains open for the
user's playback retry; parent has not interrupted it to switch trials.

### Corrected control launched

User later reported Apple lasted longer and asked about PSN/VSH/Netflix.
Parent recorded that as duration improvement, not successful playback, and
returned the live slot to Home. Mapple5 PID26208/FILETIME134386258218840330 was
verified and closed gracefully; all evidence and stores retained.

Both fresh trial launch vectors passed equivalence after normalizing their
clone/log/IPC paths. **home-control2** now runs PID5672, creation
FILETIME134386267083232193, production build7089743/SHA256
`498f1d6dc0a9a1a866e9afa4c68062798104f97320bd7f7f7170e1b392f61d5c`.
Identity is `_Runs/home-control2/launch-identity.json`, with guest/host logs in
the run directory and `_Logs/home-control2.log`.

This time guest lines37738/38109 show two saved users loaded and startup storage
recovery **code0/recovery0**, directly clearing the invalid-receipt startup
blocker from the prior trial. Login/Home acceptance remains a separate user step.

Fresh queue capture `Temp/opencode/home-control2-queue.jsonl` passed provenance
and armed227 threads at t0.880093 for user268500997; bounds240s/6000hits/
50000events/16MiB. The user was asked to press PS and select the same saved user.
Trace completion/teardown, control result and candidate run are pending.

### home-control2 initial capture completion

Capture ended at241.098204s/deadline, SHA256
`78e34b6d250007e9308e18c5a03ab019635e20859853ef0732e7d062637d7985`.
It acquired no Home requests: only two excluded non-Home lookup/action hits,
9122debug events, zero decode unknowns. Lifecycle2 verified/released304 threads,
one pre-detach residual repair, zero post-detach repairs and no cleanup errors;
cleanup_verified=true. Same-process survival and responding window were verified
after detach at frame900. Host log search found no MF_ApplicationError in this
observation. None of this establishes completed user login or catalog behavior.
Parent asked which screen is visible before another bounded capture; no repeat
idle trace or candidate launch was started. Corrected control remains open.

User subsequently clicked after the capture deadline. Guest lines816774/817105
record saved-user268500997 login, foreground268500997 and local login-export
result0. Fresh identity/survival observation still matches PID5672 and creation
FILETIME; window is responding at frame2582. This verifies the clone relocation
cleared the prior saved-login error in a live run. Home requests from this click
were outside the completed capture and cannot be inferred from it. Parent asked
whether the current display is background-only or has menu items before switching
to the candidate; no usable Home claim is made.

### Candidate trial and requested IDA chain analysis

User confirms corrected control stalled at the shining background after login.
When parent attempted verified closure, PID5672 was already absent; no closure
was performed or cause of exit established. A fresh process inventory found no
emulator. Parent then launched the previously prepared **home-candidate2** with
the same production executable and flags, using the separate candidate store.

Candidate PID27884, creation FILETIME134386273451253751; identity/path/hash and
responding window verified. Identity is `_Runs/home-candidate2/launch-identity.json`.
Guest lines37911/38222 show two saved users loaded and storage recovery0.
Fresh queue capture `Temp/opencode/home-candidate2-queue.jsonl` armed242 threads
at t0.494503, user268500997, bounds300s/6000hits/50000events/16MiB. Parent explicitly
asked user to press PS/select the same user only after arming. Candidate result
and trace teardown are pending; control's post-click Home queue was not captured.

User additionally requested disassembly of the entire chain using **IDA Pro**,
and explicitly retained the catalog experiment too. Installation exists at
`C:/Program Files/IDA Professional 9.3`; no IDA tool endpoint was exposed by tool
discovery. The Home Terra xhigh agent now owns actual IDA/IDAPython analysis in
its separate worktree: login/session -> Home entry -> database queue/native
reader/recovery -> serialization/Resolve/queued bridge/executor -> Hermes/JS
publication and visibility. It must validate hashes/RVAs/xrefs/register/closure
data flow and the psn43 decoding assumptions, distinguish native IDA from HBC
analysis, and deliver a concrete causal implementation step. This is an active
assignment, not a claim that the full chain has already been disassembled.

### Candidate initial timeout and user-requested reattach

Initial candidate capture ended at301.699943s/deadline, SHA256
`034d5a2f6c306d7e0a7426fe9d329bff1f15a314009f5c7773ed88f726dd643b`:
10597debug events, two excluded non-Home probes, no Home acquisitions or decode
unknowns. Verified/released295 threads, one pre-detach residual repair, zero
post-detach repairs, no cleanup errors and cleanup_verified=true. Same-process
survival was checked. The guest had no local login record at this observation.

A momentary frame92/0fps observation prompted concern about startup, but does
not establish a startup stall: later owned-window enumeration showed the actual
1038x614 visible window advancing at frame147/2fps. An attempted PrintWindow
capture returned only160x28 chrome and was not usable scene evidence. IsIconic
was false when subsequently checked; no window restore was executed. User then
confirmed they had been away and explicitly requested tracer reattachment.

Parent started a new same-process **independent** login queue capture at
`Temp/opencode/home-candidate2-login-queue.jsonl`, same verified PID/creation/hash,
user268500997 and300s/6000hits/50000events/16MiB limits. No observations from the
expired startup capture are joined to the new login request identities.

### Candidate login: native nonempty results and visible Settings

Reattach armed293 threads at t0.580804. User then said "we are in"; guest
lines1280403/1281205 confirm saved user268500997/foreground login and export
result0. Parent read one fixed live prefix436462bytes/4409 complete records,
SHA256 `f94bbd9f9035b0c064316041ece145e84cc67af8ce41211fb555a1539068f586`,
retained unchanged as `Temp/opencode/home-candidate2-login-prefix.jsonl`.

Same generation `1f60a658ee08fa43bec064ff723a2a08`:

- Games context4 request3 callbacks180/181: acquired110.689831, serialized
  completion113.591250, **item_count1**; preceding condition selects game,
  actual user268500997, limit14.
- Media context5 request5 callbacks186/187: acquired110.747429, completed
  113.705698, **item_count1**, corresponding media condition.
- Later context6 returns2; repeated context4/5 results remain1. Prefix contains
  34lookups/34actions/11completions and10context1 scalar errors (exact exception
  remains other), with no decode-unknown record. Scalar errors therefore coexist
  with successful nonempty native item results in this run.

This directly establishes native guest reader/materialization/serialization using
the candidate, beyond the offline SQL projection. Earlier psn41/42 returned0;
the current corrected control's post-click trace was missed, so this is not a
paired same-session count comparison. No full native recovery success, scalar
revision value, callback/JS acceptance or title identity is inferred solely
from item_count. Catalog source identities are established separately offline.

Parent PrintWindow capture now shows **Settings** rendered, Network row selected,
frame2412/25fps: `Temp/opencode/home-candidate2-after-login.png` (1038x614).
This is visible foreground UI evidence, not yet a screenshot of Home tiles.
User was asked to return to Home and verify Games/Media tabs and Library tiles
are visible and navigable. Candidate remains live; original psn23 is unchanged.
The exact fixed prefix and findings were forwarded to the ongoing IDA agent to
focus production profile bootstrap and any remaining visibility boundary.

### Completed candidate login capture

Full capture552459bytes, SHA256
`5bf5d6b379a64a0c07d6a0cc23e141236e3f15db5e8ecffd0b08aea07038e779`.
Ended301.540319s/deadline,292hits/6085debug events, zero decode unknowns.
Lifecycle2 verified/released375threads with zero residual/post-detach repairs,
no cleanup errors and cleanup_verified=true. Fresh same-process survival after
detach was verified; candidate remains open with no tracer attached.

Full Home records:52lookups,48actions,16scalar errors,15completions. Seven
Games/context4 results have count1, seven Media/context5 results count1, one
context6 result count2. Last requests49-52 have no observed action for about75s
before shutdown; application/lifecycle state was not captured, so this does not
establish a permanent queue deadlock. Condition requests have known completion
coverage limitations. Completed evidence and the visible Settings screenshot
were supplied to the IDA agent. Home-tile visual/navigation confirmation remains
pending; no candidate adoption into original psn23 or production bootstrap yet.
