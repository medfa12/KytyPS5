import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_rnps_all as staging


def hbc():
    data = bytearray(128)
    data[:8] = bytes.fromhex('c61fbc03c103191f')
    struct.pack_into('<I', data, 8, 89)
    struct.pack_into('<I', data, 32, len(data))
    data[-20:] = hashlib.sha1(data[:-20]).digest()
    return bytes(data)


def container(plain):
    data = bytearray(48)
    data[:8] = b'RNPSHEDR'
    struct.pack_into('<III', data, 28, 48, len(plain), len(plain))
    return bytes(data) + plain


class CompleteStageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.retail, self.dev, self.stage = (self.root / n for n in ('retail', 'dev', 'stage'))
        self.retail.mkdir()
        self.dev.mkdir()
        self.name = 'apps/NPXS40002/application.ps.bundle'
        self.plain = hbc()
        self.wrapped = container(self.plain)
        for tree in (self.retail, self.dev):
            self.write(tree, 'apps/NPXS40002/manifest.json', json.dumps(dict(titleId='NPXS40002', jsBackend='HERMES')).encode())
            self.write(tree, 'apps/NPXS40002/assets/nested/image.png', b'asset')
            self.write(tree, 'bgs/NPXS40052/main.js', b'native shipped JavaScript')
            self.write(tree, 'bgs/NPXS40052/main.hbc', self.plain)
        self.write(self.retail, self.name, self.wrapped)
        self.write(self.dev, self.name, self.plain)
        self.write(self.dev, self.name + '.orig', self.wrapped)

    @staticmethod
    def write(root, name, data):
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def prepare(self, stage=None, host=None):
        return staging.prepare(self.retail, self.dev, host, stage or self.stage)

    def test_complete_tree_and_provenance_sources_unchanged(self):
        self.write(self.retail, 'apps/NPXS40002/application.ps.js', b'local decompiler output')
        self.write(self.retail, 'apps/NPXS40002/assets/application.ps.js', b'actual nested resource')
        self.write(self.dev, 'apps/NPXS40002/assets/application.ps.js', b'actual nested resource')
        self.write(self.retail, 'shared/transitive/resource.dat', b'transitive')
        before = [staging.inventory(p) for p in (self.retail, self.dev)]
        result = self.prepare()
        self.assertEqual((self.stage / 'rnps' / self.name).read_bytes(), self.plain)
        self.assertEqual((self.stage / 'rnps/bgs/NPXS40052/main.js').read_bytes(), b'native shipped JavaScript')
        self.assertTrue((self.stage / 'rnps/shared/transitive/resource.dat').is_file())
        self.assertFalse((self.stage / 'rnps/apps/NPXS40002/application.ps.js').exists())
        self.assertTrue((self.stage / 'rnps/apps/NPXS40002/assets/application.ps.js').is_file())
        self.assertEqual(result['counts']['substituted_plaintext_files'], 1)
        self.assertEqual(result['counts']['bgs_script_files'], 2)
        self.assertFalse(result['guest_runtime_verified'])
        self.assertEqual(before, [staging.inventory(p) for p in (self.retail, self.dev)])

    def test_bad_footer_preserves_encrypted_original(self):
        self.write(self.dev, self.name, self.plain[:-1] + b'x')
        result = self.prepare()
        self.assertEqual((self.stage / 'rnps' / self.name).read_bytes(), self.wrapped)
        self.assertIn('footer', result['unresolved'][0]['reason'])

    def test_encrypted_background_scripts_preserved_and_accounted(self):
        for tree in (self.retail, self.dev):
            self.write(tree, 'bgs/NPXS40052/main.js', self.wrapped)
            self.write(tree, 'bgs/NPXS40052/main.hbc', self.wrapped)
        result = self.prepare()
        self.assertEqual(result['counts']['bundle_files'], 3)
        self.assertEqual(result['counts']['app_bundle_files'], 1)
        self.assertEqual(result['counts']['unresolved_containers'], 2)
        self.assertTrue(result['background_services']['NPXS40052']['counterpart_inventory_and_bytes_identical'])
        self.assertEqual(len(result['background_services']['NPXS40052']['bundles']), 2)
        self.assertTrue(all('counterpart also RNPSHEDR' in row['reason'] for row in result['unresolved']))
        self.assertEqual((self.stage / 'rnps/bgs/NPXS40052/main.hbc').read_bytes(), self.wrapped)

    def test_resource_mismatch_disallows_substitution(self):
        self.write(self.dev, 'apps/NPXS40002/assets/nested/image.png', b'other')
        result = self.prepare()
        self.assertEqual(result['counts']['substituted_plaintext_files'], 0)
        self.assertEqual(result['counts']['unresolved_containers'], 1)

    def test_wrong_original_disallows_substitution(self):
        self.write(self.dev, self.name + '.orig', b'wrong')
        result = self.prepare()
        self.assertIn('differs from retail', result['unresolved'][0]['reason'])

    def test_backend_version_length_and_container_length_rejections(self):
        for kind in ('backend', 'version', 'file_length', 'container_length'):
            with self.subTest(kind=kind):
                for tree in (self.retail, self.dev):
                    self.write(tree, 'apps/NPXS40002/manifest.json', json.dumps(dict(
                        titleId='NPXS40002', jsBackend='OTHER' if kind == 'backend' else 'HERMES')).encode())
                plain = bytearray(self.plain)
                if kind == 'version':
                    struct.pack_into('<I', plain, 8, 88)
                if kind == 'file_length':
                    struct.pack_into('<I', plain, 32, len(plain) - 1)
                self.write(self.dev, self.name, bytes(plain))
                wrapped = bytearray(self.wrapped)
                if kind == 'container_length':
                    struct.pack_into('<I', wrapped, 36, len(self.plain) + 1)
                self.write(self.retail, self.name, bytes(wrapped))
                self.write(self.dev, self.name + '.orig', bytes(wrapped))
                result = self.prepare(stage=self.root / kind)
                self.assertEqual(result['counts']['substituted_plaintext_files'], 0)
                self.assertEqual(result['counts']['unresolved_containers'], 1)

    def test_retail_only_and_counterpart_only_inventory(self):
        self.write(self.retail, 'apps/NPXS40066/manifest.json', b'{"titleId":"NPXS40066"}')
        self.write(self.retail, 'apps/NPXS40066/application.ps.bundle', self.wrapped)
        self.write(self.dev, 'apps/NPXS40203/manifest.json', b'{"titleId":"NPXS40203"}')
        result = self.prepare()
        self.assertEqual(result['counts']['app_titles'], 2)
        self.assertEqual(result['counterpart_only_titles_not_retail'], ['NPXS40203'])
        self.assertTrue((self.stage / 'rnps/apps/NPXS40066/application.ps.bundle').is_file())
        self.assertFalse((self.stage / 'rnps/apps/NPXS40203').exists())

    def test_existing_and_overlapping_destinations_rejected(self):
        for target in (self.retail, self.retail / 'new', self.root, self.dev / 'new'):
            with self.assertRaises(ValueError):
                self.prepare(target)
        self.stage.mkdir()
        self.write(self.stage, 'keep', b'untouched')
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual((self.stage / 'keep').read_bytes(), b'untouched')

    def test_invalid_retail_container_fails_before_stage(self):
        self.write(self.retail, self.name, b'RNPSHEDR')
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.stage.exists())

    def test_source_addition_during_preflight_detected(self):
        original = staging.inventory
        calls = 0

        def changed(root):
            nonlocal calls
            calls += 1
            if calls == 3:
                self.write(self.retail, 'new-file', b'new')
            return original(root)

        with patch.object(staging, 'inventory', side_effect=changed):
            with self.assertRaisesRegex(ValueError, 'inventory changed'):
                self.prepare()
        self.assertFalse(self.stage.exists())

    def test_host_requires_exact_pins_and_preserved_container(self):
        short = 'host.ps.bundle.hbc'
        name = 'apps/NPXS40141/' + short
        for tree in (self.retail, self.dev):
            self.write(tree, 'apps/NPXS40141/manifest.json', b'{"titleId":"NPXS40141"}')
            self.write(tree, name, self.wrapped)
        host = self.root / 'host'
        self.write(host, 'rnps/' + name, self.plain)
        pins = {short: (staging.digest(self.wrapped), staging.digest(self.plain))}
        with patch.object(staging, 'HOST_PINS', pins):
            result = self.prepare(host=host)
        self.assertEqual(result['counts']['substituted_plaintext_files'], 2)
        self.assertEqual((self.stage / 'rnps' / name).read_bytes(), self.plain)
        self.write(host, 'rnps/' + name, self.plain[:-1] + b'x')
        with patch.object(staging, 'HOST_PINS', pins):
            result = self.prepare(stage=self.root / 'bad-host', host=host)
        self.assertTrue(any('hash mismatch' in row['reason'] for row in result['unresolved']))


if __name__ == '__main__':
    unittest.main()
