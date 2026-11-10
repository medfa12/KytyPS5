# Gallery USB enumeration audit (13.60 / local4.03)

Initial audit was read-only. Implementation was subsequently authorized; status
and fixture integration are recorded below. No builds or live runs performed
by this worker.

## Cause: success with an unwritten count

This is not a negative native return code. The real 13.60
`ReactNative.Modules.Vsh.dll.sprx`, `AutoMounterModule.GetUsbDeviceList`, RVA
`0x1b8890`, does:

1. Allocate a `uint32[8]` at `0x1b88b9`.
2. Initialize the signed count to **-1** at `0x1b88c7`.
3. Call `AutoMounterWrapper.getUsbDeviceList(array, out count)` at `0x1b88d9`.
4. Reject the promise if the return code is nonzero (`0x1b88e1` onward).
5. On zero return, sign-extend count at `0x1b8902` and call Array.Resize at
   `0x1b8914`. An unchanged -1 explains `ArgumentOutOfRangeException newSize`
   at the observed method offset `+0x88`.

The wrapper metadata (`_TempData/AutoMounterWrapper.cs`, UTF-16, lines
849-851) declares:

```csharp
[DllImport("libSceAutoMounterWrapper", EntryPoint="sceUsbStorageGetDeviceList")]
private static extern int getUsbDeviceListNative(uint[] deviceIdList,
                                                out int deviceIdNum);
```

13.60 eboot's native wrapper export table directly associates the string at
RVA `0xf66710` with the function relocation at `0x11a6108` (name pointer
at `0x11a6100`). The function is **NID `mryrNITeYvI`**, imported from
`libSceUsbStorageForShellUI`, module `libSceUsbStorage`.

Run7 `_Runs/vshruntime7/_kyty.txt:8854-8857` shows both eboot references,
including that table slot, bound to generic unresolved no-op thunks.
There is no later resolution for this NID in the captured log. The generic
handler ends in `return 0` (`src/loader/runtimeLinker.cpp:590`) and cannot
write enumeration outputs. Calls after the global log cap need not have
their own unresolved diagnostic. This matches the AOT failure precisely.

## Proven ABI

```cpp
int32_t sceUsbStorageGetDeviceList(uint32_t* device_ids, int32_t* count);
```

- Exactly two arguments. `count` is **output-only**, not input capacity.
- `device_ids` has capacity **8 IDs / 32 bytes**. No caller-supplied size.
- Status is returned separately from count; success is zero.
- NID: `mryrNITeYvI`.
- 13.60 export RVA: `libSceUsbStorage.sprx + 0xb910`.
- Local4.03 export RVA: `.../zecoxao/kek/system/common/lib/libSceUsbStorage.sprx
  + 0xb350`.
- Both files export the same address/NID through `libSceUsbStorage` and
  `libSceUsbStorageForShellUI`. ShellUI actually imports the latter:
  `[UsbStorageForShellUI_v1][UsbStorage_v1.0]`.

13.60 entry calls helper `0x25c0`; 4.03 calls `0x2da0`. Both helpers:

1. Check the AutoMounter IPC client exists; otherwise return
   **`0x80f40009`** (`SCE_AUTO_MOUNTER_ERROR_STATUS`).
2. Require both pointers non-null; otherwise return **`0x80f40002`**
   (`SCE_AUTO_MOUNTER_ERROR_INVAL`). Client-status check precedes pointer checks.
3. Pass fixed capacity 8 to the IPC marshaller (`0x8010` on 13.60,
   `0x81e0` on 4.03).

The identical marshalling layout uses method **`0x30015`** with one input
buffer (8-byte capacity value), and two output buffers: 32-byte ID array
and 4-byte signed count. Transport failure is returned as-is; otherwise
the method's separate status is returned. Early status/argument failures
do not write the ID array or count.

Managed error-code metadata independently identifies STATUS and INVAL in
`_TempData/AutoMounterWrapper.cs:1833,1847`.

## Empty-device semantics proved in the real server

13.60 `system_b/vsh/SceShellCore.elf`:

- IPC handler `0x17eae70` requires capacity exactly 8, returning INVAL for a
  different value. It calls `AutoMount::getUsbDeviceList2` at `0x3e62e0`.
- That routine selects boolean false and reaches
  `AutoMount::UsbDeviceManager::getUsbDeviceList2` at `0x41e450`.
