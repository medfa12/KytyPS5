"""Bounded, offline-testable queued Home decoder. No process or write APIs."""
import struct

from home_gate_trace_1360 import DecodeError, Image, ScopeExcluded, require

UILIB = ('Sce.Vsh.UILib.dll.sprx',
         '5a3c961361cb50fd20506310a7874865a71f0b38a33afece0f3562bb7c31482a')

PROBES = {'queue_lookup': ('vsh', 0x484b0), 'queue_action': ('vsh', 0x48630),
          'queue_complete': ('vsh', 0x196e17), 'queue_error': ('vsh', 0x197480)}
# Exact RIP-relative loads from pinned constructors/call sites. Comparing object
# vtables with these GOT values also distinguishes identically named nested types.
LOADS = {
    'outer': ('vsh', 0x47217), 'inner': ('vsh', 0x484cb),
    'delegate': ('vsh', 0x47339),
    'condition_arg': ('vsh', 0x47321), 'condition_method': ('vsh', 0x47381),
    'scalar2_arg': ('vsh', 0x47517), 'scalar2_method': ('vsh', 0x47563),
    'items_arg': ('vsh', 0x47611), 'items_method': ('vsh', 0x47665),
    'items2_arg': ('vsh', 0x47707), 'items2_method': ('vsh', 0x47753),
    'promise': ('pui', 0x8a5), 'callback': ('pui', 0x127e64),
    'appbrowse': ('vsh', 0x4631f), 'appbrowse_mixed': ('vsh', 0x46470),
    'queue': ('vsh', 0x45b17), 'resolve_method': ('vsh', 0x196e10),
}
WINDOWS = {'vsh': [(r, 8) for _, r in PROBES.values()] +
           [(0x484e6, 0x3b), (0x4863c, 0x26), (0x196e02, 0x18),
            (0x197498, 0xa), (0x48720, 0x20), (0x48790, 0x20),
            (0x487c0, 0x30), (0x48800, 0x1d)],
           'pui': [(0x900, 0x58), (0x127e90, 0x95), (0x8b9, 0x25)]}
for image, rva in LOADS.values():
    WINDOWS[image].append((rva, 7))


def abi_slots(images):
    slots = {}
    for name, (image, rva) in LOADS.items():
        code = images[image].at(rva, 7)
        require(code[:3] in (b'\x48\x8b\x3d', b'\x4c\x8b\x15', b'\x4c\x8b\x2d'),
                'queue_got_load_abi')
        slots[name] = (image, rva + 7 + struct.unpack('<i', code[3:])[0])
    require(images['vsh'].at(0x196e17, 3) == b'\xff\x50\x98', 'queue_resolve_abi')
    return slots


def layout_reference(original):
    image = Image((original / UILIB[0]).read_bytes(), UILIB[1])
    for rva, expected in ((0x47c0, '41807f3800'), (0x47cb, '498b7f20'),
                          (0x4518, '49894628'), (0x45c6, 'c6403801'),
                          (0x2e330, '49c7472800000000')):
        raw = bytes.fromhex(expected)
        require(image.at(rva, len(raw)) == raw, 'queue_layout_abi')
    return dict(name=UILIB[0], sha256=UILIB[1], role='offline_queue_layout_reference')


