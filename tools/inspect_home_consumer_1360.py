"""Read-only, hash-pinned Home managed AOT disassembly; requires capstone.

Offsets are module RVAs. No module execution, process access, or file writes.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


HASHES = {
    'vsh': '64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee',
    'db': '739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766',
    'pui': 'a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=HASHES)
    parser.add_argument('image', type=Path)
    parser.add_argument('windows', nargs='+', help='hex RVA:size, max 0x4000 bytes each')
    args = parser.parse_args()
    data = args.image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != HASHES[args.kind]:
        parser.error('Image hash does not match reviewed regular 13.60 ' + args.kind)
    assert data[:4] == b'\x7fELF'
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, offset + i * size) for i in range(count)]
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    print('SHA256', digest)
    for window in args.windows:
        va, size = (int(value, 16) for value in window.split(':'))
        if not 0 < size <= 0x4000:
            parser.error('Window must be between 1 and 0x4000 bytes')
        segment = next((s for s in segments if s[0] == 1 and s[3] <= va and va + size <= s[3] + s[5]), None)
        if segment is None:
            parser.error('Window is outside file-backed PT_LOAD')
        start = segment[2] + va - segment[3]
        print('\nWINDOW', window)
        for instruction in decoder.disasm(data[start:start + size], va):
            print(f'{instruction.address:08x} {instruction.mnemonic:8} {instruction.op_str}')


if __name__ == '__main__':
    main()
