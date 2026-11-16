import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import complete_retail_localtest_roots as tool


class CompleteRootsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'old/sd'
        (self.source / 'priv/home/10010029').mkdir(parents=True)
        (self.source / 'priv/user').mkdir(parents=True)
        roster = struct.pack('<18I', 0x10010029, *([0xffffffff] * 15), 0x10010029, 0)
        for name in ('user_list.dat', 'user_list.dat.0'):
            (self.source / 'priv/home' / name).write_bytes(roster)
        config = bytearray(0x4000)
        struct.pack_into('<I', config, 0, 0x10010029)
        (self.source / 'priv/home/10010029/config.dat').write_bytes(config)
        (self.source / 'priv/user/users.json').write_text(json.dumps({'users': [{'id': 0x10010029}]}))
        self.receipt = self.base / 'old/receipt.json'
        self.receipt.write_text(json.dumps({'mode': 'LOCAL/TEST saved-user startup, not completed OOBE or PSN',
            'user_id': '0x10010029', 'output': str(self.source.parent),
            'source_unchanged_verified': True, 'after': tool.manifest(self.source)}))
        self.output = self.base / 'new'

    def test_copy_adds_only_missing_empty_directories(self):
        before = tool.manifest(self.source)
        result = tool.prepare(self.receipt, self.output)
        self.assertEqual(result['completed_empty_roots'], ['user/home/10010029', 'eap/rodata/home/10010029'])
        self.assertEqual(tool.manifest(self.source), before)
        self.assertEqual(tool.content_only(tool.manifest(self.output / 'sd')), tool.content_only(before))
        for root in result['completed_empty_roots']:
            self.assertEqual(list((self.output / 'sd' / root).iterdir()), [])

    def test_changed_source_and_existing_output_rejected(self):
        (self.source / 'priv/user/users.json').write_text('{}')
        with self.assertRaises(ValueError):
            tool.prepare(self.receipt, self.output)
        self.assertFalse(self.output.exists())
        self.output.mkdir()
        with self.assertRaises(ValueError):
            tool.prepare(self.receipt, self.output)

    def test_mismatched_user_rejected(self):
        receipt = json.loads(self.receipt.read_text())
        receipt['user_id'] = '0x10010030'
        self.receipt.write_text(json.dumps(receipt))
        with self.assertRaises(ValueError):
            tool.prepare(self.receipt, self.output)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
