# Retail release-content audit checkpoint

## Captured shader build and regression checkpoint

Tested source: `6b267c6dbef05ab5a0d7b956e3a40463ff017534`.
Commits `39473d8c` and `e3a1ac46` externalized 17 additional captured inputs
(2,088 unique bytes); including the earlier runtime-buffer fixture, there are
18 external inputs. Their sizes, SHA-256 values and loader checksums were
verified, and the removed literal sequences were absent from both test sources.
Existing assertion counts were preserved: 2,172 CFG Check calls, 37 compute
CompareWords calls, and 1,620 compute Require calls plus one new graphics
capability requirement. See `tests/RETAINED_SHADER_FIXTURES.md` for inputs.

The fresh external source clone used all 14 pinned submodules and an external
Release clang-cl/Ninja build directory. After fixing the vector pointer
comparison, repeatable ZArchive patch application, and shared stencil-state
lifetime, both targets linked successfully:

```
[252/255] Linking CXX executable shader_cfg_tests.exe
[254/255] Linking CXX executable shader_recompiler_compute_tests.exe
```

Focused `--retained-shaders-only` results:

- CFG without fixture root: exit 2, required-input diagnostic.
- CFG with fixtures: exit 0, all ten expected fixture names loaded and checked,
  `PASS: retained shader CFG regressions`, no fixture skips.
- GPU without fixture root: exit 2, required-input diagnostic.
- GPU with fixtures: exit 3221226505 before any fixture load. Harness diagnostic:
  `no Vulkan graphics+compute device with fragment barycentrics and 64-bit LDS/image atomics`.
  Host Intel Arc reports `shaderSharedInt64Atomics = false` in vulkaninfo.
  GPU assertions were not executed; acceptance requires compatible hardware.

Binary SHA-256 values:

- CFG: `399b23bed3bae75cf71a1a3db24096491320f14b378f326a23ac6d9791e0a98c`
- GPU: `a2219bab04c7c358044bb0a1bd1644a2b359bd51c34a80941a6e4ee8578562d8`

External evidence is under `Temp/opencode/retail-shader-build-20261110/`:
`source-receipt.json`, `regression-results.json`, and per-run stdout/stderr in
`runs/`. Build log: OpenCode shell `sh_1c635a59f001TQA0u8uRUOxaJ5`.
The dependency patch contract separately passed pristine application, identical
reapplication, and rejection of conflicting input.

This supersedes the earlier unbuilt shader status below. Individual opcode,
register-table, asset and historical-content provenance decisions remain open,
as do fresh-user boot and application acceptance. No GitHub push occurred.

## Scope and result

Audited committed tree `f39334a91e0fffd3adf42630b0c02fa603bd7b65` with
`python -B tools/audit_release_content.py --ref <commit>`:

- 1,572 tree entries; 48,334,686 tracked blob bytes; 14 pinned submodules.
- The executable-magic scan found only the existing bundled
  `3rdparty/winpthread/bin/libwinpthread-1.dll` (90,084 bytes, SHA-256
  `78ebbc1480d62098aa66307db2ff139abc07c137bc188f5b73bf7b131492932c`).
  This scan does not identify encoded firmware or establish every blob's origin.
- The targeted reference scan now finds two non-proprietary-reference matches:
  the earlier readiness checklist and a Windows field-macro comment.
  Version-query API names and version values are not documentation excerpts.
- Nineteen large hexadecimal initializers remain in the two shader test files.
  These are candidates, including numerical test data; not all are copied code.
  The tool deliberately does not declare them safe or automatically remove them.

## Resolved in this checkpoint

1. Removed header/manual-derived prose, a field-name comparison table and a
   quoted controller-reference passage from the flagged research notes/comments.
   Existing independently described native observations remain. Where symbolic
   naming provenance is unresolved (Json/RNPS/video), it is explicitly recorded,
   rather than relabeling the old source as independent research.
2. Removed the confirmed 77-DWORD `RuntimeBufferRecords` captured shader literal
   from the test source. The original 308 bytes are retained outside the repo.
   The test now reads a checked, opt-in external fixture, with an explicit SKIP
   when no fixture root is supplied. Assertions and GPU dispatch are retained.
   See `tests/RETAINED_SHADER_FIXTURES.md` for the SHA-256 and input contract.
3. Independently fetched the pinned public certificate source identified in
   `tests/fixtures/esvm-certificate/README.md`, parsed it without execution, and
   matched its 802-byte signed root and the contained 398-byte production key.
   Source SHA-256: `115bcd7d11649a85c43ff0f2871f46f4201db5f870ac89448f60e6442a147d26`.
   The public certificate data remains; no private credential was found in these
   two findings. This is provenance evidence, not a legal assessment.
4. Verified all 14 populated external submodules against retail's exact gitlink
   pins; each has a root license/copyright file. Added installation of those
   notices plus bundled stb, cpuinfo/clog, winpthread and the project license.

## Verification

- Built `tests/retainedShaderFixtureTests.cpp` with clang-cl C++20 from an
  external committed-source snapshot. PASS: no opt-in skips; original retained
  input decodes; missing file, all 308 truncations and all 308 one-bit mutations
  reject. Two compiler deprecation warnings; exit 0.
- CMake packaging harness included the production notice module. All 19
  installed notice files exactly matched their source bytes. Also installed
  these notices into the existing external emulator/launcher package.
- Token comparison against `e6c8aa1d` verified executable tokens unchanged in
  libC, libRnpsBundle, libVideoDec2, libShellFocus and the certificate backend.
- The full GPU regression was not rebuilt or executed in this checkpoint;
  the fixture-loader result is not GPU execution evidence.

## Remaining release blockers

- Other captured shader blocks, including the annotated 69-word metadata fill,
  and shorter instruction signatures still need individual provenance decisions
  or external-fixture conversion. No whole-file copying claim is made.
- `agcRegisterDefaults.inc`, trophy sound and checked screenshots are inherited
  unchanged from the local `origin/main` ref. Equality establishes inheritance,
  not their original licensing/provenance. Register table first appears at
  `147188b1`; trophy sound at `412aba28`; sampled screenshot at `96db51ea`.
- Public certificate attribution does not settle licensing of all other assets
  or third-party components. Installing notices does not by itself satisfy
  corresponding-source or other license requirements.
- Git history still contains removed data. No history rewrite or force-push was
  performed, and the scanner does not audit history or submodule contents.
- Fresh-user preparation/boot remains unverified; this checkpoint does not
  establish Home, application launch or online authentication.

The tree has progressed, but a distributable release and GitHub push are not
yet verified. This report must not be used as a blanket clearance statement.
