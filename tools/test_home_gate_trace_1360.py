"""Offline synthetic-memory and debugger-context controls; never opens a process."""
import argparse
import json
from pathlib import Path
import struct
import sys
import unittest
from types import SimpleNamespace

sys.dont_write_bytecode = True

from capture_home_gates_1360 import Threads, hardware_context
from home_gate_trace_1360 import (CALLERS, COLUMNS, EXCLUDED_CALLERS, HOME_HASH, IMAGES, WINDOWS,
                                Correlator, DecodeError, Image, Reader, ScopeExcluded, digest, provenance)


class Memory:
    def __init__(self):
        self.data, self.classes, self.next = {}, {}, 0x100000
        self.mono, self.slot = 0x90000000, 0
        self.bucket = self.alloc(4096)
        self.fill(self.mono + 0x4f98e0, 0x138)
        self.q(self.mono + 0x4f98e0, self.bucket)
        self.i(self.mono + 0x4f99e0, 32)

    def fill(self, p, size):
        self.data.update((p + i, 0) for i in range(size))

    def alloc(self, size=256):
        p = self.next
        self.next += (size + 15) & ~15
        self.fill(p, size)
        return p

    def put(self, p, data):
        self.data.update((p + i, b) for i, b in enumerate(data))

    def q(self, p, value):
        self.put(p, struct.pack('<Q', value))

    def i(self, p, value):
        self.put(p, struct.pack('<i', value))

    def read(self, p, size):
        try:
            return bytes(self.data[p + i] for i in range(size))
        except KeyError:
            raise RuntimeError('missing fixture memory') from None

    def obj(self, kind):
        if kind not in self.classes:
            namespace, name = kind.rsplit('.', 1)
            klass, vtable, ns, nm = (self.alloc() for _ in range(4))
            self.put(ns, namespace.encode() + b'\0')
            self.put(nm, name.encode() + b'\0')
            self.q(klass + 0x40, nm)
            self.q(klass + 0x48, ns)
            self.q(vtable, klass)
            self.classes[kind] = vtable
        p = self.alloc()
        self.q(p, self.classes[kind])
        return p

    def string(self, value):
        p = self.obj('System.String')
        self.i(p + 0x10, len(value))
        self.put(p + 0x14, value.encode('utf-16-le'))
        return p

    def weak(self, target, generic=True):
        p = self.obj('System.WeakReference`1' if generic else 'System.WeakReference')
        self.i(p + (0x10 if generic else 0x14), (self.slot << 3) | 1)
        self.q(self.bucket + self.slot * 8, (target ^ 0xffffffffffffffff) | 3)
        self.slot += 1
        return p

    def identity(self, user=100, uid=b'1' * 16):
        context = self.obj('ReactNative.PUI.PUIReactContext')
        app = self.obj('ReactNative.PUI.PUIReactApp')
        data = self.obj('ReactNative.PUI.AppData')
        manifest = self.obj('ReactNative.ReactApplicationManifest')
        context_data = self.obj('.Data')  # nested PUIReactContext/Data, not a manifest
        instance = self.obj('ReactNative.Bridge.ReactInstance')
        bridge = self.obj('ReactNative.Bridge.ReactContextImp')
        self.q(manifest + 0x30, self.string('rnps-home'))
        self.q(context + 0x30, context_data)
        self.q(context + 0x68, data)
        self.put(context + 0x88, b'\x01')
        self.i(context + 0x8c, user)
        self.q(app + 0x10, data)
        self.q(app + 0x30, manifest)
        self.q(app + 0x38, context)
        self.put(app + 0x98, uid)
        self.q(data + 0x10, self.weak(app, False))
        self.q(instance + 0x10, self.weak(context))
        self.q(bridge + 0x10, instance)
        return bridge, context, instance

    def value(self, value):
        p = self.obj('Newtonsoft.Json.Linq.JValue')
        if isinstance(value, str):
            box = self.string(value)
        else:
            box = self.obj('System.Int64')
            self.put(box + 0x10, struct.pack('<q', value))
        self.q(p + 0x30, box)
        return p

    def sequence(self, values):
        array = self.alloc(max(256, 32 + 8 * len(values)))
        self.q(array + 0x18, len(values))
        for i, value in enumerate(values):
            self.q(array + 0x20 + i * 8, value)
        seq = self.obj('System.Collections.Generic.List`1')
        self.q(seq + 0x10, array)
        self.i(seq + 0x20, len(values))
        return seq

    def array(self, values):
        p = self.obj('Newtonsoft.Json.Linq.JArray')
        self.q(p + 0x38, self.sequence(values))
        return p

    def object(self, fields):
        props = []
        for name, value in fields.items():
            prop = self.obj('Newtonsoft.Json.Linq.JProperty')
            child = self.alloc()
            self.q(child + 0x10, value)
            self.q(prop + 0x38, child)
            self.q(prop + 0x40, self.string(name))
            props.append(prop)
        col = self.obj('Newtonsoft.Json.Linq.JPropertyKeyedCollection')
        self.q(col + 0x10, self.sequence(props))
        obj = self.obj('Newtonsoft.Json.Linq.JObject')
        self.q(obj + 0x38, col)
        return obj


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.m = Memory()
        self.bridge, self.context, self.instance = self.m.identity()
        self.module = self.m.obj('ReactNative.Modules.Vsh.Database.DatabaseModule')
        self.m.q(self.module + 0x28, self.bridge)
        self.rows = []
        self.model = Correlator(self.rows.append)
        self.now = 0

    def reader(self, budget=65536):
        return Reader(self.m.read, self.m.mono, budget)

    def request(self, op, argument, context=1, promise=None):
        promise = promise or self.m.alloc()
        self.m.q(promise + 0x10, self.m.alloc())
        self.m.q(promise + 0x18, self.m.alloc())
        stack, delegate, closure = (self.m.alloc() for _ in range(3))
        self.m.q(stack, 0x80000000 + next(k for k, v in CALLERS.items() if v == op))
        self.m.q(delegate + 0x20, closure)
        self.m.q(closure + 0x10, argument)
        self.m.q(closure + (0x20 if op == 'condition' else 0x18), promise)
        regs = dict(rsp=stack, rdi=self.module, rsi=context, rdx=delegate, rcx=promise)
        self.model.request(self.reader(), regs, 0x80000000, self.now)
        self.now += 1
        return promise

    def settle(self, promise, result=0, outcome='resolve'):
        self.model.settlement(self.reader(), dict(rdi=promise, rsi=result), outcome, self.now)
        self.now += 1

    def condition(self, space='game', context=1, settle=True):
        fields = dict(userId=100, contentLimit=20,
                      filterTypeDispLocationExperienceSwitcher='display', filterTypeDispLocationSpace=space)
        p = self.request('condition', self.m.object({k: self.m.value(v) for k, v in fields.items()}), context)
        if settle:
            self.settle(p)
        return p

    def items(self, context=1, count=None):
        props = self.m.array([self.m.value(s) for s in COLUMNS])
        p = self.request('items2', self.m.object({'func': self.m.value('GetItems'), 'itemProps': props}), context)
        if count is not None:
            # Deliberately unreadable item pointers: decoder must only read count.
            self.settle(p, self.m.object({'items': self.m.array([0xdeadbeef] * count)}))
        return p

    def test_identity_unwrap_and_user(self):
        actual = self.reader().identity(self.bridge)
        self.assertEqual(actual['generation'], (b'1' * 16).hex())
        self.assertEqual(actual['user'], 100)
        themed = self.m.obj('ReactNative.UIManager.ThemedReactContext')
        self.m.q(themed + 0x10, self.bridge)
        self.assertEqual(actual, self.reader().identity(themed))

    def test_weak_collected_is_unknown(self):
        self.m.q(self.m.bucket + 8, 0)
        with self.assertRaisesRegex(DecodeError, 'weak_collected'):
            self.reader().identity(self.bridge)

    def test_empty_games_media_nonempty(self):
        self.condition()
        self.condition('media', 2)
        self.items(count=0)
        self.items(2, 3)
        self.assertIsNone(self.model.ready)
        counts = [(r['space'], r['item_count']) for r in self.rows if 'item_count' in r]
        self.assertEqual(counts, [('game', 0), ('media', 3)])

    def test_nonempty_games_handoff_requires_both(self):
        self.condition()
        self.items(count=2)
        self.assertIsNone(self.model.ready)
        self.condition('media', 2)
        self.items(2, 0)
        self.assertEqual(self.model.ready['requests']['game'][0], 2)

    def test_pending_is_observed_request_only(self):
        p = self.request('scalar2', self.revision_argument())
        self.assertTrue(self.rows[-1]['revision'])
        self.assertEqual(self.model.outstanding(10)[0]['promise'], hex(p))
        self.settle(p, outcome='reject')
        self.assertEqual(self.model.outstanding(11), [])
        self.assertEqual(self.rows[-1]['outcome'], 'reject')

    def test_lifecycle_gap_closes_association_and_freezes_observation(self):
        p = self.request('scalar2', self.revision_argument())
        self.model.close_coverage(10, 'phase_drain')
        before = len(self.rows)
        self.settle(p, outcome='reject')  # may be a reused pointer after the gap
        self.assertEqual(len(self.rows), before)
        closed = self.model.outstanding(100)[0]
        self.assertEqual(closed['observed_unsettled_seconds'], 10)
        self.assertEqual(closed['coverage_closed'], 'phase_drain')
        self.model.close_coverage(200, 'shutdown_drain')
        self.assertEqual(self.model.outstanding(300), [closed])

    def test_revision_zero_is_real_value(self):
        p = self.request('scalar2', self.revision_argument())
        self.settle(p, self.m.object({'ctxId': self.m.value(1),
                                    'scalarMap': self.m.object({'revision': self.m.value(0)})}))
        self.assertEqual(self.rows[-1]['revision_value'], 0)

    def revision_argument(self):
        return self.m.object({'scalarProps': self.m.array([self.m.value('revision')])})

    def test_revision_caller_shape_is_not_native_shape(self):
        self.request('scalar2', self.m.object({'scalarProp': self.m.value('revision')}))
        self.assertFalse(self.rows[-1]['revision'])

    def test_revision_batch_not_single_revision_request(self):
        self.request('scalar2', self.m.object({'scalarProps': self.m.array(
            [self.m.value('revision'), self.m.value('revision')])}))
        self.assertFalse(self.rows[-1]['revision'])

    def test_revision_flat_result_not_native_result(self):
        p = self.request('scalar2', self.revision_argument())
        self.settle(p, self.m.object({'ctxId': self.m.value(1), 'revision': self.m.value(7)}))
        self.assertNotIn('revision_value', self.rows[-1])
        self.assertEqual(self.rows[-1]['decode_status'], 'scalar_map_missing')

    def test_revision_result_context_must_match(self):
        p = self.request('scalar2', self.revision_argument())
        self.settle(p, self.m.object({'ctxId': self.m.value(2),
                                    'scalarMap': self.m.object({'revision': self.m.value(7)})}))
        self.assertNotIn('revision_value', self.rows[-1])
        self.assertEqual(self.rows[-1]['decode_status'], 'revision_context_mismatch')

    def test_late_attach_no_condition(self):
        self.items(count=2)
        self.assertEqual(self.rows[-1]['decode_status'], 'condition_not_correlated')
        self.assertIsNone(self.model.ready)

    def test_unknown_result_not_empty(self):
        self.condition()
        p = self.items()
        self.settle(p, self.m.object({'revision': self.m.value(0)}))
        self.assertNotIn('item_count', self.rows[-1])
        self.assertEqual(self.rows[-1]['decode_status'], 'items_missing')

    def test_null_callback_not_settlement_delivery(self):
        self.condition()
        p = self.items()
        self.m.q(p + 0x10, 0)
        self.settle(p)
        self.assertEqual(self.rows[-1]['decode_status'], 'callback_absent')
        self.assertIsNone(self.model.ready)

    def test_user_switch_and_disposal(self):
        self.condition()
        p = self.items()
        self.m.i(self.context + 0x8c, 101)
        self.settle(p)
        self.assertEqual(self.rows[-1]['decode_status'], 'generation_changed')
        self.m.i(self.context + 0x8c, 100)
        p = self.items()
        self.m.put(self.module + 0x40, b'\x01')
        self.settle(p)
        self.assertEqual(self.rows[-1]['decode_status'], 'module_disposed')

    def test_condition_epoch_reuse(self):
        self.condition()
        p = self.items()
        self.condition('media')  # same numeric ctxId reused
        self.settle(p, self.m.object({'items': self.m.array([])}))
        self.assertEqual(self.rows[-1]['decode_status'], 'condition_not_correlated')

    def test_promise_reuse_invalidates_old(self):
        arg = self.revision_argument()
        p = self.request('scalar2', arg)
        with self.assertRaisesRegex(DecodeError, 'promise_reused'):
            self.request('scalar2', arg, promise=p)
        self.assertEqual(self.model.outstanding(10), [])

    def test_cross_generation_counts_do_not_pair(self):
        self.condition()
        self.items(count=2)
        self.bridge, self.context, self.instance = self.m.identity(uid=b'2' * 16)
        self.m.q(self.module + 0x28, self.bridge)
        self.condition('media', 2)
        self.items(2, 1)
        self.assertIsNone(self.model.ready)

    def test_pending_condition_cannot_label_count(self):
        self.condition(settle=False)
        self.items(count=3)
        self.assertEqual(self.rows[-1]['decode_status'], 'condition_not_correlated')

    def test_untracked_settlement_silent(self):
        self.settle(0xdeadbeef)
        self.assertEqual(self.rows, [])

    def test_bounds_and_no_secrets(self):
        secret = 'secret-not-an-allowed-enum'
        obj = self.m.object({'unexpected': self.m.value(secret), 'items': self.m.array([])})
        self.assertEqual(self.reader().item_count(obj), 0)
        self.assertIsNone(self.reader().value(self.m.value(secret)))
        with self.assertRaisesRegex(DecodeError, 'read_budget'):
            self.reader(1).identity(self.bridge)
        array = self.m.array([])
        seq = struct.unpack('<Q', self.m.read(array + 0x38, 8))[0]
        self.m.i(seq + 0x20, -1)
        with self.assertRaisesRegex(DecodeError, 'list_count'):
            self.reader().children(array, True)
        self.assertNotIn(secret, json.dumps(self.rows))

    def test_predicate_and_dispatch_same_target(self):
        event = self.m.obj('ReactNative.UIManager.Events.ReactPerformanceTimingEvent')
        emitter = self.m.obj('ReactNative.UIManager.Events.RCTEventEmitter')
        handler, closure, view, manager, aux, frame, vtable = (self.m.alloc() for _ in range(7))
        self.m.q(emitter + 0x10, handler)
        self.m.q(handler + 0x10, self.instance)
        self.m.q(event + 0x20, self.m.string('interactive'))
        self.m.i(event + 0x14, 123)
        self.model.timing(self.reader(), dict(rdi=event, rsi=emitter))
        self.m.q(frame + 0x28, closure)
        self.m.q(closure + 0x10, view)
        self.m.q(closure + 0x20, manager)
        self.m.q(manager, vtable)
        self.m.q(vtable + 0x188, 0x12340000)
        self.m.q(view + 0x30, aux)
        self.m.q(aux + 0x10, self.bridge)
        self.m.put(aux + 0x38, b'\x01')
        self.m.i(aux + 0x3c, 123)
        self.model.timing(self.reader(), dict(rbp=frame + 0x40, rax=0), True)
        self.assertFalse(self.rows[-1]['result'])
        self.assertEqual(self.rows[-1]['generation'], self.rows[-2]['generation'])
        self.assertEqual(self.rows[-1]['tag'], self.rows[-2]['tag'])

    def test_hardware_changes_only_debug_and_rf(self):
        raw = bytearray(1232)
        struct.pack_into('<I', raw, 48, 0x100013)
        struct.pack_into('<I', raw, 68, 0x202)
        struct.pack_into('<Q', raw, 112, 0x400)
        struct.pack_into('<Q', raw, 120, 0x12345601)
        struct.pack_into('<Q', raw, 248, 0xabc000)
        saved = struct.unpack_from('<6Q', raw, 72)
        armed = hardware_context(raw, addresses=[1, 2, 3, 4])
        hit = bytearray(armed)
        struct.pack_into('<Q', hit, 104, 2)
        resumed = hardware_context(hit, hit=1)
        self.assertEqual(resumed[120:], raw[120:])
        self.assertEqual(struct.unpack_from('<I', resumed, 68)[0], 0x10202)
        restored = hardware_context(resumed, restore=saved)
        self.assertEqual(restored[72:120], raw[72:120])
        # A queued old-slot fault survives the phase rewrite; RF continuation
        # must preserve its original RIP/AL, not jump to the new slot address.
        queued = hardware_context(hit, addresses=[5, 6, 7, 8])
        self.assertEqual(hardware_context(queued, hit=1)[120:], raw[120:])
        struct.pack_into('<Q', hit, 104, 0x4002)
        with self.assertRaisesRegex(DecodeError, 'foreign_debug_cause'):
            hardware_context(hit, hit=1)

    def test_thread_arm_new_thread_phase_and_cleanup(self):
        contexts = {1: bytes(1232), 2: bytes(1232)}
        original = dict(contexts)
        closed, suspended, resumed = [], [], []
        threads = Threads.__new__(Threads)
        threads.saved = {}
        threads.held, threads.deferred, threads.draining = {}, set(), False
        threads.emit = self.rows.append
        threads.cap = SimpleNamespace(open_thread=lambda rights, inherit, tid: tid,
                                      close=closed.append,
                                      threads=lambda: list(contexts),
                                      suspend=lambda h: suspended.append(h) or 0,
                                      resume=lambda h: resumed.append(h) or 1)
        threads.alive = lambda h: True
        def context(handle, data=None):
            if data is None:
                return contexts[handle]
            # Model Windows ContextFlags: debug-only writes must not replace GPRs.
            self.assertEqual(struct.unpack_from('<I', data, 48)[0], 0x100010)
            merged = bytearray(contexts[handle])
            merged[72:120] = data[72:120]
            contexts[handle] = bytes(merged)
        threads.context = context
        threads.arm(1, [10, 20, 30, 40])
        threads.arm(1, [10, 20, 30, 40])  # duplicate CREATE_THREAD coverage
        threads.arm(2, [10, 20, 30, 40])
        threads.rearm([50, 20, 30, 40])
        self.assertEqual(struct.unpack_from('<4Q', contexts[2], 72), (50, 20, 30, 40))
        threads.freeze_all()
        for tid in threads.held:
            threads.restore_debug(tid)
        self.assertEqual(threads.sweep(), 0)
        self.assertEqual(set(threads.saved), {1, 2})  # retained across continuation
        # A late context reinstallation must be detected, repaired, and reverified.
        contexts[2] = hardware_context(contexts[2], addresses=[50, 20, 30, 40])
        self.assertEqual(threads.sweep(), 1)
        self.assertEqual(threads.sweep(), 0)
        with self.assertRaisesRegex(DecodeError, 'rearming_during_shutdown'):
            threads.rearm([50, 20, 30, 40])
        threads.release_holds()
        for tid in list(threads.saved):
            threads.exited(tid)
        self.assertEqual(contexts, original)
        self.assertEqual(suspended, resumed)
        self.assertEqual(closed, [1, 2])
        self.assertFalse(threads.saved)

    def test_occupied_debug_register_rejected(self):
        raw = bytearray(1232)
        struct.pack_into('<Q', raw, 72, 0x1234)
        closed = []
        threads = Threads.__new__(Threads)
        threads.saved = {}
        threads.draining = False
        threads.cap = SimpleNamespace(open_thread=lambda *args: 1, close=closed.append)
        threads.context = lambda handle: raw
        with self.assertRaisesRegex(DecodeError, 'debug_registers_occupied'):
            threads.arm(1, [10, 20, 30, 40])
        self.assertFalse(threads.saved)
        self.assertEqual(closed, [1])

    def test_failed_restore_retains_handle_original_and_hold(self):
        threads = Threads.__new__(Threads)
        original = (0, 0, 0, 0, 0, 0x400)
        threads.saved, threads.held = {7: (123, original)}, {7: 0}
        def failed(*_):
            raise OSError('synthetic SetThreadContext failure')
        threads.context = failed
        with self.assertRaises(OSError):
            threads.restore_debug(7)
        self.assertEqual(threads.saved, {7: (123, original)})
        self.assertEqual(threads.held, {7: 0})

    def test_thread_liveness_uses_signal_not_ambiguous_exit_code(self):
        threads = Threads.__new__(Threads)
        wait = lambda handle, timeout: 0  # exited, even if exit code itself is259
        threads.cap = SimpleNamespace(k=SimpleNamespace(WaitForSingleObject=wait))
        self.assertFalse(threads.alive(123))
        threads.cap.k.WaitForSingleObject = lambda handle, timeout: 258
        self.assertTrue(threads.alive(123))

    def dispatch_fixture(self):
        event = self.m.obj('ReactNative.UIManager.Events.ReactPerformanceTimingEvent')
        emitter = self.m.obj('ReactNative.UIManager.Events.RCTEventEmitter')
        handler = self.m.alloc()
        self.m.q(event + 0x20, self.m.string('interactive'))
        self.m.i(event + 0x14, 123)
        self.m.q(emitter + 0x10, handler)
        self.m.q(handler + 0x10, self.instance)
        return dict(rdi=event, rsi=emitter)

    def test_dispatch_type_errors_identify_exact_hop(self):
        regs = self.dispatch_fixture()
        weak = struct.unpack('<Q', self.m.read(self.instance + 0x10, 8))[0]
        data = struct.unpack('<Q', self.m.read(self.context + 0x68, 8))[0]
        for target, actual, site in (
            (regs['rdi'], 'Newtonsoft.Json.Linq.JValue', 'dispatch.event'),
            (regs['rsi'], 'ReactNative.Modules.Core.RCTDeviceEventEmitter', 'dispatch.emitter'),
            (self.instance, 'ReactNative.PUI.PUIReactContext', 'dispatch.instance'),
            (weak, 'System.WeakReference', 'dispatch.instance.weak'),
            (data, 'Newtonsoft.Json.Linq.JObject', 'identity.app_data'),
        ):
            with self.subTest(site=site):
                original = self.m.read(target, 8)
                self.m.put(target, self.m.read(self.m.obj(actual), 8))
                reader = self.reader()
                with self.assertRaises(DecodeError) as caught:
                    self.model.timing(reader, regs)
                diagnostic = reader.diagnostic(caught.exception)
                self.assertEqual(str(caught.exception), 'object_type')
                self.assertEqual(diagnostic['site'], site)
                self.assertEqual(diagnostic['observed_type'], actual)
                self.assertIn('expected_type', diagnostic)
                self.assertFalse(self.rows)
                self.m.put(target, original)

    def test_unknown_null_unreadable_type_diagnostics_are_redacted(self):
        regs = self.dispatch_fixture()
        secret_class = 'PrivateNamespace.secret-not-an-allowed-class'
        unknown = self.m.obj(secret_class)
        for ptr, classification in ((unknown, 'other'), (0, 'null'), (0xdeadbeef, 'unreadable')):
            reader = self.reader()
            with self.assertRaises(DecodeError) as caught:
                self.model.timing(reader, dict(regs, rsi=ptr))
            diagnostic = reader.diagnostic(caught.exception)
            self.assertEqual(diagnostic['site'], 'dispatch.emitter')
            self.assertEqual(diagnostic['observed_type'], classification)
            self.assertEqual(diagnostic['type_path']['dispatch.event'],
                             'ReactNative.UIManager.Events.ReactPerformanceTimingEvent')
            self.assertNotIn(secret_class, json.dumps(diagnostic))
            self.assertNotIn('secret-not', str(caught.exception))

    def test_non_home_before_user_initialization_is_explicitly_excluded(self):
        regs = self.dispatch_fixture()
        app = self.reader().weak(self.reader().q(self.reader().q(self.context + 0x68) + 0x10), False)
        manifest = self.reader().q(app + 0x30)
        private_name = 'private-application-name'
        self.m.q(manifest + 0x30, self.m.string(private_name))
        self.m.put(self.context + 0x88, b'\x00')
        reader = self.reader()
        with self.assertRaises(ScopeExcluded) as caught:
            self.model.timing(reader, regs)
        self.assertEqual(str(caught.exception), 'not_home')
        diagnostic = reader.diagnostic(caught.exception)
        self.assertEqual(diagnostic['site'], 'identity.application_name')
        self.assertNotIn(private_name, json.dumps(diagnostic))
        self.assertFalse(self.rows)
        self.assertFalse(self.model.requests)

    def test_manifest_requires_app_owner_and_backlinks(self):
        data = self.reader().q(self.context + 0x68)
        app = self.reader().weak(self.reader().q(data + 0x10), False)
        manifest = self.reader().q(app + 0x30)
        # A convincing old-owner decoy cannot rescue an absent/incorrect app manifest.
        self.m.q(self.context + 0x30, manifest)
        for wrong in (0, self.m.obj('.Data'), self.m.obj('Private.ManifestSubclass')):
            self.m.q(app + 0x30, wrong)
            reader = self.reader()
            with self.assertRaises(DecodeError) as caught:
                reader.identity(self.context)
            self.assertEqual(reader.diagnostic(caught.exception)['site'], 'identity.manifest')
        self.m.q(app + 0x30, manifest)
        for offset, original in ((0x38, self.context), (0x10, data)):
            self.m.q(app + offset, self.m.alloc())
            with self.assertRaisesRegex(DecodeError, 'generation_backlink'):
                self.reader().identity(self.context)
            self.m.q(app + offset, original)

    def test_unavailable_app_cannot_borrow_context_manifest(self):
        data = self.reader().q(self.context + 0x68)
        weak = self.reader().q(data + 0x10)
        app = self.reader().weak(weak, False)
        self.m.q(self.context + 0x30, self.reader().q(app + 0x30))
        self.m.q(self.context + 0x68, 0)
        with self.assertRaises(DecodeError):
            self.reader().identity(self.context)
        self.m.q(self.context + 0x68, data)
        slot = self.reader().i(weak + 0x14) >> 3
        self.m.q(self.m.bucket + slot * 8, 0)
        with self.assertRaisesRegex(DecodeError, 'weak_collected'):
            self.reader().identity(self.context)

    def test_home_without_launched_user_remains_unknown(self):
        self.m.put(self.context + 0x88, b'\x00')
        reader = self.reader()
        with self.assertRaises(DecodeError) as caught:
            self.model.timing(reader, self.dispatch_fixture())
        self.assertNotIsInstance(caught.exception, ScopeExcluded)
        self.assertEqual(str(caught.exception), 'user_unavailable')
        self.assertEqual(reader.diagnostic(caught.exception)['site'], 'identity.launched_user')
        self.assertFalse(self.rows)

    def test_untraced_execute_callers_never_read_or_associate_arguments(self):
        for ret in (*EXCLUDED_CALLERS, 0x12345):
            stack = self.m.alloc()
            self.m.q(stack, 0x80000000 + ret)
            reader = self.reader()
            with self.assertRaises(ScopeExcluded) as caught:
                # No other registers supplied: exclusion must precede all object reads.
                self.model.request(reader, dict(rsp=stack), 0x80000000, 0)
            self.assertEqual(str(caught.exception), 'caller_not_traced')
            diagnostic = reader.diagnostic(caught.exception)
            self.assertEqual(diagnostic['caller'], EXCLUDED_CALLERS.get(ret, 'unrecognized'))
            self.assertEqual(diagnostic['caller_rva'], hex(ret) if ret in EXCLUDED_CALLERS else None)
            self.assertEqual(diagnostic['site'], 'execute.caller')
        self.assertFalse(self.rows)
        self.assertFalse(self.model.requests)


