"""Read-only audit of IDA-reviewed BGLayer gates and source asset presence.

No process access or audio decoding/playback. A present asset is not proof that
the guest mounted or loaded it; an absent welcome asset is not a login diagnosis.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import inspect_appbrowse_aot_1360 as aot


SHA256 = 'fdf48c4094cfa1bca06b5ecd8d47ffdd212722c50f8da3effe728458f44cdb41'
STATES = {'None': 0, 'Black': 1, 'ColdBootAnimation': 2, 'BootAnimation': 2,
          'WarmBootAnimation': 3, 'InitialBootAnimation': 4, 'InitialSetup': 5,
          'InitialWelcomeScreenAnimation': 6, 'InitialWelcomeScreenFadeOutAnimation': 7,
          'Login': 8, 'ParticleBottom': 9, 'ParticleSpread': 10, 'NoParticle': 11,
          'Shutdown': 12, 'ShutdownAnimation': 13, 'FadeOutShutdownAnimation': 14}


def audit(image_path, assets):
    aot.HASHES['home_transition'] = SHA256
    image = aot.Image(image_path, 'home_transition')
    fields = {}
    for typ in image.pe.net.mdtables.TypeDef:
        if str(typ.TypeName) == 'GlobalBackgroundState':
            fields = {f.row_index: str(f.row.Name) for f in typ.FieldList}
    constants = {fields[c.Parent.row_index]: struct.unpack('<i', bytes(c.Value.value))[0]
                 for c in image.pe.net.mdtables.Constant
                 if c.Parent.row_index in fields and c.Parent.table.name == 'Field'}
    if constants != STATES:
        raise ValueError('background state enum differs')
    # Instruction anchors exported by actual IDA, not inferred from names.
    for rva, expected in {
        0x43ba: '41899edc010000',     # requested state store
        0x4b57: '41c78424fc01000003000000',  # renderer timeout terminal state
        0x4d4e: '41f6c601',          # renderer flag bit0
        0x4d8a: '418b84240002000083f802',  # sound wait state==2
        0x4dbb: '41c78424fc01000002000000',  # normal animation starts
        0x54c9: '41c785dc01000008000000',  # coldboot next state Login
        0x575f: '41c785dc0100000b000000',  # welcome next state NoParticle
    }.items():
        raw = bytes.fromhex(expected)
        if image.read(rva, len(raw)) != raw:
            raise ValueError(f'IDA gate instruction differs at {rva:#x}')
    if struct.unpack('<f', image.read(0x75fdc, 4))[0] != 15000.0:
        raise ValueError('renderer timeout constant differs')
    literals = {0xcd158: '/system_ex/vsh_asset/sfx_coldboot.at9',
                0xcd170: '/system_ex/vsh_asset/sfx_warmboot.at9',
                0xcd1a0: '/system_ex/vsh_asset/sfx_transition.at9'}
    for slot, literal in literals.items():
        if image.patch(slot) != repr(literal):
            raise ValueError(f'AOT asset literal differs at {slot:#x}')
    result = []
    for literal in literals.values():
        file = assets / Path(literal).name
        data = file.read_bytes() if file.is_file() else None
        result.append({'guest_path': literal, 'present_in_supplied_directory': data is not None,
                       'size': len(data) if data is not None else None,
                       'sha256': hashlib.sha256(data).hexdigest() if data is not None else None})
    return {'image_sha256': SHA256, 'states': constants, 'assets': result,
            'scope': 'offline gate/asset audit; no proof of live branch or guest asset availability'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--assets', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(audit(args.image, args.assets), indent=2))


if __name__ == '__main__':
    main()
