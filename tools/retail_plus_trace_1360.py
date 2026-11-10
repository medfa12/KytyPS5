"""Retail Plus request -> worker -> settlement -> native JS executor, no guest writes.

No Hermes opcode traps. Single acquired Home request, callback-ID correlation,
bounded native code handoffs. A native callback return is not JS acceptance.
"""
from pathlib import Path
import struct

from home_gate_trace_1360 import DecodeError, ScopeExcluded, require, Image, digest
import retail_home_config_trace_1360 as config

PLUS = ('ReactNative.Modules.Vsh.dll.sprx', '9893a47fe52b2bb89a669acaf7e073cf8b80aa71a2c8d2c66eb116932f99e27c')
INITIAL = {'loader': ('host', 0x9f447e), 'loader_epilogue1': ('host', 0x9f4468),
           'loader_epilogue2': ('host', 0x9f4472), 'loader_epilogue3': ('host', 0x9f447d)}
NATIVE = {'plus_entry': ('plus', 0x2d0e0), 'plus_worker': ('plus', 0x16be70),
          'plus_resolve': ('pui', 0x1cba30), 'plus_reject': ('pui', 0x1cbaa0)}
SITES = {**INITIAL, **NATIVE, 'plus_service_call': ('plus', 0x16bf4b),
         'plus_service_result': ('plus', 0x16bf50),
         'plus_service_done': ('plus', 0x16bf6b), 'plus_delivery': ('pui', 0x66950),
         'plus_delivery_return': ('pui', 0x6697b)}
CHECKS = {
    'plus': {0x2d0e0: '554889e541574156415453803d264353',
             0x16be70: '55488bec4883ec404c8975f04c897df8',
             0x16bf4b: 'e814fe12004c8bf085c07d07c745e000',
             0x16bf50: '4c8bf085c07d07c745e000000000498b',
             0x16bf6b: '85c00f854a000000498b4718488945d0',
             0x171420: '488b3db9844000be28000000e8bbea10'},
    'pui': {0x885b0: '554889e5488b0d85847200488d471048',
            0x1cba30: '55488bec4883ec204c8975f84c8bf748',
            0x1cbaa0: '55488bec4883ec204c8975f84c8bf748',
            0x88810: '554889e5488b47285dc3cccccccccccc',
            0x127eaf: '498b4610488bf8488bf54883c6e88338',
            0x127ed9: '496346184c8bf18945e04c8b65d8498b',
            0x66950: '554889e548837f10007422488b471849',
            0x6697b: '5dc3488b3daca07400be83130100e86a',
            0x127e64: '488b3d654e6900be20000000e80fec1c'},
    'host': {0x9f447e: 'c348c785d0000000', 0x9f4468: '4c89e04881c4a801',
             0x9f4472: '5b5f5e415c415d41', 0x9f447d: '5dc348c785d00000'},
}
WINDOWS = {k: list(v) for k, v in config.WINDOWS.items()}
for key, checks in CHECKS.items():
    WINDOWS.setdefault(key, []).extend((a, len(bytes.fromhex(b))) for a, b in checks.items())


def audit(images):
    config.audit(images)
    for key, checks in CHECKS.items():
        for at, b in checks.items():
            raw = bytes.fromhex(b)
            require(images[key].at(at, len(raw)) == raw, 'plus_abi_' + key)


def provenance(original, stage, exe, log):
    images, bases, report = config.provenance(original, stage, exe, log)
    name, sha = PLUS
    source, selected = original / name, stage / name
    data = source.read_bytes()
    images['plus'] = Image(data, sha)
    require(selected.read_bytes() == data, 'plus_stage_identity')
    # Native Plus is acquired at its own lazy loader boundary, never guessed.
    require(name not in log.read_text(encoding='utf-8', errors='replace'), 'plus_requires_fresh_prefix')
    report['plus'] = dict(name=name, sha256=sha, original=str(source.resolve()), stage=str(selected.resolve()), base=None)
    audit(images)
    return images, bases, report


