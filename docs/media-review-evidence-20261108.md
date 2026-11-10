# Media review and integration checkpoint — 2026-11-08

## Evidence matrix

| Application | Established | Not established |
| --- | --- | --- |
| YouTube | User reports some videos play. Clear and protected streams can use different codecs, pipelines and access requirements. | The successful videos' codec, protection and pipeline were not captured here; success does not prove another application's path works. |
| Apple TV | mapple3 showed actual HEVC output and sufficient queued data, but repeated configuration cleared a pending source first-output Sync before readiness. The native source gate and the reproduced negative-control fixture support a real startup bug independent of any established protection failure. | Screenshot was marketing/sign-in UI; the attempted content and protection are unknown. Readiness after this patch, presentation, PCM and sustained app playback have not been live verified. |
| Netflix | Original mnetflix1 20% attempt reached a zero-length `vdec dmem` allocation and EINVAL; rejected private Videodec2 admission is a source/native-backed upstream explanation, not a logged query result. In mnetflix4, private AVC/HEVC creates succeeded, but four initial Decode records returned success with accepted=0 and no picture. Native and consumer analysis justify the integrated delayed-buffer ownership correction. | The initial Decode tuples have no caller attribution; their relation to the later 24% title error is unproven. The bounded prefix did not capture a full attempt or subsequent decode/flush; no post-fix title retry or sustained picture/audio has been validated. |

Different apps may use different container/codec profiles, decoder APIs, output
buffer lifetimes, audio paths, authentication and content protection. Successful
codec decoding alone does not establish a valid license or secure output path;
conversely neither app's visible error establishes a DRM failure. See
`apple-startup-sync-20261108.md`, `netflix-videodec-admission-20261107.md`, and
`netflix-private-buffer-ownership-20261108.md` for trace boundaries.

## Integration and independent validation

Cherry-picked Apple source fix 32ffbdec as 74f9f165. Rechecked native regular
13.60 engine disassembly: successful Flush sets source state +1d8=1 at 1e088;
positive output after zero count consumes that state and invokes Sync at
226d7..22706; Sync invalidates timestamp validity and sets flag 2 at
199d0..199f6. The native gate contains no host generation comparison. The
same-child/zero-prior-output restriction is a conservative host mapping, not a
claim that every native reconfiguration case has been observed.

Built a local overlay with `ninja -C _Build/apple-readiness-validation -j 2`:
source, MediaFramework wrapper and test driver were compiled from this tree;
remaining objects and libraries were borrowed read-only from the retained parent
link manifest. Build passed with warnings, not a full clean production build.
Test executable SHA256
`70a2d4a6b6793c40c7b99d73410140cb4440c20d82a5e76074e2c7679ec6087c`.
`--media-framework-esvm-source-only` exited zero with `KYTY_MEDIA_TRACE=0` and
`=1`. After conversion of PowerShell UTF-16 output to UTF-8, the repeated-HEVC
trace checker passed: disabled=0, enabled=821, all four control variants. The
original author's negative control and other constraints are recorded in the
Apple note; this review did not rerun the negative-control build.

Netflix fix b7e20e7d is already an ancestor of the base; no duplicate patch was
applied. Cherry-picked the independent static ARM/DRM audit 5a04d48f as
53be2b5d; its stated 23 structural checks are original-agent results, not
independently rerun here.

## Protected-media prerequisites and handoff

The audited native PlayReady4 route needs actual provider/MediaKeys and secure
session admission, a functioning TEE endpoint and TA execution, genuine license
exchange/entitlement and decrypted sample handling, plus any required protected
output guarantees. Firmware contains native components and MP3 ARM/Thumb code,
but boot mapping, privileged execution, device/mailbox, TA admission and the
secure runtime remain unresolved. Returning successful ioctl/session results
without implementing them would fabricate success. See
`protected-media-dispatch-tee-contract-20261108.md` and
`protected-media-construction-arm-firmware-20261108.md`; their static paths are
not evidence that either retained app attempt reached the protected route.

