# Settings and notification bundle comparison (2026-11-08)

Parent's explicitly mixed retail-host1 passed shared-host preload: guest read
plaintext host/base HBC, and host reported the `NPXS40008` manifest and a
promoted `rnps-settings` Hermes pool instance. This is not evidence of rendered
settings, Home, or PSN authentication. The next observed errors were wrapped
`/system_ex/rnps/apps/NPXS40008/application.ps.bundle` and
`/system_ex/rnps/apps/NPXS40011/application.ps.bundle`, both `80940001` from
`JSExecutorRunBundle`. The latter title's retail manifest identifies
`rnps-notification-overlay`, not the settings app.

## Compatibility and attribution

The original retail and comparison-tree manifests match byte-for-byte:

| title | retail name / version | manifest SHA256 |
|---|---|---|
| NPXS40008 | rnps-settings / 13.6.0+57942 | `2af65f7e817dc2ff24f98af6fe3430965c2b9e7ddda4350a278a7464bbc6beea` |
| NPXS40011 | rnps-notification-overlay / 13.0.0+22460 | `5ce7d1326383e08eb1a2e800f3cec4a2f0bf162ea4780db4a9ab4fdf217dca6b` |

Both manifests declare Hermes and twinTurbo. Original retail bundles are
`RNPSHEDR` containers. The existing 13.60 comparison tree contains raw HBC as
`application.ps.bundle`; its bytes match the generated `.hbc` sidecars found
beside the retail containers, but that match does **not** authenticate the
plaintext as retail-derived. Length matches and common manifest identity do
not prove decryption or signature verification.

| title | retail wrapped SHA256 | comparison HBC SHA256 | declared plaintext bytes |
|---|---|---|---:|
| NPXS40008 | `1cb21ba1f3f2070743802a5dc9762a3ee5f0d9041597e37f1dc3702679af4fad` | `f6e0b33e66df1847cf20a1661bfc3c3d7eeb606f3456f31ce88780438aa72718` | 5160076 |
| NPXS40011 | `a51a2d05b6fb9ae618208e17a450a38a87ae51c0913199ae046e316eba7a9dc3` | `f49b5c2e7e208e5a1acbbeed2f429343a68451c91a3133b8436bea6098985bcd` | 4345852 |

Both comparison HBC files have Hermes magic/version 89 and an exact file-size
header; both contain `base_dll` and `bundleRegistryLoad` dependency markers.
The separately supplied shared-host/base files from retail-host1 also use
Hermes version 89. This is format/dependency evidence only, not proof of full
API compatibility or execution. No generated JS sidecar was treated as a retail
original or included in the staged titles.

## Explicit isolated comparison

This extends retail-host1 by **only two title mounts**. It uses comparison-tree
plaintext directly and therefore bypasses wrapped-container cryptographic
verification. Label the whole run **MIXED / plaintext not authenticated**;
do not claim retail decryption or security/authentication success.

```powershell
python tools/prepare_retail_apps_comparison.py `
  --firmware '${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE' `
  --comparison-apps '${USERPROFILE}/Downloads/13.60/system_ex_b/rnps/apps' `
  --stage '<new-isolated-app-stage>'
```

The stage must not exist. The tool checks the four pinned manifest hashes,
retail wrapped hashes, comparison HBC hashes, declared lengths and headers,
and invokes the existing RNPS per-title staging helper with temporary inputs
containing only the two requested bundles. It removes generated sidecars only
from its own new stage, retains the original retail manifests/assets, and emits
an explicitly mixed receipt and these two title mounts:

```text
--mount /system_ex/rnps/apps/NPXS40008=<stage>/rnps/apps/NPXS40008
--mount /system_ex/rnps/apps/NPXS40011=<stage>/rnps/apps/NPXS40011
```

Retain retail-host1's unknown-provenance URL cache, already-reviewed
NPXS40141 host overlay, original retail providers, native SSL options and
replay setting; do not change live stores or setup state. Parent handles the
isolated launch. Capture actual JS execution, errors and visible output before
concluding anything about compatibility or Home.

## Verified preparation

The tool completed at private stage
`${USERPROFILE}\AppData\Local\Temp\opencode\retail-apps-comparison-final`.
Staged plaintext and retail manifest hashes match the table, input firmware
bundles remained unchanged, and generated HBC/JS sidecars are absent from the
stage. `tests/test_retail_apps_comparison.py` passed existing-stage preservation
and wrong-manifest preflight rejection; shared host tool's hash/length negative
controls also passed. A production-object `HermesVmCreate` fixture rejected the
original wrapped settings container with `80940001` and accepted and destroyed
handles for both comparison HBC files. No JS or guest launch was performed by
this work; acceptance is not cryptographic verification. SharedStore error,
actual app execution, user-visible Home, and genuine PSN exchange remain gates.
An additional production-object mount test used the same broad retail RNPS root
and all three specific comparison title mounts; `ResolveGuestPath` selected the
intended `NPXS40008`, `NPXS40011`, and `NPXS40141` staged manifests. This tests
mount selection, not guest application startup.

## Parent live comparison (2026-11-09)

`retail-apps1` used the same hash-verified M2 executable, native retail SSL/libc,
URL cache and shared-host overlay as `retail-host1`, adding the two specific
application mounts after checking their pinned plaintext/manifest hashes.
Replay remained disabled, audio normal, and the disposable store started with
only the copied URL cache. The launch manifest labels the mixed plaintext run.

Both application Hermes runtimes were promoted. The captured logs contain no
repeat of the preceding `JSExecutorRunBundle` bundle-verification failures.
More substantially, notification overlay reached `MountApp runApplication`
(host line 2293) and emitted JSConsole messages including UrlConfig parameters
and ReactProvider cache state. This demonstrates application JS activity for
NPXS40011; Settings execution/display and visible Home remain unconfirmed.

The process subsequently exited with `Guest abort()`. Host output shortly
before the abort reports `pthread_suspend_user_context_np failed ...: 45`.
That sequence needs native analysis before assigning the cause. SharedStore
`0x80020002` also remains in the log. No PSN authentication is established.

Immutable local evidence: `retail-apps1/checkpoint1/{guest.log,host.log,launch.json,summary.json}`.
Guest log SHA256: `b38060587055af252e4323d9743acb566d1bcc870c02f91afa694c3d24a68053`.
Host log SHA256: `bfac6f1f946daa95a7de80a2203f4c2e5c17d4641d6f7b9ea4ede29791e338ae`.

Asked about visible output, the user reported particles and the shine background.
No Settings or Home UI was confirmed; notification JavaScript activity above
must not be reported as visible menu progress.
