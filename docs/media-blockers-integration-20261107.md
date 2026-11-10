# Media blocker review and integration — 2026-11-07

## Reviewed agent checkpoint

The original media agent's commits `81ba97c1` and `83a17bcf` contain tested
AC3/EAC3, AVC/HEVC and 10-bit SDR support, Netflix's native TLS/callback fixes,
reservation protection, and Apple native-contract evidence. They were absent
from `ps5-vsh`; they are now integrated there as `024600c7` and `158bd7d6`.
The original dirty media worktree and inherited edits were preserved.

An isolated incremental build based on current parent objects passed, using
the original agent's real AC3/EAC3-enabled FFmpeg package. Five focused suites
passed: decoder, source (real SDL audio acceptance), guest FS TCB, guest thread
cleanup, and media memory protection. This is an overlay validation, not a
clean full build. A normal parent CMake build is separately underway with
`FFMPEG_PREBUILT_DIR` pointing at that package.

The outstanding media boundaries are:

- Apple: HLE source callbacks never deliver the recovered native readiness
  event; a dedicated agent is tracing the full queue/control semantics before
  implementing it. Frames queued by the decoder alone do not prove playback.
- Protected Apple content: PlayReady access is unsupported; later native
  MediaKeys lookup fails before backend session creation. No working CDM,
  license-session or sample-decryption provider is currently installed.
- Twitch/Spotify: previous standalone probes bound PSM Run to a host parking
  stub. The actual firmware module and managed application were not loaded.
- YouTube: earlier sponsored-video visibility is verified; sustained main-video
  playback and its later loading state have not been attributed to a backend.

## Native web-launcher probe

The bounded `mweb1` Twitch probe loaded the original NPXS40001 launcher,
libScePsm, Mono, PsmUtil, mscorlib and the actual web-launcher app.exe AOT image.
The managed PE was extracted to isolated staging with the existing extractor;
the title-specific app0/metadata stayed separate from the launcher root.
No controller input was sent. Evidence is under
`${USERPROFILE}/AppData/Local/Temp/opencode/mweb1/`.

The Run import bound to native PSM+0x238130, not the parking stub. Host logs
record `Initialize WebLoader2` and `UISystem.InitializeTopMost`. The process
then aborted at PSM+0x3c48a with `PDefaultMemoryAllocator:203 P_ASSERT_OK failed
(-3)`. This is progress beyond the old stub, not successful web UI/playback.

### Demonstrated compositor ABI defect

`mweb1/guest.log:23193` records InitWithProcessOrder arguments
`{0x400000, 0xe00000, 0, 2}`. HLE wrongly treated the first argument as order.
At lines23195-23200, its getters treated stale argument registers as output
pointers, then returned zero or -1. PSM uses their return values as arenas.

Fresh regular13.60 disassembly establishes:

- Composite+0x4d90 takes three byte sizes followed by an eight-bit process order;
  +0x4e15..+0x4e55 validates order <=3, each size <=0x20000000 and duplicate init.
- Getter NIDs `T6CVkdCDO7o`, `N6ID0KNnzY8`, `bxt+muwit0w`, `FTQCTDU0b4g`,
  `ReHQrFnGVxA`, `5x5tC7zLwq4` at +0x3f50..+0x3fa0 take **no arguments**.
  They select one of three arena descriptors and tail-call +0x8220/+0x8230,
  which return the 64-bit address/size fields directly.
- PSM+0x2ecea..+0x2ed70 consumes these direct values. Its arena constructor
  +0x3cd20 rejects null base/size, explaining the observed -3.

The local fix implements typed initialization and all six direct getters,
actual bounded aligned arenas, atomic publication after allocation, native
validation errors, duplicate-init preservation, and quit/reinitialization.
Allocator arenas are kept outside the pixel-surface registry; arbitrary pool
contents must not be presented as a completed frame. Targeted export-level
tests and a second native launch probe are pending at this source checkpoint.

## Production validation and successive launch boundaries

The pool changes are integrated on `ps5-vsh` as `5eb03f52` and `2eb1af94`.
The normal `_Build/windows` production emulator and virtual-memory test builds
passed. CTest `composite_memory_pools` passed (0.11s); decoder, guest FS TCB,
media memory protection, guest thread cleanup and source/real SDL PCM suites
also passed against production-build binaries.

The isolated probes use `tools/probe_web_psm_1360.py`. It records the executable
hash, arguments, relevant environment and copied routing-cache hash, rejects
existing run directories, and leaves profile storage isolated. Regular firmware,
existing extracted common assemblies, title metadata, executable and routing
cache are explicit arguments. Recorded RNPS plaintext replay is explicitly
selected using `--rnps-replay-dir`; it is not a new decryption implementation.

Evidence is in `${USERPROFILE}/AppData/Local/Temp/opencode/mweb2` through `mweb5`.

- `mweb2` passed the old allocator assert. Its guest log records actual
  system/video pools of 4194304/14680064 bytes with order2. Managed execution
  passed `UISystem.InitializeGraphics`, system-texture preload and
  `InitializeBeforeRendering`. It then failed resolving `webapplauncher:main`
  because the isolated run lacked `urlconfig_cache.json` (`0x80ba8082`).
- `mweb3` copied the existing routing cache, selecting genuine
  `rnps-web-launcher`/NPXS40051. It reached RNPS application creation, then
  stopped at the disabled RNPS plaintext-replay gate (`0x80940001`).
- `mweb4` enabled the existing recorded-plaintext directory and evaluated
  Hermes bytecode, activated StyleManager, and switched focus to
  `Render.NPXS40051 : AppScene`. It then failed decoding the UI3 focus-noise
  image (`0x80b35508`). VshPng imports were unresolved in this reduced setup;
  the next probe adds the same native VshPng module used by the VSH launcher.
  Compositor-client disconnection messages are a separate unresolved boundary.
