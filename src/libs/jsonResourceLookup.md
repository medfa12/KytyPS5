# JSON lookup/container ABI repair (V2)

This supersedes the rejected destructor-only V1. It is an HLE-only supported
subset, not complete native JSON parity. Do not mix its objects with native
Json2 objects. Main authorized application after independent review
`ses_e66ca1986ffewO5CV5anutbiY4` found no blocking ownership/ABI/registration
issue. The 57 central alias names still require independent provenance review. Main confirmed the
post-cwd prerequisite in `guestfscontextboot1`: a real 10333-byte read followed
by MsMO/INVALID_ID. The reviewed patch SHA-256 is
`d3021347a8e327901a4175d5404d3caaf9eff7cb5c1d1f5918c19e713eaff3cc`.
All five baselines were rechecked before patch-tool application. The emulator
and filesystem/memory test targets built successfully. All seven focused CTests
passed: json_resource_lookup, guest_fs_context, archive_file, filesystem_identity,
kernel_file_system, fs_sandbox_mounts, and np_drm.

## Native evidence

Read-only reference: local 13.60 `libSceJson2.sprx`, SHA-256
`d120fa2e769d938be3cfd89759e6c280726d2cd38bc0ffb327e3177f4109f540`.
The symbolic names below are implementation labels; this note does not establish
an independently distributable source for their original naming inventory.
Actual export identity is `[Json2_v1][Json_v1.1]`; module `Json2` is compatibility.

`MsMOdxWfbwQ` is const `Value::getValue(const String&)`, RVA 0x5100.
`clF7J7N9xXE` jumps to it from 0x54c0. C-string lookup `HwDt5lD9Bfo`
wraps it at 0x54d0. Mutable `wLsJlmgEIaI` at 0x45a0 is nullable `referValue`.
Present lookups borrow the actual inline member; missing const lookups retain
the existing no-handler static-null behavior without inserting or converting.

Object iterator RVAs: default 0xc4c0, copy 0xc4f0, D1/D2 0xc540,
assignment 0xc570, prefix 0xc5b0, postfix 0xc650, equality 0xc6f0,
inequality 0xc710, arrow 0xc730, begin 0xc810, dereference 0xc880,
advance 0xc960, end 0xcd80, find 0xcde0, insert 0xcee0.
Hidden producers use RDI=result, RSI=Object, RDX=key/Pair where applicable,
and return result in RAX. Constructors have void ABI. Postfix `dIVxrrdpS9M`
returns the updated receiver address, not a previous iterator or hidden result.
Default/copy iterators own independent state. Assignment updates existing
state or releases it for a null source node; it does not allocate missing
destination state. Both comparisons return false for a null implementation.

Independent review found Core 0x497cc3/0x497cda end/find hidden outputs reaching
D1 at 0x497cf2/0x497cfb. Retaining no-write producers while repairing D1 would
delete uninitialized stack contents. V2 replaces the producers and consumers
together. Each Object owns actual ABI Pairs; all iterators and lookups borrow
the same inline Value. Iterator destruction cannot destroy a Pair. Object
destruction/clear releases keys and nested values; copies deep-copy Pairs.

Array postfix `GEylwSsiGG8` at 0xb510 also returns the updated receiver.
`T3WReElX4BY` at 0xb450 is iterator assignment; container assignment is
`w5UbvPLGye0` at 0xc090. V2 allocates independent iterator state containing
owner/index with a stable end marker. Registered default/copy constructors,
D1/D2 aliases, assignment, dereference, arrow, increment and advance use it.

## Resolution and fixture

`InitJsonContainers` is the supported registration inventory, registering
actual `Json` before compatible `Json2`, both to identical implementations.
The early libC end/find/!=/== entries and late ShellHits end/find/!=/D1 entries
are retired. Their unrelated exports remain. Exact lookup is last-wins while
NID/name-prefix fallback is first historical record; both must be coherent.

The fixture invokes the three production registrars in order:
`LibJson2Iter::InitJson2Iter_1`, `LibJson2::InitNet_1_Json2`,
`ShellHits::InitShellHits_1`. It checks exact/compatibility/FIRST resolution,
historical registration consistency, registered SysV calls, poisoned hidden
outputs and canaries, found/missing/empty/nonempty paths, insert/duplicate
insert, Pair identity and primitive mutation, copies/assignment/self-assignment,
postfix receiver returns, and destruction. Test-only class allocation operators
count actual iterator-state and object-owned Pair allocation/release. These
counters do not measure all JSON allocations or exist in the emulator target.
Constructors are invoked with void function signatures, without RAX assertions.

The integration entry is `kernel_file_system_tests
--json-resource-lookup-only`, also CTest `json_resource_lookup`.

