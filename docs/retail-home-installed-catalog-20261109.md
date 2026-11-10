# Retail Home installed catalog: display correction, native admission still separate

## Outcome

Home rendering is accepted at the parent's `983b04f7` checkpoint; this task
addresses the actual **No Apps Installed** result, not the former blank Home.

**Proven importer defect:** the extracted-game fallback `dispLocation=2`
permits Game space but excludes Minecraft from both Library display and the
Home experience switcher. `tools/prepare_shellui_databases_1360.py` now uses
the three independently evidenced bits, **2|8|128 = 138**, only when an external
category0 descriptor omits `displayLocation`. Supplied flags stay unchanged.
An idempotent repair touches only that former integer2 default on the exact
`cid:local:<title>` / `/user/appmeta/<title>` import identity and current user
table. It changes no install, entitlement, authentication, version or recovery
field. It does not certify a real native installation or bypass admission.

**Independent native startup failure is also proven:** AppBrowse's corruption
check rejects initialization, and Core AppInfo has no usable connection.
Neither native recovery nor end-to-end installed-item delivery is fixed here.
Later saved-log reads of app.db prevent generalizing this to “no reader ever
opened the file.” There may be additional predicates/delivery blockers.

No new emulator executable was built or launched. The unchanged accepted
candidate remains `Temp/opencode/retail-plus-service-fix3/kyty_emulator.exe`,
SHA256 `16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd`.
Its launch-build receipt remains
`44d887774e68c3e4f320b38e59ef1b97fa9e256ee8cb135b1d8cfa927078cec6`.
Native LoginMgr, user0x10010029, profile/auth data, media paths and separate
animation `a161466b` are unchanged. PSN analysis/files and all-RNPS staging are
owned separately and were not edited. No live PID37568 access, input, attach,
closure, launcher edit or prepared/live-store mutation occurred.

## Frozen evidence and limits

Source: `Temp/opencode/retail-localhome8/checkpoint1`.

| File | Bytes | SHA256 |
| --- | ---: | --- |
| host.log |310703|`bedcfe186c0294267c0d51c172885b15bc86214f90051c48c74ea0d4646cbe28`|
| guest.log |75269425|`dd484a9d7cea7a56747aa653b72df4e1873d79c4038e21e1b0d4ba79c27e3963`|
| window.png | — |`44711b6746b17e68a13644732e363f1033f8d4119165578e0061299312a9bd88`|

The initial audit verified all five frozen files and all53 copied-store files
against receipt SHA256
`f2b090fcf4802f8c179d308d945adc116fe97e00d135aefd2bb018034f0d0ed4`.
The copy is non-atomic, not a coherent live SQLite/process snapshot. Native
roster has saved user0x10010029; all19 counterpart title mounts and all six
external title mounts exist in the launch receipt. Media-tab behavior was not
inspected. No mount/descriptor count is treated as native installed visibility.

Read-only immutable SQLite inspection finds integrity `ok`, user_version0 and
eight rows in each relevant shared/current-user table. The database SHA256 is
`6842ec3c23311fc98a51be163ed75c2a8bf09ab9cd832a81ca1ea08da2868300`.
Minecraft PPSA17221 has icon installStatus2, content installStatus0, visible1,
category0, `dispLocation=2`, valid concept and `psgm:play?id=PPSA17221`.
The diagnostic managed-reader-shaped join returns all eight identities offline;
that was never evidence of native admission or a delivered native result.

Host2168 records NPXS40071 `runApplication`; host2201–2275 records the
Home→Library props exchange with `space:game`, `signedIn:no`. Host2383–2392
records Library's game-grid zero state and empty searchItems. These are distinct
from any unobserved installed-item query callback/serialization result.

Guest startup evidence:

- 64391–64394: reads the existing app.db-corrupt marker, then logs
  `DbAccessor::Init(): ret=0, ver_check_result=0x100`.
- 64726: corruption report `0x80f50061`, detail100, explicitly naming
  `Daemon::InitClient() error checkCorruption() err`.
- 65082/65103 and178575/178578: icon/concept initialization SQL fails809c0103.
- 64000 separately reports AppInfo corruption;664 later null-connection
  transaction messages and three AppBrowse transaction failures are preserved.
- **472536 onward:** a later app.db open/read path exists. Do not erase this
  counterevidence or equate the first startup rejection with every later reader.

## Actual IDA and retail source pins

IDA9.3/Hex-Rays ran against private databases, not a live process. The original
Core ELF's FE10 type initially caused IDA's loader to refuse it; private
analysis-only loader headers were then normalized. Only ELF e_type changed;
all other bytes were compared equal. Runtime/firmware bytes were never patched.

| Input | Runtime/staged SHA256 | Analysis-header SHA256 |
| --- | --- | --- |
| Retail ShellCore |`f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`|`3b60894581ab08abd281a5b66a1817a1c312268bc413fe2d39fe459e5fa36352`|
| Retained strict-stage UI eboot |`f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`|`6b98d070d1a0557e11dc35dea8cb447a79d904ed39ff609b1d6c4c1cc5585f95`|
| Retail Accessor.Db AOT |`7f7dee85146f2dfcfc0c9d56d47ab4054519bbe618cc5b4639eadde130039280`|`87044b149eef6d884ccc38f8e4e366d9e18d884cead2770cf928117342c6b221`|

Verified native policy:

