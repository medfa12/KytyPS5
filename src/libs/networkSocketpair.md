# Bounded libkernel socketpair

`MZb0GKT3mo8` resolves through `ShellCoreFloor::SocketpairObserved` in both
registration paths. Its optional provenance scope captures the raw caller and
output before/after exactly one `Network::Net::Socketpair` call. No KStub is
invoked. The signature is `int (int domain, int type, int protocol, int* pair)`;
return is POSIX zero or -1 with positive guest errno. Success preserves errno.

## Implemented request and validation

* `(1, 1, 0)` creates two real connected AF_UNIX STREAM endpoints, initially
  blocking. It publishes two distinct Net table descriptors as two int32 values.
* Strip only type bits `0x30000000`. `0x10000000` is ignored and both CLOEXEC
  metadata flags start clear, matching the audited 13.60 socketpair quirk.
  `0x20000000` sets real host nonblocking mode on both endpoints before publication.
* For domain 1, nonzero protocol or stripped type zero gives EPROTONOSUPPORT=43;
  unsupported nonzero types give EPROTOTYPE=41, except native-valid DGRAM=2 and
  SEQPACKET=5, which give an explicit implementation-limit EOPNOTSUPP=45.
* Other positive domains through 255 give implementation-limit EOPNOTSUPP;
  domain <=0 or >255 gives EAFNOSUPPORT=47. No full native domain registry or
  cross-family validation-order parity is claimed by that bounded rejection.
* Transport/resource setup and descriptor availability precede output validation.
  C++ allocation failure gives ENOMEM; host failures use the existing BSD errno
  mapping. Missing safe-copy support gives EOPNOTSUPP before construction.

The static native audit is supplied separately as `socketpair-native-stage/
NATIVE-CONTRACT.md` (13.60 syscall 135 handler `ffffffff80741720`, libkernel
export RVA d00, SysCore call 49d9). These are audited semantics, not runtime proof
of this implementation.

## Host construction and ownership

Windows creates an exclusive directory under the host TEMP root with a 128-bit
BCrypt random suffix (at most eight collisions), then binds a real AF_UNIX
listener at its private `s` pathname. It uses nonblocking connect/accept with
backlog one and a shared two-second readiness deadline, then restores the final
blocking mode. There is no TCP or user-buffer fallback. Directory ACLs inherit
from the per-user TEMP root; this is not a new cross-user authentication service.
Paths too long for `sockaddr_un` are rejected, not truncated. ANSI path encoding
and host TEMP access errors are explicit host limitations.

RAII owns the listener, pathname/directory and both endpoints throughout setup.
On setup failure connections close before listener/path cleanup. Before success
the listener must close and its endpoint/directory must be removed; cleanup
failure returns EIO, with another cleanup attempt during unwind. No endless
retry is used. Windows pathname deletion while accepted endpoints remain open
is a required platform verification gate. Persistent OS cleanup failures cannot
be guaranteed away; resource counters retain evidence instead of declaring a
balanced fixture. The two-second bound covers readiness waits, not OS filesystem
or provider call scheduling. Fault-safe copyout may fault pages in as well.

Linux uses native `socketpair(AF_UNIX, SOCK_STREAM, 0, ...)`. Hosts without the
implemented safe-copy mechanism return EOPNOTSUPP. Winsock startup is the existing
Net backend's lifecycle, not a new pair-specific startup/teardown scheme.

All shared metadata is allocated before publication. The existing `g_socket_mutex`
protects selection of both free slots, both installs, eight-byte copyout and both
rollback removals. A `HostLockCriticalGuard` precedes each added table lock and
outlives its unlock. Retained owners ensure rollback destroys no host transport
under the lock. There is no unlock-then-close-by-fd rollback. No listener, loader
lookup or GPU operation runs in the publication transaction.

Windows resolves NtWriteVirtualMemory before locking, rejects guard/nonwritable
ranges inside the final copyout step, then uses the permission-respecting system
copy. Linux uses `process_vm_writev` on itself. Neither requires Memory subsystem
initialization. Copyout failure gives EFAULT and retires both endpoints/slots.
Cross-page partial writes or concurrent protect/unmap do not promise restored
output bytes; pre-copy failures and wholly read-only outputs stay unchanged.

`kernel/guestDescriptorRange.h` reserves the existing Net range `[128,1024)`.
Filesystem allocation skips it under its own existing lock, then continues at
1024 and beyond. No file-count cap or cross-table check-then-act is introduced.
Net Socket/Accept/pair use the same canonical bounds. This is an HLE allocation
policy, not native fd-number fidelity; restart before applying to a running guest.

## I/O, flags and limits

Send, receive, select and shutdown retain endpoint ownership across host calls.
Ordinary real buffered bytes drain before EOF after peer close/write shutdown.
The pair state records successful host close/shutdown, allowing EPIPE after
actual peer close, peer read shutdown or local write shutdown. Windows host send
errors SHUTDOWN/CONNRESET are also mapped to EPIPE for these pairs. No successful
byte count or synthetic receive data is manufactured. An in-flight operation
can retain a closed descriptor's transport until it completes; comprehensive
concurrent-close cancellation semantics are not implemented.

Pair fcntl GETFL/SETFL reads/changes endpoint-owned nonblocking mode; GETFD/SETFD
uses slot-owned CLOEXEC metadata. Unsupported SETFL/SETFD bits and other pair
commands, including duplication, fail EOPNOTSUPP. The old non-pair unsupported
command/FD-flag floor is retained; its stale fd-keyed nonblocking map is removed.
SO_NBIO and SETFL retain/revalidate slot generation and transport identity, then
serialize real mode updates under a guarded per-transport mode mutex outside the
table lock. GETFL reads atomic endpoint mode metadata. The sole nested lock order
is mode -> table; table holders never acquire the mode mutex. A concurrent close
can retire the numeric slot but cannot redirect a retained mode update.
GETSOCKOPT supports SO_ERROR only for pairs; other options, including linger,
are excluded. Windows accepts its existing SO_NOSIGPIPE no-signal behavior.

