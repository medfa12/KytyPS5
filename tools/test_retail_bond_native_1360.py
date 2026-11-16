"""Execute hash-pinned retail leaf consumers against an HLE-produced MBUS record.

Windows AMD64, offline only: allocates executable scratch in this test process.
No emulator attach, firmware writes, imports, navigation or runtime store access.
Runs the real PSM converter and LoginMgr virtual type/subtype getters; constructor
selection and managed dictionary/allowlist are separately audited, not executed.
"""
import argparse
import ctypes as C
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
from elftools.elf.elffile import ELFFile
import inspect_appbrowse_aot_1360 as aot


class Elf:
    def __init__(self, path, sha):
        self.data = path.read_bytes()
        assert hashlib.sha256(self.data).hexdigest() == sha, 'original firmware hash'
        elf = ELFFile(io.BytesIO(self.data))
        self.loads = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        tags = {a: b for s in elf.iter_segments() if s['p_type'] == 'PT_DYNAMIC' for a, b in struct.iter_unpack('<QQ', s.data())}
        self.rel = {a: v for a, kind, v in struct.iter_unpack('<QQq', self.read(tags[7], tags[8])) if kind == 8}

    def read(self, address, size):
        for base, offset, length in self.loads:
            if base <= address and address + size <= base + length:
                return self.data[offset + address - base:offset + address - base + size]
        raise ValueError('unmapped firmware window')


class Executable:
    def __init__(self, data):
        self.kernel = C.WinDLL('kernel32', use_last_error=True)
        alloc = self.kernel.VirtualAlloc
        alloc.argtypes, alloc.restype = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32], C.c_void_p
        self.address = alloc(None, len(data), 0x3000, 4)
        if not self.address:
            raise C.WinError(C.get_last_error())
        C.memmove(self.address, bytes(data), len(data))
        old = C.c_uint32()
        protect = self.kernel.VirtualProtect
        protect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(C.c_uint32)]
        assert protect(self.address, len(data), 0x20, C.byref(old)), 'executable protection'
        flush = self.kernel.FlushInstructionCache
        flush.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        assert flush(C.c_void_p(-1), self.address, len(data)), 'instruction cache'

    def close(self):
        free = self.kernel.VirtualFree
        free.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
        assert free(self.address, 0, 0x8000), 'scratch release'


def invoke(address, first, second=0):
    # Preserve Windows nonvolatile RDI/RSI and align the SysV call stack.
    shim = Executable(bytes.fromhex('57564883ec084889cf4889d648b8') +
                      struct.pack('<Q', address) + bytes.fromhex('ffd04883c4085e5fc3'))
    try:
        return C.WINFUNCTYPE(C.c_uint64, C.c_void_p, C.c_void_p)(shim.address)(first, second)
    finally:
        shim.close()


def native_getters(eboot, record):
    subtype = struct.unpack_from('<i', record, 12)[0]
    assert subtype in (22, 24)
    table = 0x117ce30 if subtype == 22 else 0x117bcc8
    expected = (0x390310, 0x390330) if subtype == 22 else (0x354870, 0x354890)
    assert (eboot.rel[table + 600], eboot.rel[table + 608]) == expected, 'real vtable getter identity'
    scratch = []
    try:
        methods = {}
        for slot, size in ((600, 26 if subtype == 22 else 31), (608, 29 if subtype == 22 else 26)):
            code = Executable(eboot.read(eboot.rel[table + slot], size))
            scratch.append(code)
            methods[slot] = code.address
        if subtype == 22:
            assert eboot.rel[table + 256] == 0x390390
            assert eboot.read(0x390390, 8) == bytes.fromhex('837f10000f94c0c3')
            code = Executable(eboot.read(0x390390, 8))
            scratch.append(code)
            methods[256] = code.address
        vtable = C.create_string_buffer(616)
        for slot, address in methods.items():
            C.memmove(C.addressof(vtable) + slot, struct.pack('<Q', address), 8)
        # Native device base constructor stores the MBUS record after its vptr.
        obj = C.create_string_buffer(struct.pack('<Q', C.addressof(vtable)) + record)
        return [invoke(methods[slot], C.addressof(obj)) for slot in (600, 608)]
    finally:
        for code in scratch:
            code.close()


