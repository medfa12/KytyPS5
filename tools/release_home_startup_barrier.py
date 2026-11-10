"""Parent-only barrier check/release; never attaches a debugger or launches a guest.

--release requires a matching currently armed particle/route/job observer receipt. Without
it, only process identity and the named ready event are checked (no signal).
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import re


def validate_armed(data, pid, created, exe_hash):
    if len(data) > 16777216:
        raise ValueError('observer receipt exceeds budget')
    # A live writer may be part-way through its last line. Only complete records
    # count; a truncated armed event cannot authorize release.
    lines = data.splitlines(keepends=True)
    rows = [json.loads(line) for line in lines if line.endswith(b'\n')]
    provenance = [r for r in rows if r.get('event') == 'provenance']
    armed = [r for r in rows if r.get('event') == 'armed']
    if len(provenance) != 1 or len(armed) != 1:
        raise ValueError('need one provenance and one complete armed record')
    p = provenance[0]
    expected_version = {'job': 9, 'route': 8}.get(p.get('trace_mode'), 7)
    if p.get('diagnostic_version') != expected_version or p.get('lifecycle_version') != 2:
        raise ValueError('unexpected observer/cleanup contract')
    if (p.get('pid'), p.get('created_filetime'), p.get('exe_sha256', '').lower()) != (pid, created, exe_hash.lower()):
        raise ValueError('armed observer belongs to another process generation')
    if p.get('trace_mode') not in ('particle0', 'particle1', 'route', 'job') or armed[0].get('phase') != p['trace_mode']:
        raise ValueError('only an armed particle/route/job observer authorizes this release')
    if any(r.get('event', '').startswith('shutdown') or r.get('event') == 'end' for r in rows):
        raise ValueError('observer already draining or detached')
    return p['trace_mode']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--created-filetime', type=int, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--exe-sha256', required=True)
    parser.add_argument('--token', required=True)
    parser.add_argument('--release', action='store_true')
    parser.add_argument('--armed-trace', type=Path)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{32}', args.token):
        parser.error('--token must be32 lowercase hexadecimal characters')
    if args.release and not args.armed_trace:
        parser.error('--release requires --armed-trace')
    # Same identity checks as the observer; OpenProcess read/query access only.
    from capture_native_hang import Capture, bind, HANDLE, DWORD, BOOL
    from capture_home_gates_1360 import process_identity
    cap = Capture(args.pid, '', False)
    ready = release = None
    try:
        cap.check_architecture()
        process_identity(cap, args)
        open_event = bind(cap.k, 'OpenEventW', HANDLE, DWORD, BOOL, C.c_wchar_p)
        wait = bind(cap.k, 'WaitForSingleObject', DWORD, HANDLE, DWORD)
        prefix = f'Local\\KytyPreGuest-{args.pid}-{args.token}'
        ready = open_event(0x100000, False, prefix + '-ready')
        if not ready or wait(ready, 0) != 0:
            raise RuntimeError('matching pre-guest barrier is not ready')
        if args.release:
            mode = validate_armed(args.armed_trace.read_bytes(), args.pid, args.created_filetime, args.exe_sha256)
            release = open_event(2, False, prefix + '-release')
            # Recheck identity immediately before signaling. Event possession
            # alone is not proof that the supplied executable is this process.
            process_identity(cap, args)
            debugger = BOOL()
            if not bind(cap.k, 'CheckRemoteDebuggerPresent', BOOL, HANDLE, C.POINTER(BOOL))(cap.process, C.byref(debugger)):
                raise C.WinError(C.get_last_error())
            if not debugger.value:
                raise RuntimeError('startup observer debugger is no longer attached')
            if not release or not bind(cap.k, 'SetEvent', BOOL, HANDLE)(release):
                raise C.WinError(C.get_last_error())
            print(json.dumps(dict(released=True, pid=args.pid, observer_mode=mode)))
        else:
            print(json.dumps(dict(ready=True, released=False, pid=args.pid)))
    finally:
        if release:
            cap.close(release)
        if ready:
            cap.close(ready)
        cap.finish()


if __name__ == '__main__':
    main()
