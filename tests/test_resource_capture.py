"""Synthetic memory only. No process handles, emulator, or firmware writes."""
from copy import deepcopy
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from resource_capture import (ReadResult, collect_resource, load_expected, _expected,
                              MAX_ATTEMPTS, MAX_BYTES)


def put64(data, off, value):
    struct.pack_into('<Q', data, off, value)


def put32(data, off, value):
    struct.pack_into('<I', data, off, value & 0xffffffff)


def small_config():
    def state(name, **kw):
        return dict(name=name, vcn=3, vcnType=-1, acp=4, mem=5, priority=6, **kw)
    return dict(version=3, maxVcnBudget=100, maxAcpBudget=6000, maxMemBudget=399,
                exclusiveGroups=[{}, {}], firstWinGroups=[{}], clientComponents=[
                    dict(id=0, name='Test App', needConfirm=False, states=[state('RUNNING'), state('SUSPENDING')]),
                    dict(id=7, name='A component with heap name', needConfirm=True,
                         states=[state('ACTIVE', forcedConfirmResult=False, kickPriority=9)]),
                    dict(id=40, name='Inline', needConfirm=False, states=[]),
                    dict(id=0xffffffff, name='Unsigned ID', needConfirm=False, states=[state('RUNNING')])])


class Memory:
    def __init__(self, config=None):
        self.config = deepcopy(config if config is not None else small_config())
        self.expected = _expected(self.config)
        self.blocks = {}
        self.calls = []
        self.counts = {}
        self.hook = None
        self.core = 0x100000000
        self.owner = 0x200000000
        self.rule = 0x200010000
        self.head = 0x200020000
        self.control = 0x200030000
        self.nodes = []
        self.sentinels = []
        self.state_nodes = []
        self.name_pointers = []
        self.slot = self.core+0x20f8fb0
        self.blocks[self.slot] = bytearray(struct.pack('<Q', self.owner))
        self.blocks[self.owner+0x738] = bytearray(struct.pack('<QQ', self.rule, self.control))
        header = bytearray(72)
        h = bytearray(32)
        self.blocks[self.rule] = header
        self.blocks[self.head] = h
        components = sorted(self.config['clientComponents'], key=lambda c: c['id'])
        put64(header, 0x20, self.head)
        put64(header, 0x28, len(components))
        put64(header, 0x18, len(self.config['exclusiveGroups']))
        put64(header, 8, len(self.config['firstWinGroups']))
        for key, off in (('version', 0x44), ('maxVcnBudget', 0x30), ('maxAcpBudget', 0x3c), ('maxMemBudget', 0x40)):
            put32(header, off, self.config[key])
        for index, component in enumerate(components):
            a = 0x300000000+index*0x10000
            self.nodes.append(a)
            node = bytearray(96)
            self.blocks[a] = node
            put32(node, 0x20, component['id'])
            raw = component['name'].encode()
            put64(node, 0x38, len(raw))
            put64(node, 0x40, 15 if len(raw) < 16 else len(raw))
            if len(raw) < 16:
                node[0x28:0x28+len(raw)+1] = raw+b'\0'
            else:
                p = a+0x100
                self.name_pointers.append(p)
                self.blocks[p] = bytearray(raw+b'\0')
                put64(node, 0x28, p)
            node[0x48] = int(component.get('needConfirm', False))
            sentinel = a+0x200
            self.sentinels.append(sentinel)
            put64(node, 0x50, sentinel)
            states = component['states']
            put64(node, 0x58, len(states))
            addresses = [a+0x300+i*0x100 for i in range(len(states))]
            self.state_nodes.extend(addresses)
            self.blocks[sentinel] = bytearray(struct.pack('<QQ', addresses[0] if addresses else sentinel,
                                                        addresses[-1] if addresses else sentinel))
            for i, (address, state) in enumerate(zip(addresses, states)):
                data = bytearray(80)
                self.blocks[address] = data
                put64(data, 0, addresses[i+1] if i+1 < len(addresses) else sentinel)
                put64(data, 8, addresses[i-1] if i else sentinel)
                raw = state['name'].encode()[:31]
                data[0x10:0x10+len(raw)+1] = raw+b'\0'
                # Fixture-only chosen values for absent/unassessed fields are
                # intentionally nonzero; production expected never guesses them.
                for key, off in (('vcn', 0x30), ('vcnType', 0x38), ('acp', 0x3c),
                                 ('mem', 0x40), ('priority', 0x44)):
                    put32(data, off, state.get(key, 123))
                data[0x34] = state.get('scaninType', 0)
                put32(data, 0x48, state.get('kickPriority', state.get('priority', 123)))
                for key, off in (('needBigAppSuspended', 0x4c), ('needMediaAppKilled', 0x4d), ('need8kReady', 0x4e)):
                    data[off] = int(state.get(key, True))
                data[0x4f] = 2-int(state['forcedConfirmResult']) if 'forcedConfirmResult' in state else 0
        # Balanced BST with deepest incomplete level red: a valid RB tree for
        # arbitrary size, including 32/64. This isn't the production collector.
        red_level, m = 0, len(self.nodes)-1
        while m >= 0:
            red_level += 1
            m = m//2-1
        def build(lo, hi, parent, level):
            if lo > hi:
                return self.head
            mid = (lo+hi)//2
            a = self.nodes[mid]
            data = self.blocks[a]
            put64(data, 8, parent)
            put64(data, 0, build(lo, mid-1, a, level+1))
            put64(data, 0x10, build(mid+1, hi, a, level+1))
            data[0x18] = 0 if level == red_level else 1
            return a
        root = build(0, len(self.nodes)-1, self.head, 0)
        self.root = root
        for off, value in ((0, self.nodes[0] if self.nodes else self.head), (8, root),
                           (0x10, self.nodes[-1] if self.nodes else self.head)):
            put64(h, off, value)
        h[0x18:0x1a] = b'\1\1'

    def reader(self, address, size):
        self.calls.append((address, size))
        self.counts[address] = self.counts.get(address, 0)+1
        if self.hook:
            override = self.hook(address, size, self.counts[address])
            if override is not None:
                return override
        if address not in self.blocks:
            return ReadResult(b'', False, 'synthetic unmapped')
        data = bytes(self.blocks[address][:size])
        return ReadResult(data, len(data) == size)

    def capture(self, **kwargs):
        return collect_resource(self.reader, self.core, self.expected, **kwargs)


