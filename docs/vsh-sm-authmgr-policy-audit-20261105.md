# Original ARM auth-manager audit — completed bounded handoff

## Result

**Mailbox 0x16 validates and returns an already constructed 0x88-byte auth record.**
For normal SELF inputs, its capability word at auth+8 is the first **big-endian
u64 of decrypted, authenticated SELF metadata**. The authority ID is a separate
little-endian u64 from extended info. No ShellUI/ShellCore role-to-bit62 grant
occurs in the traced construction/validator path.

**Authenticated plaintext is still required to reproduce either actual record.**
The initial extracted-file/archive-member audit found no original SELF metadata.
Parent follow-up subsequently recovered both original SELF containers from the
raw filesystem images, with matching embedded ELF headers and extended info.
**Section 9 supersedes the initial source-availability conclusion:** original
metadata regions and signature trailers are available; their decryption and
authentication remain unproven. No capability is inferred from executable names.

The validator's branch/dataflow reconstruction is complete within the bounded
normal-SELF / mailbox-0x16 scope. External secure-service results and crypto
operations remain external inputs; no claim of an executable, independently
validated replacement for the complete secure module is made.

## Binary, address convention, and method

Input: `${USERPROFILE}/Downloads/13.60/decrypted_modules/sm_authmgr.elf`.
57,028 bytes; SHA256
`cfeabbcfec15255433eec7d6d90ad9e36da4c9288c55eb5c68a4ecb3a6c27711`.
The `secure_modules/` copy is identical. All ARM addresses here are **ELF virtual
addresses**, with load base zero, not raw file offsets.

| File offset | VA | File/memory size | Flags |
|---:|---:|---:|---:|
| 0x1000 | 0 | 0x52f8 | RX |
| 0x7000 | 0x6000 | 0x1648 | R |
| 0x9000 | 0x8000 | 0x4ec4 | RW |

ELF32/LE/ARM program headers are parsed directly. The section table advertised at
0x3de6c is beyond EOF and is never used. Entry 0..0x30 is ARM; it branches to
Thumb code. `0x38` is the generic service wrapper (`r7=service; svc #0`).
Disassembly includes instruction bytes and PC-relative literal values. Linear
evidence dumps also include literal pools; their decoded mnemonics are **data,
not reachable instructions**. In particular 0x2170..0x217b is a byte jump table,
resolved explicitly below. `final-ranges` separates the validator code before
0x216c and from 0x2190 onward from its intervening literals/table.

## 1. Mailbox ABI and context lifetime

The dispatcher receives/checks a 0x80-byte mailbox (`0x538..0x588`), reads the
u16 command at `0x594`, compares 0x16 at `0x5a2`, and calls `0x233c` at `0x6a4`
with **mailbox+8**. Command 1 calls header authentication/construction `0xa70`
at `0x610`; command 0x16 is the later loadability check.

| Mailbox offset | Handler-body offset | Data used by 0x233c |
|---:|---:|---|
| 0 | — | u16 function 0x16 |
| 4 | — | outer mailbox result, supplied through dispatcher reply path |
| 8 | 0 | u32 classification |
| 0x10 | 8 | u32 context ID |
| 0x14 | 0xc | first short; not consumed by this handler |
| 0x16 | 0xe | signed short forwarded to validator |
| 0x18 | 0x10 | u64 address of caller's 0x88-byte auth record |
| 0x20 | 0x18 | u64 destination address for target's 0x88-byte auth record |
| 0x28 | 0x20 | operation result, explicitly zeroed on successful copyout |

`0x233c` performs:

1. Reject context ID >3 or inactive bit in u16 `0xce66` with **-22**.
2. Resolve `ctx = 0x8108 + id*0xdb0`; return nonzero cached `ctx+0x98` verbatim.
3. Zero a local 0x88-byte caller record; copy input through `0x434c`.
   Transfer failure maps to **-5**.
4. Call `0x1dec(caller, ctx+0x10, ctx, classification, signedShort,
   u32(ctx+8), 1)`. A nonzero validator result is returned without copyout.
5. Copy **unchanged ctx+0x10, length0x88** to destination via `0x42a8`.
   Transfer failure maps to -5; success clears body+0x20 and returns zero.

