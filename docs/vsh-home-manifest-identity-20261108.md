# psn39 manifest identity correction — paused handoff

## Ready correction / evidence

The old reader applied the correct manifest offset to the wrong owner:
`PUIReactContext+30` holds nested **PUIReactContext/Data**, not a manifest.
The corrected chain is `context+68 -> AppData+10 -> weak PUIReactApp -> app+30
-> ReactApplicationManifest+30 -> ApplicationName`. Exact type checks remain;
app+38/context and app+10/AppData backlinks must validate before reading the
manifest. Only the actual typed string `rnps-home` passes. User/UID requirements
remain. No subclass relaxation, preinit-name fallback or fabricated identity.

Pinned embedded PE metadata establishes:

| TypeDef | Evidence |
| --- | --- |
| `02000448` PUIReactContext | `_data` has nested Data type; AppData backing field has AppData type; no manifest field |
| `0200044b` Data | Empty namespace, nested under PUIReactContext, extends System.Object |
| `02000429` AppData | `_app` is System.WeakReference |
| `0200042a` PUIReactApp | `_appManifest` is ReactApplicationManifest; `_appData` and `_reactContext` have the expected types |
| `0200000d` ReactApplicationManifest | Extends System.Object; ApplicationName backing field is String |

Pinned native RVA anchors: context Data loads `92374/92394` (+30),
context.get_AppData `92804` (+68), AppData weak-app load `8d4f0` (+10),
app context/AppData loads `8e8b4/8e92e` (+38/+10), app.ApplicationName
`8f180/8f189` (+30 then+30), independent app manifest load `8f384`, manifest
name getter `1154`, UID `8d9d4` (+98), initialized/user `92514/9251d` (+88/+8c).
New live-image comparison windows cover the corrected accessor chain.

The artifact's `other` cannot retrospectively establish an exact runtime type.
The declared Data type and native use independently prove the owner error;
Data is not a manifest subclass. No additional live diagnostic is needed to
justify this correction. Any future failure at the corrected hop remains unknown.

## Hashes and parent lineage

- Read-only trace: `${USERPROFILE}/AppData/Local/Temp/opencode/home-gates-psn39.jsonl`
  SHA256 `a70296f7cd1ae1ea994c9a6b8cb2cb1c410d6bde60d732f3e97bebef91d770e6`.
- Original PUI SPRX SHA256
  `a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914`.
- Embedded PE, extracted in memory,1,705,984 bytes, SHA256
  `faad163b988ab98b1f4139e4dbce08f289b314d0ad4cd16ed6728075c9059417`.
- Other firmware/Home hashes and exact original/stage prerequisites remain in
  `docs/vsh-home-gate-tracer-20261108.md` and `home_gate_trace_1360.IMAGES`.
- Parent integrated `523d055c/5f2de22d` as `b2cfdf71/07c1a2e2`; validation
  checkpoint `a005700b`; latest parent psn39/survival checkpoint **`68a84f8d`**.

Verified JSONL:790 hits (Resolve709, Execute14, Dispatch49, Reject18),61 errors
all `identity.manifest/object_type/other` (49 Dispatch,12 Execute), two intended
caller exclusions, no associations. End241.742479s has cleanup_verified true,
347 verified threads, zero residual repairs and released holds. Parent reports
post-detach survival/rendering, successful user-controlled login and Home
promotion/mount. Those live/log observations are parent-supplied. Lifecycle-v2
is unchanged; only the capture provenance diagnostic_version changes2 ->3.

## Completed checks

- **30 offline tests passed**, including wrong-owner decoys, absent/collected app,
  invalid backlinks, unsupported manifests and normal game/media correlation.
- **Pinned embedded-metadata/AOT audit +4 real-layout negative controls passed**.
  The independent fixture uses metadata field types and disassembled memory
  operands, not the reader's constants or ordinary identity fixture. Negatives:
  old-owner `object_type/other` reproduction; context-manifest decoy cannot rescue
  null app manifest; Data with Home string is rejected; bad backlink is rejected.
- **Pinned ABI audit +14 provenance negatives passed**.
- Offline collector preflight passed with **process_access:false**.
- `git diff --check` passed. No live access or parent/profile/database writes.

Prerequisites: Python with `capstone` and `dnfile`, pinned original/Home inputs
and byte-identical stage. Exact commands completed:

```powershell
python tools/test_home_gate_trace_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn37/_kyty.txt
python tools/capture_home_gates_1360.py --original ${USERPROFILE}/Downloads/13.60/system_ex_b/common_ex/lib --stage ${USERPROFILE}/Desktop/kytyps5/_Runs/psn23/stage/common --home-bundle ${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps/NPXS40002/application.ps.bundle --run-log ${USERPROFILE}/Desktop/kytyps5/_Runs/psn37/_kyty.txt
```

## Deferred until user returns

No new lifecycle stress run: teardown code was not changed. No live validation
of the corrected identity chain or remount capture was performed. Game/media
results and the actual Home gate remain unproven. User navigation is unavailable;
the work is paused after committing this offline-verified correction.

Later, parent uses the exact **Parent command** in
`docs/vsh-home-gate-tracer-20261108.md` with newly verified process identity,
current placement log, and fresh output. Confirm diagnostic_version3 and
lifecycle_version2, wait for `armed`, then coordinate user-controlled remount.
Do not reuse historical PID9200 without current verification. Desired evidence:
condition/GetItems entry and matched native settlements with game/media counts
for the same generation/user/module. Missing pre-attach requests cannot establish
pending promises; native counts do not prove JS consumption. No capture or
navigation command has been executed on the parent's active process here.

## Parent resume validation

User resumed and requested two Astra Medium agents: Home inventory/ownership and
Apple TV/Netflix. Parent integrated this correction as `ebdd1292` and the saved
Core AppInfo evidence as `b4306c2e`. Parent reran all30 offline tests, the pinned
metadata/AOT audit with4 real-layout negatives, ABI audit with14 provenance
negatives, and offline collector preflight against psn39 placement/staged inputs.
All passed; preflight reported `process_access:false`. No lifecycle code changed.

Process inventory found no emulator running. A new user-coordinated Home capture
is next; corrected live identity correlation remains unverified at this checkpoint.
The Home agent owns native inventory/AppInfo/preparation work, the separate media
agent owns playback investigation, and parent owns tracer/live navigation setup.
