"""Pure, pinned retail native NP-connect decoding. No process APIs or payloads.

This observer follows one real retained connect. It does not create/accept an
IPC connection, inspect an account store, or read an authentication payload.
"""
from pathlib import Path
import re
import struct

from home_gate_trace_1360 import DecodeError, ScopeExcluded, Image, require, digest

EXE_HASH = '16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd'
CORE_HASH = 'f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434'
CONTRACT = 'retail-np-connect-fix3-v1'
CORE_SITES = {'server_entry': 0xcaaa80, 'new_client': 0xcaae10,
              'session_register': 0xcb2020, 'session_return': 0xcaae83,
              'new_client_result': 0xcaaf86}
HOST_SITES = {'submit': 0x937913, 'submit_return': 0x937918,
              'dispatch_connect': 0x93e054, 'accept': 0x93e085, 'accept_return': 0x93e08a}
SITES = {**{k: ('core', v) for k, v in CORE_SITES.items()},
         **{k: ('host', v) for k, v in HOST_SITES.items()}}
INITIAL = {k: SITES[k] for k in ('server_entry', 'submit', 'dispatch_connect', 'new_client')}
CHECKS = {
    'host': {
        0x937913: 'e8287ca8ff90488d4d10e8fe42000090',
        0x937918: '90488d4d10e8fe42000090488bb53801',
        0x93e054: 'e8671a0000904989c64c8b8d38010000',
        0x93e085: 'e8563ba8ff904585f64c8bbdc8040000',
        0x93e08a: '904585f64c8bbdc80400000f853f0300',
        0x93d3c0: '5541574156415541545657534881ec98',
        0x937500: '554157415641545657534881ec200200',
        0x9377de: 'ff50109085c00f85e102000083bd9800',
    },
    'core': {
        0xcaaa80: '554889e5488b07488b7708488b571048',
        0xcaae10: '554889e54157415641554154534883ec',
        0xcb2020: '807f0800743a554889e5415741564155',
        0xcaae83: '4189c685c00f88b7000000498b471048',
        0xcaaf86: '4489f04883c4185b415c415d415e415f',
    },
}
WINDOWS = {k: [(rva, len(bytes.fromhex(raw))) for rva, raw in rows.items()]
           for k, rows in CHECKS.items()}
SERVER_TABLE = 0x212abf0


def audit(images):
    for key, rows in CHECKS.items():
        for rva, raw in rows.items():
            require(images[key].at(rva, len(bytes.fromhex(raw))) == bytes.fromhex(raw), 'np_connect_abi_' + key)
    for rva, target in ((0x937913, 0x3bf540), (0x93e054, 0x93fac0), (0x93e085, 0x3c1be0)):
        code = images['host'].at(rva, 5)
        require(code[0] == 0xe8 and rva + 5 + struct.unpack('<i', code[1:])[0] == target,
                'np_connect_direct_call')
    pe = images['host'].pe
    require(all(pe.get_qword_at_rva(SERVER_TABLE + slot) - pe.OPTIONAL_HEADER.ImageBase == target
                for slot, target in ((0x10, 0x93c6e0), (0x28, 0x93c9a0))), 'np_connect_server_vtable')


def images_from_paths(exe, core):
    images = {'host': HostImage(exe.read_bytes()), 'core': Image(core.read_bytes(), CORE_HASH)}
    audit(images)
    return images


class HostImage:
    def __init__(self, data):
        require(digest(data) == EXE_HASH, 'np_connect_candidate_pin')
        import pefile
        self.pe = pefile.PE(data=data, fast_load=True)
        require(self.pe.FILE_HEADER.Machine == 0x8664 and
                self.pe.OPTIONAL_HEADER.Magic == 0x20b, 'np_connect_host_amd64')

    def at(self, rva, size):
        require(size > 0, 'np_connect_code_size')
        sections = [s for s in self.pe.sections if s.VirtualAddress <= rva and
                    rva + size <= s.VirtualAddress + min(s.Misc_VirtualSize, s.SizeOfRawData)]
        require(len(sections) == 1 and sections[0].Characteristics & 0x20000000,
                'np_connect_host_executable')
        data = self.pe.get_data(rva, size)
        require(len(data) == size, 'np_connect_host_code_bounds')
        return data


