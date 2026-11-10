"""Bounded read-only log anchors and content-free closed-capture alignment.

Anchors locate persisted log bytes, not the time guest code executed: buffering
can delay a write. No raw log text, application arguments or credentials emitted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

WINDOW = 4096


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sample(path):
    started = time.time_ns()
    try:
        with Path(path).open('rb') as f:
            stat = os.fstat(f.fileno())
            end = stat.st_size
            head = f.read(min(end, WINDOW))
            begin = max(0, end - WINDOW)
            f.seek(begin)
            tail = f.read(end - begin)
            if len(head) != min(end, WINDOW) or len(tail) != end - begin:
                raise ValueError('short_sample')
        return dict(status='sampled', file_identity=[stat.st_dev, stat.st_ino],
                    offset=end, tail_offset=begin, head_bytes=len(head),
                    head_sha256=sha(head), tail_sha256=sha(tail),
                    utc_before_ns=started, utc_after_ns=time.time_ns())
    except (OSError, ValueError):
        # Diagnostics must never obstruct verified debugger teardown.
        return dict(status='unavailable', utc_before_ns=started, utc_after_ns=time.time_ns())


def verify_sample(f, anchor):
    if anchor.get('status') != 'sampled':
        raise ValueError('timeline_sample_unavailable')
    end, begin, head_bytes = anchor['offset'], anchor['tail_offset'], anchor['head_bytes']
    if not isinstance(end, int) or end < 0 or begin != max(0, end - WINDOW) or head_bytes != min(end, WINDOW):
        raise ValueError('timeline_sample_bounds')
    f.seek(0)
    if sha(f.read(head_bytes)) != anchor['head_sha256']:
        raise ValueError('timeline_head_mismatch')
    f.seek(begin)
    tail = f.read(end - begin)
    if len(tail) != end - begin or sha(tail) != anchor['tail_sha256']:
        raise ValueError('timeline_tail_mismatch')


def classify_line(start, end, armed, stopped):
    if armed is None:
        return 'alignment_unknown'
    if end <= armed:
        return 'persisted_before_armed'
    if start >= stopped:
        return 'persisted_after_capture'
    if start >= armed and end <= stopped:
        return 'persisted_during_capture'
    return 'straddles_sample_boundary'


def analyze(trace, log):
    raw = Path(trace).read_bytes()
    rows = [json.loads(line) for line in raw.splitlines()]
    events = ('provenance', 'armed', 'shutdown_begin', 'end')
    selected = {name: [r for r in rows if r.get('event') == name] for name in events}
    if any(len(value) != 1 for value in selected.values()):
        raise ValueError('need_one_closed_capture')
    last = selected['end'][0]
    result = dict(trace_sha256=sha(raw), cleanup_verified=last.get('cleanup_verified') is True,
                  probe_hits=last.get('probe_hits', {}), excluded_codes=last.get('excluded_codes', {}),
                  alignment='unknown_no_log_anchors', milestones=[])
    armed = stopped = None
    anchors = [selected[name][0].get('timeline') for name in events]
    with Path(log).open('rb') as f:
        initial = os.fstat(f.fileno())
        if any(a is not None for a in anchors):
            if not all(a is not None for a in anchors):
                raise ValueError('incomplete_timeline')
            for a in anchors:
                verify_sample(f, a)
            if any(a['file_identity'] != anchors[0]['file_identity'] for a in anchors):
                raise ValueError('timeline_file_replaced')
            if not all(a['offset'] <= b['offset'] for a, b in zip(anchors, anchors[1:])):
                raise ValueError('timeline_log_truncated')
            armed, stopped = anchors[1]['offset'], anchors[2]['offset']
            result['alignment'] = 'verified_sampled_byte_boundaries_not_execution_time'
        f.seek(0)
        digest = hashlib.sha256()
        offset = 0
        for number, line in enumerate(f, 1):
            digest.update(line)
            end = offset + len(line)
            label = None
            if b'RNPS.rnps-home.NPXS40002' in line:
                if b'instance promoted from' in line:
                    label = 'home_instance_promoted'
                elif b'MountApp runApplication' in line:
                    label = 'home_mount_run_application'
            if b'[LoginMgr] [notifyInstanceClosed]' in line and b'"result":"success"' in line and b'"retCode":0' in line:
                label = 'local_login_success_log'
            if label:
                result['milestones'].append(dict(kind=label, line=number, byte_start=offset, byte_end=end,
                                                 placement=classify_line(offset, end, armed, stopped)))
            offset = end
        result['log_sha256'] = digest.hexdigest()
        result['log_bytes'] = offset
        final = os.fstat(f.fileno())
        if (initial.st_size, initial.st_mtime_ns) != (final.st_size, final.st_mtime_ns):
            raise ValueError('analysis_requires_closed_log')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--trace', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True, help='Closed source log or its retained byte-identical copy')
    args = p.parse_args()
    print(json.dumps(analyze(args.trace, args.log), indent=2))
