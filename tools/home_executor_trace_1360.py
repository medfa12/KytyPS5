"""Serialization -> queued closure -> executor call/return, never JS acceptance."""
import struct

from home_gate_trace_1360 import ScopeExcluded, DecodeError, require
from home_delivery_trace_1360 import DeliveryCorrelator, abi_slots as delivery_slots, WINDOWS as DELIVERY_WINDOWS

PROBES = {'delivery_acquire': ('vsh', 0x196e17),
          'executor_queued': ('pui', 0x1e291f),
          'executor_call': ('pui', 0x1cbfc0),
          'executor_return': ('pui', 0x1cbfc3)}
WINDOWS = {key: list(value) for key, value in DELIVERY_WINDOWS.items()}
WINDOWS['pui'] += [(0x93024, 7), (0x93035, 0x29), (0x1e2850, 0xd2),
                   (0x92390, 0x17), (0x1cbf90, 0x37)]


def abi_slots(images):
    slots = delivery_slots(images)
    for name, address, prefix in (('queued_closure', 0x93024, '488b3d'),
                                  ('bridge_method', 0x1e2918, '4c8b15'),
                                  ('executor_method', 0x1cbfb9, '4c8b15')):
        code = images['pui'].at(address, 7)
        require(code[:3] == bytes.fromhex(prefix), 'executor_got_abi')
        slots[name] = ('pui', address + 7 + struct.unpack('<i', code[3:])[0])
    checks = {
        0x1e2860: '4c8bff', 0x1e28c5: '498b4710', 0x1e28c9: '488b4020',
        0x1e28cd: '49637720', 0x1e28d1: '498b5718',
        0x92394: '488b4730', 0x9239d: '488b4018',
        0x1e28e0: '498b7f10', 0x1e28f6: '49634720',
        0x1e28fe: '498b4718', 0x1e2908: 'ff5048',
        0x1e290e: '488b75c8', 0x1e2912: '498bfe',
        0x1e2915: '498b06', 0x1e291f: 'ff50b0',
        0x1cbf9c: '4c8bef', 0x1cbf9f: '488975f0', 0x1cbfa3: '488955e8',
        0x1cbfa7: '498b4518', 0x1cbfae: '486375f0',
        0x1cbfc0: 'ff50e0', 0x1cbfc3: '488945e0'}
    for address, expected in checks.items():
        raw = bytes.fromhex(expected)
        require(images['pui'].at(address, len(raw)) == raw, 'executor_register_abi')
    call = images['pui'].at(0x1e28db, 5)
    require(call[0] == 0xe8 and 0x1e28e0 + struct.unpack('<i', call[1:])[0] == 0x327790,
            'executor_monitor_call_abi')
    getter = images['pui'].at(0x1e28e4, 5)
    require(getter[0] == 0xe8 and 0x1e28e9 + struct.unpack('<i', getter[1:])[0] == 0x3103b0,
            'executor_bridge_getter_call_abi')
    return slots


class ExecutorCorrelator(DeliveryCorrelator):
    phase = 'executor'

    def hit(self, name, r, regs, now, tid=None):
        if name == 'delivery_acquire':
            return super().hit(name, r, regs, now, tid)
        if self.closed_reason is not None:
            raise ScopeExcluded('executor_coverage_closed')
        require(name in PROBES, 'executor_probe_unknown')
        # Only the audited direct/tail interface call establishes that this
        # frame's R15 belongs to the queued closure. Other callers fail closed
        # before any dereference of their unrelated preserved registers.
        if name != 'executor_queued':
            require(r.q(regs['rbp'] + 8) == self.bases['pui'] + 0x1e2922,
                    'executor_frame_backlink')
        # R15 is the live queued closure, preserved by ReactBridge's prologue.
        # No saved closure/bridge/frame address is used at a subsequent hit.
        closure = self.exact(r, regs['r15'], 'queued_closure')
        context = r.q(closure + 0x10)
        identity = r.identity(context)
        if identity['user'] != self.user:
            raise ScopeExcluded('executor_foreign_user')
        self.live_owner(r, identity, tid)
        ident = r.i(closure + 0x20)
        require(0 <= ident <= 0x7fffffff, 'executor_signed_id')
        # context+20 is an optional monitor, NOT the bridge. get__bridge
        # dereferences context+30's Data and returns Data+18.
        data = r.q(context + 0x30, 'executor.bridge_data')
        bridge = r.typed(r.q(data + 0x18), 'ReactNative.Bridge.ReactBridge', 'executor.bridge')
        if name == 'executor_queued':
            require(regs['rdi'] == regs['r14'] == bridge and regs['rax'] == r.q(bridge) and
                    regs['rsi'] == ident and regs['r10'] == self.got(r, 'bridge_method'),
                    'executor_queued_backlink')
        else:
            # The queued method converts closure+18 via virtual +48 before the
            # call; the converted argument is not the original closure pointer.
            require(regs['r13'] == regs['r14'] == bridge and
                    r.i(regs['rbp'] - 0x10) == ident, 'executor_frame_backlink')
        token = (*self.owner(identity), ident)
        require(token not in self.poisoned, 'executor_ids_poisoned')
        found = self.index.get(token)
        row = dict(event=name, tid=tid, **identity, callback_id=ident, correlated=False,
                   request=None, reason='no_acquisition')
        if name == 'executor_call':
            executor = r.q(bridge + 0x18)
            require(executor >= 0x10000 and regs['rdi'] == executor and
                    regs['rsi'] == ident and regs['rdx'] == r.q(regs['rbp'] - 0x18) and
                    regs['rax'] == r.q(executor) and
                    regs['r10'] == self.got(r, 'executor_method'), 'executor_interface_registers')
            target = r.q(regs['rax'] - 0x20)
            require(target >= 0x10000, 'executor_target_null')
            row['dispatch_target'] = hex(target)
            row['target_kind'] = ('js_executor' if target == self.bases['pui'] + 0x64f50 else
                                  'websocket_executor' if target == self.bases['pui'] + 0x80c60 else
                                  'other_or_interface_thunk')
        if name == 'executor_return':
            # Returned queue can be null; never interpret it as callback success.
            row['return_nonnull'] = bool(regs['rax'])
        if found:
            key, side = found
            req = self.requests[key]
            stages = req.setdefault('executor_stages', {'resolve': set(), 'reject': set()})[side]
            if name in stages:
                self.invalidate(key, 'executor_boundary_repeated', tid)
                raise DecodeError('executor_boundary_repeated')
            required = {'executor_queued': set(), 'executor_call': {'executor_queued'},
                        'executor_return': {'executor_queued', 'executor_call'}}[name]
            matched = required <= stages
            stages.add(name)
            row.update(request=req['request'], side=side, context=req['context'],
                       item_count=req['item_count'], correlated=matched,
                       reason='matching_acquired_chain' if matched else 'predecessor_not_observed',
                       seconds_since_serialization=now - req['started'])
            if not matched:
                self.invalidate(key, 'executor_predecessor_not_observed', tid)
            if name == 'executor_return' and matched:
                req['dispatched'].add(side)
        self.emit(row)

    def outstanding(self, now):
        rows = super().outstanding(now)
        by_id = {req['request']: req for req in self.requests.values()}
        for row in rows:
            row.pop('concrete_sides', None)
            if 'dispatch_sides' in row:
                row['executor_return_sides'] = row.pop('dispatch_sides')
            req = by_id.get(row['request'])
            if req:
                row['executor_stages'] = {side: sorted(stages) for side, stages in
                                          req.get('executor_stages', {}).items()}
        return rows
