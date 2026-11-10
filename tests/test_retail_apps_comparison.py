import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('apps_compare', ROOT / 'tools/prepare_retail_apps_comparison.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AppsComparisonTest(unittest.TestCase):
    def test_existing_stage_never_replaced(self):
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp) / 'existing'
            stage.mkdir()
            marker = stage / 'keep'
            marker.write_text('preserve')
            with self.assertRaisesRegex(ValueError, 'stage must not exist'):
                module.prepare(Path(temp) / 'fw', Path(temp) / 'source', stage)
            self.assertEqual(marker.read_text(), 'preserve')

    def test_bad_manifest_rejected_before_stage(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fw = root / 'fw'
            comparison = root / 'comparison'
            for base in (fw / 'system_ex_b/rnps/apps/NPXS40008', comparison / 'NPXS40008'):
                base.mkdir(parents=True)
                (base / 'manifest.json').write_text('{}')
            stage = root / 'stage'
            with self.assertRaisesRegex(ValueError, 'manifest mismatch'):
                module.prepare(fw, comparison, stage)
            self.assertFalse(stage.exists())


if __name__ == '__main__':
    unittest.main()
