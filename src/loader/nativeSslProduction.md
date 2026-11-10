# Experimental native SSL routing (13.60)

Retail branch update (2026-11-08): the active SSL profile pins retail SHA256
`c4cd758bb1f9aa25ffef517e89e029ef4b3f6f3b22b7e525c1e4bf591b8fb3a7` and its
57-export table. The older SSL hash and run results below are historical.
See `docs/retail-native-ssl-prerequisite-20261108.md` for retail evidence and gates.

Default off. `tools/run_shellui_1360.ps1 -NativeSsl` sets `KYTY_NATIVE_SSL=1`
and enables the existing once-only libc startup bridge. The launcher clears an
inherited opt-in when the switch is absent. `-NativeSslSmoke` / exact
`KYTY_NATIVE_SSL_SMOKE=1` is mutually exclusive and still exits its isolated
process; it never continues boot. Neither mode enables a global libc preference.

`Loader::NativeSslSmoke::ProductionRequested()` is the shared network/loader
bridge. `RequestedMode()` captures both environment options together once per
process; only the exact string `1` enables either. Conflicting options stop loader
construction. Changing the environment after startup cannot change ownership.

## Admission and routing

The production path reuses smoke selection, same-open-image hash/profile checks,
raw export checks, required host registration checks, and PLT/non-PLT GOT checks.
I and S are selected before dependency loading or relocation, with duplicates,
prior initializers/relocations, and game patches rejected. The hashes are unchanged:

* I: `8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`
* S: `848900fb0ec5aee14d4e2bb2e98b78a00b910dbaa6cd96e45d24c1e06ea4f49e`

`SslPublicExports` contains all **57** raw public definitions in this S image,
with exact NID, RVA, size, global binding and function type. All are admitted
as `libSceSsl` library1 / module2.1. Production export insertion filters on that
full identity, not on NID alone. This includes the CA getter/free pair
`TDfQqO-gMbY` / `qIvLs0gYxi0`, plus every actual ShellCore SSL import.

Import compatibility is an explicit rule: the same `libSceSsl` library1 and
module identity, module version **1.1 or 2.1**, function type, and one of those
57 exact exports maps to the pinned module2.1 definition. Both raw import IDs
must occur exactly once in import metadata. Missing metadata, changed library,
module, versions, type or unknown SSL NID stops admission; the generic NID/HLE
fallback cannot rescue it. Own definitions in selected S use only its audited
`#G#A` export identity. No other library gets this compatibility rule.

Read-only audit of `SceShellCore.elf` SHA256
`6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a`
found **21** SSL imports, all raw `#Q#Q`, function type, library1/module2.1.
The host fixture lists all21 individually. Read-only checks also found
library1/module2.1 in this firmware's Http, Http2 and NpCommon clients. The
explicit 1.1 compatibility branch accommodates the older import contract;
it does not widen module-major/minor matching generally or prove every SSL ABI.

S alone gets the same **15** native libc bindings as the smoke: the indivisible
seven mspace entries plus eight primitive/stdio/finalization entries. Other
importers retain existing libc resolution. Actual stdout/stderr static-buffer
state and fresh S roots are checked after real DT_INIT without writing state.

`KernelDlsym` has a narrow loader dispatch before its normal handle/global
lookup. Known SSL names/NIDs return the exact selected native address, including
through global lookup. Unknown lookups on S's handle, and unknown SSL-prefixed
global names, return a miss without HLE fallback. SSL bindings bypass tracing
thunks so GOT and dlsym identity agree. Smoke keeps its private export policy.

## Real initialization and lifetime

The shared guest-stack adapter calls real `StartModule(I)` then `StartModule(S)`
on the primary guest stack with zero arguments and null callback. A small
admission state tracks order, in-flight/reentry, primary-thread/argument checks,
normal return and the unchanged raw result. A nonzero result stops the run.
Native exceptions, traps, hangs and nonreturns are not intercepted or converted
into completion.

Production returns from this adapter **before** any smoke init/stats/term call.
The first actual guest SSL caller owns the first SSL context. After successful
I/S admission, ordinary module initializers, cohost entries and main proceed.
I/S are pre-seeded as already initialized; no second initializer is executed.

Loader mutation is frozen only through provider initialization. Afterwards,
ordinary late module loads/relocations and unloads remain available. I/S
replacement, duplication, stop/unload and session clear/rearm remain prohibited
for their process lifetime. Normal module teardown skips these pinned providers.

Unresolved I/S thunks capture a strict-native bit when registered during
relocation. Execution aborts from that snapshot before late resolution or a zero
return. This does not add a path scan/loader lock to ordinary unresolved calls
after their existing retry/log budgets expire.

## Limits and validation

This is experimental production **routing**, not complete SSL/TLS support.
Additional CA/authentication/socket contracts, populated child/certificate/
connection cleanup and arbitrary allocator failures remain limitations. Real
initializer returns do not prove every constructor's ignored internal service
result. No readiness flags, setup outcomes, quota increases or synthetic native
state are supplied. Reaching unsupported services must retain failure/fatal
evidence rather than being called service closure or successful boot.

Author validation is source hygiene, PowerShell parse, and read-only firmware
metadata/hash comparison only; no emulator or build was run for this change.
Lead validation after review:

```powershell
ctest --test-dir <build-dir> -R '^(native_ssl_production|native_ssl_http|native_ssl_smoke|native_libc_startup|guest_fatal_trap|kernel_backtrace|standard_write|symbol_database)$' --output-on-failure
```

The new host-only production fixture exercises exact opt-in/conflict handling,
real-initializer admission/order/reentry/failure preservation, all57 export
identities, all21 ShellCore imports, the explicit version compatibility rule,
ambiguous/missing/wrong-owner metadata rejection and exact/NID/name coherence.
HTTP ownership fixtures are integrated as `native_ssl_http` separately.
Runtime obligations include actual I/S returns on the guest stack, first real
caller ownership, native CA/lifecycle behavior, ordinary late module loading,
continued initializers/cohosts, and visible boot progress. Retain actual errors;
neither the smoke result nor the host fixtures establish these outcomes.