The return flows through `0x862/0x868` and reply helper `0x4010`. Keep outer
mailbox status and operation result distinct. Native kernel `+0x337da0` checks
the transport/function/outer/operation results before publishing its staged
reply; see the preceding x86 handoff for that side of the contract.

Header handler `0xa70` reserves one of four contexts, points its backing buffer
at aligned storage inside the context, zeros its 0xd00-byte backing and
0x88-byte auth record, sets cached status **-40**, then processes the header.
The successful construction path calls the same validator with null caller,
classification0, short0, the actual context type, and caller-check0 at `0xe4e`.
Its result is cached at `ctx+0x98` (`0xe52`). Header construction success alone
must therefore not be treated as a successful loadability decision.

## 2. Normal SELF header processing and authentication boundary

Normal context type is zero (`0xce6..0xce8`). Types1 and0x10 enter separate
restricted-authority branches (`0xb82..0xe10`), including 0x31... and0x41...
identities. Those constants are not credential templates for the two 0x48...
Shell executables.

The normal path imports the header, checks declared sizes against the0xd00-byte
context buffer, checks its identifier/type, and prepares crypto handles.
`0x1014..0x1044` and `0x12a0` check header/meta extent and minima before the
normal verification sequence:

* `0x12a8..0x132a`: validates supported SELF magic/version/format bytes. Accepted
  byte7 values here are0x12,0x32,0x42,0x44,0x46. These are not an ELF fallback.
* `0x1332..0x1348`: calls `0x1734` with the original **u32 header+8** and output
  slots. That helper selects tables/handles using encoded type/revision bits,
  static data, environment flags and secure services. It never supplies a
  per-Shell authority-to-capability mapping.
* `0x13c8` calls `0x1c8c`: this helper is a **bounded copy**, not cryptography.
  It checks source offset/length against0xd00, copies via `0x5038`, and reports
  copied length. Its caller checks the returned length.
* `0x141e`: metadata starts at backing+u16(header+0xc). `0x1442` conditionally,
  then `0x146a` unconditionally, call `0x403c` with this same buffer as source
  and destination and u16(header+0xe) as length. `0x403c` constructs a crypto
  descriptor and submits it via `0x5010` → **service0x30**. This establishes
  the in-place metadata transformation and external dependency; the entire
  secure-service crypto implementation is outside this binary audit.
* `0x14b4..0x14e2`: computes trailer size0x100 or0x180 from byte7 bits4..6 and
  hashes `[H, H+hs+ms-trailer)` via `0x4e3c` into32 bytes. The descriptor path
  also terminates in service0x30.
* `0x151a`/`0x152e`: signature-related verification using the selected key
  descriptor and the trailer. The latter path explicitly checks decoded
  padding `00 01 FF... 00`, a19-byte DER SHA-256 DigestInfo prefix at0x7554,
  and the32-byte digest (`0x153a..0x15a4`). These comparisons support the
  RSA/PKCS#1-v1.5/SHA-256 interpretation for that branch, not every variant.
  Signature-related failure is **-36**. The separate context-type1 handling
  at0x15aa must not be applied to the normal type0 path.
* `0x15b8..0x15da`: requires header+0x1a low selector `(b & 0xe)==2` and
  bits4..6 in{2,3,5}. Cleanup/segment-range checks precede construction.

Consequently, reading bytes at a guessed offset in a decrypted ELF is not
equivalent to supplying the authenticated metadata consumed below.

## 3. Exact metadata location and auth-record construction

Let H be the context's SELF backing pointer and all arithmetic refer to its
**post-transformation** bytes. `0x325e` locates the SELF extended-info record:

```
n = LE16(H+0x18) & 0xfff
E = H + 0x20 + 0x20*n             // embedded ELF base
p = LE16(E+0x38)                  // ELF64 program-header count
extendedInfo = align_down(E + 0x4f + 0x38*p, 16)
```

The helper also checks room using `hs - 0x20*n - 0x20` against
`align_down(0x6f + 0x38*p,16)`. This parsing operates after earlier header
validation; it is not a standalone hostile-input parser.

`0x32a8` locates the additional metadata:

```
hs = LE16(H+0xc); ms = LE16(H+0xe)
sig = (H[7] >> 4) & 7
trailer = {1:0x100, 3:0x180, 4:0x180}[sig]   // unsupported -> -35
layout = (H[0x1a] >> 4) & 7
stride = {2:0x50, 3:0x60, 5:0x60}[layout]    // unsupported -> -22
P = H + hs + n*stride
L = ms - n*stride - trailer
```

