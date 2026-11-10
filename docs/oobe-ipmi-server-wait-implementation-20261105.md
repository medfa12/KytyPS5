# IPMI pre-registration waiting prerequisite — 2026-11-05

## Implemented

`Core::WaitServer` reserves a client's connect admission and retains a readiness
ticket even when no server exists. Server publication and waiter enrollment use
the same Core mutex, preventing lost notification. Readiness identifies one
non-reused server handle; it has neither an operation nor method acceptance result.
`Connect` consumes the exact caller/ticket/generation only after the adapter's
authorization and descriptor checks. Its separate acceptance ticket remains
pending until the receiver creates a session and explicitly accepts/rejects it.

Factories can register a provisional server, finish native object/output copyout,
and then call `PublishServer`. Failed copyout rolls back without publishing or
waking clients. Both immediate lookup and waiting exclude provisional servers.
Sealed servers cannot acquire new clients. A same-name replacement does not
inherit a prior generation's authorization; stale readiness fails revalidation.

Cancellation before connect consumes readiness releases the reservation. Client
destruction cancels an uncompleted name wait; a host Core teardown completes it
with Unsupported. An observed readiness result cannot itself attach a client
after destruction: `Connection` and `Connect` revalidate the live handle.

The shared NP/LoginMgr adapter now exposes native client slot `+0x80`. A **null**
timeout waits through service publication and actual acceptance on the native
dispatcher thread. A pointer to zero uses the existing immediate-lookup connect
path. A positive finite timeout returns Unsupported without changing output,
rather than dropping a deadline whose post-dispatch cancellation is not yet
implemented. This checkpoint does not claim full finite connect-timeout support
or cancellation of an already-dispatched accepting callback.

The wait primitive is a host-side prerequisite, not a new guest syscall or a
fabricated initialization event. No service registration, authorization or
acceptance is synthesized by it.

## Validation

Clean standalone configure/build in `_Build/oobe-ipmi-wait` using clang-cl23,
VS18 x64 environment, Ninja, Release. All **12 CTest checks passed**, including
the existing ten Core/wire/gateway/NP/LoginMgr/retirement checks.

New `ipmi_server_wait_tests` covers absent and unrelated services, provisional
publication/rollback, real acceptance separation, ownership, duplicate waits,
sealed/stale/replaced server generations, cancellation/destruction, host teardown
and 40 actual concurrent publication/enrollment races.

New `ipmi_server_wait_transport_tests` exercises the production LoginMgr vtable
through SysV function pointers with independent host callback hooks. It proves
client-before-server waiting, reentrant factory-copyout exclusion, rollback and
retry, rejection, authorization failure, genuine callback-held acceptance,
dispatcher stop refusal while connected, disconnect, stop and actual worker join.
It also verifies output canaries and the explicit finite-timeout Unsupported path.

Both production adapter translation units are compiled by this standalone build.
CTest logs: `_Build/oobe-ipmi-wait/Testing/Temporary/LastTest.log`.

## Remaining integration

This is not an OOBE or genuine PSN end-to-end result. The exact AppDb route,
production service callback/identity/credential hooks and service worker ownership
remain required. The shared callback fixture is not execution of firmware.
The firmware kernel syscall gateway still has its previous connect path; no
claim is made that selector `0x400` is now fully implemented through that gateway.
