# Isolated native NP work queues (13.60)

`SymbolDatabase::ExportPolicy::NpWorkQueue` is a bounded provider policy for
local emulator interoperability and native ownership/lifetime debugging. It
accepts function exports from the unchanged 21-NID `NpOwnershipExports` set
plus these 16 queue/work-item and four referenced-payload lifecycle NIDs:

```text
X6NVkdpRnog U0YoWwgg8aI +dGO+GS2ZXQ VnQolo6vTr4
CznMfhTIvVY eTy3L1azX4E zmOmSLnqlBQ 4DE+nnCVRPA
NeopmYshD0U 1QFKnDJxk3A KQSxXJBepQ4 laqZEULcfgw
WF2l-GUIlrw HldN461O2Dw ga4OW9MGahU Oq5aepLkEWg
JRtw5pROOiM 417JucZaE3g zurkNUps5o8 5tYi1l9CXD0
```

The original `NpOwnership` policy still exposes exactly its original 21
functions. `All` retains ordinary symbol-database behavior. The launcher flag
`-NativeNpWorkQueue` implies `-NativeNpOwnership` and sets
`KYTY_NATIVE_NP_WORKQUEUE=1`. Only `libSceNpCommon.sprx` receives this policy;
its accepted exports are preferred before HLE across resolution paths.
The exact-value queue opt-in also selects native-header-aware HLE mutex
destruction, because queue mode includes the same ownership family.

## Native boundary

The audited trophy path allocates a standalone `0xd0` WorkQueue through native
Ex-new. A no-op `X6NVkdpRnog` constructor leaves its vtable zero; acknowledgment
stubs for initialization/start then allow the object to reach native cleanup.
This policy permits the actual native queue/work-item implementation together
with its paired native ownership functions, including Ex-new and delete.

Native queue mutex, condition-variable and thread calls are internal direct
calls that ultimately reach libkernel. Their NP exports, along with NP heap,
global allocator, Arch and random exports, remain outside this policy. The
queue vtable's deleting function at `NpCommon+0x1f3c0` is reached through a
relative relocation and requires no additional exported NID.

Trophy tasks retain a separate payload object. `JRtw5pROOiM` initializes its
vtable and four-byte reference count at +8 to zero; `417JucZaE3g` is the
non-deleting base destructor. `zurkNUps5o8`/`5tYi1l9CXD0` atomically increment/
decrement that count and return its new 32-bit value. Final release dispatches
the payload's deleting destructor, which uses the existing native21 owner.
The task itself remains owned and deleted by the queue, independently of this
payload reference count. This is a 41-function provider, not broad NP exposure.

The standalone `pfJgSA4jO3M` increment retains its HLE binding, corrected to
return the **old** four-byte value as native `lock xadd` does. Its `-1 -> 0`
transition drives the first trophy scheduling predicate. The SDK getter WB66
must likewise return status and the executable's four-byte legacy SDK output.

Real trophy execution subsequently calls NpTrophy handle operations, beginning
with `cBzXEdzVzvs`; these remain a separate service dependency. Queue startup
does not establish their output/error semantics or successful trophy work.

## Filtering and regression boundary

Both `SymbolDatabase::Add` overloads apply the policy before records enter any
index. Rejected functions and all non-function types are absent from exact,
NID and name lookup, including the database paths used by `KernelDlsym`.
Library/module metadata is normalized by the existing symbol-name machinery.

`SymbolDatabaseTests.cpp` exercises the explicit ownership and queue sets,
ownership-only isolation, both insertion overloads, prefixed NpCommon metadata,
type rejection and unrelated-export exclusion. These are source-level provider
policy checks; native ABI integration, runtime binding and queue execution are
validated separately. No startup/result forcing or check bypass is part of this
policy.
