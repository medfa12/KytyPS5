# Original AppBrowse fault and bounded capture — 2026-11-05

## New direct runtime evidence

`_Runs/obroute204240/_kyty.txt:166880-166881` already captures the original
fault before Mono's fatal reporter:

```
sigsegv dispatch #0 pc=93055d79a fault=0(r) prog=eboot.bin handler=960230c10
     rdi=0 rsi=0 rcx=deadbeef00000007
```

Thus RDI is now established as zero at ShellUI `+0x55d79a` (`mov rdi,[rdi]`).
The later `libmonosgen+0x30a4f0` UD2 is not the original fault. Earlier documents
correctly described the registers as unavailable for their earlier runs; that
limitation no longer applies to this run.

Chronology in this same log:

* 43476-43477: AppDb client factory and HLE connect success, explicitly without
  native acceptance.
* 43478-43479: `SceAppBrowseEventFlag` open fails `0x80020003`; InitClient fails.
* 45457: producer finally creates that named event.
* 45536-45545: AppDb worker starts, generic dispatcher returns, worker exits.
* 166880: original null dereference; 166888 names `SceDbpNotificationThread`.

This agrees with the audited failure path in
`vsh-integration-validation-20261105.md`: failed AppBrowse prerequisite prevents
DbAccessor publication; DBP callback still consumes the null singleton. It does
not establish that the schema-query errors cause this crash.

## Capture build

Added opt-in `KYTY_GUEST_SIGSEGV_TRACE` to the existing guest near-null signal
dispatch diagnostic in `src/libs/libKernel.cpp`. Before invoking the guest
signal handler, the first four dispatches record original PC/RVA, registers,
16 stack words and at most eight frame links. Windows ReadProcessMemory contains
bad pointer reads; records are flushed before the potentially fatal handler.
No guest state, signal handling, initialization or readiness behavior is changed.

Built `_Build/vsh-integrated/kyty_emulator.exe` successfully after parent confirmed
all emulators exited; output is `original-fault-capture-build.log` in that build
directory. `git diff --check` passes. `_Build/windows` was not rebuilt.

Parent capture request: set host environment `KYTY_GUEST_SIGSEGV_TRACE=1`, run a
fresh isolated Home or OOBE once, retain the `original-sigsegv` lines, then restore
the environment. This captures the remaining dynamic caller-chain evidence
without an attach race or forced debugger pause. Presentation remains unverified.

## Actual fix boundary

The pre-registration wait core is implemented and tested, but the actual AppDb
factories still take `libC.cpp`'s generic HLE path. No production call installs
the NP/LoginMgr transport hooks; there is no AppDb exact adapter yet. Adding only
a name match or delay cannot retain the stack-owned native dispatcher, establish
acceptance or provide authentic peer authorization. The coherent production work
remains exact AppDb routing plus pinned callbacks, peer identity/authorization,
blocking dispatcher and stop/join lifetime; see the existing native AppDb and
IPMI wait contracts. No capability, event flag or readiness substitute was added.
