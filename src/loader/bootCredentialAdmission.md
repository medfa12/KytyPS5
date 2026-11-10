# Source-bound alternate Core/UI admission (2026-11-05)

## Trust root

This is host emulator admission policy, **not native mailbox0x16 acceptance**.
The reviewed offline verification in commit9383800b establishes RSA-PSS metadata,
all HMAC-SHA3-256 segments/blocks, decrypted PT_LOAD bytes, exact extracted ELF
reconstruction, and the decoded136-byte records. Runtime trusts these compiled
SHA256 bindings, never editable receipt booleans, file names or declared roles:

| Input | SHA256 |
| --- | --- |
| Original Core,35337516 bytes | `f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434` |
| Core record | `5d9c1daacc67e91fbe1ddfdd0eac992270edc0e55986564cf579da89a95cc57a` |
| Original UI,21878244 bytes | `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060` |
| UI record | `b35057cfe2d2d844f287325c438b39950ed6555f686592af546eb12fb950e0aa` |
| Reviewed1080p UI | `b4ce830af247ae6611af81dece472710fa728a5c6855280d609168b62d556f4c` |

UI's sole admitted file transform changes the eight-byte dimension immediate at
RVA0x122fd/file0x162fd from3840x2160 to1920x1080. Admission checks the complete
transformed hash, exact new immediate, then reverses those eight bytes in a host
copy and verifies the complete original hash. Parent staging resolved its PSM
call0x1241d -> PLT0xd23750 -> `lWlBrUu77Kg#B5#Bk`. Any additional file patch fails.
Active-tree credentials are never transplanted. No secure-service result is
invented; `NativeMailboxPolicyExecuted` remains false.

## Actual loader and mutation contract

With the corresponding host record environment variable set, the loader takes
one immutable snapshot from its already-open plain ELF. Exact size only selects
a candidate. Whole-image digest and actual mapped bytes establish admission.
Every PT_LOAD must match its file-backed source and zero BSS, exact signed VA,
extent and order. Parsed ELF/PHDR and cached dynamic tables are also compared to
the snapshot, preventing a different metadata read from authorizing the image.
Process-parameter reads use that same snapshot. Proof is retained before LNC
registration and becomes queryable only once the retained identity is registered.

The host's subsequent loader transformations are explicitly limited to:

* Existing checked FS diagnostic-store replacement (`PatchGuestDiagnosticStore`)
  and exact FS:0 TLS-read adaptation to the program's host TLS thunk.
* `PatchGuestInstructions`: decoded function-boundary-based Windows red-zone
  memory thunks and supported CPU instruction adaptations (reciprocal sqrt,
  EXTRQ/INSERTQ, RDPID/CLWB, timestamp handling). These are host ABI/CPU adaptation,
  not guest authorization or successful return-value rewrites.
* ASLR relocations and registered import binding from the authenticated dynamic
  tables. Admitted images accept only RELATIVE/DTPMOD64/64/GLOB_DAT/JUMP_SLOT;
  symbol indices and eight-byte destinations are bounded, destinations must lie
  in non-executable PT_LOAD memory. Invalid records revoke admission and fail.

Name-scoped VSync/Farsight patches are excluded from admitted images. Arbitrary
GamePatch plans, including pending plans when an admitted image arrives, fail.
This is admission plus a constrained host loading pipeline, not continuous
hashing of writable guest state or a claim of isolation between cohost programs.

## Identity, connection and lifetime

Core gets a distinct monotonic cohost PID after matching LNC registration. UI
keeps its registered PID100, with its own exact authenticated record. Other
cohosts receive distinct identities and no implied credentials. Transport uses
only retained current identities and actual pthread host thread IDs. Weak
Program lifetime and explicit revocation prevent stale receipt reuse.

The exact `SceAppDbIpcmsg` factories route to the dedicated adapter when either
Core/UI record is requested or that adapter is installed. In that configuration,
uninstalled routing fails rather than returning generic compatibility success.
Installation requires admitted Core. Reserved-name registration checks its real
capability62. Ordinary regular-firmware boots without this source-bound selection
retain the earlier compatibility adapter; its successful calls are not evidence
of native AppDb acceptance. This keeps the native-service experiment from making
an unavailable regular credential provider a prerequisite for existing Home/VSH.

