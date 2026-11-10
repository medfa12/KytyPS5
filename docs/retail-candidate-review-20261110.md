# Candidate content and dependency review

Reviewed candidate `2e1b2148d65fe861f703b1b5d9aebb3a24fc092b`.

The committed-tree scanner reports 1,560 entries (14 gitlinks), 24,076,625 blob
bytes, and only one executable-magic finding: the bundled winpthread DLL with
the previously recorded SHA-256. The two documentation-reference matches are
the readiness checklist and a Windows SDK field-macro comment.

## Remaining large-array findings

Read each of the nine scanner findings in context:

- Compute lines 25491, 29203, 29273, 30537, 30837 and 40033 are expected numeric
  results or input sentinels for arithmetic/conversion/atomic tests, not shader
  program captures.
- CFG line 12491 is the independently spelled-out expected serialization of a
  constructed SPIR-V module, including standard opcode headers and enumerants.
- CFG lines 2489 and 4091 mix encoder-built instructions with raw instruction
  literals. The latter explicitly identifies several boot/captured instructions.
  Nearby compute conversion tests also contain short captured instruction pairs.
  These findings remain open for individual treatment; absence of the 18 earlier
  full fixture sequences is not evidence that every captured fragment is absent.

No test assertions or instruction cases were removed to make the scan pass.

## Dependency packaging correction

The 14 submodules have exact gitlink pins. Additional FetchContent dependencies
Opus, OpenXR, Xbyak, Zydis, Zstd and ZArchive use full commit IDs; SQLite and
Noto font downloads have SHA-256 checks. These dependencies are separate from
firmware supplied by the user.

Review found missing installed notices for fetched Xbyak, Zydis and its Zycore
dependency, Zstd, ZArchive, and OpenXR's bundled JsonCpp. Added production CMake
install statements for their seven notice files (Zstd has two). A focused CMake
install harness executed those production statements plus the existing OpenXR
notice statement against the previously fetched pinned sources: all eight files
installed and matched source bytes exactly. Receipt directory:
`Temp/opencode/retail-candidate-build-20261110/notice-check/`.

This correction is a development change after the reviewed candidate; that
candidate must be refreshed before publication. This review does not establish
all binary corresponding-source requirements or all transitive notices (including
Qt deployment). A source-only candidate and a binary release have different
packaging contents to verify.

## Fresh candidate build

A new `git clone --no-local --recurse-submodules --jobs 4` was started from the
isolated candidate, with upstream submodule URLs, into
`Temp/opencode/retail-candidate-build-20261110/source`. Clone completion and the
fresh configure/build/install were pending at the initial checkpoint. The clone
subsequently completed: verified exact candidate revision, one root commit, clean
working tree, and all 14 recursively checked submodules at their pinned revisions.
Receipt: `retail-candidate-build-20261110/clone-verification.json`.
Fresh Release configure/build/install is now running as shell
`sh_1c6c63b10001lOiVh7Fro61RzV`; completion is not yet verified. No existing build
or populated dependency directories were copied into that checkout. This build
tests the recorded candidate, which predates the notice correction above.

Completion: that fresh Release build and install passed. Installed emulator
`--help` returned 0 with `KYTY_AGC_REGISTER_DATA` unset. External receipt:
`retail-candidate-build-20261110/install-verification.json`; help logs in
`help-check/`. Installed SHA-256 values:

- Emulator: `27176a80f216b0f09fb4f7009c7d7f7c72c22526a3a5f20e65d6438f7b452d67`.
- Launcher: `121f420b9fe696ace5814a56ba73b0bdb1f29467394df1c50fbaca607c364ac0`.

Build/install is no longer a pending result for this candidate. The notice
correction and unresolved content findings still require a refreshed candidate;
this result is not firmware boot acceptance.

## Runtime prerequisites

Fresh-store acceptance still requires matching URL configuration, decrypted
RNPS overlays, source-bound 136-byte Core/UI admission records, and a Vulkan GPU
with the required shared/image 64-bit atomics and fragment barycentrics. The AGC
module prerequisite is already resolved for the user's matched firmware.
Controller navigation and sign-in remain user-controlled. No fresh-store Home,
application, playback or PSN acceptance and no GitHub push are claimed.
