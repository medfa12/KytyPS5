"""Pinned native particle/readiness observer; no JS, GPU or Home-ready inference."""
import struct
from home_gate_trace_1360 import require, ScopeExcluded

SHA256 = 'cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09'
SITES = {
    'job': {'particle_mode': 0xeb590, 'job_gate': 0xe7fde,
            'job_submit': 0x155fd0, 'job_submit_return': 0xe80eb},
    'route': {'initialize_entry': 0xd9330, 'update_entry': 0xd9460,
              'resource_entry': 0xeb6e0, 'particle_entry': 0xea340},
    'particle0': {'particle_entry': 0xea340, 'asset_return': 0xea40b,
                  'fallback_return': 0xea4fb, 'particle_complete': 0xea6a3},
    'particle1': {'particle_entry': 0xea340, 'asset_return': 0xea593,
                  'fallback_return': 0xea68e, 'particle_complete': 0xea6a3},
    'native': {'native_entry': 0xd9460, 'native_call': 0xd9480,
               'renderer_call': 0xd9501, 'renderer_after': 0xd9506},
}
ANCHORS = {0xea378: '4889bd30ffffff', 0xea340: '554889e541574156',
           0xd9330: '554889e55350bf38000000e880b4c400',
           0xeb6e0: '554889e5488b7f38e853ecffff31c05dc3',
           0xeb6e8: 'e853ecffff', 0xea40b: '4c89f74189c54989',
           0xea4fb: '85c00f85b4010000', 0xea593: '4989de488b9d30ff',
           0xea68e: '85c07525c683a007', 0xea6a3: '31c04881c4b80000',
           0xd9460: '554889e5535081fe', 0xd9480: 'e84b00000031c048',
           0xd9501: 'e86a6afeff498b7e', 0xd9506: '498b7e08e89153fe',
           0xd94d0: '554889e54156534989fe488b7f084889'}


def audit(image):
    for address, text in ANCHORS.items():
        raw = bytes.fromhex(text)
        require(image.at(address, len(raw)) == raw, 'transition_instruction_abi')


def signed(value):
    return struct.unpack('<i', struct.pack('<I', value & 0xffffffff))[0]


class Observer:
    def __init__(self, mode):
        require(mode in SITES, 'transition_mode')
        self.mode = mode
        self.probes = {name: ('shell', address) for name, address in SITES[mode].items()}
        self.windows = {'shell': [(address, max(8, len(bytes.fromhex(raw)))) for address, raw in ANCHORS.items()]}
        self.windows['shell'] += [(0xea340, 0x39c), (0xd9460, 0xe0), (0xbff70, 0x40)]
        if mode == 'route':
            self.windows['shell'] += [(0xd9330, 0x33)]
        if mode == 'job':
            from home_particle_job_trace_1360 import ANCHORS as JOB_ANCHORS
            self.windows['shell'] += [(a, max(8, len(bytes.fromhex(b)))) for a, b in JOB_ANCHORS.items()]
            self.windows['shell'] += [(0xeb590, 0x106), (0xe7fb0, 0x145), (0x5b9610, 0xb8)]

    def model(self, emit, bases):
        if self.mode == 'job':
            from home_particle_job_trace_1360 import Model as JobModel
            return JobModel(emit, bases['shell'])
        if self.mode == 'route':
            return RouteModel(emit, bases['shell'])
        return Model(emit, bases['shell'], self.mode)


