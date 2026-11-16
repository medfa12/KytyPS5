"""Offline synthetic native-call, provenance, privacy and handoff controls.

No process APIs, emulator, real account/device credential or auth store needed.
"""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

from home_gate_trace_1360 import Reader, DecodeError, ScopeExcluded, digest
import retail_np_connect_trace_1360 as t
import capture_retail_np_connect_1360 as cli


class Memory:
    def __init__(self):
        self.data, self.reads = {}, []

    def put(self, address, value):
        self.data.update({address + i: byte for i, byte in enumerate(value)})

    def q(self, address, value):
        self.put(address, struct.pack('<Q', value))

    def i(self, address, value):
        self.put(address, struct.pack('<i', value))

    def read(self, address, size):
        self.reads.append((address, size))
        # Strict sparse mapping: following an unproven pointer fails the test.
        return bytes(self.data[address + i] for i in range(size))


class Scenario:
    def __init__(self):
        self.memory, self.rows = Memory(), []
        self.bases = dict(core=0x910000000, host=0x140000000)
        self.observer = t.Observer(102, lambda cap: None)
        self.model = self.observer.model(self.rows.append, self.bases)
        self.owner, self.receiver = (510 << 32) | 100, (30 << 32) | 102
        self.client, self.server_handle = 42, 18
        self.server, self.manager, self.state, self.ipmi_core = 0x710000, 0x720000, 0x730000, 0x740000
        self.handler = self.bases['core'] + 0x213cb08
        self.submit_sp, self.dispatch_sp, self.native_sp = 0x800000, 0x900000, 0xa00008
        self.submit_fp, self.dispatch_fp = self.submit_sp + 0x80, self.dispatch_sp + 0x80
        self.config, self.args = self.dispatch_fp + 0x230, self.dispatch_fp + 0x3a0
        m, c, h = self.memory, self.bases['core'], self.bases['host']
        m.q(c + 0x213cad8, self.server)
        m.q(c + 0x213cae8, self.server)
        m.q(self.server, h + t.SERVER_TABLE)
        m.q(h + t.SERVER_TABLE + 0x10, h + 0x93c6e0)
        m.q(h + t.SERVER_TABLE + 0x28, h + 0x93c9a0)
        m.q(self.handler, c + 0x202e0a0)
        m.q(c + 0x202e0c0, c + 0xcaae10)
        m.q(self.handler + 8, self.manager)
        m.q(self.state + 0x18, self.client)
        m.q(self.state + 0x10, self.owner)
        m.i(self.state + 0x38, 0)
        m.q(self.submit_fp + 0x140, 0)
        m.q(self.submit_fp + 0x148, 0)
        m.q(self.submit_sp + 0x30, self.server_handle)
        m.i(self.submit_fp + 0x10, 0)
        m.q(self.submit_fp + 0x160, 0x750000)
        m.i(self.dispatch_fp + 0x130, 1)
        m.q(self.dispatch_fp + 0x138, self.client)
        m.q(self.dispatch_fp + 0x148, self.owner)
        m.q(self.dispatch_fp + 0x4c8, self.ipmi_core - 0x2d0)
        m.q(self.dispatch_fp + 0x4d8, self.receiver)
        m.q(self.args, self.handler)
        m.q(self.args + 8, self.server)
        m.q(self.args + 16, self.config)
        m.q(self.args + 0x18, 0xdeadbeef)  # Must never be dereferenced.
        m.q(self.args + 0x20, 0)
        m.i(self.config, 100)
        m.q(self.config + 0x148, 0x10)
        m.q(self.native_sp - 0x50, c + 0xcaae83)
        m.i(self.dispatch_sp + 0x20, 0)
        m.i(self.dispatch_fp + 0x420, 0)
        self.registers = {
            'server_entry': dict(rdi=c + 0x213cae8),
            'submit': dict(rbp=self.submit_fp, rsp=self.submit_sp, r8=self.owner, r9=self.client,
                           r15=self.state, rcx=self.ipmi_core),
            'submit_return': dict(rbp=self.submit_fp, rsp=self.submit_sp),
            'dispatch_connect': dict(rbp=self.dispatch_fp, rsp=self.dispatch_sp, r9=self.args, r8=0x20),
            'new_client': dict(rdi=self.handler, rsi=self.server, rdx=self.config, rsp=self.native_sp),
            'session_register': dict(rdi=self.manager, rsi=0x760000, rdx=0x770000, rsp=self.native_sp - 0x50),
            'session_return': dict(rsp=self.native_sp - 0x48, rax=0),
            'new_client_result': dict(rsp=self.native_sp - 0x48, r14=0, r15=self.handler),
            'accept': dict(rbp=self.dispatch_fp, rsp=self.dispatch_sp, rcx=self.ipmi_core,
                           rdx=self.dispatch_fp + 0x420, r8=self.receiver, r9=self.client),
            'accept_return': dict(rbp=self.dispatch_fp, rsp=self.dispatch_sp),
        }

    def hit(self, name, tid=None, **changes):
        regs = dict(self.registers[name], **changes)
        self.model.hit(name, Reader(self.memory.read, 0), regs, len(self.rows) / 1000,
                       tid=tid if tid is not None else 200 if name in ('submit', 'submit_return') else 100)

    def start(self, returned=True):
        self.hit('server_entry')
        self.hit('submit')
        if returned:
            self.hit('submit_return')
        self.hit('dispatch_connect')
        self.hit('new_client')

    def full(self, returned=True):
        self.start(returned)
        for name in ('session_register', 'session_return', 'new_client_result', 'accept', 'accept_return'):
            self.hit(name)


