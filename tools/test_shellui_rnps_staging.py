"""RNPS input mistakes must not erase an existing overlay or firmware source."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from prepare_shellui_managed_1360 import stage_rnps_apps


class RnpsStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.fw, self.stage, self.source = root / 'fw', root / 'stage', root / 'source'
        self.title = 'NPXS40141'
        self.fw_title = self.fw / 'system_ex_b/rnps/apps' / self.title
        self.fw_title.mkdir(parents=True)
        (self.fw_title / 'host.bundle').write_bytes(b'firmware')
        self.target = self.stage / 'rnps/apps' / self.title
        self.target.mkdir(parents=True)
        (self.target / 'host.bundle').write_bytes(b'previous overlay')
        self.source.mkdir()

    def test_wrong_root_missing_and_empty_preserve_previous_overlay(self):
        for source in (self.source / 'missing', self.source, self.stage / 'rnps'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                stage_rnps_apps(self.fw, self.stage, source)
            self.assertEqual((self.target / 'host.bundle').read_bytes(), b'previous overlay')

    def test_later_invalid_title_is_rejected_before_any_replacement(self):
        valid = self.source / self.title
        valid.mkdir()
        (valid / 'host.bundle').write_bytes(b'new overlay')
        (self.source / 'NPXS49999').mkdir()
        with self.assertRaisesRegex(ValueError, 'no firmware title'):
            stage_rnps_apps(self.fw, self.stage, self.source)
        self.assertEqual((self.target / 'host.bundle').read_bytes(), b'previous overlay')

    def test_cli_rejects_wrong_root_before_managed_staging(self):
        launcher = self.fw / 'system_ex_b/app/NPXS40087/eboot.bin'
        launcher.parent.mkdir(parents=True)
        launcher.write_bytes(b'not reached: RNPS preflight must run first')
        manifest = self.stage / 'rnps/staged_titles.json'
        manifest.write_text('["NPXS40141"]', encoding='utf-8')
        result = subprocess.run([
            sys.executable, '-B', str(Path(__file__).with_name('prepare_shellui_managed_1360.py')),
            '--fw-root', str(self.fw), '--stage', str(self.stage),
            '--rnps-source', str(self.stage / 'rnps'),
        ], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must contain title directories', result.stderr)
        self.assertEqual(manifest.read_text(encoding='utf-8'), '["NPXS40141"]')
        self.assertEqual((self.target / 'host.bundle').read_bytes(), b'previous overlay')
        self.assertFalse((self.stage / 'native').exists())

    def test_source_equal_to_destination_preserves_input(self):
        with self.assertRaisesRegex(ValueError, 'overlaps'):
            stage_rnps_apps(self.fw, self.stage, self.stage / 'rnps/apps')
        self.assertEqual((self.target / 'host.bundle').read_bytes(), b'previous overlay')

    def test_valid_overlay_preserves_firmware_and_nested_assets(self):
        overlay = self.source / self.title
        overlay.mkdir()
        (overlay / 'host.bundle').write_bytes(b'plaintext')
        (self.fw_title / 'assets').mkdir()
        (self.fw_title / 'assets/icon.png').write_bytes(b'icon')
        self.assertEqual(stage_rnps_apps(self.fw, self.stage, self.source), [self.title])
        self.assertEqual((self.target / 'host.bundle').read_bytes(), b'plaintext')
        self.assertEqual((self.target / 'assets/icon.png').read_bytes(), b'icon')
        self.assertEqual((self.fw_title / 'host.bundle').read_bytes(), b'firmware')


if __name__ == '__main__':
    unittest.main()