class RouteModel:
    """Entry-only controls; only callback -> initializer is a paired edge.

    Dynamic outer callers are recorded, not named as an audited scheduler or
    managed caller. No constructor completion, asset result or readiness claim.
    """
    ready = False
    phase = 'route'

    def __init__(self, emit, base):
        self.emit, self.base = emit, base
        self.active, self.poisoned = {}, set()
        self.serial, self.closed = 0, False

    def thread_boundary(self, tid):
        if tid in self.active:
            self.emit(dict(event='route_unpaired', tid=tid, invocation=self.active[tid]['id'], reason='thread_boundary'))
        self.active.pop(tid, None)
        self.poisoned.discard(tid)

    def close_coverage(self, now, reason):
        self.closed = True

    def outstanding(self, now):
        return [dict(tid=tid, invocation=v['id'], stages=['resource_entry']) for tid, v in self.active.items()]

    @staticmethod
    def pointer(value):
        require(0x10000 <= value < 0x800000000000, 'route_pointer_bounds')
        return value

    def hit(self, name, r, g, now, tid=None):
        require(tid is not None and name in SITES['route'], 'route_probe_thread')
        if self.closed or tid in self.poisoned:
            raise ScopeExcluded('route_coverage_closed')
        try:
            self.decode(name, r, g, tid)
        except Exception:
            self.active.pop(tid, None)
            raise

    def decode(self, name, r, g, tid):
        stack = self.pointer(g['rsp'])
        caller = self.pointer(r.q(stack))
        if name in ('initialize_entry', 'update_entry'):
            # These calls are independent observations, never paired with a
            # pending async resource job merely because they share a thread.
            row = dict(event=name, tid=tid, caller_pc=hex(caller))
            if name == 'update_entry':
                require(g['rsi'] == 0xa0, 'native_flow_size')
                flow = self.pointer(g['rdi'])
                row.update(flow_word_0=r.i(flow), flow_word_4=r.i(flow + 4))
            self.emit(row)
            return
        if name == 'resource_entry':
            if tid in self.active:
                self.poisoned.add(tid)
                require(False, 'route_nested_or_unfinished')
            job = self.pointer(g['rdi'])
            vtable = self.pointer(r.q(job))
            require(vtable == self.base + 0x1169df0 and r.q(vtable + 0x18) == self.base + 0xeb6e0,
                    'route_callback_vtable')
            owner = self.pointer(r.q(job + 0x38))
            self.serial += 1
            require(self.serial <= 1024, 'route_invocation_budget')
            self.active[tid] = dict(id=self.serial, stack=stack, caller=caller,
                                    saved_rbp=g['rbp'], job=job, vtable=vtable, owner=owner)
            self.emit(dict(event=name, tid=tid, invocation=self.serial, caller_pc=hex(caller)))
            return
        require(caller == self.base + 0xeb6ed, 'route_particle_caller')
        if tid not in self.active:
            raise ScopeExcluded('route_no_observed_callback')
        v = self.active[tid]
        # Callback pushes RBP then CALL pushes its return PC. At initializer
        # entry RBP still belongs to the callback, before its own prologue.
        require(stack == v['stack'] - 16 and g['rbp'] == v['stack'] - 8,
                'route_callback_frame')
        require(r.q(v['stack']) == v['caller'] and r.q(g['rbp']) == v['saved_rbp'],
                'route_callback_backlink')
        require(r.q(v['job']) == v['vtable'] and r.q(v['vtable'] + 0x18) == self.base + 0xeb6e0,
                'route_fresh_job')
        require(r.q(v['job'] + 0x38) == v['owner'] == g['rdi'], 'route_fresh_owner')
        self.active.pop(tid)
        self.emit(dict(event=name, tid=tid, invocation=v['id'], callback_edge_verified=True))


