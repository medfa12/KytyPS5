# Purpose-generated AAC PCM fixture

All source samples are created by `generate.py`: 5120 stereo signed-16 frames
at 48 kHz; left 440 Hz, right 880 Hz, each amplitude 8192/32768. No captured
application media or third-party audio is included. These fixture samples and
the generator are dedicated to the public domain (CC0).

Offline encoder used: `ffmpeg version 8.1.1-full_build-www.gyan.dev` (native AAC
encoder), AAC-LC, 192 kbit/s, one thread, `+bitexact`, ADTS. The exact encoder
command and waveform formula are in `generate.py`. Regeneration can differ
with another encoder version; tests use the checked-in `packets.h` bytes.

- `source.wav`: generated waveform, not a decoder output/golden recording.
- `tone.aac`: six complete ADTS frames, sizes 393/445/395/401/407/433 bytes.
- `packets.h`: those same packets, independent of runtime files/demux.
- `tone.aac` SHA-256:
  `8fd000a9cce8016b8ab4a2ed2adc108d8233bfcba1010c1b8eeceb1f4533ebd2`.

AAC's encoder priming/trailing samples are deliberately retained in this packet
fixture. Without explicit AU exclusion there are six 1024-sample PCM blocks;
the packet decoder must not invent container skip/discard metadata. Tests
check independent tone amplitudes/channel separation on steady blocks 2–4,
not just nonzero output. A second decoder receives the identical history with
wire exclusions; its output must be the exact selected slice of the reference
decoder's PCM, with verified native sample rounding/PTS adjustment.

Only the offline fixture encoder was executed during implementation. The packet
decoder and tests were **not built or run**. No emulator/live/process-control
actions were performed.
