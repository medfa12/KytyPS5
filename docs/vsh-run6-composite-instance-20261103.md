# Run6 Composite instance investigation

## Findings

Run6's `command failed. disconnected from compositor server...` is a native
client-local null-state diagnostic, not evidence by itself of a socket EOF.
The strongest supported path is a SystemMonitor query against a duplicate,
uninitialized Composite instance. A live call stack is still needed to confirm
which invocation printed the first message.

### Native and managed call path (13.60)

`_ShellUIManaged/common/ReactNative.PUI.dll` has this ImplMap entry:

- Type: `ReactNative.SystemMonitor.Compositor`
- Method: `_GetRenderingTime`, MethodDef `0x06001040`
- Module: `libSceComposite`
- Entry: `sceCompsoitorGetProcessRenderingTime` (spelling in firmware)
- NID: `EcTHemmz6n4`, native RVA `0x43e0`, size `0x6c`

Native `0x43e0` reads its own qword at RVA `0x18030`. When nonzero, it
copies the rendering-time dword from command-buffer offset `0x28` to the
output pointer and returns zero. When null, it prints the exact disconnect
text at callsite `0x4435` and returns `0x80d40001`. This function performs
no socket operation. Native init sets that pointer at `0x51c1`; init and
quit can clear it. Other command functions share this diagnostic as well.

### Duplicate instance evidence

`_Runs/vshruntime6/_kyty.txt`:

- Lines 21943–21947: `/system/priv/lib/libSceComposite.sprx` loads from the
  firmware tree at base `0xab0000000`.
- Lines 80491/80494: client initialization mutexes appear once.
- Lines 310475–310501, near elapsed `00:01:09.096`: Mono probes the staged
  `common_ex/lib/libSceComposite` path and a second copy maps at
  `0xe210000000`.
- Line 310551: 121 Composite imports marked for re-resolution.
- Line 311828: second module starts.

The two host paths are **the same filesystem file** (`os.path.samefile`
returns true), hard-linked by `prepare_shellui_managed_1360.py`. Both have
126708 bytes and SHA-256
`d9abe348692c6e5b9e4de7fa38d6d46c74d675b5a5af63e1761d9a7bfc80bb04`.

`KernelLoadStartModule` asks `RuntimeLinker::FindProgramByFileName` to reuse
an image. That function compares slash-normalized, case-insensitive path
strings only, so it misses the hard-link alias. `KernelDlsym` first returns
exports from the explicitly requested module, so a successful lookup on the
second handle returns `0xe2100043e0`, whose state is independent of the
initialized first instance.

The re-resolution notice does **not** establish redirection of PSM imports
to the new instance. `Resolve` searches visible providers in insertion order;
the first session Composite remains first. The retained log contains no
later `OWU6H65+LLk`/`IUlpGnuoR1c` relocation to the second base. Direct
handle-scoped dlsym, rather than global import replacement, is the concrete
suspected split-state path. Successful dlsym logging is sampled, so the actual
SystemMonitor lookup was not captured in run6.

## Scope and next coordinated step

The parent subsequently authorized the loader correction. Implemented:

- `RuntimeLinker::FindProgramByFileName`: reuse within the requesting owner,
  retaining the explicit pinned SSL/libc provider exception; recognize file
  identity with nonthrowing `std::filesystem::equivalent` after the existing
  normalized path check. Null-owner/session behavior is retained. No basename
  or content-hash merge.
- `KernelLoadStartModule`: up to 32 targeted Composite reuse records, including
  requested/provider paths, handle, owners, base, started state and load count.
- `KernelDlsym`: up to 64 targeted Composite resolution records, including
  requested handle, symbol, requester/owner, provider/owner, path and address.

Builds and execution remain deferred to the parent while run7 fault capture
and the presentation fix are coordinated. Whitespace checks passed.

Focused regression: `tests/RuntimeProgramFileReuseTests.inc`, using real ELF
headers/files/hard links and production lookup, start and guest load/dlsym
entry points with synthetic already-mapped Program records and counted init
callbacks. Covers alias reuse with one start and reference-count increment,
SystemMonitor dlsym retaining the instance, byte-identical same-basename copies
remaining distinct, failed identity queries, owner A/B/session isolation, and
both explicit pinned-provider exceptions including aliases.

Build target: `virtual_memory_allocation_tests`. CTest:
`runtime_program_file_reuse` (CLI `--runtime-program-file-reuse-only`). Also
run existing `application_symbol_scope` when the parent builds the target.

For the parent's next diagnostic run, log Composite load/reuse and
`KernelDlsym` resolution (symbol, module handle/path, selected address,
requesting owner). Before/at the first message inspect each Composite
instance's `base+0x18030` pointer and `base+0x18188` initialized byte; a
breakpoint at the getter's `base+0x4435` identifies this specific message
source. Never assume the old bases survive a loader fix. Expected after alias
reuse: one instance for this owner, SystemMonitor getter at the initialized
provider, and no getter-originated disconnect messages.

## Other run6 observations

- The initial compositor admission succeeded (`new client`, guest pid `0x64`),
  with shared-memory allocation/mapping. No evidence currently ties this
  diagnostic to the process-identity or socket changes.
- `_Logs/vshruntime6.log:1005` reaches `HomeUI.EventManager.OnMenuOpened`;
  that is lifecycle evidence, not proof that Home pixels were presented.
- First disconnect text is line 1211; zero-valued CRSUI reports begin earlier
  and are investigated separately by the parent.
- Lines 3421+ report missing RNPS TurboModule
  `VshTmProgressEventShellUIModule` for NPXS40003; NPXS40141 appears as the
  shared `base_dll.ps.bundle.hbc` stack frame, not proof that this title crashed.
- Last `waitFlipForSystemBus` error `0x8002003c` is `ETIMEDOUT`.
- Parent identified the later process exit independently: Windows event 1000,
  PID 18600, `igvk64.dll` 32.0.101.9033, exception `0xc0000094`, offset
  `0x1688bd`, 2026-11-03 23:29:18. This does not explain the earlier getter
  diagnostic. Parent owns the next driver-exception capture.
