"""Bounded read-only evidence for the regular 13.60 compositor state owner.

Requires capstone; does not load or execute firmware. RVAs refer to the pinned
plain ELF, independently of either live mapping's base address.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    data = args.image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != 'd9abe348692c6e5b9e4de7fa38d6d46c74d675b5a5af63e1761d9a7bfc80bb04':
        raise ValueError('Unsupported compositor image; offsets require the reviewed regular 13.60 ELF')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phcount)]

    def read(va, size):
        for kind, flags, offset, start, _, filesz, _, _ in segments:
            if kind == 1 and start <= va and va + size <= start + filesz:
                return data[offset + va - start:offset + va - start + size]
        raise ValueError(f'RVA outside file-backed PT_LOAD: {va:x}')

    print(f'SHA256 {digest}')
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    for label, va, size in (
        ('Process-rendering-time getter: null client vs actual producer field', 0x43e0, 0x6c),
        ('InitWithProcessOrder clears this image-local client state', 0x4d90, 0x85),
        ('Successful initialization publishes the mapped client pointer', 0x51b7, 0x64),
        ('Quit clears the client pointer after unmapping', 0x5f81, 0x30),
    ):
        print(f'\n{label}')
        for ins in decoder.disasm(read(va, size), va):
            print(f'{ins.address:08x}  {ins.mnemonic:8} {ins.op_str}')
    assert read(0xf84c, 56) == b'command failed. disconnected from compositor server...\n\0'
    print('\nDiagnostic string @ f84c: null image-local client, not proof of socket disconnect')


if __name__ == '__main__':
    main()
