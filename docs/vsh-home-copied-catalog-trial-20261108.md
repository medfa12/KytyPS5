# Copied-store Home catalog comparison and bounded live trial

## Outcome

The requested offline control/candidate comparison **passed** using a private
copy of psn23 app.db, its actual marker and saved roster, and copied genuine
13.60 Library descriptors/icons. For both saved users and each selected space,
the control `ReadHomeRows` returned SQLite error1 at prepare; its native staging
returned `ReadFailed` with no mounts or created asset directory. This is the
local read-layer result, not an observed guest SQLite exception.

The candidate committed two firmware source records and four per-user records.
For each user, Game returned exactly `NPXS40071`, Media exactly `NPXS40139`;
both staged successfully through `StageNativeCatalog` with one candidate DB
backend and two byte-identical read-only source directories. No version or
revision was added. Existing more extensive copied-input tests checked preserved
source rows, NULL values, ordering/limit, failed commits and cancellation.

The source files' SHA256 hashes were checked before/after both copying and the
comparison; original app.db and marker and all four firmware assets were
unchanged. The comparison recorded no AppBrowse journal/WAL/SHM. Input receipt
is `_TempData/home-catalog-comparison-v2-20261108/input-manifest.json`, test log
`comparison.log`, and candidate hash receipt `comparison-receipt.json` in that
same directory. The saved roster was read from the copied `users.json`, not a
hardcoded or configured login. Candidate app.db SHA256:
`27fc48f7216471cbab2d743c0254bb5f48ac0e0623863d7737587f4212f8aa68`.
The earlier independent comparison is retained in
`_TempData/home-catalog-comparison-20261108`.

This demonstrates a source-backed SQL/asset candidate, **not** native recovery,
native managed materialization, JS consumption, or usable Home.

## Recovery and mount audit: necessary versus host policy

* Pinned Core `+ff19d5 -> +ffe830` really checks corruption; psn41 log 47551
  records `80f50061`, and its AppBrowse initialization logs `809c0103` for
  icon/concept and other managers. Copied `app.db-corrupt` is genuine, and the
  Force `(1,4)` wrapper's distinct presence/secondary rule requests rebuild.
  A full native-ready recovery claim would require the actual `(0,10)` AppInfo
  Eager result, AppInfoShellUI `(1,5)`, AppBrowse `(1,0)`, remaining selected
  operations, native Promote and composite preparation publication. Current
  local `PreparationPass` deliberately reports these unavailable; the source-
  bound transport install does not wire these candidate APIs. Version/revision
  values or results cannot be inferred from a successful SQL candidate.
* However, treating **completed recovery as a prerequisite for any diagnostic
  read trial** is stronger than the observed firmware behavior. Later in the
  same psn41 log, lines 600649–600666 show a successful OS open and header read
  of app.db after the earlier failure. This does not establish successful SQLite
  wrapper registration, a current-user query, or guest materialization: an
  adjacent SQLite wrapper error is also logged. It is enough to justify a
  controlled cloned-store test without pretending the native database is ready.
* `StageNativeCatalog`'s directory mount plan is a host artifact, not a firmware
  requirement. Mounting its candidate directory over all of `/system_data/priv/mms`
  would hide neighboring databases and sidecars. Instead, the trial helper copies
  the **entire ordinary store**, changes only the cloned `priv/mms/app.db` in
  the candidate, and preserves its pre-change bytes separately. Every ordinary
  marker, neighbor and sidecar is copied into both stores. It makes two independent
  stage copies. The real firmware already contains the exact descriptor/icon
  paths, verified by hash; no asset overlay or per-file mount is needed.
* Source psn23 currently has 117 Windows reparse-point endpoints under `sd/ipc`.
  These cannot be treated as ordinary copied files. The clone tool explicitly
  records and skips only those endpoints; it refuses reparse points elsewhere.
  Both trial variants omit the same endpoints. The runner separately creates a
  fresh `/system_tmp` endpoint directory. This omission is a comparison caveat,
  not proof that every other IPC namespace is irrelevant.