class CaptureTests(unittest.TestCase):
    def assert_bounded(self, m, r):
        self.assertLessEqual(len(m.calls), MAX_ATTEMPTS)
        self.assertLessEqual(sum(n for _, n in m.calls), MAX_BYTES)
        self.assertTrue(all(0 < n <= 96 for _, n in m.calls))
        self.assertEqual(r['read_attempts'], len(m.calls))
        self.assertEqual(r['requested_bytes'], sum(n for _, n in m.calls))
        self.assertFalse(r['atomic'])
        self.assertFalse(r['lifetime_pinned'])

    def test_nontrivial_tree_lists_names_signed_values_and_rechecks(self):
        m = Memory()
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'complete')
        self.assertTrue(r['structural_checks'])
        self.assertTrue(r['config_content_matches'])
        self.assertTrue(r['all_rechecked_bytes_equal'])
        self.assertTrue(r['publication_unchanged_during_read'])
        self.assertEqual([c['id'] for c in r['components']], [0, 7, 40, 0xffffffff])
        self.assertEqual(r['components'][0]['states'][1]['vcnType'], -1)
        self.assertEqual(r['components'][1]['states'][0]['forcedConfirmResultEncoded'], 2)
        self.assertEqual(m.calls[-1], (m.slot, 8))
        first = [s['address'] for s in r['snapshots'] if s['phase'] == 'initial']
        second = [s['address'] for s in r['snapshots'] if s['phase'] == 'recheck']
        self.assertEqual(first[::-1], second)
        self.assertTrue(all(s['finished_ns'] >= s['started_ns'] for s in r['snapshots']))
        self.assert_bounded(m, r)

    def test_config_identity_rejection(self):
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            load_expected(Path(__file__))

    def test_expected_mismatch_without_fabricating_observations(self):
        m = Memory()
        put32(m.blocks[m.state_nodes[0]], 0x30, 777)
        r = m.capture()
        self.assertTrue(r['traversal_complete'])
        self.assertFalse(r['config_content_matches'])
        self.assertIn('component[0].state[0].vcn', r['config_differences'])
        self.assertEqual(r['components'][0]['states'][0]['vcn'], 777)
        self.assertEqual(r['expected']['components'][0]['states'][0]['vcn'], 3)

    def test_absent_unproven_defaults_unassessed(self):
        config = small_config()
        del config['clientComponents'][0]['states'][0]['vcnType']
        m = Memory(config)
        r = m.capture()
        self.assertTrue(r['config_content_matches'])
        self.assertEqual(r['components'][0]['states'][0]['vcnType'], 123)
        self.assertIn('component[0].state[0].vcnType', r['unassessed_fields'])

    def test_no_owner_and_inconsistent_pair(self):
        m = Memory()
        put64(m.blocks[m.slot], 0, 0)
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'not_published')
        self.assertEqual(len(m.calls), 2)
        self.assertIsNone(r['config_content_matches'])
        self.assertIsNone(r['publication_unchanged_during_read'])
        m = Memory()
        put64(m.blocks[m.owner+0x738], 8, 0)
        self.assertEqual(m.capture()['stop_reason'], 'publication_inconsistent')

    def test_partial_initial_read_and_reader_exception(self):
        for exception in (False, True):
            with self.subTest(exception=exception):
                m = Memory()
                def fail(a, n, times):
                    if a == m.root and times == 1:
                        if exception:
                            raise OSError('synthetic read error')
                        return ReadResult(b'abc', False, 'synthetic partial')
                m.hook = fail
                r = m.capture()
                self.assertTrue(r['stop_reason'].startswith('read_unavailable:'))
                self.assertFalse(r['traversal_complete'])
                snapshot = next(s for s in r['snapshots'] if s['address'] == hex(m.root))
                self.assertEqual(snapshot['transferred'], 0 if exception else 3)
                self.assertEqual(r['root_rechecks']['owner'], True)
                self.assert_bounded(m, r)

    def test_short_read_claiming_complete_is_rejected(self):
        m = Memory()
        m.hook = lambda a, n, t: ReadResult(b'x', True) if a == m.slot else None
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'read_unavailable:owner')
        self.assertEqual(len(m.calls), 1)

    def test_failed_full_transfer_and_oversized_response_are_not_success(self):
        for data, complete in ((bytes(8), False), (bytes(100), True)):
            with self.subTest(size=len(data), complete=complete):
                m = Memory()
                m.hook = lambda a, n, t: ReadResult(data, complete, 'synthetic')
                r = m.capture()
                self.assertEqual(r['stop_reason'], 'read_unavailable:owner')
                self.assertLessEqual(len(r['snapshots'][0]['hex']), 16)
                self.assert_bounded(m, r)

    def test_mutated_reread_never_follows_new_pointer(self):
        m = Memory()
        unknown = 0x350000000
        def mutate(a, n, times):
            if a == m.root and times == 2:
                data = bytearray(m.blocks[a])
                put64(data, 0, unknown)
                return ReadResult(bytes(data))
        m.hook = mutate
        r = m.capture()
        self.assertFalse(r['all_rechecked_bytes_equal'])
        self.assertTrue(r['publication_unchanged_during_read'])
        self.assertNotIn(unknown, [a for a, _ in m.calls])
        self.assert_bounded(m, r)

    def test_mutated_publication_and_failed_reread(self):
        m = Memory()
        def mutate(a, n, times):
            if times == 2 and a == m.owner+0x738:
                return ReadResult(bytes(n))
            if times == 2 and a == m.state_nodes[0]:
                return ReadResult(b'xx', False, 'partial reread')
        m.hook = mutate
        r = m.capture()
        self.assertFalse(r['all_rechecked_bytes_equal'])
        self.assertFalse(r['publication_unchanged_during_read'])
        self.assertEqual(r['root_rechecks']['owner'], True)
        self.assertIsNotNone(r['recheck_stop_reason'])

    def test_cycles_and_overlaps(self):
        for kind in ('tree', 'state', 'overlap'):
            with self.subTest(kind=kind):
                m = Memory()
                if kind == 'tree':
                    put64(m.blocks[m.root], 0, m.root)
                elif kind == 'state':
                    put64(m.blocks[m.state_nodes[1]], 0, m.state_nodes[0])
                else:
                    put64(m.blocks[m.root], 0x50, m.root+8)
                r = m.capture()
                self.assertFalse(r['traversal_complete'])
                self.assertTrue(any(s in r['stop_reason'] for s in ('cycle', 'overlapping')))
                self.assert_bounded(m, r)

    def test_bounds_alignment_count_and_name_caps(self):
        for kind in ('low', 'high', 'alignment', 'components', 'states', 'name'):
            with self.subTest(kind=kind):
                m = Memory()
                if kind in ('low', 'high', 'alignment'):
                    put64(m.blocks[m.rule], 0x20, {'low':8, 'high':1 << 40, 'alignment':m.head+1}[kind])
                elif kind == 'components':
                    put64(m.blocks[m.rule], 0x28, 65)
                elif kind == 'states':
                    put64(m.blocks[m.root], 0x58, 33)
                else:
                    put64(m.blocks[m.root], 0x38, 65)
                r = m.capture()
                self.assertFalse(r['traversal_complete'])
                self.assert_bounded(m, r)

    def test_invalid_strings_flags_and_heap_alias(self):
        for kind in ('state_nul', 'component_nul', 'utf8', 'heap_alias', 'state_flag', 'component_flag'):
            with self.subTest(kind=kind):
                m = Memory()
                if kind == 'state_nul':
                    m.blocks[m.state_nodes[0]][0x2f] = 1
                elif kind == 'component_nul':
                    m.blocks[m.name_pointers[0]][-1] = 1
                elif kind == 'utf8':
                    m.blocks[m.name_pointers[0]][0] = 255
                elif kind == 'heap_alias':
                    put64(m.blocks[m.root], 0x28, m.root)
                elif kind == 'state_flag':
                    m.blocks[m.state_nodes[0]][0x4f] = 3
                else:
                    m.blocks[m.root][0x48] = 2
                r = m.capture()
                self.assertFalse(r['traversal_complete'])
                self.assert_bounded(m, r)

    def test_undeclared_list_longer_than_cap_and_global_state_cap(self):
        config = small_config()
        config['clientComponents'][0]['states'] *= 17
        m = Memory(config)
        put64(m.blocks[m.nodes[0]], 0x58, 1)  # Dishonest declared count cannot bypass traversal cap.
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'state_traversal_cap')
        self.assert_bounded(m, r)
        template = deepcopy(small_config()['clientComponents'][0])
        template['states'] *= 16
        config['clientComponents'] = [dict(deepcopy(template), id=i) for i in range(9)]
        m = Memory(config)
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'state_count_cap')
        self.assert_bounded(m, r)

    def test_shared_name_buffers_are_permitted(self):
        config = small_config()
        config['clientComponents'][2]['name'] = config['clientComponents'][1]['name']
        m = Memory(config)
        put64(m.blocks[m.nodes[2]], 0x28, m.name_pointers[0])
        r = m.capture()
        self.assertTrue(r['config_content_matches'])
        self.assertTrue(r['all_rechecked_bytes_equal'])

    def test_tree_list_integrity_checks(self):
        for kind in ('parent', 'order', 'root_color', 'black_height', 'extrema', 'tail', 'count', 'prev'):
            with self.subTest(kind=kind):
                m = Memory()
                if kind == 'parent':
                    put64(m.blocks[m.nodes[0]], 8, m.head)
                elif kind == 'order':
                    put32(m.blocks[m.nodes[0]], 0x20, 8)
                elif kind == 'root_color':
                    m.blocks[m.root][0x18] = 0
                elif kind == 'black_height':
                    m.blocks[m.nodes[-1]][0x18] = 1
                elif kind == 'extrema':
                    put64(m.blocks[m.head], 0, m.root)
                elif kind == 'tail':
                    put64(m.blocks[m.sentinels[0]], 8, m.sentinels[0])
                elif kind == 'count':
                    put64(m.blocks[m.nodes[0]], 0x58, 1)
                else:
                    put64(m.blocks[m.state_nodes[0]], 8, m.head)
                r = m.capture()
                self.assertFalse(r['structural_checks'])
                self.assertNotEqual(r['stop_reason'], 'complete')
                self.assert_bounded(m, r)

    def test_deadline_is_cooperative_and_prevents_more_reads(self):
        m = Memory()
        now = [0.0]
        def slow(a, n, times):
            now[0] = 2.01
        m.hook = slow
        r = m.capture(clock=lambda: now[0])
        self.assertEqual(r['stop_reason'], 'deadline')
        self.assertEqual(r['recheck_stop_reason'], 'deadline')
        self.assertEqual(len(m.calls), 1)
        self.assertFalse(r['all_rechecked_bytes_equal'])

    def test_exact_maximum_budget_and_no_unbounded_reads(self):
        config = small_config()
        template = deepcopy(config['clientComponents'][1])
        template['name'] = 'X'*64
        template['states'] *= 4
        config['clientComponents'] = [dict(deepcopy(template), id=i) for i in range(64)]
        m = Memory(config)
        r = m.capture()
        self.assertEqual(r['stop_reason'], 'complete')
        self.assertTrue(r['config_content_matches'])
        self.assertTrue(r['all_rechecked_bytes_equal'])
        self.assertEqual(len(m.calls), 904)
        self.assertEqual(sum(n for _, n in m.calls), 63872)
        self.assert_bounded(m, r)

    def test_empty_valid_tree_is_not_expected_population(self):
        config = small_config()
        config['clientComponents'] = []
        m = Memory(config)
        m.expected = _expected(small_config())
        r = m.capture()
        self.assertTrue(r['structural_checks'])
        self.assertFalse(r['config_content_matches'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
