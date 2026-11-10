# Isolated native NP ownership (13.60)

`tools/run_shellui_1360.ps1 -NativeNpOwnership` appends the read-only firmware
`libSceNpCommon.sprx` provider and sets host `KYTY_NATIVE_NP_OWNERSHIP=1`.
The default launcher clears that variable. This mode does not load NpManager.

At symbol-database construction, only that filename with the explicit opt-in
receives `SymbolDatabase::ExportPolicy::NpOwnership`. The 21 function NIDs in
`npOwnershipExports.h` are the complete exposed export set. Excluded symbols
are never inserted into any database index, including exact, NID and name
indices used by late resolution and `KernelDlsym`. Import databases are unchanged.
The resolver prefers this filtered provider before HLE, including when import
module/version metadata is unavailable. Other providers retain their policies.

The native code supplies object new/new[], their paired deletes, allocator
dispatch/conversion, and string duplication. Its execution closure is supplied
allocator callbacks, direct internal identity conversion, and libc
`strlen`/`strnlen`/`strncpy`. Native startup has empty constructors and a returning
module-start routine. Startup/finalization use ELF entry addresses rather than
exports. Unrelated NP heap, thread, mutex, arch and service exports remain hidden.

## Ownership contracts

- Object allocation requests `size+0x10`, saves the descriptor at raw+0, and
  returns raw+0x10. Deletion recovers raw and calls its saved descriptor's free.
- ShellCore's worker-array cookie occupies another eight bytes after that
  header: request `0x558` produces callback request `0x568`, cookie at raw+0x10,
  and workers at raw+0x18. Array delete receives raw+0x10.
- Dispatcher and string allocations are raw callback allocations, without the
  object header. They pair with dispatcher free, not object delete.
- Descriptor ABI is `{allocate(size,ctx), realloc(ptr,size,ctx), free(ptr,ctx), ctx}`.
  The corrected manager provides persistent callbacks over its bounded arena.

The opt-in HLE mutex deleting-destructor adapter must respect the object header
after releasing its host mutex state. Mapping native object allocation while
retaining a Lock function in the deleting-destructor slot breaks native rollback.

## Verification

`symbol_database_tests` checks selected exposure, type rejection, exclusion of
actual unintended NP exports across all lookup paths, and unchanged ordinary
databases. Runtime validation additionally requires native new/delete GOT
addresses, actual callback allocation sizes and arena addresses, and unchanged
unrelated NP bindings. Symbol policy tests alone do not establish successful
firmware startup or visible rendering.

Full native NpManager startup remains a separate integration: ShellCore supplies
mode1 via WithArg. Eager default-argument module startup is incompatible with
that lifecycle and must not be used as a substitute.
