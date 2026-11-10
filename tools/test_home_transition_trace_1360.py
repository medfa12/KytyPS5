"""Synthetic-memory negative controls; no process access or firmware writes."""
import argparse
from pathlib import Path
import struct
import unittest

from home_gate_trace_1360 import Reader, DecodeError, Image
from home_transition_trace_1360 import Model, Observer, SHA256, audit


class Tests(unittest.TestCase):
    def setUp(self):
        self.memory, self.rows = {}, []
        self.base, self.stack, self.owner = 0x930000000, 0x200100, 0x300000
        self.frame = self.stack - 8
        self.model = Model(self.rows.append, self.base, 'particle0')
        self.put(self.stack, self.base + 0xeb6ed)
        self.put(self.frame - 0xd0, self.owner)
        self.regs = dict(rsp=self.stack, rbp=self.frame, rdi=self.owner, rax=0)

    def put(self, address, value):
        for i, b in enumerate(struct.pack('<Q', value)):
            self.memory[address + i] = b

    def read(self, address, size):
        try:
            return bytes(self.memory[address + i] for i in range(size))
        except KeyError:
            raise OSError('unmapped test memory')

    def hit(self, name, tid=1, **changes):
        self.model.hit(name, Reader(self.read, 0), {**self.regs, **changes}, 1, tid)

    def test_both_particle_modes_genuine_completion(self):
        for mode in ('particle0', 'particle1'):
            self.model = Model(self.rows.append, self.base, mode)
            self.hit('particle_entry')
            self.hit('asset_return')
            self.hit('particle_complete')
            self.assertEqual(self.rows[-1]['selected_path'], 'asset')
            self.assertFalse(self.model.active)
            self.assertEqual(len(Observer(mode).probes), 4)

    def test_failed_loader_fallback_same_invocation(self):
        self.hit('particle_entry')
        self.hit('asset_return', rax=0xffffffff)
        self.assertEqual(self.rows[-1]['result'], -1)
        self.hit('fallback_return')
        self.hit('particle_complete')
        self.assertEqual(self.rows[-1]['selected_path'], 'fallback')
        self.assertEqual({row['invocation'] for row in self.rows}, {1})

    def test_early_file_failure_fallback_without_loader(self):
        self.hit('particle_entry')
        self.hit('fallback_return')
        self.hit('particle_complete')
        self.assertEqual(self.rows[-1]['selected_path'], 'fallback')

    def test_late_attach_never_correlates(self):
        with self.assertRaisesRegex(DecodeError, 'no_observed_entry'):
            self.hit('asset_return')
        self.assertEqual(self.rows, [])

    def test_wrong_caller_rejected_before_owner_read(self):
        self.put(self.stack, self.base + 0xdead)
        with self.assertRaisesRegex(DecodeError, 'entry_caller'):
            self.hit('particle_entry', rdi=1)

    def test_fresh_owner_mismatch_invalidates(self):
        self.hit('particle_entry')
        self.put(self.frame - 0xd0, self.owner + 8)
        with self.assertRaisesRegex(DecodeError, 'fresh_owner'):
            self.hit('asset_return')
        self.put(self.frame - 0xd0, self.owner)
        with self.assertRaisesRegex(DecodeError, 'no_observed_entry'):
            self.hit('particle_complete')

    def test_threads_do_not_pair_and_exit_invalidates(self):
        self.hit('particle_entry')
        with self.assertRaisesRegex(DecodeError, 'no_observed_entry'):
            self.hit('asset_return', tid=2)
        self.model.thread_boundary(1)
        with self.assertRaisesRegex(DecodeError, 'no_observed_entry'):
            self.hit('asset_return')

    def test_duplicate_nested_and_closed_rejected(self):
        self.hit('particle_entry')
        with self.assertRaisesRegex(DecodeError, 'nested_or_unfinished'):
            self.hit('particle_entry')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.hit('asset_return')
        self.model.thread_boundary(1)
        self.hit('particle_entry')
        self.model.close_coverage(2, 'detach')
        with self.assertRaisesRegex(DecodeError, 'coverage_closed'):
            self.hit('asset_return')

    def test_negative_fallback_cannot_complete(self):
        self.hit('particle_entry')
        self.hit('fallback_return', rax=0xffffffff)
        with self.assertRaisesRegex(DecodeError, 'impossible_completion'):
            self.hit('particle_complete')

    def test_success_cannot_take_fallback(self):
        self.hit('particle_entry')
        self.hit('asset_return')
        with self.assertRaisesRegex(DecodeError, 'unexpected_fallback'):
            self.hit('fallback_return')

    def test_instruction_tamper_rejected(self):
        class WrongImage:
            def at(self, address, size):
                return b'\0' * size
        with self.assertRaisesRegex(DecodeError, 'instruction_abi'):
            audit(WrongImage())

    def native(self):
        self.model = Model(self.rows.append, self.base, 'native')
        self.root, self.renderer = 0x400000, 0x500000
        self.put(self.base + 0x13a8ee0, self.root)
        self.put(self.root + 8, self.renderer)
        self.put(self.renderer, 1)
        self.put(self.owner, 0x123400000001)
        self.regs.update(rsi=0xa0)

    def native_call(self):
        self.hit('native_call', rdi=self.root, rsi=self.owner, rax=self.owner)
        self.nested = self.frame - 0x20
        self.put(self.nested, self.frame)
        self.put(self.nested + 8, self.base + 0xd9485)

    def renderer_hit(self, name):
        self.hit(name, rbp=self.nested, r14=self.root, rbx=self.owner,
                 rdi=self.renderer, rsi=self.owner)

    def test_native_null_is_not_ready(self):
        self.native()
        self.put(self.base + 0x13a8ee0, 0)
        self.hit('native_entry')
        self.assertFalse(self.rows[-1]['singleton_present'])
        self.assertNotIn('renderer_ready_byte', self.rows[-1])

    def test_native_fresh_nested_owner_and_flow(self):
        self.native()
        self.hit('native_entry')
        self.native_call()
        self.renderer_hit('renderer_call')
        self.put(self.renderer, 0)
        self.renderer_hit('renderer_after')
        self.assertEqual(self.rows[-1]['renderer_ready_byte'], 0)
        self.assertEqual(self.rows[-1]['flow_word_4'], 0x1234)
        self.assertFalse(self.model.active)

    def test_native_wrong_size_and_missing_call(self):
        self.native()
        with self.assertRaisesRegex(DecodeError, 'flow_size'):
            self.hit('native_entry', rsi=159)
        self.hit('native_entry')
        with self.assertRaisesRegex(DecodeError, 'missing_call'):
            self.hit('renderer_call')

    def test_native_replaced_singleton_rejected(self):
        self.native()
        self.hit('native_entry')
        self.native_call()
        self.put(self.base + 0x13a8ee0, self.root + 8)
        with self.assertRaisesRegex(DecodeError, 'fresh_owner'):
            self.renderer_hit('renderer_call')

    def test_native_unrelated_caller_frame_rejected(self):
        self.native()
        self.hit('native_entry')
        self.native_call()
        self.put(self.nested + 8, self.base + 0xd9486)
        with self.assertRaisesRegex(DecodeError, 'nested_caller'):
            self.renderer_hit('renderer_call')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--original', type=Path)
    args, rest = p.parse_known_args()
    if args.original:
        audit(Image(args.original.read_bytes(), SHA256))
        print('Pinned original shell instruction audit passed.')
    unittest.main(argv=[__file__, *rest])
