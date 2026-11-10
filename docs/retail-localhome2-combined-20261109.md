# Retail local Home combined retry

The three fixes retain separate source commits:

- `265f06d1`: native auto-login registry selector.
- `a161466b`: shutdown/animation registered-stdio asset reads and ownership.
- `a309d375`: optional M.2 controller absence contract.

The parent combined their audited production objects without changing source
or shared build outputs. Starting from the shutdown candidate retains its
libC/libGuestFile pair and the earlier auto-login/local-test changes. Exactly
three M.2 objects replace their counterparts: fileSystem, libFsSandbox and
libShellHits. Input/candidate/source hashes were validated against both
component receipts and the auto-login ancestry, before/after serial linking.
All output/map/PDB paths were relocated into the new isolated build directory.

`Temp/opencode/retail-home-combined1/receipt.json` records the full provenance.
Executable SHA256:
`46ca14194ea0ec66c91e4e625664d990b3d0bef85cad2984f1bb81b096a4c6ea`.

The parent relinked each fixture with **all five** replacement objects and ran
nine passing cases: the native-style shutdown/particle GNF stdio fixture, the
M.2 absence/device regression fixture, and seven auto-login registry cases.
These establish combined-object behavior, not guest Home or rendered animation.

The external launcher accepts this combined build's receipt while retaining
the original LOCAL/TEST prepared-store receipt and its original candidate pin.
Explicit ancestry binds the new binary to that prepared seed; neither receipt
is rewritten. The copied store's exact size/hash inventory is still checked
before importing six external applications for actual native user0x10010029.
An isolated launcher check passed combined-candidate ancestry and seed checks.

No emulator was present before the next launch. `retail-localhome2` uses a new
copy of `retail-localtest-prepared1/sd`, native retail admission/providers,
audited mixed RNPS/font mounts, normal default audio, the saved native user,
and all six external app/game directories including Minecraft. This remains
an explicit LOCAL/TEST setup-skip comparison, not completed OOBE or PSN.
Runtime observations are recorded below after launch.

## Initial live result

Launch succeeded: PID35252, creation FILETIME134387000725240302, expected
combined executable path/hash. A process query found a responding NPXS40087
window at frame120. Native guest62885..88 now receives ENODEV and explicitly
logs **no m.2 is detected**;63130 reports `_m2Exists() returned=0, exists=0`.
The subsequent0x80f40007 mount result is retained as observed, not converted
to a fabricated success. This clears the previously observed ENOENT probe
classification. Initial host startup advanced to EvalDDDCheckImproperAttachDetach;
auto-login completion/Home were not yet observed at that point.

A bounded read-only host-log observation completed after76.37seconds, reading
126371bytes. `retail-localhome2/startup-observation.json` records auto-login
eligibility for0x10010029, auto-assign enabled, `[AutoLogin] End(8-0)` and
`BackgroundAutoLoginFinished`. No controller input was sent.

## Foreground-session progress and next blocker

Inspection of the complete host prefix found more than the observer's narrow
event filter: line1127 records `[onForegroundUserChanged] data=268501033`.
Theme unload completes, then applying Default for user10010029 raises an
unhandled `System.NullReferenceException` in
`Sce.Vsh.ShellUI.Theme.ThemeManager.Apply(System.Int32 userId)` at
`0xe100001610 + 0x00132`. The caller is the queued
`ForegroundUserChangedAction` callback. Repeated managed exception reports
include `Killing this process. Generating the crash dump report.` The host
emulator remains responding with an NPXS40087 window at frame135; that is
not evidence that the guest UI survived or Home is usable.

The observer did not search for this exception, so absence of its selected
`MF_ApplicationError`/Home/login markers must not be interpreted as clean boot.
The foreground-user notification is positive progress beyond selector discovery;
durable login and usable Home still require verification. The null object's
identity and underlying cause are not yet established.

External `retail-localhome2/checkpoint1/receipt.json` records frozen log prefixes,
launch/observer receipts and a51-file live-store copy (not an atomic snapshot).
Host126371bytes SHA256:
`510e6fe98f95858044b3bf9b755de9c08663cdbe9457fad6eb8e74ccf1f86e32`.
Guest71356252bytes SHA256:
`504c2727f2de6f6d35e76e9f2476d3786629a4f7d24e8cdef34acf8a32283b94`.
No controller navigation, process closure, or source-store modification occurred.
