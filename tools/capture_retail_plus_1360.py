"""Offline by default; parent-only fresh-barrier native Plus capture, no opcode traps."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from home_gate_trace_1360 import require, digest
from retail_home_config_trace_1360 import EXE_HASH, HOME_HASH, PINS
from retail_plus_trace_1360 import PLUS, INITIAL, Observer, provenance
from capture_retail_home_config_1360 import check_barrier

CONTRACT = 'retail-plus-native-v1'


def sources():
    root = Path(__file__).resolve().parent
    return {name: digest((root/name).read_bytes()) for name in (
        'capture_retail_plus_1360.py', 'retail_plus_trace_1360.py',
        'retail_home_config_trace_1360.py', 'capture_retail_home_config_1360.py',
        'capture_home_gates_1360.py', 'home_gate_trace_1360.py',
        'capture_native_hang.py', 'capture_first_fault.py')}


def validate_profile(p):
    require(p.get('trace_mode') == 'retail_plus' and p.get('exe_sha256') == EXE_HASH and
            p.get('diagnostic_version') == 7 and p.get('lifecycle_version') == 2, 'plus_contract')
    images = p.get('images', {})
    require(all(images.get(k, {}).get('sha256') == sha for k, (_, sha) in {**PINS, 'plus': PLUS}.items()), 'plus_pins')
    require(images.get('home', {}).get('sha256') == HOME_HASH and
            images.get('observer', {}).get('contract') == CONTRACT, 'plus_profile')


def validate_release(data, pid, created):
    require(len(data) <= 8388608, 'plus_release_size')
    rows = [json.loads(l) for l in data.splitlines(keepends=True) if l.endswith(b'\n')]
    ps = [r for r in rows if r.get('event') == 'provenance']
    armed = [r for r in rows if r.get('event') == 'armed']
    require(len(ps) == len(armed) == 1, 'plus_release_armed')
    p = ps[0]
    validate_profile(p)
    require((p.get('pid'), p.get('created_filetime')) == (pid, created), 'plus_release_identity')
    require(p['images']['observer'].get('sources') == sources(), 'plus_release_sources')
    require(armed[0].get('phase') == 'retail_plus' and set(armed[0].get('probes', {})) == set(INITIAL), 'plus_release_initial')
    require(not any(r.get('event','').startswith('shutdown') or r.get('event') in
                    ('end','aborted','event_abort') for r in rows), 'plus_release_closed')


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
            cap.process, C.byref(present)) and present.value, 'plus_release_debugger')
        handle = bind(cap.k, 'OpenEventW', HANDLE, DWORD, BOOL, C.c_wchar_p)(
            2, False, f'Local\\KytyPreGuest-{args.pid}-{args.token}-release')
        process_identity(cap, args)
        require(handle and bind(cap.k, 'SetEvent', BOOL, HANDLE)(handle), 'plus_release_signal')
        print(json.dumps(dict(released=True, pid=args.pid, trace_mode='retail_plus')))
    finally:
        if handle:
            cap.close(handle)
        cap.finish()


def analyze(path):
    require(path.stat().st_size <= 16777216, 'plus_analysis_size')
    data = path.read_bytes()
    require(data.endswith(b'\n'), 'plus_partial_trace')
    rows = [json.loads(l) for l in data.splitlines()]
    ps = [r for r in rows if r.get('event') == 'provenance']
    ends = [r for r in rows if r.get('event') == 'end']
    require(len(ps) == len(ends) == 1, 'plus_analysis_contract')
    validate_profile(ps[0])
    end = ends[0]
    cleanup = bool(end.get('cleanup_verified') and end.get('cleanup', {}).get('detached') and
                   not end.get('cleanup_errors'))
    decode = not end.get('decode_unknowns') and not any(r.get('event') in
        ('aborted','event_abort','plus_thread_boundary') for r in rows)
    request = [r for r in rows if r.get('event') == 'plus_request']
    require(len(request) <= 1, 'plus_analysis_single_request')
    relevant = [r for r in rows if r.get('request') == 1]
    require(not request or all((r.get('generation'), r.get('user')) ==
        (request[0]['generation'], 0x10010029) for r in relevant), 'plus_analysis_generation')
    seen = {r.get('event') for r in relevant}
    ordered = ('plus_request','plus_settlement','plus_executor_callback','plus_executor_return')
    indices = {}
    for event in ordered:
        at = [i for i,r in enumerate(rows) if r.get('event') == event]
        require(len(at) <= 1, 'plus_analysis_duplicate_boundary')
        if at:
            indices[event] = at[0]
    for before,after in zip(ordered,ordered[1:]):
        if after in indices:
            require(before in indices and indices[before] < indices[after], 'plus_analysis_boundary_order')
    if 'plus_executor_callback' in indices:
        settle,call = rows[indices['plus_settlement']],rows[indices['plus_executor_callback']]
        require(settle['side'] == call['side'] and call['side'] in ('resolve','reject'), 'plus_analysis_callback_side')
        require(call['callback_id'] == request[0]['resolve_id' if call['side'] == 'resolve' else 'reject_id'],
                'plus_analysis_callback_id')
    exceptions = [r for r in rows if r.get('event') == 'forwarded_exception']
    # First-chance startup faults may be consumed by the target's own handlers.
    # They are not decoder errors or a failed detach. Never hide them: the
    # acquired interval remains conservative when exception scope is unknown.
    start = rows.index(request[0]) if request else len(rows)
    scoped_exceptions = [r for i,r in enumerate(rows) if i >= start and r.get('event') == 'forwarded_exception']
    uncertain_exception_scope = bool(scoped_exceptions or any(not r.get('first') for r in exceptions))
    budget = end.get('reason') in ('hit_budget','event_budget','output_budget','decode_budget')
    if not cleanup or not decode or budget or uncertain_exception_scope:
        boundary = 'incomplete_observation'
    elif not request:
        boundary = 'native_plus_entry_not_observed'
    elif 'plus_executor_return' in seen:
        boundary = 'native_executor_callback_returned_JS_consumer_unknown'
    elif 'plus_executor_callback' in seen:
        boundary = 'native_executor_entered_return_not_observed'
    elif 'plus_settlement' in seen:
        boundary = 'native_settlement_without_observed_executor_delivery'
    elif any(r.get('cancelled') for r in relevant if r.get('event') == 'plus_service_return'):
        boundary = 'cancellation_skip_branch_observed_no_settlement'
    elif 'plus_service_return' in seen:
        boundary = 'service_return_without_observed_settlement'
    elif 'plus_native_service_return' in seen:
        boundary = 'service_returned_cancellation_check_not_observed'
    elif 'plus_service_call' in seen:
        boundary = 'native_service_return_not_observed'
    elif 'plus_worker' in seen:
        boundary = 'worker_entered_service_or_settlement_not_observed'
    else:
        boundary = 'native_entry_without_observed_worker_or_settlement'
    return dict(trace_sha256=digest(data), cleanup_verified=cleanup, decode_valid=decode,
                budget_exhausted=budget, boundary_result=boundary,
                forwarded_exceptions=len(exceptions), forwarded_exceptions_after_acquisition=len(scoped_exceptions),
                exception_scope_uncertain=uncertain_exception_scope,
                focused_boundary_complete=bool(cleanup and decode and not budget and not uncertain_exception_scope and 'plus_executor_return' in seen),
                native_settlement=[r['side'] for r in relevant if r.get('event') == 'plus_settlement'],
                outstanding=end.get('outstanding', []),
                exact_HBC_caller='not decoded; first native Home Plus request, not assumed #3266',
                native_promise_state='absence is not proof of pending',
                config_publication='not covered by this native-only profile', usable_home='not established')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('original-common','stage','bundle','run-log','timeline-log','exe','output','armed-trace'):
        p.add_argument('--'+name, type=Path)
    p.add_argument('--exe-sha256', default=EXE_HASH)
    p.add_argument('--pid', type=int)
    p.add_argument('--created-filetime', type=int)
    p.add_argument('--token')
    p.add_argument('--seconds', type=int, default=180)
    p.add_argument('--max-hits', type=int, default=20000)
    p.add_argument('--max-events', type=int, default=40000)
    p.add_argument('--max-bytes', type=int, default=8388608)
    modes = p.add_mutually_exclusive_group()
    modes.add_argument('--attach', action='store_true')
    modes.add_argument('--release', action='store_true')
    modes.add_argument('--analyze', type=Path)
    args = p.parse_args()
    if args.analyze:
        print(json.dumps(analyze(args.analyze), indent=2));return
    require(args.exe_sha256 == EXE_HASH, 'plus_candidate_pin')
    if args.attach or args.release:
        require(os.name == 'nt' and C.sizeof(C.c_void_p) == 8, 'plus_windows_amd64')
        require(all((args.pid,args.created_filetime,args.exe,args.token)), 'plus_runtime_args')
    if args.release:
        require(args.armed_trace is not None, 'plus_release_trace_required')
        release(args);return
    require(all((args.original_common,args.stage,args.bundle,args.run_log,args.exe)), 'plus_offline_args')
    require(1 <= args.seconds <= 300 and 1 <= args.max_hits <= 20000 and
            1 <= args.max_events <= 50000 and 2097152 <= args.max_bytes <= 16777216, 'plus_budgets')
    require(digest(args.bundle.read_bytes()) == HOME_HASH, 'plus_home_bundle_pin')
    images,bases,report = provenance(args.original_common,args.stage,args.exe,args.run_log)
    report['host'] = dict(sha256=EXE_HASH)
    report['home'] = dict(sha256=HOME_HASH, source='authorized mixed devkit counterpart; not retail decryption')
    report['observer'] = dict(contract=CONTRACT, sources=sources())
    if not args.attach:
        print(json.dumps(dict(offline_verified=True,process_access=False,images=report,
                             active_Hermes_opcode_traps=0,hit_budget=args.max_hits),indent=2));return
    require(args.output is not None and not args.output.exists() and 'pui' not in bases, 'plus_new_fresh_capture')
    from capture_native_hang import Capture
    from capture_home_gates_1360 import capture,process_identity
    cap = Capture(args.pid,'',False)
    try:
        cap.check_architecture();process_identity(cap,args);check_barrier(cap,args.token)
        hosts=[b for b,_,n in cap.modules() if Path(n).resolve() == args.exe.resolve()]
        require(len(hosts)==1,'plus_host_mapping');bases['host']=hosts[0]
    finally:
        cap.finish()
    observer=Observer({'pui': args.stage/PINS['pui'][0], 'plus': args.stage/PLUS[0]},args.token)
    observer.exe=args.exe
    capture(args,images,bases,report,observer=observer)


if __name__ == '__main__':
    main()
