"""Snapshot CPU backing for bounded graphics Texture observations (Windows x64).

Read-only, non-atomic: GPU-written textures may have stale or inaccessible CPU
backing. This captures evidence, not a synchronized GPU readback.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt' or C.sizeof(C.c_void_p) != 8 or args.pid <= 0:
        parser.error('Requires Windows x64 and a positive PID')
    with args.capture.open('rb') as source:
        data = source.read(524545)
    if len(data) != 524544 or struct.unpack_from('<QII', data) != (0x3142534f5550474b, 1, 4096):
        raise ValueError('Unexpected observation schema')
    pid, frequency, start, delay = struct.unpack_from('<4Q', data, 16)
    if pid != args.pid or not frequency or struct.unpack_from('<Q', data, 200)[0] != 1:
        raise ValueError('Capture PID mismatch or incomplete initialization')
    entries = {}
    for i in range(4096):
        commit, event, tick, *values = struct.unpack_from('<16Q', data, 256 + 128 * i)
        if commit != i + 1 or event != 15:
            continue
        shader, stage, ordinal, address, size, *words = values
        key = (address, size, tuple(words))
        if key not in entries:
            if len(entries) >= 128:
                break
            entries[key] = dict(address=hex(address), size=size,
                                descriptor=[hex(w) for w in words],
                                width=(((words[1] >> 30) & 3) | ((words[2] & 0xfff) << 2)) + 1,
                                height=((words[2] >> 14) & 0x3fff) + 1,
                                format=(words[1] >> 20) & 0x1ff,
                                tile=(words[3] >> 20) & 31,
                                first_seconds=(tick - start) / frequency, shaders=[])
        use = dict(hash=hex(shader), stage=stage, ordinal=ordinal)
        if use not in entries[key]['shaders']:
            entries[key]['shaders'].append(use)
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    k.QueryPerformanceCounter.argtypes = [C.POINTER(C.c_longlong)]
    process = k.OpenProcess(0x1010, False, args.pid)
    if not process:
        raise C.WinError(C.get_last_error())
    requested = 0
    try:
        name, length = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not k.QueryFullProcessImageNameW(process, 0, name, C.byref(length)):
            raise C.WinError(C.get_last_error())
        if os.path.normcase(os.path.realpath(name.value)) != os.path.normcase(str(args.image.resolve())):
            raise ValueError('Unexpected target executable')
        times = [W.FILETIME() for _ in range(4)]
        if not k.GetProcessTimes(process, *(C.byref(t) for t in times)):
            raise C.WinError(C.get_last_error())
        creation = ((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime) / 1e7 - 11644473600
        now = C.c_longlong()
        if not k.QueryPerformanceCounter(C.byref(now)):
            raise C.WinError(C.get_last_error())
        capture_time = time.time() - (now.value - start) / frequency
        if creation > capture_time + 1:
            raise ValueError('Process was created after the observation; possible PID reuse')
        args.output.mkdir(parents=True, exist_ok=False)
        deadline = time.monotonic() + 10
        for index, ((address, size, _), entry) in enumerate(entries.items()):
            if not 0 < address < (1 << 47) - size or not 0 < size <= 4 * 1024 * 1024:
                entry['skipped'] = 'address or per-image size bound'
                continue
            if requested + size > 32 * 1024 * 1024 or time.monotonic() >= deadline:
                entry['skipped'] = 'total size or time bound'
                continue
            requested += size
            buffer, count = C.create_string_buffer(size), C.c_size_t()
            if not k.ReadProcessMemory(process, address, buffer, size, C.byref(count)) or count.value != size:
                entry['read_error'] = C.get_last_error()
                entry['transferred'] = count.value
                continue
            filename = f'{index:03d}-{address:x}.bin'
            (args.output / filename).write_bytes(buffer.raw)
            entry['file'] = filename
        result = dict(pid=args.pid, image=name.value, creation_unix=creation,
                      snapshot_unix=time.time(),
                      capture=str(args.capture.resolve()), non_atomic=True,
                      requested_bytes=requested, textures=list(entries.values()))
        (args.output / 'textures.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(f'{len(entries)} texture bindings; {requested} requested bytes; {args.output}')
    finally:
        k.CloseHandle(process)


if __name__ == '__main__':
    main()
