# Netflix private Videodec2 buffer ownership — 2026-11-08

## Outcome and scope

Implemented a **private-resource buffer-ownership correction**, supported by regular native 13.60 disassembly and a real delayed-output regression. This is not yet verified as the cause/resolution of Netflix's 24% playback failure. No guest attachment, process control, production rebuild or additional Netflix retry was performed.

Latest user result supersedes the login-stall interpretation for mnetflix4: playback attempt reached 24%, then French video-playback troubleshooting for **Pablo Escobar, le patron du mal**. This differs from prior KAMPEN attempts. The earlier startup image showed a darkened Plans UI/spinner and Sign In header, not black output. Browser activation, app login, decoder picture output and sustained audiovisual playback remain separate observations. Parent is switching to Apple with its original stores; this checkpoint does not change that schedule.

## One fixed mnetflix4 prefix

Parent identifies PID25740, production source `cff5c4b`, executable SHA256 `51a39858abcdf853a1a32c69c39000f3c207bac54cccacf90f719d427ff8dc6c`. Read once to a size-at-start limit of **103586585 bytes**; copy completed `2026-11-08T12:49:59.575914+00:00`. Retained prefix SHA256: `0f3e2f6272ed9c23abdd39b57d87a9973195c21755c13fa536614fd9e5a50397`. Local files: `_TempData/mnetflix4-audit/{guest-prefix.log,limits.json,results.json}`.

Confirmed query/create success for three distinct AVC handles, each followed by exactly one logged Decode: lines 958140, 1073985, 1214331, each AU length2284, output storage4194304 bytes, return0/accepted0/valid0/pictures0. A fourth HEVC handle (1920x1080, depth2) has one Decode at1842376, AU length15843, storage27262976, same tuple. Equal AU lengths do not prove equal content. All use private resource `0x12384`. No later Decode/Flush tuple occurs in this prefix; budgets are not exhausted. Two DeleteDecoder matches are relocations in the first400KB, so **deletion/recreation causality is not established**. Four creates are established. The fixed tail reaches layered counter11070, which is not pixel evidence. No completed-run suffix was recaptured.

## App's concrete nonacceptance branch

Actual Netflix eboot SHA256 `be42bd7ccc0b436ef0e4309d78266a9661c8827e1012b564c14d838bd5edf8d9`:

- AVC creator `0x1506be6`: `[object+0x80]==0` selects resource`0x12384`; nonzero selects resource1 (`0x1506c2b`, `0x1506c60..73`). Thus the observed private AVC configuration establishes which resource branch this helper selects, if this is the executing helper.
- Decode routine`0x1506638` calls the export at`0x150681f`, then increments its input ring index. On return0, `0x1506846..4c` ORs `[object+0x80]` with the returned `is_accepted` byte. When both are zero it assigns status**-2**, calls pool routine`0x1504a6c`, writes status at`0x1506874` and returns false through`0x1506894`. The pool routine clears slot flags at+0x28/+0x29 for this call. This is not a normal "wait for another picture" branch.
- Accepted private storage with no picture instead reaches `0x15068d7..f1`, returns true and keeps the slot. Output validity is tested separately. Vtable references to the routine are`0x2563f90`, `0x2563ff0`, `0x2564070`.

The log lacks return-address attribution and the upper caller's status/teardown decision. Therefore it supports, but does not prove execution of, this -2 branch or that it caused the observed title failure.

## Native contract: accepted storage is not a copied picture

Inspected **regular** `13.60/system_b/common/lib/libSceVideodec2.sprx` SHA256 `c380676887c7e1480c6068e75e5986fb11159f95f63dd818406390113ca40dfa`, and regular `libSceVdecCore.sprx` SHA256 `9e358a87b99c606620518df309a4e7ba698922cc213fb1a27712d12df23058a3`.

