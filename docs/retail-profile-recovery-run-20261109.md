# Recovered native-profile run with six external applications

## Offline store recovery

After `384714a3`, the production copy-only recovery succeeded against offline
`retail-rpc1/sd`, publishing `Temp/opencode/retail-profile-recovered1`.
Parent verification rehashed all 43 source files, 42 recovered files, and three
backup files. Both recovered lists contain sixteen -1 IDs and retain the
original high-water/group words. The exact zero-profile config and native
empty nobackup directory are quarantined outside the guest mount.

Receipt SHA256:
`330296f2a39e84d0e7ac22718a4e5f5d5d6a13963fc0cbb7328431994e3477a1`.
The candidate remains `retail-profile-registry1/kyty_emulator.exe`, SHA256
`bd540d9f06a558dcecfa8ef072935ab9a5c1cec1f05c09da9e92e7f8f63dafab`.
The launcher clones this recovered seed into a new run and validates its
receipt/inventory before importing application records.

## Minecraft import correction

Minecraft's completed verification permits mounting
`Downloads/storeormediaapps/PPSA17221-app0`: 35,266 files, 1,327,547,429 bytes,
version01.044.000. Package/extraction evidence is documented separately.
The agent's documentation landed on shared ps5-vsh accidentally; it was
cherry-picked onto retail as `5d38d5a5` and reverted on shared as `fb9a25ba`.
No shared runtime code or unrelated artifacts were changed by this correction.

The first parent launch attempt (`retail-profile1`) stopped **before process
creation** because the importer required `displayLocation`, which Minecraft's
actual param.json omits. The retained failed directory was not reused.

`prepare_shellui_databases_1360.py` now derives Game-space bit2 for external
category0 games only when that field is absent. This is explicit offline
catalog placement using the verified accessor SPACE_GAME1/SHIFT1 constants,
not a claim about the native package importer's default. Supplied flags remain
unchanged; other absent-field categories still fail. Original manifests are
not rewritten.

The parent launcher also requests tables for native allocator start ID
0x10010000, alongside the existing companion-store preparation. Catalog tables
do not allocate or authenticate a user.

`retail-six-apps-validation1/verification.json` records a real isolated import:
six external titles returned for 0x10000001 and0x10010000, correct Minecraft
Game-space/deeplink, unchanged supplied media flags and Minecraft manifest,
all238 old rows in34 tables preserved, SQLite integrity ok. Launcher syntax
passed. No media playback or native Home success is inferred.

## Next runtime observation

The second launch preparation uses `retail-profile2`, normal audio, the
hash-pinned candidate, recovered cloned store, native SSL/libc, all15 audited
RNPS counterparts, corrected font mounts and six external titles. It remains
an explicitly mixed firmware comparison. User controls setup/navigation.
Native positive-ID creation, profile persistence across restart, Home and
genuine PSN authentication require runtime verification.

The launch completed: PID28864, creation FILETIME134386961545497906, with
the expected registry candidate path/hash. The retained launch receipt confirms
six external applications, recovery-receipt identity, native-provider settings,
normal audio configuration and new-run writable mounts/user-store path. A
subsequent process query found a responding NPXS40087 window (frame110).
Initial logs show ShellUI scene initialization; they also contain a
SharedStoreUriManager save error0x80020002 and SQLite path-logger0x81b80006.
These are recorded observations, not yet attributed setup blockers. No user
navigation or native profile creation has been verified in this checkpoint.

## User-directed local Home startup

The user deferred OOBE and requested loading a user with mounted apps/games.
When explicitly offered the working shared Home versus keeping retail, they
selected **keep retail firmware**. The next task is an isolated local-user
startup on retail; it must not be described as completed OOBE or genuine PSN.

Before preparing that run, PID28864 was already absent. No closure was
performed. `retail-profile2/checkpoint1` now contains verified complete copies
of its46 store files and logs. Frozen guest log SHA256:
`579694cb74272d5a0759373148e0b1cb160ba8dd64ac515006c3c99a0841ac91`;
host SHA256:
`7d8736fa4b2430c305014f2c9e5b055569b46216e27e80d1a240b40f812c5f6a`.

Both saved native user lists now contain first ID0x10010029, fifteen empty
sentinels, high-water0x10010029 and group0. This is persisted positive-ID
evidence; the initiating call, login state and usable Home were not established
by the parent file capture. The existing Astra medium profile task received the
complete frozen store/logs to use this actual profile and investigate the retail
startup flags, instead of unnecessarily creating a new one.

The external launcher now reads validated native roster IDs from its copied
store for app-list preparation. An isolated real import for actual ID0x10010029
passed: six external titles/twelve mounts, all288 prior database rows preserved,
SQLite integrity ok, and every non-catalog store file (including profile files)
unchanged. Evidence: `retail-saved-user-catalog-validation1/verification.json`.
Original profile2 and its frozen checkpoint were unchanged; this catalog-only
verification does not prove native login or setup skipping.
