"""RNPS setup-to-staging regression checks using synthetic, non-executable data."""
from pathlib import Path
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from setup_retail import rnps_plan
from prepare_shellui_managed_1360 import stage_rnps_apps


class RetailRnpsStaging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rnps-', dir=os.environ.get('TEMP'))
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fw = self.root / 'firmware'
        self.title = self.fw / 'system_ex_b/rnps/apps/NPXS40002'
        self.title.mkdir(parents=True)
        (self.title / 'application.ps.bundle').write_bytes(b'RNPSHEDR synthetic original')
        (self.title / 'assets/nested').mkdir(parents=True)
        (self.title / 'assets/nested/example.txt').write_text('synthetic asset')
        self.source = self.root / 'input'
        self.overlay = self.source / 'NPXS40002'
        self.overlay.mkdir(parents=True)
        self.stage = self.root / 'stage'
        # Only the format marker is real; the remainder is deliberately not HBC.
        self.hbc = bytes.fromhex('c61fbc03c103191f') + b'synthetic test data'

    def stage_and_check_inputs(self):
        def snapshot(root):
            return {str(p.relative_to(root)): p.read_bytes()
                    for p in root.rglob('*') if p.is_file()}
        before = snapshot(self.fw), snapshot(self.source)
        plan = rnps_plan(self.fw, self.stage, self.source)
        normalized = self.root / 'normalized'
        for title, files, deletes in plan:
            target = normalized / title
            target.mkdir(parents=True)
            for name, source in files.items():
                shutil.copyfile(source, target / name)
            (target / '_delete.txt').write_text('\n'.join(sorted(deletes)), encoding='utf-8')
        self.assertEqual(stage_rnps_apps(self.fw, self.stage, normalized), ['NPXS40002'])
        self.assertEqual(before, (snapshot(self.fw), snapshot(self.source)))
        output = self.stage / 'rnps/apps/NPXS40002'
        self.assertEqual((output / 'assets/nested/example.txt').read_text(), 'synthetic asset')
        return output

    def test_short_hbc_name_satisfies_both_native_paths(self):
        (self.overlay / 'application.ps.hbc').write_bytes(self.hbc)
        # Older input manifests may still request removal of this path.
        (self.overlay / '_delete.txt').write_text('application.ps.bundle\n')
        output = self.stage_and_check_inputs()
        self.assertEqual((output / 'application.ps.bundle').read_bytes(), self.hbc)
        self.assertEqual((output / 'application.ps.bundle.hbc').read_bytes(), self.hbc)
        self.assertFalse((output / 'application.ps.hbc').exists())

    def test_long_hbc_name_replaces_encrypted_overlay_sibling(self):
        (self.overlay / 'application.ps.bundle.hbc').write_bytes(self.hbc)
        (self.overlay / 'application.ps.bundle').write_bytes(b'RNPSHEDR synthetic overlay')
        output = self.stage_and_check_inputs()
        self.assertEqual((output / 'application.ps.bundle').read_bytes(), self.hbc)

    def test_explicit_decrypted_sibling_is_preserved(self):
        (self.overlay / 'application.ps.hbc').write_bytes(self.hbc)
        sibling = b'/* synthetic explicit plaintext sibling */'
        (self.overlay / 'application.ps.bundle').write_bytes(sibling)
        output = self.stage_and_check_inputs()
        self.assertEqual((output / 'application.ps.bundle').read_bytes(), sibling)
        self.assertEqual((output / 'application.ps.bundle.hbc').read_bytes(), self.hbc)

    def test_conflicting_hbc_variants_are_rejected_before_staging(self):
        (self.overlay / 'application.ps.hbc').write_bytes(self.hbc)
        (self.overlay / 'application.ps.bundle.hbc').write_bytes(self.hbc + b'different')
        with self.assertRaisesRegex(ValueError, 'Conflicting decrypted'):
            rnps_plan(self.fw, self.stage, self.source)
        self.assertFalse(self.stage.exists())

    def test_encrypted_hbc_is_rejected_before_staging(self):
        (self.overlay / 'application.ps.hbc').write_bytes(b'RNPSHEDR synthetic container')
        with self.assertRaisesRegex(ValueError, 'requires decrypted Hermes HBC'):
            rnps_plan(self.fw, self.stage, self.source)
        self.assertFalse(self.stage.exists())


if __name__ == '__main__':
    unittest.main()
