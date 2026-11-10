# Network instance configuration and pool IDs

## Audited contract

The native audit located the `6MojQ8uFHEI` setter at `N+290` and ShellCore's
explicit call at `Ca7bc6f`, supplying 32 bytes with first u32 equal to 1.
The old HLE pointer-returning getter was the wrong ABI.

The final edge audit confirmed decimal 3000 through `N+a161` (`add r12d,0xbb8`)
and `N+a190` (`cmp edi,0xbb8`). Production uses one `POOL_ID_BASE` constant.
Setter order at N+290 is readiness, already-set, null, then 32-byte load/copy.
Pool creation does not latch configuration; a first setter after pool creation
can expand the quota while retaining existing IDs and occupancy.

The exported wrapper is now
`int KYTY_SYSV_ABI Libs::LibNet::SceNetInitParam(const void* param)`.
It calls the header-declared
`int KYTY_SYSV_ABI Libs::Network::Net::NetSetInitParam(const void* param)`
and returns `FinishNetCall`'s actual status. The wrapper captures the actual
caller and invokes the `NetPoolConfigured` observer after the
setter returns and releases its lock. There is no length argument:
the input size is exactly 32 bytes, copied into instance-owned storage.

- An absent Network instance returns `0x804101c8` (ENOTINIT), before input checks.
- Any subsequent call after success returns `0x80410110` (EBUSY), including
  null or unreadable pointers,
  without reading/copying the new input or changing configuration.
- Ready but unconfigured null input returns `0x80410116` (EINVAL), without latching.
- The first successful call retains all 32 bytes without modifying the input.
  First-u32 bit0 selects 128 slots; otherwise the limit is 32. Remaining
  bits/bytes are retained without additional validation or invented meaning.
  An all-zero 32-byte input also latches successfully in 32-slot mode.
- Wrapper failures set thread-local net errno relative to `0x80410100`
  (`ENOTINIT=c8`, `EBUSY=10`, `EINVAL=16`);
  successful configuration returns zero and preserves the previous net errno.
- Unconfigured instances use 32 slots. Only a successful bit0-set call expands
  that instance; pool exhaustion itself never changes the quota.
- Configuration and pool allocation share the existing mutex. Public NetInit
  and NetTerm do not reset pools or configuration. Existing NetInit host socket
  backend initialization remains; instance lifetime belongs to Network
  Initialize/Shutdown.

The table has 128 entries. Allocation scans configured slots from zero and
returns decimal `3000 + slot` (default 3000..3031, extended 3000..3127).
Destroy immediately releases a slot for reuse, without a generation number.
PoolDestroy, ResolverCreate and HttpInit check the public range before
subtracting 3000, then require a live entry. Raw old indices are rejected.
Existing error mappings remain: invalid destroy is EBADF, invalid resolver
pool is RESOLVER_ENOSPACE, invalid HTTP pool is HTTP_OUT_OF_MEMORY.

Public pool creation checks instance readiness first, then rejects null names,
size below `0x1000`, and nonzero flags
with EINVAL before allocation/quota inspection. Full configured tables return
`0x80410117` (ENFILE).

## Scope and remaining gaps

`g_net` is still **one shared cohost Network instance**, not a native-process
registry or per-process library instance. This correction does not establish
process identity, credentials, capability checks, or per-process pool isolation.

Pools retain the existing host name/size/used metadata representation. Native
backing allocations, mappings, allocator behavior and busy-reference accounting
are not implemented by this change. Resolver/HTTP ownership and destruction
behavior remain the existing HLE behavior. In particular, destroying a pool
does not add a native busy check or tear down dependent contexts.

The audited native name-length error for names of at least 32 bytes is
`0x8041013f`, but its mapping-before-quota ordering requires coherent allocator
support. No new name-length rejection is imposed here. Names remain host
strings; observer names are separately bounded to 32 bytes. Non-null unreadable
setter inputs still use the existing direct guest-pointer model; no fault-to-
status translation is invented. Native N+316 loads the user input before
latching; the HLE likewise copies under the mutex before setting its latch.

Runtime evidence motivating the correction: `shinenetpool1`, PID 20680,
`ipmi-observe-20680-15528101885237.json`, reported 44 creates, 31 occupied
entries, two destroys and zero Recovery drops. Caller `Cada231` returned ENFILE
for `NpSyndySyncLogicPool`, size `0x4000`, flags zero. This is evidence of the
old quota failure, not validation of the corrected implementation or recovery
animation success.

The corrected `shinenetconfig1` run (PID 19808, executable SHA256
`0595aa0762e207d0cd83c6ea1bda10f275e6c2b84ffc0792acd768f4a6fca459`)
records configuration success at return PC `C+a7bc74` (call `C+a7bc6f`),
with the expected 32-byte input. `C+ada231` then returns pool ID 3031 for
`NpSyndySyncLogicPool`; later allocations use extended IDs 3032 and 3033.
All 53 observed creates succeed. ShellUI's later configuration returns EBUSY,
consistent with the documented shared-instance limitation.

This removes the observed ENFILE failure, not the remaining startup blockers:
the 35-second snapshot still shows SaveData failure cleanup, with `0x8095f008`
in the previously audited saved-error slot. Recovery still receives zero bytes;
the inspected 20/55-second screens remain black with thin colored top lines.

## Bounded observation

The snapshot container uses `IpmiObserve::NetPoolOccupiedLimit`, copying
`min(configured_limit, container.size())` occupied entries under the pool mutex
with public IDs and publishing after unlocking. The old 31-slot static assertion
is removed. Observer capacity is 128 and publication
skips zero-ID tail entries, so default mode emits 32 occupied entries and
extended mode emits 128. The copy remains bounded if capacity changes again.

The budget is 96 routine + 128 occupied + 1 reserved failure =
225, leaving 287 Recovery slots before other traffic. Configuration Event27
operation3 shares the routine budget. The setter wrapper now calls the provided
`NetPoolConfigured(caller, param, result)` API outside the setter lock. Its
checked read observes caller memory at wrapper return time, not a locked copy
of retained configuration. Publication can interleave with other threads after
unlocking; timestamps do not establish exact pool-operation ordering. Other
Recovery producers can still exhaust the shared bank.

## Dedicated fixture

`tests/NetPoolConfigTests.inc` defines `TestNetPoolConfig`, integrated
it in `--net-pool-config-only` / CTest `net_pool_config`, after InitSubsystems
and before InitAll or any other Network owner. It requires `libs/network.h`,
`libs/errno.h`, `loader/symbolDatabase.h` and the global declaration
`namespace Libs { void InitNet_1(Loader::SymbolDatabase*); }`.

RAII scopes own fresh Network instances. The fixture checks production exact
and NID-fallback bindings through typed SysV function pointers, default quota,
live expansion, input/canary preservation, caller-storage mutation independence,
null/busy precedence, zero/other-bit configurations, Init/Term persistence,
readiness and thread-local errno preservation, immediate slot reuse, invalid
create arguments and public-ID resolver/HTTP
consumption. All created contexts and pools are explicitly cleaned up before
instance shutdown. NetTerm is called directly because this registration table
does not expose a NetTerm NID. Opaque retained configuration bytes have no public
getter: their full copy is statically inspectable, while the fixture observes
input preservation and the resulting quota through the public ABI.

The combined Windows emulator/fixture build passed. CTest `net_pool_config`
passed after correcting its EBUSY errno expectation to `0x10`; `ipmi_observe`
also passed. Independent bounded contract review found no blocking defect.
Genuine recovery `0x160008` replies and animated ShellUI output require separate
runtime evidence.
