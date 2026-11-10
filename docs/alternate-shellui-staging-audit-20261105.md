# Alternate ShellUI staging audit — 2026-11-05

Parent read-only audit while Astra Medium checks authenticated alternate image
provenance. No alternate boot has been attempted.

The current launch script fixes firmware and managed-stage paths to the active
tree. The Python managed preparer already accepts explicit `--fw-root`, `--stage`
and `--rnps-source`; first-boot preparation independently checks the Legacy hash.

An isolated `prepare_launcher` attempt for the alternate tree correctly refused
the active render-patch anchor at RVA0x126ab, before writing a launcher image.
Alternate ShellUI's startup contains the corresponding 3840x2160 `movabs` at
0x122fb, with store `[rbp-0x288]` at0x1231d and config address `[rbp-0x2d8]`
passed at0x1241d to call0xd23750. These offsets are candidates for a separate
audited patch profile, not a verified port of the active image's transform.

Legacy source hashes differ:

* Active: `070de0f8845090812b0490c1cd0ee8fde7bd67cf607fa63b3b0260f1ef6352ae`
* Alternate: `fb6cc305e69345aa08d2d5e31e8f2c75ee817a2ac357de19d04b3df94b1f6837`

Alternate Legacy code around file offset0x2a4ec has a similar SYSTEM_initialize
GetInt/conditional SetInt sequence, but following control flow differs. The
active first-boot patch must not be applied just by shifting its offset.

Original NPXS40141 RNPS containers compared byte-for-byte by SHA256 are identical
between the two trees:

* `host.ps.bundle.hbc`: `ce47ad765106566203ef9d801f0bd67aa5f58f8ab3617df8c4851b875bfee04f`
* `base_dll.ps.bundle.hbc`: `acb39d6aa9d9ae9699cf8dbb5da67492e7107b8d990c8b58c23fb7001855017b`

That equivalence concerns those original containers only, not every runtime
dependency or the provenance of all existing decrypted overlays. A complete
matching boot needs an isolated stage and validated per-image transformations.

## Independently validated alternate launcher profile

Resolved the alternate call's PLT0xd23750 through its GOT relocation to import
`lWlBrUu77Kg#B5#Bk`, the same PSM initialization NID as the active launcher.
The startup config pointer is `[rbp-0x2d8]`; the dimension store is at
`[rbp-0x288]`, hence config+0x50. This establishes the alternate site's purpose
independently of an offset shift.

`prepare_shellui_managed_1360.py --launcher-profile alternate` now accepts only
the reviewed original alternate UI hash and checks all three startup anchors.
It changes only the eight-byte 3840x2160 immediate to 1920x1080, and checks the
complete transformed image against a second pin:

* Source: `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`
* Output: `b4ce830af247ae6611af81dece472710fa728a5c6855280d609168b62d556f4c`

The active profile now also pins its complete source/output while producing
the same existing output. Default profile remains active. Alternate preparation
requires an isolated empty or source-labelled stage, rejecting an existing
unlabelled stage before writes. `native/launcher-transform.json` records the
exact byte substitution; it is audit evidence, not a trusted credential receipt
or a claim of native mailbox0x16 acceptance.

Four retained-input tests passed in 1.332 seconds: complete transforms and exact
inverse reconstruction for both trees, cross-profile/tampered/already-patched
input rejection, preservation of a hardlinked original, and populated-stage
rejection before writes.

Prepared `_Runs/alt1360-stage` from the alternate tree: 180 common managed
assemblies, seven application assemblies, original AOT companions and native
dependencies. RNPS overlay source was an absent isolated directory, so no
unverified overlay was imported. No emulator was launched. Recursive native
dependency closure and RNPS payload provenance remain to be checked.

Alternate Legacy's AOT metadata maps
`Sce.Vsh.ShellUI.AppMain.BootManager.ResetInitialSetupPhase` to RVA0x264d0,
file0x2a4d0. The embedded PE is reference-only; ILSpy cannot establish its
implementation. Native calls at0x264f6 and0x2650a resemble registry GetInt/SetInt,
but their AOT PLT metadata and following changed control flow still need auditing
before any alternate first-boot transform. Normal Home staging uses original
Legacy bytes.

