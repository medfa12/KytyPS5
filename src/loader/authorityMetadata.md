# Declared authority metadata

`AuthorityMetadata` preserves the input's declared authority, not a computed
credential or a cryptographic verification result. The user-selected local
firmware is the code-and-declared-metadata input boundary. No signature approval,
filename rule, caller-PC inference, or authority-ID whitelist is added.

## API and lifetime

- `ReadAuthorityMetadata(span<const uint8_t> prefix, uint64_t file_size)` returns
  `optional<AuthorityMetadata>`. It is a bounded, endian-explicit, host-only parser.
- `Elf64::GetAuthorityMetadata()` returns a const reference to its optional
  snapshot. `Open` reads at most `AuthorityMetadataHeaderLimit` (64 KiB), passes
  only bytes actually read, and resets the previous snapshot through `Clear`.
- `Program::authority_metadata` is copied from that snapshot in `LoadProgram`,
  before publishing the program. It is immutable by loader convention after
  publication, like `compiled_sdk_version`; the public struct is not C++ const.
- Runtime consumers use `RuntimeLinker::GetCurrentProgram()` and read
  `program->authority_metadata->authority_id` only when the optional is present.
  No loader lookup, filesystem read, or loader lock is needed for this query.

The public record contains `authority_id`, `program_type`, `firmware_version`,
`application_version`, `digest[32]`, `raw_record[64]`, original input `file_offset`,
and `AuthorityMetadataProvenance::{SELFContainer, RetainedExtendedInfo}`. The two
version-word field names identify the conventional extended-info slots; both
audited values are preserved verbatim. Digest bytes are retained, not verified.
An unfamiliar authority ID is still a declaration; consumers perform their own
exact authority comparison. Unknown metadata never becomes a default authority.

## Recognition boundary

The deliberately narrow supported profile is ELF64 little-endian FreeBSD ABI 2,
x86-64, version 1, zero ELF flags/padding, 64-byte ELF header, `e_phoff=64`,
56-byte program headers, nonempty fully available program table, and no section
table. Plain images support FE10/FE18; the metadata parser also recognizes FE00
inside a SELF container. The existing executable loader's FE10/FE18 validity
policy is unchanged: recognizing FE00 metadata does not enable loading FE00.

The 64-byte record must immediately follow the program table. Its type is 8,
and its two version words must both be `1360000000000000`, the locally audited
13.60 profile. Other types, ABI versions, layouts, or firmware-version tuples
remain unknown (`nullopt`), rather than being guessed from plausible padding.
The parser does not hardcode `0x350`, program-header count 14, or privileged IDs.

Every nonempty segment is checked for offset/length overflow and overlap with
the record. Plain-image segment starts must be at or before EOF. At least one
nonempty PT_LOAD must follow the record; plain-image
PT_LOAD contents must fit the declared file length. Non-load payload availability
is not a prerequisite for preserving metadata: both selected plain images have
a final NOTE entry beyond EOF. This parser does not replace whole-image validation.

For SELF, the wrapper magic, known identifier tuple, reserved word, segment-table
bounds, declared file/header lengths, embedded ELF header/table/record, and all
nonempty physical container extents are checked. The record must fit the declared
SELF header and be outside physical container payloads as well as logical ELF
segments. At least one physical payload is required. Known identifier tuples
include the local update-container `...10 01 01 32 01 07 00 10` / `unknown=52`
variant in the pure metadata parser. Production `Elf64::IsSelf` recognition is
unchanged. No decryption or signature check has been added.

## Read-only local header check

Bounded 4096-byte reads of the currently selected files confirmed:

| Input under `${USERPROFILE}\Downloads\13.60` | ELF offset | PH count | Record | Authority |
| --- | ---: | ---: | ---: | --- |
| `system_b/vsh/SceShellCore.elf` | 0 | 14 | `350` | `4800000000000010` |
| `system_ex_b/app/NPXS40087/eboot.bin` | 0 | 14 | `350` | `480000000000000f` |
| `segments/updatemode.self` | `e0` | 6 | `270` | `3800100000000001` |

Offsets above are hex. All three records have type 8 and both version words
`1360000000000000`. Both plain images' first PT_LOAD begins at `4000`. The SELF
has six 32-byte wrapper entries, header size `2b0`, metadata size `3f0`, and its
first physical segment begins at `6a0`. The implementation was checked against
these observed fields by source review and the integrated parser fixtures.

## Save and regression integration

`Elf64::Save` always emits plain ELF. For either input provenance it writes the
entire recognized record at the logical ELF offset
`e_phoff + e_phnum * e_phentsize`. Recognized images are sectionless and the parser
has excluded segment overlap. Reopening the output yields `RetainedExtendedInfo`
with the same authority, raw record, and digest, including when the input was
SELF. Saving does not change the original input snapshot's provenance.

The lead includes `loader/elf.h` at top level and includes
`tests/AuthorityMetadataTests.inc` in the memory-test fixture namespace, then
dispatches `TestAuthorityMetadata()`. No new compiled source file is needed.
Fixtures use literal independent ELF/SELF/record bytes and synthetic temporary
files, covering unknown/truncated/overflowing/overlapping inputs, non-whitelisted
IDs, raw bytes/provenance, repeated `Elf64::Open`, plain and SELF Save/reopen, and separate
current-program contexts. Actual `RuntimeLinker::LoadProgram` is not exercised by
these focused fixtures because it also maps/relocates/initializes programs.
The combined Windows build and `authority_metadata` focused CTest passed after
integration, alongside the NPDRM descriptor/persistence regressions.
