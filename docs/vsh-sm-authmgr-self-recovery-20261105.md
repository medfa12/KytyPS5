# Shell SELF metadata recovery — bounded handoff, 2026-11-05

## Result

**Both alternate-tree ShellCore/ShellUI metadata regions were decrypted and
RSA-PSS verified against that tree's own retained auth-manager public key.**
Their authenticated first capability word is `0x40001c0000000000`.

**The active pair remains unverified.** Its original SELF containers are present
and structurally bound to the active decrypted ELFs, but the retained dictionary
slot41 candidate fails signature verification for both. Neither active
capabilities nor active `0x88` auth records have been recovered.

The source blocker is now precise: a verified **active-build CCP slot41 AES-128
key**, or the corresponding successful secure-service `0x30` metadata-decryption
outputs, followed by signature verification using the active public key. The
traced secure-kernel path passes a key-slot address to the crypto engine; it
does not yield that key's bytes from the retained executable.

This is a completed local source/crypto evidence handoff. It establishes no
remote PSN session, account authentication, token, or Store sign-in success.
The alternate results are not credentials for the active executables.

## 1. Inputs and target binding

Tree names below refer to separate directories, not interchangeable versions:

* **Active:** `${USERPROFILE}/Downloads/13.60/`.
* **Alternate:** `${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE/`.

The parent-owned `tools/inspect_shell_auth_images_1360.py` reads named members
from raw exFAT images with validated directory-set checksums and physical extent
bounds. Recovery uses `Image.target()` in memory, without mounting or extraction.

| Tree / target | Image / member | Contiguous cluster | SELF length |
|---|---|---:|---:|
| Active Core | `segments/dev_ssd0.system_b`, `vsh/SceShellCore.elf` | 6542 | 16157964 |
| Active UI | `segments/dev_ssd0.system_ex_b`, `app/NPXS40087/eboot.bin` | 1465 | 9423671 |
| Alternate Core | same relative image/member in alternate tree | 6011 | 16079121 |
| Alternate UI | same relative image/member in alternate tree | 1456 | 9425466 |

SHA256 identities:

| Input | SHA256 |
|---|---|
| Active Core SELF | `e44e37dada3c1d756c0a4c6937c96ef6e99c04f8db6dbc687063e25765f616a3` |
| Active UI SELF | `d4d6d736dce381442e79e5143b72b6172172dd9c257ae3fce4f2be2cf96efd9c` |
| Active Core ELF | `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a` |
| Active UI ELF | `cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09` |
| Alternate Core SELF | `0e365cf5850f156cc099777faf12aa12ba4fb2e71d7d49d82f2aaf5b7708beab` |
| Alternate UI SELF | `73fd5ed27d0341620f7431598f2828f7206665b39675739dec942740bd703130` |
| Alternate Core ELF | `f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434` |
| Alternate UI ELF | `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060` |
| Active `decrypted_modules/sm_authmgr.elf` | `cfeabbcfec15255433eec7d6d90ad9e36da4c9288c55eb5c68a4ecb3a6c27711` |
| Alternate `decrypted_modules/sm_authmgr.elf` | `990711f28f502255695fe61b5e60364ad29ea525faf30ca5aa5709520af3039f` |
| Alternate `key_dictionary.json` | `275cf33fefe9cb47b7411ea65a8672cc3437a3117640c83023f8470e6e4aac30` |

For each of the four containers, the embedded 64-byte ELF header, all fourteen
program headers, and 64-byte extended-info record exactly match its **same-tree**
decrypted ELF. These are asserted comparisons, with individual PHDR evidence in
the recovery logs. This is structural/source binding, not a new verification of
every decrypted executable segment's contents.

Common fields: SELF identifier `0x10000301`, header size `hs=0x530`, twelve SELF
entries, signature selector3, layout5, embedded ELF at `0x1a0`, extended info at
`0x4f0` (matching ELF file offset `0x350`). Core authority is
`0x4800000000000010`; UI authority is `0x480000000000000f`.

| Target | Metadata extent | Payload after decryption | Encrypted signature trailer |
|---|---|---|---|
| Core, either tree | `[0x530,0xb60)`, `ms=0x630` | `[0x9b0,0x9e0)`, length0x30 | `[0x9e0,0xb60)` |
| UI, either tree | `[0x530,0xb80)`, `ms=0x650` | `[0x9b0,0xa00)`, length0x50 | `[0xa00,0xb80)` |

