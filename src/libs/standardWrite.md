# Standard-descriptor byte writes

The 13.60 `FxVZqBAA7ks` kernel export at `K+0x400` wraps syscall 4 with
`int64_t write(int, const void*, uint64_t)`. Native LibcInternal's allocator
diagnostics reach it through the native formatter, stream buffer and flush path.
The previous POSIX shim returned a narrowed requested length for descriptors
0/1/2 without emitting bytes.

`StandardWrite::Write` now writes stdout/stderr bytes to the actual host endpoint.
On Windows it uses the inherited standard handle and synchronous `WriteFile`,
without CRT newline translation, printf formatting, or a separate buffered flush.
Requests larger than the host DWORD count are allowed to make a short write;
the result is the actual transferred count. Positive progress is retained even
if the backend also reports an error. A full nonblocking byte pipe's successful
zero-byte result becomes guest `EAGAIN`. Errors without progress become guest
POSIX errors; the POSIX export alone writes guest errno. Success leaves it alone.

The emulator's stdin descriptor is read-only. Invalid descriptors, missing host
endpoints, lengths above `INT64_MAX`, and null buffers for nonzero lengths are
rejected. A zero-length write on an available stdout/stderr endpoint returns zero.
Socket and ordinary file descriptor dispatch continue through their existing
backends. Kernel `KernelWrite` uses the same unbuffered byte backend for stdout
and stderr, translates failures to kernel error codes, and leaves guest errno
alone. It does not enter the CRT's FILE lock or flush another writer's buffer.
This matters when native Mono suspends a guest thread during collection: a
suspended owner of the shared host stdout lock can block compositor logging.

Portable fallback uses host `write` and explicit errno translation. It retains
the host's SIGPIPE policy; this work does not implement guest signal delivery on
broken pipes. Windows broken-pipe errors are returned as `EPIPE`.

`StandardWriteTests.inc` uses actual Windows pipes and restores process standard
handles before assertions. It checks exact binary bytes/counts, validation and
endpoint errors, oversized nonblocking writes, and would-block on a full pipe.
Windows can reject an oversized request with no progress rather than write a
prefix; the fixture verifies either outcome against actual queued bytes. This
host showed the no-progress case, so positive partial-write behavior has source
review but no observed positive-partial runtime result here.
No backend success, output, or partial count is fabricated by these fixtures.
Run the fixture serially because standard-handle redirection is process-global.
