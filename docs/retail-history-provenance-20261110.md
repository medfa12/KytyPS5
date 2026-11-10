# Retail historical-content provenance checkpoint

Audited retail commit `67cd2f6e2d04ff106b61c9cf1c896e68e5afdf19`.
The reproducible metadata-only inventory command is:

```powershell
python -B tools/audit_content_history.py --ref 67cd2f6e src/libs/agcRegisterDefaults.inc src/libs/graphicsRegisterDefaults.inc assets docs/screenshots tests/shaderCfgTests.cpp tests/ShaderRecompilerComputeTests.cpp
```

The scanner visits both sides of changes and every merge parent, includes deleted
versions, and independently verifies each Git blob ID against its payload.
It emits paths, object IDs, lengths and SHA-256 values, not historical payloads.
This is a selected-path inventory, not a full-repository clearance.

## Verified inventory

| Selected group | Unique blobs | Historical-only blobs | Unique bytes |
| --- | ---: | ---: | ---: |
| Assets | 56 | 26 | 377,204 |
| Screenshots | 31 | 8 | 36,239,805 |
| Register defaults | 4 | 3 | 852,070 |
| Two shader test sources | 1,101 | 1,099 | 1,497,239,413 |
| Total | 1,192 | 1,136 | 1,534,708,492 |

Counts deduplicate blob IDs, not filenames. Historical-only means the blob is
absent from the selected tip paths, not unreachable from the branch. The JSON
receipt is preserved externally at
`Temp/opencode/retail-content-check-20261110/history-inventory.json`.

All 18 externalized shader inputs remain represented in reachable old sources.
Their complete DWORD sequences were matched against the retained external
fixtures in these inventoried blobs (the stencil input is shared):

- `c0d64d2d0a51aaf5a809cef6932e845d779575c4`, compute tests at `e6c8aa1d`:
  nine fixture sequences, including runtime-buffer records.
- `df7ff342dab243941f05462778c9b1015ce1398b`, CFG tests at `7b985971`:
  ten fixture sequences.

Deleting literals at the branch tip therefore did not remove those captures
from the branch's reachable history. A normal branch push can still publish
them; another tip-only cleanup commit cannot resolve this finding.

## Register-table provenance

`git log --follow` identifies the initial table at `147188b1` (initial blob
`709f73af856abba84f8433f882d4fb4c815fcb0d`), formatting at `601c82a0`, an
AGC rename at `69a77a01`, and version-13 additions at `6f83327b`.
The version-13 commit changes the table by 88 lines plus the dispatch integration;
its message is only `add agc ver 13`. These commit records and the table comments
do not establish an independent source or a generation procedure for the values.
The repository GPLv2 text does not independently answer that origin question.
No table was removed: it is used by production AGC dispatch, and deleting it
without a verified replacement would regress functionality.

## Asset provenance

The 316,844-byte trophy sound was introduced by `412aba28`, `Add trophy
notification support`. Its commit message contains a co-author credit but no
audio-source attribution. Repository searches found no asset-specific source
or licensing explanation for the sound. This is unresolved origin, not proof
that the file was copied from firmware.

Screenshots have distinct histories. Game images include additions at
`96db51ea` and `9456ced4`; ShellUI images accompany local diagnostic milestones
such as `c05cc194`, `7cf0fc56`, `7694e9c1` and `69d365d0`. They should not all be
described as unchanged upstream game screenshots. Introducing commits identify
their recording context but do not establish permission for every rendered
third-party asset. Cursor/hidden-trophy image origins also need an explicit
source record; the historical inventory includes these files.

## Decisions needed before publication

1. Supply the table's original source/generator and asset attribution evidence,
   or authorize replacement/omission of assets whose origin cannot be established.
   An independently generated replacement sound is feasible; the production
   register table needs a separately validated replacement or external input.
2. Choose the publication history strategy after tip cleanup: a new clean-history
   release branch, or a reviewed filtered-history branch. Preserve existing local
   refs/backups in either case. Existing published refs may require separate
   treatment; a new branch alone does not erase remote history.

No history was rewritten and no refs or assets were deleted in this checkpoint.
GPU acceptance remains hardware-blocked; fresh-user firmware preparation/boot
remains unverified. No GitHub push occurred.
