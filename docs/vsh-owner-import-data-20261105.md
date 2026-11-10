# Owner-local imported data — 2026-11-05

## Implementation

`src/loader/applicationImportData.cpp` replaces four exact audited host-object
imports with owner-pinned guest RW cells:

| NID | Object | Initial value |
| --- | --- | --- |
| `f7uOxY9mM1U` | stack guard | legacy `0xDeadBeef00000007` |
| `djxxOmW6-aw` | program-name pointer | owner executable filename, bounded to 511 bytes |
| `P330P3dFF68` | libc flag | separate 32-bit value 1 |
| `ZT4ODD2Ts9o` | libc-internal flag | separate 32-bit value 1 |

The guard is the existing emulator policy, not firmware-randomized semantics.
Selection requires the exact registered NID, library/module versions, object
type, expression and original address. Nonzero ELF declared sizes must match;
signed addends must remain within the selected object. Unknown host objects stay
unsupported.

Relocation, hard admission and dynamic lookup use the same owner-local storage.
Private modules share their executable owner's cells; other owners and subsequent
launches get distinct cells. Publication is serialized against owner stop. Stored
pointers remain pinned after stop, matching retained private module mappings.
The session still uses its original objects.

## Integration and validation

The existing CMake `CONFIGURE_DEPENDS` loader source glob includes the new `.cpp`
in emulator and full-emulator test targets. The parent registered
`runtime_application_import_data` in CTest and the VM-test dispatcher.

Its fixture exercises production registrations, actual guest relocation slots and
`KernelDlsym`: two owners, private modules, session preservation, typed registration
and size/addend refusal, mutable-cell isolation, stop rejection, retained pointers,
and fresh relaunch storage. The full emulator build and all eight selected
loader/lifecycle tests passed in `_TempData/vsh-owner-import-data-build.log` and
`_TempData/vsh-owner-import-data-tests.log`.

The previous visible Apple TV and YouTube launches failed before entry on these
objects. A new visible run is required to establish progress beyond preparation.
