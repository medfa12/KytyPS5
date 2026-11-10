"""Parent-operated retail query capture; default is offline ABI audit only.

Uses the existing hardware-breakpoint lifecycle, identity checks and verified
detach. Attach before onboarding queries; absence after late attach is unknown.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re

from home_gate_trace_1360 import Image, digest, require
from retail_peripheral_trace_1360 import NAME, SHA256, CORLIB_SHA256, Observer, audit


def provenance(original, corlib, stage=None, log=None):
    data = original.read_bytes()
    image = Image(data, SHA256)
    audit(image, Image(corlib.read_bytes(), CORLIB_SHA256))
    report = {'peripheral': dict(sha256=SHA256, original=str(original.resolve())),
              'corlib_layout': dict(sha256=CORLIB_SHA256, original=str(corlib.resolve()))}
    if stage is None and log is None:
        return {'peripheral': image}, {}, report
    require(stage is not None and log is not None, 'stage_and_log_required')
    require(stage.read_bytes() == data, 'peripheral_stage_diff')
    text = log.read_text(encoding='utf-8', errors='replace')
    mappings = re.findall(r'place ' + re.escape(NAME) + r' base=(0x[\da-fA-F]+) mapped=(0x[\da-fA-F]+)', text)
    require(len(mappings) == 1, 'peripheral_ambiguous_mapping')
    base, extent = (int(v, 16) for v in mappings[0])
    require(base > 0 and base % 4096 == 0, 'peripheral_mapping_alignment')
    require(all(s[3] + s[6] <= extent for s in image.loads), 'peripheral_mapping_extent')
    loads = re.findall(r'^Loading: (.*[\\/]' + re.escape(NAME) + r')\s*$', text, re.M)
    require(bool(loads) and all(Path(p.strip()).resolve() == stage.resolve() for p in loads), 'peripheral_stage_path')
    report['peripheral'].update(base=hex(base), extent=extent, stage=str(stage.resolve()))
    return {'peripheral': image}, {'peripheral': base}, report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original', type=Path, required=True)
    p.add_argument('--corlib', type=Path, required=True, help='Original retail mscorlib layout reference')
    p.add_argument('--stage', type=Path)
    p.add_argument('--run-log', type=Path, help='Startup prefix including the AOT placement and Loading path')
    p.add_argument('--mode', choices=('query', 'filters'), default='query')
    p.add_argument('--attach', action='store_true')
    p.add_argument('--pid', type=int)
    p.add_argument('--created-filetime', type=int)
    p.add_argument('--exe', type=Path)
    p.add_argument('--exe-sha256')
    p.add_argument('--output', type=Path)
    p.add_argument('--seconds', type=int, default=120)
    p.add_argument('--max-hits', type=int, default=2000)
    p.add_argument('--max-events', type=int, default=10000)
    p.add_argument('--max-bytes', type=int, default=8388608)
    args = p.parse_args()
    require(1 <= args.seconds <= 300 and 1 <= args.max_hits <= 20000 and 1 <= args.max_events <= 50000
            and 2097152 <= args.max_bytes <= 16777216, 'budget_arguments')
    images, bases, report = provenance(args.original, args.corlib, args.stage, args.run_log)
    observer = Observer(args.mode)
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, process_access=False, probes=observer.probes, images=report), indent=2))
        return
    if not all((args.pid, args.created_filetime, args.exe, args.exe_sha256, args.output, args.stage, args.run_log)):
        p.error('--attach requires process identity, output, stage and run-log arguments')
    require(os.name == 'nt' and ctypes.sizeof(ctypes.c_void_p) == 8, 'windows_amd64_required')
    from capture_home_gates_1360 import capture
    capture(args, images, bases, report, observer=observer)


if __name__ == '__main__':
    main()
