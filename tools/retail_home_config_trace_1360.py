"""Retail-pinned, read-only Home HBC consumer observation. No process APIs.

Native opcode handler traps are NOT breakpoints on HBC data. Only a complete
hash-verified Home buffer and a freshly validated managed owner admit a hit.
Values are classified, never traversed/stringified. No auth payload is emitted.
"""
import hashlib
import math
from pathlib import Path
import re
import struct

from home_gate_trace_1360 import Image, DecodeError, ScopeExcluded, require, digest

EXE_HASH = '05d0ef41c5b3e68aaec8d5e45af7aeb6374c69a5af05da441f65e9da13f4be13'
HOME_HASH = '565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f'
PINS = {
    'hermes': ('libhermes-0.11.sprx', 'e279a9d4272df0347515a481bd0fe4d4bb0f7bb9281c3ece20032688c0f9d10d'),
    'pui': ('ReactNative.PUI.dll.sprx', 'bcd86b01f584eb83eca75172606a017a72e4f2e734dc967fc25a42a40c90e7da'),
    'extensions': ('libNativeExtensions.sprx', '2799847208988cb63ae455a974b0d0949fb1159cf24f8f8bbb382672cc65a8fb'),
    'mono': ('libmonosgen-2.0.sprx', '294af118b930006345855ecc63e452d3fa28a2c138136755c6f2066b15938a40'),
}
INITIAL = {'loader': ('host', 0x9f447e), 'call2': ('hermes', 0x59d00),
           'ret': ('hermes', 0x5a7f0), 'false': ('hermes', 0x5cc60)}
ACTIVE = {'owner': ('pui', 0x1c8790), **{k: v for k, v in INITIAL.items() if k != 'loader'}}
CHECKS = {
    'hermes': {0x59d00: '410fb64a03498b842428240000498d5a',
               0x5a7f0: '410fb6842450250000a8030f85b80000',
               0x5cc60: '410fb642024d89f54d89d6498b3cc7e8',
               0x14046: '4c8b7e70488db508ffffff4c89bdf8fe',
               0x14f19: '488d0528742500488d0d717625004889',
               0x59558: '488b9570feffffb90f000000488b4208',
               0x5946e: '4d8bbc24282400004d89bc2440240000'},
    'pui': {0x1c8790: '55488bec4883ec304c896df84c8bef48',
            0x668c9: '488b47284989d14989f0488b80800000',
            0x92394: '488b47304885c07406488b40185dc331',
            0x8d9d0: '554889e5c5f8108798000000c5f81106',
            0x8f180: '488b41304885c0742f488b40304885c0',
            0x92800: '554889e5488b47685dc3cccccccccccc',
            0x8e800: '554889e5488b4738488b4f10488b1048',
            0x92d40: '554889e58a87920000005dc3cccccccc'},
    'extensions': {0x2c902: '4c8b7f784889d7e8122203004d8b0748',
                   0x196d0: '488b07488b4028ffe0cccccccccccccc'},
    'host': {0x9f447e: 'c348c785d0000000'},
}
WINDOWS = {k: [(a, len(bytes.fromhex(b))) for a, b in values.items()] for k, values in CHECKS.items()}


class HostImage:
    def __init__(self, path):
        import pefile
        self.data = path.read_bytes()
        require(digest(self.data) == EXE_HASH, 'host_hash')
        self.pe = pefile.PE(data=self.data)
        require(self.pe.FILE_HEADER.Machine == 0x8664, 'host_machine')

    def at(self, rva, size):
        s = self.pe.get_section_by_rva(rva)
        require(s is not None and s.Characteristics & 0x20000000 and
                rva + size <= s.VirtualAddress + s.Misc_VirtualSize, 'host_window')
        return self.pe.get_data(rva, size)


def audit(images):
    for key, values in CHECKS.items():
        for rva, value in values.items():
            require(images[key].at(rva, len(bytes.fromhex(value))) == bytes.fromhex(value), 'abi_' + key)


