# OOBE startup comparison — 2026-11-05

User requested an immediate OOBE/login attempt and investigation with Astra
Medium. Parent handled the visible experiments; Astra Medium owns diagnosis and
source changes. Both runs used separate fresh profiles and normal windows.

## Integrated build

* `_Build/vsh-integrated/kyty_emulator.exe`, source label `66e6715-dirty`
  (built network-policy changes subsequently committed as `e55308f6`).
* PID23116; `_Runs/obfix203624/sd`; log `_Runs/obfix203624/_kyty.txt`.
* `-FirstBoot -ForegroundUser -RnpsReplay -HostNetwork -NoGraphicsDebugDump
  -MonoLogLevel warning -Visible`, isolated UserStorePath and WorkDir.
* At bounded observation: black window, frame92/0fps; screenshot
  `_Runs/obfix203624/boot-observation.png`.
* Log includes OnLaunched start/end. At lines240355–240425: unhandled managed
  `System.NullReferenceException` in
  `Sce.Vsh.ShellUI.InitialSetup.MenuScene.CreateRnpsPhase` at IL `0x50`, native
  `0xf0004e760+0x18c`, through CreateScene and UI FrameTask. Guest reports fatal
  exception and crash-report generation. This identifies the failure site,
  not the null object's origin.
* DBP notification thread ended; this attempt did not show the old DBP SIGSEGV.
* Normal CloseMainWindow requested after evidence; actual process exit observed.

## Preserved executable comparison

* `_Build/windows/kyty_emulator.exe`, source label `aa05d6c-dirty`, unmodified.
* PID27996; `_Runs/obbase203930/sd`; same flags, new profile.
* Exited by itself before the bounded 75-second observation completed.
* `_Logs/obbase203930.log` reports unhandled exception code3221225501 at
  `0x96030a4f0` (`libmonosgen+0x30a4f0`), the previously observed fatal-reporting
  UD2. No CreateRnpsPhase exception was found in this attempt.

The old executable fails earlier, so this comparison does not establish whether
the later RNPS null reference is a new regression. No successful OOBE or login
screen is claimed. Evidence paths were sent to the investigating agent.

## After RNPS routing provisioning fix

Checkpoint `c620e160` provisions the missing URL cache for isolated profiles.
Parent reran its four-case PowerShell fixture successfully before live attempts.

* Fresh integrated OOBE `_Runs/obroute204240`, PID15380: launcher confirmed the
  cache copy, but process exited before setup presentation with fatal-reporting
  UD2 `0x96030a4f0`; host log `_Logs/obroute204240.log`.
* Fresh integrated Home `_Runs/homeroute204347`, PID22996: cache copy confirmed,
  foreground user/RNPS/host network/VSH runtime/external apps, no FirstBoot or
  UserStore. Same early fatal-reporting UD2, before Home presentation; host log
  `_Logs/homeroute204347.log`.

These attempts do not invalidate the missing-route evidence, but block its live
presentation validation. The earlier crash is not exclusive to OOBE. Both
processes exited themselves; Astra Medium is continuing the original-fault
investigation and has permission to rebuild the now-unused integrated target.

## Original caller-chain capture

Parent launched `_Runs/obtrace204929` as PID9584 using the rebuilt integrated
executable and host `KYTY_GUEST_SIGSEGV_TRACE=1`; parent environment restored in
`finally`. The process exited naturally. No debugger attach was used.

`_kyty.txt:166752–166762` records original ShellUI PC `0x93055d79a`, RDI=0,
fault address0 and frame returns:

`0x93054fcc0 -> 0x9300007a1 -> 0x93007564a -> 0x93007b455 -> 0x930075252
-> 0x14042d6f7 -> 0`.

The original register/stack/frame evidence was delivered to Astra Medium for
production AppDb startup repair. This capture is diagnostic, not a startup fix.