Note the masks select **three bits**, not all four bits of the high nibble.
`0xe66` requires L>=0x30. The returned u32 length uses native arithmetic;
earlier whole-header validation is part of the contract.

The destination record A is initially all zero. `0xd94..0xd98` copies its
authority; `0xe82..0xeea` performs the following byte-exact mapping:

| A offset | Source | Conversion |
|---:|---|---|
| 0 | extendedInfo+0 | LE u64, copied unchanged |
| 8 | P+0 | BE u64 → LE u64; **caps[0], including bit62** |
| 0x10 | P+8 | BE u64 → LE u64 |
| 0x28 | P+0x20 | BE u64 → LE u64 |
| 0x30 | P+0x28 | BE u64 → LE u64 |
| 0x38 | P+0x10 | BE u64 → LE u64 |
| 0x40 | P+0x18 | BE u64 → LE u64 |
| 0x48+16*i, i=0..3 | optional record i | two independently BE-decoded u64s |
| 0x18 | optional record8 | two independently BE-decoded u64s |

This covers every byte of A[0..0x87]. The code reverses each input32-bit word
and swaps the two words in each u64; it does not reverse a whole16-byte record.

### Optional records (`0x332c`)

* L<0x30 -> -35. If `L-0x30<0x20`, no optional record: -2.
* Bitmap is16 bytes at P+0x30, least-significant bit first within each byte.
  Packed16-byte records start at P+0x40.
* Capacity `k=((L-0x30)>>4)-1`. For requested index j, first check bitmap bit j
  (j<=127), then count set bits at indices<j. If rank>=k -> -34;
  otherwise publish `P+0x40+rank*16`.
* The actual helper continues through subsequent contiguous present indices,
  bounded by k. It checks each subsequent rank, but once at least one record
  was accepted, a later missing/out-of-range record returns success. Thus an
  implementation claiming exact helper behavior must not substitute a global
  "validate all bitmap entries" rule. Invalid initial index -> -22; absent
  initial bit -> -2. Rank failure before accepting any record -> -34.
* Construction calls indices0..3 and8 individually. It tolerates only -2 as
  absence; the corresponding initially zero destination remains zero. Any
  other returned error aborts construction.

## 4. Validator `0x1dec`: bounded reconstruction

Signature by observed call convention:

```
validate(callerAuth, targetAuth, context, u32 classification,
         i32 signExtendedShort, u32 contextType, bool checkCaller)
```

Let A=targetAuth, C=callerAuth, G=0xce64, and **ERR=0x800f0d2f**.
`word(A+x)` below means an LE32 load; `bit` numbering is zero-based.
This function does not write the target record. All record stores in its body
are stack bookkeeping. Its only nonlocal calls are the extended-info helper,
secure-service wrapper, zero-fill helper, 64-bit right-shift helper0x5266,
and stack-canary failure handler.

### 4.1 Version/environment stage (0x1e10..0x1f68)

1. Expected version-high is0x13600000, or0x13590000 when
   `(word(A+4)&0xf0000000)==0x30000000`.
2. Nonzero contextType uses this expected high and zero low. Normal type0
   gets the qword at extendedInfo+0x18. For the0x3... authority family,
   normalize low=`(low>>20)&0xffffff00`, high=`high<<16`; otherwise if
   high<0x10000, set low=0 and high<<=16.
3. Call **service0x7c, selector1**, writing a stack u16. Return its failure
   directly. Also call **service0x76, selector0x38a9cf67**, output length0x30,
   last arg0; its success is qualified by output word0==0x20002. Failure of
   this second query is retained as a flag rather than immediately returned.
4. Flags `F=(word(G+4)!=0 ? 4:2) | (shortArg==0 ? 8:0) |
   (qualifiedService76Success ? 16:0)`.
5. Version mask is0xf0 if G[0x10] bit2 is set, otherwise0xfff0.
   Authorities0x3800100000000001 and0x4800100000000001 skip the main version
   comparison. Otherwise, if word(A+0x28) bit30 is clear, require
   `(high & 0xffff0000)==expectedHigh` and `(low & mask)==0`.
   If that bit is set, require the u64 pair
   `{high & 0xffff0000, low & mask & ~0xff}` <= `{expectedHigh,0}`.
   Failure returns **ERR+8 =0x800f0d37**.
