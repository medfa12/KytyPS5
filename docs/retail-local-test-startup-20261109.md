# Retail saved-user LOCAL/TEST startup

## localhome1 follow-up: missing global auto-login selector

The first run restored the native profile and enabled the setup gate but
reported no auto-login user. Frozen `retail-localhome1/checkpoint1` hashes:

- host.log (6079682bytes): `102dae580a2bfb1b04fad127488e163931ae0bcfab6846473c7e0aa956292266`
- guest.log (153413547bytes): `5d98017fb4b6f0881e33b5d1cc15b8637830cb9647540e7164ca96f1fd6c1512`

These were independently rehashed. Guest32461 writes native profile
AutoLogin key0x07800400=1; guest32791 writes ID0x07800100=268501033. Thus
native saved-user restoration worked. However, guest309123 reads
**0x07010000=0**. Host1245 explicitly names that value
`SCE_REGMGR_ENT_KEY_USER_auto_login_user : 0`; host1247 reports
`autoLoginUserExist=False`, IDffffffff, followed by End(8-0).

Actual IDA of the selected LoginMgr AOT image SHA256
`cbb965a671bfa1601d07cff15f18d81ce32126b7df52f48489869a1b6b192587`
confirms `Utility.GetAutoLoginUserId` at +0x44010:

- +0x4402b supplies registry key0x07010000, initializes output to -1, then
  calls the registry getter at +0x4403a.
- On getter success it logs the returned ID, passes it to the subsequent
  validation predicate (+0x44094..a2), and returns true only when accepted.
  Otherwise +0x440af replaces it with -1 and returns false.

The address/name mapping came from the embedded MethodDef/call table; IDA
assembly is retained in external `automounter-ida/retail-autologin-query.txt`.
This selector is a **user ID**, distinct from the per-profile AutoLogin boolean.
The preceding preparation omitted this selection; no new native profile or
callback change is needed to correct that demonstrated omission.

With `KYTY_RETAIL_LOCAL_TEST_BOOT=1`, the registry now supplies the configured
`--user-id` for missing0x07010000, limited to the native persistent-home range
0x10010000..0x1fffffff. Out-of-range configured IDs select -1. Explicit guest
SetInt writes (including -1 to disable) still win. Default-off and all other
keys remain unchanged; the normal native validation predicate and login path
are not bypassed.

### Updated parent build/integration

- Candidate: `Temp/opencode/retail-autologin-selector1/kyty_emulator.exe`
- SHA256: `e611068dca9560150b092c73793da1e902ce56db1518c42a361e3da5b2dc8537`
- Only production replacement: `retail-autologin-selector1/libC.cpp.obj`.
- Base is the pinned773dacca local/test candidate, **not** an earlier Bond
  executable. Its other objects/providers are retained and rehashed.
- `tools/build_retail_autologin_fixture.py` records exact link inputs, source
  hashes, compiler/link responses and test results. At most two compile jobs.
- Old773dacca production object fails the selector check. Seven updated cases
  pass: actual0x10010029, alternate valid ID, out-of-range ID, absent/0/true/11
  opt-in settings. Cases also check adjacent keys, guest-disable/reselection
  precedence, setup-gate coexistence and unchanged native empty-slot defaults.

Use the same prepared profile/settings, actual `--user-id 268501033`, and
host `KYTY_RETAIL_LOCAL_TEST_BOOT=1`. No additional store edit is required.
The preparation tool now labels new receipts with the updated standalone
candidate hash; previously prepared stores remain valid. Parent should record
the final combined executable hash if combining this libC object with the
independent M.2 changes. This candidate deliberately does not include those
parallel source changes. No M.2 source, media code, launcher, live store or
shutdown-animation code was modified here.

Expected next observation: registry0x07010000 resolves to0x10010029, then the
native query recognizes that user and proceeds to native login. These fixture
results do not establish a successful runtime auto-login or Home transition;
the candidate remains unlaunched by this agent. The parent-reported shutdown
and process exit are separate from this startup diagnosis.

## Direction and result

The user explicitly chose retail firmware with local/test startup rather than
finishing OOBE. This checkpoint prepares the **existing native user**, not a
new native user, and exposes an opt-in setup gate for the comparison. It is
not a record of completed OOBE or PSN authentication. No emulator was launched.

Next-run artifacts:

- Candidate: `Temp/opencode/retail-autologin-selector1/kyty_emulator.exe`
- SHA256: `e611068dca9560150b092c73793da1e902ce56db1518c42a361e3da5b2dc8537`
- Prepared full copied store: `Temp/opencode/retail-localtest-prepared1/sd`
- Preparation receipt/backups: `Temp/opencode/retail-localtest-prepared1/`
- Native user: **0x10010029** (decimal268501033).

