# Bounded external VSH launch capture — 2026-11-04

`tools/capture_vsh_launch.py` implements a **diagnostic only**, following
`vsh-native-prelaunch-followup-20261104.md`. No production code was changed,
build performed, process opened, debugger attached, or guest launched during
preparation. Run15's host commit exhaustion is not launch-job evidence; see
`vsh-runtime15-20261104.md`. Do not run this until the parent has authorized a
single ordinary VSH attempt with adequate host commit/disk headroom.

## Future command

Use native 64-bit Windows Python from the repository root. Supply the **new
VSH PID and that run's guest placement bases**, not Windows module enumeration
or a previous run's addresses. This command neither starts a process nor
selects/launches an application:

```powershell
python tools/capture_vsh_launch.py --pid <VSH_PID> --appsystem-base <APPSYSTEM_BASE_HEX> --pui-base <PUI_BASE_HEX> --seconds 70
```

Replace all three placeholders with explicit values. PID23664 is explicitly
rejected as a safeguard for the user's Apple test; do not target another
ordinary-app PID either. Wait for `ARMED`, then make **one normal VSH Cross
selection**. No input is injected. Default output is
`%LOCALAPPDATA%/Temp/opencode/vsh-launch-PID-TIMENS.json`. A normal completed
observation reports `timeout` (the observation window ended), **not** launch
success; inspect `events`, `final_retained`, and cleanup status. Exit code0
requires an armed, normally ended window, successful detach and no cleanup
errors. No native HLE-handler address is assumed and the tool does not claim
it observed that handler or terminate early on a wrapper call.

Ctrl+C/Break requests cleanup. **Never kill the debugger forcibly**, close its
console, or run it under an external hard-kill timeout while attached.

## Exact identity and instruction audit

The script reads only these authoritative local files (64MiB/file ceiling),
validates their pinned SHA-256 and ELF64 AMD64 executable program-header
mapping, and compares **all16 live bytes at every site before any patch**.
No relocation normalization or guessed expected opcode is accepted. Wrong
bases, other image versions, existing hooks, or inaccessible/non-executable
memory fail closed. Each patched byte is journaled before the write, verified
afterward, and instruction-cache flushed. No persistent page-protection change
is requested; Windows debugger `WriteProcessMemory` performs the code write.

* AppSystem: `298dd0c63867ee96c92977ac9643b188dd99b2f2f280a6b24f50491d78c63870`
* PUI: `458b2112e088c00ed200a6b06b1a41cb02fad8d9fb43b235f767746dbf4e56d7`

All AppSystem offsets below are **before** the listed instruction executes:

| RVA | Audited instruction / captured meaning |
| --- | --- |
| `0x5ddd` | `mov rcx,[rip+0x15fb34]`: RAX is queue result/job, R12 closure, R14 launcher, callee-saved R15 queue; volatile RDI is not assumed to retain the queue. Job static is `**(base+0x165918)`, not inline data. |
| `0x6f90` | `push rbp`: RDI closure and RSI job, before either is copied into R12/R15. Launcher is `*(closure+0x10)`. |
| `0x7011` | `mov rax,[r12+0x10]`: the call at `0x700c` returned; raw return/registers retained, no invented success interpretation. |
| `0x704a` | `test al,al`: first observed boot-poll AL, requested mask `0x08000000`; flags have not yet been updated by the test. |
| `0x7081` | `call 0x97784`: RDI is ToArray's task array, immediately before WaitAll. Launcher `+0x30` is the prepare list. |
| `0x7086` | `mov rax,[r15]`: WaitAll returned; cancellation is subsequently tested at job `+0x38`. |
| `0x7115` | `mov rcx,[rip+0x15e324]`: RBX is newly constructed target info; it has **not yet** been stored at R14 `+0x10`. |
| `0x7190` | `test al,al`: killing-decision AL; target info now comes from `*(r14+0x10)`. |
| `0x7264` | `call 0x97848`: raw RDI target, RSI launch-info, EDX and RCX stack options, before native wrapper call. |
| `0x7269` | `mov esi,0x80`: raw wrapper-return RAX before the following allocation; return type/result meaning is not guessed. |
| `0x72ca` | `mov rax,[r13]`: **common epilogue**, including the normal launch path as well as deadline, cancellation, target and killing exits. R15 is reused after `0x7198` and again at `0x71bb`; it is not a job pointer here. Retained job is used instead. |
| `0x7310` | `push rbp`: finish callback RDI closure/RSI job, before its static-job clear; captures stored exception/result/status even if catch breakpoint was consumed by another job. |

PUI `0xc5a1d` is `mov [rbp-0x38],rax`: caught exception is RAX and the
invocation's job is `*(rbp-0x18)`. The exception is retained **only if that
job equals the already-retained launch job**. This is not an arbitrary host
first-chance exception filter. Job exception/result/status/HRESULT/cancel
fields at `+0x20/+0x28/+0x30/+0x34/+0x38` are read from the audited InvokeRun
instructions; status3 alone does not indicate success.

## Ownership, timeout and cleanup

Each site is process-wide **one-shot**: restore its original byte, flush the
instruction cache, and set RIP from site+1 back to site. No single-step trap,
trap-flag modification, function call, register-result modification, synthetic
event or forced launch is used. SetThreadContext requests CONTROL only, with
the fetched control state/flags unchanged except RIP. Integer, floating-point,
vector and debug registers are not written.

Unrelated exceptions, including guest faults and unrelated INT3s, are forwarded
with `DBG_EXCEPTION_NOT_HANDLED`. Only the verified debugger break-in thread's
ntdll breakpoint and journaled diagnostic INT3s are consumed. The retired-site
table remains valid for simultaneous traps already issued by other threads.
Overlapping16-byte validation windows (`7081/7086`, `7264/7269`) explicitly
permit only journaled diagnostic INT3 bytes, not arbitrary code differences.

