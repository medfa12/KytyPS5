"""Read-only, hash-pinned AppBrowse AOT/metadata inspector.

Resolves JIT and LLVM GOT string patches to embedded #US values, and maps
method-address call-table entries to MethodDef names. No managed code execution.
Requires capstone, dnfile, pyelftools. All addresses/counts are hexadecimal.
"""
import argparse
import hashlib
import io
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import capstone
import dnfile
from elftools.elf.elffile import ELFFile
from prepare_shellui_managed_1360 import embedded_pe

HASHES = {
    'db': '739392215badb90d3caed3616d01e753c587c65aeaacd378fb4e3129948ad766',
    'vsh': '64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee',
}


class Image:
    def __init__(self, path, kind):
        self.data = Path(path).read_bytes()
        if hashlib.sha256(self.data).hexdigest() != HASHES[kind]:
            raise ValueError('Expected pinned regular 13.60 ' + kind + ' image')
        elf = ELFFile(io.BytesIO(self.data))
        self.loads = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        tags = {}
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_DYNAMIC':
                for tag, value in struct.iter_unpack('<QQ', segment.data()):
                    if not tag:
                        break
                    tags[tag] = value
        self.rel = {a: v for a, info, v in struct.iter_unpack('<QQq', self.read(tags[7], tags[8])) if info == 8}
        symbols = [struct.unpack('<IBBHQQ', self.read(tags[6] + i * 24, 24)) for i in range(tags[0x6100003f] // 24)]
        infos = [s[4] for s in symbols if s[5] == 552]
        if len(infos) != 1:
            raise ValueError('Ambiguous AOT file-info structure')
        info = infos[0]
        self.gots = [(self.rel[info + 8], self.rel[info + 0x90]), (self.rel[info + 0x10], self.rel[info + 0x98])]
        self.blob = self.rel[info + 0x58]
        self.pe = dnfile.dnPE(data=embedded_pe(self.data))
        call_table = self.rel[info + 0x40]
        self.methods = {}
        for typ in self.pe.net.mdtables.TypeDef:
            for method in typ.MethodList:
                address = call_table + (method.row_index - 1) * 5
                code = self.read(address, 5)
                if code[0] == 0xe8:
                    target = address + 5 + struct.unpack('<i', code[1:])[0]
                    self.methods[target] = str(typ.TypeNamespace) + '.' + str(typ.TypeName) + '::' + str(method.row.Name)

    def read(self, address, size):
        for base, offset, length in self.loads:
            if base <= address and address + size <= base + length:
                start = offset + address - base
                return self.data[start:start + size]
        raise ValueError('Unmapped address ' + hex(address))

    def decode(self, address):
        data = self.read(address, 5)
        head = data[0]
        if head < 128:
            return head, address + 1
        if head < 192:
            return ((head & 63) << 8) | data[1], address + 2
        if head != 255:
            return ((head & 31) << 24) | int.from_bytes(data[1:4], 'big'), address + 4
        return int.from_bytes(data[1:5], 'big', signed=True), address + 5

    def offset(self, table, index):
        count, group_size, groups, width = struct.unpack('<IIII', self.read(table, 16))
        if index >= count or not group_size or width not in (2, 4):
            raise ValueError('Invalid GOT offset index')
        group = index // group_size
        cursor = table + 16 + width * groups + int.from_bytes(self.read(table + 16 + width * group, width), 'little')
        value, cursor = self.decode(cursor)
        for _ in range(group * group_size + 1, index + 1):
            delta, cursor = self.decode(cursor)
            value += delta
        return value

    def patch(self, address):
        for base, table in self.gots:
            if address < base or (address - base) % 8:
                continue
            try:
                index = (address - base) // 8
                cursor = self.blob + self.offset(table, index)
                kind, cursor = self.decode(cursor)
                first, cursor = self.decode(cursor)
                second, cursor = self.decode(cursor)
                # Mono patch-info's LDSTR=15; image0 is this embedded assembly.
                if kind == 15 and first == 0:
                    return repr(self.pe.net.user_strings.get(second & 0xffffff).value)
                return f'GOT[{index}] kind={kind} {first:x} {second:x}'
            except ValueError:
                continue
        return ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=HASHES)
    parser.add_argument('image', type=Path)
    parser.add_argument('windows', nargs='*')
    parser.add_argument('--methods', help='Print matching MethodDef names and AOT addresses')
    args = parser.parse_args()
    image = Image(args.image, args.kind)
    print('SHA256', HASHES[args.kind])
    if args.methods:
        for address, name in image.methods.items():
            if args.methods.lower() in name.lower():
                print(hex(address), name)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    for window in args.windows:
        address, size = [int(s, 16) for s in window.split(':')]
        if not 0 < size <= 0x8000:
            parser.error('Window must be 1..0x8000 bytes')
        print('WINDOW', window, image.methods.get(address, ''))
        for instruction in decoder.disasm(image.read(address, size), address):
            notes = []
            for operand in instruction.operands:
                if operand.type == capstone.CS_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP:
                    notes.append(image.patch(instruction.address + instruction.size + operand.mem.disp))
            if instruction.mnemonic in ('call', 'jmp') and instruction.operands[0].type == capstone.CS_OP_IMM:
                notes.append(image.methods.get(instruction.operands[0].imm, ''))
            print(hex(instruction.address), instruction.mnemonic, instruction.op_str, ' '.join(notes))


if __name__ == '__main__':
    main()
