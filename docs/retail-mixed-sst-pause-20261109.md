# Mixed SST comparison: paused checkpoint

User requested an immediate pause before resolver-fixture compilation/testing.

## Completed and verified

`tools/prepare_mixed_sst_fonts.py` successfully created an isolated overlay:

`${USERPROFILE}/AppData/Local/Temp/opencode/retail-mixed-sst1/preinst/common/font`

Receipt with every source path, source/output SHA256, size, selection reason,
font names and scalable-font metadata:

`${USERPROFILE}/AppData/Local/Temp/opencode/retail-mixed-sst1/receipt.json`

There are 23 files: eight base SST styles, thirteen localized SST resources
selected by exact filename references in hash-pinned retail PSM, and the two
original retail font.zip TTFs. SST sources are the user's working directory
`${USERPROFILE}/Downloads/13.60/preinst/common/font`. No unrelated font was
renamed to SST. Files were copied byte-for-byte, rechecked after writing, and
working sources and the retail archive rechecked for changes during staging.
All fonts passed scalable SFNT header/outline-table/name parsing; Bold and
BoldItalic also passed weight700/upright-versus-italic metadata checks.
fontTools emitted four low-created-timestamp warnings; no font bytes were
rewritten or normalized.

Classification: **mixed font comparison, NOT authenticated retail-derived**.
The previous crash report's exclusion of cross-source copying is superseded
for this explicitly authorized mixed comparison. Strict retail provenance
remains a separate, unresolved question.

## Proposed parent mounts (not runtime tested)

All three read-only mappings target the overlay font directory above:

- `/common/font` replaces the existing font mapping.
- `/kyty/common/font` replaces the existing font mapping.
- `/preinst/common/font` is a more-specific child of the existing `/preinst`.

Retain the original `/preinst` root mapping. Source inspection confirms
same-point/same-scope mount replacement and longest-visible-prefix selection.
Hex sandbox prefixes alias to `/common`, so the observed
`/3a058c110031659f3d186ba8976aaa804f5ff4e63ece5038e7f55e3383f852e/common/font`
should select `/common/font`. These conclusions have not yet been exercised
with this overlay against the production resolver.

`tests/RetailFontMountTests.cpp` was written to verify old-mapping control,
replacement, longest child, sandbox alias, retained TTFs, read-only policy and
missing-file preservation. **It has not been compiled or run.** No new emulator
build or candidate was produced. Existing candidate remains `87cdf596…`.

## Parent report and resume boundary

Parent reports checkpoint `8299f320`: uncaptured retail-bond2 reproduced
SST-Bold ENOENT -> PSM `0x95022fc36` -> Mono `0x96030a4f0` without a debugger.
This was reported by the parent, not independently reanalyzed in this paused
task. It establishes that the observer is not necessary for that failure.

On explicit resume: compile/run the resolver fixture using isolated outputs
and existing candidate production objects, then hand off verified mounts for
parent-controlled testing. Font-open success, font rendering and usable
language navigation with this overlay remain unverified. No guest process,
controller or runtime store was accessed during font preparation.

## Resumed verification: ready for parent launch

The fixture now compiles and passes against the existing Bond candidate's
production objects. Outputs are isolated in
`Temp/opencode/retail-mixed-sst-validation`; `verification.json` records hashes,
and `result.log` records the successful test. The external repeatable recipe is
`Temp/opencode/automounter-ida/validate_mixed_fonts.py`. Linking used two jobs;
336 candidate-receipt link inputs were hash-verified before and after. Shared
build source base remains `5ee6a153`; shared outputs were read-only.

All 23 overlay files exactly match receipt hashes, sizes and their original
working files or retail ZIP members. Reparsed scalable-font metadata matches
the receipt. The existing four low-font-timestamp warnings remain informational.
Overlay hashes were checked again after the fixture. The original receipt is
unchanged, SHA256 `46d5505b40e52b8ad1a0cb799cd481915972ce69d79970db116877f3425e03b8`.

The production resolver and KernelOpen/KernelClose passed for all 23 files at
each of the four guest paths (92 successful opens). Complete reads through the
resolved host backings were byte-compared with the staged files. All write
opens returned EROFS. The old empty mapping and an unknown filename returned
ENOENT on open. The fixture also verified same-point replacement, child-over-
parent precedence even with the parent registered later, and preservation of
the unrelated preinst path. Both retail TTFs are included in those checks.

Fixture corrections were limited to missing parent-mount setup and explicit
exception diagnostics. The initial missing-path resolution error was due to
the absent `/common` parent in the fixture. Production KernelRead requires
installed renderer resources, so this offline fixture uses real KernelOpen/
KernelClose and host-stream reads of the production-resolved backing instead.
It does not claim KernelRead, renderer, font creation or guest UI execution.
No production code or preparer changes were needed.

### Exact mount recipe

Keep `/preinst` pointing at the existing `retail-strict1/stage/preinst`.
Retain (or explicitly ensure) `/common` pointing at that same root's `common`
directory: sandbox-alias traversal checks this parent before the font child.
This parent requirement was exercised by the fixture.

After existing mappings, mount the following read-only, all targeting
`${USERPROFILE}/AppData/Local/Temp/opencode/retail-mixed-sst1/preinst/common/font`:

1. `/common/font`
2. `/kyty/common/font`
3. `/preinst/common/font`

Use the same global scope as the existing font mappings. The first two replace
same-point mappings; the third overrides only the font child of `/preinst`.
No explicit hash-prefixed mount is needed: the tested sandbox spelling aliases
to `/common/font`. Keep other parent mappings in place.

The candidate remains SHA256
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.
No emulator rebuild or guest launch occurred. Parent may now launch the mixed
comparison; runtime font creation and usable language UI remain to be verified.
