# Retail RNPS shared-host comparison (2026-11-08)

## Observed gate

Parent's explicitly mixed cache-only run resolved the settings URI to
`NPXS40008/Main` and read the retail manifest. Its next failure was **not yet
the settings application bundle**: guest 334401 opened shared-host
`/system_ex/rnps/apps/NPXS40141/host.ps.bundle.hbc` and guest 334433 reported
`RNPSHEDR`, offset 2848, stored 2992, plaintext 2980, no key/replay disabled,
`80940001`; 334460 reported `JSExecutorPreloadHostDll` failure. The earlier
`psinitialsetup:launched` miss became `80ba8087` and processing continued.
No Home, settings execution or authentication succeeded in that snapshot.

## Input and compatibility evidence

The four original retail `NPXS40141` containers and manifest are byte-for-byte
identical to the corresponding 13.60 comparison-tree containers and manifest.
Existing working preparation `_TempData/rnps_staged/NPXS40141` contains four
plaintext files. Their SHA256 values match the corresponding known replay
payloads keyed by those containers' header lengths and ciphertext prefixes:

| file | retail container SHA256 | comparison plaintext SHA256 |
|---|---|---|
| host.ps.bundle | `f31a86d7385339156b12ae0e65da0093ca80d972e2cfcbc8cbb890dc6b7a1c89` | `ce1bef142fe8acdb448d616b65567c1b4245df15cb0a9ad7c398485a1e75e2eb` |
| host.ps.bundle.hbc | `ce47ad765106566203ef9d801f0bd67aa5f58f8ab3617df8c4851b875bfee04f` | `eb6c5a98be8cb01cb60d409304be3567eeabd66d8b89b0ddd78d645dcc7bc237` |
| base_dll.ps.bundle | `bbfce5480c222c2376d1969090d60ca3020e5737e4d640750e736f04642c9cbe` | `d4c797b851c3c6f52fb1e2ff3a2ff57cf78c01613d5aa677ca495451c943d6a3` |
| base_dll.ps.bundle.hbc | `acb39d6aa9d9ae9699cf8dbb5da67492e7107b8d990c8b58c23fb7001855017b` | `12c7a876da06d1d01dc9c8dd559c6dd41c961f187863d637d6003e1e72dffd74` |

All four plaintext lengths match retail header declarations. The two HBCs have
Hermes magic and version 89; the comparison host JS references `base_dll`,
`bundleRegistryLoad`, `getBundleExport`, `registerComponent` and `Main`, and
all nine of its referenced module IDs occur in the supplied base JS. These are
compatibility checks, not proof that the JS and HBC pairs correspond or that
the native signature/decryption chain authenticated these plaintexts.

Actual IDA Pro 9.3 on original retail `libNativeExtensions.sprx` (source SHA
`2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb`, private
ELF-type-only analysis SHA
`230ec1aca983a93fbf67abacd1abe0cc6145f5abe699779020c56b0a08e53326`) shows
the wrapper at `+20030` passing the bundle to `t8toeP54Mx8` at `+201b1` with
public-key/CRL configuration and propagating negative return. These facts are
native evidence, not authentication of the comparison files.
IDA also shows caller `+1f28b` checking `RNPSHEDR`; wrapped input calls the
wrapper at `+1f2cc`, while non-header input follows a separate path subject to
the guest's `+9c350` byte gate. That disassembly does not establish the gate's
runtime value or authenticate plaintext; the comparison must observe it.

## Isolated comparison preparation

This is an explicitly **mixed plaintext comparison**: it bypasses the wrapped
container's native decrypt/verification path and must never be represented as
retail-authenticated, strict retail, or PSN authentication. It is not an
implementation of cryptography. The setup does not modify original firmware,
source overlay, existing stores, setup flags, or profiles.

```powershell
python tools/prepare_retail_host_comparison.py `
  --firmware '${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE' `
  --source '${USERPROFILE}/Desktop/kytyps5/_TempData/rnps_staged' `
  --stage '<new-isolated-stage-directory>'
```

The tool requires a **nonexistent** stage and exactly the four hash-pinned
`NPXS40141` plaintexts, checks original container hashes, declared lengths,
HBC headers and host/base IDs, and calls the existing per-title staging
implementation. It prints a `--mount` value and writes an explicit mixed
comparison receipt. Append the printed mapping as a more-specific mount:

```text
--mount /system_ex/rnps/apps/NPXS40141=<new-stage>/rnps/apps/NPXS40141
```

Use the same explicit cache preparation as retail-cache1, same executable and
retail providers, replay disabled; do not alter the existing broad RNPS mount.
The more-specific title mount supplies the four comparison plaintexts and the
retail manifest. `NPXS40008` remains the original wrapped retail settings app:
this isolates the shared-host gate and does not silently overlay settings.
Record settings-app open/decrypt and runtime errors as the next gate.

## Validation and remaining gates

The tool was run against a new private temporary stage. All four source and
staged hashes, retail manifest, declared lengths and Hermes headers matched;
original files remained unchanged. `tests/test_retail_host_comparison.py`
passed hash/length negative controls and rejection of an existing stage.
An independent production-object `HermesVmCreate` fixture rejected the original
wrapped host with `80940001` and accepted both comparison HBC headers, then
destroyed the handles. Header acceptance is not JS execution or crypto proof.
No guest was launched by this work. Shared-host execution, settings bundle,
SharedStore failure, visible Home and genuine PSN authentication remain gates.

## Parent live comparison

The parent hash-checked the four prepared plaintext files and retail manifest,
then tested the title-specific mount with the same executable and URL-cache
comparison setup. Replay remained disabled; the run manifest explicitly labels
the supplied plaintext comparison rather than strict/authenticated retail.

The guest read the plaintext host and base HBCs, advanced past host preload,
and promoted a Hermes pool instance to `rnps-settings` (`NPXS40008`). The prior
shared-host preload exception did not recur. Execution then failed in
`JSExecutorRunBundle` for the original wrapped `NPXS40008/application.ps.bundle`
and `NPXS40011/application.ps.bundle`, returning `0x80940001` and producing
managed aggregate exceptions. Runtime creation is not successful execution of
those application bundles or a visible Settings/Home boot.

The immutable run manifest/logs are retained locally in
`retail-host1/checkpoint1`. Application-bundle loading and genuine PSN
authentication remain unverified.

The user supplied a screenshot showing the shine/particle background rendered
at frame 232 / 0 fps and reported closing the stuck process. This confirms
visible background progress in this comparison, but no Settings or Home menu
is visible. The application-bundle failures above remain the captured blocker.
