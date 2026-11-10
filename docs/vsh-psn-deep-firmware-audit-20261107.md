# Regular 13.60: PSN ticket, platform authentication and envelope audit

## Result

The sign-in failure spans an incomplete NP ticket transport and two secure
service dependencies before the authentication-ticket POST. The supplied
ShellCore contains intact, cryptographically verified OAuth envelopes. Their
plaintext is not recovered. No authenticated PSN exchange was performed.

This audit adds a reproducible offline verifier at
`tools/verify_psn_oauth_envelopes_1360.py`. Retained annotated disassembly and its
per-file source hashes are in `_TempData/psn-deep-audit/`. This document is local:
the repository's `/docs/**` ignore rule applies to it.

## Source and address conventions

Source root: `${USERPROFILE}/Downloads/13.60`. User-space addresses below are ELF
RVAs. Kernel addresses are full ELF virtual addresses. ARM secure-module
addresses are module-relative; entry at zero is ARM, handler bodies are Thumb.
Linear disassembly can include literal pools/jump tables; those bytes are not
treated as executed instructions. Xref scan results need aligned validation.

| Image | SHA256 |
|---|---|
| `system_b/vsh/SceShellCore.elf` | `6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a` |
| `system_b/common/lib/libSceNpManager.sprx` | `9fb15e49f0967015ce9ae19c39aaf316638a5a5dda89eb5c397b9a86dbbb0121` |
| `system_b/common/lib/libSceNpCommon.sprx` | `a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3` |
| `decrypted_modules/x86_kernel.elf` | `7b55225db09dc39ce46ec4123905d22e061eca8a35ca08d6ac4873f15216d379` |
| `decrypted_modules/sm_pltauth.elf` | `ad282e70be375c255db98f08f381e424d971ff89669db186d360cb23c47cdef0` |
| `decrypted_modules/sm_kms.elf` | `918ecf65156cd40e5a983cb75f61918780d074e8c11e8efb7d1808f3f022033e` |
| `decrypted_modules/secure_kernel_4.elf` | `9d6eef254156a364cbf41202c933015facc01f9069b87b237a0c556449e97b7e` |

`kernel.dec.bin` starts with SLB2, not an ELF header; the inspected kernel is the
extracted `decrypted_modules/x86_kernel.elf`.

## 1. Managed sign-in to the NP server

`ReactNative.Modules.Vsh.LoginMgr.LoginMgrModule::Run_CreateAuthenticationTicket`
is at `ReactNative.Modules.Vsh.dll.sprx +0x18da90`. Native ordinary ticket export
`IxqaB0MHl0s` is NpManager `+0x125e0`; login variant `xR8S67myUos` is `+0x135e0`.

| Operation | NpManager RVA | IPC method / behavior |
|---|---|---|
| Private create export | `0xfc90 -> 0x15340 -> 0x7ad0` | 5; server-issued positive request ID |
| Delete | `0xfd10 -> 0x15660 -> 0x7b50` | 6; local deferred deletion while active |
| Abort | `0x15840 -> 0x7bd0` | 7; sends 4-byte request ID |
| Acquire/bind completion | `0x158a0 -> 0x7c50` | 8; request ID + 64-bit mask, event-name descriptor, 16-byte output |
| Release | `0x15ab0 -> 0x7d10` | 9; request ID, then local active-count cleanup |
| Ordinary ticket submit/result | `0xada0 / 0xae50` | `0x65 / 0x66` |
| Login ticket submit/result | `0xc130 / 0xc1e0` | `0x88 / 0x89` |

Method 5 has two inputs: a 16-byte header (selector at +0, length at +8), and
the payload of that length. Default private create selects zero and sends a
4-byte zero. This is not an output-pointer API that can be satisfied by returning
an arbitrary emulator handle.

