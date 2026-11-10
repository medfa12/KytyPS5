# Default-off native LibcInternal primary-startup bridge helper

## Evidence and scope

The audited export is `h8jq9ee4h5c#C#A`, native RVA `I+0x63f0`, size `0x62`,
ABI **`void KYTY_SYSV_ABI ()`**. The local 13.60 `libSceLibcInternal.sprx` used
to derive the leaf profile has SHA256
`8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`.
The leaf bytes were read from file offset `0xa3f0`; none were executed during
this task. `MatchesLeafProfile` hashes exactly those 98 bytes in a caller-supplied
readable span against the pinned digest; the firmware instructions are not embedded.
It uses checked offset/subtraction bounds. The span offset is not
implicitly an RVA or ELF file offset. This **does not verify whole-image identity**.

The leaf contains no calls, TLS access, locks or allocation. Its own native
instructions set the readiness word, conditionally OR bit1 at three non-null
auxiliary roots' `+0x370` fields, and reset three diagnostic words. It has no
native once guard: a second call resets diagnostics again. Current captures
reported gate0 and null auxiliary roots in all seven samples; that observation
does not justify replacing the leaf with host stores or assuming roots always null.

The audited call at `K+0x403a9` belongs to **primary TCB startup before the module
initialization loop**, not ordinary child-thread creation. This helper invokes
only the selected native entry. It contains no gate/global writes, constructors,
default-heap setup, provider lookup, address-to-program inference, global symbol
resolution, HLE fallback, guest-status conversion or SSL routing.

## Session lifetime and enablement

Keep exactly one `State` as session-lifetime loader state, alive across Program
unloads. It is noncopyable/nonmovable and has no reset or reenable API. Its
constructor consumes an environment **value supplied by the caller**; only the
exact string `"1"` enables it, with empty/default and every other value off.
The helper does not choose an environment variable name or read the environment.
Disabled binding/preparation/completion never arm or invoke a callback.

`RecordStartupAdmission` maintains sticky session history, including while off.
Call it under the existing loader mutex at every initializer/application-entry
admission, not only when a surviving Program's `started` flag is set. An earlier
admission permanently makes fresh binding/preparation late, even if that Program
has since been unloaded. Bind then admit then prepare is also rejected. No
history is reset by provider invalidation.

`Bind(owner, entry, profile_verified)` requires a nonnegative Program `unique_id`,
a non-null typed callback, and caller-provided verification. The unique_id is
an ownership key, **not a process identity/PID**. A successful selection cannot
be replaced by another owner or callback, nor rebound after completion/unload.
Validation failures do not arm a selection; correction is possible only while
still fresh and before any startup admission.

## External serialization and call handoff

There is no own mutex. All State operations and queries must run under existing
`RuntimeLinker::m_mutex` (or equivalent exclusive serialization). Integration:

1. After primary HLE pthread setup, selected-provider load/relocation and game
   patching, **before StartAllModules**, verify and bind the exact selected provider.
2. Under the loader mutex, `Prepare(call)` transitions Bound -> Invoking and
   hands the typed endpoint to one noncopyable/nonmovable `PreparedCall`.
3. Release the loader mutex. `call.Invoke()` consumes its endpoint before the
   actual native call, invokes it with no arguments, then records normal return.
   It never interprets RAX and never catches/translates a native fault/exception.
4. Reacquire the loader mutex. `Complete(call)` requires the exact prepared
   ticket and proof of return, then publishes Completed. Only now proceed to
   initializer/application-entry admission.

Keep the State and ticket alive through completion. A dropped ticket or abnormal
return deliberately leaves the state Invoking/fail-closed; there is no automatic
retry, destructor completion, timeout or recovery write. Invoke is a single-owner
operation; it does not grant concurrent ticket access. Reentry through Prepare,
Bind, startup admission or provider invalidation returns internal `Invoking`.
Reentering the same ticket returns `AlreadyInvoked`; completion from inside the
callback returns `NotReturned`.

`RecordStartupAdmission` records admissions; outside Invoking it is not itself a
policy that requires bridge completion. This allows honest recording of startup
that preceded this seam. Integration must separately require successful bridge
completion when its enabled primary-startup policy requires the bridge, rather
than treating a history-record operation as permission to bypass it.

## Unload contract

Under the same loader mutex, call `InvalidateProvider(owner)` **before unmapping**
the selected provider. On Ok it clears the cached endpoint, retains owner,
admission and completion history, and permanently transitions to Invalidated.
Unrelated owner unloads return `UnrelatedProvider` without changing selection.

If a ticket is in flight (including prepared but not yet invoked), invalidation
returns `Invoking`: **unmap must not proceed**. The caller must fail closed/defer
unload admission and retry after completion. This helper does not implement a
native EBUSY result, a guest pin/refcount, or an automatic wait. The existing
loader must honor this internal result so the handed-off code remains mapped
while the mutex is released. No new lock is held across guest initializers.

## Loader integration

`KYTY_NATIVE_LIBC_STARTUP=1` is captured once by the RuntimeLinker constructor.
The launcher exposes `-NativeLibcStartup` and clears inherited enablement when
the switch is absent. The state lives across `Clear` and provider unloads.

