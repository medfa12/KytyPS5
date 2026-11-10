# Native allocator fatal boundaries

The audited 13.60 `libSceLibcInternal` invalid-free path calls helper `+150b0`
at `+12fa0`, followed by a fallback `ud2` at `+12fa5`. The helper stores diagnostic
code `a002008b` through guest FS:28, then executes `int45; nop; ud2`.

Previously, `PatchProgram` replaced that complete sequence with `pop rbp; ret`.
The instruction emulator then skipped the caller's fallback `ud2`. Together
these converted a nonreturning native error into continued execution.

## Preserved boundaries

- `PatchGuestDiagnosticStore` retains the existing twelve-byte FS-store host
  adaptation. It never changes the following `int45`, `nop`, or `ud2`.
- Generic instruction emulation refuses UD2 instead of advancing RIP.
- Verified allocator fatal PCs bypass all runtime-linker recovery before
  nullcall recovery, guest SIGSEGV dispatch, and GPU page-fault handling. The
  handler returns unhandled without changing the saved context, regardless of
  the host's access-violation classification for INT45.

The loader recognizes the existing LibcInternal provider by canonical filename,
All export policy, same-open-file SHA256, unique raw own function export
(`Vla-Z+eXlxo#C#A`, library1/module1.1, RVA `12eb0`, size `fe`) and its resolved
provider-local address. Every audited window must lie in a mapped executable
PT_LOAD extent and match its post-adaptation bytes. No global NID fallback or
historical load address is used. Registration follows loading/patching and
precedes provider publication; deletion retires the sites before unmapping.
The startup bridge revalidates the complete profile and all eight registrations
immediately before invocation. A mismatch does not register a partial profile;
the opt-in startup bridge rejects a provider without the full profile.

Fault lookup uses a fixed lock-free atomic table. It reads no guest memory and
does not acquire the loader mutex. Registration and retirement remain serialized
by loader ownership. There are32 batch records, each with room for eight PCs,
and an unchanged96-slot admission limit (32 legacy three-site registrations or
12 complete profiles). Duplicate, zero, overflow, byte mismatch and exhaustion
rejections publish nothing. A release-published width/generation exposes the
entire batch only after all PCs are populated. Lookup rechecks that state;
atomic PC stores/loads and the retirement generation prevent accepting a stale
batch while its storage is reused. Generation saturation permanently disables
reuse rather than wrapping. Lookup has no retry loop.

Production unload must still quiesce provider execution before retirement and
unmapping. This is not a replacement for module lifetime synchronization.
Partial range retirement is supported for the legacy API; remaining entries
retain their original slot reservation until the whole batch is retired.

## Extended 13.60 profile: offline proof

Source: local read-only `system_b/common/lib/libSceLibcInternal.sprx`, SHA256
`8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`.
All addresses below are RVAs; lengths and offsets are hexadecimal. The native
store at each new site is exactly:

```
64 c7 04 25 28 00 00 00 8b 00 02 a0   mov dword fs:[28],a002008b
```

The existing adapter replaces only those twelve bytes with NOPs. Each checked
window begins with `85 c0 74 xx` (`test eax,eax; je success`), then the twelve
NOPs, `cd 45`, and the exact successor bytes listed below. The trap is at
window+10; no branch, trap, successor, register or saved exception context is
rewritten by this extension.

| Failure | Window RVA / length | File offset | JE displacement | INT45 RVA | Successor bytes checked |
|---|---|---|---|---|---|
| Full-stats lock | 11ab6 / 18 | 15ab6 | 0e | 11ac6 | `8b 0f 48 8d 47 40` |
| Full-stats unlock | 120e9 / 17 | 160e9 | 0e | 120f9 | `b8 01 00 00 00` |
| Mspace destroy | f3dc / 1c | 133dc | 11 | f3ec | `41 89 c4 41 8b 86 f4 03 00 00` |
| Diagnostic lock | 66ab / 18 | a6ab | 0e | 66bb | `8b 83 10 04 00 00` |
| Diagnostic unlock | 674f / 1d | a74f | 0e | 675f | `48 83 c4 08 5b 41 5e 41 5f 5d c3` |

None of these five INT45s has a contiguous UD2 fallback: their native successors
are ordinary instructions, including successful-path epilogues. Those successors
are deliberately not classified as fatal PCs. The previous helper INT45 and two
proven UD2s remain covered: `150c0`, `150c3`, `12fa5`. Total: eight PCs. The legacy
`MatchCode` and three-site `Register` APIs remain available and exercised.