Client tracker at `+0x15340` allocates one of 64 event bits and a 0x28-byte record:
ID +0, active count +4, deletion flag +8, mask +0x10, links +0x18/+0x20.
Exhaustion returns `0x80559d05`; missing request lookup returns `0x80559d0a`.
Initialization `+0x150e0` creates the name `SceNpMgrIpcC%08x` using the process ID,
with event attributes `0x121`.
Acquire clears the selected event bit and passes the name/mask to method 8.
Wait at `+0x159a0` uses that mask and mode `0x21`; it is not an HTTP polling loop.
Ticket flow is acquire -> submit -> wait -> result -> release. Result retrieval
uses JSON and correlation-ID output descriptors, with capacities supplied in a
24-byte input: ID at +0, JSON size at +8, correlation size at +0x10. In
`+0xae50`, the two actual output descriptors use fixed capacities 0x801/0x25;
these are distinct from the caller-supplied sizes carried inside the input.
Ordinary submit uses an 8-byte ID/user header and two NUL-terminated strings.
Login submit uses a **4-byte** request-ID header and the same two strings.

### Implemented private lifecycle and descriptor transport

`src/libs/libOfflineLogin.cpp` now binds create/delete/abort to typed exports in
`src/libs/libNpManager.cpp`. They acquire the retained owner's client, never the
owner-neutral fixture Session. Create submits the two method-5 descriptors and
tracks only positive server-issued IDs in 64 slots. Zero/duplicate successful
IDs retire the endpoint. Abort preserves the slot. Delete retains ownership on
server/transport failure so retry or Detach can clean up; this is an intentional
failure-handling difference from native idle deletion, which clears its local
record after sending. Successful deletion of an acquired record retains its
completion bit until the ticket operation releases it.

`Endpoint::InvokeInputs` preserves input boundaries and copies before worker
dispatch. `InvokeBuffers` supports the two independent ticket-result outputs,
validates every returned descriptor, retains actual per-buffer written lengths,
and publishes only after success. Timeouts cannot write public buffers later.

The native endpoint now supplies optional request-completion hooks, installed by
`runtimeNpManagerClient.cpp`. On the actual retained transport worker it lazily
creates the named event with attributes `0x121` and initial bits zero, clears only
the selected bit (`KernelClearEventFlag` keep-mask `~mask`), and binds method8.
Successful bind replies must contain the 16-byte server alias/matching mask.
Polling uses a separate full-width mask and mode `0x21`; it does not block the
transport worker waiting for completion. Creator deletion occurs only after
connection retirement drains native operations/server aliases, with failed
deletion retained for retry on that same worker.

Both ticket exports now resolve the retained owner's client and execute
acquire/bind -> submit -> completion poll -> two-buffer result -> method9 release.
The client mutex is dropped between polls, permitting abort, delete and Detach.
Generation checks prevent an old wait from releasing a replacement client's
request. Reacquiring an active request rejects before clearing its completion bit.
The export retains its client lease across the entire wait.

Output descriptor capacities remain fixed at `0x801/0x25`, while caller sizes
travel in the 24-byte result input. Returned lengths and terminators are checked
in staging buffers before publication. Bind, submit, poll, result and release
failures leave caller outputs untouched. Query-only adapters still return
`0x80550002` before inspecting ticket arguments.

Intentional defensive differences from native: failed binding rolls back local
acquisition (the native path can retain the increment); duplicate local waits
are rejected before clearing; wait errors are propagated; a failed method9
retires the endpoint rather than reusing a possibly still-bound bit. A null
correlation pointer with nonzero size is rejected. Oversized/nonterminated
input strings are rejected at the admitted transport's combined `0x2000` limit.
No arbitrary HTTP/ticket completion deadline is introduced: individual worker
jobs retain their bounded host observation deadline, and Detach/abort can
interrupt a pending operation.

### Server dispatch and completion

ShellCore dispatcher `+0xccdd90` uses the signed-relative table at `+0x1f53d98`:

