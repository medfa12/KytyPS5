# Retail language-scene font crash

## Verified origin

The Bond candidate remains `87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.
Parent verified query native21/subtype2, conversion true and result count 1,
followed by language-scene load/focus. Visual confirmation remains pending.

Immutable `retail-bond1/checkpoint1` logs were rehashed during this analysis:

- guest: `ccb8f12431495b5dd195bb8174e22816280c644007662313cad249d615fae3d9`
- host: `9acb57f1fa2aaaf66eb914ba9165ae9fd15b55c8c7c259d21fd59641662869bc`

Guest line 470 places PSM at `0x950000000`. Lines 448244–448249 record
`common/font/SST-Bold.otf` opening through the sandbox prefix, resolving to
`retail-strict1/stage/preinst/common/font/SST-Bold.otf`, and failing with
ENOENT (`0x80020002`). These are immutable log observations; the parent runtime
tree was not accessed.

Lines 448254–448256 capture the **original** SIGSEGV:

- PC `0x95022fc36` = PSM `+0x22fc36`, read fault at zero.
- RDI = 0; RBX = R12 = `0x201473450`.
- The logged first qword of that object is `0x8002000200000000`, matching
  reference count zero at +0 and the failed-open status at +4.

Actual retail IDA identifies `+0x22fb70..+0x22fcdf` as the backing-font cleanup.
At `+0x22fc32` it loads RDI from `[RBX+0x20]`; at `+0x22fc36` it executes
`mov rax,[rdi]` before any null check. The later check at `+0x22fc76` cannot
protect this dereference. No unsupported-opcode hypothesis is needed.

## Provider failure contract

IDA of PSM `+0x206e50..+0x207647` establishes the causal path:

1. Select the font filename and construct `/<sandbox>/common/font/<filename>`.
2. Allocate a 72-byte backing record; initialize +0x20/+0x28 and +0x40 to zero,
   descriptor +0x38 to -1, reference count to zero.
3. Call the native kernel-open import. On negative return, save it at +4 and
   print `[PSM] sceKernelOpen '%s' ret=0x%x`.
4. At +0x207581 test the backing pointer +0x20. If null, undo the reference
   increment and call cleanup at +0x2075a8 when the prior count was zero.
5. Cleanup dereferences that null backing pointer at the observed fault PC.

The logged ENOENT, error-bearing zero-count object and fault operands agree
with this failure path. This is static retail control-flow evidence correlated
with guest runtime evidence, not a newly executed native failure fixture.

## Why Mono traps afterward

Guest line 504 places Mono at `0x960000000`. Its SIGSEGV handler `+0x230c10`
calls fatal reporting `+0x30a1b0` at +0x230e2e, giving return PC +0x230e33,
which matches the final captured stack. The fatal reporter executes `UD2` at
+0x30a4f0 after its final import call. The captured bytes `0f 0b` match IDA.
Thus the final C000001D is downstream of the original font null dereference.
Skipping either fault would not repair the missing backing resource.

## Source identity and asset blocker

Original PSM SHA256:
`0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6`.
Original Mono SHA256:
`294af118b930006345855ecc63e452d3fa28a2c138136755c6f2066b15938a40`.
Both private IDA inputs were compared byte-for-byte with these originals and
differ only at the two-byte ELF type field. Original firmware was not modified.
External IDA reports in `Temp/opencode/automounter-ida`:
`retail-font-fault.txt`, `retail-font-cleanup.txt`, `retail-font-callers.txt`,
`retail-font-create.txt`, and `retail-mono-font-fatal.txt`.

A filename search of the supplied retail firmware tree found only SST
HeavyItalic and Condensed under NPXS40087/ugc, not SST-Bold. Its
`segments/font.zip`, SHA256
`749222813eac78539b926099067b17af14642bee22d922be810eab35fdfb50ba`,
contains only `DFHEI5-SONY2022.ttf` and `SIE-EmojiStyle-PS5.ttf`.
This does not establish whether another unextracted retail container contains
the missing font. `prepare_preinst_fonts` currently extracts that TTF archive;
it cannot supply SST-Bold from those two members.

The supported next correction is to recover the authentic requested font from
retail-source assets and stage/mount it with a provenance receipt. No substitute
font, cross-firmware copy, forced open success, native patch or fault skip is
justified by this analysis. No new emulator candidate was built.

## Parent follow-up and limits

- Run the same candidate without the query observer and preserve immutable
  logs, using user-controlled navigation. Compare the SST-Bold open result,
  PSM PC and original operands. This directly tests whether capture is necessary
  for the failure; observer influence is not yet experimentally excluded.
- Provide or identify the retail preinstalled SST font source/container. Once
  recovered, validate the exact requested path and provenance, then run again
  to verify font creation and a usable language scene. Merely opening the font
  successfully does not prove subsequent UI success.
- Stack-watch range-end-zero warnings have no established causal role.
- Observer cleanup_verified=false reflects process death, not orderly detach.

This is an analysis checkpoint. No process, controller, runtime store or shared
build output was accessed or changed. Bond/state/GC implementation is intact;
no build or regression rerun was warranted for this documentation-only change.

## Parent control and mixed-source follow-up

The parent launched `retail-bond2` with the identical hash-verified executable
and mixed preparation, fresh disposable store and normal audio, without the
query observer or any debugger. The background command only observes process
exit; the uncaptured failure result is pending.

The parent located the exact requested filename in
`Downloads/13.60/preinst/common/font/SST-Bold.otf`. In keeping with the user's
working-preparation reuse requirement, an isolated mixed-source font comparison
is being prepared with explicit provenance. This is a permitted comparison
within the already mixed run, not proof of authentic retail font recovery.
The earlier exclusion of all cross-tree copies should not be read as blocking
that scoped experiment. Original firmware and prior staged resources remain
preserved; font format/identity and guest success still need verification.

The uncaptured control reproduced the failure: guest lines 410339–410345 show
the same SST-Bold path resolving to the font stage and returning ENOENT, then
line 410346 dispatches a null-read fault at PSM `0x95022fc36`. The process ends
at the same Mono fatal trap `0x96030a4f0`. The observer is therefore not required
to trigger this failure; this does not establish timing equivalence.
Immutable evidence is `retail-bond2/checkpoint1`: guest SHA256
`72ec43d70e06c863915378da2dc4df8a0e2bffda6d3ce35ef5dce5a8dbdce9bf`,
host SHA256 `8a74b808cdc0c2bdcfab6fdbdb4720d3726a7fc7ef821d5787c2abeddfa41670`.
