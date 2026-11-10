# Bounded kernel frame-chain backtrace

## Native evidence

Read-only hash and ELF/disassembly inspection of local firmware 13.60
`system_b/common/lib/libkernel.sprx` established:

- SHA256: `4f07827e84228db3c10fd1e804e208fc7799240990b7f37e3d97367882ff3c49`.
- Export `Wl2o5hOVZdw#I#A`, RVA `0x34c70`, size `0x21d`, file offset `0x38c70`.
- ABI `void(const char* prefix)`. Entry saves RBP and starts at its own frame;
  records contain the previous RBP at `+0` and saved return PC at `+8`.
- The native first pass (`0x34ca0`–`0x34ccd`) follows links until null without a
  bound and excludes initial PCs inside its kernel image range.
- Header output at `0x34cdc` is `[%s] backtrace:`. The second pass reads PCs at
  `0x34d3e`, implements first/last-100 presentation at `0x34d60`–`0x34d92`, and
  optionally symbolizes at `0x34e08`, falling back to raw output at
  `0x34e26`/`0x34e36`.

The firmware was only read. This profile is evidence for the ABI and layout,
not a claim of compatibility with other firmware profiles.

## Implementation and limits

`Libs::LibKernel::KernelBacktrace` is a Windows-only SysV entry shim. Before any
compiler prologue it captures the incoming RBP and the actual return PC at RSP,
then tail-calls a stack-realigning SysV helper. Only caller-saved RSI/RDX change.
The helper logs that entry return PC separately and walks the captured guest RBP.
A registration/tracing trampoline may be that caller; the service does not guess
or skip through such frames. The first implementation used
`__builtin_frame_address(0)`, but lead disassembly found Windows Clang returned a
local-frame base with a non-SysV layout. That implementation was rejected before
native SSL admission; pure collector fixtures alone could not catch it.

`Libs::KernelBacktrace::Collect` follows x86-64 little-endian frame records once.
Every PC requires a successful, exact 16-byte read. Both failed reads and short
copies stop capture without consuming the partial record. At most 512 records
are attempted (8192 requested bytes, including failed reads). A fixed visited
array detects cycles without monotonic-address or alignment assumptions, which
allows transitions between guest and host stacks. Range overflow is rejected
before reading. A null link completes the walk, including at exactly 512 frames;
a non-null continuation at the cap is reported as truncated.

Production reads use `ReadProcessMemory(GetCurrentProcess(), ...)`, check its
boolean result and exact byte count, and preserve `LastError`. The entire export
also preserves `LastError` across logging. There is no raw guest dereference,
guest write, guest callback, module lookup, loader lock, symbol resolution or
host-stack-only unwinding API. Only the portable core accepts an explicitly
supplied trusted host reader; production has no reader override.

Prefix capture reads at most 128 bytes, one byte at a time so a terminated string
at a readable-page boundary is accepted. A separate local terminator is always
present. Null, unreadable, overflowing and unterminated prefixes have explicit
diagnostics; partial readable prefix text is retained. Prefix reads are a
separate maximum of 128 bytes in addition to the 8 KiB frame-read budget.

All output uses `LOGF`; silent logging returns before memory reads. Captured
addresses are logged directly, with all frames shown up to 200 and first/last
100 above that, plus an explicit omitted-captured-frame count. A limit, cycle,
overflow or read error reports truncation, the number of captured frames and
the stopping frame address. The displayed tail is the tail of the bounded
capture, not a claim to have found the actual end of a longer chain.
The production service reserves one of its 512 total frame slots for the entry
return PC and collects at most 511 linked records. Its first/last-100 selection
and omitted-frame count include that entry PC. The portable collector's default
limit remains 512 records; caller-supplied lower limits are supported and larger
limits are clamped to the fixed storage/read bound.

Frame pointers may be omitted, corrupted, reused or concurrently modified.
Readable saved values are evidence of frame-chain contents, not proof of valid
executable return sites. No unwind metadata, stack scanning or invented frames
fill gaps. Native symbolization and leading-kernel-frame filtering are omitted;
there is no second unbounded pass. The collector uses fixed local storage;
the existing logging implementation may allocate and serialize output. Multiple
threads' output may interleave. This prerequisite does not establish SSL or
updater readiness.

## Lead integration and validation

Only these four new files are owned by this change. Lead integration:

1. Include `libs/kernelBacktrace.h` in `src/libs/libKernel.cpp`. In its existing
   libkernel registration context, under `#ifdef _WIN32`, register
   `LIB_FUNC("Wl2o5hOVZdw", Libs::LibKernel::KernelBacktrace);`. Do not register
   it on other platforms.
2. Include `libs/kernelBacktrace.h` at file scope in
   `tests/VirtualMemoryAllocationTests.cpp`, then include
   `KernelBacktraceTests.inc` alongside the other `.inc` fixtures. Dispatch
   `RunTest(TestKernelBacktrace);`. The fixture defines `void TestKernelBacktrace()`
   and uses the parent's `Check(const char*, bool, ...)` helper. It requires no
   firmware, live Windows reads or additional fixture headers.
3. `CMakeLists.txt:199` uses `file(GLOB ... CONFIGURE_DEPENDS)` including
   `src/libs/*.cpp` and `*.h`. Ensure CMake reconfiguration sees the new source;
   the lead owns any test-target source wiring.

Fixtures cover byte-backed nonmonotonic/unaligned chains, real PC order,
midchain partial reads (both false and incorrectly successful reader results),
unreadable starts, cycles, address overflow, 512-frame/8192-byte limits, exact
termination at the cap, and bounded prefix boundary/failure cases.

The initial collector implementation built and passed its portable fixtures
alongside startup/fatal checks. The subsequent compiled-prologue inspection found
the entry-frame defect described above, so those passes did not validate the
export's guest frame acquisition.

A Windows service subprocess fixture now calls the real export from executable
SysV test code with a known RBP chain. Its parent checks the actual entry return
PC, a known saved PC, prefix output, safe invalid-prefix reporting and preserved
LastError on both calls. It is bounded to five seconds. The corrected shim and
this service fixture passed the fresh combined Windows build/run. Actual object
disassembly confirmed exactly the intended three entry instructions. The test
verified caller/saved PC, invalid-prefix output and LastError preservation; these
checks do not claim that all native allocator failure paths are supported.
