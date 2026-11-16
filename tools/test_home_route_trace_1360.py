"""Offline route controls: bounded synthetic memory, no process access."""
import struct
import unittest
from home_gate_trace_1360 import Reader, DecodeError
from home_transition_trace_1360 import RouteModel, Observer, ANCHORS, audit


class Tests(unittest.TestCase):
    def setUp(self):
        self.base, self.stack, self.job, self.owner = 0x930000000, 0x200100, 0x300000, 0x400000
        self.vtable = self.base + 0x1169df0
        self.memory, self.rows = {}, []
        self.model = RouteModel(self.rows.append, self.base)
        self.put(self.stack, self.base + 0x12345)
        self.put(self.job, self.vtable)
        self.put(self.vtable + 0x18, self.base + 0xeb6e0)
        self.put(self.job + 0x38, self.owner)
        self.put(self.owner, 528384)
        self.g = dict(rsp=self.stack, rbp=0x200200, rdi=self.job, rsi=0xa0)

    def put(self, address, value):
        for i, b in enumerate(struct.pack('<Q', value)):
            self.memory[address + i] = b

    def read(self, address, size):
        try:
            return bytes(self.memory[address + i] for i in range(size))
        except KeyError:
            raise OSError('unmapped')

    def hit(self, name, tid=1, **changes):
        self.model.hit(name, Reader(self.read, 0), {**self.g, **changes}, 0, tid)

    def particle(self, tid=1, **changes):
        self.put(self.stack - 8, self.g['rbp'])
        self.put(self.stack - 16, self.base + 0xeb6ed)
        self.hit('particle_entry', tid, **dict(rsp=self.stack - 16, rbp=self.stack - 8,
                                             rdi=self.owner, **changes))

    def test_exact_four_slots(self):
        self.assertEqual(list(Observer('route').probes.values()),
                         [('shell', 0xd9330), ('shell', 0xd9460), ('shell', 0xeb6e0), ('shell', 0xea340)])

    def test_new_route_instruction_tamper_rejected(self):
        for target in (0xd9330, 0xeb6e0):
            class Image:
                def at(self, address, size):
                    raw = bytes.fromhex(ANCHORS[address])
                    return bytes([raw[0] ^ 1]) + raw[1:] if address == target else raw
            with self.assertRaisesRegex(DecodeError, 'instruction_abi'):
                audit(Image())

    def test_controls_are_independent_and_not_readiness(self):
        self.hit('initialize_entry')
        self.hit('update_entry', rdi=self.owner)
        self.assertFalse(self.model.active)
        self.assertNotIn('invocation', self.rows[-1])
        self.assertNotIn('renderer_ready_byte', self.rows[-1])
        self.assertEqual(self.rows[-1]['flow_word_0'], 528384)

    def test_callback_pair_survives_independent_control(self):
        self.hit('resource_entry')
        self.hit('update_entry', rdi=self.owner)
        self.particle()
        self.assertTrue(self.rows[-1]['callback_edge_verified'])
        self.assertEqual(self.rows[0]['invocation'], self.rows[-1]['invocation'])
        self.assertFalse(self.model.outstanding(0))

    def test_late_and_cross_thread_never_pair(self):
        self.hit('resource_entry')
        with self.assertRaisesRegex(DecodeError, 'no_observed_callback'):
            self.particle(2)
        self.assertTrue(self.model.outstanding(0))
        self.model.thread_boundary(1)
        with self.assertRaisesRegex(DecodeError, 'no_observed_callback'):
            self.particle()

    def test_nested_poison_and_boundary_reset(self):
        self.hit('resource_entry')
        with self.assertRaisesRegex(DecodeError, 'nested_or_unfinished'):
            self.hit('resource_entry')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.particle()
        self.model.thread_boundary(1)
        self.hit('resource_entry')
        self.particle()

    def test_changed_owner_invalidates(self):
        self.hit('resource_entry')
        self.put(self.job + 0x38, self.owner + 8)
        with self.assertRaisesRegex(DecodeError, 'fresh_owner'):
            self.particle()
        self.assertFalse(self.model.active)

    def test_changed_vtable_invalidates(self):
        self.hit('resource_entry')
        self.put(self.job, self.vtable + 8)
        with self.assertRaisesRegex(DecodeError, 'fresh_job'):
            self.particle()

    def test_wrong_callback_slot(self):
        self.put(self.vtable + 0x18, self.base + 0xeb700)
        with self.assertRaisesRegex(DecodeError, 'callback_vtable'):
            self.hit('resource_entry')

    def test_forged_table_with_same_slot_rejected(self):
        self.put(self.job, 0x500000)
        self.put(0x500018, self.base + 0xeb6e0)
        with self.assertRaisesRegex(DecodeError, 'callback_vtable'):
            self.hit('resource_entry')

    def test_saved_rbp_and_argument_rechecked(self):
        for wrong_frame in (True, False):
            self.setUp()
            self.hit('resource_entry')
            self.put(self.stack - 8, self.g['rbp'] + int(wrong_frame))
            self.put(self.stack - 16, self.base + 0xeb6ed)
            with self.assertRaisesRegex(DecodeError, 'callback_backlink' if wrong_frame else 'fresh_owner'):
                self.hit('particle_entry', rsp=self.stack - 16, rbp=self.stack - 8,
                         rdi=self.owner if wrong_frame else self.owner + 8)

    def test_changed_outer_caller(self):
        self.hit('resource_entry')
        self.put(self.stack, self.base + 0x12346)
        with self.assertRaisesRegex(DecodeError, 'callback_backlink'):
            self.particle()

    def test_wrong_stack_frame(self):
        self.hit('resource_entry')
        self.put(self.stack - 24, self.base + 0xeb6ed)
        with self.assertRaisesRegex(DecodeError, 'callback_frame'):
            self.hit('particle_entry', rsp=self.stack - 24, rbp=self.stack - 8, rdi=self.owner)

    def test_wrong_particle_caller(self):
        self.hit('resource_entry')
        with self.assertRaisesRegex(DecodeError, 'particle_caller'):
            self.hit('particle_entry')

    def test_flow_size_and_unreadable(self):
        with self.assertRaisesRegex(DecodeError, 'flow_size'):
            self.hit('update_entry', rsi=159)
        with self.assertRaisesRegex(DecodeError, 'unreadable'):
            self.hit('update_entry', rdi=0x600000)

    def test_closed_and_budget(self):
        self.model.serial = 1024
        with self.assertRaisesRegex(DecodeError, 'invocation_budget'):
            self.hit('resource_entry')
        self.model.close_coverage(0, 'end')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.hit('initialize_entry')

    def test_duplicate_particle_does_not_reuse_pair(self):
        self.hit('resource_entry')
        self.particle()
        with self.assertRaisesRegex(DecodeError, 'no_observed_callback'):
            self.particle()


if __name__ == '__main__':
    unittest.main()