def audit_manifest_layout(image):
    """Build an independent fixture from pinned PE types and native operands.

    Unlike Memory.identity, this never assumes context+30 is an app manifest.
    TypeDefOrRef field signatures establish ownership; AOT loads establish the
    offsets. No reconstructed reference assembly or external method map is used.
    """
    import dnfile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_MEM
    from prepare_shellui_managed_1360 import embedded_pe

    pe = dnfile.dnPE(data=embedded_pe(image.data))
    tables = pe.net.mdtables
    def name(row):
        return str(row.TypeNamespace) + '.' + str(row.TypeName)
    def typedef(fullname):
        rows = [t for t in tables.TypeDef.rows if name(t) == fullname]
        assert len(rows) == 1, fullname
        return rows[0]
    def field(owner, fieldname):
        rows = [f.row for f in owner.FieldList if str(f.row.Name) == fieldname]
        assert len(rows) == 1, fieldname
        return rows[0]
    def field_class(owner, fieldname):
        sig = field(owner, fieldname).Signature.value
        assert sig[:2] == b'\x06\x12', (fieldname, sig.hex())  # FIELD CLASS
        raw = sig[2:]
        if raw[0] < 0x80:
            assert len(raw) == 1
            token = raw[0]
        elif raw[0] < 0xc0:
            assert len(raw) == 2
            token = ((raw[0] & 0x3f) << 8) | raw[1]
        else:
            assert raw[0] < 0xe0 and len(raw) == 4
            token = int.from_bytes(raw, 'big') & 0x1fffffff
        tag, index = token & 3, token >> 2
        assert tag in (0, 1) and index > 0
        return (tables.TypeDef if tag == 0 else tables.TypeRef).rows[index - 1]

    context_type = typedef('ReactNative.PUI.PUIReactContext')
    app_type = typedef('ReactNative.PUI.PUIReactApp')
    data_type = typedef('ReactNative.PUI.AppData')
    manifest_type = typedef('ReactNative.ReactApplicationManifest')
    nested_type = field_class(context_type, '_data')
    assert name(nested_type) == '.Data'
    assert any(n.NestedClass.row is nested_type and n.EnclosingClass.row is context_type
               for n in tables.NestedClass.rows)
    for row in (context_type, app_type, data_type, manifest_type, nested_type):
        assert name(row.Extends.row) == 'System.Object'
    assert field_class(context_type, '<AppData>k__BackingField') is data_type
    assert field_class(app_type, '_appData') is data_type
    assert field_class(app_type, '_reactContext') is context_type
    assert field_class(app_type, '_appManifest') is manifest_type
    assert name(field_class(data_type, '_app')) == 'System.WeakReference'
    assert not any('Manifest' in str(f.row.Name) for f in context_type.FieldList)
    assert field(manifest_type, '<ApplicationName>k__BackingField').Signature.value == b'\x06\x0e'

    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    def displacement(rva, length, mnemonic, operands):
        ins = list(cs.disasm(image.at(rva, length), rva))
        assert len(ins) == 1 and (ins[0].mnemonic, ins[0].op_str) == (mnemonic, operands)
        mem = [op.mem for op in ins[0].operands if op.type == X86_OP_MEM]
        assert len(mem) == 1
        return mem[0].disp
    # These independent accessor paths distinguish the two +30 owners.
    context_data = displacement(0x92374, 4, 'mov', 'rax, qword ptr [rdi + 0x30]')
    assert displacement(0x92394, 4, 'mov', 'rax, qword ptr [rdi + 0x30]') == context_data
    context_appdata = displacement(0x92804, 4, 'mov', 'rax, qword ptr [rdi + 0x68]')
    app_manifest = displacement(0x8f180, 4, 'mov', 'rax, qword ptr [rcx + 0x30]')
    manifest_name = displacement(0x8f189, 4, 'mov', 'rax, qword ptr [rax + 0x30]')
    assert displacement(0x1154, 4, 'mov', 'rax, qword ptr [rdi + 0x30]') == manifest_name
    assert displacement(0x8f384, 4, 'mov', 'rax, qword ptr [rdi + 0x30]') == app_manifest
    data_weak = displacement(0x8d4f0, 4, 'mov', 'rdi, qword ptr [rcx + 0x10]')
    app_context = displacement(0x8e8b4, 4, 'mov', 'rax, qword ptr [rdi + 0x38]')
    app_data = displacement(0x8e92e, 4, 'mov', 'r13, qword ptr [r15 + 0x10]')
    uid = displacement(0x8d9d4, 8, 'vmovups', 'xmm0, xmmword ptr [rdi + 0x98]')
    initialized = displacement(0x92514, 7, 'cmp', 'byte ptr [rdi + 0x88], 0')
    user = displacement(0x9251d, 6, 'mov', 'eax, dword ptr [rdi + 0x8c]')

    m = Memory()
    context, app, data, manifest, nested = (m.obj(name(t)) for t in
        (context_type, app_type, data_type, manifest_type, nested_type))
    m.q(context + context_data, nested)
    m.q(context + context_appdata, data)
    m.q(data + data_weak, m.weak(app, False))
    m.q(app + app_context, context)
    m.q(app + app_data, data)
    m.q(app + app_manifest, manifest)
    m.q(manifest + manifest_name, m.string('rnps-home'))
    m.put(context + initialized, b'\x01')
    m.i(context + user, 123)
    m.put(app + uid, b'A' * 16)
    reader = Reader(m.read, m.mono)
    assert reader.identity(context)['generation'] == (b'A' * 16).hex()
    assert reader.identity(context)['user'] == 123

    def rejects(fn, code, site):
        r = Reader(m.read, m.mono)
        try:
            fn(r)
        except DecodeError as error:
            assert str(error) == code and r.diagnostic(error)['site'] == site
            return r.diagnostic(error)
        raise AssertionError('wrong manifest owner/layout accepted')
    # Reproduce the old psn39 diagnostic from actual declared field types.
    diagnostic = rejects(lambda r: r.typed(r.q(context + context_data), name(manifest_type),
                                          'identity.manifest'), 'object_type', 'identity.manifest')
    assert diagnostic['observed_type'] == 'other'
    # Wrong-owner decoy must never supply Home identity when the app manifest is absent.
    m.q(context + context_data, manifest)
    m.q(app + app_manifest, 0)
    rejects(lambda r: r.identity(context), 'read_bounds', 'identity.manifest')
    # Even a Data object with a plausible name string is not a manifest.
    m.q(app + app_manifest, nested)
    m.q(nested + manifest_name, m.string('rnps-home'))
    rejects(lambda r: r.identity(context), 'object_type', 'identity.manifest')
    m.q(app + app_manifest, manifest)
    m.q(app + app_context, m.alloc())
    rejects(lambda r: r.identity(context), 'generation_backlink', 'identity.app_backlinks')
    pe.close()
    print('Pinned embedded metadata/AOT manifest-owner audit and 4 real-layout negative controls passed.')