## Matching-image launch preparation and dependency inventory

Runner now accepts explicit `FirmwareRoot`, `ManagedStage`, `LauncherProfile`
and `RnpsSource`. `PrepareOnly` stages and saves argv before starting a process
or changing host environment flags. Default active launch paths are preserved.
Alternate Python staging has no implicit workspace RNPS overlay source.

The complete preparation run passed using isolated `_Runs/a1360sd`,
`_Runs/alt1360-stage`, `_Runs/alt1360-rnps-verified` and development executable
`_Build/vsh-integrated/kyty_emulator.exe`. Result:
`_Runs/a1360prep/prepared-launch.json`. Independently checked all 25 boot image
paths exist; none refers to active firmware or `_ShellUIManaged`; Home mode and
RNPS replay are selected. Longest compositor endpoint is 82 bytes, below the
108-byte AF_UNIX limit. Offline descriptor provisioning produced two app rows.
This was preparation only; no runtime acceptance is inferred.

Read-only inventory examined 564 alternate ELF paths and traversed 158 extracted
`.sprx` candidates corresponding to `.prx` DT_NEEDED names. Seven root-imported
native library names lack a provider in that inventory: `libSceAppChecker`,
`libSceNpIga`, `libScePerf`, `libSceRazorCpu_debug`, `libSceVshctl`,
`libhermesRemoteInspector`, `libswreset`. The last three are also absent in the
active inventory. This is file availability, not live symbol resolution; the
emulator uses HLE and selective native providers rather than loading this entire
graph. Therefore full native dependency closure is not claimed. PSM, PSMUtil,
mono-btls and LibcInternal are available from matching-tree paths; a missing
separate `libc.sprx` is not an absent explicit startup image.

## RNPS replay recheck

Rechecked all four NPXS40141 containers (JS and HBC) byte-for-byte across both
trees, not only the two HBCs. Existing staged payloads exactly equal their
content-keyed `_TempData/rnps_replay` copies and match each container's plainSize.
Both HBCs have v89 headers, exact file lengths and valid trailing SHA1; their
sourceHash fields equal the corresponding JS SHA1. Evidence is retained in
`_Runs/alternate-stage-audit/rnps-replay-binding.json`; four payload copies are
isolated under `_Runs/alt1360-rnps-verified/NPXS40141` and staged into the alternate
tree. Their original acquisition is documented in
`docs/rnps-research/provenance.md`. This remains recorded-plaintext replay;
neither decryption nor cryptographic container-to-plaintext verification was
performed by this audit.

## Completed alternate first-boot control-flow audit

Resolved the actual AOT metadata rather than inferring callees from offsets.
Alternate Legacy's MonoAotFileInfo (version171, size552) is at RVA0x648030;
relocation at info+0x58 supplies blob0x3ea490 and info+0xa0 supplies the ordered
76-image table0x3e8ac8. PLT stubs begin `mov eax, blob_offset`; patch kind3
encodes a MethodDef image/row reference. The selected entries decode to:

| PLT RVA | Blob offset | Assembly / MethodDef row | Method |
|---|---|---|---|
| 0x2d7e6c | 0x328cb | SysBridge / 331 | Kernel.IsCronos() -> int |
| 0x2d8dbc | 0x32bb5 | Lx / 157 | Registry.GetInt(uint,int) -> int |
| 0x2d8da0 | 0x32bb0 | Lx / 158 | Registry.SetInt(uint,int) -> void |
| 0x2da2f4 | 0x330b0 | ShellUI.AppSystem / 1479 | Utility.ManufacturingMode -> bool |
| 0x2da310 | 0x330b5 | Lx / 156 | Registry.GetInt(uint) -> int |

`Sce.Vsh.Registry.dll` constants independently identify 0x02040000 as
`SCE_REGMGR_ENT_KEY_SYSTEM_initialize` and 0x02200000 as
`SCE_REGMGR_ENT_KEY_SYSTEM_initialize_phase`.

