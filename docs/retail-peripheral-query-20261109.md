# Retail initial-onboarding query: resolved contract and targeted capture

## Verified state and remaining blocker

Parent's `retail-event1/checkpoint1` and the user's visual report confirm that
candidate `4735fba3` still asks for a PS5 controller. State 1000, subtype 24,
managed DeviceAdded and PS-button delivery are verified prerequisites, not
peripheral acceptance. The snapshot contains no returned managed peripheral
array or conversion outcome. The first-24 MBUS logs were consumed by earlier
PSM scans. There is no evidence yet distinguishing an absent/stalled query,
empty enumeration, owner exclusion, conversion failure or final type filtering.

No further controller identity or owner change is justified by these logs.
This checkpoint supplies a targeted observer, not an acceptance-fix candidate.
Existing GC/state/subtype implementation remains at the tested candidate.

## Actual retail consumer evidence

Original `Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx.elf` SHA256:
`8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa`.
Its AOT image table resolves cross-image method references as image-index in
the high byte and MethodDef row in the low 24 bits. MethodSpec references are
resolved through the named image's embedded metadata, not treated as local
MethodDef indices. This resolves formerly unnamed calls:

- Worker `+0x1a6650`, call `+0x1a684e`: image 6 (LoginMgr), MethodDef `0x8b2`,
  `Utility.GetDeviceInfoList`; all-device type 0, user -1 and type -1.
- Call `+0x1a6962`: image 26 (UserServiceWrapper), MethodDef `0x24a`,
  `IsValidAvailableUserIdRange`. Its result is negated by the worker.
- Calls `+0x1a0f85` and `+0x1a0fb6`: generic
  `PeripheralJsonConvertor.ConvertToStringValue`, for native device type and
  connection type. Conversion can fail before the allowed-type test.
- Call `+0x1a6a8c`: `Enumerable.Contains`, comparing the converted type with
  the onboarding allowlist. `+0x1a6aab` adds accepted objects to the JArray.

The worker skips device ID -1, owner 255, and owners for which the native
available-user-range predicate is true. Actual retail eboot registration pair
at `+0x1199660/+0x1199668` maps
`Sce.Vsh.UserServiceWrapper::_IsValidAvailableUserIdRange` to `+0x6e36c0`.
IDA confirms the native predicate (unsigned arithmetic):

```c
return user != 0 && (user < 0x100 || user - 0x10000000 < 0x30000000);
```

**Owner -1 passes this unassigned filter.** A normal assigned user in the
predicate's range is excluded. This is not a prerequisite to create/assign a
user. The eboot original hash and ELF-type-only analysis copy are respectively
`f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060` and
`0ea43b4cc6b323a5ffa53e9db8c6c4473ac72abe9da65455c87b049e81a2dd79`.

Original LoginMgr AOT SHA256:
`72a74dce1d5d8fad59d3fb9872cbd23472dd432d2e3d99fd209391af9b8f5088`.
Actual IDA converter constructor `+0x48020` and decoded GOT strings establish:

- Native device types 0, 16, 21, 22 map to `controller`; 1 maps to
  `standardController`; 27 maps to `leonardo`.
- Connection type 0 maps to `usb` (`+0x4874b..+0x48757`); 1 to `bt`,
  2 to `hdmi`, 4 to `remotePlay`.
- The worker's onboarding allowlist includes `controller`,
  `standardController`, `leonardo`.

These native device types are not MBUS subtype numbers. Runtime conversion
values still need observing. `ConvertToStringValue` at `+0xd3470` fails for an
absent map/key or empty mapped string; it does not silently accept unknown
values. Private IDA reports are `retail-query-conversion.txt` and
`retail-owner-predicate.txt` under the existing external analysis directory.

## Parent-run capture

`tools/capture_retail_peripheral_1360.py` uses the existing hardware-breakpoint
observer lifecycle. It does not write guest code/data, call guest functions,
change device state, press buttons or settle promises. It temporarily pauses
threads, so timing is perturbed. Process creation time, executable path/hash,
staged AOT identity and live instruction windows are verified.

Default `--mode query` uses four hardware slots:

| RVA | Observation |
| --- | --- |
| `1a6650` | Worker entry, per-thread/per-frame correlation |
| `1a6853` | Native query return, onboarding flag and complete bounded device array |
| `1a6a2d` | Conversion boolean and the exact input record/index |
| `1a6af9` | Assembled JArray count, before setting the response `results` property |

