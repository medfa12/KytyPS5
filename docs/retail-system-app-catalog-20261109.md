# Retail Store and Plus catalog investigation

The `retail-complete-roots1` run used one verified root mount containing the
complete 1,703-file mixed RNPS stage. The actual mounted Store (`NPXS40047`,
elysion) and Plus (`NPXS40037`, service-hub) bundles were plaintext Hermes.
Home, Library, and several other RNPS apps reached `MountApp runApplication`.
No Store/Plus launch was observed, and the user reported their absence. Mounted
files and running Home do not establish that these tiles were delivered.

The run's offline importer invocation imported only Library and six external
titles; neither Store nor Plus was in the live per-user icon table. The real
retail descriptors provide Store `pshome:gamestore`, display location 146,
and Plus `psplus:browse`, display location 147. The catalog importer now has
repeatable, opt-in `--system-app NPXS40047` / `--system-app NPXS40037` options;
`run_shellui_1360.ps1` exposes them through `-SystemApps`. Existing defaults do
not change. Inputs are the original descriptors and icons; no fabricated PSN
state, entitlements or native installation are written.

An offline experiment copied the untouched prepared seed's MMS files, imported
both options alongside the six external apps, checked `integrity_check=ok`, and
verified the two exact rows/flags/URIs. Source hashes remained unchanged. The
copy and its import log are preserved externally as `retail-system-catalog-offline2`.
The earlier all-development-descriptors experiment failed on a missing staging
manifest and was not used; it is retained separately as offline1.

Native admission is a **separate known blocker** in the actual run: Core
AppInfo and AppBrowse reported corruption (`0x80f50061`), AppBrowse transaction
calls failed `0x809c0103`, and WebAPI requests returned `0x80553407`. The host
reported signed-in `no` and NP request errors. Adding catalog rows does not
remove corruption markers, establish native recovery, deliver tiles, connect
PSN, or launch Store/Plus. Do not claim end-to-end availability based on the
offline rows. `tools/test_retail_catalog_display_1360.py` (12 cases),
`tools/test_retail_installed_catalog_1360.py` (12 cases), and the launcher
path-selection test passed. Live visibility has not yet been tested with these
two opt-in rows; native database recovery remains unresolved.

## Isolated runtime retry

`retail-system-catalog1` was launched as PID 25596, using the same pinned
candidate, corrected local/test profile roots and full mixed RNPS mount. Its
copied-store importer reported 10 descriptors for the saved user; a read-only
query confirmed Store and Plus rows and their original display flags and URIs.
The window initially responded at frame 122 / 2 fps. The experiment is limited
to importing those two real descriptors; native recovery is unchanged. Whether
the UI displays or launches either item remains user-unverified.

User later confirmed Store and Plus tiles appeared and asked about other apps.
The running copied catalog currently has ten rows: Store, Plus, Game Library,
App Library, Netflix, YouTube, Twitch, Apple TV, Spotify and Minecraft. Their
presence as rows/mounts is not proof they appear or launch. The mounted RNPS
inventory contains 71 app manifests; most represent dialogs, overlays, background
functionality or internal/test modules rather than Home tiles. Twelve title
directories contain 21 built-in app descriptors, including Welcome, Media
Gallery, Discover and All Apps, as well as debug and conditional disc entries.
PS Now player `NPXS40043` is in the stage but has no standalone appdb tile
descriptor. Selection and service availability remain separate questions; no
additional tiles or fake streaming services were added in this observation.

## Native system-app mount audit

At the user's request, the `retail-system-catalog1` launch receipt was checked
against the retail firmware paths using longest-prefix mount selection. All 13
native `system_ex_b/app` directories with executables, excluding Disc Player,
resolve to the exact original files through the existing `/system_ex` mount.
Remote Play `NPXS40074`, Share Play `NPXS40099`, Game Streaming `NPXS40106`,
Share Video Transcoder `NPXS40146`, Discord Daemon `NPXS40210`, HRTF
Personalization `NPXS40250`, and the other native service executables were
all plain ELF at audit time. The complete per-file path, size and SHA256 report
is external `retail-system-native-mount-audit1.json`.

No duplicate mount, application launch, PSN assertion, user-input event, or
restart was introduced. Being mounted and ELF-readable does not establish that
the shell launches these programs or that networking/streaming works.
