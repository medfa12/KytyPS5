"""Offline queue ABI, real-object-layout and correlation controls; no process APIs."""
import argparse
from pathlib import Path
import struct
import unittest

from home_gate_trace_1360 import DecodeError, Image, IMAGES, Reader
from home_queue_trace_1360 import QueueCorrelator, LOADS, WINDOWS, PROBES, abi_slots, layout_reference
from test_home_gate_trace_1360 import Memory


class QueueTests(unittest.TestCase):
    def setUp(self):
        self.m = Memory()
        self.bases = dict(vsh=0x80000000, pui=0x81000000)
        self.slots = {name: (image, 0x1000 + n * 8)
                      for n, (name, (image, _)) in enumerate(LOADS.items())}
        self.values = {}
        for name, (image, off) in self.slots.items():
            # Allocate padding before vtable for Mono interface slots.
            self.m.alloc()
            value = self.m.alloc()
            self.m.q(self.bases[image] + off, value)
            self.values[name] = value
        self.bridge, self.context, self.instance = self.m.identity()
        self.module = self.m.obj('ReactNative.Modules.Vsh.Database.DatabaseModule')
        self.m.q(self.module + 0x28, self.bridge)
        self.m.q(self.module + 0x30, self.m.alloc())
        self.promise = self.make_promise(self.instance)
        self.arg = self.exact('items2_arg')
        self.m.q(self.arg + 0x10, 0xdeadbeef)  # request contents never traversed for items2
        self.m.q(self.arg + 0x18, self.promise)
        self.delegate = self.exact('delegate')
        self.m.q(self.delegate + 0x20, self.arg)
        self.m.q(self.delegate + 0x28, self.values['items2_method'])
        self.m.q(self.delegate + 0x18, self.bases['vsh'] + 0x48800)
        self.outer = self.exact('outer')
        self.m.q(self.outer + 0x10, self.module)
        self.m.q(self.outer + 0x18, self.promise)
        self.m.q(self.outer + 0x20, self.delegate)
        self.m.i(self.outer + 0x28, 4)
        self.queue = self.exact('queue')
        self.m.q(self.queue + 0x20, self.m.sequence([]))
        self.worker = self.m.alloc()
        self.m.i(self.worker + 0x44, 0x2000400)
        self.m.q(self.queue + 0x28, self.worker)
        self.adapter = self.exact('appbrowse')
        self.m.q(self.adapter + 0x10, self.queue)
        self.inner = self.exact('inner')
        self.m.q(self.inner + 0x10, self.adapter)
        self.m.q(self.inner + 0x18, self.outer)
        self.m.q(self.values['promise'] - 0x68, self.bases['pui'] + 0x1cba30)
        self.m.q(self.values['promise'] - 0x10, self.bases['pui'] + 0x88240)
        self.rows = []
        self.model = QueueCorrelator(self.rows.append, self.bases, self.slots, 100)

    def exact(self, name):
        p = self.m.alloc()
        self.m.q(p, self.values[name])
        return p

    def make_promise(self, instance, ids=(30, 31)):
        p = self.exact('promise')
        for off, ident in zip((0x10, 0x18), ids):
            cb = self.exact('callback')
            self.m.q(cb + 0x10, self.m.weak(instance))
            self.m.i(cb + 0x18, ident)
            self.m.q(p + off, cb)
        return p

    def hit(self, name, **regs):
        self.model.hit(name, Reader(self.m.read, self.m.mono), regs, len(self.rows))

    def lookup(self):
        self.hit('queue_lookup', rdi=self.outer)

    def action(self):
        self.hit('queue_action', rdi=self.inner)

    def complete(self, promise=None, **extra):
        result = self.m.object({'items': self.m.array([0xdeadbeef] * 3)})
        frame = self.m.alloc() + 0x40
        self.m.q(frame - 0x30, promise or self.promise)
        regs = dict(rdi=promise or self.promise, rsi=result,
                    rax=self.values['promise'], r10=self.values['resolve_method'], rbp=frame)
        regs.update(extra)
        self.hit('queue_complete', **regs)

    def test_four_sites_and_success_is_not_js_settlement(self):
        self.assertEqual([r for _, r in PROBES.values()], [0x484b0, 0x48630, 0x196e17, 0x197480])
        self.lookup(); self.action(); self.complete()
        self.assertEqual(self.rows[-1]['item_count'], 3)
        self.assertTrue(self.rows[-1]['action_observed'])
        self.assertEqual(self.rows[-1]['dispatch_target'], hex(self.bases['pui'] + 0x1cba30))
        self.assertIsNone(self.model.ready)

    def test_movement_of_promise_callbacks_and_closures(self):
        self.lookup()
        moved = self.make_promise(self.instance)
        self.m.q(self.outer + 0x18, moved)
        self.m.q(self.arg + 0x18, moved)
        old = self.outer
        self.outer = self.exact('outer')
        self.m.put(self.outer, self.m.read(old, 256))
        self.m.q(self.inner + 0x18, self.outer)
        self.m.fill(old, 256)
        self.m.fill(self.promise, 256)  # old address must not be consulted
        self.action(); self.complete(moved)
        self.assertTrue(self.rows[-1]['correlated'])

    def test_wrong_owner_same_user(self):
        _, _, other = self.m.identity(uid=b'2' * 16)
        self.m.q(self.outer + 0x18, self.make_promise(other))
        with self.assertRaisesRegex(DecodeError, 'queue_wrong_owner'):
            self.lookup()
        self.assertFalse(self.rows)

    def test_foreign_user(self):
        self.m.i(self.context + 0x8c, 101)
        with self.assertRaisesRegex(DecodeError, 'queue_foreign_user'):
            self.lookup()

    def test_callback_owner_pair_mismatch(self):
        _, _, other = self.m.identity(uid=b'2' * 16)
        foreign = self.make_promise(other)
        self.m.q(self.promise + 0x18, struct.unpack('<Q', self.m.read(foreign + 0x18, 8))[0])
        with self.assertRaisesRegex(DecodeError, 'callback_owner_mismatch'):
            self.lookup()

    def test_reused_single_id_poisons_both_pairs(self):
        self.lookup()
        p = self.make_promise(self.instance, (30, 99))
        self.m.q(self.outer + 0x18, p)
        self.m.q(self.arg + 0x18, p)
        with self.assertRaisesRegex(DecodeError, 'queue_ids_reused'):
            self.lookup()
        with self.assertRaisesRegex(DecodeError, 'queue_ids_poisoned'):
            self.complete()

    def test_queue_retired_null_fields_is_explicit(self):
        self.lookup()
        self.m.put(self.queue + 0x38, b'\x01')
        self.m.q(self.queue + 0x20, 0)
        self.m.q(self.queue + 0x28, 0)
        self.m.put(self.module + 0x40, b'\x01')
        self.m.q(self.module + 0x30, 0)
        self.action()
        self.assertTrue(self.rows[-1]['queue_disposed'])
        self.assertTrue(self.rows[-1]['module_disposed'])
        self.assertIsNone(self.rows[-1]['queued_count'])

    def test_changed_context_invalidates_later_completion(self):
        self.lookup()
        self.m.i(self.outer + 0x28, 5)
        with self.assertRaisesRegex(DecodeError, 'queue_request_changed'):
            self.action()
        with self.assertRaisesRegex(DecodeError, 'queue_ids_poisoned'):
            self.complete()

    def test_live_queue_null_list_is_not_empty(self):
        self.lookup(); self.m.q(self.queue + 0x20, 0)
        with self.assertRaisesRegex(DecodeError, 'queue_live_list_null'):
            self.action()
        self.assertFalse(next(iter(self.model.requests.values()))['action'])

    def test_unsupported_promise_receiver(self):
        self.m.q(self.promise, self.m.alloc())
        with self.assertRaisesRegex(DecodeError, 'queue_receiver_promise'):
            self.lookup()

    def test_wrong_nested_closure_owner_even_same_name(self):
        self.m.q(self.outer, self.m.classes.get('.<>c__DisplayClass18_0', self.m.alloc()))
        with self.assertRaisesRegex(DecodeError, 'queue_receiver_outer'):
            self.lookup()

    def test_backlink_and_weak_retirement(self):
        self.m.q(self.arg + 0x18, self.m.alloc())
        with self.assertRaisesRegex(DecodeError, 'queue_promise_backlink'):
            self.lookup()
        self.m.q(self.arg + 0x18, self.promise)
        self.m.q(self.m.bucket + 16, 0)  # callback's weak ReactInstance
        with self.assertRaisesRegex(DecodeError, 'weak_collected'):
            self.lookup()

    def test_dispatch_registers_not_assumed(self):
        self.lookup()
        with self.assertRaisesRegex(DecodeError, 'queue_interface_registers'):
            self.complete(r10=0)

    def test_exception_after_resolve_attempt_retained(self):
        self.lookup(); self.action(); self.complete()
        self.hit('queue_error', rdi=self.promise,
                 rsi=self.m.obj('System.InvalidOperationException'), rdx=self.adapter)
        self.assertTrue(self.rows[-1]['correlated'])
        self.assertEqual(self.rows[-1]['exception_class'], 'System.InvalidOperationException')

    def test_fresh_identity_after_entire_owner_graph_moves(self):
        self.lookup()
        bridge, _, instance = self.m.identity()
        p = self.make_promise(instance)
        self.m.q(self.module + 0x28, bridge)
        self.m.q(self.outer + 0x18, p)
        self.m.q(self.arg + 0x18, p)
        self.m.fill(self.context, 256)
        self.m.fill(self.instance, 256)
        self.action(); self.complete(p)
        self.assertTrue(self.rows[-1]['correlated'])

    def test_foreign_exception_class_is_redacted(self):
        self.lookup()
        self.hit('queue_error', rdi=self.promise,
                 rsi=self.m.obj('Secrets.DoNotEmitThis'), rdx=0)
        self.assertEqual(self.rows[-1]['exception_class'], 'other')
        self.assertNotIn('DoNotEmitThis', str(self.rows))

    def test_old_ids_in_new_generation_do_not_match(self):
        self.lookup()
        _, _, instance = self.m.identity(uid=b'9' * 16)
        self.complete(self.make_promise(instance))
        self.assertFalse(self.rows[-1]['correlated'])

    def test_no_lookup_no_inferred_correlation(self):
        self.action(); self.complete()
        self.assertFalse(self.rows[-1]['correlated'])

    def test_close_coverage_never_joins_later_completion(self):
        self.lookup()
        self.model.close_coverage(20, 'shutdown_drain')
        self.complete()
        self.assertFalse(self.rows[-1]['correlated'])
        self.assertEqual(self.model.outstanding(21)[0]['coverage_closed'], 'shutdown_drain')

    def test_uninitialized_unrelated_got_is_not_required(self):
        for name in ('condition_method', 'scalar2_method', 'items_method'):
            image, off = self.slots[name]
            self.m.q(self.bases[image] + off, 0)
        self.lookup()
        self.assertEqual(self.rows[-1]['operation'], 'items2')

    def test_bounded_reads(self):
        with self.assertRaisesRegex(DecodeError, 'read_budget'):
            self.model.hit('queue_lookup', Reader(self.m.read, self.m.mono, 8), dict(rdi=self.outer), 0)


