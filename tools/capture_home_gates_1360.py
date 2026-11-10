"""Opt-in, bounded Windows AMD64 Home gate trace. Default action is offline check.

Hardware execution breakpoints only: no WriteProcessMemory, guest calls, code
patches, forced promise results or native-return modifications. A debugger pause
affects timing. Four slots: Execute/Resolve/Reject/timing Dispatch; after correlated
nonempty Games + settled Media, Execute is replaced by the predicate result site.
--trace-mode queue instead fixes four slots at adapter lookup/action, GetItems2
completion and rejection-helper entry, with callback-ID correlation and no rearm.
--trace-mode delivery --user ID fixes serialization/Resolve/Reject/first callback
dispatch; acquisition starts at serialization and does not establish JS consumption.
--trace-mode executor follows serialization/queued bridge call/executor call/return
with fresh closure-owner identities. Executor return is not JS acceptance.
Use Ctrl+C for orderly restoration/detach; do not forcibly kill a debugger.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import signal
import struct
import sys
import time

sys.dont_write_bytecode = True

from capture_first_fault import DebugEvent, CONTINUE, NOT_HANDLED, validate_layouts
from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE, MemoryInfo
from home_gate_trace_1360 import (PROBES, WINDOWS, Correlator, DecodeError, Reader, ScopeExcluded,
                                digest, provenance, require)
from home_queue_trace_1360 import (PROBES as QUEUE_PROBES, WINDOWS as QUEUE_WINDOWS,
                                 QueueCorrelator, abi_slots, layout_reference)
from home_delivery_trace_1360 import (PROBES as DELIVERY_PROBES, WINDOWS as DELIVERY_WINDOWS,
                                    DeliveryCorrelator, abi_slots as delivery_abi_slots)
from home_executor_trace_1360 import (PROBES as EXECUTOR_PROBES, WINDOWS as EXECUTOR_WINDOWS,
                                    ExecutorCorrelator, abi_slots as executor_abi_slots)

PROBES = {**PROBES, **QUEUE_PROBES, **DELIVERY_PROBES, **EXECUTOR_PROBES}


REGISTERS = ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
             'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip')
SINGLE_STEP = 0x80000004


def hardware_context(raw, addresses=None, restore=None, hit=None):
    """Pure AMD64 CONTEXT transform, also used by offline negative controls.

    DR0..3/DR6/DR7 at 72..112; EFlags at68; RIP248 (Windows winnt.h).
    At an owned execution fault set RF only, preserving TF/native AL and RIP.
    """
    result = bytearray(raw)
    require(len(raw) == 1232, 'context_size')
    if addresses is not None:
        require(len(addresses) == 4 and len(set(addresses)) == 4, 'hardware_slots')
        struct.pack_into('<4Q', result, 72, *addresses)
        reserved = struct.unpack_from('<Q', raw, 112)[0] & 0x400
        struct.pack_into('<Q', result, 112, reserved | 0x55)  # local execute, len=1
    if restore is not None:
        struct.pack_into('<6Q', result, 72, *restore)
    if hit is not None:
        dr6 = struct.unpack_from('<Q', raw, 104)[0]
        require(dr6 & 15 == 1 << hit and not dr6 & 0xe000, 'foreign_debug_cause')
        struct.pack_into('<Q', result, 104, dr6 & ~(1 << hit))
        flags = struct.unpack_from('<I', result, 68)[0]
        struct.pack_into('<I', result, 68, flags | 0x10000)
    return bytes(result)


def acknowledge_handoff_step(raw, slot, queued=False):
    """A DR write can clear DR6 for an already queued execution exception.

    Zero is admitted only with a pre-handoff (thread,PC,slot) ownership proof.
    TF, mixed causes and nonzero foreign causes remain fatal. Only RF changes.
    """
    flags = struct.unpack_from('<I', raw, 68)[0]
    dr6 = struct.unpack_from('<Q', raw, 104)[0]
    if queued and not dr6 & 15:
        require(not dr6 & 0xe000 and not flags & 0x100, 'foreign_handoff_step')
        result = bytearray(raw)
        struct.pack_into('<I', result, 68, flags | 0x10000)
        return bytes(result)
    return hardware_context(raw, hit=slot)


class Threads:
    def __init__(self, cap, emit):
        self.cap, self.emit, self.saved = cap, emit, {}
        self.held, self.draining, self.deferred = {}, False, set()
        self.set_context = bind(cap.k, 'SetThreadContext', BOOL, HANDLE, HANDLE)

    def context(self, handle, data=None):
        backing = C.create_string_buffer(1247)
        p = (C.addressof(backing) + 15) & ~15
        if data is None:
            DWORD.from_address(p + 48).value = 0x100013  # control, integer, debug
            if not self.cap.context(handle, p):
                raise C.WinError(C.get_last_error())
            return C.string_at(p, 1232)
        C.memmove(p, bytes(data), 1232)
        if not self.set_context(handle, p):
            raise C.WinError(C.get_last_error())

    def arm(self, tid, addresses):
        require(not self.draining, 'arming_during_shutdown')
        if tid in self.saved:
            raw = self.context(self.saved[tid][0])
            require(struct.unpack_from('<4Q', raw, 72) == tuple(addresses), 'duplicate_thread_state')
            return
        require(len(self.saved) < 1024, 'thread_budget')
        handle = self.cap.open_thread(0x10005a, False, tid)  # plus SYNCHRONIZE for liveness
        if not handle:
            raise C.WinError(C.get_last_error())
        try:
            raw = self.context(handle)
            debug = struct.unpack_from('<6Q', raw, 72)
            require(not any(debug[:4]) and not debug[5] & 0xffff23ff, 'debug_registers_occupied')
            require(not struct.unpack_from('<I', raw, 68)[0] & 0x100, 'thread_single_stepping')
            # Save before SetThreadContext, including a failed/partial API attempt.
            self.saved[tid] = (handle, debug)
            update = bytearray(hardware_context(raw, addresses=addresses))
            struct.pack_into('<I', update, 48, 0x100010)
            self.context(handle, update)
        except BaseException:
            if tid not in self.saved:
                self.cap.close(handle)
            raise

    def rearm(self, addresses):
        require(not self.draining, 'rearming_during_shutdown')
        for handle, _ in self.saved.values():
            update = bytearray(hardware_context(self.context(handle), addresses=addresses))
            struct.pack_into('<I', update, 48, 0x100010)
            self.context(handle, update)

    def exited(self, tid):
        self.held.pop(tid, None)
        self.deferred.discard(tid)
        state = self.saved.pop(tid, None)
        if state:
            self.cap.close(state[0])

    def alive(self, handle):
        result = bind(self.cap.k, 'WaitForSingleObject', DWORD, HANDLE, DWORD)(handle, 0)
        require(result in (0, 258), 'thread_liveness_wait')
        # An exited thread can itself return259 (STILL_ACTIVE); the signaled
        # thread object, unlike GetExitCodeThread, is unambiguous.
        return result == 258

    def freeze_all(self):
        """Acquire exactly one explicit suspension; keep it across event continuation.

        Debug-event suspension alone is insufficient: ContinueDebugEvent can
        resume a pending exception with a context different from our readback.
        GetThreadContext after SuspendThread provides a suspension rendezvous.
        """
        self.draining = True
        added = 0
        for tid in self.cap.threads():
            if tid in self.held:
                continue
            require(len(self.saved) < 1024 or tid in self.saved, 'shutdown_thread_budget')
            known = tid in self.saved
            uninitialized = not known or self.saved[tid][1] is None
            handle = self.saved[tid][0] if known else self.cap.open_thread(0x10005a, False, tid)
            if not handle:
                # A snapshot member can exit before OpenThread; next snapshot and
                # EXIT_THREAD events close the race. Other failures are fatal.
                require(C.get_last_error() == 87, 'shutdown_open_thread')
                continue
            previous = self.cap.suspend(handle)
            if previous == 0xffffffff:
                error = C.get_last_error()
                # A terminating thread can reject SuspendThread while its exit
                # code still says STILL_ACTIVE. Defer, then consume EXIT_THREAD;
                # never treat that transient as a verified/restored live thread.
                if self.alive(handle):
                    require(error in (5, 87), 'shutdown_suspend')
                    self.deferred.add(tid)
                    if not known:
                        self.saved[tid] = (handle, None)
                elif not known:
                    self.cap.close(handle)
                continue
            self.deferred.discard(tid)
            self.held[tid] = previous
            # Retain the handle even if a later API fails; never lose recovery state.
            if uninitialized:
                self.saved[tid] = (handle, None)
            raw = self.context(handle)
            if uninitialized:
                debug = struct.unpack_from('<6Q', raw, 72)
                require(not any(debug[:4]) and not debug[5] & 0xffff23ff,
                        'unknown_thread_debug_state')
                self.saved[tid] = (handle, debug)
            added += 1
        return added

    @staticmethod
    def debug_equal(raw, expected):
        actual = struct.unpack_from('<6Q', raw, 72)
        # Architectural reserved DR6 bits and DR7 bit10 may be normalized by Windows.
        return (actual[:4] == expected[:4] and
                (actual[4] & 0xe00f) == (expected[4] & 0xe00f) and
                (actual[5] & ~0x400) == (expected[5] & ~0x400))

    def restore_debug(self, tid):
        handle, debug = self.saved[tid]
        require(debug is not None and tid in self.held, 'restore_without_owned_hold')
        # Debug-only SetThreadContext: do not write stale GPR/control snapshots
        # from another thread's outstanding exception.
        raw = bytearray(1232)
        struct.pack_into('<I', raw, 48, 0x100010)
        struct.pack_into('<6Q', raw, 72, *debug)
        self.context(handle, raw)
        require(self.debug_equal(self.context(handle), debug), 'debug_restore_readback')

    def sweep(self):
        repairs = 0
        for tid, (handle, debug) in list(self.saved.items()):
            if not self.alive(handle):
                self.exited(tid)
                continue
            if tid in self.deferred:
                continue
            require(tid in self.held and debug is not None, 'unheld_live_thread')
            if not self.debug_equal(self.context(handle), debug):
                self.restore_debug(tid)
                repairs += 1
        return repairs

    def release_holds(self):
        # Release under debugger protection during drain, or after verified detach.
        # Remove exactly our increment, preserving preexisting suspend counts.
        for tid in list(self.held):
            handle = self.saved[tid][0]
            if self.alive(handle):
                result = self.cap.resume(handle)
                require(result != 0xffffffff, 'shutdown_resume')
                self.held.pop(tid)
                # SuspendThread's earlier return can include debugger suspension;
                # that count disappears during Continue/Detach. Never "balance"
                # to that stale count: release exactly our one increment.
                require(result > 0 or not self.alive(handle), 'owned_suspend_missing')
            self.held.pop(tid, None)


def quiesce_detach(cap, threads, continuation, traps, starts, consumed, bp, breakin,
                   emit, timeout=30, rearm=None):
    try:
        return _quiesce_detach(cap, threads, continuation, traps, starts, consumed,
                               bp, breakin, emit, timeout, rearm)
    except BaseException:
        # Do not let an aborted running-drain turn into an implicit unsafe detach
        # when Python exits. Retain explicit holds for owner recovery, not success.
        recovery_error = None
        try:
            threads.freeze_all()
            threads.sweep()
        except (OSError, DecodeError) as error:
            recovery_error = type(error).__name__
        emit(dict(event='shutdown_recovery_required', detached=continuation['detached'],
                  holds=dict(threads.held), deferred=sorted(threads.deferred),
                  recovery_error=recovery_error))
        raise


def _quiesce_detach(cap, threads, continuation, traps, starts, consumed, bp, breakin,
                    emit, timeout, rearm):
    """Stop generation, drain debug events, verify while held, detach, verify again.

    `continuation` is shared with the caller so a failure never double-continues
    an event. On failure explicit suspension increments and handles are retained;
    no best-effort resume/detach is presented as successful cleanup.
    """
    wait = bind(cap.k, 'WaitForDebugEvent', BOOL, C.POINTER(DebugEvent), DWORD)
    cont = bind(cap.k, 'ContinueDebugEvent', BOOL, DWORD, DWORD, DWORD)
    detach = bind(cap.k, 'DebugActiveProcessStop', BOOL, DWORD)
    deadline = time.monotonic() + timeout
    owned = set(threads.saved)  # immutable ownership before seeing unarmed creates
    stats = dict(drained_events=0, drained_owned_steps=0, drained_creates=0,
                 residual_repairs=0, post_detach_repairs=0)
    emit(dict(event='shutdown_begin', owned_threads=len(owned)))
    threads.freeze_all()
    candidates = {tid for tid, (h, _) in threads.saved.items() if tid in owned and
                  struct.unpack_from('<Q', threads.context(h), 248)[0] in traps}
    stats['initial_trap_contexts'] = len(candidates)
    for tid in list(threads.held):
        threads.restore_debug(tid)

    def continue_pending():
        event = continuation.get('event')
        if event is not None and not continuation.get('continued', False):
            if not cont(event.pid, event.tid, continuation['status']):
                raise C.WinError(C.get_last_error())
            continuation['continued'] = True

    # A suspended thread may not publish its pending debug exception until its
    # explicit hold is released. Empty queue while all held is NOT a drain proof.
    # Release under debugger protection, then continue to service owned traps.
    threads.release_holds()
    continue_pending()
    last_owned = time.monotonic()
    final_frozen, quiet = False, 0
    while True:
        if time.monotonic() >= deadline:
            emit(dict(event='shutdown_blocked', candidates=sorted(candidates),
                      deferred=sorted(threads.deferred), held=len(threads.held), **stats))
        require(time.monotonic() < deadline, 'shutdown_drain_timeout')
        if not final_frozen and time.monotonic() - last_owned >= 0.1:
            # Rendezvous after running drain. A candidate may be retired only
            # after its context has advanced away from all owned trap sites.
            threads.freeze_all()
            repairs = threads.sweep()
            stats['residual_repairs'] += repairs
            candidates = {tid for tid in candidates if tid in threads.saved and
                          struct.unpack_from('<Q', threads.context(threads.saved[tid][0]), 248)[0] in traps}
            if candidates or repairs or threads.freeze_all() or threads.deferred:
                threads.release_holds()
                last_owned = time.monotonic()
                continue
            final_frozen = True
        event = DebugEvent()
        if not wait(C.byref(event), 50):
            require(C.get_last_error() in (121, 258), 'shutdown_wait')
            if final_frozen:
                quiet += 1
                if threads.freeze_all():
                    quiet = 0
                repairs = threads.sweep()
                stats['residual_repairs'] += repairs
                if repairs or threads.deferred:
                    threads.release_holds()
                    final_frozen, quiet, last_owned = False, 0, time.monotonic()
                    continue
                if quiet >= 2:
                    break
            continue
        quiet = 0
        continuation.update(event=event, status=CONTINUE, continued=False)
        stats['drained_events'] += 1
        require(stats['drained_events'] <= 10000, 'shutdown_event_budget')
        if event.code == 2:
            starts[event.tid] = event.data.thread.start or 0
            stats['drained_creates'] += 1
            threads.freeze_all()  # include a create arriving after the prior snapshot
        elif event.code == 3:
            starts[event.tid] = event.data.process.start or 0
            if event.data.process.file:
                cap.close(event.data.process.file)
            threads.freeze_all()
        elif event.code == 4:
            threads.exited(event.tid)
            starts.pop(event.tid, None)
            consumed.discard(event.tid)
        elif event.code == 6 and event.data.dll.file:
            cap.close(event.data.dll.file)
        elif event.code == 5:
            continue_pending()
            return dict(stats, process_exited=True, detached=False, verified_threads=0)
        elif event.code == 1:
            ex = event.data.exception
            pc = ex.record.address or 0
            continuation['status'] = NOT_HANDLED
            if (ex.first and ex.record.code == 0x80000003 and pc in (bp, bp + 1)
                    and 0 <= starts.get(event.tid, 0) - breakin < 128
                    and event.tid not in consumed):
                consumed.add(event.tid)
                continuation['status'] = CONTINUE
            elif (ex.first and ex.record.code == SINGLE_STEP and pc in traps
                  and event.tid in owned and event.tid in threads.saved):
                final_frozen = False
                last_owned = time.monotonic()
                threads.freeze_all()
                candidates.add(event.tid)
                handle = threads.saved[event.tid][0]
                raw = bytearray(threads.context(handle))
                dr6 = struct.unpack_from('<Q', raw, 104)[0]
                flags = struct.unpack_from('<I', raw, 68)[0]
                require(struct.unpack_from('<Q', raw, 248)[0] == pc and
                        not dr6 & 0xe000 and dr6 & 15 in (0, 1 << traps[pc]) and
                        not flags & 0x100, 'shutdown_foreign_step')
                # DR6 can already be zero because the initial disarm ran before
                # this queued exception became the debugger's current event.
                struct.pack_into('<I', raw, 48, 0x100001)  # control only (RF)
                struct.pack_into('<I', raw, 68, flags | 0x10000)
                threads.context(handle, raw)
                threads.restore_debug(event.tid)
                continuation['status'] = CONTINUE
                stats['drained_owned_steps'] += 1
            if continuation['status'] == NOT_HANDLED:
                emit(dict(event='shutdown_forwarded_exception', code=hex(ex.record.code),
                          first=bool(ex.first)))
        # Includes held threads from a late CREATE_THREAD or a failed final
        # empty-queue check. They must run under debugger protection again.
        if not final_frozen:
            threads.release_holds()
        continue_pending()
    stats['verified_threads'] = len(threads.held)
    emit(dict(event='shutdown_pre_detach_verified', **stats))
    if rearm is not None:
        threads.draining = False
        threads.rearm(rearm)
        threads.release_holds()
        emit(dict(event='phase_rearmed_after_drain', threads=stats['verified_threads']))
        return dict(stats, process_exited=False, detached=False, rearmed=True)
    if not detach(cap.pid):
        raise C.WinError(C.get_last_error())
    continuation['detached'] = True
    # Detach must not be followed by running user code until these readbacks pass.
    stats['post_detach_repairs'] = threads.sweep()
    require(stats['post_detach_repairs'] == 0, 'post_detach_residual_requires_recovery')
    for tid, (handle, debug) in threads.saved.items():
        require(threads.debug_equal(threads.context(handle), debug), 'post_detach_debug_state')
    emit(dict(event='shutdown_post_detach_verified', threads=len(threads.held),
              repairs=stats['post_detach_repairs']))
    threads.release_holds()
    emit(dict(event='shutdown_released', threads=stats['verified_threads']))
    for tid in list(threads.saved):
        threads.exited(tid)
    return dict(stats, process_exited=False, detached=True)


def created(cap, handle):
    times = (C.c_uint64 * 4)()
    fn = bind(cap.k, 'GetProcessTimes', BOOL, HANDLE, HANDLE, HANDLE, HANDLE, HANDLE)
    if not fn(handle, *[C.byref(times, i * 8) for i in range(4)]):
        raise C.WinError(C.get_last_error())
    return times[0]


def process_identity(cap, args):
    require(created(cap, cap.process) == args.created_filetime, 'process_creation_mismatch')
    path, size = C.create_unicode_buffer(32768), DWORD(32768)
    fn = bind(cap.k, 'QueryFullProcessImageNameW', BOOL, HANDLE, DWORD, C.c_wchar_p, C.POINTER(DWORD))
    if not fn(cap.process, 0, path, C.byref(size)):
        raise C.WinError(C.get_last_error())
    require(Path(path.value).resolve() == args.exe.resolve(), 'process_path_mismatch')
    require(digest(args.exe.read_bytes()) == args.exe_sha256.lower(), 'process_image_hash')


def live_windows(cap, images, bases, extra=None):
    windows_by_image = {key: list(value) for key, value in WINDOWS.items()}
    for key, windows in (extra or {}).items():
        windows_by_image.setdefault(key, []).extend(windows)
    for key, windows in windows_by_image.items():
        if key not in bases:
            continue
        for rva, size in windows:
            at = bases[key] + rva
            info = MemoryInfo()
            require(bool(cap.query(cap.process, at, C.byref(info), C.sizeof(info))), 'mapping_query')
            require(info.state == 0x1000 and not info.protect & 0x101
                    and info.protect & 0xf0 and at + size <= info.base + info.size, 'mapping_protection')
            require(cap.read_exact(at, size) == images[key].at(rva, size), 'live_image_bytes_' + key)


def replace_probe_slot(threads, names, addresses, probes, retired, updated, bases):
    """One code-slot handoff while a debug event stops all target threads.

    No running drain/gap. Retain the former PC/slot so already queued owned
    exceptions are consumed, not forwarded after DR0 changes. Never use for data
    watchpoints or multiple/repeated migrations. All GPRs/RIP remain unchanged.
    """
    new_names = list(updated)
    new_addresses = [bases[updated[n][0]] + updated[n][1] for n in new_names]
    require(len(new_names) == 4 and len(set(new_addresses)) == 4 and not retired,
            'single_code_handoff')
    require(new_names[1:] == names[1:] and new_addresses[1:] == addresses[1:] and
            new_addresses[0] != addresses[0], 'handoff_only_slot_zero')
    retired[addresses[0]] = (0, names[0])
    pending = {}
    for tid, (handle, _) in threads.saved.items():
        raw = threads.context(handle)
        pc = struct.unpack_from('<Q', raw, 248)[0]
        flags = struct.unpack_from('<I', raw, 68)[0]
        if pc in addresses and not flags & (0x100 | 0x10000):
            slot = addresses.index(pc)
            dr6 = struct.unpack_from('<Q', raw, 104)[0]
            require(not dr6 & 0xe000 and dr6 & 15 in (0, 1 << slot), 'handoff_foreign_pending')
            pending[(tid, pc)] = slot
    # Keep both old and new definitions for queued-trap byte verification.
    probes.update(updated)
    threads.rearm(new_addresses)
    names[:], addresses[:] = new_names, new_addresses
    return pending


def capture(args, images, bases, report, observer=None):
    validate_layouts()
    cap = Capture(args.pid, '', False)
    attached, exited = False, False
    continuation = dict(event=None, continued=True, detached=False)
    starts, consumed, retired = {}, set(), {}
    handoff_pending = {}
    queue_mode = getattr(args, 'trace_mode', 'gates') == 'queue'
    delivery_mode = getattr(args, 'trace_mode', 'gates') == 'delivery'
    executor_mode = getattr(args, 'trace_mode', 'gates') == 'executor'
    extra_windows = (EXECUTOR_WINDOWS if executor_mode else DELIVERY_WINDOWS if delivery_mode
                     else QUEUE_WINDOWS if queue_mode else None)
    names = (list(EXECUTOR_PROBES) if executor_mode else list(DELIVERY_PROBES) if delivery_mode else list(QUEUE_PROBES) if queue_mode
             else ['execute', 'resolve', 'reject', 'dispatch'])
    probes = dict(observer.probes) if observer is not None else PROBES
    if observer is not None:
        names, extra_windows = list(probes), observer.windows
    addresses = [bases[probes[n][0]] + probes[n][1] for n in names]
    bp = breakin = 0
    threads = None
    old_signals = {}
    output = None
    stop = False
    reason = 'deadline'
    started = time.monotonic()
    try:
        cap.check_architecture()
        process_identity(cap, args)
        # Refuse overwrite: output artifacts may belong to another run.
        output = args.output.open('x', encoding='utf-8', buffering=1)
        written = 0

        def emit(row):
            nonlocal written, stop, reason
            if getattr(args, 'timeline_log', None) and row.get('event') in ('provenance', 'armed', 'shutdown_begin', 'end'):
                from home_capture_timeline import sample
                row = dict(row, timeline=sample(args.timeline_log))
            data = json.dumps(dict(t=round(time.monotonic() - started, 6), **row), separators=(',', ':')) + '\n'
            written += len(data.encode('utf-8'))
            # Reserve cleanup room; event/hit/request bounds limit the tail too.
            if written > args.max_bytes - 1048576:
                if not stop:
                    reason = 'output_budget'
                stop = True
            output.write(data)

        emit(dict(event='provenance', diagnostic_version=9 if observer is not None and observer.mode == 'job' else 8 if observer is not None and observer.mode == 'route' else 7 if observer is not None else 6 if executor_mode else 5 if delivery_mode else 4 if queue_mode else 3, lifecycle_version=2,
                   trace_mode=observer.mode if observer is not None else 'executor' if executor_mode else 'delivery' if delivery_mode else 'queue' if queue_mode else 'gates',
                  pid=args.pid, created_filetime=args.created_filetime,
                  exe_sha256=args.exe_sha256, images=report, run_log_sha256=digest(args.run_log.read_bytes()),
                   coverage=('native invocations only after observed entry; no GPU/JS/Home readiness inference'
                             if observer is not None else 'starts at armed event; pre-attach requests and JS consumption unknown')))
        threads = Threads(cap, emit)
        model = (ExecutorCorrelator(emit, bases, executor_abi_slots(images), args.user) if executor_mode else
                 DeliveryCorrelator(emit, bases, delivery_abi_slots(images), args.user) if delivery_mode else
                 QueueCorrelator(emit, bases, abi_slots(images), args.user)
                 if queue_mode else Correlator(emit))
        if observer is not None:
            model = observer.model(emit, bases)
        local = bind(cap.k, 'GetModuleHandleW', HANDLE, C.c_wchar_p)('ntdll.dll')
        proc = bind(cap.k, 'GetProcAddress', HANDLE, HANDLE, C.c_char_p)
        remote = [base for base, _, name in cap.modules() if Path(name).name.lower() == 'ntdll.dll']
        require(len(remote) == 1 and bool(local), 'ntdll_identity')
        bp_local, breakin_local = proc(local, b'DbgBreakPoint'), proc(local, b'DbgUiRemoteBreakin')
        require(bool(bp_local and breakin_local), 'ntdll_exports')
        bp, breakin = remote[0] + bp_local - local, remote[0] + breakin_local - local
        attach = bind(cap.k, 'DebugActiveProcess', BOOL, DWORD)
        wait = bind(cap.k, 'WaitForDebugEvent', BOOL, C.POINTER(DebugEvent), DWORD)
        cont = bind(cap.k, 'ContinueDebugEvent', BOOL, DWORD, DWORD, DWORD)
        kill = bind(cap.k, 'DebugSetProcessKillOnExit', BOOL, BOOL)

        def request_stop(*_):
            nonlocal stop
            stop = True

        for sig in (signal.SIGINT, signal.SIGBREAK):
            old_signals[sig] = signal.signal(sig, request_stop)
        if not attach(args.pid):
            raise C.WinError(C.get_last_error())
        attached = True
        if not kill(False):
            raise C.WinError(C.get_last_error())
        armed = False
        hits, events, errors = 0, 0, 0
        probe_hits, decode_codes, excluded_codes, diagnostic_codes = {}, {}, {}, {}
        excluded = 0
        cleanup_result = {}

        def shutdown():
            model.close_coverage(time.monotonic() - started, 'shutdown_drain')
            traps = {address: slot for slot, address in enumerate(addresses)}
            traps.update({address: slot for address, (slot, _) in retired.items()})
            return quiesce_detach(cap, threads, continuation, traps, starts, consumed,
                                  bp, breakin, emit)

        while True:
            if time.monotonic() - started >= args.seconds:
                stop = True
            event = DebugEvent()
            if not wait(C.byref(event), 100):
                if C.get_last_error() not in (121, 258):
                    raise C.WinError(C.get_last_error())
                if stop:
                    cleanup_result = shutdown()
                    attached = False
                    break
                continue
            continuation.update(event=event, status=CONTINUE, continued=False)
            status, done = CONTINUE, False
            events += 1
            if events >= args.max_events:
                stop, reason = True, 'event_budget'
            try:
                if event.code == 3:
                    require(created(cap, event.data.process.process) == args.created_filetime, 'attached_process_identity')
                    starts[event.tid] = event.data.process.start or 0
                    if event.data.process.file:
                        cap.close(event.data.process.file)
                elif event.code == 2:
                    if observer is not None:
                        model.thread_boundary(event.tid)
                    starts[event.tid] = event.data.thread.start or 0
                    if armed:
                        threads.arm(event.tid, addresses)
                        emit(dict(event='thread_armed', tid=event.tid))
                elif event.code == 4:
                    if observer is not None:
                        model.thread_boundary(event.tid)
                    threads.exited(event.tid)
                    starts.pop(event.tid, None)
                    consumed.discard(event.tid)
                    handoff_pending = {k: v for k, v in handoff_pending.items() if k[0] != event.tid}
                elif event.code == 5:
                    exited, done, reason = True, True, 'process_exit'
                elif event.code == 6 and event.data.dll.file:
                    cap.close(event.data.dll.file)
                elif (event.code == 7 and observer is not None and
                      event.data.unload.base in getattr(observer, 'watched_module_bases', ())):
                    stop, reason = True, 'observed_module_unloaded'
                    emit(dict(event='observed_module_unloaded', base=hex(event.data.unload.base),
                              coverage_complete=False))
                elif event.code == 1:
                    ex = event.data.exception
                    status = NOT_HANDLED
                    pc = ex.record.address or 0
                    ours_bp = (ex.record.code == 0x80000003 and ex.first and pc in (bp, bp + 1)
                               and 0 <= starts.get(event.tid, 0) - breakin < 128
                               and event.tid not in consumed)
                    if ours_bp:
                        consumed.add(event.tid)
                        status = CONTINUE
                        if stop:
                            done = True
                        elif not armed:
                            if observer is not None and hasattr(observer, 'validate_attached'):
                                observer.validate_attached(cap, bases)
                            live_windows(cap, images, bases, extra_windows)
                            for tid in cap.threads():
                                threads.arm(tid, addresses)
                            armed = True
                            emit(dict(event='armed', phase=model.phase, threads=len(threads.saved),
                                      probes=dict(zip(names, map(hex, addresses)))))
                    elif ex.record.code == SINGLE_STEP and ex.first and event.tid in threads.saved:
                        handle = threads.saved[event.tid][0]
                        raw = threads.context(handle)
                        regs = dict(zip(REGISTERS, struct.unpack_from('<17Q', raw, 120)))
                        if pc == regs['rip'] and (pc in addresses or pc in retired):
                            slot = addresses.index(pc) if pc in addresses else retired[pc][0]
                            # Reject mixed/foreign debug causes before acknowledging.
                            queued = handoff_pending.pop((event.tid, pc), None)
                            require(queued is None or queued == slot, 'handoff_pending_slot')
                            resumed = acknowledge_handoff_step(raw, slot, queued=queued is not None)
                            require(struct.unpack_from('<4Q', raw, 72) == tuple(addresses)
                                    and struct.unpack_from('<Q', raw, 112)[0] & ~0x400 == 0x55, 'debug_state_changed')
                            threads.context(handle, resumed)
                            status = CONTINUE
                            hits += 1
                            name = names[slot] if pc in addresses else retired[pc][1]
                            probe_hits[name] = probe_hits.get(name, 0) + 1
                            key, rva = probes[name]
                            require(cap.read_exact(pc, 8) == images[key].at(rva, 8), 'probe_bytes_changed')
                            reader = Reader(cap.read_exact, bases.get('mono', 0))
                            now = time.monotonic() - started
                            try:
                                if pc in retired:
                                    # Another thread may already have faulted at
                                    # the old slot when the phase was changed.
                                    # Consume that owned queued trap without
                                    # extending request-entry coverage.
                                    emit(dict(event='retired_probe_event', probe=name, tid=event.tid))
                                elif observer is not None or queue_mode or delivery_mode or executor_mode:
                                    model.hit(name, reader, regs, now, tid=event.tid)
                                elif name == 'execute':
                                    model.request(reader, regs, bases['vsh'], now)
                                elif name in ('resolve', 'reject'):
                                    model.settlement(reader, regs, name, now)
                                else:
                                    model.timing(reader, regs, name == 'predicate')
                            except DecodeError as error:
                                diagnostic = reader.diagnostic(error)
                                bucket = ':'.join((name, diagnostic['site'], str(error),
                                                   diagnostic.get('observed_type', '-')))
                                diagnostic_codes[bucket] = diagnostic_codes.get(bucket, 0) + 1
                                if isinstance(error, ScopeExcluded):
                                    excluded += 1
                                    excluded_codes[str(error)] = excluded_codes.get(str(error), 0) + 1
                                    count, category = excluded, 'scope_excluded'
                                else:
                                    errors += 1
                                    decode_codes[str(error)] = decode_codes.get(str(error), 0) + 1
                                    count, category = errors, 'decode_unknown'
                                # Separate fixed caps keep normal non-Home traffic
                                # from hiding the first unsupported-layout errors.
                                if count <= 100:
                                    emit(dict(event=category, probe=name, tid=event.tid,
                                              code=str(error), diagnostic=diagnostic))
                                if observer is not None and errors >= getattr(observer, 'max_decode_unknowns', 1000000):
                                    stop, reason = True, 'decode_budget'
                            # Provenance/context-write failures are fatal and use
                            # the outer verified shutdown path, not decode-and-run.
                            if observer is not None and hasattr(observer, 'transition'):
                                update = observer.transition()
                                if update is not None:
                                    updated, placements = update
                                    require(not any(k in bases for k in placements), 'handoff_mapping_reused')
                                    bases.update(placements)
                                    live_windows(cap, images, bases, extra_windows)
                                    if hasattr(observer, 'handoff'):
                                        handoff_pending = observer.handoff(
                                            threads, names, addresses, probes, retired, updated, bases, handoff_pending)
                                    else:
                                        handoff_pending = replace_probe_slot(
                                            threads, names, addresses, probes, retired, updated, bases)
                                    emit(dict(event='probe_handoff', phase=model.phase,
                                              probes=dict(zip(names, map(hex, addresses))),
                                              placements={k: hex(v) for k, v in placements.items()},
                                              running_gap=False))
                            if observer is not None and getattr(model, 'complete', False):
                                stop, reason = True, 'focused_boundary_complete'
                            if model.ready and model.phase == 'database':
                                live_windows(cap, images, bases)
                                model.close_coverage(time.monotonic() - started, 'phase_drain')
                                old_traps = {a: i for i, a in enumerate(addresses)}
                                retired[addresses[0]] = (0, names[0])
                                names[0] = 'predicate'
                                addresses[0] = bases['pui'] + PROBES['predicate'][1]
                                continuation['status'] = status
                                phase_cleanup = quiesce_detach(cap, threads, continuation,
                                    old_traps, starts, consumed, bp, breakin, emit, rearm=addresses)
                                if phase_cleanup['process_exited']:
                                    exited, done, attached = True, True, False
                                    reason = 'process_exit'
                                    cleanup_result = phase_cleanup
                                else:
                                    model.phase = 'interactive'
                                    emit(dict(event='phase', phase=model.phase, evidence=model.ready,
                                              drain=phase_cleanup,
                                              coverage='Execute no longer observed; subsequent DB requests/user switches not covered'))
                            if hits >= args.max_hits:
                                stop, reason = True, 'hit_budget'
                    if status == NOT_HANDLED:
                        emit(dict(event='forwarded_exception', code=hex(ex.record.code), first=bool(ex.first)))
                if stop:
                    done = True
                if done and not exited:
                    continuation['status'] = status
                    cleanup_result = shutdown()
                    attached = False
            except BaseException:
                if not exited and not threads.draining:
                    continuation['status'] = status
                    cleanup_result = shutdown()
                    attached = False
                emit(dict(event='event_abort', cleanup=cleanup_result,
                          coverage_complete=False))
                raise
            finally:
                # Forward every exception except the verified debugger/hardware traps.
                if not threads.draining and not continuation['continued']:
                    continuation['status'] = status
                    if not cont(event.pid, event.tid, status):
                        raise C.WinError(C.get_last_error())
                    continuation['continued'] = True
            if done:
                break
        attached = False
        emit(dict(event='end', reason=reason, hits=hits, debug_events=events,
                   decode_unknowns=errors, phase=model.phase, cleanup_errors=[],
                  probe_hits=probe_hits, decode_codes=decode_codes,
                  scope_exclusions=excluded, excluded_codes=excluded_codes,
                  diagnostic_codes=diagnostic_codes,
                  cleanup=cleanup_result,
                  cleanup_verified=bool(cleanup_result.get('detached') and not threads.held),
                  outstanding=model.outstanding(time.monotonic() - started),
                   conclusion=getattr(observer, 'conclusion',
                       'observations only; native callback entry does not prove JS consumption')))
    finally:
        if attached:
            # Never fall back to the old one-pass restore/continue/detach path.
            if threads and not threads.draining:
                traps = {address: slot for slot, address in enumerate(addresses)}
                traps.update({address: slot for address, (slot, _) in retired.items()})
                quiesce_detach(cap, threads, continuation, traps, starts, consumed,
                               bp, breakin, emit)
            if output:
                output.write(json.dumps(dict(event='aborted', detached=continuation['detached'],
                    recovery_holds=threads.held if threads else {}, coverage_complete=False)) + '\n')
        if threads:
            for handle, _ in threads.saved.values():
                cap.close(handle)
        for sig, handler in old_signals.items():
            signal.signal(sig, handler)
        if output:
            output.close()
        cap.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--timeline-log', type=Path, help='Optional live host log: bounded read-only byte anchors, no content output')
    parser.add_argument('--trace-mode', choices=('gates', 'queue', 'delivery', 'executor'), default='gates')
    parser.add_argument('--user', type=int, help='Required launched Home user ID for queue/delivery/executor modes')
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--home-bundle', type=Path, required=True)
    parser.add_argument('--run-log', type=Path, required=True)
    parser.add_argument('--attach', action='store_true', help='Explicitly enable process access; otherwise offline only')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--created-filetime', type=int, help='Parent-observed process creation UTC FILETIME')
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--exe-sha256')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--max-hits', type=int, default=4000)
    parser.add_argument('--max-events', type=int, default=10000)
    parser.add_argument('--max-bytes', type=int, default=8388608)
    args = parser.parse_args()
    require(1 <= args.seconds <= 300 and 1 <= args.max_hits <= 20000
            and 1 <= args.max_events <= 50000 and 2097152 <= args.max_bytes <= 16777216, 'budget_arguments')
    images, bases, report = provenance(args.original, args.stage, args.home_bundle, args.run_log)
    if args.trace_mode == 'queue':
        require(args.user is not None and 0 <= args.user <= 0x7fffffff, 'queue_user_required')
        report['uilib_layout'] = layout_reference(args.original)
        abi_slots(images)
        for key, windows in QUEUE_WINDOWS.items():
            for rva, size in windows:
                images[key].at(rva, size)
    if args.trace_mode == 'delivery':
        require(args.user is not None and 0 <= args.user <= 0x7fffffff, 'delivery_user_required')
        delivery_abi_slots(images)
    if args.trace_mode == 'executor':
        require(args.user is not None and 0 <= args.user <= 0x7fffffff, 'executor_user_required')
        executor_abi_slots(images)
    if not args.attach:
        print(json.dumps(dict(offline_verified=True, images=report, process_access=False,
                             trace_mode=args.trace_mode), indent=2))
        return
    if not all((args.pid, args.created_filetime, args.exe, args.exe_sha256, args.output)):
        parser.error('--attach requires --pid --created-filetime --exe --exe-sha256 --output')
    require(os.name == 'nt' and C.sizeof(C.c_void_p) == 8, 'windows_amd64_required')
    capture(args, images, bases, report)


if __name__ == '__main__':
    main()