| Method | Dispatch target | Handler / continuation |
|---|---|---|
| 5 | `0xcce65f` | `0xccb0e0 -> 0xccec10 -> 0xc8fb70`, session registration `0xcd22b0` |
| 6 | `0xccdffe` | `0xccecc0 -> 0xcd2350 / 0xc8fbd0` |
| 7 | `0xcce923` | `0xcced00 -> 0xc8fd00` |
| 8 | `0xccea52` | `0xccb1b0 -> 0xcced30 -> 0xcd2490 -> 0xcd3a80` |
| 9 | `0xcceab2` | `0xcced90 -> 0xcd2690 -> 0xcd3b00` |
| 0x65 | `0xcce24d` | `0xccce30 -> 0xcd0c60 -> 0xc91b70` |
| 0x66 | `0xcce0b9` | `0xccce80 -> 0xcd0cd0 -> 0xc91c70 -> 0xca8ac0` |
| 0x88 | `0xcce5b1` | `0xccd910 -> 0xcd1710 -> 0xc93080` |
| 0x89 | `0xcce389` | `0xccd960 -> 0xcd1770 -> 0xc93100` |

Method 8 first finds the actual session, then its tracked request. The request
list node has ID +0x10, bound flag +0x14, event handle +0x18 and mask +0x20.
`+0xcd3a80` rejects a second binding with `0x80559d1c`. It opens the supplied
name through NpCommon `5DkyduAF2rs`, returning handle/mask in the 16-byte output
and retaining that pair only if open succeeds. The handle is the server's alias,
not a replacement for the client's created event handle.

Completion callback `+0xccfdf0` stores a negative operation result through
`+0xc90020`, then calls `+0xcd2550`. That function finds the session owning the
request and calls `+0xcd3b00`: if bound, set the event mask, close the server
alias, and clear the bound flag. Method 9 invokes the **same** function, so
release also signals an outstanding binding; it is not a close-only operation.
NpCommon +0x6860/+0x6880/+0x68f0 resolve respectively to kernel event open/close/set
(`1vDaenmJtyA`, `s9-RaxukuzQ`, `IOnSvHzqu6A`). These addresses were checked against
the regular image hash above, independently of the alternate-image inventory.

For ordinary submit with user -1, `+0xc91b70 -> +0xcb3790` allocates a 0xd98-byte
work item (`+0xcc8bd0`) and a 0x38-byte tracking object (`+0xca26b0`). The tracker
stores the callback/context at +0x18/+0x20. Enqueue `+0xcb21d0` records kind 0x11
and installs `+0xcb3cd0` at work-item +0x48 (context at +0x40), links the tracker,
and submits via `+0xcc5dd0` to NpCommon `KQSxXJBepQ4` (`+0x1f760`). That routine
rejects duplicate IDs across the queue's three lists, binds the work item's
queue pointer at +0x30, appends through +0x1ef70, and wakes the queue condition.
The user-specific path instead calls `+0xc98a00`.

NpCommon worker loop `+0x1fce0` waits for queue +0x40, moves an item through
`+0x1f040`, drops the queue mutex, and calls item vtable+0x18. The ticket vtable
at Core +0x2051ec8 resolves that slot to `+0xcc65e0`, which checks positive request
ID, calls preparation at vtable+0x38 (`+0xcc66d0`), then execution at +0x40
(`+0xcc8d00`). The verified execution chain continues `+0xca8840 -> +0xcbc200`.
After return, the common worker records the result via +0x1f110 and drains state
6 through +0x1fa40. Outside the mutex, draining calls item vtable+0x20 (Core
`+0xcc6650`) with the stored result, then deletes the item. `+0xcc6650` forwards
request ID, result and context to the installed work-item callback.
`+0xcb3cd0` saves the registered callback/context, cleans up through `+0xcb3e40`,
then invokes that callback with request ID and result, reaching `+0xccfdf0`.

Result `+0xca8ac0` finds request data, checks its result through `+0xcae590`,
gets the payload via `+0xcaf520`, and copies JSON/correlation fields at
payload +0x1e8/+0x9e9. Missing request data returns `0x80559b27`.

Named client-event creation, acquisition/deferred-deletion accounting, and both
ticket export sequences are implemented and fixture-tested. Native endpoint
admission still needs the matching regular SELF credentials; fixtures do not
establish that admission or a successful PSN response.

## 2. Platform authentication precedes OAuth

ShellCore `+0xca65f0` generates a 16-byte nonce, constructs request type 6, then
calls PFAuth create `+0x7ed580` and execute `+0x7ed6a0`. Execution reaches
`+0x7ee6f0`. URL construction at `+0x7efffc..+0x7f01d6` produces:

