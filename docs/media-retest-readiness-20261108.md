# Apple TV / Netflix: ready-to-test checkpoint, 2026-11-08

**Superseded for Netflix startup:** the parent-owned mnetflix2 retry exited at
NGS2 sampler setup before usable UI was established. The four suites below did
not cover NGS2. See `netflix-ngs2-float-startup-20261108.md` for the F32 PCM fix,
real sample-output regressions and required production rebuild/new hash. The
pinned executable in the historical launch recipe below lacks that correction.

## Executable and gates

**Live correction:** the parent `mnetflix2` launch of this executable exited
during startup at NGS2 `SetupSampler`, `ngs2.cpp:705`, because the waveform type
was neither the implemented ATRAC9 path nor `0x12`. User reported no usable app
opened. The four component suites below did not cover this application startup
path. Netflix is therefore not yet ready for the intended authenticated playback
retry. See `netflix-retest-attribution-20261108.md` for the retained failure.

At the offline checkpoint, both applications were candidates for live retesting
with the corrected production binary; that did not establish launch success.
Parent/user owns process selection, launch, navigation and closure.
No live process was launched, attached to or controlled by this investigation.

Read-only identity check:

- `${USERPROFILE}/Desktop/kytyps5/_Build/windows/kyty_emulator.exe`
- 34,299,904 bytes; last-write November 7, 23:53:49 local.
- SHA256 `3aa3636099727914e0cb00ceb2ecfec5c79e65ca3f1c9bda6e04f71be0786e82`.
- Embedded build string `3a12861`; matches the production identity recorded in
  `vsh-home-trace-lifecycle-20261108.md`.
- Git ancestry includes Netflix `8b601a65`, Apple ABI `99627b31`, and initial
  all-stream Flush/input barrier `6a88b336`. The source, decoder, Videodec2 and
  memory header have no diff between that commit and this tree's base `980881e9`.
- The executable contains `startup-flush-complete` and `source-ready-delivered`.
  Parent CMake cache selects the AC3/EAC3-enabled FFmpeg package in the old media
  tree. That package and all old investigation trees were read-only.

This supersedes the earlier media documents' **production relink pending** notes.
It does not supersede their failed live results or prove new playback acceptance.

| Path | Corrected/available now | Next missing gate |
| --- | --- | --- |
| Netflix, authenticated KAMPEN MOD EN ORKAN | Resource `0x12384` admitted in query/create; queue needed only for resource 1 | Live nonzero requirements/allocation, decoder creation, then sustained picture and sound beyond 20% |
| Apple clear previews | AC3/EAC3/HEVC decoding, source event `0x2001`, initial Flush producer/input barrier | Live startup credits, explicit Pause plus actual video Sync, readiness delivery, presentation and PCM |
| Apple Gfx release | Guest name at VirtualQuery result `+0x21` | Live allocation/query/release without Gnmp.cpp:55 abort |
| Apple protected episodes/live | Codec support only | Genuine CDM/access, MediaKeys, license session and sample decryption provider absent |

The first demonstrated missing Apple lifecycle producer was initial all-stream
configuration -> internal Flush, **already fixed**. `FinishInitialConfiguration`
checks every live child's valid codec/empty ownership, actually resets decoders,
binds generations, requests first-output Sync, then releases input admission.
`ObserveReadiness` still requires actual zero-to-positive output; explicit Pause
is not fabricated. There is no evidence of another missing producer in a run of
this corrected binary yet. Changing the readiness threshold or estimating hidden
HEVC identities now would outrun the evidence.

## Canonical parent-owned retest commands

Retained manifests are the launch authority:

- Netflix: `%LOCALAPPDATA%/Temp/opencode/mnetflix1/launch.json`, PPSA01616.
- Apple: `%LOCALAPPDATA%/Temp/opencode/mapple1/launch.json`, PPSA01783.

