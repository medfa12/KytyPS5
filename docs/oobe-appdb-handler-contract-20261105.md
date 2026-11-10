# AppDb native handler, authorization and lifetime — 2026-11-05

Follow-up to `oobe-appbrowse-native-wait-contract-20261105.md`. Addresses below
are relative to the original 13.60 ShellCore ELF. This is source evidence, not
an OOBE runtime pass.

## Handler ABI

Constructor `+0xab2290` installs vtable `+0x2038668` and initializes a native
session-to-backing map. Slots:

| Offset | Target | Contract |
| --- | --- | --- |
| `0`, `8` | `ab22e0`, `ab2330` | destructor, deleting destructor |
| `10` | `ab2380` | sync `(handler, session, method, DataInfo*, count, BufferInfo*, count)` |
| `18` | `ab2450` | async `(handler, session, ticket, method, DataInfo*, count)` |
| `20` | `ab2510` | connect `(handler, server, negotiated-config, argument, size)` |
| `28` | `ab25f0` | disconnect `(handler, session)` |
| `30` | `ab26c0` | forced disconnect; jumps to ordinary disconnect |

Connect allocates the negotiated session storage size at config `+0x148`, then
calls server vtable `+0x28` with `(server, &session, nullptr, backing)`. Allocation
failure returns `0x80f50002`; session creation failure frees backing and returns
the creation error. Success inserts session/backing into the handler map.
Disconnect calls session `+0x10`, finds the session in that map, frees its backing
and erases/frees its map node. A successful connection therefore retains both
the native handler and session backing through actual disconnect completion.

## Request authorization

Both dispatch callbacks obtain the actual session peer through session `+0x18`.
They require helper `+0xb3ef70` to return exactly `1`, then require either helper
`+0xb3fb20` to return exactly `1` or the peer PID to equal `getpid()`.

- `b3ef70` zeroes a `0x88`-byte auth buffer and calls `get_authinfo(peer, buffer)`.
  A successful call returns bit 62 of the first capability word at buffer `+8`.
  A nonzero syscall result forces a negative return, not authorization.
- `b3fb20` calls the same provider and compares authid at buffer `+0` against
  `0x480000000000000f`. A provider failure cannot return `1`.
- Rejection sends `0x80f50013` using session `+0x30` (sync) or `+0x38` (async).
  Same-process PID collapse and compatibility capabilities must not be presented
  as proof of this native authorization contract.
- These checks apply to requests; the connect callback itself does not perform
  them. Connect acceptance and authorized method completion are separate gates.

This links AppDb method execution to the same authentic credential provenance
being audited for LoginMgr. No role/name-derived capability is introduced here.

## Method dispatch and replies

Sync dispatcher `+0xab2900` handles two methods:

- `0x12340000`: takes a 32-bit value from the first input, calls `+0xab1ff0`
  (ultimately `+0xab1870`) with the first output data pointer. If output is
  nonnull it sets that descriptor's written length at `+0x10` to `strlen+1`;
  otherwise written length is zero. It explicitly replies via session `+0x30`.
- `0x12340001`: takes a 64-bit value from the first input, calls `+0xab2080`
  (ultimately `+0xab1d80`), and explicitly replies through the same session slot.
- Unknown method returns `0x800205c1` without the success/reply path. Callback
  return must not be promoted into a synthetic successful method reply.

Async dispatcher `+0xab29a0` recognizes `0x12340002` and transfers to `+0xab2100`.
That target in this firmware is literally `xor eax,eax; ret`, with no reply.
Unknown async methods return `0x800205c1`. Returning from this callback does not
prove async reply publication or ticket consumption.

## Dispatcher ownership

The producer creates/clears the named event before starting worker `+0xab1290`.
The worker constructs its handler on its own stack, creates the server, allocates
working memory and calls server `+0x10`. Dispatcher return frees working memory
and destroys the stack handler. Thus retaining only the host server object cannot
make an early dispatcher return safe.

Stop path `+0xab1500` calls server `+0x20`, then joins the real worker at
`+0xab1546`, then destroys the server at `+0xab156d` and releases server storage.
An adapter needs the genuine worker's blocking dispatch, explicit stop/drain,
actual thread join and only then backing release.

## Implementation handoff

The existing isolated NP/LoginMgr adapter has compatible handler argument layouts,
explicit session replies, dispatcher loop and session map ownership, but lacks
pre-registration server waiting and client slot `+0x80`. Its connect lookup must
also exclude provisional server factory state until copyout commits. These are
required before routing AppDb through that machinery. Production callback hooks,
identity/credential provider and backing lifetime remain separate runtime seams.

Retained disassembly:

- `_TempData/oobe-appdb-handler-{ctor,methods}.txt`
- `_TempData/oobe-appdb-auth-dispatch.txt`
- `_TempData/oobe-appdb-method-producers.txt`
- `_TempData/oobe-appdb-server-entry.txt`