`https://ps5.<environment>.playstation.net/api/csla/v10/console/consoleToken`

The User-Agent is `PlatformAuthClient/2.0`. Request header setup at `+0x7f04a0`
uses `Content-Type: application/octet-stream`.

| ShellCore helper | ioctl | Kernel destination / observed contract |
|---|---|---|
| `0x1797670` | `0xc040530c` | Generate C1; copy 0x1d0 bytes to packet output pointer +0x18 |
| `0x1797760` | `0xc040530d` | Require 0x1e0 input and 0x1290 context; verify R1/C2 and generate 0x3c0 R2 |
| `0x17977f0` | `0xc040530e` | Process final response; output length is input length minus 0x58 on success |

Device open `+0x17975f0` selects `/dev/pltauth0` or `/dev/pltauth1`.
Kernel dispatcher `0xffffffff80801430` serializes calls and checks caller
authorization. Device packets carry operation status at +0x38 and additional
status at +0x3c; an ioctl syscall returning zero alone does not mean success.

Kernel secure wrappers:

- `0xffffffff80a85b00`, debug name `pltAuthSmGenC1`: service string `80021005`,
  mailbox command `0x201`, 0x80-byte message.
- `0xffffffff80a85cc0`, debug name `pltAuthSmVeriR1C2GenR2`: command `0x204` on
  this path. It also gathers `SceSysCore.elf` and `SceShellCore.elf` information
  before submitting the mailbox; the exact authentication meaning of these
  fields remains to be traced.
- `0xffffffff80a860d0`: final-response processing, reached from ioctl 0x530e.
- `0xffffffff807b4610` is identified by its debug string as
  `sceSblServiceMailbox` and calls the secure transport at `0xffffffff809c0c70`.

ARM `sm_pltauth` has a matching state machine at +0x414. Its TBB table at +0x528
maps 0x200 to reset, 0x201 to C1, 0x204 to R1/C2, and 0x203 to final processing.
Successful transitions are 0 -> 1 -> 3 -> 4; invalid state returns -0x28.
Command 0x210 retrieves a 32-byte value only in states 3 or 4.

This is not self-contained user-space code: +0x68c restores a 0x170-byte context
through secure syscall 0x76; +0x2168 reserves a 0x18d0-byte aligned scratch area
and fills it through secure syscall 0x71; C1 generation +0x21e8 also calls secure
cryptographic operations. The syscall implementations are now mapped below.

### Secure-kernel input provenance

`secure_kernel_4.elf` contains ARM/Thumb code with entry VA 0x51000. Addresses in
this subsection are its ELF VAs. Its section-header offset is past EOF; only
file-backed PT_LOAD mappings were used. Startup sets VBAR to 0x57220. The SVC
vector at 0x57228 branches to ARM 0x57070, which saves registers and enters Thumb
dispatcher 0x578bc. It reads the saved r7 as syscall number. This matches the
`sm_pltauth` Thumb veneer at +0x38: move syscall number r0 to r7, shift arguments,
then `svc #0`. The halfword jump table at 0x57912 covers syscalls 0x60..0x88.

- **0x71:** table entry 0x57934 selects 0x57db8, calling 0x58de2 with the current
  secure process and destination pointer. This checks the identity field at
  process+0x90 against `0x4e00000000000001`, validates a 0x18c0-byte destination
  through 0x5cfec, and copies **0x18c0 bytes from VA 0x380000** using 0x6e2b6.
  The 0x18d0 seen in the caller includes alignment slack; it is not the syscall
  payload size. VA 0x380000 is outside every file-backed segment of this image.
  This operation copies runtime data; it does not derive that data from the ELF.