## Evidence limits

The FS diagnostic-code store is still adapted away. This change preserves the
fatal control-flow boundary; it does not implement native FS:28 storage, native
trap reporting, allocator initialization, or missing diagnostic imports.
Existing Mono-specific recovery and guest INT3 handling remain separate paths;
there is no claim that every historical fault recovery has been removed.
The additional sites cover the five audited mutex-error edges, not every inline
allocator assertion, callback, OOM diagnostic, or TLS/debug service. Native
diagnostic imports still require their own implementations and binding review.

`guest_fatal_trap` tests the production store adapter, code recognition,
unmodified contexts under adversarial low-address fault classifications,
retirement, actual trap execution, and bounded Windows subprocesses with the
production exception handler installed. Those subprocesses must terminate on
the trap; continuation and timeout are failures. Their mappings are synthetic
audited instruction shapes, not native SSL heap allocations.

The Windows combined emulator/fixture build and `guest_fatal_trap`, `rng_sysctl`
and `process_sdk` checks passed. The production-handler subprocesses terminated
with `0xc0000005` for INT45 and `0xc000001d` for UD2. This demonstrates fault
termination for the fixture paths; it does not establish native diagnostic
service readiness or an allocator operation's complete failure cleanup.

Independent source review found no blocking defect. The bounded
`shinefatalpreserve1` boot (executable SHA256
`48a830c8df5885aa364d3497d19cab10e2cc79eb5054acf16ce257258e286f49`)
logged successful registration for the actual LibcInternal instance at
`0xa70000000`. SaveData retained the known SSL quota failure at `ShellCore+ada246`;
the observed threading gate remained zero and the screen remained black.
No native allocator failure was deliberately triggered in that boot.

## Extended fixtures and lead validation

The results above are historical three-site milestone results. The author used
offline image disassembly and source review. A
read-only Python comparison parsed the source profile constants and matched all
seven windows against the SHA-matched image, applying only the twelve-byte
FS-store transformation in memory; every byte matched. `git diff --check` passed.

The lead independently reviewed the batch publication/retirement protocol,
provider validation and extended fixtures. The combined Windows emulator and
fixture build passed, as did `guest_fatal_trap`, `native_libc_startup`,
`rng_sysctl`, and `process_sdk` (4/4). A bounded native-load run is recorded
separately in the evidence journal; these checks do not establish SSL readiness.
`shinepaforiginal1` admitted the actual provider at `0xa70000000` with `count=8`
and survived to the 15s polling threshold (16.6s measured). No allocator fatal
was deliberately triggered; the intermittent PAF fault did not reproduce.

The existing `guest_fatal_trap` fixture now additionally checks:

- Independent native fragments through the production FS-store adapter; all
  checked profile bytes are individually mutated to require rejection.
- Raw/unadapted and truncated profile rejection, base overflow/zero rejection,
  duplicate/legacy overlap rejection, and no prefix publication on failure.
- All eight PCs under the production handler's adversarial low-address fault
  classifications, with byte-for-byte unchanged CONTEXT; neighbors and ordinary
  successors excluded; partial/full retirement and storage reuse.
- The original32 legacy batches, the96-slot limit, and an eight-site admission
  rejected with only five slots free even though batch records remain available.
- Five additional Windows child processes, each entering one exact conditional
  native error block with EAX=1 in a synthetic mapped profile. The matched native
  windows remain unchanged. Each has a5-second bound and must terminate with the
  expected INT45 exception exit, not return or timeout. A child-only first VEH
  forwards the original exact-PC fault unchanged; a later/different fault exits
  with75, preventing a secondary crash from passing as the original termination.
  The original INT45/UD2 child fixtures remain.

Lead checks after building the emulator and `virtual_memory_allocation_tests`:

```
ctest --test-dir _Build/windows -R "^(guest_fatal_trap|native_libc_startup)$" --output-on-failure
```

Or run `virtual_memory_allocation_tests --guest-fatal-trap-only` and
`--native-libc-startup-only` from the actual build location. Expect seven fatal
subprocess results (two legacy, five inline); only the existing legacy UD2 case
expects `c000001d`, while INT45 cases require `c0000005` or `c0000096`.
Native load validation should log `Native allocator fatal sites registered` with
`count=8`. That proves profile admission, not deliberate runtime execution of
native allocator failures or SSL readiness.