def provenance(original, stage, exe, log):
    images, bases, report = {'host': HostImage(exe)}, {}, {}
    text = log.read_text(encoding='utf-8', errors='replace')
    for key, (name, sha) in PINS.items():
        source, selected = original / name, stage / name
        data = source.read_bytes()
        images[key] = Image(data, sha)
        require(selected.read_bytes() == data, 'stage_bytes_' + key)
        places = re.findall(r'place ' + re.escape(name) + r' base=(0x[\da-fA-F]+) mapped=(0x[\da-fA-F]+)', text)
        loads = re.findall(r'^Loading: (.*[\\/]' + re.escape(name) + r')\s*$', text, re.M)
        require(len(places) <= 1, 'multiple_mappings_' + key)
        if key != 'pui':
            require(len(places) == 1, 'missing_mapping_' + key)
        if places:
            base, extent = (int(v, 16) for v in places[0])
            require(base >= 0x10000 and base % 4096 == 0 and
                    all(s[3] + s[6] <= extent for s in images[key].loads), 'mapping_' + key)
            require(loads and all(Path(p.strip()).resolve() == selected.resolve() for p in loads), 'load_path_' + key)
            bases[key] = base
        report[key] = dict(name=name, sha256=sha, stage=str(selected.resolve()), original=str(source.resolve()),
                           base=hex(bases[key]) if key in bases else None,
                           acquisition='lazy loader return' if key == 'pui' else 'pre-guest placement')
    audit(images)
    return images, bases, report


def value_kind(raw):
    tag = raw >> 48
    if tag < 0xfff9:
        return 'number'
    if tag == 0xfffa:
        return 'null' if raw & (1 << 47) else 'undefined'
    return {0xfff9: 'empty', 0xfffb: 'boolean', 0xfffc: 'symbol',
            0xfffd: 'native', 0xfffe: 'string', 0xffff: 'object'}[tag]


def count_value(raw):
    require(value_kind(raw) == 'number', 'count_type')
    value = struct.unpack('<d', struct.pack('<Q', raw))[0]
    require(math.isfinite(value) and value.is_integer() and 0 <= value <= 1000000, 'count_bound')
    return int(value)


FUNCTIONS = (3081, 3254, *range(3255, 3270), 3277, 1958, 1959, 5744, 9016, 9019)
HEADERS = {3081: (0x10e591, 0xf3), 3254: (0x1121fe, 0xd8), 3277: (0x1127c1, 0x16f),
           1958: (0xf33dd, 0x64), 1959: (0xf3441, 0x70), 5744: (0x147444, 0x27),
           9016: (0x1a06c7, 0x90), 9019: (0x1a0782, 0x61)}
LABELS = {(3081, 0x87): 'root_config_read', (3254, 0x98): 'verifier_all_yield',
          (3254, 0xd0): 'verifier_all_return', (3277, 0x63): 'config_await_yield',
          (3277, 0xee): 'onConfigured_call_attempt', (3277, 0x10e): 'onConfigured_generator_return_attempt',
          (3277, 0x147): 'onConfigured_handled_error_attempt', (3277, 0x165): 'onConfigured_finally_attempt',
          (3277, 0xf4): 'config_normal_return', (3277, 0x14f): 'config_handled_error_return',
          (9016, 0x16): 'selected_experience_count', (9016, 0x3f): 'startup_signal_yield',
          (9016, 0x50): 'startup_after_await_mounted_check', (9016, 0x80): 'startup_animation_call_attempt',
          (1959, 0x69): 'timing_callback_call_attempt', (5744, 0x20): 'loadComponents_pipe_call_attempt'}


def catalog(bundle, decoder):
    """Parse only pinned HBC; import an existing hermes-dec, never execute JS."""
    from io import BytesIO
    import sys
    data = bundle.read_bytes()
    require(digest(data) == HOME_HASH, 'home_hash')
    sys.path.insert(0, str(decoder.resolve()))
    from hermes_dec.parsers.hbc_file_parser import HBCReader
    from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode
    reader = HBCReader()
    reader.read_whole_file(BytesIO(data))
    require(reader.header.version == 89, 'home_version')
    functions = {}
    for fid in FUNCTIONS:
        h = reader.function_headers[fid]
        if fid in HEADERS:
            require((h.offset, h.bytecodeSizeInBytes) == HEADERS[fid], 'reviewed_hbc_header')
        instructions = {}
        for i in parse_hbc_bytecode(h, reader):
            if i.inst.name in ('Call2', 'Ret', 'JmpFalse'):
                args = [getattr(i, 'arg' + str(n + 1)) for n in range(len(i.inst.operands))]
                instructions[i.original_pos] = (i.inst.name, args, data[h.offset + i.original_pos:h.offset + i.next_pos])
        functions[fid] = dict(offset=h.offset, size=h.bytecodeSizeInBytes,
                              prefix=data[h.offset:h.offset + 8], instructions=instructions)
    return data, functions


