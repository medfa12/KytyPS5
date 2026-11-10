# Retail initial-setup URL preparation (2026-11-08)

Strictretail5 reached `InitialSetupStart` but resolver requests for
`psinitialsetup:launched` and
`pssettings:play?function=initial_setup&phase=1&onPhaseFinish=false` failed
with `80ba8082`. Guest lines 283941–283942 and 284989–284990 show why: the
current HLE cannot find `urlconfig_cache.json` in the mounted system-data
cache and then its workspace fallback. The later `CreateRnpsPhase` null
exception is downstream evidence, not proof of authentication failure or the
only remaining Home gate. The SharedStore exception is a separate observation.

The existing working launch preparation is `tools/prepare_shellui_urlconfig.ps1`.
It preserves an existing target file and copies an explicitly selected source
only when missing. The usable workspace cache at
`${USERPROFILE}\Desktop\kytyps5\_SystemData\priv\cache\urlconfig\urlconfig_cache.json`
has SHA256 `4558619e510719600ef7dcd6d20a1977405ac0d05accad4604620767c1be72dd`.
Its provenance is **not established as retail**. Supplying it is therefore an
explicit mixed/unknown-provenance experiment, not a strict-retail run. Do not
overwrite a working or authenticated store or silently reuse another tree.

The actual production-object resolver was tested read-only against a private
copy of these exact bytes mounted at `/system_data`: the settings request above
returned `0` with `{"rnps":{"component":"Main","titleId":"NPXS40008"}}`.
The original retail `system_ex_b/rnps/apps/NPXS40008/manifest.json` independently
identifies `rnps-settings`, title `NPXS40008`, version `13.6.0+57942`, SHA256
`2af65f7e817dc2ff24f98af6fe3430965c2b9e7ddda4350a278a7464bbc6beea`.
`tools/check_retail_rnps_route.py` checks this route/manifest match and reports
source hash without staging anything. Its unit tests reject a missing route and
a mismatched manifest. Existing `tests/shellui_urlconfig_tests.ps1` passed for
missing/existing-target preservation and explicit source fallback.

This cache **does not contain** a `psinitialsetup` route: the same production
resolver test returned `80ba8087` for `psinitialsetup:launched`, not success.
The original retail settings bundle begins `RNPSHEDR` (SHA256
`1cb21ba1f3f2070743802a5dc9762a3ee5f0d9041597e37f1dc3702679af4fad`);
the emulator's bundle path fails closed without a supported key/plaintext.
Matching an app name/title is not proof that the bundle will execute. Retail-
derived compatible decoded bundles or existing storage methods may be used
when independently verified; no cross-tree sidecar or replay was supplied here.
Strictretail5's launch receipt already mounts `/system_ex/rnps` from
`retail-strict1/rnps`; its staged `NPXS40008/manifest.json` and
`application.ps.bundle` hashes match the retail originals cited above. The
same route check passes against this actual staged app directory. The mount
therefore contains the matching encrypted settings bundle, but this is not a
successful decryption or application launch test.

## Explicit preparation for a parent-owned experiment

For a disposable, parent-selected system-data directory only, first run:

```powershell
python tools/check_retail_rnps_route.py --cache '<explicit-cache.json>' --retail-apps '<retail-firmware>/system_ex_b/rnps/apps'
```

After reviewing its SHA/provenance and output, the existing preparer can stage
that exact cache without overwriting an existing target:

```powershell
./tools/prepare_shellui_urlconfig.ps1 -SystemDataPath '<isolated-system-data>' -WorkspacePath '<nonexistent-workspace>' -ReferencePath '<explicit-cache.json>'
```

Mount that isolated directory as `/system_data` using the existing launch
configuration. Label the run mixed/unknown if the cache lacks verified retail
provenance. This is a verified settings-route preparation, **not a verified Home
boot**: the missing initialsetup route and actual settings-bundle execution
must still be observed. No local user/profile, setup-complete state, event,
authentication or app response was created or copied by this analysis. No
guest process, parent runtime store, or launch was accessed or modified.

