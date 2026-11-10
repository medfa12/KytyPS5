# Production catalog bootstrap: ownership boundary before integration

This is a design checkpoint, not an implemented runtime bootstrap. The private
incremental producer in `62f3865e` remains usable independently. Local persistent
login does not establish PSN authentication.

## Selected strategy

Use quiescent prelaunch preparation under a lease held for the entire guest
process lifetime. Do not use a host SQLite connection against a running guest:
guest `fcntl` currently grants locks without matching host file locks. A lease
around only the preparation function cannot solve this.

The lease identity must be the canonical persistent store root, not a launch
directory or a PID. Every launch path that can mount that root must participate;
the child must retain ownership across the launcher-to-child handoff. Before
that invariant is implemented and tested, no CLI `--quiescent` boolean or absence
of an observed process is sufficient authorization to activate a candidate.
Older/uncoordinated executables cannot be assumed to honor a new lock file.

## Required transaction sequence

1. Acquire exclusive ownership before opening a guest storage file. Recover any
   prior bootstrap journal before mounting AppBrowse. Obtain real local roster
   IDs and locale from validated user storage, not guessed integer sequences.
2. Under that ownership, use SQLite backup against the complete existing store
   including any WAL state to create a private same-volume candidate. Do not
   copy only `app.db`, delete sidecars, or ignore a hot journal. Existing SQLite
   recovery behavior must be exercised on disposable crash fixtures first.
3. Invoke the source-backed producer with the actual roster. Validate its output
   and source ledger; preserve existing table pairs and separately report their
   unverified completeness. A partial pair stays a conflict. Retain corruption
   markers and native recovery state. No scalar revision/readiness override.
4. Close all host SQLite handles before promotion. Flush candidate data and a
   journal recording source identity, candidate hash, source-asset hashes,
   intended target and rollback identity. Durable publication and recovery must
   be platform-specific; `std::filesystem::rename` alone is not a durability
   proof on Windows. Do not claim cross-file atomicity for DB plus sidecars.
5. Promote while still exclusively owned. After a crash, journal states must
   select an intact old generation or a fully validated new generation; never
   merge a new DB with old WAL/SHM files. Recovery must be deterministic and
   idempotent. Preserve originals until recovery has made a durable decision.
6. Only then expose the authoritative database to the guest, with the lease
   retained until guest shutdown and handle closure. The host producer must not
   reopen it online. A user created during the session needs either a genuinely
   serialized guest-side producer or a documented next-launch bootstrap; this
   offline approach alone does not implement same-session profile creation.

## Tests required before wiring this into startup

* Two competing launches of one canonical store, including aliases, must not
  both mount it; different stores should remain independent.
* Parent exit and failed child startup must not create an ownership gap.
* Crash injection at each flush/publication boundary must leave exactly one
  recoverable generation; retrying recovery must produce the same selection.
* WAL-backed committed rows and interrupted SQLite transactions must survive
  preparation according to SQLite semantics; original sidecars remain intact
  on rejected preparation. Test externally held handles on Windows.
* Roster changes during preparation, wrong source hashes, partial table pairs,
  cancellation and promotion failure must retain the authoritative old store.
* Successful preparation must be consumed by the normal mount path, then by real
  managed Home readers. Offline SQL success alone is not Home acceptance.

These lifecycle and durability pieces are prerequisites, not excuses to force
recovery flags. No live activation implementation is included in this checkpoint.