- `mweb5` loaded native VshPng and passed the focus-noise image failure. The
  first managed startup failure is now `DllNotFoundException` for
  `/dl_sysmodule:SCE_SYSMODULE_INTERNAL_CLOUD_MESSAGING` during native-module
  registry initialization. The firmware module exists and the host sysmodule
  ID table names it; the PSM load/resolve contract still needs tracing.

These logs verify startup progress, not visible Twitch UI or video playback.
The mweb4 screen-region capture was occluded by another application and is
not visual evidence. No controller input was sent. The later Mono host fault
at +0x23cd44 follows managed exception termination and is not the first failure.

### CloudMessaging load contract

`mweb5/guest.log:83637..83642` shows PSM calling internal load `0x800000c6`
then requesting its handle, repeatedly. The load registry treated this ID as
HLE-only success, whereas handle lookup required an actually loaded provider.
The regular13.60 `libSceSysmodule.sprx` record at +0x27440 contains ID
`0x800000c6`; its name relocation +0x27450 points at +0xfc71,
`libSceCloudMessaging`. The native provider is present in `system_b/common/lib`.

Add that provider to the existing native module load/start and retirement
registry. This propagates missing-provider/start failures and returns a genuine
kernel module handle. Extend the existing sysmodule fixture to verify the
missing-provider internal call does not publish success, and the native load
seam returns the canonical provider's handle. Build/test/live validation of this
additional fix is pending.

The CloudMessaging build and `sysmodule_native` regression passed. In `mweb6`,
the firmware image loaded and started; guest.log:84961 reports actual handle74,
followed by its real initialization opening `SceCloudMessaging` IPC. This
verifies load/resolve progression, not a working cloud-service backend (the IPC
connect still uses HLE). The next managed exception is the same missing-provider
contract for RNPS AppMgr (`0x80000076`). Its native sysmodule record +0x25b80
has name relocation +0x25b90 -> +0xf9e5 (`libSceRnpsAppMgr`), also confirmed by
record +0x1cb00. Add this observed provider to the same registry and make both
handle lookups share the loader's canonical paths. Combined validation pending.

The window-specific `mweb6/window-0.png` capture is black; it does not establish
a usable UI or video presentation.

`mweb7` with both native providers reached `MountApp runApplication` and Hermes
`Running "Main"` without the previous missing-DLL exceptions. It remained
black in a window-specific capture and continuously reported compositor-client
disconnection. It was closed gracefully through its own window before rebuilding.

The new provider mappings retain full internal IDs in the registry, avoiding
aliasing public IDs 0xc6/0x76. Existing legacy low-half mappings remain compatible.
The final production rebuild and `sysmodule_native` test passed, including
missing-provider errors, actual loader-handle propagation and public/internal
reference separation. Final live probe `mweb8` confirmed actual native handles
for both internal IDs (guest.log:86847/87152), reached RNPS `runApplication` /
`Running "Main"` (host.log:159/161), and showed no managed unhandled exception
in the captured log. Compositor-client disconnection persisted. The probe was
closed gracefully through its own window; no controller navigation was sent.

## Apple readiness implementation review

Agent checkpoint `2bec4edd` is on `media-readiness-integration`, not promoted to
`ps5-vsh`. It introduces source event0x2001 delivery, bounded/reentrant dispatch,
payload-AU accounting, and EOS-before-codec-EOF readiness tests. Native evidence
now identifies the selected HEVC count producer and wire-EOS admission path.

Parent review identified two outstanding semantic gates: the adapter arms Pause
at source admission and Sync at first output before matching the complete native
initial setup sequence; its standalone timestamp invalidation model is not wired
to production observations. The agent has resumed tracing these gates.

The isolated overlay build passed for checkpoint `2bec4edd`, recompiling the
source/decoder and header consumers (`libMediaFramework.cpp` and
`mediaFrameworkEsvm.cpp`). Both `media_framework_esvm_decoder_tests.exe` and
`virtual_memory_allocation_tests.exe --media-framework-esvm-source-only` passed
with exit0, including the new readiness regressions. This is overlay validation;
it does not establish native arming equivalence or Apple playback. The agent was
notified that editing can resume. Production promotion remains gated on review.

### Revised readiness integration

Revision `2cfc9d75` removes implicit admission Pause and generation Sync, arms
first-output synchronization only after successful source Flush, and observes
normalized PTS on actual output retrieval. Parent spot-checked the constructor
tail, Play consumer, successful-Flush branch and Timestamp invalidation against
regular13.60 native disassembly. The decoder's new `FlushCurrent` preserves the
real sequence-validity state, permitting initial source Flush without inventing
an AU sequence.

The revised isolated build and both decoder/source-readiness suites passed
(exit0). The audit and implementation are now integrated on `ps5-vsh` as
`a602a7d6`, `f3ef6055`, `d2710e6b`, and `384b9791`. Normal-build targets
`virtual_memory_allocation_tests` and `media_framework_esvm_decoder_tests`
passed. Integrated CTest `media_framework_esvm_source` (3.16s) and
`media_framework_esvm_decoder` (0.12s) passed, 2/2, total3.36s. The production
emulator executable has not been rebuilt with readiness yet: the Netflix
session launched as PID13596 uses the previous executable and was not restarted
or replaced for Apple validation.

Live Apple readiness and hidden HEVC reorder coverage remain unverified. This
patch implements the evidenced notification contract with conservative queue
accounting; it does not demonstrate sustained playback or supply PlayReady.