class QueueCorrelator:
    phase, ready = 'queue', None  # fixed four probes; never enters phase rearming

    def __init__(self, emit, bases, slots, user):
        self.emit, self.bases, self.slots, self.user = emit, bases, slots, user
        self.requests, self.used, self.poisoned = {}, set(), set()
        self.serial, self.closed = 0, []

    def got(self, r, name, required=True):
        image, offset = self.slots[name]
        value = r.q(self.bases[image] + offset, 'queue.got.' + name)
        require(value != 0 or not required, 'queue_got_unresolved')
        return value

    def exact(self, r, p, name):
        require(p != 0 and r.q(p, 'queue.type.' + name) == self.got(r, name),
                'queue_receiver_' + name)
        return p

    @staticmethod
    def owner(identity):
        return identity['generation'], identity['user']

    def promise(self, r, p):
        self.exact(r, p, 'promise')
        callbacks, identities, instances = [], [], []
        for offset in (0x10, 0x18):
            cb = self.exact(r, r.q(p + offset), 'callback')
            ident = r.i(cb + 0x18)
            require(0 <= ident <= 0x7fffffff, 'callback_id_bounds')
            instance = r.weak(r.q(cb + 0x10), site='queue.callback.weak')
            identity = r.identity(r.instance_context(instance))
            if identity['user'] != self.user:
                raise ScopeExcluded('queue_foreign_user')
            callbacks.append(ident)
            instances.append(instance)
            identities.append(identity)
        require(instances[0] == instances[1] and identities[0] == identities[1],
                'callback_owner_mismatch')
        require(callbacks[0] != callbacks[1], 'callback_ids_alias')
        identity = identities[0]
        return (*self.owner(identity), *callbacks), identity

    def adapter(self, r, p):
        require(p != 0, 'queue_adapter_null')
        vt = r.q(p)
        kind = next((k for k in ('appbrowse', 'appbrowse_mixed')
                     if vt == self.got(r, k, False)), None)
        if kind is None:
            return dict(adapter_kind='other', adapter=hex(p))
        q = self.exact(r, r.q(p + 0x10), 'queue')
        disposed = r.byte(q + 0x38)
        require(disposed in (0, 1), 'queue_disposed_flag')
        task, items = r.q(q + 0x28), r.q(q + 0x20)
        count = None
        if items:
            count, _ = r.list_info(items, 4096)
        else:
            require(disposed == 1, 'queue_live_list_null')
        flags = None
        if task:
            # Unwrapped Task returned by Task.Run(Func<Task>) can be a derived
            # task; no task-body/delegate or exception graph is traversed.
            flags = r.i(task + 0x44) & 0xffffffff
        else:
            require(disposed == 1, 'queue_live_worker_null')
        return dict(adapter_kind=kind, adapter=hex(p), queue=hex(q),
                    queue_disposed=bool(disposed), queued_count=count,
                    worker_flags=flags)

    def closure(self, r, c):
        self.exact(r, c, 'outer')
        module = r.typed(r.q(c + 0x10),
                        'ReactNative.Modules.Vsh.Database.DatabaseModule', 'queue.module')
        identity = r.identity(r.q(module + 0x28))
        if identity['user'] != self.user:
            raise ScopeExcluded('queue_foreign_user')
        promise = r.q(c + 0x18)
        key, callback_identity = self.promise(r, promise)
        require(self.owner(identity) == self.owner(callback_identity) and
                identity['pui_context'] == callback_identity['pui_context'], 'queue_wrong_owner')
        context = r.i(c + 0x28)
        require(0 <= context <= 1000000, 'queue_context_bound')
        delegate = self.exact(r, r.q(c + 0x20), 'delegate')
        method = r.q(delegate + 0x28)
        op = next((op for op in ('condition', 'scalar2', 'items', 'items2')
                   if method != 0 and method == self.got(r, op + '_method', False)), None)
        if op is None:
            raise ScopeExcluded('queue_operation_not_traced')
        arg = self.exact(r, r.q(delegate + 0x20), op + '_arg')
        require(r.q(arg + (0x20 if op == 'condition' else 0x18)) == promise,
                'queue_promise_backlink')
        condition = None
        if op == 'condition':
            # Partial conditions remain observable, but never become Home scope.
            try:
                condition = r.condition(r.q(arg + 0x10))
            except DecodeError as error:
                if str(error) != 'condition_incomplete':
                    raise
            if condition is not None:
                require(condition['user'] == identity['user'], 'condition_user_mismatch')
        disposed = r.byte(module + 0x40)
        require(disposed in (0, 1), 'module_disposed_flag')
        return key, dict(**identity, context=context, operation=op, promise=hex(promise),
                         module=hex(module), module_disposed=bool(disposed),
                         dictionary_present=bool(r.q(module + 0x30)), condition=condition,
                         action_dispatch=hex(r.q(delegate + 0x18)),
                         resolve_id=key[2], reject_id=key[3])

    def hit(self, name, r, regs, now, tid=None):
        if name in ('queue_lookup', 'queue_action'):
            c = regs['rdi']
            adapter = None
            if name == 'queue_action':
                self.exact(r, c, 'inner')
                adapter, c = r.q(c + 0x10), r.q(c + 0x18)
            key, row = self.closure(r, c)
            if adapter is not None:
                row.update(self.adapter(r, adapter))
            ids = {(key[0], key[1], key[2]), (key[0], key[1], key[3])}
            require(not ids & self.poisoned, 'queue_ids_poisoned')
            if name == 'queue_lookup':
                if ids & self.used:
                    self.poisoned.update(ids)
                    for old in list(self.requests):
                        if {(old[0], old[1], old[2]), (old[0], old[1], old[3])} & ids:
                            self.requests[old]['invalidated'] = True
                            self.emit(dict(event='queue_request_invalidated', tid=tid,
                                           request=self.requests[old]['request'], reason='callback_id_reuse'))
                    raise DecodeError('queue_ids_reused')
                require(len(self.used) < 2048, 'queue_request_budget')
                self.used.update(ids)
                self.serial += 1
                self.requests[key] = dict(request=self.serial, started=now, row=row,
                                          action=False, terminal=False, invalidated=False)
            req = self.requests.get(key)
            if req:
                require(not req['invalidated'], 'queue_ids_poisoned')
                changed = ((req['row']['context'], req['row']['operation']) !=
                           (row['context'], row['operation']))
                repeated = name == 'queue_action' and (req['action'] or req['terminal'])
                if changed or repeated:
                    req['invalidated'] = True
                    self.poisoned.update(ids)
                    code = 'queue_request_changed' if changed else 'queue_action_repeated'
                    self.emit(dict(event='queue_request_invalidated', tid=tid,
                                   request=req['request'], reason=code))
                    raise DecodeError(code)
                if name == 'queue_action':
                    req['action'] = True
            self.emit(dict(event=name, tid=tid, request=req['request'] if req else None,
                           correlated=bool(req), **row))
            return

        key, identity = self.promise(r, regs['rdi'])
        req = self.requests.get(key)
        ids = {(key[0], key[1], key[2]), (key[0], key[1], key[3])}
        require(not ids & self.poisoned and not (req and req['invalidated']), 'queue_ids_poisoned')
        row = dict(event=name, tid=tid, **identity, resolve_id=key[2], reject_id=key[3],
                   promise=hex(regs['rdi']), correlated=bool(req),
                   request=req['request'] if req else None)
        if name == 'queue_complete':
            require(regs['rax'] == r.q(regs['rdi']) and
                    regs['r10'] == self.got(r, 'resolve_method') and
                    r.q(regs['rbp'] - 0x30) == regs['rdi'], 'queue_interface_registers')
            row['dispatch_target'] = hex(r.q(regs['rax'] - 0x68))
            require(int(row['dispatch_target'], 16) >= 0x10000, 'queue_dispatch_null')
            row['dispatch_class'] = ('concrete_resolve' if int(row['dispatch_target'], 16) ==
                                     self.bases['pui'] + 0x1cba30 else 'other_or_interface_thunk')
            row['item_count'] = r.item_count(regs['rsi'])
            if req:
                require(req['row']['operation'] == 'items2', 'queue_completion_operation')
        elif name == 'queue_error':
            # No exception message, stack, Data or arbitrary class name output.
            require(regs['rsi'] != 0, 'queue_exception_null')
            kind = r.kind(regs['rsi'])
            row['exception_class'] = kind if kind in {
                'System.Exception', 'System.ArgumentException', 'System.InvalidOperationException',
                'System.ObjectDisposedException', 'System.NullReferenceException',
                'System.AggregateException', 'System.OperationCanceledException',
                'System.Threading.Tasks.TaskCanceledException'} else 'other'
            row['reject_interface_target'] = hex(r.q(r.q(regs['rdi']) - 0x10))
            if regs['rdx']:
                row.update(self.adapter(r, regs['rdx']))
        else:
            raise DecodeError('queue_probe_unknown')
        if req:
            # A Resolve callback can throw and subsequently enter Reject. Keep
            # both observations; neither is a delivered JavaScript settlement.
            row.update(context=req['row']['context'], operation=req['row']['operation'],
                       action_observed=req['action'], elapsed=now - req['started'])
            req['terminal'] = True
        self.emit(row)

    def outstanding(self, now):
        return self.closed + [dict(request=req['request'], context=req['row']['context'],
                                   operation=req['row']['operation'], invalidated=req['invalidated'],
                                   action_observed=req['action'],
                                   observed_without_completion_seconds=now - req['started'])
                              for req in self.requests.values() if not req['terminal']]

    def close_coverage(self, now, reason):
        self.closed = [dict(row, coverage_closed=row.get('coverage_closed', reason))
                       for row in self.outstanding(now)]
        self.requests.clear()