Both use regular `${USERPROFILE}/Downloads/13.60`, user `268435457`, host networking,
native SSL/libc, 1920x1080 content/1280x720 window and `KYTY_MEDIA_TRACE=1`.
Reuse **all four original mounts**: `/data`, `/system_data`, `/user`, `/system_tmp`.
Do not copy, reset, replace or reseed their stores. Do not use a fresh profile or
the old overlay executable. New log directories must be separate from the stores.
Normal application writes during the parent-authorized run remain user-owned.

The following PowerShell command is a complete **launch recipe for the parent**;
it was not executed by this agent. Run one app at a time after the parent selects
the live slot. For Apple replace the last line's `netflix mnetflix2` with
`apple mapple2`. Choose another new short name if that output directory exists.
It preserves argv/mounts/environment from the original manifest, changes only
the guest log destination, and checks the corrected executable hash before launch.

```powershell
@'
import hashlib, json, os, pathlib, subprocess, sys, time
app, name = sys.argv[1:]
old = {'netflix': 'mnetflix1', 'apple': 'mapple1'}[app]
root = pathlib.Path(os.environ['LOCALAPPDATA']) / 'Temp' / 'opencode'
assert name and pathlib.Path(name).name == name and name not in ('.', '..')
original = json.loads((root / old / 'launch.json').read_text())
exe = pathlib.Path(original['executable'])
digest = hashlib.sha256(exe.read_bytes()).hexdigest()
assert digest == '3aa3636099727914e0cb00ceb2ecfec5c79e65ca3f1c9bda6e04f71be0786e82', digest
argv = list(original['arguments'])
for i, arg in enumerate(argv):
    if arg == '--mount':
        assert pathlib.Path(argv[i + 1].split('=', 1)[1]).is_dir()
run = root / name
run.mkdir()  # refuses reuse; never truncates retained evidence
argv[argv.index('--printf-output-file') + 1] = str(run / 'guest.log')
env = os.environ.copy()
env.update(original['environment'])
record = dict(executable=str(exe), executable_sha256=digest, arguments=argv,
              environment=original['environment'], replay_of=str(root / old / 'launch.json'))
(run / 'launch.json').write_text(json.dumps(record, indent=2))
with (run / 'host.log').open('xb') as host:
    process = subprocess.Popen([str(exe), *argv], cwd=run, env=env,
                               stdout=host, stderr=subprocess.STDOUT)
record.update(pid=process.pid, launched_at=time.time())
(run / 'launch.json').write_text(json.dumps(record, indent=2))
print('User-controlled process:', process.pid, 'Evidence:', run)
'@ | python -B - netflix mnetflix2
```

No automatic timeout, restart or controller input is attached. The parent should
record the selected title, attempt time, visible percentage/error, and whether
actual moving video and audible sound persist for at least 60 seconds. That
interval is an acceptance observation, not a readiness predicate change.

## Bounded acceptance and next-failure capture

### Netflix

Select the same authenticated title, not merely the login/splash view. Preserve
guest/host logs and screenshot with attempt time. Inspect the first requirements,
allocation and create calls after selection. Confirm the former zero-length
type-12 request / `vdec dmem` EINVAL trap is gone and actual decoder output follows.
Passing 20% alone is not playback. If a new failure occurs, retain the first
nonzero API result, configuration/resource and caller RVA. Do not attribute it
to DRM, network or ESVM without that trace: Netflix uses Videodec2/Audiodec.

### Apple: clear preview first, protected content separately

Use one fresh application process and one clear-preview attempt first. Compare:

1. `source-admitted`, both `decoder-admitted-1360`, separate audio/video configs.
2. Successful `startup-flush-complete` (result zero; children count in `bytes`),
   then input credits/refills. The initial path need not emit `flush-complete`,
   which belongs to the explicit Flush operation.
3. Actual queued decoded output and `source-ready-delivered`, then Render/mailbox
   activation and `audio-pcm-accepted`. Mailbox/PCM acceptance is backend evidence;
   the user must still observe moving picture and audible playback.
