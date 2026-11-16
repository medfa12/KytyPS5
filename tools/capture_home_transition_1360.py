"""Offline by default; explicit bounded native transition capture for the parent.

Uses capture_home_gates' process identity, hardware traps and verified detach.
Modes are separate runs; never rearm slots to infer missed initialization.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re

from home_gate_trace_1360 import Image, require, digest
from home_transition_trace_1360 import Observer, SHA256, audit


def provenance(original, stage, log):
    data = original.read_bytes()
    image = Image(data, SHA256)
    staged = stage.read_bytes()
    stage_hash = digest(staged)
    # Existing prepare_shellui_managed_1360 active profile changes only its
    # startup dimensions. No observer site or renderer function is patched.
    require(stage_hash in (SHA256, '7e04d0389edcb0881183e20e2ee49c836d422617aecbef552d00a4e35b8d1451'),
            'transition_stage_hash')
    expected = bytearray(data)
    if stage_hash != SHA256:
        for offset, value in ((0x166ad, 0x80), (0x166ae, 7), (0x166b1, 0x38), (0x166b2, 4)):
            expected[offset] = value
    require(staged == expected, 'transition_stage_diff')
    text = log.read_text(encoding='utf-8', errors='replace')
    mappings = re.findall(r'place eboot\.bin base=(0x[\da-fA-F]+) mapped=(0x[\da-fA-F]+)', text)
    require(len(mappings) == 1, 'transition_ambiguous_mapping')
    base, extent = (int(v, 16) for v in mappings[0])
    require(base > 0 and base % 4096 == 0, 'transition_mapping_alignment')
    require(all(s[3] + s[6] <= extent for s in image.loads), 'transition_mapping_extent')
    loads = re.findall(r'^Loading: (.*[\\/]eboot\.bin)\s*$', text, re.M)
    require(bool(loads) and all(Path(p.strip()).resolve() == stage.resolve() for p in loads), 'transition_stage_path')
    audit(image)
    return {'shell': image}, {'shell': base}, {'shell': dict(sha256=SHA256, stage_sha256=stage_hash, base=hex(base), extent=extent,
            stage=str(stage.resolve()), original=str(original.resolve()))}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original', type=Path, required=True, help='Original NPXS40087 eboot.bin')
    p.add_argument('--timeline-log', type=Path, help='Optional live host log: bounded read-only byte anchors, no content output')
    p.add_argument('--stage', type=Path, required=True, help='Exact staged eboot.bin named by run log')
    p.add_argument('--run-log', type=Path, required=True, help='Immutable startup log prefix with placement and Loading path')
    p.add_argument('--mode', choices=('particle0', 'particle1', 'native', 'route', 'job'), required=True)
    p.add_argument('--attach', action='store_true')
    p.add_argument('--pid', type=int)
    p.add_argument('--created-filetime', type=int)
    p.add_argument('--exe', type=Path)
    p.add_argument('--exe-sha256')
    p.add_argument('--output', type=Path)
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--max-hits', type=int, default=2000)
    p.add_argument('--max-events', type=int, default=10000)
    p.add_argument('--max-bytes', type=int, default=8388608)
    args = p.parse_args()
    require(1 <= args.seconds <= 300 and 1 <= args.max_hits <= 20000 and 1 <= args.max_events <= 50000
            and 2097152 <= args.max_bytes <= 16777216, 'budget_arguments')
    images, bases, report = provenance(args.original, args.stage, args.run_log)
    if args.mode == 'route':
        from audit_home_native_registration_1360 import audit as audit_registration
        report['route_registration'] = audit_registration(args.original)
    if args.mode == 'job':
        from home_particle_job_trace_1360 import audit as audit_job
        audit_job(images['shell'])
    observer = Observer(args.mode)
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, process_access=False, mode=args.mode,
                             probes=observer.probes, images=report), indent=2))
        return
    if not all((args.pid, args.created_filetime, args.exe, args.exe_sha256, args.output)):
        p.error('--attach requires --pid --created-filetime --exe --exe-sha256 --output')
    require(os.name == 'nt' and ctypes.sizeof(ctypes.c_void_p) == 8, 'windows_amd64_required')
    from capture_home_gates_1360 import capture
    capture(args, images, bases, report, observer=observer)


if __name__ == '__main__':
    main()