- **0x76:** table entry 0x5793e selects 0x57e42, passing a read-direction flag to
  0x5a738. Lookup at 0x5a6f0 searches eight 0x20-byte records at VA 0x740c0.
  `sm_pltauth +0x68c` requests ID `0x5c33a056`, length 0x170, offset zero. Records
  carry ID +0, validity +4, owner identity +8, backing pointer +0x10, length +0x14,
  and read/write permission masks +0x18/+0x1c. The helper checks caller permission,
  extent and destination mapping before copying from backing pointer + offset.
  **All eight registry records are initially zero in the supplied ELF.** This is
  persistent module state, not evidence of another missing console secret:
  command 0x200 initializes the state through `sm_pltauth +0x6b0`, and the success
  path at +0x5fc deletes/recreates the record via syscalls 0x75/0x74, then writes
  the 0x170-byte module state using syscall 0x77. Subsequent commands restore it
  with 0x76. The kernel 0x74 branch reaches 0x580a2, allocating from an 0x8c0-byte
  pool and publishing a registry record. These memory/permission/persistence
  operations need implementation, but their state can originate from executing
  the module's reset path. Treating the empty initial registry as a successful
  restored context would skip that initialization.

Retained evidence: `secure-kernel-*.txt`, `sm-pltauth-svc-veneer.txt` and
`secure-input-provenance.json`. Reproduce the source-pinned addresses/extents with
`python -B _TempData/capture_psn_secure_inputs.py`. The admitted secure caller
identity and runtime contents of VA 0x380000 have not been demonstrated. The
state-registry creation path is now identified separately from that unresolved
boot-data source. Secure cryptographic execution and input provisioning remain
necessary.

ShellCore sends 0x1d0 C1, receives the first binary response, obtains 0x3c0 R2
from the device, sends R2, and passes the final response back through the device
before returning token data. Relevant callsites: +0x7eeaa7, +0x7ef02e,
+0x7ef111, +0x7ef3c8. No captured live exchange validates this path yet.

## 3. OAuth envelope: structure verified, secure key operation unresolved

Client config `+0xc8c5c0 -> +0xc8c390` selects one of two 0x1e0 envelopes.
Unwrap `+0x1622e50` opens `/dev/envelope`, ioctl `0xc3483101`.

The 0x348-byte ioctl packet is now mapped on both sides:

| Offset | Field |
|---|---|
| 0x000 | Type (0x28 here) |
| 0x004 | 0x300-byte RSA public blob (two 0x180-byte big-endian integers) |
| 0x304 | Parameter (0xb here) |
| 0x308 | 32-byte input blob |
| 0x328 | Envelope length |
| 0x330 | Envelope pointer |
| 0x338 | Output pointer |
| 0x340 | Service status |

Kernel ioctl `0xffffffff80e48430` validates length, copies in the envelope,
calls `0xffffffff80e47bb0`, copies out length minus 0x1d0, wipes the temporary
buffer, and writes status to +0x340. Its worker verifies magic `cc cb 73 51`,
version 1, type, and payload length. Signature verification at
`0xffffffff80e47d60` hashes header[0:0x50] + 0x180 zero bytes + ciphertext.

**Both actual envelopes verify using RSA-3072/PSS/SHA-256 and the embedded public
key.** Each contains a 16-byte encrypted payload. Report:
`_TempData/psn-deep-audit/envelope-verification.json`.

After verification, kernel `0xffffffff80e47f50` allocates a secure key slot and
sends command 0x401 (slot at +8, type at +0xc, parameter at +0x10, blob at +0x14).
`0xffffffff804a07b0` opens service `80021001`; `0xffffffff804a0780` submits it.
The matching `sm_kms` command-0x401 handler is +0x7d0 -> +0xc9e: it accepts slots
0x50..0x52 and parameter <=0xd, uses a parameter-indexed 32-byte table and
secure slots 0x3a/0x3b, then derives/installs into the requested slot. It crosses
another secure operation at +0x19b0. Service-name-to-module registration is not
independently reconstructed; this association is supported by matching command
and packet fields. `sm_authmgr` also contains command 0x401 but has a different
packet contract and must not be substituted by opcode alone.

The KMS +0x19b0 boundary is now resolved one level further: it constructs a
0x58-byte crypto packet and calls +0x2216 with operation 0x20. That veneer invokes
secure **syscall 0x30**, not syscall 0x20. In `secure_kernel_4.elf`, the syscall
dispatch at VA 0x579e6 reaches 0x66534/0x66560, checks the caller and packet mapping,
then uses the byte jump table at 0x665c4. Operation 0x20 selects 0x665f6 and helper
0x68200. Operation 0x20 continues through 0x68234 to 0x6a3f8. Descriptor mode1
selects 0x6a86c, validates 16/32-byte data and programs the hardware register
block referenced by the literal `0x03007018` at 0x6ac1c. Submission reaches
0x6a228 and completion polling through 0x69f08. These are static control-flow
observations, not successful hardware execution or recovered root material.
Provisioning of slot 0x3a remains unresolved.

