# Shared-owner executable memory policy — 2026-11-05

## Outcome

Guest memory APIs cannot create executable mappings or add CPU execute access
for a shared application owner. The bounded native stopper recognizes only
loader-published private ELF `PT_LOAD/PF_X` text; a successful guest RWX allocation
would create a second, unsupported executable domain. This change refuses that
request before allocation, backing preparation, accounting, GPU invalidation,
callback invocation or mapping/protection publication. It does not broaden stop
proofs or add JIT support.

The error is the typed kernel `KERNEL_ERROR_EACCES`, reused from the existing
`MapDirectMemory` CPU-execute refusal in `src/kernel/memory.cpp`. This is an
explicit shared-backend protection policy, not a claim that all native firmware
memory operations reject executable memory. Kernel API outputs remain unchanged
on this policy rejection; it does not latch an owner execution failure. Existing
POSIX adapters remain responsible for converting kernel errors to errno.

## Boundary and coverage

`IsSharedMemoryCaller` uses `RuntimeLinker::GetCurrentProgram()` and its
`application_owner`. Both an owner and its private modules are covered. An owner
whose execution control has not yet been initialized is also denied: requiring a
live control or active guest boundary would create a preparation-time bypass.
The check is at guest-facing API boundaries, not the common protection decoder
or trusted allocation helpers.

| Path | Treatment |
| --- | --- |
| `KernelMapFlexibleMemory`, `KernelMapNamedFlexibleMemory` | Shared execute requests return EACCES before calling the mapping implementation, including fixed replacement. |
| `KernelMapDirectMemory`, `KernelMapNamedDirectMemory`, `KernelMapDirectMemory2` | Existing common direct-map implementation already returns EACCES for CPU execute before mapping/accounting/GPU mutation; retained for all callers. |
| `KernelMprotect`, `KernelMtypeprotect` | Shared execute requests return EACCES before protection, SHM dispatch or flexible-reservation commitment. |
| `KernelBatchMap2`, `KernelBatchMap` | Shared batches preflight direct/flexible map and both protection operations across the entire array before processing any entry, including compositor-view reuse. Policy refusal preserves entries and `num_entries_out`. |
| `MapAutomaticMemory` (AMPR route) | Shared execute refusal precedes `ReserveAutomatic`, not merely the eventual direct-map rejection. |
| `KernelMemoryPoolCommit`, `KernelMemoryPoolBatch` | Shared commit rejects execute before backing work; shared batches preflight commit/protect/type-protect before processing any entry. Session commit's existing EINVAL execute rule remains unchanged. |
| Anonymous SHM | Existing map/protect implementation already rejects all non-RW protections with EOPNOTSUPP before section/view mutation; unchanged. |

Nonshared/session flexible executable maps and protection retain their existing
behavior. Session batch processing is not given the new whole-batch preflight.
Removing execute access remains allowed; the policy does not forbid ordinary RW
startup allocations.

**Trusted loader/host calls remain separate.** `AllocateProgramMemory`,
`AllocateProgramMemoryAt`, `AllocateRuntimeMemory`, `SetProgramMemoryProtection`
and `ProtectGuestMemory` are unchanged. Loading/sealing executable ELF pages or
allocating host transition/runtime memory under an application ProgramScope
therefore remains legal. This policy neither exposes those helpers as guest
exports nor certifies arbitrary executable allocations made by trusted code as
guest-stop text. It does not alter returning-HLE registration decisions.

## Fixture and integration

New `tests/RuntimeExecutableMemoryPolicyTests.inc` is included by `memory.cpp`
under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS`. The declaration is
`Libs::LibKernel::Memory::TestRuntimeExecutableMemoryPolicy(std::string*)` in
`memory.h`; the parent must wire its test-runner invocation outside this slice.
Memory must be initialized, with no installed renderer or existing callbacks.

The fixture calls production APIs under owner, private-importer, uninitialized
owner and session scopes. On supported Windows builds it initializes a real
owner execution control. No guest instructions or graphics are required. It
checks:

- Execute-only, RX, RWX and CPU/GPU RWX request rejection.
- Flexible and all three direct-map entry points; fixed replacement preserves
  existing RW content and protection.
- Mprotect/type-protect preserve semantic protection and actual host writability.
- Automatic mapping and pool commit refuse without physical reservation changes.
- Mixed mapping batches cannot allocate an earlier RW entry; mixed pool batches
  cannot change an earlier entry's protection. Policy outputs remain untouched.
- Debug accounting, mapping counts, backing size/high-water/zeroing counters,
  free-range search counts and GPU/allocation/free observer counts stay unchanged
  across each denied request.
- Trusted executable runtime/program allocation and sealing still work under
  the shared owner scope; session flexible executable mapping/protection still
  work; existing session direct/pool execute errors are unchanged.
- A typed policy refusal does not poison the initialized owner's execution.

No build, test, emulator/live run, agent, commit or push was performed for this
slice. Fixture source coverage is not a test-pass claim.

## Transitive GPU wait investigation — unchanged

`AllocateGuestRuntimeMemory` checks `FindGuestFreeRange`/`HasOverlap`, then calls
`UnmapGpuRange` **before** committing its newly selected host placeholder. This
also affects heap-arena growth and TLS allocation through `AllocateRuntimeMemory`.
When a renderer exists and the address is GPU-valid, `UnmapGpuRange` calls
`RenderContext::UnmapMemory`, which dispatches synchronously to the GPU thread.
That path may wait for scheduler completion/priority operations;
`GuestGpu::SendCommandSync` has a stack-backed semaphore completion and an
unconditional acquire.

Thus even an allocation selected from currently free placeholders can perform a
synchronous GPU round trip. **No cancellation or no-GPU fast path is added.**
The allocator's free/virtual-range registries are not a complete proof of absent
renderer state:

- PRT apertures can cover free placeholders independently of ordinary virtual
  mappings (the SHM implementation explicitly checks this case).
- Renderer unmap deliberately considers pending priority operations and guest
  memory callbacks even where cached data is absent.
- The available renderer `IsMapped` query tests full containment, not absence of
  every overlapping registration or pending operation. A false answer is not a
  safe skip predicate.

A justified fast path needs a serialized, authoritative proof of **no affected
GPU registration or pending access**, including PRT and reused-address lifetime
state; free-page status alone is insufficient. Adding that renderer-side proof
is outside this slice's ownership. In particular, simply returning on owner stop
would abandon `SendCommandSync`'s captured stack semaphore and is unsafe. These
service-progress/cancellation concerns remain separate from executable-memory
exclusion and are not resolved by it.
