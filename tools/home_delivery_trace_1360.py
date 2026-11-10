"""Four read-only delivery boundaries; acquisition is serialization, not enqueue.

No process APIs. Identity is reacquired on every hit; saved addresses are never
dereferenced. Callback dispatch is not JavaScript consumption.
"""
import struct

from home_gate_trace_1360 import DecodeError, ScopeExcluded, require
from home_queue_trace_1360 import QueueCorrelator, LOADS as QUEUE_LOADS

PROBES = {'delivery_acquire': ('vsh', 0x196e17),
          'delivery_resolve': ('pui', 0x1cba30),
          'delivery_reject': ('pui', 0x1cbaa0),
          'delivery_dispatch': ('pui', 0x127f15)}
LOADS = {k: QUEUE_LOADS[k] for k in ('promise', 'callback', 'resolve_method')}
LOADS['invoke_method'] = ('pui', 0x127f0e)
WINDOWS = {'vsh': [(0x196cb0, 0x22), (0x196d3a, 7), (0x196e02, 0x1a),
                   (0x4a136, 0x12), (0x4a195, 7)],
           'pui': [(0x1cba30, 0xd5), (0x127e90, 0x95), (0x87030, 0x4f),
                   (0x92d40, 0x10), (0x93063, 0x2b), (0x8b9, 0x25)]}
for image, address in LOADS.values():
    WINDOWS[image].append((address, 7))


def abi_slots(images):
    result = {}
    for name, (image, address) in LOADS.items():
        code = images[image].at(address, 7)
        require(code[:3] in (b'\x48\x8b\x3d', b'\x4c\x8b\x15'), 'delivery_got_abi')
        result[name] = (image, address + 7 + struct.unpack('<i', code[3:])[0])
    for image, address, expected in (
        ('vsh', 0x196e17, 'ff5098'), ('vsh', 0x196e02, '488b7dd0'),
        ('vsh', 0x196d3d, '488945c8'), ('pui', 0x127f15, 'ff9068ffffff'),
        ('pui', 0x127ed5, '488b4de8'), ('pui', 0x127ed9, '49634618'),
        ('pui', 0x127ee0, '8945e0'), ('pui', 0x127f04, '498bfe'),
        ('pui', 0x127f07, '486375e0'), ('pui', 0x92d44, '8a8792000000')):
        raw = bytes.fromhex(expected)
        require(images[image].at(address, len(raw)) == raw, 'delivery_register_abi')
    for image, windows in WINDOWS.items():
        for address, size in windows:
            images[image].at(address, size)
    return result