Read-only alternate Core disassembly locates worker0xa916c0, config ctor call
0xa916ea -> PLT0x1958b30, factory0xa9174c and dispatcher0xa91779. It leaves all
three config flags at zero. Matching libSceIpmi ctor0x1160 confirms this. Kernel
connect0xffffffff805a68c0 bypasses cross-process checks only for the actual same
PID; otherwise helper0x808192d0 rejects restricted authorities with unrestricted
server config, and0x80818f90 requires client capability62 when cross_no_cap=0.
Thus **UI credentials are required for connection too**, not only methods.

Production hooks enforce that policy using exact source-bound records. Core's
native method handler sees the actual peer PID and cannot fall back to compositor
compatibility credentials. Unadmitted or revoked peers deny. Callback addresses
must be in Core executable file-backed PT_LOAD; the existing guest boundary runs
on the real dispatcher pthread with Core context and a retained execution lease.

The shared tested transport implements transactional server publication,
wait-for-server, real callback acceptance, explicit replies and stop/drain.
Dispatcher remains on its native caller's stack until stop; guest code performs
the actual worker join before destroying backing. `RuntimeLinker::Clear` refuses
mapping release while any adapter objects/API calls remain. No readiness events,
connect acceptance, method success or timed delays are synthesized. Finite
nonzero connect deadlines remain explicitly unsupported by the shared adapter.

## Runtime handoff

Use the parent's matching prepared argv `_Runs/a1360prep/prepared-launch.json`
(or its OOBE counterpart) with `_Build/vsh-integrated/kyty_emulator.exe`. Set
**host environment**, not `--guest-env`:

```powershell
$env:KYTY_APPDB_CORE_AUTH_RECORD = "$env:LOCALAPPDATA/Temp/opencode/alternate-core-auth-record.bin"
$env:KYTY_APPDB_UI_AUTH_RECORD = "$env:LOCALAPPDATA/Temp/opencode/alternate-ui-auth-record.bin"
```

Both are local raw136-byte records with the hashes above. Append `--vsh-session`
to the prepared arguments: production LNC/cohost identity setup is gated by that
option, and the current prepared JSON does not contain it. Use original alternate
Core and the parent's pinned UI.

`tools/run_shellui_1360.ps1` also accepts `-CoreAuthRecord` and `-UiAuthRecord`
directly (defaulting to those host environment variables). With
`-LauncherProfile alternate -VshRuntime`, both records are required before
staging or launch. The runner checks file presence/size; runtime still checks
the exact record and image hashes. `-PrepareOnly` records both paths in its
launch manifest. Omitting the records previously left AppDb uninstalled and
allowed ShellUI to proceed into a null AppBrowse accessor.

## Tests

`virtual_memory_allocation_tests` adds retained-input modes:

* `--source-bound-core-identity-only`: `KYTY_TEST_ORIGINAL_CORE`, Core record env.
* `--source-bound-ui-identity-only`: `KYTY_TEST_UI_IMAGE`, `KYTY_TEST_UI_PARAM`, UI
  record env; run separately for original and reviewed1080p UI.
* `--source-bound-loader-only`: all five env variables; runs real mapping and
  host transforms, LNC identity setup and production adapter installation,
  registration/copyout, missing-client-credential denial and teardown. It never
  invokes guest entries, initializers or handlers.

The loader test calls `LoadProgram`, not `Relocate`. Offline scans of both pinned
images found only the four expected relocation types and no invalid destinations;
the parent's independent evidence is
`_Runs/alternate-stage-audit/admitted-relocation-shape.json`. Full relocation and
import binding still require the parent's native boot observation.

`runtime_process_identity` covers registry regressions. The standalone `tests/ipmi`
suite includes `appdb_native_transport_tests`, exercising failed publication,
wait/cancel, acceptance/rejection, actual peer PID, blocking dispatcher and join.
These host tests do not establish end-to-end native firmware startup success;
the parent owns runtime launch and observation.
