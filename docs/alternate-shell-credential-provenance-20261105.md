# Alternate Shell credentials: complete extracted-code binding

## Result

`tools/verify_alternate_shell_credentials_1360.py` now verifies **both alternate
Core/UI signed metadata AND every SELF segment / file-backed PT_LOAD byte**.
It also reconstructs the entire extracted ELF byte-for-byte from authenticated
headers, authenticated decrypted segments and explicitly zero-filled gaps.
This closes the earlier header-only pairing limitation for these exact inputs.

The decoded 0x88-byte records are construction results, not claimed mailbox0x16
acceptance results. The tool intentionally reports `runtime_credential_eligible:
false`: native environment/caller policy has not run, and no runtime provider is
installed. It exports no encryption keys. Optional decoded records are written
only to a user-selected local file, with exclusive creation (no overwrite).

Input pins (SELF, extracted ELF, SM) are in the verifier and agree with
`vsh-sm-authmgr-self-recovery-20261105.md`. Active-tree files and modified ELF
inputs fail the pins. A tree/name/authority is never a capability lookup key.

## Cryptographic chain and validated extraction

1. Same-tree pinned SM supplies the IV and RSA public descriptor. The local
   dictionary supplies one slot41 AES-128 candidate; the RSA signature, not the
   dictionary's assertion, validates its resulting plaintext.
2. AES-CBC metadata decryption and RSA-3072-PSS/SHA256 (MGF1-SHA256, salt32)
   authenticate the header, segment descriptors and all twelve 0x60-byte records.
3. Each segment metadata record M contains AES key at0, IV at0x10, expected
   MAC at0x20 and MAC key at0x40. **HMAC-SHA3-256 over stored ciphertext** matches
   all twelve records for each executable. SHA256/HMAC-SHA256 hypotheses failed;
   they are not used as fallbacks.
4. Each even entry decrypts to a digest/extent table. AES-CBC handles full
   blocks; a partial final block XORs AES-ECB(last ciphertext block, or initial
   IV if no full block). The authenticated extent table supplies compressed
   block offsets/encoded lengths. The native low-nibble length masking and
   uncompressed last-block exception are reproduced.
5. Each odd entry's blocks independently verify HMAC-SHA3-256 using its signed
   key and the authenticated table digest, decrypt with that entry's initial
   IV, and decompress when required. Every resulting byte must equal the
   corresponding extracted ELF segment. Decoder size and bounds failures abort.
6. The extraction manifest zero-initializes through the highest authenticated
   segment end, copies signed ELF header/PHDRs, copies signed extended info, and
   places the six authenticated plaintext segments at their signed offsets.
   **The result equals the entire retained ELF**, including all gaps. No guest
   startup, managed, resolution, transport or policy patches are part of it.
   Signed PHDRs are preserved even where a note advertises an extent beyond EOF.

Validated coverage:

| ELF PHDR | Core blocks | UI blocks |
|---:|---:|---:|
| 0 | 1625 | 844 |
| 1 | 420 | 269 |
| 3 | 30 | 39 |
| 7 | 4 | 105 |
| 8 | 77 | 77 |
| 11 | 1 | 1 |

Every PT_LOAD is covered, including the metadata/dynamic ranges nested inside
these extents. Exact output lengths: Core35337516, UI21878244 bytes. Per-segment
plaintext hashes and source/output hashes are emitted in the local manifest.

### Native source anchors

The retained Core/UI SM executable code is identical across the two trees as
documented in the earlier recovery; selected public-key/IV data differ. New
read-only disassemblies use the existing inspector, not firmware execution:

* Mailbox2 calls `0x2418`; mailbox6 calls `0x2790` (dispatcher table `0x5f0`).
* `0x340c` indexes signed per-segment metadata at hs + index*0x60 for layout5.
* `0x344c` retains that pointer; `0x34d8` copies IV from metadata+0x10.
* `0x3c68`, especially `0x3ccc..0x3d04`, selects layout5 hash context and
  passes metadata+0x40, length0x20, to key initialization `0x47c2`.
* `0x4fd6` initializes the layout5 hash descriptor with algorithm6 and a
  0xc8-byte state. Successful HMAC-SHA3-256 matches independently identify the
  software equivalent, rather than relying on an algorithm-number guess.
* `0x3984` decrypts; `0x3a26..0x3a76` uses `0x403c` for encrypted entries.
  `0x403c` reverses key/IV for the hardware interface; canonical software AES
  with the original metadata bytes is validated by the full ELF comparison.
