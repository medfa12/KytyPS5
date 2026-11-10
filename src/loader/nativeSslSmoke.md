# Isolated native SSL fresh smoke (13.60)

Retail branch update (2026-11-08): the active SSL hash/export profile is now
the original retail provider documented in
`docs/retail-native-ssl-prerequisite-20261108.md`. Older provider hashes and
smoke results below are historical, not verification of the retail provider.

Default off. Only `KYTY_NATIVE_SSL_SMOKE=1` enables it. The launcher switch is
`tools/run_shellui_1360.ps1 -NativeSslSmoke`; it also enables the existing libc
startup bridge and clears inherited smoke enablement when the switch is absent.
The implementation currently requires Windows for fault-safe bounded evidence reads.

The separate default-off `-NativeSsl` experimental routing mode is documented in
`nativeSslProduction.md`. It is mutually exclusive with this smoke. Shared
provider admission and DT_INIT helpers do not change the smoke's isolated exit.

## Admission and execution

* Select one I (`libSceLibcInternal.sprx`) and one S (`libSceSsl.sprx`) before
  `EnsureImportProviders` or any relocation. Missing providers load from the mounted
  `/common/lib`. Reject duplicate providers, prior relocation/startup, replacement,
  unsupported versions, and game patches. Missing mounted files are not downloaded.
* I SHA256: `8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`.
  S SHA256: `848900fb0ec5aee14d4e2bb2e98b78a00b910dbaa6cd96e45d24c1e06ea4f49e`.
  Same-open-image hashes, unique raw definitions (including size/type/binding),
  semantic library/module identity and versions, executable/data segment bounds,
  and the existing I leaf/fatal profile must agree. Firmware executable segments
  have PF_X without PF_R; data checks require PF_R|PF_W.
  S exports library1/module**2.1**; I exports library1/module1.1. S's imports
  of I and libkernel use library1/module1.1.
* S's export database accepts no exports. Exact, NID and name/global lookup cannot
  expose its functions. `FindProgramById` rejects its handle (including handle0
  aliasing), so existing `KernelDlsym` cannot reach its global fallback through S.
  The driver alone retains the three validated typed endpoints. I keeps its full
  export/unwind database.
* `nativeSslSmoke.h` lists the seven indivisible allocator bindings and eight
  additional primitive/stdio bindings. They override resolution only for importer
  identity S, with library1/module1.1 checked against raw import metadata. They
  include native printf, stdin/stderr objects and cxa_finalize. No global native
  preference is added.
* `HostContracts()` requires exact HLE-only registration identities for the mutex,
  map/query, RNG, diagnostic, stack-guard and constructor-hook services. Registration
  expressions come from existing `LIB_FUNC`/`LIB_OBJECT` stringification; conflicting
  overwritten registrations, incorrect expressions and zero addresses fail. The
  required imports of I/S route directly to these registrations, with no NID-only
  fallback. Final admission compares actual PLT **and** non-PLT GOT contents with
  the selected addresses. I/S import trace wrappers are bypassed to preserve that
  identity check. Unresolved function thunks reached by I/S abort this smoke rather
  than late-resolving and returning a misleading zero on the first call.
* Freeze loader mutation and pin provider lifetime. Reuse the existing once-only
  libc startup bridge. Enter a typed adapter through `RunEntry` on the initialized
  primary pthread's guest stack. It starts I then S through real `StartModule`
  admission, using their real DT_INIT (I+10, S+20), null callback and zero arguments.
  Admission enforces provider/order/thread/no replay; normal return and raw result
  are recorded separately from `started`. A nonzero result stops the probe. No
  direct calls to S+160, RNG setup, constructors or I+5120 are added.
* After real initializers, require fresh S roots (169188,16a060,16a458) and audited
  native stdout/stderr static-buffer state. No state is repaired. Call typed
  `int init(uint64_t)` with **0x80000**, then `int stats(int,void*)` with exactly
  32 output bytes and adjacent 16-byte canaries, then `int term(int)` with the
  **unchanged returned ID**. A negative init result gets no stats/term. Any
  nonnegative ID, including zero, is preserved. A normally returning stats error
  or damaged canary still gets exactly one term. No catch/retry intercepts native
  traps, exceptions, hangs or nonreturns.
* `std::quick_exit(0)` means the probe's calls returned success with intact
  canaries; `quick_exit(1)` means a returned probe/setup failure. Admission rejects
  use the existing fatal loader diagnostic exit. Native fatal paths retain their
  existing termination behavior. No unrelated initializer, cohost entry, ShellUI
  main or module fini sequence is run after the smoke.

## Evidence, not expected-value substitution

Logs retain initializer results, init ID, stats/term results, all32 actual output
bytes and actual before/after canaries. Stats/canaries are logged **before term**
so a fatal term does not erase that evidence. No expected counters are imposed.

