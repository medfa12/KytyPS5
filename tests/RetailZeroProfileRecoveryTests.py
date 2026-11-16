"""Copy-recovery preservation/failure tests using the authorized frozen artifacts."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('recovery', Path(__file__).resolve().parents[1] / 'tools/recover_retail_zero_profile.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ARGS.workdir)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.output = self.root / 'recovered'
        self.seed('retail-rpc1')

    def seed(self, run):
        if self.source.exists():
            shutil.rmtree(self.source)
        home = self.source / 'priv/home'
        (home / '00000000').mkdir(parents=True)
        shutil.copyfile(ARGS.config_evidence / run / '00000000/config.dat', home / '00000000/config.dat')
        for name in ('user_list.dat', 'user_list.dat.0'):
            shutil.copyfile(ARGS.list_evidence / run / name, home / name)
        # Unrelated setup/app state must survive byte-for-byte.
        (self.source / 'appdb').mkdir()
        (self.source / 'appdb/fixture.db').write_bytes(bytes(range(256)) * 13)
        (self.source / 'priv/setup-progress').write_bytes(b'synthetic setup state\0\xff')

    def reject_unchanged(self):
        before = recovery.manifest(self.source)
        before_directories = recovery.directory_manifest(self.source)
        with self.assertRaises((ValueError, OSError)):
            recovery.recover(self.source, self.output)
        self.assertEqual(before, recovery.manifest(self.source))
        self.assertEqual(before_directories, recovery.directory_manifest(self.source))
        self.assertFalse(self.output.exists())

    def test_three_frozen_runs_and_backups(self):
        for run in ('retail-font3', 'retail-rnps-batch1', 'retail-rpc1'):
            with self.subTest(run=run):
                self.seed(run)
                before = recovery.manifest(self.source)
                output = self.root / run
                receipt = recovery.recover(self.source, output)
                self.assertEqual(before, recovery.manifest(self.source))
                self.assertEqual(receipt, json.loads((output / 'receipt.json').read_text()))
                for name in ('user_list.dat', 'user_list.dat.0'):
                    raw = (output / 'sd/priv/home' / name).read_bytes()
                    self.assertEqual(struct.unpack('<18I', raw), (0xffffffff,) * 16 + (0x0fffffff, 0))
                self.assertFalse((output / 'sd/priv/home/00000000').exists())
                for name in recovery.TARGETS:
                    self.assertEqual((output / 'backup' / name).read_bytes(), (self.source / name).read_bytes())
                for name in ('appdb/fixture.db', 'priv/setup-progress'):
                    self.assertEqual((output / 'sd' / name).read_bytes(), (self.source / name).read_bytes())

    def test_fresh_store_is_not_migration_input(self):
        shutil.rmtree(self.source / 'priv/home')
        (self.source / 'priv/home').mkdir()
        self.reject_unchanged()

    def test_optional_empty_nobackup_quarantined_with_metadata(self):
        directory = self.source / recovery.NO_BACKUP
        directory.mkdir()
        os.utime(directory, ns=(1600000000000000000, 1600000000000000000))
        before = recovery.manifest(self.source)
        before_dirs = recovery.directory_manifest(self.source)
        receipt = recovery.recover(self.source, self.output)
        self.assertEqual(before, recovery.manifest(self.source))
        self.assertEqual(before_dirs, recovery.directory_manifest(self.source))
        backup = self.output / 'backup' / recovery.NO_BACKUP
        self.assertTrue(backup.is_dir())
        self.assertEqual(list(backup.iterdir()), [])
        self.assertFalse((self.output / 'sd' / recovery.ZERO_HOME).exists())
        self.assertEqual(receipt['before_directories'], before_dirs)
        self.assertEqual(receipt['after_directories'], recovery.directory_manifest(self.output / 'sd'))
        self.assertEqual(receipt['backup_directories'], recovery.directory_manifest(self.output / 'backup'))
        self.assertEqual(receipt['backup_directories'][recovery.NO_BACKUP]['mtime_ns'],
                         before_dirs[recovery.NO_BACKUP]['mtime_ns'])
        self.assertIn(recovery.NO_BACKUP, receipt['quarantined_directories'])

    def test_nobackup_contents_and_other_empty_children_rejected(self):
        directory = self.source / recovery.NO_BACKUP
        directory.mkdir()
        child = directory / 'config.dat.bak'
        child.write_bytes(recovery.CONFIG)
        self.reject_unchanged()
        child.unlink()
        # Even a zero-byte file or empty nested directory makes it nonempty.
        child.write_bytes(b'')
        self.reject_unchanged()
        child.unlink()
        child.mkdir()
        self.reject_unchanged()
        child.rmdir()
        other = directory.parent / 'unexpected'
        other.mkdir()
        self.reject_unchanged()

    def test_valid_profile_id_and_data_preserved_by_rejection(self):
        home = self.source / 'priv/home'
        raw = bytearray((home / 'user_list.dat').read_bytes())
        struct.pack_into('<I', raw, 0, 0x10010000)
        (home / 'user_list.dat').write_bytes(raw)
        (home / '10010000').mkdir()
        (home / '10010000/config.dat').write_bytes(b'valid profile preservation fixture')
        self.reject_unchanged()

    def test_even_one_nonzero_payload_byte_is_ambiguous(self):
        config = self.source / 'priv/home/00000000/config.dat'
        for offset in (4, 0x18, 0x58, 0x1000, 0x3fff):
            with self.subTest(offset=offset):
                raw = bytearray(recovery.CONFIG)
                raw[offset] ^= 1
                config.write_bytes(raw)
                self.reject_unchanged()

    def test_backup_or_other_profile_or_truncated_list_rejected(self):
        home = self.source / 'priv/home'
        extra = home / '00000000/config.dat.bak'
        extra.write_bytes(recovery.CONFIG)
        self.reject_unchanged()
        extra.unlink()
        (home / '10010000').mkdir()
        self.reject_unchanged()
        (home / '10010000').rmdir()
        (home / 'user_list.dat.0').write_bytes(recovery.LIST[:-1])
        self.reject_unchanged()

    def test_companion_valid_user_store_rejected_empty_store_preserved(self):
        user = self.source / 'priv/user/users.json'
        user.parent.mkdir()
        user.write_text(json.dumps({'version': 1, 'next_user_number': 2, 'users': [{'id': 0x10010000}]}))
        self.reject_unchanged()
        user.write_text(json.dumps({'version': 1, 'next_user_number': 2, 'users': []}))
        self.reject_unchanged()
        user.write_text(json.dumps({'version': 1, 'next_user_number': 1, 'users': []}))
        recovery.recover(self.source, self.output)
        self.assertEqual(user.read_bytes(), (self.output / 'sd/priv/user/users.json').read_bytes())

    def test_no_in_place_or_repeated_recovery(self):
        for target in (self.source, self.source / 'child', self.root):
            with self.assertRaises(ValueError):
                recovery.recover(self.source, target)
        recovery.recover(self.source, self.output)
        before = recovery.manifest(self.output)
        with self.assertRaises(ValueError):
            recovery.recover(self.source, self.output)
        self.assertEqual(before, recovery.manifest(self.output))

    def test_copy_failure_does_not_publish_or_change_source(self):
        before = recovery.manifest(self.source)
        with patch.object(recovery.shutil, 'copytree', side_effect=OSError('injected copy failure')):
            with self.assertRaises(OSError):
                recovery.recover(self.source, self.output)
        self.assertEqual(before, recovery.manifest(self.source))
        self.assertFalse(self.output.exists())
        self.assertEqual(len(list(self.root.glob('recovered.incomplete-*/FAILED.txt'))), 1)

    def test_incomplete_copy_rejected(self):
        original_copy = shutil.copytree
        def corrupt_after_copy(source, target):
            with patch.object(recovery.shutil, 'copytree', original_copy):
                original_copy(source, target)
            (target / 'appdb/fixture.db').write_bytes(b'injected bad copy')
        with patch.object(recovery.shutil, 'copytree', side_effect=corrupt_after_copy):
            self.reject_unchanged()

    def test_concurrent_source_change_prevents_publication(self):
        original_copy = shutil.copytree
        original_config = (self.source / 'priv/home/00000000/config.dat').read_bytes()
        def source_changes(source, target):
            with patch.object(recovery.shutil, 'copytree', original_copy):
                original_copy(source, target)
            (source / 'priv/setup-progress').write_bytes(b'simulated external update')
        with patch.object(recovery.shutil, 'copytree', side_effect=source_changes):
            with self.assertRaisesRegex(ValueError, 'source changed'):
                recovery.recover(self.source, self.output)
        self.assertFalse(self.output.exists())
        self.assertEqual((self.source / 'priv/home/00000000/config.dat').read_bytes(), original_config)

    def test_metadata_change_in_list_rejected(self):
        for word in (16, 17):
            with self.subTest(word=word):
                self.seed('retail-rpc1')
                path = self.source / 'priv/home/user_list.dat'
                raw = bytearray(path.read_bytes())
                struct.pack_into('<I', raw, word * 4, 1)
                path.write_bytes(raw)
                self.reject_unchanged()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config-evidence', type=Path, required=True)
    parser.add_argument('--list-evidence', type=Path, required=True)
    parser.add_argument('--workdir', type=Path, required=True)
    ARGS = parser.parse_args()
    unittest.main(argv=[__file__])