class Model:
    ready = False

    def __init__(self, emit, base, mode):
        self.emit, self.base, self.phase = emit, base, mode
        self.active, self.poisoned = {}, set()
        self.serial, self.closed = 0, False

    def thread_boundary(self, tid):
        self.active.pop(tid, None)
        self.poisoned.discard(tid)

    def close_coverage(self, now, reason):
        self.closed = True

    def outstanding(self, now):
        return [{'invocation': v['id'], 'tid': tid, 'stages': sorted(v['stages'])}
                for tid, v in self.active.items()]

    def hit(self, name, r, regs, now, tid=None):
        require(tid is not None, 'transition_thread_required')
        if self.closed or tid in self.poisoned:
            raise ScopeExcluded('transition_coverage_closed')
        try:
            self.decode(name, r, regs, tid)
        except Exception:
            # A failed backlink invalidates this thread's association. A later
            # entry can start fresh; interior hits can never revive it.
            self.active.pop(tid, None)
            raise

    def decode(self, name, r, g, tid):
        require(name in SITES[self.phase], 'transition_probe')
        if name in ('particle_entry', 'native_entry'):
            if name == 'particle_entry':
                require(r.q(g['rsp']) == self.base + 0xeb6ed, 'particle_entry_caller')
                if tid in self.active:
                    self.poisoned.add(tid)
                    require(False, 'particle_nested_or_unfinished')
            else:
                require(g['rsi'] == 0xa0, 'native_flow_size')
                if tid in self.active:
                    self.emit(dict(event='native_previous_incomplete', invocation=self.active[tid]['id'], tid=tid))
            require(g['rdi'] >= 0x10000, 'transition_entry_owner')
            self.serial += 1
            require(self.serial <= 1024, 'transition_invocation_budget')
            v = {'id': self.serial, 'owner': g['rdi'], 'frame': g['rsp'] - 8,
                 'caller': r.q(g['rsp']), 'stages': set(), 'results': {}}
            self.active[tid] = v
            row = dict(event=name, invocation=v['id'], tid=tid)
            if name == 'native_entry':
                root = r.q(self.base + 0x13a8ee0)
                row['singleton_present'] = bool(root)
                row['renderer_present'] = bool(r.q(root + 8)) if root else False
                # A null singleton returns zero in original firmware. Never ready.
                row['caller_pc'] = hex(v['caller'])
            self.emit(row)
            return
        if tid not in self.active:
            raise ScopeExcluded('transition_no_observed_entry')
        v = self.active[tid]
        require(name not in v['stages'], 'transition_duplicate_stage')
        row = dict(event=name, invocation=v['id'], tid=tid)
        if self.phase.startswith('particle'):
            require(g['rbp'] == v['frame'] and r.q(g['rbp'] + 8) == self.base + 0xeb6ed,
                    'particle_frame_caller')
            require(r.q(g['rbp'] - 0xd0) == v['owner'], 'particle_fresh_owner')
            require(name not in v['stages'], 'particle_duplicate_stage')
            if name in ('asset_return', 'fallback_return'):
                if name == 'asset_return':
                    require('fallback_return' not in v['stages'], 'particle_stage_order')
                elif 'asset_return' in v['stages']:
                    require(v['results']['asset_return'] < 0, 'particle_unexpected_fallback')
                row['result'] = signed(g['rax'])
                v['results'][name] = row['result']
                row['path'] = 'genuine_asset_loader' if name == 'asset_return' else 'one_pixel_fallback'
                # Loader return alone is not successful upload/composition.
            else:
                require(bool(v['stages']), 'particle_missing_selected_path')
                require(v['results'].get('fallback_return') == 0 if 'fallback_return' in v['stages']
                        else v['results'].get('asset_return', -1) >= 0, 'particle_impossible_completion')
                row['selected_path'] = 'fallback' if 'fallback_return' in v['stages'] else 'asset'
                self.active.pop(tid)
        elif name == 'native_call':
            require(g['rbp'] == v['frame'] and r.q(g['rbp'] + 8) == v['caller'], 'native_outer_caller')
            require(g['rsi'] == g['rax'] == v['owner'], 'native_fresh_flow')
            root = r.q(self.base + 0x13a8ee0)
            require(root >= 0x10000 and g['rdi'] == root, 'native_fresh_singleton')
            v['singleton'] = root
            row['renderer_present'] = bool(r.q(root + 8))
        else:
            require('native_call' in v['stages'], 'native_missing_call')
            require(r.q(g['rbp'] + 8) == self.base + 0xd9485 and r.q(g['rbp']) == v['frame']
                    and r.q(v['frame'] + 8) == v['caller'], 'renderer_nested_caller')
            root = r.q(self.base + 0x13a8ee0)
            require(root == v['singleton'] == g['r14'] and g['rbx'] == v['owner'], 'renderer_fresh_owner')
            renderer = r.q(root + 8)
            require(renderer >= 0x10000, 'renderer_missing')
            if name == 'renderer_call':
                require(g['rdi'] == renderer and g['rsi'] == v['owner'], 'renderer_call_arguments')
                v['renderer'] = renderer
            else:
                require('renderer_call' in v['stages'] and renderer == v['renderer'], 'renderer_return_owner')
                self.active.pop(tid)
            row['renderer_ready_byte'] = r.byte(renderer)
            row['flow_word_0'] = r.i(v['owner'])
            row['flow_word_4'] = r.i(v['owner'] + 4)
        v['stages'].add(name)
        self.emit(row)
