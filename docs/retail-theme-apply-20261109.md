# Retail foreground Theme.Apply null — 2026-11-09

## Result and next candidate

The exact null is **ThemeManager.Apply's local UserEntry**, not a missing GNF,
color-theme asset, or BGLayer plugin. Actual retail AOT code looks up the user
entry, receives null on a miss, invokes OnLogin as fallback, but never refreshes
that local before dereferencing it. The missing login lifecycle is visible
before the crash: the LOCAL/TEST run's UserService login/query imports are
unresolved stubs and its callback registration is disabled by an unrelated
native-LoginMgr selection gate.

This checkpoint enables the existing real local UserService session and event
machinery **only for the explicitly selected LOCAL/TEST native-credential
comparison**. It does not patch Theme or invent native-service completion.
Native LoginMgr and source-bound credential admission stay unchanged.

Unlaunched candidate:
`${USERPROFILE}/AppData/Local/Temp/opencode/retail-theme-session4/kyty_emulator.exe`

SHA256: `05d0ef41c5b3e68aaec8d5e45af7aeb6374c69a5af05da441f65e9da13f4be13`.
Only replacement production object: **libUserService.cpp.obj**. The link base
is the complete five-object combined candidate, SHA256
`46ca14194ea0ec66c91e4e625664d990b3d0bef85cad2984f1bb81b096a4c6ea`.
All earlier auto-login, registered-stdio, M.2 and media/provider inputs remain.
Object SHA256: `b18b4a2d191b683620ddb017e9deb371f043323592e2a9727548bc12e6749a1d`.

This corrects a demonstrated missing session API route. A parent/user run
still must establish that login events reach Theme before queued foreground
Apply, and that the remaining Home path works. Neither is claimed from fixtures.

## Frozen runtime evidence

The parent observation/history is preserved in
`retail-localhome2-combined-20261109.md`; it was not rewritten. Both frozen
`retail-localhome2/checkpoint1` prefixes were independently rehashed:

- host.log126371bytes:
  `510e6fe98f95858044b3bf9b755de9c08663cdbe9457fad6eb8e74ccf1f86e32`.
- guest.log71356252bytes:
  `504c2727f2de6f6d35e76e9f2476d3786629a4f7d24e8cdef34acf8a32283b94`.

Host951 reports eligible auto-login user0x10010029. End(8-0) and
BackgroundAutoLoginFinished are **not proof of a completed login**: host980
shows unresolved `uvVR70ZxFrQ` (UserService Login); host977..78 shows unresolved
`MZxH8029+Wg` (IsLoggedIn); host971/984 shows unresolved `RJX7T4sjNgI`
(GetUserStatus). Generic unresolved-import return0 does not implement those
contracts or publish a session event.

Guest13607..08 independently shows the login NID unresolved at eboot import
0x9311fb9a8; guest255888 records its stub invocation. No libSceUserService
native provider is loaded in this selected run. Existing HLE initial/event,
name/list/property exports coexist with native LoginMgr; adding a JSON query
view alone did not bind the session operations.

Host735 logs Theme.OnLogin(-1). The first real-user OnLogin is host1134,
**inside the failing Apply fallback**, after host1133 warns it is being called
for a not-logged-in user. Host1127's foreground notification therefore cannot
be treated as proof that the normal login lifecycle ran. Unload(-1) completes;
the real-user Apply label is printed, then the null exception occurs at
ThemeManager.Apply base0xe100001610+0x132. This exception blocks usable Home
even though the host window remains responsive.

## Actual IDA: exact null and fallback contract

Selected original retail `Sce.Vsh.ShellUI.Theme.dll.sprx` SHA256:
`d00e6b7c6b271930fa1460e62dd5e22f7b2540e9034df997e3dca8e4081ed9c5`.
The staged image was checked byte-identical to the original retail firmware
image. Private normalized IDA SHA256:
`a0a149e13f2fcc70add833e9d23bba0ce832fbf9c0ec49efc5180483871f3516`.
Only private ELF e_type bytes16/17 are normalized; source assets are read-only.

The embedded managed DLL is metadata-only for these methods (RVA0x2050/ret),
so those placeholder IL bodies cannot diagnose this crash. Embedded MethodDef
and AOT call-table mapping identifies real **Apply at RVA0x1610**, not a JIT
replacement. Actual IDA assembly/decompilation is external
`automounter-ida/retail-theme-apply.txt`.

- +0x1655 initializes stack local `[rbp-0x38]` to null.
- +0x16a9..b7 calls the user-entry dictionary TryGetValue with that local's
  address and requested ID. A successful lookup branches to +0x1715.
- On a miss, +0x16c0..1705 logs the not-logged-in warning, and +0x1710 calls
  OnLogin(this,user). OnLogin initializes/inserts a UserEntry into the dictionary
  (+0x12d5 → InitializeUserEntry+0x18e0; insertion+0x19ca).
- No second lookup updates the original stack local. After unload and the
  Apply log, +0x173e loads that local into RDI and **+0x1742 dereferences RDI**.
  0x1742-0x1610=0x132, exactly the frozen exception offset.
- The later theme mounter Apply call is at+0x175d; it was not reached. Therefore
  changing texture/assets or skipping mounter work would not fix this null.

The native fallback itself is unsafe when the login lifecycle has not already
created the entry. This task does not alter that retail code, add a null bypass,
or pre-insert a fabricated Theme entry. It corrects missing UserService login
state/events so normal registered consumers can perform their own login work.

## Actual native UserService contract and adapter boundary