At timeout, Ctrl+C, or error, cleanup obtains a debugger stop. It requests
DebugBreakProcess only if no event or earlier attach/cleanup break-in is pending.
**CREATE_THREAD is not the requested breakpoint stop.** Intermediary events are
handled/continued with their correct exception dispositions until the remote
break-in reaches its authenticated ntdll DbgBreakPoint. Only then may cleanup
quiesce threads. A successful request remains journaled across timeout/retry,
including before CREATE delivery; it is journaled before the API call so an
ambiguous failure cannot enqueue repeated break-ins either. Initial attach
break-ins are reused rather than supplemented with another request.

Cleanup adds one suspension increment to each enumerated thread while the
debug event already stops the process, restores **all** patched sites, and
processes pending/queued owned traps. It drains the debug-event queue
while those increments prevent fresh target execution, detaches, then removes
only its own increments. It does **not** blindly rewind every RIP==site+1:
the one-byte `push rbp` entries make that unsafe. Failed ResumeThread handles
are retained for retry instead of losing ownership of a suspend increment.
If the drain reveals another queued break-in CREATE_THREAD, detach is refused:
cleanup releases its holds, retains that pending event/thread, and retries
acquisition until that same thread reaches its breakpoint, without requesting
another break-in. Known break-in threads that have already passed their
consumed breakpoint may finish after detach; unconsumed ones may not.

If a requested break-in never reaches an authenticated breakpoint (including
an ambiguous failed request with no subsequent events), the request stays
outstanding and recovery fails safe instead of repeatedly injecting threads.

The observation is capped at70 seconds; stop acquisition and drain each have
a5-second cooperative recovery bound. Win32 calls themselves have no hard
deadline. If code restore, context repair, event draining, detach or resume
fails, the tool reports conspicuously and retries recovery rather than exiting
over live INT3s or claiming safe detach. **Consequently irrecoverable OS/debugger
failure has no guaranteed wall-clock exit bound.** A hard deadline and guaranteed
restoration cannot both be promised after arbitrary OS failure. Normal timeout,
ordinary capture errors, partial arming and Ctrl+C all take the same cleanup
path. No live cleanup test has been authorized/performed.

## Retained data and limitations

* Retains the first consistent job/closure pair, launcher, queue, task-array
  address, per-event TID/time/registers/EFLAGS and up to1024 stack bytes.
  Queue/worker identity, target, launch arguments, job fields and bounded raw
  pointer graphs are included. At most two event snapshots per site are retained;
  additional simultaneous traps are still restored/rewound and counted.
* Job field and object snapshots are repeated at correlated milestones,
  finish callback and the final stopped capture, even if the static job field
  is cleared. **Addresses are not managed GC roots.** No guest retain/pin call
  is made, so movement, collection, reuse and unreadable memory remain possible.
* Prepare reference arrays use the Mono x64 header (`length+0x18`, data+0x20),
  capped at16 entries and256 raw bytes/task. Task internals and managed
  exception type/message offsets are **not validated here**. Exception objects,
  first-qword memory and bounded pointer-field pointees are retained for offline
  layout audit; the tool does not label guessed strings or Task flags as facts.
* A one-shot boot site records the **first** poll, not the final poll or actual
  native flag handle/bits/return. A later epilogue plus prior milestones may
  constrain the path but does not uniquely identify the return reason. The
  native wrapper before/return is not actual libLncUtil handler entry.
* The global PUI catch site may be consumed by an unrelated job; that hit is
  ignored, not attributed to launch. Another launch can similarly consume a
  milestone. Absence of a captured milestone is not proof of non-execution.
  If the worker is never observed, raw queue state is retained when its result
  is captured, but nested queue-worker/event contracts are not decoded.
* Every debugger event briefly stops **all** target threads. Per-event handler
  milliseconds are recorded, not the complete kernel scheduling pause. This
  changes timing; it is not a transparent race/deadline proof. Final thread
  quiescence can be longer with hundreds of guest threads.

This is deliberately narrower than all evidence requested by the follow-up:
it can discriminate observed entry/return/catch milestones, not independently
prove all absent milestones, final boot state, decoded task exceptions or an
unentered queue's synchronization contract. Prefer those honest limits to
re-arming unrelated breakpoints, changing flags, guessing layouts, or executing
guest methods. Extend only after this capture establishes the relevant branch.

## Local-only validation

```powershell
python tools/capture_vsh_launch.py --self-test
python -c "from pathlib import Path; p=Path('tools/capture_vsh_launch.py'); compile(p.read_text(encoding='utf8'),str(p),'exec'); print('syntax OK')"
```

Passed: ctypes AMD64 debug-event/memory layouts; two pinned staged ELFs;
13 instruction anchors; overlapping windows; real context-rewind code on fake
ctypes buffers; two simultaneous retired-site traps; idempotent pending-event
retry; unrelated-exception forwarding; real receive/handle/cleanup ordering
against mocked CREATE_THREAD -> unrelated AV/unauthenticated INT3 ->
authenticated breakpoint; multiple created break-ins all completing before hold;
acquisition timeout after CREATE with reuse on retry; ambiguous request API
failure before CREATE; reuse of the initial attach break-in; and queued
break-in CREATE during drain causing refusal to detach and safe reacquisition.
The CREATE-order regression asserts no hold/restore occurs before the actual
breakpoint and fails against the previous first-debug-event cleanup flow.
The self-test does not instantiate
Capture or call any Win32 process/debug API. These are static/mock guards, not
a claim that live debugger timing, suspension/draining or restoration is proven.
