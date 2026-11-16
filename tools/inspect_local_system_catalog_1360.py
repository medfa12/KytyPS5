"""Verify copied Library sources and managed-reader constants; never write a DB.

Requires dnfile, as does the existing embedded managed metadata tooling.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import dnfile
from prepare_shellui_managed_1360 import embedded_pe

HASHES = {
    'default/param.json': 'e54f7557211d7958faebb06c78505b5cdbd3052b94413db5403de5aa92486d81',
    'default/icon0.png': 'aed2443204cf6740c5bdb7d66d82a3a8ec9d8d7ace76b17ee719e8b0f437b2eb',
    'NPXS40139/param.json': '43258bbff64dc1abbfaf776550d8601a7a6e3c846e24e41ec13b5696bc3c8282',
    'NPXS40139/icon0.png': 'c96e519210175afa3c8034198d468df7b3ab52a5ddc4375d61b84523c4a720da',
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('copied_appdb', type=Path)
    p.add_argument('accessor_aot', type=Path)
    args = p.parse_args()
    image = args.accessor_aot.read_bytes()
    if hashlib.sha256(image).hexdigest() != '739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766':
        p.error('Expected pinned regular 13.60 accessor AOT')
    pe = dnfile.dnPE(data=embedded_pe(image))
    constants = {id(c.Parent.row): int.from_bytes(c.Value.value, 'little') for c in pe.net.mdtables.Constant}
    wanted = {'DISPLAYINFO_SPACE_SHIFT': 1, 'DISPLAYINFO_SPACE_GAME': 1,
              'DISPLAYINFO_SPACE_MEDIA': 2, 'DISPLAYINFO_EXSW_SHIFT': 7,
              'DISPLAYINFO_EXSW_DISPLAY': 1, 'DISPLAYINFO_ROOTHIDEFLAG_HIDE': 1}
    actual = {str(f.row.Name): constants[id(f.row)] for t in pe.net.mdtables.TypeDef
              if str(t.TypeName) == 'AppBrowseConstant' for f in t.FieldList if str(f.row.Name) in wanted}
    if actual != wanted:
        p.error('Display-bit contract mismatch')
    for relative, expected in HASHES.items():
        data = (args.copied_appdb / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            p.error('Copied source mismatch: ' + relative)
        print(relative, expected)
        if relative.endswith('.json'):
            source = json.loads(data)
            flags = source['displayLocation']
            print(json.dumps({'titleId': source['titleId'], 'hubAppUri': source['hubAppUri'],
                              'displayLocation': flags, 'game': bool(flags & 2),
                              'media': bool(flags & 4), 'experienceSwitcher': bool(flags & 128)}, sort_keys=True))
    print('Pinned source bytes and accessor display constants verified; no readiness claim.')


if __name__ == '__main__':
    main()
