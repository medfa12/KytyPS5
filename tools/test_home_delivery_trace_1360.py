"""Offline delivery correlation/ABI controls. No attach or process operations."""
import argparse
from pathlib import Path
import struct
import unittest

from home_gate_trace_1360 import DecodeError, Image, IMAGES, Reader
from home_delivery_trace_1360 import DeliveryCorrelator, PROBES, WINDOWS, abi_slots
from test_home_queue_trace_1360 import QueueTests


class DeliveryTests(unittest.TestCase):
    exact = QueueTests.exact
    make_promise = QueueTests.make_promise

    def setUp(self):
        QueueTests.setUp(self)
        self.slots['invoke_method'] = ('pui', 0x2000)
        self.values['invoke_method'] = self.m.alloc()
        self.m.q(self.bases['pui'] + 0x2000, self.values['invoke_method'])
        self.model = DeliveryCorrelator(self.rows.append, self.bases, self.slots, 100)
        self.now = 1

    def hit(self, name, **regs):
        self.model.hit(name, Reader(self.m.read, self.m.mono), regs, self.now, tid=7)
        self.now += 1

    def result(self, context=4, count=0):
        return self.m.object({'ctxId': self.m.value(context),
                              'items': self.m.array([0xdeadbeef] * count)})

    def acquire(self, p=None, context=4, count=0, **changes):
        p = p or self.promise
        result = self.result(context, count)
        frame = self.m.alloc() + 0x80
        self.m.q(frame - 0x30, p)
        self.m.q(frame - 0x38, result)
        regs = dict(rdi=p, rsi=result, r13=result, rbp=frame,
                    rax=self.values['promise'], r10=self.values['resolve_method'])
        regs.update(changes)
        self.hit('delivery_acquire', **regs)

    def concrete(self, side='resolve', p=None, context=4, count=0):
        self.hit('delivery_' + side, rdi=p or self.promise,
                 rsi=self.result(context, count) if side == 'resolve' else 0xdeadbeef)

    def dispatch(self, ident=30, instance=None, **changes):
        instance = instance or self.instance
        frame = self.m.alloc() + 0x80
        self.m.q(frame - 0x18, instance)
        self.m.i(frame - 0x20, ident)
        vt = struct.unpack('<Q', self.m.read(instance, 8))[0]
        self.m.q(vt - 0x98, self.bases['pui'] + 0x87030)
        regs = dict(rdi=instance, r14=instance, rsi=ident, rbp=frame, rax=vt,
                    r10=self.values['invoke_method'], rdx=0xdeadbeef)
        regs.update(changes)
        self.hit('delivery_dispatch', **regs)

    def test_exact_four_sites_and_zero_is_acquired(self):
        self.assertEqual(list(PROBES.values()), [('vsh', 0x196e17), ('pui', 0x1cba30),
                                               ('pui', 0x1cbaa0), ('pui', 0x127f15)])
        self.acquire(); self.concrete(); self.dispatch()
        self.assertEqual(self.rows[0]['item_count'], 0)
        self.assertTrue(self.rows[-1]['correlated'])
        self.assertEqual(self.rows[-1]['side'], 'resolve')
        self.assertIsNone(self.model.ready)
        self.assertNotIn('elapsed', self.rows[-1])
        self.assertNotIn('operation', self.rows[-1])

    def test_gc_movement_entire_graph_and_promise(self):
        self.acquire()
        old_context, old_instance, old_promise = self.context, self.instance, self.promise
        self.bridge, self.context, self.instance = self.m.identity()
        self.promise = self.make_promise(self.instance)
        for old in (old_context, old_instance, old_promise):
            self.m.fill(old, 256)
        self.concrete(); self.dispatch()
        self.assertTrue(self.rows[-1]['correlated'])

    def test_resolve_payload_and_frame_backlinks(self):
        for changes in ({'r13': 0}, {'r10': 0}, {'rax': 0}, {'rdi': self.make_promise(self.instance)}):
            with self.assertRaisesRegex(DecodeError, 'delivery_acquisition_backlink'):
                self.acquire(**changes)
        self.assertFalse(self.model.requests)

    def test_collision_single_id_invalidates_both_sides(self):
        self.acquire(); self.concrete(); self.dispatch()
        p = self.make_promise(self.instance, (31, 90))
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_reused'):
            self.acquire(p)
        self.assertTrue(next(iter(self.model.requests.values()))['invalidated'])
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_poisoned'):
            self.dispatch(30)
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_poisoned'):
            self.dispatch(90)

    def test_same_pair_reacquisition_is_not_second_request(self):
        self.acquire()
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_reused'):
            self.acquire()
        self.assertEqual(self.model.serial, 1)

    def test_weak_collected_is_unknown_not_delivery(self):
        self.acquire()
        weak = struct.unpack('<Q', self.m.read(self.instance + 0x10, 8))[0]
        handle = struct.unpack('<i', self.m.read(weak + 0x10, 4))[0]
        self.m.q(self.m.bucket + (handle >> 3) * 8, 0)
        with self.assertRaisesRegex(DecodeError, 'weak_collected'):
            self.concrete()
        with self.assertRaisesRegex(DecodeError, 'weak_collected'):
            self.dispatch()

    def test_foreign_instance_generation_not_correlated(self):
        self.acquire(); self.concrete()
        _, _, other = self.m.identity(uid=b'2' * 16)
        self.dispatch(instance=other)
        self.assertFalse(self.rows[-1]['correlated'])
        self.assertEqual(self.rows[-1]['reason'], 'no_acquisition')

    def test_foreign_user_excluded(self):
        _, _, other = self.m.identity(user=101)
        with self.assertRaisesRegex(DecodeError, 'foreign_user'):
            self.dispatch(instance=other)

    def test_receiver_is_not_pui_context(self):
        self.acquire(); self.concrete()
        with self.assertRaisesRegex(DecodeError, 'object_type'):
            self.dispatch(instance=self.context)

    def test_receiver_register_and_interface_mismatch(self):
        for changes in ({'r14': 0}, {'rax': 0}, {'r10': 0}):
            with self.assertRaisesRegex(DecodeError, 'delivery_receiver_registers'):
                self.dispatch(**changes)

    def test_signed_callback_id_not_truncated(self):
        with self.assertRaisesRegex(DecodeError, 'delivery_signed_callback_id'):
            self.dispatch(-1)
        with self.assertRaisesRegex(DecodeError, 'delivery_signed_callback_id'):
            self.dispatch(30, rsi=(1 << 32) + 30)

    def test_no_acquisition_never_infers_scalar(self):
        self.concrete('reject'); self.dispatch(31)
        self.assertFalse(self.rows[0]['correlated'])
        self.assertFalse(self.rows[-1]['correlated'])
        self.assertNotIn('context', self.rows[-1])

    def test_side_requires_its_concrete_entry(self):
        self.acquire(); self.concrete(); self.dispatch(31)
        self.assertFalse(self.rows[-1]['correlated'])
        self.assertEqual(self.rows[-1]['reason'], 'concrete_entry_not_observed')
        self.concrete('reject')
        with self.assertRaisesRegex(DecodeError, 'delivery_dispatch_repeated'):
            self.dispatch(31)
        self.setUp()
        self.acquire(); self.concrete('reject'); self.dispatch(31)
        self.assertTrue(self.rows[-1]['correlated'])
        self.assertEqual(self.rows[-1]['side'], 'reject')

    def test_resolve_then_reject_both_retained(self):
        self.acquire(); self.concrete(); self.dispatch()
        self.concrete('reject'); self.dispatch(31)
        self.assertEqual(next(iter(self.model.requests.values()))['dispatched'], {'resolve', 'reject'})

    def test_wrong_context_invalidates(self):
        self.acquire()
        with self.assertRaisesRegex(DecodeError, 'delivery_result_changed'):
            self.concrete(context=5)
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_poisoned'):
            self.dispatch()

    def test_changed_count_invalidates(self):
        self.acquire(count=3)
        with self.assertRaisesRegex(DecodeError, 'delivery_result_changed'):
            self.concrete(count=2)

    def test_repeated_concrete_or_dispatch_invalidates(self):
        self.acquire(); self.concrete()
        with self.assertRaisesRegex(DecodeError, 'delivery_concrete_repeated'):
            self.concrete()
        self.setUp()
        self.acquire(); self.concrete(); self.dispatch()
        with self.assertRaisesRegex(DecodeError, 'delivery_dispatch_repeated'):
            self.dispatch()

    def test_terminated_owner_invalidates_pending(self):
        self.acquire()
        self.m.put(self.context + 0x92, b'\x01')
        with self.assertRaisesRegex(DecodeError, 'delivery_context_terminated'):
            self.concrete()
        self.assertTrue(next(iter(self.model.requests.values()))['invalidated'])
        self.m.put(self.context + 0x92, b'\x00')
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_poisoned'):
            self.dispatch()

    def test_generation_budget_and_id_epoch(self):
        self.model.MAX_GENERATIONS = 1
        self.acquire()
        _, _, other = self.m.identity(uid=b'2' * 16)
        with self.assertRaisesRegex(DecodeError, 'delivery_generation_budget'):
            self.acquire(self.make_promise(other))
        self.assertEqual(len(self.model.generations), 1)

    def test_request_and_reader_budgets(self):
        self.model.MAX_REQUESTS = 1
        self.acquire()
        with self.assertRaisesRegex(DecodeError, 'delivery_request_budget'):
            self.acquire(self.make_promise(self.instance, (32, 33)))
        with self.assertRaises(DecodeError):
            self.model.promise(Reader(self.m.read, self.m.mono, 8), self.promise)
        self.model.poisoned.update(('other', 100, n) for n in range(2048))
        with self.assertRaisesRegex(DecodeError, 'delivery_id_budget'):
            self.acquire(self.make_promise(self.instance, (34, 35)))

    def test_close_freezes_and_disallows_reacquisition(self):
        self.acquire()
        self.model.close_coverage(10, 'shutdown')
        old = self.model.outstanding(100)
        self.model.close_coverage(200, 'later')
        self.assertEqual(old, self.model.outstanding(300))
        with self.assertRaisesRegex(DecodeError, 'delivery_coverage_closed'):
            self.acquire()

    def test_invalid_owner_and_missing_ctx(self):
        with self.assertRaisesRegex(DecodeError, 'delivery_context_missing'):
            self.model.result(Reader(self.m.read, self.m.mono), self.m.object({'items': self.m.array([])}))
        appdata = struct.unpack('<Q', self.m.read(self.context + 0x68, 8))[0]
        self.m.q(appdata + 0x10, 0)
        with self.assertRaises(DecodeError):
            self.acquire()