6. If word(A+0x3c) bit26 is set, except authority0x4800000000000001,
   require `((normalizedHigh>>8)&0xff)` <= the low byte returned by service7c.
   Failure returns **ERR+9 =0x800f0d38**.

### 4.2 Caller compatibility (0x1f74..0x202a)

Skipped when checkCaller=0. Otherwise:

1. If word(A+0x28) bit28 is set, require C[0x17] bit3 or G[0x11] bit0.
2. Look for **any** equal16-byte pair between a nonzero caller entry at
   C+0x48+i*16 (i=0..3) and any target entry A+0x48+j*16 (j=0..3).
   A match passes this stage.
3. Without a match, choose the **highest-priority set bit among62,61,60** in
   the caller's u64 at C+8. Require that same bit in target u64 A+0x28.
   No such caller bit, or absent selected target bit, fails. It does not try
   lower-priority bits after a failed selected bit.

Failure of either check returns **ERR+10 =0x800f0d39**. In particular, target
A+8 is not altered to satisfy a caller requirement. The target mask tested
here is **A+0x28**, not the capability word exported as caps[0].

### 4.3 Platform stage (0x202c..0x20c0,0x2196..0x21c0)

Platform `id=LE16(G+0x24)`, target platform flags `w=word(A+0x44)`.
The required predicates are:

| Platform ID | Predicate |
|---|---|
| 0xa102, or `(bswap16(id)-0x283)&0xffff <=0x10` | w bit31 |
| 0x8102 / 0xa202 | w bit30 |
| 0x8202 / 0xa302 | w bit29 |
| 0xab02 | w bit25 |
| 0xa902 | w bit26 or bit30 |
| 0xa802 | w bit27 or bit31; alternatively authority category1 and G[0x10] bit6 |

Other IDs or a false predicate return **ERR**. These are numeric platform
selectors; marketing/devkit names have not been assigned from guesswork.

### 4.4 Classification stage (0x20c2..0x20da,0x213a..0x225e)

Category=`word(A+4)&0x0f000000`. Skip classification when checkCaller=0,
category is9/5/7, word(A+0x28) bit27 is set, shortArg!=0 and category1, or
authority==0x4800000000000001. Otherwise classification must be1..12 and
satisfy this table. `w=word(A+0x3c)`:

| Class | Actual TBB target | Required predicate |
|---:|---:|---|
| 1 | 0x2190 | w bit27 |
| 2 | 0x21fe | w bit26 |
| 3 | 0x220a | w bit31 |
| 4 | 0x2218 | w bit25 |
| 5 | 0x21c2 | w bit24 |
| 6 | 0x221e | w bit23 |
| 7 | 0x2224 | w bit22 |
| 8 | 0x222a | reject shortArg==0/category1; then G[0x10] bit5 or w bit21 |
| 9 | 0x224e | w bit20 |
| 10 | 0x21c8 | w bit19 |
| 11 | 0x2254 | w bit18 |
| 12 | 0x21ce | w bit17 AND platform in{0x8102,0xa202,0xa902,0xa802} |

TBB bytes at0x2170 are `12 49 4f 56 2b 59 5c 5f 71 2e 74 31`;
target=`0x216c+2*byte`. Failure is **ERR+11 =0x800f0d3a**. The code's
temporary registers/stack slots are reused: at0x1f70 it saves authority-high
over a former version-high slot. Confusing these produces an incorrect
"firmware category" interpretation of this stage.

### 4.5 Final flags stage (0x20dc..0x2124)

* If F bit4 is clear and `(authorityHigh&0xff0f8000)==0x48010000`, reject
  with **ERR+12 =0x800f0d3b**.
* With G+4 zero (F bit1), word(A+0x34) bit29 must be clear; with G+4 nonzero
  (F bit2), its bit30 must be clear. Failure is ERR+12.
* If word(A+0x34) bit28 is set and G[0x13] bit0 is clear, return
  **ERR+13 =0x800f0d3c**. Otherwise return0.

### Environment provenance