def audit(original, stage, home, log):
    """Pinned-image integration and full-file/mapping negative controls in memory."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    images, bases, _ = provenance(original, stage, home, log)
    audit_manifest_layout(images['pui'])
    # Derive property names from the pinned native method, independently of
    # synthetic JSON fixtures (caller-facing HBC properties differ).
    from inspect_appbrowse_aot_1360 import Image as AotImage
    native = AotImage(original / IMAGES['vsh'][0], 'vsh')
    for address, expected in ((0x196801, 'scalarProps'), (0x1969b0, 'ctxId'),
                              (0x196a62, 'scalarMap')):
        code = native.read(address, 7)
        assert code[:3] in (b'\x48\x8b\x35', b'\x48\x8b\x05')
        got = address + 7 + struct.unpack('<i', code[3:])[0]
        assert native.patch(got) == repr(expected)
    native.pe.close()
    print('Pinned native GetScalar2 request/result property audit passed.')
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    checks = [('pui', 0x88814, 4, 'mov', 'rax, qword ptr [rdi + 0x28]'),
              ('pui', 0x555b8, 3, 'mov', 'r14, rdi'),
              ('pui', 0x555bb, 4, 'mov', 'qword ptr [rbp - 0x38], rsi'),
              ('pui', 0x556ef, 4, 'mov', 'rdi, qword ptr [rbp - 0x38]'),
              ('pui', 0x87704, 4, 'mov', 'qword ptr [rdi + 0x10], rsi'),
              ('pui', 0x92514, 7, 'cmp', 'byte ptr [rdi + 0x88], 0'),
              ('pui', 0x177cc6, 4, 'movzx', 'r15, al'),
              ('pui', 0x16968f, 4, 'mov', 'rax, qword ptr [rax + 0x10]'),
              ('json', 0x2ca34, 4, 'mov', 'rax, qword ptr [rdi + 0x38]'),
              ('json', 0x30114, 4, 'mov', 'rax, qword ptr [rdi + 0x38]'),
              ('json', 0x30118, 4, 'mov', 'rax, qword ptr [rax + 0x10]'),
              ('corlib', 0x27cc74, 3, 'mov', 'eax, dword ptr [rax + 0x20]'),
              ('corlib', 0x27cc7f, 4, 'mov', 'rax, qword ptr [rax + 0x10]'),
              ('corlib', 0x27cc8d, 5, 'mov', 'rax, qword ptr [rax + rcx*8 + 0x20]')]
    for key, rva, length, mnemonic, operands in checks:
        ins = list(cs.disasm(images[key].at(rva, length), rva))
        assert len(ins) == 1 and (ins[0].mnemonic, ins[0].op_str) == (mnemonic, operands)
    for key, image in images.items():
        bad = bytearray(image.data)
        bad[-1] ^= 1
        try:
            Image(bytes(bad), IMAGES[key][1])
        except DecodeError as error:
            assert str(error) == 'image_hash'
        else:
            raise AssertionError('wrong image accepted')
        try:
            image.at(0xffffffffffff, 8)
        except DecodeError:
            pass
        else:
            raise AssertionError('wrong RVA accepted')
    # No on-disk firmware copies or mutations for provenance negative controls.
    class FakeLog:
        def __init__(self, text):
            self.text = text
        def read_text(self, **_):
            return self.text
    text = log.read_text(encoding='utf-8', errors='replace')
    name = IMAGES['vsh'][0]
    extra = f'\nplace {name} base=0x{bases["vsh"]:x} mapped=0x5e4000\n'
    for badlog in (text + extra, text.replace('place ' + name, 'missing ' + name)):
        try:
            provenance(original, stage, home, FakeLog(badlog))
        except DecodeError as error:
            assert str(error) == 'ambiguous_mapping_vsh'
        else:
            raise AssertionError('ambiguous mapping accepted')
    class WrongStage:
        def __truediv__(self, _):
            return self
        def read_bytes(self):
            return b'not the original image'
    for badstage, badhome, expected in ((WrongStage(), home, 'stage_diff_vsh'),
                                       (stage, WrongStage(), 'home_hash')):
        try:
            provenance(original, badstage, badhome, log)
        except DecodeError as error:
            assert str(error) == expected
        else:
            raise AssertionError('wrong provenance accepted')
    print('Pinned-image ABI audit and 14 hash/RVA/mapping/stage/Home negative controls passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--home-bundle', type=Path)
    parser.add_argument('--run-log', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TraceTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.original:
        audit(args.original, args.stage, args.home_bundle, args.run_log)