## Parent cache-only runtime comparison

The parent tested the same M2-error-preserving executable with only the
hash-verified workspace URL cache added to a disposable system-data directory.
The manifest labels this `retail with unknown-provenance URL cache comparison`.
Original retail RNPS staging and disabled replay were retained.

The settings URI now resolves to `NPXS40008/Main`, and the guest reads its
retail manifest successfully. The missing `psinitialsetup:launched` route
reports `0x80ba8087`, but processing continues to the Settings scene factory.
The next observed fatal gate is shared-host preload: the guest opens
`/system_ex/rnps/apps/NPXS40141/host.ps.bundle.hbc`, reads an `RNPSHEDR`
container, and the loader returns `0x80940001` because no key is available and
replay is disabled. `JSExecutorPreloadHostDll` fails and the RNPS factory throws.
This is before demonstrated execution of the Settings application bundle;
the failure must not be attributed solely to that application's container.

No Settings UI, Home boot, or genuine PSN authentication was established.
Immutable logs and manifest are retained locally under `retail-cache1/checkpoint1`.

The user subsequently confirmed that the shine background began moving briefly
and then froze. The parent verified the same process remained responsive as a
window but stopped at frame 106 / 0 fps after the RNPS exception. This is visible
partial background rendering, not a completed initial-setup or Home boot.

## Follow-up: actual caller and plaintext attribution

The strictretail5 host log continued from the failed `psinitialsetup:launched`
lookup (706) into InitialSetupPlugin load and the separate settings lookup
(710–712). Thus that first lookup failure was **not immediately fatal in this
run**; its intended notification/launch semantics are not established and no
route should be invented. The verified working cache itself has no such route.

Actual IDA Pro 9.3 inspected the original retail ReactNativeApps AOT image
(source SHA256 `9439c80967aff822cba6d3d44f9b88a326e8090e89f2fc54714d2d2a060f172f`,
private ELF-type-only analysis SHA256
`6ec98a16908e0e5bcad43d1774b74ccfd610f8c6433b90d98a601920515a8114`).
`CreateRnpsPhase` at `+4ba50` makes a virtual factory call at `+4bbce`;
strictretail5 faulted at `+4bbdc` on the returned null object. This confirms
the immediate null dereference, but not by itself why the factory returned null
or the semantics of the separate `psinitialsetup` URI. IDA direct xref checks
on the UTF-16 Legacy metadata string did not identify its managed caller.

Read-only comparison found the retail-tree `NPXS40008/application.ps.hbc`
sidecar has SHA256
`f6e0b33e66df1847cf20a1661bfc3c3d7eeb606f3456f31ce88780438aa72718`,
identical to the other firmware tree's **plaintext** `application.ps.bundle`.
The retail original encrypted container is different; its declared plaintext
length matches the sidecar length, but length/hash identity with another tree
does not authenticate it as retail-derived. Its SHA-256 and SHA-1 are not
literally present in the examined retail container header. Do not silently
attribute or stage that sidecar as retail, or disable authentication to make it
load. An independently authenticated retail plaintext would be eligible for
the existing `prepare_shellui_managed_1360.py --rnps-source` mechanism; a mixed
comparison must state its inputs and must not be presented as strict retail.

For the explicitly mixed cache-only experiment above, leave the existing
strictretail5 RNPS mount and replay setting unchanged, and collect guest/host
logs after the user-controlled launch. The next distinguishing evidence is:
(1) settings resolver result and title/component; (2) actual app-manager
manifest lookup for `NPXS40008`; (3) bundle open and decrypt result; (4) RNPS
factory return / exception and visible scene. Absence of a line is not proof
that a stage succeeded. If route resolution succeeds but the bundle is still
wrapped, the existing decrypt provider reports its real `80940001` on a key
miss rather than delivering invented plaintext. No retail authenticated
plaintext or complete boot has been established in this follow-up.
