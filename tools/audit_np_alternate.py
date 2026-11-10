"""Read-only reproduction of the reviewed plain-ELF NpCommon comparison.

Usage: python -B tools/audit_np_alternate.py ORIGINAL ALTERNATE
Requires Capstone. No guest execution or file writes. Not credential admission.
"""
import hashlib
import json
import pathlib
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


class Image:
    def __init__(self, path, digest):
        self.b = pathlib.Path(path).read_bytes()
        assert len(self.b) == 411092 and hashlib.sha256(self.b).hexdigest() == digest
        po = struct.unpack_from('<Q', self.b, 32)[0]
        pe, pn = struct.unpack_from('<HH', self.b, 54)
        self.ph = [struct.unpack_from('<IIQQQQQQ', self.b, po + pe*i) for i in range(pn)]
        self.d = {}
        for p in self.ph:
            if p[0] == 2:
                for tag, value in struct.iter_unpack('<QQ', self.b[p[2]:p[2]+p[5]]):
                    if not tag:
                        break
                    self.d[tag] = value
        self.relocs = {}
        for tag, size in [(23, 2), (7, 8)]:
            start = self.off(self.d[tag])
            for at in range(start, start+self.d[size], 24):
                va, info, addend = struct.unpack_from('<QQq', self.b, at)
                assert va not in self.relocs and va % 8 == 0
                assert any(p[0] == 1 and p[1] & 2 and p[3] <= va and va+8 <= p[3]+p[6] for p in self.ph)
                self.relocs[va] = (info, addend)
                if info & 0xffffffff == 8:
                    assert any(p[0] == 1 and p[3] <= addend < p[3]+p[6] for p in self.ph)
        count = struct.unpack_from('<I', self.b, self.off(self.d[4])+4)[0]
        self.symbols = self.data(self.d[6], count*24)

    def off(self, va):
        for p in self.ph:
            if p[0] == 1 and p[3] <= va < p[3]+p[5]:
                return p[2]+va-p[3]
        raise ValueError(hex(va))

    def data(self, va, size):
        at = self.off(va)
        return self.b[at:at+size]

    def text(self, va):
        raw = self.data(va, min(4096, 0x45950-va))
        assert b'\0' in raw
        return raw.split(b'\0')[0]


a = Image(sys.argv[1], 'a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3')
b = Image(sys.argv[2], '81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35')
assert a.ph == b.ph and a.d == b.d and a.symbols == b.symbols
assert a.relocs.keys() == b.relocs.keys() and len(a.relocs) == 537
changed_relocs = 0
for va, old in a.relocs.items():
    new = b.relocs[va]
    if old != new:
        assert old[0] == new[0] == 8
        assert a.text(old[1]) == b.text(new[1])
        changed_relocs += 1
assert changed_relocs == 73
for p in a.ph:
    if p[0] == 1 and p[1] & 2:
        assert a.b[p[2]:p[2]+p[5]] == b.b[p[2]:p[2]+p[5]]
cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True
cs.skipdata = True
p = next(p for p in a.ph if p[0] == 1 and p[1] & 1)
aa, bb = [list(cs.disasm(x.data(0, p[5]), 0)) for x in (a, b)]
assert len(aa) == len(bb)


def target(ins, image):
    op = ins.operands[-1]
    if op.type != 3 or ins.reg_name(op.mem.base) != 'rip':
        return None
    va = ins.address + ins.size + op.mem.disp
    if va in image.relocs:
        return ('reloc', image.relocs[va])
    if 0x34000 <= va < 0x45950:
        return ('text', image.text(va).replace(b'W:\\Build\\J03950017\\', b'W:\\Build\\J03950016\\'))
    return ('address', va)


changed = 0
for i, j in zip(aa, bb):
    if i.address == j.address and i.bytes == j.bytes:
        continue
    changed += 1
    assert (i.address, i.size, i.mnemonic) == (j.address, j.size, j.mnemonic)
    assert i.id and j.id and i.disp_size and i.disp_size == j.disp_size
    left, right = bytearray(i.bytes), bytearray(j.bytes)
    left[i.disp_offset:i.disp_offset+i.disp_size] = b'\0' * i.disp_size
    right[j.disp_offset:j.disp_offset+j.disp_size] = b'\0' * j.disp_size
    assert left == right and target(i, a) is not None and target(i, a) == target(j, b), hex(i.address)
assert len(aa) == 66693 and changed == 1457
print(json.dumps(dict(decoded_items=len(aa), explained_changes=changed,
                     relocations=len(a.relocs), reordered_string_relocations=changed_relocs,
                     symbols_equal=True, writable_bytes_equal=True), indent=2))
