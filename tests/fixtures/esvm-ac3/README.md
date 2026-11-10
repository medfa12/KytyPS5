# Synthetic AC3/EAC3 decoder fixtures

`generate.py` uses the FFmpeg CLI's AC3/EAC3 encoders to create three complete
1536-sample syncframes per case at 48 kHz, 192 kbit/s. No service media is used.
Tests consume the checked-in `packets.h`; regeneration is optional.

* Stereo: left 440 Hz and right 880 Hz, amplitude 0.25.
* 5.1: center-only 660 Hz, amplitude 0.25; all other channels silent. This
  verifies actual decoder downmix, not accidental front-channel truncation.

Generated using FFmpeg 8.1.1 (Gyan full build). SHA-256 of each three-packet set:

| Case | SHA-256 |
| --- | --- |
| AC3 stereo | `b33d6dd0da99ce966b1b7779a65ba0623ec9fb6bffe7b26dc1ca603caace0a63` |
| AC3 5.1 | `46aaadf3cdfce1197ce648a14ac6514deb11c7e8cb4f76b6a23b38e11d7a2e70` |
| EAC3 stereo | `398c5b157b69a105d35694b36aa6f65a1d8fbe9c45298e94417184aa29751012` |
| EAC3 5.1 | `9d218fd9085bedd91e2f41ca32306e0c4a11ada1edb9176a407dafb4a1c86e61` |

The emulator's decoder dependency is independently built from the published
FFmpeg 7.1.1 source, with `ac3` and `eac3` enabled. Tests check real PCM tone
content, stereo separation/downmix, exclusion timing, packet ownership,
backpressure, EOS/flush, and rejection of truncated/concatenated/protected AUs.
