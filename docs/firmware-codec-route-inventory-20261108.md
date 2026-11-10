# Firmware codec providers versus active host routes

Parent read-only inventory of regular13.60 and current host source. This
complements `protected-media-native-topology-20261108.md`; it does not establish
that every codec/profile is supported, licensed, selected or executable here.

## Actual files

The following files were individually read and their headers checked: all are
ELF64, little-endian x86-64 (`e_machine=62`), not opaque payload containers.

Under `system_b/common/lib`:
- `libSceAjm.sprx`, `libSceAjm.native.sprx`, `libSceAjmi.sprx`.
- `libSceAudiodec.sprx`, `libSceAudiodec.native.sprx`, `libSceAudiodecCpu.sprx`.
- CPU providers `libSceAudiodecCpuDdp.sprx`, `libSceAudiodecCpuDtsHdLbr.sprx`,
  `libSceAudiodecCpuHevag.sprx`, `libSceAudiodecCpuM4aac.sprx`.
- `libSceVideodec.sprx`, `libSceVideodec2.sprx`,
  `libSceVideoDecoderArbitration.sprx`.

Under `system_b/priv/lib`:
- CPU providers `libSceAudiodecCpuDts.sprx`, `libSceAudiodecCpuDtsHdMa.sprx`,
  `libSceAudiodecCpuLpcm.sprx`, `libSceAudiodecCpuOpus.sprx`,
  `libSceAudiodecCpuTrhd.sprx`.
- `libSceVcodec.sprx`.

Names indicate candidate families, not verified profile limits or particular
application usage. Native ABI/service/hardware requirements are not implied by
the readable ELF header. Selected reproducibility receipts:

| File | Bytes | SHA256 |
|---|---:|---|
| common/lib/libSceAudiodecCpuDdp.sprx |380388|8b2cab55ed77df39f9537addaf18dbebf77796ad3b2c17c5b4972e85729d6516|
| common/lib/libSceAudiodecCpuM4aac.sprx |405432|74287a9327d0852b4b6bead0647f9bca29a3bccbf21f6aa52bbf87cbfc78e5a3|
| priv/lib/libSceAudiodecCpuOpus.sprx |250196|ff6b4fc2796ffe74def7c9e5600c7d2768359bcc90930f9ce176bb7ecdf40d54|
| priv/lib/libSceAudiodecCpuTrhd.sprx |217092|e37982353a5f62b28a85ba4de8ddd3d679605039e3fa0cff129a8df421c9b5f1|
| priv/lib/libSceVcodec.sprx |67804|b5bee9f57a0b2b4cf4ae45e8e0a5ee831d84314ecaf45a8ed56194b2cf80fdf8|

## Host source distinctions

- `libAudio.cpp:670–672,779–785`: Audiodec facade accepts type1 ATRAC9,
  type2 MP3, type3 MPEG4-AAC. Type admission is not by itself decoding evidence.
- `ajm.cpp:74–80,183–188,216–228`: separate AJM namespace uses0 MP3,1 ATRAC9,
  2 MPEG4-AAC,24 Opus, and14 labelled unknown-new-ABI. Only the four named
  concrete decoder constructors appear in that switch;14 is in its support
  predicate but falls through without a decoder. Do not infer a working codec
  from the label/support predicate, or confuse Audiodec and AJM numeric IDs.
- `mediaFrameworkEsvmDecoder.cpp:674–706`: the host ESVM route chooses FFmpeg
  decoders aac/ac3/eac3/h264/hevc/vp9, checks availability/allocation/open errors
  and applies config constraints. Those source branches do not assert all
  codecs are enabled in every linked build. They are separate from the supplied
  native CPU-provider implementations.
- `libSysAudio.cpp:453–475`: the current Vcodec export `Ac6aYjr1pY8` is a
  log-and-zero stub with unresolved semantics. It cannot substantiate a claim
  that Vcodec functionality or firmware-wide codec support is implemented.

## Current evidence-led work

DRM agent continues server1234207a/provider/TEE mapping. Media agent now owns
Apple's actual source/config/AU/readiness sequence in the completed mapple2
capture and the smallest missing-state diagnostic. Six successful startup
Flushes and24 queued-frame log records are not24 proven displayed frames.

Separately, native-backed private Videodec2 buffer ownership change `c5cd8256`
was integrated as `b7e20e7d`; full parent consumer/production rebuild and resource
tests with tracing both off and on passed.
This corrects a demonstrated contract-model gap; the Netflix24% result is not
yet proven resolved. No all-codec activation or protected-playback claim follows
from provider inventory, and no firmware files were modified.