Records include device ID, owner, VID/PID, connection, native type/subtype and
function masks. Device names and Bluetooth addresses are not collected.
Negative returns and unreadable outputs remain errors, not empty lists.
The `results` observation is **not** proof of promise resolution/JS receipt.

If the first capture shows filtered entries, `--mode filters` replaces the
last two slots with `+0x1a6967` (actual owner predicate result) and
`+0x1a6a91` (actual allowed-type predicate and allowlisted type string).
That mode deliberately has no completion probe: pending entries are expected.

Offline ABI audit, with no process access:

```powershell
$fw = '${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/decrypted_modules'
python tools/capture_retail_peripheral_1360.py `
  --original "$fw/Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx.elf" `
  --corlib "$fw/mscorlib.dll.sprx.elf"
```

For a parent-owned launch of the already-tested emulator, attach after this
AOT is placed but **before** the settings query. The previous snapshot places
it at guest line 160766, before the controller scene. Use a saved startup
prefix containing its `Loading:` and `place` lines, the exact stage file from
that prefix, and the new launch receipt's PID/creation time/executable/hash:

```powershell
python tools/capture_retail_peripheral_1360.py `
  --original "$fw/Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx.elf" `
  --corlib "$fw/mscorlib.dll.sprx.elf" `
  --stage $stageAot --run-log $startupPrefix --mode query `
  --attach --pid $launchPid --created-filetime $createdFiletime `
  --exe $candidateExe --exe-sha256 $candidateSha `
  --output $newTraceJsonl --seconds 120
```

Do not infer query absence from a late attach. No `query_entry` means entry
coverage is unestablished; an unmatched later probe remains explicitly
uncorrelated. Preserve JSONL plus parent snapshots. `cleanup_verified=true`
confirms orderly detach. A nonempty assembled list narrows the next boundary
to response/promise delivery; zero entries with successful enumeration and
failed conversion identifies the conversion boundary instead.

## Verification

- Full-hash/actual-byte offline ABI audit passed against original retail AOT
  and corlib. Retail `List<T>.get_Count` at `+0x27cb60` reads `+0x20`; using
  `+0x18` would read the wrong field. Device array stride is 112 bytes.
- Ten offline replay tests cover empty enumeration, failed/successful
  conversion, result counts, negative native return with invalid output,
  late attach, thread/frame reuse, incomplete coverage and filters.
- Existing Windows sacrificial-child lifecycle test passed with one round,
  four workers: hit/deadline/create/phase/idle/restore-error cases. Children
  reported zero remaining debug-register threads after normal detach.
  Artifacts: external `home-trace-lifecycle-dae4ujf1` directory.
- These are observer tests, not a retail guest acceptance run. No parent
  process, runtime store or controller was accessed. No emulator rebuild was
required, and the stable build base was not modified.

## Parent capture preparation

The parent independently passed the offline ABI audit against the original
retail peripheral AOT and corlib. A local startup watcher was prepared to
launch the already-tested `4735fba3` executable as `retail-query1`, preserve
the AOT placement prefix and attach this query observer immediately afterward.
It retains the mixed RNPS overlays, native SSL/libc, cache and replay-disabled
setup, with a fresh disposable store. The bounded capture is scheduled in the
background for 180 seconds; attachment, query coverage and cleanup results are
pending. This is a timing-perturbed diagnostic, not an acceptance-fix build.
The user reports that this diagnostic run still asks for a controller; visible
acceptance remains blocked. Capture completion and interpretation are separate
from that confirmed UI observation.

## Parent runtime capture result

`retail-query1` armed before both observed queries. Two complete, correlated
`initial_onboarding=true` invocations returned native success and enumerated
device 2 with owner -1, connection 0, native type 24, native subtype 2,
vendor/product 0 and enabled/builtin functions 1. Both reached the conversion
probe with `conversion_ok=true`, then assembled `result_count=0`. The other
enumerated record was device -2 / owner 255, connection 2, native type 14.

This rules out absent enumeration or failed conversion for device 2 in these
queries and narrows rejection to the subsequent onboarding filtering boundary.
It does not yet identify the converted type string or justify substituting a
different device identity. No language/Home/authentication success occurred.

Capture recorded 8 hits, zero decode unknowns and no outstanding invocations.
The final record confirms `cleanup_verified=true`, detached=true, no cleanup
errors and process_exited=false; the observer left the emulator running.
Immutable local evidence is `retail-query1/checkpoint1`, including the startup
prefix, launch identities and `peripheral-query.jsonl` (SHA256
`ffe7e5ca68bdd60f8446cd85b7aba8bb473f799cc9a73560b526c51a3a304c74`).