Additional test-only coverage checks nested Object/Array/string deep copies and
assignment over nonempty destinations, destroys the source before accessing
copies, mutates one copy independently, then destroys it before mutating the
survivor. Array growth coverage retains both an element and an existing end
iterator across 1024 appends and traverses appended values to that original end.
Both iterator families cover assignment from a null node and a released state,
followed by assignment into released destinations without allocating new state.
Resolution coverage includes all 57 supported central entries and alias groups.

## Bounded runtime evidence

`jsonresourceboot1`, PID33472, used emulator SHA256
`264d7c002e21c13b8fa82213a11dd0a86b0ca8cf6cf9d2e16e49871b839d7aa6`.
The unchanged firmware configuration was read in full (10,333 bytes), and both
ShellCore and ShellUI's exact MsMO imports resolved to JsonValueGetValueKey.
The retained log contains no INVALID_ID resource errors.

The read-only 4-second capture traversed 32 native components and 46 states.
Explicit config fields and verified defaults matched; group counts were 5/1,
budgets 100/6000/399 and version 3. All captured ranges reread unchanged:
260 reads, 15,506 bytes, approximately 7.8ms. This is concurrent, non-atomic,
lifetime-unpinned evidence; 176 unproven default fields remain unassessed and
group contents were not traversed. It does not establish whole-service parity.

The window remained black with colored top lines. The run exited at 8.68s in
the separately confirmed ShmOpen(SHM_ANON) pointer-dereference failure. Shine,
initial setup, and working home input remain unverified.

## Guest file parser

`LB3jxppxyKU`, native RVA 0x6980, now uses its two-argument path ABI and
guest KernelOpen/Read/Close, preserving launch cwd and mount selection.
The demand stream handles short reads and requires real EOF; raw NUL and
trailing documents cannot turn into successful parsing. Open ENOENT maps to
0x80848103; other returned I/O errors pass through. Returned close errors do
not replace the parse result. Resource loading still uses `S5JxQnoGF3E`.

Conversion owns temporary children/Pairs until insertion, then commits the
completed tree while retaining destination parent/rootparam and Pair identity.
Unlike native malformed-input parsing, this HLE preserves the destination until
success. Initializer buffers, custom allocators, callbacks and root fallback
remain unsupported. The 4096-byte transport buffer is an HLE choice.

`json_path_parser` exercises the registered ABI with real disposable guest files,
mount/cwd selection, nested and multichunk input, short reads, malformed/trailing
input, NUL boundaries, metadata retention, and descriptor cleanup for returned
errors. Test-only read wrappers induce real kernel EFAULT; close-error injection
follows a successful real close. Allocation injection covers recursive
materialization checkpoints only. The emulator/filesystem targets built and all
eight focused CTests passed, including `json_path_parser` (1.33s).

General OOM recovery is not established: KernelOpen can allocate after publishing
a descriptor, KernelClose can allocate before removal inside noexcept cleanup,
and nlohmann noexcept DOM teardown allocates. These dependency exception paths
can leak or terminate; they are excluded from the cleanup/NOMEM certification.
Wrapper allocation, container insertion and all heap allocations are not
exhaustively tested by the recursive checkpoints or Pair/iterator counters.

`jsonpathboot1` used emulator SHA256
`b3103160eb325e2ea3c4b0d16d9868edc760d98e430f6607d79155f8d64b398b`.
BGFT logged two actual parser returns of 0x80848103, replacing the former
forced success. A retained LB3 call used the native `/update/notification.json`
pointer; that entry trace does not record its return. Missing-path resolution
can return before KernelOpen's path diagnostic, so no per-path open result is
inferred from absent log lines.

The 8-second capture retained 32 components/46 states matching the configured
fields, with 130 equal rereads, 260 reads/15,506 bytes and the same 176 unassessed
defaults. The prior non-atomic/lifetime/group-content limits still apply. The
window remained black with colored top lines, and the process stopped after
9.76s at ShmOpen's sentinel dereference. No UI success is established.

## Explicit remaining boundaries

- Native lookup failure includes version-gated element-access-failure callbacks
  (missing key 0x80848115, non-object 0x80848117) and root/global fallback.
  This is not the null-access callback. No callback/root behavior is invented
  here; the existing no-handler static-null subset is retained.
- Only the central inventory is admitted. Legacy Object erase `3ZhL-MkICwI`
  in libJscdMisc and other unsupported operations are not repaired. Unrelated
  Value/String constructor implementations retain their pre-existing returns;
  callers must not rely on those returns as constructor ABI guarantees.
- Custom allocators, allocation failure, version/root propagation, invalid iterator
  dereference, cross-container comparisons, and container mutation invalidation
  beyond the covered insertion/growth cases are not certified native-compatible.
- The bounded population evidence above does not establish successful ShellUI
  boot or whole JSON compatibility.
