# Native Home transition: offline analysis

Follow-up: the native IDA job subsequently completed. See
`vsh-home-native-particle-path-20261108.md` for the resolved Update registration,
particle fallback branch, and correction to the proposed negative-return probe.

## Actual IDA work

Fresh private IDA Professional 9.3/Hex-Rays databases were created sequentially
for Legacy transition selection and BGLayer animation/update code. Original
BGLayer SHA256 is
`fdf48c4094cfa1bca06b5ecd8d47ffdd212722c50f8da3effe728458f44cdb41`.
The analysis-only ELF header view SHA256 is
`741da5eda3233a62cf74e7f3ba978e80533bfa7b458af0d2b7b6b532a17365a0`.
Reports/specs/IDBs remain in `_TempData/ida-home-20261108/`:
`legacy-transition`, `bg-transition`, `bg-states`, `bg-update`.
Mono metadata and AOT string references were independently decoded from the
original, then compared with actual IDA instructions. Hex-Rays incorrectly
expresses some vector floating comparisons in terms of stale integer variables;
use the raw `vucomiss`/conditional branches, not those decompiled predicates.

### Concrete distinct gates

* `BackgroundLayer.SetGlobalBackgroundAnimation +4240` ultimately writes the
  requested state at object+`1dc` (`+43ba`). Its preceding log-level branch is
  **not** an animation readiness check; both paths reach that write.
* `UpdateGlobalBGState +4490` dispatches current state+`1d8`. The exact enum is
  ColdBoot/Boot=2, WarmBoot=3, InitialBoot=4, InitialSetup=5,
  InitialWelcomeScreenAnimation=6, InitialWelcomeScreenFadeOutAnimation=7,
  Login=8, ParticleBottom=9, ParticleSpread=10, NoParticle=11.
* `SetGlobalBGState +5230` cold/warm cases request Login=8 afterwards. The
  InitialWelcomeScreenAnimation case instead requests NoParticle=11 and loads
  `sfx_transition.at9`. These are not interchangeable login phases.
* Cold/warm `UpdateBootAnim +4990` accumulates elapsed time at+`1e0` and counts
  updates at+`1f8`. At `+4b1e`, a comparison with float **15000** branches to a
  renderer-check timeout if the renderer phase at+`1fc` is not2. `+4b57` marks
  phase3, clears bit1 of+`cc`, and selects the requested global state. The
  normal path tests captured+`cc` bit0 at `+4d4e`; `+4d8a` additionally requires
  sound-wait phase+`200` equal2 before starting phase2 at `+4dbb`.
* Sound waiting has its own bounded exit at `+4cd7` (update count30). It is not
  the same predicate as renderer readiness. A later visible state is compatible
  with a timeout escape, but does not demonstrate that this escape occurred.
* BGLayerNative.Update `+6bb0` checks the native result at `+6bd9`; a negative
  return constructs and throws an error at `+6bee`. Do not replace it with
  success or infer its result from the background's later appearance.
* Legacy `PLayerTransition.UpdateBackgroundTransition +1fb380` compares selected
  background properties before applying changes; `UpdateTransition +1fab20`
  and visibility update+`39460` are separate lifecycle layers. Their live
  arguments and ordering have not been observed.

### JS classification is also conditional

Separately decoded Home HBC89 (hash
`565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f`)
function6183 classifies query state:

| Input | Classification |
|---|---|
| intro=login, bootCondition=login | initialLoad |
| intro=login, bootCondition=autoLogin | autoLogin |
| same with truthy postAutoLogin | appRestart |
| intro=hidden | autoLogin |
| no intro, bootCondition=switchUser | switchUser |

These results are verified by executing the actual pinned function's bounded
bytecode subset with an explicit atom test double. They are not live JS results
or proof of which particle sequence should run. The eight-case probe includes
paired boot-condition and postAutoLogin controls. The shared interpreter gained
only `JStrictNotEqual`; all25 existing first-render subset cases still pass.

