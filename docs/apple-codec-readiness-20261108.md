# Apple codec/source readiness checkpoint — 2026-11-08

## Established runtime boundary

One bounded pass over completed `mapple2/guest.log` read 342850799 bytes,
10295771 lines, SHA256
`2a1596a0ed898e2c44d749e3c93e34bb8fb025b01421f404629cfd68cd3b66b7`.
Sanitized numeric records are retained locally at
`_TempData/mapple2-audit/numeric-records.json`; the large guest log was not reread.
That run used production `cff5c4b`, executable SHA256
`51a39858abcdf853a1a32c69c39000f3c207bac54cccacf90f719d427ff8dc6c`.

* Six sources selected HEVC (wire subtype 2). Sources 65536/65539/65542
  selected AC-3 (5); 65545/65548/65551 selected E-AC-3 (6). Each source's
  video child is source+1, audio child source+2. No AAC configuration was captured.
* All six startup Flush operations succeeded. All 24 submissions/completions,
  12 refills and 12 removals recorded result zero. Eighteen first-record
  subtype observations and six first-record AU observations describe batch
  prefixes, not total AU counts; later HEVC batches can contain config and AUs.
* Each of twelve children has two decoded-queue observations: 0, then 4.
  The 24 records sum to occupancy 48 (24 video, 24 audio), **not a produced-frame
  count**. No subsequent Submit/refill precedes retirement in these unsaturated
  trace budgets. No source-ready delivery or PCM acceptance was recorded.
* First source: admission line 544058, startup Flush 544905, video queue 4 at
  546316, audio queue 4 at 548540, removals 803579/803645. Last source:
  admission 5536218, Flush 5536864, video 5538127, audio 5539305,
  removals 5677536/5677574. These establish ordering, not elapsed time.

The first missing recorded boundary is source readiness. The old trace does
not reveal whether Pause/Sync was armed, the upstream payload count, held frame,
EOS, or dispatch eligibility. Four output frames suppress host refill under
the current capacity policy; this alone does not establish a native deadlock
or justify lowering threshold 12. The Apple title and protection classification
remain unknown. Playback recovery is unproven.

## Native ownership and codec scope

The pinned disassembly in [native readiness audit](media-readiness-native-20261107.md)
establishes engine predicate RVA `0x19760`, state 3→1, normal-rate threshold 12,
and EOS producer `0x19b50`. Native accounting includes distinct in-flight
timestamps and allocator availability, which host queue counts cannot invent.
Engine identity is
`95df64b49dedd751913b423106ff547ed6b56d81b356d3d27025e3cfa90f2b9b`.
Source callback translation ends at source event `0x2001`, not direct engine
event 7. Regular 13.60 `libSceEsvm.sprx` identity is
`31593167e10813bdc195fb099da1da723c36818c063c35de6f6a086962d2e3f2`;
source callback RVA `0x24ec0`, decoder event `0x253a0`, completion `0x25470`.

Read-only inspection verified these `system_b` codec images as actual ELF64
AMD64 (class 2, machine `0x3e`), rather than inferring support from filenames:

| `libSceAudiodecCpu*.sprx` | Directory | SHA256 |
| --- | --- | --- |
| Ddp | common/lib | `8b2cab55ed77df39f9537addaf18dbebf77796ad3b2c17c5b4972e85729d6516` |
| M4aac | common/lib | `74287a9327d0852b4b6bead0647f9bca29a3bccbf21f6aa52bbf87cbfc78e5a3` |
| Dts | priv/lib | `ba12e9ce16f4ecafb7d338891dbedf2c05ac71d5d0b29b62a8555c0216d2f1d8` |
| DtsHdMa | priv/lib | `210b574c45816f825d5717c62a8ddac64f172ced4e2d10af1335dffebd6a59cc` |
| Lpcm | priv/lib | `1199fd2526d360f60d4833622c97d88ac1cc6252ec6e3a034f60ebf39673568a` |
| Opus | priv/lib | `ff6b4fc2796ffe74def7c9e5600c7d2768359bcc90930f9ce176bb7ecdf40d54` |
| Trhd | priv/lib | `e37982353a5f62b28a85ba4de8ddd3d679605039e3fa0cff129a8df421c9b5f1` |

This is an inspected subset, not a complete firmware codec capability matrix.
The HLE ESVM decoder routes to FFmpeg `aac/ac3/eac3/h264/hevc/vp9`; installed
native codec modules do not automatically extend those routes. Actual Apple
selection in this run was HEVC and AC-3/E-AC-3. DRM/TEE/key-file investigation
belongs to the separate native DRM assignment.

## Implemented observations

`KYTY_MEDIA_TRACE=1` adds per-source/per-child state records at controls,
readiness observation, refill/readiness claims, configuration/submission,
Flush and retirement. Wrapper return-PC forwarding identifies the guest
Pause/Play/delete caller; direct service entrypoints capture their own caller.
`entry`/`caller` describe the last captured entry, not necessarily the producer
of every later internal observation. `host-us` is a host monotonic timestamp.

