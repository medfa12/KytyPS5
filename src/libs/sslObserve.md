# Bounded SSL context observation (Event28)

## Question and evidence boundary

After pool correction `b09547f`, `shinenetconfig1` (PID 19808) showed successful
Network configuration and Syndy's pool ID 3031. The saved SaveData error became
`0x8095f008`, SCE_SSL_ERROR_OUT_OF_SIZE. The native audit identified a candidate
call at `C+ada241`, `SslInit(0x80000)`, returning to `C+ada246`. It also reported
native dynamically linked SSL contexts rather than the HLE's fixed 32-slot table.

The capture below confirms that caller receives the error while all 32 HLE
slots are occupied. This observation does not establish a leak: it records
existing calls/state and leaves capacity, IDs, allocation and cleanup intact.

## Confirmed runtime

`shinesslobserve1`, PID 17952, executable SHA256
`d6f1004e301abe49ec99caf585ded8f1fa3a310bacccb05bd5af24e64e421ac0`,
ran to its 30-second limit (launcher elapsed 31.94s). Decoded artifact
`ipmi-observe-17952-15545194608558.json` contains 47 init records, one term
and all 32 occupied entries, with zero Recovery drops. Routine usage is below
the 64-record cap.

The first 32 successful inits are from ShellCore, allocating IDs
`0x20000..0x2001f`. The full-table snapshot at 1.49549s matches those IDs/sizes;
the first rejected caller is `C+13d3edd`. At 1.895625s, the target caller
`C+ada246` requests `0x80000` and receives `0x8095f008`. No successful term
precedes it. The only observed term is ShellUI's invalid `-1` handle, returning
`0x8095f006`; later ShellUI inits also fail. Thus the HLE quota failure and its
SaveData producer are directly observed, without assuming a missing release.

The inspected screenshot remains black with colored top lines; preparation and
recovery owners remain zero. Native SSL allocation/lifetime support and visible
startup progress remain separate work.

## Capture points and lock consistency

`Libs::Network::Ssl::SslInit` and `SslTerm` are directly registered exports.
`KYTY_IPMI_CALLER` is captured in those functions, before calling the internal
Network methods, so it records the actual caller return address rather than an
extra HLE wrapper frame. The init record contains the original uint64 size and
actual returned ID/error; the term record contains the original public ID and
actual result. Existing fatal checks and status mappings remain in place.

The first observed full table is selected inside `Network::SslInit` under the
same `m_mutex` used by SSL init/term. A fixed stack array copies all 32 entries:
public `Id::Create(slot, Id::Type::Ssl).ToInt()` and the full uint64 stored size.
No observer publication, guest-memory reads, allocation, logging or callbacks
occur in the locked snapshot block. Both init and term mutex scopes have short
`HostLockCriticalGuard`s, destroyed after the mutex guard releases ownership.
Snapshot emission occurs after both guards have left scope. Selection order is
not publication/completion order; other threads' records may interleave.

Selection is once per host process, including across Network recreation. The
observer separately admits at most one snapshot. A thread-local reservation
belongs to that snapshot producer's following `SslInitialized` call; a concurrent
failed init or term cannot take it. Occupied records have caller zero and share
the failed init's host thread ID. This is a mutex-consistent snapshot at failure
selection time, even if another thread changes the table before publication.

## API and record layout

Declared in `libs/ipmiObserve.h`:

- `SslInitialized(uint64_t caller, uint64_t size, int32_t result)`
- `SslTerminated(uint64_t caller, int32_t id, int32_t result)`
- `SslFullSnapshot(const std::array<SslEntry, SslOccupiedLimit>&)`
- `SslEntry { int32_t id; uint64_t size; }`

All hooks are `noexcept`. Producers use fixed records, atomic reservations and
the existing lock-free slot publication; they do not allocate or inspect guest
memory. No file-format version/record-size change is required.

Event28 (`SslContext`, decoder `ssl_context`) uses Recovery and these values:

| Index | Meaning |
| --- | --- |
| 0 | Caller return address; zero for occupied |
| 1 | Operation: 0 init, 1 term, 2 occupied |
| 2 | Public ID as uint32; failed init uses `UINT32_MAX` |
| 3 | Actual return bits as uint32; zero for occupied |
| 4 | Full unsigned64 init/occupied size; zero for term |

Remaining values, name and preview are zero. The decoder reports `caller`,
`operation`, signed32 `ssl_id`, hexadecimal `result` for init/term, and unsigned64
`size` for init/occupied. Size is never passed through Event27's signed32 handling.

## Budgets and validation handoff

SSL has 64 shared routine init/term records, 32 occupied records and one reserved
failed init: **97 maximum**. Routine churn cannot consume snapshot/failure
allowances. Combined with NetPool's 225-record ceiling, the two sources use at
most **322 of 512 Recovery slots**, leaving **190 before other traffic**. These
are producer caps, not exclusive bank reservations: other Recovery producers
can still exhaust the bank and cause drops. Inspect dropped counts and committed
slots when interpreting a capture.

`tests/IpmiObserveTests.cpp` adds disabled-hook checks, full-width caller/size
and status forwarding, successful/failed term metadata, concurrent routine cap
enforcement, snapshot immutability, TLS pairing across competing failures,
one-shot snapshot limits, actual combined pool/SSL record counts, and a following
recovery invocation. The mapped fixture checks that its existing traffic plus
both full budgets and the reply fit the bank before adding SSL records.

Independent bounded regression/memory-safety review found no blocking defect.
The combined Windows emulator/memory-fixture/observer build passed, followed by
`ipmi_observe` (0.35s) and `net_pool_config` (0.11s). Decoder syntax and whitespace
checks passed.
Fixtures cover observer metadata and budgets; actual table exhaustion, guest-PC
capture and export-return preservation require runtime evidence. The reservation
is valid for the current callback-free production path, not a general reentrant
transaction. Genuine recovery replies and animated ShellUI remain the overall
success criteria.
