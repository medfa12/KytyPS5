"""Offline pin of shell BGLayer native registration, not a live dispatch trace."""
import argparse
import json
from pathlib import Path
import struct

from audit_home_hermes_call_1360 import Image


SHA256 = 'cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09'


def audit(path):
    image = Image(path, SHA256)
    for address, expected in {
        0xd9466: '81fea0000000',  # ExperienceFlowState size assertion
        0xd9485: '31c0',          # zero also on absent singleton
        0xea3ed: '4983fe01',      # exactly one whole read item
    }.items():
        raw = bytes.fromhex(expected)
        if image.read(address, len(raw)) != raw:
            raise ValueError(f'IDA instruction differs at {address:#x}')
    relocations = {}
    for address, info, value in struct.iter_unpack('<QQq', image.read(image.tags[7], image.tags[8])):
        if info == 8:
            if address in relocations:
                raise ValueError('ambiguous relative relocation')
            relocations[address] = value
    def text(address):
        data = image.read(address, 128)
        if b'\0' not in data:
            raise ValueError('unbounded registration string')
        return data.split(b'\0')[0].decode('ascii')
    if text(relocations[0x1169aa0]) != 'ShellUIBackgroundLayer' or relocations[0x1169aa8] != 0x11693f0:
        raise ValueError('native library registration changed')
    entries = {}
    if relocations.get(0x1169e08) != 0xeb6e0:
        raise ValueError('resource callback vtable slot changed')
    for address, expected_name, expected_target in [
        (0x11693f0, 'InitializeNative', 0xd9330),
        (0x1169410, 'UpdateNative', 0xd9460),
        (0x1169500, 'UpdateDrawSomethingNative', 0xda030),
        (0x1169540, 'InitializeSoundNative', 0xda140),
    ]:
        name, target = text(relocations[address]), relocations[address + 8]
        if (name, target) != (expected_name, expected_target):
            raise ValueError('native method registration changed')
        entries[name] = hex(target)
    return {'sha256': SHA256, 'library': 'ShellUIBackgroundLayer', 'entries': entries,
            'scope': 'static pinned ELF RELA registration; not live target or renderer readiness'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shell', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.shell), indent=2))