Before provider publication, the loader requires the canonical filename,
SHA256 of the same open ELF backing (bounded to4MiB), All exports, unique raw
startup/Free definitions and raw library/module IDs and versions. The checks
include exact RVA/size/type/binding, mapped executable extents, writable leaf
data, the provider-local resolved entries, the98-byte leaf, and the complete
eight-site allocator fatal profile with its exact post-adaptation bytes. Raw definitions are
counted before dictionary normalization; duplicate and alias candidates fail.
Native libkernel export providers are rejected while the bridge is enabled,
preventing the supported native primary-startup route from replaying the leaf.

Primary `Execute` establishes the HLE pthread/stack, loads and relocates providers,
and applies game patches. It then revalidates the selected provider, prepares
under the loader mutex, invokes outside it, and completes under the mutex before
`StartAllModules`. A second primary Execute is rejected. Child/cohost paths never
invoke the leaf. Initializer, finalizer and application-entry admissions require
Completed when enabled; their history is sticky. Module starts publish `started`
under the admission lock, which ends before their guest callback.

Load/publication, preload discovery, relocation, unload and Clear share the
loader mutex and reject mutation during Invoking. Unload/Clear invalidate the
cached endpoint before unmapping; later replacement cannot rearm it. Violations
fail the opt-in launch explicitly rather than inventing a native error code or
reporting a deferred module start as successful. Existing module-execution
quiescence requirements for ordinary unload remain the loader's responsibility.

The new short admission sections use the existing host-lock critical guard.
No new guard spans guest initializers or the native call. StartAllModules still
has its pre-existing outer recursive loader lock; this change does not establish
shared-initializer ownership or complete constructor success.

All `Result` values are internal integration outcomes, never guest errno/status.
`Disabled` is the ordinary opt-out path; other non-Ok outcomes must be handled
explicitly at the call site without synthesizing native success or retrying the
leaf after a completed invocation.

## Fixtures

Source globbing includes the helper; CTest `native_libc_startup` dispatches
`TestNativeLibcStartup`. The fixture is host-only and needs no firmware/provider:
it checks exact enablement, synthetic digest bounds/all-byte mismatch/truncation, non-null
endpoint and verification requirements, single typed invocation/order,
post-return completion, diagnostic sentinel preservation on repeat, callback
reentry, earlier admission history, unload invalidation and replacement rejection.
It also exercises externally serialized competing admission/unload, raw export
ambiguity/version/alias rejection, and bounded SHA256 verification on a synthetic
ELF. It makes no unsynchronized-concurrent-helper claim and never writes native
globals. These checks do not prove full native allocator dependency readiness;
the unresolved imports and native SSL allocation/statistics work remain separate.

## Validation result

2026-11-10 release-content checkpoint: the firmware-derived leaf and first-boot
instruction arrays were replaced with SHA-256 digests. An isolated standalone
clang-cl smoke test using an existing FFmpeg avutil 59 DLL matched the selected
retail firmware leaf, rejected each of 98 single-byte mutations and all truncated
spans, checked bounds, and exercised the one-shot state lifecycle. This is not a
full emulator build or a live boot. Seven retained-input launcher/first-boot
Python tests passed for both known 13.60 profiles in an external test directory;
the first invocation using unittest module discovery failed because its tool
imports require running the test script directly. Source checkouts remained
clean during both runs. Public certificates, shorter signatures, shader and
asset provenance, and fresh-user runtime prerequisites remain open.

The Windows emulator/fixture build passed. CTests `native_libc_startup`,
`guest_fatal_trap`, `rng_sysctl`, `process_sdk`, and `authority_metadata` passed.
The first fixture run exposed a test-name offset pointing at its separating NUL;
using the literal's `sizeof` corrected the alternate-library duplicate test.
An offline read independently matched the embedded image digest and all98 leaf
bytes against the local read-only image.

Bounded comparison used one executable, SHA256
`e459090fa0c765137803db334fffb37ad59395c7d9bdff2f834e60db59a045e7`:

- `shinelibcstartupoff1`, PID18332: no bridge binding/invocation log; two samples
  observed gate0. Process exited321 after launcher elapsed4.29s.
- `shinelibcstartupon1`, PID14364: provider owner24 at `0xa70000000` passed
  registration/identity checks. One completion log precedes the first module
  initializer. One sample observed gate1, null auxiliary roots and native
  diagnostic state including `-1` at+1b7ce8. Process exited321 after3.75s.

Both logs report the previously observed primary `ShellCore+f4ff8b` fault,
followed by Mono crash reporting and an unhandled UD2 at `libmonosgen+30a4f0`.
This demonstrates the native transition in the enabled run, not sustained
allocator readiness or a regression-free boot. Neither run lasted until the
scheduled screenshot; no new visual success is claimed. Both observed32 occupied
SSL slots but ended before the previously tracked SaveData caller was captured.
Independent integration review and longer runtime validation are recorded below.

An independent static integration review found no concrete correctness/regression
issues in state lifetime, invocation/admission serialization, provider validation,
or default-off handling. It did not execute RuntimeLinker concurrency tests or
claim to resolve existing initializer locking/unload quiescence.

With bounded printf diagnostics added, Silent visible runs `shinelibcassert1`
and `shinelibcvisual2` survived their 30s and 60s polling thresholds respectively.
The latter stopped at 62.98s; all inspected 8s/20s/40s window captures remained
black with thin colored top lines. Neither animation nor setup/home input works.
The earlier intermittent PSM graphics-context assertion remains unresolved.
