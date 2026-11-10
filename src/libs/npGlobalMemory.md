# NP global heap compatibility (13.60)

This owner is distinct from the NP manager singleton and per-service heaps.
It reuses `NpMemoryHeap`'s bounded backing and callback ABI. Native NpCommon's
isolated ownership exports still implement object headers and deletion.

| Export | ABI |
|---|---|
| `jGF+MaB4b-M` | `int32_t ArchInit()` |
| `UskWpVWxSvg` | `void ArchTerm()` |
| `DI5n4aOdxmk` | `Allocator* GetGlobalAllocator()` |
| `P4YpPziLBd4` | `AllocatorEx* GetGlobalAllocatorEx()` |

Both getters return separate process-lifetime descriptors, repopulated on each
call. Their callbacks share one stable global Heap context. Ex writes its error
`0x80550005` at +0x20 and preserves padding +0x24..27. Getters do not create backing
and return their descriptors before initialization and after termination.

## Lifecycle

ArchInit creates `SceNpGlobalHeap`, size `0x80000`, only on its zero-state path.
The prerequisite init/term routines are literal no-ops in this firmware. It reads
clock4 microseconds into an initially zero value, ignores clock failure, and seeds
the persistent SFMT19937 state using the low 32 bits. The heap's negative failure
status is preserved; native generator cleanup is also a no-op.

The firmware's state behavior is deliberately preserved:

- Arch state starts at zero.
- Nonzero Init increments it with uint32 wrapping and returns zero immediately.
- Successful zero-state Init **does not set the state to one**.
- Repeated zero-state Init therefore repeats allocation, without destroying old
  backing. Callers must serialize lifecycle operations, as with per-service H.
- Term always decrements with wrapping; it skips destruction only when the
  **original signed state is greater than one**.
- Term preserves both descriptors and random state. A retained descriptor uses
  the same H's current handle; a zero handle produces null/no-op callbacks.

This is not normalized reference counting. Tests explicitly release duplicate-Init
orphan resources only after verifying that native-compatible duplication retained
them; production does not synthesize that cleanup.

## Seed state

SFMT19937 state is `0x9d0` bytes: uint32 index, 12 untouched padding bytes, then
624 uint32 words. Initialization uses multiplier `0x6c078965`, sets index 624,
and applies SFMT period certification with parity lanes
`{1, 0, 0, 0x13c9e684}`. Zero is a valid seed. This is not ordinary MT19937.
The separate random-output export is not implemented by this startup correction.

## Evidence and regression boundary

ShellCore calls ArchInit at `C+c4d6c7`, then P4Y at `C+c4d9a6`. The captured failure
at `NpCommon+18652` was a null descriptor returned by the missing P4Y import,
before any allocation callback. A valid descriptor preserves native Ex-new's
allocation-failure path; a successful `0x30` object requests raw `0x40` bytes.

`np_global` covers registered bindings, separate ownership, descriptor persistence,
padding/error metadata, real bounded backing, rollback, mapping-time getter
re-entry, and Arch's state transitions. The separate split mapping/raw-heap exports
have different ownership contracts and are not aliases of this global owner.