Records expose actual flags, timestamp presence, pending Sync, selected child
and generation, queues, held-frame/EOS, host predicate, callback eligibility,
refill capacity/pending, staging, audio format and failure state. `batch-units`
includes EOS-only controls; `payload-aus` is the current payload-only queue.
`subtype` is the batch's first record. `allocator-known=0` explicitly marks the
unobserved native allocator fallback. `eligible` is only a necessary delivery
gate; `ready-claimed` and existing `source-ready-delivered` identify later
boundaries. No payloads, keys or URLs are emitted.

Deduplication is per child (or childless source), including callback/guest/depth
gates but excluding time/caller. Repeated unchanged polling is inert. Limits
are 64 state records per source plus one marker, and 1024 process-wide records
(including source markers) plus one global marker. Diagnostic state is separate
from readiness; Pause/Play/Sync, threshold and capacity policy are unchanged.

## Verified isolated validation

After parent build completion, `_TempData/build-apple-readiness-validation.py`
built with `-j2`: source service, native wrappers and full test TU, linked against
read-only parent objects using the retained pre-Home link manifest. This is an
isolated test overlay, not a production rebuild. Test executable SHA256:
`6090c2875524d96df3d7358838867257b5115837bd411656c206361db98528f6`.

`--media-framework-esvm-source-only` passed with tracing 0 and 1. Existing
startup Flush variants, genuine threshold/EOS readiness, callback reentry,
source deletion and owner retirement remain exercised. Added saturation and
blocked polling regressions verify diagnostics cannot generate readiness.

```powershell
python -B tools/check_apple_readiness_trace.py _Build/apple-readiness-validation/source-trace-0.log _Build/apple-readiness-validation/source-trace-1.log
```

Machine-checked results: disabled 0 records; enabled 660; saturation fixture
exactly 64 records plus one marker; blocked flags=3, decoded=4, payload=1,
held=1, EOS=0, sufficient=0, refill-capacity=0; admitted EOS changes sufficiency
and leads to a readiness claim. Pause caller PC and host timestamps are nonzero.
The assertion helper initially expected two blocked fixtures; review confirmed
one blocked fixture and two separate startup variants, and corrected that
helper expectation before the passing check.

## Remaining handoff

The parent owns production integration and runtime decisions. A future bounded
trace must capture actual Apple Pause/Play/Flush order and the blocked source's
predicate/dispatch gates, then readiness delivery and PCM acceptance. Neither
this isolated VP9 fixture nor prior HEVC queue occupancy proves sustained Apple
audiovisual playback or support for every installed native codec.

## Parent validation and mapple3 observation

Integrated `1312f11d` as `6b8cc8b7` on `ps5-vsh`. Parent rebuilt both
`virtual_memory_allocation_tests` and `kyty_emulator` with parallel2, including
the changed source/header consumers. Source-only tests passed with tracing0/1;
`tools/check_apple_readiness_trace.py` reported disabled0/enabled660, cap64+1,
blocked predicate, EOS, readiness claim, caller and timestamp checks passing.
Logs are in `Temp/opencode/apple-readiness-parent-validation/`.
Production executable SHA256:
`048351953452fa10d6bc7d0f7081c5b20b71dfff1159f8e58056b84ce0e72739`.

Parent launched mapple3 PID15320 from that verified executable using the
mapple1 arguments, original four mounts and environment, changing only the
guest-log destination. Launch manifest is `Temp/opencode/mapple3/launch.json`.
Subsequent process observation matched the executable and launch time; the
window reported build6b8cc8b, responded, and advanced to frame8330. Screenshot
`mapple3/window-initial.png` shows Apple's marketing/sign-in screen, not an
authenticated browsing screen. Original stores were retained; this observation
does not establish their authentication state. Parent supplied no controller input.

One bounded pass over the first75012228 bytes retained42 numeric state records
in `mapple3/readiness-prefix.txt`, covering two source lifecycles. There were
four decoder removals and no ready-delivery or PCM-acceptance records. A raw
substring count of four `source-admitted` occurrences double-counts event/state
records: there are two actual sources65536/65539.

Both lifecycles demonstrate a more specific boundary than mapple2:

- Pause caller `0x940017e6e` arms flags1, timestamp0. Input startup later has
  waiting-sync1 and selected video generation3.
- The next HEVC config/payload submission advances video generation to4.
  Source65536 has decoded4, payload-AUs14, held1, sufficient1; source65539 has
  decoded4, payload-AUs18, held1, sufficient1. Threshold remains12.
- Observation updates selected generation to4/revision3 and clears waiting-sync
  to0, while flags1, timestamp0, pending0 and eligible0 remain.
- AC-3 audio reaches decoded4, payload-AUs27, held1, 48000Hz/stereo. Both
  sources retire about11 seconds after the video submission with no recorded
  ready delivery or accepted PCM.

Thus insufficient video buffering is not the observed gate here: the host
video sufficiency predicate is true. The generation-selection/Sync transition
is the next concrete investigation, assigned to the media agent with this
snapshot. Its causality and native-backed correction remain unverified. The
content identity and whether these were automatic previews or user-selected
attempts are unknown. No Apple playback recovery is claimed.