def prefix_provenance(data, core_path, core_image, pid, token):
    """Bind a small pre-guest log prefix to the exact PID/token and retail Core.

    No log text is returned. This is firmware IPC admission evidence, NOT PSN
    authentication. Live code/vtables and barrier state are checked separately.
    """
    require(len(data) <= 4 * 1024 * 1024, 'np_connect_prefix_budget')
    require(type(pid) is int and 0 < pid <= 0xffffffff and
            re.fullmatch('[0-9a-f]{32}', token or '') is not None, 'np_connect_barrier_identity')
    text = data.decode('utf-8', errors='replace')
    placements = re.findall(r'^ps5-vsh: place SceShellCore\.elf base=(0x[\da-fA-F]+) mapped=(0x[\da-fA-F]+)(?: .*|)$', text, re.M)
    loads = re.findall(r'^Loading: (.*[\\/]SceShellCore\.elf)\s*$', text, re.M)
    require(len(placements) == len(loads) == 1 and
            Path(loads[0].strip()).resolve() == core_path.resolve(), 'np_connect_core_selection')
    base, extent = map(lambda x: int(x, 16), placements[0])
    require(base >= 0x10000 and base % 4096 == 0 and
            all(s[3] + s[6] <= extent for s in core_image.loads), 'np_connect_core_extent')
    admitted = re.findall(r'^NP: source-bound Core pid=(\d+) adapter install=0 \(server not yet published\)\s*$', text, re.M)
    require(len(admitted) == 1 and 0 < int(admitted[0]) <= 0x7fffffff, 'np_connect_source_bound_admission')
    barriers = re.findall(r'^pre-guest-barrier: ready pid=(\d+) timeout_ms=(\d+) phase=relocated-before-guest names=(\S+)\s*$', text, re.M)
    require(len(barriers) == 1 and int(barriers[0][0]) == pid and
            barriers[0][2] == f'Local\\KytyPreGuest-{pid}-{token}', 'np_connect_prefix_barrier')
    require(not re.search(r'^pre-guest-barrier: (released|timeout|failed)', text, re.M), 'np_connect_prefix_already_released')
    return {'base': base, 'extent': extent, 'core_pid': int(admitted[0]),
            'prefix_sha256': digest(data), 'barrier_token_sha256': digest(token.encode())}


def signed32(value):
    value &= 0xffffffff
    return value - 0x100000000 if value & 0x80000000 else value


class Observer:
    mode, max_decode_unknowns = 'retail_np_connect', 1
    probes, windows = INITIAL, WINDOWS
    conclusion = 'one local retained NP connect only; no PSN authentication or native client readiness inference'

    def __init__(self, core_pid, validate):
        self.core_pid, self.validate, self.pending = core_pid, validate, None

    def validate_attached(self, cap, bases):
        self.validate(cap)

    def model(self, emit, bases):
        self.decoder = Model(self, emit, bases)
        return self.decoder

    def transition(self):
        result, self.pending = self.pending, None
        return result

    @staticmethod
    def handoff(*args):
        # Reuse the existing code-only, stopped-event migration/cleanup protocol.
        # Its old executable profile is not changed or used for admission here.
        from retail_plus_trace_1360 import handoff
        return handoff(*args)


