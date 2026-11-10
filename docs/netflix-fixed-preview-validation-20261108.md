# Actual Netflix preview after native decoder correction

**Latest result: Netflix audio and playback are user-confirmed working.**
Implementation commits: `63bba120` (native CPU/DDP providers) and `a987c31f`
(`frexpf` and strict DDP dependency rejection). Audio confirmation and normal-output
evidence were checkpointed in `1bc45d27` and `6ebb88d8` respectively.

User clarifies Home and YouTube audio already worked; screeching was Netflix-only.
Parent uses that as evidence of distinct media paths, not a general audio failure.
No system-wide mute or format override is part of the decoder correction.

## mnetflix8: actual preview PCM is now valid in sampled records

Parent gracefully closed its verified home-audio5 process and launched Netflix
with original mnetflix1 arguments/store mounts, both trace flags and **per-process**
SDL dummy output for a numeric check before listening. Production executable
SHA256 `b75485633fbb8600a44b870111bcbd60dedb57a35968608f70e54e0e8cfaa65f`
contains the CPU/DDP loading correction, frexpf registration and strict DDP
dependency rejection, plus the unrelated disabled-by-default startup barrier.
PID25152/creation FILETIME134386373738574006; responding Netflix window verified.

Actual guest log confirms native CPU API binding at0x9400004c0/590/680,
DDP loading, its `frexpf` binding to production LibcInternal::FrexpF, and native
DDP/CPU startup. A bounded event-driven log collector retained fixed evidence
at `Temp/opencode/mnetflix8/fixed-preview-evidence`:

- Guest44427495bytes SHA256
  `8b36d2b45dad15fc8bb3e0043f0733801d3ac27fef6da081c3f0c1b0dbd78803`.
- Host3846bytes SHA256
  `ca2bbcf9a6995558078f65bae29f62a748028b8c8cc58239fe5f85b19f3df4a9`.
-25source and25AudioOut PCM summaries, **zero nonfinite/above-unit samples**
  in all reported scans. Maximum source peak0.148369119; maximum reported
  AudioOut input/output peak0.100982495. These independently sampled grains
  need not have equal peaks across stages.
- Observed sampler adds now carry **36864bytes/1536six-channel frames**,
  matching real native decoded output, instead of the prior untouched
  capacity40960bytes/1706frames.

This is actual application-path evidence addressing prior source peaks up to
2.1387162e38, not merely the synthetic decoder test. It covers bounded samples,
not every frame or speaker output; dummy timing does not establish device pacing.
At this stage, preview video/sustained title playback still needed user observation;
see the final playback confirmation below.

## mnetflix9: normal audio verification launched

After the clean application PCM check, parent told user it would restart with
normal audio and requested low initial volume. Verified mnetflix8 identity/hash,
gracefully closed it and confirmed exit. Launched **mnetflix9** with original
manifest/stores and both trace flags, **no SDL dummy override**, no system volume
change or sample suppression. PID22260/creation FILETIME134386374914250223;
same executable hash, responding Netflix frame608/60fps. Parent verified absence
of dummy/disk driver overrides in manifest and launch environment.

At launch, an event-driven bounded numeric collector was running for normal-device
PCM in `Temp/opencode/mnetflix9`. Audible confirmation and completed numeric capture
were pending; subsequent results are recorded below.

## User confirmation

During mnetflix9 with normal output, user explicitly reports **"audio works"**.
This verifies audible recovery after the concrete native decoder correction and
clean mnetflix8 actual-preview PCM evidence. Parent asked for about one minute
of continued playback and video/audio synchronization confirmation. Selected
title/authenticated playback and sustained duration remain unconfirmed; this
checkpoint establishes Netflix audio recovery, not the entire media-app scope.

## Normal-output evidence after user confirmation

Initial event-driven collector ended at its deadline with a15974336-byte prefix
and zero PCM records (SHA256
`d173269c4366ffa2bc0f93d3bddb5fe2ee99e79ce88686fa74d4607809feb8c4`).
That is a capture coverage limit, not evidence that the working sound was silent.
Parent found the active-audio records in the current log and retained a separate
fixed complete-line snapshot without replacing the earlier evidence. The helper
now performs one final file drain at deadline because write notifications can be
delayed while the writer keeps the file open; the exact notification timing of
this missed window was not separately established.

`Temp/opencode/mnetflix9/audible-confirmation-evidence`:

- Guest136187327bytes SHA256
  `b5c69c55a932e0fd0bbf0df75200f92d6edf21a4536d4abbe6f7cca3753e7d42`.
- Host3846bytes SHA256
  `ca2bbcf9a6995558078f65bae29f62a748028b8c8cc58239fe5f85b19f3df4a9`.
-61source PCM records and128AudioOut PCM records; zero nonfinite or above-unit
  samples in all recorded scans. Maximum source peak0.148369119; maximum sampled
  AudioOut input/output peak0.158196047.
- Zero logged queue clears or underruns in this prefix. Diagnostics are bounded;
  absence in these records does not prove absence for all future playback.

Same mnetflix9 process identity was verified alive/responding at frame10934/60fps
before snapshot. This supports the user's normal-output audio confirmation.

## Final user playback confirmation

After the audio confirmation and normal-output evidence checkpoint, user explicitly
reports **"omg playback works did u commmiitttt"**. Netflix playback is therefore
user-confirmed working alongside the previously confirmed audio; it is no longer
pending user verification. Exact title, authentication state, playback duration and
an explicit synchronization measurement were not supplied.

The working executable and mnetflix9 evidence remain the reference for this result.
Home handoff, genuine PSN authentication and Apple sustained audiovisual playback
remain separate unresolved goals.
