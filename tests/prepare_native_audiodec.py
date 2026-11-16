"""Prepare local-only 13.60 decoder images for NativeAudiodecCpuTests.

No firmware bytes are committed. No emulator, stores, network or audio device.
Usage: python prepare_native_audiodec.py <firmware common/lib> <output directory>
"""
import pathlib
import struct
import sys


def prepare(source, output):
    data = source.read_bytes()
    assert data[:4] == b'\x7fELF'
    po = struct.unpack_from('<Q', data, 32)[0]
    pe, pn = struct.unpack_from('<HH', data, 54)
    ph = [struct.unpack_from('<IIQQQQQQ', data, po + pe * n) for n in range(pn)]
    image = bytearray(max(p[3] + p[6] for p in ph if p[0] == 1))
    dynamic = {}
    for p in ph:
        if p[0] == 1:
            image[p[3]:p[3] + p[5]] = data[p[2]:p[2] + p[5]]
        if p[0] == 2:
            for tag, value in struct.iter_unpack('<QQ', data[p[2]:p[2] + p[5]]):
                dynamic[tag] = value
    base = next((p[2] for p in ph if p[0] == 0x61000000), 0)
    def off(address):
        return next(p[2] + address - p[3] for p in ph if p[0] == 1 and p[3] <= address < p[3] + p[5])
    sony = 0x61000035 in dynamic
    so = base + dynamic[0x61000035] if sony else off(dynamic[5])
    strings = data[so:so + dynamic[0x61000037 if sony else 10]]
    def name(offset):
        return strings[offset:strings.index(0, offset)].decode().split('#')[0]
    sy = base + dynamic[0x61000039] if sony else off(dynamic[6])
    symbol_bytes = dynamic[0x6100003f] if sony else struct.unpack_from('<I', data, off(dynamic[4]) + 4)[0] * 24
    symbols = list(struct.iter_unpack('<IBBHQQ', data[sy:sy + symbol_bytes]))
    relocations = []
    tables = ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)) if sony else ((23, 2), (7, 8))
    for table, size in tables:
        start = base + dynamic[table] if sony else off(dynamic[table])
        for address, info, addend in struct.iter_unpack('<QQq', data[start:start + dynamic[size]]):
            kind, index = info & 0xffffffff, info >> 32
            assert kind in (1, 6, 7, 8)
            relocations.append(f'{address} {kind} {addend} {name(symbols[index][0]) if index else "-"}')
    output.with_suffix('.image').write_bytes(image)
    output.with_suffix('.relocs').write_text('\n'.join(relocations))
    exports = [f'{name(s[0])} {s[4]}' for s in symbols if s[3] and s[0]]
    output.with_suffix('.exports').write_text('\n'.join(exports))


if __name__ == '__main__':
    firmware, out = map(pathlib.Path, sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    for module in ('libSceAudiodecCpu', 'libSceAudiodecCpuDdp'):
        prepare(firmware / (module + '.sprx'), out / module)