G is zero-filled in the file, **not** a captured initialized environment.
Initialization0x430 calls `0x4030(2,G+0x10)`, then
`0x4030(0,G+0x20)`; 0x4030 translates these to **service0x70** selectors2/0.
G+4 is computed after service0x70 selectors5/6 at0x482..0x4ac. The live
service0x7c/0x76 results above are additional inputs. File zeros cannot be
substituted for those platform-service responses when claiming exact policy.

## 5. Missing bytes and retained-source provenance

### Current target pair

| Target | SHA256 | Authority |
|---|---|---|
| `13.60/system_b/vsh/SceShellCore.elf` | `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a` | 0x4800000000000010 |
| `13.60/system_ex_b/app/NPXS40087/eboot.bin` | `cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09` | 0x480000000000000f |

Each starts with ELF, has14 program headers, a retained64-byte extended-info
record at0x350, and **all zero bytes in [0x390,0x4000)** before the first
PT_LOAD. Its retained versions are both0x1360000000000000; these can support
the normal version input, not capability decoding.

The initial extracted-file inventory identified the following missing source.
Section 9 subsequently recovers items 1 and the original-container portions of
item 4; authenticated plaintext in items 2/3 remains unproven:

1. Original SELF prefix `[H,H+0x20)`, including format byte7, identifier+8,
   hs+0xc, ms+0xe, entry count+0x18, and layout+0x1a; original segment table
   `[H+0x20,H+0x20+n*0x20)` and embedded-ELF association.
2. **Post-decryption P[0..0x2f]**, the fixed48-byte auth payload. The first
   missing8 bytes alone determine caps[0]; bit62 is bit6 of P[0].
3. If present, **P[0x30..0x3f]**, the16-byte optional presence bitmap, plus
   packed16-byte records at `P+0x40+16*popcount(bitmap[0..j-1])` for
   j=0,1,2,3,8. Their absence must be proved by length/bitmap, not assumed.
4. To independently authenticate rather than merely decode those fields:
   the original transformed metadata region `[H+hs,H+hs+ms)` (or original
   ciphertext plus the appropriate secure-service inputs) and signature
   trailer `[H+hs+ms-trailer,H+hs+ms)`, with binding to this exact target.
   For complete IsLoadable2 reproduction, also supply the actual caller
   record, classification/short/context type, and platform-service outputs.

The extracted ELFs alone cannot supply P/L: their hs/ms/n/layout bytes are
missing. Section 9 supplies those offsets from newly located original SELF
headers. ELF0x350's64-byte record is extended info, not P; padding zeros are
not optional-record absence proof.

### Additional retained sources checked

Read-only prefix inventory covered **3,484 files /770 ELFs** in `13.60` and
**4,059 files /1,319 ELFs** in
`13_Firmware 13.60.00_PS5UPDATE`, with no read errors. The first tree's only
top-level SELF-magic files were updater and EAP kernel. The second additionally
has firmware/boot-component SELF files under `kbin_unpacked`; their paths are
listed in the provenance artifact, not treated as Shell credentials.

The second tree's target copies are also decrypted ELFs with the same missing
header gap. Their hashes differ from the active pair:

* ShellCore `f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`.
* ShellUI `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`.

The five-volume `13_Firmware 13.60.00_PS5UPDATE.part01.rar` archive was listed;
its corresponding ShellUI member and one representative ShellCore member were
streamed to memory with `7z x -so`, never extracted to executable files. They
match the second tree's hashes and contain the same zero gap. Other listed
ShellCore copies have identical archive size/CRC. The archive is not a backup
of the original SELF metadata for this purpose. These differently hashed
builds must not be used as provenance for the active pair.

The surviving updater SELF independently exercises the location formula:
hs0x2b0, ms0x3f0, n6, signature selector3, layout5 -> P offset0x4f0,
L0x30. Its extended-info authority is0x3800100000000001, not either target.
Its bytes cannot supply the missing Shell metadata. This location calculation
does not claim its ciphertext is already authenticated plaintext.

Conclusion of this initial inventory is bounded to extracted files and streamed
archive members: metadata cannot be reconstructed from their retained
authority/version/digest records or zero padding. Raw filesystem-image members
were not included in that inventory. Section 9 records the original SELF
containers subsequently found there.

## 6. Implementation consequence and independent checks

A genuine provider is blocked on the bytes/provenance above. The coherent
boundary, once those inputs exist, is:

