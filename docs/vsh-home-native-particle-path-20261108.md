# Native particle load and Update registration — IDA follow-up

Original native shell SHA256:
`cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09`.
Analysis-only ET_EXEC view:
`beded661699ad3dac61cc71b1cddb9f26a34de12478cf35998c42c51cf8a3db2`.
No original was changed. All native addresses below are this image's RVAs.

The initial full native IDA job completed. A later targeted fresh-load export
timed out before producing its report. Subsequent exports used separate copies
of our own completed private IDB; the existing database was preserved. Actual
IDA/Hex-Rays reports are `shell-native-update.txt`, `shell-renderer-update.txt`,
`shell-particle-refs.txt`, and `shell-particle-load.txt` under the private evidence
directory. No overlapping IDA jobs were intentionally launched.

## Resolved native binding

ELF RELA maps the library entry at+1169aa0 to `ShellUIBackgroundLayer` and its
method table pointer at+1169aa8 to+11693f0. Table entry+1169410 names
`UpdateNative`; pointer+1169418 resolves to+d9460. The corresponding IDA xref
agrees. `audit_home_native_registration_1360.py` pins this registration and the
key instructions against the original hash.

IDA at+d9460 requires argument2 (ESI) ==0xa0, otherwise asserts/traps for an
invalid ExperienceFlowState size. It loads global+13a8ee0, conditionally calls
+d94d0, then **returns zero even if that global is null**. This is original
firmware behavior, not an emulator stub to patch. The previous proposed managed
negative-return probe is therefore insufficient to distinguish renderer absence.
No zero result may be labeled renderer-ready.

Within+d94d0, singleton+8 is checked before renderer update+bff70 and tick+be8a0.
The update function+bff70 additionally checks renderer byte0 ==1 at+bff96 before
doing its full work. Optional singleton+10 receives the same flow-state pointer.
These are concrete preconditions independent of Home item delivery. Actual
singleton identity, ready byte, and return flow flags are still unobserved.

## Particle file -> texture object and fallback

The first string search used the interior basename, which explains the absent
xrefs; IDA finds xrefs to full paths beginning `/system_ex/vsh_asset/`:

* Particle0 full path+f3c463 referenced at+ea37f and+ea397.
* Particle1 full path+f820da referenced at+ea503 and+ea51f.
* Both are in initializer+ea340, called from+eb6e8.

The first path opens in `rb` mode, tests for a nonnull handle, obtains a file
size, allocates storage, and reads **one item of that size**. At+ea3ed it checks
that read result ==1. Only then does it call texture constructor/loader+c4c810
at+ea406 with the destination at owner+6f0. The loader return is tested for a
negative value at+ea419. Open/stat/allocation/read/load failures converge on the
fallback setup beginning+ea45c. That setup supplies dimensions packed as
0x100000001 and a 32-bit pixel value0xff000000, then calls+c4c340 at+ea4f6.
If fallback succeeds, execution proceeds to Particle1 rather than establishing
that Particle0's genuine texture loaded. The report includes the parallel
second path. This can explain how initialization continues after an asset-path
failure, but is not evidence that it happened during candidate2.

Source comparison: `src/libs/libC.cpp` maps the observed open import
`xeYO4u7uyJ0` to GuestFopen. The host GuestFread helper returns std::fread's item
count, consistent with a one-item check, but the exact import resolution of the
native read call must still be verified before attributing that call to this
helper. No read-return emulation defect has been established. The internals of
+c4c810 and downstream GPU upload/composition still need tracing; naming this
call a successful GPU upload would overstate the evidence.

## Observer correction and exact remaining boundary

Do not implement the earlier four-site proposal unchanged. Replace the
low-information negative Update-return observation with a singleton/renderer
ready-state observation after establishing how the managed layer's flow struct
maps to the native pointer. A valid bounded observer must freshly validate
caller+owner at each hit and must not correlate unrelated objects merely because
R12 or a prior pointer was preserved. The native registration above supplies a
real target, not a guessed P/Invoke address.

For asset diagnosis the precise next distinction is: genuine load call+ea406
return versus fallback+ea4f6 and its result, for the same initializer invocation
whose caller returns to+eb6ed. The native init owner is saved in the IDA frame;
the offset and full caller/stack convention must be byte-audited before encoding
it in a tracer. No live attach command is supplied because that decoder and its
teardown integration are not yet implemented or tested. Existing executor mode
does not support these probes. Parent may continue to preserve silent video,
module/mount identities and original logs without adding any audible retry.

The user-reported missing particle phase remains tentative. This checkpoint
identifies a concrete fallback branch and removes a misleading success signal;
it does not claim a causal renderer fix, successful Home tiles, or PSN login.
