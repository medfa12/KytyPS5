# Run11 startup failure: native RTTI cast — 2026-11-04

Read-only investigation of retained logs, current source and local firmware;
no build, emulator run, process inspection/control or production edit.

## Outcome

Run11 fails at approximately **48.45 seconds**, not at a controller-open call.
The initiating exception is **`std::bad_cast`**, thrown because the HLE
`__dynamic_cast` returns null in NativeExtensions' `SetContextName`.
The final illegal instruction is libc's deliberate `ud2` after termination
machinery returns; it is not an unsupported CPU instruction.

There is a concrete address-dependent defect in `FloorRttiFindBase`: it
distinguishes single inheritance from multiple inheritance by interpreting
the word at type_info +0x10 as a possible readable base-type pointer first.
For HermesRuntimeImpl this is actually **flags=0, base_count=3**, hence
`0x300000000`. Run11 maps live Hermes runtime memory at that exact address
immediately before the failing casts. Thus ordinary heap contents can change
the RTTI classification. The retained log does not dump that heap's first
words at each failed cast, so the precise erroneous recursion is not observed;
the classification collision itself is established by firmware and source.

## Exact retained causal chain

All native-log line numbers below refer to `_Runs/vshruntime11/_kyty.txt`.

- 19455: NativeExtensions slot `0x9a0098120`, NID `hMAe+TWS9mQ`, binds to
  host `DynCastProbe`, not real libc `__dynamic_cast`.
- 327550–327587: map `0x300000000`; make its first 4 MiB readable/writable;
  name it `hermes-rt`.
- 327786 and 327903: casts of `0x21a7b4030` and `0x21a885350` return null,
  despite logging dynamic type `N8facebook6hermes17HermesRuntimeImplE` and
  destination `N8facebook6hermes13HermesRuntimeE`. Nearby identical casts
  succeed at 327884 and 327971.
- 464170–464172 and 466222–466224: each failing thread's import ring directly
  pairs `hMAe+TWS9mQ` return `0x9a0023d3e` with `pBxafllkvt0` return
  `0x9a0023f0e`, followed by real exception-personality work.
- NativeExtensions RVA `0x23cb0` is `SetContextName` (embedded diagnostic
  string RVA `0x7ef5f`). Runtime kind 1 takes the Hermes branch. RVA `0x23d39`
  calls dynamic_cast with src RTTI `0x9700184a0`, dst `0x9a00951c0`, hint 0.
  Null at `0x23d3e` branches to `0x23f09`, which calls `pBxafllkvt0`.
- 19457 binds that throw helper to real libc +`0x8a7a0`. Its code allocates
  eight bytes, installs the bad_cast vptr, then calls `__cxa_throw` +`0x1ede0`.
  The libc diagnostic inputs at 330804–330832 independently identify
  `St8bad_cast` (libc +`0x17fc47`) and `std::bad_cast` (+`0x16946c`).
- 330834–330869 and 338373 onward retain two fault contexts. Chain:
  libc +`0x8a7d2` -> +`0x1ee66` -> +`0x1f08b` -> fault +`0x34fdf`.
  +`0x1f070` invokes the saved terminate handler through +`0x34fd0`;
  +`0x34fdf` is `0f 0b` (`ud2`) after that routine's calls return.
  The shared frame below is NativeExtensions +`0x23f0e`, then
  ReactNative.PUI.dll.sprx +`0x2c5138` and managed runtime frames.

## RTTI-layout evidence and supported next change

`src/libs/libShellCoreFloor.cpp:988–1019` tries an SI interpretation first:
`FloorTypeInfoNameCopy(v10, ...)` followed by immediate recursive return.
Its comment's assumption that VMI flags/count is a small, unmapped integer
is false in this guest address space.

Reconstructing 13.60 Hermes relative relocations gives:

```text
HermesRuntimeImpl type_info RVA 0x26c608
  +0x08 name -> 0x2619b6 (N8facebook6hermes17HermesRuntimeImplE)
  +0x10 0x0000000300000000       # flags 0, base_count 3
  +0x18 base -> 0x26d6f0        # HermesRuntime
  +0x20 offset_flags = 2        # public, nonvirtual, offset zero
HermesRuntime type_info RVA 0x26d6f0
  +0x08 name -> 0x262ffc (N8facebook6hermes13HermesRuntimeE)
```

