# Retail shutdown GNF stdio correction — 2026-11-09

## Outcome and scope

The observed shutdown texture header read was rejected by emulator stream
routing, not by missing asset bytes or a failed native header parser.
Registered `GuestFopen` streams now support the floor stdio read/seek family
in VSH chain boots as well as direct-title boots. Cross-family close is
returned to the registered owner so it closes and unregisters its wrapper.

This is a dedicated shutdown/animation-asset I/O checkpoint, separate from
auto-login commit265f06d1 and the independent M.2 work. It does **not** claim
that the complete shutdown animation rendered. No guest was launched, attached,
closed, or controlled for this task; no asset/native code/store was modified.

## Historical runtime evidence (not rewritten)

The original observation remains in
`retail-localhome1-observation-20261109.md` (parent checkpoint9e81a2ba).
External frozen `retail-localhome1/shutdown-evidence/receipt.json` records:

- Host final6166339bytes, SHA256
  `909c521052f25982b99a38ea746b32df729571a03b28fb55f678ed2b85950ccd`.
- Guest final154676286bytes, SHA256
  `8971f2570d4992cb3257094083b71d1a0451142b9f0027a18b1fcb05dedf2a1d`.
- Host54369 requests shutdown;54393 loads/focuses ShutdownScene.
- Guest2204563 opens `/system_ex/vsh_asset/shutdown_ramp.gnf` as `rb`, yielding
  shim0xffe0b3bea0. Guest2204597 stats that path. Guest2204598 reports
  `fread(72 x 1) -> 0 items` at guest return0x93015cc30, followed by native
  `[XF] LoadTextureFromGnf: failed file read` at2204599.
- The separate lightbar model opens through kernel I/O and reads99600bytes
  successfully at2204745..49. That success is not proof of texture/animation
  rendering. The user subsequently closed the window; process1004 is absent.

The task used those frozen excerpts/receipt, not a live debugger or live store.

## Actual IDA consumer contract

The frozen launch places ShellUI eboot at0x930000000, so the fread return
address is eboot-relative **0x15cc30**. Actual IDA of the selected retail eboot
confirms function +0x15cb40..+0x15cee0 (`XF LoadTextureFromGnf`):

1. +0x15cbb4 calls fopen NID `xeYO4u7uyJ0`, mode`rb`; the result becomes RBX.
2. +0x15cbcb calls stat NID `E6ao34wPw+U`; the native code checks file size
   before proceeding. It does not read/seek the stdio stream here.
3. +0x15cc1a..2b passes stack destination, size0x48, count1, **the same RBX
   stream**, then calls fread NID `lbB+UlZqVG0`.
4. +0x15cc30 compares EAX with1. A mismatch reaches +0x15cc7a and the exact
   failed-file-read message. Thus zero means no complete header item; the
   native caller expects item count, not bytes or synthetic success.
5. Success proceeds to GNF magic0x20464e47/version parsing. For the pinned
   shutdown v5 asset, helper+0x15cee0 computes data offset0xf8+8+0=0x100.
   +0x15cd95 seeks to this offset; +0x15cdd7 reads the0x800-byte payload as
   one item; +0x15cddc again requires1. +0x15cde8 closes the stream.

Frozen relocation lines12425..31 bind those eboot imports to:
`GuestFopen`, **CFread**, **CFseek**, and **CFclose**, respectively. The libc
and floor wrappers are mixed by the actual native consumer, not a hypothetical
test-only combination.

Analysis provenance:

- Original staged retail eboot SHA256:
  `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`.
- Private normalized IDA copy:
  `0ea43b4cc6b323a5ffa53e9db8c6c4473ac72abe9da65455c87b049e81a2dd79`.
  Equal length and byte-identical outside the private ELF e_type normalization
  bytes16/17 were checked. Source firmware was read only.
- Actual IDA assembly/decompilation and pinned specification are external
  `automounter-ida/retail-shutdown-gnf-contract.{txt,json}`.

## Emulator root and correction

`GuestFopen` resolves the mounted path, opens a real host FILE, and registers
its opaque wrapper with `GuestFile`. CFread calls production `GuestFile::Read`.
That helper recognizes its own `_Fiopen` family first, then asks
`ForeignShimHost` for registered fopen-family wrappers.

Previously `ForeignShimHost` returned null whenever `IsDirectTitleBoot()` was
false, **even for an exact registered pointer**. This VSH run is a chain boot.
Read therefore returned0 without invoking host fread or advancing the cursor.
KernelStat and the kernel-model read use different paths, explaining their
success. The same boot gate also rejected cross-family seek/tell/getc.

