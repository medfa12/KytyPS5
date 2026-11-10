# Retail PSM font directory kinds: correction and verified mount

## Failure and corrected scope

Parent retail-font2 requested
`/9381b383471c982d252a52143c66c233a732263f35a4dd1aa7a3d3a8f891649/common/font2/DFHEI5-SONY2022.ttf`.
The launch provided `/system/common/font2` but not `/common/font2`. ENOENT
preceded the same PSM `+0x22fc36` cleanup null dereference and Mono fatal trap.
Immutable log hashes were independently verified:

- guest: `ebb3c42c3cc532e52b1d3d9bdc2fa43e7c70b589cf59632f1eaf2fba71962849`
- host: `24600d70c95647207da65311be1385a49d9c86899585062f067967be2ab50157`

The prior claim of complete dependencies was too broad: filename coverage did
not establish runtime path coverage. The initial DFHEI copy was from the retail
ZIP, not a working-tree font, and equals the authentic original file exactly.
Its location in the kind0 overlay still could not satisfy the kind1 request.

## Actual retail IDA alias-to-path contract

Pinned PSM SHA256 remains
`0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6`.
Existing actual-retail IDA reports were reviewed in full for the directory
fields: `automounter-ida/retail-font-alias-init.txt` (+0x203760..+0x204c99)
and `retail-font-create.txt` (+0x206e50..+0x207647).

Each built-in alias record is 0xf0 bytes, with eight style filename slots and
directory kind at +0xd0. The consumer reads exactly this field (`v4[52]` as
32-bit words), then builds the sandbox path:

| Table RVA | Family/filename membership | Kind | Guest directory |
| --- | --- | --- | --- |
| 0x445200 | SST, eight base styles | 0 | `/common/font` |
| 0x4452f0 | SIE-RDC-Pr5N M/B-JPN | 0 | `/common/font` |
| 0x4453e0 | SSTArabic, four weights | 0 | `/common/font` |
| 0x4454d0 | DFHEI5-SONY2022.ttf, all eight style slots | 1 | `/common/font2` |
| 0x4455c0 | SCEPS4Yoongd, three weights | 0 | `/common/font` |
| 0x4456b0 | SSTThai, four weights | 0 | `/common/font` |
| 0x4457a0 | SSTVietnamese, four weights | 0 | `/common/font` |
| 0x445890 | PS4Icon.ttf, all eight style slots | 2 | `/common_ex/font2` |
| 0x445980 | SSTAribStdB24-Regular.ttf | 0 | `/common/font` |

DFHEI's +0x4455a0 directory field is copied from SIMD constant +0x2c6e90,
whose first dword is 1. The other SIMD kind sources at +0x2c6970, +0x2c6490,
+0x2c6f00, +0x2c6c10 and +0x2c6520 begin with zero. PS4Icon's +0x445960
field is explicitly assigned 2; ARIB's +0x445a50 is zeroed. This verifies alias
membership, not an inference from font filenames. The consumer branches on
2 and 1, otherwise using kind0; `/common/font2/` is explicitly constructed in
the kind1 branch. All three path forms include the sandbox prefix.

`alias_contract` in the preparer records these nine audited groups and exact
filenames, verifies the pinned source and relevant kind constants, and emits
the contract into future receipts. SIE-EmojiStyle-PS5.ttf is retained as the
original font2 sibling, not claimed as one of these built-in alias slots.

## Minimal parent correction: reuse current stage

Add this global read-only mount after the existing parent mounts:

`/common/font2` ->
`${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/system_b/common/font2`

Retain `/common` (required by sandbox alias parent traversal), `/preinst`,
`/common_ex`, all four existing mixed-fonts2 font/icon mappings, and the
existing `/system/common/font2`. No hash-prefixed mount or new launch stage is
needed. The full native path set is now kind0 `/common/font`, kind1
`/common/font2`, and kind2 `/common_ex/font2`.

Direct original resource hashes:

- DFHEI5-SONY2022.ttf, 8393676 bytes:
  `3f6631e092dfaf223a25cd3ac2f92902b4d6b7caaea04740c426088b2629edaf`
- SIE-EmojiStyle-PS5.ttf, 6701216 bytes:
  `94240cfcab350e245cb03a343948fc0d25eb7370e7d69f5ab45ff463c602cc03`

Both original files equal the corresponding members of the pinned retail
font.zip byte-for-byte. The working font directory is not used for kind1.

Supplement receipt:
`Temp/opencode/retail-font-kinds-validation/font-path-receipt.json`

SHA256: `b5e55bb3c716756892942220ac83e85b6c7983b4d9136f7df956d3f5880f341b`.
It includes the original-source paths/hashes/scalable metadata, audited aliases,
additional mount and existing mixed-stage receipt pin. The existing stage and
receipt remain unchanged; the comparison as a whole remains explicitly mixed.

## Verified fixtures and preparer correction

The production-object fixture now first mounts only `/system/common/font2`.
It verifies ENOENT for both `/common/font2/DFHEI5-SONY2022.ttf` and the exact
observed 9381... sandbox path. After adding `/common/font2`, both original
TTFs open through direct, sandbox and retained system paths. Kind0 and kind2
checks remain in place. Real KernelOpen/KernelClose, complete byte comparisons
through resolved backing streams, EROFS write checks, missing-file errors,
same-point replacement and longest-child behavior all passed (120 opens).
As before, KernelRead/renderer execution is not claimed by this offline test.

The fixture was compiled in `retail-font-kinds-validation`, linked against the
unchanged Bond candidate's objects with at most two linker jobs; 336 link inputs
were hash-verified before and after. Shared base `5ee6a153` was read-only.
All previous staged files, receipts and original sources were verified unchanged.

The revised preparer requires `--retail-font2`, verifies the original files
against retail ZIP members, and emits a direct read-only `/common/font2` mount.
Future preparations no longer copy those TTFs into the kind0 font directory.
An isolated preparer smoke run produced 26 kind0 fonts and one kind2 icon,
plus two direct original kind1 resources. The same count-parameterized fixture
passed this corrected layout too (112 opens). This smoke output is only a
test artifact under `retail-font-kinds-validation/preparer-check`; the parent
launch recipe above reuses mixed-fonts2.

Results: `verification.json`, `result.log`, `preparer-check-result.log` in that
external validation directory. No emulator rebuild, guest launch, process,
controller, store or parent launcher access occurred. Stable emulator remains
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.
Usable language rendering still needs the parent run; audited built-in path
coverage is not proof of arbitrary future app-selected font coverage.
