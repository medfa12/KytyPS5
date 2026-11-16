"""Read-only external AMD64 first-chance exception capture (Windows, 64-bit Python).

Example: python tools/capture_first_fault.py --pid 1234 --address 0x95022fc36
AV is the default; --exception-code can select a host C++ exception instead.
All application exceptions are forwarded, including the captured exception. Only the
identified debugger attach breakpoint is consumed. No target writes, injected
breakpoints, context changes, or explicit thread suspension are performed.
Attaching itself uses Windows' debugger break-in thread and pauses the process.
Outputs stay under %LOCALAPPDATA%/Temp/opencode. MiniDumpNormal is optional;
it omits full memory and uses cooperative time/size cancellation, not a hard
OS I/O deadline. Never forcibly terminate this script while attached.
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

from capture_native_hang import Capture, bind, BOOL, DWORD, HANDLE, SIZE, U64, MemoryInfo


CONTINUE = 0x00010002
NOT_HANDLED = 0x80010001
AV = 0xC0000005
BREAKPOINT = 0x80000003


class ExceptionRecord(C.Structure):
    _fields_ = [("code", DWORD), ("flags", DWORD), ("record", HANDLE),
                ("address", HANDLE), ("count", DWORD), ("information", U64 * 15)]


class ExceptionInfo(C.Structure):
    _fields_ = [("record", ExceptionRecord), ("first", DWORD)]


class CreateThread(C.Structure):
    _fields_ = [("thread", HANDLE), ("tls", HANDLE), ("start", HANDLE)]


class CreateProcess(C.Structure):
    _fields_ = [("file", HANDLE), ("process", HANDLE), ("thread", HANDLE),
                ("base", HANDLE), ("debug_offset", DWORD), ("debug_size", DWORD),
                ("tls", HANDLE), ("start", HANDLE), ("image", HANDLE), ("unicode", C.c_uint16)]


class LoadDll(C.Structure):
    _fields_ = [("file", HANDLE), ("base", HANDLE), ("debug_offset", DWORD),
                 ("debug_size", DWORD), ("image", HANDLE), ("unicode", C.c_uint16)]


class UnloadDll(C.Structure):
    _fields_ = [("base", HANDLE)]


class EventUnion(C.Union):
    _fields_ = [("exception", ExceptionInfo), ("thread", CreateThread),
                 ("process", CreateProcess), ("dll", LoadDll), ("unload", UnloadDll), ("exit_code", DWORD)]


class DebugEvent(C.Structure):
    _fields_ = [("code", DWORD), ("pid", DWORD), ("tid", DWORD), ("data", EventUnion)]


class ExceptionPointers(C.Structure):
    _fields_ = [("record", C.POINTER(ExceptionRecord)), ("context", HANDLE)]


class DumpException(C.Structure):
    _pack_ = 4  # minidumpapiset.h uses pack(4), including pointer fields
    _fields_ = [("tid", DWORD), ("pointers", C.POINTER(ExceptionPointers)), ("client", BOOL)]


def validate_layouts():
    assert C.sizeof(HANDLE) == 8, "64-bit Python required"
    assert C.sizeof(ExceptionRecord) == 152
    assert ExceptionRecord.information.offset == 32
    assert C.sizeof(ExceptionInfo) == 160
    assert C.sizeof(CreateProcess) == 72
    assert C.sizeof(CreateThread) == 24 and C.sizeof(LoadDll) == 40
    assert C.sizeof(UnloadDll) == 8 and UnloadDll.base.offset == 0
    assert DebugEvent.data.offset == 16 and C.sizeof(DebugEvent) == 176
    assert C.sizeof(DumpException) == 16 and DumpException.pointers.offset == 4


def memory(cap, address, length):
    result = {"address": hex(address), "requested": length}
    info, count = MemoryInfo(), SIZE()
    if not address or not cap.query(cap.process, address, C.byref(info), C.sizeof(info)):
        return dict(result, error="unmapped or null")
    result["region"] = {"base": hex(info.base or 0), "size": info.size,
                        "state": hex(info.state), "protect": hex(info.protect)}
    if info.state != 0x1000 or info.protect & 0x101:
        return dict(result, error="not readable without guard/noaccess")
    length = min(length, max(0, (info.base or 0) + info.size - address))
    buffer = C.create_string_buffer(length)
    ok = cap.read(cap.process, address, buffer, length, C.byref(count))
    result["hex"] = buffer.raw[:count.value].hex()
    if not ok:
        result["error"] = f"ReadProcessMemory: {C.get_last_error()}"
    return result


def snapshot(cap, event, stack_bytes):
    backing = C.create_string_buffer(1247)
    context = (C.addressof(backing) + 15) & ~15
    DWORD.from_address(context + 48).value = 0x10001F  # ALL, including debug registers
    thread = cap.open_thread(0x48, False, event.tid)
    if not thread:
        raise C.WinError(C.get_last_error())
    try:
        owner = bind(cap.k, "GetProcessIdOfThread", DWORD, HANDLE)(thread)
        if owner != cap.pid:
            raise RuntimeError("Thread ownership changed")
        if not cap.context(thread, context):
            raise C.WinError(C.get_last_error())
    finally:
        cap.close(thread)
    raw = C.string_at(context, 1232)
    names = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip".split()
    regs = dict(zip(names, struct.unpack_from("<17Q", raw, 120)))
    regs["eflags"] = struct.unpack_from("<I", raw, 68)[0]
    regs["mxcsr"] = struct.unpack_from("<I", raw, 52)[0]
    record = event.data.exception.record
    info = list(record.information[:min(record.count, 15)])
    result = {"tid": event.tid, "exception_pc": hex(record.address or 0),
              "first_chance": bool(event.data.exception.first), "code": hex(record.code),
              "information": [hex(v) for v in info],
              "access": {0: "read", 1: "write", 8: "execute"}.get(info[0], "unknown") if info and record.code == AV else None,
              "fault_address": hex(info[1]) if len(info) > 1 and record.code == AV else None,
              "registers": {k: hex(v) for k, v in regs.items()}, "context_hex": raw.hex(),
              "stack": memory(cap, regs["rsp"], stack_bytes),
              "instruction": memory(cap, regs["rip"], 32),
              "rbx_object": memory(cap, regs["rbx"], 0x80), "fields": {},
               "register_memory": {name: memory(cap, regs[name], 0x80)
                                   for name in ("rax", "rsi", "rdi", "rbp", "r15")},
              "possible_allocation_headers": {
                  name: memory(cap, regs[name] - 0x10, 0x30)
                  for name in ("rbx", "rdi") if regs[name] >= 0x10}}
    resource = bytes.fromhex(result["rbx_object"].get("hex", ""))
    if len(resource) >= 0x18:
        filename, length = struct.unpack_from("<QQ", resource, 8)
        captured = memory(cap, filename, min(length, 4096)) if length else {"hex": ""}
        captured["declared_length"] = length
        captured["truncated"] = length > 4096
        captured["text"] = bytes.fromhex(captured.get("hex", "")).split(b"\0", 1)[0].decode("utf-8", "replace")
        result["resource_filename"] = captured
    for offset in (0x20, 0x28, 0x38, 0x40, 0x48):
        field = memory(cap, regs["rbx"] + offset, 8) if regs["rbx"] else {"error": "null RBX"}
        data = bytes.fromhex(field.get("hex", ""))
        if len(data) == 8:
            pointer = struct.unpack("<Q", data)[0]
            field["value"] = hex(pointer)
            field["pointee"] = memory(cap, pointer, 128)
            pointee = bytes.fromhex(field["pointee"].get("hex", ""))
            if len(pointee) >= 8:
                field["possible_vtable"] = memory(cap, struct.unpack_from("<Q", pointee)[0], 64)
        result["fields"][hex(offset)] = field
    if regs["rip"] == 0x910f4ff8b:
        # Audited 13.60 PAF queue pump: RAX=node, R15=job, [node+18]=control.
        node = bytes.fromhex(result["register_memory"]["rax"].get("hex", ""))
        if len(node) >= 0x20:
            result["paf_node_targets"] = {
                label: memory(cap, struct.unpack_from("<Q", node, offset)[0], 0x80)
                 for label, offset in (("next", 0), ("previous", 8), ("job", 0x10), ("control", 0x18))}
    if record.code == 0xE06D7363 and len(info) >= 3:
        result["cpp_exception_object"] = memory(cap, info[1], 128)
        result["cpp_throw_info"] = memory(cap, info[2], 16)
    return result, backing, context


def minidump(cap, event, context, path, deadline):
    import msvcrt
    # Callback input has pack(4): PID, HANDLE, CallbackType, union.
    callback_type = C.WINFUNCTYPE(BOOL, HANDLE, HANDLE, HANDLE)
    cancelled = False
    with path.open("xb") as file:
        @callback_type
        def callback(param, input_, output):
            nonlocal cancelled
            if DWORD.from_address(input_ + 12).value == 6:  # CancelCallback
                cancelled = time.monotonic() >= deadline or os.fstat(file.fileno()).st_size >= 32 * 1024 * 1024
                BOOL.from_address(output).value = 1  # CheckCancel
                BOOL.from_address(output + 4).value = int(cancelled)
            return 1

        class CallbackInfo(C.Structure):
            _pack_ = 4
            _fields_ = [("routine", callback_type), ("param", HANDLE)]

        pointers = ExceptionPointers(C.pointer(event.data.exception.record), context)
        exception = DumpException(event.tid, C.pointer(pointers), False)
        cb = CallbackInfo(callback, None)
        write = bind(cap.d, "MiniDumpWriteDump", BOOL, HANDLE, DWORD, HANDLE, DWORD,
                     C.POINTER(DumpException), HANDLE, C.POINTER(CallbackInfo))
        ok = write(cap.process, cap.pid, msvcrt.get_osfhandle(file.fileno()), 0,
                   C.byref(exception), None, C.byref(cb))
        error = C.get_last_error()
    size = path.stat().st_size
    if not ok or cancelled or size > 32 * 1024 * 1024:
        path.unlink()
        return {"error": error, "cancelled": cancelled, "discarded_bytes": size}
    return {"path": str(path), "bytes": size}


def capture(args, output):
    cap = Capture(args.pid, "", args.minidump)
    attached, pending, exited = False, None, False
    status = CONTINUE
    started = time.monotonic()
    result = {"tool": "capture_first_fault.py", "pid": args.pid,
              "watch_pc": hex(args.address) if args.address is not None else None, "watch_code": hex(args.exception_code),
              "watch_cpp_throw_info": hex(args.cpp_throw_info) if args.cpp_throw_info else None,
              "seconds": args.seconds, "status": "timeout",
              "forwarded_exceptions": 0, "attach_breakpoint_handled": False}
    old_signals = {}
    interrupted = False

    def interrupt(signum, frame):
        nonlocal interrupted
        interrupted = True  # no asynchronous exception in ownership transitions

    try:
        cap.check_architecture()
        local = bind(cap.k, "GetModuleHandleW", HANDLE, C.c_wchar_p)("ntdll.dll")
        proc = bind(cap.k, "GetProcAddress", HANDLE, HANDLE, C.c_char_p)
        remote = next(base for base, size, name in cap.modules() if Path(name).name.lower() == "ntdll.dll")
        bp_local = proc(local, b"DbgBreakPoint")
        breakin_local = proc(local, b"DbgUiRemoteBreakin")
        if not local or not bp_local or not breakin_local:
            raise RuntimeError("Cannot identify ntdll debugger attach entrypoints")
        bp = remote + bp_local - local
        breakin = remote + breakin_local - local
        attach = bind(cap.k, "DebugActiveProcess", BOOL, DWORD)
        detach = bind(cap.k, "DebugActiveProcessStop", BOOL, DWORD)
        kill = bind(cap.k, "DebugSetProcessKillOnExit", BOOL, BOOL)
        wait = bind(cap.k, "WaitForDebugEvent", BOOL, C.POINTER(DebugEvent), DWORD)
        continue_ = bind(cap.k, "ContinueDebugEvent", BOOL, DWORD, DWORD, DWORD)
        for sig in (signal.SIGINT, signal.SIGBREAK):
            old_signals[sig] = signal.signal(sig, interrupt)
        if not attach(args.pid):
            raise C.WinError(C.get_last_error())
        attached = True
        if not kill(False):
            raise C.WinError(C.get_last_error())
        deadline = time.monotonic() + args.seconds
        breakin_threads = set()
        while time.monotonic() < deadline and not interrupted:
            event = DebugEvent()
            if not wait(C.byref(event), min(100, max(1, int((deadline - time.monotonic()) * 1000)))):
                error = C.get_last_error()
                if error == 121:  # ERROR_SEM_TIMEOUT
                    continue
                raise C.WinError(error)
            pending = event
            status = NOT_HANDLED if event.code == 1 else CONTINUE
            # CREATE/EXIT process and thread handles are owned/closed by Windows
            # at matching continued EXIT events or detach. Do not double-close
            # those borrowed handles. File handles MUST be closed by debugger.
            if event.code == 3 and event.data.process.file:
                cap.close(event.data.process.file)
            elif event.code == 6 and event.data.dll.file:
                cap.close(event.data.dll.file)
            if event.code == 2 and event.data.thread.start == breakin:
                breakin_threads.add(event.tid)
            if event.code == 4:
                breakin_threads.discard(event.tid)
            if event.code == 1:
                ex = event.data.exception
                if (not result["attach_breakpoint_handled"] and ex.first and
                        ex.record.code == BREAKPOINT and ex.record.address == bp and event.tid in breakin_threads):
                    status = CONTINUE
                    result["attach_breakpoint_handled"] = True
                else:
                    result["forwarded_exceptions"] += 1
                type_matches = (not args.cpp_throw_info or
                                (ex.record.count >= 3 and ex.record.information[2] == args.cpp_throw_info))
                if ((ex.first or args.include_second_chance) and ex.record.code == args.exception_code and
                        (args.address is None or ex.record.address == args.address) and type_matches):
                    result["snapshot"], backing, context = snapshot(cap, event, args.stack_bytes)
                    result["modules"] = [{"base": hex(base), "size": size, "path": name}
                                         for base, size, name in cap.modules()]
                    result["snapshot"]["requested_memory"] = [memory(cap, address, 128) for address in args.read_address]
                    result["status"] = "captured"
                    if args.minidump and time.monotonic() < deadline:
                        result["minidump"] = minidump(cap, event, context, output.with_suffix(".dmp"), deadline)
                    break
            if event.code == 5:
                result.update(status="target_exited", exit_code=event.data.exit_code)
                exited = True
            if not continue_(event.pid, event.tid, status):
                raise C.WinError(C.get_last_error())
            pending = None
            if exited:
                break
        if interrupted:
            result["status"] = "interrupted"
    except Exception as exc:
        result.update(status="error", error=str(exc))
    finally:
        if pending is not None:
            if not continue_(pending.pid, pending.tid, status):
                result["continue_error"] = C.get_last_error()
        if attached and not exited:
            if not detach(args.pid):
                result["detach_error"] = C.get_last_error()
            else:
                result["detached"] = True
        cap.finish()
        for sig, handler in old_signals.items():
            signal.signal(sig, handler)
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--address", type=lambda s: int(s, 0), help="Exact exception PC; omit to match any address")
    parser.add_argument("--include-second-chance", action="store_true", help="Also capture second-chance exceptions (including fail-fast)")
    parser.add_argument("--exception-code", type=lambda s: int(s, 0), default=AV,
                        help="First-chance exception code (default access violation)")
    parser.add_argument("--cpp-throw-info", type=lambda s: int(s, 0),
                        help="Optional exact, audited C++ ThrowInfo address filter")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--stack-bytes", type=int, default=4096)
    parser.add_argument("--minidump", action="store_true")
    parser.add_argument("--read-address", type=lambda s: int(s, 0), action="append", default=[],
                        help="Read 128 bytes at a known address when the watched fault occurs (maximum eight)")
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(HANDLE) != 8:
        parser.error("Windows 64-bit Python is required")
    if not (args.pid > 0 and (args.address is None or 0 < args.address < (1 << 64)) and 0 < args.seconds <= 3600 and 0 <= args.stack_bytes <= 65536):
        parser.error("require positive PID/address, seconds in (0,3600], stack bytes in [0,65536]")
    if not 0 < args.exception_code <= 0xffffffff or args.exception_code == BREAKPOINT:
        parser.error("require a 32-bit exception code other than the attach breakpoint")
    if args.cpp_throw_info is not None and (args.exception_code != 0xe06d7363 or not 0 < args.cpp_throw_info < (1 << 64)):
        parser.error("ThrowInfo filtering requires a positive address and the C++ exception code")
    if len(args.read_address) > 8 or any(not 0 < address < (1 << 64) - 128 for address in args.read_address):
        parser.error("require at most eight positive read addresses with room for 128 bytes")
    validate_layouts()
    root = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "opencode"
    root.mkdir(parents=True, exist_ok=True)
    output = root / f"first-fault-{args.pid}-{time.time_ns()}.json"
    result = capture(args, output)
    with output.open("x", encoding="utf8") as file:
        json.dump(result, file, indent=2)
    print(output)
    print(result["status"])
    return 0 if result["status"] == "captured" and not any(k in result for k in ("continue_error", "detach_error")) else 1


if __name__ == "__main__":
    sys.exit(main())