def provenance():
    return dict(event='provenance', trace_mode='retail_np_connect', exe_sha256=t.EXE_HASH,
                diagnostic_version=7, lifecycle_version=2, pid=1234, created_filetime=5678,
                run_log_sha256='f' * 64,
                images=dict(host=dict(sha256=t.EXE_HASH, base='0x140000000'),
                            core=dict(sha256=t.CORE_HASH, base='0x910000000'),
                            observer=dict(contract=t.CONTRACT, sources={'test': 'test'}, fresh_barrier=True,
                                          core_pid=102, prefix_sha256='f' * 64,
                                          barrier_token_sha256=digest(('a' * 32).encode()))))


def trace(rows, **end_changes):
    end = dict(event='end', reason='focused_boundary_complete', cleanup_verified=True,
               cleanup=dict(detached=True), cleanup_errors=[], decode_unknowns=0)
    end.update(end_changes)
    return b''.join((json.dumps(row) + '\n').encode() for row in [provenance(), *rows, end])


class NativeConnectTests(unittest.TestCase):
    def setUp(self):
        self.s = Scenario()

    def test_full_chain_is_local_only_and_never_reads_payload(self):
        self.s.full()
        self.assertTrue(self.s.model.complete)
        report = cli.analyze(trace(self.s.rows), {'test': 'test'})
        self.assertTrue(report['completion_publication_observed'])
        self.assertTrue(report['native_connection_accepted'])
        self.assertFalse(report['consumer_completion_observed'])
        self.assertFalse(report['psn_authenticated'])
        output = json.dumps(self.s.rows)
        for private in (str(0xdeadbeef), hex(self.s.state), str(self.s.state), str(self.s.client)):
            self.assertNotIn(private, output)
        self.assertFalse(any(address == 0xdeadbeef for address, size in self.s.memory.reads))
        self.assertLess(sum(size for _, size in self.s.memory.reads), 512)
        self.assertNotEqual(self.s.owner & 0xffffffff, self.s.receiver & 0xffffffff)

    def test_callback_can_race_enqueue_return(self):
        self.s.full(returned=False)
        report = cli.analyze(trace(self.s.rows), {'test': 'test'})
        self.assertTrue(report['completion_publication_observed'])
        self.assertNotIn('np_retained_enqueue_return', self.s.model.steps)

    def test_same_process_client_uses_observed_owner_not_a_pid_guess(self):
        owner = (510 << 32) | 102
        self.s.owner = owner
        self.s.registers['submit']['r8'] = owner
        self.s.memory.q(self.s.state + 0x10, owner)
        self.s.memory.q(self.s.dispatch_fp + 0x148, owner)
        self.s.memory.i(self.s.config, 102)
        self.s.full()
        self.assertTrue(cli.analyze(trace(self.s.rows), {'test': 'test'})['native_connection_accepted'])

    def test_complete_plan_keeps_four_slots_and_never_reuses_retired_code(self):
        class Threads:
            saved = {}
            def rearm(self, addresses):
                self.last = tuple(addresses)
        threads = Threads()
        names, probes, retired, queued = list(t.INITIAL), dict(t.INITIAL), {}, {}
        addresses = [self.s.bases[key] + rva for key, rva in probes.values()]
        for event in ('server_entry', 'submit', 'submit_return', 'dispatch_connect', 'new_client',
                      'session_register', 'session_return', 'new_client_result', 'accept', 'accept_return'):
            self.s.hit(event)
            change = self.s.observer.transition()
            if change:
                updated, placements = change
                self.assertFalse(placements)
                queued = t.Observer.handoff(threads, names, addresses, probes, retired,
                                           updated, self.s.bases, queued)
            self.assertEqual(len(addresses), 4)
            self.assertEqual(len(set(addresses)), 4)
            self.assertFalse(set(addresses) & set(retired))
        self.assertEqual(len(retired), 6)
        self.assertTrue(self.s.model.complete)

    def test_pre_handoff_fault_is_owned_until_cleanup(self):
        class Threads:
            def __init__(self, raw):
                self.saved, self.raw = {100: (None, ())}, raw
            def context(self, handle):
                return self.raw
            def rearm(self, addresses):
                self.addresses = addresses
        bases = self.s.bases
        names, probes, retired = list(t.INITIAL), dict(t.INITIAL), {}
        addresses = [bases[key] + rva for key, rva in probes.values()]
        old = addresses[0]
        raw = bytearray(1232)
        struct.pack_into('<Q', raw, 248, old)
        struct.pack_into('<Q', raw, 104, 1)
        threads = Threads(raw)
        changes = list(t.INITIAL.items())
        changes[0] = 'submit_return', t.SITES['submit_return']
        changes[1] = 'session_register', t.SITES['session_register']
        queued = t.Observer.handoff(threads, names, addresses, probes, retired, dict(changes), bases, {})
        self.assertEqual(queued, {(100, old): 0})
        self.assertEqual(retired[old], (0, 'server_entry'))
        # The shared capture engine consumes retired owned traps without giving
        # them to the model as if their old phase were still covered.
        self.assertNotIn(old, addresses)

    def test_early_native_rejection_is_not_connection_success(self):
        self.s.start()
        rejection = -2141913083  # Test path: allocation failed, not a fake success.
        self.s.hit('new_client_result', r14=rejection)
        self.s.memory.i(self.s.dispatch_sp + 0x20, rejection)
        self.s.hit('accept')
        self.s.hit('accept_return')
        result = cli.analyze(trace(self.s.rows), {'test': 'test'})
        self.assertTrue(result['completion_publication_observed'])
        self.assertFalse(result['native_connection_accepted'])

    def test_enqueue_failure_has_no_ticket_or_native_auth_result(self):
        self.s.hit('submit')
        self.s.memory.i(self.s.submit_fp + 0x10, 16)
        self.s.hit('submit_return')
        self.assertTrue(self.s.model.complete)
        self.assertFalse(self.s.rows[-1]['ticket_present'])
        self.assertFalse(cli.analyze(trace(self.s.rows), {'test': 'test'})['completion_publication_observed'])

    def test_foreign_client_is_excluded_without_heap_or_payload_reads(self):
        self.s.hit('submit')
        self.s.memory.q(self.s.dispatch_fp + 0x138, 999)
        before = len(self.s.memory.reads)
        with self.assertRaises(ScopeExcluded):
            self.s.hit('dispatch_connect')
        self.assertEqual(self.s.memory.reads[before:], [(self.s.dispatch_fp + 0x138, 8)])

    def test_wrong_owner_and_nonempty_argument_rejected(self):
        with self.assertRaisesRegex(DecodeError, 'retained_owner'):
            self.s.hit('submit', r8=(510 << 32))
        self.s.memory.q(self.s.submit_fp + 0x140, 0xdeadbeef)
        with self.assertRaisesRegex(DecodeError, 'empty_local_argument'):
            self.s.hit('submit')

    def test_stale_retained_incarnation_rejected(self):
        self.s.memory.q(self.s.state + 0x18, 99)
        with self.assertRaisesRegex(DecodeError, 'retained_state'):
            self.s.hit('submit')

    def test_wrong_server_vtable_rejected(self):
        self.s.memory.q(self.s.server, self.s.bases['host'] + 4)
        with self.assertRaisesRegex(DecodeError, 'server_incarnation'):
            self.s.hit('server_entry')

    def test_native_entry_requires_exact_dispatched_arguments(self):
        self.s.hit('submit')
        self.s.hit('dispatch_connect')
        with self.assertRaisesRegex(DecodeError, 'native_entry_identity'):
            self.s.hit('new_client', rdx=self.s.config + 8)

    def test_callback_read_config_is_metadata_only(self):
        self.s.hit('submit')
        self.s.memory.q(self.s.config + 0x148, 0x20)
        with self.assertRaisesRegex(DecodeError, 'native_callback_abi'):
            self.s.hit('dispatch_connect')

    def test_session_registration_wrong_manager_or_caller_rejected(self):
        self.s.start()
        with self.assertRaisesRegex(DecodeError, 'session_register_identity'):
            self.s.hit('session_register', rdi=self.s.manager + 8)
        self.s.memory.q(self.s.native_sp - 0x50, self.s.bases['core'] + 0x10)
        with self.assertRaisesRegex(DecodeError, 'session_register_caller'):
            self.s.hit('session_register')

    def test_success_cannot_skip_real_session_registration(self):
        self.s.start()
        with self.assertRaisesRegex(DecodeError, 'success_requires_session_registration'):
            self.s.hit('new_client_result')

    def test_native_frame_reuse_and_duplicate_dispatch_rejected(self):
        self.s.start()
        self.s.hit('session_register')
        with self.assertRaisesRegex(DecodeError, 'session_return_frame'):
            self.s.hit('session_return', rsp=self.s.native_sp - 0x40)

    def test_accept_requires_same_handle_result_receiver_and_frame(self):
        self.s.start()
        self.s.hit('new_client_result', r14=-1)
        for values in ({'r9': 100}, {'r8': (31 << 32) | 102}, {'rbp': self.s.dispatch_fp + 8}):
            with self.assertRaisesRegex(DecodeError, 'accept_identity|host_frame'):
                self.s.hit('accept', **values)
        with self.assertRaisesRegex(DecodeError, 'accept_native_result'):
            self.s.hit('accept')  # native -1 but supplied acceptance still zero

    def test_accept_failure_does_not_publish_success(self):
        self.s.start()
        self.s.hit('session_register')
        self.s.hit('session_return')
        self.s.hit('new_client_result')
        self.s.hit('accept')
        self.s.memory.i(self.s.dispatch_fp + 0x420, 22)
        self.s.hit('accept_return')
        result = cli.analyze(trace(self.s.rows), {'test': 'test'})
        self.assertFalse(result['completion_publication_observed'])
        self.assertFalse(result['native_connection_accepted'])

    def test_owned_thread_exit_invalidates_coverage(self):
        self.s.hit('submit')
        with self.assertRaisesRegex(DecodeError, 'owned_thread_lifecycle_gap'):
            self.s.model.thread_boundary(200)

    def test_partial_observation_never_claims_deadlock_or_auth(self):
        self.s.start()
        self.s.hit('session_register')
        report = cli.analyze(trace(self.s.rows, reason='deadline'), {'test': 'test'})
        self.assertEqual(report['last_observed'], 'np_session_register_enter')
        self.assertFalse(report['completion_publication_observed'])
        self.assertFalse(report['psn_authenticated'])


