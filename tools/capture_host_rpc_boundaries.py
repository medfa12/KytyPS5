"""Bounded RPC binding snapshots using the existing hardware-execution lifecycle.

No data watchpoints or code patches. Four snapshots narrow the corruption
interval; they cannot identify a writer or observe changes between boundaries.
DLL identities and RVAs are specific to the installed, IDA-audited Windows build.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import time

import pefile

from capture_home_gates_1360 import capture, process_identity
from capture_native_hang import Capture
from home_gate_trace_1360 import DecodeError, digest, require

PINS = {
    'dhcp': ('dhcpcsvc6.dll', '8f552e6e00a13bdb439180fad0277fe8cb664d2e8d38e7f58fbabdf0e67976a8'),
    'rpc': ('RPCRT4.dll', 'c060f02c11fb19c9c8a30b5077a87cdb17d4be40f08070a539ec73eb53402775'),
}
SITES = {'created': 0x1eb2, 'option': 0x1ee8, 'auth': 0x1f1c, 'unbind': 0x1df0}


class PEImage:
    def __init__(self, path, expected):
        self.data = path.read_bytes()
        require(digest(self.data) == expected, 'rpc_dll_hash')
        self.pe = pefile.PE(data=self.data, fast_load=True)
        require(self.pe.FILE_HEADER.Machine == 0x8664 and self.pe.OPTIONAL_HEADER.Magic == 0x20b,
                'rpc_dll_architecture')
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage

    def at(self, rva, size):
        require(0 <= rva < self.size and size > 0 and rva + size <= self.size, 'rpc_image_bounds')
        data = self.pe.get_data(rva, size)
        require(len(data) == size, 'rpc_image_short')
        return data


def images():
    root = Path(os.environ['SystemRoot'])/'System32'
    result = {key: PEImage(root/name, sha) for key, (name, sha) in PINS.items()}
    # Audited instruction boundaries, not inferred nearest-export locations.
    for name in ('created', 'option', 'auth'):
        require(result['dhcp'].at(SITES[name], 5) == bytes.fromhex('0f1f440000'), 'rpc_boundary_nop')
    require(result['dhcp'].at(SITES['unbind'], 5) == bytes.fromhex('4889542410'), 'rpc_unbind_abi')
    require(result['rpc'].at(0x649f4, 7) == bytes.fromhex('488b80a0000000'), 'rpc_fault_abi')
    return result


class Model:
    phase = 'rpc_boundaries'
    ready = False

    def __init__(self, emit, bases, sizes):
        self.emit, self.bases, self.sizes = emit, bases, sizes
        self.pending, self.frames, self.sequence = {}, {}, 0

    def thread_boundary(self, tid):
        for pointer, item in list(self.pending.items()):
            if item['tid'] == tid:
                self.emit(dict(event='binding_incomplete', reason='thread_boundary', **item))
                self.pending.pop(pointer)
        self.frames = {key: value for key, value in self.frames.items() if key[0] != tid}

    def close_coverage(self, now, reason):
        self.emit(dict(event='coverage_closed', reason=reason, pending=list(self.pending.values()),
                       limitation='Boundary snapshots only; no writer or inter-boundary coverage'))

    def outstanding(self, now):
        return list(self.pending.values())

    def hit(self, name, reader, regs, now, tid):
        status = regs['rax'] & 0xffffffff
        frame = (tid, regs['rsp'])
        row = dict(event='binding_boundary', boundary=name, tid=tid,
                   registers={k: hex(v) for k, v in regs.items()})
        if name != 'unbind':
            row['status'] = status
        if name == 'created' and status:
            self.frames.pop(frame, None)
            self.emit(dict(row, creation_failed=True))
            return
        pointer = regs['rdx'] if name == 'unbind' else reader.q(regs['rsp'] + 0x48)
        require(pointer != 0 and pointer % 8 == 0, 'rpc_binding_pointer')
        if name == 'created':
            if pointer in self.pending:
                self.emit(dict(event='binding_incomplete', reason='address_reused_without_observed_unbind',
                               **self.pending.pop(pointer)))
            require(len(self.pending) < 256, 'rpc_pending_budget')
            self.sequence += 1
            self.pending[pointer] = dict(binding=self.sequence, pointer=hex(pointer), tid=tid)
            self.frames[frame] = pointer
        item = self.pending.get(pointer)
        correlated = item is not None and (name == 'unbind' or self.frames.get(frame) == pointer)
        row.update(pointer=hex(pointer), binding=item['binding'] if correlated else None,
                   creation_observed=correlated)
        try:
            raw = reader.raw(pointer, 0x40)
            vtable = struct.unpack_from('<Q', raw)[0]
            row.update(object_hex=raw.hex(), vtable=hex(vtable),
                       signature=hex(struct.unpack_from('<I', raw, 8)[0]),
                       kind=hex(struct.unpack_from('<I', raw, 12)[0]),
                       vtable_in_rpc_image=self.bases['rpc'] <= vtable < self.bases['rpc'] + self.sizes['rpc'])
            if item is not None and correlated:
                row['changed_since_previous_boundary'] = 'last_vtable' in item and item['last_vtable'] != hex(vtable)
                row['previous_vtable'] = item.get('last_vtable')
                item['last_vtable'] = hex(vtable)
            try:
                row['free_slot'] = hex(reader.q(vtable + 0xa0))
            except DecodeError:
                row['free_slot_unreadable'] = True
            row['stack_qwords'] = [hex(v) for v in struct.unpack('<24Q', reader.raw(regs['rsp'], 24 * 8))]
        except DecodeError:
            self.emit(dict(row, snapshot_incomplete=True))
            raise
        finally:
            if name == 'unbind':
                self.pending.pop(pointer, None)
                self.frames = {k: v for k, v in self.frames.items() if v != pointer}
        self.emit(row)


class Observer:
    mode = 'host_rpc_boundaries'
    conclusion = 'Boundary snapshots only; no data write watch, writer attribution or RNPS success inference'
    probes = {name: ('dhcp', rva) for name, rva in SITES.items()}
    windows = {'dhcp': [(rva, 8) for rva in SITES.values()], 'rpc': [(0x649f4, 7)]}

    def __init__(self, local, bases):
        self.local = local
        self.sizes = {key: image.size for key, image in local.items()}
        self.watched_module_bases = tuple(bases.values())

    def validate_attached(self, cap, bases):
        # Revalidate after attachment while the target is stopped, closing the
        # discovery-to-attach race. Later watched unload events stop capture.
        current, _ = validate_modules(cap, self.local)
        require(current == bases, 'rpc_module_changed_before_arm')

    def model(self, emit, bases):
        return Model(emit, bases, self.sizes)


def validate_modules(cap, local):
    modules = cap.modules()
    bases, report = {}, {}
    for key, (name, sha) in PINS.items():
        found = [(b, s, p) for b, s, p in modules if Path(p).name.lower() == name.lower()]
        require(len(found) == 1, 'rpc_module_missing_or_ambiguous')
        base, size, path = found[0]
        expected = Path(os.environ['SystemRoot'])/'System32'/name
        require(Path(path).resolve() == expected.resolve(), 'rpc_module_path')
        require(digest(Path(path).read_bytes()) == sha and size == local[key].size, 'rpc_module_identity')
        header = local[key].data[:local[key].pe.OPTIONAL_HEADER.SizeOfHeaders]
        rebased = bytearray(header)
        offset = local[key].pe.OPTIONAL_HEADER.get_field_absolute_offset('ImageBase')
        struct.pack_into('<Q', rebased, offset, base)
        require(cap.read_exact(base, len(header)) in (header, bytes(rebased)), 'rpc_live_headers')
        bases[key] = base
        report[key] = dict(path=path, sha256=sha, base=hex(base), size=size)
    return bases, report


def locate(args, local):
    cap = Capture(args.pid, '', False)
    try:
        cap.check_architecture()
        process_identity(cap, args)
        deadline = time.monotonic() + args.module_wait
        while True:
            modules = cap.modules()
            selected = {key: [(b, s, p) for b, s, p in modules if Path(p).name.lower() == name.lower()]
                        for key, (name, _) in PINS.items()}
            require(all(len(found) <= 1 for found in selected.values()), 'ambiguous_rpc_module')
            if all(selected.values()):
                break
            require(time.monotonic() < deadline, 'rpc_modules_not_loaded')
            time.sleep(0.1)
        bases, report = validate_modules(cap, local)
        report['coverage'] = 'Attaches after modules load; earlier bindings and between-boundary writes are unobserved'
        report['timing_budget'] = dict(module_wait_seconds=args.module_wait,
            observation_seconds=args.seconds, cleanup_reserve_seconds=30,
            note='Observation ceiling 180s; module wait and orderly cleanup are additional. Never kill debugger on deadline.')
        return bases, report
    finally:
        cap.finish()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--attach', action='store_true')
    p.add_argument('--pid', type=int)
    p.add_argument('--created-filetime', type=int)
    p.add_argument('--exe', type=Path)
    p.add_argument('--exe-sha256')
    p.add_argument('--run-log', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--seconds', type=int, default=180, help='Observation window, maximum 180s; cleanup additional')
    p.add_argument('--module-wait', type=int, default=60, help='Pre-attach module wait, maximum 60s')
    p.add_argument('--max-hits', type=int, default=4000)
    p.add_argument('--max-events', type=int, default=20000)
    p.add_argument('--max-bytes', type=int, default=8388608)
    args = p.parse_args()
    require(os.name == 'nt' and C.sizeof(C.c_void_p) == 8, 'windows_amd64_required')
    require(1 <= args.seconds <= 180 and 0 <= args.module_wait <= 60 and 1 <= args.max_hits <= 20000
            and 1 <= args.max_events <= 50000 and 2097152 <= args.max_bytes <= 16777216, 'capture_budgets')
    local = images()
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, process_access=False, sites=SITES, pins=PINS), indent=2))
        return
    if not all((args.pid, args.created_filetime, args.exe, args.exe_sha256, args.run_log, args.output)):
        p.error('--attach requires identity, run-log and output')
    bases, report = locate(args, local)
    capture(args, local, bases, report, observer=Observer(local, bases))


if __name__ == '__main__':
    main()
