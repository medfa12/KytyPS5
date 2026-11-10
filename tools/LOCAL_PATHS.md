# Local paths

ShellUI launch is configured at runtime, without a workstation-specific layout.
Select the extracted 13.60 firmware with `-FirmwareRoot` or the process environment
variable `KYTY_FIRMWARE_ROOT`. Explicit arguments take precedence. The directory
must contain `system_b` and `system_ex_b`; missing configuration fails before staging.

```powershell
$env:KYTY_FIRMWARE_ROOT = (Resolve-Path './firmware/13.60').Path
./tools/run_shellui_1360.ps1 -PrepareOnly
```

The launcher identifies known images by hash by default. To inspect a dump
without creating stages or stores, run:

```powershell
python -B ./tools/prepare_shellui_managed_1360.py --fw-root './firmware/13.60' --detect-profile
```

`-LauncherProfile active|alternate` explicitly selects an audited image profile.
Alternate boots additionally require separate `-ManagedStage` and `-SystemDataPath`.
Firmware selection does not select or copy a saved user store.
The URL cache must already exist in the selected store or be provided explicitly
using `-UrlConfigReference`. Alternate boots do not inherit the active workspace
cache; an explicit reference must come from the correct source. The runner checks
availability before staging, but does not establish cache provenance or validity.

Validation checkpoint 2026-11-10: nine launcher-profile tests passed in an
external source snapshot, including both local 13.60 profiles and rejection of
unknown inputs. The existing PowerShell path-selection tests also passed. A
fresh retail dump was identified as `alternate` and correctly requested isolated
paths. Isolated URL-cache, input-path and setup-preflight PowerShell tests passed,
including rejecting a workspace cache in alternate mode. A fresh retail preflight
reported its missing cache before creating a stage or store. No ShellUI or PSN
boot was performed, and presence of a cache does not prove its provenance.

Optional input overrides:

| Launcher option | Environment variable | Default |
| --- | --- | --- |
| `-VshAssetPath` | `KYTY_VSH_ASSET_PATH` | Selected firmware's `system_ex_b/vsh_asset` |
| `-UrlConfigReference` | `KYTY_URLCONFIG_REFERENCE` | Existing selected store; active profile may use workspace cache |
| `-NotificationReference` | `KYTY_NOTIFICATION_REFERENCE` | No reference database copied |

Python staging tools accept `--fw-root` (managed) or `--firmware` (databases),
falling back to `KYTY_FIRMWARE_ROOT`. Read-only inspection tools use that same
variable unless they accept explicit root arguments. Metadata comparison uses
`KYTY_ALTERNATE_FIRMWARE_ROOT` and an explicit `KYTY_KEY_DICTIONARY`.

For an isolated local/test catalog, `-SystemApps NPXS40037,NPXS40047` opts into
the actual Plus and Store metadata descriptors. This imports local rows only;
it does not establish native catalog admission, app visibility or PSN access.

Frame dumps remain opt-in through `KYTY_RN_DUMPFRAME`. Set
`KYTY_RN_DUMPFRAME_PATH` to choose their output file; otherwise `frame0.rgba`
is written in the process working directory. The parent directory must exist.

Keep personal absolute paths in your process environment or external launch
scripts. Generated stores, receipts and logs can contain local paths and should
remain outside tracked source. Documentation examples should use relative paths
or descriptive placeholders, never a developer's home directory.
