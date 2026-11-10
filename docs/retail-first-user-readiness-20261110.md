# Retail first-user readiness checkpoint

## Packaged setup checkpoint

Source `e729410e` adds `tools/setup_retail.py`, an explicit CMake install
allowlist for ten runtime scripts, and installed `RETAIL_SETUP.md` instructions.
The setup entry point provides check-only, prepare-only (default), and launch
modes. It checks launcher identity, required directories, Python dependency,
URL JSON, RNPS overlay structure and alternate Core/UI record hashes before
creating runtime data. A dedicated external data path holds staging, stores,
logs, IPC and process working files. Developer reference environment variables
are removed from the setup child environment.

Verified using a committed-source archive and a CMake packaging-only harness
including the production `cmake/InstallRetailSetup.cmake`. `cmake --install`
installed the scripts and guide into the previously verified binary package.
This was an incremental tools install, not a new emulator build.

- Installed `--help` passed.
- Installed check, prepare and launch requests against the user's retail dump
  each detected `alternate`, returned exit 1 and listed the four missing inputs:
  URL configuration, RNPS source, Core record and UI record.
- All installed file SHA-256 values remained unchanged across those requests;
  no runtime data directory was created. Installation/data overlap was rejected.
- `tests/retail_setup_package_tests.py <install>` passed (one test covering
  check/prepare/launch dispatch, external paths and ambient-reference removal).
  This test uses synthetic input and mocked launcher detection/process dispatch;
  it is not firmware preparation or boot evidence.
- Existing PowerShell setup-preflight, path-selection and URL-configuration
  suites passed from the external committed-source archive.

The previous missing-scripts blocker below is resolved for CMake installs.
The launcher still has no graphical firmware importer. Complete preparation with
matching inputs, fresh-store Home/app acceptance and release-content review
remain outstanding. No GitHub push or release publication occurred in this
checkpoint.

## Clean build

A Windows Release build and CMake install completed from source commit
`781a8729894481d0dedfbadfee35cc741b0f1942`, using a clean isolated clone with
14 pinned submodules, an external build directory, clang-cl, Ninja and Qt 6.9.3.
The source clone remained clean. The installed emulator printed `--help`
successfully; no firmware was started, and this is not a runtime-acceptance or
compatibility result. This documentation-only commit does not change the tested
source.

| Installed artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `kyty_emulator.exe` | 35,651,584 | `be4a0edd480dd1613ccb3c7c469d46dbc08c49c2b1726f21b1ef3b89d0bc2c70` |
| `kyty_launcher.exe` | 5,250,560 | `0c44ff8b5373edaaa04d41abdee1f629d6b89b15ca179bfc77f646847bf92a27` |

The CMake install does not contain `tools/run_shellui_1360.ps1`; the launcher
does not yet offer a firmware-import/setup flow. Therefore this package is not
a first-user firmware release despite the successful build.

## Fresh-user blockers

- The runner now identifies the two known 13.60 launcher profiles and fails
  before staging if the required RNPS URL cache is absent. Alternate firmware
  does not silently inherit the regular workspace's URL cache. Cache presence
  is not proof of correct provenance or validity.
- Alternate VSH still requires source-bound Core/UI records; a fresh firmware
  folder alone does not provide a verified, supported setup route for them.
- RNPS staged content, native catalog/admission, app launch, and local Home
  need clean-store end-to-end validation. Prior mixed-object or copied-store
  experiments are not a clean first-user result.
- Genuine online authentication and PSN are not established. A release must not
  imply otherwise.
- SDK references, public certificate attribution, shorter instruction
  signatures, captured shader provenance, assets, submodules and historical
  blobs remain subject to release review. Removing two literal firmware-code
  arrays from HEAD does not remove their historical copies or prove the full
  tree distributable.

Keep test outputs and writable profiles outside the source checkout, never
alter the user's firmware or authenticated stores, and report actual boot and
user-observed navigation separately from staging and compilation.