## Source assets, not guest-load claims

In the supplied original `system_ex_b/vsh_asset`:

* `sfx_coldboot.at9`: 332100 bytes, SHA256
  `38bbef69511730a2466ab2a0187a654565e910c68d355a6e825796b7556d40d1`.
* `sfx_warmboot.at9`: 202500 bytes, SHA256
  `5ab3f76191643499d65d10fb21f5df2b6221581dae0d4622a0c1f611adf40c08`.
* `sfx_transition.at9`: absent in this directory and not found by filename in
  the supplied firmware tree. IDA ties this literal to state6, so this absence
  is **not** an established explanation for a regular login transition.
* `Sce.Vsh.ShellUI.BGLayer.Particle0.gnf`: 167936 bytes, SHA256
  `e989bcd6325a7b889f869863e4c32019c89e7aaa4cace49795f17b6d4db23991`.
* `Sce.Vsh.ShellUI.BGLayer.Particle1.gnf`: 167936 bytes, SHA256
  `9cb10598c0b00a99743ab504eb5a9026fdb18f326b182de31052c2acec1a782b`.

Presence does not prove staged mount resolution, texture upload, shader execution
or composition. Native `NPXS40087/eboot.bin` contains the particle asset strings
and `ShellUIBackgroundLayer`/`UpdateNative` registration strings; managed metadata
names that native library. Native binding and renderer callsites still require
their own IDA inspection, rather than guessing a target from those strings.
One native IDA analysis is outstanding at this checkpoint. Original shell SHA256
is `cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`;
the private view is
`beded661699ad3dac61cc71b1cddb9f26a34de12478cf35998c42c51cf8a3db2`.
The helper initially rejected this executable's Sony type0xfe10; it now maps
that type to ET_EXEC=2, while retaining ET_DYN=3 for type0xfe18. Full byte
comparison confirmed that only the ELF type field differs in both cases.

## Parent's minimal runtime evidence, if a new run is needed

No active Home instance is assumed. First preserve existing artifacts. On a
parent-controlled new launch, capture video silently from before login through
the reported missing phase and Home navigation. Use the user's normal controller
actions; do not replay transition audio for diagnosis. Record launch kind
(cold/warm/auto/manual) and mount identity. A Settings screenshot alone is not
Home acceptance.

Before implementing a new bounded tracer, pin the exact BGLayer image above and
verify its live module base. Candidate observation sites (not a validated tracer):

1. `+43ba`: R14=layer, EBX=requested global state; bounded snapshots of current
   state+1d8 and request+1dc distinguish which transition was requested.
2. `+4b57`: R12=layer; a hit specifically identifies renderer timeout. Capture
   +1e0 elapsed, +1f8 count, +1fc renderer phase, +200 sound phase, +cc flags.
3. `+4dbb`: R12=layer; normal-start branch, same bounded fields.
4. `+6bd9`: native Update return EAX, before the sign test. This needs proper
   owner correlation before attributing a failure to a particular layer.

These four sites fit hardware slot limits, but require an audited decoder,
caller/owner validation, process identity, and the established detach cleanup
controls before use. They are a specification for a parent-approved observer,
not a command to attach or a claim that the existing executor tracer supports
these offsets. No memory writes, forced state, particle synthesis, opacity
override, or success/error suppression is justified by the evidence so far.

Offline repeat commands:

```
python -B tools/audit_home_transition_1360.py <original-BGLayer.dll.sprx> --assets <vsh_asset-directory>
python -B tools/probe_home_intro_1360.py <original-Home-application.ps.bundle> --decoder <hermes-dec/src>
```

The audit pins enum values, exact gate instructions, timeout constant and AOT
asset literals. Passing the analysis-only modified ELF as original is rejected.
Production bootstrap ownership requirements are recorded separately in
`vsh-home-bootstrap-ownership-design-20261108.md`; no live bootstrap or causal
animation fix is claimed in this checkpoint.
