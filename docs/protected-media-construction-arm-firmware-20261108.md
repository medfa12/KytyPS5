# Native object construction and MP3 ARM firmware: third checkpoint

Continues d3ab88a6 (parent ae71b297). Static inspection only; docs-only change.
Firmware root: `${USERPROFILE}/Downloads/13.60`. Native AMD64 addresses below
are module RVAs. Numbers are hexadecimal unless stated otherwise.
MP3 addresses are explicitly payload offsets or ARM virtual addresses (VAs).

## Result

The server controller and EsvmEngine manager/provider construction joins are now
resolved. The MP3 payloads contain readable **32-bit ARM and Thumb-2 code**, despite
their ELF64 AMD64 wrappers. They are not wholly opaque encrypted images. This
supersedes the previous checkpoint's unresolved ISA classification, but does not
demonstrate a working secure endpoint or native Apple protected playback.

## Server: registry -> controller -> actual video command

File: `system_b/vsh/app/NPXS40039/eboot.bin`.

* d0050 obtains the controller registry via f6a00 at d00ba; 3e7b0 matches the
  requested core, and 3e3b0 returns its controller pointer. At 3e462 the lookup
  reads entry+8 from ten 30-byte entries beginning at registry+18.
* Controller launch at 3db90 takes the video-core branch for 101/102. At 3ddad
  it allocates through 15800, and at **3ddcf** invokes constructor **8ecb0**.
  The constructor's 8ecda LEA and 8ecee store install **vtable 1efee8**.
* Successful initialization reaches 3df69..3df8b: input+8 receives that same
  object, and 3e050 copies the registry entry. Existing-controller lookup returns
  the stored pointer, not another guessed interface object.
* Relocation at **1f0088 = 1efee8+1a0** resolves to **9a3d0**. This binds the
  virtual call at e73bf from the preceding note to the video controller method.
* 9a3ee calls command-pool accessor **70f00**; e5a0, 8cb60, 8cb70 and 86840
  populate request/configuration state; it queues through **985a0**.
* The command pool is independently typed: 69372 starts an index at -a0;
  69391 constructs each command with **8ca80**, and 6939a stores it at
  pool+index+1c20. Thus the twenty entries occupy **1b80..1c18**, exactly the
  slots consumed by 70f00. 710ff resets a selected object via e4a0, then clears
  the pool slot before returning ownership to the caller.
* Constructor 8ca98/8ca9f installs **vtable 1ef770**, whose **+18 = 8cbb0**.
  That is the video command execution method already mapped to 14a1b0 and the
  VideoCore request. This closes pool-object identity; queue scheduling itself
  has not been exercised, and this is not an observed call trace.

The precise unresolved point is no longer the controller's +1a0 binding.

## Engine: initializer -> manager -> Navigator -> PlayReady factory

File: `system_b/common/lib/libSceEsvmEngine.sprx`.

### Manager and asynchronous request

Access export 250a0 obtains its manager with f2d0. Initializer **eec0** writes
global **389580** at efbc/f06d, returns early if already initialized, and teardown
clears it at f311. A null initializer argument slot0 selects constructor **9470**;
a nonnull slot0 supplies a call queue and selects **94b0**. Both constructors
install **364918**, whose first method is **94f0**.

94f0 validates/copies the request and queues callback **9840** through 9730.
9840 recovers the arguments and tail-calls **3240** at 98ae. At 32b8, 3240 obtains
the Navigator through 35800, then invokes its first method at 32d0. Constructor
**35840** installs **367190 -> 354f0**, storing global **389740**.

The initialized manager is therefore distinct from the fallback object returned
by f2d0 when no manager has been constructed. Existence of the export is not
evidence that initialization occurred in Apple's retained HLE run.

### Actual factory input and provider object

The direct eec0 caller is **2681e**, inside exported initializer
**RoPRqUDZyZ8 at 26570**. Its eec0 argument block begins at `[rbp-f0]`:

* slot+8 becomes `[rbp-148]` when caller input slot0 is nonnull (267a1..267ab);
  26641/26686/2668a install this factory's vtable **367800**;
* slot+10 becomes `[rbp-c0]` when caller input slot+8 is nonnull
  (267bd..267dc); its separate vtable is **367f90**;
* slot+18 is explicitly zeroed at 266f9. eec0 supports this slot generally but
  this caller does not populate it.

eec0 assembles the nonnull factories in slot order +18, +8, +10 and calls
35840 at f290. At **35920**, 35840 invokes each factory's virtual **+10**,
checks the constructed object's status at +18, and retains provider shared
pointers. For **367800+10**, the relocated target is **3d850**:

`3d850 -> allocate e0 bytes -> 3f2f0 helper -> 402f0 provider constructor`