def handoff(threads, names, addresses, probes, retired, updated, bases, pending):
    """Bounded repeated code-only migration under a stopped debug event.

    A retired address is never reused or moved to another slot. Snapshot every
    already faulted PC before DR writes; retain previous queued proofs. No GPR,
    control, IP, data breakpoint or running-drain change is allowed here.
    """
    new_names = list(updated)
    new = [bases[updated[n][0]] + updated[n][1] for n in new_names]
    require(len(new) == 4 and len(set(new)) == 4 and len(retired) <= 12, 'plus_handoff_bound')
    require(not set(new) & set(retired), 'plus_retired_reuse')
    require(all(a not in addresses or addresses.index(a) == n for n, a in enumerate(new)), 'plus_slot_migration')
    if new == addresses:
        require(new_names == names, 'plus_probe_rename')
        return pending
    result = dict(pending)
    for tid, (handle, _) in threads.saved.items():
        raw = threads.context(handle)
        pc = struct.unpack_from('<Q', raw, 248)[0]
        flags = struct.unpack_from('<I', raw, 68)[0]
        if pc in addresses and not flags & (0x100 | 0x10000):
            slot = addresses.index(pc)
            dr6 = struct.unpack_from('<Q', raw, 104)[0]
            require(not dr6 & 0xe000 and dr6 & 15 in (0, 1 << slot), 'plus_foreign_pending')
            require((tid, pc) not in result or result[(tid, pc)] == slot, 'plus_queue_collision')
            result[(tid, pc)] = slot
    for slot, old in enumerate(addresses):
        if old != new[slot]:
            require(old not in retired, 'plus_repeated_retirement')
            retired[old] = slot, names[slot]
    probes.update(updated)
    # Publish the prospective PC/slot ownership BEFORE a possibly partial API
    # write. On failure the outer cleanup sees new active + old retired PCs and
    # can drain both plans; it must never forward an orphaned owned breakpoint.
    names[:], addresses[:] = new_names, new
    threads.rearm(new)
    return result


class Observer(config.Observer):
    mode = 'retail_plus'
    probes, windows = INITIAL, WINDOWS
    max_decode_unknowns = 1
    handoff = staticmethod(handoff)
    conclusion = 'single Home Plus native request; callback entry/return is not JS consumer fulfillment or usable Home'

    def __init__(self, selected, token):
        self.selected = {k: v.resolve() for k, v in selected.items()}
        self.token, self.pending = token, None

    def model(self, emit, bases):
        self.decoder = Model(self, emit, bases)
        return self.decoder

    def transition(self):
        result, self.pending = self.pending, None
        return result


