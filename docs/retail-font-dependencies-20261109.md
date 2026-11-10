# PSM font dependency closure for the mixed comparison

**Correction after retail-font2:** this checkpoint covered the filename set,
but omitted directory kind1 (`/common/font2`). Its runtime-path completeness
claim was incorrect. See `retail-font-directory-kinds-20261109.md` for the
verified alias membership, missing mount and corrected preparer.

## Runtime evidence

Parent's `retail-font1` verified SST-Bold opening successfully (89308 bytes),
then missing `SIE-RDC-Pr5N-B-JPN.otf`, followed by the same PSM null cleanup
fault and Mono fatal trap. Immutable logs were independently rehashed:

- guest: `de7b098134edfcd397c5152ec73a9ff26a519cfbf5929c28f6f90d4b7a3c3faa`
- host: `3acd52ae7eb75dbc5ce864baa809f606143741a93403883f6c7c8991f5fa2c33`

The initial preparer's SST-prefix filter was incomplete. The revised preparer
selects every exact font filename in non-executable load segments of the pinned
retail PSM, plus names constructed by its alias initializer. Selection is no
longer limited by font family or to filenames already present in the working
directory: missing required files cause failure before creating the stage.

## Actual retail dependency audit

Original PSM hash remains
`0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6`.
Actual IDA of initializer `+0x203760..+0x204c99` shows eight style slots per
alias. It constructs base SST filenames using immediate stores and copies the
other family names from data constants. The audited dependency set is:

- SST: Light, Medium, Roman, Bold and their four italic styles (8).
- SIE-RDC-Pr5N: M-JPN and B-JPN (2).
- SCEPS4Yoongd: Light, Medium, Bold (3).
- SSTArabic, SSTThai, SSTVietnamese: Light, Medium, Roman, Bold (12).
- SSTAribStdB24-Regular.ttf (1).
- DFHEI5-SONY2022.ttf (1).
- PS4Icon.ttf (1, separate directory).

The original retail SIE-EmojiStyle-PS5.ttf is retained as well: 29 total files.
The common-font directory has 28 files, including both retail archive TTFs.
No unresolved reference remains in this audited built-in set. This is not a
claim that arbitrary future app-specified font filenames are covered.

PS4Icon is constructed inline, so a data-string-only scan would miss it.
Its alias starts at +0x445890 and directory kind2 is stored at +0x445960.
The font creation routine +0x206e50 selects `/common_ex/font2/` for kind2;
this literal is at +0x2ae667. The exact original retail file exists at
`system_ex_b/common_ex/font2/PS4Icon.ttf`, SHA256
`ef875d3f99830bec5914874699ecf430f0a6fb40fae163b9168b3b644cbe1b20`.
It was copied under its real name in the matching separate directory.

External IDA evidence: `automounter-ida/retail-font-alias-init.txt` and
`retail-font-create.txt`. File offsets were translated using ELF load segments;
the code segment has file offset 0x4000 for RVA zero.

## New stage and provenance

Stage root:
`${USERPROFILE}/AppData/Local/Temp/opencode/retail-mixed-fonts2`

Receipt: `receipt.json`, SHA256
`2a111637fd05e561c01915eae957825db2ba0fcc359031b582c9f52b8fbc2995`.

The receipt records every exact source, hash, byte size, name/style metadata,
scalable-font header, outline tables, weight, italic angle, glyph/cmap counts,
selection reason and provenance. It also inventories data references,
constructed names, zero missing references and unused working resources.
Working-source files must have the expected PostScript identity; SIE filenames
use their actual SIERodinCattleya identities. No unrelated font was renamed.

The 26 working-source fonts are explicitly **mixed comparison inputs, not
authenticated retail-derived assets**. The two retail archive TTFs and retail
icon retain their recorded original-source provenance. Unreferenced legacy
Japanese/Korean/typewriter/decorative working fonts were not copied.

All 29 source/overlay hashes, sizes and metadata were verified. Source files
and staged files were rechecked after fixture execution. The earlier 23-font
stage and receipt were separately verified unchanged. Existing low-created-
timestamp fontTools warnings do not modify the font bytes.

## Production-object fixture verification

`tests/RetailFontMountTests.cpp` now accepts declared common/icon counts.
The isolated recipe `automounter-ida/validate_mixed_fonts2.py` compiled it and
linked existing Bond production objects, verifying 336 receipt-pinned link
inputs before and after. Shared base `5ee6a153` was read-only; linker jobs=2.
No emulator rebuild occurred.

Passed:

- All 28 common fonts through `/common/font`, `/kyty/common/font`,
  `/preinst/common/font` and the observed hex-sandbox alias (112 opens).
- Retail icon through `/common_ex/font2` and its sandbox alias (2 opens).
- Complete byte comparisons of each resolved host backing, real production
  KernelOpen/KernelClose, EROFS for write opens, retained retail TTFs,
  same-point replacement, longest-child selection, unrelated-parent retention,
  old-empty-stage and unknown-file ENOENT controls.
- Wrong declared count rejected; missing exact dependencies rejected before
  creating an output stage, including the Japanese filename that caused font1.

Read boundary: as in the prior fixture, reads use host streams of the production
resolver's selected backing after KernelOpen. KernelRead requires renderer
invalidation resources and is not claimed by this offline fixture.

External results: `retail-mixed-fonts2-validation/verification.json`,
`result.log`, `negative-controls.json`. Fixture exit code 0; 114 opens passed.

## Exact parent mount recipe

Retain existing `/preinst`, `/common` and `/common_ex` parent mounts. In
particular `/common` must resolve to the original preinst/common directory,
and `/common_ex` to the existing retail common_ex directory for alias traversal.
After existing mounts, register these global read-only mappings:

| Guest path | Host directory relative to new stage root |
| --- | --- |
| `/common/font` | `preinst/common/font` |
| `/kyty/common/font` | `preinst/common/font` |
| `/preinst/common/font` | `preinst/common/font` |
| `/common_ex/font2` | `common_ex/font2` |

Do not replace entire parent trees with the sparse overlay. Sandbox aliases
select these mappings without an explicit hash-prefixed mount.

Stable candidate remains
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.
No guest process, store, controller or parent launcher was accessed. Parent
launch verification of font creation, rendering and usable language selection
is still pending; this checkpoint verifies dependency staging and access.
