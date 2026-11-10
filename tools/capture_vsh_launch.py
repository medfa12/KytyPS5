"""Bounded external VSH launch diagnostic (Windows AMD64 Python).

Uses verified, process-wide ONE-SHOT INT3s, not single stepping. Each site is
restored before its instruction executes; only RIP is rewound. Debug events
briefly stop the entire process and perturb timing. No guest calls, forced
launches, exception swallowing, or arbitrary first-exception capture.

Do not forcibly terminate this debugger. Ctrl+C/Break requests orderly cleanup.
--self-test inspects local files/layouts only; it NEVER opens a process.
See docs/vsh-launch-capture-20261104.md for scope and important limitations.
"""

import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import sys
import time

from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE, SIZE, U64, MemoryInfo
from capture_first_fault import (DebugEvent, BREAKPOINT, CONTINUE, NOT_HANDLED,
                                 memory, validate_layouts)


IMAGES = {
    "appsystem": ("Sce.Vsh.ShellUI.AppSystem.dll.sprx",
                  "298dd0c63867ee96c92977ac9643b188dd99b2f2f280a6b24f50491d78c63870"),
    "pui": ("Sce.PlayStation.PUI.dll.sprx",
            "458b2112e088c00ed200a6b06b1a41cb02fad8d9fb43b235f767746dbf4e56d7"),
}
# Anchors are whole audited instructions, in addition to file hashes and the
# 16-byte comparison with live code. No instruction decoder is used at runtime.
SITES = [
    ("appsystem", 0x5ddd, "queue_result", "488b0d34fb1500"),
    ("appsystem", 0x6f90, "worker_entry", "55"),
    ("appsystem", 0x7011, "history_return", "498b442410"),
    ("appsystem", 0x704a, "boot_return_first", "84c0"),
    ("appsystem", 0x7081, "prepare_before", "e8fe060900"),
    ("appsystem", 0x7086, "prepare_after", "498b07"),
    ("appsystem", 0x7115, "target_constructed", "488b0d24e31500"),
    ("appsystem", 0x7190, "killing_return", "84c0"),
    ("appsystem", 0x7264, "native_wrapper_before", "e8df050900"),
    ("appsystem", 0x7269, "native_wrapper_return", "be80000000"),
    ("appsystem", 0x72ca, "worker_epilogue", "498b4500"),
    ("appsystem", 0x7310, "finish_callback_entry", "55"),
    ("pui", 0xc5a1d, "pui_catch", "488945c8"),
]
REGISTERS = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip".split()
MAX_THREADS = 4096


def layouts():
    validate_layouts()
    assert C.sizeof(MemoryInfo) == 48 and MemoryInfo.size.offset == 24
    assert C.sizeof(SIZE) == C.sizeof(U64) == 8
    assert 120 + 16 * 8 == 248  # AMD64 CONTEXT.Rip
    assert struct.calcsize("<17Q") == 136