* authenticated, image-bound header/context source;
* exact normal-SELF record decoder with optional-record semantics;
* loadability validator consuming explicit caller/platform inputs;
* staged0x88-byte output installed into the correct process credential only
  after all statuses succeed, following the already traced x86 exec chain.

No production provider or accepted Shell credential is established by this
audit. Section 9 provides original containers for further recovery, not verified
capabilities. The frozen NP ABI fixture proves attached-endpoint
behavior only. It cannot validate the missing credential input.

Independent, hand-checkable structural vectors (not real credentials):

* BE bytes `01 23 45 67 89 ab cd ef` at P+0 must become LE bytes
  `ef cd ab 89 67 45 23 01` at A+8. A16-byte optional record reverses each
  8-byte half separately.
* P+0x30 bitmap `05 01 00...` has indices0,2,8. Their records are respectively
  P+0x40,P+0x50,P+0x60, and indices1/3 return -2. A length0x70 permits all
  three; length0x60 makes the initial lookup of index8 fail -34.
* For the caller gate alone: C+8 bits62 and61 set, target A+0x28 only61 set,
  and no matching optional entries -> ERR+10. Selecting any common bit would
  incorrectly pass. Adding bit62 to the target mask passes that stage only;
  it does not predict overall platform/version/classification acceptance.
* The updater's independent structural values above yield0x4f0/0x30.
* The actual TBB data resolves to the12 code targets in the classification
  table, rather than instructions suggested by linear decoding of its bytes.

These are reconstruction checks, not native success vectors. No successful
ShellUI/ShellCore vector exists in this evidence set.

**PASS:** `python -B tools/check_sm_authmgr_evidence_1360.py` checks actual
PT_LOAD bounds/copy hashes, actual TBB destinations and DER prefix, independently
specified endian/bitmap vectors, and the surviving updater's location formula.
Result: `_TempData/loginmgr-agent-arm-audit-checks.txt`. It executes no firmware.

## 7. Parent integration findings carried forward

Parent's AppDb finding (`_TempData/oobe-appdb-auth-dispatch.txt`): native sync
ShellCore+0xab2380 and async+0xab2450 require successful `get_authinfo` and
caps[0] bit62 through+0xb3ef70, then ShellUI authority through+0xb3fb20 or
actual session peer==getpid; otherwise0x80f50013. Connect callback+0xab2510
allocates session backing/calls server+0x28 without that method check. This
ties AppDb/OOBE methods to the same missing authenticated credential source;
successful connection alone is not sufficient. This is parent-supplied
evidence, not a duplicated AppDb audit.

Parent subsequently completed the transport prerequisite in commit `16644fa4`
after handler audit `8e686cbb`: exclusive pre-registration WaitServer readiness
reservation, provisional publication barrier, generation-revalidated Connect,
and cancellation before acceptance. Shared NP/LoginMgr slot+0x80 now waits for
actual publication and native acceptance for null timeout; pointer-to-zero uses
the existing immediate path. Positive finite timeout explicitly returns
Unsupported pending post-dispatch timeout cancellation. Parent reports all12
standalone IPC CTests pass in `_Build/oobe-ipmi-wait`; see
`docs/oobe-ipmi-server-wait-implementation-20261105.md`. Production service hooks
and AppDb routes are still uninstalled, with no runtime success claim. This
supersedes the earlier handoff's missing-slot16 status and does not supply the
credential bytes identified here.

## 8. Evidence and reproduction

Read-only program-header-based disassembler:
`tools/inspect_sm_authmgr_1360.py` (label plus `VA:size[:arm|words]`).
Disassembly artifacts under `_TempData/loginmgr-agent-arm-auth-`:

* `entry.txt`, `dispatcher.txt`: entry mode, service wrapper, dispatch and reply.
* `isloadable.txt`: exact0x233c handler and validator continuation.
* `policy.txt`: validator entry, construction and memory-transfer helpers.
* `metadata.txt`: construction, metadata helpers, header verification.
* `helpers.txt`: selector/key preparation, bounded copy, signature/hash and
  endian/shift support.
* `environment.txt`: environment initialization, zero file globals, crypto
  descriptor submission.
* `final-ranges.txt`: validator code regions, table bytes, reply and service
  boundaries.

