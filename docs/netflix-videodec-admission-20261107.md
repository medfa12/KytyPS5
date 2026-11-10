# Netflix 20% stall: Videodec2 resource admission

## Proven failure chain

Investigation is isolated to `netflix-stall-investigation`, based on `427d9daa`.
No live process, profile, parent source or parent build was changed.

Retained run: `${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix1`.
`stall-evidence.json` records the original file hashes. User screenshot identifies
the authenticated title **KAMPEN MOD EN ORKAN**, stuck at 20%.

* `guest.log:970398–970417`: a 0x13800000-byte, type-12 direct allocation and
  mapping succeeds. This is not evidence of physical-memory exhaustion.
* `970428–970435`: next main-direct request has **length zero**, alignment
  0x4000, type 12, search range [0, 0x300000000).
* `1033733–1033742`: guest deliberate fault at app RVA 0x105f532, r14
  0x80020016 (EINVAL), r15 pointing at `vdec dmem`. The long interval consists
  largely of the app's failure-time virtual-memory inventory.
* App allocator 0x14fefc6 rounds its input to 0x4000 and calls
  AllocateMainDirectMemory at 0x14ff002. On error it enters fatal reporting
  0x105f430 at 0x14ff03c. The faulting store to address 0x1234 is deliberate.
* Caller 0x1503cac obtains AVC requirements at 0x1503e67 (helper 0x1506d44),
  optionally HEVC requirements at 0x1503e42 (helper 0x1505b26), and passes the
  larger **gpuMemorySize** to the allocator at 0x1503ebd. Both helpers return
  zero on a failed query; they do not propagate the query error to this caller.
* AVC helper constructs the size-72 config: resource is **0x12384 or 1**,
  codec 1, profile 100, level 40, dimensions -1/-1, DPB -1, input depth 1,
  null compute queue, priority -1, progressive optimization true. On successful
  query it returns memory-info+0x18 and copies +8 to its CPU-size output.
  Current HLE accepts only resource 1. The otherwise-valid AVC configuration
  returns a nonzero GPU requirement for resource 1, but rejects 0x12384 with
  **0x811d0203**, leaving the app's zero-initialized requirements unchanged.

Thus the kernel correctly rejects zero length; relaxing that guard would hide
the upstream Videodec2 admission error. The exact query result was not printed
in the captured run; its derivation is from the app instructions, HLE admission
and the observed zero-size request, not a claimed live return-value trace.

## Regular 13.60 native contract

`libSceVideodec2.sprx` SHA256:
`c380676887c7e1480c6068e75e5986fb11159f95f63dd818406390113ca40dfa`.

* Query export qqMCwlULR+E at 0xa10 enters 0xa20; config conversion and sizing
  are shared with creation through 0x3eb0.
* 0x3edd–0x3f41 explicitly accepts resource **0x12384**, converting it to
  VdecCore resource **8**. Resource 1 converts to 9 for PS5 (2 for PS4).
  Other private resources are visible but outside this correction's scope.
* Creation's resource dispatch at 0xf95–0xfdb accepts 0x12384 without the
  compute-queue validation at 0x10f4. Only resource 1 takes that validation.
  Both routes proceed to genuine decoder creation. Consequently accepting the
  query alone would immediately expose the HLE's unconditional queue check.
* Query fills CPU/GPU/shared requirements through VdecCore Zsh1K8YTD1E,
  adds 0x40000 CPU bytes, and derives frame bounds separately. Existing HLE
  conservative 16-MiB requirements are not a byte-exact native sizing model.
  No new native-size equivalence is asserted or required for admission.
* App creation at 0x1506be6 queries again, supplies actual CPU/GPU memory via
  helper 0x15061f6 (which also allocates any nonzero shared requirement), and
  calls CreateDecoder at 0x1506cdd. Its compute queue is created only for
  resource 1. This independently confirms the resource-specific lifetime.

Read-only audit used the retained ELF inspector
`${USERPROFILE}/Desktop/kytyps5/_TempData/lifecycle-re-inspect.py`, with bounded
`dis` ranges above, actual PPSA01616 `eboot.bin`, and regular 13.60 only.