class Elf:
    def __init__(self, path, digest):
        self.path = str(path.resolve())
        if self.path.startswith("\\\\"):
            raise RuntimeError("Staged ELF must be a local file, not a network path")
        with path.open("rb") as source:
            self.data = source.read(64 * 1024 * 1024 + 1)
        if len(self.data) > 64 * 1024 * 1024:
            raise RuntimeError("ELF exceeds 64 MiB bound")
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        if self.sha256 != digest:
            raise RuntimeError(f"Unaudited staged ELF: {path}; SHA256={self.sha256}")
        if self.data[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", self.data, 18)[0] != 62:
            raise RuntimeError("Expected little-endian AMD64 ELF64")
        offset = struct.unpack_from("<Q", self.data, 32)[0]
        stride, count = struct.unpack_from("<HH", self.data, 54)
        if stride != 56 or not 1 <= count <= 128 or offset + stride * count > len(self.data):
            raise RuntimeError("Invalid ELF program headers")
        self.segments = [struct.unpack_from("<IIQQQQQQ", self.data, offset + i * stride)
                         for i in range(count)]

    def code(self, rva, size):
        segments = [s for s in self.segments if s[0] == 1 and s[1] & 1 and
                    s[3] <= rva and rva + size <= s[3] + s[5]]
        if len(segments) != 1:
            raise RuntimeError(f"RVA {rva:#x} is not uniquely file-backed executable code")
        segment = segments[0]
        offset = segment[2] + rva - segment[3]
        data = self.data[offset:offset + size]
        if len(data) != size:
            raise RuntimeError("Truncated ELF segment")
        return data


def local_sites(root, bases):
    images = {name: Elf(root / filename, digest) for name, (filename, digest) in IMAGES.items()}
    sites = {}
    for module, rva, label, anchor in SITES:
        expected = images[module].code(rva, 16)
        if not expected.startswith(bytes.fromhex(anchor)) or expected[0] == 0xcc:
            raise RuntimeError(f"Audited instruction mismatch: {module}+{rva:#x}")
        address = bases[module] + rva
        if address in sites:
            raise RuntimeError("Module base/site collision")
        sites[address] = dict(module=module, rva=rva, label=label, expected=expected,
                              armed=False, touched=False, hits=0)
    return images, sites


def restorable_window(address, site, current, sites):
    """Permit only our journaled INT3 bytes in overlapping validation windows."""
    if len(current) != len(site["expected"]):
        return False
    expected = bytearray(site["expected"])
    for other_address, other in sites.items():
        offset = other_address - address
        if other["touched"] and 0 <= offset < len(expected) and current[offset] == 0xcc:
            expected[offset] = 0xcc
    return current == bytes(expected)


class Context:
    def __init__(self, cap, tid):
        self.cap = cap
        self.backing = C.create_string_buffer(1247)
        self.pointer = (C.addressof(self.backing) + 15) & ~15
        # CONTROL only: SetThreadContext cannot rewrite integer/FPU/debug state.
        DWORD.from_address(self.pointer + 48).value = 0x100001
        self.thread = cap.open_thread(0x58, False, tid)  # GET/SET_CONTEXT + QUERY
        if not self.thread:
            raise C.WinError(C.get_last_error())
        try:
            owner = bind(cap.k, "GetProcessIdOfThread", DWORD, HANDLE)(self.thread)
            if owner != cap.pid:
                raise RuntimeError("Thread ownership changed")
            # Read FULL for evidence, then reduce flags to CONTROL for the set.
            DWORD.from_address(self.pointer + 48).value = 0x10000b
            if not cap.context(self.thread, self.pointer):
                raise C.WinError(C.get_last_error())
            self.raw = C.string_at(self.pointer, 1232)
            self.regs = dict(zip(REGISTERS, struct.unpack_from("<17Q", self.raw, 120)))
            self.regs["eflags"] = struct.unpack_from("<I", self.raw, 68)[0]
        except BaseException:
            cap.close(self.thread)
            raise

    def rewind(self, address):
        if self.regs["rip"] != address + 1:
            raise RuntimeError(f"Unexpected INT3 RIP {self.regs['rip']:#x} for {address:#x}")
        # All fetched control registers/flags remain byte-for-byte unchanged.
        U64.from_address(self.pointer + 248).value = address
        DWORD.from_address(self.pointer + 48).value = 0x100001
        if not bind(self.cap.k, "SetThreadContext", BOOL, HANDLE, HANDLE)(self.thread, self.pointer):
            raise C.WinError(C.get_last_error())

    def close(self):
        self.cap.close(self.thread)


class Observer:
    def __init__(self, cap, base):
        self.cap, self.base = cap, base
        self.job = self.closure = self.launcher = self.queue = self.tasks = 0

    def mem(self, address, size=128):
        if not 0 < address < (1 << 47) - size:
            return {"address": hex(address), "error": "null/non-user address"}
        return memory(self.cap, address, size)

    def number(self, address, size=8):
        data = bytes.fromhex(self.mem(address, size).get("hex", ""))
        return int.from_bytes(data, "little") if len(data) == size else None

    def obj(self, address, size=128):
        result = self.mem(address, size)
        first = bytes.fromhex(result.get("hex", ""))
        if len(first) >= 8:
            vtable = int.from_bytes(first[:8], "little")
            result["first_qword_memory_untyped"] = self.mem(vtable, 64)
        return result

    def pointer_fields(self, address, start=0x10, count=8):
        # Raw bounded graph for later offline layout/type/message audit. These
        # are NOT asserted to be managed references, handles or valid strings.
        fields = []
        for offset in range(start, start + 8 * count, 8):
            pointer = self.number(address + offset) if address else None
            fields.append({"offset": hex(offset), "value": hex(pointer) if pointer is not None else None,
                           "pointee_untyped": self.mem(pointer or 0, 128)})
        return fields

    def job_state(self):
        if not self.job:
            return {"error": "no correlated job retained"}
        result = {"pointer": hex(self.job), "raw": self.obj(self.job, 0x80)}
        for name, offset, size in (("exception", 0x20, 8), ("result", 0x28, 8),
                                   ("status", 0x30, 4), ("catch_hresult", 0x34, 4),
                                   ("cancel_byte", 0x38, 1)):
            value = self.number(self.job + offset, size)
            result[name] = hex(value) if value is not None else None
            if name in ("exception", "result") and value:
                result[name + "_object"] = self.obj(value, 0x100)
                result[name + "_pointer_fields_untyped"] = self.pointer_fields(value, count=12)
        return result

    def array(self, address):
        # MonoArray: object[16], bounds pointer[8], native length[8], data.
        result = {"raw": self.mem(address, 32), "layout": "MonoArray x64 reference array"}
        length = self.number(address + 24) if address else None
        result["declared_length"] = length
        if length is not None and length <= 65536:
            result["truncated"] = length > 16
            result["entries"] = []
            for index in range(min(length, 16)):
                pointer = self.number(address + 32 + 8 * index)
                result["entries"].append({"index": index, "pointer": hex(pointer or 0),
                                          "task_untyped": self.obj(pointer or 0, 0x100)})
        return result

    def state(self):
        slot = self.base + 0x165918
        storage = self.number(slot)
        result = {"job": self.job_state(), "static_job_got": hex(slot),
                  "static_job_storage": hex(storage or 0),
                  "static_job_now": hex(self.number(storage) or 0) if storage else None,
                  "closure": self.obj(self.closure, 0x40),
                  "launcher": self.obj(self.launcher, 0x80),
                  "queue_untyped": self.obj(self.queue, 0x100),
                  "queue_pointer_fields_untyped": self.pointer_fields(self.queue)}
        prepare = self.number(self.launcher + 0x30) if self.launcher else None
        if prepare:
            result["prepare_list_untyped"] = self.obj(prepare, 0x40)
        if self.tasks:
            result["prepare_tasks"] = self.array(self.tasks)
        return result

    def observe(self, site, regs, tid):
        label = site["label"]
        evidence = {"label": label, "tid": tid, "unix_ns": time.time_ns(),
                    "registers": {k: hex(v) for k, v in regs.items()},
                    "stack": self.mem(regs["rsp"], 1024)}
        if label in ("queue_result", "worker_entry"):
            job = regs["rax"] if label == "queue_result" else regs["rsi"]
            closure = regs["r12"] if label == "queue_result" else regs["rdi"]
            if not job or (self.job and self.job != job) or (self.closure and self.closure != closure):
                evidence["correlated"] = False
                return evidence
            self.job, self.closure = job, closure
            self.launcher = self.number(closure + 0x10) or 0
            if label == "queue_result":
                self.queue = regs["r15"]  # callee-saved queue, not volatile RDI
        elif label == "pui_catch":
            job = self.number(regs["rbp"] - 0x18)
            if not self.job or job != self.job:
                evidence.update(correlated=False, ignored="catch belongs to another/unknown job",
                                observed_job=hex(job or 0))
                return evidence
            evidence["caught_exception_before_store"] = self.obj(regs["rax"], 0x100)
            evidence["exception_pointer_fields_untyped"] = self.pointer_fields(regs["rax"], count=12)
        elif label == "finish_callback_entry":
            if not self.job or regs["rsi"] != self.job or regs["rdi"] != self.closure:
                evidence["correlated"] = False
                return evidence
        elif not self.closure or regs["r12"] != self.closure:
            evidence["correlated"] = False
            return evidence
        evidence["correlated"] = True
        if label == "boot_return_first":
            evidence["boot_poll_al"] = regs["rax"] & 255
            evidence["requested_bits"] = "0x08000000"
        if label == "prepare_before":
            self.tasks = regs["rdi"]  # ToArray return passed to Task.WaitAll
        if label in ("target_constructed", "killing_return"):
            target = regs["rbx"] if label == "target_constructed" else self.number(regs["r14"] + 0x10)
            evidence["target_info"] = self.obj(target or 0, 0x80)
            if label == "killing_return":
                evidence["killing_al"] = regs["rax"] & 255
        if label == "native_wrapper_before":
            evidence["arguments"] = {"rdi_target": self.obj(regs["rdi"]),
                                     "rsi_launch_info": self.obj(regs["rsi"]),
                                     "edx": hex(regs["rdx"] & 0xffffffff),
                                     "rcx_stack_options": self.mem(regs["rcx"], 0x30)}
        if label == "native_wrapper_return":
            evidence["wrapper_return_rax_untyped"] = hex(regs["rax"])
        if label == "worker_epilogue":
            evidence["reason"] = "common epilogue; infer only from prior milestones, not R15 (reused)"
        evidence["retained"] = self.state()
        return evidence


class Debugger:
    def __init__(self, cap, sites, observer, result):
        self.cap, self.sites, self.observer, self.result = cap, sites, observer, result
        self.pending = None
        self.pending_handled = False
        self.last_evidence = None
        self.status = CONTINUE
        self.attached = self.exited = False
        self.breakin_threads = set()
        self.breakin_waiting = set()  # CREATE observed, DbgBreakPoint not yet consumed
        self.attach_break_pending = False
        self.cleanup_break_pending = False  # survives acquisition timeouts/retries
        self.held = []
        self.wait = bind(cap.k, "WaitForDebugEvent", BOOL, C.POINTER(DebugEvent), DWORD)
        self.continue_ = bind(cap.k, "ContinueDebugEvent", BOOL, DWORD, DWORD, DWORD)
        self.write = bind(cap.k, "WriteProcessMemory", BOOL, HANDLE, HANDLE, HANDLE, SIZE, C.POINTER(SIZE))
        self.flush = bind(cap.k, "FlushInstructionCache", BOOL, HANDLE, HANDLE, SIZE)
        self.break_ = bind(cap.k, "DebugBreakProcess", BOOL, HANDLE)
        local = bind(cap.k, "GetModuleHandleW", HANDLE, C.c_wchar_p)("ntdll.dll")
        proc = bind(cap.k, "GetProcAddress", HANDLE, HANDLE, C.c_char_p)
        remote = next(base for base, size, name in cap.modules() if Path(name).name.lower() == "ntdll.dll")
        bp, breakin = proc(local, b"DbgBreakPoint"), proc(local, b"DbgUiRemoteBreakin")
        if not local or not bp or not breakin:
            raise RuntimeError("Cannot identify debugger break-in entrypoints")
        self.bp, self.breakin = remote + bp - local, remote + breakin - local

    def patch(self, address, data):
        count, source = SIZE(), C.create_string_buffer(data)
        # Windows' debugger WriteProcessMemory handles executable page writes;
        # no VirtualProtectEx changes, and no deliberately persistent RWX pages.
        if not self.write(self.cap.process, address, source, len(data), C.byref(count)) or count.value != len(data):
            raise RuntimeError(f"WriteProcessMemory {address:#x}: {count.value}/{len(data)}, error={C.get_last_error()}")
        if not self.flush(self.cap.process, address, len(data)):
            raise C.WinError(C.get_last_error())
        if self.cap.read_exact(address, len(data)) != data:
            raise RuntimeError("Code write verification failed")

    def arm(self):
        # Validate ALL sites before the first patch; fail closed on relocation,
        # version, protection, or wrong-base differences. Never normalize bytes.
        for address, site in self.sites.items():
            info = MemoryInfo()
            if not self.cap.query(self.cap.process, address, C.byref(info), C.sizeof(info)) or (
                    info.state != 0x1000 or info.protect & 0x101 or
                    info.protect & 0xff not in (0x10, 0x20, 0x40, 0x80) or
                    address + 16 > (info.base or 0) + info.size):
                raise RuntimeError(f"Not committed executable code: {address:#x}")
            if self.cap.read_exact(address, 16) != site["expected"]:
                raise RuntimeError(f"Exact staged ELF/live mismatch: {site['label']} at {address:#x}")
        for address, site in self.sites.items():
            site["touched"] = True  # journal BEFORE a possibly partial write
            self.patch(address, b"\xcc")
            site["armed"] = True
        self.result["armed_unix_ns"] = time.time_ns()

    def restore(self, address, site):
        if site["touched"]:
            current = self.cap.read_exact(address, 16)
            if not restorable_window(address, site, current, self.sites):
                raise RuntimeError(f"Refusing to overwrite third-party code change at {address:#x}")
            # Always flush even after an earlier write succeeded but flush failed.
            self.patch(address, site["expected"][:1])
            site["armed"] = False

    def receive(self, milliseconds):
        event = DebugEvent()
        if not self.wait(C.byref(event), milliseconds):
            error = C.get_last_error()
            if error == 121:
                return False
            raise C.WinError(error)
        self.pending = event
        self.pending_handled = False
        self.event_received = time.monotonic()
        self.last_evidence = None
        self.status = NOT_HANDLED if event.code == 1 else CONTINUE
        if event.code == 3 and event.data.process.file:
            self.cap.close(event.data.process.file)
        if event.code == 6 and event.data.dll.file:
            self.cap.close(event.data.dll.file)
        if event.code == 2 and event.data.thread.start == self.breakin:
            self.breakin_threads.add(event.tid)
            self.breakin_waiting.add(event.tid)
        if event.code == 4:
            self.breakin_threads.discard(event.tid)
            self.breakin_waiting.discard(event.tid)
        if event.code == 5:
            self.exited = True
            self.result.update(status="target_exited", exit_code=event.data.exit_code)
        return True

    def is_breakin_breakpoint(self):
        event = self.pending
        return bool(event is not None and event.code == 1 and
                    event.data.exception.first and event.data.exception.record.code == BREAKPOINT and
                    event.data.exception.record.address == self.bp and event.tid in self.breakin_threads)

    def breakin_inflight(self):
        return bool(self.attach_break_pending or self.cleanup_break_pending or self.breakin_waiting)

    def handle(self, collect=True):
        event = self.pending
        if self.pending_handled:
            return False
        if event.code != 1:
            return False
        exception = event.data.exception
        address = exception.record.address or 0
        if exception.first and exception.record.code == BREAKPOINT:
            if self.is_breakin_breakpoint():
                self.status = CONTINUE
                self.pending_handled = True
                self.breakin_waiting.discard(event.tid)
                self.attach_break_pending = False
                self.cleanup_break_pending = False
                return True
            site = self.sites.get(address)
            # Keep retired sites in the table: multiple threads may have already
            # executed the same INT3 before Windows delivered the first event.
            if site and site["touched"]:
                context = Context(self.cap, event.tid)
                try:
                    self.restore(address, site)
                    context.rewind(address)
                    self.status = CONTINUE  # only AFTER restoration and rewind
                    self.pending_handled = True
                    site["hits"] += 1
                    if collect and site["hits"] <= 2:
                        self.last_evidence = self.observer.observe(site, context.regs, event.tid)
                        self.result["events"].append(self.last_evidence)
                    elif collect:
                        self.result["simultaneous_hit_snapshots_omitted"] = self.result.get("simultaneous_hit_snapshots_omitted", 0) + 1
                finally:
                    context.close()
                return False
        self.result["forwarded_exceptions"] += 1
        self.pending_handled = True
        # No arbitrary first-chance exception is treated as the launch fault.
        return False

    def proceed(self):
        event = self.pending
        if self.last_evidence is not None:
            self.last_evidence["debug_stop_handler_ms"] = round((time.monotonic() - self.event_received) * 1000, 3)
        if not self.continue_(event.pid, event.tid, self.status):
            raise C.WinError(C.get_last_error())
        self.pending = None
        self.pending_handled = False

    def hold_all(self):
        # Called ONLY while a debug event already stops every target thread.
        tids = self.cap.threads()
        if len(tids) > MAX_THREADS:
            raise RuntimeError("Cleanup thread limit exceeded")
        for tid in tids:
            if any(held_tid == tid for _, held_tid, _ in self.held):
                continue
            thread = self.cap.open_thread(0x42, False, tid)  # suspend + query
            if not thread:
                raise C.WinError(C.get_last_error())
            try:
                if bind(self.cap.k, "GetProcessIdOfThread", DWORD, HANDLE)(thread) != self.cap.pid:
                    raise RuntimeError("Thread ownership changed during cleanup")
                previous = self.cap.suspend(thread)
                if previous == 0xffffffff:
                    raise C.WinError(C.get_last_error())
                self.held.append((thread, tid, previous))
                thread = None
            finally:
                if thread:
                    self.cap.close(thread)

    def release_holds(self):
        remaining = []
        for thread, tid, previous in reversed(self.held):
            resumed = self.cap.resume(thread)
            if resumed == 0xffffffff and not self.exited:
                exit_code = DWORD()
                get_exit = bind(self.cap.k, "GetExitCodeThread", BOOL, HANDLE, C.POINTER(DWORD))
                if not get_exit(thread, C.byref(exit_code)) or exit_code.value == 259:
                    errors = self.result["cleanup_errors"]
                    errors.append(f"ResumeThread tid={tid} failed; retaining handle for retry")
                    del errors[:-64]
                    remaining.append((thread, tid, previous))
                    continue
            self.cap.close(thread)
        self.held = remaining

    def acquire_cleanup_stop(self):
        """Do not suspend a remote break-in before it executes DbgBreakPoint.

        A successful DebugBreakProcess request remains outstanding across errors
        and timeouts, including before its CREATE_THREAD is delivered. Only an
        authenticated break-in breakpoint clears it; never issue a second request
        merely because receive() returned another event or timed out.
        """
        if self.pending is None and not self.breakin_inflight():
            # Journal before the API call: even an ambiguous API failure must
            # not cause retries to enqueue additional remote break-in threads.
            self.cleanup_break_pending = True
            if not self.break_(self.cap.process):
                raise C.WinError(C.get_last_error())
        until = time.monotonic() + 5
        while not self.exited:
            if self.pending is not None:
                self.handle(collect=False)
                # A pending non-break-in event is also a valid stop ONLY when
                # there is no outstanding attach/request/created break-in. In
                # particular CREATE_THREAD must be continued before hold_all.
                if not self.breakin_inflight():
                    return
                self.proceed()
            if time.monotonic() >= until:
                raise RuntimeError("No authenticated cleanup break-in stop within 5 seconds; request retained")
            self.receive(100)

    def cleanup(self):
        """Quiesce, restore, drain queued traps, detach, release OUR suspends."""
        errors = self.result.setdefault("cleanup_errors", [])
        if not self.attached:
            self.release_holds()
            return not self.held
        try:
            if not self.exited:
                self.acquire_cleanup_stop()
            if not self.exited:
                self.hold_all()
                # Current owned trap must be rewound too, including error paths.
                recovery_errors = []
                if self.pending:
                    try:
                        self.handle(collect=False)
                    except Exception as error:
                        recovery_errors.append(str(error))
                try:
                    self.result["final_retained"] = self.observer.state()
                except Exception as error:
                    self.result["final_snapshot_error"] = str(error)
                for address, site in self.sites.items():
                    try:
                        self.restore(address, site)
                    except Exception as error:
                        recovery_errors.append(str(error))
                if recovery_errors:
                    raise RuntimeError("; ".join(recovery_errors))
            if self.pending:
                self.proceed()
            # All threads now have an additional suspension increment. Drain
            # exceptions ALREADY issued, without allowing fresh target execution.
            # In particular never infer a trap from RIP==site+1 (push is 1 byte).
            drained = 0
            until = time.monotonic() + 5
            while not self.exited and self.receive(100):
                self.handle(collect=False)
                if self.breakin_inflight():
                    # A queued CREATE can reveal a break-in missed by the
                    # quiescence enumeration. Keep this event pending, release
                    # our suspends in finally, and let the SAME thread reach its
                    # authenticated breakpoint on retry. Never detach it here.
                    raise RuntimeError("Queued break-in still in flight; retry acquisition without another request")
                self.proceed()
                drained += 1
                if drained > MAX_THREADS * 4 or time.monotonic() >= until:
                    raise RuntimeError("Cleanup event-drain bound exceeded")
            self.result["cleanup_drained_events"] = drained
            if not self.exited:
                if self.breakin_inflight():
                    raise RuntimeError("Refusing detach with an outstanding break-in")
                if not bind(self.cap.k, "DebugActiveProcessStop", BOOL, DWORD)(self.cap.pid):
                    raise C.WinError(C.get_last_error())
                self.result["detached"] = True
                self.attached = False
        except Exception as error:
            # Do not detach over a live INT3 or an unprocessed owned trap. A
            # failed cleanup is conspicuous, not a claimed safe completion.
            errors.append(str(error))
        finally:
            self.release_holds()
        del errors[:-64]
        if (self.attached and not self.exited) or self.held:
            # Windows detach on debugger exit is unsafe while patches remain.
            # Keep ownership and retry instead of knowingly abandoning INT3s.
            print("CLEANUP FAILED; retaining debugger ownership and retrying. DO NOT KILL. " +
                  "; ".join(errors[-3:]), file=sys.stderr, flush=True)
            return False
        return True


def capture(args, images, sites):
    result = {"tool": "capture_vsh_launch.py", "pid": args.pid, "seconds": args.seconds,
              "bases": {k: hex(v) for k, v in args.bases.items()},
              "images": {k: {"path": v.path, "sha256": v.sha256} for k, v in images.items()},
              "status": "timeout", "events": [], "forwarded_exceptions": 0,
              "limitations": ["one-shot process-wide sites; unrelated hit can consume a site",
                              "first boot poll only; no final poll or native flag-handle decoding",
                              "brief stop-the-world debugger timing perturbation",
                              "retained addresses are NOT GC roots; movement/collection/reuse possible",
                              "raw task/exception layouts not semantic type/message/status decoding",
                              "wrapper entry/return is not proof of native HLE dispatch"]}
    started = time.monotonic()
    interrupted = False
    old_signals = {}
    cap = debugger = None

    def interrupt(signum, frame):
        nonlocal interrupted
        interrupted = True

    try:
        for sig in (signal.SIGINT, signal.SIGBREAK):
            old_signals[sig] = signal.signal(sig, interrupt)
        cap = Capture(args.pid, "", False)
        # Capture's original read/query handle is deliberately replaced with
        # an explicit VM write/operation handle, used solely for audited INT3s.
        # CREATE_THREAD is required by DebugBreakProcess's debugger break-in;
        # it is never used to execute a guest method or custom injected code.
        process = bind(cap.k, "OpenProcess", HANDLE, DWORD, BOOL, DWORD)(0x43a, False, args.pid)
        if not process:
            raise C.WinError(C.get_last_error())
        cap.close(cap.process)
        cap.process = process
        cap.check_architecture()
        observer = Observer(cap, args.bases["appsystem"])
        debugger = Debugger(cap, sites, observer, result)
        if not bind(cap.k, "DebugActiveProcess", BOOL, DWORD)(args.pid):
            raise C.WinError(C.get_last_error())
        debugger.attached = True
        debugger.attach_break_pending = True
        if not bind(cap.k, "DebugSetProcessKillOnExit", BOOL, BOOL)(False):
            raise C.WinError(C.get_last_error())
        deadline = time.monotonic() + args.seconds
        armed = False
        while time.monotonic() < deadline and not interrupted:
            if not debugger.receive(100):
                continue
            attach_break = debugger.handle()
            announce = False
            if attach_break and not armed:
                debugger.arm()
                armed = True
                announce = True
            debugger.proceed()
            if announce:
                print("ARMED: make exactly one ordinary VSH selection now; do not kill this debugger.", flush=True)
            if debugger.exited:
                break
        if not armed and not debugger.exited:
            result["status"] = "not_armed"
        if interrupted:
            result["status"] = "interrupted"
    except Exception as error:
        result.update(status="error", error=str(error))
    finally:
        if debugger:
            while debugger.cleanup() is False:
                # Fail-safe recovery, NOT a normal capture extension. Never
                # knowingly abandon live patches to satisfy a wall-clock cap.
                # A broken OS/debugger contract cannot have both guarantees.
                time.sleep(0.25)
        if cap:
            cap.finish()
        for sig, handler in old_signals.items():
            signal.signal(sig, handler)
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        result["sites"] = [{"address": hex(a), **{k: v for k, v in s.items() if k != "expected"},
                            "expected_hex": s["expected"].hex()} for a, s in sites.items()]
    return result


def test_cleanup_event_order():
    """Real receive/handle/cleanup control flow; fake Win32 calls, no target."""
    import contextlib
    import io
    from types import SimpleNamespace
    from unittest.mock import patch

    def event(code, tid, address=0):
        item = DebugEvent()
        item.code, item.pid, item.tid = code, 999, tid
        if code == 2:
            item.data.thread.start = 0x2000
        elif code == 1:
            item.data.exception.first = 1
            item.data.exception.record.address = address
            item.data.exception.record.code = BREAKPOINT if address == 0x1000 else 0xc0000005
        return item

    def fixture(events, attach_pending=False, request_failure=False):
        log, clock = [], {"expired": False}
        debug = Debugger.__new__(Debugger)

        def detach(pid):
            assert not debug.breakin_inflight() and debug.pending is None
            log.append(("detach",))
            return True

        debug.cap = SimpleNamespace(pid=999, process=1, close=lambda h: None,
                                    k=SimpleNamespace(DebugActiveProcessStop=detach))
        debug.pending, debug.pending_handled, debug.last_evidence = None, False, None
        debug.attached, debug.exited, debug.status = True, False, CONTINUE
        debug.bp, debug.breakin = 0x1000, 0x2000
        debug.breakin_threads, debug.breakin_waiting, debug.held = set(), set(), []
        debug.attach_break_pending, debug.cleanup_break_pending = attach_pending, False
        debug.result = {"events": [], "forwarded_exceptions": 0}
        debug.sites = {0x1234: {"touched": True}}
        debug.observer = SimpleNamespace(state=lambda: {"mock": True})
        debug.restore = lambda address, site: log.append(("restore", address))

        def break_(process):
            assert debug.cleanup_break_pending and not debug.attach_break_pending and not debug.breakin_waiting
            log.append(("request",))
            if request_failure:
                raise RuntimeError("injected ambiguous break request failure")
            return True

        def wait(pointer, milliseconds):
            item = events.pop(0) if events else "empty"
            if isinstance(item, str):
                if item == "deadline":
                    clock["expired"] = True
                C.set_last_error(121)
                return False
            log.append(("receive", item.code, item.tid))
            C.memmove(pointer, C.byref(item), C.sizeof(DebugEvent))
            return True

        def continue_(pid, tid, status):
            log.append(("continue", debug.pending.code, tid, status))
            return True

        def hold_all():
            # This assertion fails under the old FIRST-event cleanup: it tried
            # to suspend at CREATE_THREAD, before the remote INT3 could execute.
            assert debug.is_breakin_breakpoint() and not debug.breakin_inflight()
            log.append(("hold", debug.pending.tid))
            debug.held = [(1, debug.pending.tid, 0)]

        def release_holds():
            if debug.held:
                log.append(("release",))
                debug.held.clear()

        debug.break_, debug.wait, debug.continue_ = break_, wait, continue_
        debug.hold_all, debug.release_holds = hold_all, release_holds
        return debug, log, clock

    # Normal ordering: CREATE must be continued; an unrelated AV is forwarded;
    # only then does the authenticated breakpoint permit hold/restore/detach.
    debug, log, _ = fixture([event(2, 900), event(1, 800, 0x7777),
                             event(1, 800, 0x1000), event(1, 900, 0x1000)])
    assert debug.cleanup() is True
    assert log.count(("request",)) == 1 and log.count(("detach",)) == 1
    assert log.index(("continue", 2, 900, CONTINUE)) < log.index(("receive", 1, 900)) < log.index(("hold", 900))
    assert log.index(("restore", 0x1234)) < log.index(("continue", 1, 900, CONTINUE)) < log.index(("detach",))
    assert log.count(("continue", 1, 800, NOT_HANDLED)) == 2 and debug.result["forwarded_exceptions"] == 2

    # Multiple CREATED break-ins must ALL pass their authenticated breakpoint
    # before quiescence, even after the request flag clears at the first hit.
    debug, log, _ = fixture([event(2, 900), event(2, 901),
                             event(1, 900, 0x1000), event(1, 901, 0x1000)])
    assert debug.cleanup() is True
    assert ("hold", 900) not in log and ("hold", 901) in log
    assert log.index(("continue", 1, 900, CONTINUE)) < log.index(("hold", 901))

    # A timeout AFTER CREATE retains the existing request/thread. Retrying must
    # wait for its breakpoint, not call DebugBreakProcess a second time.
    debug, log, clock = fixture([event(2, 900), "deadline", event(1, 900, 0x1000)])
    with patch.object(time, "monotonic", lambda: 100.0 + (6 if clock["expired"] else 0)):
        with contextlib.redirect_stderr(io.StringIO()):
            assert debug.cleanup() is False
        assert debug.cleanup_break_pending and debug.breakin_waiting == {900}
        assert ("hold", 900) not in log and ("detach",) not in log
        assert debug.cleanup() is True
    assert log.count(("request",)) == 1 and log.count(("detach",)) == 1

    # Journal the request before its API call, even if that call fails before
    # CREATE delivery: an ambiguous failure is not permission to inject again.
    debug, log, _ = fixture([event(2, 900), event(1, 900, 0x1000)], request_failure=True)
    with contextlib.redirect_stderr(io.StringIO()):
        assert debug.cleanup() is False
    assert debug.cleanup_break_pending and not debug.breakin_waiting
    assert debug.cleanup() is True and log.count(("request",)) == 1

    # Cleanup before attach's initial breakpoint must reuse that in-flight
    # break-in as well, including when its CREATE has not yet been delivered.
    debug, log, _ = fixture([event(2, 900), event(1, 900, 0x1000)], attach_pending=True)
    assert debug.cleanup() is True and ("request",) not in log

    # A queued CREATE revealed during the quiescent drain invalidates detach.
    # Release holds, continue that CREATE on retry, and wait for THAT thread's
    # breakpoint, without requesting yet another break-in.
    debug, log, _ = fixture([event(2, 900), event(1, 900, 0x1000),
                             event(2, 901), event(1, 901, 0x1000)])
    with contextlib.redirect_stderr(io.StringIO()):
        assert debug.cleanup() is False
    assert debug.pending.code == 2 and debug.pending.tid == 901
    assert debug.breakin_waiting == {901} and ("detach",) not in log and not debug.held
    assert debug.cleanup() is True
    assert log.count(("request",)) == 1
    assert log.index(("continue", 2, 901, CONTINUE)) < log.index(("hold", 901)) < log.index(("detach",))


def self_test(root):
    layouts()
    images, sites = local_sites(root, {"appsystem": 0xb50000000, "pui": 0xb60000000})
    # Static context mutation guard: the simulated rewind changes only Rip and
    # the requested ContextFlags, never EFlags or any register value.
    raw = bytearray((i % 256 for i in range(1232)))
    changed = bytearray(raw)
    struct.pack_into("<I", changed, 48, 0x100001)
    struct.pack_into("<Q", changed, 248, 0x12345678)
    assert all(raw[i] == changed[i] for i in range(1232) if not (48 <= i < 52 or 248 <= i < 256))
    # Retired-site ownership survives the first hit (simultaneous trap guard).
    site = next(iter(sites.values()))
    site.update(touched=True, armed=False)
    assert site["touched"] and not site["armed"]
    # Overlapping 7081/7086 and 7264/7269 windows must tolerate ONLY our bytes.
    for address, item in sites.items():
        item["touched"] = True
    for address, item in sites.items():
        window = bytearray(item["expected"])
        for other_address in sites:
            if 0 <= other_address - address < len(window):
                window[other_address - address] = 0xcc
        assert restorable_window(address, item, bytes(window), sites)
        window[-1] ^= 1
        assert not restorable_window(address, item, bytes(window), sites)

    # Exercise the REAL rewind and event handler against in-memory ctypes
    # buffers/fake APIs. No Capture, WinDLL, OpenProcess or debugger API is used.
    from types import SimpleNamespace
    context_class = Context
    calls, contexts = [], []

    def set_context(thread, pointer):
        calls.append(thread)
        return True

    fake_cap = SimpleNamespace(k=SimpleNamespace(SetThreadContext=set_context), close=lambda h: None)
    address, site = next(iter(sites.items()))

    def context_factory(cap, tid):
        context = context_class.__new__(context_class)
        context.cap, context.thread = cap, tid
        context.backing = C.create_string_buffer(1247)
        context.pointer = (C.addressof(context.backing) + 15) & ~15
        original = bytearray(raw)
        struct.pack_into("<Q", original, 248, address + 1)
        C.memmove(context.pointer, bytes(original), 1232)
        context.regs = {"rip": address + 1}
        contexts.append((context, bytes(original)))
        return context

    debug = Debugger.__new__(Debugger)
    debug.cap, debug.sites, debug.bp = fake_cap, sites, 0
    debug.breakin_threads = set()
    debug.breakin_waiting = set()
    debug.attach_break_pending = debug.cleanup_break_pending = False
    debug.result = {"events": [], "forwarded_exceptions": 0}
    restores = []
    debug.restore = lambda a, s: (restores.append(a), s.update(armed=False))
    try:
        globals()["Context"] = context_factory
        for tid in (101, 102):
            event = DebugEvent()
            event.code, event.tid = 1, tid
            event.data.exception.first = 1
            event.data.exception.record.code = BREAKPOINT
            event.data.exception.record.address = address
            debug.pending, debug.pending_handled, debug.status = event, False, NOT_HANDLED
            debug.handle(collect=False)
            debug.handle(collect=False)  # cleanup retry must NOT rewind twice
            assert debug.status == CONTINUE and debug.pending_handled
        assert site["hits"] == 2 and len(restores) == len(calls) == 2
        for context, original in contexts:
            actual = C.string_at(context.pointer, 1232)
            assert struct.unpack_from("<Q", actual, 248)[0] == address
            assert all(original[i] == actual[i] for i in range(1232)
                       if not (48 <= i < 52 or 248 <= i < 256))
        event.data.exception.record.code = 0xc0000005
        debug.pending_handled, debug.status = False, NOT_HANDLED
        debug.handle(collect=False)
        assert debug.status == NOT_HANDLED and debug.result["forwarded_exceptions"] == 1
    finally:
        globals()["Context"] = context_class
    test_cleanup_event_order()
    print(f"LOCAL ONLY: layouts, {len(images)} pinned ELFs, {len(sites)} anchors, overlapping windows, "
          "real context rewind, simultaneous retired traps, exception forwarding, CREATE-before-breakpoint, "
          "acquisition-timeout/API-failure retry, attach reuse and queued-break-in drain guards passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--appsystem-base", type=lambda s: int(s, 0))
    parser.add_argument("--pui-base", type=lambda s: int(s, 0))
    parser.add_argument("--elf-dir", type=Path, default=Path(__file__).resolve().parents[1] / "_ShellUIManaged/common")
    parser.add_argument("--seconds", type=float, default=70)
    parser.add_argument("--self-test", action="store_true", help="Local files/layouts only; no process APIs")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.elf_dir)
        return 0
    if sys.platform != "win32" or C.sizeof(HANDLE) != 8:
        parser.error("Native Windows AMD64 Python required")
    if not args.pid or args.pid <= 0 or args.pid in (os.getpid(), 23664):
        parser.error("Explicit positive VSH PID required; PID23664 (active Apple TV) is prohibited")
    args.bases = {"appsystem": args.appsystem_base, "pui": args.pui_base}
    if any(v is None or not 0 < v < (1 << 47) - 0x2000000 or v % 4096 for v in args.bases.values()):
        parser.error("Both explicit, page-aligned guest module bases are required")
    if args.appsystem_base == args.pui_base or not 0 < args.seconds <= 70:
        parser.error("Distinct bases and duration in (0,70] seconds required")
    layouts()
    images, sites = local_sites(args.elf_dir, args.bases)
    root = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
    root.mkdir(parents=True, exist_ok=True)
    output = root / f"vsh-launch-{args.pid}-{time.time_ns()}.json"
    # Reserve output BEFORE attaching; no file I/O while a debug stop is held.
    with output.open("x", encoding="utf-8") as file:
        result = capture(args, images, sites)
        json.dump(result, file, indent=2)
    print(output)
    print(result["status"])
    return 0 if result.get("detached") and result.get("armed_unix_ns") and not result.get("cleanup_errors") and result["status"] == "timeout" else 1


if __name__ == "__main__":
    sys.exit(main())