def audit(original):
    images = {k: Image((original / IMAGES[k][0]).read_bytes(), IMAGES[k][1]) for k in ('vsh', 'pui')}
    abi_slots(images)
    from inspect_appbrowse_aot_1360 import Image as AotImage
    native = AotImage(original / IMAGES['vsh'][0], 'vsh')
    for address, expected in ((0x4a141, 'ctxId'), (0x4a195, 'items')):
        code = native.read(address, 7)
        got = address + 7 + struct.unpack('<i', code[3:])[0]
        assert native.patch(got) == repr(expected)
    native.pe.close()
    # Independently check the complete register dataflow and Promise-side offsets.
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    for address, size, mnemonic, operand in (
        (0x127ed9, 4, 'movsxd', 'rax, dword ptr [r14 + 0x18]'),
        (0x127edd, 3, 'mov', 'r14, rcx'), (0x127ee0, 3, 'mov', 'dword ptr [rbp - 0x20], eax'),
        (0x127f04, 3, 'mov', 'rdi, r14'), (0x127f07, 4, 'movsxd', 'rsi, dword ptr [rbp - 0x20]'),
        (0x1cba43, 4, 'mov', 'rax, qword ptr [r14 + 0x10]'),
        (0x1cbab3, 4, 'mov', 'rax, qword ptr [r14 + 0x18]'),
        (0x92d44, 6, 'mov', 'al, byte ptr [rdi + 0x92]')):
        ins = list(cs.disasm(images['pui'].at(address, size), address))
        assert len(ins) == 1 and (ins[0].mnemonic, ins[0].op_str) == (mnemonic, operand)
    print('Pinned delivery register/backlink, Promise sides, retirement and result-property audits passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DeliveryTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.original:
        audit(args.original)
