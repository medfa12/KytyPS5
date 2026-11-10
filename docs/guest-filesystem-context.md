# Launch-owned guest filesystem contexts

This change addresses ShellCore's native relative
`system/priv/resource_arbitrator_ro/ra-config.json` lookup. It does not change
the string, firmware JSON, native factory, or failure handling. With ShellCore
cwd `/`, normal stat/open/read reach the mounted firmware file.

## Ownership and startup

`GuestFsContext` is one shared object per executable launch. Threads of that
launch share the object; other launches receive copies of its state. The object
is not keyed by a module ID, PID, authority or capability. Existing Program IDs
remain a separate filter for legacy program-scoped mounts.

`Execute` constructs the explicit `/` root/cwd primary context and clones all
predeclared executable cohosts before pthread-main initialization and before
native libc, SSL, or shared-module initialization. Loading an SPRX creates no
context. Shared bootstrap initializers still execute under the primary context;
there is no name-based PSM/ShellUI ownership inference. Dynamic module startup
retains its caller's context. Once-per-load shared initializers are still not a
per-process module-instance model.

Pthread creation captures a strong reference before host thread creation. Entry
installs it; guest TLS/pthread destructors retain it. TLS ownership is cleared
after guest cleanup, record ownership at successful join or failed creation,
and reusable records are reset before publication. Main-thread records capture
the preinstalled context. Existing main-thread lifetime/exit behavior is not
redesigned here.

The record holds a plain shared_ptr, immutable from before pthread_create until
host join completes. The ownership getter is valid on the live target (including
its special APC), or for a caller that owns the pre-join lifetime and excludes
concurrent reclamation. It is not a general thread-table query, and joined records
must not be inspected. This deliberately avoids atomic<shared_ptr>'s internal
object lock: HostLockCriticalGuard excludes guest suspension, not special APCs.

The off-thread Windows signal helper captures the live target's context and
program before dispatch, then installs them on the helper. The strong context
reference survives target cleanup. It copies the immutable record even if the
APC interrupted startup's TLS installation; helpers never re-read that binding
after dispatch. This does not redesign the existing raw
`Pthread` lifetime used by signal-ucontext/swap-self machinery.

## Resolution and locking

Each kernel pathname operation captures cwd and a copy of mount metadata once.
Rename resolves both operands from the same snapshot. Open and mutations use
the same result for host pathname and readonly policy. Libc file-opening shims
and remove also consume a paired result; they do not re-read cwd on fallback.
Absolute infrastructure lookups can run without a guest context; unbound
relative lookups return EINVAL rather than inheriting a mutable primary cwd.

Mapping-only GetRealFilename retains its historical ability to map names whose
parents do not yet exist, for mount planning and owned creation. Syscalls use
validated lookup. Failed lookups retain the captured absolute name and policy;
only syntax-safe unmapped file paths can enter the existing sandbox fallback
after missing-parent ENOENT. Invalid components, failed dot traversal and trailing
directory requirements cannot use that route. The fallback's /data backing is
also selected from the captured mount/scope snapshot, not re-resolved later.
Existing direct-title license seeding and O_CREAT policy stay explicit; ordinary
stat does not create arbitrary sandbox files. If license seeding creates missing
parents, stat revalidates the original pathname using the same captured cwd,
mounts and scope before returning; it does not return the pre-seed ENOENT.
NPDRM persistence uses mapping-only
preflight before its existing authorized mkdir/create sequence, without replacing
activation contents or changing device/authentication policy.

Filesystem mutexes have HostLockCriticalGuard protection. Cwd snapshots and
generation-checked updates use short locks. Directory validation and host/archive
I/O run outside them; concurrent chdir commits cause re-resolution. Mount opening
and destruction of retired archive references happen outside the mount lock.
Operation snapshots keep archive references alive. No common file/archive API
or implementation is changed by this patch.

Empty paths stay ENOENT. Intermediate directories are validated before consuming
`.`/`..`; a trailing slash requires a directory (mkdir may create a missing final
directory). Mutation entry points retain terminal dot/dotdot metadata and reject
those operands instead of deleting/renaming their normalized parent. Supplied
pathnames are limited to 1023 bytes and components to 255;
accumulated cwd may be longer through short relative chdir operations. Guest
components are parsed before host path conversion. Windows drive/backslash and
forbidden filename syntax is rejected rather than interpreted as host traversal.

Before consuming `..` in a host-backed directory, lookup follows that directory's
host object. A target within the selected backing is rebased to its physical
guest spelling before visiting the parent; subsequent lookup uses the captured
mount namespace and paired readonly policy. Explicit mount roots still have guest
parents, and descendant mounts beneath a link spelling retain precedence. Thus
`/rw/link -> real/child` followed by `../target` reaches `/rw/real/target`, including
after chdir into the link. Namespace-only ancestors and archive traversal keep
their guest parent rules. Admitted host links outside the selected backing retain
their traversal spelling; host prefixes are followed before applying `..` rather
than letting Windows lexically collapse it. Such cwd values remain path-backed.

