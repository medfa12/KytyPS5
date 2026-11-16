"""Launch/attach ONLY a freshly built sacrificial child; never accepts a PID/exe.

python tools/test_home_trace_windows.py --rounds 3 --workers 64
Requires Windows x64 Python and MinGW-w64 gcc. Compiles one tiny C target into a
new %LOCALAPPDATA%/Temp/opencode directory, no parent/emulator build dependency.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

from capture_home_gates_1360 import Threads, hardware_context, quiesce_detach, replace_probe_slot, acknowledge_handoff_step
from capture_first_fault import DebugEvent, CONTINUE, NOT_HANDLED, validate_layouts
from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE


def trial(exe, workers, mode, log_path, legacy=False):
    child = subprocess.Popen([str(exe), str(workers)], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    cap, attached = None, False
    continuation = dict(event=None, status=CONTINUE, continued=True, detached=False)
    result = dict(mode=mode, legacy=legacy, pid=child.pid)
    try:
        ready = json.loads(child.stdout.readline())
        assert ready['pid'] == child.pid
        probes = [int(p, 16) for p in ready['probes']]
        addresses, retired, handoff_pending = probes[:4], {}, {}
        cap = Capture(child.pid, '', False)
        cap.check_architecture()
        with log_path.open('x', encoding='utf-8', buffering=1) as output:
            def emit(row):
                output.write(json.dumps(row) + '\n')
            threads = Threads(cap, emit)
            local = bind(cap.k, 'GetModuleHandleW', HANDLE, C.c_wchar_p)('ntdll.dll')
            getproc = bind(cap.k, 'GetProcAddress', HANDLE, HANDLE, C.c_char_p)
            remote = next(base for base, _, name in cap.modules() if Path(name).name.lower() == 'ntdll.dll')
            bp = remote + getproc(local, b'DbgBreakPoint') - local
            breakin = remote + getproc(local, b'DbgUiRemoteBreakin') - local
            wait = bind(cap.k, 'WaitForDebugEvent', BOOL, C.POINTER(DebugEvent), DWORD)
            cont = bind(cap.k, 'ContinueDebugEvent', BOOL, DWORD, DWORD, DWORD)
            assert bind(cap.k, 'DebugActiveProcess', BOOL, DWORD)(child.pid)
            attached = True
            assert bind(cap.k, 'DebugSetProcessKillOnExit', BOOL, BOOL)(False)
            starts, consumed, hits, creates, foreign = {}, set(), 0, 0, 0
            armed, shifted, begin = False, False, time.monotonic()
            plus_phase, plus_retired = 0, {}
            plus_names = ['old0','old1','old2','old3']
            plus_definitions = {n: ('fixture', a) for n,a in zip(plus_names,addresses)}
            while True:
                assert time.monotonic() - begin < 30, 'sacrificial trace timeout'
                event = DebugEvent()
                if not wait(C.byref(event), 100):
                    assert C.get_last_error() in (121, 258)
                    if mode == 'idle' and armed and time.monotonic() - begin >= 0.5:
                        result['cleanup'] = quiesce_detach(cap, threads, continuation,
                            {p: i for i, p in enumerate(addresses)}, starts, consumed, bp, breakin, emit)
                        result.update(hits=hits, creates=creates, foreign_forwarded=foreign)
                        attached = False
                        break
                    continue
                status, stop = CONTINUE, False
                continuation.update(event=event, status=status, continued=False)
                if event.code == 3:
                    starts[event.tid] = event.data.process.start or 0
                    if event.data.process.file:
                        cap.close(event.data.process.file)
                elif event.code == 2:
                    starts[event.tid] = event.data.thread.start or 0
                    if armed:
                        creates += 1
                        # 'create' stops before arming this new thread, exercising
                        # an already delivered but uninstrumented CREATE_THREAD.
                        if mode == 'create':
                            stop = True
                        else:
                            threads.arm(event.tid, addresses)
                elif event.code == 4:
                    threads.exited(event.tid)
                    starts.pop(event.tid, None)
                    consumed.discard(event.tid)
                elif event.code == 5:
                    raise AssertionError('sacrificial child exited before detach')
                elif event.code == 6 and event.data.dll.file:
                    cap.close(event.data.dll.file)
                elif event.code == 1:
                    ex = event.data.exception
                    pc = ex.record.address or 0
                    status = NOT_HANDLED
                    if (ex.record.code == 0x80000003 and ex.first and pc in (bp, bp + 1)
                            and 0 <= starts.get(event.tid, 0) - breakin < 128 and event.tid not in consumed):
                        consumed.add(event.tid)
                        status = CONTINUE
                        if not armed:
                            for tid in cap.threads():
                                threads.arm(tid, addresses)
                            armed = True
                            if mode != 'idle':
                                child.stdin.write('GO\n')
                                child.stdin.flush()
                    elif ex.record.code == 0x80000004 and pc in addresses + list(retired):
                        handle = threads.saved[event.tid][0]
                        raw = threads.context(handle)
                        slot = addresses.index(pc) if pc in addresses else retired[pc]
                        proof = handoff_pending.pop((event.tid, pc), None)
                        assert proof is None or proof == slot
                        threads.context(handle, acknowledge_handoff_step(raw, slot, queued=proof is not None))
                        status = CONTINUE
                        hits += 1
                        if mode == 'plus-handoff-error' and hits >= 500 and not shifted:
                            from retail_plus_trace_1360 import handoff
                            new_plan = {n: ('fixture', a) for n,a in zip('efgh',probes[4:8])}
                            original_rearm = threads.rearm
                            def partial_rearm(plan):
                                # Only this disposable child: emulate one DR
                                # write succeeding before the next API fails.
                                handle = next(iter(threads.saved.values()))[0]
                                update = bytearray(hardware_context(threads.context(handle),addresses=plan))
                                struct.pack_into('<I',update,48,0x100010)
                                threads.context(handle,update)
                                raise OSError('sacrificial partial handoff failure')
                            threads.rearm = partial_rearm
                            try:
                                handoff(threads,plus_names,addresses,plus_definitions,plus_retired,
                                        new_plan,{'fixture':0},handoff_pending)
                                raise AssertionError('expected injected handoff failure')
                            except OSError:
                                result['expected_partial_handoff_failure'] = True
                                retired.update({a:s for a,(s,_) in plus_retired.items()})
                                assert addresses == probes[4:8] and set(retired) == set(probes[:4])
                                stop,shifted = True,True
                            finally:
                                threads.rearm = original_rearm
                        if mode == 'plus-phases' and plus_phase < 6 and hits >= (plus_phase + 1) * 100:
                            from retail_plus_trace_1360 import handoff
                            plans = [probes[4:8], [probes[8],*probes[5:8]],
                                     [probes[8],probes[9],*probes[6:8]],
                                     [probes[8],probes[10],*probes[6:8]],
                                     [probes[8],probes[11],*probes[6:8]],
                                     [probes[8],probes[12],*probes[6:8]]]
                            plan = plans[plus_phase]
                            updated = {('site'+str(a)): ('fixture',a) for a in plan}
                            handoff_pending = handoff(threads,plus_names,addresses,plus_definitions,
                                plus_retired,updated,{'fixture':0},handoff_pending)
                            retired.update({a: slot for a,(slot,_) in plus_retired.items()})
                            plus_phase += 1
                            result['plus_handoffs'] = plus_phase
                        if mode == 'atomic-phase' and hits >= 100 and not shifted:
                            names = ['old', 'one', 'two', 'three']
                            definitions = {n: ('fixture', a) for n, a in zip(names, addresses)}
                            updated = {'new': ('fixture', probes[4]),
                                       **{n: definitions[n] for n in names[1:]}}
                            queued = {}
                            handoff_pending = replace_probe_slot(threads, names, addresses, definitions, queued, updated, {'fixture': 0})
                            retired.update({a: slot for a, (slot, _) in queued.items()})
                            result['atomic_handoff'] = True
                            shifted = True
                        if mode == 'phase' and hits >= 100 and not shifted:
                            old_traps = {p: i for i, p in enumerate(addresses)}
                            retired[addresses[0]] = 0
                            addresses[0] = probes[4]
                            continuation['status'] = status
                            result['phase_drain'] = quiesce_detach(cap, threads, continuation,
                                old_traps, starts, consumed, bp, breakin, emit, rearm=addresses)
                            shifted = True
                        if mode in ('hit', 'phase', 'atomic-phase', 'restore-error') and hits >= 500:
                            stop = True
                        if mode == 'plus-phases' and hits >= 900:
                            assert plus_phase == 6
                            stop = True
                    elif ex.record.code == 0xe0424242:
                        foreign += 1  # forward; child VEH must receive it
                    else:
                        raise AssertionError(f'unexpected exception {ex.record.code:x} at {pc:x}')
                if mode == 'deadline' and armed and hits >= 100 and time.monotonic() - begin >= 1:
                    stop = True
                continuation['status'] = status
                if stop:
                    if legacy:
                        # Isolated negative control: exact former one-pass ordering.
                        for tid, (handle, debug) in list(threads.saved.items()):
                            threads.context(handle, hardware_context(threads.context(handle), restore=debug))
                        assert cont(event.pid, event.tid, status)
                        continuation['continued'] = True
                        assert bind(cap.k, 'DebugActiveProcessStop', BOOL, DWORD)(child.pid)
                        continuation['detached'] = True
                        result['cleanup'] = {'legacy_api_success': True}
                    else:
                        traps = {p: i for i, p in enumerate(addresses)} | retired
                        if mode == 'restore-error':
                            original_restore = threads.restore_debug
                            injected = False
                            def fail_once(tid):
                                nonlocal injected
                                if not injected:
                                    injected = True
                                    raise OSError('sacrificial injected restore failure')
                                return original_restore(tid)
                            threads.restore_debug = fail_once
                        try:
                            result['cleanup'] = quiesce_detach(cap, threads, continuation, traps,
                                                              starts, consumed, bp, breakin, emit)
                        except OSError:
                            if mode != 'restore-error':
                                raise
                            assert not continuation['detached'] and threads.held and child.poll() is None
                            result.update(expected_recovery=True, held_threads=len(threads.held),
                                          hits=hits, cleanup={'drained_owned_steps': 0})
                            return result  # finally terminates ONLY this owned test child
                    attached = False
                    result.update(hits=hits, creates=creates, foreign_forwarded=foreign)
                    break
                if not continuation['continued']:
                    assert cont(event.pid, event.tid, status)
                    continuation['continued'] = True
            try:
                if mode == 'idle':
                    child.stdin.write('GO\n')
                child.stdin.write('VERIFY\n')
                child.stdin.flush()
            except BrokenPipeError:
                pass  # a failing legacy control can crash before this write
            stdout, stderr = child.communicate(timeout=20)
            result.update(exit_code=child.returncode, child_output=stdout.strip(), stderr=stderr.strip())
            if not legacy:
                assert child.returncode == 0, result
                verified = json.loads(stdout.strip().splitlines()[-1])
                assert verified['active_dr_threads'] == 0 and verified['progress'] > 1000, result
                assert verified['foreign_seen'] == 2 and foreign == (0 if mode == 'idle' else 1), result
                assert verified['sentry_suspend_count'] == 1, result
                if mode != 'idle':
                    assert (hits >= 100 or mode == 'create') and creates + result['cleanup']['drained_creates'] > 0, result
            for tid in list(threads.saved):
                threads.exited(tid)
        return result
    finally:
        # Only this Popen-owned sacrificial child may be terminated by the harness.
        if child.poll() is None:
            child.kill()
        if attached and cap:
            bind(cap.k, 'DebugActiveProcessStop', BOOL, DWORD)(child.pid)
        child.wait(timeout=10)
        if cap:
            cap.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--workers', type=int, default=64)
    parser.add_argument('--legacy-negative', action='store_true')
    parser.add_argument('--cases', nargs='+', choices=('hit', 'deadline', 'create', 'phase', 'atomic-phase', 'plus-phases', 'plus-handoff-error', 'idle', 'restore-error'),
                        default=['hit', 'deadline', 'create', 'phase', 'idle', 'restore-error'])
    args = parser.parse_args()
    if args.legacy_negative and any(case in ('idle', 'restore-error') for case in args.cases):
        parser.error('--legacy-negative requires explicit --cases hit deadline create (or phase)')
    assert os.name == 'nt' and 1 <= args.rounds <= 20 and 4 <= args.workers <= 256
    validate_layouts()
    root = Path(tempfile.mkdtemp(prefix='home-trace-lifecycle-',
                               dir=Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode'))
    exe = root / 'home_trace_sacrificial.exe'
    subprocess.run(['gcc', '-O0', '-g', '-Wall', '-Wextra', '-o', str(exe),
                    str(Path(__file__).parent / 'tests/home_trace_sacrificial.c')], check=True)
    results = []
    print('ARTIFACTS', root, flush=True)
    for round_ in range(args.rounds):
        for mode in args.cases:
            result = trial(exe, args.workers, mode, root / f'{round_}-{mode}.jsonl', args.legacy_negative)
            results.append(result)
            (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
            print(json.dumps(result), flush=True)
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    if args.legacy_negative:
        assert any(r['exit_code'] == 0x80000004 for r in results), 'legacy failure was not reproduced'
    elif any(case not in ('idle', 'restore-error') for case in args.cases):
        assert sum(r['cleanup']['drained_owned_steps'] for r in results) > 0, 'no queued-step coverage'
    print('PASS' if not args.legacy_negative else 'NEGATIVE CONTROL COMPLETED', flush=True)


if __name__ == '__main__':
    main()
