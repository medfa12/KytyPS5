# NP heap ownership and standalone lifecycle

The standalone helpers use the same bounded `NpMemoryArena::Arena` registry as
the combined `Heap` API. They do not implement SNS or replace its native
descriptors, callbacks, object allocation headers, mutexes, or cleanup. Native
ownership21/queue41/callout51 export-selection policies are unaffected.

## Export API

All functions use `KYTY_SYSV_ABI` in `Libs::NpMemoryHeap`:

| NID | Function |
| --- | --- |
| `gMlY6eewr-c` | `int32_t MapMemory(uint64_t size, const char* name, void** optional_base, MappingRecord* record)` |
| `B+yGIX1+BTI` | `int32_t CreateHeap(Handle* slot, void* backing, uint64_t size, const char* name)` |
| `wVdn78HKc30` | `void DestroyHeap(Handle* slot)` |
| `vjwlDmsGtME` | `int32_t UnmapMemory(MappingRecord* record)` |

`MappingRecord` is exactly 24 bytes: `{uint64_t size, uint64_t reserved,
void* mapping}`. It is separate from the 32-byte combined `Heap` whose handle
is at +0x18. Standalone callbacks pass **the address of their handle slot** to
`Malloc`/`Realloc`/`Free`, regardless of the caller's enclosing object layout.

## Native evidence and field ordering

Read-only disassembly of the selected local 13.60 `libSceNpCommon.sprx` confirmed:

- **N+5b10, map:** null fourth argument returns `80558005`. Otherwise writes
  record+8=0, rounds the full unsigned 64-bit size to 0x4000 with wrapping
  arithmetic, writes record+0, and calls kernel map with `&record.mapping`,
  protection=3, flags=0, supplied name. Existing backing is not pre-cleared.
  At +5b5c..5b68 the optional output is written from record+0x10 only for a
  nonnegative kernel result. Kernel partial output writes/errors propagate.
- **N+57d0, create:** calls Libc mspace-create(name, backing, size, flags=0).
  Only a nonnull result writes `*slot` (+57eb); null returns `8055800a`
  (+57f0), leaving the prior handle untouched. There is no duplicate-slot check
  and no implicit retirement of a previously stored handle.
- **N+5800, destroy:** reads `*slot`; a zero value is a no-op. For a nonzero
  value, calls mspace-destroy then writes slot=0. No unmap occurs here.
- **N+5be0, unmap:** null record returns `80558005`. A nonzero backing is passed
  to kernel munmap with record size. Irrespective of backend status, writes
  backing=0 and returns 0, preserving size/reserved. Zero backing also returns 0.

The map imports resolve to `mL8NDH86iQI` and `cQke9UuBQOk`; the create/destroy
imports are `-hn1tcVHq5Q` and `W6SiVSiCDtI`.

The selected Libc create wrapper L+d6a0 enters L+e9b0 with optional config=0.
Its flags-zero backing path queries `WB66evu8bsU` at +ea10. SDK query failure or
SDK >=03500000 selects strict validation (+eb10, +ebe6): backing and size must
be 8-byte aligned and size must exceed 5a0. The older path requires size>580.
Both reject size >= `ffffffffffffc000 - overhead` (+ec25..ec32).
The implementation uses the existing immutable compiled-SDK query for these
checks. Null name selects a native default diagnostic name (+ed29..ed53), so
it is accepted here, unlike a null name passed to the kernel mapping helper.

Null/invalid **handle-slot pointers** have no native checked-error contract:
create dereferences the slot only after successful creation; destroy directly
reads it. The wrappers do not invent a null-slot errno. Foreign opaque handles
have no audited native graceful-recovery guarantee. The host registry rejects
their allocation use and clears such a slot on destruction without freeing
unrelated host memory.

## Bounded backend and lifecycle

`CreateState` and `RetireState` are shared with existing combined Init/Destroy.
Combined mapping-failure partial writes, post-map failure rollback/32-byte clear,
duplicate-init orphaning, descriptor ABI, and observer events retain their
existing behavior. The split path independently validates full mapped writable
coverage outside NP locks, then creates a non-owning arena. Failures preserve the
old slot; the existing test-only allocation-failure injection covers publication.

The allocator remains the existing emulated arena: native Libc's in-buffer
metadata, precise capacity/fragmentation, platform-specific address-window checks
and arbitrary Libc mspace options are not reproduced. No broad Libc mspace change
or new native export-policy exposure is needed for this backend boundary.

Retired registry entries remain allocated as inactive tombstones. A copied stale
handle cannot become a new heap's identity, and recreating through the same slot
does not revive old handles. Destroy invalidates outstanding arena allocations
without freeing their backing. The caller must separately unmap its record.
Unmapping a live arena is not converted into implicit destruction.

Callers serialize lifecycle/slot changes and retain backing coverage through all
allocations. Registry and arena locks are suspension-protected, acquired in
separate scopes. Kernel mapping, coverage queries, logging/observation, and native
callbacks do not execute while NP locks are held. No lazy creation occurs.

## SNS reproduction and tests

Native SNS memory state is 0x80 bytes at ShellCore+21667d0: mapping record at
+10, handle at +28, ordinary descriptor at +38. Init requests 0x10000 bytes.
The first list mutex requests **0x18 object bytes**, and native scalar new
NpCommon+185a0 requests **0x28 raw bytes (40 decimal)** after adding its 0x10
header. The first qword stores the original descriptor; paired deletion passes
the raw header base back to that descriptor's free callback. This is not a
0x40-byte raw request and must not receive another backend allocation header.

`tests/NpSplitHeapTests.inc` defines `TestNpSplitHeap()`, intended after
`NpMemoryHeapTests.inc` under the existing `--np-heap-only` dispatch. Direct API
fixtures exercise 64 KiB mapping, SNS-shaped callbacks and native header-format
pairing, arena exhaustion, failed creation/realloc preservation, stale handles,
slot replacement, read-only/partial backing rejection on Windows,
destroy-without-unmap, explicit unmap, null/default-name and
audited size/rounding cases, ignored unmap failure, output canaries and critical
depth. Header-format fixtures exercise actual backend callbacks; they do not
claim to execute a loaded native new/delete provider. The four bindings are
registered at `InitJscdSmallNpCommon_1`; fixtures verify exact and NID lookup after
normal provider registration order. The combined Windows build and `np_heap`,
`np_global`, allocator and mutex regressions passed after integration.
