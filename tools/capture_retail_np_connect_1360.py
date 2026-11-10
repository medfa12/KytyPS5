"""Offline by default. Fresh-barrier, bounded native NP-connect observation.

Explicit --attach/--release are parent-only process access. No launcher, guest
calls, code/data writes, fabricated acceptance, auth stores or request payloads.
Four hardware execution breakpoints use the existing verified detach protocol.
Debug pauses affect timeouts; no result establishes PSN login/client readiness.
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
from retail_np_connect_trace_1360 import (
    EXE_HASH, CORE_HASH, CONTRACT, INITIAL, SITES, Observer, images_from_paths, prefix_provenance)


def sources():
    root = Path(__file__).resolve().parent
    names = ('capture_retail_np_connect_1360.py', 'retail_np_connect_trace_1360.py',
             'capture_home_gates_1360.py', 'capture_native_hang.py', 'capture_first_fault.py',
             'home_gate_trace_1360.py', 'retail_plus_trace_1360.py',
             'retail_home_config_trace_1360.py', 'capture_retail_home_config_1360.py')
    return {name: digest((root / name).read_bytes()) for name in names}


def bounded_file(path, size, code):
    with path.open('rb') as source:
        data = source.read(size + 1)
    require(len(data) <= size, code)
    return data


def validate_profile(profile, expected_sources=None):
    require(profile.get('trace_mode') == 'retail_np_connect' and
            profile.get('exe_sha256') == EXE_HASH and
            profile.get('diagnostic_version') == 7 and profile.get('lifecycle_version') == 2,
            'np_connect_profile')
    images = profile.get('images', {})
    require(images.get('host', {}).get('sha256') == EXE_HASH and
            images.get('core', {}).get('sha256') == CORE_HASH and
            images.get('observer', {}).get('contract') == CONTRACT, 'np_connect_profile_pins')
    observer = images['observer']
    require(observer.get('sources') == (expected_sources if expected_sources is not None else sources()),
            'np_connect_profile_sources')
    require(observer.get('fresh_barrier') is True and type(observer.get('core_pid')) is int and
            0 < observer['core_pid'] <= 0x7fffffff and
            all(isinstance(observer.get(key), str) and re.fullmatch('[0-9a-f]{64}', observer[key])
                for key in ('barrier_token_sha256', 'prefix_sha256')) and
            profile.get('run_log_sha256') == observer['prefix_sha256'], 'np_connect_profile_scope')
    for key in ('host', 'core'):
        value = images[key].get('base')
        require(isinstance(value, str) and re.fullmatch('0x[0-9a-f]{1,12}', value) is not None and
                0x10000 <= int(value, 16) < 0x800000000000 and int(value, 16) % 4096 == 0,
                'np_connect_profile_mapping')


def validate_release(data, pid, created, token, expected_sources=None):
    require(len(data) <= 8 * 1024 * 1024, 'np_connect_release_budget')
    rows = [json.loads(line) for line in data.splitlines(keepends=True) if line.endswith(b'\n')]
    provenance = [row for row in rows if row.get('event') == 'provenance']
    armed = [row for row in rows if row.get('event') == 'armed']
    require(len(provenance) == len(armed) == 1, 'np_connect_release_armed')
    p = provenance[0]
    validate_profile(p, expected_sources)
    require((p.get('pid'), p.get('created_filetime')) == (pid, created) and
            p['images']['observer']['barrier_token_sha256'] == digest(token.encode()), 'np_connect_release_identity')
    expected = {name: hex(int(p['images'][key]['base'], 16) + rva) for name, (key, rva) in INITIAL.items()}
    require(armed[0].get('phase') == 'retail_np_connect' and
            armed[0].get('probes') == expected, 'np_connect_release_initial')
    require(not any(row.get('event', '').startswith('shutdown') or row.get('event') in
                    ('end', 'aborted', 'event_abort', 'probe_handoff') for row in rows),
            'np_connect_release_closed_or_started')


NATIVE_FIELDS = {
    'np_server_entry': (), 'np_retained_submit_before_enqueue': (),
    'np_retained_enqueue_return': ('syscall_errno', 'ticket_present'),
    'np_connect_dispatched_before_native': ('server_entry_observed',),
    'np_new_client_enter': (), 'np_session_register_enter': (),
    'np_session_register_return': ('service_result',), 'np_new_client_result': ('service_result',),
    'np_core_accept_call': ('service_result',),
    'np_core_accept_return': ('syscall_errno', 'native_acceptance', 'completion_published', 'consumer_completion_observed'),
    'np_connect_owned_thread_lifecycle_gap': (),
}


def analyze(data, expected_sources=None):
    require(len(data) <= 8 * 1024 * 1024 and data.endswith(b'\n'), 'np_connect_analysis_complete_file')
    rows = [json.loads(line) for line in data.splitlines()]
    ps = [row for row in rows if row.get('event') == 'provenance']
    ends = [row for row in rows if row.get('event') == 'end']
    require(len(ps) == len(ends) == 1 and rows[0] is ps[0] and rows[-1] is ends[0], 'np_connect_analysis_envelope')
    validate_profile(ps[0], expected_sources)
    relevant = [(i, row) for i, row in enumerate(rows) if row.get('event', '').startswith('np_')]
    indices = {}
    booleans = {'ticket_present', 'server_entry_observed', 'completion_published', 'consumer_completion_observed'}
    for i, row in relevant:
        event = row['event']
        require(event in NATIVE_FIELDS and set(row) <= {'event', 'connection', 't', 'tid', *NATIVE_FIELDS[event]},
                'np_connect_analysis_private_fields')
        require(row.get('connection') == 1 and type(row.get('tid')) is int and row['tid'] > 0,
                'np_connect_analysis_native_identity')
        for field in NATIVE_FIELDS[event]:
            require(field in row and (type(row[field]) is bool if field in booleans else
                                     type(row[field]) is int and -0x80000000 <= row[field] <= 0x7fffffff),
                    'np_connect_analysis_native_field')
        require(event not in indices, 'np_connect_analysis_duplicate_stage')
        indices[event] = i
    ordered = ('np_retained_submit_before_enqueue', 'np_connect_dispatched_before_native', 'np_new_client_enter')
    ordered += ('np_new_client_result', 'np_core_accept_call', 'np_core_accept_return')
    for before, after in zip(ordered, ordered[1:]):
        if after in indices:
            require(before in indices and indices[before] < indices[after], 'np_connect_analysis_order')
    for before, after in (('np_new_client_enter', 'np_session_register_enter'),
                          ('np_session_register_enter', 'np_session_register_return'),
                          ('np_session_register_return', 'np_new_client_result')):
        if after in indices and (after != 'np_new_client_result' or 'np_session_register_return' in indices):
            require(before in indices and indices[before] < indices[after], 'np_connect_analysis_session_order')
    if 'np_retained_enqueue_return' in indices:
        require('np_retained_submit_before_enqueue' in indices and
                indices['np_retained_submit_before_enqueue'] < indices['np_retained_enqueue_return'],
                'np_connect_analysis_enqueue_order')
    if 'np_new_client_result' in indices and rows[indices['np_new_client_result']]['service_result'] == 0:
        require('np_session_register_return' in indices and
                rows[indices['np_session_register_return']]['service_result'] == 0,
                'np_connect_analysis_real_session')
    bound_events = ('np_connect_dispatched_before_native', 'np_new_client_enter',
                    'np_session_register_enter', 'np_session_register_return',
                    'np_new_client_result', 'np_core_accept_call', 'np_core_accept_return')
    bound_tids = {rows[indices[event]]['tid'] for event in bound_events if event in indices}
    require(len(bound_tids) <= 1, 'np_connect_analysis_dispatch_thread')
    end = ends[0]
    cleanup = bool(end.get('cleanup_verified') and end.get('cleanup', {}).get('detached') and not end.get('cleanup_errors'))
    decode = not end.get('decode_unknowns') and not any(row.get('event') in
        ('aborted', 'event_abort', 'np_connect_owned_thread_lifecycle_gap') for row in rows)
    budget = end.get('reason') in ('hit_budget', 'event_budget', 'output_budget', 'decode_budget')
    exceptions = [(i, row) for i, row in enumerate(rows) if row.get('event') == 'forwarded_exception']
    acquired = indices.get('np_retained_submit_before_enqueue', len(rows))
    scoped_exceptions = [row for i, row in exceptions if i >= acquired or not row.get('first')]
    # Do not turn a truncated or exception-interrupted interval into a deadlock
    # or lost completion claim. Native events can still be reported as observed.
    reliable = cleanup and decode and not budget and not scoped_exceptions
    final = next((row for _, row in relevant if row['event'] == 'np_core_accept_return'), None)
    if final:
        require(final['completion_published'] == (final['syscall_errno'] == 0) and
                final['consumer_completion_observed'] is False, 'np_connect_analysis_accept_semantics')
        native = rows[indices['np_new_client_result']]['service_result']
        require(final['native_acceptance'] == native == rows[indices['np_core_accept_call']]['service_result'],
                'np_connect_analysis_accept_result')
    return dict(trace_sha256=digest(data), cleanup_verified=cleanup, decode_valid=decode,
                budget_exhausted=budget, forwarded_exceptions=len(exceptions),
                forwarded_exceptions_after_acquisition=len(scoped_exceptions), reliable_interval=reliable,
                last_observed=relevant[-1][1]['event'] if relevant else 'no_local_connect_observed',
                core_accept_return_observed=bool(final),
                completion_publication_observed=bool(reliable and final and final['completion_published']),
                native_connection_accepted=bool(reliable and final and final['completion_published'] and final['native_acceptance'] == 0),
                consumer_completion_observed=False, native_client_ready='not established', psn_authenticated=False,
                limitation='Debug pauses affect deadlines; absence is not proof of deadlock, nonexecution or Sony rejection.')


def live_identity(cap, args, attached):
    from capture_native_hang import bind, BOOL, HANDLE
    from capture_home_gates_1360 import process_identity
    from capture_retail_home_config_1360 import check_barrier
    process_identity(cap, args)
    check_barrier(cap, args.token)
    present = BOOL()
    require(bind(cap.k, 'CheckRemoteDebuggerPresent', BOOL, HANDLE, C.POINTER(BOOL))(
        cap.process, C.byref(present)) and bool(present.value) == attached, 'np_connect_debugger_identity')


def release(args):
    from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE
    validate_release(bounded_file(args.armed_trace, 8 * 1024 * 1024, 'np_connect_release_budget'),
                     args.pid, args.created_filetime, args.token)
    cap, handle = Capture(args.pid, '', False), None
    try:
        cap.check_architecture()
        live_identity(cap, args, True)
        handle = bind(cap.k, 'OpenEventW', HANDLE, DWORD, BOOL, C.c_wchar_p)(
            2, False, f'Local\\KytyPreGuest-{args.pid}-{args.token}-release')
        live_identity(cap, args, True)
        require(handle and bind(cap.k, 'SetEvent', BOOL, HANDLE)(handle), 'np_connect_release_signal')
        print(json.dumps(dict(released=True, trace_mode='retail_np_connect', pid=args.pid)))
    finally:
        if handle:
            cap.close(handle)
        cap.finish()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'core', 'run-log', 'output', 'armed-trace'):
        p.add_argument('--' + name, type=Path)
    p.add_argument('--exe-sha256', default=EXE_HASH)
    p.add_argument('--pid', type=int)
    p.add_argument('--created-filetime', type=int)
    p.add_argument('--token')
    p.add_argument('--seconds', type=int, default=120)
    p.add_argument('--max-hits', type=int, default=256)
    p.add_argument('--max-events', type=int, default=10000)
    p.add_argument('--max-bytes', type=int, default=4 * 1024 * 1024)
    modes = p.add_mutually_exclusive_group()
    modes.add_argument('--attach', action='store_true')
    modes.add_argument('--release', action='store_true')
    modes.add_argument('--analyze', type=Path)
    args = p.parse_args()
    if args.analyze:
        print(json.dumps(analyze(bounded_file(args.analyze, 8 * 1024 * 1024, 'np_connect_analysis_budget')), indent=2))
        return
    require(args.exe_sha256 == EXE_HASH, 'np_connect_candidate_pin')
    require(1 <= args.seconds <= 180 and 1 <= args.max_hits <= 512 and
            1 <= args.max_events <= 20000 and 2 * 1024 * 1024 <= args.max_bytes <= 8 * 1024 * 1024,
            'np_connect_budgets')
    require(args.exe is not None, 'np_connect_executable_required')
    if args.attach or args.release:
        require(os.name == 'nt' and C.sizeof(C.c_void_p) == 8, 'np_connect_windows_amd64')
        require(args.pid is not None and args.pid > 0 and args.created_filetime is not None and
                args.created_filetime > 0 and args.token is not None, 'np_connect_explicit_target_required')
    if args.release:
        require(args.armed_trace is not None, 'np_connect_armed_trace_required')
        release(args)
        return
    require(args.core is not None, 'np_connect_core_required')
    images = images_from_paths(args.exe, args.core)
    bases = {}
    profile = dict(contract=CONTRACT, sources=sources(), fresh_barrier=False)
    if args.run_log is not None:
        prefix = prefix_provenance(bounded_file(args.run_log, 4 * 1024 * 1024, 'np_connect_prefix_budget'),
                                   args.core, images['core'], args.pid, args.token)
        bases['core'] = prefix['base']
        profile.update(fresh_barrier=True, core_pid=prefix['core_pid'], prefix_sha256=prefix['prefix_sha256'],
                       barrier_token_sha256=prefix['barrier_token_sha256'])
    report = dict(host=dict(sha256=EXE_HASH), core=dict(sha256=CORE_HASH), observer=profile)
    if 'core' in bases:
        report['core']['base'] = hex(bases['core'])
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, process_access=False, images=report,
                             probes={name: dict(image=image, rva=hex(rva)) for name, (image, rva) in SITES.items()},
                             ready_for_fresh_parent_capture=True, live_validated=False,
                             psn_authenticated=False), indent=2))
        return
    require(args.run_log is not None and profile['fresh_barrier'] and args.output is not None and
            not args.output.exists(), 'np_connect_fresh_new_capture_required')
    from capture_native_hang import Capture
    from capture_home_gates_1360 import capture
    cap = Capture(args.pid, '', False)
    try:
        cap.check_architecture()
        live_identity(cap, args, False)
        hosts = [base for base, _, path in cap.modules() if Path(path).resolve() == args.exe.resolve()]
        require(len(hosts) == 1, 'np_connect_host_mapping')
        bases['host'] = hosts[0]
        report['host']['base'] = hex(bases['host'])
    finally:
        cap.finish()

    def revalidate(cap):
        live_identity(cap, args, True)
        require(digest(bounded_file(args.run_log, 4 * 1024 * 1024, 'np_connect_prefix_budget')) ==
                profile['prefix_sha256'], 'np_connect_prefix_changed')

    capture(args, images, bases, report, Observer(profile['core_pid'], revalidate))


if __name__ == '__main__':
    main()
