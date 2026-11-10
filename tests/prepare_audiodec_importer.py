"""Generate a tiny non-executed ELF importer for production loader validation.

Contains four real CPU API imports and GOT relocations; no firmware bytes.
"""
import pathlib
import struct
import sys


def prepare(path):
    image = bytearray(0x4000)
    strings = bytearray(b'\0')
    def string(value):
        offset = len(strings)
        strings.extend(value.encode() + b'\0')
        return offset
    library = string('libSceAudiodecCpu')
    names = [string(nid + '#A#A') for nid in
             ('ktD2w3D4G2U', 'hdFsxo3MFu8', 'lSVTiWV5wLc', 'hAS5WH6hxrE')]
    for index, name in enumerate(names, 1):
        struct.pack_into('<IBBHQQ', image, 0x800 + index * 24, name, 0x12, 0, 0, 0, 0)
        struct.pack_into('<QQq', image, 0xa00 + (index - 1) * 24,
                         0xc00 + (index - 1) * 8, (index << 32) | 7, 0)
    image[0x600:0x600 + len(strings)] = strings
    tags = [(5, 0x600), (10, len(strings)), (6, 0x800), (11, 24),
            (0x6100003f, 5 * 24), (23, 0xa00), (2, 4 * 24), (20, 7),
            (7, 0xb00), (8, 0), (9, 24),
            (0x61000015, (1 << 32) | library),
            (0x6100000f, (1 << 40) | (1 << 32) | library), (0, 0)]
    for index, tag in enumerate(tags):
        struct.pack_into('<QQ', image, 0x200 + index * 16, *tag)
    ident = b'\x7fELF\x02\x01\x01\x09\x02' + bytes(7)
    struct.pack_into('<16sHHIQQQIHHHHHH', image, 0, ident, 0xfe10, 62, 1,
                     0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', image, 64, 1, 6, 0, 0, 0, len(image), len(image), 0x4000)
    struct.pack_into('<IIQQQQQQ', image, 120, 2, 6, 0x200, 0x200, 0,
                     len(tags) * 16, len(tags) * 16, 8)
    pathlib.Path(path).write_bytes(image)


if __name__ == '__main__':
    prepare(sys.argv[1])
