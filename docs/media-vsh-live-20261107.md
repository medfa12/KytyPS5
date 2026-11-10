# Apple/VSH relaunch and Netflix stall checkpoint

## Build and evidence

Production `cmake --build _Build/windows --target kyty_emulator --parallel 3`
passed at source checkpoint `444ad5b2`, including merged ESVM readiness. The
preceding decoder/source CTest result was 2/2; no new playback success is claimed.

Netflix `mnetflix1` was already absent at follow-up process inventory. Original
logs remain in `${USERPROFILE}/AppData/Local/Temp/opencode/mnetflix1`; added
`stall-evidence.json` records their lengths and SHA256 hashes, and
`stall-fault-excerpt.txt` retains the fault neighborhood. The user's conversation
screenshot establishes the 20% loading stall; no local screenshot copy was found.
The run contains an app fault at RVA `0x105f532`, register r14 `0x80020016`,
and string `vdec dmem` in r15. Caller chain includes `0x14ff041`, `0x1503ec2`,
and `0x1502ffd`. This is an allocation-path lead, not a fully traced cause.
Netflix uses Videodec2/Audiodec; ESVM tests do not verify its playback.

## Live launches

- Apple TV `mapple1`, PID19220: rebuilt production executable, regular13.60,
  isolated data/system_data/user/system_tmp, host network, native SSL/libc,
  media tracing. Launch arguments and executable SHA256 are in that temp run's
  `launch.json`. Window exists and frame count advances; visible content,
  readiness and actual audio/video playback await user testing.
- VSH `psn34`, PID33516: existing `psn23/sd` profile and `psn23/stage`, native72
  startup, local-account HLE, RNPS replay and runtime enabled. Log explicitly
  confirms `NP regular72: native EventQueue lifecycle selected`. LoginMgr/NP
  native server admission still reports `0x8002004e`. Process exited after Mono
  reported SIGSEGV at `0x910d1cac3`, followed by its UD2 at `0x96030a4f0`.
  Evidence: `_Runs/psn34/_kyty.txt`, `_Logs/psn34.log`. This is failed live
  acceptance, not proof of queue cleanup or usable Home.
- VSH `psn35`, PID34428: same configuration except `-NativeNpStartup` omitted;
  retains `-NativeNpHttpPool` (native54). This diagnostic fallback keeps local
  login/UI testing available, but does not validate native72. Process/window
  exists; log reaches `LoginMgrPlugin.Create()`. User-visible login/Home and
  PSN remain unverified. Profiles/databases were not reset (`-NoDatabases`).

Controller navigation remains user-owned. Test Apple content/trailer playback,
then existing User1 login and Home; only if Home is usable continue to Settings,
Users and Accounts, Account, Sign In. Record errors/screens rather than infer
success from process liveness. PlayReady backend remains absent.

## User acceptance follow-up

User reports selecting User1 in psn35 leaves only the animated background and
no menu. Guest Quick Play, user creation and PSN sign-in were not tested because
of this blockage. `_Logs/psn35.log` records login result success/retCode0 for
user268500996 and one logged-in user; this does not establish usable Home.

Apple still reports a connection problem. The supplied screenshot shows
"Troubleshooting Complete / No problems could be detected. Try again later."
This is a changed diagnostic outcome, not verified service/playback recovery.
User reports Apple closed by itself. No launcher timeout or agent close was
configured/executed. `mapple1/host.log` contains the Apple Gfx assertion
`info.name[0]=='m' && info.name[1]=='e' && info.name[2]=='m'`, described as
"failed to release memory", at `Gfx/source/Backend/PS5/Gnmp.cpp:55`, and ends
with `Guest abort()` (`src/libs/libC.cpp:1131`). The exact allocation/query
contract causing that assertion still needs tracing. Both emulator processes
were absent at follow-up inventory. No new build, fix or playback acceptance
is established by this follow-up.

## User-creation retest and delegated investigations

At user request, launched `psn36` PID23476 using the same native54 fallback
configuration and existing psn23 storage/stage. No timeout, automatic restart,
or controller input was configured. Process/window confirmed present; user
owns Add User/create-user testing. No creation outcome yet.

User-authorized background investigations start from427d9daa in separate clean
worktrees/branches `netflix-stall-investigation` and `apple-stream-investigation`.
Netflix owns actual Videodec2/direct-memory stall tracing; Apple owns streaming
failure and graphics-memory abort tracing. Both must preserve the active VSH,
parent executable/build/storage and original dirty media worktree, use isolated
validation, and report evidence-backed fixes/checkpoint commits for integration.

### psn36 user results

User confirms creation completes, then Home remains empty/animated-background
only; also confirms PSN sign-in fails. `_Logs/psn36.log:15015` records login
success/retCode0 for new user268500997 (`0x10010005`); lines15141-15143 show
rnps-home MountApp/runApplication and Running Main. Restart persistence and
guest Quick Play remain unverified. Lines13331-13350 record LoginMgr `pfAuth`
failure `0x80550002`, consistent with the user's PSN report.

Compositor-disconnection messages start before login and continue after Home
startup; they cannot alone establish the cause of Home's missing menu.
`_Runs/psn36/_kyty.txt` also contains unhandled DbPreparation transport requests,
AppBrowse event-flag initialization `0x80020003`, transaction `0x809c0103`,
and `SCE_LSDB_ERROR_WAITING_RECOVERY`. No database reset or fabricated readiness
was applied. Active VSH PID23476 remains user-owned.

Netflix diagnosis/fix reviewed and integrated as `8b601a65` / `6ff5521e`.
The actual query return was inferred from disassembly and observed zero-size
allocation, not captured directly. Agent isolated regression/negative control
passed as documented; parent normal-build test target validation is pending.
The running emulator executable has not been rebuilt/replaced with this fix.