Source-hashed captures: `_TempData/psn-deep-audit/secure-kernel-crypto-op20.txt`,
`secure-kernel-crypto-op20-execute.txt`, and annotated literal/descriptor evidence
in `_TempData/loginmgr-agent-sk-crypto-psn-op20-hardware.txt`.

Kernel `0xffffffff80e48060` decrypts through the allocated slot and crypto
requests, then releases the slot and performs the post-decrypt check. Neither
slot 0x3a's root material nor working secure crypto execution has been
established. The public RSA blob is sufficient for signature verification only.

This OAuth-envelope dependency is distinct from the regular SELF slot41
metadata/admission blocker and from PFAuth's device context.

## 4. OAuth and ticket submission

ShellCore +0xca8840 obtains the console token and client config before calling
+0xcbc200. The selected ticket path is
`/api/authn/v3/sso/ticket/consoleSignin`, with scope
`oauth:create_authn_ticket_for_console_signin`.

OAuth at +0xcb9ad0 uses `/api/authz/v3/oauth/token` with client_credentials,
client_id, unwrapped client_secret, console_token, scope, and token-format
selection. Ticket construction +0xcbc630 uses JSON, Bearer authorization,
`X-Psn-Console-Token`, and optional npsso cookie; request create/submit/result
calls occur at +0xcbc994/+0xcbca27/+0xcbca93.

## Implementation consequences and checks

1. Validate the implemented named-event/ticket flow against an admitted regular
   firmware server. Completion may close the server alias before method9 release;
   creator deletion remains behind connection retirement.
2. Supply native/HLE secure-service execution and actual runtime inputs for
   PFAuth and envelope operations. The missing pieces are now identified below
   the user-space HTTP layer, but their complete implementation is not proven.
3. Retain the explicit local-account UI path while these dependencies are
   incomplete. It establishes local NotSignedUp/SignedOut state only.
4. The invisible dialog remains a separate measured defect: popup width is zero
   despite a 1920x1080 parent. Nothing in this static authentication audit fixes
   or revalidates its rendering.

Verification performed: both source-pinned envelope signatures pass; deliberate
signature and ciphertext corruption are rejected; empty/truncated envelopes are
rejected. Command:

```powershell
python -B tools/verify_psn_oauth_envelopes_1360.py ${USERPROFILE}/Downloads/13.60/system_b/vsh/SceShellCore.elf
```

The private request, query export and native endpoint suites pass with the
production implementations and deterministic transport fixtures. Coverage includes
server IDs, capacity/abort/delete behavior, owner rejection, lease cleanup,
input-copy lifetime, distinct output lengths, and failure-output preservation.
New ticket coverage exercises both export ABIs, initialization/owner rejection,
all execution-stage errors, malformed/oversized output lengths, bit63, duplicate
acquisition, abort/delete/Detach during a wait, replacement-generation isolation,
and export-lease cleanup. Endpoint coverage exercises owner-worker event hooks,
clear-mask isolation, pending/signaled polls, unrelated IPC progress while
pending, create/clear/poll errors, malformed binding replies, failed creator
deletion retries, and late method8 completion after host timeout.
The full `kyty_emulator`, `np_account_abi_tests` and
`virtual_memory_allocation_tests` targets build successfully with these changes.
The five selected integrated checks also pass: `np_account_abi`,
`np_manager_client`, `np_manager_service`, `runtime_user_service_owners`, and
`runtime_user_service_controllers`. The account ABI check resolves and invokes
the production create/delete/abort NIDs and verifies owner rejection. Native
firmware-server admission and live ticket completion remain unverified.
No interactive navigation run was performed for this transport change;
Home/app switching/media behavior and the invisible dialog are not revalidated.