The observer makes one fault-safe `ReadProcessMemory` attempt per block, at most
256 bytes per attempt. Reads cover two24-byte stdio headers, three8-byte fresh
roots, two8-byte context-root snapshots (pre/post-term), one96-byte context
header, and one176-byte native heap header at H+350. Total requested bytes are
at most360, excluding loader profile/hash checks. No list walks, heap scans,
dynamic retries, external process access or provider-state writes occur.
The context header is followed only if its actual ID equals the returned ID.
Heap/header fields and backing pointer/length are raw observations, not readiness
flags. The internal40-byte full-stat temporary belongs to native S's stack and
is not fabricated or recovered after return; the probe captures the public32
bytes and the separate actual allocator header.

`KernelVirtualQuery` records the backing mapping before and after term, preserving
the actual query result and returned range. An unavailable read/query is explicitly
marked unavailable, never interpreted as a zero counter or proof of retirement.
Post-term context memory is not dereferenced. A null retired root plus a changed
mapping/query is evidence for review, not an imposed success criterion. The reads
are bounded observations, not an atomic allocator snapshot.

## Limits

This is a fresh parent-context experiment, not native SSL adoption or a complete
constructor/failure-service closure claim. Real I/S initializer returns establish
those calls returned, not that every constructor's ignored internal operation
succeeded. Native stdout/stderr use static256-byte buffers on the admitted path;
diagnostic printing is not a reason to bootstrap the default heap.

Lead owns W's bounded SysV backtrace service and the real POSIX fd1/2 write service.
Their exact registrations are required; host fixtures and a real run must still
validate behavior. Invalid-parameter callback `OMDRKKAZ8I4`, allocator fatal
I+7789 outside the registered eight-site profile, bootstrap-only alternate mutex
services, alternate `/dev/rng` paths, populated child/certificate/connection
cleanup and arbitrary allocator failures remain unsupported. A run reaching an
unsupported path is not a readiness success. There are no quota increases,
synthetic counters, heap/TLS/stdio writes or blanket readiness assertions.

## Review / lead validation

Author-side read-only offline checking confirmed both hashes, all18 listed raw export
RVAs/sizes/types, raw module versions and all26 host registration expressions
in the current sources; `git diff --check` is the source hygiene check.

Lead validation: the combined Windows emulator/fixture build passed. The five
focused checks passed after correcting the standard-write fixture's readable
pipe-handle query and accommodating observed all-or-no-progress oversized writes.
Independent static integration review found no confirmed blocking regression;
actual loader/GOT/stack behavior remained a runtime obligation.

`nativesslsmoke1` (binary SHA256
`E073D1AD8152A659FB31910C707C49CF693B3BA89B4239157FCCD0C45188FA76`)
completed in 2.52s with exit0. I/S initializers both returned0. Native init
returned1, stats returned0 with intact canaries and words
`80000,15c0,15c0,0`, and term returned0 exactly once. Actual backing was
`2002b0000..200330000`, heap `2002b0020`, context `2002b0540`.
After term the registry root was null and querying the former backing returned
`8002000d` instead of the prior valid range. No unrelated initializer or ShellUI
entry followed. The three additional constructor bindings identified by review
(POSIX mutex lock/unlock and rwlock-init) resolved to their real HLE implementations.
This proves this fresh successful lifecycle, not arbitrary native SSL operation.

The default-off visible control `shinesslcontrol1` reached ordinary ShellUI and
stopped at its22s polling threshold (23.48s measured). Its inspected12s window
capture remained black with thin colored top lines. No shine/setup/home success.

1. Review private lookup policy, raw identity checks, importer-scoped routing,
   actual GOT verification, mutation/start admission and the guest-stack adapter.
2. Build the existing `virtual_memory_allocation_tests` and emulator targets.
   Loader/helper sources are included by the existing CONFIGURE_DEPENDS glob.
3. Run the integration fixtures (substitute the lead's existing build directory):

   ```powershell
   ctest --test-dir <build-dir> -R '^(kernel_backtrace|standard_write|native_ssl_smoke|guest_fatal_trap|native_libc_startup)$' --output-on-failure
   ```

   `native_ssl_smoke` is host-only: raw export ambiguity/version/type/string
   rejection, private export lookup, exact/conflicting host registrations,
   default-off, reentry, failure propagation, unchanged IDs, canary damage,
   term-once and nonreturn simulation. The backtrace service child dispatch is
   before `InitSubsystems`; its Windows service fixture runs in the existing
   `--kernel-backtrace-only` branch. Standard-write has its own CLI/CTest branch.
4. Lead runs one fresh isolated `-NativeSslSmoke` session using the normal mounted
   13.60 setup. Retain bindings, I/S return events, actual stats/canaries, backing
   queries, exit code and any fatal evidence. Confirm no unrelated initializer or
   ShellUI entry follows. Compare with a default-off launch to verify opt-in scope.
   Do not infer a successful native run from the host fixtures alone.
