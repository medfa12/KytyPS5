# Media applications requested for the next retail run

The user requested media mounts on the next run and asked whether the Minecraft
package in Downloads could be unpacked beside them. The current emulator stays
open at Data Collection while the separate LoginMgr investigation continues.

The external parent launcher `Temp/opencode/replay-strict-retail.py` now defaults
to `Downloads/storeormediaapps` as its external application root. It follows the
existing `run_shellui_1360.ps1` recipe: validate unique title IDs, manifests and
icons, then mount each app at `/user/app/<title>` and its `sce_sys` directory at
`/user/appmeta/<title>`. It records manifest/icon/executable identity and whether
the expected executable is plain ELF. It dynamically includes new extracted
directories that have genuine `sce_sys/param.json` and icon metadata.

Current inventory:

| Title | Application | Expected eboot.bin |
| --- | --- | --- |
| PPSA01616 | Netflix | ELF present |
| PPSA01651 | YouTube | ELF present |
| PPSA01783 | Apple TV | ELF present |
| PPSA01667 | Twitch | Missing; metadata-only folder |
| PPSA05688 | Spotify | Missing; eboot2.bin is not silently renamed |

The existing `prepare_shellui_databases_1360.py` imports actual descriptors into
the new run's copied database only. It preserves existing rows and supplies
per-user app-list tables without registering/logging in users. This is the
existing offline AppDb support, not proof of native AppDb service behavior.
The first isolated check passed SQLite integrity, five external title rows and
ten mount mappings; the user-store bytes were unchanged. Its seed had no app.db,
so preservation of preexisting app rows was not established by that first check.

The launcher also copies the three existing per-title data directories from
`kytyps5/launches/vsh/title-data` into the new run's child-launch data root,
verifying source and copy hashes/sizes. It retains the original media stores.
A second isolated check exercises existing-row preservation and these copies;
its results are pending. No new emulator run or media playback is claimed yet.

The second check passed: all 238 existing rows across 34 app.db tables were
preserved, five external titles and ten mappings remained correct, and SQLite
integrity was `ok`. All three existing title-data directories were copied and
source/copy byte hashes reverified (one file each in this source). Evidence is
`retail-external-apps-validation2/verification.json`. Launcher syntax passed.
These copies preserve the available source data; current authentication or
successful playback in the upcoming retail run is not inferred from them.

## Minecraft package inspection

Requested file:
`Downloads/UP4433-PPSA17221_00-MINECRAFTPS50000-A0144-V0144.pkg`

The file exists, is 1069476470 bytes and starts with `7f464948` (FIH). This
matches a format handled by the open-source `awake-devel/pkg-to-anyps5` tool;
its README documents support for unencrypted debug FIH/PFS/NAPS content.
Header magic alone does not establish extractability or complete game contents.
An independent Astra medium task is inspecting the package and reviewing the
source-pinned extractor before any isolated extraction. Original PKG, media
applications and firmware trees remain retained. Minecraft compatibility is
unverified even if extraction succeeds.
