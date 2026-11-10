# Apple TV pre-launch and run10 crash audit — 2026-11-04

**Follow-up correction:** missing handler-name logging does not prove no LNC
entry, because `PRINT_NAME` is thread-locally disabled by default. Run18 traces
the worker through its gates to a rejected native wrapper request and establishes
the size40-versus36 validation mismatch. See
`vsh-run18-wrapper-rejection-20261104.md`; the run10 exception-vtable findings
below are separate evidence.

Read-only source, retained-log and firmware audit. No builds, live process
inspection/control, input injection, or production changes were performed.
The earlier Esvm audit is in `vsh-esvm-audit-20261103.md`.

## Findings

1. Run8 and run10 show Apple TV splash preparation but no logged invocation
   of the native `sceLncUtilLaunchApp` handler. Splash creation is an earlier,
   independent action; it does not establish that the launch worker reached
   the native request.
2. The guest launch worker has a boot-event poll and a prepare-task barrier.
   Retained evidence does not establish which operation was active in the
   stalled worker. No missing launch event semantics is proven yet.
3. Run10's later null-execute crash **is explained**: Hermes catches a native
   HostFunction exception and calls its virtual `what()` through a zero-filled
   HLE `std::exception` vtable. This has a narrowly supported fix: unshadow
   the real libc vtable export `dCzeFfg9WWI`.
4. The crash is not proof of the earlier launch stall's cause. Its recovered
   frames are Hermes frames, on a `SceRnJs-HERMES TTPoolInstance [` thread.

## Launch timeline and binding

Run8 `_Logs/vshruntime8.log`: selection at 7230–7240,
`SCE_BOOT_STAT_BIGAPP_LAUNCH_READY` at 7251, SplashScreen/LaunchFlow at
7262–7265, missing background image at 7268–7269. Boot initialization had
already set `BackgroundInitFinishedInitDaemons` at 1752 and
`ReadyGameBootNotified` at 1760.

Run10 `_Logs/vshruntime10.log`: selection at 5830–5831, BIGAPP readiness
at 5841, splash/LaunchFlow at 5843–5846. Later focus changes from splash to
`FocusCapture` at 6003 and back at 6015 show that UI processing continued.
The missing background image at 5854 is not evidence of a launch barrier.

An exact-name scan of each entire native trace found only two occurrences
of `sceLncUtilLaunchApp`, both relocations:

| Run | `_kyty.txt` lines | Native handler entry logs |
| --- | --- | --- |
| 8 | 7565, 9349 | none |
| 10 | 7635, 9419 | none |

Both resolve the native registration slot `0x931195ec8` to HLE
`0x1406954e0`. The handler in `src/libs/libLncUtil.cpp:577` calls
`PRINT_NAME()` before validating parameters. Thus the logs do not support
blaming a rejected parameter inside that handler.

The apparent managed four-argument/native three-argument discrepancy is
**not an ABI defect**. `Sce.Vsh.LncUtilWrapper.dll.sprx` RVA `0x750`
adapts the managed call. It preserves param from RCX, constructs a
null-terminated argument array, sets its final element to null at `0x7ac`,
then passes title RDI, array RSI, param RDX at `0x7c3–0x7c6` to the native
wrapper. The explicit managed argument count is not a fourth native argument.

## Actual guest launch gates (13.60 AOT)

Metadata DLL method bodies are reference stubs; these offsets come from the
matching `.dll.sprx` AOT code and decoded Mono PLT method references.

`Sce.Vsh.ShellUI.AppSystem`:

- `AppLauncher.Start`, `0x5c60`: cancel prior launch at `0x5caa`, set
  BigAppLaunchReady at `0x5caf`, create EventLocker, queue launch work,
  invoke LaunchStartedAction at `0x5e05`.
- Worker `<>c__DisplayClass57_0.<Start>b__0`, `0x6f90`:
  history update at `0x700c`; poll `GetBootEvent(0x08000000)` at `0x7045`,
  sleeping 100 ms, with a 60000 ms deadline; `Task.WaitAll` on prepare tasks
  at `0x7081`; schedule LaunchRequestingAction using FrameTask.CallOnce at
  `0x7219`; call LncUtilWrapper.LaunchApp at `0x7264`; schedule
  LaunchRequestedAction afterward at `0x72b9`.

`Sce.Vsh.ShellUI.ReactNativeApps`:

- `LaunchFlowContainer.OnLaunchStartedAction`, `0x1c0e0`, starts a task
  at `0x1c22e` and adds it to AppLauncher prepare tasks at `0x1c23d`.
- That task (`0x1d890`) obtains target info and invokes
  `FrameTask.CallAndWait` at `0x1d946`.
- Its UI callback (`0x1d980`) opens LaunchFlow at `0x1da99`, sets its
  application-focus locker at `0x1dabd`, disposes the temporary locker at
  `0x1dac5`, then selects focus layer 1 at `0x1dad3`.