class Model:
    phase, ready, complete = 'retail_np_connect', None, False

    def __init__(self, observer, emit, bases):
        self.observer, self.emit, self.bases = observer, emit, bases
        self.probes, self.req, self.bound, self.native = dict(INITIAL), None, None, None
        self.server_tids, self.closed = set(), False
        self.steps = []

    def row(self, event, **facts):
        self.steps.append(event)
        self.emit(dict(event=event, connection=1, **facts))

    def change(self, changes):
        items = list(self.probes.items())
        for slot, name in changes.items():
            items[slot] = name, SITES[name]
        updated = dict(items)
        require(len(updated) == 4, 'np_connect_unique_probes')
        self.probes = updated
        self.observer.pending = updated, {}

    def scope(self, ok):
        if not ok:
            raise ScopeExcluded('np_connect_other_local_connection')

    def server(self, r, pointer):
        core, host = self.bases['core'], self.bases['host']
        require(pointer != 0 and r.q(core + 0x213cad8) == pointer and
                r.q(pointer) == host + SERVER_TABLE and
                r.q(host + SERVER_TABLE + 0x10) == host + 0x93c6e0 and
                r.q(host + SERVER_TABLE + 0x28) == host + 0x93c9a0, 'np_connect_server_incarnation')

    @staticmethod
    def host_frame(regs):
        require(regs['rbp'] == regs['rsp'] + 0x80, 'np_connect_host_frame')

    def hit(self, name, r, regs, now, tid=None):
        require(not self.closed and not self.complete and type(tid) is int and tid > 0, 'np_connect_capture_open')
        require(name in self.probes and SITES[name] == self.probes[name], 'np_connect_active_probe')
        core = self.bases['core']
        if name == 'server_entry':
            require(regs['rdi'] == core + 0x213cae8, 'np_connect_server_entry_args')
            self.server(r, r.q(regs['rdi']))
            require(tid not in self.server_tids, 'np_connect_repeated_server_entry')
            self.server_tids.add(tid)
            self.row('np_server_entry', tid=tid)
        elif name == 'submit':
            self.host_frame(regs)
            identity = regs['r8']
            # Client and server need not have the same PID: a source-bound UI
            # owner can connect to Core. The audited production authorize hook
            # has already succeeded before this site; positive PID alone is
            # NOT treated as authorization. Match the actual retained owner.
            caller_pid = identity & 0xffffffff
            require(0 < caller_pid <= 0x7fffffff and 0 < identity >> 32 <= 0x7fffffff,
                    'np_connect_retained_owner')
            require(self.req is None, 'np_connect_single_retained_submit')
            state, handle = regs['r15'], regs['r9']
            require(handle > 0 and r.q(state + 0x18) == handle and r.q(state + 0x10) == identity and
                    r.i(state + 0x38) == 0, 'np_connect_retained_state')
            # Read only local vector metadata. The audited retained route passes
            # an empty connect argument. Never follow any argument/payload bytes.
            require(r.q(regs['rbp'] + 0x140) == r.q(regs['rbp'] + 0x148) == 0,
                    'np_connect_empty_local_argument')
            require(regs['rcx'] >= 0x10000 and r.q(regs['rsp'] + 0x30) > 0, 'np_connect_submit_server')
            self.req = dict(client=handle, owner=identity, caller_pid=caller_pid, core=regs['rcx'], frame=regs['rbp'],
                            tid=tid, submitted=now, submit_return=False)
            self.row('np_retained_submit_before_enqueue', tid=tid)
            # Arm the return before enqueue can run. Once the native callback
            # enters, its dispatch already proves enqueue even if this return
            # raced it and has not been observed. Never infer missing execution.
            self.change({0: 'submit_return', 1: 'session_register'})
        elif name == 'submit_return':
            self.scope(self.req is not None and tid == self.req['tid'])
            self.host_frame(regs)
            require(regs['rbp'] == self.req['frame'] and not self.req['submit_return'], 'np_connect_submit_return_frame')
            error = r.i(regs['rbp'] + 0x10)
            require(error >= 0, 'np_connect_submit_errno')
            self.req['submit_return'] = True
            if not error:
                require(r.q(regs['rbp'] + 0x160) != 0, 'np_connect_real_ticket')
            self.row('np_retained_enqueue_return', tid=tid, syscall_errno=error, ticket_present=not bool(error))
            if error:
                self.complete = True
        elif name == 'dispatch_connect':
            self.scope(self.req is not None)
            self.host_frame(regs)
            fp = regs['rbp']
            self.scope(r.q(fp + 0x138) == self.req['client'])
            require(r.i(fp + 0x130) == 1 and r.q(fp + 0x148) == self.req['owner'] and
                    r.q(fp + 0x4c8) + 0x2d0 == self.req['core'], 'np_connect_dispatch_identity')
            require(self.bound is None, 'np_connect_duplicate_dispatch')
            args = regs['r9']
            require(regs['r8'] == 0x20 and args == fp + 0x3a0, 'np_connect_callback_slot')
            handler, server, config = r.q(args), r.q(args + 8), r.q(args + 16)
            self.server(r, server)
            require(handler == core + 0x213cb08 and r.q(handler) == core + 0x202e0a0 and
                    r.q(core + 0x202e0c0) == core + 0xcaae10 and
                    config == fp + 0x230 and r.i(config) == self.req['caller_pid'] and
                    r.q(config + 0x148) == 0x10 and r.q(args + 0x20) == 0,
                    'np_connect_native_callback_abi')
            receiver = r.q(fp + 0x4d8)
            require(receiver & 0xffffffff == self.observer.core_pid and receiver >> 32 > 0,
                    'np_connect_dispatch_receiver')
            self.bound = dict(tid=tid, frame=fp, rsp=regs['rsp'], handler=handler,
                              server=server, config=config, receiver=receiver)
            self.row('np_connect_dispatched_before_native', tid=tid,
                     server_entry_observed=tid in self.server_tids)
        elif name == 'new_client':
            self.scope(self.bound is not None and tid == self.bound['tid'])
            b = self.bound
            require(self.native is None and regs['rdi'] == b['handler'] and
                    regs['rsi'] == b['server'] and regs['rdx'] == b['config'], 'np_connect_native_entry_identity')
            require(r.q(b['handler']) == core + 0x202e0a0 and
                    r.q(core + 0x202e0c0) == core + 0xcaae10, 'np_connect_native_vtable')
            manager = r.q(b['handler'] + 8)
            require(manager >= 0x10000, 'np_connect_session_manager')
            self.native = dict(sp=regs['rsp'], manager=manager, register=False, register_return=False,
                               result=None, session=None, storage=None)
            self.row('np_new_client_enter', tid=tid)
            self.change({0: 'new_client_result', 2: 'session_return'})
        elif name == 'session_register':
            self.scope(self.native is not None and tid == self.bound['tid'])
            n = self.native
            require(not n['register'] and regs['rdi'] == n['manager'] and
                    regs['rsi'] >= 0x10000 and regs['rdx'] >= 0x10000, 'np_connect_session_register_identity')
            require(r.q(regs['rsp']) == core + 0xcaae83, 'np_connect_session_register_caller')
            n.update(register=True, session=regs['rsi'], storage=regs['rdx'])
            self.row('np_session_register_enter', tid=tid)
        elif name == 'session_return':
            self.scope(self.native is not None and tid == self.bound['tid'])
            n = self.native
            require(n['register'] and not n['register_return'] and regs['rsp'] + 0x48 == n['sp'],
                    'np_connect_session_return_frame')
            n['register_return'] = True
            n['register_result'] = signed32(regs['rax'])
            self.row('np_session_register_return', tid=tid, service_result=n['register_result'])
        elif name == 'new_client_result':
            self.scope(self.native is not None and tid == self.bound['tid'])
            n = self.native
            require(n['result'] is None and regs['rsp'] + 0x48 == n['sp'] and
                    regs['r15'] == self.bound['handler'], 'np_connect_native_result_frame')
            n['result'] = signed32(regs['r14'])
            require(n['result'] != 0 or n['register_return'] and n.get('register_result') == 0,
                    'np_connect_success_requires_session_registration')
            self.row('np_new_client_result', tid=tid, service_result=n['result'])
            self.change({2: 'accept', 3: 'accept_return'})
        elif name == 'accept':
            self.scope(self.bound is not None and tid == self.bound['tid'])
            self.host_frame(regs)
            b, n = self.bound, self.native
            require(n is not None and n['result'] is not None and regs['rbp'] == b['frame'] and
                    regs['rcx'] == self.req['core'] and regs['r9'] == self.req['client'] and
                    regs['r8'] == b['receiver'] and
                    regs['rdx'] == b['frame'] + 0x420 and 'accept_buffer' not in b,
                    'np_connect_accept_identity')
            accepted = r.i(regs['rsp'] + 0x20)
            require(accepted == n['result'], 'np_connect_accept_native_result')
            b['accept_buffer'] = regs['rdx']
            self.row('np_core_accept_call', tid=tid, service_result=accepted)
        elif name == 'accept_return':
            self.scope(self.bound is not None and tid == self.bound['tid'])
            self.host_frame(regs)
            b = self.bound
            require('accept_buffer' in b and regs['rbp'] == b['frame'], 'np_connect_accept_return_frame')
            error = r.i(b['accept_buffer'])
            require(error >= 0, 'np_connect_accept_errno')
            self.row('np_core_accept_return', tid=tid, syscall_errno=error,
                     native_acceptance=self.native['result'], completion_published=error == 0,
                     consumer_completion_observed=False)
            self.complete = True
        else:
            raise DecodeError('np_connect_unknown_site')

    def thread_boundary(self, tid):
        owned = {self.req['tid']} if self.req else set()
        if self.bound:
            owned.add(self.bound['tid'])
        if tid in owned:
            self.row('np_connect_owned_thread_lifecycle_gap', tid=tid)
            raise DecodeError('np_connect_owned_thread_lifecycle_gap')

    def close_coverage(self, now, reason):
        self.closed = True

    def outstanding(self, now):
        return [dict(connection=1, last_observed=self.steps[-1] if self.steps else 'no_local_connect_observed',
                     native_client_ready='not established', psn_authenticated=False)] if not self.complete else []
