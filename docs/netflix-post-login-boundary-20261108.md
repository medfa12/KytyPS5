# Netflix post-login boundary checkpoint — 2026-11-08

## Finding

The user's exact clarification is: **“it was responsive in the login page when i logged in it faded to black”**. This establishes a rendered, responsive login UI, a visible login-triggered fade, and persistent black afterward. It does not establish authentication-server success, completed post-auth navigation, playback, or a compositor defect. The cause of the persistent black screen remains unproven.

The earlier NGS2 F32LE startup fix is separate (see `netflix-ngs2-float-startup-20261108.md`). The current bounded prefix contains two `type=0x18 channels=6 rate=48000 field16=0 field20=0` sampler setups and no recurrence of the original fatal. This supports startup progression, not successful audible output.

## Fixed evidence boundary

Parent owns live PID 22780. Analysis used size-at-start snapshots captured at `2026-11-08T11:43:45.868834+00:00`, retained locally under `_TempData/mnetflix3-audit/`:

| Snapshot | Bytes | Complete lines | SHA256 |
| --- | ---: | ---: | --- |
| guest.log | 569301521 | 18798081, plus partial tail | `6109491e9f72b93b3d19d35b3e07e23c3549149c956800b7dee36fad5a6a5530` |
| host.log | 3776 | 44 | `4015720d959789f029ab1c73ea8145adb06779a3aa2de596088d125d134b7cc3` |

The parent-reported active executable is source `e758b67`, SHA256 `a3a95197873b5db433caada3b25b6c395d75c43322ccc50a46c679cb56fcef91`. The instrumentation below is not in that executable.

- There are 366 swapchain-recreation failures and 184 exhausted-presentation-retry records, confined to two minimize/restore intervals around guest lines 2033877–2119511. The first follows suboptimal acquisition and minimization; restoration/exposure and Fifo fallback follow the final failure. Later records continue through `present layered x54210`. These transient failures do not establish the persistent black-screen cause.
- Two `host-exc` records with code `0xe06d7363` occur near restoration. The prefix does not demonstrate an unhandled terminal exception.
- There are 1300 sampled `draw-executed` records, latest submit 108419. Neither draw records nor flip counters measure pixel contents. In `src/graphics/presentation/videoOut.cpp`, `FlipQueue` increments the layered counter on `any_due`, after an optional `Present`; the count is not a presentation-success result.
- Videodec2 matches comprise relocation mentions only (24 library-name matches, one QueryDecoderMemoryInfo and two CreateDecoder matches). They neither establish nor exclude execution. The completed old mnetflix1 log also has relocation-only matches; it lacks the current swapchain-failure and `0x40002000` records. Proximity or novelty of these records is not causal attribution.
- No captured result establishes authentication completion or identifies the application state after the fade. Application/network outcome attribution and the post-login UI/navigation boundary remain open.

## Minimal missing-boundary diagnostics

`src/libs/libs.h` defines the library `g_print_name` as `thread_local`, initially false. Enabling `PRINT_NAME` during library registration does not enable decoder-worker call logging. Existing call-name silence therefore cannot prove that the decoder was unused.

`src/libs/libVideoDec2.cpp` now wraps the exported QueryDecoderMemoryInfo, CreateDecoder, Decode and Flush return boundaries. Original ABI wrappers and implementation return values are preserved. `KYTY_MEDIA_TRACE=1` enables `Videodec2 result:` records independently of `PRINT_NAME`, subject to the normal logging destination. The environment choice is cached on first use.

- Query/create report the actual return code, configuration, returned requirements versus supplied memory, and created decoder identity.
- Decode/flush report the actual return code, whether output was initialized/observed, buffer acceptance, picture validity/count, error-frame flag, codec, dimensions, pitch and buffer size. API success, accepted storage and a valid picture remain distinct.
- Prevalidation failures report unobserved output without dereferencing descriptors the implementation never inspected. Payloads, URLs and keys are not logged.
- Per operation, success records are capped at 32 for query/create and 64 for decode/flush; error records are capped separately at 16. A saturation marker identifies each exhausted class. These are process-wide budgets, so a cap can hide later transitions; absence after a cap is not evidence of inactivity.

Source-backed Netflix consumer/call-site attribution is retained in `netflix-retest-attribution-20261108.md`; the resource regression uses the query configuration from Netflix helper RVA `0x1506d44`. This checkpoint supplies observability, not a demonstrated black-screen correction. A valid decoder picture would still leave application composition and final presentation to establish; no decoder call would still leave authentication/navigation to establish.

## Isolated validation and provenance

Built the local test driver and Videodec2 objects with at most two jobs, using `_TempData/build-videodec-trace-validation.py`. The parent's newer CMake link graph initially failed because it references unbuilt Home objects and a source absent from this baseline. The successful overlay instead used the retained pre-Home `netflix-stall-investigation/_Build/netflix-validation/virtual_memory_allocation_tests.link.rsp`, replacing only the local test-driver/Videodec2 object paths and output paths. Other objects/libraries were borrowed read-only. This is a focused component check, not a fresh production build.