The correction uses exact registration in either boot mode. It does not infer
ownership by reading guest memory and does not accept an unregistered host
FILE or arbitrary guest pointer. Missing paths and read-only mount policy are
unchanged. Reads preserve stdio item-count, partial-item and EOF semantics;
zero-size/count and multiplication overflow return0 without moving the cursor.

The registry now optionally carries an owning close callback. GuestFopen
supplies one that unregisters, closes the host FILE and destroys its own
wrapper. `GuestFile::Close` invokes it outside the registry lock, without
guessing the foreign wrapper layout. Its own `_Fiopen` cleanup is unchanged.
Previously floor CFclose reported success on these foreign wrappers without
actually closing them. This change prevents the texture loader leaking them.
Unknown-pointer close retains the existing fail-soft return behavior.

## Production fixture and limits

`tests/RetailShutdownStdioTests.cpp` opens through production GuestFopen,
registers the production floor libc exports in a real SymbolDatabase, and
calls their resolved fread/fseek/fclose functions. Filesystem mounting, stat,
stdio wrappers, registration, host reads and cleanup are production code.
Only GPU invalidation is replaced for these host-only fixture buffers; no
native guest function, renderer or texture upload is executed.

Pinned original retail assets, read-only and rehashed before/after:

| Asset | Bytes | SHA256 |
| --- | ---: | --- |
| shutdown_ramp.gnf |2304|`090515e14a8ab18ee9a2b33f7f0676887f4172306a4a9b529d4596cb0c31bcf7`|
| Sce.Vsh.ShellUI.BGLayer.Particle0.gnf |167936|`e989bcd6325a7b889f869863e4c32019c89e7aaa4cace49795f17b6d4db23991`|
| Sce.Vsh.ShellUI.BGLayer.Particle1.gnf |167936|`9cb10598c0b00a99743ab504eb5a9026fdb18f326b182de31052c2acec1a782b`|

The fixture covers chain **and** direct-title boots: fopen→stat→72-byte
header read; shutdown's native-style256-byte seek/2048-byte payload read;
complete byte-for-byte reads of all three assets; EOF without fabricated
success; shared seek/getc/tell cursors; owner-close unregistering; reads and
owning-close after close; unchanged own-family and reverse-family reads;
unknown guest and raw host FILE rejection; missing files; read-only retail
mount; partial item counts/data; zero size; overflow; null destinations.

Old e611 production objects fail the native header item-count check, reproducing
the observed chain-mode failure. Updated objects pass. This validates the
asset-read root and lifecycle, not all previously gated cross-family callers
in a complete VSH run. The old gate's broad stability comment had no explicit
caller admission contract; the new tests establish behavior for exact known
streams while preserving rejection of unknown pointers. Broader VSH runtime
regression and rendered shutdown animation remain parent/user-run checks.

## Isolated build and parent merge

Unlaunched candidate:
`${USERPROFILE}/AppData/Local/Temp/opencode/retail-shutdown-stdio2/kyty_emulator.exe`.
SHA256: `154bfc67e5986c08f8519feefb819cc3ed2138da23189c548e7385e736a3f2d9`.
Fixture results are exit1 for the expected old-object control and exit0 for
the updated production objects, including the native-style payload read.

`tools/build_retail_shutdown_stdio_fixture.py` uses **only** the receipt-pinned
e611 auto-login candidate as its link base, with two concurrent compile jobs
maximum and serial links. Shared5ee6a153/media/provider objects and original
assets are read-only/rehashed. Parallel M.2 sources are not compiled into it.

Production replacements to merge together:

- `libC.cpp.obj` (fopen owner-close registration; retains committed auto-login).
- `libGuestFile.cpp.obj` (cross-family lookup/read/lifecycle).
- The changed `libGuestFile.h` changes the registration signature, so replacing
  only one of these objects is not a valid overlay.

Object SHA256s in `retail-shutdown-stdio2`:

- libC: `713f6d6ece839404c9c026b41180b1b0daf4c12cfc77ae198426a71b0de540e8`
- libGuestFile: `e5c8df30ab01ae1107a4e74a3069d28b6129664939a67626e9011028237b95dc`

The external candidate directory contains responses, object files, logs,
tests.json and receipt.json for parent combination with independent M.2
objects. Final combined-candidate identity belongs in the parent's launch
receipt. No launcher or runtime-store changes are needed for this stdio fix.