Parent owns production integration and all launches: rebuild relevant consumers,
then test a known-clear Apple preview first, recording source readiness, actual
presentation and PCM. Separately test identified protected content only with a
genuine provider/license flow. For Netflix retain a new full bounded attempt with
the corrected build, actual Decode acceptance/output and caller attribution,
then verify sustained picture/audio; a percentage is not playback. Preserve all
existing stores. No emulator was launched or controlled, and no shared build,
credential contents or live emulator process was modified by this review.

## Parent production integration and new attempt

Integrated Apple fix as `468bc7ab`, ARM audit as `ee348cb5`, and this review as
`be35e1fa`. The normal production `virtual_memory_allocation_tests` and
`kyty_emulator` targets rebuilt with parallel2 using the installed VS18 Build
Tools environment. An initial VS2022 environment failed linking older runtime
libraries against retained newer objects; its newly compiled objects were
removed and rebuilt with VS18. Logs: `Temp/opencode/apple-sync-parent-build.log`
and `apple-sync-parent-build-vs18.log`. No production launch used the failed build.

Fresh integrated source-only tests passed with `KYTY_MEDIA_TRACE=0` and `=1`
from isolated working directories. `check_apple_readiness_trace.py
--repeated-hevc` passed: disabled0/enabled820, all four repeated-HEVC controls,
source cap64+1, blocked predicate/EOS and callback checks. Results are retained
under `Temp/opencode/apple-sync-parent-validation/`.

- Production: 35,721,728 bytes, SHA256
  `516e8cb3b0d8488d0fa9a6fa8126665a3ecf89e976530ced42dfd9476d65c975`.
- Test executable SHA256:
  `f093da60ab04fe65da82615cc7c67371a323628797df1d9283a502d498515499`.

After confirming no emulator was running, parent launched `mapple4`, replaying
the original `mapple1/launch.json` arguments/environment and all four original
store mounts, changing only the guest log destination. Evidence directory:
`${USERPROFILE}/AppData/Local/Temp/opencode/mapple4`.
PID36240, creation FILETIME134386243332018858; fresh process inspection matched
both identity and executable path. The responding Apple TV window reported
build `dde5d43`, PPSA01783, frame551. This proves launch/window activity only.
Controller navigation is user-controlled; known-clear preview picture/audio,
runtime readiness, protected playback and Netflix title retry remain unverified.

### mapple4: readiness crossed, decoded color rejected

User reports the closest progress so far: Play button and controls are visible.
They tried a **free episode**. Free availability does not establish clear versus
protected encoding; no particular title or protection status is established.
Parent did not request abandoning that attempt or supply controller input.

Fresh process inspection still matched PID36240/creation FILETIME and executable;
the responding window had reached frame10362. Parent read exactly one bounded
guest-log prefix, 119,127,022 bytes / 3,618,191 lines, SHA256
`3d82a6f3d34a89bb86af7fc001fc02d61efb5bc13955e7d637af514ca9932652`.
Only parsed numeric source/state records were retained in
`Temp/opencode/mapple4/controls-visible-prefix.json`.

Observed event counts: seven `source-ready-delivered`, seven
`audio-lease-opened`, nineteen `audio-pcm-accepted`, seven
`video-mailbox-activated`, and seven `decoded-colour-rejected` with result
`81790fff`, detail0. First readiness is line892839; first mailbox activation
898085 is followed by color rejection898091. Eight source admissions and sixteen
decoder removals occur in the prefix; these are multiple source lifecycles, not
seven proven playback attempts. State records total229.

This establishes live progress past the previously missing readiness boundary
and backend acceptance of some PCM. It does not establish audible output or
presented moving video. The color rejection is a new concrete downstream failure,
assigned to the same media Terra agent for an evidence-backed conversion/output
fix. The source fix remains separate from this failure and from protection.
