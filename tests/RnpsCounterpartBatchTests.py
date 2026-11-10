"""Offline preflight/copy validation, not a Hermes execution/decryption test."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_rnps_counterpart_batch as batch


def hbc():
    data = bytearray(128)
    data[:8] = batch.HBC_MAGIC
    struct.pack_into('<I', data, 8, 89)
    struct.pack_into('<I', data, 32, 148)
    return bytes(data) + hashlib.sha1(data).digest()


def wrapped(plain):
    data = bytearray(48 + len(plain))
    data[:8] = b'RNPSHEDR'
    struct.pack_into('<III', data, 28, 48, len(plain), len(plain))
    return bytes(data)


class BatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='rnps-counterpart-test-',
                                               dir=Path(tempfile.gettempdir()) / 'opencode')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.retail, self.dev, self.stage = (self.root / n for n in ('retail', 'dev', 'stage'))
        for title in (*batch.HOME_FOLLOWUP_TITLES, *batch.HOME_LIBRARY_TITLES):
            for directory in (self.retail / title, self.dev / title):
                (directory / 'assets').mkdir(parents=True)
                (directory / 'manifest.json').write_text(json.dumps({'titleId': title, 'jsBackend': 'HERMES'}))
                (directory / 'assets/icon.bin').write_bytes(b'synthetic matching asset')
                (directory / 'license.txt').write_bytes(b'synthetic license')
            plain = hbc()
            container = wrapped(plain)
            (self.retail / title / 'application.ps.bundle').write_bytes(container)
            (self.dev / title / 'application.ps.bundle').write_bytes(plain)
            (self.dev / title / 'application.ps.bundle.orig').write_bytes(container)
            (self.retail / title / 'application.ps.hbc').write_bytes(b'local artifact never staged')
            (self.retail / title / 'application.ps.js').write_bytes(b'local artifact never staged')

    def inventory(self, root):
        return {p.relative_to(root).as_posix(): batch.digest(p.read_bytes()) for p in root.rglob('*') if p.is_file()}

    def prepare(self, titles=None):
        return batch.prepare(self.retail, self.dev, None, None, self.stage,
                             batch.HOME_FOLLOWUP_TITLES if titles is None else titles)

    def rejected_before_stage(self):
        before = self.inventory(self.retail), self.inventory(self.dev)
        with self.assertRaises((ValueError, KeyError)):
            self.prepare()
        self.assertFalse(self.stage.exists())
        self.assertEqual(before, (self.inventory(self.retail), self.inventory(self.dev)))

    def test_exact_three_additive_titles_and_provenance(self):
        before = self.inventory(self.retail), self.inventory(self.dev)
        receipt = self.prepare()
        self.assertEqual(receipt['requested_titles'], list(batch.HOME_FOLLOWUP_TITLES))
        self.assertEqual(len(receipt['titles']), 3)
        self.assertEqual(len(receipt['files']), 12)
        self.assertFalse(receipt['guest_runtime_verified'])
        self.assertIn('NOT authenticated retail decryption', receipt['classification'])
        for mount in receipt['mounts']:
            self.assertTrue(mount['read_only'])
        for row in receipt['files']:
            self.assertEqual(batch.digest((self.stage / row['path']).read_bytes()), row['sha256'])
            self.assertFalse(row['path'].endswith(('.hbc', '.js', '.orig')))
            if row['path'].endswith('application.ps.bundle'):
                self.assertTrue(row['source'].startswith(str(self.dev)))
            else:
                self.assertTrue(row['source'].startswith(str(self.retail)))
        self.assertEqual(before, (self.inventory(self.retail), self.inventory(self.dev)))

    def test_manifest_mismatch_rejected(self):
        (self.dev / batch.HOME_FOLLOWUP_TITLES[-1] / 'manifest.json').write_text('{}')
        self.rejected_before_stage()

    def test_library_only_additive_selection_and_nested_appdb(self):
        title = 'NPXS40071'
        for root in (self.retail, self.dev):
            (root / title / 'appdb/default').mkdir(parents=True)
            (root / title / 'appdb/default/descriptor.json').write_bytes(b'{"fixture":true}')
        before = self.inventory(self.retail), self.inventory(self.dev)
        receipt = self.prepare([title])
        self.assertEqual(receipt['requested_titles'], [title])
        self.assertEqual(set(receipt['titles']), {title})
        self.assertEqual(len(receipt['files']), 5)
        self.assertEqual(receipt['mounts'], [dict(guest='/system_ex/rnps/apps/' + title,
            host=str(self.stage.resolve() / 'rnps/apps' / title), read_only=True)])
        self.assertTrue(receipt['titles'][title]['bundles']['application.ps.bundle']['footer_sha1_verified'])
        self.assertFalse(receipt['guest_runtime_verified'])
        self.assertIn('NOT authenticated retail decryption', receipt['classification'])
        for row in receipt['files']:
            self.assertEqual((self.stage / row['path']).read_bytes(), Path(row['source']).read_bytes())
        self.assertEqual(before, (self.inventory(self.retail), self.inventory(self.dev)))

    def test_library_nested_appdb_mismatch_prevents_stage(self):
        title = 'NPXS40071'
        for root, content in ((self.retail, b'retail'), (self.dev, b'different')):
            (root / title / 'appdb').mkdir()
            (root / title / 'appdb/descriptor.json').write_bytes(content)
        with self.assertRaisesRegex(ValueError, 'Sibling mismatch'):
            self.prepare([title])
        self.assertFalse(self.stage.exists())

    def test_library_wrong_manifest_title_rejected(self):
        title = 'NPXS40071'
        for root in (self.retail, self.dev):
            (root / title / 'manifest.json').write_text(json.dumps({'titleId': 'NPXS40072', 'jsBackend': 'HERMES'}))
        with self.assertRaisesRegex(ValueError, 'manifest title mismatch'):
            self.prepare([title])
        self.assertFalse(self.stage.exists())

    def test_library_invalid_version_or_length_rejected(self):
        path = self.dev / 'NPXS40071/application.ps.bundle'
        for offset, value in ((8, 90), (32, 149)):
            with self.subTest(offset=offset):
                data = bytearray(hbc())
                struct.pack_into('<I', data, offset, value)
                data[-20:] = hashlib.sha1(data[:-20]).digest()
                path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'Unsupported/truncated Hermes'):
                    self.prepare(['NPXS40071'])
                self.assertFalse(self.stage.exists())

    def test_library_invalid_container_bounds_rejected(self):
        path = self.retail / 'NPXS40071/application.ps.bundle'
        data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 28, len(data) + 1)
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'Invalid RNPS container lengths'):
            self.prepare(['NPXS40071'])
        self.assertFalse(self.stage.exists())

    def test_last_title_asset_mismatch_prevents_entire_stage(self):
        (self.dev / batch.HOME_FOLLOWUP_TITLES[-1] / 'assets/icon.bin').write_bytes(b'different')
        self.rejected_before_stage()

    def test_asset_inventory_mismatch_rejected(self):
        (self.dev / batch.HOME_FOLLOWUP_TITLES[0] / 'assets/additional.bin').write_bytes(b'x')
        self.rejected_before_stage()

    def test_encrypted_counterpart_rejected(self):
        title = batch.HOME_FOLLOWUP_TITLES[0]
        (self.dev / title / 'application.ps.bundle').write_bytes(wrapped(hbc()))
        self.rejected_before_stage()

    def test_invalid_footer_rejected(self):
        path = self.dev / batch.HOME_FOLLOWUP_TITLES[0] / 'application.ps.bundle'
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        self.rejected_before_stage()

    def test_different_preserved_container_rejected(self):
        path = self.dev / batch.HOME_FOLLOWUP_TITLES[0] / 'application.ps.bundle.orig'
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        self.rejected_before_stage()

    def test_plaintext_declared_size_mismatch_rejected(self):
        path = self.retail / batch.HOME_FOLLOWUP_TITLES[0] / 'application.ps.bundle'
        data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 36, 1)
        path.write_bytes(data)
        self.rejected_before_stage()

    def test_unknown_resource_rejected(self):
        (self.dev / batch.HOME_FOLLOWUP_TITLES[0] / 'unreviewed.bin').write_bytes(b'x')
        self.rejected_before_stage()

    def test_selection_bounds_and_host_requirements(self):
        for titles in ([], ['NPXS40048', 'NPXS40048'], ['NPXS40071', 'NPXS40071'],
                       ['NPXS40072'], ['../escape'], [batch.HOST_TITLE]):
            with self.subTest(titles=titles):
                with self.assertRaises(ValueError):
                    self.prepare(titles)
                self.assertFalse(self.stage.exists())
        # Default API selection remains the original 15-title batch and hence
        # still requires its prior audited host comparison.
        with self.assertRaisesRegex(ValueError, 'Host title requires'):
            batch.prepare(self.retail, self.dev, None, None, self.stage)

    def test_existing_or_source_nested_destination_rejected(self):
        self.stage.mkdir()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(list(self.stage.iterdir()), [])
        for stage in (self.retail, self.dev / 'nested'):
            with self.assertRaises(ValueError):
                batch.prepare(self.retail, self.dev, None, None, stage, batch.HOME_FOLLOWUP_TITLES)


if __name__ == '__main__':
    unittest.main()