Thus the original metadata and signature bytes are available. The initial
extracted-file/archive-only source conclusion is superseded, as already corrected
in the parent-owned policy audit.

## 2. Reconstructed transform and authentication

ARM addresses are ELF VAs mapped through file-backed `PT_LOAD` segments. No ELF
section table is used. Both retained auth-manager executable segments
(`file[0x1000:0x62f8]`) are byte-identical; their selected IV/public-key data differ.

1. Auth-manager `0x1734`, selection at `0x1a46..0x1a90`, resolves identifier
   `0x10000301` to CCP slot `0x29` (41), IV VA `0x62d0`, public descriptor
   VA `0x8024`.
2. Header byte7 is `0x32`: the conditional extra transform at `0x1442` is skipped.
   The mandatory call at `0x146a` invokes `0x403c` in place over `[hs,hs+ms)`,
   operation1, direction0, slot41, selected IV, options `0x04000000`.
3. `0x403c` reverses IV bytes through `0x4200` for the hardware-facing request,
   constructs key-address41/type6, and reaches service `0x30` through `0x5010`.
   Canonical software **AES-128-CBC with the original selected IV bytes** is
   independently validated by the alternate-tree positive signatures below.
   No padding is stripped: the stored extent is already block aligned.
4. The RSA descriptor is `{modulus=0x7078, length=0x180,
   exponent=0x71f8, length=8}`. The big-endian exponent is65537.
5. The verified message is original SELF bytes `[0,hs)` concatenated with the
   decrypted metadata excluding its final `0x180` bytes. That decrypted trailer
   is the signature. Verification uses **RSA-3072-PSS, SHA256, MGF1-SHA256,
   salt length32**, matching the traced verifier (`0x4b60`, `0x4c50`).

Selected RSA modulus SHA256:

* Active: `351a766da326c36397c425cf79cbd9eca3e31d94819b8fda5af9739adf0455b5`.
* Alternate: `bd5ea3f6f74c6424bd756154dd90f51b585b0ca1b045676a8f5f6b17a1c85365`.

The retained dictionary's CCP41 value is16 bytes and equals its
`cf_keys.x86_system.key`; that descriptor's identifier is `0x10000301`.
Its IV equals the alternate SM's selected IV, not the active SM's. Each run uses
its own tree's SM IV and RSA modulus, never the other tree's public key.

## 3. Verification outcomes and authenticated alternate payloads

| Check | Active Core | Active UI | Alternate Core | Alternate UI |
|---|---|---|---|---|
| Same-tree header/PHT/extended-info equality | PASS | PASS | PASS | PASS |
| Signature on untransformed stored metadata | FAIL | FAIL | FAIL | FAIL |
| Dictionary CCP41 AES-CBC then selected-key PSS | FAIL | FAIL | PASS | PASS |
| Header / payload / signature single-bit negative controls | — | — | all rejected | all rejected |

The failed active candidate plaintexts are not emitted as auth data. Their
metadata SHA256 values, for reproducibility only, are:

* Core: `37ef4d00a6bf7e3ff13a05475bd1055e66587ef8f44a1df49ad57cddfa98e334`.
* UI: `0955af3aeb7e72435977dd8b0cc77d46b46bae9b84259e6c457302f73e9e1206`.

A failed signature establishes that this candidate combination is unacceptable;
it does not alone prove whether the active key, source data, or retained
verification data is the mismatched component.

**Alternate Core authenticated payload**, length0x30:

```text
40001c0000000000900000000000ff000800000000000000f0000000ffff400040004000800000008000000000000000
```

**Alternate UI authenticated payload**, length0x50:

```text
40001c0000000000850000000000ff000400000000000001f0000000ffff4000400040008000000080000000000000000100000000000000000000000000000023cc68d3bf31b29ba8447eccb3078d58
```

Under the previously traced decoder, the first big-endian u64 yields
`caps[0]=0x40001c0000000000`, including bit62, for **these alternate inputs only**.
The authority is separately read from their signed extended-info bytes. This is
cryptographic metadata evidence, not an authority-to-capability inference and
not an executed mailbox-0x16 acceptance test. No full native `0x88` success record
or active provider installation is claimed.

## 4. Active secure-kernel key-slot boundary

Input `13.60/decrypted_modules/secure_kernel_4.elf`, 194944 bytes, SHA256
`9d6eef254156a364cbf41202c933015facc01f9069b87b237a0c556449e97b7e`.
Its advertised section table is beyond EOF; all disassembly uses program headers.

