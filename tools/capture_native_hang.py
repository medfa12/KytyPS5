"""External Windows x64 thread snapshots; no debugger install or target hooks.

Example: python tools/capture_native_hang.py --pid 12345 --minidump
Output defaults to %LOCALAPPDATA%/Temp/opencode/native-hang-PID-TIMESTAMP.txt.
--raw-json saves bounded raw AMD64 contexts and stacks to a sibling .raw.json.
The optional stdout CS probe requires --stdout-cs-rva, --ucrt-image and
--ucrt-sha256 together. The RVA must be audited for that exact local image;
no stdout layout/RVA is assumed across UCRT versions.

Run with 64-bit Python. Threads are sampled sequentially, not atomically. Only
GetThreadContext/VirtualQueryEx/ReadProcessMemory run while a thread is stopped;
unwinding and local-only symbol loading happen after its suspend increment has
been removed. Existing suspension counts are preserved. Guest/JIT frames may
not unwind because they have no Windows unwind metadata.
Do not terminate the capture process forcibly during sampling. Ctrl+C/Ctrl+Break
are masked during each short suspension window. Optional minidumps use DbgHelp's
own capture implementation, independently of the bounded stack sampler.
"""

import argparse
import base64
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import sys
import time


U64 = C.c_uint64
DWORD = C.c_uint32
BOOL = C.c_int32
HANDLE = C.c_void_p
SIZE = C.c_size_t
MAX_THREADS = 4096
MAX_IMAGE_BYTES = 32 * 1024 * 1024


def local_ucrt_identity(path, expected_sha256, cs_rva):
    """Bounded local PE inspection; never load/execute the supplied DLL."""
    with path.open("rb") as image:
        data = image.read(MAX_IMAGE_BYTES + 1)
    if len(data) > MAX_IMAGE_BYTES:
        raise RuntimeError("UCRT image exceeds 32 MiB inspection limit")
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected_sha256.lower():
        raise RuntimeError("Local UCRT SHA-256 does not match --ucrt-sha256")
    if len(data) < 64 or data[:2] != b"MZ":
        raise RuntimeError("Local UCRT is not a PE image")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe > 65536 or pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError("Invalid local UCRT PE header")
    machine, sections, timestamp = struct.unpack_from("<HHI", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    table = optional + optional_size
    if machine != 0x8664 or not 1 <= sections <= 96 or optional_size < 112 or table + sections * 40 > len(data):
        raise RuntimeError("Invalid local AMD64 UCRT section/optional headers")
    if struct.unpack_from("<H", data, optional)[0] != 0x20b:
        raise RuntimeError("Local UCRT must be PE32+")
    image_size, header_size = struct.unpack_from("<II", data, optional + 56)
    if not table + sections * 40 <= header_size <= min(len(data), 65536):
        raise RuntimeError("Invalid local UCRT SizeOfHeaders")
    if not 0 < image_size <= MAX_IMAGE_BYTES or cs_rva < 0 or cs_rva % 8 or cs_rva + 40 > image_size:
        raise RuntimeError("stdout CS RVA must be aligned and within the bounded UCRT image")
    for i in range(sections):
        section = table + i * 40
        virtual_size, rva = struct.unpack_from("<II", data, section + 8)
        characteristics = struct.unpack_from("<I", data, section + 36)[0]
        if rva <= cs_rva and cs_rva + 40 <= rva + virtual_size and characteristics & 0x80000000:
            break
    else:
        raise RuntimeError("stdout CS RVA is not wholly within a writable UCRT section")
    return {"local_path": str(path), "sha256": digest, "machine": machine,
            "timestamp": timestamp, "image_size": image_size, "header_size": header_size,
            "image_base_offset": optional + 24,
            "headers_sha256": hashlib.sha256(data[:header_size]).hexdigest()}, data[:header_size]


def raw_sample(sample):
    result = {k: v for k, v in sample.items() if k not in ("context", "stack")}
    if "context" in sample:
        context = sample["context"]
        result["context_base64"] = base64.b64encode(context).decode("ascii")
        result["context_size"] = len(context)
        result["context_flags"] = struct.unpack_from("<I", context, 48)[0]
        names = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                 "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip")
        result["registers"] = {name: f"0x{struct.unpack_from('<Q', context, 120 + i * 8)[0]:016x}"
                               for i, name in enumerate(names)}
    if "stack" in sample:
        result["stack_base64"] = base64.b64encode(sample["stack"]).decode("ascii")
    return result


