# VSH shared-native UserService owners — 2026-11-05

## Host integration

Include `libs/runtimeUserService.h`.

1. Initialize the boot/session UserService identity through the existing session
   path. Owner preparation does **not** call `EnsureStoreInitialized`,
   `InStoreMode`, file-system APIs, native user-list seeding, or shell wake.
2. After `InitializeApplicationExecution(owner)` and before any application
   initializer/entry or owner publication, call
   `Libs::UserService::PrepareUserServiceOwner(owner)`. Require `OK`; propagate a
   failure rather than entering native code with a borrowed session identity.
3. After stop admission is closed **and actual native threads and execution
   leases have drained**, call `RetireUserServiceOwner(owner)` before releasing
   owner state. This API does not stop threads or establish drain. It requires
   `StopRequested()`, zero `Outstanding()`, and zero `application_threads`.
   A retained or already-retired execution control is supported. Repeated
   retirement returns `USER_SERVICE_ERROR_NOT_INITIALIZED`.

Both APIs return UserService error codes, not booleans. Invalid/non-root owners
return `INVALID_ARGUMENT`; absent execution/session preparation returns
`NOT_INITIALIZED`; duplicate preparation returns `ALREADY_INITIALIZED`;
allocation failure returns `NO_MEMORY`; unexpected host exceptions return
`INTERNAL`; premature retirement returns `OPERATION_NOT_SUPPORTED`.

No build/lifecycle/dispatcher files or returning-import tables were modified by
this implementation. The caller must integrate these hooks and the fixtures.

## Identity, initialization, and event contract

- Registry key: non-reused `RuntimeGuestExecution::Owner().token`, verified
  against the execution object and `Owner().identity`. A private module resolves
  through `Program::application_owner`; a retained Program address alone is
  never the identity. Missing, stopped, retired, or unprepared owners cannot
  fall through to session behavior.
- Preparation copies current registered IDs/names and real current logins from
  already-loaded session state. No passcodes/account records are copied to the
  subscription. Store mode starts logged out until a real store login, including
  when a profile has `auto_login=true`.
- The initial user is the foreground identity **at application preparation**,
  provided it is actually logged in; the non-store counterpart is the configured
  primary user when foreground login is enabled. No foreground means `-1` and
  `NOT_LOGGED_IN`, even if another store user remains logged in. Initial user is
  not recalculated from later logins/logouts. This captures the current foreground
  launch policy; an explicit different launching-user selection would need an
  explicit host-interface extension, not a fabricated login.
- Preparation starts the subscription and queues one login event for each real
  startup login. Changes between preparation and guest initialization are kept in
  order. `Initialize`/`Initialize2` enable only this owner's prepared state;
  repeated initialization returns `ALREADY_INITIALIZED` without replaying events.
  Queries before initialization return `NOT_INITIALIZED` without writing output.
- Store login/logout publishes into the unchanged session queue **and** each
  active owner's private queue under one identity/subscription lock. Owner
  `GetEvent` never pops `g_store_events`. Idempotent login remains idempotent.
- Non-store controller state is not frozen at launch. Owner preparation/initialization,
  event/list/name queries, and session event/list queries sample current routing
  and fan observed transitions to **all** non-store owners. Each owner preserves
  transitions observed while a peer polls. As with the existing legacy polling
  implementation, a complete disconnect/reconnect between all polls is not
  observable; no controller callback/source expansion was introduced.
- `GetLoginUserIdList` reflects current subscription state; `GetUserName` uses
  the copied original names and legacy validity/short-buffer rules. In store
  mode registered-but-logged-out names remain queryable, matching the session
  implementation. No profile writes, controller ports, or additional guest
  DualSense devices are introduced.
- A fanout allocation failure poisons that subscription with `NO_MEMORY`; it
  does not silently lose a transition and claim a coherent successful query.
  Other subscribers and the session retain independent queues.
- The existing identity/store lock now uses `GuestExecutionMutex`, providing
  host-critical accounting during these shared-native host calls. Session
  store loading, profile persistence, wake/seeding, legacy initial-user behavior,
  and the legacy session controller-event cursor otherwise remain unchanged.

## Exact reviewed returning-registration pairs

Library/module: `UserService` / `UserService`, library version 1, module 1.1.
Existing `LIB_FUNC` registrations are unchanged; these six implementations now
have explicit owner-local returning paths:

| NID | Registered implementation |
| --- | --- |
| `j3YMu1MVNNo` | `UserService::UserServiceInitialize` |
| `az-0R6eviZ0` | `UserService::UserServiceInitialize2` |
| `CdWp0oHWGr0` | `UserService::UserServiceGetInitialUser` |
| `yH17Q6NWtVg` | `UserService::UserServiceGetEvent` |
| `fPhymKNvK-A` | `UserService::UserServiceGetLoginUserIdList` |
| `1xxcMiGu2fo` | `UserService::UserServiceGetUserName` |

No other UserService exports are included in this review/registration request.

## Fixtures and validation status

`tests/RuntimeUserServiceOwnerTests.inc` supplies:

- `TestRuntimeUserServiceOwners()`: private synthetic two-profile JSON; a legacy
  session, two active owners, a private-module caller, and logged-out/unprepared
  cases; independent startup consumption, live store fanout, stable launch user,
  truthful failures, running/thread/lease retirement refusal, same-address fresh
  token relaunch, and byte-for-byte profile preservation around owner operations.
  Real session login/save checks retain original names, passcodes, account IDs,
  sign-in IDs, flags, and auto-login values.
- `TestRuntimeUserServiceControllerOwners()`: two independently initialized local
  owners; host keyboard routing changes exercise dynamic secondary-player login
  and logout fanout without physical devices, guest PadOpen handles, or replacement
  profiles.

Run each fixture in a **separate process** after `InitSubsystems`, since the real
session store is intentionally initialized once. The include's header comment
lists its top-level includes/declaration. No builds, test runs, live emulator
runs, staging, or commits were performed for this subtask. Static diff/whitespace
review only; parent owns build and dispatch integration.
