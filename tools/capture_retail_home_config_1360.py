"""Offline by default. Parent-only fresh-start retail Home configuration capture.

Four hardware CODE traps, no memory/code/guest-result writes. Start at the
existing pre-guest barrier; a pinned host loader return discovers lazy PUI and
hands DR0 to its managed owner boundary before the loader returns to its caller.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True

from home_gate_trace_1360 import require, digest
from retail_home_config_trace_1360 import (EXE_HASH, HOME_HASH, PINS, Observer, catalog, provenance)


def observer_sources():
    root = Path(__file__).resolve().parent
    return {name: digest((root / name).read_bytes()) for name in (
        'capture_retail_home_config_1360.py', 'retail_home_config_trace_1360.py',
        'capture_home_gates_1360.py', 'home_gate_trace_1360.py',
        'capture_native_hang.py', 'capture_first_fault.py')}


def check_barrier(cap, token):
    from capture_native_hang import bind, HANDLE, DWORD, BOOL
    require(re.fullmatch('[0-9a-f]{32}', token) is not None, 'barrier_token')
    open_event = bind(cap.k, 'OpenEventW', HANDLE, DWORD, BOOL, C.c_wchar_p)
    wait = bind(cap.k, 'WaitForSingleObject', DWORD, HANDLE, DWORD)
    prefix = f'Local\\KytyPreGuest-{cap.pid}-{token}'
    handles = []
    try:
        for suffix, state in (('-ready', 0), ('-release', 258)):
            h = open_event(0x100000, False, prefix + suffix)
            if h:
                handles.append(h)
            require(h and wait(h, 0) == state, 'fresh_unreleased_barrier_required')
    finally:
        for h in handles:
            cap.close(h)


def validate_release(data, pid, created):
    require(len(data) <= 8388608, 'trace_size')
    rows = [json.loads(l) for l in data.splitlines(keepends=True) if l.endswith(b'\n')]
    p = [r for r in rows if r.get('event') == 'provenance']
    a = [r for r in rows if r.get('event') == 'armed']
    require(len(p) == len(a) == 1, 'release_requires_one_armed_trace')
    p, a = p[0], a[0]
    require(p.get('trace_mode') == 'retail_config' and p.get('diagnostic_version') == 7 and
            p.get('lifecycle_version') == 2 and a.get('phase') == 'retail_config', 'release_contract')
    require((p.get('pid'), p.get('created_filetime'), p.get('exe_sha256')) ==
            (pid, created, EXE_HASH), 'release_identity')
    require(all(p.get('images', {}).get(k, {}).get('sha256') == sha for k, (_, sha) in PINS.items()),
            'release_image_pins')
    require(p.get('images', {}).get('home', {}).get('sha256') == HOME_HASH, 'release_home_pin')
    profile = p.get('images', {}).get('observer', {})
    require(profile.get('contract') == 'retail-home-config-v1' and
            profile.get('sources') == observer_sources(), 'release_observer_sources')
    require(not any(r.get('event', '').startswith('shutdown') or r.get('event') in ('end', 'aborted', 'event_abort')
                    for r in rows), 'release_after_shutdown')
    require(set(a.get('probes', {})) == {'loader', 'call2', 'ret', 'false'}, 'release_initial_probes')


def release(args):
    from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE
    from capture_home_gates_1360 import process_identity
    validate_release(args.armed_trace.read_bytes(), args.pid, args.created_filetime)
    cap = Capture(args.pid, '', False)
    handle = None
    try:
        cap.check_architecture()
        process_identity(cap, args)
        check_barrier(cap, args.token)
        present = BOOL()
        require(bind(cap.k, 'CheckRemoteDebuggerPresent', BOOL, HANDLE, C.POINTER(BOOL))(
            cap.process, C.byref(present)) and present.value, 'release_debugger_absent')
        handle = bind(cap.k, 'OpenEventW', HANDLE, DWORD, BOOL, C.c_wchar_p)(
            2, False, f'Local\\KytyPreGuest-{args.pid}-{args.token}-release')
        process_identity(cap, args)
        require(handle and bind(cap.k, 'SetEvent', BOOL, HANDLE)(handle), 'barrier_release_failed')
        print(json.dumps(dict(released=True, pid=args.pid, trace_mode='retail_config')))
    finally:
        if handle:
            cap.close(handle)
        cap.finish()


def analyze(path):
    require(path.stat().st_size <= 16777216, 'analysis_size')
    raw = path.read_bytes()
    require(raw.endswith(b'\n'), 'incomplete_trace')
    rows = [json.loads(l) for l in raw.splitlines()]
    p = [r for r in rows if r.get('event') == 'provenance']
    ends = [r for r in rows if r.get('event') == 'end']
    require(len(p) == len(ends) == 1 and p[0].get('trace_mode') == 'retail_config', 'trace_contract')
    require(p[0].get('exe_sha256') == EXE_HASH and all(
        p[0].get('images', {}).get(k, {}).get('sha256') == sha for k, (_, sha) in PINS.items()), 'analysis_pins')
    require(p[0].get('images', {}).get('observer', {}).get('contract') == 'retail-home-config-v1', 'analysis_profile')
    end = ends[0]
    clean = bool(end.get('cleanup_verified') and not end.get('decode_unknowns') and
                 not any(r.get('event') in ('event_abort', 'aborted', 'forwarded_exception') for r in rows))
    hits = [r for r in rows if r.get('event') == 'home_hbc']
    owners = {(r['generation'], r['user']) for r in rows if r.get('event') == 'home_owner'}
    require(len(owners) <= 1, 'analysis_mixed_generations')
    sites = {(r['fid'], r['offset']) for r in hits}
    root = [r['config_kind'] for r in hits if 'config_kind' in r]
    published = 'object' in root
    callbacks = [pc for fid, pc in sites if fid == 3277 and pc in (0xee, 0x10e, 0x147, 0x165)]
    initial_empty = any(k in ('null', 'undefined') for k in root)
    await_started = (3277, 0x63) in sites
    valid = clean and bool(owners) and any(r.get('event') == 'home_buffer_verified' and
                                         r.get('sha256') == HOME_HASH for r in rows)
    require(all((r['generation'], r['user']) in owners for r in hits), 'analysis_foreign_hit')
    empty_indices = [i for i, r in enumerate(hits) if r.get('config_kind') in ('undefined', 'null')]
    call_indices = [i for i, r in enumerate(hits) if r['fid'] == 3277 and r['offset'] in callbacks]
    published_indices = [i for i, r in enumerate(hits) if r.get('config_kind') == 'object']
    chain = any(a < b < c for a in empty_indices for b in call_indices for c in published_indices)
    postcall = any(a < b for a in call_indices for b, r in enumerate(hits) if
                   r['fid'] == 3277 and r['opcode'] == 'Ret' and r['offset'] in (0xf4, 0x114, 0x14f))
    repeated_config = sum(r['fid'] == 3277 and r['offset'] == 0x63 for r in hits) > 1
    fulfillment = {'line': 3256, 'minimumVshVersion': 3258, 'devkitType': 3259,
                   'countryCode': 3262, 'minimumAge': 3265, 'isPlusMember': 3268}
    verifiers = {name: ('consumer_callback_return_observed' if any(
        r['fid'] == fid and r['opcode'] == 'Ret' for r in hits) else 'completion_not_observed')
        for name, fid in fulfillment.items()}
    if not valid or repeated_config:
        boundary = 'incomplete_or_ambiguous_coverage'
    elif not initial_empty:
        boundary = 'initial_empty_root_not_observed'
    elif chain and postcall:
        boundary = 'config_publication_chain_observed'
    elif callbacks and postcall:
        boundary = 'callback_return_without_observed_root_publication'
    elif await_started:
        boundary = 'config_await_continuation_or_delivery_gap'
    else:
        boundary = 'config_invocation_or_await_not_observed'
    return dict(trace_sha256=digest(raw), cleanup_and_decode_valid=valid,
                boundary_result=boundary,
                initial_empty_config_observed=initial_empty, config_await_started=await_started,
                onConfigured_call_attempt_sites=sorted(callbacks), root_config_object_observed=published,
                config_callback_return_observed=postcall,
                publication_chain_observed=bool(valid and chain and postcall and not repeated_config),
                repeated_config_attempts=repeated_config, verifier_consumers=verifiers,
                account_rejection_handler_observed=any(r['fid'] == 3261 for r in hits),
                plus_rejection_handler_observed=any(r['fid'] == 3267 for r in hits),
                selected_counts=[r['selected_count'] for r in hits if 'selected_count' in r],
                outstanding=end.get('outstanding', []),
                native_promise_state='not decoded; continuation absence is not proof of pending',
                react_commit_and_native_interactive='not covered',
                usable_home='not established')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original-common', type=Path)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--decoder', type=Path)
    parser.add_argument('--run-log', type=Path)
    parser.add_argument('--timeline-log', type=Path)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--exe-sha256', default=EXE_HASH)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--created-filetime', type=int)
    parser.add_argument('--token')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--max-hits', type=int, default=500000)
    parser.add_argument('--max-events', type=int, default=600000)
    parser.add_argument('--max-bytes', type=int, default=8388608)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--attach', action='store_true')
    modes.add_argument('--release', action='store_true')
    modes.add_argument('--analyze', type=Path)
    parser.add_argument('--armed-trace', type=Path)
    args = parser.parse_args()
    if args.analyze:
        print(json.dumps(analyze(args.analyze), indent=2))
        return
    require(args.exe_sha256 == EXE_HASH, 'only_audited_candidate')
    if args.attach or args.release:
        require(all((args.pid, args.created_filetime, args.exe, args.token)), 'runtime_arguments')
        require(os.name == 'nt' and C.sizeof(C.c_void_p) == 8, 'windows_amd64')
    if args.release:
        require(args.armed_trace is not None, 'armed_trace_required')
        release(args)
        return
    require(all((args.original_common, args.stage, args.bundle, args.decoder, args.run_log, args.exe)),
            'offline_provenance_arguments')
    require(1 <= args.seconds <= 300 and 1 <= args.max_hits <= 1000000 and
            1 <= args.max_events <= 1100000 and 2097152 <= args.max_bytes <= 16777216, 'capture_budgets')
    images, bases, report = provenance(args.original_common, args.stage, args.exe, args.run_log)
    data, functions = catalog(args.bundle, args.decoder)
    report['home'] = dict(sha256=HOME_HASH, bytes=len(data), source='authorized devkit counterpart; NOT retail decryption')
    report['host'] = dict(sha256=EXE_HASH, loader_return_rva='0x9f447e')
    report['observer'] = dict(contract='retail-home-config-v1', sources=observer_sources())
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, process_access=False, images=report,
                             functions=len(functions), lazy_pui='pui' not in bases), indent=2))
        return
    require(args.output is not None and not args.output.exists(), 'new_output_required')
    require('pui' not in bases, 'attach_requires_pre_guest_prefix_not_old_runtime_log')
    # Query-only precheck. Core repeats identity and barrier checks under the
    # initial debugger stop; this does not signal/release the barrier.
    from capture_native_hang import Capture
    from capture_home_gates_1360 import capture, process_identity
    cap = Capture(args.pid, '', False)
    try:
        cap.check_architecture()
        process_identity(cap, args)
        check_barrier(cap, args.token)
        hosts = [base for base, _, name in cap.modules() if Path(name).resolve() == args.exe.resolve()]
        require(len(hosts) == 1, 'host_mapping')
        bases['host'] = hosts[0]
    finally:
        cap.finish()
    observer = Observer(data, functions, args.stage / PINS['pui'][0], args.token)
    observer.exe = args.exe
    capture(args, images, bases, report, observer=observer)


if __name__ == '__main__':
    main()
