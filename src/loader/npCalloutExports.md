# Isolated native NP callouts (13.60)

`-NativeNpCallout` / exact host `KYTY_NATIVE_NP_CALLOUT=1` selects 51 NpCommon
functions: the unchanged ownership21, queue/payload20, and ten context/timer
exports in `npCalloutExports.h`. Ownership-only and queue-only remain 21/41.
Only the selected NpCommon provider is filtered and preferred over HLE.

The native trophy context allocated at ShellCore `ee85f0` is `0x38` bytes.
Its constructor installs vtable NpCommon+`48b40`; initialization creates the
mutex/condition and starts the scheduler. Shutdown signals and joins before
resource destruction. Virtual deleting destruction retains the native21
allocation-header/free pairing. No destructor aliases need extra exposure.

The `0x48` timer owns an inline scheduler record but borrows a separate callback
object at +0x40. Native trophy cleanup cancels/waits and deletes the timer, then
deletes that callback, before destroying the queue and context. Null callbacks,
duplicate scheduling and uninitialized-context errors remain native results.

The same imports serve Push and UDS, so this separate opt-in starts those
schedulers too. Native clocks, relative timed condition waits, guest pthreads
and joins remain authoritative. Successful callbacks and service backends are
separate runtime dependencies; no result, timer event or callback is synthesized.
