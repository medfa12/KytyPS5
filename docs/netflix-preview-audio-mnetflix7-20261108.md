# mnetflix7: unresolved CPU decoder publishes uninitialized PCM

## New evidence supersedes the queue-stall hypothesis

Promotional landing preview, not established authenticated-title playback.
User reports loud screeching. No further audible retries are requested or made.
Parent reports PID25036 already absent when closure was attempted; neither exit
cause nor a successful closure is established.

Snapshot manifest: guest38461277 bytes, SHA256
`17b4293353f1d0a4ed9d6db1e19062a434ae2de97c744b8fbaec73e9c94401ec`;
host4032 bytes, SHA256
`dc328096be7a9035cadb3e479357d8f765f1f358b2abaafa06a9d6dfd5b85750`.
Read filtered audio records plus narrowly selected decoder-NID relocation and
EnsureImportProviders records from the immutable guest snapshot. No live process,
stores, full-log output or credentials were accessed.

235 audio records contain27 NGS2 and26 AudioOut PCM summaries. First source grain
already has6 nonfinite values and102 finite samples above unit amplitude among
1536 samples; peak6.09420357e32. Later source peaks reach2.1387162e38. Every logged
AudioOut input/output pair has identical peak amplitude (hash changes are expected
from channel reordering). There are **zero logged clears or underruns** in this
snapshot. Thus corruption precedes NGS2 mixing and AudioOut preparation; queue
stalls from mnetflix6 do not explain this run. Ratio1.0/pitch0 cents remain recorded.

## Causal producer path

All four CPU-decoder APIs are unresolved at relocation:
`ktD2w3D4G2U` query, `hdFsxo3MFu8` init, `hAS5WH6hxrE` clear,
`lSVTiWV5wLc` decode. HLE implements Audiodec, but not AudiodecCpu. The generic
unresolved-import thunk returns zero without writing output parameters.
`EnsureImportProviders` reports4 programs,0 provider candidates,0 missing;
its native-provider map omitted AudiodecCpu entirely.

Bounded disassembly of the actual Netflix executable establishes:

* `150a458..150a6bb`: constructs CPU-decoder params for codec0x1007, parameter
  size0x2d48 and BSI size0xa6b4; calls query/init CPU imports.
* `150a7a2..150a96b`: allocates0xa000 (40960) bytes; sets output capacity0xa000;
  calls CPU decode at150a8df. On zero return it reads the output size field at
 150a918 and transfers that buffer/size to its caller. A zero-return no-op
  therefore publishes the entire **uninitialized allocation**.
* `1508168` virtual decoder call -> `1508380` -> `1075703` queues that allocation
  without sample conversion. Float branch at1075806 computes frames as
  bytes/(channels*4), yielding40960/24=1706, precisely the captured submissions.
* Separate AAC branch uses AudiodecDecode and its actual-output size; it is not
  the unresolved CPU path described above.

No live call stack for the CPU thunk was captured (stub logging is capped).
The unresolved bindings, actual producer code, exact unchanged-capacity shape and
invalid first source grain identify a concrete false-success path; the offline
negative control below reproduces that path rather than relying on the warning.

Actual13.60 `libSceAudiodecCpu.sprx` dispatch table at0xc020+7*8 imports
`xqlFMfkztKg` from DDP. Its exported functions query/init/decode/clear are at
0x4c0/0x590/0x680/0x690. DDP exports a32-byte function table at0x58010 pointing
to0xd0/0x180/0x430/0x1440. These are native code observations,
not an inferred codec name or a generic pitch-effect theory. No IDA use is claimed.

Native image SHA256:

* CPU: `9ed292b64d5d4a0194c070f2d6df2a928276f189ec0109b8b144e100c40e24a2`
* DDP: `8b2cab55ed77df39f9537addaf18dbebf77796ad3b2c17c5b4972e85729d6516`

## Correction

`RuntimeLinker::EnsureImportProviders` now discovers the real native dispatcher
and DDP plugin in its existing two-pass startup dependency loading. Matching is
exact for this family: a DDP filename must never substitute for a missing CPU
dispatcher merely because it shares its prefix. Added canonical
`/system/common/lib` to firmware search roots. Missing required CPU/DDP files now
fail explicitly rather than allowing zero-return decode fallback. DDP's own
global dependencies must resolve; unrelated optional codec tables in the CPU
dispatcher retain the existing loader policy. This does not claim support for
other CPU codecs or alter HLE Audiodec, NGS2, output gain or video.

The decoder executes native code with its full parameter semantics, including
Netflix's channel permutation {0,2,1,5,3,4,6,7}; no guessed FFmpeg parameter
translation, PCM clamping, forced stereo, silence substitution or effect success.

## Device-free failing-before/passing-after test

