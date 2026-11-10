"""Offline executor-chain controls; no process APIs."""
import argparse
from pathlib import Path
import struct
import unittest

from home_gate_trace_1360 import DecodeError, Image, IMAGES
from home_executor_trace_1360 import ExecutorCorrelator, PROBES, abi_slots
from test_home_delivery_trace_1360 import DeliveryTests, audit as delivery_audit


class ExecutorTests(unittest.TestCase):
    exact = DeliveryTests.exact
    make_promise = DeliveryTests.make_promise
    hit = DeliveryTests.hit
    result = DeliveryTests.result
    acquire = DeliveryTests.acquire

    def setUp(self):
        DeliveryTests.setUp(self)
        for name, off in [('queued_closure', 0x2010), ('executor_method', 0x2018), ('bridge_method', 0x2020)]:
            self.slots[name] = ('pui', off)
            self.values[name] = self.m.alloc()
            self.m.q(self.bases['pui'] + off, self.values[name])
        self.model = ExecutorCorrelator(self.rows.append, self.bases, self.slots, 100)
        self.graph()

    def graph(self, ident=30):
        self.closure = self.exact('queued_closure')
        self.bridge_object = self.m.obj('ReactNative.Bridge.ReactBridge')
        self.executor = self.m.obj('ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor')
        self.m.q(self.closure + 0x10, self.context)
        self.m.i(self.closure + 0x20, ident)
        self.m.q(self.closure + 0x18, 0xdeadbeef)  # must not read arguments
        data = struct.unpack('<Q', self.m.read(self.context + 0x30, 8))[0]
        self.m.q(data + 0x18, self.bridge_object)
        self.m.q(self.context + 0x20, 0)  # optional monitor, not the actual bridge
        self.m.q(self.bridge_object + 0x18, self.executor)
        vt = struct.unpack('<Q', self.m.read(self.executor, 8))[0]
        self.m.q(vt - 0x20, self.bases['pui'] + 0x64f50)

    def boundary(self, name, ident=30, **changes):
        frame = self.m.alloc() + 0x80
        self.m.q(frame + 8, self.bases['pui'] + 0x1e2922)
        self.m.i(frame - 0x10, ident)
        self.m.q(frame - 0x18, 0xcafebabe)  # converted, not closure+18
        vt = struct.unpack('<Q', self.m.read(self.executor, 8))[0]
        bridge_vt = struct.unpack('<Q', self.m.read(self.bridge_object, 8))[0]
        regs = dict(r15=self.closure, r14=self.bridge_object, r13=self.bridge_object, rbp=frame,
                    rdi=self.bridge_object if name == 'executor_queued' else self.executor,
                    rsi=ident, rdx=0xcafebabe,
                    rax=bridge_vt if name == 'executor_queued' else vt,
                    r10=self.values['bridge_method'] if name == 'executor_queued' else self.values['executor_method'])
        regs.update(changes)
        self.hit(name, **regs)

    def test_four_slots_and_full_chain(self):
        self.assertEqual(list(PROBES.values()), [('vsh', 0x196e17), ('pui', 0x1e291f),
                                               ('pui', 0x1cbfc0), ('pui', 0x1cbfc3)])
        self.acquire()
        for name in list(PROBES)[1:]:
            self.boundary(name)
            self.assertTrue(self.rows[-1]['correlated'])
        self.assertFalse(self.model.outstanding(10))
        self.assertNotIn('js_consumed', self.rows[-1])

    def test_full_graph_movement_between_every_boundary(self):
        self.acquire()
        for name in list(PROBES)[1:]:
            old = (self.context, self.closure, self.bridge_object, self.executor)
            self.bridge, self.context, self.instance = self.m.identity()
            self.graph()
            for pointer in old:
                self.m.fill(pointer, 256)
            self.boundary(name)
            self.assertTrue(self.rows[-1]['correlated'])

    def test_foreign_owner_and_no_acquisition(self):
        self.boundary('executor_queued')
        self.assertFalse(self.rows[-1]['correlated'])
        self.acquire()
        self.bridge, self.context, self.instance = self.m.identity(uid=b'2' * 16)
        self.graph()
        self.boundary('executor_queued')
        self.assertFalse(self.rows[-1]['correlated'])

    def test_missing_predecessor_and_duplicate_poison(self):
        self.acquire()
        self.boundary('executor_call')
        self.assertFalse(self.rows[-1]['correlated'])
        with self.assertRaisesRegex(DecodeError, 'executor_ids_poisoned'):
            self.boundary('executor_return')
        self.setUp(); self.acquire(); self.boundary('executor_queued')
        with self.assertRaisesRegex(DecodeError, 'executor_boundary_repeated'):
            self.boundary('executor_queued')

    def test_frame_receiver_and_argument_backlinks(self):
        self.acquire()
        for changes in ({'rdi': 0}, {'rsi': 0}, {'r10': 0}):
            with self.assertRaisesRegex(DecodeError, 'executor_queued_backlink'):
                self.boundary('executor_queued', **changes)
        for changes in ({'rbp': self.m.alloc()}, {'r13': 0}):
            with self.assertRaisesRegex(DecodeError, 'executor_frame_backlink'):
                self.boundary('executor_call', **changes)
        with self.assertRaisesRegex(DecodeError, 'executor_interface_registers'):
            self.boundary('executor_call', r10=0)

    def test_wrong_closure_and_bridge_types(self):
        with self.assertRaises(DecodeError):
            self.boundary('executor_queued', r15=self.context)
        data = struct.unpack('<Q', self.m.read(self.context + 0x30, 8))[0]
        self.m.q(data + 0x18, self.instance)
        with self.assertRaisesRegex(DecodeError, 'object_type'):
            self.boundary('executor_queued')

    def test_monitor_is_not_bridge_and_old_caller_rejected_before_dereference(self):
        self.acquire()
        self.m.q(self.context + 0x20, 0)
        self.boundary('executor_queued')
        self.assertTrue(self.rows[-1]['correlated'])
        frame = self.m.alloc() + 0x80
        self.m.q(frame + 8, self.bases['pui'] + 0x1e28e0)
        with self.assertRaisesRegex(DecodeError, 'executor_frame_backlink'):
            self.boundary('executor_call', rbp=frame, r15=0)
        self.boundary('executor_call')
        self.assertTrue(self.rows[-1]['correlated'])

    def test_return_null_does_not_mean_failure_or_success(self):
        self.acquire(); self.boundary('executor_queued'); self.boundary('executor_call')
        self.boundary('executor_return', rax=0, rdi=0, rsi=0, rdx=0, r10=0)
        self.assertTrue(self.rows[-1]['correlated'])
        self.assertFalse(self.rows[-1]['return_nonnull'])

    def test_reject_side_and_reused_id(self):
        self.acquire(); self.graph(31)
        for name in list(PROBES)[1:]:
            self.boundary(name, ident=31)
            self.assertEqual(self.rows[-1]['side'], 'reject')
        with self.assertRaisesRegex(DecodeError, 'delivery_ids_reused'):
            self.acquire(self.make_promise(self.instance, (31, 90)))
        with self.assertRaisesRegex(DecodeError, 'executor_ids_poisoned'):
            self.boundary('executor_return', ident=31)

    def test_termination_and_closed_coverage(self):
        self.acquire()
        self.m.put(self.context + 0x92, b'\x01')
        with self.assertRaisesRegex(DecodeError, 'delivery_context_terminated'):
            self.boundary('executor_queued')
        self.model.close_coverage(10, 'stop')
        with self.assertRaisesRegex(DecodeError, 'executor_coverage_closed'):
            self.boundary('executor_queued')

    def test_thunk_is_not_concrete_executor(self):
        self.acquire(); self.boundary('executor_queued')
        vt = struct.unpack('<Q', self.m.read(self.executor, 8))[0]
        self.m.q(vt - 0x20, 0xaf09c8999)
        self.boundary('executor_call')
        self.assertEqual(self.rows[-1]['target_kind'], 'other_or_interface_thunk')


