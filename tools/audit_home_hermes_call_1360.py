"""Pin the conditional native-extension -> Hermes runtime vtable relationship.

This verifies ELF bytes/relocations only. Actual backend selection and JS
callback execution require independent runtime evidence.
"""
import argparse
import hashlib
import io
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile


class Image:
    def __init__(self, path, expected):
        self.data = path.read_bytes()
        if hashlib.sha256(self.data).hexdigest() != expected:
            raise ValueError('unexpected native image hash')
        elf = ELFFile(io.BytesIO(self.data))
        self.loads = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.tags = {}
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_DYNAMIC':
                for tag, value in struct.iter_unpack('<QQ', segment.data()):
                    if not tag: break
                    self.tags[tag] = value

    def read(self, rva, size):
        for address, offset, length in self.loads:
            if address <= rva and rva + size <= address + length:
                start = offset + rva - address
                return self.data[start:start + size]
        raise ValueError(f'unmapped RVA {rva:#x}')

    def rip_target(self, rva):
        code = self.read(rva, 7)
        if code[:3] not in (b'\x48\x8d\x05', b'\x48\x8d\x0d'):
            raise ValueError('unexpected RIP-relative lea')
        return rva + 7 + struct.unpack('<i', code[3:])[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('extensions', type=Path)
    parser.add_argument('hermes', type=Path)
    args = parser.parse_args()
    ext = Image(args.extensions, '2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb')
    hermes = Image(args.hermes, 'e279a9d4272df0347515a481bd0fe4d4bb0f7bb9281c3ece20032688c0f9d10d')
    if ext.read(0x2c902, 4) != bytes.fromhex('4c8b7f78') or ext.read(0x2c976, 6) != bytes.fromhex('ff9098010000'):
        raise ValueError('extension receiver or virtual call changed')
    if ext.read(ext.rip_target(0x2c043), 36) != b'invokeCallbackAndReturnFlushedQueue\0':
        raise ValueError('named bridge function changed')
    if hermes.rip_target(0x14f19) != 0x26c348 or hermes.read(0x14f27, 3) != bytes.fromhex('488903'):
        raise ValueError('Hermes constructor vtable store changed')
    slot = 0x26c348 + 0x198
    relas = [value for target, info, value in struct.iter_unpack('<QQq', hermes.read(hermes.tags[7], hermes.tags[8]))
             if target == slot and info == 8]
    if relas != [0x14010]: raise ValueError('Hermes call slot relocation changed')
    print('Pinned extension receiver+78 virtual slot+198 -> conditional HermesRuntimeImpl call RVA 0x14010.')
    print('Static relationship only; active backend and JS delivery are unobserved.')


if __name__ == '__main__':
    main()
