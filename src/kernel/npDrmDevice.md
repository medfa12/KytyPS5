# NPDRM unavailable-SM core

`Libs::LibKernel::NpDrm::Device` implements the audited unavailable secure-module
branch only. It exposes no license/activation verdict, backend success stub,
syscall registration or guest memory access itself. The filesystem adapter owns
**one device for the driver lifetime**, shared across all opens
and programs; closing/reopening a descriptor never resets it. Independent device
instances exist for independent test fixtures, not for production descriptors.

## State and requests

Initial state is attempted=false, class/error/length/time-adjust=0, SM handle=-1,
32 zero digest bytes, and an actual zero-initialized 16 KiB activation buffer.

| Request | Local buffer | Behavior |
| --- | --- | --- |
| `C0044E05` | 4 bytes | Every call sets length=0, attempted=true, error=`800f0a0e`; sets class=`10000` only when zero; clears the full activation buffer; attempts zero-length persistence; writes status=0. |
| `C0044E06` | 4 bytes | Writes status=0 only. No reset or persistence. |
| `C00C4E0F` | 12 bytes | Before init, preserves first 8 bytes and writes `800f0a32` at +8. After init writes class/error/zero at +0/+4/+8. |
| `C0144E10` | 20 bytes | Before init, preserves first 16 bytes and writes `800f0a32` at +0x10. After init writes the 16-byte state hash and zero status. |
| `C0344E18`, subcommand 3 at +8 | 52 bytes | Secure-clock query: writes operation status at +12; on failure preserves tick +0..7 and tail +16..51. Independent of NPDRM initialization. Production explicitly has no srtc provider and returns `800f0513` in the payload, with transport success. |
| `C0044EFF` | no core payload requirement | Audited unknown request: native ENOTTY (decimal 25), no output. |

The hash is the first 16 bytes of
`SHA256(6d99f4bf8d1e6d9b3f9578821dad6fb4 || digest[32])`.
The private digest is initialized to zero and **no implemented transition changes
it**, so the native-audited known answer `261c4bbf7482c9f563f44272e543f655`
is exact for every reachable post-init state. This is a state hash, not license
proof. A future digest-changing implementation must replace the constant with
SHA-256 of the current digest and extend regression coverage.

All payload words are little-endian. Larger local spans are accepted, but bytes
beyond the documented payload are untouched. Short spans return `InvalidBuffer`
without mutation; that is an adapter-contract error, not an invented syscall
error. `UnsupportedRequest` likewise has no native errno: the lead must handle it
explicitly. All unimplemented requests, including other `C0344E18` subcommands, receive that result
after authorization. Only the audited `C0044EFF` is classified as native ENOTTY;
other unclassified requests are deliberately unsupported pending their audit.

## Authorization and adapter boundary

`Authority` is the actual optional authority ID retained from the input's
structurally validated SELF metadata. Missing metadata remains unknown. No
filename, caller PC, capability flag, or test-mode boolean grants authority.
Open/close allow `4800000000000010` or `480000000000000f`; ordinary ioctls require
`...0010`. `...000f` is authorized only for `C0344E18`.
Unauthorized calls return raw native EPERM=1 before any mutation,
persistence or output changes.

`Result::transport_errno` contains raw BSD errno (0/1/25), not `0x800200xx` and not
POSIX -1. The syscall adapter owns translation. Only `Completed` permits copyout.
The adapter must perform native copyin first (bad pointer EFAULT precedes bad fd),
retain/validate the descriptor, obtain the current program's explicit authority,
then invoke the core on the local copied buffer. Preserved pre-init bytes must
come from that copyin. Copyout failure **does not roll back** an init or persistence.
Do not route these outcomes through ShellHits' unknown-request success fallback.

## Persistence and lock ordering

The required `Persistence::WriteActivation` host callback receives length=0 on
**every** init, including repeats. It must execute real guest filesystem
operations: ensure `/user/license` exists (mkdir mode `0770` if absent), then
open `/user/license/act.dat` with flags **`0x601`** (write/create/truncate), mode
**`0770`**, skip the write for zero length, synchronize, and close any opened
descriptor. A zero byte count must still open/create/truncate. Return filesystem failure normally;
the core intentionally ignores it and still acknowledges init. No operation is
performed by this core against a host path or user store.

Production experiments must route `/user` to an **isolated writable guest mount**;
native persistence truncates `act.dat`. Never map this callback to immutable
firmware or reference user data. `fileSystem.cpp` supplies the descriptor,
copy-ordering and persistence boundary; `kernelIoctl.h` shares POSIX translation
between the registered ioctl entry points.

One nonrecursive device mutex serializes state operations **and the complete
init persistence attempt**, matching native ordering. A host-critical guard spans
acquisition through release, including persistence; narrowing it further would
allow suspension while owning a lock needed by other guests. No logging/flush,
loader lookup, or guest callback occurs in the core.

Lock order is **device -> filesystem mount/table/file locks**. Future dispatch
must retain its descriptor and release filesystem table/file locks before entering
the device. No filesystem path holding those locks may call back into the device.
The persistence callback must not throw, invoke guest code, or reenter the device,
including `Snapshot()`. It must outlive the device. Open/close only authorize and
do not acquire the device lock. Snapshot returns a copy and cannot modify state.

## Integration/testing

`np_drm` covers the core and filesystem integration: authorization, descriptor
lifetime, copy ordering, persistence and shared ioctl bindings. Core fixtures
inject recording callbacks; filesystem fixtures use temporary isolated mounts.
The combined Windows build and focused CTests passed. Bounded native51/native54
runs created zero-byte `act.dat` in their selected isolated stores. This
acknowledges the unavailable-SM attempt, not activation success or complete
startup. A longer native54 run reached an explicitly unsupported NPDRM operation
that still requires attribution/audit.

## Secure-clock query audit (continuation)

13.60 kernel `ffffffff80b802e9` calls the separate srtc helper
`ffffffff8070dc30`; negative status jumps to `ffffffff80b80749` to write +12.
Success at `ffffffff80b804df` converts seconds since 2012 using
`(seconds + 63460972800 + time_adjust) * 1000000` modulo 64 bits.
The optional host `SecureClockRead` provider models this boundary, not activation.
Production passes no provider: there is no implemented srtc device or secure
clock cache. This selects the helper's missing-device branch at
`ffffffff8070dcad` (`800f0513`), rather than aborting or substituting host time.
Native srtc device initialization at `ffffffff8070e2d0` creates its own 64-byte
object and zero cache at +0x28; successful publication occurs at `8070e3bb`,
and failure clears its root at `8070e5b8`. Its uncached backend uses SM
`8002100D`, separate from NPDRM's `80021006`.

The focused `np_drm` test passes with wire-preservation, both authorities,
pre/post-NPDRM-init queries, provider epoch conversion, short buffer validation,
and production ioctl transport/errno coverage.
