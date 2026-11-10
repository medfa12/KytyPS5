# Retail Home retry with three additive counterparts

The preceding run verified the saved-user login lifecycle and Theme application,
then crashed on NPXS40048 bundle verification. NPXS40024 and NPXS40062 also
failed verification. User observed the stall; the former process23560 was
subsequently absent. Its frozen evidence remains in retail-localhome3/checkpoint1.

Staging checkpoint `422efa57` supplies the authorized three-title mixed
comparison documented in `retail-home-rnps-counterparts-20261109.md`.
The parent external launcher now accepts `--rnps-home-stage` after the existing
15-title batch. It pins receipt SHA256
`ab5bcad81639e5830d0c826f8d105f5974218769ffd3d73ebfbe77d73b1a5d8c`,
checks the exact141-file inventory, every size/hash, title/mount sets and
read-only declarations, then adds exactly three previously absent mount points.
The launch receipt retains the explicit mixed/devkit plaintext classification.
An isolated execution of this validation block passed against the actual stage;
the complete launcher parsed/compiled successfully.

No emulator was present before launch. `retail-localhome4` successfully launched
as PID37324, creation FILETIME134387023112413144, using the unchanged
`retail-theme-session4/kyty_emulator.exe`, SHA256
`05d0ef41c5b3e68aaec8d5e45af7aeb6374c69a5af05da441f65e9da13f4be13`.
It retains a fresh validated prepared-store copy, actual native user0x10010029,
native LoginMgr/admission, LOCAL/TEST UserService, normal default audio settings,
six external app/game mounts and all prior audited mixed RNPS/font mounts.
No original firmware/store was overwritten; no controller input was sent.
The shutdown-animation fix remains its own commit `a161466b`.

## Runtime: bundle crash cleared, blank Home remains

The bounded observer completed at91.29seconds on its time limit, with no
unhandled-exception trigger. External `startup-observation.json` covers
host161196bytes and guest35503281bytes. All three added titles reach
`MountApp runApplication`, as does `rnps-home.NPXS40002` (host1616). Theme
Apply finishes again; scene focus changes to Render.NPXS40002 at host1227.
Thus the previous bundle-verification crash is cleared in this observed run.

The visible result is still **not usable Home**. A window-only PrintWindow
capture, without input/focus/navigation, shows only a gradient background and
no Home tiles/navigation. Its title reports frame1342/fps32. An earlier process
query reported responding at frame467/fps20; later logs continue compositor
flips. An active rendering loop and runApplication do not establish that the
Home view initialized or rendered correctly. The remaining cause is unknown.

`retail-localhome4/checkpoint1` preserves the window screenshot, launch/observer
receipts, later bounded logs and a52-file live-store copy (not atomic):

- Host283695bytes SHA256
  `6d0f6e29437d4ece7d65c7b441be04a728ca2bef5281caaca10694ed65ba47e2`.
- Guest56991332bytes SHA256
  `776965ba01df3bf167b393e37a8d674b7bd3ade7b452f498230f1b1255364844`.
- Screenshot SHA256
  `5b2630f911aebaea8fad2ecea4c7fedc8b7f2bef466772b57a98a9969eab2359`.

Sol high is investigating the blank Home path from this frozen evidence.
No controller input, closure or source-store change occurred. Playback and
rendered shutdown animation remain unverified.
