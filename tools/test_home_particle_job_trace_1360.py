"""Offline synthetic-memory tests; never opens processes or firmware stores."""
import struct
import unittest
from home_gate_trace_1360 import Reader, DecodeError
from home_particle_job_trace_1360 import Model, mode_write, audit, ANCHORS
from home_transition_trace_1360 import Observer


class Tests(unittest.TestCase):
    def setUp(self):
        self.base, self.owner, self.job, self.scheduler = 0x930000000, 0x200000, 0x300000, 0x400000
        self.stack, self.frame = 0x500000, 0x500100
        self.memory, self.rows = {}, []
        self.model = Model(self.rows.append, self.base)
        self.put(self.owner, self.base + 0x1169d68)
        self.put(self.owner + 0x720, 1)
        self.put(self.owner + 0x7a8, self.job)
        self.put(self.owner + 0x7d4, 0)
        self.put(self.job, self.base + 0x1169df0)
        self.put(self.job + 0x38, self.owner)
        self.put(self.job + 0x28, 0)
        self.put(self.base + 0x13acc98, self.scheduler)
        self.put(self.scheduler + 0x30, 1)
        self.put(self.stack, self.base + 0xe80eb)
        self.put(self.frame + 8, self.base + 0x100)
        self.g = dict(rsp=self.stack, rbp=self.frame, rbx=self.owner, rdi=self.owner + 0x7a8, rax=0, rsi=3)

    def put(self, p, value):
        for i, b in enumerate(struct.pack('<Q', value)):
            self.memory[p + i] = b

    def read(self, p, n):
        try:
            return bytes(self.memory[p + i] for i in range(n))
        except KeyError:
            raise OSError('unmapped')

    def hit(self, name, tid=1, **changes):
        self.model.hit(name, Reader(self.read, 0), {**self.g, **changes}, 0, tid)

    def finish(self, **changes):
        self.hit('job_submit_return', rsp=self.stack + 8, **changes)

    def test_four_slots(self):
        self.assertEqual(len(Observer('job').probes), 4)

    def test_mode_policy(self):
        for mode in range(1, 7):
            self.assertEqual(mode_write(0, mode), int(mode in (3, 4)))
        for old in (1, 2, 3, 4, 5, 6, -1):
            self.assertIsNone(mode_write(old, 3))
            self.assertIsNone(mode_write(old, 4))
        self.assertIsNone(mode_write(0, 0))
        self.assertEqual(mode_write(3, 0), 0)
        self.assertEqual(mode_write(3, -1), 0)

    def test_mode_requires_audited_caller(self):
        with self.assertRaisesRegex(DecodeError, 'mode_caller'):
            self.hit('particle_mode', rdi=self.owner)
        self.put(self.stack, self.base + 0xc0833)
        self.hit('particle_mode', rdi=self.owner)
        self.assertEqual(self.rows[-1]['predicted_gate_write'], 1)

    def test_gate_snapshot_does_not_infer_submission(self):
        for flag, job, expected in ((0, 0, False), (1, 0, True), (1, self.job, False), (2, 0, False)):
            self.put(self.owner + 0x720, flag)
            self.put(self.owner + 0x7a8, job)
            self.hit('job_gate', rdi=self.owner)
            self.assertEqual(self.rows[-1]['creation_eligible_snapshot'], expected)
        self.assertFalse(self.model.active)

    def test_result_signed_and_paired_even_if_worker_completed(self):
        self.hit('job_submit')
        self.put(self.job + 0x28, 3)
        self.finish(rax=0x80af0902)
        self.assertEqual(self.rows[-1]['result'], -2136012542)
        self.assertEqual(self.rows[-1]['job_state_after'], 3)
        self.assertEqual(self.rows[0]['invocation'], self.rows[-1]['invocation'])
        self.assertFalse(self.model.active)

    def test_null_scheduler_and_no_workers(self):
        self.put(self.base + 0x13acc98, 0)
        self.hit('job_submit')
        self.assertIsNone(self.rows[-1]['registered_workers'])
        self.finish(rax=0xffffffff)
        self.assertEqual(self.rows[-1]['result'], -1)
        self.put(self.base + 0x13acc98, self.scheduler)
        self.put(self.scheduler + 0x30, 0)
        self.hit('job_submit')
        self.assertEqual(self.rows[-1]['registered_workers'], 0)

    def test_foreign_call_excluded_before_owner_dereference(self):
        self.put(self.stack, self.base + 0xdead)
        with self.assertRaisesRegex(DecodeError, 'foreign_submit'):
            self.hit('job_submit', rbx=1)

    def test_skip_or_late_return_cannot_read_result(self):
        with self.assertRaisesRegex(DecodeError, 'no_observed_submit'):
            self.finish(rax=0)
        self.assertEqual(self.rows, [])

    def test_wrong_reference_and_owner(self):
        with self.assertRaisesRegex(DecodeError, 'reference_argument'):
            self.hit('job_submit', rdi=self.owner)
        self.put(self.owner, self.base + 0x1169d70)
        with self.assertRaisesRegex(DecodeError, 'owner_vtable'):
            self.hit('job_submit')

    def test_backlink_changed(self):
        self.hit('job_submit')
        self.put(self.job + 0x38, self.owner + 8)
        with self.assertRaisesRegex(DecodeError, 'fresh_backlink'):
            self.finish()

    def test_frame_and_scheduler_changes(self):
        self.hit('job_submit')
        with self.assertRaisesRegex(DecodeError, 'return_frame'):
            self.finish(rbp=self.frame + 8)
        self.hit('job_submit')
        self.put(self.base + 0x13acc98, self.scheduler + 8)
        with self.assertRaisesRegex(DecodeError, 'scheduler_changed'):
            self.finish()

    def test_thread_lifecycle_and_nested(self):
        self.hit('job_submit')
        with self.assertRaisesRegex(DecodeError, 'no_observed_submit'):
            self.hit('job_submit_return', tid=2, rsp=self.stack + 8)
        with self.assertRaisesRegex(DecodeError, 'nested_submit'):
            self.hit('job_submit')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.finish()
        self.model.thread_boundary(1)
        self.hit('job_submit')
        self.model.thread_boundary(1)
        with self.assertRaisesRegex(DecodeError, 'no_observed_submit'):
            self.finish()

    def test_close_and_budget(self):
        self.model.serial = 1024
        with self.assertRaisesRegex(DecodeError, 'invocation_budget'):
            self.hit('job_submit')
        self.model.close_coverage(0, 'stop')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.hit('job_gate')

    def test_instruction_tamper(self):
        for target in ANCHORS:
            class Image:
                def at(self, address, size):
                    raw = bytes.fromhex(ANCHORS[address])
                    return bytes([raw[0] ^ 1]) + raw[1:] if address == target else raw
            with self.assertRaisesRegex(DecodeError, 'instruction_abi'):
                audit(Image())


if __name__ == '__main__':
    unittest.main()
