"""Offline snapshot replay: rejection, empty results and incomplete coverage."""
import struct
import unittest

from home_gate_trace_1360 import DecodeError, Reader
from retail_peripheral_trace_1360 import Model, Observer, device_array, result_count


class Memory:
    def __init__(self):
        self.data = bytearray(0x10000)

    def put(self, address, raw):
        self.data[address - 0x10000:address - 0x10000 + len(raw)] = raw

    def q(self, address, value):
        self.put(address, struct.pack('<Q', value))

    def i(self, address, value):
        self.put(address, struct.pack('<i', value))

    def read(self, address, length):
        return bytes(self.data[address - 0x10000:address - 0x10000 + length])


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.memory, self.rows = Memory(), []
        self.reader = Reader(self.memory.read, 0)
        self.model = Model(self.rows.append, {})
        self.frame, self.array, self.result = 0x18000, 0x11000, 0x13000
        self.regs = dict(rsp=self.frame + 8, rbp=self.frame, rax=0, rbx=0, r13=1, rdx=self.result)
        self.memory.q(self.frame - 0x148, self.array)
        self.memory.q(self.array + 24, 1)
        raw = bytearray(112)
        struct.pack_into('<qiHHi', raw, 0, 2, -1, 0x54c, 0xce6, 0)
        struct.pack_into('<qqQQ', raw, 40, 16, 2, 1, 1)
        self.memory.put(self.array + 32, raw)
        self.memory.put(self.frame - 0x120, raw)
        self.memory.q(self.result + 56, 0x14000)
        self.memory.q(0x14010, 0x15000)
        self.memory.q(0x15018, 4)

    def hit(self, name, tid=1, **regs):
        self.model.hit(name, self.reader, dict(self.regs, **regs), 0, tid)

    def test_converted_nonempty_is_native_only(self):
        self.hit('entry')
        self.hit('enumerated')
        self.hit('converted', rax=1)
        self.memory.i(0x14020, 1)
        self.hit('results')
        self.assertEqual([r['query'] for r in self.rows], [1, 1, 1, 1])
        self.assertEqual(self.rows[1]['devices'][0]['owner'], -1)
        self.assertEqual(self.rows[1]['devices'][0]['native_type'], 16)
        self.assertTrue(self.rows[2]['conversion_ok'])
        self.assertEqual(self.rows[3]['result_count'], 1)
        self.assertFalse(self.model.outstanding(0))
        self.assertFalse(self.model.ready)

    def test_failed_conversion_distinguished_from_empty_enumeration(self):
        self.hit('entry')
        self.hit('enumerated')
        self.hit('converted', rax=0)
        self.hit('results')
        self.assertEqual(len(self.rows[1]['devices']), 1)
        self.assertFalse(self.rows[2]['conversion_ok'])
        self.assertEqual(self.rows[3]['result_count'], 0)
        self.memory.q(self.array + 24, 0)
        self.hit('entry')
        self.hit('enumerated')
        self.assertEqual(self.rows[-1]['devices'], [])

    def test_native_error_preserved(self):
        self.hit('enumerated', rax=0x80b60003)
        self.assertEqual(self.rows[-1]['native_return'], -2135556093)
        self.assertFalse(self.rows[-1]['entry_observed'])

    def test_native_error_with_invalid_output_is_not_empty(self):
        self.memory.q(self.frame - 0x148, 0)
        with self.assertRaises(DecodeError):
            self.hit('enumerated', rax=0x80b60003)
        self.assertEqual(self.rows[-1]['native_return'], -2135556093)
        self.assertTrue(self.rows[-1]['devices_unknown'])
        self.assertNotIn('devices', self.rows[-1])

    def test_late_attach_and_thread_reuse_not_correlated(self):
        self.hit('converted', rax=1)
        self.assertIsNone(self.rows[-1]['query'])
        self.hit('entry')
        self.hit('enumerated', tid=2)
        self.assertIsNone(self.rows[-1]['query'])
        self.model.thread_boundary(1)
        self.hit('results')
        self.assertIsNone(self.rows[-1]['query'])

    def test_incomplete_query_is_retained(self):
        self.hit('entry')
        self.model.close_coverage(1, 'deadline')
        self.assertEqual(self.model.outstanding(1), [1])
        self.hit('entry')
        self.assertEqual(self.rows[-2]['reason'], 'frame_reused')
        self.assertEqual(self.model.outstanding(1), [2])

    def test_result_count_uses_retail_size_not_sync_root(self):
        self.memory.q(0x14018, 0xdeadbeef)
        self.memory.i(0x14020, 2)
        self.assertEqual(result_count(self.reader, self.result), 2)
        self.memory.i(0x14020, 5)
        with self.assertRaisesRegex(DecodeError, 'peripheral_result_count'):
            result_count(self.reader, self.result)

    def test_bad_array_not_reported_empty(self):
        self.memory.q(self.array + 24, 257)
        with self.assertRaisesRegex(DecodeError, 'peripheral_array_count'):
            device_array(self.reader, self.array)

    def test_owner_and_type_filter_results(self):
        self.hit('entry')
        self.hit('owner', rax=0)
        self.assertFalse(self.rows[-1]['valid_available_owner'])
        self.memory.q(self.frame - 0x160, 0x16000)
        text = 'standardController'
        self.memory.i(0x16010, len(text))
        self.memory.put(0x16014, text.encode('utf-16-le'))
        self.hit('allowed', rax=1)
        self.assertEqual(self.rows[-1]['peripheral_type'], text)
        self.assertTrue(self.rows[-1]['allowed'])

    def test_four_distinct_hardware_slots(self):
        for mode in ('query', 'filters'):
            probes = Observer(mode).probes
            self.assertEqual(len(probes), 4)
            self.assertEqual(len(set(probes.values())), 4)


if __name__ == '__main__':
    unittest.main()