- Native Decode`0x1290` clears acceptance at`0x1504`. For private resources`0x12384`/`0xb6c8`, `0x1635..165c` passes the supplied storage pointer/size into the core submission structure. It calls core import`F4nrEfi-M84` at`0x17a8` and derives `isAccepted` from the **separate core submission result** at`0x17ad..17b8`, before checking whether output can be collected. Startup queue-depth handling can return success without an output picture (`0x17bd..17ce`).
- Core export`0x1b40` forwards that storage descriptor (`0x1d19..1d70`) to`0x7ee0`; the core copies its acceptance result separately at`0x8d9f..8dbb`. This does not prove every arbitrary header-only AU must accept storage.
- Native picture collector`0x19d0` obtains a backend picture via`K4sH-0WUfuc` at`0x1a46`. For private resources it returns the **backend-selected picture pointer** (`0x1ab1..1aca`), unlike the public branch which sets acceptance and returns the current caller buffer (`0x1ad9..1ae2`). Hence delayed private picture identity cannot be replaced by the next call's buffer.
- Native Flush calls core completion import`b0xbD0x+02M` at`0x1ff2`, then the picture collector at`0x20b5`. The core completion entry is`0x2a50` ->`0xc020`. The old combined backend's Flush only received currently available frames and never signaled FFmpeg EOS.

The prior implementation supported caller-owned copy-out but applied it to private-resource submission too: only `CopyFrame` set acceptance, and every returned picture used the current call's buffer. It had no retained per-AU output-storage identity. This is a demonstrated contract-model gap even though its responsibility for Netflix's exact failure remains unverified.

## Correction and ownership limits

Only admitted private resource`0x12384` enables `Decoder::Config::retain_input_buffer`. Its descriptor travels with the AU's PTS/DTS/attachedData in FFmpeg's refcounted opaque metadata (`AV_CODEC_FLAG_COPY_OPAQUE`). Acceptance is set only when a successful `avcodec_send_packet` leaves an additional backend reference to that token; merely returning success is insufficient. No pixels or picture-valid flag are fabricated.

When a picture is received, its own token selects the destination pointer/size. There is no FIFO substitution that could confuse reordered frames. Missing ownership metadata fails rather than copying into unrelated caller storage. Acceptance on a combined Decode describes **this call's submitted storage**, independent of whether the output belongs to an older AU. Private Flush signals EOS, drains retained pictures, and does not claim ownership of its scratch descriptor. Reset/destruction release decoder/frame references without writing pending pixels. Public compute-resource and ESVM copy-out paths retain their existing mode.

This is a borrowed guest-buffer contract: once accepted, the caller must keep that storage alive until returned or reset/deletion. Refcounted metadata retains its identity, not an OS memory pin. It does not authorize overwriting/unmapping caller storage. The existing private test was corrected to drain accepted storage rather than resubmit it while owned.

## Isolated validation

Built local `libVideoDec2.cpp`, `videoDec2Decoder.cpp` and the test driver using at most2 jobs. Retained pre-Home link manifest and other objects/libraries are read-only inputs, as described in the earlier boundary checkpoint; explicit local header/test-include dependencies were added to the local overlay graph. No production executable was built or replaced. Final focused executable SHA256: `474a7f30ad0a6542fbed2b4a27702c751a96deb0ad8633e3eea2fb1bb582754a`.

`--videodec2-resources-only` passes with trace0 and trace1. Regressions check:

- Real HEVC first AU: accepted1/valid0, untouched sentinel pixels; Flush without another AU produces real pixels in original storage, leaving distinct scratch storage untouched.
- Four independently submitted buffers: each picture returns its own pointer, timestamp and attachedData exactly once, including delayed output; all four drain.
- No duplicate EOS output; reset discards delayed references and prevents writes to retired storage; new lifecycle returns new storage; destruction with a pending AU does not write it.
- Existing actual AVC/HEVC pixels, private/public resource admission, direct memory, early-pointer guards and lifecycle checks continue to pass.

Negative controls linked against the same focused test driver: original backend fails **private first-AU storage retained before picture availability**; original backend plus a flag-only `accepted=true` override fails **actual 64x48 decoded output** (it cannot drain/return the owned picture). These reject both the old behavior and superficial acceptance suppression. Artifacts/logs are local under `_Build/videodec-ownership-validation/`.