Added `tests/prepare_native_audiodec.py` and `tests/NativeAudiodecCpuTests.cpp`.
The preparer reads local firmware into uncommitted flat images/relocations. The
standalone test maps both actual images, relocates the CPU->DDP table, binds a
small explicit set of host ABI helpers, and calls native query/init/decode.
Unrelated codec-table imports are null and never exercised. Unexpected helpers
fail. This is a focused native decoder test, **not the emulator's full loader or
module-initializer integration test**. No audio output library/device is used.

Generate three48kHz5.1 E-AC-3 frames from distinct low-amplitude per-channel tones.
The test uses the actual Netflix parameter values and40960-byte output capacity,
prepoisoned with NaNs. Verified results:

* Native decode: consumes2560 bytes each call; output sizes0,36864,36864.
  First call is decoder delay, not fake success. Both output frames are finite,
  bounded, nonzero; canaries survive. Frequency-correlation checks verify the
  intended tone on every channel, including LFE, detecting channel/stride errors.
* `stub` negative control substitutes only the old zero-return decode behavior:
  output size stays40960 and the test fails immediately with exit1,
  `decoder published capacity instead of actual PCM length`.
* Modified production runtimeLinker.cpp compiles with production flags in an
  isolated Ninja overlay, -j2. No shared build outputs changed. This is an object
  compile, not a production relink/run. Existing sampler/audio suites were not
  rerun because their source paths were unchanged by this checkpoint.

Reproduction (PowerShell, from worktree; directory must exist before compilation):

```powershell
python -B tests/prepare_native_audiodec.py ${USERPROFILE}/Downloads/13.60/system_b/common/lib _Build/native-audiodec-validation
ffmpeg -hide_banner -loglevel error -f lavfi -i "aevalsrc=0.05*sin(2*PI*440*t)|0.05*sin(2*PI*550*t)|0.05*sin(2*PI*660*t)|0.05*sin(2*PI*80*t)|0.05*sin(2*PI*770*t)|0.05*sin(2*PI*880*t):s=48000:d=0.096:c=5.1" -c:a eac3 -b:a 640k -y _Build/native-audiodec-validation/tones.eac3
```

From that isolated output directory:

```powershell
& 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /std:c++20 /EHsc /O2 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /clang:-march=x86-64-v3 ../../tests/NativeAudiodecCpuTests.cpp /Fe:NativeAudiodecCpuTests.exe /link /stack:8388608,8388608
.\NativeAudiodecCpuTests.exe . tones.eac3
.\NativeAudiodecCpuTests.exe . tones.eac3 stub # expected exit1
```

Precommitting the test stack is necessary: native SysV code has a~95KB decode
frame without Windows stack probing. Initial harness experiments exposed this
before using a fully committed stack. Other initial harness failures were missing
ELF-dialect handling/import binding and incomplete parameter reconstruction;
they were corrected before the successful test. Firmware and generated media
are local artifacts, not committed.

Test executable SHA256 `b1dbd82c3c3b2a332587450819d520dedb1f0cb78771555562f969c84c50e552`.
Synthetic fixture SHA256 `6bbc90f4f1f0df912bacb1bd575e38abea12ef5f9ffd159735b4f79086939ce4`.

## Parent handoff / remaining verification

Integrate this checkpoint only;922994a3 already has parent equivalent655e1168.
Production target: `kyty_emulator`, parallel<=2. Supply the exact local native
CPU/DDP images through the existing firmware mounts. The required production
check is decoder-provider/import closure and native decoding in a **device-free
offline harness**, including the real emulator ABI helpers/module startup.
Do not treat another audible preview retry as routine verification.

The false-success decoder path is corrected and the real native sample path is
verified offline. **Netflix playback with the integrated production loader is
not yet verified**, and user-confirmed sound recovery is not claimed. Native
module startup/guest stack integration remain production verification gates.

## Parent integration and independent offline reproduction

Parent reviewed and integrated3b885675 as63bba120. VS18 production rebuild of
`kyty_emulator` succeeded with parallel2; log
`Temp/opencode/netflix-native-decoder-parent-build.log`. Executable SHA256:
`4d734d8668438f67cb5910a94886c49caff325bf23522945a2918030b8470745`.
No emulator or audio playback was launched.

Parent regenerated the flat-image fixtures and synthetic tone stream in
`Temp/opencode/netflix-native-decoder-parent`, verified both original native
image hashes against the values above, and independently built the standalone
test with VS18/clang-cl and a fully committed8MiB stack. Synthetic media SHA256
matches the agent fixture. Parent test SHA256:
`29afc988fac8e5bd38880a699f04f2ba180daa3a93e9ae3cb9181b11cffea845`.
Positive run consumed2560 bytes per frame, produced0/36864/36864 bytes, and
passed finite-amplitude, canary and channel-tone checks. Zero-return negative
control failed with exit1 and unchanged40960-byte output capacity as expected.

The existing Astra medium media agent continues on a separate **device-free
production integration** check: actual RuntimeLinker provider/import closure,
module initialization, real ABI helpers and guest stack. The standalone custom
helper harness and successful production relink do not establish these gates.
No audible retry is scheduled and sound recovery is not yet user-confirmed.
