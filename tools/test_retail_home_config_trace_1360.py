"""Synthetic-memory/receipt tests. Never opens or attaches to any process."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from home_gate_trace_1360 import Reader, DecodeError, ScopeExcluded, digest
from test_home_gate_trace_1360 import Memory
import retail_home_config_trace_1360 as trace
from capture_home_gates_1360 import replace_probe_slot, acknowledge_handoff_step
from capture_retail_home_config_1360 import validate_release, analyze, observer_sources
import prepare_retail_home_config_launcher as launcher


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.m = Memory()
        self.bridge_ctx, self.context, self.instance = self.m.identity(user=0x10010029)
        self.handler = self.m.obj('.JavaScriptModuleInvocationHandler')
        self.m.q(self.handler + 0x10, self.instance)
        bridge = self.m.obj('ReactNative.Bridge.ReactBridge')
        executor = self.m.obj('ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor')
        self.m.q(self.q(self.context + 0x30) + 0x18, bridge)
        self.m.q(bridge + 0x18, executor)
        native, impl = self.m.alloc(), self.m.alloc()
        self.runtime = self.m.alloc(0x2800)
        self.m.q(executor + 0x80, native)
        self.m.q(native + 0x78, impl)
        self.hermes = 0x980000000
        self.extensions = 0x9a0000000
        self.m.q(native, self.extensions + 0x95320)
        self.m.q(self.extensions + 0x95320 + 0x28, self.extensions + 0x2c1f0)
        self.m.q(self.extensions + 0x95320 + 0x30, self.extensions + 0x2c8d0)
        self.m.q(impl, self.hermes + 0x26c348)
        self.m.q(impl + 0x70, self.runtime)
        # Deliberately synthetic buffer, not console code. The hash override is
        # confined to each test's mock context; production pins are unchanged.
        data = bytearray(512)
        data[:8] = b'HBC-test'
        data[128:136] = b'RootTest'
        data[128 + 0x87:128 + 0x8a] = b'\x90\x4f\x04'
        self.data = bytes(data)
        functions = {3081: dict(offset=128, size=256, prefix=b'RootTest',
                               instructions={0x87: ('JmpFalse', [79, 4], b'\x90\x4f\x04')})}
        self.observer = trace.Observer(self.data, functions, Path('C:/fixture/common/ReactNative.PUI.dll.sprx'), 'a' * 32)
        self.rows = []
        self.model = self.observer.model(self.rows.append, {'hermes': self.hermes, 'extensions': self.extensions})
        self.buffer = self.m.alloc(512)
        self.m.put(self.buffer, self.data)
        self.frame = self.m.alloc()
        self.cstack = self.m.alloc(0x300)
        cb = self.m.alloc()
        self.m.q(cb + 0x10, self.buffer + 128)
        self.m.q(self.cstack + 0x200 - 0x190, cb)
        self.m.q(self.runtime + 0x2440, self.frame - 0x10)
        self.regs = dict(rdi=self.handler, rsi=self.m.string('runApplication'), r12=self.runtime, r10=self.buffer + 128 + 0x87,
                         r15=self.frame, rbp=self.cstack + 0x200)

    def q(self, p):
        return struct.unpack('<Q', self.m.read(p, 8))[0]

    def reader(self):
        return Reader(self.m.read, self.m.mono)

    def bind(self):
        self.model.hit('owner', self.reader(), self.regs, 0, tid=10)

    def root(self, raw):
        self.m.q(self.frame + 4 * 8, raw)
        with patch.object(trace, 'HOME_HASH', digest(self.data)):
            self.model.hit('false', self.reader(), self.regs, 1, tid=11)

    def test_owner_and_full_buffer_and_root_type_only(self):
        self.bind()
        self.root(0xfffa000000000000)
        self.root(0xffff000000123456)
        self.assertEqual([r['config_kind'] for r in self.rows if 'config_kind' in r], ['undefined', 'object'])
        self.assertEqual(sum(r['event'] == 'home_buffer_verified' for r in self.rows), 1)
        self.assertNotIn('123456', json.dumps(self.rows))

    def test_no_owner_is_not_silently_home(self):
        with self.assertRaises(ScopeExcluded):
            self.root(0)
        self.assertEqual(self.rows, [])

    def test_foreign_runtime_excluded(self):
        self.bind()
        self.regs['r12'] += 8
        with self.assertRaises(ScopeExcluded):
            self.root(0)

    def test_unreviewed_native_executor_slot_rejected(self):
        self.m.q(self.extensions + 0x95320 + 0x28, self.extensions + 0x2222)
        with self.assertRaisesRegex(DecodeError, 'not_audited_jsi_executor'):
            self.bind()

    def test_native_runtime_replacement_revalidated(self):
        self.bind()
        data = self.q(self.context + 0x30)
        bridge = self.q(data + 0x18)
        executor = self.q(bridge + 0x18)
        native = self.q(executor + 0x80)
        impl = self.q(native + 0x78)
        self.m.q(impl + 0x70, self.runtime + 8)
        with self.assertRaisesRegex(DecodeError, 'home_owner_expired'):
            self.root(0)

    def test_wrong_user_fails(self):
        self.m.i(self.context + 0x8c, 7)
        with self.assertRaisesRegex(DecodeError, 'home_user_mismatch'):
            self.bind()

    def test_expired_owner_fails(self):
        self.bind()
        self.m.put(self.context + 0x92, b'\x01')
        with self.assertRaisesRegex(DecodeError, 'home_owner_expired'):
            self.root(0)

    def test_changed_bundle_rejected(self):
        self.bind()
        self.m.put(self.buffer + 450, b'X')
        with self.assertRaisesRegex(DecodeError, 'home_live_buffer_hash'):
            self.root(0)

    def test_post_hash_instruction_mutation_rejected(self):
        self.bind()
        self.root(0xfffa000000000000)
        self.m.put(self.regs['r10'] + 1, b'X')
        with self.assertRaisesRegex(DecodeError, 'hbc_instruction_or_frame'):
            self.root(0)

    def test_frame_backlink_rejected(self):
        self.bind()
        self.m.q(self.runtime + 0x2440, self.frame + 8)
        with self.assertRaisesRegex(DecodeError, 'hermes_frame_backlink'):
            self.root(0)

    def test_second_generation_rejected(self):
        self.bind()
        app_data = self.q(self.context + 0x68)
        app = self.reader().weak(self.q(app_data + 0x10), False)
        self.m.put(app + 0x98, b'2' * 16)
        with self.assertRaisesRegex(DecodeError, 'home_generation_changed'):
            self.bind()

    def test_loader_requires_exact_path_and_lazy_base(self):
        p = self.m.alloc(512)
        text = str(self.observer.selected_pui).encode('utf-16-le')
        at = self.m.alloc(len(text))
        self.m.put(at, text)
        self.m.q(p + 0x80, at)
        self.m.q(p + 0x90, len(text) // 2)
        self.m.q(p + 0x98, len(text) // 2)
        self.m.q(p + 0xb0, 0xdc0000000)
        self.m.q(p + 0xc8, 0x860000)
        self.model.hit('loader', self.reader(), {'rax': p}, 0)
        self.assertEqual(self.observer.transition(), (trace.ACTIVE, {'pui': 0xdc0000000}))
        self.model.bases['pui'] = 0xdc0000000
        with self.assertRaisesRegex(DecodeError, 'pui_loaded_before_coverage'):
            self.model.hit('loader', self.reader(), {'rax': p}, 0)

    def test_await_is_not_native_pending(self):
        self.bind()
        self.model.record(dict(fid=3277, offset=0x63), 0)
        self.assertEqual(self.model.outstanding(10)[0]['native_promise_state'], 'unknown')
        self.model.record(dict(fid=3277, offset=0xf4), 10)
        self.assertEqual(self.model.outstanding(10), [])

    def test_count_bound_and_noncounts(self):
        for v in (0, 6, 1000000):
            self.assertEqual(trace.count_value(struct.unpack('<Q', struct.pack('<d', v))[0]), v)
        for v in (-1, 1.5, float('nan'), 1000001):
            with self.assertRaises(DecodeError):
                trace.count_value(struct.unpack('<Q', struct.pack('<d', v))[0])
        with self.assertRaises(DecodeError):
            trace.count_value(0xffff000000123456)


class HandoffTests(unittest.TestCase):
    def test_zero_dr6_requires_owned_queue_proof_and_preserves_registers(self):
        raw = bytearray(1232)
        struct.pack_into('<Q', raw, 120, 0x123456)
        with self.assertRaises(DecodeError):
            acknowledge_handoff_step(raw, 0)
        after = acknowledge_handoff_step(raw, 0, queued=True)
        expected = bytearray(raw)
        struct.pack_into('<I', expected, 68, 0x10000)
        self.assertEqual(after, expected)
        for cause in (2, 3, 0x4000):
            struct.pack_into('<Q', raw, 104, cause)
            with self.assertRaises(DecodeError):
                acknowledge_handoff_step(raw, 0, queued=True)

    def test_atomic_slot_zero_and_retired_pending_site(self):
        names, addresses, retired = ['loader', 'a', 'b', 'c'], [100, 200, 300, 400], {}
        probes = {n: ('x', a) for n, a in zip(names, addresses)}
        updated = {'owner': ('x', 500), **{n: probes[n] for n in names[1:]}}
        class Threads:
            saved = {}
            def rearm(self, a): self.addresses = a
        threads = Threads()
        replace_probe_slot(threads, names, addresses, probes, retired, updated, {'x': 0})
        self.assertEqual(retired, {100: (0, 'loader')})
        self.assertEqual(addresses, [500, 200, 300, 400])
        self.assertIn('loader', probes)
        with self.assertRaises(DecodeError):
            replace_probe_slot(threads, names, addresses, probes, retired, updated, {'x': 0})

    def test_nonzero_slot_change_rejected_before_write(self):
        class Threads:
            def rearm(self, a): raise AssertionError('must not write')
        with self.assertRaises(DecodeError):
            replace_probe_slot(Threads(), list('abcd'), [1, 2, 3, 4], {}, {},
                               {'x': ('m', 5), 'b': ('m', 6), 'c': ('m', 3), 'd': ('m', 4)}, {'m': 0})


class ReceiptTests(unittest.TestCase):
    def rows(self):
        images = {k: {'sha256': sha} for k, (_, sha) in trace.PINS.items()}
        images['home'] = {'sha256': trace.HOME_HASH}
        images['observer'] = {'contract': 'retail-home-config-v1', 'sources': observer_sources()}
        return [dict(event='provenance', trace_mode='retail_config', diagnostic_version=7, lifecycle_version=2,
                     pid=10, created_filetime=123, exe_sha256=trace.EXE_HASH, images=images),
                dict(event='armed', phase='retail_config', probes={n: '0x100' for n in trace.INITIAL})]

    def test_release_complete_armed_only(self):
        rows = self.rows()
        data = lambda: b''.join(json.dumps(r).encode() + b'\n' for r in rows)
        validate_release(data() + b'{partial', 10, 123)
        with self.assertRaises(DecodeError): validate_release(data(), 11, 123)
        rows.append({'event': 'shutdown_begin'})
        with self.assertRaises(DecodeError): validate_release(data(), 10, 123)

    def test_wrong_firmware_release_refused(self):
        rows = self.rows()
        rows[0]['images']['pui']['sha256'] = 'historical-image'
        with self.assertRaises(DecodeError):
            validate_release(b''.join(json.dumps(r).encode() + b'\n' for r in rows), 10, 123)

    def test_publication_order_and_decode_gaps(self):
        rows = self.rows() + [dict(event='home_owner', generation='1', user=0x10010029),
                             dict(event='home_buffer_verified', sha256=trace.HOME_HASH)]
        def hit(fid, pc, **kw):
            return dict(event='home_hbc', generation='1', user=0x10010029, fid=fid, offset=pc, opcode='Ret', **kw)
        rows += [hit(3081, 0x87, config_kind='undefined'), hit(3277, 0xee),
                 hit(3081, 0x87, config_kind='object'), hit(3277, 0xf4),
                 dict(event='end', cleanup_verified=True, decode_unknowns=0)]
        with tempfile.TemporaryDirectory(dir=Path(tempfile.gettempdir()) / 'opencode') as temp:
            p = Path(temp) / 'trace.jsonl'
            def result():
                p.write_text(''.join(json.dumps(r) + '\n' for r in rows))
                return analyze(p)
            self.assertTrue(result()['publication_chain_observed'])
            rows[-1]['decode_unknowns'] = 1
            self.assertFalse(result()['publication_chain_observed'])
            rows[-1]['decode_unknowns'] = 0
            rows[4:7] = [rows[6], rows[5], rows[4]]
            self.assertFalse(result()['publication_chain_observed'])


class LauncherTests(unittest.TestCase):
    def test_only_token_assignment_is_added(self):
        source = ("import os\n" + launcher.ANCHOR + "\nuntouched = 1\n").encode()
        with patch.object(launcher, 'SOURCE_SHA256', digest(source)):
            output = launcher.transform(source, 'a' * 32)
        addition = ("env['KYTY_PRE_GUEST_GATE_TOKEN'] = '" + 'a' * 32 + "'\n").encode()
        self.assertEqual(output.replace(addition, b''), source)
        self.assertEqual(output.count(addition), 1)

    def test_changed_launcher_needs_new_review(self):
        with self.assertRaisesRegex(ValueError, 'parent launcher changed'):
            launcher.transform(b'unreviewed', 'a' * 32)

    def test_invalid_token_never_emits_python(self):
        for value in ('', 'A' * 32, "'\nimport os", 'a' * 31):
            with self.assertRaises(ValueError):
                launcher.transform(b'unreviewed', value)


if __name__ == '__main__':
    unittest.main()