def audit(original):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    images = {k: Image((original / IMAGES[k][0]).read_bytes(), IMAGES[k][1]) for k in ('vsh', 'pui')}
    slots = abi_slots(images)
    layout_reference(original)
    for key, windows in WINDOWS.items():
        for rva, size in windows:
            images[key].at(rva, size)
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    # Independent actual AOT accesses establish fixture offsets, register ABI,
    # callback owner/ID and interface target. No mirrored mock Reader methods.
    checks = [('vsh', 0x484ed, 4, 'mov', 'qword ptr [rbx + 0x18], r14'),
              ('vsh', 0x484f6, 4, 'mov', 'rax, qword ptr [r14 + 0x10]'),
              ('vsh', 0x48531, 4, 'mov', 'r14, qword ptr [rax + 0x10]'),
              ('vsh', 0x4864e, 4, 'mov', 'rdi, qword ptr [rax + 0x20]'),
              ('vsh', 0x4865b, 4, 'mov', 'rsi, qword ptr [rax + 0x10]'),
              ('vsh', 0x4865f, 3, 'call', 'qword ptr [rdi + 0x18]'),
              ('vsh', 0x196e02, 4, 'mov', 'rdi, qword ptr [rbp - 0x30]'),
              ('vsh', 0x196e06, 3, 'mov', 'rsi, r13'),
              ('vsh', 0x196e17, 3, 'call', 'qword ptr [rax - 0x68]'),
              ('vsh', 0x197498, 4, 'mov', 'qword ptr [rbp - 0x38], rdi'),
              ('pui', 0x920, 4, 'mov', 'dword ptr [r15 + 0x18], esi'),
              ('pui', 0x942, 4, 'mov', 'qword ptr [r15 + 0x10], rbx'),
              ('pui', 0x127eaf, 4, 'mov', 'rax, qword ptr [r14 + 0x10]'),
              ('pui', 0x127ed9, 4, 'movsxd', 'rax, dword ptr [r14 + 0x18]')]
    for image, rva, size, mnemonic, operands in checks:
        code = list(cs.disasm(images[image].at(rva, size), rva))
        assert len(code) == 1 and (code[0].mnemonic, code[0].op_str) == (mnemonic, operands), hex(rva)
    # Validate GOT entries against pinned AOT metadata patches, not only opcodes.
    from inspect_appbrowse_aot_1360 import Image as AotImage
    aot = AotImage(original / IMAGES['vsh'][0], 'vsh')
    expected = {'outer': 'kind=13 1 1c0', 'inner': 'kind=13 1 1c1',
                'items2_arg': 'kind=13 1 1c6', 'items2_method': 'kind=5 9ef',
                'appbrowse': 'kind=13 1 25f', 'appbrowse_mixed': 'kind=13 1 26d'}
    for name, marker in expected.items():
        assert marker in aot.patch(slots[name][1]), (name, aot.patch(slots[name][1]))
    import dnfile
    from prepare_shellui_managed_1360 import embedded_pe
    pe = dnfile.dnPE(data=embedded_pe(images['pui'].data))
    table = pe.net.mdtables
    callback = table.TypeDef.rows[2]  # GOT35 type3, audited CreateCallback allocation
    promise = table.TypeDef.rows[0x3f0 - 1]  # GOT31 type3f0, CreatePromise allocation
    assert str(callback.TypeName) == 'Callback' and str(promise.TypeName) == 'Promise'
    assert any(n.NestedClass.row is callback and
               str(n.EnclosingClass.row.TypeName) == 'ReactDelegateFactoryBase'
               for n in table.NestedClass.rows)
    fields = {str(f.row.Name): f.row.Signature.value for f in callback.FieldList}
    assert fields['_id'] == b'\x06\x08' and fields['_instance'].startswith(b'\x06\x15')
    assert {str(f.row.Name) for f in promise.FieldList} == {'_resolve', '_reject'}
    pe.close()
    print('Pinned queue ABI windows, nested-owner GOT identities and real-layout offsets passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path)
    args, rest = parser.parse_known_args()
    result = unittest.main(argv=['queue-tests', *rest], exit=False)
    if not result.result.wasSuccessful():
        raise SystemExit(1)
    if args.original:
        audit(args.original)