Final test executable: `_Build/videodec-trace-validation/virtual_memory_allocation_tests.exe`; SHA256 `fd0cbd98c01308650c5c965f41589e1f2aecfc50e2d9281ebaf93d0c328c3ff8`.

`--videodec2-resources-only` passed with `KYTY_MEDIA_TRACE=0` and `=1`. Local logs are `resources-trace-0.log` and `resources-trace-1.log` in the same build directory. After stripping console ANSI escapes, the result parser verified zero versus 46 diagnostic records, real AVC/HEVC 64x48 valid-picture tuples, no-picture success, decode/flush untouched-output guards, initialized flush output, exactly 16 query-error records and one error-cap marker. The suite checks actual decoded pixels, resource/direct-memory admission, worker-thread query requirements and lifecycle behavior. The focused test driver enables console diagnostics only for the opt-in resource run. Initial silent logging and an ANSI-unaware parser were corrected before this final validation.

## Handoff

Verified: F32 startup progressed beyond the earlier fatal in the captured prefix; focused decoder output tests pass with diagnostics on and off. Unresolved: authentication result, post-login application/fade completion, actual live decoder return/output tuples and rendered/presented pixel contents. Parent retains integration and future launch decisions. This checkpoint involved no live-process control or production rebuild.

## Parent integration validation

Integrated `b16f7fad` as `7bab9dcc`. Parent successfully built the full current
`virtual_memory_allocation_tests` target with parallel2, then ran
`--videodec2-resources-only` with `KYTY_MEDIA_TRACE=0` and `=1`. Both passed,
including actual AVC/HEVC pixels and early-return pointer guards. This supersedes
the isolated borrowed-object validation for the integrated test executable.

Completed shell evidence: `sh_1bb1030170010C9eNFgsR5No3M.out` under the parent's
OpenCode shell evidence directory. ANSI-stripped output verification found46
result records, four valid-picture records, one query-error cap marker at16,
and two focused-suite pass markers. These pictures are test fixtures, not Netflix.

Process inventory after validation still shows active Netflix PID22780 using the
old production executable. The emulator has not been rebuilt with these diagnostics
or relaunched. User was asked to close the retained black-screen run; parent is
awaiting closure before the production rebuild. The current failure remains the
responsive login page fading to persistent black after submission.

## Diagnostic production build and next launch

User explicitly authorized closing/restarting assistant-launched instances;
this preference is retained in AGENTS.md. The shutdown/build job verified no
emulator remained before successfully building `kyty_emulator` with parallel2.
Its output contains no graceful-close result, so no specific closure mechanism
is asserted for mnetflix3.

New executable SHA256:
`51a39858abcdf853a1a32c69c39000f3c207bac54cccacf90f719d427ff8dc6c`.

Parent checked that hash and launched `mnetflix4`, PID25740, with the original
mnetflix1 mounts/environment and only a new guest-log destination. Original
`KYTY_MEDIA_TRACE=1` was asserted. Evidence is in
`${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix4/`.
The75-second exit/window/decoder-result observation is pending at this checkpoint.
No controller input or renewed playback acceptance is claimed. Navigation probes
in `netflix-login-navigation-ownership-20261108.md` remain unimplemented candidates;
this build adds decoder return/output observations only.

## mnetflix4 bounded live result

PID25740 survived the75-second observation and was responding. Window title
reports source cff5c4b. The foreground screenshot `window-startup.png` shows the
Netflix Plans page, Sign In header and a red loading spinner over dimmed UI.
It establishes visible application content, not successful authentication or
playback. Parent supplied no controller navigation; whether the user attempted
login in this new run has not yet been established.

Actual decoder observations (guest line references):

-911857: AVC create returns0, decoder ffa306a4c0.958140: decode returns0,
  AU2284 bytes, output buffer4194304 bytes, accepted0, valid0, pictures0.
-1027057/1073985 and1166419/1214331: two newly created AVC decoders repeat the
  same first-AU size and no-picture/unaccepted-output tuple.
-1796862: HEVC create returns0 (1920x1080, depth2), decoder5c5e50.
 1842376: decode returns0, AU15843 bytes, output buffer27262976 bytes,
 accepted0, valid0, pictures0.
- Four NGS2 setups report0x18,6 channels,48000Hz and zero field16/field20.

These observations establish execution and actual output values missing from
mnetflix3. No nonzero decoder API result appears in the bounded report, and no
valid picture is established. Buffer nonacceptance with a successful no-picture
return needs native/app contract analysis; it is not automatically an error.
The media agent now owns that focused investigation, including the actual Netflix
consumer's nonacceptance branch and delayed-output ownership semantics. The UI
fade cause remains unproven. No repeated login or playback success is claimed.