The bounded chain is:

* ARM vector `0x57228` → `0x57070` → Thumb `0x578bc`. Saved r7 is at
  frame+`0x1c`; service `0x30` reaches `0x579e6` → `0x66534` → `0x66560`.
* Crypto selector0 dispatches through `0x66604/0x66618` to `0x66d50`; the AES
  case begins at `0x66e1a`.
* Descriptor validator `0x66680` selects type6 at `0x66980`, checks the enable
  byte at global `0x7de28+0x38`, then invokes
  `0x6ad18(0, slot, ceil(length/32), ownerClass)`.
* `0x6ad18` checks slot bounds, owner permissions, and allocation extent using
  `0x6ae30`/`0x6ae58`. Its table at `0x7c408` contains slot permissions, not
  the AES key. The file-backed slot41 permission word at `0x7c4ac` and enable
  word at `0x7de60` are zero: these on-disk initial values are not evidence of
  the live provisioned state.
* The AES descriptor construction at `0x67c62..0x67c72` passes request+`0x34`
  (key descriptor) to `0x69390`. Its type is descriptor+8, i.e. request+`0x3c`.
* `0x693d2` is a TBB with table at `0x693d6`. Type6's byte is `0x2b`, giving
  destination **`0x6942c`**. That case requires slot<=`0x7f` and computes
  **`0x2000 + (slot << 5)`**, hence **`0x2520` for slot41**. It stores this
  address and selects engine address-space code1 at `0x69440..0x69466`.
* `0x67cf6..0x67cfa` writes that converted key address into command+`0x18`;
  `0x67cdc..0x67cf2` packs its high/address-space bits into command+`0x1c`.
  This path forwards a CCP-addressed key reference, rather than loading16 key
  bytes from an ELF data table.

The retained dictionary key was also absent as literal forward/reversed bytes
in the active secure-kernel file; this limited byte search is not a proof that
no derivation exists elsewhere. Boot provisioning and hardware secret state
were not reconstructed. The traced boundary nevertheless identifies the exact
missing input without substituting slot metadata for secret key material.

### Sufficient next evidence

For the exact active SELF hashes in section1, obtain either:

* the verified AES-128 key provisioned at CCP slot41 for that active key set; or
* successful service `0x30` in-place decryption outputs for Core
  `[0x530,0xb60)` and UI `[0x530,0xb80)`, using the selected active IV and the
  operation/direction/options above, with source and result provenance.

Then verify the complete signed messages against the active RSA key before
decoding any active capability. A reported service-success status without those
bound output bytes is insufficient. Signature failures must remain failures.

## 5. Reproduction and handoff files

Run static, in-memory recovery:

```powershell
python -B tools/check_shell_self_metadata_1360.py
python -B tools/check_shell_self_metadata_1360.py alternate
```

Both completed after adding the three independent signature-negative controls.
The active log records two candidate-verification failures; the alternate log
records two positive signatures and six rejected bit-flip cases. These are
offline crypto checks, not firmware execution or runtime sign-in tests.

Artifacts:

* `tools/check_shell_self_metadata_1360.py`: recovery/check source; text output
  only, with authenticated payload printing gated on signature success.
* `_TempData/loginmgr-agent-self-recovery.txt`: active binding and failures.
* `_TempData/loginmgr-agent-self-recovery-alternate.txt`: separately bound
  alternate positive/negative checks and authenticated payloads.
* `tools/inspect_secure_kernel_crypto_1360.py`: PT_LOAD-only static disassembler.
* `_TempData/loginmgr-agent-sk-crypto-service30.txt`, `-keypath.txt`,
  `-keyslot.txt`, `-aes-submit.txt`, `-aes-key-descriptor.txt`,
  `-key-address-table.txt`: secure-service/slot evidence (same prefix).
* `_TempData/loginmgr-agent-arm-auth-recovery-crypto.txt`: SM crypto evidence.

The parent-owned exFAT helper and prior policy document remain the references
for traversal and the complete auth-record decoder/validator respectively.
This handoff supplies verified alternate-only metadata and the active dependency
boundary; an active credential-provider source handoff is not ready. Local
credential recovery remains distinct from genuine remote PSN authentication.

Parent review moved the reproduction scripts into `tools/` and independently
reran both commands. Results reproduced: two alternate positive signatures,
six rejected tamper controls, and two active candidate failures. This review
did not execute firmware or install credentials into the running emulator.
