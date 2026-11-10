"""Bounded, read-only LibcInternal state capture; requires a run-bound inventory."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time

PROVIDER_HASH = '8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935'
BLOCKS = [(0x1b2a10, 0x608), (0x1b3018, 0x150), (0x1b40e8, 8),
          (0x1b5278, 0x98), (0x1b54c0, 0x48), (0x1b77b8, 0x450),
          (0x1b7cb8, 0x60), (0x1b7f18, 0x30), (0x1b81b8, 8),
          (0x1c09f8, 0xc20), (0x1c16c0, 0x30), (0x1c4e20, 8)]
ROOTS = (0x1b7cc8, 0x1b7cd0, 0x1b7cd8)
RELATIVE = {0x1b2a18: 0x2b0, 0x1b2a88: 0x1b3168, 0x1b3160: 0x100,
            0x1b40e8: 0x4830, 0x1b54c0: 0x1e9a0, 0x1b54e0: 0x1ead0,
            0x1b5500: 0x1eb20}


def stable_bytes(path):
    before = path.stat()
    data = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f'File changed during hashing: {path}')
    return data, dict(path=str(path.resolve()), bytes=len(data), mtime_ns=after.st_mtime_ns,
                      sha256=hashlib.sha256(data).hexdigest())


def elf_metadata(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Requires the audited plain little-endian ELF64 provider')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    entsize, count = struct.unpack_from('<HH', data, 54)
    if entsize != 56 or count > 128:
        raise ValueError('Unexpected ELF program headers')
    headers = [struct.unpack_from('<II6Q', data, phoff + n * entsize) for n in range(count)]
    segments = [dict(type=h[0], flags=h[1], offset=h[2], rva=h[3], filesz=h[5], memsz=h[6])
                for h in headers]
    dyn = next(h for h in headers if h[0] == 2)
    tags = dict(struct.iter_unpack('<qQ', data[dyn[2]:dyn[2] + dyn[5]]))

    def file_offset(rva):
        for h in headers:
            if h[0] == 1 and h[3] <= rva < h[3] + h[5]:
                return h[2] + rva - h[3]
        raise ValueError('Dynamic address lacks file backing')

    strings = file_offset(tags[5])
    symbols = file_offset(tags[6])
    relocations = []
    for offset_tag, size_tag in [(7, 8), (23, 2)]:
        start = file_offset(tags[offset_tag])
        for slot, info, addend in struct.iter_unpack('<QQq', data[start:start + tags[size_tag]]):
            index, kind = info >> 32, info & 0xffffffff
            if not index:
                continue
            name_offset, symbol_info = struct.unpack_from('<IB', data, symbols + index * 24)
            name_start = strings + name_offset
            name = data[name_start:data.index(b'\0', name_start)].decode('ascii')
            if name:
                if not 0x1b2a10 <= slot < 0x1b3018:
                    raise ValueError('Named relocation outside audited block')
                relocations.append(dict(slot_rva=hex(slot), symbol=name, kind=kind,
                                        symbol_type=symbol_info & 15))
    if len(relocations) != 179:
        raise ValueError('Unexpected named relocation count')
    return segments, relocations


def file_code(data, segments, rva, length):
    for s in segments:
        if s['type'] == 1 and s['rva'] <= rva and rva + length <= s['rva'] + s['filesz']:
            start = s['offset'] + rva - s['rva']
            return data[start:start + length]
    raise ValueError('Fingerprint lacks file backing')


class Region(C.Structure):
    _fields_ = [('base', C.c_void_p), ('allocation', C.c_void_p), ('allocation_protect', W.DWORD),
                ('partition', W.WORD), ('size', C.c_size_t), ('state', W.DWORD),
                ('protect', W.DWORD), ('kind', W.DWORD)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', required=True, type=Path,
                        help='JSON: pid, creation_time (FILETIME integer), image_path, provider_path, provider_bias, placement evidence')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if C.sizeof(C.c_void_p) != 8 or os.name != 'nt':
        parser.error('Requires Windows x64')
    inventory_bytes, inventory_identity = stable_bytes(args.inventory)
    inventory = json.loads(inventory_bytes)
    provider_data, provider_identity = stable_bytes(Path(inventory['provider_path']))
    if provider_identity['sha256'] != PROVIDER_HASH:
        raise ValueError('Provider differs from audited image')
    segments, relocations = elf_metadata(provider_data)
    _, image_identity = stable_bytes(Path(inventory['image_path']))
    _, observer_identity = stable_bytes(Path(__file__))
    base = int(inventory['provider_bias'], 0)
    if not inventory.get('placement_evidence') or not 0 < base < (1 << 47) - 0x200000:
        raise ValueError('Missing placement evidence or invalid bias')
    evidence = inventory['placement_evidence']
    log_path = Path(evidence['log'])
    if log_path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('Placement log exceeds bound')
    log_bytes, log_identity = stable_bytes(log_path)
    if log_identity['sha256'] != evidence['sha256']:
        raise ValueError('Placement evidence hash mismatch')
    log_text = log_bytes.decode('utf-8', errors='replace').replace('\\', '/')
    places = re.findall(r'ps5-vsh: place libSceLibcInternal\.sprx base=(0x[0-9a-fA-F]+) mapped=(0x[0-9a-fA-F]+)', log_text)
    if len(places) != 1 or int(places[0][0], 0) != base:
        raise ValueError('Missing, ambiguous or inconsistent placement')
    loader_paths = re.findall(r'--- Relocate program: ([^\r\n]+?) ---', log_text)
    expected_path = os.path.normcase(str(Path(inventory['provider_path']).resolve()))
    if not any(os.path.normcase(str(Path(path).resolve())) == expected_path for path in loader_paths):
        raise ValueError('Provider path missing from placement log')
    if any(not 0 <= s['rva'] <= s['rva'] + s['memsz'] <= int(places[0][1], 0)
           for s in segments if s['type'] == 1):
        raise ValueError('ELF segments exceed observed mapping extent')
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    k.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
    k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    k.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(Region), C.c_size_t]
    k.VirtualQueryEx.restype = C.c_size_t
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    # VirtualQueryEx requires QUERY_INFORMATION, in addition to VM_READ.
    # These rights permit observation only: no VM_WRITE/VM_OPERATION/suspend.
    process = k.OpenProcess(0x1410, False, inventory['pid'])
    if not process:
        raise C.WinError(C.get_last_error())
    started = time.monotonic()
    result = dict(schema='native-libc-provider-state/v1', identity=dict(inventory=inventory_identity,
                  provider=provider_identity, image=image_identity, observer=observer_identity,
                  pid=inventory['pid'], provider_bias=hex(base), file_segments=segments,
                  placement_log=log_identity, process_access='0x1410'),
                  samples=[], requested_bytes=0, read_attempts=0, termination_reason='identity_failure',
                  assessment=dict(constructor_completion='unknown', required_bindings='unknown'))
    sample_bytes = 0

    def read(label, address, length, records):
        nonlocal sample_bytes
        if (time.monotonic() - started >= 20 or not 0 < length <= 4096 or
                sample_bytes + length > 16384 or result['requested_bytes'] + length > 163840 or
                result['read_attempts'] >= 512):
            result['termination_reason'] = 'deadline' if time.monotonic() - started >= 20 else 'budget'
            raise RuntimeError('capture budget')
        sample_bytes += length
        result['requested_bytes'] += length
        result['read_attempts'] += 1
        item = dict(label=label, address=hex(address), requested=length, transferred=0,
                    elapsed_seconds=time.monotonic() - started, raw_hex=None, error=None)
        records.append(item)
        if not 0 < address < (1 << 47) - length:
            item['error'] = 'address range'
            return None
        cursor = address
        # At most two mapping boundaries for these short reads; never walk guest lists.
        for _ in range(3):
            region = Region()
            if not k.VirtualQueryEx(process, cursor, C.byref(region), C.sizeof(region)):
                item['error'] = C.get_last_error()
                return None
            if region.state != 0x1000 or region.protect & 0x100 or (region.protect & 0xff) not in (2, 4, 8, 0x20, 0x40, 0x80):
                item['error'] = 'unreadable region'
                return None
            cursor = (region.base or 0) + region.size
            if cursor >= address + length:
                break
        else:
            item['error'] = 'region bound'
            return None
        buffer, transferred = C.create_string_buffer(length), C.c_size_t()
        ok = k.ReadProcessMemory(process, address, buffer, length, C.byref(transferred))
        item['transferred'] = transferred.value
        item['raw_hex'] = buffer.raw[:transferred.value].hex()
        if not ok or transferred.value != length:
            item['error'] = C.get_last_error() or 'partial read'
            return None
        return buffer.raw

    try:
        times = [W.FILETIME() for _ in range(4)]
        if not k.GetProcessTimes(process, *(C.byref(t) for t in times)):
            raise C.WinError(C.get_last_error())
        creation = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        if creation != inventory['creation_time']:
            raise ValueError('PID creation time mismatch')
        name, count = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not k.QueryFullProcessImageNameW(process, 0, name, C.byref(count)):
            raise C.WinError(C.get_last_error())
        if os.path.normcase(str(Path(name.value).resolve())) != os.path.normcase(image_identity['path']):
            raise ValueError('Process executable mismatch')
        result['identity']['creation_time'] = creation
        identity_reads = result['identity']['reads'] = []
        for rva in (0x10, 0x63f0):
            live = read('code_identity', base + rva, 64, identity_reads)
            if live != file_code(provider_data, segments, rva, 64):
                raise ValueError(f'Unexplained provider code mismatch at {rva:x}')
        result['termination_reason'] = 'sampling'
        # Seven snapshots leave headroom for the supervisor's hard 20s deadline.
        for index, offset in enumerate((0, 1, 2, 4, 8, 12, 16)):
            time.sleep(max(0, started + offset - time.monotonic()))
            if time.monotonic() - started >= 20:
                result['termination_reason'] = 'deadline'
                break
            code = W.DWORD()
            if not k.GetExitCodeProcess(process, C.byref(code)) or code.value != 259:
                result['termination_reason'] = 'process_exit'
                break
            sample_bytes = 0
            sample = dict(index=index, elapsed_seconds=time.monotonic() - started,
                          non_atomic=True, reads=[], auxiliary_heaps=[], unstable_fields=[])
            result['samples'].append(sample)
            blocks = {rva: read(hex(rva), base + rva, size, sample['reads']) for rva, size in BLOCKS}

            def value(rva, size=8):
                for start, block in blocks.items():
                    if block is not None and start <= rva and rva + size <= start + len(block):
                        return int.from_bytes(block[rva - start:rva - start + size], 'little')
                return None

            sample['thread_mode'] = value(0x1b7cc0, 4)
            sample['relative_bindings'] = [dict(slot=hex(r), actual=hex(value(r)) if value(r) is not None else None,
                expected=hex(base + target), matches=value(r) == base + target if value(r) is not None else None)
                for r, target in RELATIVE.items()]
            sample['bindings'] = [dict(rel, live_address=hex(value(int(rel['slot_rva'], 0)))
                                      if value(int(rel['slot_rva'], 0)) is not None else None,
                                      implementation='unknown', provenance='live GOT; resolution path not observed')
                                  for rel in relocations]
            for root in ROOTS:
                pointer = value(root)
                if pointer:
                    block = read('auxiliary_heap', pointer + 0x350, 0xb0, sample['reads'])
                    sample['auxiliary_heaps'].append(dict(root=hex(root), pointer=hex(pointer), raw_hex=block.hex() if block else None))
            for slot, length in ((0x1b2a10, 8), (0x1b2a70, 0x10), (0x1b2a78, 8), (0x1b2a80, 8)):
                pointer = value(slot)
                if pointer:
                    read('known_data_' + hex(slot), pointer, length, sample['reads'])
            for root, length in [(0x1b7cc0, 4)] + [(r, 8) for r in ROOTS + (0x1b2a10, 0x1b2a70, 0x1b2a78, 0x1b2a80)]:
                again = read('root_reread', base + root, length, sample['reads'])
                if again is None or int.from_bytes(again, 'little') != value(root, length):
                    sample['unstable_fields'].append(hex(root))
            sample['stability_caveat'] = 'Equal rereads do not exclude ABA or establish atomicity'
        else:
            result['termination_reason'] = 'completed'
    except (OSError, ValueError, RuntimeError) as error:
        result['error'] = str(error)
        if result['termination_reason'] == 'sampling':
            result['termination_reason'] = 'capture_error'
    finally:
        k.CloseHandle(process)
        result['elapsed_seconds'] = time.monotonic() - started
        args.output.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(result, indent=2).encode()
        args.output.write_bytes(payload)
        args.output.with_suffix(args.output.suffix + '.sha256').write_text(hashlib.sha256(payload).hexdigest() + '\n')
    print(args.output)
    if 'error' in result:
        raise SystemExit(result['error'])


if __name__ == '__main__':
    main()