All paths above are under `${USERPROFILE}/AppData/Local/Temp/opencode`.
The parent owns launch, app imports, process lifecycle and controller navigation.

## Frozen profile2 evidence

The offline checkpoint hashes were independently reverified:

- host.log: `7d8736fa4b2430c305014f2c9e5b055569b46216e27e80d1a240b40f812c5f6a`
- guest.log: `579694cb74272d5a0759373148e0b1cb160ba8dd64ac515006c3c99a0841ac91`
- launch.json: `5d06e8d13cf6eb034b822706b7b0300cf341e5d2b2bbe882722080f9439b35bd`

Both72-byte native lists contain ID0x10010029, fifteen empty -1 slots,
high-water0x10010029 and group0. The existing config is16384 bytes, SHA256
`e9d1079b2b12f4b3107367bed1ee0865b5f5777b5d08a0a3e7ed7290af830d9e`.
It has a nonempty name, version0x91e, ordering1, auto-login0, flags1, and
zero passcode/account-ID fields. Names and auth payloads were not printed.
All three native provisioning roots already contain that user directory.

Guest1187544 writes268501033 to registry key0x07800100. Host19965 reports
`[LoggedinFromNM] userId=0x10010029`. These are positive profile/login
observations, but do not establish the user's precise navigation, completed
OOBE, or a successful later boot. The checkpoint also contains
`10010029/np/auth.dat`; it is copied unchanged, not interpreted as PSN proof.

The selected guest still read SYSTEM_initialize0. Its ShellUI imports for
registered-user list, auto-login, ordering, and name queries include HLE
UserService views; `priv/user/users.json` is absent in the frozen store.
Host19703 reports an empty local-user-info name for this ID. Thus a native
profile alone does not populate the current emulator's separate HLE view.

## Actual native contract

Actual IDA used the same pinned normalized retail Core
`3b60894581ab08abd281a5b66a1817a1c312268bc413fe2d39fe459e5fa36352`.
Reports are external `automounter-ida/retail-localtest-scan.txt` and
`retail-localtest-fields.txt`, alongside the previous creation/load reports.

- Native home IDs are the persistent class0x10010000..0x1fffffff. The saved
  ID/list/config must match; there is no blanket renumbering.
- Creation +0x9d4feb copies the name to record+4 with length0x11.
- The registry loader at +0x9de7d6/+0x9de7db selects record+0x48 and
  key0x07800400+slot*0x10000; +0x9de806 checks property3. The existing native
  client audit identifies property3 as four-byte AutoLogin, matching the
  `nqDEnj7M0QE` ShellUI getter and `GetStoredScalar(user,3)` HLE view.
- Creation +0x9d4fff..5005 sets record+0x5c bit0 for `isIncomplete`.
  Local/test preparation clears only that bit, retaining every other flag bit.
- Saved config loading +0x9d2420 accepts the0x4000 record, restores it to a
  free native record and installs the listed ID. Native group reconciliation
  restores profile state; the tool does not install callbacks or patch code.
- Core +0xa609a2..ac writes key0x02040000=1; the corresponding native readers
  test that key against1 (including +0xa69122). The existing emulator default
  is0, explicitly marked `SYSTEM_initialize: not done`.

## Opt-in source behavior

Host environment **`KYTY_RETAIL_LOCAL_TEST_BOOT=1`** makes the initial value
of key0x02040000 be1 and supplies the configured home-user ID for0x07010000
(the follow-up above). These defaults apply only before a guest SetInt
write; explicit guest values retain precedence. Unset,0,`true`, and11 do not
enable it. A local/test log label states that this is not completed OOBE/PSN.
The explicit comparison option takes precedence over the legacy first-boot
seed; parent should nevertheless set `KYTY_FIRST_BOOT=0` for clarity.

Source-bound Core/UI admission, native LoginMgr selection/transport, native
SSL, libc/provider selection and media paths are unchanged. In particular,
`LocalLoginMgr::Selected()` still excludes source-bound/native-adapter runs.

## Copied-store preparation

`tools/prepare_retail_local_test_boot.py --local-test-boot` requires one
matching, positive, existing native profile, schema0x91e, no passcode or bound
account ID, consistent lists/group/high-water and all three provisioning
roots. Additional configs/backups or a populated/unknown companion HLE store
are rejected rather than overwritten. It does not create missing native users.

The new full copy changes only:

