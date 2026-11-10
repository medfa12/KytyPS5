import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('host_compare', Path(__file__).resolve().parents[1] / 'tools/prepare_retail_host_comparison.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class HostComparisonTest(unittest.TestCase):
    def test_pair_negative_controls(self):
        payload = bytearray(40)
        payload[:8] = bytes.fromhex('c61fbc03c103191f')
        struct.pack_into('<I', payload, 8, 89)
        struct.pack_into('<I', payload, 32, len(payload))
        wrapped = bytearray(48 + len(payload))
        wrapped[:8] = b'RNPSHEDR'
        struct.pack_into('<III', wrapped, 0x1c, 48, len(payload), len(payload))
        h = lambda data: hashlib.sha256(data).hexdigest()
        module.validate_pair('host.hbc', wrapped, payload, h(wrapped), h(payload))
        bad = bytearray(payload); bad[10] ^= 1
        with self.assertRaises(ValueError):
            module.validate_pair('host.hbc', wrapped, bad, h(wrapped), h(payload))
        struct.pack_into('<I', wrapped, 0x24, len(payload) + 1)
        with self.assertRaises(ValueError):
            module.validate_pair('host.hbc', wrapped, payload, h(wrapped), h(payload))

    def test_existing_stage_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(ValueError, 'stage must not exist'):
                module.prepare(root / 'firmware', root / 'source', root)


if __name__ == '__main__':
    unittest.main()
