# Executable compiled SDK query

`WB66evu8bsU` is `int32_t sceKernelGetCompiledSdkVersion(uint32_t* out)`.
It writes exactly four bytes and returns status, unlike the former shared
SDK14 direct-return stub.

The 13.60 native getter queries sysctl `{1,14,36,getpid()}`. The kernel's
process field comes from executable `PT_SCE_PROCPARAM` +0x10, with ORBI magic
and a declared size of at least 0x14. The adjacent +0x14 field is a separate
Prospero SDK value. ShellCore's fields are respectively `0x13590001` and
`0x13600007`.

The loader snapshots the bounded initial executable image into
`Program::compiled_sdk_version`. Missing/malformed metadata leaves zero;
module parameters and firmware versions are not fallback sources. Later guest
writes cannot change the process snapshot. The query uses the lock-free current
program context, inherited by guest pthreads, so co-hosted executable identities
remain separate. No global cross-process SDK cache is introduced.

Null output returns `0x80020016`; unavailable process returns `0x80020003` and
preserves output. A valid process with SDK zero succeeds and writes zero.
Native directly writes a nonnull output; this handler follows that contract.

`process_sdk` checks early/late and exact/NID bindings, literal process-header
fixtures, per-program context, error-output preservation, adjacent canaries and
a four-byte output at a writable page boundary.

## Separate Prospero query

`GGeRJk1XdWc` is `int32_t sceKernelGetProsperoCompiledSdkVersion(uint32_t* out)`.
Native implementations are at libkernel+`1c9c0`, libkernel_sys+`1d6d0` and
libkernel_web+`78b0`. Its old HLE stub returned success without writing output.

The query distinguishes the executable ABI from its SDK value:

| State | Status | Output |
| --- | --- | --- |
| Null pointer | `80020016` | No write, first check |
| Known legacy executable | `80020001` | Exactly four zero bytes |
| Known Prospero executable | 0 | Exactly four bytes from its Prospero snapshot |
| Unavailable process snapshot | `80020003` | Preserved |

`ProcessAbi` is derived from the admitted executable ELF's `EI_ABIVERSION`:
0/1 legacy, 2 Prospero. Shared images and unsupported values remain unknown;
this classification does not add executable admission rules. Unknown ABI is an
unavailable emulator query source, not evidence of a native legacy process.

Kernel snapshot reader `80d37070` selects process-parameter `+14` only for
Prospero processes and format version >=4. Known Prospero processes with absent,
old-format or zero-valued parameters legitimately retain zero. The bounded
parser requires ORBI, declared size >=`14`, and actual bytes through `17`;
it does not invent a declared-size >=`18` condition. Loader reads are bounded by
file-backed size, memory extent, image extent and actual bytes read. Module
parameters, the legacy SDK, filenames and firmware versions are never fallback
sources. `+58`'s build component belongs to the eight-byte kernel sysctl result,
not this four-byte public query.

The separately gated native exec override from image-parameter `+20c` has no
established producer in this emulator. This implementation exposes the selected
executable snapshot, without inventing override metadata. Shared-initializer
execution ownership and full process identity remain separate limitations.

The focused `process_sdk` fixture also checks Prospero exact/NID bindings,
unaligned and guard-page outputs, legacy error-plus-zero, null precedence,
unknown versus known zero, format/ABI gates, separate co-executable snapshots
and post-snapshot source mutation. `authority_metadata` checks actual short-read
counts through the loader's optional `LoadSegment` byte-count output.

The combined Windows emulator/fixture build and `process_sdk`,
`authority_metadata`, and `np_heap` CTests passed. Independent review found no
blocking defect. Snapshot-context fixtures construct Program records directly;
SELF byte-count forwarding is source-reviewed, while the EOF fixture exercises
plain ELF. This does not establish a change in SSL certificate selection (the
audited 13.60 table's Prospero bounds are all zero).
