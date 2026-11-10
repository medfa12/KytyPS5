# Guest filesystem identity

`KernelStat` and `KernelFstat` expose a process-stable `(uint32_t st_dev,
uint32_t st_ino)` pair for ordinary files and directories. This is an object
identity, not a descriptor number, pathname hash, size, or timestamp.

## Native evidence

In the 13.60 ShellCore ELF, offsets `0x1034a15..0x1034aea` query `fstat`,
read the first two 32-bit stat fields, and look up SQLite's shared inode record.
The previous all-zero fields merged unrelated database handles. Native
`unixLock` at `0x10360ee` rejects EXCLUSIVE escalation when that inode record's
shared-holder count exceeds one, returning `SQLITE_BUSY` before the exclusive
`fcntl` call. The default busy handler retries until its timeout.

The `nativesslboot2` log records BGFT's RESERVED/PENDING locks at lines
48696–48720, no further descriptor-22 fcntl until line 262495, and then
`nRef=5, nLock=2, eFileLock=3`. `COMMIT` returns SQLite result 5. The conversion
at ELF offset `0x115791b` ORs it with `0x81880000`, producing `0x81880005`.
`GETLK` belongs to error reporting after this failure. The missing
`nobackup/sqlite/SceShellCore.dat` is a separate record-file sidepath.

## Implementation

* Windows queries the existing `HANDLE` with `GetFileInformationByHandleEx`
  (`FileIdInfo`): full 64-bit volume serial and 128-bit file ID. A filesystem
  reporting that query unsupported can supply the legacy volume/file-index
  pair through `GetFileInformationByHandle`. Other errors propagate.
* POSIX queries `fstat(fileno(stream))`, retaining full host device/inode
  values. Metadata-only descriptors use `O_PATH` on Linux and `O_EVTONLY` on
  Darwin; they are closed with `close`, independently of stdio streams.
* Path identity queries own a temporary metadata handle and close it through
  `Common::File` RAII. Windows opens metadata with read-attributes access,
  read/write/delete sharing and backup semantics, including directories.
* Guest directory descriptors retain a metadata handle until close/shutdown.
  Descriptor identity never reopens the saved pathname, so rename, replacement
  of that pathname, hard links and unlink-while-open preserve object identity.
* Archive members use the backing host identity plus the archive's entry ID.
  This preserves case/path aliases while distinguishing members and the host
  archive. ZArchive's owned random-access input reads through one retained
  `Common::File`; identity is captured from that same handle. Member IDs come
  from the retained reader. File and directory metadata views retain that
  reader rather than reopening the backing pathname. Cached reader A continues
  to expose A's bytes and identity after rename, replacement by B, or unlink;
  after A's last owner closes, a fresh reader opens B. A failed identity query
  is retained and returned by stat without disabling supported archive reads.
* `FileIdentityMap` compares complete keys and allocates nonzero 32-bit device
  and inode numbers. No truncation or hash collisions are possible. Entries
  remain for the process lifetime, including filesystem reinitialization;
  numbers need not match between emulator processes. Counter exhaustion fails
  with `EOVERFLOW`; exception-enabled builds translate registry allocation
  failure to `ENOMEM`. No-exception builds retain the toolchain's allocation
  failure behavior.
* A short `HostLockCriticalGuard` surrounds the registry mutex. Handle queries
  run outside that mutex. Descriptor fstat/close retain their per-file mutex
  and corresponding suspension exclusion while accessing the handle. Failed
  queries never publish a zero/fabricated successful identity or overwrite
  the guest output stat.

Host filesystems may reuse an inode/file ID after the underlying object has
been deleted and all handles closed, as on the native filesystem. Identity
is guaranteed for live objects, not as a permanent historical object UUID.

### Admission and existing device behavior

Host identity is admitted for ordinary host files, directory metadata handles,
and archive entries with a retained backing-file handle. A failed host query
returns an error; it does not fall back to a pathname, descriptor number or
all-zero successful identity. Memory-only `Common::File` objects have no host
identity and the new identity API reports `bad_file_descriptor` for them.

Synthetic guest devices, pipes and standard descriptors keep their existing
stat/fstat branches and error behavior; they are outside this host-file
identity contract. NPDRM retains its existing explicit unsupported-stat path.
The fixture covers archive and directory admission in addition to host files.
Guest path resolution, including relative-path/cwd semantics, is unchanged.

## Focused verification

Build target: `kernel_file_system_tests`.

Run `ctest -R '^filesystem_identity$' --output-on-failure`, or
`kernel_file_system_tests --filesystem-identity-only`. The focused path needs
no Vulkan window and uses an isolated temporary directory with normal cleanup.
It checks:

* Empty database/journal files produce distinct native SQLite inode keys.
* Path/descriptor agreement, multiple opens, reopen, lexical/mount aliases.
* Hard links and symbolic links; explicit platform-support/privilege skips.
* External rename and old-path replacement, unlink while a handle remains open.
* Directory-handle identity across external rename/replacement.
* Concurrent first identity allocation and full-width synthetic collision keys.
* Failed queries/closed handles and unchanged output on failure.
* Archive member/directory aliases and separation from the backing file.
* Distinguishable archive A/B payloads across backing rename, replacement and
  unlink, existing/new cached opens, path/descriptor metadata, directory-only
  reader retention after unmount, and fresh B after releasing A. These lifecycle
  operations are required on POSIX and Windows (the input uses delete-sharing).

The maintained `3rdparty/patches/zarchive-reader.patch` includes the input API
and existing range/tree/name/short-read hardening. Its application must be
checked offline against pinned upstream `d2c717730092c7bf8cbb033b12fd4001b7c4d932`.
The already-populated `_Build/windows/_deps/zarchive_source-src` is updated
explicitly too: FetchContent may not rerun a patch when only its contents change.
Parent validation should run `archive_file`, `filesystem_identity`, and
`kernel_file_system` after rebuilding the affected targets.

The fixture exercises SQLite's actual key contract; it does not load firmware
or replace SQLite with a mock database. The full `kernel_file_system` test also
includes the identity fixture.

Windows validation passed `archive_file`, `filesystem_identity`, and
`kernel_file_system`. In the fresh 13.60 `filesystemidentityboot1` run, native
BGFTDB initialization completed in 59 ms rather than the previous 31.4-second
BUSY failure. Read-only captures observed BGFT ready at 4 seconds and non-null
preparation/recovery owners at 8 and 14 seconds. The process subsequently exited
with a host `std::bad_alloc`; this is not evidence of completed ShellUI boot.
