# Control Center TurboModule registration (read-only investigation)

## Finding

Run7 loses ShellUI's native TurboModule factory registrations because
`libNativeExtensions.sprx` is first loaded **after** ShellUI calls its
registration export. This explains the missing
`VshTmProgressEventShellUIModule` without inventing a JS module or modifying
the application bundle. Successful live correction has not been tested.

## Evidence

Verified 13.60 `system_ex_b/app/NPXS40087/eboot.bin`:

- Native registration wrapper at RVA `0xee70` calls `0xb752e0` at `0xee79`.
- `0xb7565f`/`0xb75667` copy the exact progress module name from RVA
  `0xfd2937` into a native string. `0xb7566f` installs the callable's vtable
  at `0x11b9050`; its invocation slot points to `0xb84ad0`.
- At `0xb756ab`, the name and callable are passed to PLT `0xd2e4f0`, whose
  relocation is NID `9h4kSOsmZAE`, library **libNativeExtensions**.
- Callable `0xb84ad0` creates the actual module through constructor
  `0xb84bb0`. This is a real native implementation already in firmware.
- Verified 13.60 `libNativeExtensions.sprx` exports `9h4kSOsmZAE` at
  RVA `0x54480`. It locks the mutex at `0x9c600` and inserts the supplied
  name/callable into the registry at `0x9c5c0` through `0x54540`.
- The staged eboot differs from firmware at only four bytes, the known
  resolution immediates (file offsets `0x166ad`, `0x166ae`, `0x166b1`,
  `0x166b2`). Its registration instructions are unchanged.

Run7 chronology in `_Runs/vshruntime7/_kyty.txt`:

1. Lines 15724-15725: eboot's registration import is unresolved and patched
   to a stub (GOT `0x9311fe2c0`).
2. Lines 51344 and 51357 (~4.898 s): that stub is actually called. These
   log entries are capped diagnostics, not a count of all registrations.
3. Line 167761 (~22.616 s): `libNativeExtensions.sprx` first loads, at
   base `0xe60000000`.
4. Line 167854: the registration import finally resolves to
   `0xe60054480`. Repairing the GOT does not replay earlier calls.
5. `_Logs/vshruntime7.log:2734`: Control Center's `getEnforcing` fails;
   lines 138-139 contain the earlier registration-stub calls.

NPXS40141 in the JS stack is the shared base bundle. NPXS40003 is the
failing Control Center application. The standalone diagnostic Hermes HLE
TurboModule stubs in `src/libs/libHermesVm.cpp` are not this native route.

## Bootstrap comparison and proposed minimal fix

`tools/run_shellui_1360.ps1:280-309` explicitly preloads Hermes, JSI,
RnpsBundle and other ShellUI prerequisites, but omits libNativeExtensions.
`RuntimeLinker::EnsureImportProviders()` currently knows only libmonosgen,
libSceJsc and libSceJsi-0.70 (`src/loader/runtimeLinker.cpp:4885-4889`), so
it does not fill that omission. Its existing JSC/JSI provider logic will
see libNativeExtensions once it is supplied at startup.

**Recommended narrow experiment:** add the real staged
`libNativeExtensions.sprx` to the runner's existing `--co-elf` prerequisites,
alongside Hermes/RnpsBundle:

```powershell
'--co-elf', "$stage\common\libNativeExtensions.sprx",
```

The existing startup order is provider discovery (`runtimeLinker.cpp:4039`),
relocation (`4044`), shared-module initialization (`4078`), then cohost entry
threads (`4086+`). Thus the real registry exists before eboot registers its
factories. No JS shim, module-name alias, registry replay, or guest patch is
needed for the identified failure. Parent owns this edit/build/live test.

The local4.03 reference `voldemort/zecoxao/kek/system_ex` eboot and
libNativeExtensions contain neither `9h4kSOsmZAE` nor the progress TurboModule
name, and neither contains `TurboModule` strings. Its `tools/run_shellui.ps1`
prerequisite list also omits libNativeExtensions. Therefore copying that
older preload list misses this newer 13.60 native registration requirement.
The managed 13.60 ShellApp assembly does contain the separate legacy
`ReactNative.Modules.ShellUI.BGFT.ProgressEventModule`; it is not a missing
assembly that needs to be fabricated.

## Parent verification checkpoints

- Real registration export resolves before the first ShellUI entry runs;
  no `9h4kSOsmZAE` unresolved-stub calls.
- The early loaded libNativeExtensions is reused when managed DllImport
  requests it, preserving one registry (existing hardlink identity fix).
- NPXS40003 passes this `getEnforcing` and renders, then PS/Home focus return
  is exercised. A subsequent service/module failure should be attributed
  independently rather than treating this fix as proof of full UI success.

Only this note and `_TempData/cc_rnps_research.py` were created. No production
edits, builds, firmware writes, or live emulator launches were performed.