The immediate missing native observation is whether the **actual Home**
`DatabaseModule` GetItems2 action for current user/context4 executes the reader
against the candidate and serializes a nonempty Game result. The current SQL
component is not wired into that path. If the candidate still produces zero,
attribute the actual `DbItemAccessor.GetItems` reader/exception branch at
`+1450c` / `+1455b..+146b6` and its accessor/connection state; do not add a
recovery-success shim. If it produces real rows but Home is blank, establish
same-generation JS publication and the tile `interactive` signal. No unchanged
psn43 executor trace is proposed.

## Parent-only preparation and launch, after source quiescence

The following is a **diagnostic A/B test**, not a production activation or
recommendation to change the original profile. No runtime trial clone or launch
was performed by this work. Parent must first ensure no process holds or writes
psn23's store or stage, including the other databases' WAL/SHM state;
the helper's `--source-quiescent` is an explicit caller assertion, not a process
check. It inventories/hashes all ordinary files before and after both copies,
rejects app.db sidecars or changed comparison inputs, preserves candidate-side
original app.db outside the guest tree, and refuses an existing output directory.
Other DB sidecars, including HTTP cache WAL/SHM, are retained in both copies.
Use fresh names; launcher logs are truncated if a log name is reused. The native
guest may itself change the **clones**, including recovery reports, so retain
the original marker and pre-run manifests for post-run comparison.

From the parent-owned PowerShell session, with the parent runner and a verified
finished emulator build, for example:

```powershell
$Review = '${USERPROFILE}/.local/share/opencode/worktree/fbbe5d/home-review-20261108'
$Parent = '${USERPROFILE}/Desktop/kytyps5'
$Trial = "$Review/_TempData/home-trial-20261108"
python -B "$Review/tools/prepare_home_catalog_trial.py" `
  --source-store "$Parent/_Runs/psn23/sd" --source-stage "$Parent/_Runs/psn23/stage" `
  --comparison "$Review/_TempData/home-catalog-comparison-v2-20261108" `
  --output $Trial --source-quiescent
if ($LASTEXITCODE -ne 0) { throw 'Clone preparation failed; do not launch' }

# Run one at a time, with user-controlled login/navigation. Use distinct fresh
# LogName values and preserve the first clone before running the other.
& "$Parent/tools/run_shellui_1360.ps1" -LogName 'home-catalog-control-20261108' `
  -EmulatorPath "$Parent/_Build/windows/kyty_emulator.exe" `
  -SystemDataPath "$Trial/control/sd" -ManagedStage "$Trial/control/stage" `
  -WorkDir "$Trial/control" `
  -RnpsSource "$Parent/_TempData/rnps_staged" -UserStore -ForegroundUser `
  -LocalAccountHle -VshRuntime -RnpsReplay -NativeNpHttpPool `
  -NativeLibcStartup -NativeSsl -HostNetwork -NoDatabases -Visible `
  -MonoLogLevel warning -NoGraphicsDebugDump

& "$Parent/tools/run_shellui_1360.ps1" -LogName 'home-catalog-candidate-20261108' `
  -EmulatorPath "$Parent/_Build/windows/kyty_emulator.exe" `
  -SystemDataPath "$Trial/candidate/sd" -ManagedStage "$Trial/candidate/stage" `
  -WorkDir "$Trial/candidate" `
  -RnpsSource "$Parent/_TempData/rnps_staged" -UserStore -ForegroundUser `
  -LocalAccountHle -VshRuntime -RnpsReplay -NativeNpHttpPool `
  -NativeLibcStartup -NativeSsl -HostNetwork -NoDatabases -Visible `
  -MonoLogLevel warning -NoGraphicsDebugDump