4. Navigate away/retire the preview and verify the former Gfx release abort does
   not recur. One successful release does not prove every lifetime path.

Run the read-only summary from this tree against the new guest log:

```powershell
python -B tools/summarize_apple_media_log.py "$env:LOCALAPPDATA/Temp/opencode/mapple2/guest.log" --timeline-limit 64
```

The updated helper prints file identity, operation counts, nonzero numeric
results, and bounded first lifecycle/error records per operation/PID/handle/result.
It does not dump guest lines, tokens, URLs, access units or keys.

If still waiting, locate the **first absent boundary**, rather than repeat a long
episode retry loop. Existing process-wide trace caps are lifecycle128, Render16,
submission256, AU64, queued-frame32, audio64 and refill64. A saturated group makes
absence inconclusive. Preserve early logs before retries spend those budgets.

- No successful startup Flush: inspect both config completions and the child
  states at `FinishInitialConfiguration`, including its empty-ownership checks.
- Successful startup Flush, no readiness: the current trace does **not** expose
  explicit Pause/Play ordering or the whole predicate. If reproduced, next bounded
  instrumentation should capture at most 64 changed observations for that source:
  source/child/generation, Pause/Play, waiting-output/Sync transition, decoded
  count, queued payload AUs, held-frame boolean, accepted EOS, readiness state and
  delivery eligibility. Do not log payload bytes or infer native hidden reorder
  identities from accepted-minus-output counts. Do not lower threshold12.
- Readiness delivered, no playback: inspect Play/Render, selected stream, mailbox
  submission/presentation and PCM sink acceptance; do not reopen startup admission
  merely because the app still displays a spinner.
- Protected attempt: record separately. Prior unsupported PlayReady access
  `0x8a790010` precedes native MediaKeys lookup failure `0x8a790004`; a real provider
  is absent. Clear-preview success cannot satisfy this gate, and creating a fake
  successful session/entitlement would not decrypt samples.

## Verification performed in this clean worktree

No new production C++ change or build was needed to make this test recipe usable.
Four isolated component executions passed, sequentially (one process at a time):

- `virtual_memory_allocation_tests.exe --videodec2-resources-only`: exit0.
- `virtual_memory_allocation_tests.exe --media-memory-only`: exit0.
- `virtual_memory_allocation_tests.exe --media-framework-esvm-source-only`: exit0.
- `media_framework_esvm_decoder_tests.exe`: exit0.

These were copies of existing parent test executables plus `libwinpthread-1.dll`,
run with cwd `_Build/media-retest` in this tree; parent inputs were read-only.
This is **existing-binary rerun evidence**, not a fresh build at `980881e9` or live
app acceptance. Test SHA256 values:

- memory/source/resource driver:
  `6cf65b541eb3a084e63edc19ff0763117968b7f20772e753db25e4ffdf3b9e0f`.
- packet decoder:
  `af7e6da2213d9e4cf2eb8248c9537967e7ff9b698d01e20cdaedadaf066dc820`.

Logs remain in `_Build/media-retest/*-only.log` and `media-decoder.log`. Current
parent `Testing/Temporary/LastTest.log` contains a later database-only run, so it
must not be cited as the media pass. Prior normal-build media results remain in
the Apple/Netflix investigation documents.

The enhanced summary was executed on retained `mapple1/guest.log`: 292,742,700
bytes, 8,547,517 lines, SHA256
`57d1622af18bfbf722c4ceaac2f87a36ef3d9c9c299005be468d80f0f0505f4a`.
It reproduces six sources/twelve children, zero recorded readiness/PCM/Flush,
and identifies the first source at line553683 and children at553919/554219.
That log predates the startup producer; zero startup records there are expected.
An in-memory synthetic check passed duplicate suppression, nonzero-error reporting,
the timeline bound and exclusion of unrelated private text. The documented Python
launch body passed syntax compilation only; it was not executed. `git diff --check`
passed.
No profiles, old evidence, old dirty media tree, main build or Home/PSN source
were changed.