Remaining verification: production integration must rebuild consumers of the changed backend Config header. The actual Netflix AU payloads were not captured, so their FFmpeg retention/reorder details are not claimed. No sustained Netflix picture/audio or Apple outcome is established by these component checks. Parent retains integration and future runtime decisions; no additional Netflix retry is requested.

## Parent integration and production validation

Integrated as `b7e20e7d`. With no emulator running, parent rebuilt
`virtual_memory_allocation_tests` and `kyty_emulator` using parallel2. The build
recompiled changed backend-header consumers, including libVdecsw, libVideoDec2,
videoDec2Decoder and the resource tests. Production linking succeeded.

`ctest --test-dir _Build/windows -R '^videodec2_resources$' --output-on-failure`
passed with `KYTY_MEDIA_TRACE=0` and again with `=1` (1/1 each, zero failures).
New executable SHA256:
`ed65417374c9b328b52fe8bfe3296023a7d84a28f10f08bbf01d42a8636e928e`.

This is production build/component validation, not a new Netflix attempt. User's
current priority remains the Apple/firmware protected-media deep dive; no app
was automatically relaunched at this checkpoint.

## Parent Netflix retry after Apple/Home checkpoints

User explicitly requested launching Netflix to continue media playback work.
A fresh inventory found no emulator running; Home candidate PID27884 was already
absent, so no closure was performed. Its cloned stores, queue traces and visible
Settings evidence remain preserved; the IDA Home investigation continues.

Parent launched **mnetflix5** from the current verified production build7089743,
SHA256 `498f1d6dc0a9a1a866e9afa4c68062798104f97320bd7f7f7170e1b392f61d5c`.
This includes integrated private-buffer fixb7e20e7d and later Apple source/color
fixes. Original `mnetflix1/launch.json` arguments/environment and all four original
store mounts were replayed, changing only the guest log destination.

Identity/logs: `${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix5`.
PID16036, creation FILETIME134386290476080873; fresh inspection matched identity
and executable path. The responding Netflix PPSA01616 window showed frame546,
60fps. This establishes startup/window activity, not authenticated playback.
User was asked to retry the same title that previously failed at24% (prior report:
Pablo Escobar, le patron du mal), with controller navigation user-owned.
Title selection, new Decode acceptance/output, sustained video/audio and any
next failure are pending. No media playback completion is claimed.

## mnetflix5: video confirmed, audio correction pending

User subsequently confirms **video is playing**, then describes audio as
"distorted crackign sound repeating". This establishes visible playback beyond
the earlier loading failure, but not completed audiovisual playback. Current
title and sustained duration remain unconfirmed.

Completed guest log: 49,156,110 bytes / 1,630,971 lines, SHA256
`24457a5c914620b10468a21c854fa71a1a8027cf907fe09d5c1585c92e9930ed`.
Numeric extraction is retained at `mnetflix5/playback-audio-summary.json`:
3 query, 1 create and 65 decode records; 64 accepted decode records and 62
valid-picture records, with no parsed nonzero decoder result. These bounded
diagnostic counts are not total playback frames. Picture output reaches1280x720.

Audio evidence: SDL output48000Hz/8channels/format0x8120; NGS2 sampler setup at
guest line1075564 is type0x18/6channels/48000Hz, fields16/20 zero. Host records
ignored custom control0x40002000 and render module0x20/index0. AudioOut imports
HkXJ7FFaQK8, PhnNN0UEV6Y and lyHeaetMYXg remain unresolved. None of these
observations alone establishes the cause of distortion or repetition.

Terra's active media task covers actual sampler/graph/channel contracts, buffer
ownership, pacing and AudioOut, with meaningful sample regressions required.
No audio fix has yet been integrated or verified. Latest process inventories
found no emulator, including PID16036; parent did not close it and exit cause
is unestablished. Original stores and logs are retained.

Before any replacement build, parent copied and hash-verified the video-playing
production executable to `mnetflix5/video-baseline/kyty_emulator.exe`, SHA256
`498f1d6dc0a9a1a866e9afa4c68062798104f97320bd7f7f7170e1b392f61d5c`.
This is a comparison executable, not an independent replay manifest or store.