def returned_path(r, program):
    # Host Program::file_name+0x80, path MSVC wstring. Offsets checked with the
    # exact compile options and pinned host LoadProgram return disassembly.
    p = program + 0x80
    n, capacity = r.q(p + 0x10), r.q(p + 0x18)
    require(0 < n <= 1024 and n <= capacity <= 32768, 'loader_path_bound')
    at = r.q(p) if capacity >= 8 else p
    try:
        return Path(r.raw(at, n * 2).decode('utf-16-le'))
    except UnicodeError:
        raise DecodeError('loader_path_encoding') from None


class Observer:
    mode = 'retail_config'
    max_decode_unknowns = 16
    probes, windows = INITIAL, WINDOWS
    conclusion = 'JS consumer observations only; await outstanding is not proof of an unsettled native promise'

    def __init__(self, data, functions, selected_pui, token):
        self.data, self.functions, self.selected_pui, self.token = data, functions, selected_pui.resolve(), token
        self.pending_base = None

    def model(self, emit, bases):
        self.decoder = Model(self, emit, bases)
        return self.decoder

    def validate_attached(self, cap, bases):
        from capture_retail_home_config_1360 import check_barrier
        check_barrier(cap, self.token)
        matches = [base for base, _, name in cap.modules() if Path(name).resolve() == self.exe.resolve()]
        require(len(matches) == 1 and matches[0] == bases['host'], 'host_mapping_identity')

    def transition(self):
        if self.pending_base is None:
            return None
        base, self.pending_base = self.pending_base, None
        return ACTIVE, {'pui': base}