Visible mounts use longest component-prefix selection. A program-scoped mount
overrides a global entry at the same point, but cannot hide a longer global
child mount. The existing hashed common/common_ex alias is a mapping rule, not
a jail/root change. Visible child mounts establish traversable namespace ancestors
even when a broader mounted host backing lacks those directories. Archive mounts
are readonly regardless of their flag.

The previously disabled kernel unlink/rename/mkdir/rmdir readonly checks are
enabled using the resolved operation result. Rename no longer fabricates missing
parent directories, and rejects different selected mounts with EXDEV. Existing
non-filesystem HLE routines that directly mutate host files are not turned into
a complete VFS by this change.

## Cwd ABIs

`6mMQ1MSPW-Q` is `int chdir(const char*)`, with 0 success or -1 and guest errno.
It validates directory lookup before committing cwd. Readonly directories are
valid cwd targets. Host-reported access errors propagate; this is not a new
guest credential/ACL model.

`DYivN1nO-JQ` is `char* getcwd(char*, size_t)`. A supplied size 0 gives EINVAL;
insufficient capacity gives ERANGE. Copy faults give EFAULT. Null buffers ignore
the supplied size, start with 1024-byte storage and grow by doubling, matching
the audited 13.60 libc entry. Allocation failure gives ENOMEM. Storage comes
from GuestHeap, paired with the existing HLE libc free, never host strdup or
the runtime-global application-heap callback. This export remains in the HLE
libc allocation domain; it is not a mixed native/HLE allocator bridge.

## Deliberate boundaries

- Root stays `/`. The pre-existing chroot success-only stub remains unimplemented;
  this patch does not supply chroot, jail confinement or a security boundary.
- Cwd is path-backed, not a retained directory vnode. Rename, unlink/recreate,
  mount replacement and host filesystem races do not have full native cwd
  identity semantics. A removed cwd normally fails lookup instead of returning
  a fabricated path; replacement at the same spelling cannot be distinguished.
- Supported host symlinks continue to be followed by stat/open and directory
  traversal, including parent lookup after following a directory link. Absolute
  host-link targets are still host names, not guest-root-relative link contents;
  Windows reparse-point confinement and full guest-root link interpretation remain
  separate limitations. This is not a new sandbox boundary.
- Namespace-only root/ancestors permit traversal/chdir. This change does not
  fabricate stat identities or directory streams for unbacked namespace nodes.
- The separate C++ filesystem `_Current_get` stub and raw native `__getcwd`
  syscall/provider integration are not implemented by this HLE getcwd export.
- Bootstrap shared initializer workers remain primary-owned. Intended ShellUI
  ownership needs explicit launch/initializer metadata, not module-name guessing.

## Verification

The emulator and filesystem/archive/memory test targets built successfully.
The following focused selectors passed, along with the ordinary filesystem and
archive suites (CTest `guest_fs_context`, `filesystem_identity`,
`fs_sandbox_mounts`, `np_drm`, `kernel_file_system`, and `archive_file`):

On this Windows host, the long-cwd growth and symlink subcases were skipped:
directory creation reported a path error and symlink creation lacked privilege.
Their source-review coverage is not runtime validation. The real special-APC,
pthread ownership, ordinary path and exported cwd cases executed successfully.

```
kernel_file_system_tests --guest-fs-context-only
kernel_file_system_tests --filesystem-identity-only
virtual_memory_allocation_tests --fs-sandbox-mounts-only
virtual_memory_allocation_tests --np-drm-only
```

The firmware-free fixture uses the existing hidden Vulkan context because real
kernel reads invalidate graphics memory. It covers the exact relative config shape and identity
equality; early launch copies; PSM-style parent traversal; empty/invalid/component/
trailing-slash errors; readonly mutations and cwd races; scoped longest matching;
archive cwd; actual guest pthread sharing, destructors and pool reuse; captured
target helper binding; exported chdir/getcwd errno, size, pointer and allocator
behavior. Long-cwd allocation growth runs if the host supports those path lengths.
The revision fixtures also cover host symlink identity, child-mount ancestor
traversal, all terminal-dot mutation targets, unmounted O_CREAT, both libc license
fallbacks, captured /data across remount, and the actual first authorized NPDRM
mkdir/create route. Distinct-payload/identity fixtures cover symlink `..`, linked
cwd parent lookup, descendant and physical-parent mount overrides, mount-root
parents, and admitted links outside the selected backing. A first-call relative
stat fixture seeds missing license parents and verifies the returned file identity
against open/read. On Windows, special APCs copy the immutable record while a
live worker repeatedly changes its separate TLS binding. Full guest-handler
ucontext/APC-dispatch behavior still needs runtime integration validation.

Fresh bounded `guestfscontextboot1` used separate logged launch contexts:
ShellUI changed its cwd to `/app0/psm`, while ShellCore's unchanged relative
resource path reached the real firmware file and read all 10,333 bytes. Its
subsequent missing JSON lookup and `INVALID_ID` errors remain genuine failures;
the version-3 log does not establish valid rule population. The process later
faulted in `ShmOpen` on name address 1. No working UI was demonstrated.
A boot advancing past the prior path failure does not establish full filesystem
or native boot fidelity.