At **4030f/40322**, constructor 402f0 installs provider **vtable 367978**.
It copies factory flags +48/+49 to provider +88/+89 and constructs its lazy
PlayReady initializer through 435b0, storing that pointer at provider+98.
Constructor diagnostics name `CdmPlayReady`; actual imported PlayReady4 calls
remain as pinned in the second checkpoint. The other factory's +10 is 4a0e0;
this note does not assign it a 4L identity or compatibility contract.

Navigator 354f0 walks provider pointers, invoking **+18 at 355d9** to match the
key-system and **+20 at 35609** for configuration access. Exact PlayReady slots:

| Provider vtable 367978 slot | Target | Meaning established here |
|---|---|---|
|+18|404e0|Key-system matcher; recommendation plus conditional legacy spelling|
|+20|36aa0|Generic access/configuration validation wrapper|
|+b8|36e30|Validation method called by wrapper at 36b76|
|+d0|40560|PlayReady-specific access, called by wrapper at 36bcd|

**Correction to the preliminary direct-call hypothesis:** provider +20 does not
directly point to 40560. Generic validation precedes the +d0 virtual call.
40560 then reaches the previously documented initializer/TEE admission barrier.
No new PlayReady4L selection is evidenced by this construction.

## MP3 contents: wrapper metadata versus executable evidence

Files under `decrypted_modules/`:

| File | Decimal bytes | SHA-256 |
|---|---:|---|
|mp3_dram.elf|1049600|`b0b5e5d9e09656c25a9e18eef92f136490ab50949999e90df61a054f761dcb6d`|
|mp3_sram.elf|11520|`b7f1dfecc225cd9a4a8a249a46ed44242941f216e8e9410a2633b31d22369fbb`|

Both declare ELF64/type2/machine62/entry0, one RWX PT_LOAD at file1000/VA0,
no section table, payload sizes ff400 and 1d00 respectively. **Add 1000 to all
payload offsets below to obtain wrapper file offsets.** These declarations do
not describe the actual instruction set or DRAM runtime address map.

### SRAM: ARM vectors, privileged startup and Thumb-2 page-table setup

* Payload0 is ARM `ldr pc,[pc,#118]`: with ARM PC bias8 it fetches literal120,
  whose value is **13c**. The vector table at100..11c uses matching ARM loads.
  Literal destinations120..138 are **13c,290,28c,294,2a0,2bc,2c0**. Error vectors
  lead to coherent handlers or deliberate self-branches, not x86 entry code.
* At13c/144, `mrc/mcr p15` reads/writes SCTLR; 14c writes VBAR from literal390
  (=100). Calls150/154 enter2c4/2dc to disable MMU/cache bits and invalidate
  TLB/cache state. Cache maintenance uses CP15 set/way operations and DSB/ISB
  at330/334. The code includes WFI at220.
* SP is loaded at168 from literal398 (=f000). The BLX at170 uses literal39c
  (=**1675**), explicitly selecting Thumb at **1674**.
* Thumb1674 has a coherent register-save/startup sequence. At16bc it calls
  **13cc**, which shifts VA right by20, clears the low20 PA bits, combines
  mapping attributes, and writes a 32-bit descriptor indexed by VA into a
  translation table. The loop maps1000000..4000000 in100000 steps.
  Later loops use13e0 and1000 increments;177a restores registers and returns.

This is AArch32 ARM/Thumb-2 code with ARMv7-style privileged/MMU facilities.
It requires more than a user-mode ARM instruction decoder. These instructions
do not uniquely identify a particular Cortex core, SoC revision, or TrustZone
configuration; no exact CPU model is assigned from the wrapper or filename.

### DRAM: explicit ARM-to-Thumb entry and command handling

The payload begins with zero padding and a `$PS1` marker at10. At100..120 the
u32 values are `100,e00000,e0b000,e1b7e0,e019c4,5244,c000,0,e019d4`.
Field names are not fully established. Base **e00000** is independently supported
by code pointers rather than assumed from the ELF VA0.

At payload19c4 / VA **e019c4**:

```text
mov r0,r2
ldr ip,[pc,#10]  ; literal at payload19e0 = e04aed
blx ip          ; enters Thumb at VA e04aec / payload4aec
svc #75
```

The Thumb target saves registers, reads command `[r0]`, compares against1000,
uses bounded60-byte copies and calls e016d4/e0161c/e098e4. This establishes an
executable command-processing path and a supervisor-service dependency. It does
not yet identify command1000 as a host ioctl, TA load, or a PlayReady operation.
Public `gpd.tee.*` property strings also appear at wrapper1839..1a42. They support
TEE-related contents but do not prove the DRAM image alone is the complete secure
OS or implements every PlayReady prerequisite.

