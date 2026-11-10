"""One-shot read-only native SSL/BGFT capture from a run's observed placements.

No debugger attachment, suspension, target writes or injected calls. Bounded
reads cover code fingerprints, SSL/BGFT, service owners and selected resource rules.
This concurrent snapshot is evidence, not an atomic state or UI-success check.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import re
import struct

from resource_capture import ReadResult, collect_resource, load_expected


def code(path, rva, size):
    data = path.read_bytes()
    phoff = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    for i in range(count):
        h = struct.unpack_from('<II6Q', data, phoff + i * stride)
        if h[0] == 1 and h[3] <= rva and rva + size <= h[3] + h[5]:
            start = h[2] + rva - h[3]
            return data[start:start + size], hashlib.sha256(data).hexdigest()
    raise ValueError('fingerprint outside file segments')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--creation-time', type=int, required=True, help='FILETIME from the launching controller process handle')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    log_size = args.log.stat().st_size
    # Placements precede guest execution. Hash the explicitly bounded prefix,
    # rather than rejecting an otherwise useful capture after guest log growth.
    with args.log.open('rb') as stream:
        log_bytes = stream.read(1024 * 1024)
    text = log_bytes.decode('utf-8', errors='replace')
    bases = {}
    for name in ('SceShellCore.elf', 'libSceSsl.sprx'):
        matches = re.findall(r'ps5-vsh: place ' + re.escape(name) + r' base=(0x[0-9a-fA-F]+)', text)
        if len(matches) != 1:
            raise ValueError('missing/ambiguous placement for ' + name)
        bases[name] = int(matches[0], 16)
    ssl, ssl_hash = code(args.firmware / 'system_b/common/lib/libSceSsl.sprx', 0x340, 16)
    core, core_hash = code(args.firmware / 'system_b/vsh/SceShellCore.elf', 0x12221fe, 12)
    if ssl_hash != '848900fb0ec5aee14d4e2bb2e98b78a00b910dbaa6cd96e45d24c1e06ea4f49e':
        raise ValueError('unrecognized SSL profile')
    if core_hash != '6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a':
        raise ValueError('unrecognized ShellCore profile')
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    process = k.OpenProcess(0x1010, False, args.pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not process:
        raise C.WinError(C.get_last_error())
    result = dict(pid=args.pid, bases={n: hex(v) for n, v in bases.items()}, ssl_sha256=ssl_hash,
                  core_sha256=core_hash, snapshots=[], atomic=False,
                  placement_log=dict(path=str(args.log.resolve()), observed_file_bytes=log_size,
                                     prefix_bytes=len(log_bytes), prefix_sha256=hashlib.sha256(log_bytes).hexdigest()))

    def read(label, address, size):
        buffer = C.create_string_buffer(size)
        got = C.c_size_t()
        ok = k.ReadProcessMemory(process, address, buffer, size, C.byref(got))
        result['snapshots'].append(dict(label=label, address=hex(address), transferred=got.value,
                                        hex=buffer.raw[:got.value].hex(), available=bool(ok and got.value == size)))
        if not ok or got.value != size:
            raise RuntimeError(label + ': unavailable memory')
        return buffer.raw

    try:
        name, capacity = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not k.QueryFullProcessImageNameW(process, 0, name, C.byref(capacity)):
            raise C.WinError(C.get_last_error())
        if Path(name.value).resolve() != args.emulator.resolve():
            raise ValueError('unexpected process image')
        times = [W.FILETIME() for _ in range(4)]
        if not k.GetProcessTimes(process, *(C.byref(t) for t in times)):
            raise C.WinError(C.get_last_error())
        result['creation_filetime'] = times[0].dwLowDateTime | times[0].dwHighDateTime << 32
        if result['creation_filetime'] != args.creation_time:
            raise ValueError('process creation time differs from launch controller')
        if args.log.stat().st_mtime_ns // 100 + 116444736000000000 < args.creation_time:
            raise ValueError('placement log predates target process')
        result['emulator_sha256'] = hashlib.sha256(args.emulator.read_bytes()).hexdigest()
        s, c = bases['libSceSsl.sprx'], bases['SceShellCore.elf']
        if read('SSL code', s + 0x340, len(ssl)) != ssl or read('BGFT check code', c + 0x12221fe, len(core)) != core:
            raise ValueError('runtime fingerprints differ')
        root, last_id, count = struct.unpack('<Qii', read('SSL registry', s + 0x169188, 16))
        ready = read('BGFT ready', c + 0x2262eb8, 1)[0]
        result.update(ssl_root=hex(root), ssl_last_id=last_id, ssl_context_count=count, bgft_ready=ready)
        for label, rva in (('preparation_owner', 0x2156ae0), ('recovery_owner', 0x226d298)):
            result[label] = hex(struct.unpack('<Q', read(label, c + rva, 8))[0])
        # All process/placement/hash/code admission above remains prerequisite.
        # This adapter owns no target references and never retries partial reads.
        def resource_read(address, size):
            buffer = C.create_string_buffer(size)
            got = C.c_size_t()
            ok = k.ReadProcessMemory(process, address, buffer, size, C.byref(got))
            error = C.get_last_error() if not ok else 0
            return ReadResult(buffer.raw[:got.value], bool(ok and got.value == size),
                              'ReadProcessMemory error=' + str(error) if error else '')

        expected = load_expected(args.firmware / 'system_b/priv/resource_arbitrator_ro/ra-config.json')
        resource = collect_resource(resource_read, c, expected)
        result['resource_capture'] = resource
        # Preserve old summary keys for existing evidence consumers. The nested
        # collector's completeness/match/recheck fields govern interpretation.
        for source, target in (('owner', 'resource_owner'), ('rule', 'resource_rule'),
                               ('control', 'resource_rule_control'), ('fields', 'resource_rule_fields'),
                               ('publication_unchanged_during_read', 'resource_publication_unchanged_during_read')):
            if source in resource:
                result[target] = resource[source]
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        k.CloseHandle(process)
        args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