The original retail client (analyzed read-only, **not loaded/added to the run**)
is `system_b/common/lib/libSceUserService.sprx`, SHA256
`2fdb85b05495a9bb870d926278aeea19d9e0ef12d915ab3bb6d88fefb662fe93`.
Private normalized SHA256:
`30e023dc5e2ab7ed524201ba5941c25a898cd9afb6de9074d6689a1822837d81`.
Actual IDA output is `automounter-ida/retail-theme-user-client.txt`.
Its +0x28c0 Login takes `(user,device)`, submits/polls the real job, checks
completion/result, and waits for callback acknowledgment at+0x2a13 onward.
This is not compatible with an unconditional unresolved-stub success.

Additional actual retail eboot IDA (`retail-theme-callback-owner.txt` and
`retail-theme-callback-order.txt`) confirms callback registrations such as
eboot+0x5c45 passing executable-owner target+0x734d0 and null userdata to
`wuI7c7UNk0A`. The callback reads the event type at+0 and ID at+4. That
particular native DB-preparation callback filters creation/deletion0x100/0x101;
it is **not falsely identified as Theme's login callback**. Session consumers
also use the existing poll API. The fixture verifies both callback and poll
delivery, rather than assuming one registered callback directly invokes Theme.

Before this change, `LocalLoginMgr::Selected()` correctly excluded source-bound
native configurations from local LoginMgr creation. But that same test also
excluded UserService Login/Logout/IsLoggedIn/GetUserStatus registration and
made `ChangeEventCallback` return OK without registering anything. Those were
incorrectly coupled for the explicitly authorized hybrid LOCAL/TEST startup.

The change separates **UserService selection**, without changing native
LoginMgr selection:

- Existing local-only behavior remains selected as before.
- The additional route requires exact host `KYTY_RETAIL_LOCAL_TEST_BOOT=1`,
  both nonempty native Core/UI auth-record settings, and guest local-account
  HLE opt-in. Other values/default-off retain previous routing.
- Login/Logout/GetUserStatus/IsLoggedIn and callback registration now use the
  already implemented local session/store machinery in that explicit mode.
- Login and callback mutation still require a retained current boot-role
  program/thread, active identity, capability62 from the existing auth-info
  lookup, no application owner, and a selected UserService store. Callback
  targets must be within the owner's executable segment.
- The change does not enable local LoginMgr CreateUser/DestroyUser or alter
  its native factory/server/admission rules. It does not waive source-bound
  credential validation or add another native firmware provider.

The adapter's successful local login performs real local state work: save/order
update, stable login slot/foreground assignment, poll event0 and callback fanout,
and callback acknowledgment before return. Unsupported device association,
missing user, missing caller identity and failed save return errors. It does
**not** claim native UserService server completion, OOBE completion, NP token
issuance, or PSN authentication. The logs retain the `userservice-local` label.

## Regression evidence and isolated build

`tests/RetailThemeSessionTests.cpp` uses a fresh synthetic profile and calls the
production registered exports. Its callback runs through the actual retained
pthread/RuntimeGuestExecution boundary, with real registration, dispatch and
join/retirement. It is not a replacement session implementation. GPU invalidation
is replaced only for host-only fixture buffers. Neither Theme AOT nor any
console firmware is executed by this offline fixture.

Tests establish:

- Old combined-base object fails because the required session export is absent.
- Opt-in selected exports exist but LoginMgr CreateUser/DestroyUser overrides
  remain absent; default/unset,0,`true`,11 do not select the new route.
- Unretained callers, out-of-owner callback targets, unknown profile IDs and
  unsupported device association are rejected before login publication.
- Loading a profile does not log it in. Successful Login changes membership,
  foreground and stable slot1; the worker callback receives event0 with the
  actual ID before success, and the polling consumer reads that same event
  immediately after the successful return.
- Callback ownership/thread identity and reentrant foreground queries work;
  pair deduplication and idempotent login do not duplicate events.
- Logout commits state, emits/acknowledges event1 and clears membership.
- A real save failure (destination temporarily a directory in the synthetic
  fixture) returns internal error and leaves logged-out state with no new
  callback or poll event. Registration is removed and its worker retired/joined.

Five selection/session cases pass, with the old-object negative control. All
**nine earlier combined regressions were relinked with this UserService object**
and pass: shutdown/particle stdio, M.2 absence/device regressions, and seven
auto-login selector cases. This is preservation evidence, not a Home test.

`tools/build_retail_theme_session_fixture.py` compiles at most two jobs and
links serially against the pinned combined46ca base. It rehashes source and
read-only object/library inputs before/after; it neither rebuilds shared
5ee6a153 outputs nor drops the five combined replacements. External
`retail-theme-session4/{receipt.json,tests.json,regressions.json}` and compiler/
link responses preserve the exact inputs/results. An intermediate recipe run
stopped at an overly strict M.2 fixture duplicate-symbol assertion; the corrected
recipe expects no renderer shim for that fixture, and all final cases pass.

## Parent integration and remaining verification

Use the candidate above (or replace only `libUserService.cpp.obj` on the same
combined base). Keep current prepared local/test store, user0x10010029,
host/guest opt-ins, native credential/provider setup and six-app catalog mounts.
No additional store seeding, Theme asset replacement or launcher change is
needed. This agent has not accessed or changed live stores/process35252.

In the next parent-controlled run, verify actual `userservice-local` callback
registration and Login export result, a login poll/callback for0x10010029,
Theme.OnLogin before foreground Apply, absence of the not-logged-in fallback,
and then the real Home bundle/UI. Queued managed event ordering and any later
provider/asset issues remain unverified until that run. Keep the current
crashed guest evidence rather than interpreting its responding host as Home.
