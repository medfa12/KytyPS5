# Production Audiodec integration checkpoint

## Result

Device-free **boot-session production loader integration passed**, using actual
13.60 CPU/DDP firmware and production HLE helpers. This closes the loader,
relocation closure, native initializer and guest-stack gates left by `3b885675`.
Netflix itself, NGS2/output-device playback and authenticated-title playback were
not exercised. The fixture is synthetic six-channel E-AC-3; no sound-recovery
claim is made for a live application.

## Integration found two real defects

1. `fail_if_global_not_resolved=true` did **not** reject missing DDP imports:
   `RelocateRecord` logged `Stubbed` and installed a zero-return thunk anyway.
   The earlier checkpoint's description of that flag as strict was incorrect.
   Now unresolved global imports of the strict native DDP provider terminate
   relocation explicitly, before startup. Other modules retain existing policy.
2. `aaDMGGkXFxo` (`frexpf`) lacked production HLE registration. Added a float
   SysV ABI wrapper calling `std::frexp`. The actual DDP import resolves to it.
   The integration also directly tests mixed float/pointer ABI and subnormal
   normalization through the production registry on the guest stack.

Before adding the helper, the corrected failure policy rejected the actual
firmware with `Required native DDP dependency unresolved: aaDMGGkXFxo...`, exit321.
The final negative fixture changes that NID only in a local copy, proving the
policy still rejects a missing helper after the registration is present.

## What the harness actually executes

`prepare_audiodec_importer.py` generates a small non-executed PS5 ELF containing
four CPU API imports and GOT relocations. `ProductionAudiodecTests.cpp` uses:

- Actual VirtualMemory, Memory, FileSystem and pthread initialization.
- `Libs::InitAll` production symbol registration.
- Read-only `firmware/system_b/common -> /common` and `firmware/system_b -> /system` mounts.
- `RuntimeLinker::LoadProgram`, `EnsureImportProviders`, and `RelocateAll`.
- Two-pass discovery: one importer -> CPU on pass0 -> DDP on pass1.
- All DDP relocation completion bits checked; all four importer GOT entries
  checked against the actual CPU exports through production trace thunks.
- Ordinary production `PthreadCreate`/`PthreadJoin`, no custom stack switching.
  `KernelVirtualQuery` proves the worker's local variable lies on a committed
  guest stack (8388608 bytes). The executable needs no host `/STACK` override.
- `StartAllModules`: native DDP then CPU initializers. Both must record a returned,
  nonnegative initializer result and started state before decoding.
- Query/init/decode through the importer's relocated GOT, production trace thunks
  and actual production libc/kernel helpers. No standalone helper bindings or
  custom image mapper are compiled into this executable.
- Shared PCM contract: output0/36864/36864, finite amplitude<0.5, nonzero energy,
  intact canaries and dominant expected tone on each of six channels.

The standalone contract is shared via a compile-time mode in
`NativeAudiodecCpuTests.cpp`; its mapper/helper code is excluded in production
mode. The original standalone positive and old-zero-return negative were rerun
after this refactoring: positive passes, negative exits1 with40960 bytes unchanged.

## Final verified scenarios

| Scenario | Observed result |
|---|---|
| Actual13.60 mounts | exit0; native startup and guest-stack decode pass |
| Empty firmware root | exit321; missing CPU explicitly rejected |
| DDP-only root (wrong dispatcher prefix) | exit321; missing CPU explicitly rejected |
| CPU-only root | exit321; missing DDP explicitly rejected on second pass |
| CPU + DDP copy with unknown helper NID | exit321; DDP dependency explicitly rejected |

Each negative runs in a fresh process and a unique local staging root; the runner
asserts neither module-startup completion nor PCM decode occurred. Source firmware
is read only. Full fixture logs remain under `_Build/production-audiodec-validation`.

Final integration executable SHA256:
`ad064661052c7ee971bda1261cc68e822c430aa03862c7b7abff1ab9b961f29a`.
Firmware/sample hashes are recorded in the preceding mnetflix7 document.

## Reproduce

The parent confirmed a build slot before compilation. All compilation used at
most two jobs. No parent files were written. The executed overlay freshly compiles
this worktree's runtimeLinker, libC and harness, and links the completed parent's
production objects/archives read-only. Thus this is an actual production-component
integration run, not a clean rebuild of every source in this older worktree.
The embedded build banner belongs to the reused parent common archive and is not
an identity for this checkpoint. The overlay must be rebuilt against compatible,
completed production objects, with the parent build idle.

From the media worktree (fixture preparation command is in the previous report):

```powershell
python -B tests/build_production_audiodec.py ${USERPROFILE}/Desktop/kytyps5/_Build/windows _Build/production-audiodec-validation
ninja -C _Build/production-audiodec-validation -j2
$env:PATH = '${USERPROFILE}/Desktop/kytyps5/_Build/windows;' + $env:PATH
python -B tests/run_production_audiodec.py _Build/production-audiodec-validation/ProductionAudiodecTests.exe ${USERPROFILE}/Downloads/13.60 _Build/native-audiodec-validation/tones.eac3
```

Also registered opt-in CMake target `production_audiodec_tests` for a fully isolated
normal build. It is excluded from default builds and automatic CTest runs because
firmware and encoded fixtures are external inputs. That full CMake target was not
built in this checkpoint; the overlay recipe above is the executed build path.

Remaining scope: parent integrates/rebuilds production. This result establishes
the offline boot-session CPU/DDP path, not a live Netflix playback result or the
separate application-owner execution mode. No audible retry is required by this
checkpoint.

## Parent integration checkpoint

Reviewed and integratedfca4429c asa987c31f. Parent production emulator rebuild
is running with VS18 and parallel2, log
`Temp/opencode/netflix-ddp-integration-parent-build.log`. After that build
completes, parent will independently compile the device-free overlay against
the completed production objects and run the positive plus four rejection
scenarios. The prepared command is
`Temp/opencode/netflix-ddp-integration-parent-check.cmd`; it has not been run
at this checkpoint. No audible retry or live Netflix validation is scheduled.

### Parent production build and device-free scenarios passed

VS18 production rebuild completed. Emulator SHA256:
`f0cafed9539615914642ce84d5105cae8386f10045bd2fd6fafe8be5f40e92e1`.
With that build idle, parent freshly compiled the overlay's production loader,
libc and harness using the completed production support objects, parallel2.
Harness SHA256:
`9cd3ec9113a79da4a8d5bf3dfd209d4022bd1cfbe6588a0435ca48255fd6256f`.
Evidence directory: `Temp/opencode/netflix-production-audiodec-parent`.

Independent parent runner passed all5 scenarios: positive exit0; missing
dispatcher, wrong-prefix provider, missing DDP and missing helper each exit321,
with the expected explicit rejection and no startup/decode completion. Positive
log verifies two-pass discovery/actual relocations, an8388608-byte committed
guest stack, native module initializers, and output0/36864/36864bytes with finite
nonzero5.1 channel-tone/canary validation. No audio device or Netflix process was
opened. Actual Netflix playback and audible sound recovery remain unverified.
