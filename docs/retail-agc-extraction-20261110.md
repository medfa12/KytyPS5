# AGC extraction from user firmware

## Runtime/setup integration checkpoint

Implementation `d948a709`, installed setup regression `e64c22e3`:

- Removed `src/libs/agcRegisterDefaults.inc` and its register/type payloads.
- `src/libs/agcRegisterData.h` owns extracted arrays and derived banks for the
  lifetime of the process. AGC initializes it lazily on the first defaults call;
  its function-local static initialization is thread-safe. Existing pointer-table
  initialization remains under the existing mutex.
- Public/internal version aliases, fallback to version 13, the public >=11
  table-0 transform and internal version-13 bank are preserved. Host pointer
  index metadata remains in code; register/type payloads come from user firmware.
- Installed setup checks/extracts the matched AGC module before writing runtime
  data. It creates `agc-register-data.json` under `--data`, reuses identical
  data, and rejects differing existing data without overwriting it.
- `--agc-module` supports an explicit input path; otherwise setup uses
  `firmware/decrypted_modules/libSceAgc.sprx.elf`. The PowerShell runner receives
  `-AgcRegisterData`, records it in prepared-launch metadata, and sets the child
  `KYTY_AGC_REGISTER_DATA` environment variable. Ambient developer AGC paths are
  removed from packaged setup dispatch.
- The installed extractor uses explicit LF newlines, matching setup on Windows
  and other platforms. Canonical output is 72,258 bytes, SHA-256
  `7086f76d484808c975a346b581d115f90bb5e0f811a1bab022a5a2900b2cb5b9`.
  The earlier extraction hash below reflects the previous Windows CRLF ending.
  Runtime checks exact size and an accidental-change checksum before parsing.
  This is not a new authentication policy; setup binds source identity by SHA-256.

### Verified results

From a clean external source clone with the pinned nlohmann dependency, compiled
`tests/agcRegisterDataTests.cpp` twice: once against the new loader, once with
`AGC_REFERENCE_INCLUDE` pointing to the preserved external original table.
Compared the complete serialized ABI snapshots (values, counts, pointer offsets,
table identities, register/type aliases) for both families, versions 0..13,
14 and UINT32_MAX. Result: identical 137,336 bytes, SHA-256
`44da405dc4cf2f68f5ebfa39b33d7587af0fe6801933b09271bfda0d7be611ef`.
Repeated lookup retained identical addresses. No GPU was required for this test.

Installed extractor output was accepted by the runtime test and reproduced the
same oracle snapshot. Missing, truncated, extended and three same-size mutated
data inputs were rejected before any snapshot output. The original table and
all extracted JSON/snapshots stay outside Git in
`Temp/opencode/retail-content-check-20261110/`.

Both full shader targets rebuilt successfully with production `agc.cpp`.
Focused CFG regression exit 0 with ten expected fixture names and no AGC
environment variable set. New CFG binary SHA-256:
`54871e0aa630a246c4e3ee596cf8d779e4dd7e48b0fb9588e42ad241c5b6da1d`.
GPU execution remains blocked by the already recorded host capabilities.

Installed tests passed:

- `retail_setup_package_tests.py`: synthetic dispatch and isolated paths.
- `retail_agc_setup_tests.py`: real user-module extraction, check-only no writes,
  prepare/launch dispatch, reuse, preserved mismatched data and missing-module
  rejection. Launcher detection/process launch are mocked; this is not boot.
- Real retail dump check/prepare/launch preflight: AGC accepted; the same four
  unrelated missing prerequisites were reported, no runtime directory created,
  installed file hashes unchanged.
- Existing PowerShell preflight, path-selection and URL-configuration suites.

The emulator/launcher Release rebuild and CMake install subsequently passed from
clean external source `d948a7090f420ef5af5c5c27919510f6928be6fb`.
Installed `kyty_emulator.exe --help` returned 0 with `KYTY_AGC_REGISTER_DATA`
unset, confirming startup/help does not eagerly require the extracted payload.
Installed setup, extractor and runner bytes match the built source; the package
contains no `agc-register-data.json`.

Installed binary SHA-256 values:

- Emulator: `40726df1817154513355b99077b62470e457397fecc2ddb03535c4e1842c343a`.
- Launcher: `82eaf259a664f5ff12cbb7a567d4165b41437e2c54a00d651558425bd563c87c`.

Evidence: external `retail-user-build-20261110/agc-install-verification.json`,
`agc-help-check/` logs, and build shell `sh_1c6a786e0001sGiVDQaOVaCdcU`.
This establishes build/install/help, not graphical launcher interaction. Complete
fresh-user preparation/boot, GPU acceptance, assets and publication-history
cleanup remain outstanding. No GitHub push occurred.

The register-data source is now verified against the user's alternate retail
firmware. No additional source attribution is needed to establish this extraction
path: the distributed tool contains offsets, lengths and an input digest, while
the user supplies the module. The extracted payload must stay outside Git and
release packages.

## Verified source and coverage

- Input: `decrypted_modules/libSceAgc.sprx.elf` from the user's retail dump.
- Size: 382,976 bytes.
- SHA-256: `a12b0f396a6464800a68006d6d64e00349a7b37fc3c1c08b6236e8ca564cbd6d`.
- All 67 directly initialized register/type arrays in
  `src/libs/agcRegisterDefaults.inc` match contiguous regions of that module.
- Total extracted array bytes: 62,084 (includes repeated/version-specific data).
- Comparison was DWORD-for-DWORD against independently parsed existing table
  initializers, not merely a successful extraction exit code.
- Deterministic extracted JSON SHA-256:
  `d43fbfb53d68750119bf93e2224f3c4e2b47b1bc55482cf04616007c6f9fd324`.

## Tool

```powershell
python -B tools/extract_agc_register_data.py --module "D:/Firmware/decrypted_modules/libSceAgc.sprx.elf" --check
python -B tools/extract_agc_register_data.py --module "D:/Firmware/decrypted_modules/libSceAgc.sprx.elf" --output "D:/KytyData/agc-base-arrays.json"
```

The output parent must already exist, must be outside the installation, and the
output file must be new. The tool validates the entire module before extraction;
it does not search for approximate opcode/data signatures or accept a different
module merely because the filename matches. `--check` writes nothing.

Verified rejection cases: missing input, truncated input, same-size mutations at
the first/middle/last byte, existing output, input-file output, and output inside
the installation. Existing output and input firmware hashes remained unchanged.
External evidence: `retail-content-check-20261110/agc-source-match.json` and
`agc-extraction-verification.json` under the session's external temporary root.

## Earlier extractor-only checkpoint (superseded above)

This extractor is not yet consumed by the runtime or installed setup workflow.
The embedded arrays have not yet been removed. The next change must add lazy
runtime loading with bounded validation and stable storage, wire extraction into
setup, then remove the literal arrays. Preserve the pointer-offset mappings,
public/internal version aliases and the existing version-12/13 transforms; the
67-array comparison alone does not validate those derived runtime structures.
Missing data must produce a clear prerequisite error without preventing normal
help/configuration operations. Verify runtime structures against retained external
fixtures before replacing the production path.

This supersedes the publication plan's request for additional AGC source inputs
for this exact firmware profile. Other profiles still need their own validated
module identities and extraction ranges. Fresh-user boot, GPU acceptance and
clean-history publication remain outstanding; nothing has been pushed.
