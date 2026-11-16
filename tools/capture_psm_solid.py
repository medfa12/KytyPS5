"""Read-only bounded 13.60 PSM solid-fill state capture; never invokes guest code."""
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
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--image', type=Path, required=True)
    args = parser.parse_args()
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    h = k.OpenProcess(0x1010, False, args.pid)
    if not h:
        raise C.WinError(C.get_last_error())
    used = 0
    output = dict(pid=args.pid, time=time.time(), non_atomic=True, programs=[])

    def read(a, n):
        nonlocal used
        if not a or not 0 < n <= 4096 or used + n > 65536:
            raise ValueError('Invalid pointer/length or 64KiB capture budget exceeded')
        used += n
        b = C.create_string_buffer(n)
        got = C.c_size_t()
        if not k.ReadProcessMemory(h, a, b, n, C.byref(got)) or got.value != n:
            raise C.WinError(C.get_last_error())
        return b.raw

    def q(a):
        return int.from_bytes(read(a, 8), 'little')

    def u(a):
        return int.from_bytes(read(a, 4), 'little')

    def resource(b, n):
        entry = dict(object=hex(b))
        try:
            backing = q(q(b + 0x178) + 0x58)
            raw = read(backing, n)
            entry.update(backing=hex(backing), hex=raw.hex(), floats=struct.unpack('<' + 'f' * (n // 4), raw))
        except (OSError, ValueError) as error:
            entry['error'] = str(error)
        return entry

    def program(s):
        ps, vs = q(s + 0xa0), q(s + 0xa8)
        if not ps or not vs or q(ps + 0x70) != P + 0x366098 or q(vs + 0x70) != P + 0x363ad8:
            return
        entry = dict(pointer=hex(s), current=s == current, ps=hex(ps), vs=hex(vs))
        output['programs'].append(entry)
        for stage, offset, length in [('ps', 0xb0, 0x3c), ('vs', 0xc8, 0x50)]:
            count = u(s + offset + 0x10)
            data = dict(size=count, dirty=read(s + offset + 0x14, 1)[0])
            entry[stage + '_uniforms'] = data
            if count >= length:
                shadow = read(q(s + offset + 8), length)
                data.update(shadow_hex=shadow.hex(), shadow_floats=struct.unpack('<' + 'f' * (length // 4), shadow),
                            uploaded=resource(q(s + offset), length))

    P = 0x950000000
    try:
        path = C.create_unicode_buffer(32768)
        size = W.DWORD(len(path))
        if not k.QueryFullProcessImageNameW(h, 0, path, C.byref(size)):
            raise C.WinError(C.get_last_error())
        if os.path.normcase(os.path.realpath(path.value)) != os.path.normcase(str(args.image.resolve())):
            raise ValueError('Unexpected target image')
        output['image'] = path.value
        g = q(P + 0x42ced8)
        current = q(g + 0x28)
        output.update(context=hex(g), current_program=hex(current), global_buffer=resource(q(g + 0x170), 8),
                      blend_hex=read(g + 0x14c, 0x1c).hex())
        m = q(P + 0x42ceb8)
        buckets, count = q(m), u(m + 8)
        if count > 1024:
            raise ValueError('Registry bucket count exceeds cap')
        seen, programs = set(), set()
        for index in range(count):
            node = q(buckets + index * 8)
            while node:
                if node in seen or len(seen) >= 256:
                    raise ValueError('Registry cycle/race or entry cap')
                seen.add(node)
                s = q(node + 0x18)
                if s and s not in programs:
                    programs.add(s)
                    program(s)
                node = q(node + 8)
        output.update(registry_entries=len(seen), registry_stable=q(P + 0x42ceb8) == m and q(m) == buckets and u(m + 8) == count,
                      context_stable=q(P + 0x42ced8) == g, current_stable=q(g + 0x28) == current)
        vertex = q(g + 0x38)
        output['vertex'] = dict(pointer=hex(vertex))
        if vertex:
            vc, streams = u(vertex + 0x28), u(vertex + 0x30)
            output['vertex'].update(count=vc, stream_count=streams, streams=[])
            if streams <= 16:
                for i, stride in enumerate([12, 16, 8][:min(streams, 3)]):
                    address = q(vertex + 0x80 + 8 * i)
                    n = min(vc, 4) * stride
                    output['vertex']['streams'].append(dict(address=hex(address), hex=read(address, n).hex() if n else '',
                                                            uploaded_object=hex(q(vertex + 0xf0 + 8 * i))))
    except (OSError, ValueError) as error:
        output['error'] = str(error)
    finally:
        k.CloseHandle(h)
    output['read_bytes'] = used
    dest = Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode' / f'psm-solid-{args.pid}-{time.time_ns()}.json'
    with dest.open('x', encoding='utf-8') as file:
        json.dump(output, file, indent=2)
    print(dest)
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
