"""Read-only, bounded 13.60 PsmUtil video-player snapshot (Windows x64)."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--base', type=lambda v: int(v, 0), default=0x9c0000000)
    parser.add_argument('--image', type=Path, default=Path(__file__).resolve().parents[1] /
                        '_Build/windows/kyty_emulator.exe')
    args = parser.parse_args()
    if C.sizeof(C.c_void_p) != 8 or args.pid <= 0:
        parser.error('Requires x64 Python and a positive PID')
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p,
                                       C.c_size_t, C.POINTER(C.c_size_t)]
    kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR,
                                                C.POINTER(W.DWORD)]
    handle = kernel.OpenProcess(0x1010, False, args.pid)
    if not handle:
        raise C.WinError(C.get_last_error())
    total = 0

    def read(address, length):
        nonlocal total
        if not address or not 0 < length <= 4096 or total + length > 65536:
            raise ValueError('Read exceeds snapshot bounds')
        total += length
        buf = C.create_string_buffer(length)
        count = C.c_size_t()
        if not kernel.ReadProcessMemory(handle, address, buf, length, C.byref(count)) or count.value != length:
            raise C.WinError(C.get_last_error())
        return buf.raw

    def q(address):
        return struct.unpack('<Q', read(address, 8))[0]

    result = dict(pid=args.pid, base=hex(args.base), non_atomic=True, players=[])
    try:
        image = C.create_unicode_buffer(32768)
        size = W.DWORD(len(image))
        if not kernel.QueryFullProcessImageNameW(handle, 0, image, C.byref(size)):
            raise C.WinError(C.get_last_error())
        if os.path.normcase(os.path.realpath(image.value)) != os.path.normcase(str(args.image.resolve())):
            raise ValueError('Target is not the expected emulator')
        # Native 13.60 PsmVideoPlayer::Init entry and Create registration string.
        if read(args.base + 0xa630, 6) != bytes.fromhex('554889e55350'):
            raise ValueError('Unexpected PsmUtil code identity')
        identity = b'Sce.Vsh.Video.PsmVideoPlayer::Create('
        if read(args.base + 0x2b9e3, len(identity)) != identity:
            raise ValueError('Unexpected PsmUtil registration identity')
        # Native handle-table lookup at +0xa630 uses sentinel +0x38988;
        # each list node contains prev/next, int handle, and player pointer.
        root = args.base + 0x38988
        sentinel = q(root)
        result['sentinel'] = hex(sentinel)
        node = q(sentinel + 8)
        seen = set()
        while node != sentinel:
            if node in seen or len(seen) >= 16:
                raise ValueError('Unexpected or concurrently changing handle list')
            seen.add(node)
            raw = read(node, 32)
            player = struct.unpack_from('<Q', raw, 24)[0]
            entry = dict(node=hex(node), handle=struct.unpack_from('<i', raw, 16)[0],
                         player=hex(player))
            result['players'].append(entry)
            vtable = q(player)
            entry['vtable'] = hex(vtable)
            if not args.base <= vtable < args.base + 0x44000:
                entry['error'] = 'Player vtable is outside PsmUtil; possibly retired node'
                node = struct.unpack_from('<Q', raw, 8)[0]
                continue
            init = q(vtable + 0xd0)
            entry['native_init'] = hex(init)
            # Only VideoCore's audited subclass has source id at +0x2c40.
            if init == args.base + 0x14f30:
                uri, length = struct.unpack('<QQ', read(player + 0xe0, 16))
                entry['uri'] = read(uri, min(length, 4096)).decode('utf-8', errors='replace') if length else ''
                entry['uri_length'] = length
                entry['source_id'] = struct.unpack('<i', read(player + 0x2c40, 4))[0]
                entry['canvas_id'] = struct.unpack('<i', read(player + 0x2c4c, 4))[0]
                entry['canvas_type'] = struct.unpack('<i', read(player + 0x178, 4))[0]
                entry['state'] = struct.unpack('<i', read(player + 0x180, 4))[0]
                entry['ready_state'] = struct.unpack('<i', read(player + 0x2c74, 4))[0]
                entry['resolution'] = struct.unpack('<ii', read(player + 0x2c64, 8))
                entry['position_ms'] = q(player + 0x1b0)
                entry['duration_ms'] = q(player + 0x1b8)
                entry['uri_root_stable'] = read(player + 0xe0, 16) == struct.pack('<QQ', uri, length)
            entry['node_root_stable'] = read(node + 16, 16) == raw[16:32]
            node = struct.unpack_from('<Q', raw, 8)[0]
        result['table_root_stable'] = q(root) == sentinel
        result['bytes_read'] = total
    finally:
        kernel.CloseHandle(handle)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
