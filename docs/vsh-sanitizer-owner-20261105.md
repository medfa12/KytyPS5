# Owner-scoped inactive sanitizer replacement queries — 2026-11-05

## Outcome / parent integration

The live Apple launch reached its libc initializer and invoked
`py6L8jiVAN8#A#B` at return PC `libc+0x6442`. This slice implements owner-local
**query-result storage**, not sanitizer allocation/replacement callbacks.

Exact production `LIB_FUNC` identities in `src/libs/libKernel.cpp`:

```cpp
{"py6L8jiVAN8", "LibKernel::KernelGetSanitizerMallocReplaceExternal"},
{"bnZxYgAFeA0", "LibKernel::KernelGetSanitizerNewReplaceExternal"},
```

Both have registration library/module `libkernel`, versions `1` / `1.1`,
`SymbolType::Func`, no input arguments, and a table pointer returned in RAX under
the baseline SysV ABI. **The parent owns adding these pairs to the returning
table after review.** This slice does not change that table or disable first-call
classification for any other export.

The parent should register
`Loader::RuntimeLinker::TestRuntimeSanitizerOwner(std::string*)` in the test
dispatcher, e.g. `runtime_sanitizer_owner`. Its new `.inc` is already included by
`runtimeLinker.cpp` under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS`. The already
integrated `applicationImportData.cpp` supplies the implementation; there is no
new translation unit/CMake source to add for this follow-up.

Parent integration added both exact returning entries and the fixture dispatcher.
The full emulator build and all seven selected checks, including
`runtime_sanitizer_owner`, passed in `_TempData/vsh-sanitizer-np-client-build.log`
and `_TempData/vsh-sanitizer-np-client-tests.log`. Live retry is pending.
The original agent handoff used read-only offline binary analysis and performed
no builds/tests or emulator runs.

## Native evidence and its limits

Read original ELF program headers, dynamic/string/symbol and relocation tables,
and bounded instruction windows with Capstone; no guest execution or binary
patching:

| Input | Identity |
| --- | --- |
| `${USERPROFILE}/Downloads/13.60/system_b/common/lib/libkernel.sprx` | 563,736 bytes; SHA-256 `4f07827e84228db3c10fd1e804e208fc7799240990b7f37e3d97367882ff3c49` |
| `${USERPROFILE}/Downloads/storeormediaapps/PPSA01783-app0/sce_module/libc.prx` | 1,964,138 bytes; SHA-256 `c0e1690a122b597cce854e9ac9987406fa4a3d7b0d4aee16e4dc7ace368fae83` |

The first is the native **13.60 implementation**. The second is the exact
app-bundled native **caller** corresponding to the live PC, not a claim that this
app libc is identical to firmware's separate `libSceLibcInternal.sprx`.

### libkernel wrappers

- `py6L8jiVAN8#I#A` exports RVA `0x331a0`.
- `bnZxYgAFeA0#I#A` exports RVA `0x331f0`.
- Both initialize a local output pointer to zero, read the optional provider
  pointer at RVA `0x84268`, and return zero when that pointer is absent.
- With a provider, they read its handle at `+0x10` and call native KernelDlsym
  (`0x35bf0`) with respectively `__asan_malloc_replace_external` (string RVA
  `0x6282e`) or `__asan_new_replace_external` (`0x611b3`). Only a successful lookup
  returns the found pointer; lookup failure also returns zero.
- These wrappers take no guest arguments, return a pointer, and do not themselves
  construct a replacement table or invoke its allocation hooks.

Thus **native absent-provider semantics are NULL**, not a fabricated successful
sanitizer installation. The existing emulator instead returns a stable inactive
table containing a size word and null hooks. This follow-up preserves that
existing no-sanitizer representation while isolating its storage. It does not
claim the native absent-provider wrapper literally returns such a table.

### Exact libc caller

Its query relocations/GOT slots and PLT entries are:

| Query | GOT | PLT | Direct call sites inspected |
| --- | --- | --- | --- |
| malloc | `0x19e390` | `0x11d3b0` | `0x643d`, `0x64cf` |
| new | `0x19e430` | `0x11d4f0` | `0x188c3`, `0x18a4b` |