class Model:
    phase, ready = 'retail_config', None

    def __init__(self, observer, emit, bases):
        self.observer, self.emit, self.bases = observer, emit, bases
        self.owner, self.buffer_base = None, None
        self.closed, self.seen, self.awaiting = False, {}, set()
        self.prefixes = {}
        for fid, f in observer.functions.items():
            self.prefixes.setdefault(f['prefix'], []).append((fid, f))

    def thread_boundary(self, tid):
        pass  # no thread/frame pointer is used as a persistent request identity

    def runtime_owner(self, r, context):
        data = r.q(context + 0x30)
        bridge = r.typed(r.q(data + 0x18), 'ReactNative.Bridge.ReactBridge')
        executor = r.typed(r.q(bridge + 0x18), 'ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor')
        native = r.q(executor + 0x80)
        vtable = r.q(native)
        require(vtable == self.bases['extensions'] + 0x95320 and
                r.q(vtable + 0x28) == self.bases['extensions'] + 0x2c1f0 and
                r.q(vtable + 0x30) == self.bases['extensions'] + 0x2c8d0, 'not_audited_jsi_executor')
        impl = r.q(native + 0x78)
        require(r.q(impl) == self.bases['hermes'] + 0x26c348, 'not_hermes_backend')
        runtime = r.q(impl + 0x70)
        require(runtime >= 0x10000, 'runtime_null')
        return runtime

    def hit(self, name, r, regs, now, tid=None):
        if self.closed:
            raise ScopeExcluded('config_coverage_closed')
        if name == 'loader':
            p = regs['rax']
            if not p:
                raise ScopeExcluded('loader_null_return')
            path = returned_path(r, p)
            if path.name != PINS['pui'][0]:
                raise ScopeExcluded('loader_other_module')
            require(path.resolve() == self.observer.selected_pui, 'loader_unreviewed_pui')
            base, size = r.q(p + 0xb0), r.q(p + 0xc8)
            require(base >= 0x10000 and base % 4096 == 0 and size >= 0x860000, 'loader_pui_extent')
            require('pui' not in self.bases, 'pui_loaded_before_coverage')
            self.observer.pending_base = base
            self.emit(dict(event='pui_loader_boundary', base=hex(base), tid=tid))
            return
        if name == 'owner':
            # Invoke(this, name, args): bind only the actual application start,
            # not an earlier event on a not-yet-initialized pooled context.
            if r.string(regs['rsi'], {'runApplication'}) != 'runApplication':
                raise ScopeExcluded('owner_other_method')
            handler = r.typed(regs['rdi'], '.JavaScriptModuleInvocationHandler')
            instance = r.q(handler + 0x10)
            context = r.instance_context(instance)
            identity = r.identity(context)
            require(identity['user'] == 0x10010029, 'home_user_mismatch')
            runtime = self.runtime_owner(r, context)
            if self.owner:
                require((context, runtime, identity['generation']) ==
                        (self.owner['context'], self.owner['runtime'], self.owner['generation']), 'home_generation_changed')
            else:
                self.owner = dict(context=context, runtime=runtime, **{k: identity[k] for k in ('generation', 'user')})
                self.emit(dict(event='home_owner', generation=identity['generation'], user=identity['user'], tid=tid))
            return
        if not self.owner or regs['r12'] != self.owner['runtime']:
            raise ScopeExcluded('not_bound_home_runtime')
        # Revalidate the managed owner/backlinks at every admitted hit. A moved,
        # terminated or reused object fails closed rather than becoming new scope.
        identity = r.identity(self.owner['context'])
        require(identity['generation'] == self.owner['generation'] and identity['user'] == self.owner['user'] and
                r.byte(self.owner['context'] + 0x92) == 0 and
                self.runtime_owner(r, self.owner['context']) == regs['r12'], 'home_owner_expired')
        ip, frame = regs['r10'], regs['r15']
        cb = r.q(regs['rbp'] - 0x190)
        begin = r.q(cb + 0x10)
        offset = ip - begin
        matches = [(fid, f) for fid, f in self.prefixes.get(r.raw(begin, 8), [])
                   if offset in f['instructions'] and f['instructions'][offset][0] ==
                   {'call2': 'Call2', 'ret': 'Ret', 'false': 'JmpFalse'}[name]]
        if not matches:
            raise ScopeExcluded('hbc_function_not_traced')
        if self.buffer_base is None:
            candidates = []
            for fid, f in matches:
                at = begin - f['offset']
                if at < 0x10000 or r.raw(at, 8) != self.observer.data[:8]:
                    continue
                # Only a header candidate can spend this additional fixed budget.
                r.remaining += len(self.observer.data)
                h = hashlib.sha256()
                for start in range(0, len(self.observer.data), 2048):
                    h.update(r.raw(at + start, min(2048, len(self.observer.data) - start)))
                if h.hexdigest() == HOME_HASH:
                    candidates.append(at)
            require(len(candidates) == 1, 'home_live_buffer_hash')
            self.buffer_base = candidates[0]
            self.emit(dict(event='home_buffer_verified', sha256=HOME_HASH,
                           generation=self.owner['generation'], user=self.owner['user']))
        matches = [(fid, f) for fid, f in matches if begin == self.buffer_base + f['offset']]
        require(len(matches) == 1, 'hbc_function_identity')
        fid, f = matches[0]
        op, args, code = f['instructions'][offset]
        require(r.raw(ip, len(code)) == code and frame >= 0x10000 and frame % 8 == 0,
                'hbc_instruction_or_frame')
        require(r.q(self.owner['runtime'] + 0x2440) + 0x10 == frame, 'hermes_frame_backlink')
        row = dict(event='home_hbc', generation=self.owner['generation'], user=self.owner['user'],
                   fid=fid, offset=offset, opcode=op, tid=tid,
                   label=LABELS.get((fid, offset), 'reviewed_consumer_site'))
        # The only numeric payload admitted is the selected experience count.
        if fid == 9016 and offset == 0x16:
            row['selected_count'] = count_value(r.q(frame + args[1] * 8))
        if fid == 3081 and offset == 0x87:
            row['config_kind'] = value_kind(r.q(frame + args[1] * 8))
        if op == 'Ret':
            row['return_kind'] = value_kind(r.q(frame + args[0] * 8))
        self.record(row, now)

    def record(self, row, now):
        fid, pc = row['fid'], row['offset']
        key = (fid, pc)
        self.seen[key] = self.seen.get(key, 0) + 1
        require(sum(self.seen.values()) <= 4096, 'home_record_budget')
        if fid == 3277 and pc == 0x63:
            self.awaiting.add('config_verifiers')
        elif fid == 3277 and pc in (0xf4, 0x14f):
            self.awaiting.discard('config_verifiers')
        if fid == 9016 and pc == 0x3f:
            self.awaiting.add('startup_signal')
        elif fid == 9016 and pc in (0x50, 0x80):
            self.awaiting.discard('startup_signal')
        self.emit(row)

    def close_coverage(self, now, reason):
        self.closed = True

    def outstanding(self, now):
        return [dict(boundary=name, status='await_continuation_not_observed',
                     native_promise_state='unknown', association='generation only; repeated invocations unpaired',
                     generation=self.owner['generation'], user=self.owner['user'])
                for name in sorted(self.awaiting)] if self.owner else []
