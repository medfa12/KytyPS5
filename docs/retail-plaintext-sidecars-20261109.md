# Retail 13.60 plaintext sidecars

User requested missing plaintext counterparts alongside the existing retail
files, preserving originals. This is an additive mixed-source inventory, not a
claim of authenticated retail decryption or runtime compatibility.

## Completed

- Added 16 files, totaling 30,274,967 bytes, to the external retail firmware tree.
- All 65 previously audited ordinary RNPS `application.ps.hbc` sidecars already
  existed with matching hashes and were preserved.
- Added `base_dll.ps.js`, `base_dll.ps.hbc`, `host.ps.js` and `host.ps.hbc` beside
  the encrypted files in `system_ex_b/rnps/apps/NPXS40141`.
- Added 12 missing native/managed counterparts with `.devkit-plaintext` appended
  to the original filename. They cover the staged common/Mono and ShellUI trees:
  NKWebDriver, two mscorlib locations, ShellUI-local Psm/PsmUtil, app.exe and six
  ShellUI assemblies. All twelve lacked a same-path retail file; these additions
  do not replace an encrypted native module or a different retail ELF.
- 273 scoped native/managed retail files were already plaintext and were retained.

Scope includes module names referenced by tracked runtime/tools/tests in both
development branches, the common/Mono and ShellUI managed/AOT wildcard staging
inputs, and the previously audited RNPS plaintext substitutions. It is not an
inventory of every firmware executable or installed title.

## Verification and remaining limits

Every new file was opened with exclusive-create semantics and hash-verified.
All 407 checked pre-existing files and all copied sources retained their hashes.
An independent reread verified all 16 outputs. No existing-file conflicts occurred.

The non-module suffix keeps the new native/managed files out of automatic
`*.dll`/`*.sprx` selection. No launcher or mount was changed, and no guest was run.
The prior RNPS audit's 132 unresolved containers remain unresolved; no plaintext
was available to add for those files. Existing strict inventory receipts describe
the pre-addition tree; future full-tree staging must inventory the new sidecars
explicitly rather than treating them as shipped retail resources.

Machine-specific paths, source provenance, per-file hashes and the exact creation
journal are retained outside Git in the local `retail-missing-counterparts-added1`
receipt directory (`plan.json`, `created.json`, `verified.json`). Firmware bytes
and local paths are not committed.

## Fresh retail retry

`retail-full-rnps3` launched the previously validated retail candidate with a
fresh copied local/test store, the verified disposable mixed RNPS stage, mixed
fonts, native SSL/libc, and external title mounts. The existing audited RNPS
master already contains counterpart host/base and app content; newly added
`.devkit-plaintext` sidecars were not automatically selected. It is not a pure
retail or authenticated-PSN run. At initial observation PID 15680 was responding
and the ShellUI startup log reached scene loading; rendered Home, user login,
and application launches were not yet confirmed. The prior full-stage run logged
an unhandled `ThemeManager.Apply` null-reference exception; its cause remains
unproven. Runtime logs and the exact launch receipt remain outside Git.

The user subsequently reported an early freeze. The window was observed at
frame 114 / 0 fps; this run again logged an unhandled `ThemeManager.Apply`
null-reference exception. PID 15680 later exited; no exit code was captured and
the underlying cause remains unproven. Logs and store were preserved. As requested,
29 superseded early guest streams were deleted (5,312,282,310 bytes); current and
recent diagnostic logs, stores, firmware, and builds were retained. The exact
file list, hashes and deletion journal are in the external
`cleanup-early-guest-logs-20261109` receipt. No executable was deleted in this pass.