Malloc's first call returns to `0x6442`, matching the live report. The caller
tests RAX and skips replacement at `0x6583` if NULL. A non-null pointer whose
word at `+0x10` is zero takes that same no-replacement continuation. Further
required-hook tests precede copying active hooks. Only after all these nonzero
checks does its optional extended-field path test size `>= 0x78` before reading
`+0x70`.

New's call at `0x188c3` similarly checks NULL, then the word at `+0x10`, before
reading/copying subsequent hooks. The later extended path at `0x18a4b` rejects
NULL and sizes below `0xc0` before accessing its larger layout.

**Do not derive an active native hook ABI from the existing HLE structs.** These
callers have additional/versioned layout branches. For this bounded inactive
case, all hook words are zero, so the inspected consumers take the absent-hook
continuations without installing/calling any hook or reaching the larger-table
reads. Active ASan providers, their full layouts, allocation interception,
callbacks and ownership transitions remain unsupported.

## Storage and lifecycle

The existing HLE structs retain their session behavior:

- `MallocReplace`: 112 bytes, uint64 size then 13 null pointer words.
- `NewReplace`: 104 bytes, uint64 size then 12 null pointer words.

Compile-time assertions bind those existing layouts to the new inactive guest
table representations. Neither representation contains an emulator callback or
host/global data pointer.

For shared owners, the functions call
`RuntimeLinker::GetApplicationSanitizerReplacement(kind)` using the actual current
program/linker. The accessor validates owner/execution identity and published
importer/owner membership, acquires execution admission, and lazily creates the
same owner-pinned **RW, non-executable** guest allocation already used for the
four audited import objects. It then returns one of two separate table regions.
Either query can be first; both tables initialize independently of session data.

- Repeated queries and all private modules return the same per-owner addresses.
- Guest changes persist in that owner's storage; another owner and the session
  cannot observe them through these query APIs.
- The emulator never reads these hook words as callbacks, registers them as
  allocators, or executes them. Writing a word is not an implemented active hook
  feature.
- API-returned regions are **not** added to the four-entry import-object table.
  `Supports` and `ContainsObjectAddress` continue to recognize only the exact
  audited guard/progname/libc-flag/internal-flag objects, not arbitrary bytes of
  their common backing allocation.
- Missing execution, stop, pending exit, wrong membership/lifetime or an unknown
  kind returns no shared pointer and never falls back to a session global.
- Storage publication uses `Lease::Publish`, serialized with owner stop. A stop
  racing after pointer materialization is propagated by the existing owner-bound
  host return gateway, after query locks/leases/host RAII finish normally.
- Stop does not release the allocation. Copied pointers remain backed until the
  retained executable `Program` is finally destroyed under the same quiescence
  requirement as its private mappings. A subsequent owner gets fresh initialized
  tables and cannot reuse a still-pinned stopped owner's cells.

This uses the existing runtime allocator and its synchronization/renderer paths;
it does not cancel GPU completion waits or establish a broader host-fault or
whole-service liveness guarantee.

## Fixture coverage

`tests/RuntimeSanitizerOwnerTests.inc` looks up both exact production Func
registrations and calls their actual SysV bodies through production
`HostCallReturnThunk` and `RuntimeGuestExecution` boundaries. Explicit test
gateway construction permits source review before the parent adds the returning
pairs; it is not a production classifier override.

The fixture covers two owners, their private modules, the unchanged session,
missing execution, stop before first query, and a same-file relaunch. It checks
stable/distinct pointers, RW backing, exact 112/104 sizes, all null initial hook
words, isolated persistent writes, fresh relaunch initialization, foreign-lifetime
refusal and exclusion of every table byte from `ContainsObjectAddress`.

A separate test-only host adapter calls the actual registered malloc query and
then requests stop before its outer gateway returns. The fixture requires host
RAII destruction, no guest continuation, no owner failure and balanced work/
transition accounting while the previously materialized table remains pinned.
This adapter is solely fixture code; no production guest callback, active
sanitizer hook or stop-time nonlocal jump is added to either query body.
