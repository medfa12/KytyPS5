"""Offline coverage checks for RPC snapshots, not a corruption reproducer."""
import struct
import unittest

from capture_host_rpc_boundaries import Model
from home_gate_trace_1360 import Reader


class Boundaries(unittest.TestCase):
    def setUp(self):
        self.rows = []
        self.model = Model(self.rows.append, {'rpc': 0x100000}, {'rpc': 0x10000})
        self.memory = bytearray(0x300000)
        self.reader = Reader(lambda at, n: bytes(self.memory[at:at+n]), 0)
        self.regs = dict(rsp=0x200000, rdx=0x210000, rax=0, rip=0x300000)
        struct.pack_into('<Q', self.memory, self.regs['rsp'] + 0x48, self.regs['rdx'])
        struct.pack_into('<QII', self.memory, self.regs['rdx'], 0x101000, 0x89abcdef, 0x10000)
        struct.pack_into('<Q', self.memory, 0x1010a0, 0x102000)

    def test_change_is_interval_not_writer(self):
        self.model.hit('created', self.reader, self.regs, 0, tid=1)
        self.model.hit('option', self.reader, self.regs, 1, tid=1)
        struct.pack_into('<Q', self.memory, self.regs['rdx'], 0x7ff900000000)
        self.model.hit('auth', self.reader, self.regs, 2, tid=1)
        self.model.hit('unbind', self.reader, self.regs, 3, tid=1)
        self.assertNotIn('status', self.rows[-1])
        self.assertFalse(self.rows[1]['changed_since_previous_boundary'])
        self.assertTrue(self.rows[2]['changed_since_previous_boundary'])
        self.assertTrue(self.rows[2]['free_slot_unreadable'])
        self.assertFalse(self.rows[2]['vtable_in_rpc_image'])
        self.assertEqual(self.rows[2]['previous_vtable'], '0x101000')
        self.assertEqual(self.model.outstanding(4), [])

    def test_late_attach_unbind_is_uncorrelated(self):
        self.model.hit('unbind', self.reader, self.regs, 0, tid=1)
        self.assertFalse(self.rows[-1]['creation_observed'])
        self.assertIsNone(self.rows[-1]['binding'])

    def test_failure_does_not_read_output(self):
        self.regs.update(rax=5, rsp=0)
        self.model.hit('created', self.reader, self.regs, 0, tid=1)
        self.assertTrue(self.rows[-1]['creation_failed'])
        self.assertEqual(self.model.outstanding(0), [])

    def test_other_thread_frame_does_not_claim_creation(self):
        self.model.hit('created', self.reader, self.regs, 0, tid=1)
        self.model.hit('option', self.reader, self.regs, 1, tid=2)
        self.assertFalse(self.rows[-1]['creation_observed'])
        self.model.thread_boundary(1)
        self.assertEqual(self.model.outstanding(2), [])


if __name__ == '__main__':
    unittest.main()
