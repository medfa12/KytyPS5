// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_APPLICATION_RETURNING_EXPORTS_H_
#define KYTY_LOADER_APPLICATION_RETURNING_EXPORTS_H_

namespace Loader {

struct ApplicationReturningExport {
	const char* nid;
	const char* expression;
};

// Production registration identities, NOT names inferred from NIDs or a test
// override. Match BOTH fields against the resolved LIB_FUNC record after normal
// owner/version/address checks. An absent entry must remain a closed transition.
// This classifies the baseline-SysV normal-return/recovery ABI only: it neither
// proves semantic completeness nor admits host exceptions, unsafe guest pointers,
// arbitrary-PC preemption, or stoppability of blocking operations. See
// docs/vsh-returning-hle-audit-20261105.md for evidence and explicit exclusions.
inline constexpr ApplicationReturningExport ApplicationReturningExports[] {
	// Checked once/guard callbacks and owner-scoped CRT registration/drain.
	// Process exit has a distinct nonreturning contract and remains unadmitted.
	{"14bOACANTBo", "Posix::pthread_once"},
	{"DiGVep5yB5w", "LibC::std_execute_once"},
	{"3GPpjQdAMTw", "LibcInternal::CxaGuardAcquire"},
	{"9rAeANT2tyE", "LibcInternal::CxaGuardRelease"},
	{"2emaaluWzUw", "LibcInternal::CxaGuardAbort"},
	{"8G2LB+A3rzg", "LibC::atexit"},
	{"XKRegsFpEpk", "LibC::catchReturnFromMain"},
	// Bundled libc 13.60's decoded kernel-status rwlock wrapper family.
	// Typed pins, owner/token authority, terminal waits and stop-serialized
	// publication live in pthread.cpp. Same-body POSIX-looking aliases are NOT
	// inferred from these identities (sIl/iGjs/Egm and SFx/XhWH stay closed).
	{"6ULAa0fq4jA", "LibKernel::PthreadRwlockInit"},
	{"BB+kb08Tl9A", "LibKernel::PthreadRwlockDestroy"},
	{"Ox9i0c7L5w0", "LibKernel::PthreadRwlockRdlock"},
	{"mqdNorrB+gI", "LibKernel::PthreadRwlockWrlock"},
	{"XD3mDeybCnk", "LibKernel::PthreadRwlockTryrdlock"},
	{"bIHoZCTomsI", "LibKernel::PthreadRwlockTrywrlock"},
	{"iPtZRWICjrM", "LibKernel::PthreadRwlockTimedrdlock"},
	{"adh--6nIqTk", "LibKernel::PthreadRwlockTimedwrlock"},
	{"+L98PIbGttk", "LibKernel::PthreadRwlockUnlock"},
	// Checked owner-private heap vector and pinned inactive trace storage.
	{"p5EcQeEeJAE", "LibKernel::KernelRtldSetApplicationHeapAPI"},
	{"NWtTN10cJzE", "LibcInternalExt::LibcHeapGetTraceInfo"},
	{"jh+8XiK4LeE", "Memory::KernelIsAddressSanitizerEnabled"},
	// Inactive sanitizer queries return owner-pinned guest tables with null hooks;
	// no shared host table or allocator callback is exposed to a shared app.
	{"py6L8jiVAN8", "LibKernel::KernelGetSanitizerMallocReplaceExternal"},
	{"bnZxYgAFeA0", "LibKernel::KernelGetSanitizerNewReplaceExternal"},
	// Original eight entries: libKernel.cpp registrations; module callbacks use
	// RuntimeLinker::{Start,Stop}ModuleChecked nested guest boundaries. Process
	// counters are implemented in kernel/pthread.cpp; SDK query in processSdk.cpp.
	{"wzvqT4UqKX8", "LibKernel::KernelLoadStartModule"},
	{"QKd0qM58Qes", "LibKernel::KernelStopUnloadModule"},
	// Shared owners use ResolveApplicationDlsym: private objects stay pinned;
	// typed functions get immutable owner-checked native/HLE call entries.
	{"LwG8g3niqwA", "LibKernel::KernelDlsym"},
	{"4J2sUJmuHZQ", "LibKernel::KernelGetProcessTime"},
	{"BNowx2l588E", "LibKernel::KernelGetProcessTimeCounterFrequency"},
	{"fgxnMeTNUtY", "LibKernel::KernelGetProcessTimeCounter"},
	{"WB66evu8bsU", "LibKernel::KernelGetCompiledSdkVersion"},
	{"3k6kx-zOOSQ", "Memory::KernelMlock"},
	{"xQIIfJ860sk", "Memory::KernelMunlock"},

	// Only the scoped adapters in kernel/pthread.cpp return to their matching
	// owner gateway before typed thread-exit propagation at the worker root.
	// This does NOT authorize raw pthread_exit, process exit, cancellation or kill.
	{"3kg7rT0NQIs", "LibKernel::PthreadExit"},
	{"FJrT5LuUBAU", "Posix::pthread_exit"},

	// kernel/pthread.cpp clock/conversion bodies and libKernel.cpp POSIX adapters:
	// real host clock reads / scalar output conversion, no guest callback or wait.
	{"wRYVA5Zolso", "LibKernel::KernelClockGetres"},
	{"QBi7HCK03hw", "LibKernel::KernelClockGettime"},
	{"ejekcaNQNq0", "LibKernel::KernelGettimeofday"},
	{"kOcnerypnQA", "LibKernel::KernelGettimezone"},
	{"0NTHN1NKONI", "LibKernel::KernelConvertLocaltimeToUtc"},
	{"-o5uEDpN+oY", "LibKernel::KernelConvertUtcToLocaltime"},
	{"-2IRUCO--PM", "LibKernel::KernelReadTsc"},
	{"1j3S3n-tTW4", "LibKernel::KernelGetTscFrequency"},
	{"lLMT9vJAck0", "LibKernel::clock_gettime"},
	{"lLMT9vJAck0", "clock_gettime"},
	{"smIj7eqzZE8", "clock_getres"},
	{"n88vx3C5nW8", "Posix::gettimeofday"},
	{"k+AXqu2-eBc", "Posix::getpagesize"},

	// libKernel.cpp / errno.cpp: owner process parameter/PID, actual thread-local
	// errno and TLS allocation. RuntimeLinker::TlsGetAddr uses host runtime memory,
	// not the registered guest application-heap API.
	{"959qrazPIrg", "LibKernel::KernelGetProcParam"},
	{"HoLVWNanBBc", "LibKernel::getpid"},
	{"9BcDykPmo1I", "LibKernel::get_error_addr"},
	{"9BcDykPmo1I", "LibC::libc_error"},
	{"vNe1w4diLCs", "LibKernel::tls_get_addr"},
	// kernelTextLocation.cpp: real unsupported/absent-provider query policy;
	// public wrapper intentionally discards status, NOT errno or output rules.
	{"AmJ0mn2l4lM", "LibKernel::KernelGetLibkernelTextLocation"},
	{"-YTW+qXc3CQ", "LibKernel::KernelInternalMemoryGetModuleSegmentInfo"},

	// kernel/memory.cpp implemented VM/accounting operations, registered verbatim
	// in libKernel.cpp. The alloc/free hooks are HOST ASan callbacks installed only
	// by libDbgAsan.cpp, not guest function pointers. No batch/pool/PRT blanket rule.
	{"mL8NDH86iQI", "Memory::KernelMapNamedFlexibleMemory"},
	{"IWIBBdTHit4", "Memory::KernelMapFlexibleMemory"},
	{"4h6F1LLbTiw", "Memory::KernelMapFlexibleMemory"},
	{"cQke9UuBQOk", "Memory::KernelMunmap"},
	{"pO96TwzOm5E", "Memory::KernelGetDirectMemorySize"},
	{"C0f7TJcbfac", "Memory::KernelAvailableDirectMemorySize"},
	{"rTXw65xmLIA", "Memory::KernelAllocateDirectMemory"},
	{"B+vc2AO2Zrc", "Memory::KernelAllocateMainDirectMemory"},
	{"L-Q3LEjIbgA", "Memory::KernelMapDirectMemory"},
	{"NcaWUxfMNIQ", "Memory::KernelMapNamedDirectMemory"},
	{"MBuItvba6z8", "Memory::KernelReleaseDirectMemory"},
	{"rVjRvHJ0X6c", "Memory::KernelVirtualQuery"},

	// kernel/pthread.cpp: creation publishes an owner lease before RunRuntimeWorker
	// enters guest code at a separate root. Identity/detach/yield do not dispatch
	// guest signals. Rwlock/barrier/once waits remain omitted.
	{"6UgtwV+0zb4", "LibKernel::PthreadCreate"},
	{"OxhIB8LB-PQ", "Posix::pthread_create"},
	{"4qGrR6eoP9Y", "LibKernel::PthreadDetach"},
	{"+U1R4WtXvoc", "Posix::pthread_detach"},
	{"aI+OeCz8xrQ", "LibKernel::PthreadSelf"},
	{"EotR8a3ASf4", "LibKernel::PthreadSelf"},
	{"EotR8a3ASf4", "Posix::pthread_self"},
	{"3PtV6p3QNX4", "LibKernel::PthreadEqual"},
	{"7Xl257M4VNI", "LibKernel::PthreadEqual"},
	{"EI-5-jlq2dE", "LibKernel::PthreadGetthreadid"},
	{"T72hz6ffq08", "LibKernel::PthreadYield"},
	{"6XG4B33N09g", "LibKernel::PthreadYield"},
	// Native-worker regressions cover terminal owner cancellation, preserved
	// timeout/FIFO state, host RAII and suppression at the matching return gateway.
	// Guest joins cannot reap a stopped/foreign target or replace a real host join.
	{"onNY9Byn-W8", "LibKernel::PthreadJoin"},
	{"h9CcP3J0oVM", "Posix::pthread_join"},
	{"9UK1vLZQft4", "LibKernel::PthreadMutexLock"},
	{"7H0iTOciTLo", "Posix::pthread_mutex_lock"},
	{"IafI2PxcPnQ", "LibKernel::PthreadMutexTimedlock"},
	{"Io9+nTKXZtA", "Posix::pthread_mutex_timedlock"},
	{"WKAXJ4XBPQ4", "LibKernel::PthreadCondWait"},
	{"BmMjYxmew1w", "LibKernel::PthreadCondTimedwait"},
	{"Op8TBGY5KHg", "Posix::pthread_cond_wait"},
	{"27bAgiJmOh0", "Posix::pthread_cond_timedwait"},
	{"K953PF5u6Pc", "Posix::pthread_cond_reltimedwait_np"},
	{"mkx2fVhNMsg", "Posix::pthread_cond_broadcast"},
	{"2MOy+rUfuhQ", "Posix::pthread_cond_signal"},

	// Real opaque attribute state and host pthread attribute adapters. Only these
	// inspected operations; no scheduling/cancellation or setter-family inference.
	{"nsYoNRywwNg", "LibKernel::PthreadAttrInit"},
	{"62KCwEMmzcM", "LibKernel::PthreadAttrDestroy"},
	{"x1X76arYMxU", "LibKernel::PthreadAttrGet"},
	{"8+s5BzZjxSg", "LibKernel::PthreadAttrGetaffinity"},
	{"JaRMy+QcpeU", "LibKernel::PthreadAttrGetdetachstate"},
	{"FXPWHNk8Of0", "LibKernel::PthreadAttrGetschedparam"},
	{"UTXzJbWhhTE", "LibKernel::PthreadAttrSetstacksize"},
	{"2Q0z6rnBrTE", "LibKernel::PthreadAttrSetstacksize"},
	{"DzES9hQF4f4", "LibKernel::PthreadAttrSetschedparam"},
	{"4+h9EzwKF4I", "LibKernel::PthreadAttrSetschedpolicy"},
	{"3qxgM4ezETA", "LibKernel::PthreadAttrSetaffinity"},
	{"wtkt-teR1so", "Posix::pthread_attr_init"},
	{"zHchY8ft5pk", "Posix::pthread_attr_destroy"},
	{"FXPWHNk8Of0", "Posix::pthread_attr_getschedparam"},
	{"2Q0z6rnBrTE", "Posix::pthread_attr_setstacksize"},
	{"0qOtCR-ZHck", "Posix::pthread_attr_getstacksize"},
	{"VUT1ZSrHT0I", "Posix::pthread_attr_getdetachstate"},
	{"JarMIy8kKEY", "Posix::pthread_attr_setschedpolicy"},
	{"xesmlSI-KCI", "Posix::pthread_attr_setscope"},

	// Real mutex state transitions / host attr adapters, not blocking acquisition.
	// NativeMutexTrylock/Unlock do not call the signal-dispatch wait path.
	{"cmo1RIYva9o", "LibKernel::PthreadMutexInit"},
	{"2Of0f+3mhhE", "LibKernel::PthreadMutexDestroy"},
	{"upoVrzMHFeE", "LibKernel::PthreadMutexTrylock"},
	{"tn3VlD0hG60", "LibKernel::PthreadMutexUnlock"},
	{"F8bUHwAG284", "LibKernel::PthreadMutexattrInit"},
	{"smWEktiyyG0", "LibKernel::PthreadMutexattrDestroy"},
	{"iMp8QpE+XO4", "LibKernel::PthreadMutexattrSettype"},
	{"ttHNfU+qDBU", "Posix::pthread_mutex_init"},
	{"ltCfaGr2JGE", "Posix::pthread_mutex_destroy"},
	{"K-jXhbt2gn4", "Posix::pthread_mutex_trylock"},
	{"2Z+PpY6CaJg", "Posix::pthread_mutex_unlock"},
	{"mDmgMOGVUqg", "Posix::pthread_mutexattr_settype"},

	// Key registration records execution identity; DestructPthreadSpecific routes
	// each owned destructor through RuntimeGuestExecution::InvokeCallback.
	{"geDaqgH9lTg", "LibKernel::PthreadKeyCreate"},
	{"PrdHuuDekhY", "LibKernel::PthreadKeyDelete"},
	{"+BzXYkqYeLE", "LibKernel::PthreadSetspecific"},
	{"eoht7mQOCmo", "LibKernel::PthreadGetspecific"},
	{"mqULNdimTn0", "Posix::pthread_key_create"},
	{"6BpEZuDT7YI", "Posix::pthread_key_delete"},
	{"WrOLvHU0yQM", "Posix::pthread_setspecific"},
	{"0-KXaS70xy4", "Posix::pthread_getspecific"},

	// libC.cpp: real owner environment and cxa registration/finalization. Claimed
	// callbacks execute outside registry locks via InvokeCallback, including thread
	// atexit cleanup. NOT LibKernel::pthread_cxa_finalize (currently log-only).
	{"bzQExy189ZI", "LibC::init_env"},
	{"tsvEmnenz48", "LibC::cxa_atexit"},
	{"H2e8t5ScQGc", "LibC::cxa_finalize"},
	{"qBS714-Jr3g", "LibcInternalExt::LibcInternalExtCxaThreadAtexit"},
	// printf's VA_ARGS shim uses only baseline GP/XMM/stack state; its context
	// callback is fixed HOST kyty_printf_ctx (guestPrintf.cpp), never guest code.
	{"hcuQgD53UxM", "LibC::libc_printf"},

	// libC.cpp direct CRT string/comparison bodies and scalar math: no sort,
	// comparator, guest new-handler, locale callback or long-double/AVX ABI.
	{"DfivPArhucg", "LibcInternal::memcmp"},
	{"aesyjrHVWy4", "LibcInternal::strncmp"},
	{"Ovb2dSJOAuE", "LibcInternal::strcmp"},
	{"j4ViWNHEgww", "LibcInternal::strlen"},
	{"6sJWiWSRuqk", "LibcInternal::strncpy"},
	{"Ls4tzzhimqQ", "LibcInternal::strcat"},
	{"ob5xAW4ln-0", "LibcInternal::strchr"},
	{"9yDWMxEFdJU", "LibcInternal::strrchr"},
	{"viiwFMaNamA", "LibcInternal::strstr"},
	{"5jNubw4vlAA", "LibcInternal::Strnlen"},
	{"kHg45qPC6f0", "LibcInternal::Strncat"},
	{"-P6FNMzk2Kc", "LibcInternal::cosf"},
	{"pztV4AF18iI", "LibcInternal::sincosf"},
	{"88Vv-AzHVj8", "LibcInternal::fmodf"},
	{"lA94ZgT+vMM", "LibcInternal::libc_isnanf"},
	{"jMB7EFyu30Y", "LibcInternal::sincos"},
	{"H8ya2H00jbI", "LibcInternal::sin"},
	{"2WE3BTYVwKM", "LibcInternal::cos"},
	// Malloc/Free use the implemented GuestHeap arena/block registry and VM
	// backing; FreeAlignedBase removes tracked alignment metadata before freeing.
	{"gQX+4GDQjpM", "LibcInternal::Malloc"},
	{"tIhsqj0qsFE", "LibcInternal::Free"},
};

} // namespace Loader
#endif