Windows per-call MSG_DONTWAIT is rejected instead of silently blocking; use the
real endpoint nonblocking flag. Existing mapped flags otherwise go to the host.
SIGPIPE delivery is **not implemented** (Linux sends suppress the host signal).
Full dup/rfork/exec inheritance, ancillary data, sendmsg/recvmsg, credentials,
general families, complete kqueue/epoll parity and general guest-buffer fault
handling are outside this implementation. GetUnixPeerProcessId explicitly rejects
pairs rather than returning fabricated credentials. Getsockname exposes an
unnamed endpoint using the existing 16-byte guest representation, never the host
construction path. This does not repair unrelated legacy floor exports.

## Verification entrypoints

`tests/SocketpairTests.cpp` exports `TestSocketpairNetworkOnly()` and
`TestSocketpairFileDescriptors(const std::filesystem::path&)`. Both use registered
MZb resolution, verify identical primary/floor wrapper addresses and have a
120-second fixture watchdog. Individual readiness waits are at most two seconds.
The first fixture must run without Memory initialization. The filesystem fixture
needs the existing hidden-Vulkan setup for KernelRead/Write plus initialized
FileSystem and a writable isolated temporary root. Both the tests and emulator
sources must define `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS`.

Coverage includes duplex, kernel dispatch, select, half-close, buffered EOF,
EPIPE, errno/flags, null/read-only/inaccessible copyout, injected bind/connect/
accept/second-slot failures, actual table exhaustion (including one free slot),
resource balance, concurrent rollback/allocation/close/reuse, and filesystem
allocation through the reserved interval with 140 held files. Exhaustion needs
enough host handles for 896 table sockets plus temporary pair construction.
Tests require an isolated socket table and no concurrent shutdown.

Windows verification: both socketpair selectors, the ordinary Unix transport
fixture, P2P ownership fixture and focused filesystem/JSON/SHM/SSL/HTTP regressions
passed (21 CTests). The cleanup fixture reported 161 OS handles before and after
its measured cycles; the external TEMP directory inventory found no new `kp-*`
paths. Seven existing guest-suspension fixtures also passed. Linux execution and
genuine ShellUI boot success are not established.

The bounded `socketpairrealboot1` Windows boot observed both SysCore socketpair
calls returning zero with descriptors `(130,131)`, replacing their stale inputs.
Its notification reader used fd12 generation 21. The run remained visually black
and exited on `KernelPollSema` dereferencing handle `-1`; the producer of that
invalid semaphore is under investigation. The observation does not trace Net
socket retirement or establish full process/IPC correctness.

Run the socket-focused checks from the configured build directory with:

```text
ctest -R "^(socketpair_network|socketpair_filesystem|network_p2p_ownership|network_unix_transport)$" --output-on-failure
```

The P2P fixture runs eight loopback port-3658 bind/share/conflict/close/reuse rounds.
It verifies logical mode independence and surviving physical ownership, but does
not force a particular host-bind interleaving or guest suspension.

## Ownership review fix

Every explicit `g_socket_mutex` ownership scope in network.cpp and this include
has a preceding HostLockCriticalGuard. Bind host calls, accept, logging, guest
option input reads, name allocation and final name destruction are outside that
mutex. AF_UNIX names are immutable shared snapshots, with pointer-only publication
under the table lock. Connect/Listen also retain transports across their host calls.

P2P bind reserves a physical address/port using a preallocated attempt in a fixed
array under the table lock. Contending binders wait on its completion atomically
outside table locks and guards, then revalidate their generation and conflicts.
This preserves normal same-port sharing and virtual-port rejection instead of
introducing a transient EADDRINUSE just because another host bind is in flight.
Reservations survive numeric descriptor close until the host call completes.
The waiter inherits the host bind's completion latency; no new timeout/error is
introduced for this legacy non-pair operation. Reservation-array exhaustion gives
ENOBUFS. It is not a new descriptor allocation framework.

Ordinary Accept allocates ownership before host accept and captures an immutable
name. Windows additionally captures listener mode and explicitly sets the
unpublished accepted socket to that mode before publication. This keeps Windows
inheritance and centralized GETFL coherent even with listener SETFL racing accept.
Linux retains its existing blocking accept default. Native Accept concurrency
semantics are not newly claimed by this fix.

Additional tests use the existing fixture entrypoints: filesystem mode tests an
ordinary nonblocking AF_UNIX listener/accept, GETFL, actual empty would-block,
independent subsequent modes, name retention and SO_NBIO. Network-only mode gates
a send after transport retention, closes/reuses its numeric fd, resumes it and
proves routing to the original peer, no byte on the replacement peer, EOF and
balanced retained resources. The hook is one-shot, calling-thread-only, test-only
and outside all Net locks/critical guards; gate waits are five seconds, no sleeps.

Windows network-only mode also measures GetProcessHandleCount on the main thread
before any concurrency fixture. Three identical success/setup-failure/copyout
cycles warm Winsock/provider/BCrypt paths, a fourth must establish a stable count,
then eight measured cycles must show no growth. A moving warmup baseline fails
with an explicit diagnostic rather than being labeled a socket leak. This is
additional OS evidence; main still checks kp-* path cleanup independently. The
existing fixture watchdog remains 120 seconds and the main runner timeout 150.
