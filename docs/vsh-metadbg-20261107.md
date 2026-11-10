# Regular 13.60 meta-debugger startup

## Evidence

`psn16` was not a clean boot: `_Runs/psn16/_kyty.txt:46753` records
`rip=0x90003daa8` and bytes `0f 0b`, and `_Logs/psn16.log:60` starts the
corresponding fault report. Reporting stops after the frame walk while other
threads continue. `psn17` and `psn18` reach the same fault and complete fatal
reporting. The retained allocator message is not this trap's origin. The exact
reason the earlier reporting thread stopped has not been established.

The source-hashed disassembly is
`_TempData/psn-deep-audit/syscore-metadbg-reader.txt`:

- `SceSysCore+0x3dba0` opens `/dev/metadbg` with `0x100002` (RDWR/CLOEXEC).
- It stores the descriptor at `+0x164aac`, logs open failure, and creates the
  reader anyway.
- Reader `+0x3da80` traps at `+0x3daa8` if the descriptor is -1.
- `+0x3dadc` reads a 32-byte event header, loops on zero, and dispatches the
  header/payload through `+0x3bed0`. Empty reads would spin; invented events
  would execute debugger commands.

## Implementation

Add `/dev/metadbg` as an idle character-device endpoint. A blocking read waits
on its retained descriptor's condition variable. Nonblocking reads return
EAGAIN. Close wakes readers with EBADF; descriptor reuse does not transfer
ownership of the old read. Switching to nonblocking wakes an existing wait.
There is no event producer or debugger-command backend. Writes return
EOPNOTSUPP, positional reads/writes ESPIPE, and ioctls ENOTTY. The device is
not mapped to a host backing file. No guest code or exception recovery changes
are involved.

Regression coverage in `tests/MetaDbgDeviceTests.inc` checks open/namespace
contracts, flags, empty/nonblocking reads, unsupported operations, blocking
wait, close/reuse, and untouched caller output.

## Validation

`kyty_emulator` and `kernel_file_system_tests` built. The final pre-initialization
ioctl guard was compiled/tested in the filesystem target after launching psn19;
the running emulator predates that guard and needs relinking after it exits.
`metadbg_device`, `sshot_device`, `posix_open`, and `anonymous_shm` pass (4/4).

`psn19` (PID 27852) uses regular 13.60, an isolated copy of psn16 system data,
host networking, local-account HLE, native libc startup, and VSH runtime.
`_Runs/psn19/_kyty.txt:47021` records successful device open; line47068 records
the reader waiting. No meta-debugger trap occurred through the captured startup.
`_TempData/psn19-home-observation/0003.jpg` visibly shows the PS-button prompt.
No controller input was injected. Home navigation/sign-in awaits user input.

The widened read-only popup scanner examined 3,052,198,912 bytes before UI
entry and found no managed width strings; this does not establish that the
constants are missing after RNPS initialization. Popup rendering remains
unfixed. A separate static review found suspicious integer-return ABIs for
`CToFloat`/`CToDouble` in libShellCoreFloor, but neither baseline nor psn19 logs
show calls to them, so no causal link to dialog width has been established.