App SHA256: `be42bd7ccc0b436ef0e4309d78266a9661c8827e1012b564c14d838bd5edf8d9`.
Repeatable local audit: `_TempData/netflix-native-contracts.py`; resulting
`_TempData/netflix-native-contracts.txt` SHA256:
`57ad07b54e9d644f9da90735989ecf5d19235a679c14e01f81567fee7e6d66f9`.

## Diagnosis checkpoint (f9dbe7a4)

Diagnosis established. Production correction and regressions are pending at
this checkpoint. No playback recovery or new live acceptance is claimed.

## Implementation continuation

`libVideoDec2.cpp` now admits the disassembled 0x12384 resource in the shared
query/create validation and requires a compute queue only for resource 1.
Both use the existing genuine FFmpeg decoder. The existing memory requirements,
allocation validation, decoder errors and decoded output are retained. ABI
structures moved unchanged into `videoDec2Types.h` so export-level regressions
can use the production layouts.

`VideoDec2ResourceTests.inc` tests the exact Netflix AVC query configuration,
unknown resource/codec rejection, config rejection, zero-length kernel rejection,
real direct allocation and mapping of queried GPU/shared buffers, insufficient
memory rejection, creation with resource-specific queue ownership, actual
synthetic AVC/HEVC red NV12 pixels and picture metadata, and decoder retirement.
Compute queue allocation/release is exercised through its real exports.
CTest registration: `videodec2_resources`; direct invocation:
`virtual_memory_allocation_tests.exe --videodec2-resources-only`.

The isolated incremental emulator/test build passed with Ninja -j2. Only the
changed production translation unit and test driver were compiled locally;
unchanged donor objects/libraries were borrowed read-only. This is not a clean
full build. Initial regression caught an incorrect test expectation (64x64;
the existing synthetic fixture is 64x48); corrected test passes as below.

### Verified results

* `_TempData/build-netflix-validation.py`: isolated emulator and test compile/
  link passed, **-j2**, log `_Build/netflix-validation/build.log`.
* `_TempData/run-netflix-validation.py`: final focused suite **exit 0**, log
  `_Build/netflix-validation/resources-final.log`. Both resources and both
  codecs produced actual 64x48 red NV12 pixels; AVC metadata was preserved.
  HEVC is allowed a bounded sequence of real IDR AUs for decoder reordering.
* Negative control links the same final test driver with the unchanged donor
  Videodec2 object (donor source compared equal to `427d9daa`). It **fails as
  expected, exit 1**, at the first exact Netflix configuration query with
  **0x811d0203**. Log: `_Build/netflix-validation/resources-baseline.log`.
  This proves the test detects the original admission gap, rather than merely
  exercising a decoder below that boundary.
* `git diff --check` passed. CTest registration was added; these executions
  used the direct focused command, not a full CMake/CTest reconfiguration.

Isolated executable `_Build/netflix-validation/kyty_emulator.exe` SHA256:
`d2145858be2489900cb8d2572ed1fee127d3d232b4453e4713dff904d5d781f1`.
Test executable SHA256:
`bab1129782e62eb8a33a8c704c828358ee516c89a43e24513cc8f2888d7db487`.
No emulator application was launched. Existing VSH and profiles remained
user-controlled throughout; no process input or control was performed.

## Live acceptance still required

Coordinate with the parent/user before any Netflix launch. Re-test the same
authenticated title using the corrected build and preserve launch identity,
guest/host logs, and visible progress. Verify creation proceeds beyond the
former zero-length allocation and establishes sustained video and audible
audio. If another error appears, capture its precise call/configuration and
trace that boundary. Clear synthetic frame decoding does not establish licensed
title decryption, presentation, audio playback or sustained Netflix playback.

## Parent integration validation

Integrated on `ps5-vsh` as `8b601a65` and `6ff5521e`. Normal CMake target
`virtual_memory_allocation_tests` built successfully with parallel2, and CTest
`videodec2_resources` passed (0.20s; total0.28s). The production emulator has
not been relinked with this change while the user's VSH psn36 is running.
Actual authenticated Netflix playback beyond20% remains pending.