def correlate_stdout(probes, samples):
    by_tid = {s["tid"]: s for s in samples}
    for probe in probes:
        owner = probe.get("owning_thread")
        sample = by_tid.get(owner)
        probe["owner_sample"] = None if sample is None else {
            k: sample[k] for k in ("tid", "previous_suspend_count", "resume_previous_count",
                                   "rip", "rsp", "error") if k in sample}
        # These are sequential observations, not the suspend count at probe time.
        probe["owner_correlation"] = "sampled" if sample is not None else (
            "unowned" if owner == 0 else "unavailable" if owner is None else "not_sampled")


def write_raw_json(path, metadata, samples):
    # Encode one bounded sample at a time rather than duplicate all raw stacks.
    with path.open("x", encoding="utf-8") as out:
        out.write('{"metadata":')
        json.dump(metadata, out, indent=2)
        out.write(',\n"threads":[\n')
        for i, sample in enumerate(samples):
            if i:
                out.write(',\n')
            json.dump(raw_sample(sample), out)
        out.write('\n]}\n')


class ThreadEntry(C.Structure):
    _fields_ = [(n, DWORD) for n in ("size", "usage", "tid", "pid", "priority", "delta", "flags")]


class ModuleInfo(C.Structure):
    _fields_ = [("base", HANDLE), ("size", DWORD), ("entry", HANDLE)]


class MemoryInfo(C.Structure):
    _fields_ = [("base", HANDLE), ("allocation", HANDLE), ("protect0", DWORD),
                ("partition", C.c_uint16), ("size", SIZE), ("state", DWORD),
                ("protect", DWORD), ("type", DWORD)]


class Address(C.Structure):
    _fields_ = [("offset", U64), ("segment", C.c_uint16), ("mode", DWORD)]


class StackFrame(C.Structure):
    # KDHELP64 is output/opaque to this caller. Reserve more than its current
    # 112 bytes for compatibility with OS DbgHelp versions.
    _fields_ = [(n, Address) for n in ("pc", "ret", "frame", "stack", "bstore")] + [
        ("function", HANDLE), ("params", U64 * 4), ("far", BOOL), ("virtual", BOOL),
        ("reserved", U64 * 3), ("kdhelp", C.c_byte * 256)]


class Symbol(C.Structure):
    _fields_ = [("size", DWORD), ("type", DWORD), ("reserved", U64 * 2),
                ("index", DWORD), ("symbol_size", DWORD), ("base", U64),
                ("flags", DWORD), ("value", U64), ("address", U64),
                ("register", DWORD), ("scope", DWORD), ("tag", DWORD),
                ("name_len", DWORD), ("max_name", DWORD), ("name", C.c_char * 1)]


def bind(dll, name, result, *args):
    fn = getattr(dll, name)
    fn.restype, fn.argtypes = result, args
    return fn