class ProvenanceAndAnalysisTests(unittest.TestCase):
    def prefix(self, extra=''):
        return (f'Loading: C:/fixture/SceShellCore.elf\n'
                'ps5-vsh: place SceShellCore.elf base=0x910000000 mapped=0x23e5000 desired-in=0x910000000\n'
                'NP: source-bound Core pid=102 adapter install=0 (server not yet published)\n'
                'pre-guest-barrier: ready pid=1234 timeout_ms=300000 phase=relocated-before-guest names=Local\\KytyPreGuest-1234-' +
                'a' * 32 + '\n' + extra).encode()

    def parse(self, data):
        image = type('LayoutOnly', (), {'loads': [(1, 5, 0, 0, 0, 0x2000, 0x23e5000)]})()
        return t.prefix_provenance(data, Path('C:/fixture/SceShellCore.elf'), image, 1234, 'a' * 32)

    def test_prefix_requires_retail_selection_admission_and_target_barrier(self):
        result = self.parse(self.prefix('Authorization: Bearer TEST_ONLY_MUST_NOT_ECHO\n'))
        self.assertNotIn('TEST_ONLY_MUST_NOT_ECHO', json.dumps(result))
        for old, new in ((b'install=0', b'install=1'), (b'pid=1234', b'pid=5678'),
                         (b'0x910000000', b'0x910000001'), (b'0x23e5000', b'0x2000'),
                         (b'C:/fixture/', b'C:/other/'), (b'a' * 32, b'b' * 32)):
            with self.subTest(old=old), self.assertRaises(DecodeError):
                self.parse(self.prefix().replace(old, new))
        with self.assertRaises(DecodeError):
            self.parse(self.prefix('pre-guest-barrier: released\n'))

    def test_duplicate_provenance_and_oversized_prefix_reject(self):
        with self.assertRaises(DecodeError):
            self.parse(self.prefix() * 2)
        with self.assertRaisesRegex(DecodeError, 'prefix_budget'):
            self.parse(bytes(4 * 1024 * 1024 + 1))

    def test_old_profile_still_rejects_fix3(self):
        import capture_retail_plus_1360 as old
        with self.assertRaises(DecodeError):
            old.validate_profile(dict(trace_mode='retail_plus', exe_sha256=t.EXE_HASH,
                                      diagnostic_version=7, lifecycle_version=2))
        self.assertNotEqual(old.EXE_HASH, t.EXE_HASH)
        with self.assertRaisesRegex(DecodeError, 'candidate_pin'):
            t.HostImage(b'synthetic non-candidate binary')

    def test_release_requires_current_sources_exact_target_and_no_handoff(self):
        p = provenance()
        probes = {name: hex(int(p['images'][key]['base'], 16) + rva) for name, (key, rva) in t.INITIAL.items()}
        rows = [p, dict(event='armed', phase='retail_np_connect', probes=probes)]
        raw = lambda rs: b''.join((json.dumps(row) + '\n').encode() for row in rs)
        cli.validate_release(raw(rows), 1234, 5678, 'a' * 32, {'test': 'test'})
        for pid, created, token, sources in ((999, 5678, 'a' * 32, {'test': 'test'}),
                                            (1234, 999, 'a' * 32, {'test': 'test'}),
                                            (1234, 5678, 'b' * 32, {'test': 'test'}),
                                            (1234, 5678, 'a' * 32, {'test': 'changed'})):
            with self.assertRaises(DecodeError):
                cli.validate_release(raw(rows), pid, created, token, sources)
        with self.assertRaises(DecodeError):
            cli.validate_release(raw(rows + [dict(event='probe_handoff')]), 1234, 5678, 'a' * 32, {'test': 'test'})
        rows[1]['probes'] = dict(probes, submit='0x140000000')
        with self.assertRaisesRegex(DecodeError, 'release_initial'):
            cli.validate_release(raw(rows), 1234, 5678, 'a' * 32, {'test': 'test'})

    def test_analysis_rejects_injected_payload_fields_and_missing_session(self):
        s = Scenario()
        s.full()
        rows = copy.deepcopy(s.rows)
        rows[-1]['payload'] = 'TEST_ONLY_PRIVATE_DO_NOT_ECHO'
        with self.assertRaisesRegex(DecodeError, 'private_fields'):
            cli.analyze(trace(rows), {'test': 'test'})
        rows = [row for row in s.rows if row['event'] not in ('np_session_register_enter', 'np_session_register_return')]
        with self.assertRaisesRegex(DecodeError, 'real_session'):
            cli.analyze(trace(rows), {'test': 'test'})

    def test_incomplete_detach_budget_and_exception_intervals_not_reliable(self):
        s = Scenario()
        s.full()
        for changes in (dict(cleanup_verified=False), dict(reason='hit_budget'), dict(decode_unknowns=1)):
            result = cli.analyze(trace(s.rows, **changes), {'test': 'test'})
            self.assertFalse(result['completion_publication_observed'])
        rows = list(s.rows)
        rows.insert(3, dict(event='forwarded_exception', first=True, code='0xc0000005'))
        self.assertFalse(cli.analyze(trace(rows), {'test': 'test'})['reliable_interval'])
        rows = [dict(event='forwarded_exception', first=True, code='0xc0000005'), *s.rows]
        self.assertTrue(cli.analyze(trace(rows), {'test': 'test'})['reliable_interval'])
        with self.assertRaises(DecodeError):
            cli.analyze(trace(s.rows)[:-1], {'test': 'test'})

    def test_analysis_rejects_dispatch_thread_reuse_and_reordered_events(self):
        s = Scenario()
        s.full()
        rows = copy.deepcopy(s.rows)
        rows[-1]['tid'] = 777
        with self.assertRaisesRegex(DecodeError, 'dispatch_thread'):
            cli.analyze(trace(rows), {'test': 'test'})
        rows = copy.deepcopy(s.rows)
        rows[-1], rows[-2] = rows[-2], rows[-1]
        with self.assertRaisesRegex(DecodeError, 'analysis_order'):
            cli.analyze(trace(rows), {'test': 'test'})

    def test_bounded_file_does_not_read_or_overwrite_unbounded_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'synthetic-prefix'
            path.write_bytes(b'test' * 20)
            with self.assertRaisesRegex(DecodeError, 'fixture_budget'):
                cli.bounded_file(path, 10, 'fixture_budget')
            self.assertEqual(path.read_bytes(), b'test' * 20)

    def test_handoff_publishes_new_and_retired_traps_even_on_context_failure(self):
        class Threads:
            saved = {}
            def rearm(self, addresses):
                raise RuntimeError('synthetic SetThreadContext failure')
        bases = dict(core=0x910000000, host=0x140000000)
        names = list(t.INITIAL)
        addresses = [bases[key] + rva for key, rva in t.INITIAL.values()]
        previous = list(addresses)
        probes, retired = dict(t.INITIAL), {}
        updated = dict(t.INITIAL)
        items = list(updated.items())
        items[0] = 'submit_return', t.SITES['submit_return']
        items[1] = 'session_register', t.SITES['session_register']
        with self.assertRaises(RuntimeError):
            t.Observer.handoff(Threads(), names, addresses, probes, retired, dict(items), bases, {})
        self.assertIn(previous[0], retired)
        self.assertIn(previous[1], retired)
        self.assertIn(bases['host'] + t.HOST_SITES['submit_return'], addresses)
        self.assertEqual(set(probes), set(t.INITIAL) | {'submit_return', 'session_register'})


if __name__ == '__main__':
    unittest.main()
