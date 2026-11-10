# Minecraft package inspection — 2026-11-09

## Input and extractor

- Original: `${USERPROFILE}/Downloads/UP4433-PPSA17221_00-MINECRAFTPS50000-A0144-V0144.pkg` (1,069,476,470 bytes), preserved.
- SHA-256: `0720e1207c0f2607d907a76f51f282c1b285c4031aee3214b1aa6f2c5da46919`.
- Source: <https://github.com/awake-devel/pkg-to-anyps5>, pinned commit `2e9b12267840f9777f8d825335c4b4938160d9c4`, version 0.2.5.
- Built locally with Rust/Cargo 1.99.1: `cargo build --release --locked --offline --jobs 2`. No dependencies or build script; no downloaded executable used.
- Locally built extractor SHA-256: `f7e15bbfb7bbb920b847a54cee40d4ad9114d7b639921a34b5011953cd5e2b61`; pinned tool checkout remained clean.
- Reviewed source input-range checks, FIH/CNT bounds, inner directory and block limits, path rejection (traversal, drive separators, Windows device names), SELF segment bounds, and invocation sites. Relinker invocation is opt-in; it was never selected. This was a targeted review, not a comprehensive security audit.
- `cargo test --locked --offline --jobs 2`: 57 unit and 13 CLI tests passed. The optional real-package integration test reports success without exercising a package unless its environment variable is set; it was not set. Actual package listing and extraction below were separate successful runs.

## Verified package findings

- `--list --jobs 2` succeeded before extraction. FIH format 3; outer PFS `0x10000+0x3d550000`, five inodes, 64-KiB blocks, explicit `PPRPLAIN-NOAUTH!` seed. This particular image's filesystem is readable without decryption.
- Inner image: 40,387 blocks (17,022 stored, 22,590 Kraken, 775 sparse); 35,245 regular files under `uroot`, including `eboot.bin`, bundled modules, and Minecraft assets.
- Full extraction with `--jobs 2` succeeded in 54.4 seconds into `${USERPROFILE}/AppData/Local/Temp/opencode/minecraft-stage-20261109`. About 15.8 GB was free before extraction.
- Metadata: `PPSA17221`, `UP4433-PPSA17221_00-MINECRAFTPS50000`, title `Minecraft`, content version `01.044.000`, master version `01.00`, required system software and SDK `0x1200000000000000`.
- Eight executable/module files were unwrapped as original PS5 ELF, not Windows-relinked output: `eboot.bin`, `sce_module/libSceNpCppWebApi.prx`, `sce_module/libc.prx`, `MediaDecoders.Prospero.prx`, `libPlayFabMultiplayer.prx`, `libRenoirCore.PS5.prx`, `libcohtml.Prospero.prx`, `libfmod.prx`.
- Five encrypted CNT entries were explicitly skipped: `license.dat`, `license.info`, `nptitle.dat`, `uds/npbind.dat`, `trophy2/npbind.dat`. No keys or decryption were attempted.
- The package supplies a full readable application filesystem at version 01.044.000; extraction did not request or use a separate base package. A formal base-versus-patch designation is not established by this extractor; the filename's version alone does not establish it. Completeness here means all listed application files, not a verified install entitlement or proof of standalone execution.

## Verification and placement

Independent verification completed and the ready application directory was moved to `${USERPROFILE}/Downloads/storeormediaapps/PPSA17221-app0`. The destination did not previously exist. Every listed application path was present; non-executable lengths matched the listing; Windows case-folded names had no collisions. All eight executable outputs passed ELF64 little-endian x86-64 type and program-header file-range checks (14 program headers each). These checks establish structural consistency, not complete ELF semantic validity.

- Listed application files: **35,245**, **1,298,054,848 bytes** before SELF unwrapping.
- Extracted asset/CNT layout before ELF merge: **35,263 files**, **1,028,194,776 bytes** (computed from the listing and 21 additional CNT files).
- Final Kyty layout: **35,266 files**, **1,327,547,429 bytes**, with SHA-256 inventory for every file.
- ELF outputs were moved onto the extracted `app0` paths, replacing the duplicate extracted SELF module copies. The directory was then renamed on the same volume; no redundant full application copy was retained.
- `eboot.bin`: 288,919,884 bytes, ELF type `0xfe10`, SHA-256 `b9fabdab2334b7216dd7ecd5591592ff40d7080f53fc7246b1088d0a27331b1c`. The seven modules have ELF type `0xfe18`.
- Two foreground inventory attempts timed out before placement; the resumable validation script subsequently completed in an unlimited background run. The final receipt was written only after successful validation, hashing and directory placement.

External audit artifacts use `${USERPROFILE}/AppData/Local/Temp/opencode/`:

- `minecraft-pkg-tool-20261109/` — pinned source and locally built tool.
- `minecraft-package-sha256-20261109.txt`, `minecraft-package-list-20261109.txt`, `minecraft-tool-tests-20261109.txt`, `minecraft-extract-20261109.txt` — original logs (PowerShell UTF-16).
- `minecraft-verify-20261109.py` — independent path/size and ELF-header/program-header validation, merge and placement script.
- `minecraft-final-inventory-20261109.json` — all final paths, lengths and SHA-256 hashes.
- `minecraft-copy-receipt-20261109.json` — exact counts, lengths, module hashes, metadata, skipped entries and final placement receipt.

## Remaining runtime blockers

Extraction is not a playability result. No guest launch was performed. Shell mounting/registration, the separately investigated LoginMgr blocker, module/API compatibility, graphics, input, saves, network services, and entitlement behavior remain untested for this title. Original PKG and firmware/13.60 trees are separate and unchanged; this task does not modify the media launcher, other applications, or authenticated stores.
