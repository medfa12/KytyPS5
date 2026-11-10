"""Read-only, hash-pinned regular 13.60 database disassembly; requires capstone.

No ELF execution or database mutation. Explicit RVA:size windows are capped.
RIP-relative comments show file-backed addresses and printable literals only.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

HASHES = {
    'core': '6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a',
    'ui': 'cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09',
}


class Image:
    def __init__(self, path, kind):
        self.data = Path(path).read_bytes()
        self.digest = hashlib.sha256(self.data).hexdigest()
        if self.digest != HASHES[kind]:
            raise ValueError('Image hash does not match reviewed regular 13.60 ' + kind)
        if self.data[:4] != b'\x7fELF':
            raise ValueError('Expected plain ELF')
        offset = struct.unpack_from('<Q', self.data, 32)[0]
        size, count = struct.unpack_from('<HH', self.data, 54)
        self.segments = [struct.unpack_from('<IIQQQQQQ', self.data, offset + i * size) for i in range(count)]

    def read(self, va, size):
        for kind, _, offset, start, _, filesz, _, _ in self.segments:
            if kind == 1 and start <= va and va + size <= start + filesz:
                return self.data[offset + va - start:offset + va - start + size]
        raise ValueError(f'Outside file-backed PT_LOAD: {va:x}+{size:x}')

    def literal(self, va):
        try:
            data = self.read(va, 512).split(b'\0', 1)[0]
        except ValueError:
            return ''
        return repr(data.decode('ascii')) if len(data) >= 4 and all(32 <= b < 127 or b in (9, 10, 13) for b in data) else ''

    def relative_slots(self, va, count):
        tags = {}
        for kind, _, offset, _, _, size, _, _ in self.segments:
            if kind == 2:
                for tag, value in struct.iter_unpack('<QQ', self.data[offset:offset + size]):
                    if tag == 0:
                        break
                    tags[tag] = value
        if tags.get(9) != 24 or not all(t in tags for t in (7, 8)):
            raise ValueError('Expected ELF64 DT_RELA table')
        relocations = {}
        for address, info, addend in struct.iter_unpack('<QQq', self.read(tags[7], tags[8])):
            if va <= address < va + count * 8 and address % 8 == 0:
                if info != 8 or address in relocations:
                    raise ValueError(f'Not a unique R_X86_64_RELATIVE slot: {address:x}')
                relocations[address] = addend
        return [(address, relocations.get(address)) for address in range(va, va + count * 8, 8)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=HASHES)
    parser.add_argument('image', type=Path)
    parser.add_argument('windows', nargs='*', help='hex RVA:size, max 0x4000 bytes each')
    parser.add_argument('--slots', action='append', default=[], help='hex RVA:count of DT_RELA relative pointers, max 32')
    args = parser.parse_args()
    image = Image(args.image, args.kind)
    print('SHA256', image.digest)
    for spec in args.slots:
        va, count = (int(s, 16) for s in spec.split(':'))
        if not 0 < count <= 32:
            raise ValueError('Slot count must be 1..32')
        for address, target in image.relative_slots(va, count):
            print(f'SLOT {address:08x} -> ' + ('no relative relocation' if target is None else f'{target:08x}'))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    for window in args.windows:
        va, size = (int(s, 16) for s in window.split(':'))
        if not 0 < size <= 0x4000:
            raise ValueError('Window must be between 1 and 0x4000 bytes')
        print('\nWINDOW', window)
        for ins in decoder.disasm(image.read(va, size), va):
            notes = []
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    target = ins.address + ins.size + op.mem.disp
                    notes.append(f'@{target:x} {image.literal(target)}')
            print(f'{ins.address:08x} {ins.mnemonic:8} {ins.op_str:48} ' + '; '.join(notes))


if __name__ == '__main__':
    main()
