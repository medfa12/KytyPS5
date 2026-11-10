# Isolated native NP HTTP pool (13.60)

`-NativeNpHttpPool` / exact host `KYTY_NATIVE_NP_HTTP_POOL=1` selects 54
NpCommon functions: unchanged ownership21, queue/payload20, callout10, plus
exactly these three exports (offsets relative to native NpCommon):

| NID | Operation | Offset |
| --- | --- | --- |
| `PznfSvchYJ8` | Constructor | `0xeb90` |
| `Vzob5RCgfnY` | Init | `0xecf0` |
| `jCeUP0CpiNs` | Purge idle | `0xf060` |

The separate opt-in implies callout, queue and ownership, including the
native-header-aware HLE mutex destructor adapter. Other values do not enable
it. Only the selected NpCommon provider is filtered and preferred over HLE;
ownership-only, queue-only and callout-only remain 21/41/51, and `All` is unchanged.
ShellCore imports constructor/init; NpManager imports all three.

The native pool is `0x40` bytes: +`0x00` vptr (NpCommon+`0x48498`), +`0x08`
borrowed Ex allocator, +`0x10` maximum, +`0x14` count, +`0x18` head, +`0x20`
tail, +`0x28` mutex vptr (NpCommon+`0x48be0`), +`0x30` handle, +`0x38`
initialized. Init creates the mutex, sets the requested capacity (ten for
ShellCore's trophy pool) and an empty list;
it creates no HTTP connections until acquire. The audited empty lifecycle
and SDK bindings address cleanup of an unconstructed pool at ShellCore
`0xe91fdd` after SNS `0x80550005`.

Cleanup remains native: deleting destructor `0xec60` reaches native `0x18700`
raw-header deletion. Vtable/direct destructors need no additional exported
aliases. The borrowed allocator must remain alive and all borrowers must be
quiescent before deletion. Acquire, request and other HTTP exports remain
outside this policy. This does not establish a working HTTP service backend.

For isolated launcher experiments, an explicitly nonempty `-SystemDataPath`
also creates and mounts its `data` and `original_user` directories at `/data`
and `/original_user`, alongside the selected `/user` and `/system_data`.
The launcher logs these roots; these new directories copy no shared user data.