1. Existing native config+0x48 to1 (auto-login).
2. Existing native config+0x5c bit0 to0 (explicit test skip of incomplete-user
   flow). All other profile bytes remain identical.
3. Creates the conventional HLE `priv/user/users.json` view with that same
   native ID/name/order, auto_login=true, matching local flags, and no NP
   account/sign-in identity. This mirrors the source-backed query schema in
   `TryLoadStoreLocked`, `GetStoredScalar`, and registered-user queries.

Loading that JSON clears HLE login slots/events and foreground state; it does
**not** report a login. Native LoginMgr still needs to execute its startup/login
path. The existing event/controller mechanisms remain responsible for session
state. This checkpoint does not claim that native and HLE session propagation
has been verified for the prepared next run.

Native lists, auth files, apps/databases and all other source files are
hash-verified unchanged in the clone. Original config and any empty companion
store are backed up outside the new mount. The original/frozen store is never
modified, and failed preparation does not publish the destination.

## Parent integration

The full prepared clone is already available. To reproduce from the frozen
offline checkpoint into another new bundle:

```powershell
python -B tools/prepare_retail_local_test_boot.py --source ${USERPROFILE}/AppData/Local/Temp/opencode/retail-profile2/checkpoint1/sd --output <new-localtest-bundle> --local-test-boot
```

For the next parent-owned launch:

- Use the candidate/hash above and label the run **retail LOCAL/TEST startup**.
- Set **host process environment** `KYTY_RETAIL_LOCAL_TEST_BOOT=1` and
  `KYTY_FIRST_BOOT=0`. The new opt-in is read by `std::getenv`; a guest-env-only
  setting is insufficient.
- Mount the prepared `sd` child, with existing writable home aliases pointing
  into that copy. Keep the bundle's receipt/backup outside guest mounts.
- Set guest `KYTY_USER_STORE` to the prepared `sd/priv/user/users.json`; set
  `--user-id 268501033`. Keep the existing retail native/source-bound auth
  records, native adapter and provider configuration.
- Parent imports the mounted apps for the actual0x10010029 catalog identity
  in this new clone. This work does not edit the external launcher/importer.

Expected observations to check: native saved-user restoration, ShellUI finding
one registered auto-login user, native LoginMgr login/event handling, and the
real Home bundle starting without the initial-setup branch. If startup fails,
capture the new logs/store rather than treating the preparation receipt as
proof of Home. Original profile2 and shared stores remain available unchanged.

## Verification

- Isolated two-job Bond-overlay build succeeds; receipt-pinned inputs rehashed
  before/after. Old-object control fails the new setup-gate check as expected.
- Five production registry fixture cases pass: exact opt-in, default off,0,
  `true`,11. They also check native slot defaults, adjacent-key behavior and
  guest-write precedence, including legacy first-boot coexistence.
- Seven offline preparation tests pass against frozen profile2: existing
  profile/list/auth/unrelated-file preservation, byte-bounded config edits,
  mirrored identity, explicit opt-in, no in-place/overwrite, multiple/mismatched
  profiles, account/passcode/schema rejection, populated companion rejection,
  unrelated flag-bit preservation and copy-failure nonpublication.
- CLI preparation of `retail-localtest-prepared1` succeeds with a complete
  preservation receipt. No native guest was run for this checkpoint.

## Parent launch integration

Parent independently rehashed the candidate, receipt-listed production sources,
and every file in both the original frozen store and prepared clone. All pins
matched. No emulator process was present before preparing the next launch.

The external `replay-strict-retail.py` now accepts an explicit
`--local-test-boot-receipt`. It validates prepared seed path/inventory, mode,
candidate hash and actual saved ID before application imports. The copied run
receives host LOCAL/TEST/first-boot settings, configured user268501033 and a
relocated **guest** user-store path. The launch manifest retains the preparation
receipt hash and local/test label. A focused integration check passed these
settings and rejected a different candidate hash; launcher syntax passed.

`retail-localhome1` preparation uses a new store clone, real retail native
providers and admission, the audited mixed RNPS/font resources, normal audio,
and six external app/game mounts. Catalog preparation targets the saved native
ID, including Minecraft. The original prepared clone and OOBE run are retained.
Runtime results follow separately; setup skipping does not establish Home.

Launch completed as PID1004, creation FILETIME134386977204807014, expected
candidate path/hash. Initial process query found a responding NPXS40087 window
at frame99. Guest log reports the explicit LOCAL/TEST gate, loads one saved
HLE user, and reads registry0x02040000 as1. Host log enters BackgroundAutoLogin.
These initial observations establish configuration/loading, not yet completed
native auto-login, Home presentation or application playback.
