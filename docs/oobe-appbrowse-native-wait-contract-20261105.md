# OOBE AppBrowse: native wait-for-server contract — 2026-11-05

## Finding

The native AppBrowse initialization path uses IPMI **connect with wait for
server**, with a null timeout. The HLE discards that wait and returns success
before the service exists. This supplies the missing ordering contract behind
the fresh-profile event-flag failure. It is no longer necessary to infer that
contract from relative log timing alone.

The finding does not establish a running corrected OOBE path. Actual native
acceptance, dispatcher lifetime, safe callback ownership and teardown still
need implementation/validation together. A named flag or delayed fake connect
success would not provide those contracts.

## Native source identity

Surviving decrypted firmware kernel:

`${USERPROFILE}/Downloads/13.60/decrypted_modules/x86_kernel.elf`

- Size: 47,451,147 bytes.
- SHA256: `7b55225db09dc39ce46ec4123905d22e061eca8a35ca08d6ac4873f15216d379`.
- First executable LOAD virtual base: `0xffffffff80210000`.
- All kernel `+offset` references below are relative to that base, not file
  offsets. This file contains actual readable kernel code and IPMI strings.
- `kernel.dec.bin` is an SLB2 container; its filename alone does not identify a
  flat decoded kernel image.

Read-only inspection helper: `tools/inspect_oobe_ipmi_kernel_1360.py` (Python +
Capstone; no guest execution or firmware writes). It supports bounded `dis`,
string `xref`, and candidate direct `calls`. Candidate xrefs must be confirmed
against decoded containing functions. Existing userland evidence uses
`_TempData/lifecycle-re-inspect.py`.

## Userland call and kernel selector

1. ShellUI `+0x550cae` loads its client. At `+0x550cc0` it clears R8, then calls
   client vtable `+0x80` at `+0x550cc6`. Argument/size are zero; the result pointer
   is real stack storage. Only after successful connect does it open
   `SceAppBrowseEventFlag` at `+0x550d4d`.
2. Native 13.60 `libSceIpmi.sprx` client vtable is at `+0xc010`:
   - slot `+0x80` points to `+0x4e0`:
     `(self, argument, size, result, uint32_t* timeout)`;
   - slot `+0x10` points to `+0x490`, which calls `+0x80` with a pointer to a local
     **zero** timeout. A null timeout and pointer-to-zero are distinct.
3. Wrapper `+0x4e0` calls `+0x4040`, which marshals 32 bytes for syscall selector
   `0x400`. It retries EINTR (`0x80020004`), and `0x80020025` while the timeout
   pointer is null or the pointed timeout is nonzero. This is not an arbitrary
   userland sleep/retry added by the emulator.
4. Kernel `sys_ipmimgr_call` is at `+0x7dcd40`. Selector `0x400` indexes the table
   at `+0x1780c8c`, reaches `+0x7dce9b`, copies the caller arguments and invokes
   `+0x395240`.
5. The kernel's own diagnostic at `+0x395f0c` names that function
   **`syscallConnectWithWaitServer`**.

## Actual missing-server handling

- `+0x3953a2` examines the timeout pointer. Null goes through `+0x3955aa` with no
  deadline; nonnull reads a 32-bit timeout and derives the timing state.
- Server lookup at `+0x396850` walks the native server list, compares the service
  name (32-byte bound), and checks the published server state. It returns `3`
  when no qualifying server exists. This is not successful connection.
- The caller at `+0x395a11` distinguishes absent server with null timeout from
  pointer-to-zero. The latter takes the immediate missing-server branch;
  the former proceeds through `+0x395b6a` with a null server record.
- At `+0x395bb6` the null server record branches to `+0x395eb2`. The kernel
  publishes connecting state `0x02000000`, retains/enqueues the request through
  `+0x396a90`/`+0x396b30`, and enters `+0x396b80`.
- That helper links waiter bookkeeping, releases object locks/references in
  the native order, and sleeps at `+0x396ce7` using the literal wait-channel
  name **`IpmWSv0`**. After wake it reacquires/revalidates the client identity and
  connection state. Thread/process cancellation and disappearance are distinct
  outcomes, not a connected response.
- Native acceptance remains a subsequent operation. Discovering a server must
  not by itself populate the method/acceptance result as success.

## Producer side and failure chronology

ShellCore `+0xab13d0` creates and clears `SceAppBrowseEventFlag` **before** starting
the AppDb server worker. That worker (`+0xab1290`) creates its native IPMI handler
on its stack, creates the service at `+0xab131c`, and calls its dispatcher at
`+0xab1349`. Returning from dispatch frees working memory and destroys the
handler. Thus actual connection acceptance cannot precede this event's creation.

Fresh `oobe5` contradicts that native sequence:

- `_Runs/oobe5/_kyty.txt:43954`: HLE connect success with no native acceptance.
- `43970–43971`: event Open fails `0x80020003`; AppBrowse initialization fails.
- `44091`: ShellCore creates the named event afterward.
- `44156–44159`: HLE server creation and immediate dispatcher return, followed
  by worker teardown.

The previously established DbAccessor publication/error path and DBP callback
then explain the null-singleton crash. Original fault register capture remains
unavailable; this audit establishes the earlier contract violation independently.

## Coherent implementation boundary

An actual AppDb route needs an exact-name client/server adapter and retained
native dispatcher execution. It also needs **pre-registration wait semantics**,
not just a connection ticket issued after a server already exists. Account for
null/zero/finite timeouts, canceled/destroyed clients, server publication races,
same-name replacement and stopped dispatchers. Only a native acceptance reply
permits successful connect; only a native explicit method reply completes a
request. Every waiting operation must retain the required backing until its
actual completion/cancellation.

The existing exact NP/LoginMgr adapters have no client slot `+0x80` and currently
query an already-existing server before Connect. Their current component results
do not prove this newly recovered contract. Generic `ClientConnectTimeout` in
`libC.cpp` discards the timeout and delegates to false-success ClientConnect.
No production source was changed by this kernel investigation.

## Retained bounded artifacts

- `_TempData/oobe-appdb-server-entry.txt`
- `_TempData/oobe-ipmi-connect-timeout.txt`
- `_TempData/oobe-ipmi-connect-syscall.txt`
- `_TempData/oobe-kernel-ipmi-{selector,dispatch}.txt`
- `_TempData/oobe-kernel-connect-{handler,wait,pending,queue,publish}.txt`

The `pending` artifact contains only the successfully decoded `+0x396b80`
region; an earlier requested mid-instruction region produced no output. The
`queue` artifact separately decodes the actual `+0x395ea4/+0x395eb2` path. The
committed helper now rejects a wholly undecodable region instead of silently
printing nothing.
