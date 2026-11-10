# Anonymous shared memory

The initial implementation supports the 13.60 sequence
`shm_open(SHM_ANON, 0x202, 0644)` → `ftruncate(fd, 0x30000)` →
RW `MAP_SHARED`. `SHM_ANON` is pointer value 1, not a pathname. Named PSM
shared-memory handling retains its existing route.

## Ownership and transactions

Descriptors retain an anonymous object; each mapped 16-KiB view independently
retains that object and its section. Descriptor I/O and all aliases use the same
section bytes. Closing/reusing a descriptor cannot redirect an existing mapping
or a retained operation to a replacement object.

Metadata is prepared before host mutation. Failed unpublished map cleanup retains
quarantined ownership and excludes that VA from reuse. Failed changes to existing
views restore their previous mapping/protection; inability to restore is fatal.
Shutdown drains mapped and quarantined views before releasing VA reservations.
Splits preserve object-relative offsets, mapping identity and permissions.

CPU-only SHM and PRT apertures reject overlap in both operation orders under the
memory-operation lock, before aperture/GPU mutation. Backing readers stabilize
ownership with the address-space lock without acquiring memory-operation or GPU
locks. Retained ordinary-file reads preserve EOF-bounded renderer invalidation.

## Native-informed subset

- New objects start at size zero with CLOEXEC. Anonymous open requires O_RDWR;
  O_CREAT is optional.
- Scalar reads/writes stop at logical EOF; writes never extend. Positioned I/O
  preserves the descriptor cursor. Buffer access follows validation and EOF
  bounding, so a zero actual transfer does not touch the supplied buffer.
- Growth preserves bytes already present in the final mapped page. Supported
  shrink clears the retained tail and discards subsequent pages.
- Stable identity, logical size, page count and tracked timestamps describe the
  actual object. Requested `uint16(mode) & 0777` is retained; effective umask and
  ownership are unmodeled, and zero uid/gid fields are not credentials.

Windows-only creation/mapping is supported initially. EOPNOTSUPP identifies
affected-live-map shrink, GPU/execute/private/fixed mappings, unaligned SHM
offsets, whole pages beyond EOF, O_SYSTEM, mixed-owner mprotect/munmap, vectored
SHM I/O and unsupported fcntl changes. Common malformed arguments still fail
before kind-specific dispatch. These restrictions do not claim native rejection
of those operations. Beyond-EOF fault delivery, exhaustive copy-fault accounting,
native budget/extreme-size policy and complete metadata parity remain unverified.

## Verification

`tests/AnonymousShmTests.inc` uses registered ABIs and disposable files. The
Windows filesystem test runner has separate core/file regression, descriptor-first
shutdown, memory-first shutdown, and fatal rollback modes. CMake explicitly enables
exceptions for memory.cpp and fileSystem.cpp in production and test compilations.

Coverage includes coherent aliases, close-before-unmap, descriptor reuse, EOF and
zeroing, offset/protection splits, PRT isolation, mixed-owner rejection/backing
reads, ordinary libc mmap invalidation, failed host operations and quarantine.
Prepare injection is three coarse checkpoints, not exhaustive STL allocation
failure interception. Host-failure hooks model outcomes rather than proving real
host resource exhaustion. Timestamp tests do not control clock advancement;
invalidation observation proves invocation/span rather than full GPU coherence.

The emulator and both test targets build. All 13 focused CTests pass, including
the five SHM cases, and the existing memory-only suite passes. Actual Windows
preprocessing with KYTY_FINAL also confirms the inspected cleanup/rollback
operations remain active; this is not runtime FINAL or whole-VM certification.

The first visible boot (`anonymousshmboot1`) was stopped at the log cap after
6.44 seconds. It repeatedly read the named SceGlsSharedMemory file and reported
illegal SysCore button events. The captured resource state was not yet published;
the window remained black. That run does not establish execution or success of
the anonymous native sequence, nor successful ShellUI boot. Descriptor-lifetime
investigation is pending before another run.
