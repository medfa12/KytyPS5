# Retail firmware setup (experimental)

Current runtime status: mixed retail/devkit diagnostics pass the earlier
AutoMounter startup exception with native NP and SSL enabled, but the tested
Intel Arc system still displays a black surface at 0 FPS. Usable Home and
retail-only startup are not verified.

Supply decrypted RNPS files under `--rnps-source <directory>`, with one title
directory per app (for example `NPXS40141/host.ps.hbc`). Setup accepts bare
Hermes bytecode as `*.ps.hbc` or `*.ps.bundle.hbc`, normalizes the former to the
runtime filename, and omits the corresponding JS bundle sibling in its staged
copy. Conflicting plaintext variants and encrypted HBC containers are rejected.
Setup creates a separate RNPS input snapshot under your data directory; source
firmware and decrypted input files are not modified. It does not decrypt RNPS
or compile decompiled JavaScript back into bytecode.

For the hash-detected alternate retail profile, setup automatically selects
native NP startup and native SSL/libc initialization. Their exact firmware
providers are checked before preparation, then validated again by the runtime.
These options remove observed startup prerequisites; they do not establish
successful PSN authentication or working graphics.

Windows requires Python 3.10+ with `dnfile` (`python -m pip install dnfile`),
Windows PowerShell, and the installed emulator/runtime DLLs.

Setup extracts AGC register data from your own matching decrypted
`decrypted_modules/libSceAgc.sprx.elf`. Use `--agc-module <path>` if it is stored
elsewhere. The currently supported module SHA-256 is
`a12b0f396a6464800a68006d6d64e00349a7b37fc3c1c08b6236e8ca564cbd6d`.
Other module identities are rejected explicitly. The extracted
`agc-register-data.json` stays under your data directory; do not redistribute it.
The launcher passes its path as `KYTY_AGC_REGISTER_DATA`. AGC loads it only when
register defaults are requested, so help/configuration needs no firmware data.
An existing identical extraction is reused; a differing file is preserved and
reported as a prerequisite error. The data is UTF-8 JSON with LF line endings;
do not edit/reformat it because the runtime checks its exact size/checksum.

From the installed package, check your own extracted 13.60 firmware:

```powershell
python -B tools/setup_retail.py --firmware "D:/Firmware/13.60" --data "C:/Kyty/retail" --check
```

The command reports all missing setup inputs before creating any stage or store.
Supply `--url-config <urlconfig_cache.json>`, `--rnps-source <overlay-directory>`,
and, for the alternate retail profile, `--core-record <file>` and
`--ui-record <file>`. These inputs must come from the matching firmware source;
the package does not include them or derive them from an encrypted update.
RNPS overlays use per-title directories such as `NPXS40141`. Input checks verify
known launcher/record hashes and overlay structure; they do not establish the
provenance of supplied URL configuration or decrypted bundles.

Remove `--check` to stage and write a prepared-launch manifest. Add `--launch`
to prepare and start the emulator. Controller navigation is manual.
Use a fresh, short dedicated data path for a different firmware/profile.
Managed images, system data, logs, IPC and the emulator working directory are
placed under that path. Firmware and installation paths must not overlap it.
The tools use the selected Python interpreter and suppress bytecode caches.

This is a setup entry point, not a verified first-user Home release. Fresh-store
Home, application launch and PSN remain unverified. A successful prerequisite
check or preparation must not be interpreted as successful firmware boot.