Use actual RTTI-kind/vtable discrimination rather than pointer plausibility
if retaining this walker. Alternatively evaluate restoring real libc
`__dynamic_cast`: the class-type-info vtables and JSI source RTTI already
resolve natively. Do not add an unconditional successful Hermes cast or
swallow bad_cast. Add an address-collision regression fixture with valid
unrelated memory at `count << 32`; ensure the public offset-zero base is
still found. Parent owns implementation and validation.

## Firmware/provider comparison and limits

| libc export | 13.60 RVA | local 4.03 zecoxao/kek RVA |
| --- | --- | --- |
| `pBxafllkvt0` (bad_cast throw) | `0x8a7a0` | `0x8af70` |
| `hMAe+TWS9mQ` (dynamic_cast) | `0x33b90` | `0x3dce0` |
| `tVHE+C8vGXk` (bad_cast vtable) | `0x1ae130` | `0x12a1a8` |
| `dCzeFfg9WWI` (exception vtable) | `0x1acc60` | `0x1282f8` |

Both bad_cast helpers perform the same allocate/vptr/throw sequence; both
bad_cast vtables contain real destructor/what/raise targets. No firmware ABI
justification exists for treating the cast-failure helper as returning.

Local files examined: 13.60 `system_b/common/lib/libSceLibcInternal.sprx`,
`system_ex_b/common_ex/lib/{libNativeExtensions,libhermes-0.11}.sprx`; 4.03
`voldemort/zecoxao/kek/system/common/lib/libSceLibcInternal.sprx`.
The run's `_ShellUIManaged/common` NativeExtensions and Hermes copies are
byte-identical to those 13.60 firmware files (SHA-256 respectively
`2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb`,
`e279a9d4272df0347515a481bd0fe4d4bb0f7bb9281c3ece20032688c0f9d10d`).

Run11 `dCzeFfg9WWI` bindings at 3796/11769/19064 correctly use real libc
`0xa801acc60`: the run10 zero-vtable shadow is gone. This is a different,
earlier exception path, not evidence identifying run10's original HostFunction
exception. Controller rejection does appear separately at 173009, but neither
the failing import rings nor the causal frames involve Pad. There is no
evidence supporting a controller-policy rollback to address this failure.

## Follow-up implementation (not built or launched here)

`libShellCoreFloor.cpp` now determines the RTTI record kind from the vtable
header's type-info **exact ABI name**, with a canonical zero offset-to-top,
before interpreting its payload. Native 13.60 reconstructed identities:

| Native vtable RVA | Its type-info RVA | Exact kind name |
| --- | --- | --- |
| `0x1ad7c8` | `0x1ace20` | `N10__cxxabiv117__class_type_infoE` |
| `0x1ad818` | `0x1ad868` | `N10__cxxabiv120__si_class_type_infoE` |
| `0x1ad880` | `0x1ad8d0` | `N10__cxxabiv121__vmi_class_type_infoE` |

Plain classes stop after the header; SI follows its one base; VMI reads a
bounded array using the actual flags/count. The walker retains offset-zero SI
and nonzero public/nonvirtual VMI offsets. Private paths do not satisfy casts;
virtual bases, unsupported flags, negative offsets, excessive counts/depth and
ambiguous nonvirtual matches fail closed. Direct same-name casts no longer
bypass RTTI-kind validation.
The actual HermesRuntimeImpl base entries were checked in full: offset_flags
are `0x2`, `0x0`, `0x800` (public HermesRuntime, private fatal-error helper,
private Instrumentation at offset 8). This record is within the supported
layout; unrelated private bases do not obstruct the required public cast.

**Native switch deliberately deferred:** current code still has shadowed
non-class RTTI objects and the explicit 4.03 JSCRuntime fallback. Globally
unshadowing `__dynamic_cast` changes those paths too; replacing the faulty
classification is narrower and does not alter import ownership or the fallback.

`tests/FloorRttiKindTests.inc`, entry **`TestFloorRttiKind()`**, exercises the
actual registered `hMAe+TWS9mQ` export. Parent integration requires the
top-level declaration `Libs::ShellCoreFloor::InitShellCoreFloorLibc_1`, inclusion
inside the VirtualMemoryAllocationTests fixture namespace, and a dedicated
dispatch/CTest entry after InitSubsystems. No production test seam is added.
Fixtures cover plain/SI/VMI kinds, nested and nonzero base offsets, private,
unrelated, ambiguous and unsupported cases, recursion bounds, Windows guarded
metadata/names, and **valid unrelated RTTI mapped at exactly `3 << 32`**.
The collision is also changed to a matching target name to ensure it cannot
silently replace the real base's nonzero offset. `git diff --check` passed for
the production file. Compilation and execution remain parent-owned/pending.
