# AppDb: verified credential gate BEFORE connect acceptance

The AppDb native **connect callback does not check authinfo**. The earlier
handoff was incomplete in treating the method gate as the reason connection
support could not be enabled. A new read-only inspection establishes a separate
gate in **kernel server registration**, before a server can be published or a
native connect callback dispatched.

## Exact source chain

Addresses below are original 13.60 image virtual addresses; kernel offsets use
base `ffffffff80210000`.

1. ShellCore worker `ab1290` constructs the stack-owned AppDb handler and calls
   `hoWSsRT0ktA` at `ab131c`. Failure branches at `ab1323` to cleanup; only success
   reaches blocking dispatcher call `ab1349`. Existing retained disassembly:
   `_TempData/oobe-appdb-server-entry.txt`.
2. Original `libSceIpmi.sprx` export `hoWSsRT0ktA` is `1200`. It calls `13b0` at
   `124c`. This prepares the kernel server config and calls `3c30` at `170c`.
3. `3c30` supplies `(selector=0, flags=0, result*, args, 0x18, ...)` to
   `Hk7iHmGxB18` at `3c76`: both EDI and ESI are zeroed immediately beforehand.
4. Kernel `sys_ipmimgr_call` table at `+178057c`, index **0**, resolves to
   `+7dceea`. After copying the arguments it calls `+6f0b70` at `+7dcf0c`.
5. Registration `+6f0e43..+6f0e53` compares the copied server name's first three
   bytes against string `+1348f97`, directly verified as `53 63 65 00` (`Sce`).
6. `+6f0e58` loads the registering process credential pointer `[r15+140]`, then
   `+6f0e61` calls `+608f90` (`ffffffff80818f90`). This helper is exactly:

   ```
   mov rax, [rdi+60]
   shr rax, 3e
   and eax, 1
   ret
   ```

   It tests capability **bit62**, not executable name, authority, connect-method
   arguments or a server config permission flag.
7. For a matching `Sce` prefix, `+6f0e68` branches to `+6f0ea7`; a zero helper
   result branches at `+6f0ea9` to `+6f0f19`, which sets error **1** and takes the
   common failure path. Configuration copy/validation and publication occur
   only on the other path, starting at `+6f0eab`.

For the exact service name `SceAppDbIpcmsg`, successful native registration
therefore requires the registering **ShellCore** credential's bit62. This gate
precedes connection, event consumption and every protected AppDb method. The
client's method credential is a separate later requirement.

## Existing host implementation agrees

`src/kernel/ipmiGateway.cpp:129-132` implements this reserved-prefix/capability
test in `AuthorizeName`, called before server registration at147. The generic
Core is intentionally policy-free. `nativeIpmiTransportImpl.inc:647` requires
`authorize_registration` before `Core::RegisterServer`; `Install` refuses a
missing callback. Supplying an always-success callback solely for AppDb would
bypass the now-verified original registration gate.

## Validation and consequence

Read-only disassembly regenerated into
`_TempData/appdb-registration-policy-current.txt` using the existing helper:

```
python tools/inspect_oobe_ipmi_kernel_1360.py dis 0x6f0b70:0xe7 0x6f0e43:0xe9 0x608f90:0x11 0x7dceea:0x27
```

The jump-table entry and four prefix bytes were read directly through the ELF
program-header mapping; index0 resolved to `+7dceea` and the bytes to `Sce\0`.
Original IPMI export and the complete userland chain were independently decoded
with `_TempData/lifecycle-re-inspect.py`.

The minimum credential prerequisite for the proposed connection-only repair is
**verified active ShellCore credentials**, not necessarily ShellUI credentials.
Current boot compatibility caps are synthesized; the alternate authenticated
record is bound to a different ELF. Neither is valid evidence for this gate.
See `vsh-sm-authmgr-self-recovery-20261105.md` for active SELF hashes, metadata
extent, active public key and the missing active CCP41 decryption input.

Once ShellCore registration can be authorized truthfully, connection-only
integration can use real wait/publication/acceptance and pinned dispatcher
lifetime, while denying protected requests whose peer credentials are missing.
Without that prerequisite, publishing the server would still require an
unaudited authorization bypass. No production routing, identity, guest event,
readiness or credential changes were made in this pass; no runtime pass is
claimed.
