"""Offline saved-user local/test preparation against frozen retail-profile2."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_retail_local_test_boot as boot


class LocalTestPreparation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ARGS.workdir)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.output = self.root / 'source', self.root / 'output'
        shutil.copytree(ARGS.source, self.source)
        self.profile = self.source / 'priv/home/10010029/config.dat'

    def reject(self, **kwargs):
        before = boot.manifest(self.source)
        with self.assertRaises((ValueError, OSError)):
            boot.prepare(self.source, self.output, **kwargs)
        self.assertEqual(before, boot.manifest(self.source))
        self.assertFalse(self.output.exists())

    def test_existing_native_user_preserved_and_only_audited_fields_changed(self):
        before = boot.manifest(self.source)
        original = self.profile.read_bytes()
        self.assertEqual(boot.digest(original), 'e9d1079b2b12f4b3107367bed1ee0865b5f5777b5d08a0a3e7ed7290af830d9e')
        receipt = boot.prepare(self.source, self.output, local_test_boot=True)
        self.assertEqual(before, boot.manifest(self.source))
        self.assertEqual(receipt['user_id'], '0x10010029')
        relative = self.profile.relative_to(self.source)
        changed = (self.output / 'sd' / relative).read_bytes()
        for i, (a, b) in enumerate(zip(original, changed)):
            if i not in (*range(0x48, 0x4c), *range(0x5c, 0x60)):
                self.assertEqual(a, b, f'unexpected profile change at {i:x}')
        self.assertEqual(struct.unpack_from('<I', changed, 0x48)[0], 1)
        self.assertEqual(struct.unpack_from('<I', changed, 0x5c)[0], struct.unpack_from('<I', original, 0x5c)[0] & ~1)
        self.assertEqual((self.output / 'backup' / relative).read_bytes(), original)
        for name in before:
            if name != relative.as_posix():
                self.assertEqual(boot.digest((self.output / 'sd' / name).read_bytes()), before[name]['sha256'])
        mirror = json.loads((self.output / 'sd' / boot.USER_STORE).read_text(encoding='utf-8'))
        self.assertEqual(len(mirror['users']), 1)
        user = mirror['users'][0]
        self.assertEqual(user['id'], 0x10010029)
        self.assertTrue(user['auto_login'])
        self.assertTrue(user['name'].encode() == original[4:21].split(b'\0', 1)[0])
        self.assertEqual(user['account_id'], 0)
        self.assertEqual(user['signin_id'], '')
        self.assertFalse(receipt['guest_launched'])

    def test_opt_in_and_no_inplace_or_overwrite(self):
        self.reject()
        with self.assertRaises(ValueError):
            boot.prepare(self.source, self.source, local_test_boot=True)
        self.output.mkdir()
        with self.assertRaises(ValueError):
            boot.prepare(self.source, self.output, local_test_boot=True)

    def test_multiple_profiles_or_mismatched_list_rejected(self):
        path = self.source / 'priv/home/user_list.dat'
        data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 4, 0x10010030)
        path.write_bytes(data)
        self.reject(local_test_boot=True)
        (self.source / 'priv/home/user_list.dat.0').write_bytes(data)
        self.reject(local_test_boot=True)

    def test_profile_identity_schema_account_and_passcode_rejected(self):
        original = self.profile.read_bytes()
        for offset in (0, 0x58, 0x100, 0x18):
            with self.subTest(offset=offset):
                data = bytearray(original)
                data[offset] ^= 1
                self.profile.write_bytes(data)
                self.reject(local_test_boot=True)

    def test_existing_companion_profile_preserved_by_rejection(self):
        path = self.source / boot.USER_STORE
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'version': 1, 'next_user_number': 2, 'users': [{'id': 0x10010029}]}))
        self.reject(local_test_boot=True)

    def test_unrelated_flag_bits_retained(self):
        data = bytearray(self.profile.read_bytes())
        struct.pack_into('<I', data, 0x5c, 0x41)
        self.profile.write_bytes(data)
        boot.prepare(self.source, self.output, local_test_boot=True)
        result = (self.output / 'sd' / self.profile.relative_to(self.source)).read_bytes()
        self.assertEqual(struct.unpack_from('<I', result, 0x5c)[0], 0x40)

    def test_copy_failure_does_not_publish(self):
        with patch.object(boot.shutil, 'copytree', side_effect=OSError('injected copy failure')):
            self.reject(local_test_boot=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--workdir', type=Path, required=True)
    ARGS = parser.parse_args()
    unittest.main(argv=[__file__])