class Capture:
    def __init__(self, pid, symbols, dump):
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.p = C.WinDLL("psapi", use_last_error=True)
        # Explicit System32 path prevents a workspace DLL from being loaded.
        self.d = C.WinDLL(str(Path(os.environ["SystemRoot"]) / "System32/dbghelp.dll"), use_last_error=True)
        self.close = bind(self.k, "CloseHandle", BOOL, HANDLE)
        self.open_thread = bind(self.k, "OpenThread", HANDLE, DWORD, BOOL, DWORD)
        self.suspend = bind(self.k, "SuspendThread", DWORD, HANDLE)
        self.resume = bind(self.k, "ResumeThread", DWORD, HANDLE)
        self.context = bind(self.k, "GetThreadContext", BOOL, HANDLE, HANDLE)
        self.query = bind(self.k, "VirtualQueryEx", SIZE, HANDLE, HANDLE, HANDLE, SIZE)
        self.read = bind(self.k, "ReadProcessMemory", BOOL, HANDLE, HANDLE, HANDLE, SIZE, C.POINTER(SIZE))
        open_process = bind(self.k, "OpenProcess", HANDLE, DWORD, BOOL, DWORD)
        self.process = open_process(0x410 | (0x40 if dump else 0), False, pid)
        if not self.process:
            raise C.WinError(C.get_last_error())
        self.initialized = False
        self.pid, self.symbol_path = pid, symbols

    def finish(self):
        if self.initialized:
            bind(self.d, "SymCleanup", BOOL, HANDLE)(self.process)
        self.close(self.process)

    def check_architecture(self):
        fn = bind(self.k, "IsWow64Process2", BOOL, HANDLE, C.POINTER(C.c_uint16), C.POINTER(C.c_uint16))
        process_machine, native_machine = C.c_uint16(), C.c_uint16()
        if not fn(self.process, C.byref(process_machine), C.byref(native_machine)):
            raise C.WinError(C.get_last_error())
        if process_machine.value != 0 or native_machine.value != 0x8664:
            raise RuntimeError("Only native AMD64 targets are supported")

    def threads(self):
        snap = bind(self.k, "CreateToolhelp32Snapshot", HANDLE, DWORD, DWORD)(4, 0)
        if snap == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        try:
            first = bind(self.k, "Thread32First", BOOL, HANDLE, C.POINTER(ThreadEntry))
            next_ = bind(self.k, "Thread32Next", BOOL, HANDLE, C.POINTER(ThreadEntry))
            e = ThreadEntry()
            e.size = C.sizeof(e)
            ok = first(snap, C.byref(e))
            if not ok:
                raise C.WinError(C.get_last_error())
            result = []
            while ok:
                if e.pid == self.pid:
                    result.append(e.tid)
                ok = next_(snap, C.byref(e))
            return sorted(result)
        finally:
            self.close(snap)

    def modules(self):
        handles, needed = (HANDLE * 4096)(), DWORD()
        enum = bind(self.p, "EnumProcessModulesEx", BOOL, HANDLE, HANDLE, DWORD, C.POINTER(DWORD), DWORD)
        if not enum(self.process, handles, C.sizeof(handles), C.byref(needed), 3):
            raise C.WinError(C.get_last_error())
        if needed.value > C.sizeof(handles):
            raise RuntimeError("Module list exceeded 4096 entries")
        get_name = bind(self.p, "GetModuleFileNameExW", DWORD, HANDLE, HANDLE, W.LPWSTR, DWORD)
        get_info = bind(self.p, "GetModuleInformation", BOOL, HANDLE, HANDLE, C.POINTER(ModuleInfo), DWORD)
        result = []
        for h in handles[:needed.value // C.sizeof(HANDLE)]:
            name, info = C.create_unicode_buffer(32768), ModuleInfo()
            if get_name(self.process, h, name, len(name)) and get_info(self.process, h, C.byref(info), C.sizeof(info)):
                result.append((info.base, info.size, name.value))
        return result

    def read_exact(self, address, size):
        # Extra probes call this only outside sample(), never while holding a
        # sampler suspension increment. Preexisting suspensions are untouched.
        buffer, count = C.create_string_buffer(size), SIZE()
        if not self.read(self.process, address, buffer, size, C.byref(count)) or count.value != size:
            raise RuntimeError(f"ReadProcessMemory at 0x{address:x}: got {count.value}/{size}, error {C.get_last_error()}")
        return buffer.raw

    def prepare_stdout_probe(self, path, sha256, rva):
        identity, headers = local_ucrt_identity(path, sha256, rva)
        candidates = [(base, size, name) for base, size, name in self.module_list
                      if Path(name).name.lower() == "ucrtbase.dll"]
        if len(candidates) != 1:
            raise RuntimeError("stdout probe requires exactly one loaded ucrtbase.dll")
        base, size, name = candidates[0]
        # Windows may replace OptionalHeader.ImageBase with the ASLR load
        # address. Permit only that specific loader change, not other mismatches.
        rebased_headers = bytearray(headers)
        struct.pack_into("<Q", rebased_headers, identity["image_base_offset"], base)
        remote_headers = self.read_exact(base, len(headers))
        if size != identity["image_size"] or remote_headers not in (headers, bytes(rebased_headers)):
            raise RuntimeError("Target UCRT PE headers/size do not match the supplied local image")
        identity.update(target_path=name, target_base=base, cs_rva=rva, cs_address=base + rva,
                        target_headers_sha256=hashlib.sha256(remote_headers).hexdigest(),
                        validation="explicit local SHA-256 + target PE header/SizeOfImage match (only ASLR ImageBase change allowed)")
        self.stdout_identity, self.stdout_headers = identity, remote_headers
        return identity

    def stdout_probe(self, phase):
        identity = self.stdout_identity
        result = {"phase": phase, "started_unix_ns": time.time_ns(),
                  "address": identity["cs_address"]}
        try:
            # Recheck even after sampling: do not silently read an unloaded or
            # replaced module at the old address.
            if self.read_exact(identity["target_base"], len(self.stdout_headers)) != self.stdout_headers:
                raise RuntimeError("Target UCRT PE headers changed")
            info = MemoryInfo()
            address = identity["cs_address"]
            if not self.query(self.process, address, C.byref(info), C.sizeof(info)) or (
                info.state != 0x1000 or info.type != 0x1000000 or info.protect & 0x101 or
                info.allocation != identity["target_base"] or address + 40 > info.base + info.size
            ):
                raise RuntimeError("stdout CS is not in a readable committed UCRT image region")
            data = self.read_exact(address, 40)
            debug, lock_count, recursion, owner, semaphore, spin = struct.unpack("<QiiQQQ", data)
            result.update(raw_hex=data.hex(), debug_info=debug, lock_count=lock_count,
                          recursion_count=recursion, owning_thread=owner,
                          lock_semaphore=semaphore, spin_count=spin)
        except (OSError, RuntimeError) as error:
            result["error"] = str(error)
        result["finished_unix_ns"] = time.time_ns()
        return result

    def sample(self, tid, stack_bytes, stack_bounds=None):
        # Preallocate the native buffers before stopping the target. AMD64 CONTEXT is
        # 1232 bytes and must be 16-byte aligned (winnt.h).
        backing = C.create_string_buffer(1232 + 15)
        context = (C.addressof(backing) + 15) & ~15
        C.c_uint32.from_address(context + 48).value = 0x10000B  # FULL
        stack = C.create_string_buffer(stack_bytes)
        info, count = MemoryInfo(), SIZE()
        h = self.open_thread(0x004A, False, tid)  # suspend/resume, context, query
        if not h:
            return {"tid": tid, "error": f"OpenThread error {C.get_last_error()}"}
        stopped = False
        # A Python KeyboardInterrupt between SuspendThread returning and the
        # stopped flag assignment could otherwise skip the matching resume.
        old_signals = {}
        for sig in (signal.SIGINT, signal.SIGBREAK):
            old_signals[sig] = signal.signal(sig, signal.SIG_IGN)
        result = {"tid": tid}
        try:
            # Verify ownership in case the thread ID was recycled.
            owner = bind(self.k, "GetProcessIdOfThread", DWORD, HANDLE)(h)
            if owner != self.pid:
                result["error"] = "Thread exited or ID reused"
                return result
            started = time.monotonic()
            previous = self.suspend(h)
            if previous == 0xFFFFFFFF:
                result["error"] = f"SuspendThread error {C.get_last_error()}"
                return result
            stopped = True
            result["previous_suspend_count"] = previous
            if not self.context(h, context):
                result["error"] = f"GetThreadContext error {C.get_last_error()}"
                return result
            rsp = C.c_uint64.from_address(context + 152).value
            result["rsp"] = rsp
            result["rip"] = C.c_uint64.from_address(context + 248).value
            if stack_bounds is not None and not stack_bounds[0] <= rsp < stack_bounds[1]:
                result['error'] = 'RSP outside supplied stack provenance bounds'
                return result
            # One contiguous read, clipped to the committed readable region.
            if self.query(self.process, rsp, C.byref(info), C.sizeof(info)) and info.state == 0x1000 and not (info.protect & 0x101):
                length = min(stack_bytes, max(0, info.base + info.size - rsp))
                if stack_bounds is not None:
                    length = min(length, stack_bounds[1] - rsp)
                self.read(self.process, rsp, stack, length, C.byref(count))
            result["stack_size"] = count.value
        finally:
            try:
                if stopped:
                    # Remove ONLY our increment, including when the target was
                    # already suspended by Mono. Never resume down to zero.
                    resumed = self.resume(h)
                    result["resume_previous_count"] = resumed
                    result["stopped_ms"] = (time.monotonic() - started) * 1000
                    if resumed == 0xFFFFFFFF:
                        raise RuntimeError(f"ResumeThread FAILED tid={tid} error={C.get_last_error()}; target may remain suspended")
            finally:
                self.close(h)
                for sig, handler in old_signals.items():
                    signal.signal(sig, handler)
        result["context"] = C.string_at(context, 1232)
        result["stack"] = stack.raw[:count.value]
        return result

    def init_symbols(self):
        # No inherited symbol servers, network paths, dialogs, or target calls.
        bind(self.d, "SymSetOptions", DWORD, DWORD)(0x2 | 0x4 | 0x200 | 0x1000 | 0x80000)
        init = bind(self.d, "SymInitializeW", BOOL, HANDLE, W.LPCWSTR, BOOL)
        if not init(self.process, self.symbol_path, True):
            raise C.WinError(C.get_last_error())
        self.initialized = True

    def unwind(self, sample, frame_limit):
        backing = C.create_string_buffer(1247)
        context = (C.addressof(backing) + 15) & ~15
        C.memmove(context, sample["context"], 1232)
        frame = StackFrame()
        frame.pc.offset = sample["rip"]
        frame.stack.offset = sample["rsp"]
        frame.frame.offset = struct.unpack_from("<Q", sample["context"], 160)[0]
        for a in (frame.pc, frame.stack, frame.frame):
            a.mode = 3  # AddrModeFlat
        start, data = sample["rsp"], sample["stack"]
        read_type = C.WINFUNCTYPE(BOOL, HANDLE, U64, HANDLE, DWORD, C.POINTER(DWORD))

        @read_type
        def read_memory(process, address, output, size, actual):
            actual[0] = 0
            try:
                if start <= address < start + len(data):
                    offset = address - start
                    chunk = data[offset:offset + size]
                    C.memmove(output, chunk, len(chunk))
                    actual[0] = len(chunk)
                    return len(chunk) == size
                # Only read stable image metadata outside the saved stack;
                # never fall back to a now-running thread's live stack.
                if not any(base <= address and address + size <= base + length for base, length, _ in self.module_list):
                    return False
                n = SIZE()
                ok = self.read(process, address, output, size, C.byref(n))
                actual[0] = n.value
                return ok
            except Exception:
                return False

        walk = bind(self.d, "StackWalk64", BOOL, DWORD, HANDLE, HANDLE, C.POINTER(StackFrame), HANDLE,
                    read_type, HANDLE, HANDLE, HANDLE)
        table = C.cast(self.d.SymFunctionTableAccess64, HANDLE)
        base = C.cast(self.d.SymGetModuleBase64, HANDLE)
        # The read callback handles all remote access; thread handle is an
        # opaque identifier here, not an opened or suspended thread.
        frames = [(sample["rip"], sample["rsp"])]
        seen = set(frames)
        for _ in range(frame_limit):
            if not walk(0x8664, self.process, sample["tid"], C.byref(frame), context, read_memory, table, base, None):
                break
            item = (frame.pc.offset, frame.stack.offset)
            if not item[0]:
                break
            if item == frames[0] and len(frames) == 1:
                continue  # StackWalk64 may return the initial frame first.
            if item in seen:
                break
            frames.append(item)
            seen.add(item)
            if len(frames) >= frame_limit:
                break
        return frames

    def symbol(self, address):
        label = "<unknown/guest/JIT>"
        for base, length, path in self.module_list:
            if base <= address < base + length:
                label = f"{Path(path).name}+0x{address-base:x}"
                break
        buffer = C.create_string_buffer(C.sizeof(Symbol) + 1024)
        symbol = C.cast(buffer, C.POINTER(Symbol))
        symbol.contents.size, symbol.contents.max_name = C.sizeof(Symbol), 1024
        displacement = U64()
        fn = bind(self.d, "SymFromAddr", BOOL, HANDLE, U64, C.POINTER(U64), C.POINTER(Symbol))
        if fn(self.process, address, C.byref(displacement), symbol):
            name = C.string_at(C.addressof(buffer) + Symbol.name.offset, symbol.contents.name_len).decode("utf-8", "replace")
            label += f" {name}+0x{displacement.value:x}"
        return label

    def minidump(self, path):
        import msvcrt
        fn = bind(self.d, "MiniDumpWriteDump", BOOL, HANDLE, DWORD, HANDLE, DWORD, HANDLE, HANDLE, HANDLE)
        # Normal + unloaded modules + thread info. No FullMemory, private RW,
        # indirect memory, or handle-data flags. Size depends on thread count.
        with path.open("xb") as out:
            if not fn(self.process, self.pid, msvcrt.get_osfhandle(out.fileno()), 0x1020, None, None, None):
                raise C.WinError(C.get_last_error())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="Explicit target process ID")
    parser.add_argument("--output", type=Path, help="Output .txt path (default: LocalAppData/Temp/opencode)")
    parser.add_argument("--symbols", type=Path, default=Path(__file__).resolve().parents[1] / "_Build/windows")
    parser.add_argument("--frames", type=int, default=64)
    parser.add_argument("--stack-kib", type=int, default=128)
    parser.add_argument("--minidump", action="store_true", help="Also write a small-policy .dmp; excludes full/private process memory")
    parser.add_argument("--raw-json", action="store_true", help="Save raw CONTEXT/stack bytes to <output>.raw.json before symbolization")
    parser.add_argument("--stdout-cs-rva", type=lambda v: int(v, 0), help="Audited stdout RTL_CRITICAL_SECTION RVA for the explicit UCRT image")
    parser.add_argument("--ucrt-image", type=Path, help="Local ucrtbase.dll used to audit --stdout-cs-rva; never executed")
    parser.add_argument("--ucrt-sha256", help="Expected SHA-256 of that exact local UCRT image")
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(HANDLE) != 8:
        parser.error("Requires 64-bit Windows Python")
    if args.pid <= 0 or args.pid == os.getpid():
        parser.error("PID must be positive and cannot be this capture process")
    if not 1 <= args.frames <= 256 or not 4 <= args.stack_kib <= 1024:
        parser.error("frames must be 1..256 and stack-kib 4..1024")
    probe_options = (args.stdout_cs_rva is not None, args.ucrt_image is not None, args.ucrt_sha256 is not None)
    if any(probe_options) and not all(probe_options):
        parser.error("--stdout-cs-rva, --ucrt-image and --ucrt-sha256 must be supplied together")
    if all(probe_options):
        args.ucrt_image = args.ucrt_image.resolve()
        if str(args.ucrt_image).startswith("\\\\") or not args.ucrt_image.is_file():
            parser.error("ucrt-image must be an existing local file")
        if len(args.ucrt_sha256) != 64 or any(c not in "0123456789abcdefABCDEF" for c in args.ucrt_sha256):
            parser.error("ucrt-sha256 must contain exactly 64 hexadecimal digits")
    symbol_path = str(args.symbols.resolve())
    if symbol_path.startswith("\\\\") or any(c in symbol_path for c in (";", "*")) or not args.symbols.is_dir():
        parser.error("symbols must be an existing local directory, not a server/search expression")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output = args.output or Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode" / f"native-hang-{args.pid}-{stamp}.txt"
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_path = output.with_suffix(".raw.json")
    if args.raw_json and (raw_path == output or raw_path.exists()):
        parser.error("raw JSON output must be distinct and must not already exist")
    capture = Capture(args.pid, symbol_path, args.minidump)
    try:
        capture.check_architecture()
        capture.module_list = capture.modules()
        tids = capture.threads()
        if len(tids) > MAX_THREADS:
            raise RuntimeError(f"Thread list exceeded {MAX_THREADS} entries; refusing partial capture")
        stdout_identity = None
        if all(probe_options):
            stdout_identity = capture.prepare_stdout_probe(args.ucrt_image, args.ucrt_sha256, args.stdout_cs_rva)
        with output.open("x", encoding="utf-8", buffering=1) as report:
            def emit(text):
                print(text, file=report, flush=True)
            emit(f"Native AMD64 snapshot PID={args.pid} time={stamp}")
            emit(f"Symbols: {symbol_path}; PDB exists: {(args.symbols / 'kyty_emulator.pdb').is_file()}")
            emit("Sequential snapshots. Registers/stack bytes captured while stopped; unwind after resume.")
            emit("Native images only; guest/JIT frames may terminate unwinding. Existing suspensions preserved.")
            for base, size, path in capture.module_list:
                emit(f"MODULE {base:016x}-{base+size:016x} {path}")
            probes = []
            if stdout_identity:
                emit("STDOUT_CS_IDENTITY " + json.dumps(stdout_identity))
                probes.append(capture.stdout_probe("before"))
            samples = []
            for tid in tids:
                s = capture.sample(tid, args.stack_kib * 1024)
                samples.append(s)
                emit("THREAD " + " ".join(f"{k}={v:#x}" if k in ("rip", "rsp") else f"{k}={v}" for k, v in s.items() if k not in ("context", "stack")))
            if stdout_identity:
                probes.append(capture.stdout_probe("after"))
                correlate_stdout(probes, samples)
                for probe in probes:
                    emit("STDOUT_CS " + json.dumps(probe))
            if args.raw_json:
                metadata = {"schema_version": 1, "pid": args.pid, "timestamp": stamp,
                            "architecture": "AMD64", "context_format": "Windows CONTEXT, 1232 bytes, CONTEXT_FULL (not XSTATE)",
                            "binary_encoding": "base64", "stack_origin": "rsp in each thread record",
                            "sequential": True, "owner_counts_are_sequential_samples": True,
                            "stack_limit_bytes": args.stack_kib * 1024, "thread_limit": MAX_THREADS,
                            "enumerated_threads": len(tids), "frame_limit": args.frames,
                            "modules": [{"base": b, "size": n, "path": p} for b, n, p in capture.module_list],
                            "stdout_identity": stdout_identity, "stdout_probes": probes}
                write_raw_json(raw_path, metadata, samples)
                emit(f"Raw JSON: {raw_path} ({raw_path.stat().st_size} bytes)")
            # All sampler suspension increments have been removed before any
            # potentially slow DbgHelp operation, including initialization.
            capture.init_symbols()
            for s in samples:
                if "error" in s:
                    continue
                emit(f"\nSTACK tid={s['tid']} original_suspend_count={s['previous_suspend_count']}")
                for i, (pc, sp) in enumerate(capture.unwind(s, args.frames)):
                    emit(f"  #{i:02} RIP={pc:016x} RSP={sp:016x} {capture.symbol(pc)}")
            if args.minidump:
                dump = output.with_suffix(".dmp")
                emit(f"Writing optional minidump: {dump} (DbgHelp manages its own capture)")
                capture.minidump(dump)
                emit(f"Minidump bytes: {dump.stat().st_size}")
        print(output)
    finally:
        capture.finish()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"capture failed: {error}", file=sys.stderr)
        sys.exit(1)