def managed_maps(login_path, peripheral_path):
    aot.HASHES['bond_login'] = '72a74dce1d5d8fad59d3fb9872cbd23472dd432d2e3d99fd209391af9b8f5088'
    aot.HASHES['bond_peripheral'] = '8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa'
    # Full source identity, then the actual constructor key and string patches.
    login = aot.Image(login_path, 'bond_login')
    def string(image, instruction):
        code = image.read(instruction, 7)
        return image.patch(instruction + 7 + struct.unpack('<i', code[3:])[0])
    assert login.read(0x48085, 5) == bytes.fromhex('be15000000')
    assert string(login, 0x48061) == "'controller'"
    assert login.read(0x481f9, 5) == bytes.fromhex('be18000000')
    assert string(login, 0x481e2) == "'vrController'"
    peripheral = aot.Image(peripheral_path, 'bond_peripheral')
    assert [string(peripheral, a) for a in (0x73d33, 0x73d48, 0x73d60)] == ["'controller'", "'standardController'", "'leonardo'"]
    return {21: 'controller', 24: 'vrController'}, {'controller', 'standardController', 'leonardo'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--eboot', type=Path, required=True)
    parser.add_argument('--modules', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True, help='256-byte production export fixture output')
    args = parser.parse_args()
    assert os.name == 'nt' and C.sizeof(C.c_void_p) == 8
    eboot = Elf(args.eboot, 'f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060')
    psm = Elf(args.modules / 'libScePsm.sprx.elf', '0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6')
    record = args.record.read_bytes()
    assert len(record) == 256 and struct.unpack_from('<ii', record, 8) == (0, 22)
    assert struct.unpack_from('<i', record, 200)[0] == -1
    mapping, allowed = managed_maps(args.modules / 'Sce.Vsh.ShellUI.LoginMgr.dll.sprx.elf',
                                    args.modules / 'Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx.elf')
    # PSM leaf uses a RIP-relative switch table; retain its exact RVA spacing.
    image = bytearray(0x2ca000)
    image[0x40eb0:0x40fbf] = psm.read(0x40eb0, 0x10f)
    image[0x2c97c0:0x2c9844] = psm.read(0x2c97c0, 33 * 4)
    executable = Executable(image)
    rows = []
    try:
        for kind in (22, 24):
            wire = bytearray(record)
            struct.pack_into('<i', wire, 12, kind)
            source, result = C.create_string_buffer(bytes(wire)), C.create_string_buffer(32)
            invoke(executable.address + 0x40eb0, C.addressof(result), C.addressof(source))
            hardware = struct.unpack_from('<i', result.raw, 16)[0]
            port = struct.unpack('<i', psm.read(0x2c9a2c + (hardware - 1) * 4, 4))[0]
            native_type, native_subtype = native_getters(eboot, bytes(wire))
            text = mapping[native_type]
            rows.append(dict(mbus_subtype=kind, psm_internal_hardware=hardware, pad_open_type=port,
                             native_type=native_type, native_subtype=native_subtype,
                             peripheral_type=text, onboarding_type_allowed=text in allowed))
        assert rows[0] == dict(mbus_subtype=22, psm_internal_hardware=10, pad_open_type=0,
                               native_type=21, native_subtype=2, peripheral_type='controller', onboarding_type_allowed=True)
        assert rows[1] == dict(mbus_subtype=24, psm_internal_hardware=9, pad_open_type=4,
                               native_type=24, native_subtype=2, peripheral_type='vrController', onboarding_type_allowed=False)
    finally:
        executable.close()
    print(json.dumps(dict(production_record_sha256=hashlib.sha256(record).hexdigest(),
                         native_leaf_execution=True, retail_guest_run=False, cases=rows), indent=2))


if __name__ == '__main__':
    main()