### Negative controls and provenance limits

* Zero bytes decode as ARM conditional instructions and Thumb register moves;
  even the DRAM container header can decode. Decoding percentage therefore is
  not executable/ISA proof. Here, vector PC-relative addressing, privileged
  semantics, literal targets with the Thumb bit, and coherent callee control
  flow agree independently. Cross-mode decoding of the same entry/header bytes
  lacks that combined evidence. Literal pools must not be disassembled as code.
* Payload entropy/zero proportions (DRAM about0.84 bits/byte and93.3% zero;
  SRAM about6.65 and14% zero) are descriptive only. They establish neither
  encryption nor complete availability of all secure components.
* Neighbor `mp4_dram.elf` also declares machine62/entry0, yet contains the public
  `ELF image (a53.elf) by PSP` marker at file15aa19. This is a wrapper-label
  counterexample, not an MP3 CPU identification. `mp4_sram.elf` instead declares
  machine183/entry0; `mp1_sram.elf` declares62/entry0. `phy_fw_91002.elf` declares
  62 but entry64800000. Uniform wrapper naming cannot establish uniform ISA.
* The bounded neighbor pass also read `abl0.elf`: it is only1332 decimal bytes,
  declares ELF32/ARM/entry54100, but its phoff bd60 is outside the file. It cannot
  be treated as a valid complete ELF load image. An earlier entropy script
  stopped on its empty assumed file1000 payload; the subsequent header-only
  pass completed for the seven explicitly named files. No all-firmware audit
  or successful abl0 segment parsing is claimed.
* Repository searches for MP3 filenames, `decrypted_modules`, and wrapper/unpack
  terms found inspectors/notes but no identified MP3 wrapper-producing script.
  The supplied `ENCRYPTION_STATUS.md` claims executables are decrypted and six
  DRM blobs encrypted/signed; that note supplies neither tool/version nor a
  source-to-output transformation receipt. It is not independent proof.
* `segments/` contains update/boot files and metadata, but no MP3-named raw
  counterpart in its directory listing. No verified raw-container comparison
  or original load manifest was recovered. Generated-wrapper provenance remains
  unknown; readable ARM code is independently established without that history.

## Genuine emulation prerequisites versus user-space glue

There is now a concrete firmware research route rather than a blanket claim
that the endpoint firmware is absent:

1. Recover the actual boot/load contract: SRAM vector mapping, stack backing,
   translation-table storage, DRAM base/extent and startup arguments. Wrapper
   entry0/VA0 must not be passed blindly to the AMD64 loader.
2. Supply AArch32 ARM/Thumb-2 interworking and the privileged CP15/MMU, exception,
   barrier/cache and WFI behavior the SRAM demonstrably uses. Determine actual
   memory/device regions and reset state before selecting a CPU/board model.
3. Trace the SVC handler and DRAM `svc #75` return/service contract, then connect
   the command dispatcher to the real host/firmware mailbox and shared-memory
   ownership protocol. These are unresolved device/secure-runtime contracts,
   not the already recovered guest-side ioctl layouts.
4. Establish real TA container admission and command execution, including any
   platform-authentication dependencies. Readable MP3 code does not prove the
   separately supplied PlayReady TA is directly executable plaintext or that
   its full admission environment is available. No secrets were inspected.
5. Only then connect guest TEE transport to that endpoint and validate native
   initialize/register/open/close/finalize and failure unwinding. The server and
   EsvmEngine construction above supply user-space ownership/call-path evidence,
   but successful registration of their callbacks is not secure-session success.

**Next bounded static test:** follow SRAM vector108 -> literal128 ->28c (a
self-loop here), distinguish it from whichever runtime later installs the DRAM
SVC service vector, and trace the actual supervisor handler/load handoff before
assigning service75 semantics. In parallel with that logical analysis (no agent
delegation required), seek a source container/load manifest to verify the DRAM
100..120 fields. A self-loop in the initial SVC vector is evidence of a missing
runtime transition to explain, not evidence that the payload cannot run.

No source shim is justified by these findings. No firmware execution, process
access, builds, keys/credential extraction or license bypass was performed.

## Verification receipt

23 offline structural assertions passed using the existing ELF relocation
inspector and Capstone: eight relocated object slots, two constructor LEAs,
command-pool range equality, two firmware hashes, two wrapper checks, vectors,
two interworking literals, DRAM load metadata, CP15 startup, two Thumb entry
instructions, and the zero-decoding negative control. Scratch checker:
`${USERPROFILE}/AppData/Local/Temp/opencode/drm-third-check.py` (outside the tree).
Bounded disassembly supplies the surrounding data-flow evidence. These checks
are not runtime tests or a complete CPU conformance test.