- UI InitClient+54e650 calls DbAccessor::Init+55abf0 with null creation callback.
  The initializer checks corruption before +55af50 opens the ordinary accessor.
  A reported corruption condition sets flag100 and returns0 without that open;
  **ret=0 does not mean an admitted usable connection**. The native policy was
  not changed. Missing version/schema metadata is a separate check, not proof
  that the current100 came from user_version0.
- Core+fde360 returns80f50061 when DbRecoveryCheckCorruptionReport produces a
  report. Core Init+fd12b0 skips the normal +10022f0 accessor open on this path
  and latches100. No guessed AppInfo/AppBrowse version tuple was written.
- Retail Accessor.Db metadata defines Game space as1<<1, Library display1<<3,
  and experience-switcher display1<<7. Actual AOT Library setters are7ca0/cb30;
  App/Concept BuildWhereClause are44e10/53080. Their Library branches bind mask8
  at456ec/45758 and537e7/53850. GOT19bdb8/19bdb0 decode to the assembly's actual
  `(icon.dispLocation & {0}) = 0` / `<> 0` fragments. EXSW branches bind80h.
  Actual AppBrowseItem::IsInstalled+6090 compares the icon status with2; no
  installation/entitlement value needed changing for this display experiment.

The previous regular-image inspector correctly rejected this retail AOT hash.
Its guard, and the old live-observer executable/RVA guards, remain unchanged.
The new private metadata audit independently pins the retail input.

## Copy-only predicate regression and current input drift

A full-store copy experiment stopped before destination creation when a later
validation found the receipt-pinned frozen file
`sd/priv/cache/urlconfig/urlconfig_cache.json` missing. This task neither removed
nor restored it and did not substitute the live-store cache. Thus the complete
53-file seed is **not currently revalidated**; parent must supply/re-freeze a
coherent input before treating it as a new complete store seed.

Instead, an explicitly isolated two-file DB-family experiment independently
rechecked the unchanged app.db and app.db-corrupt source pins, then copied those
two files to `Temp/opencode/retail-installed-catalog-dbcopy1/mms`.
It changes exactly two cells for native-user table suffix0268501033:

- icon Minecraft `dispLocation`:2→138;
- concept `cid:local:PPSA17221` `dispLocation`:2→138.

All70 schemas, every other row/cell, row counts and user_version are preserved.
The four-byte marker remains exactly `0100e1fe`. No profile/auth files were copied.
Copied app.db SHA256:
`d7814c960e00d85c85187f93e3bf276a4962a8a71cd026ee2f0ce4410b50da7b`.
`catalog-copy-receipt.json` SHA256:
`8b4a9401d600dd13d958b8f7a6e30c0e9c926c71c69cd383e30ddaeed56052a9`.

Replayed **evidenced predicate subsets**, not full dynamic native queries:

| Offline predicate subset | Frozen DB | Corrected copy |
| --- | --- | --- |
| visible + Game + EXSW | Game Library only | Game Library + Minecraft |
| visible + Game + Library + icon installed2 | empty | Minecraft |

**Do not mount this incomplete MMS directory or use it as a store seed.** It is
a controlled input/regression artifact, not a recovered native catalog backend.
The safe production change is in the ordinary copied-store importer, not a new
executable, full-MMS overlay, fabricated native result or marker deletion.

Private evidence, actual IDA reports/specs, predicate queries and copy script are
under `Temp/opencode/retail-installed-catalog1`. The initial complete frozen audit
is `Temp/opencode/retail-installed-catalog-audit1.json`; later input drift does
not retroactively invalidate its earlier verification but prevents reusing it
as a statement of the current full-store state.

Private checkpoint receipt `Temp/opencode/retail-installed-catalog1/checkpoint-receipt.json`
pins current sources, actual IDA inputs/reports, the copy and test results;
SHA256 `99fd606c29c7503da3212bf0955a383aa866dfcdefa449b5910e586a4f25e731`.

## Tests and next actual retail step

- `python -B tools/test_retail_catalog_display_1360.py`:11 pass, exercising the
  actual importer/repair, exact two-cell repair, idempotence, supplied/custom
  flags, nonlocal identities, malformed typed flags and transaction rollback.
  The actual importer integration retains fixture media flags188, custom rows,
  versions and corruption marker. SQLite connections now close deterministically
  after their existing transactions; no implicit file-lock leak remains.
- `python -B tools/test_retail_installed_catalog_1360.py`:12 pass, covering
  read-only/no-repair behavior, correct saved-user roster, missing/wrong tables,
  non-atomic journal rejection and the distinction between rows, startup errors,
  later file reads, individual display predicates and native visibility.
- Real pinned DB-copy replay demonstrates both predicate changes above.

When the parent next has a hardware window and authorization: prepare a **fresh
validated copied store**, run the corrected ordinary descriptor importer for
actual user0x10010029, preserve candidate16d9d8c and normal media settings, and
integrate only separately audited RNPS mounts (all-RNPS staging has its own owner).
Then verify Minecraft's visible tile/installed Library entry and its actual
query delivery. Navigation remains user-controlled. Do not call a successful
offline query or a descriptor mount native visibility, and do not weaken old
observer guards for this candidate.

If native APIs still lack a connection, the next implementation target is the
real AppBrowse/AppInfo **creator/recovery transaction and its original owner**,
not a version-row stamp or marker removal. The recovered admission branches
above provide the starting boundary. No complete recovery producer or installed
query/JS delivery capture is claimed by this checkpoint; playback, launchability,
Media-tab contents and PSN authentication remain unverified.