* `0x3ed2..0x3f24` handles extent size masking; `0x3f50..0x3f60` resets the IV
  from metadata+0x10 per block. `0x3814` finalizes/checks each expected digest.

Local evidence: `_TempData/loginmgr-agent-arm-auth-segment-{dispatch,load,crypto,hash,aes-wrapper}.txt`
and `...-hash-algorithm.txt`. These contain instruction evidence, not keys.

## Auth-record decoding and defaults

The new decoder implements the exact mapping audited in
`vsh-sm-authmgr-policy-audit-20261105.md` section3: signed extended-info authority,
six individually endian-converted u64 fields, optional indices0..3 and8.
The destination starts as 136 zero bytes. Only native optional-record absence
(-2) leaves those bytes zero; an initially present but out-of-capacity requested
rank aborts (-34). Unrequested trailing bitmap bits are not globally rejected.
Core has no optional records; UI contains optional index0. No caller, uid,
role-based capabilities or invented successful service outputs are added.

## Reproduce

```powershell
$fw = '${USERPROFILE}/Downloads/13_Firmware 13.60.00_PS5UPDATE'
python -B tools/verify_alternate_shell_credentials_1360.py --firmware $fw --output "$env:LOCALAPPDATA/Temp/opencode/alternate-shell-full-provenance.json" --include-record
$env:SHELL_ALT_FIRMWARE = $fw
python -B tools/test_alternate_shell_credentials_1360.py
```

Choose a fresh output name on repeats. Verification and all five tests passed.
Tests cover endian/default semantics, requested optional rank failure,
unrequested bitmap behavior, source mismatch, both real signatures, header /
metadata / signature tampering, segment ciphertext tampering, and ELF code
tampering **below the outer source-pin check**. Real-input tests skip when the
environment variable is absent. No runtime execution is claimed.

## Runtime tree/staging audit and next implementation boundary

Static inventory compared the four relevant runtime directories:

| Directory | Active files | Alternate files |
|---|---:|---:|
| system_b/common/lib |315|285|
| system_ex_b/common_ex/lib |247|244|
| system_ex_b/app/NPXS40087 |62|52|
| system_ex_b/rnps/apps/NPXS40141 |5|5|

All **187** alternate managed AOT inputs across common_ex and ShellUI Application
successfully yield embedded managed PEs with the existing extractor. Missing
raw `mscorlib.dll` and seven Application DLL/EXE files can therefore be generated
from their own tree. `libScePsm.sprx`, `libScePsmUtil.sprx` and
`libmono-btls-shared.sprx` exist in alternate common_ex/lib, but no duplicate
ShellUI sce_module copies exist. Alternate `libc.sprx` is absent; active also has
29 additional common debug/instrumentation modules and `libSceNKWebDriver.sprx`.
Inventory is not a recursive import-closure or runnable-boot claim: loader search
paths and required imports must be checked by staging/launch work. Do not fill
missing alternate paths from active files merely because basenames match.

Parent's independent staging audit found active RVA/hash-pinned launcher and
Legacy patches correctly reject alternate inputs. Alternate resolution anchor
and Legacy changes need their own audited source/output transformation manifest.
Original NPXS40141 bundle containers compare byte-identically across trees;
an overlay must still record the original-container identity and plaintext
transformation. Existing stage contents must not be reused blindly.

**Exact next implementable native AppDb path:** use the verified alternate Core
ELF and source-bound constructed auth record as evidence for a narrowly scoped
credential provider, with the provider's trust/validation semantics explicitly
audited. Bind identity to exact verified bytes/manifest, not the filename; reject
any downstream modification lacking a separately validated manifest. The Core
record's authenticated bit62 can then support the real reserved-name registration
predicate. Only after truthful registration authorization, wire native worker
publication/wait/accept and pin dispatcher lifetime. Deny protected calls lacking
verified peer credentials. This verifier does not emulate mailbox0x16's external
environment/caller checks or silently mark them passed.

The remaining work is provider/launch integration and audited alternate staging,
not recovery of the active CCP key. This remains local OOBE/Home startup work,
distinct from remote PSN authentication.

Parent review read the verifier, decoder and tamper tests and independently ran
all five tests with the retained alternate tree configured. They passed in
1.906 seconds, including both real-input code/provenance checks. No runtime
credential installation or alternate boot was performed during this review.
