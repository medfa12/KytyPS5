# Source-backed incremental profile catalog candidate

## Evidence and scope

The parent-run candidate2 Home capture is stronger than the earlier offline
projection: with real persistent user268500997, context4/Game and context5/Media
actually serialized one item apiece, repeatedly, while the original corruption
marker remained in the copied store. The completed queue trace SHA256 is
`5bf5d6b379a64a0c07d6a0cc23e141236e3f15db5e8ecffd0b08aea07038e779`.
It contains seven count1 Game completions, seven count1 Media completions,
one count2 context6 completion, and 16 context1 scalar error entries. The latter
are not proof that a fabricated revision is needed. User reports entering;
the supplied screenshot independently shows the Settings screen, not Home
tiles. Four later lookups lack action observations over the remaining bounded
window; lifecycle/coverage does not establish a deadlock.

`BuildSystemCatalogCandidate` could create a first source-backed candidate for
two absent profile pairs, but failed if called again with those users or when a
later real profile had to be added. The new explicitly selected
`CatalogRequest::preserve_existing_profiles` mode closes that **candidate
producer** gap:

* It backs up the caller's existing private AppBrowse input and retains its
  marker exactly as before. With the option off, prior behavior is unchanged.
* When a source ledger already exists, it verifies the exact metadata path,
  descriptor JSON and PNG bytes for both current firmware Libraries. It never
  silently rewrites a changed ledger to match new inputs. Absent ledger creation
  and all new profile work share the same transaction.
* A complete existing icon/concept table pair is preserved byte-for-byte at the
  typed-row level; its user ID is explicitly listed in `preserved_profiles` and
  counted in `existing_profiles`. This is **not validation** that the pair is
  current, complete or native-ready. Exactly one existing table is a conflict.
* Only an absent pair for a caller-supplied real roster ID receives the two
  genuine descriptor-derived Library rows. `profile_records` counts newly added
  rows, not preserved users; no entitlement, installed inventory, version,
  revision, recovery result or readiness is synthesized. No existing pair is
  rewritten to make the result pass.

The test's additional user ID is synthetic and is used only in a disposable
SQLite candidate. This API does not create a user, persist a fake identity,
or implicitly include users outside the caller's roster.

## Remaining production integration boundary

This is not yet a launcher or live-user-create integration and should not be
advertised as an installed runtime fix. It produces a private candidate only;
the caller must obtain the actual validated roster and firmware descriptors,
choose locale, preserve unrecognized catalog/user data, and coordinate a single
authoritative database with crash-safe ownership and rollback. Existing table
pairs are preserved rather than reconciled. Installed games/apps beyond the two
Library descriptors are not produced by this scoped operation.

In particular, do **not** invoke the host SQLite candidate builder concurrently
against the active guest DB and assume SQLite's normal interprocess locks make
it safe. Current `src/kernel/fileSystem.cpp:2132-2156` reports guest `F_GETLK`
unlocked and grants `F_SETLK/F_SETLKW` without acquiring a host lock. This is a
concrete emulator implementation limitation, not evidence that the firmware
requires fabricated lock success. Safe integration must either establish
quiescent prelaunch ownership with durable promotion/recovery and sidecar
handling, or implement/test real guest-compatible locking and a properly
serialized online native producer. Do not turn this component receipt into a
claim of exclusive access, native recovery or Home readiness.

## Verification

Isolated local target built with at most two jobs. The copied psn23 input suite
passed: original profile tables/typed rows remained identical; a new profile
read the genuine Game and Media Libraries; a repeat preserved all three profile
pairs; a lone existing table and changed source PNG both rejected without
leaking new profile/ledger changes. Existing real SQLite rollback, COMMIT
contention, cancellation, marker and source-preservation controls also passed.
No runtime profile/store or parent source/build was modified or launched.