```

The parent runner is intentional: its shorter workspace path supplies a short
AF_UNIX `/system_tmp` path and its own fresh run/log directories; it must not be
run until the production build is complete. Review actual generated arguments,
RNPS overlay and executable hash for equivalence. The parent chooses process
ownership and capture; user chooses input. Do not run the two at once. A
nonempty serialized response is still not interactive Home acceptance.

## Verification

The accessor/display-constant inspector independently verified the four pinned
firmware hashes and Game/Media display masks in the copied descriptor inputs.
Standalone worktree-local `local_catalog_comparison` and `local_appdb_tests`
built with `-j2` using read-only SQLite sources from the preserved startup tree.
Both the ordinary suite and the copied-input suite passed. Three disposable
trial-preparer controls passed: preserved neighbors/markers/sidecars, no
overwrite, no unconfirmed quiescence, changed marker/candidate rejection, and
AppBrowse WAL refusal. No emulator launch, process access, original-store write,
parent source/build write, marker deletion or fabricated identity occurred.

## Parent integration and control launch

Integrated as `ebcefc5d`. Parent reran all three trial-preparer tests successfully.
The only live emulator before preparation was the separate Apple mapple4 run.
An exclusive read-open probe succeeded for all496 ordinary psn23 store/stage
files;117 IPC reparse endpoints were skipped. Full preparation then verified
source inventories/hashes before and after copying and produced:
`${USERPROFILE}/AppData/Local/Temp/opencode/home-catalog-trial-20261108`.
Original inputs were not changed. Both cloned stores preserve markers/neighbors;
candidate replaces only its own app.db, retaining its pre-change copy outside
the guest tree. The preparer's receipt records the endpoint omission.

Parent ran `-PrepareOnly` for both variants with the documented flags. Both
135-argument launch vectors matched after normalizing clone/log/fresh IPC paths,
and neither referenced psn23. Stage preparation operated on each clone.

After checking mapple4 PID36240 and creation FILETIME134386243332018858, parent
requested graceful window closure; it exited within15s. Apple logs/stores remain.
Parent launched the **control** only, using production builddde5d43, SHA256
`516e8cb3b0d8488d0fa9a6fa8126665a3ecf89e976530ced42dfd9476d65c975`.
PID29212, creation FILETIME134386251781959068, matching path and responding window.
Identity: `_Runs/home-catalog-control-20261108/launch-identity.json`;
logs under that run directory and `_Logs/home-catalog-control-20261108.log`.

Fresh queue capture passed provenance and armed239 threads at t0.831413:
`Temp/opencode/home-catalog-control-queue.jsonl`. Same selected user268500997,
four queue probes, lifecycle2, bounds300s/6000hits/50000events/16MiB. Module bases
were recovered for this process (Vsh df0000000, PUI da0000000), not reused.
User was asked to perform normal PS/login navigation. Capture completion and
teardown, observed Home result, candidate launch and A/B outcome are pending.

### Control trial invalidated: copied absolute storage receipts

User reported CE-106456-2 on saved-user login and again during Data Collection
in user creation. Host log records `LoginMgrShellUI.login` returning80960001
at line6349, and `LoginMgr.createUser` returning80960001 at9150. Separate pfAuth
80550002 failures remain local NP startup failures; they are not the same error.

The immediate cause is in **trial preparation**: guest log line37859 reports
`userservice-storage: startup recovery ... code=4 recovery=1` for copied
`user-10010004.receipt`. All three receipts (10010004,10010005,20010000) retain
absolute roots/journal under the original psn23 store. `localUserStorage.cpp`
Decode compares these to the mounted clone layout, returning InvalidReceipt.
`InitializeStorageLocked` consequently fails and disables store writes, so both
login and creation report the internal error. Ordinary byte-for-byte cloning
was insufficient for this host-owned, path-bound journal. This is not evidence
for or against the Home catalog hypothesis, nor a demonstrated regression in the
original profile. Parent acknowledged the preparation mistake to the user.

The queue capture ended at301.525798s/deadline:19410 debug events, two non-Home
probe observations, zero acquired Home requests/decode unknowns. Detach verified
302 threads, one pre-detach residual repair, zero post-detach repairs, no cleanup
errors; cleanup_verified=true. Same-process survival was observed afterward.
Parent then verified identity and closed the invalid control gracefully within15s.
Candidate was not launched. Both clones and logs are retained as failure evidence.

Post-trial hashes confirm original app.db, corruption marker, users.json and all
three original storage receipts still match the preparation manifest. Home Terra
now owns a guarded **explicit clone relocation** correction with real recovery
tests, preserving source bytes, ownership tokens and transaction intent. No runtime
receipt checks will be weakened, and no receipts or corruption markers dropped.
Fresh corrected clones are required before restarting the A/B comparison.