- `LaunchFlow.OnLaunchRequesting`, `0x16ba0`, subscribes to
  LaunchRequestedAction, starts the busy indicator and releases its
  application-focus locker at `0x16c3a`.

Consequently splash creation is possible while launch preparation is still
pending. It does not identify an awaited LNC completion event.

`Sce.PlayStation.PUI.FrameTask.CallAndWait`, `0xf6070`, runs inline on the
main thread; otherwise it constructs an initially nonsignaled auto-reset
EventWaitHandle (`0xf60e5`), queues its callback, waits (`0xf618f`), disposes
the handle and propagates any captured exception. Its callback wrapper at
`0x3eb0a0` invokes the action at `0x3eb0c1`, then signals the wait handle at
`0x3eb0cf`. The exception-catching path also returns to the signaling path.
This is a managed synchronization completion, not an LNC launch event.

Native BootManager `GetBootEvent`, eboot RVA `0x12980`, polls the handle
at `0x13a69b8` with mode 1 and null result pointer and returns whether the
poll succeeded. That handle is `SceShellUIBootManager`, distinct from
`SceBootStatusFlags`. Current `eventFlag.cpp` accepts a null result pointer,
implements mode 1 as an AND test, and does not clear bits without a clear
mode. Together with the retained boot-completion log, this does not justify
synthetically adding the daemon-ready bit.

### Evidence still needed for a launch semantics fix

The retained logs do not expose the worker's prepare-task states or its
CallAndWait handle at the stall. A targeted future observation owned by the
parent should distinguish worker `0x7045` (boot poll), `0x7081` (prepare
barrier), prepare worker `0x1d946` (UI completion), and passage through
`0x7264` (native wrapper). If the UI callback has returned and signaled its
handle but the worker remains parked, investigate that handle's real
signal/wait implementation; do not substitute a fake LNC launch event.
No event-semantic production patch is supported by this audit alone.

## Run10 null-execute crash: exact causal chain

`_Runs/vshruntime10/_kyty.txt:1490676–1490710` contains the fault registers,
stack and frame chain, before the large trace-ring dump. The final error at
1683119 is the same fault, reported after the dump, around 4m37s.
`runtimeLinker.cpp:2750` is the diagnostic exit site, not the faulting call.

The shorter `_Logs/vshruntime10.log:6066–6091` also retains the evidence:

- RIP = 0; top stack return = `0x98001394a`.
- RDI/R14 = `0xa801c1ba0`; the object dump's first word is `0x141f8c890`.
- RAX = `0x141f8c890`.
- RSI points to `"Exception in HostFunction: "` at Hermes RVA `0x25e20a`.
- Thread is `SceRnJs-HERMES TTPoolInstance [`.

Module placement identifies Hermes base `0x980000000` (native trace line
570) and real libc base `0xa80000000` (line 1114).

Hermes disassembly:

```text
0x138ec call __cxa_begin_catch
0x138f5 cmp  r15d, 2
...
0x13909 mov  r14, rax
...
0x13941 mov  rax, [r14]
0x13944 mov  rdi, r14
0x13947 call [rax+0x10]
0x1394a ...                 ; exact retained return address
```

This is virtual exception-message dispatch. Run10 relocations (for example
native trace lines 3795, 11770, 19064) bind `dCzeFfg9WWI` to
`&g_vtableException[0] = 0x141f8c880`. The exception object's address point
is that address +0x10, exactly the observed `0x141f8c890`.

`src/libs/libShellCoreFloor.cpp:702` defines that vtable as four zero
uint64s. The virtual call reads address-point +0x10 = export +0x20,
`0x141f8c8a0`, which in this build is the adjacent zero-filled
`g_typeinfoInt` object (native relocation line 3640). It jumps to zero.

### Firmware comparison and minimal fix proposal

| Firmware | `dCzeFfg9WWI` vtable RVA | Size | `what()` target at export +0x20 |
| --- | --- | --- | --- |
| verified 13.60 | `0x1acc60` | 48 bytes | `0x1c4f0` |
| local4.03 `zecoxao/kek` | `0x1282f8` | 48 bytes | `0x19f00` |

Both have callable, relocated destructor/what/raise slots and a real RTTI
pointer. In 13.60 the address point is `0x1acc70`; `what()` simply returns
its firmware string. There is no version-specific zero-slot ABI here.

**Proposed minimal production change:** remove the single
`LIB_OBJECT("dCzeFfg9WWI", &g_vtableException[0])` registration at
`libShellCoreFloor.cpp:3139`, allowing the already-loaded real libc to own
this object. Do not replace the slot with invented success or an empty
message stub. This follows the source's existing treatment of real RTTI
vtables and the real C++ exception mechanism.

Parent validation should verify that `dCzeFfg9WWI` relocations resolve to
real libc +`0x1acc60` and that this exception-message dispatch no longer
jumps to zero. This addresses the demonstrated secondary crash, not the
unidentified original HostFunction exception or the earlier launch barrier.
NativeExtensions startup timing alone does not establish its responsibility
for either fault. No build or run was performed to validate the proposal.