Complete ResetInitialSetupPhase disassembly (0x264d0–0x265d7) establishes:

1. After static initialization, `Kernel.IsCronos() > 0` selects the automatic
   initialize write; both initialized/uninitialized paths converge there.
2. If `Registry.GetInt(0x02040000,1) != 1`, the call at0x2650a writes1.
3. Control continues at0x2650f to ManufacturingMode and subsequent initialize/
   initialize_phase decisions. Unlike the active sequence, there is no adjacent
   SetBootEvent call to preserve or remove at this site.

The alternate `prepare_shellui_first_boot_1360.py --profile alternate` transform
now replaces only the five-byte call at file0x2a50a with NOPs. It checks the
original full Legacy hash, the independently decoded surrounding sequence and
the complete output hash
`edf75dd3e49cbeb740715c4653c78b2c9efc97be0b6f050158eebbaa757751c3`.
All later branch instructions and guest registry writes elsewhere remain intact.
`firstboot-transform.json` records the substitution. This experiment does not
force service readiness or make setup-complete claims.

Six retained-input checks passed in 1.345 seconds, including both profiles'
whole-image transforms, cross-profile/tamper rejection and hardlink preservation.
Alternate OOBE preparation also completed without launching a process:
`_Runs/a1360oobe/prepared-launch.json` uses separate `_Runs/a1360ob` data and
`_Runs/a1360oobe/firstboot-common`.

An AssemblyRef inventory of all187 staged assemblies found one absent reference,
`System.ServiceModel -> System.IdentityModel`, also absent from the active stage.
The two Legacy references not present in common (`Sl2Manage` and
`VideoRecordingWrapper`) are correctly staged under Application. Reference
availability alone does not establish dynamic initialization success. Inventory:
`_Runs/alternate-stage-audit/managed-reference-closure.json`.

## Parent review of runtime admission handoff

Read the production loader/identity changes and the AppDb adapter/hook
implementation while the integrated build and final tests were pending.
Independently repeated full original Core/UI signature, segment and exact-ELF
verification and compared both local136-byte records byte-for-byte. The UI
record digest is `b35057cfe2d2d844f287325c438b39950ed6555f686592af546eb12fb950e0aa`.

An offline relocation scan found only supported R_X86_64 types1/6/7/8 and
eight-byte destinations in non-executable PT_LOAD memory:

| Image | RELATIVE | 64 | GLOB_DAT | JUMP_SLOT |
|---|---:|---:|---:|---:|
| Core | 42441 | 1430 | 103 | 2502 |
| reviewed1080p UI | 38633 | 2059 | 112 | 2849 |

Evidence: `_Runs/alternate-stage-audit/admitted-relocation-shape.json`. This
checks relocation shape, not successful production import binding. The worker's
mapping-only loader test does not call Relocate; actual runtime boot will cover
the complete sequence.

Also checked alternate Legacy at the older active-only Farsight patch's
RVA0x36235: bytes `415f5dc3c745c000` do not match its guard
`488b07e85374fdff`, so that existing patch does not apply to this input.

Live handoff requires host `KYTY_APPDB_CORE_AUTH_RECORD` and
`KYTY_APPDB_UI_AUTH_RECORD`, plus `--vsh-session` (`-VshRuntime` in the runner).
The original prepared JSON predates this requirement and lacks that switch.
Final runtime validation and an actual alternate boot are still pending at this
review checkpoint.

## Reused first-boot destination check

Found and fixed a concrete reuse hazard: `link_or_copy` preserves an existing
companion even if it came from another firmware. Before writing anything,
first-boot staging now checks every retained companion against its source
(same-file hardlinks are accepted; other copies must be byte-identical).
Different or unknown companions reject the stage, leaving its existing files
untouched. Legacy itself is replaced by the already pinned transform.

Seven retained-input tests passed in 1.815 seconds, including mixed-companion
rejection before any patch write. Re-preparing the actual matching alternate
`_Runs/a1360oobe/firstboot-common` also passed the new checks.
