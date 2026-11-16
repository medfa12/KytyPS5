"""Read-only 13.60 readiness audit. Requires capstone and retained ELF inspector.

No guest execution or firmware writes. Addresses are RVAs, sizes are bytes.
"""
import argparse
import runpy
import struct
from pathlib import Path
from local_paths import configured_path

p = argparse.ArgumentParser()
p.add_argument('module', choices=['engine', 'citron', 'esvm', 'interface', 'videocore', 'server'])
p.add_argument('operation', choices=['dis', 'near', 'around', 'calls', 'refs', 'table', 'strings'])
p.add_argument('values', nargs='+')
p.add_argument('--inspector', default=str(Path(__file__).resolve().parents[1] / '_TempData/lifecycle-re-inspect.py'))
p.add_argument('--firmware', help='Extracted firmware root (or KYTY_FIRMWARE_ROOT)')
a = p.parse_args()
a.firmware = str(configured_path(a.firmware))
names = dict(engine='libSceEsvmEngine.sprx', citron='citroncore.elf', esvm='libSceEsvm.sprx', interface='libSceMediaFrameworkInterface.sprx', videocore='libSceVideoCoreInterface.sprx')
relative = '/system_b/vsh/app/NPXS40039/eboot.bin' if a.module == 'server' else '/system_b/common/lib/' + names[a.module]
i = runpy.run_path(a.inspector)['Image'](a.firmware + relative)
if a.operation in ('near', 'around'):
    address = int(a.values[0], 0)
    back = min(address, 0x1000)
    offset = i.data(address - back, back).rfind(b'\x55\x48\x89\xe5')
    assert offset >= 0, 'no bounded candidate prologue'
    start = address - back + offset
    if a.operation == 'around':
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        radius = int(a.values[1], 0)
        assert 0 < radius <= 0x1000
        for at, size, mnemonic, operands in Cs(CS_ARCH_X86, CS_MODE_64).disasm_lite(i.data(start, address - start + radius), start):
            if at >= address - radius:
                print(f'{at:x}: {mnemonic:10} {operands}')
    else:
        i.dis(start, address - start + int(a.values[1], 0), {})
elif a.operation == 'dis':
    i.dis(int(a.values[0], 0), int(a.values[1], 0), {})
elif a.operation == 'calls':
    i.calls(a.values)
elif a.operation == 'table':
    start, size = [int(x, 0) for x in a.values]
    assert 0 < size <= 0x1000
    for pos in range(start, start + size, 8):
        print(hex(pos), hex(i.ptr(pos)), i.relocs.get(pos, ''))
else:
    targets = set(int(x, 0) for x in a.values) if a.operation == 'refs' else set()
    if a.operation == 'strings':
        for text in a.values:
            needle = text.encode()
            for segment in i.ph:
                if segment[0] != 1:
                    continue
                data = i.b[segment[2]:segment[2] + segment[5]]
                pos = data.find(needle)
                while pos >= 0:
                    targets.add(segment[3] + pos)
                    print('STRING', hex(segment[3] + pos), text)
                    pos = data.find(needle, pos + 1)
    for addr, rel in i.relocs.items():
        if rel[2] in targets:
            print('RELOCATION', hex(addr), rel)
    for seg in i.ph:
        if seg[0] != 1 or not seg[1] & 1:
            continue
        data = i.data(seg[3], seg[5])
        for n in range(len(data) - 7):
            if data[n] in (0x48, 0x4c) and data[n + 1] == 0x8d and data[n + 2] & 0xc7 == 5:
                target = seg[3] + n + 7 + struct.unpack_from('<i', data, n + 3)[0]
                if target in targets:
                    print('LEA', hex(seg[3] + n), hex(target))