`tools/inspect_sm_authmgr_inputs_1360.py` reproduces the source inventory
and in-memory archive inspection, producing
`_TempData/loginmgr-agent-arm-input-provenance.txt`.
Prior `loginmgr-agent-authority-inputs.txt` retains target program headers,
extended-info bytes and SM segment inventory. The x86 provider/install evidence
remains linked from `docs/vsh-np-account-abi-and-credential-source-20261105.md`.

All new work is audit scripts/artifacts/documentation. No production/test
source edits, live emulator/app execution, UI interaction, executable overwrite,
subagent, or commit was used for this continuation. Optional native user-record/
property6 work was not expanded; this bounded credential audit ends at the
identified missing original inputs.

Parent review retained the three read-only reproduction scripts under `tools/`
for the rollback checkpoint. Artifact output names remain as recorded above.
Structural checks do not execute the validator or establish real credential
acceptance; section 9 records the later source-availability correction.

## 9. Parent follow-up: original containers recovered from filesystem images

The source search was extended to members **inside** the retained raw exFAT
images `segments/dev_ssd0.system_b` and `segments/dev_ssd0.system_ex_b`.
7-Zip rejects these images: physical file lengths are shorter than declared
exFAT volume lengths. However, the relevant directory sets and complete target
extents are physically present. The bounded read-only helper
`tools/inspect_shell_auth_images_1360.py` traverses the real directory entries,
verifies directory-set checksums, validates FAT/physical extent bounds and reads
the targets into memory. It performs no mounting, file extraction or firmware
modification.

### Active 13.60 image pair

| Target inside image | Cluster / bytes | SHA256 of original SELF |
|---|---|---|
| system_b: `vsh/SceShellCore.elf` | 6542 / 16,157,964 | `e44e37dada3c1d756c0a4c6937c96ef6e99c04f8db6dbc687063e25765f616a3` |
| system_ex_b: `app/NPXS40087/eboot.bin` | 1465 / 9,423,671 | `d4d6d736dce381442e79e5143b72b6172172dd9c257ae3fce4f2be2cf96efd9c` |

Both directory streams are contiguous with equal valid/data lengths; clusters
are 65,536 bytes. Both target prefixes are original SELF magic `54 14 f5 ee`,
despite their ELF-like filenames. Both embedded ELF headers plus program-header
tables compare byte-for-byte equal to the corresponding active extracted ELF.
All 64 extended-info bytes also compare equal, including retained digest. This
is source association evidence; it is not signature verification or full
decrypted-segment comparison.

For both SELF headers: `n=12`, embedded ELF at `0x1a0`, extended info at `0x4f0`,
`hs=0x530`, signature selector3, layout5 and stride0x60. The original metadata
locations are now known:

| Target | ms | Metadata region | P / L after transformation | Signature trailer |
|---|---:|---|---|---|
| ShellCore | 0x630 | `[0x530,0xb60)` | `0x9b0 / 0x30` | `[0x9e0,0xb60)` |
| ShellUI | 0x650 | `[0x530,0xb80)` | `0x9b0 / 0x50` | `[0xa00,0xb80)` |

The bytes at P are **not claimed to be decrypted or authenticated**. In
particular, interpreting these original bytes directly as capabilities would
not follow the transformation/verification sequence in section 2.

### Alternate image pair

The second firmware tree also retains original SELF members. Their hashes are
ShellCore `0e365cf5850f156cc099777faf12aa12ba4fb2e71d7d49d82f2aaf5b7708beab`
and ShellUI `73fd5ed27d0341620f7431598f2828f7206665b39675739dec942740bd703130`.
Embedded ELF headers/program headers/extended info match that tree's own
extracted pair, not the active pair. Preserve these associations separately.

Reproduction: `python -B tools/inspect_shell_auth_images_1360.py`.
Captured output: `_TempData/shell-auth-image-provenance-1360.txt`.
All four target traversals and header/extended-info comparisons passed.

The credential worker has been redirected to the recovered original containers
to establish metadata decryption/authentication or identify the precise remaining
crypto/service input. The earlier absence conclusion is not a reason to stop
that investigation. Genuine credential construction and PSN sign-in remain
unverified.

Follow-up evidence is now recorded in
`vsh-sm-authmgr-self-recovery-20261105.md`: alternate-tree metadata verifies
against its own auth-manager key; active-tree candidate verification fails.
Those alternate records must retain their own image provenance.
