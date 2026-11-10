# Alternate native NP startup54 — review and execution handoff

## Contract

Parent-owned runner: `-NativeNpAlternateStartup` sets exact host
`KYTY_NATIVE_NP_ALTERNATE_STARTUP=1`, requires the alternate launcher profile,
and supplies that profile's `libSceNpCommon.sprx`. Runtime accepts this variable
alone or with **all four** ownership/workqueue/callout/HTTP-pool variables set
to `1`. Partial or malformed implications and dispatcher selection are rejected.
Empty/`0` means disabled. The runner must reject original `-NativeNpStartup`:
that switch has no distinct runtime marker to distinguish it from legacy54.

Both reviewed images are 411092 bytes:

* Original: `a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3`
* Alternate: `81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35`

Every named NpCommon load now requires its route's exact pin, including a load
without legacy switches. This deliberately closes narrower-policy and unfiltered
provider bypasses. The original pin is unchanged. Application-owned and duplicate
providers are refused. Renamed NpCommon exporters are rejected using both parsed
export library and module identities, before symbol publication or native entry.
These metadata names are unversioned; versions are separate parsed fields.

### Intentional legacy behavior changes (alternate OFF)

The loader previously selected original NP policy by filename/flags without
these admission restrictions. This batch intentionally changes:

* **Provenance and backing lifetime:** all named original providers require the
  original hash and retained same-open bytes, even with no NP flags. Otherwise
  alternate bytes could enter through an unfiltered or narrower legacy route.
* **Duplicate loads:** a second explicit provider mapping is rejected rather than
  creating another allocator/queue lifecycle. Existing module-reference reuse is
  separate from creating a new provider mapping.
* **Renamed exports:** NpCommon library/module identity cannot evade the filename
  pin or receive an unfiltered symbol database by changing the file's name.
* **Application/private ownership:** a private NpCommon provider is rejected for
  both images, including co-modules inheriting an application owner. NP provider
  selection and the HLE native-header-aware mutex mode are session-wide; private
  native allocator/queue lifetimes are not independently selected or isolated.
  Admitting an original private copy alongside the alternate session provider
  would bypass the single-provider lifetime model. This is a deliberate current
  support boundary, not a claim that private providers are intrinsically invalid.
  Supporting them later requires owner-scoped provider selection and matching
  allocator/mutex lifetime handling. The restriction applies to NpCommon
  providers, not every private module or application.

The positive real-input regression calls `LoadProgram(original)` with alternate
OFF, first without family flags (`All`), then with legacy54 (`NpHttpPool`). It
requires a nonzero mapped range, compares actual mapped read-only data to the
original file, checks allocator/dispatcher export visibility, and verifies no
credential grant. It also checks duplicate rejection and inherited private-owner
rejection for both image routes. No native entry or initializer is executed.

`Elf64::FreezePlainFile` hashes one same-open-file snapshot and compares cached
ELF/program/section/dynamic/string metadata against it. All later segment reads
use retained bytes. The verified Program bit selects existing `NpHttpPool` policy:
ownership21 + queue/payload20 + callout10 + HTTP-pool3, never dispatcher56.
Guest startup rechecks environment/provider agreement. This route grants no
credentials and uses existing host ABI instruction adaptation; it does not alter
authenticated Core/UI snapshot admission. Exact alternate opt-in also selects the
existing native-header-aware HLE mutex vtable, preserving raw+0 descriptor,
raw+0x10 object, destroy-before-free, and unlocked free callback semantics.

## Completed static audit (before implementation)

* PHDRs, dynamic tags and symbol records identical, including all selected54
  function RVAs/sizes. Writable file-backed segments identical.
* 537 unique aligned relocations, all destinations writable/in range. Import
  relocations identical. All relative targets in mapped ranges. The only 73
  differing relative relocations point to identical reordered strings.
* 66,693 decoded executable items; 1,457 changed items explained by equivalent
  RIP references, with only the exact diagnostic `J03950017` → `J03950016`
  build-directory substitution allowed. Other metadata differences are that
  directory text and GNU build ID. Empty constructor/destructor lists;
  static inspection shows normally returning module start/stop routines.
* Selected-family direct-call/vtable traversal: 57 roots, 2,967 instructions,
  22 referenced vtable slots, 33 registered HLE function dependencies, no unknown
  decoding sites. Dependencies include real pthread creation/join, mutex/condvar,
  scheduling attributes, clock/sleep, string operations, logging, SDK queries,
  fatal/finalization handlers; data bindings include stack guard and libc need
  flag, plus pure-virtual handler bindings.
* Dynamic allocator/task/payload/timer callbacks remain native and service-owned.
  Allocator descriptor ABI is `{allocate,reallocate,free,context}` (32 bytes).
  Static equivalence does not prove callback execution, nonempty pool behavior,
  or downstream service readiness.

Reproducible byte/relocation comparison: `tools/audit_np_alternate.py ORIGINAL
ALTERNATE` (Python with Capstone, use `python -B`). Parent execution reproduced
66,693 decoded items, 1,457 explained changes, 537 relocations and 73 reordered
string relocations; symbol records and writable bytes matched. Detailed helpers remain
at `_TempData/np-alternate-code-audit.py` and `_TempData/lifecycle-re-inspect.py`;
causal disassembly notes are `_TempData/alternate-native-startup-causality.md`.

Reviewed inputs:

* `${USERPROFILE}/Downloads/13.60/system_b/common/lib/libSceNpCommon.sprx`
* `${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/system_b/common/lib/libSceNpCommon.sprx`

## Parent validation — not run by this implementation session

No CMake or runner edits. Build only `_Build/vsh-integrated`, parallelism <=4:

```powershell
cmake --build _Build/vsh-integrated --target kyty_emulator virtual_memory_allocation_tests symbol_database_tests --parallel 4
ctest --test-dir _Build/vsh-integrated --output-on-failure -R '^(symbol_database|np_allocator|np_mutex|np_heap|np_atomic|np_native_worker_http|runtime_guest_execution|runtime_process_identity)$'
```

Use the integrated test executable's actual configuration path for:

```powershell
$env:KYTY_TEST_NP_ORIGINAL_IMAGE = '${USERPROFILE}/Downloads/13.60/system_b/common/lib/libSceNpCommon.sprx'
$env:KYTY_TEST_NP_ALTERNATE_IMAGE = '${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/system_b/common/lib/libSceNpCommon.sprx'
# virtual_memory_allocation_tests.exe --np-alternate-startup-only
```

The new mapping-only dispatch checks real wrong-hash inputs,
post-admission backing mutation isolation, ownership-only bypass,
partial/dispatcher conflicts, original-as-alternate rejection, renamed/duplicate
providers, exact54 export visibility/excluded dispatcher exports, standalone
alternate selection, and positive original ordinary/legacy54 admission and mapping.
`symbol_database` covers the complete implication bitmask and malformed values;
existing mutex/allocator tests now exercise alternate-only raw-header destruction.
The existing worker/HTTP regression remains the queue baseline.

After checks pass, parent owns fresh avsh4 normal Home execution. Expected evidence:
`NP alternate54: immutable reviewed provider selected`, native allocation/queue
bindings and actual workers; disappearance or replacement of genuine PsnCore
`0x80559b23`; then actual Syndy service-slot creation and SaveData progression.
Record watchdog joins/callers and AutoMounter/AppDb outcomes even if they still
fail. avsh3 remains preserved. No readiness or native mailbox0x16 success is
inferred from static audit or mapping tests.