class Model:
    phase, ready, complete = 'retail_plus', None, False

    def __init__(self, observer, emit, bases):
        self.observer, self.emit, self.bases = observer, emit, bases
        self.probes, self.req, self.closed = dict(INITIAL), None, False
        self.calls, self.worker_thread = {}, None

    def row(self, event, **kw):
        owner = dict(generation=self.req['key'][0], user=self.req['key'][1], request=1) if self.req else {}
        self.emit(dict(event=event, **owner, **kw))

    def change(self, slot, name):
        items = list(self.probes.items())
        items[slot] = name, SITES[name]
        self.probes = dict(items)
        self.observer.pending = self.probes, {}

    def thread_boundary(self, tid):
        if tid in self.calls or tid == self.worker_thread:
            self.row('plus_thread_boundary', tid=tid, coverage_complete=False)
            raise DecodeError('plus_owned_thread_lifecycle_gap')

    def live(self, r, context):
        identity = r.identity(context)
        if identity['user'] != 0x10010029:
            raise ScopeExcluded('plus_other_user')
        context = int(identity['pui_context'], 16)
        require(r.byte(context + 0x92) == 0, 'plus_context_terminated')
        return identity

    def promise(self, r, p):
        r.typed(p, 'ReactNative.Bridge.Promise')
        ids, owners = [], []
        # Exact callback vtable from its audited allocation site, not just a
        # possibly repeated nested class name in Mono metadata.
        at = self.bases['pui'] + 0x127e64
        code = r.raw(at, 7)
        require(code[:3] == bytes.fromhex('488b3d'), 'plus_callback_got_instruction')
        vt = r.q(at + 7 + struct.unpack('<i', code[3:])[0])
        require(vt != 0, 'plus_callback_got_null')
        for offset in (0x10, 0x18):
            cb = r.q(p + offset)
            require(cb and r.q(cb) == vt, 'plus_callback_type')
            ident = r.i(cb + 0x18)
            require(0 <= ident <= 0x7fffffff, 'plus_callback_id')
            instance = r.weak(r.q(cb + 0x10))
            identity = self.live(r, r.instance_context(instance))
            ids.append(ident)
            owners.append(identity)
        require(owners[0] == owners[1] and ids[0] != ids[1], 'plus_callback_owner_mismatch')
        ident = owners[0]
        return (ident['generation'], ident['user'], *ids), ident

    def match(self, r, p):
        key, identity = self.promise(r, p)
        if not self.req or key != self.req['key']:
            raise ScopeExcluded('plus_unacquired_promise')
        self.req['context'] = int(identity['pui_context'], 16)
        return identity

    def worker(self, r, p):
        vt = r.q(self.bases['plus'] + 0x5798e0)
        require(vt != 0 and r.q(p) == vt, 'plus_worker_type')
        self.match(r, r.q(p + 0x18))
        module = r.typed(r.q(p + 0x20), 'ReactNative.Modules.Vsh.PSN.EntitlementCSIModule')
        owner = self.live(r, r.q(module + 0x28))
        require((owner['generation'], owner['user']) == self.req['key'][:2], 'plus_worker_module_owner')

    def hit(self, name, r, regs, now, tid=None):
        require(not self.closed, 'plus_coverage_closed')
        if name.startswith('loader_epilogue'):
            raise ScopeExcluded('plus_loader_epilogue')
        if name == 'loader':
            if not regs['rax']:
                raise ScopeExcluded('plus_loader_null')
            path = config.returned_path(r, regs['rax'])
            key = next((k for k, p in self.observer.selected.items() if p.name == path.name), None)
            if key is None:
                raise ScopeExcluded('plus_loader_other')
            require(path.resolve() == self.observer.selected[key], 'plus_loader_wrong_path')
            require(key not in self.bases, 'plus_mapping_reused')
            base, size = r.q(regs['rax'] + 0xb0), r.q(regs['rax'] + 0xc8)
            require(base >= 0x10000 and base % 4096 == 0 and size >= {'pui': 0x860000, 'plus': 0x580000}[key],
                    'plus_loader_extent')
            self.row('plus_provider_loaded', image=key, base=hex(base), tid=tid)
            active = all(k in self.bases or k == key for k in ('pui', 'plus'))
            self.probes = dict(NATIVE if active else INITIAL)
            self.observer.pending = self.probes, {key: base}
            return
        if name == 'plus_entry':
            module = r.typed(regs['rdi'], 'ReactNative.Modules.Vsh.PSN.EntitlementCSIModule')
            owner = self.live(r, r.q(module + 0x28))
            key, callback_owner = self.promise(r, regs['rdx'])
            require(owner['generation'] == key[0] and owner['user'] == key[1] and
                    owner['pui_context'] == callback_owner['pui_context'], 'plus_entry_owner')
            fields = r.fields(regs['rsi'])
            require('userId' in fields and r.value(fields['userId']) == 0x10010029, 'plus_param_user')
            require(self.req is None, 'plus_duplicate_acquisition')
            self.req = dict(key=key, context=int(owner['pui_context'], 16), started=now,
                            worker=False, service_call=False, service_returned=False, service_done=False, cancelled=None,
                            settlement=None, delivery=False, delivery_return=False)
            self.row('plus_request', resolve_id=key[2], reject_id=key[3], tid=tid,
                     acquisition='first native Home IsPlusMember; HBC caller not decoded')
            self.change(0, 'plus_delivery')
        elif name in ('plus_worker', 'plus_service_call', 'plus_service_result', 'plus_service_done'):
            p = regs['rdi'] if name == 'plus_worker' else regs['r15']
            self.worker(r, p)
            self.worker_thread = tid
            if name == 'plus_worker':
                require(not self.req['worker'], 'plus_repeated_worker')
                self.req['worker'] = True
                self.change(1, 'plus_service_call')
            elif name == 'plus_service_call':
                require(self.req['worker'] and not self.req['service_call'], 'plus_service_order')
                require(regs['rdi'] & 0xffffffff == 0x10010029, 'plus_service_user')
                self.req['service_call'] = True
                self.change(1, 'plus_service_result')
            elif name == 'plus_service_result':
                require(self.req['service_call'] and not self.req['service_returned'], 'plus_service_result_order')
                self.req['service_returned'] = True
                self.row('plus_native_service_return', tid=tid, service_failed=bool(regs['rax'] & 0x80000000))
                self.change(1, 'plus_service_done')
                return
            else:
                require(self.req['service_returned'] and not self.req['service_done'], 'plus_service_done_order')
                cancelled = regs['rax'] & 0xffffffff
                require(cancelled in (0, 1), 'plus_cancellation_boolean')
                self.req.update(service_done=True, cancelled=bool(cancelled))
                self.worker_thread = None
                self.row('plus_service_return', tid=tid, cancelled=bool(cancelled),
                         service_failed=bool(regs['r14'] & 0x80000000))
                return
            self.row(name, tid=tid)
        elif name in ('plus_resolve', 'plus_reject'):
            if self.req is None:
                raise ScopeExcluded('plus_before_acquisition')
            self.match(r, regs['rdi'])
            require(self.req['settlement'] is None, 'plus_duplicate_settlement')
            self.req['settlement'] = name.removeprefix('plus_')
            self.worker_thread = None
            # Resolve/Reject data argument is deliberately not read.
            self.row('plus_settlement', side=self.req['settlement'], tid=tid,
                     payload_read=False)
        elif name == 'plus_delivery':
            if not self.req:
                raise ScopeExcluded('plus_before_acquisition')
            ident = r.i(regs['rdi'] + 0x20)
            if ident not in self.req['key'][2:]:
                raise ScopeExcluded('plus_other_callback')
            owner = self.live(r, self.req['context'])
            require((owner['generation'], owner['user']) == self.req['key'][:2], 'plus_delivery_owner_changed')
            context = int(owner['pui_context'], 16)
            bridge = r.typed(r.q(r.q(context + 0x30) + 0x18), 'ReactNative.Bridge.ReactBridge')
            executor = r.typed(r.q(bridge + 0x18), 'ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor')
            # Callback counters are per VM. A foreign executor may legitimately
            # have the same numeric ID: exclude it, never pair it or abort Home.
            if r.q(regs['rdi'] + 0x18) != executor:
                raise ScopeExcluded('plus_other_executor')
            require(r.q(regs['rdi'] + 0x10) != 0, 'plus_delivery_arguments_null')
            side = 'resolve' if ident == self.req['key'][2] else 'reject'
            require(self.req['settlement'] == side and not self.req['delivery'], 'plus_delivery_order_or_reuse')
            # Validate the native backend without emitting its pointer or JS data.
            config.Model.runtime_owner(self, r, context)
            self.calls[tid] = (regs['rsp'], r.q(regs['rsp']))
            self.req['delivery'] = True
            self.row('plus_executor_callback', callback_id=ident, side=side, tid=tid,
                     payload_read=False, js_consumption='unknown')
            self.change(1, 'plus_delivery_return')
        elif name == 'plus_delivery_return':
            if tid not in self.calls:
                raise ScopeExcluded('plus_other_callback_return')
            stack, ret = self.calls[tid]
            require(regs['rsp'] == regs['rbp'] == stack - 8 and r.q(regs['rbp'] + 8) == ret,
                    'plus_callback_return_frame')
            owner = self.live(r, self.req['context'])
            require((owner['generation'], owner['user']) == self.req['key'][:2], 'plus_return_owner_changed')
            del self.calls[tid]
            self.worker_thread = None
            self.req['delivery_return'] = self.complete = True
            self.row('plus_executor_return', tid=tid, js_consumption='unknown')
        else:
            raise DecodeError('plus_unknown_probe')

    def outstanding(self, now):
        if not self.req:
            return [dict(boundary='native_plus_entry_not_observed', native_promise_state='unknown')]
        return [dict(generation=self.req['key'][0], user=self.req['key'][1], request=1,
                     seconds_since_entry=round(now - self.req['started'], 3),
                     **{k: self.req[k] for k in ('worker','service_call','service_returned','service_done','cancelled','settlement','delivery','delivery_return')},
                     native_promise_state='not inferred from absence', js_consumption='not covered')]

    def close_coverage(self, now, reason):
        self.closed = True