def audit_bridge_metadata(original):
    # Separately decode Mono's pinned AOT PLT patch records and embedded PE
    # MethodDefs. IDA instruction flow alone cannot name these two callees.
    import inspect_appbrowse_aot_1360 as native
    native.HASHES['pui'] = IMAGES['pui'][1]
    image = native.Image(original / IMAGES['pui'][0], 'pui')
    names = {}
    for typ in image.pe.net.mdtables.TypeDef:
        for method in typ.MethodList:
            names[method.row_index] = f'{typ.TypeNamespace}.{typ.TypeName}::{method.row.Name}'
    for plt, expected in ((0x3103b0, 'ReactNative.PUI.PUIReactContext::get__bridge'),
                          (0x327790, 'ReactNative.PUI.PUI.PUIReactBridgeMonitor::InvokeCallback')):
        raw = image.read(plt, 5)
        assert raw[0] == 0xb8
        offset = struct.unpack('<I', raw[1:])[0]
        kind, cursor = image.decode(image.blob + offset)
        index, _ = image.decode(cursor)
        assert kind == 3 and names[index] == expected, (hex(plt), kind, index, names.get(index))
    assert image.patch(0x7c4228) == 'GOT[3790] kind=5 1916 5'
    assert names[0x1916] == 'ReactNative.Bridge.IReactBridge::InvokeCallback'
    assert image.patch(0x7c3340) == 'GOT[3313] kind=5 18f4 5'
    assert names[0x18f4] == 'ReactNative.Bridge.IJavaScriptExecutor::InvokeCallbackAndReturnFlushedQueue'
    print('Pinned Mono metadata identifies monitor, real bridge getter and both interfaces.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ExecutorTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.original:
        delivery_audit(args.original)
        images = {k: Image((args.original / IMAGES[k][0]).read_bytes(), IMAGES[k][1])
                  for k in ('vsh', 'pui')}
        abi_slots(images)
        audit_bridge_metadata(args.original)
        print('Pinned queued closure, preserved-register, executor-call/return ABI passed.')
