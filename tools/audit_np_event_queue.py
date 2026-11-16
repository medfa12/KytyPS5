"""Read-only regular-13.60 EventQueue export/control-flow inventory.

Usage: python -B tools/audit_np_event_queue.py PATH_TO_libSceNpCommon.sprx
Requires Capstone. Reports indirect calls for manual ownership review; it does
not treat a decoded closure as proof of runtime initialization or credentials.
"""
import hashlib
import json
import pathlib
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

b = pathlib.Path(sys.argv[1]).read_bytes()
assert len(b) == 411092
assert hashlib.sha256(b).hexdigest() == 'a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3'
po = struct.unpack_from('<Q', b, 32)[0]
pe, pn = struct.unpack_from('<HH', b, 54)
ph = [struct.unpack_from('<IIQQQQQQ', b, po + i * pe) for i in range(pn)]


def off(va):
    return next(p[2] + va - p[3] for p in ph if p[0] == 1 and p[3] <= va < p[3] + p[5])


def executable(va):
    return any(p[0] == 1 and p[1] & 1 and p[3] <= va < p[3] + p[5] for p in ph)


dynamic = next(p for p in ph if p[0] == 2)
tags = dict(struct.iter_unpack('<QQ', b[dynamic[2]:dynamic[2] + dynamic[5]]))
strings = off(tags[5])
count = struct.unpack_from('<I', b, off(tags[4]) + 4)[0]
symbols = []
for i in range(count):
    name, info, other, section, va, size = struct.unpack_from('<IBBHQQ', b, off(tags[6]) + i * 24)
    name = b[strings + name:].split(b'\0', 1)[0].decode()
    symbols.append((name, va, size))
expected = {
    'esiO4He2WTU': 0x1dd50, 'OGAIG7dVmUk': 0x1dd50,
    '8kUkQPQP7bA': 0x1ddb0, 'lQXgvDXBGtA': 0x1ddb0,
    'N1gnYosdK7Q': 0x1ddf0, 'E4uoqSdo8ek': 0x1de00,
    'xu9qWN0YYC4': 0x1de50, 'b20e017Ei94': 0x1de60,
    'qvpEuKumIGM': 0x1dff0, 'VI8AHrfLdqY': 0x1e030,
    'jxPY-0x8e-M': 0x1e040, 'AV5jHo8O3+E': 0x1e050,
    'suxln7PooIo': 0x1e180, 'QlP4t2SGZ4I': 0x1e2b0,
    'slmKkuIoC28': 0x1e350, 'aoas3bJANfY': 0x1e3c0,
}
actual = {name.split('#')[0]: va for name, va, size in symbols if 0x1dd50 <= va < 0x1e410}
assert actual == expected
relocations = {}
for tag, size in [(7, 8), (23, 2)]:
    start = off(tags[tag])
    for va, info, addend in struct.iter_unpack('<QQq', b[start:start + tags[size]]):
        relocations[va] = (symbols[info >> 32][0] if info >> 32 else '', addend)

decoder = Cs(CS_ARCH_X86, CS_MODE_64)
decoder.detail = True
pending = list(expected.values())
seen, functions, indirect, imports, data, vtables = set(), set(pending), {}, set(), set(), set()


def enqueue(va):
    assert executable(va), hex(va)
    if va not in seen:
        pending.append(va)


while pending:
    address = pending.pop()
    if address in seen:
        continue
    i = next(decoder.disasm(b[off(address):off(address) + 15], address, count=1), None)
    assert i and i.address == address, hex(address)
    seen.add(address)
    # Follow the complete contiguous relative-relocation vtable when referenced
    # by a native constructor/destructor, including virtual deleting entries.
    for operand in i.operands:
        if operand.type == 3 and i.reg_name(operand.mem.base) == 'rip':
            target = i.address + i.size + operand.mem.disp
            data.add(target)
            at = target
            while at in relocations:
                name, relative = relocations[at]
                if name or not executable(relative):
                    break
                vtables.add(at)
                functions.add(relative)
                enqueue(relative)
                at += 8
    branch = i.mnemonic == 'call' or i.mnemonic.startswith('j')
    if branch:
        operand = i.operands[0]
        if operand.type == 2:
            enqueue(operand.imm)
            if i.mnemonic == 'call' or i.mnemonic == 'jmp':
                functions.add(operand.imm)
        elif operand.type == 3 and i.reg_name(operand.mem.base) == 'rip':
            target = i.address + i.size + operand.mem.disp
            assert target in relocations and relocations[target][0], hex(address)
            imports.add(relocations[target][0])
        else:
            indirect[hex(address)] = i.mnemonic + ' ' + i.op_str
    if i.mnemonic not in ('jmp', 'ret', 'ud2', 'int3'):
        enqueue(i.address + i.size)

print(json.dumps({
    'exports': {name: hex(va) for name, va in expected.items()},
    'decoded_instruction_count': len(seen),
    'internal_function_and_tail_targets': sorted(hex(va) for va in functions),
    'vtable_slots_followed': sorted(hex(va) for va in vtables),
    'imports': sorted(imports),
    'relocated_data_references': {hex(va): relocations[va][0] for va in sorted(data)
                                  if va in relocations and relocations[va][0]},
    'indirect_callback_sites_to_review': indirect,
    'scope': 'native component closure, not Core startup or online service readiness',
}, indent=2))
