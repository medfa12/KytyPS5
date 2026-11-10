# mapple4: decoded HEVC colour metadata at mailbox — 2026-11-08

## Live observation and exact failure

Parent's sanitized numeric `mapple4/controls-visible-prefix.json` covers
119127022 bytes / 3618191 lines, prefix SHA256
`3d82a6f3d34a89bb86af7fc001fc02d61efb5bc13955e7d637af514ca9932652`.
Parent identifies production build dde5d43, executable SHA256
`516e8cb3b0d8488d0fa9a6fa8126665a3ecf89e976530ced42dfd9476d65c975`.
User reports controls and trying FREE EPISODE; neither content protection nor
successful video presentation is established. First source 65539 delivered
readiness at line 892839; mailbox for child 65540 activated at 898085, then
`decoded-colour-rejected` at 898091. Seven rejections were recorded alongside
seven readiness events and 19 PCM acceptance events. Audio acceptance is not
speaker presentation. Rejection is a real host service failure, but it is not
by itself a proof that the entire episode failure has only this cause.

`PublishDue` rejects frames whose transfer/primaries fail `SdrColour`, then
sets service failure and retires the mailbox. The old trace hard-coded bytes=0
and detail=0, so those numbers are **not** colour metadata. The HEVC config
serializer subtype 2 has no separate colour fields: regular 13.60 libSceEsvm
2df30 copies a 16-byte prefix, three length-prefixed blobs and one flag
(2df8c..2dff9). Existing native analysis identifies these as HEVC NAL size,
VPS/SPS/PPS and byte-stream flag. The host starts `Config` colour fields at 2
(unspecified), and only the VP9 parser writes explicit colour fields. Thus all
host-decoded AVC/HEVC frames previously left `Convert` with transfer=2,
primaries=2, even if FFmpeg's frame contained valid BT.709 VUI; `SdrColour`
necessarily rejected them. The retained numeric prefix does not tell us the
actual AVFrame colour metadata or whether these frames were SDR/HDR.

## Correction and boundaries

When a config field is unspecified, the decoder now carries the corresponding
actual FFmpeg `AVFrame` matrix/transfer/primaries into `EsvmDecodedFrame`.
Explicit config values retain existing precedence. Values that are genuinely
unspecified remain unspecified; PQ/HLG decoded frames remain rejected before
conversion. Source continues to reject unsupported/unknown transfer or primaries.
This is a metadata propagation fix using the existing real swscale 601/709
conversion; it does not implement HDR tone mapping, wide-gamut conversion,
protected decode, license access, or fake presentation. It does not prove that
the mapple4 stream's VUI was BT.709. Existing decoder handling of unspecified
matrix/range retains its prior policy; this change does not assert perfect
colour for all metadata combinations.

The rejection now reports numeric matrix in `bytes`, `(transfer << 8) |
primaries` in `detail`, with its own process-wide budget of 16 (independent of
lifecycle spam). No pixels, AU payload, credentials or URLs are logged. For
example `bytes=2 detail=514` means matrix=2 transfer=2 primaries=2. FFmpeg's
existing `Esvm HDR conversion unsupported` stderr diagnostic separately reports
numeric decoded pixel format, matrix, transfer, primaries and dimensions before
this source gate when it detects PQ/HLG. Neither diagnostic existed with usable
source colour values in the original mapple4 prefix.

## Focused validation

Local overlay built `mediaFrameworkEsvmDecoder.cpp`, source, wrapper and test
driver from this tree with at most two compile jobs; the remaining objects and
libraries came read-only from the preserved parent manifest. This is not a clean
production rebuild. Source suite passed with trace0 and trace1. The real 10-bit
BT.709 HEVC red fixture reaches the mailbox with actual red RGBA pixels; a real
HEVC fixture with unspecified VUI remains rejected. The trace-on unknown fixture
reports matrix=2, transfer=2, primaries=2 (detail 514), even after the lifecycle
trace budget was spent. The existing HEVC PQ fixture remains rejected.

Meaningful negative control: a second local test binary linked the same new
source/test objects against the decoder from parent commit 97d05f12 and exited
1 at `BT.709 HEVC frame reaches mailbox instead of losing VUI at source gate`.
The standalone decoder suite passed with assertions for decoded BT.709 VUI,
unknown VUI remaining unknown, actual red pixels and PQ rejection. Readiness
trace checker also passed, disabled=0 enabled=840 with repeated-HEVC controls.
Test SHA256: source driver
`1dda902331df80253d7a4ef271af5b0789cb0d16676a0f41e8b8cb13db455bc1`;
standalone decoder
`5bd144656017685e7494b1288fbf6b38bc55b7fd307b09314b624adaac27b3ba`.

## Parent integration

Cherry-pick this code/tests/docs commit, then rebuild **both**
`virtual_memory_allocation_tests` and `media_framework_esvm_decoder_tests`,
and the production `kyty_emulator` with parallel at most 2. Run the standalone
decoder suite and source suite with `KYTY_MEDIA_TRACE=0` and `=1`; run
`tools/check_apple_readiness_trace.py` on UTF-8 source logs with
`--repeated-hevc`. The parent controls any later retry, UI and process lifecycle.
On a new bounded attempt record colour rejection details or HDR diagnostic,
mailbox publication, actual render/present and audible output separately. If
metadata remains unspecified, or HDR is detected, do not substitute BT.709;
the exact stream metadata/required conversion remains a blocker.

### Parent production validation and mapple5

Integrated as `70897430`. Parent rebuilt normal production targets
`virtual_memory_allocation_tests`, `media_framework_esvm_decoder_tests`, and
`kyty_emulator` with VS18 Build Tools and parallel2. Build log:
`Temp/opencode/apple-colour-parent-build.log`.

Both integrated source-only runs passed with tracing0/1 in isolated working
directories. Repeated-HEVC checker passed (disabled0/enabled840), as did the
standalone decoder suite. Logs: `Temp/opencode/apple-colour-parent-validation`.
SHA256 receipts:

- Production: `498f1d6dc0a9a1a866e9afa4c68062798104f97320bd7f7f7170e1b392f61d5c`.
- Source test: `f4ba325487582044b12e864467ab516c152492c3b5463e579bdc3d10a6599ae7`.
- Decoder test: `8ab5b0cb7ef957ed093422b4452aaa77d83667b416a179517b8be5af25054969`.

After the invalid Home clone control was closed and no emulator remained,
parent launched **mapple5**, replaying original mapple1 arguments/environment
and all four original stores, changing only the guest-log destination. Identity
and logs are in `${USERPROFILE}/AppData/Local/Temp/opencode/mapple5`.
PID26208, creation FILETIME134386258218840330; a fresh observation matched both
identity and executable path. Responding window reported build7089743, frame374.
User was asked to retry the same free episode. Presentation/audio and runtime
color result remain pending; successful launch and offline fixtures are not live
playback acceptance. Home clone relocation is being corrected independently.

### mapple5 first bounded runtime prefix

Parent captured one numeric source-event prefix:250563483 bytes /7334842 lines,
SHA256 `758ccf10429fd159b8ac27cbc99f749d9b8739c98030fe867299569b6b8f5303`.
Receipt: `Temp/opencode/mapple5/colour-fix-prefix.json`. It records two source
admissions, one readiness delivery, one audio lease,32 PCM acceptances, one video
mailbox activation,16 render-mailbox selections,12 refills and one explicit
Flush. No parsed nonzero source result or decoded-color rejection occurred in
this prefix. This is progress beyond mapple4's demonstrated rejection, not proof
of moving display/audio or of the selected episode's protection state. The
render diagnostic budget is16, so counts are not total render/frame counts.
