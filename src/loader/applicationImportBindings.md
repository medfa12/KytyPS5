# Shared-owner import call integration

`RelocateRecord` installs importer-owned `DeferredImportThunk` entries for
shared-owner global/weak `STT_FUNC` imports with supported relocation ABI. Both
resolved and missing functions take this route; no shared missing function uses
the legacy zero-return thunk. Nonfunction/ambiguous deferred relocations remain
admission blockers.

Each immutable entry keeps the raw ELF import spelling and slot identity. Its
resolver uses that exact importer and the real loader's owner-filtered symbol
lookup. The first invocation is tail-transferred with its original SysV
registers/stack; resolution does not discard a call or fabricate an API result.

Private native providers remain guest-to-guest calls. Returning HLE requires an
explicit NID/registration-expression match and gets an owner-bound
`HostCallReturnThunk`. `applicationReturningExports.h` contains the source-audited
registration pairs for module lifecycle, selected kernel/VM/thread/libc services
and scoped pthread-exit adapters. A registered address alone is not proof of a
safe returning ABI. Native session providers and unaudited HLE are
blocked. Arbitrary exits, longjmp/fibers, signals, unreviewed callback-heavy
services and unreviewed exports are not implicitly classified by this list.

The resolver validates actual-thread boundary/owner identity, acquires work
admission, finishes all host RAII, and returns either the current target or the
matching innermost failure gateway. Missing/unsupported calls latch an
owner-local failure. A mismatched active boundary is an invariant trap rather
than permission to skip another owner's frames. Cached targets still pass the
owner-stop check on every invocation. GOT slots retain immutable entry addresses;
rebinding invalidates caches and keeps prior gateways pinned through quiescence.

`GetApplicationImportBlockers` reports transition/ABI/provider constraints in
addition to unresolved symbols. The VSH gate remains closed until the required
host callbacks, exit/cancellation and owner-wide stopping are covered. This
integration does not itself stop a pure guest loop or cancel a blocked HLE wait.
Private module unload retains mappings: copied native pointers cannot be revoked
by changing only relocation slots. Owner-safe reclamation is still external.

`application_import_bindings` exercises real relocation writes, generated import
entries, late private-provider resolution on the first call, eight-argument
register/stack preservation, two-owner symbol isolation, actual registered HLE
return gateways, unresolved/unaudited refusal, cached-target stopping and clean
transition accounting. The registered pthread-exit import is exercised through
the real worker/coordinator join path. Registered scalar math and printf imports
exercise XMM arguments/results and mixed varargs/overflow arguments through the
production resolver and return gateway. The initial import/exit/allocator slice
built and passed its focused Windows checks; see the validation report below.

Application heap callbacks use a typed allocator transition inside a nested
owner boundary. Host caller cleanup finishes before a failure propagates through
its outer HLE return gateway. Allocator registration is serialized with owner
stop, and stopped owners cannot publish or invoke a replacement allocator.
`runtime_guest_execution` covers malloc/memalign argument/result preservation,
nested allocator failure and host cleanup, and stop-time publication refusal.

## Dynamic lookup

Shared-owner `KernelDlsym` now uses `RuntimeLinker::ResolveApplicationDlsym`.
The explicit module must be visible to that owner; a foreign private handle is
refused. Typed function results are immutable resolved-symbol snapshots in the
same importer-owned entry store. Copied pointers therefore keep the per-call
owner/stop check and the reviewed HLE return gateway. Repeated lookup of an
identical target reuses its entry. Private native data keeps its original address;
ambiguous, session-native and unaudited HLE targets return an ordinary lookup
error without poisoning an optional-probing application's owner. Publication is
serialized with stop. Session/direct-app lookup retains its existing path.

This route does not add raw `scriptingGetMem`, RNPS diagnostic interposers, shared
HLE data pointers or unaudited native-SSL special-case pointers. The new dynamic
lookup fixture covers private/native/HLE dispatch, object addresses, exact name
versus encoded lookup, foreign handles, unsupported probes and stopped pointers.
It also calls the registered `KernelDlsym` through a real relocated import and
invokes the returned native function within the same guest segment.
Windows execution validation passed in
`_TempData/vsh-dlsym-end-to-end-tests.log` after the matching VM test build.

## Windows validation before dynamic lookup

`_TempData/vsh-launch-close-verified-build.log` built the emulator and affected
executables. Fifteen of sixteen selected checks passed in
`vsh-launch-close-verified-tests.log`; the legacy scope fixture then gained live
execution admission for its allocator owners and passed in
`vsh-scope-fixture-tests.log`. The production allocator policy was retained.
The import fixture also exposed/fixed a real Windows issue: generated deferred
entries must use host `VirtualAlloc`, not the guest-address allocator after
runtime memory reserves those ranges. The standalone thunk check and production
import check both passed with that correction.

This establishes the exercised host integration, not visible title startup or
whole-owner preemption. Shared launch admission remains guarded.