class DeliveryCorrelator(QueueCorrelator):
    # Only the stateless GOT/type/promise-owner helpers are reused. No queue state.
    phase, ready = 'delivery', None
    MAX_REQUESTS, MAX_GENERATIONS = 512, 16

    def __init__(self, emit, bases, slots, user):
        self.emit, self.bases, self.slots, self.user = emit, bases, slots, user
        self.requests, self.index, self.used, self.poisoned = {}, {}, set(), set()
        self.generations = set()
        self.serial, self.closed, self.closed_reason = 0, [], None

    @staticmethod
    def ids(key):
        return {(key[0], key[1], key[2]), (key[0], key[1], key[3])}

    def invalidate(self, key, reason, tid):
        req = self.requests.get(key)
        self.poisoned.update(self.ids(key))
        if req and not req['invalidated']:
            req['invalidated'] = True
            self.emit(dict(event='delivery_invalidated', request=req['request'],
                           generation=key[0], user=key[1], reason=reason, tid=tid))

    def live_owner(self, r, identity, tid):
        context = int(identity['pui_context'], 16)  # fresh identity, never retained pointer
        r.site = 'delivery.context.terminated'
        terminated = r.byte(context + 0x92)
        require(terminated in (0, 1), 'delivery_terminated_flag')
        if terminated:
            for key in self.requests:
                if key[:2] == self.owner(identity):
                    self.invalidate(key, 'context_terminated', tid)
            raise DecodeError('delivery_context_terminated')

    @staticmethod
    def result(r, p):
        f = r.fields(p)
        require('ctxId' in f, 'delivery_context_missing')
        context = r.value(f['ctxId'])
        require(type(context) is int and 0 <= context <= 1000000, 'delivery_context_bound')
        return context, r.item_count(p)

    def hit(self, name, r, regs, now, tid=None):
        if self.closed_reason is not None:
            raise ScopeExcluded('delivery_coverage_closed')
        if name == 'delivery_dispatch':
            self.dispatch(r, regs, now, tid)
            return
        require(name in PROBES, 'delivery_probe_unknown')
        key, identity = self.promise(r, regs['rdi'])
        self.live_owner(r, identity, tid)
        row = dict(event=name, tid=tid, **identity, resolve_id=key[2], reject_id=key[3])
        if name == 'delivery_acquire':
            require(regs['rax'] == r.q(regs['rdi']) and
                    regs['r10'] == self.got(r, 'resolve_method') and
                    r.q(regs['rbp'] - 0x30) == regs['rdi'] and
                    regs['r13'] == regs['rsi'] == r.q(regs['rbp'] - 0x38),
                    'delivery_acquisition_backlink')
            context, count = self.result(r, regs['rsi'])
            target = r.q(regs['rax'] - 0x68)
            require(target >= 0x10000, 'delivery_dispatch_null')
            ids = self.ids(key)
            require(len(self.used | self.poisoned | ids) <= 2048, 'delivery_id_budget')
            if ids & self.used:
                # Collision of either side invalidates both complete pairs.
                for old in list(self.requests):
                    if self.ids(old) & ids:
                        self.invalidate(old, 'callback_id_reuse', tid)
                self.poisoned.update(ids)
                raise DecodeError('delivery_ids_reused')
            require(not ids & self.poisoned, 'delivery_ids_poisoned')
            require(len(self.requests) < self.MAX_REQUESTS, 'delivery_request_budget')
            owner = self.owner(identity)
            require(owner in self.generations or len(self.generations) < self.MAX_GENERATIONS,
                    'delivery_generation_budget')
            self.generations.add(owner)
            self.used.update(ids)
            self.serial += 1
            req = dict(request=self.serial, started=now, context=context, item_count=count,
                       concrete=set(), dispatched=set(), dispatch_seen=set(), invalidated=False)
            self.requests[key] = req
            for side, ident in (('resolve', key[2]), ('reject', key[3])):
                self.index[(key[0], key[1], ident)] = key, side
            row.update(request=self.serial, correlated=True, context=context, item_count=count,
                       acquisition='serialized_getitems2', dispatch_target=hex(target))
        else:
            require(not self.ids(key) & self.poisoned, 'delivery_ids_poisoned')
            req = self.requests.get(key)
            side = name.removeprefix('delivery_')
            row.update(correlated=bool(req), request=req['request'] if req else None, side=side)
            if req:
                if side in req['concrete']:
                    self.invalidate(key, 'concrete_entry_repeated', tid)
                    raise DecodeError('delivery_concrete_repeated')
                if side == 'resolve':
                    context, count = self.result(r, regs['rsi'])
                    if (context, count) != (req['context'], req['item_count']):
                        self.invalidate(key, 'serialized_result_changed', tid)
                        raise DecodeError('delivery_result_changed')
                # Reject payload is deliberately never read.
                req['concrete'].add(side)
                row.update(context=req['context'], seconds_since_serialization=now - req['started'])
        self.emit(row)

    def dispatch(self, r, regs, now, tid):
        # At127f15 R14 is ReactInstance, NOT the Callback or PUIReactContext.
        # The compiler saved the weak target at RBP-18 and signed ID at RBP-20.
        ident = r.i(regs['rbp'] - 0x20)
        require(0 <= ident <= 0x7fffffff and regs['rsi'] == ident,
                'delivery_signed_callback_id')
        require(regs['rdi'] == regs['r14'] == r.q(regs['rbp'] - 0x18) and
                regs['rax'] == r.q(regs['rdi']) and
                regs['r10'] == self.got(r, 'invoke_method'), 'delivery_receiver_registers')
        context = r.instance_context(regs['rdi'], 'delivery.dispatch.instance')
        identity = r.identity(context)
        if identity['user'] != self.user:
            raise ScopeExcluded('delivery_foreign_user')
        self.live_owner(r, identity, tid)
        target = r.q(regs['rax'] - 0x98)
        require(target >= 0x10000, 'delivery_dispatch_null')
        token = (*self.owner(identity), ident)
        require(token not in self.poisoned, 'delivery_ids_poisoned')
        found = self.index.get(token)
        row = dict(event='delivery_dispatch', tid=tid, **identity, callback_id=ident,
                   receiver_kind='ReactInstance', dispatch_target=hex(target), correlated=False,
                   request=None, reason='no_acquisition')
        if found:
            key, side = found
            req = self.requests[key]
            if side in req['dispatch_seen']:
                self.invalidate(key, 'callback_dispatch_repeated', tid)
                raise DecodeError('delivery_dispatch_repeated')
            req['dispatch_seen'].add(side)
            row.update(request=req['request'], side=side, context=req['context'],
                       concrete_observed=side in req['concrete'],
                       seconds_since_serialization=now - req['started'])
            if side in req['concrete']:
                req['dispatched'].add(side)
                row.update(correlated=True, reason='matching_concrete_entry')
            else:
                row['reason'] = 'concrete_entry_not_observed'
        self.emit(row)

    def outstanding(self, now):
        return self.closed + [dict(request=req['request'], generation=key[0], user=key[1],
                                  context=req['context'], invalidated=req['invalidated'],
                                  concrete_sides=sorted(req['concrete']),
                                  dispatch_sides=sorted(req['dispatched']),
                                  observed_since_serialization_seconds=now - req['started'])
                              for key, req in self.requests.items() if not req['dispatched']]

    def close_coverage(self, now, reason):
        if self.closed_reason is None:
            self.closed = [dict(row, coverage_closed=reason) for row in self.outstanding(now)]
            self.requests.clear()
            self.index.clear()
            self.closed_reason = reason
