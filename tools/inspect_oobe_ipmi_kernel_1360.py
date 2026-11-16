"""Bounded read-only 13.60 IPMI kernel inspection for the OOBE ordering audit."""
from pathlib import Path
import re
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

def parse_phdrs(blob):
    offset = struct.unpack_from('<Q', blob, 0x20)[0]
    extent, count = struct.unpack_from('<HH', blob, 0x36)
    result = []
    for index in range(count):
        kind, flags, file_offset, address, _, file_size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', blob, offset + extent * index)
        result.append(dict(type=kind, flags=flags, off=file_offset, vaddr=address, filesz=file_size))
    return result

def vaddr_to_off(headers, address):
    return next((p['off'] + address - p['vaddr'] for p in headers
                 if p['type'] == 1 and p['vaddr'] <= address < p['vaddr'] + p['filesz']), None)

from local_paths import configured_path
path = configured_path() / 'decrypted_modules/x86_kernel.elf'
data = path.read_bytes()
segments = parse_phdrs(data)
base = next(p['vaddr'] for p in segments if p['type'] == 1)
decoder = Cs(CS_ARCH_X86, CS_MODE_64)
decoder.detail = True

def va(offset):
    return next(p['vaddr'] + offset - p['off'] for p in segments
                if p['type'] == 1 and p['off'] <= offset < p['off'] + p['filesz'])

def text(address):
    offset = vaddr_to_off(segments, address)
    if offset is None:
        return ''
    end = data.find(b'\0', offset, offset + 240)
    value = data[offset:end] if end >= offset else b''
    if len(value) > 3 and all(32 <= c < 127 or c in (9, 10, 13) for c in value):
        return repr(value.decode())
    return ''

def dis(address, count):
    if address < base:
        address += base
    offset = vaddr_to_off(segments, address)
    assert offset is not None and 0 < count <= 0x4000
    instructions = list(decoder.disasm(data[offset:offset + count], address))
    if not instructions:
        raise ValueError(f'No instruction decoded at {address:#x}; verify the function boundary')
    for ins in instructions:
        annotations = []
        for op in ins.operands:
            if op.type == 3 and ins.reg_name(op.mem.base) == 'rip':
                target = ins.address + ins.size + op.mem.disp
                annotations.append(f'@+{target-base:x} {text(target)}')
        print(f'+{ins.address-base:x}: {ins.mnemonic:8} {ins.op_str:48} {"; ".join(annotations)}')

if sys.argv[1] == 'dis':
    for region in sys.argv[2:]:
        address, count = (int(v, 0) for v in region.split(':'))
        dis(address, count)
elif sys.argv[1] == 'xref':
    targets = {}
    for needle in sys.argv[2:]:
        for match in re.finditer(re.escape(needle.encode()), data):
            targets[va(match.start())] = needle
    print('base', hex(base), 'targets', {hex(k-base): v for k,v in targets.items()})
    for p in segments:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        code = data[p['off']:p['off'] + p['filesz']]
        for match in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
            at = match.start()
            address = p['vaddr'] + at
            target = address + 7 + struct.unpack_from('<i', code, at + 3)[0]
            if target in targets:
                print(f'+{address-base:x} -> +{target-base:x}: {targets[target]}')
elif sys.argv[1] == 'calls':
    targets = {base + int(v, 0) for v in sys.argv[2:]}
    for p in segments:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        code = data[p['off']:p['off'] + p['filesz']]
        for match in re.finditer(rb'[\xe8\xe9]', code):
            at = match.start()
            if at + 5 <= len(code):
                address = p['vaddr'] + at
                target = address + 5 + struct.unpack_from('<i', code, at + 1)[0]
                if target in targets:
                    print(f'candidate +{address-base:x} -> +{target-base:x}')