- The manager checks initialization, locks its guest device list, initializes
  the result count to zero (`0x41e68e`), and iterates eligible registered
  devices. The iterator checks status/format flags; it is not a host USB scan.
- Empty list branches from `0x41e69b` to `0x41e6e7`; `0x41e6ea` writes
  **zero to the caller's count**, then returns status zero. It does not
  write any IDs when no devices qualify. NODEV is not the empty-list result.

Thus correctly reporting the emulator's currently empty guest USB-storage
device set is ordinary success with `*count = 0`, not an error or synthetic
device, and does not require touching the array's unused elements.

## Proposed narrow implementation for parent coordination

Add the genuine enumeration export to the existing
`UsbStorageForShellUI` implementation in `src/libs/libJscdSmall.cpp`, whose
`LIB_VERSION("UsbStorageForShellUI", 1, "UsbStorage", 1, 0)` already matches
the observed import. Register `mryrNITeYvI` with the exact two-pointer ABI.
Use only emulator-managed guest USB-storage state. The audited tree has no
USB-storage attachment/device registry yet; existing Init/Term and map
functions are log-only. An initialized empty guest-device set therefore
produces `OK` and a written zero count. Do not derive devices from host USB,
arbitrary host directories, or fabricate IDs.

To preserve the proven lifecycle/error contract, track service readiness in
the existing Init/Term path rather than introduce an always-successful
uninitialized enumerator. Run7 already calls `sceUsbStorageInit` successfully
at `_Runs/vshruntime7/_kyty.txt:50515`, before the Gallery failure. Its real
signature is `sceUsbStorageInit(ScePthreadAttr*)` (13.60 RVA `0x9810`), and
the logged argument is null, which is accepted. Native internal
`sceAutoMounterIpcClient::initialize(ScePthreadAttr*)` is NID `rZgSodboauI`
at `0x1030`, a separate entry; preserve shared readiness if that route is
also implemented. No need to preload the real IPC client to fix this HLE
service boundary.

Keep outputs untouched on validation/status errors, validate the output
memory using the project's guest-memory checks before writes, and write
exactly four count bytes. Do not change the managed resize or catch its
exception to hide the missing output.

Suggested focused verification: initialized empty list with count sentinel
-1 becomes 0/status OK; eight-ID buffer guards remain intact; null pointer
returns INVAL without writes; uninitialized service returns STATUS; then
parent's live Gallery run verifies promise completion and real UI progress.

## Authorized implementation handoff

Changed only production file `src/libs/libJscdSmall.cpp` and added fixture
`tests/UsbStorageEnumerationTests.inc`:

- Init now uses its verified optional pointer argument and marks the empty
  guest-owned service ready; repeat initialization is idempotent.
- Term uses its verified zero-argument ABI, retires readiness, and returns
  STATUS when already uninitialized. Additional firmware confirmation:
  `libSceUsbStorage +0x9da0 -> +0x1440`; missing-client path `+0x158f`
  returns `0x80f40009` at `+0x15bc`.
- New NID `mryrNITeYvI` checks readiness before output pointers, preflights
  32 ID bytes / 4 count bytes through `NpMemoryArena::RangeReadWrite`, and
  uses `FileSystem::CopyPathResultToGuest` for the exact zero-count copyout.
  No registered USB storage devices or host USB enumeration are introduced.

Parent integration in `VirtualMemoryAllocationTests.cpp`:

```cpp
// Top level, before the fixture namespace:
namespace Libs::JscdSmall::UsbStorageForShellUI {
void InitJscdSmallUsbStorageForShellUI_1(Loader::SymbolDatabase*);
}
// Inside the existing fixture namespace:
#include "UsbStorageEnumerationTests.inc"
```

Dispatch `TestUsbStorageEnumeration()` after `InitSubsystems()` in a dedicated
process, suggested flag `--usb-storage-enumeration-only` and CTest name
`usb_storage_enumeration`. The full-emulator target already contains the
normal HLE dependencies; a reduced target needs libJscdSmall, npMemoryArena,
npMemoryHeap and filesystem dependencies. No production .cpp inclusion.

The fixture checks the actual registered exports, the -1 count regression,
fixed eight-ID boundary, output-only count, lifecycle and error precedence,
canaries, invalid/overflowing addresses, read-only buffers, inaccessible
tails and split count writes, and reinitialization. `git diff --check` passed.
Compilation/execution remain parent-owned and pending; no build was started
during the live run.
