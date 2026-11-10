# Native database lifecycle checkpoint — 2026-11-07

## Result

Implemented exact-name native transports for `SceDbPreparationServer` and
`SceDbRecoveryShellCore`, installed and retired with the source-bound AppDb
owner. Five standalone transport tests pass. Production compilation is pending
the parent-owned integrated build at this checkpoint. **Regular 13.60 activation
and recovery of the background-only Home have not been demonstrated.**

## Implementation

- Both services reuse the native IPMI implementation with events enabled and
  independent registries. Their callbacks must explicitly publish reply bytes,
  output lengths, native result codes and completion events; returning zero
  from a callback does not supply a reply.
- `libC.cpp` selects the exact service names under the existing source-bound
  database configuration. Requested or installed native routing does not fall
  back to the generic success-only implementation when installation fails.
- `runtimeAppDb.cpp` installs all three database transports before native
  initializers publish endpoints. Installation failure rolls back adapters
  acquired by that attempt. Retirement attempts all three services and blocks
  mapping release while native objects or dispatcher pins remain live.
- Callbacks require a live admitted Core and its actual guest-owned thread.
  Guest thread IDs replace host thread IDs; dispatcher retention lasts through
  actual pthread join. Credential revocation is checked at registration,
  connection, callback invocation and cooperative waiting.
- Native initialization/recovery still owns schema creation, operation results
  and readiness. No databases, corruption markers, profiles, revision rows or
  experience rows were changed. Existing post-storage-commit user-create
  callbacks were not duplicated.

## Firmware evidence motivating the change

Addresses below are image-relative offsets in the investigated regular 13.60
ShellCore or ShellUI images, not addresses from the alternate firmware.

- Persisted `app.db-corrupt` admission bypasses native open, returns status
  `0x100`, and leaves the database handle null. Query error `0x809c0103` then
  occurs before SQL preparation. Normal initialization uses flags 2 without
  CREATE or a schema callback. The structurally valid shared `app.db` lacks
  `tbl_version` and the current User1/guest schemas. Home's revision query is
  `SELECT status FROM tbl_version WHERE category=?`, with `table_access_index`.
  A rejected revision cache alone does not prove why uncached Home reads fail.
- Core startup registers recovery at `+a7a528`, AppInfoDaemon at `+a7a587`,
  and preparation at `+a7a699/+a7a6d9`. AppInfo's preparation callback
  `+27d670 -> +ff1780` owns initialization of `SceAppDbSyncManagement`.
- Core preparation `+b27cf0` and recovery `+17bce20` leave server flags zero;
  connection admission therefore requires client capability 62. The new
  adapters preserve this policy rather than inventing credentials.
- Recovery operation query `0x150011` accepts eight bytes `{category,operation}`
  and returns twelve bytes `{status@0,isCorrupt byte@4,promoteType@8}`. The UI
  selector at `+88e5e0` consumes `(1,5)`, `(0,0xa)` and conditional `(0,0xe)`;
  unavailable-result error `0x809c1145` is tolerated, other errors abort.
- Preparation `0x160002` carries a 16-byte initialization notification;
  `0x160008` returns 24-byte status. Other preparation operations own bad-power
  state (`0x160007`), invalid-user enumeration (`0x160004/5`) and cleanup
  (`0x160006`). Recovery `0x15000b` carries lifecycle state (normal completion 4,
  recovery 2/3). Generic acknowledgment cannot replace their native outputs.
- Only recovery-selected UI reset at `+88e830` with the `+4e8` field equal to 1
  reaches reset `+55e600/+55e640` with flags 6 and schema callback `+55f7e0`.
  Deleting a marker is not an equivalent recovery operation.

## Verified tests

Commands completed successfully in `_Build/ipmi-native`:

```text
cmake --build _Build/ipmi-native --target db_preparation_transport_tests db_recovery_transport_tests appdb_native_transport_tests np_native_transport_tests loginmgr_native_transport_tests -j 3
ctest --test-dir _Build/ipmi-native -R "^(db_(preparation|recovery)_transport_tests|appdb_native_transport_tests|np_native_transport_tests|loginmgr_native_transport_tests)$" --output-on-failure
```

All five passed (0.91 seconds). The new fixtures check exact output bytes and
lengths, native errors, delayed notification/event completion, independent
exact-name registration, owner-thread invocation and retained dispatcher
lifetime. A callback that returns zero without replying produces caller
`0x80020059` and dispatcher `0x80020010`, preserving result/length sentinels.
Fixture payloads are isolated test values, not production database contents.
These tests do not exercise firmware initializers or production loader hooks.

## Remaining prerequisites and runtime handoff

1. `BootCredentialAdmission::Admit` admits only the audited alternate Core/UI
   pair. Regular active-build capability/auth records remain unverified. As
   recorded in `vsh-sm-authmgr-self-recovery-20261105.md`, recovery requires a
   verified active-build CCP slot41 key or successful secure-service `0x30`
   metadata-decryption outputs, followed by active-key signature verification.
   Alternate records cannot admit regular images. No new activation flag or
   credential bypass was added.
2. In psn30, native NP server creation returned `0x8002004e` without an installed
   source-bound adapter. PsnCore cleanup follows before database startup. After
   the successful push-callout join, unresolved imports match Core queue drain
   `+d146f0..+d14717`: `jxPY-0x8e-M` (`IsEmpty`) and dequeue
   `QlP4t2SGZ4I`. The current generic zero return for `IsEmpty` prevents the
   loop from reporting empty. No live RIP was captured, and replacing it with
   an unconditional true result would not implement queue ownership/drain.
3. psn30 wrote the eight-byte `dbr.db-err` request, but its later read payload
   and managed request boolean were not captured. Request-check milestones
   do not prove request admission or repair. Downstream AppBrowse VFS behavior
   remains unobserved because corruption admission bypassed open.
4. Parent owns the integrated production build and emulator verification. Once
   admission/startup prerequisites are met, verify native endpoint publication,
   request admission, operation-result bytes, preparation completion and actual
   Home queries before claiming recovery. No live emulator stop/restart,
   debugger attachment or controller input was performed for this checkpoint.
