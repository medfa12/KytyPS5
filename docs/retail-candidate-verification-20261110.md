# Asset cleanup and isolated publication candidate

Asset source revision: `0a708dbe231b8588eb2564465d75179bde7dd2ea`.
Local parentless candidate: `2e1b2148d65fe861f703b1b5d9aebb3a24fc092b`.
Identical tree: `8918014e803af5fcb5f3f33d850400847d738e21`.

## Verified

- Archived and byte-verified 27 original tracked asset/screenshot files externally
  before replacement. Existing development history is preserved.
- Four original procedural replacements regenerate byte-for-byte. All PNG
  chunks pass CRC checks and decompress to 64x64 RGBA with transparent/opaque
  pixels. WAV is mono signed PCM16, 48 kHz, 16,800 frames (0.35 seconds).
- The hidden-achievement icon was visually inspected. In-emulator cursor
  appearance and audible notification playback are not yet verified.
- Release emulator/launcher build and install passed from the committed source
  in the external build checkout. Qt RCC rebuilt the changed resource and the
  launcher linked successfully. Installed and build cursor/sound bytes equal
  the committed source. Build shell: `sh_1c6bed4a8001aEtLM4OuOgBRn3`.
- Fetched only the parentless candidate into a new external repository:
  `Temp/opencode/retail-publication-candidate-20261110`. Commit count is one,
  parent list is empty, and recursive tree entries (including modes/gitlinks)
  exactly match the source. `git fsck --full` passed.
- Compared reachable objects against 1,163 historical/removed blob IDs from
  the prior inventory and original asset manifest: zero matches. Empty screenshot
  `.gitkeep` is excluded because the empty blob is shared by unrelated files.
- Scanned 1,546 top-level tracked blobs for all 18 external shader fixtures as
  binary sequences and contiguous hexadecimal DWORD sequences: zero matches.
  This check does not inspect the contents of dependency gitlinks or establish
  general provenance for every source literal.

External receipts and backup are under
`Temp/opencode/retail-assets-cleanup-20261110/`: `original-assets.tar`,
`original-assets.json`, `asset-verification.json`, `candidate-verification.json`.
No extracted firmware or retained shader fixture was added to the candidate.

## Remaining acceptance

The candidate is local and experimental, not a published ready release. Broader
content/dependency review and fresh recursive-candidate build remain pending.
Fresh-user preparation still requires matching URL configuration, decrypted
per-title RNPS overlays and source-bound Core/UI admission records. GPU execution
requires compatible hardware; the current Intel Arc lacks shared 64-bit atomics.
Home, application launch, playback and genuine PSN authentication remain
unverified for a fresh store. No GitHub push or remote history rewrite occurred.

This evidence document is a later development checkpoint and is not part of the
candidate tree identified above.
