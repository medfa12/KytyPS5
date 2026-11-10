# Particle0 zero-hit follow-up: Gen2Vr log is startup policy

Follow-up: the proposed route mode is now implemented and tested; see
`vsh-home-startup-route-observer-20261108.md` for its current contract/commands.

## Result

`Enable Gen2Vr Rendering: True` is **not evidence of an incorrectly detected
headset**. The pinned BGLayer `BackgroundLayer.Start` supplies literal true to
that setter. No headset-capability or renderer patch is justified by this line.

One actual IDA Pro 9.3/Hex-Rays batch analysis used a new copy of the existing
private BGLayer database. Original databases and firmware were preserved.
`_TempData/ida-home-20261108/bg-gen2-selector.txt` records imagebase0, Hex-Rays
enabled and input SHA256
`741da5eda3233a62cf74e7f3ba978e80533bfa7b458af0d2b7b6b532a17365a0`
(the existing analysis-only ET_DYN view). Its spec and IDA log are beside it.

IDA identifies getter+3c70 and setter+3c80; the setter's direct call xref is
+1e42. It compares owner byte+c8, logs the change, writes that byte and toggles
bits0/1 in a shared managed flags value. It does not call the particle initializer.

Independent embedded MethodDef/call-table decoding names the caller at+1c60
`Sce.Vsh.ShellUI.BGLayer.BackgroundLayer::Start`. Pinned caller instructions:

```
+1e23  mov rdi,r14             ; this
+1e26  mov esi,1               ; literal true, not an API return
+1e2b  ... store transition and managed write barrier ...
+1e37  mov dword [r14+f0],4f
+1e42  call +3c80
```

The setter's GOT entry+cd088 decodes through the AOT patch table to embedded
`#US` string `Enable Gen2Vr Rendering: {0}`. This explicitly links the observed
line to the setter instead of inferring semantics from the English log text.
The caller window was independently byte-decoded, not Hex-Rays-decompiled in
this batch; IDA's setter xref and metadata mapping agree on its edge.

Reproducible **offline** audit passed:

```
python -B tools/audit_home_gen2_selector_1360.py _TempData/ida-home-20261108/bg-transition-view.elf
```

It pins the complete view hash, three method identities, literal argument,
call target, field access/write, flags operation and exact GOT log string.
It does not execute the analysis ELF or access any live process.

## Live boundary supplied by parent

Particle0 trace SHA256
`b1d2925ce5125c5772f489aec9f39d9f5f1ec1455dd33fe35272b2fdb850b209`,
142092 bytes: armed29 threads before gate release; zero initializer/load/fallback
hits,2016 events,335 thread arms. Cleanup verified298 threads, three pre-detach
repairs, no post-detach repairs/errors. No login was observed. This supports
early-arm coverage, but does not establish dispatch suppression or a GPU result.
These supplied receipts were not independently replayed or opened here.

Do not run particle1 merely to repeat the same unhit entry. Native3's56 ready
renderer updates were from a different run; they do not positively validate
hardware-site delivery during particle0's capture.

## Next narrow runtime distinction

Before another texture/fallback capture, use one startup route capture with a
**same-run positive control** and upstream dispatch:

* native shell+d9330: registered `InitializeNative` entry;
* native shell+d9460: already live-validated UpdateNative entry, as a positive
  native-code/probe-delivery control in this same run;
* native shell+eb6e0: resource-job callback upstream of particle initialization;
* native shell+ea340: particle initializer entry.

Static callback bytes at+eb6e0 are `push rbp; mov rbp,rsp; mov rdi,[rdi+38];
call ea340; xor eax,eax; pop rbp; ret`. Existing IDA identifies the direct call
at+eb6e8. Fresh callback object/owner, stack and caller validation is required
before pairing it with initializer entry; a positive Update hit alone must not
be used to manufacture that pairing. This route mode is **not implemented by
this checkpoint**. It needs its own byte audit/negative decoder controls and a
particle-gate release contract before a parent launch.

If Update and native initialization hit but neither callback nor initializer
does, investigate construction/enqueue/dispatch of this resource job. If the
callback hits without its unconditional initializer call, first investigate
observer delivery and lifecycle evidence. If the positive control also misses,
do not infer a renderer branch skip. Current evidence proves none of those
outcomes and does not identify a swallowed HLE behavior. A deeper IDA caller/
enqueue graph remains pending; this slice used its one analysis job.

The separate C++ lease implementation is committed as49b578ec; its disposable
subprocess tests passed. No live emulator, store, audio or media-decoder access
was used for either slice.
