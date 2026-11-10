# Original generated UI assets

The two cursor PNGs, hidden-achievement PNG and notification WAV are generated
by `tools/generate_ui_assets.py`, under the project's GPL-2.0 license. They use
original geometric constructions and integer triangle-wave synthesis; no game,
firmware, font, sampled recording or external artwork is an input.

Regenerate from the repository root:

```powershell
python -B tools/generate_ui_assets.py --output assets
```

The PNGs are 64x64 RGBA. The WAV is mono 48 kHz signed 16-bit PCM, 0.35 seconds.
Existing filenames are retained for the Qt resource and emulator install paths.
